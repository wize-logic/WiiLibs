// SPDX-License-Identifier: MIT
// Leaving the program (wl/platform/shutdown.c) on the host backend: the shutdown hooks and their
// order, the audio output and the motors stopped before the program goes, the thread that
// serves the reset and power buttons, and the GX FIFO's size from its heap's budget.
#include <pthread.h>
#include <setjmp.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/audio_host.h"
#include "wl/platform/host/input_host.h"
#include "wl/platform/host/system_host.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

static jmp_buf g_escape;
static WlExitTarget g_left_with;
static unsigned g_left;

// the exit made on the test's own thread: leave by longjmp
static void leave_by_jump(WlExitTarget target) {
    g_left_with = target;
    g_left++;
    longjmp(g_escape, 1);
}

// the exit made on the watcher's thread: tell the test, and end the thread
static WlPlatformSemaphore g_left_sem;
static void leave_by_thread(WlExitTarget target) {
    g_left_with = target;
    g_left++;
    wl_platform_sem_post(g_left_sem);
    pthread_exit(NULL);
}

static char g_order[64];
static unsigned g_order_len;
static WlExitTarget g_hook_target[4];

static void note(char c) {
    if (g_order_len + 1u < sizeof g_order) {
        g_order[g_order_len++] = c;
        g_order[g_order_len] = '\0';
    }
}

static void hook_a(WlExitTarget target, void *user) {
    (void)user;
    g_hook_target[0] = target;
    // the motors are stopped after the hooks: one may still be running here
    note(wl_host_remote_rumble(0) ? 'A' : 'a');
}

static void hook_b(WlExitTarget target, void *user) {
    g_hook_target[1] = target;
    note(*(char *)user);
}

static unsigned g_fills;
static void fill(void *user, int16_t *stereo, uint32_t frames) {
    (void)user;
    memset(stereo, 0, (size_t)frames * 4u);
    g_fills++;
}

static void fresh(void) {
    wl_host_system_reset();
    wl_host_exit_set_hook(leave_by_jump);
    g_order[0] = '\0';
    g_order_len = 0;
    g_left = 0;
}

static void ticker(void *user) {
    (*(unsigned *)user)++;
}

TEST watcher_needs_the_memory_system_and_a_period(void) {
    ASSERT(!wl_mem_ready());
    ASSERT_EQ(WL_ERR_STATE, wl_platform_system_watch_start(10, NULL, NULL));
    ASSERT_EQ(WL_OK, wl_mem_init(NULL));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_system_watch_start(0, NULL, NULL));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_system_watch_start(1001, NULL, NULL));
    PASS();
}

TEST hooks_run_newest_first_with_the_target_before_the_motors_stop(void) {
    fresh();
    char b = 'b';
    ASSERT_EQ(WL_OK, wl_platform_shutdown_hook(hook_a, NULL));
    ASSERT_EQ(WL_OK, wl_platform_shutdown_hook(hook_b, &b));
    wl_platform_rumble(0, true);
    wl_platform_pad_rumble(2, true);
    ASSERT(wl_host_remote_rumble(0));
    ASSERT(wl_platform_audio_start(32000, 256, fill, NULL) == WL_OK);
    ASSERT_EQ(256u, wl_host_audio_render(NULL, 256));
    if (setjmp(g_escape) == 0) {
        wl_platform_exit(WL_EXIT_WII_MENU);
        FAIL();
    }
    ASSERT_EQ(1u, g_left);
    ASSERT_EQ(WL_EXIT_WII_MENU, g_left_with);
    ASSERT_STR_EQ("bA", g_order); // b first (the newest), then a: the motor still on in it
    ASSERT_EQ(WL_EXIT_WII_MENU, g_hook_target[0]);
    ASSERT_EQ(WL_EXIT_WII_MENU, g_hook_target[1]);
    ASSERT(!wl_host_remote_rumble(0)); // ...and stopped after
    ASSERT(!wl_host_pad_rumble(2));
    unsigned fills = g_fills;
    ASSERT_EQ(0u, wl_host_audio_render(NULL, 256)); // the audio output is stopped
    ASSERT_EQ(fills, g_fills);
    PASS();
}

TEST a_later_case_can_leave_again(void) {
    // (the exit above left "leaving" set: a second caller would wait for it forever; the host's
    // reset forgets it for the next case)
    fresh();
    char c = 'c';
    ASSERT_EQ(WL_OK, wl_platform_shutdown_hook(hook_b, &c));
    if (setjmp(g_escape) == 0) {
        wl_platform_exit(WL_EXIT_LOADER);
        FAIL();
    }
    ASSERT_STR_EQ("c", g_order);
    PASS();
}

TEST hooks_are_limited_and_checked(void) {
    fresh();
    ASSERT_EQ(WL_ERR_ARG, wl_platform_shutdown_hook(NULL, NULL));
    for (unsigned i = 0; i < WL_SHUTDOWN_HOOKS; i++) {
        ASSERT_EQ(WL_OK, wl_platform_shutdown_hook(hook_a, NULL));
    }
    ASSERT_EQ(WL_ERR_LIMIT, wl_platform_shutdown_hook(hook_a, NULL));
    fresh(); // (forgets them)
    ASSERT_EQ(WL_OK, wl_platform_shutdown_hook(hook_a, NULL));
    PASS();
}

