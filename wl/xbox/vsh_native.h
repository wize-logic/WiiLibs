// SPDX-License-Identifier: MIT
// Vertex programs compiled ahead of time (tools/xbox_vsh_codegen.c writes them as C, vsh.c runs
// them): what their functions share with the interpreter. A generated file includes this header
// and defines its table of WlXboxVshNative, which the program hands to wl_xbox_vsh_compile.
#pragma once

#include "wl/gfx/gfx_sqrt.h"
#include "wl/xbox/vsh.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* the ILU's exponent, logarithm and lighting (from c into out) */
void wl_xbox_vsh_ilu(int ilu, const float c[4], float out[4]);

static const float wl_xbox_vsh_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};

/* a relative constant's index, kept in the constants */
static inline int wl_xbox_vsh_clamp(int index) {
    return index < 0                             ? 0
           : index >= WL_XBOX_VSH_CONSTANT_COUNT ? WL_XBOX_VSH_CONSTANT_COUNT - 1
                                                 : index;
}

/* RCC: the reciprocal, clamped away from 0 and infinity */
static inline float wl_xbox_vsh_reciprocal_clamped(float x) {
    float r = 1.0f / x;

    if (r > 0.0f)
        return r < 5.42101e-20f ? 5.42101e-20f : r > 1.884467e+19f ? 1.884467e+19f : r;
    return r > -5.42101e-20f ? -5.42101e-20f : r < -1.884467e+19f ? -1.884467e+19f : r;
}

/* ARL's index, (int)floorf(x) without floorf's call: the same for every x a
float's whole part fits in an int, floorf's past that */
static inline int wl_xbox_vsh_floor_index(float x) {
    int whole;

    if (!(x > -2147483648.0f && x < 2147483648.0f))
        return (int)floorf(x);
    whole = (int)x;
    return (float)whole > x ? whole - 1 : whole;
}

/* 1 / sqrt(x) to frsqrte's 5 bits for x > 0 (wl/gfx/gfx_sqrt.h: the
exact value on the host) */
static inline double wl_xbox_vsh_reciprocal_root_estimate(double x) {
    return wl_rsqrt_estimate(x);
}

/* sqrtf correctly rounded, as newlib's is, without its loop of a bit a
step: wl_sqrtf_exact (frsqrte's estimate made exact by Newton's steps in
double, the last bit decided by the midpoint between neighbours) */
static inline float wl_xbox_vsh_sqrtf(float x) {
    return wl_sqrtf_exact(x);
}
