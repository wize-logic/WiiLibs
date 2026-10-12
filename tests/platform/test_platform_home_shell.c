// SPDX-License-Identifier: MIT
// The HOME menu and the strap reminder joined to the console (wl/platform/home_shell.h) on the host
// backend: the overlay hooked only while something is drawn (a frame's quads show it), the sound faded
// through wl_platform_audio_fade, the program's hooks (pause, reset, leave, world, the reminder's
// setting) called as the machines ask, the reminder taking the controllers before the menu, and the
// flags a rumble thread reads.
#include <string.h>

#include "greatest.h"
#include "wl/platform/audio_gain.h"
#include "wl/platform/home_shell.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/overlay.h"
#include "wl/platform/platform.h"

static struct {
    int paused_calls, resumed_calls;
    bool paused;
    int resets, leaves;
    int worlds;
    int strap_asked;
    bool strap_wanted;
    bool in_play;
    int logs;
} g_game;

static WlHomeShell g_shell;
static WlRawInput g_raw;
static uint64_t g_now;
static WlHostGxDraw g_draws[64];

static void game_set_paused(void *user, bool paused) {
    (void)user;
    g_game.paused = paused;
    if (paused) {
        g_game.paused_calls++;
    } else {
        g_game.resumed_calls++;
    }
}

static void game_reset(void *user) {
    (void)user;
    g_game.resets++;
}

static void game_leave(void *user) {
    (void)user;
    g_game.leaves++;
}

static void game_log(void *user, const char *text) {
    (void)user;
    (void)text;
    g_game.logs++;
}

static void game_world(void *user, const WlRawInput *raw, WlHomeWorld *world) {
    (void)user;
    (void)raw;
    g_game.worlds++;
    world->in_play = g_game.in_play;
    world->local_player[0] = true;
}

static bool game_strap(void *user) {
    (void)user;
    g_game.strap_asked++;
    return g_game.strap_wanted;
}

static const WlHomeShellOps g_ops = {NULL, game_set_paused, game_reset, game_leave, game_log, game_world, game_strap};

static void start(bool strap_wanted) {
    WlHomeConfig menu = wl_home_config_default();
    WlStrapConfig strap = wl_strap_config_default();

    memset(&g_game, 0, sizeof g_game);
    g_game.in_play = true;
    g_game.strap_wanted = strap_wanted;
    memset(&g_raw, 0, sizeof g_raw);
    g_raw.remotes[0].connected = true;
    g_raw.remotes[0].extension = WL_EXT_NUNCHUK;
    g_raw.remotes[0].battery = 80;
    g_now = 1000000;
    menu.reset_label = "Reset";
    wl_gx_set_overlay(NULL, NULL);
    (void)wl_gx_init();
    wl_audio_gain_request(wl_audio_gain_master(), 1.0f, 0);
    wl_home_shell_init(&g_shell, &menu, &strap, &g_ops);
    memset(g_draws, 0, sizeof g_draws);
    wl_host_gx_record(g_draws, 64);
}

static void finish(void) {
    wl_gx_set_overlay(NULL, NULL);
    wl_host_gx_record(NULL, 0);
    wl_gx_shutdown();
}

static void look(void) {
    g_now += 33000;
    wl_home_shell_poll(&g_shell, &g_raw, g_now, 0, false);
}

static void looks(int count) {
    while (count-- > 0) {
        look();
    }
}

static void press(uint32_t buttons) {
    g_raw.remotes[0].buttons = buttons;
    looks(2);
    g_raw.remotes[0].buttons = 0;
    looks(2);
}

// the quads one frame draws (the overlay's, if it is hooked)
static unsigned frame_quads(void) {
    unsigned quads = 0;

    memset(g_draws, 0, sizeof g_draws);
    wl_host_gx_record(g_draws, 64);
    wl_gx_begin_frame(0);
    wl_gx_end_frame();
    for (uint32_t i = 0; i < wl_host_gx_recorded() && i < 64u; i++) {
        quads += g_draws[i].count / 4u;
    }
    return quads;
}

TEST nothing_is_hooked_or_drawn_while_the_menu_is_closed(void) {
    start(false);
    looks(200);
    ASSERT_EQ(0u, frame_quads());
    ASSERT(!wl_home_shell_open(&g_shell));
    ASSERT(!wl_home_shell_blocks_input(&g_shell));
    ASSERT_EQ(0, g_game.paused_calls);
    ASSERT_EQ(1, g_game.strap_asked); // once
    ASSERT(g_game.worlds > 0);
    finish();
    PASS();
}

