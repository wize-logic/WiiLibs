// SPDX-License-Identifier: MIT
// The portable part of the stop screen (stop.h): the messages and the rule that the first stop
// wins. Waiting for the user is the backend's (wl_platform_stop_wait).
#include "wl/platform/stop.h"

#include <stdarg.h>
#include <stdio.h>

#include "wl/platform/platform.h"
#include "wl/platform/stop_backend.h"

#define LINE_BYTES 256u

void wl_platform_console_report(const char *tag, const char *fmt, ...) {
    char line[LINE_BYTES];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    fputs(line, stdout);
    WL_LOG_I(tag, "%s", line);
}

void wl_platform_stop_screen(const char *fmt, ...) {
    static unsigned stops;
    char message[LINE_BYTES];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    if (stops++ != 0u) {
        wl_platform_stop_wait(message, true); // (nothing more is said)
    }
    (void)wl_platform_console_start();
    fputs("\n", stdout);
    wl_platform_console_report("stop", "%s\n", message);
    WL_LOG_E("stop", "stopped: %s", message);
    wl_platform_log_flush();
    wl_platform_input_stop_rumble(); // (a motor must not run while the screen waits)
    wl_platform_stop_wait(message, false);
}

void wl_platform_missing_symbol(const char *name) {
    wl_platform_stop_screen("%s is not provided yet (a stub made by tools/link_stubs.py).", name);
}
