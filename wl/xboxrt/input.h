// SPDX-License-Identifier: MIT
// The Xbox's four controller ports, as the title's USB stack finds them (usb.h's gamepads report
// what is here). Each port has a pad state made of what the host's controllers press (the Wii's
// Remotes, Nunchuks, Classic Controllers and GameCube controllers, mapped by pad.h) and what the
// play harness presses (play.h), and a connection: port 1 always has a controller in it, the
// others while the host has one there, unless the harness says otherwise. Rumble from the title
// comes back here and goes to the host controller of the same number.
//
// wl_xb_input_frame runs at each vertical blank (the runtime calls it, lock held): it polls the
// host's controllers (once a frame, as wl_platform_input_poll wants) and makes the states the
// gamepads report until the next one.
#pragma once

#include "wl/base/base.h"
#include "wl/xboxrt/pad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_XB_INPUT_PORTS 4u

typedef enum {
    WL_XB_PORT_AUTO = 0, // port 1: connected; the others while the host has a controller there
    WL_XB_PORT_OFF,      // unplugged, whatever the host has
    WL_XB_PORT_ON,       // plugged in (nothing pressed unless something presses it)
} WlXbPortMode;

// Back to the start: every port AUTO, nothing pressed, no rumble. The host's controllers are
// read when `host_controllers` (the Wii by default; the host's stub has none).
void wl_xb_input_reset(bool host_controllers);
void wl_xb_input_set_host_controllers(bool on);

// The harness's part of a port's state (NULL: none), laid over the host's (wl_xbox_pad_merge).
void wl_xb_input_set_script(uint32_t port, const WlXboxPad *pad);
// The host's part, when a title maps the host's controllers itself (NULL: back to pad.h's maps).
void wl_xb_input_set_host_pad(uint32_t port, const WlXboxPad *pad);
// Whether the title's menus are up (pad.h's maps give some buttons a second job only in play).
void wl_xb_input_set_menu(bool menu);
void wl_xb_input_set_mode(uint32_t port, WlXbPortMode mode);
WlXbPortMode wl_xb_input_mode(uint32_t port);

// Whether a controller is in the port now, and its state (what the next report carries).
bool wl_xb_input_connected(uint32_t port);
void wl_xb_input_state(uint32_t port, WlXboxPad *out);

// The gamepads' side (usb.h): a report was handed to the title (counted: a press is held for a few
// of them), the title asked for rumble (0..65535 each motor).
void wl_xb_input_report_sent(uint32_t port);
uint32_t wl_xb_input_reports(uint32_t port);
void wl_xb_input_rumble(uint32_t port, uint16_t left, uint16_t right);
void wl_xb_input_last_rumble(uint32_t port, uint16_t *left, uint16_t *right);

// Once per vertical blank (lock held): polls the host's controllers, composes each port's state
// and drives the host's rumble motors.
void wl_xb_input_frame(void);

#ifdef __cplusplus
}
#endif
