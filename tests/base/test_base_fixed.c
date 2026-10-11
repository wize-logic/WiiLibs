// SPDX-License-Identifier: MIT
// WlFx32 helpers: rounding (nearest, ties away from zero), floor conversion, saturation.
#include "wl/base/fixed.h"

#include "greatest.h"

#define FX(n, d) ((WlFx32)(((n) * WL_FX32_ONE) / (d))) // exact for the values used below

TEST from_to_int(void) {
    ASSERT_EQ(4096, wl_fx_from_int(1));
    ASSERT_EQ(-8192, wl_fx_from_int(-2));
    ASSERT_EQ(INT32_MAX, wl_fx_from_int(524288));
    ASSERT_EQ(INT32_MIN, wl_fx_from_int(-524288 - 1));
    ASSERT_EQ(INT32_MIN, wl_fx_from_int(-524288));
    ASSERT_EQ(INT32_MIN, wl_fx_from_int(INT32_MIN));
    ASSERT_EQ(1, wl_fx_to_int(FX(19, 10) ));
    ASSERT_EQ(-1, wl_fx_to_int(-FX(1, 2)));    // floor
    ASSERT_EQ(-1, wl_fx_to_int(-1));           // smallest negative
    ASSERT_EQ(-2, wl_fx_to_int(-FX(3, 2)));
    ASSERT_EQ(0, wl_fx_to_int(4095));
    ASSERT_EQ(-524288, wl_fx_to_int(INT32_MIN));
    PASS();
}

TEST to_int_round(void) {
    ASSERT_EQ(1, wl_fx_to_int_round(FX(1, 2)));
    ASSERT_EQ(-1, wl_fx_to_int_round(-FX(1, 2)));
    ASSERT_EQ(0, wl_fx_to_int_round(2047));
    ASSERT_EQ(0, wl_fx_to_int_round(-2047));
    ASSERT_EQ(2, wl_fx_to_int_round(FX(3, 2)));
    ASSERT_EQ(-2, wl_fx_to_int_round(-FX(3, 2)));
    ASSERT_EQ(1, wl_fx_to_int_round(FX(14, 10) - 1 + 1)); // 1.4 truncated in fixed is < 1.5
    PASS();
}

TEST mul_basic_and_sign(void) {
    ASSERT_EQ(FX(3, 1), wl_fx_mul(FX(3, 2), FX(2, 1)));
    ASSERT_EQ(-FX(3, 1), wl_fx_mul(-FX(3, 2), FX(2, 1)));
    ASSERT_EQ(FX(3, 1), wl_fx_mul(-FX(3, 2), -FX(2, 1)));
    ASSERT_EQ(0, wl_fx_mul(0, FX(5, 1)));
    ASSERT_EQ(5, wl_fx_mul(5, WL_FX32_ONE));
    PASS();
}

TEST mul_rounding(void) {
    // 1 * 2048 (0.5) = 0.5 raw unit exactly: tie, away from zero.
    ASSERT_EQ(1, wl_fx_mul(1, 2048));
    ASSERT_EQ(-1, wl_fx_mul(-1, 2048));
    ASSERT_EQ(-1, wl_fx_mul(1, -2048));
    ASSERT_EQ(0, wl_fx_mul(1, 2047));   // just below the tie
    ASSERT_EQ(0, wl_fx_mul(-1, 2047));
    ASSERT_EQ(2, wl_fx_mul(3, 2048));   // 1.5 -> 2
    ASSERT_EQ(-2, wl_fx_mul(3, -2048));
    PASS();
}

TEST mul_saturates(void) {
    ASSERT_EQ(INT32_MAX, wl_fx_mul(INT32_MAX, INT32_MAX));
    ASSERT_EQ(INT32_MIN, wl_fx_mul(INT32_MAX, INT32_MIN));
    ASSERT_EQ(INT32_MAX, wl_fx_mul(INT32_MIN, INT32_MIN));
    PASS();
}

TEST div_basic_and_rounding(void) {
    ASSERT_EQ(FX(3, 2), wl_fx_div(FX(3, 1), FX(2, 1)));
    ASSERT_EQ(-FX(3, 2), wl_fx_div(-FX(3, 1), FX(2, 1)));
    ASSERT_EQ(-FX(3, 2), wl_fx_div(FX(3, 1), -FX(2, 1)));
    ASSERT_EQ(FX(3, 2), wl_fx_div(-FX(3, 1), -FX(2, 1)));
    ASSERT_EQ(1365, wl_fx_div(WL_FX32_ONE, FX(3, 1))); // 1/3 = 1365.33
    ASSERT_EQ(2731, wl_fx_div(FX(2, 1), FX(3, 1)));    // 2/3 = 2730.67
    ASSERT_EQ(-2731, wl_fx_div(-FX(2, 1), FX(3, 1)));
    ASSERT_EQ(1, wl_fx_div(1, 2 * WL_FX32_ONE));       // 0.5 raw: tie, away from zero
    ASSERT_EQ(-1, wl_fx_div(-1, 2 * WL_FX32_ONE));
    ASSERT_EQ(-1, wl_fx_div(1, -2 * WL_FX32_ONE));
    ASSERT_EQ(0, wl_fx_div(0, 7));
    PASS();
}

TEST div_saturates_and_zero(void) {
    ASSERT_EQ(INT32_MAX, wl_fx_div(5, 0));
    ASSERT_EQ(INT32_MIN, wl_fx_div(-5, 0));
    ASSERT_EQ(0, wl_fx_div(0, 0));
    ASSERT_EQ(INT32_MAX, wl_fx_div(INT32_MAX, 1));
    ASSERT_EQ(INT32_MIN, wl_fx_div(INT32_MIN, 1));
    ASSERT_EQ(INT32_MAX, wl_fx_div(INT32_MIN, -1));
    PASS();
}

SUITE(fixed_suite) {
    RUN_TEST(from_to_int);
    RUN_TEST(to_int_round);
    RUN_TEST(mul_basic_and_sign);
    RUN_TEST(mul_rounding);
    RUN_TEST(mul_saturates);
    RUN_TEST(div_basic_and_rounding);
    RUN_TEST(div_saturates_and_zero);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(fixed_suite);
    GREATEST_MAIN_END();
}
