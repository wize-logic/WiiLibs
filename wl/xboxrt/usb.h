// SPDX-License-Identifier: MIT
// The MCPX's USB: two OHCI 1.0a host controllers (0xFED00000, interrupt line 1; 0xFED08000, line 9)
// and the Xbox's gamepads on the first one's root hub, so that a title's own statically linked
// XAPI (its USB driver, the XID class driver behind XInputOpen, XInputGetState, XInputSetState,
// XGetDeviceChanges) enumerates and reads them exactly as on the console.
//
// The controllers are bus masters: the title builds endpoint and transfer descriptors (ED, TD) and
// the HCCA in its memory and points the registers at them; the model walks those lists (physical
// addresses, read and written little-endian through the window: wl_xb_phys_ptr), moves the bytes
// to and from the devices, writes the completed descriptors to the done queue and raises the
// interrupt (a level source the kernel's ISR delivery asks). The lists are walked at each frame
// tick (each vertical blank: the periodic list, one packet per interrupt endpoint) and, for the
// control and bulk lists, as soon as the title fills them (its write to HcCommandStatus) and when
// it acknowledges a done queue.
//
// A gamepad (usb_xid.c) is a Controller S (045E:0289, interface class 0x58 subclass 0x42): its
// standard descriptors, the XID descriptor and capabilities, GET_REPORT, and its 20-byte report
// on interrupt endpoint 1, made from the port's state (input.h); rumble arrives on interrupt
// endpoint 2 or as SET_REPORT and goes back to input.h. Xbox port p is root hub port
// root_port[p] (the console's front sockets 1..4 are the root hub's ports 3, 4, 1, 2). A gamepad
// is plugged in once the controller runs with the root hub's change interrupt enabled (the
// driver's own bring-up scan clears an earlier connect change), one at a time (the next once the
// last is configured), and unplugged when its port's connection goes (input.h).
//
// Derived from xboxrecomp's src/usb/ohci.c and usb_gamepad.c (MIT; third_party/README.md), which
// were "written from the OHCI 1.0a specification and this title's own code": the register
// semantics, the list walking and the lessons recorded in their comments are theirs; the
// trapping, threads and Win32 parts are replaced by wl/xboxrt's device faults and vertical blank.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XB_USB_PORTS 4u // the Xbox's controller ports

typedef struct WlXbUsbConfig {
    uint8_t root_port[WL_XB_USB_PORTS]; // the first controller's root hub port (0..3) of each Xbox port
    uint32_t settle_frames;             // frame ticks between one gamepad's configuration and the next's plug-in
    bool trace;                         // log registers, descriptors and transfers (a lot)
} WlXbUsbConfig;
void wl_xb_usb_config_default(WlXbUsbConfig *cfg);

// Registers both controllers (replacing devices.c's idle ones), their interrupt levels and the
// frame tick (a vertical blank hook). Needs the service (irq.h) for the tick.
WlResult wl_xb_usb_register(const WlXbUsbConfig *cfg);

// One frame tick (the runtime calls it at each vertical blank, lock held; tests call it by hand).
void wl_xb_usb_frame(void);

typedef struct {
    uint32_t reads, writes;            // register accesses
    uint32_t setups, reports, outs;    // SETUP packets, input reports handed over, OUT packets
    uint32_t stalls;                   // requests a device refused
    uint32_t done_queues;              // done queues written
    uint32_t plugged, unplugged;       // gamepads plugged in and out
    uint32_t configured;               // gamepads configured (SET_CONFIGURATION)
} WlXbUsbStats;
void wl_xb_usb_stats(WlXbUsbStats *out);
// The address and configuration a gamepad has (0, 0 before enumeration), for tests and logs.
void wl_xb_usb_pad_info(uint32_t port, uint8_t *address, uint8_t *configuration, bool *attached);

// ---- The device side (usb_xid.c) -------------------------------------------------------------

typedef struct {
    uint8_t request_type, request;
    uint16_t value, index, length;
} WlXbUsbSetup;

// A control request with its OUT data stage (`data`, `data_len`, host to device): returns the
// bytes of its IN data stage written to `out` (at most `max`), 0 for none, or -1 to stall.
int wl_xb_xid_control(uint32_t port, const WlXbUsbSetup *setup, const uint8_t *data, uint32_t data_len, uint8_t *out,
                      uint32_t max);
// A transfer on endpoint `ep` (not 0): IN returns the bytes written (0: NAK, nothing yet), OUT
// takes the bytes.
int wl_xb_xid_in(uint32_t port, uint8_t ep, uint8_t *out, uint32_t max);
void wl_xb_xid_out(uint32_t port, uint8_t ep, const uint8_t *data, uint32_t len);
// The bus reset of its port: address 0, not configured.
void wl_xb_xid_reset(uint32_t port);
uint8_t wl_xb_xid_address(uint32_t port);
uint8_t wl_xb_xid_configuration(uint32_t port);

#ifdef __cplusplus
}
#endif
