// SPDX-License-Identifier: MIT
// The HOME menu (home_menu.h) and the wrist strap reminder (strap_screen.h) joined to the console, for
// a program that wants both as they are: the sound fade (wl_platform_audio_fade), the leave
// (wl_platform_exit), and the frame overlay they are drawn with (wl_gx_set_overlay, hooked only while
// something is to be drawn, so a closed menu costs the frame nothing). The program calls
// wl_home_shell_poll each time it reads the controllers and gives the shell its few hooks: pause,
// reset, the facts of its world, whether it wants the reminder.
//
// The reminder, while it is up, takes the controllers: HOME does nothing under it. The program is to
// read wl_home_shell_blocks_input and show its game no controller while it is true, and
// wl_home_shell_open to stop rumble. Both may be read from any thread.
//
// The shell owns the overlay hook: a program that has an overlay of its own cannot use it.
#pragma once

#include <stdatomic.h>

#include "wl/base/base.h"
#include "wl/platform/home_menu.h"
#include "wl/platform/strap_screen.h"

typedef struct {
    void *user;
    // the program's time stopped, or running again (NULL: the program is never paused)
    void (*set_paused)(void *user, bool paused);
    // the reset item chosen (the program is running again first)
    void (*reset)(void *user);
    // the Homebrew Channel; NULL: wl_platform_exit(WL_EXIT_LOADER)
    void (*leave_for_loader)(void *user);
    // a line for the log; NULL: the WiiLibs log, at info
    void (*log)(void *user, const char *text);
    // The program's facts, asked for each look at the menu (not while the reminder is up). `world`
    // is cleared before. NULL: nothing is in play.
    void (*world)(void *user, const WlRawInput *raw, WlHomeWorld *world);
    // Whether the reminder is wanted, asked once, at the first poll (so that the program's settings
    // can be read late). NULL: not wanted.
    bool (*strap_enabled)(void *user);
} WlHomeShellOps;

typedef struct {
    WlHomeMenu menu;
    WlStrapScreen strap;
    WlHomeShellOps ops;
    WlHomeOps menu_ops;
    WlStrapConfig strap_config;
    bool started;
    // what has the overlay hooked in: the menu or a notice, the reminder
    bool menu_visible;
    bool strap_visible;
    bool hooked;
    atomic_int open_flag;
    atomic_int blocking_flag;
} WlHomeShell;

// `menu_config` and `strap_config` NULL for the defaults; their strings must outlive the shell. The
// shell is not to move or be copied once initialised (the overlay holds its address).
void wl_home_shell_init(WlHomeShell *shell, const WlHomeConfig *menu_config, const WlStrapConfig *strap_config,
                        const WlHomeShellOps *ops);

// Each time the program reads the controllers, from the thread that ends frames: `raw` as read (and
// changed by anything scripted), `extra_nav` the navigation of a controller the console does not
// have, `home_extra` a HOME press that is not on a controller.
void wl_home_shell_poll(WlHomeShell *shell, const WlRawInput *raw, uint64_t now_us, unsigned extra_nav,
                        bool home_extra);

// the menu or the reminder is up (any thread): no rumble
bool wl_home_shell_open(const WlHomeShell *shell);
// the program sees no controller: the menu or the reminder is up, or a button that closed it is still down
bool wl_home_shell_blocks_input(const WlHomeShell *shell);
