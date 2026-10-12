// SPDX-License-Identifier: MIT
// hwcount.h on the hosts: the clock and the log, no counters (an armed program logs frame times
// and "no counters").
#include <stdio.h>

#include "wl/platform/hwcount.h"
#include "wl/platform/platform.h"

static uint64_t host_ticks(void *user) {
    (void)user;
    return wl_platform_time_us();
}

static void host_log(void *user, const char *line) {
    (void)user;
    printf("[hw] %s\n", line);
}

static const WlHwcountHw g_hw = {
    .tb_hz = 1000000u,
    .ticks = host_ticks,
    .log = host_log,
};

const WlHwcountHw *wl_hwcount_platform_hw(void) {
    return &g_hw;
}
