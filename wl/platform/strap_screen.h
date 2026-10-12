// SPDX-License-Identifier: MIT
// A wrist strap reminder as a state machine apart from the program, the console and the screen: a
// plain screen shown at the start of a program, for the Wii Remote's strap and the room to swing it.
// It is given the controllers each time the program reads them. At the start it shows the screen,
// ignoring the controllers for the first second (a button held from the loader does nothing), and
// goes on when A is pressed after that; the program is given no controller while the screen is up,
// and until the A that went on is let go. Drawn with the frame overlay (strap_screen_draw.c); the
// controllers' A is home_menu.h's (wl_home_navigation).
#pragma once

#include "wl/base/base.h"
#include "wl/platform/home_menu.h"
#include "wl/platform/platform.h"

#define WL_STRAP_MAX_LINES 6

typedef enum { WL_STRAP_OFF, WL_STRAP_WAITING, WL_STRAP_UP, WL_STRAP_SWALLOWING, WL_STRAP_DONE } WlStrapState;

typedef struct {
    const char *text;       // NULL ends the list
    bool with_nunchuk_only; // shown only while a Nunchuk is connected
} WlStrapLine;

// Strings are not copied: they must outlive the screen. Text is drawn in overlay.h's font (capitals,
// numbers and a little punctuation: wl_overlay_text_drawable checks a string against it); the lines
// are one row each.
typedef struct {
    const char *title;
    WlStrapLine body[WL_STRAP_MAX_LINES];
    const char *prompt; // drawn dim until the screen is ready for A, then lit
    // as WlHomeConfig's: the Classic Controller's buttons swapped
    bool classic_swap;
    // 0 for the default of each
    uint32_t ignore_us;  // the controllers are ignored for this long after the screen first shows: 1 s
    uint32_t swallow_us; // the A that goes on is kept from the program until it is let go, or this long: 1.5 s
} WlStrapConfig;

// The English defaults: the title, four lines (the third for a Nunchuk) and "A: CONTINUE".
WlStrapConfig wl_strap_config_default(void);

typedef struct {
    WlStrapConfig config;    // as given, with the defaults put in
    WlHomeConfig navigation; // what the controllers' navigation reads
    WlStrapState state;
    uint64_t shown_us;
    uint64_t gone_us;
    bool accept_was;
    // a Nunchuk is connected, as of the last look
    bool nunchuk;
} WlStrapScreen;

// `enabled` is the program's setting: when false the screen never shows. It shows at the first look.
// `config` NULL for the defaults.
void wl_strap_screen_init(WlStrapScreen *strap, const WlStrapConfig *config, bool enabled);

// One look at the controllers (`raw` is what wl_platform_input_poll read; `extra_nav` the navigation
// of a scripted controller, home_menu.h). Returns whether the screen is up.
bool wl_strap_screen_step(WlStrapScreen *strap, const WlRawInput *raw, unsigned extra_nav, uint64_t now_us);

// the screen is up: the program sees no controller and no rumble
bool wl_strap_screen_up(const WlStrapScreen *strap);
// and for a moment after, while the A that went on is let go
bool wl_strap_screen_blocks_input(const WlStrapScreen *strap);
// A would go on now (the first second is over)
bool wl_strap_screen_ready(const WlStrapScreen *strap, uint64_t now_us);
// The screen has started and is not finished: waiting for its first look, up, or letting go of the A.
bool wl_strap_screen_active(const WlStrapScreen *strap);

// The words: the title; the body lines that show now (index 0 up, NULL past the last: a line kept
// for a Nunchuk is not counted while none is connected); the prompt.
const char *wl_strap_screen_title(const WlStrapScreen *strap);
const char *wl_strap_screen_line(const WlStrapScreen *strap, int index);
const char *wl_strap_screen_prompt(const WlStrapScreen *strap);

// ---- drawing (strap_screen_draw.c): the calls of overlay.h only, between the caller's
// wl_overlay_begin and wl_overlay_end: a screen of black with the words
void wl_strap_screen_draw(const WlStrapScreen *strap, uint32_t width, uint32_t height, uint64_t now_us);
