// SPDX-License-Identifier: MIT
// The play harness (wl/xboxrt/play.h) against a pretend title: its registrations (a screen and a
// focus that follow the D-pad and A, a tick that runs once a map is loaded, checkpoints, a load
// action, a predicate, a long step that drives the controller), a script of every kind of step run
// from the title's frame point under the guest lock, the controller ports it sets (input.h), and
// the "test_input:" lines it logs. Also the parser's errors and the bot's determinism. Host and
// ppc-be.
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/platform/host/fs_host.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/play.h"
#include "wl/xboxrt/thread.h"

#define MEM_VA 0x00500000u

static struct {
    char log[256][200];
    int log_count;
    bool finished, stopped, memory_seen, step_done;
    char loaded[64];
    int focus;
    bool menu;
    int64_t tick, checkpoints;
    bool step_ran, fast;
    uint32_t step_frames;
    bool saw_rt, saw_stick, saw_x, port2_b, port2_bot, saw_walk_a;
    WlXboxPad last[4];
} T;

static WlPlatformMutex g_log_m;

static void sink(void *user, WlLogLevel level, const char *tag, const char *line) {
    (void)user;
    (void)level;
    if (strcmp(tag, "play") != 0) {
        return;
    }
    fprintf(stderr, "  %s\n", line);
    wl_platform_mutex_lock(g_log_m);
    if (strstr(line, "script finished") != NULL) {
        T.finished = true;
    }
    if (strstr(line, "script stopped") != NULL) {
        T.stopped = true;
    }
    if (strstr(line, "memory 0x00500000 = 0x7") != NULL) {
        T.memory_seen = true;
    }
    if (strstr(line, "walkto done") != NULL) {
        T.step_done = true;
    }
    if (strstr(line, "test_input: twice") != NULL) {
        T.fast = true; // the game's minutes go by quickly from here
    }
    if (T.log_count < 256) {
        snprintf(T.log[T.log_count++], sizeof T.log[0], "%s", line);
    }
    wl_platform_mutex_unlock(g_log_m);
}

static bool t_screen(void *user, char *out, size_t cap) {
    (void)user;
    if (!T.menu) {
        return false;
    }
    snprintf(out, cap, "ui\\shell\\main_menu\\main_menu");
    return true;
}

static bool t_focus(void *user, char *out, size_t cap) {
    (void)user;
    if (!T.menu) {
        return false;
    }
    snprintf(out, cap, "main_menu_item_%d", T.focus);
    return true;
}

static bool t_tick(void *user, int64_t *out) {
    (void)user;
    *out = T.tick;
    return true;
}

static bool t_checkpoints(void *user, int64_t *out) {
    (void)user;
    *out = T.checkpoints;
    return true;
}

static WlResult t_load(void *user, const char *arg) {
    (void)user;
    snprintf(T.loaded, sizeof T.loaded, "%s", arg);
    T.menu = false;
    T.tick = 0;
    return WL_OK;
}

static bool t_level(void *user, const char *arg, bool first, int64_t *scratch) {
    (void)user;
    (void)first;
    (void)scratch;
    return strstr(T.loaded, arg) != NULL;
}

static WlResult walk_begin(void *user, WlXbPlayStep *s) {
    (void)user;
    T.step_ran = strcmp(s->arg, "north") == 0;
    return WL_OK;
}

static WlXbStepStatus walk_frame(void *user, WlXbPlayStep *s) {
    (void)user;
    s->pad->analog[WL_XBOX_PAD_A] = 255;
    s->pad->thumb[WL_XBOX_PAD_LY] = 20000;
    T.step_frames = s->frames;
    return s->frames >= 3 ? WL_XB_STEP_DONE : WL_XB_STEP_RUNNING;
}

static const char k_script[] =
    "# a pretend title's menus and level\n"
    "screen main_menu 5\n"
    "select item_3 down 8\n"
    "press a\n"
    "load levels\\a10\\a10\n"
    "play 5\n"
    "ticks 10\n"
    "hold rt 3f\n"
    "stick 1 0 0 -1 2f\n"
    "down x\n"
    "frames 2\n"
    "up x\n"
    "memory 0x00500000:1 == 7 5\n"
    "peek 0x00500000\n"
    "until level a10 5\n"
    "walkto north 2\n"
    "checkpoint 5\n"
    "connect 2\n"
    "2: press b\n"
    "bot 2 7\n"
    "frames 3\n"
    "bot 2 off\n"
    "disconnect 2\n"
    "capture 1 cap\n"
    "repeat 2\n"
    "log twice\n"
    "end\n"
    "ticks 2000\n"
    "log done\n";

static char g_root[256];

