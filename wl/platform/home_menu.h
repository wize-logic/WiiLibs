// SPDX-License-Identifier: MIT
// The HOME button's menu and the Wii's notices (a Remote with nothing to move with, a weak battery)
// as a state machine apart from the program, the console and the screen, so that host tests run it
// against fake controllers and a clock. It is given the controllers' state each time the program
// reads them, and tells its ops what to do: pause the program, fade the sound, reset, leave. The
// drawing through the frame overlay (home_menu_draw.c) is a separate file; home_shell.h joins the
// machine, the strap screen and the overlay to the console for a program that wants all of it.
//
// HOME (a Wii Remote's, or a Classic Controller's own) opens a small menu over the program: Resume,
// Reset (optional: only while the program is `in_play`, and only when the configuration gives it a
// label) and Return to the loader (asked about first). While it is up the program is paused (unless
// it was paused already), the sound fades out, and the program should be shown no controller
// (wl_home_menu_blocks_input); HOME again, or B, closes it. The GameCube controller has no HOME
// button. The menu is steered by whichever controller has a hand on it: the D-pad or the stick, A to
// choose, B to go back (the Classic Controller's two buttons swapped if the configuration says).
//
// The notices need no menu: a Remote with no Nunchuk or Classic Controller on a player in play
// cannot move him, and a Remote's battery has fallen low. Each is given once (the first again only
// when the Remote has had something else to move with, the second when the battery has been good
// again) and stays on the screen a few seconds.
#pragma once

#include "wl/base/base.h"
#include "wl/platform/platform.h"

#define WL_HOME_PORTS 4

typedef enum { WL_HOME_CLOSED, WL_HOME_MENU, WL_HOME_CONFIRM } WlHomeState;

typedef enum {
    WL_HOME_ITEM_RESUME,
    WL_HOME_ITEM_RESET,
    WL_HOME_ITEM_LOADER,
    WL_HOME_ITEMS
} WlHomeItem;

// the confirmation before the loader: what is chosen
typedef enum { WL_HOME_CHOICE_CANCEL, WL_HOME_CHOICE_LEAVE, WL_HOME_CHOICES } WlHomeChoice;

typedef enum { WL_HOME_NOTICE_BATTERY, WL_HOME_NOTICE_NO_EXTENSION, WL_HOME_NOTICES } WlHomeNotice;

// the navigation a controller gives the menu
enum { WL_HOME_NAV_UP = 1, WL_HOME_NAV_DOWN = 2, WL_HOME_NAV_ACCEPT = 4, WL_HOME_NAV_BACK = 8 };

// Everything a program may set. Strings are not copied: they must outlive the menu. Text is drawn in
// overlay.h's font (capitals, digits and a little punctuation; lower case is drawn as capitals):
// wl_overlay_text_drawable checks a string against it. In the two notice templates `{port}` is the
// Remote's number (1 to 4) and `{percent}` the battery's level; a template may leave either out.
typedef struct {
    const char *title;             // the menu's heading
    const char *resume_label;
    const char *reset_label;       // NULL: no reset item
    const char *loader_label;
    const char *confirm_text;      // the warning above the choices before the loader
    const char *cancel_label;      // the confirmation's other choice; the first is loader_label
    const char *batteries_heading; // above the Remotes' gauges
    const char *remote_label;      // beside each gauge: "REMOTE {port}"
    const char *hint_menu;         // the line at the foot of the menu, and of the confirmation
    const char *hint_confirm;
    const char *notice_battery;      // NULL: no notice for a weak battery
    const char *notice_no_extension; // NULL: no notice for a Remote with nothing to move with

    // false: the Classic Controller's lower right button (A) accepts and its right (B) goes back, as
    // the Wii's menus have them; true: swapped, as the layout of a Xbox-style game's menus has them
    bool classic_swap;
    // a stick counts as a press of up or down beyond this (of 127); 0 for the default, 64
    int stick_threshold;

    // 0 for the default of each
    uint32_t fade_ms;          // sound out and back in: 300
    uint32_t no_extension_us;  // a Remote with nothing to move with is told so after this long (a
                               // Nunchuk plugged in takes a moment to be seen): 1.5 s
    uint32_t notice_us;        // a notice stays this long: 7 s
    uint32_t battery_check_us; // batteries are looked at this often, not each frame: 2 s
    uint32_t swallow_us;       // buttons held when the menu closes do nothing in the program until
                               // they are let go, or this long: 1.5 s
    // a Remote's battery below `battery_low` percent is low, and at `battery_good` or above is good
    // again (the notice is given once per fall): 10 and 15
    uint8_t battery_low;
    uint8_t battery_good;
} WlHomeConfig;

