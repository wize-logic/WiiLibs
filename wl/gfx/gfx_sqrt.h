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
    return 1.0 / __builtin_sqrt(x);
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

// A program that links with -Wl,--wrap=sqrtf sends every call of sqrtf, the libraries' too, to __wrap_sqrtf,
// which is wl_sqrtf_exact (gfx_sqrt_wrap.c, in libwl_gfx.a: the linker takes it from there). CMake:
// wl_gfx_wrap_sqrtf(<target>) (cmake/WiiLibs.cmake) adds the flag. It needs no __real_sqrtf.

#ifdef __cplusplus
}
#endif
