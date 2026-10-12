// SPDX-License-Identifier: MIT
// Leaving the program (platform.h, wl_platform_exit): the shutdown hooks and the wind-down every
// backend runs first, and the thread that serves the reset and power buttons. The backends
// (wii/exit_wii.c, host/system_host.c) call wl_platform_shutdown_prepare, then close the log and
// leave their own way.
#include <stdatomic.h>

#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "wl/platform/shutdown_internal.h"

#define LOG_TAG "system"
#define WATCH_STACK_BYTES (16u * 1024u)

static struct {
    WlShutdownFn fn[WL_SHUTDOWN_HOOKS];
    void *user[WL_SHUTDOWN_HOOKS];
    atomic_uint count;
} g_hooks;

static atomic_uint g_leaving;

WlResult wl_platform_shutdown_hook(WlShutdownFn fn, void *user) {
    if (fn == NULL) {
        return WL_ERR_ARG;
    }
    unsigned n = atomic_load(&g_hooks.count);
    if (n >= WL_SHUTDOWN_HOOKS) {
        return WL_ERR_LIMIT;
    }
    g_hooks.fn[n] = fn;
    g_hooks.user[n] = user;
    atomic_store(&g_hooks.count, n + 1u); // (hooks are added from one thread: the main one)
    return WL_OK;
}

void wl_platform_shutdown_prepare(WlExitTarget target) {
    if (atomic_exchange(&g_leaving, 1u) != 0u) {
        // someone else is leaving (the watcher, as the program quits): let them finish
        for (;;) {
            wl_platform_thread_sleep_ms(10);
        }
    }
    WL_LOG_I(LOG_TAG, "leaving (%s)",
             target == WL_EXIT_POWER_OFF ? "power off" : target == WL_EXIT_WII_MENU ? "Wii menu" : "loader");
    for (unsigned n = atomic_load(&g_hooks.count); n-- > 0;) {
        g_hooks.fn[n](target, g_hooks.user[n]);
    }
    wl_platform_audio_stop();
    wl_platform_input_stop_rumble();
}

void wl_platform_shutdown_reset(void) {
    atomic_store(&g_leaving, 0u);
    atomic_store(&g_hooks.count, 0u);
}

// ---- the system watcher -----------------------------------------------------------------------

static struct {
    WlPlatformThread thread;
    void *stack;
    uint32_t period_ms;
    WlSystemTickFn tick;
    void *tick_user;
    atomic_uint stop;
    bool running;
} g_watch;

static void *watch_main(void *arg) {
    (void)arg;
    while (atomic_load(&g_watch.stop) == 0u) {
        uint32_t events = wl_platform_system_events();
        if ((events & WL_SYS_POWER) != 0u) {
            wl_platform_exit(WL_EXIT_POWER_OFF);
        } else if ((events & WL_SYS_RESET) != 0u) {
            wl_platform_exit(WL_EXIT_LOADER);
        }
        if (g_watch.tick != NULL) {
            g_watch.tick(g_watch.tick_user);
        }
        wl_platform_thread_sleep_ms(g_watch.period_ms);
    }
    return NULL;
}

WlResult wl_platform_system_watch_start(uint32_t period_ms, WlSystemTickFn tick, void *tick_user) {
    if (period_ms == 0u || period_ms > 1000u) {
        return WL_ERR_ARG;
    }
    if (g_watch.running || !wl_mem_ready()) {
        return WL_ERR_STATE;
    }
    void *stack = WL_MEM_ALLOC_GPU(WL_TAG_STACK, WATCH_STACK_BYTES);
    if (stack == NULL) {
        return WL_ERR_NOMEM;
    }
    wl_platform_system_init();
    g_watch.stack = stack;
    g_watch.period_ms = period_ms;
    g_watch.tick = tick;
    g_watch.tick_user = tick_user;
    atomic_store(&g_watch.stop, 0u);
    // above the program's main thread, so that a stall in it does not hold the buttons back
    WlResult r = wl_platform_thread_create(&g_watch.thread, watch_main, NULL, stack, WATCH_STACK_BYTES,
                                           WL_THREAD_PRIO_NET);
    if (r != WL_OK) {
        wl_mem_free(stack);
        return r;
    }
    g_watch.running = true;
    return WL_OK;
}

void wl_platform_system_watch_stop(void) {
    if (!g_watch.running) {
        return;
    }
    atomic_store(&g_watch.stop, 1u);
    (void)wl_platform_thread_join(g_watch.thread, NULL);
    wl_mem_free(g_watch.stack);
    g_watch.stack = NULL;
    g_watch.running = false;
}