// The English defaults: every text, no reset item, the Wii's own Classic Controller buttons, the
// times above.
WlHomeConfig wl_home_config_default(void);

// what the machine asks of the rest of the program; each may be NULL
typedef struct {
    void *user;
    // the program's time stopped, or running again
    void (*set_paused)(void *user, bool paused);
    // the sound's master gain (0 to 1) reached over the time
    void (*audio_fade)(void *user, float gain, uint32_t milliseconds);
    // the reset item chosen (the menu has closed and resumed the program first)
    void (*reset)(void *user);
    // the Homebrew Channel (does not return on the console)
    void (*leave_for_loader)(void *user);
    // something is to be drawn, or nothing is any more
    void (*visible)(void *user, bool visible);
    // a line for the log
    void (*log)(void *user, const char *text);
} WlHomeOps;

// what only the program knows
typedef struct {
    // something is running that can be paused and reset (not a menu of its own)
    bool in_play;
    // its time is already stopped (a pause menu of its own)
    bool paused;
    // a player is on the controller of that port
    bool local_player[WL_HOME_PORTS];
} WlHomeWorld;

typedef struct {
    WlHomeConfig config; // as given, with the defaults put in for the zeros
    WlHomeState state;
    int items[WL_HOME_ITEMS]; // WlHomeItem
    int item_count;
    // the item chosen, or the choice while confirming
    int selection;
    unsigned previous_nav;
    bool previous_home;
    bool we_paused;
    bool swallowing;
    uint64_t swallow_since_us;
    bool shown;

    // the controllers as of the last look (for the menu's battery list)
    bool present[WL_HOME_PORTS];
    uint8_t extension[WL_HOME_PORTS];
    uint8_t battery[WL_HOME_PORTS];

    // notices
    uint64_t next_battery_check_us;
    bool battery_known[WL_HOME_PORTS];
    bool battery_told[WL_HOME_PORTS];
    uint64_t no_extension_since_us[WL_HOME_PORTS];
    bool no_extension_told[WL_HOME_PORTS];
    uint64_t notice_until_us[WL_HOME_NOTICES][WL_HOME_PORTS];
    uint8_t notice_percent[WL_HOME_PORTS];
} WlHomeMenu;

// `config` NULL for the defaults.
void wl_home_menu_init(WlHomeMenu *menu, const WlHomeConfig *config);

// One look at the controllers: `raw` is what wl_platform_input_poll read, `extra_nav` the navigation
// of a controller the console does not have (a scripted one), `home_extra` a HOME press that is not
// on a controller, `world` what the program says; `ops` may be NULL.
void wl_home_menu_step(WlHomeMenu *menu, const WlRawInput *raw, unsigned extra_nav, bool home_extra,
                       const WlHomeWorld *world, uint64_t now_us, const WlHomeOps *ops);

// the menu is up: the program sees no controller, rumble is off
bool wl_home_menu_open(const WlHomeMenu *menu);
// and for a moment after, while a button held for the menu is let go
bool wl_home_menu_blocks_input(const WlHomeMenu *menu);

// What the controllers give the menu (an OR over all of them), with the configuration's Classic
// Controller buttons and stick threshold.
unsigned wl_home_navigation(const WlRawInput *raw, const WlHomeConfig *config);
// whether a controller's HOME is down
bool wl_home_pressed(const WlRawInput *raw);

// The words of the menu, as configured ("" for a reset item the configuration does not have).
const char *wl_home_menu_item_label(const WlHomeMenu *menu, int item);
const char *wl_home_menu_choice_label(const WlHomeMenu *menu, int choice);
const char *wl_home_menu_confirm_text(const WlHomeMenu *menu);
// The notices still showing at that time, a line each, with a newline after each; the length of the
// text (0 for none).
unsigned wl_home_menu_notice_text(const WlHomeMenu *menu, uint64_t now_us, char *text, unsigned size);

// ---- drawing (home_menu_draw.c): the calls of overlay.h only, between the caller's wl_overlay_begin
// and wl_overlay_end, for a frame `width` by `height`

// the menu when it is up, over a dimmed picture, with the batteries
void wl_home_menu_draw(const WlHomeMenu *menu, uint32_t width, uint32_t height);
// the notices still showing, in a box at the top
void wl_home_menu_draw_notices(const WlHomeMenu *menu, uint32_t width, uint64_t now_us);
