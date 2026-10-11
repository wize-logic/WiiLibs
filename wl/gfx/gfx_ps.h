// SPDX-License-Identifier: MIT
// Matrix and vertex routines for skeletons and CPU skinning (Broadway paired singles).
//
// On the console these use the Gekko's paired-single unit (two floats per register, inline assembly, no SDK header
// involved); on the host the same functions are plain C, and the `_ref` variants are plain C
// everywhere, so a test on the target can compare the two. Results may differ in the last bits between the two.
//
// Skin matrix layout (WlSkinMtx): the 3x4 matrix of a bone or envelope, rearranged in pairs the vertex loop loads
// with one paired load each: (m00 m10) (m01 m11) (m02 m12) (m03 m13) (m20 m21) (m22 m23).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/gfx/gfx_math.h"

typedef struct {
    float c[12];
} WlSkinMtx;

// True when the paired-single routines are what the plain names run (the console build).
bool wl_ps_available(void);

// out = a * b for 3x4 affine matrices (b applied first); `out` may alias a or b. Same result as wl_mat34_mul.
void wl_mtx_concat(WlMat34 *out, const WlMat34 *a, const WlMat34 *b);
void wl_mtx_concat_ref(WlMat34 *out, const WlMat34 *a, const WlMat34 *b);

void wl_skinmtx_set(WlSkinMtx *out, const WlMat34 *m);
// The envelope matrix: sum over `n` (1..4) influences of weight[k] * skin[bone[k]]. Weights are expected to sum to 1.
void wl_skinmtx_blend(WlSkinMtx *out, const WlMat34 *skin, const uint16_t bone[4], const float weight[4], uint32_t n);

// out[i] = mtx[env[i]] * (in[i], 1) for n points of three floats (xyz, 12 bytes apart); `out` and `in` must not overlap.
void wl_skin_points(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx);
// out[i] = normalize(upper 3x3 of mtx[env[i]] * in[i]); the squared length is clamped below at 1e-12 before the
// reciprocal square root, so a zero vector stays zero and a degenerate normal never becomes NaN.
void wl_skin_normals(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx);
void wl_skin_points_ref(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx);
void wl_skin_normals_ref(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx);
