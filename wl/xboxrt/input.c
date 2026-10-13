// SPDX-License-Identifier: MIT
// The Xbox's controller ports (input.h). Everything here runs with the guest lock held (the
// vertical blank's work, a device access, the play harness's thread), so nothing more guards it.
#include <string.h>

#include "wl/platform/input_shape.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/input.h"

#define TAG "xbinput"

typedef struct {
    WlXbPortMode mode;
    bool script_on, host_pad_on;
    WlXboxPad script, host_pad; // the harness's part, a title's own map of the host's controllers
    WlXboxPad host;             // pad.h's map of the host's controllers, this frame
    bool host_present;          // a host controller of this number is there
    WlXboxPad state;            // what the reports carry
    uint32_t reports;
    uint16_t rumble[2];
    WlXboxRumble duty;
    bool motor_remote, motor_pad;
    WlPointer pointer;
} Port;

static Port g_ports[WL_XB_INPUT_PORTS];
static bool g_host_controllers;
static bool g_menu;

void wl_xb_input_reset(bool host_controllers) {
    memset(g_ports, 0, sizeof g_ports);
    g_host_controllers = host_controllers;
    g_menu = false;
}

void wl_xb_input_set_host_controllers(bool on) {
    g_host_controllers = on;
}

void wl_xb_input_set_script(uint32_t port, const WlXboxPad *pad) {
    if (port < WL_XB_INPUT_PORTS) {
        g_ports[port].script_on = pad != NULL;
        if (pad != NULL) {
            g_ports[port].script = *pad;
        }
    }
}

void wl_xb_input_set_host_pad(uint32_t port, const WlXboxPad *pad) {
    if (port < WL_XB_INPUT_PORTS) {
        g_ports[port].host_pad_on = pad != NULL;
        if (pad != NULL) {
            g_ports[port].host_pad = *pad;
        }
    }
}

void wl_xb_input_set_menu(bool menu) {
    g_menu = menu;
}

void wl_xb_input_set_mode(uint32_t port, WlXbPortMode mode) {
    if (port < WL_XB_INPUT_PORTS) {
        g_ports[port].mode = mode;
    }
}

WlXbPortMode wl_xb_input_mode(uint32_t port) {
    return port < WL_XB_INPUT_PORTS ? g_ports[port].mode : WL_XB_PORT_OFF;
}

bool wl_xb_input_connected(uint32_t port) {
    if (port >= WL_XB_INPUT_PORTS) {
        return false;
    }
    const Port *p = &g_ports[port];
    switch (p->mode) {
    case WL_XB_PORT_OFF:
        return false;
    case WL_XB_PORT_ON:
        return true;
    default:
        return port == 0 || p->host_present;
    }
}

static void compose(Port *p) {
    WlXboxPad s;
    memset(&s, 0, sizeof s);
    if (p->host_pad_on) {
        s = p->host_pad;
    } else if (p->host_present) {
        s = p->host;
    }
    if (p->script_on) {
        wl_xbox_pad_merge(&s, &p->script);
    }
    p->state = s;
}

void wl_xb_input_state(uint32_t port, WlXboxPad *out) {
    if (port >= WL_XB_INPUT_PORTS) {
        memset(out, 0, sizeof *out);
        return;
    }
    Port *p = &g_ports[port];
    compose(p); // the harness may have changed its part since the frame began
    *out = p->state;
}

void wl_xb_input_report_sent(uint32_t port) {
    if (port < WL_XB_INPUT_PORTS) {
        g_ports[port].reports++;
    }
}

uint32_t wl_xb_input_reports(uint32_t port) {
    return port < WL_XB_INPUT_PORTS ? g_ports[port].reports : 0;
}

void wl_xb_input_rumble(uint32_t port, uint16_t left, uint16_t right) {
    if (port >= WL_XB_INPUT_PORTS) {
        return;
    }
    Port *p = &g_ports[port];
    if (p->rumble[0] != left || p->rumble[1] != right) {
        WL_LOG_D(TAG, "port %u: rumble %u %u", (unsigned)port + 1u, (unsigned)left, (unsigned)right);
    }
    p->rumble[0] = left;
    p->rumble[1] = right;
}

void wl_xb_input_last_rumble(uint32_t port, uint16_t *left, uint16_t *right) {
    *left = port < WL_XB_INPUT_PORTS ? g_ports[port].rumble[0] : 0;
    *right = port < WL_XB_INPUT_PORTS ? g_ports[port].rumble[1] : 0;
}

void wl_xb_input_frame(void) {
    WlRawInput raw;
    memset(&raw, 0, sizeof raw);
    if (g_host_controllers) {
        wl_platform_input_poll(&raw);
    }
    uint64_t now = wl_platform_time_us();
    for (uint32_t i = 0; i < WL_XB_INPUT_PORTS; i++) {
        Port *p = &g_ports[i];
        WlXboxPad pad, more;
        memset(&pad, 0, sizeof pad);
        bool present = false;
        if (g_host_controllers && i < WL_INPUT_MAX_REMOTES && raw.remotes[i].connected) {
            const WlRawRemote *r = &raw.remotes[i];
            present = true;
            if (r->extension == WL_EXT_CLASSIC) {
                memset(&p->pointer, 0, sizeof p->pointer); // the Classic's sticks look, not the pointer
                wl_xbox_pad_from_classic(r, g_menu, &pad);
            } else {
                wl_pointer_update_steady(&p->pointer, r, now, WL_XBOX_POINTER_HOLD_US, WL_XBOX_POINTER_STEADINESS);
                wl_xbox_pad_from_remote(r, &p->pointer, WL_XBOX_POINTER_DEAD_ZONE, g_menu, &pad);
            }
        }
        if (g_host_controllers && i < WL_INPUT_MAX_PADS && raw.pads[i].connected) {
            present = true;
            wl_xbox_pad_from_gamecube(&raw.pads[i], g_menu, &more);
            wl_xbox_pad_merge(&pad, &more);
        }
        p->host = pad;
        p->host_present = present;
        compose(p);
        // the rumble: the stronger motor, duty-cycled onto the host controller of this number
        uint32_t strength = p->rumble[0] > p->rumble[1] ? p->rumble[0] : p->rumble[1];
        bool on = wl_xbox_rumble_duty(&p->duty, wl_xb_input_connected(i) ? strength : 0u, now);
        bool remote = on && i < WL_INPUT_MAX_REMOTES && raw.remotes[i].connected;
        bool gc = on && i < WL_INPUT_MAX_PADS && raw.pads[i].connected;
        if (g_host_controllers && remote != p->motor_remote) {
            wl_platform_rumble((uint8_t)i, remote);
            p->motor_remote = remote;
        }
        if (g_host_controllers && gc != p->motor_pad) {
            wl_platform_pad_rumble((uint8_t)i, gc);
            p->motor_pad = gc;
        }
    }
}
