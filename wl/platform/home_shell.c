// SPDX-License-Identifier: MIT
// The HOME menu and the strap reminder joined to the console: see home_shell.h.
#include "wl/platform/home_shell.h"

#include <string.h>

#include "wl/platform/overlay.h"
#include "wl/platform/platform.h"

// ---- the overlay

static void shell_draw(void *user, uint32_t width, uint32_t height) {
    WlHomeShell *shell = user;
    uint64_t now_us = wl_platform_time_us();

    wl_overlay_begin();
    if (shell->strap_visible) {
        wl_strap_screen_draw(&shell->strap, width, height, now_us);
    } else {
        wl_home_menu_draw(&shell->menu, width, height);
        wl_home_menu_draw_notices(&shell->menu, width, now_us);
    }
    wl_overlay_end();
}

// (nothing is hooked into the frame while there is nothing to draw)
static void overlay_update(WlHomeShell *shell) {
    bool wanted = shell->menu_visible || shell->strap_visible;

    if (wanted != shell->hooked) {
        shell->hooked = wanted;
        wl_gx_set_overlay(wanted ? shell_draw : NULL, shell);
    }
}

// ---- the menu's ops, forwarded

static void menu_set_paused(void *user, bool paused) {
    WlHomeShell *shell = user;

    if (shell->ops.set_paused != NULL) {
        shell->ops.set_paused(shell->ops.user, paused);
    }
}

static void menu_audio_fade(void *user, float gain, uint32_t milliseconds) {
    (void)user;
    wl_platform_audio_fade(gain, milliseconds);
}

static void menu_reset(void *user) {
    WlHomeShell *shell = user;

    if (shell->ops.reset != NULL) {
        shell->ops.reset(shell->ops.user);
    }
}

static void menu_leave(void *user) {
    WlHomeShell *shell = user;

    if (shell->ops.leave_for_loader != NULL) {
        shell->ops.leave_for_loader(shell->ops.user);
    } else {
        wl_platform_exit(WL_EXIT_LOADER);
    }
}

static void menu_visible(void *user, bool visible) {
    WlHomeShell *shell = user;

    shell->menu_visible = visible;
    overlay_update(shell);
}

static void menu_log(void *user, const char *text) {
    WlHomeShell *shell = user;

    if (shell->ops.log != NULL) {
        shell->ops.log(shell->ops.user, text);
    } else {
        WL_LOG_I("home", "%s", text);
    }
}

void wl_home_shell_init(WlHomeShell *shell, const WlHomeConfig *menu_config, const WlStrapConfig *strap_config,
                        const WlHomeShellOps *ops) {
    memset(shell, 0, sizeof *shell);
    wl_home_menu_init(&shell->menu, menu_config);
    shell->strap_config = strap_config != NULL ? *strap_config : wl_strap_config_default();
    if (ops != NULL) {
        shell->ops = *ops;
    }
    shell->menu_ops.user = shell;
    shell->menu_ops.set_paused = menu_set_paused;
    shell->menu_ops.audio_fade = menu_audio_fade;
    shell->menu_ops.reset = menu_reset;
    shell->menu_ops.leave_for_loader = menu_leave;
    shell->menu_ops.visible = menu_visible;
    shell->menu_ops.log = menu_log;
    atomic_init(&shell->open_flag, 0);
    atomic_init(&shell->blocking_flag, 0);
}

// ---- the poll

void wl_home_shell_poll(WlHomeShell *shell, const WlRawInput *raw, uint64_t now_us, unsigned extra_nav,
                        bool home_extra) {
    WlHomeWorld world;
    bool strap_blocks = false;

    if (!shell->started) {
        shell->started = true;
        wl_strap_screen_init(&shell->strap, &shell->strap_config,
                             shell->ops.strap_enabled != NULL && shell->ops.strap_enabled(shell->ops.user));
    }
    // (the reminder, while it is up, takes the controllers: HOME does nothing under it, and the
    // program sees nothing)
    if (wl_strap_screen_active(&shell->strap)) {
        bool up = wl_strap_screen_step(&shell->strap, raw, extra_nav, now_us);

        if (up != shell->strap_visible) {
            shell->strap_visible = up;
            overlay_update(shell);
        }
        strap_blocks = wl_strap_screen_blocks_input(&shell->strap);
        if (up) {
            atomic_store(&shell->open_flag, 1);
            atomic_store(&shell->blocking_flag, 1);
            return;
        }
    }
    memset(&world, 0, sizeof world);
    if (shell->ops.world != NULL) {
        shell->ops.world(shell->ops.user, raw, &world);
    }
    wl_home_menu_step(&shell->menu, raw, extra_nav, home_extra, &world, now_us, &shell->menu_ops);
    atomic_store(&shell->open_flag, wl_home_menu_open(&shell->menu) ? 1 : 0);
    atomic_store(&shell->blocking_flag, (wl_home_menu_blocks_input(&shell->menu) || strap_blocks) ? 1 : 0);
}

bool wl_home_shell_open(const WlHomeShell *shell) {
    return atomic_load(&shell->open_flag) != 0;
}

bool wl_home_shell_blocks_input(const WlHomeShell *shell) {
    return atomic_load(&shell->blocking_flag) != 0;
}
