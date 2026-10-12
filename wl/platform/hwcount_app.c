// SPDX-License-Identifier: MIT
// hwcount.h's application face: one instance on the platform's machine, armed by a control file.
#include <stdio.h>
#include <string.h>

#include "wl/platform/hwcount.h"

static WlHwcount g_hc;
static bool g_armed;
static uint32_t g_fifo_bytes;
static const char *g_names[WL_HWCOUNT_MARKS];

static void settings_issue(void *user, int issue, const char *key, uint32_t line) {
    (void)user;
    WL_LOG_W("hw", "%s line %u: %s %s", WL_HWCOUNT_FILE, (unsigned)line,
             issue == 1 ? "no such setting" : "no number for", key);
}

WlResult wl_hwcount_arm(const char *folder) {
    WlHwcountSettings settings;
    char path[160];
    WlResult r;

    if (g_armed) {
        return WL_ERR_STATE;
    }
    if (folder == NULL || snprintf(path, sizeof path, "%s/%s", folder, WL_HWCOUNT_FILE) >= (int)sizeof path) {
        return WL_ERR_RANGE;
    }
    r = wl_hwcount_settings_load(&settings, path, settings_issue, NULL);
    if (r != WL_OK) {
        return r;
    }
    r = wl_hwcount_start(&g_hc, &settings, wl_hwcount_platform_hw());
    if (r != WL_OK) {
        return r;
    }
    g_hc.fifo_cap = g_fifo_bytes;
    memcpy(g_hc.mark_names, g_names, sizeof g_names);
    g_armed = true;
    return WL_OK;
}

bool wl_hwcount_armed(void) {
    return g_armed;
}

void wl_hwcount_frame_begin(void) {
    if (g_armed) {
        wl_hwcount_begin_frame(&g_hc);
    }
}

void wl_hwcount_frame_end(void) {
    if (g_armed) {
        wl_hwcount_end_frame(&g_hc);
    }
}

void wl_hwcount_wait_begin(WlHwWait kind) {
    if (g_armed) {
        wl_hwcount_wait_start(&g_hc, kind);
    }
}

void wl_hwcount_wait_end(WlHwWait kind) {
    if (g_armed) {
        wl_hwcount_wait_stop(&g_hc, kind);
    }
}

void wl_hwcount_mark(uint32_t id) {
    if (g_armed) {
        wl_hwcount_mark_at(&g_hc, id);
    }
}

void wl_hwcount_mark_name(uint32_t id, const char *name) {
    if (id < WL_HWCOUNT_MARKS) {
        g_names[id] = name;
        g_hc.mark_names[id] = name;
    }
}

void wl_hwcount_selftest_begin(void) {
    if (g_armed && g_hc.s.selftest) {
        wl_hwcount_selftest_start(&g_hc);
    }
}

void wl_hwcount_selftest_end(uint32_t vertices) {
    if (g_armed) {
        wl_hwcount_selftest_stop(&g_hc, vertices);
    }
}

void wl_hwcount_set_fifo_size(uint32_t bytes) {
    g_fifo_bytes = bytes;
    if (g_armed) {
        g_hc.fifo_cap = bytes;
    }
}

void wl_hwcount_stop(void) {
    if (g_armed) {
        wl_hwcount_end(&g_hc);
        g_armed = false;
    }
}
