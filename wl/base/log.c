// SPDX-License-Identifier: MIT
// Logging (see base.h). Host default sink: one fprintf per line to stderr, so threads do not
// interleave within a line. On the console there is no default sink
// (wl/platform installs one); lines go nowhere.
#include <stdarg.h>
#include <stdio.h>

#include "wl/base/base.h"

static WlLogLevel g_level = WL_LOG_LEVEL_INFO;
static WlLogSink g_sink;
static void *g_sink_user;

void wl_log_set_sink(WlLogSink sink, void *user) {
    g_sink = sink;
    g_sink_user = user;
}

void wl_log_set_level(WlLogLevel level) {
    g_level = level;
}

void wl_log_write(WlLogLevel level, const char *tag, const char *fmt, ...) {
    static const char letters[] = "EWID";
    char msg[512];
    va_list ap;
    if (level > g_level) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_sink != NULL) {
        g_sink(g_sink_user, level, tag, msg);
        return;
    }
#if !defined(__PPC__)
    (void)fprintf(stderr, "[%c][%s] %s\n", letters[level], tag, msg);
#else
    (void)letters;
#endif
}
