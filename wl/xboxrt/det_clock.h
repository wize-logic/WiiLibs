// SPDX-License-Identifier: MIT
// The deterministic mode's virtual clock (thread.h), kept in the unit it is read in: whole
// microseconds (wl_xb_time_us) and the nanoseconds below the next one. A safe point moves it on by a
// few nanoseconds (WlXbThreadConfig.det_backedge_ns and the others) and reads its microseconds; a
// count of nanoseconds divided by 1000 there was a 64-bit division, a library call on the Wii's
// 32-bit processor at every backward jump (4-6% of a deterministic run's instructions on ppc-be).
// Here a step under a microsecond is an addition and a compare, a longer one one 32-bit division;
// the time is the same to the nanosecond (us * 1000 + ns is the count the clock kept before).
#pragma once

#include <stdint.h>

typedef struct WlXbDetClock {
    uint64_t us;
    uint32_t ns; // below 1000
} WlXbDetClock;

static inline void wl_xb_det_clock_add_ns(volatile WlXbDetClock *clock, uint32_t ns) {
    uint32_t us = 0, sub;

    if (ns >= 1000u) {
        us = ns / 1000u;
        ns -= us * 1000u;
    }
    sub = clock->ns + ns; // (both below 1000)
    if (sub >= 1000u) {
        sub -= 1000u;
        us++;
    }
    clock->ns = sub;
    clock->us += us;
}

static inline void wl_xb_det_clock_add_us(volatile WlXbDetClock *clock, uint32_t us) {
    clock->us += us;
}

// the clock at a whole microsecond (the idle thread's jump to what is due next)
static inline void wl_xb_det_clock_set_us(volatile WlXbDetClock *clock, uint64_t us) {
    clock->ns = 0;
    clock->us = us;
}

static inline uint64_t wl_xb_det_clock_ns(const volatile WlXbDetClock *clock) {
    return clock->us * 1000u + clock->ns;
}
