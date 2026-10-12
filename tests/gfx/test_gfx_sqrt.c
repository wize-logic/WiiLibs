// SPDX-License-Identifier: MIT
// wl_sqrtf_exact and its parts against the platform's sqrtf, bit for bit. On ppc-be the platform's sqrtf is
// glibc's and wl_sqrtf_exact runs frsqrte (qemu's, which is nearly exact, not the 750's 5-bit table), so the
// estimate is also made as coarse as the 750's by hand and fed to wl_sqrtf_refine: the result must not depend
// on the estimate.
#include "wl/gfx/gfx_sqrt.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"

float __wrap_sqrtf(float x);

// The test links with --wrap=wl_sqrtf_integer, to count the calls the refinement leaves to it (it never should).
float __real_wl_sqrtf_integer(float value);
float __wrap_wl_sqrtf_integer(float value);
static unsigned long integer_calls;

float __wrap_wl_sqrtf_integer(float value) {
    integer_calls++;
    return __real_wl_sqrtf_integer(value);
}

static float bits_float(uint32_t bits) {
    float x;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

static uint32_t float_bits(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

// An estimate of 1 / sqrt(x) to 5 bits (frsqrte's worst), then off by 1/32 either way, as `which` says.
static double coarse_estimate(double x, int which) {
    double exact = 1.0 / sqrt(x);
    int exponent;
    double scale = frexp(exact, &exponent);
    scale = floor(scale * 32.0) / 32.0;
    return ldexp(scale, exponent) * (which == 0 ? 1.0 : which == 1 ? 1.0 + 1.0 / 32.0 : 1.0 - 1.0 / 32.0);
}

// The sqrtf that is compared with: the platform's, called through a volatile so the compiler cannot fold it.
static float (*volatile platform_sqrtf)(float) = sqrtf;

// Every STRIDE'th float, and in the smallest (the subnormals and the first binade) every SMALL_STRIDE'th. Under qemu
// a wider stride keeps the test to seconds: the host runs 10 million floats, the emulator 1 million.
#if defined(__powerpc__)
#define STRIDE 1993u
#define DENSE_STRIDE 61u
#define SMALL_STRIDE 5u
#else
#define STRIDE 199u
#define DENSE_STRIDE 7u
#define SMALL_STRIDE 1u
#endif
#define DENSE_END 0x00100000u

// WL_TEST_EXHAUSTIVE=1 in the environment compares every positive float (about a minute on the host).
static int exhaustive;
static uint32_t stride, small_stride;

TEST exact_matches_sqrtf_over_all_floats(void) {
    unsigned long differ = 0;
    integer_calls = 0;
    for (uint32_t bits = 1; bits < 0x7F800000u; bits += bits < DENSE_END ? small_stride : stride) {
        float x = bits_float(bits);
        if (float_bits(wl_sqrtf_exact(x)) != float_bits(platform_sqrtf(x))) {
            differ++;
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    ASSERT_EQ_FMT(0ul, integer_calls, "%lu");
    PASS();
}

// Two binades that hold the common lengths, a float in every DENSE_STRIDE (prime to STRIDE).
TEST exact_matches_sqrtf_densely_where_games_live(void) {
    unsigned long differ = 0;
    for (uint32_t bits = 0x3F800000u; bits < 0x40800000u; bits += exhaustive ? 1u : DENSE_STRIDE) {
        float x = bits_float(bits);
        if (float_bits(wl_sqrtf_exact(x)) != float_bits(platform_sqrtf(x))) {
            differ++;
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    PASS();
}

TEST refine_is_exact_from_a_coarse_estimate(void) {
    unsigned long differ = 0;
    integer_calls = 0;
    for (int which = 0; which < 3; which++) {
        for (uint32_t bits = 1; bits < 0x7F800000u; bits += bits < DENSE_END ? small_stride : stride) {
            float x = bits_float(bits);
            if (float_bits(wl_sqrtf_refine(x, coarse_estimate((double)x, which))) !=
                float_bits(platform_sqrtf(x))) {
                differ++;
            }
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    ASSERT_EQ_FMT(0ul, integer_calls, "%lu");
    PASS();
}

// The roots that are exact (the squares of whole numbers) and the floats beside them: where a wrong
// rounding shows soonest.
TEST perfect_squares_and_their_neighbours(void) {
    unsigned long differ = 0;
    for (int k = 1; k <= 4096; k++) {
        float square = (float)k * (float)k;
        for (int step = -2; step <= 2; step++) {
            float x = bits_float(float_bits(square) + (uint32_t)step);
            if (float_bits(wl_sqrtf_exact(x)) != float_bits(platform_sqrtf(x))) {
                differ++;
            }
        }
        if (wl_sqrtf_exact(square) != (float)k) {
            differ++;
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    PASS();
}

TEST integer_root_matches_sqrtf(void) {
    unsigned long differ = 0;
    integer_calls = 0;
    for (uint32_t bits = 1; bits < 0x7F800000u; bits += bits < DENSE_END ? small_stride : exhaustive ? 1u : 4001u) {
        float x = bits_float(bits);
        if (float_bits(wl_sqrtf_integer(x)) != float_bits(platform_sqrtf(x))) {
            differ++;
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    // the largest and the smallest
    ASSERT_EQ(float_bits(platform_sqrtf(3.40282347e+38f)), float_bits(wl_sqrtf_integer(3.40282347e+38f)));
    ASSERT_EQ(float_bits(platform_sqrtf(1.0e-45f)), float_bits(wl_sqrtf_integer(1.0e-45f)));
    // (the count sees these calls, so that a count of none in the tests above means none)
    ASSERT(integer_calls > 0);
    PASS();
}

TEST special_values_are_ieees(void) {
    float inf = bits_float(0x7F800000u), nan = bits_float(0x7FC00000u);
    // zeros keep their sign
    ASSERT_EQ(0x00000000u, float_bits(wl_sqrtf_exact(0.0f)));
    ASSERT_EQ(0x80000000u, float_bits(wl_sqrtf_exact(-0.0f)));
    ASSERT_EQ(0x7F800000u, float_bits(wl_sqrtf_exact(inf)));
    ASSERT(isnan(wl_sqrtf_exact(-1.0f)));
    ASSERT(isnan(wl_sqrtf_exact(-1.0e-45f)));
    ASSERT(isnan(wl_sqrtf_exact(-3.40282347e+38f)));
    ASSERT(isnan(wl_sqrtf_exact(-inf)));
    ASSERT(isnan(wl_sqrtf_exact(nan)));
    ASSERT_EQ(float_bits(platform_sqrtf(1.0f)), float_bits(wl_sqrtf_exact(1.0f)));
    ASSERT_EQ(float_bits(platform_sqrtf(4.0f)), float_bits(wl_sqrtf_exact(4.0f)));
    ASSERT_EQ(float_bits(platform_sqrtf(1.17549435e-38f)), float_bits(wl_sqrtf_exact(1.17549435e-38f)));
    ASSERT_EQ(float_bits(platform_sqrtf(3.40282347e+38f)), float_bits(wl_sqrtf_exact(3.40282347e+38f)));
    ASSERT_EQ(float_bits(platform_sqrtf(1.0e-45f)), float_bits(wl_sqrtf_exact(1.0e-45f)));
    PASS();
}

TEST wrap_is_the_exact_root(void) {
    ASSERT_EQ(float_bits(wl_sqrtf_exact(2.0f)), float_bits(__wrap_sqrtf(2.0f)));
    ASSERT_EQ(float_bits(platform_sqrtf(2.0f)), float_bits(__wrap_sqrtf(2.0f)));
    ASSERT(isnan(__wrap_sqrtf(-2.0f)));
    PASS();
}

SUITE(gfx_sqrt_suite) {
    RUN_TEST(exact_matches_sqrtf_over_all_floats);
    RUN_TEST(exact_matches_sqrtf_densely_where_games_live);
    RUN_TEST(refine_is_exact_from_a_coarse_estimate);
    RUN_TEST(perfect_squares_and_their_neighbours);
    RUN_TEST(integer_root_matches_sqrtf);
    RUN_TEST(special_values_are_ieees);
    RUN_TEST(wrap_is_the_exact_root);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    exhaustive = getenv("WL_TEST_EXHAUSTIVE") != NULL;
    stride = exhaustive ? 1u : STRIDE;
    small_stride = exhaustive ? 1u : SMALL_STRIDE;
    RUN_SUITE(gfx_sqrt_suite);
    GREATEST_MAIN_END();
}