TEST parse_errors(void) {
    int bad = 0;
    ASSERT_EQ(3, wl_xb_play_parse("wait 1\nrepeat 2\npress a\nend\n", &bad));
    ASSERT_EQ(-1, wl_xb_play_parse("wait 1\npress zz\n", &bad));
    ASSERT_EQ(2, bad);
    ASSERT_EQ(-1, wl_xb_play_parse("repeat 2\npress a\n", &bad)); // no end
    ASSERT_EQ(-1, wl_xb_play_parse("end\n", &bad));
    ASSERT_EQ(-1, wl_xb_play_parse("memory 0x10 ~ 3\n", &bad));
    ASSERT_EQ(-1, wl_xb_play_parse("hold a\n", &bad));
    ASSERT_EQ(1, wl_xb_play_parse("objective 3600\n", &bad)); // a title's step, checked when run
    ASSERT_EQ(2, wl_xb_play_parse("hold a+b+lup 2.5\n2: hold x 30f\n", &bad));
    PASS();
}

TEST bot_is_deterministic(void) {
    WlXboxPad a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    wl_xb_play_bot(&a, 12345, 3, false);
    wl_xb_play_bot(&b, 12345, 3, false);
    ASSERT(wl_xbox_pad_equal(&a, &b));
    memset(&b, 0, sizeof b);
    wl_xb_play_bot(&b, 12345, 4, false);
    ASSERT_FALSE(wl_xbox_pad_equal(&a, &b));
    memset(&a, 0, sizeof a);
    wl_xb_play_bot(&a, 2000, 0, true); // looking only: no movement, no buttons
    ASSERT_EQ(0, a.thumb[WL_XBOX_PAD_LX]);
    ASSERT_EQ(0, a.analog[WL_XBOX_PAD_RT]);
    PASS();
}

static void svc(void *user, uint32_t reasons) {
    (void)user;
    (void)reasons;
}

