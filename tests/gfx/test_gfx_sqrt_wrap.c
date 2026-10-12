// SPDX-License-Identifier: MIT
// A program linked with wl_gfx_wrap_sqrtf: its sqrtf is __wrap_sqrtf, the exact root.
#include "wl/gfx/gfx_sqrt.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"

float __wrap_sqrtf(float x);

static uint32_t float_bits(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

// With --wrap=sqrtf the address of sqrtf is __wrap_sqrtf's.
static float (*volatile the_sqrtf)(float) = sqrtf;

TEST sqrtf_is_the_wrap(void) {
    ASSERT(the_sqrtf == __wrap_sqrtf);
    PASS();
}

TEST the_wrapped_root_is_exact_and_does_not_recurse(void) {
    volatile float values[] = { 0.0f, -0.0f, 1.0f, 2.0f, 3.0f, 1.0e-45f, 3.40282347e+38f, 12345.678f };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        ASSERT_EQ(float_bits(wl_sqrtf_exact(values[i])), float_bits(the_sqrtf(values[i])));
        ASSERT_EQ(float_bits(wl_sqrtf_exact(values[i])), float_bits(sqrtf(values[i])));
    }
    ASSERT(isnan(the_sqrtf(-4.0f)));
    // (a libm function that may use sqrtf inside the library still gives its answer)
    ASSERT_IN_RANGE(5.0, (double)hypotf(3.0f, 4.0f), 1e-6);
    PASS();
}

SUITE(gfx_sqrt_wrap_suite) {
    RUN_TEST(sqrtf_is_the_wrap);
    RUN_TEST(the_wrapped_root_is_exact_and_does_not_recurse);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gfx_sqrt_wrap_suite);
    GREATEST_MAIN_END();
}
