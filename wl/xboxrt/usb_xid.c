// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026 sp00nz and contributors
// The Xbox gamepad as a USB device (usb.h): standard descriptors over endpoint 0, the XID
// descriptor and capabilities (XAPI's test of "is this a controller"), GET_REPORT, the 20-byte
// report on interrupt endpoint 1 and the rumble on endpoint 2 or SET_REPORT. From xboxrecomp's
// src/usb/usb_gamepad.c (descriptors, requests, the reasons each is answered); its host input,
// scripted presses and keyboard are replaced by the ports of input.h.
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/pad.h"
#include "wl/xboxrt/usb.h"

#define TAG "xbusb"

// ---- descriptors (USB 1.1; the gamepad's own interface class) ----------------------------------

static const uint8_t k_device_desc[18] = {
    18,         // bLength
    0x01,       // DEVICE
    0x10, 0x01, // bcdUSB 1.10
    0x00,       // class per interface
    0x00, 0x00, // subclass, protocol
    0x08,       // bMaxPacketSize0
    0x5E, 0x04, // Microsoft
    0x89, 0x02, // Controller S
    0x21, 0x01, // bcdDevice
    0x00, 0x00, 0x00, // no strings
    0x01,       // one configuration
};

// configuration, interface (class 0x58 subclass 0x42), endpoint 0x81 IN and 0x02 OUT, interrupt,
// 32 bytes, every 4 ms
static const uint8_t k_config_desc[32] = {
    9, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 50,
    9, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,
    7, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    7, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04,
};

// XID: gamepad, subtype S, 20-byte input and 6-byte output reports
static const uint8_t k_xid_desc[16] = {
    0x10, 0x42, 0x00, 0x01, 0x01, 0x02, 20, 6, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// capabilities: the report with every field it has set to ones (the id and length are values)
static const uint8_t k_caps_in[20] = {
    0x00, 20, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t k_caps_out[6] = {0x00, 6, 0xFF, 0xFF, 0xFF, 0xFF};

#define REQ_GET_STATUS 0x00
#define REQ_CLEAR_FEATURE 0x01
#define REQ_SET_FEATURE 0x03
#define REQ_SET_ADDRESS 0x05
#define REQ_GET_DESCRIPTOR 0x06
#define REQ_GET_CONFIGURATION 0x08
#define REQ_SET_CONFIGURATION 0x09
#define REQ_GET_INTERFACE 0x0A
#define REQ_SET_INTERFACE 0x0B

typedef struct {
    uint8_t address, configuration;
} Pad;

static Pad g_pads[WL_XB_USB_PORTS];

void wl_xb_xid_reset(uint32_t port) {
    if (port < WL_XB_USB_PORTS) {
        g_pads[port].address = 0;
        g_pads[port].configuration = 0;
    }
}

uint8_t wl_xb_xid_address(uint32_t port) {
    return port < WL_XB_USB_PORTS ? g_pads[port].address : 0;
}

uint8_t wl_xb_xid_configuration(uint32_t port) {
    return port < WL_XB_USB_PORTS ? g_pads[port].configuration : 0;
}

// the smaller of what was asked and what there is
static int copy_out(uint8_t *out, uint32_t max, const uint8_t *src, uint32_t len, uint16_t asked) {
    if (len > asked) {
        len = asked;
    }
    if (len > max) {
        len = max;
    }
    memcpy(out, src, len);
    return (int)len;
}

static void rumble(uint32_t port, const uint8_t *data, uint32_t len) {
    uint16_t left, right;
    if (wl_xbox_rumble_parse(data, len, &left, &right)) {
        wl_xb_input_rumble(port, left, right);
    }
}

int wl_xb_xid_control(uint32_t port, const WlXbUsbSetup *s, const uint8_t *data, uint32_t data_len, uint8_t *out,
                      uint32_t max) {
    if (port >= WL_XB_USB_PORTS) {
        return -1;
    }
    Pad *pad = &g_pads[port];
    bool in = (s->request_type & 0x80u) != 0;
    unsigned type = (s->request_type >> 5) & 3u; // 0 standard, 1 class, 2 vendor
    if (type == 0) {
        switch (s->request) {
        case REQ_GET_DESCRIPTOR:
            switch (s->value >> 8) {
            case 0x01:
                return copy_out(out, max, k_device_desc, sizeof k_device_desc, s->length);
            case 0x02:
                return copy_out(out, max, k_config_desc, sizeof k_config_desc, s->length);
            default:
                return -1; // no strings: a stall is the answer a host expects
            }
        case REQ_SET_ADDRESS:
            pad->address = (uint8_t)(s->value & 0x7Fu);
            return 0;
        case REQ_SET_CONFIGURATION:
            pad->configuration = (uint8_t)(s->value & 0xFFu);
            return 0;
        case REQ_GET_CONFIGURATION:
            if (!in || max < 1) {
                return -1;
            }
            out[0] = pad->configuration;
            return 1;
        case REQ_GET_INTERFACE:
            if (!in || max < 1) {
                return -1;
            }
            out[0] = 0;
            return 1;
        case REQ_GET_STATUS:
            if (!in || max < 2) {
                return -1;
            }
            out[0] = 0; // bus-powered, no remote wake-up
            out[1] = 0;
            return 2;
        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
        case REQ_SET_INTERFACE:
            return 0;
        default:
            return -1;
        }
    }
    if (type == 1) {
        // GET_REPORT: XAPI asks for the report over the control pipe too; a stall there halts the
        // endpoint and the pad is never polled again (xboxrecomp, measured on Shin Megami Tensei)
        if (in && s->request == 0x01) {
            WlXboxPad st;
            uint8_t report[WL_XBOX_PAD_REPORT_BYTES];
            wl_xb_input_state(port, &st);
            wl_xbox_pad_report(&st, report);
            return copy_out(out, max, report, sizeof report, s->length);
        }
        // SET_REPORT: the rumble, as some titles send it (Burnout 3, every frame)
        if (!in && s->request == 0x09) {
            rumble(port, data, data_len);
            return 0;
        }
        return -1;
    }
    if (type == 2) {
        // XID: how XAPI tells a controller from any other device; a stall means "not a controller"
        if (!in) {
            return 0;
        }
        if (s->request == REQ_GET_DESCRIPTOR && (s->value >> 8) == 0x42) {
            return copy_out(out, max, k_xid_desc, sizeof k_xid_desc, s->length);
        }
        if (s->request == 0x01) { // GET_CAPABILITIES
            if ((s->value >> 8) == 0x01) {
                return copy_out(out, max, k_caps_in, sizeof k_caps_in, s->length);
            }
            if ((s->value >> 8) == 0x02) {
                return copy_out(out, max, k_caps_out, sizeof k_caps_out, s->length);
            }
        }
        return -1;
    }
    return -1;
}

int wl_xb_xid_in(uint32_t port, uint8_t ep, uint8_t *out, uint32_t max) {
    if (port >= WL_XB_USB_PORTS || ep != 1) {
        return -1;
    }
    WlXboxPad st;
    uint8_t report[WL_XBOX_PAD_REPORT_BYTES];
    wl_xb_input_state(port, &st);
    wl_xbox_pad_report(&st, report);
    uint32_t n = max < sizeof report ? max : (uint32_t)sizeof report;
    memcpy(out, report, n);
    wl_xb_input_report_sent(port);
    return (int)n;
}

void wl_xb_xid_out(uint32_t port, uint8_t ep, const uint8_t *data, uint32_t len) {
    if (port < WL_XB_USB_PORTS && ep == 2) {
        rumble(port, data, len);
    }
}
