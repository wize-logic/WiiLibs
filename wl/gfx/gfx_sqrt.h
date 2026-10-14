// SPDX-License-Identifier: MIT
// A correctly rounded single-precision square root that is fast on Broadway, which has no square root
// instruction (the 750 lacks fsqrt). newlib's sqrtf finds the root a bit at a time, about 230 to 290
// instructions a call; this takes frsqrte's estimate of 1 / sqrt(x), refines it with Newton's steps in
// double and then decides the last bit exactly, in about 60.
//
// The result is the IEEE round-to-nearest root, bit for bit: the one newlib, glibc and x86's sqrtss give. The
// squares of the midpoints between a float's neighbours fit a double exactly (25 bits squared) and are never a
// float, so comparing x with a midpoint's square decides the rounding with no error. The result therefore does
// not depend on how good the estimate is (frsqrte's is good to 5 bits at worst, an emulator's may be better),
// only on its being within a few percent, and the same bits come out on every machine, with or without frsqrte.
// Simulation code that must be bit-identical across machines can use it.
//
// Everything here is inline except wl_sqrtf_integer, so the code can sit in a hot loop. It makes no call of
// sqrtf, so a program may wrap sqrtf with it (gfx_sqrt_wrap.c, below).
#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// An estimate of 1 / sqrt(x) for x > 0, finite: frsqrte (5 bits at worst) on PowerPC, the exact value
// elsewhere.
static inline double wl_rsqrt_estimate(double x) {
#if defined(__powerpc__) || defined(__PPC__)
    double estimate;
    __asm__("frsqrte %0,%1" : "=f"(estimate) : "f"(x));
    return estimate;
#else
    // (through sqrtl: a title's build wraps sqrt to wl_sqrt_exact, which asks for this estimate, and
    // without -ffast-math gcc may call sqrt here, which would never end)
    return 1.0 / (double)__builtin_sqrtl((long double)x);
#endif
}

// The float whose bits are `step` away from those of x > 0 (a neighbour of x).
static inline float wl_float_step(float x, int32_t step) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    bits += (uint32_t)step;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

// Whether x is past the midpoint of the neighbouring floats `low` and `high` (exact: the midpoint's square
// fits a double, and is never a float, so never x).
static inline int wl_float_past_middle(double x, float low, float high) {
    double middle = 0.5 * ((double)low + (double)high);
    return x > middle * middle;
}

// The root of value > 0, finite, by integer arithmetic alone: correctly rounded, slow (about a hundred
// operations), no estimate and no libm. wl_sqrtf_refine falls back to it in a case its steps cannot reach.
float wl_sqrtf_integer(float value);

// The correctly rounded square root of `value` > 0, finite, from `estimate`, an estimate of 1 / sqrt(value)
// within a few percent. Three steps of Newton's method in double give 36 bits or more from 5; the root rounded
// to float is moved to the neighbour on x's side of the midpoint between them. Were the root ever more than a
// neighbour away, which the steps cannot leave, wl_sqrtf_integer decides it. A contracted multiply-add makes the
// steps only more exact, so the result does not depend on -ffp-contract.
static inline float wl_sqrtf_refine(float value, double estimate) {
    double x = (double)value, y = estimate;
    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);
    float root = (float)(x * y);
    float neighbour = wl_float_step(root, 1);
    if (wl_float_past_middle(x, root, neighbour)) {
        if (wl_float_past_middle(x, neighbour, wl_float_step(neighbour, 1))) {
            return wl_sqrtf_integer(value);
        }
        return neighbour;
    }
    neighbour = wl_float_step(root, -1);
    if (!wl_float_past_middle(x, neighbour, root)) {
        if (!wl_float_past_middle(x, wl_float_step(neighbour, -1), neighbour)) {
            return wl_sqrtf_integer(value);
        }
        return neighbour;
    }
    return root;
}

// sqrtf, correctly rounded as IEEE and newlib give it: a zero (also -0) and +infinity are themselves; a
// negative is the NaN newlib's (x - x) / (x - x) makes, a NaN or -infinity newlib's x * x + x. On PowerPC the
// estimate is frsqrte's, elsewhere 1 / sqrt(x) in double. The result is the same bits either way, so a test on
// the host is a test of the console's result.
static inline float wl_sqrtf_exact(float x) {
    if (!(x > 0.0f && x <= 3.40282347e+38f)) {
        if (x == 0.0f || x > 0.0f) {
            return x; // zero or +infinity
        }
        if (x != x || x < -3.40282347e+38f) {
            return x * x + x; // NaN or -infinity
        }
        return (x - x) / (x - x);
    }
    return wl_sqrtf_refine(x, wl_rsqrt_estimate((double)x));
}

