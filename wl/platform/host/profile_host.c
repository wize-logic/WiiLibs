// SPDX-License-Identifier: MIT
// profile.h on the hosts: there is no sampler (WL_ERR_UNSUPPORTED); the dump goes to standard output.
#include <stdio.h>

#include "wl/platform/profile.h"

uint32_t wl_profile_timebase_hz(void) {
    return 60750000u; // (the Wii's, so a dump made by a test reads as the console's)
}

WlResult wl_profile_start(WlProfileRing *ring, uint32_t hz) {
    if (ring == NULL || hz == 0 || hz > 10000u) {
        return WL_ERR_ARG;
    }
    return WL_ERR_UNSUPPORTED;
}

void wl_profile_stop(void) {
}

bool wl_profile_running(void) {
    return false;
}

static void stdout_sink(void *user, const char *line) {
    (void)user;
    puts(line);
}

uint32_t wl_profile_dump_log(const WlProfileRing *ring, const WlProfileDumpInfo *info) {
    return wl_profile_dump(ring, info, stdout_sink, NULL);
}