TEST script_runs(void) {
    snprintf(g_root, sizeof g_root, "/tmp/wl_xb_play_test_%d", (int)getpid());
    mkdir(g_root, 0777);
    char path[300];
    snprintf(path, sizeof path, "%s/play.txt", g_root);
    FILE *f = fopen(path, "w");
    ASSERT(f != NULL);
    fputs(k_script, f);
    fclose(f);
    wl_host_fs_set_root(WL_DEVICE_SD, g_root);
    wl_platform_fs_mount(WL_DEVICE_SD);
    ASSERT_EQ(WL_OK, wl_platform_mutex_create(&g_log_m));
    wl_log_set_sink(sink, NULL);

    WlXbMemConfig mc;
    wl_xb_mem_config_default(&mc);
    ASSERT_EQ(WL_OK, wl_xb_mem_init(&mc));
    WlXbThread *self = NULL;
    ASSERT_EQ(WL_OK, wl_xb_threads_init(NULL, &self));
    ASSERT_EQ(WL_OK, wl_xb_thread_setup_guest(self, 0x10000u, 0, 0, 0));
    uint32_t va = MEM_VA;
    ASSERT_EQ(WL_OK, wl_xb_va_alloc(&va, 0x10000u, MEM_VA, WL_XB_PROT_READWRITE));
    ASSERT_EQ_FMT(MEM_VA, va, "%08x");
    wl_xb_service_set_handler(svc, NULL);
    WlXbServiceConfig sc;
    wl_xb_service_config_default(&sc);
    ASSERT_EQ(WL_OK, wl_xb_service_start(&sc));
    wl_xb_input_reset(false);

    wl_xb_play_register_text("screen", t_screen, NULL);
    wl_xb_play_register_text("focus", t_focus, NULL);
    wl_xb_play_register_number("tick", t_tick, NULL);
    wl_xb_play_register_number("checkpoints", t_checkpoints, NULL);
    wl_xb_play_register_action("load", t_load, NULL);
    wl_xb_play_register_predicate("level", t_level, NULL);
    WlXbPlayStepType walk = {walk_begin, walk_frame, NULL, 60.0};
    ASSERT_EQ(WL_OK, wl_xb_play_register_step("walkto", &walk, NULL));
    T.menu = true;
    T.tick = -1;
    ASSERT_EQ(WL_OK, wl_xb_play_start("script:play.txt", "sd:/", "sd:/"));
    ASSERT(wl_xb_play_active());

    WlXboxPad prev[4];
    memset(prev, 0, sizeof prev);
    for (int i = 0; i < 2500 && !T.finished && !T.stopped; i++) {
        for (uint32_t p = 0; p < 4u; p++) {
            wl_xb_input_report_sent(p); // the title read its controllers
        }
        WlXboxPad st[4];
        for (uint32_t p = 0; p < 4u; p++) {
            wl_xb_input_state(p, &st[p]);
        }
        // the pretend title: the D-pad moves the focus, A on item 3 is noted; ticks run in a game
        if (T.menu && (st[0].buttons & WL_XBOX_PAD_DOWN) && !(prev[0].buttons & WL_XBOX_PAD_DOWN)) {
            T.focus++;
        }
        if (!T.menu) {
            T.tick += T.fast ? 40 : 1; // under the harness's jump that reads as a checkpoint loaded
            if (T.tick == 15) {
                wl_xb_wr8(MEM_VA, 7);
            }
            if (T.tick == 60) {
                T.checkpoints++;
            }
        }
        T.saw_rt |= st[0].analog[WL_XBOX_PAD_RT] == 255;
        T.saw_stick |= st[0].thumb[WL_XBOX_PAD_LX] == 32767 && st[0].thumb[WL_XBOX_PAD_RY] == -32767;
        T.saw_x |= st[0].analog[WL_XBOX_PAD_X] == 255;
        T.saw_walk_a |= st[0].analog[WL_XBOX_PAD_A] == 255 && st[0].thumb[WL_XBOX_PAD_LY] == 20000;
        T.port2_b |= st[1].analog[WL_XBOX_PAD_B] == 255 && wl_xb_input_connected(1);
        T.port2_bot |= st[1].thumb[WL_XBOX_PAD_LY] != 0;
        memcpy(prev, st, sizeof prev);
        wl_xb_play_frame_point(); // the title's frame point (this thread holds the lock)
        wl_xb_wait(self, 16);     // a frame passes; the vertical blank runs meanwhile
    }
    wl_log_set_sink(NULL, NULL);
    ASSERT(T.finished);
    ASSERT_FALSE(T.stopped);
    ASSERT_EQ(3, T.focus);
    ASSERT_STR_EQ("levels\\a10\\a10", T.loaded);
    ASSERT(T.saw_rt);
    ASSERT(T.saw_stick);
    ASSERT(T.saw_x);
    ASSERT(T.memory_seen);
    ASSERT(T.step_ran);
    ASSERT(T.step_done);
    ASSERT(T.saw_walk_a);
    ASSERT(T.port2_b);
    ASSERT(T.port2_bot);
    ASSERT_FALSE(wl_xb_input_connected(1)); // disconnected again
    // every step logged as it began, twice for the repeat
    int twice = 0, lines = 0;
    for (int i = 0; i < T.log_count; i++) {
        twice += strstr(T.log[i], "test_input: twice") != NULL ? 1 : 0;
        lines += strstr(T.log[i], "test_input: line ") != NULL ? 1 : 0;
    }
    ASSERT_EQ(2, twice);
    // a line a game minute (1800 ticks at 30 a second) says the time played: a run's watcher
    // (host_run.py) sees the game move on through a long cutscene that logs nothing else
    int progress = 0;
    for (int i = 0; i < T.log_count; i++) {
        const char *at = strstr(T.log[i], "test_input: played to tick ");
        long long played = 0, tick = 0;
        if (at != NULL && sscanf(at, "test_input: played to tick %lld (the game's tick %lld)", &played, &tick) == 2) {
            ASSERT(played >= 1800 && played < 1800 + 40 && tick == played);
            progress++;
        }
    }
    ASSERT_EQ(1, progress);
    ASSERT(lines >= 25);
    // the capture step waits for its frames; with no GPU to take them (still armed here) it gives
    // up after 5 s and a second a frame, says so, and the script goes on
    int armed = 0, gave_up = 0;
    for (int i = 0; i < T.log_count; i++) {
        armed += strstr(T.log[i], "test_input: capture of 1 frames to") != NULL ? 1 : 0;
        gave_up += strstr(T.log[i], "test_input: capture: not done after 6 s") != NULL ? 1 : 0;
    }
    ASSERT_EQ(1, armed);
    if (wl_xb_gpu_capture_busy()) {
        ASSERT_EQ(1, gave_up);
    }
    PASS();
}

// a title with many hooks (Halo's plugin registers 32): every one of them is kept
TEST many_registrations_are_kept(void) {
    char name[32];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "many_%d", i);
        ASSERT_EQ(WL_OK, wl_xb_play_register_action(name, t_load, NULL));
    }
    PASS();
}

SUITE(play) {
    RUN_TEST(parse_errors);
    RUN_TEST(bot_is_deterministic);
    RUN_TEST(script_runs);
    RUN_TEST(many_registrations_are_kept);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(play);
    GREATEST_MAIN_END();
}