// ---- the same for double ------------------------------------------------------------------------------
// sqrt correctly rounded, the bits newlib's (a bit a step, several hundred instructions on Broadway), glibc's
// and x86's sqrtsd give. A double's neighbours' midpoints have squares of 108 bits, so the rounding is decided
// in integers: the candidate's significand K (53 bits, the root in [1, 2)) against the value's M (the value in
// [1, 4) times 2^52): the root is above the midpoint K + 1/2 when M * 2^54 > (2K + 1)^2, below K - 1/2 when
// M * 2^54 < (2K - 1)^2, never equal. Newton's steps from frsqrte's estimate leave the candidate a few
// neighbours off at most, and the decision moves it until it is the nearest, so the result does not depend on
// the estimate.

static inline uint64_t wl_double_bits(double x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return bits;
}

static inline double wl_bits_double(uint64_t bits) {
    double x;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

// (high, low), a 128-bit number, against the square of s < 2^55: 1 above it, -1 below, 0 equal.
static inline int wl_u128_vs_square(uint64_t high, uint64_t low, uint64_t s) {
    uint32_t s_high = (uint32_t)(s >> 32), s_low = (uint32_t)s;
    uint64_t lo = (uint64_t)s_low * s_low, mid = (uint64_t)s_high * s_low, hi = (uint64_t)s_high * s_high;
    uint64_t square_low = lo + (mid << 33);
    uint64_t square_high = hi + (mid >> 31) + (square_low < lo ? 1u : 0u);
    if (high != square_high) {
        return high > square_high ? 1 : -1;
    }
    return low > square_low ? 1 : low < square_low ? -1 : 0;
}

// The correctly rounded root of `value` in [1, 4), from `estimate`, an estimate of 1 / sqrt(value) within a few
// percent: four steps of Newton's method, then the candidate moved to the nearest double by the integers.
static inline double wl_sqrt_refine(double value, double estimate) {
    double y = estimate;
    y = y * (1.5 - 0.5 * value * y * y);
    y = y * (1.5 - 0.5 * value * y * y);
    y = y * (1.5 - 0.5 * value * y * y);
    y = y * (1.5 - 0.5 * value * y * y);
    double root = value * y;
    if (!(root >= 1.0)) {
        root = 1.0;
    }
    if (!(root < 2.0)) {
        root = 1.9999999999999998; // (the largest double below 2)
    }
    uint64_t bits = wl_double_bits(value), one = (uint64_t)1 << 52, mantissa = one - 1;
    uint64_t m = (bits & mantissa) | one;
    if ((bits >> 52) != 1023) {
        m <<= 1; // (the value in [2, 4))
    }
    uint64_t high = m >> 10, low = m << 54, k = (wl_double_bits(root) & mantissa) | one;
    for (;;) {
        if (wl_u128_vs_square(high, low, 2 * k + 1) > 0) {
            k++;
        } else if (k > one && wl_u128_vs_square(high, low, 2 * k - 1) < 0) {
            k--;
        } else {
            break;
        }
    }
    return wl_bits_double(((uint64_t)1023 << 52) | (k & mantissa));
}

// sqrt, correctly rounded as newlib gives it: a zero (also -0) and +infinity are themselves; a negative is the
// NaN newlib's (x - x) / (x - x) makes, a NaN or -infinity newlib's x * x + x. The value is scaled into
// [1, 4) by an even power of two (a subnormal first by 2^54), its root scaled back by half of it.
static inline double wl_sqrt_exact(double x) {
    if (!(x > 0.0 && x <= 1.7976931348623157e308)) {
        if (x == 0.0 || x > 0.0) {
            return x; // zero or +infinity
        }
        if (x != x || x < -1.7976931348623157e308) {
            return x * x + x; // NaN or -infinity
        }
        return (x - x) / (x - x);
    }
    int scale = 0;
    if (x < 2.2250738585072014e-308) {
        x *= 18014398509481984.0; // 2^54
        scale = -27;
    }
    uint64_t bits = wl_double_bits(x);
    int exponent = (int)(bits >> 52) - 1023;
    int half = exponent >= 0 ? exponent / 2 : -((1 - exponent) / 2); // (the floor of exponent / 2)
    double value = wl_bits_double((bits & (((uint64_t)1 << 52) - 1)) | ((uint64_t)(1023 + exponent - 2 * half) << 52));
    double root = wl_sqrt_refine(value, wl_rsqrt_estimate(value));
    return root * wl_bits_double((uint64_t)(1023 + half + scale) << 52);
}

// A program that links with -Wl,--wrap=sqrtf sends every call of sqrtf, the libraries' too, to __wrap_sqrtf,
// which is wl_sqrtf_exact (gfx_sqrt_wrap.c, in libwl_gfx.a: the linker takes it from there). CMake:
// wl_gfx_wrap_sqrtf(<target>) (cmake/WiiLibs.cmake) adds the flag. It needs no __real_sqrtf.

#ifdef __cplusplus
}
#endif
