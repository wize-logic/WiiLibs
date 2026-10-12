// SPDX-License-Identifier: MIT
// Host stub of the system, exit and log parts of platform.h. System events are raised by tests
// (wl_host_system_raise in system_host.h); exit calls exit(0); the log sink keeps the same
// in-memory ring as the console and prints to stderr.
#include "wl/platform/host/system_host.h"
#include "wl/platform/shutdown_internal.h"
#include "wl/platform/shutdown_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_LINES 32u
#define RING_LINE_LEN 160u

static atomic_uint g_events;

void wl_platform_system_init(void) {}

uint32_t wl_platform_system_events(void) {
    return atomic_exchange(&g_events, 0u);
}

void wl_host_system_raise(uint32_t events) {
    atomic_fetch_or(&g_events, events);
}

static WlHostExitHook g_exit_hook;

void wl_host_exit_set_hook(WlHostExitHook hook) {
    g_exit_hook = hook;
}

void wl_host_system_reset(void) {
    atomic_store(&g_events, 0u);
    wl_platform_shutdown_reset();
}

WL_NORETURN void wl_platform_exit(WlExitTarget target) {
    wl_platform_shutdown_prepare(target);
    wl_platform_log_flush();
    if (g_exit_hook != NULL) {
        g_exit_hook(target); // (a test's: it does not return)
    }
    wl_platform_log_shutdown();
    exit(0);
}

// ---- log ------------------------------------------------------------------------------------

static char g_ring[RING_LINES][RING_LINE_LEN];
static atomic_uint g_ring_count;

void wl_platform_log_sink(void *user, WlLogLevel level, const char *tag, const char *line) {
    static const char letters[] = "EWID";
    (void)user;
    char text[RING_LINE_LEN];
    int n = snprintf(text, sizeof text, "[%c][%s] %s", letters[(unsigned)level & 3u], tag, line);
    if (n < 0) {
        return;
    }
    unsigned slot = atomic_fetch_add(&g_ring_count, 1u) % RING_LINES;
    memcpy(g_ring[slot], text, sizeof text);
    fprintf(stderr, "%s\n", text);
}

uint32_t wl_platform_log_tail(const char **lines, uint32_t max) {
    uint32_t count = atomic_load(&g_ring_count);
    uint32_t have = count < RING_LINES ? count : RING_LINES;
    uint32_t n = have < max ? have : max;
    for (uint32_t i = 0; i < n; i++) {
        lines[i] = g_ring[(count - n + i) % RING_LINES];
    }
    return n;
}

void wl_platform_log_init(const char *sd_dir) {
    (void)sd_dir;
    wl_log_set_sink(wl_platform_log_sink, NULL);
}

void wl_platform_log_flush(void) {}

void wl_platform_log_shutdown(void) {}

void wl_platform_crash_report_install(void) {}