TEST the_power_button_powers_off_from_the_watcher_thread(void) {
    fresh();
    wl_host_exit_set_hook(leave_by_thread);
    ASSERT_EQ(WL_OK, wl_platform_sem_create(&g_left_sem, 0, 4));
    char b = 'b';
    ASSERT_EQ(WL_OK, wl_platform_shutdown_hook(hook_b, &b));
    unsigned ticks = 0;
    ASSERT_EQ(WL_OK, wl_platform_system_watch_start(2, ticker, &ticks));
    ASSERT_EQ(WL_ERR_STATE, wl_platform_system_watch_start(2, NULL, NULL));
    wl_platform_rumble(1, true);
    // the tick goes on while nothing is pressed
    wl_platform_thread_sleep_ms(30);
    ASSERT(ticks > 2u);
    ASSERT_EQ(0u, g_left);
    wl_host_system_raise(WL_SYS_POWER);
    wl_platform_sem_wait(g_left_sem);
    ASSERT_EQ(1u, g_left);
    ASSERT_EQ(WL_EXIT_POWER_OFF, g_left_with);
    ASSERT_STR_EQ("b", g_order);
    ASSERT(!wl_host_remote_rumble(1));
    wl_platform_system_watch_stop();
    wl_platform_sem_destroy(g_left_sem);
    PASS();
}

TEST the_reset_button_returns_to_the_loader(void) {
    fresh();
    wl_host_exit_set_hook(leave_by_thread);
    ASSERT_EQ(WL_OK, wl_platform_sem_create(&g_left_sem, 0, 4));
    ASSERT_EQ(WL_OK, wl_platform_system_watch_start(2, NULL, NULL));
    wl_host_system_raise(WL_SYS_RESET);
    wl_platform_sem_wait(g_left_sem);
    ASSERT_EQ(WL_EXIT_LOADER, g_left_with);
    wl_platform_system_watch_stop();
    wl_platform_sem_destroy(g_left_sem);
    PASS();
}

TEST both_buttons_at_once_power_off(void) {
    fresh();
    wl_host_exit_set_hook(leave_by_thread);
    ASSERT_EQ(WL_OK, wl_platform_sem_create(&g_left_sem, 0, 4));
    wl_host_system_raise(WL_SYS_RESET | WL_SYS_POWER);
    ASSERT_EQ(WL_OK, wl_platform_system_watch_start(2, NULL, NULL));
    wl_platform_sem_wait(g_left_sem);
    ASSERT_EQ(WL_EXIT_POWER_OFF, g_left_with);
    wl_platform_system_watch_stop();
    wl_platform_sem_destroy(g_left_sem);
    PASS();
}

TEST a_watcher_stopped_without_a_button_ends_cleanly(void) {
    fresh();
    unsigned ticks = 0;
    ASSERT_EQ(WL_OK, wl_platform_system_watch_start(1, ticker, &ticks));
    wl_platform_thread_sleep_ms(10);
    wl_platform_system_watch_stop();
    unsigned seen = ticks;
    wl_platform_thread_sleep_ms(10);
    ASSERT_EQ(seen, ticks);
    ASSERT_EQ(0u, g_left);
    wl_platform_system_watch_stop(); // (again: nothing)
    PASS();
}

TEST fifo_takes_its_heaps_budget(void) {
    static WlMemTagDef tags[WL_TAG_BUILTIN_COUNT];
    WlMemConfig cfg;
    ASSERT(wl_mem_shutdown() == 0u);
    ASSERT_EQ(0u, wl_gx_fifo_bytes()); // no memory system
    ASSERT_EQ(WL_OK, wl_mem_init(NULL));
    ASSERT_EQ(WL_GX_FIFO_BYTES, wl_gx_fifo_bytes()); // the default budget: 1 MiB
    ASSERT(wl_mem_shutdown() == 0u);

    memcpy(tags, wl_mem_builtin_tags(), sizeof tags);
    wl_mem_config_default(&cfg);
    cfg.tags = tags;
    cfg.tag_count = WL_TAG_BUILTIN_COUNT;

    tags[WL_TAG_GXFIFO].budget = 512u * 1024u;
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    ASSERT_EQ(512u * 1024u - 32u, wl_gx_fifo_bytes());
    // and the heap really holds a block that size
    void *fifo = WL_MEM_ALLOC_GPU(WL_TAG_GXFIFO, wl_gx_fifo_bytes());
    ASSERT(fifo != NULL);
    wl_mem_free(fifo);
    ASSERT(wl_mem_shutdown() == 0u);

    tags[WL_TAG_GXFIFO].budget = 4u * 1024u * 1024u; // more than the most a FIFO needs
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    ASSERT_EQ(WL_GX_FIFO_BYTES, wl_gx_fifo_bytes());
    ASSERT(wl_mem_shutdown() == 0u);

    tags[WL_TAG_GXFIFO].budget = 32u * 1024u; // less than GX_Init takes
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    ASSERT_EQ(0u, wl_gx_fifo_bytes());
    ASSERT(wl_mem_shutdown() == 0u);

    tags[WL_TAG_GXFIFO].budget = 0; // disabled
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    ASSERT_EQ(0u, wl_gx_fifo_bytes());
    PASS();
}

SUITE(shutdown_suite) {
    RUN_TEST(watcher_needs_the_memory_system_and_a_period);
    RUN_TEST(hooks_run_newest_first_with_the_target_before_the_motors_stop);
    RUN_TEST(a_later_case_can_leave_again);
    RUN_TEST(hooks_are_limited_and_checked);
    RUN_TEST(the_power_button_powers_off_from_the_watcher_thread);
    RUN_TEST(the_reset_button_returns_to_the_loader);
    RUN_TEST(both_buttons_at_once_power_off);
    RUN_TEST(a_watcher_stopped_without_a_button_ends_cleanly);
    RUN_TEST(fifo_takes_its_heaps_budget);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(shutdown_suite);
    GREATEST_MAIN_END();
}
