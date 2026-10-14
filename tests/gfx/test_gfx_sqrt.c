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
double __wrap_sqrt(double x);

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

// ---- double -------------------------------------------------------------------------------------------

static double (*volatile platform_sqrt)(double) = sqrt;

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

#if defined(__powerpc__)
#define DOUBLES 200000u
#else
#define DOUBLES 4000000u
#endif

// random positive finite doubles of every exponent, subnormals among them, and doubles in [1, 4)
TEST double_exact_matches_sqrt(void) {
    unsigned long differ = 0;
    for (uint32_t i = 0; i < DOUBLES; i++) {
        uint64_t bits = rnd64() & 0x7FFFFFFFFFFFFFFFull;
        if ((bits >> 52) == 0x7FF) {
            bits &= 0x7FEFFFFFFFFFFFFFull;
        }
        if (i % 4 == 0) {
            bits = (bits & 0x000FFFFFFFFFFFFFull) | ((uint64_t)(1023 + (i / 4) % 2) << 52);
        }
        if (i % 16 == 1) {
            bits &= 0x000FFFFFFFFFFFFFull; // a subnormal
        }
        if (bits == 0) {
            continue;
        }
        double x = wl_bits_double(bits);
        if (wl_double_bits(wl_sqrt_exact(x)) != wl_double_bits(platform_sqrt(x))) {
            differ++;
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    PASS();
}

TEST double_refine_is_exact_from_a_coarse_estimate(void) {
    unsigned long differ = 0;
    for (int which = 0; which < 3; which++) {
        for (uint32_t i = 0; i < DOUBLES / 4; i++) {
            uint64_t bits = (rnd64() & 0x000FFFFFFFFFFFFFull) | ((uint64_t)(1023 + i % 2) << 52);
            double x = wl_bits_double(bits);
            if (wl_double_bits(wl_sqrt_refine(x, coarse_estimate(x, which))) != wl_double_bits(platform_sqrt(x))) {
                differ++;
            }
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    PASS();
}

// squares of whole numbers of up to 26 bits and the doubles beside them, and the squares of the
// midpoints' neighbours: where a wrong rounding shows soonest
TEST double_squares_and_their_neighbours(void) {
    unsigned long differ = 0;
    for (uint32_t i = 0; i < DOUBLES / 8; i++) {
        uint64_t k = (rnd64() >> 38) | 1u;
        double square = (double)k * (double)k;
        for (int step = -2; step <= 2; step++) {
            double x = wl_bits_double(wl_double_bits(square) + (uint64_t)(int64_t)step);
            if (wl_double_bits(wl_sqrt_exact(x)) != wl_double_bits(platform_sqrt(x))) {
                differ++;
            }
        }
        // a double's square, rounded: its root is the double or a neighbour
        double r = wl_bits_double((rnd64() & 0x000FFFFFFFFFFFFFull) | ((uint64_t)1023 << 52)), x = r * r;
        if (wl_double_bits(wl_sqrt_exact(x)) != wl_double_bits(platform_sqrt(x))) {
            differ++;
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    PASS();
}

TEST double_special_values_are_ieees(void) {
    double inf = wl_bits_double(0x7FF0000000000000ull), nan = wl_bits_double(0x7FF8000000000000ull);
    ASSERT_EQ(0x0000000000000000ull, wl_double_bits(wl_sqrt_exact(0.0)));
    ASSERT_EQ(0x8000000000000000ull, wl_double_bits(wl_sqrt_exact(-0.0)));
    ASSERT_EQ(0x7FF0000000000000ull, wl_double_bits(wl_sqrt_exact(inf)));
    ASSERT(isnan(wl_sqrt_exact(-1.0)));
    ASSERT(isnan(wl_sqrt_exact(-4.9e-324)));
    ASSERT(isnan(wl_sqrt_exact(-inf)));
    ASSERT(isnan(wl_sqrt_exact(nan)));
    ASSERT_EQ(wl_double_bits(platform_sqrt(4.9e-324)), wl_double_bits(wl_sqrt_exact(4.9e-324)));
    ASSERT_EQ(wl_double_bits(platform_sqrt(2.2250738585072014e-308)),
              wl_double_bits(wl_sqrt_exact(2.2250738585072014e-308)));
    ASSERT_EQ(wl_double_bits(platform_sqrt(1.7976931348623157e308)),
              wl_double_bits(wl_sqrt_exact(1.7976931348623157e308)));
    ASSERT_EQ(wl_double_bits(platform_sqrt(3.9999999999999996)), wl_double_bits(wl_sqrt_exact(3.9999999999999996)));
    ASSERT_EQ(0x3FF0000000000000ull, wl_double_bits(wl_sqrt_exact(1.0)));
    ASSERT_EQ(0x4000000000000000ull, wl_double_bits(wl_sqrt_exact(4.0)));
    ASSERT_EQ(wl_double_bits(platform_sqrt(2.0)), wl_double_bits(__wrap_sqrt(2.0)));
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
    RUN_TEST(double_exact_matches_sqrt);
    RUN_TEST(double_refine_is_exact_from_a_coarse_estimate);
    RUN_TEST(double_squares_and_their_neighbours);
    RUN_TEST(double_special_values_are_ieees);
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