TEST home_opens_the_menu_pauses_fades_and_draws_then_closes_it(void) {
    start(false);
    g_raw.remotes[0].buttons = WL_WM_HOME;
    looks(1);
    ASSERT(wl_home_shell_open(&g_shell));
    ASSERT(wl_home_shell_blocks_input(&g_shell));
    ASSERT(g_game.paused);
    ASSERT_EQ(1, g_game.paused_calls);
    ASSERT_EQ(0.0f, wl_audio_gain_requested(wl_audio_gain_master()));
    ASSERTm("the menu is drawn over the frame", frame_quads() > 300u);
    g_raw.remotes[0].buttons = 0;
    looks(2);
    g_raw.remotes[0].buttons = WL_WM_HOME;
    looks(1);
    ASSERT(!wl_home_shell_open(&g_shell));
    ASSERT(!g_game.paused);
    ASSERT_EQ(1.0f, wl_audio_gain_requested(wl_audio_gain_master()));
    ASSERTm("and the hook is let go: the frame is free again", frame_quads() == 0u);
    ASSERTm("the button that closed it is kept from the program", wl_home_shell_blocks_input(&g_shell));
    g_raw.remotes[0].buttons = 0;
    looks(1);
    ASSERT(!wl_home_shell_blocks_input(&g_shell));
    ASSERT(g_game.logs >= 2);
    finish();
    PASS();
}

TEST the_reset_and_the_loader_are_the_programs_hooks(void) {
    start(false);
    press(WL_WM_HOME);
    press(WL_WM_DOWN);
    press(WL_WM_A);
    ASSERT_EQ(1, g_game.resets);
    ASSERT_EQ(1, g_game.resumed_calls);
    ASSERT(!wl_home_shell_open(&g_shell));

    start(false);
    g_game.in_play = false;
    press(WL_WM_HOME);
    press(WL_WM_DOWN); // (no reset item with nothing in play: the loader)
    press(WL_WM_A);
    press(WL_WM_DOWN); // (Cancel is chosen: down is the loader)
    press(WL_WM_A);
    ASSERT_EQ(1, g_game.leaves);
    ASSERT_EQ(0, g_game.resets);
    finish();
    PASS();
}

TEST the_reminder_comes_first_takes_the_controllers_and_goes_on_with_a(void) {
    start(true);
    look();
    ASSERT_EQ(1, g_game.strap_asked);
    ASSERT(wl_home_shell_open(&g_shell));
    ASSERT(wl_home_shell_blocks_input(&g_shell));
    ASSERTm("the screen is drawn", frame_quads() > 300u);
    // HOME does nothing under it, and the program's world is not asked
    press(WL_WM_HOME);
    ASSERT(!g_game.paused);
    ASSERT_EQ(0, g_game.worlds);
    // A in the first second does nothing
    press(WL_WM_A);
    ASSERT(wl_home_shell_blocks_input(&g_shell));
    ASSERT(frame_quads() > 0u);
    g_now += 1000000u;
    g_raw.remotes[0].buttons = WL_WM_A;
    looks(1);
    ASSERTm("A after the first second goes on", !wl_home_shell_open(&g_shell));
    ASSERTm("the A is kept from the program", wl_home_shell_blocks_input(&g_shell));
    ASSERTm("and the hook is let go", frame_quads() == 0u);
    g_raw.remotes[0].buttons = 0;
    looks(1);
    ASSERT(!wl_home_shell_blocks_input(&g_shell));
    // now HOME opens the menu
    press(WL_WM_HOME);
    ASSERT_EQ(1, g_game.paused_calls);
    ASSERT_EQ(1, g_game.strap_asked); // (never asked again)
    finish();
    PASS();
}

TEST a_shell_with_no_hooks_runs_on_its_defaults(void) {
    start(false);
    wl_home_shell_init(&g_shell, NULL, NULL, NULL);
    g_raw.remotes[0].buttons = WL_WM_HOME;
    looks(2);
    ASSERT(wl_home_shell_open(&g_shell)); // nothing in play: only Resume and the loader
    ASSERT(frame_quads() > 300u);
    g_raw.remotes[0].buttons = 0;
    looks(2);
    press(WL_WM_A);
    ASSERT(!wl_home_shell_open(&g_shell));
    finish();
    PASS();
}

SUITE(home_shell_suite) {
    RUN_TEST(nothing_is_hooked_or_drawn_while_the_menu_is_closed);
    RUN_TEST(home_opens_the_menu_pauses_fades_and_draws_then_closes_it);
    RUN_TEST(the_reset_and_the_loader_are_the_programs_hooks);
    RUN_TEST(the_reminder_comes_first_takes_the_controllers_and_goes_on_with_a);
    RUN_TEST(a_shell_with_no_hooks_runs_on_its_defaults);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(home_shell_suite);
    GREATEST_MAIN_END();
}
