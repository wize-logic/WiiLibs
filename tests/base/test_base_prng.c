// SPDX-License-Identifier: MIT
// xoshiro128** and SplitMix64 seeding. Expected values were produced with an independent
// implementation of the public-domain reference algorithms (prng.di.unimi.it, xoshiro128starstar.c
// and splitmix64.c); the SplitMix64 seed-0 outputs 0xE220A8397B1DCDAF and 0x6E789E6AA1B965F4
// are the widely published ones.
#include "wl/base/prng.h"

#include <stdbool.h>

#include "greatest.h"

TEST reference_sequence(void) {
    static const uint32_t expect[6] = {0x00002d00u, 0x00000000u, 0x005a7080u,
                                       0x04389d80u, 0x79199d9bu, 0x61963b24u};
    WlRng r = {{1, 2, 3, 4}};
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_FMT(expect[i], wl_rng_next(&r), "%#x");
    }
    PASS();
}

TEST seed_state_from_splitmix(void) {
    WlRng r;
    wl_rng_seed(&r, 0);
    ASSERT_EQ_FMT(0x7b1dcdafu, r.s[0], "%#x");
    ASSERT_EQ_FMT(0xe220a839u, r.s[1], "%#x");
    ASSERT_EQ_FMT(0xa1b965f4u, r.s[2], "%#x");
    ASSERT_EQ_FMT(0x6e789e6au, r.s[3], "%#x");
    static const uint32_t expect[4] = {0xdec9045du, 0x9a089d75u, 0xab77d362u, 0xc3e16405u};
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_FMT(expect[i], wl_rng_next(&r), "%#x");
    }
    PASS();
}

TEST same_seed_same_stream(void) {
    WlRng a, b, c;
    wl_rng_seed(&a, 0x123456789abcdefull);
    wl_rng_seed(&b, 0x123456789abcdefull);
    wl_rng_seed(&c, 0x123456789abcdf0ull);
    bool differs = false;
    for (int i = 0; i < 32; i++) {
        uint32_t x = wl_rng_next(&a);
        ASSERT_EQ_FMT(x, wl_rng_next(&b), "%#x");
        differs = differs || x != wl_rng_next(&c);
    }
    ASSERT(differs);
    PASS();
}

TEST below_stays_in_range(void) {
    WlRng r;
    wl_rng_seed(&r, 7);
    ASSERT_EQ(0u, wl_rng_below(&r, 0));
    ASSERT_EQ(0u, wl_rng_below(&r, 1));
    unsigned seen[6] = {0};
    for (int i = 0; i < 6000; i++) {
        uint32_t v = wl_rng_below(&r, 6);
        ASSERT(v < 6);
        seen[v]++;
    }
    for (int i = 0; i < 6; i++) {
        ASSERT(seen[i] > 800 && seen[i] < 1200); // loose uniformity sanity check
    }
    PASS();
}

SUITE(prng_suite) {
    RUN_TEST(reference_sequence);
    RUN_TEST(seed_state_from_splitmix);
    RUN_TEST(same_seed_same_stream);
    RUN_TEST(below_stays_in_range);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(prng_suite);
    GREATEST_MAIN_END();
}
