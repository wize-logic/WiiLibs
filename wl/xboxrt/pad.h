// SPDX-License-Identifier: MIT
// The Xbox gamepad as plain data (WlXboxPad), its 20-byte USB input report, its rumble report,
// and the Wii's controllers mapped onto it. Pure C: no guest memory, no libogc, no byte-order
// assumption, so the host and ppc-be tests run all of it.
//
// The layout is the console's (the XDK's XINPUT_GAMEPAD, which the controller's report carries
// byte for byte): a digital word (the D-pad, Start, Back, the stick clicks), eight analogue buttons
// 0..255 (A, B, X, Y, Black, White, the left and right triggers) and four signed 16-bit stick axes
// (left x, left y, right x, right y; y up).
//
// The Wii mappings (wl_xbox_pad_from_*) are OpenCE-Wii's (port/wii/src/xinput_wii_map.c, CC0),
// which follow Halo's default layout; they suit any title that uses the pad the same way, and a
// title with other needs maps WlRawInput itself and hands the result to wl_xb_input_set_host_pad.
#pragma once

#include "wl/base/base.h"
#include "wl/platform/input_shape.h"
#include "wl/platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// digital buttons (WlXboxPad.buttons)
#define WL_XBOX_PAD_UP 0x0001u
#define WL_XBOX_PAD_DOWN 0x0002u
#define WL_XBOX_PAD_LEFT 0x0004u
#define WL_XBOX_PAD_RIGHT 0x0008u
#define WL_XBOX_PAD_START 0x0010u
#define WL_XBOX_PAD_BACK 0x0020u
#define WL_XBOX_PAD_LTHUMB 0x0040u
#define WL_XBOX_PAD_RTHUMB 0x0080u

// analogue buttons (indexes into WlXboxPad.analog)
enum {
    WL_XBOX_PAD_A = 0,
    WL_XBOX_PAD_B,
    WL_XBOX_PAD_X,
    WL_XBOX_PAD_Y,
    WL_XBOX_PAD_BLACK,
    WL_XBOX_PAD_WHITE,
    WL_XBOX_PAD_LT,
    WL_XBOX_PAD_RT,
    WL_XBOX_PAD_ANALOG_COUNT
};

// stick axes (indexes into WlXboxPad.thumb)
enum { WL_XBOX_PAD_LX = 0, WL_XBOX_PAD_LY, WL_XBOX_PAD_RX, WL_XBOX_PAD_RY };

#define WL_XBOX_PAD_REPORT_BYTES 20u
#define WL_XBOX_PAD_RUMBLE_BYTES 6u

typedef struct {
    uint16_t buttons;                           // WL_XBOX_PAD_UP...
    uint8_t analog[WL_XBOX_PAD_ANALOG_COUNT];   // 0..255
    int16_t thumb[4];                           // -32768..32767, y up
} WlXboxPad;

// Equal states (memcmp would do, but the struct may have padding).
bool wl_xbox_pad_equal(const WlXboxPad *a, const WlXboxPad *b);
// `over` laid on `base`: buttons or'ed, each analogue button the larger, each axis `over`'s
// unless it is 0.
void wl_xbox_pad_merge(WlXboxPad *base, const WlXboxPad *over);

// The controller's input report: 0, 20, the digital byte, 0, the eight analogue bytes, the four
// axes little-endian. Returns 20.
uint32_t wl_xbox_pad_report(const WlXboxPad *pad, uint8_t out[WL_XBOX_PAD_REPORT_BYTES]);
// The other way (the tests, and a title reading a report back). false for a report that is not
// one (too short, a wrong length byte).
bool wl_xbox_pad_parse(const uint8_t *report, uint32_t bytes, WlXboxPad *out);

// The rumble (output) report: 0, 6, the left motor's speed, the right's (16 bits, little-endian).
// false when `bytes` is short or the length byte is not 6.
bool wl_xbox_rumble_parse(const uint8_t *report, uint32_t bytes, uint16_t *left, uint16_t *right);

// ---- The Wii's controllers -------------------------------------------------------------------
// Each fills a zeroed pad. `menu`: the title's menus are up, so the second jobs some buttons have
// in play (the D-pad's clicks, the Remote's B as the trigger) are left out and B is the menus'
// back button.

// The GameCube controller: A, B, X, Y as the Xbox's; the C stick looks; L and R are the analogue
// triggers; Z is Black; in play the D-pad's up also clicks the right stick, down the left, left is
// Back and right White.
void wl_xbox_pad_from_gamecube(const WlRawPad *pad, bool menu, WlXboxPad *out);
// The Classic Controller (and the Pro): its buttons where the Xbox's are by position (b is A, a
// B, y X, x Y), ZL and ZR Black and White, L and R the triggers, + Start, - Back; in play the
// D-pad's up also clicks the right stick and down the left.
void wl_xbox_pad_from_classic(const WlRawRemote *remote, bool menu, WlXboxPad *out);
// The Wii Remote with a Nunchuk (without one, no left stick): the Nunchuk's stick moves, A is A,
// 1 is B, B the right trigger (B, back, in the menus), Z the left trigger, C clicks the left stick,
// 2 the right, + Start, - Back; in play the D-pad's up is also Y, down X, left Black, right White.
// The pointer (`pointer`, tracked: wl_pointer_update_steady) is the right stick: nothing within
// `dead_zone` of the screen's middle, then a ramp to full at the edge (wl_pointer_turn).
void wl_xbox_pad_from_remote(const WlRawRemote *remote, const WlPointer *pointer, float dead_zone, bool menu,
                             WlXboxPad *out);
#define WL_XBOX_POINTER_DEAD_ZONE 0.35f
#define WL_XBOX_POINTER_HOLD_US 150000u
#define WL_XBOX_POINTER_STEADINESS 0.03f

// ---- Rumble on a motor that is on or off -----------------------------------------------------
// The Xbox has two motors of 16-bit speed; a Wii controller's one is on or off. The strength is the
// stronger of the two, and the motor is switched from tick to tick so that it is on for that
// fraction of the ticks (what was owed carries over), never switched again within
// WL_XBOX_RUMBLE_SWITCH_US of its last switch; 0 stops it at once. (OpenCE-Wii's wii_rumble_duty.)
#define WL_XBOX_RUMBLE_FULL 65535u
#define WL_XBOX_RUMBLE_SWITCH_US 10000u
typedef struct {
    int32_t carry;
    bool on;
    uint64_t switched_us;
} WlXboxRumble;
bool wl_xbox_rumble_duty(WlXboxRumble *r, uint32_t strength, uint64_t now_us);

#ifdef __cplusplus
}
#endif
