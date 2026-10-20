// SPDX-License-Identifier: MIT
// The deterministic mode's virtual clock (det_clock.h) against the count of nanoseconds it replaced
// (thread.c kept one and divided it by 1000 at every safe point): the same microseconds and
// nanoseconds after every step, for the safe points' steps, steps of a microsecond and more, the
// microseconds a kernel call spends and the idle thread's jumps.
#include <stdint.h>

#include "greatest.h"
#include "wl/xboxrt/det_clock.h"

static uint64_t g_ref; // nanoseconds, as thread.c counted them
static WlXbDetClock g_clock;

static void start(void) {
    g_ref = 1000000000ull; // (thread.c's start: 1 s)
    wl_xb_det_clock_set_us(&g_clock, 1000000u);
}

static int same(void) {
    return g_clock.us == g_ref / 1000u && g_clock.ns == g_ref % 1000u && wl_xb_det_clock_ns(&g_clock) == g_ref;
}

static void step_ns(uint32_t ns) {
    g_ref += ns;
    wl_xb_det_clock_add_ns(&g_clock, ns);
}

static uint32_t g_seed = 12345u;
static uint32_t next_random(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 8;
}

TEST safe_point_steps(void) {
    static const uint32_t steps[] = {40u, 400u, 400u, 1u, 999u, 0u, 7u, 993u};
    unsigned i;

    start();
    ASSERT(same());
    for (i = 0; i < 3000000u; i++) {
        step_ns(steps[i % (sizeof steps / sizeof steps[0])]);
        if (!same()) {
            FAILm("the clock left the count of nanoseconds");
        }
    }
    PASS();
}

TEST long_steps(void) {
    static const uint32_t steps[] = {1000u, 1001u, 1999u, 2000u, 123456u, 999999u, 1000000u, 4294967000u, UINT32_MAX};
    unsigned i, k;

    start();
    for (k = 0; k < 4u; k++) {
        for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
            step_ns(steps[i]);
            ASSERT(same());
            step_ns(999u); // (the nanoseconds below a microsecond carried with a long step)
            ASSERT(same());
        }
    }
    PASS();
}

TEST mixed(void) {
    unsigned i;

    start();
    for (i = 0; i < 2000000u; i++) {
        uint32_t r = next_random(), kind = r & 15u;

        if (kind == 0) {
            uint32_t us = (r >> 4) & 0xFFFFu; // a kernel call's time spent (wl_xb_time_spend_us)
            g_ref += (uint64_t)us * 1000u;
            wl_xb_det_clock_add_us(&g_clock, us);
        } else if (kind == 1) {
            uint64_t next = g_ref / 1000u + 1u + ((r >> 4) & 0x3FFu); // the idle thread's jump
            g_ref = next * 1000u;
            wl_xb_det_clock_set_us(&g_clock, next);
        } else if (kind == 2) {
            step_ns(r >> 4);
        } else {
            step_ns((r >> 4) & 0x7FFu);
        }
        if (!same()) {
            FAILm("the clock left the count of nanoseconds");
        }
    }
    PASS();
}

SUITE(det_clock) {
    RUN_TEST(safe_point_steps);
    RUN_TEST(long_steps);
    RUN_TEST(mixed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(det_clock);
    GREATEST_MAIN_END();
}
