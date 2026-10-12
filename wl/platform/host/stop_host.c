// SPDX-License-Identifier: MIT
// Host stub of the console and stop screen (stop.h): no screen, so the text goes to stdout; a
// stop prints its message to stderr and exits with status 1, or goes to the test's hook.
#include "wl/platform/host/stop_host.h"

#include <stdio.h>
#include <stdlib.h>

#include "wl/platform/stop_backend.h"

static WlHostStopHook g_hook;

WlResult wl_platform_console_start(void) {
    return WL_OK;
}

void *wl_platform_console_framebuffer(void) {
    return NULL;
}

void wl_host_stop_set_hook(WlHostStopHook hook) {
    g_hook = hook;
}

void wl_platform_stop_wait(const char *message, bool again) {
    if (g_hook != NULL) {
        g_hook(message, again);
    }
    fflush(stdout);
    fprintf(stderr, "stopped: %s\n", message);
    exit(1);
}
