// SPDX-License-Identifier: MIT
// timebase.h: the time base's conversions to microseconds and milliseconds against a bit-by-bit
// long division (which uses no division of the compiler's), over the edges of every digit, the
// wrap of the upper half and of the whole count, and a few hundred thousand pseudo-random counts
// of every width; and the host clock's two units.
#include <stdint.h>

#include "greatest.h"
#include "wl/platform/platform.h"
#include "wl/platform/timebase.h"

// floor((n << shift) / d) for a 66-bit numerator, by shifting the bits in one at a time. The
// quotient must fit 64 bits (it does for shift 2 and d 243, and for shift 0).
static uint64_t ref_div(uint64_t n, unsigned shift, uint32_t d, uint32_t *rem_out) {
    uint64_t q = 0;
    uint32_t rem = 0;
    for (int bit = 63 + (int)shift; bit >= 0; bit--) {
        int src = bit - (int)shift;
        uint32_t in = src >= 0 ? (uint32_t)((n >> src) & 1u) : 0u;
        rem = (rem << 1) | in;
        q <<= 1;
        if (rem >= d) {
            rem -= d;
            q |= 1u;
        }
    }
    if (rem_out != NULL) {
        *rem_out = rem;
    }
    return q;
}

static uint64_t g_state = 0x9E3779B97F4A7C15ull;

static uint64_t next_random(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

static int check_count(uint64_t t) {
    return wl_timebase_us(t) == ref_div(t, 2, 243u, NULL) && wl_timebase_ms(t) == ref_div(t, 0, 60750u, NULL);
}

TEST edges_agree(void) {
    // around every power of two, and around multiples of the divisors, from 0 to the last count
    for (int bit = 0; bit < 64; bit++) {
        uint64_t base = (uint64_t)1 << bit;
        for (int64_t d = -300; d <= 300; d++) {
            uint64_t t = base + (uint64_t)d;
            ASSERT(check_count(t));
        }
    }
    for (uint64_t k = 0; k < 600; k++) {
        ASSERT(check_count(k));
        ASSERT(check_count(k * 243u));
        ASSERT(check_count(k * 60750u));
        ASSERT(check_count(k * 60750u - 1u));
        ASSERT(check_count(UINT64_MAX - k));                      // the wrap of the whole count
        ASSERT(check_count(((uint64_t)1 << 32) - 300u + k));      // the upper half's carry
        ASSERT(check_count(((uint64_t)k << 32) | 0xFFFFFFFFu));   // the lower half all ones
        ASSERT(check_count(((uint64_t)0xFFFFFFFFu << 32) + k));
    }
    PASS();
}

TEST known_values(void) {
    ASSERT_EQ(0u, wl_timebase_us(0));
    ASSERT_EQ(0u, wl_timebase_us(60u));                       // 0.99 us
    ASSERT_EQ(1u, wl_timebase_us(61u));
    ASSERT_EQ(1000000u, wl_timebase_us(WL_TIMEBASE_HZ));       // one second
    ASSERT_EQ(1000u, wl_timebase_ms(WL_TIMEBASE_HZ));
    ASSERT_EQ(3600ull * 1000000u, wl_timebase_us(3600ull * WL_TIMEBASE_HZ));
    ASSERT_EQ(3600ull * 24 * 365 * 100 * 1000ull, wl_timebase_ms(3600ull * 24 * 365 * 100 * WL_TIMEBASE_HZ));
    ASSERT_EQ(16u, wl_timebase_ms(60750u * 16u));
    ASSERT_EQ(15u, wl_timebase_ms(60750u * 16u - 1u));
    PASS();
}

TEST random_counts_agree(void) {
    for (int i = 0; i < 300000; i++) {
        uint64_t t = next_random() >> (next_random() % 64u);   // every width
        ASSERT(check_count(t));
    }
    PASS();
}

TEST both_units_never_run_backwards(void) {
    uint64_t base[] = {0, 0xFFFFFFF0u, 0x1FFFFFFF0ull, 0xFFFFFFFFFFFFFF00ull};
    for (unsigned b = 0; b < sizeof base / sizeof base[0]; b++) {
        uint64_t last_us = wl_timebase_us(base[b]), last_ms = wl_timebase_ms(base[b]);
        for (uint64_t k = 1; k < 5000u; k++) {
            uint64_t us = wl_timebase_us(base[b] + k), ms = wl_timebase_ms(base[b] + k);
            if (base[b] + k < base[b]) {
                break;                                           // wrapped: a new count
            }
            ASSERT(us >= last_us && us - last_us <= 1u);         // 16 ns a tick: at most one microsecond
            ASSERT(ms >= last_ms && ms - last_ms <= 1u);
            last_us = us;
            last_ms = ms;
        }
    }
    PASS();
}

TEST long_division_by_any_small_divisor(void) {
    for (int i = 0; i < 100000; i++) {
        uint64_t n = next_random() >> (next_random() % 64u);
        uint32_t d = 1u + (uint32_t)(next_random() % 65535u);
        uint32_t want_rem, got_rem;
        uint64_t want = ref_div(n, 0, d, &want_rem);
        uint64_t got = wl_udiv64_16(n, d, &got_rem);
        ASSERT_EQ(want, got);
        ASSERT_EQ(want_rem, got_rem);
        ASSERT_EQ(want, wl_udiv64_16(n, d, NULL));
    }
    PASS();
}

TEST host_clock_units_agree(void) {
    uint64_t us0 = wl_platform_time_us();
    uint64_t ms = wl_platform_time_ms();
    uint64_t us1 = wl_platform_time_us();
    ASSERT(ms >= us0 / 1000u && ms <= us1 / 1000u);
    PASS();
}

SUITE(timebase_suite) {
    RUN_TEST(known_values);
    RUN_TEST(edges_agree);
    RUN_TEST(random_counts_agree);
    RUN_TEST(both_units_never_run_backwards);
    RUN_TEST(long_division_by_any_small_divisor);
    RUN_TEST(host_clock_units_agree);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(timebase_suite);
    GREATEST_MAIN_END();
}
