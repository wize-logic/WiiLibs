// SPDX-License-Identifier: MIT
// Paired-single matrix and skinning routines (gfx_ps.h).
//
// The console path is inline assembly over the Gekko paired-single instructions (psq_l / psq_st with the default
// float quantisation of GQR0, ps_muls0 / ps_madds0 / ps_madds1 / ps_madd / ps_sum0 ...), the semantics of which are
// the PowerPC 750CL's published instruction set. Everything has a plain C twin (`_ref`); a test on the console can compare
// the two (the host and ppc-be builds only ever run the C path).
#include "wl/gfx/gfx_ps.h"

#include <math.h>
#include <string.h>

#define SKIN_EPS 1.0e-12f

// ---- plain C ----------------------------------------------------------------------------------

void wl_mtx_concat_ref(WlMat34 *out, const WlMat34 *a, const WlMat34 *b) {
    wl_mat34_mul(out, a, b);
}

void wl_skinmtx_set(WlSkinMtx *out, const WlMat34 *m) {
    const float *s = m->m;
    float *c = out->c;
    c[0] = s[0];
    c[1] = s[4];
    c[2] = s[1];
    c[3] = s[5];
    c[4] = s[2];
    c[5] = s[6];
    c[6] = s[3];
    c[7] = s[7];
    c[8] = s[8];
    c[9] = s[9];
    c[10] = s[10];
    c[11] = s[11];
}

void wl_skinmtx_blend(WlSkinMtx *out, const WlMat34 *skin, const uint16_t bone[4], const float weight[4], uint32_t n) {
    WlMat34 acc;
    memset(&acc, 0, sizeof acc);
    for (uint32_t k = 0; k < n && k < 4u; k++) {
        const float *m = skin[bone[k]].m;
        float w = weight[k];
        for (int i = 0; i < 12; i++) {
            acc.m[i] += w * m[i];
        }
    }
    wl_skinmtx_set(out, &acc);
}

void wl_skin_points_ref(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx) {
    for (uint32_t i = 0; i < n; i++) {
        const float *c = mtx[env[i]].c;
        float x = in[3 * i], y = in[3 * i + 1], z = in[3 * i + 2];
        out[3 * i] = c[0] * x + c[2] * y + c[4] * z + c[6];
        out[3 * i + 1] = c[1] * x + c[3] * y + c[5] * z + c[7];
        out[3 * i + 2] = c[8] * x + c[9] * y + c[10] * z + c[11];
    }
}

void wl_skin_normals_ref(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx) {
    for (uint32_t i = 0; i < n; i++) {
        const float *c = mtx[env[i]].c;
        float x = in[3 * i], y = in[3 * i + 1], z = in[3 * i + 2];
        float nx = c[0] * x + c[2] * y + c[4] * z;
        float ny = c[1] * x + c[3] * y + c[5] * z;
        float nz = c[8] * x + c[9] * y + c[10] * z;
        float len2 = nx * nx + ny * ny + nz * nz;
        if (len2 < SKIN_EPS) {
            len2 = SKIN_EPS;
        }
        float k = 1.0f / sqrtf(len2);
        out[3 * i] = nx * k;
        out[3 * i + 1] = ny * k;
        out[3 * i + 2] = nz * k;
    }
}

#if defined(__wii__)

// ---- paired single ----------------------------------------------------------------------------

bool wl_ps_available(void) {
    return true;
}

void wl_mtx_concat(WlMat34 *out, const WlMat34 *a, const WlMat34 *b) {
    static const float k01[2] = {0.0f, 1.0f};
    // f0..f5: the six pairs of b's rows, f6 = (0, 1) (the implicit fourth row of b), f7 f8: a row's four entries,
    // f9 f10: the result row. Row i of the result = a_i0 * b_row0 + a_i1 * b_row1 + a_i2 * b_row2 + (0 0 0 a_i3).
    __asm__ volatile(
        "psq_l 0, 0(%1), 0, 0\n\t"
        "psq_l 1, 8(%1), 0, 0\n\t"
        "psq_l 2, 16(%1), 0, 0\n\t"
        "psq_l 3, 24(%1), 0, 0\n\t"
        "psq_l 4, 32(%1), 0, 0\n\t"
        "psq_l 5, 40(%1), 0, 0\n\t"
        "psq_l 6, 0(%3), 0, 0\n\t"
        // row 0
        "psq_l 7, 0(%0), 0, 0\n\t"
        "psq_l 8, 8(%0), 0, 0\n\t"
        "ps_muls0 9, 0, 7\n\t"
        "ps_muls0 10, 1, 7\n\t"
        "ps_madds1 9, 2, 7, 9\n\t"
        "ps_madds1 10, 3, 7, 10\n\t"
        "ps_madds0 9, 4, 8, 9\n\t"
        "ps_madds0 10, 5, 8, 10\n\t"
        "ps_madds1 10, 6, 8, 10\n\t"
        "psq_st 9, 0(%2), 0, 0\n\t"
        "psq_st 10, 8(%2), 0, 0\n\t"
        // row 1
        "psq_l 7, 16(%0), 0, 0\n\t"
        "psq_l 8, 24(%0), 0, 0\n\t"
        "ps_muls0 9, 0, 7\n\t"
        "ps_muls0 10, 1, 7\n\t"
        "ps_madds1 9, 2, 7, 9\n\t"
        "ps_madds1 10, 3, 7, 10\n\t"
        "ps_madds0 9, 4, 8, 9\n\t"
        "ps_madds0 10, 5, 8, 10\n\t"
        "ps_madds1 10, 6, 8, 10\n\t"
        "psq_st 9, 16(%2), 0, 0\n\t"
        "psq_st 10, 24(%2), 0, 0\n\t"
        // row 2
        "psq_l 7, 32(%0), 0, 0\n\t"
        "psq_l 8, 40(%0), 0, 0\n\t"
        "ps_muls0 9, 0, 7\n\t"
        "ps_muls0 10, 1, 7\n\t"
        "ps_madds1 9, 2, 7, 9\n\t"
        "ps_madds1 10, 3, 7, 10\n\t"
        "ps_madds0 9, 4, 8, 9\n\t"
        "ps_madds0 10, 5, 8, 10\n\t"
        "ps_madds1 10, 6, 8, 10\n\t"
        "psq_st 9, 32(%2), 0, 0\n\t"
        "psq_st 10, 40(%2), 0, 0\n\t"
        :
        : "b"(a->m), "b"(b->m), "b"(out->m), "b"(k01)
        : "memory", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10");
}

void wl_skin_points(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx) {
    for (uint32_t i = 0; i < n; i++) {
        const float *m = mtx[env[i]].c;
        const float *p = in + 3u * i;
        float *o = out + 3u * i;
        // f0 = (x y), f1 = (z 1), f2..f5 the columns (m00 m10) (m01 m11) (m02 m12) (m03 m13), f6 f7 the third row's pairs.
        __asm__ volatile(
            "psq_l 0, 0(%0), 0, 0\n\t"
            "psq_l 1, 8(%0), 1, 0\n\t"
            "psq_l 2, 0(%1), 0, 0\n\t"
            "psq_l 3, 8(%1), 0, 0\n\t"
            "psq_l 4, 16(%1), 0, 0\n\t"
            "psq_l 5, 24(%1), 0, 0\n\t"
            "psq_l 6, 32(%1), 0, 0\n\t"
            "psq_l 7, 40(%1), 0, 0\n\t"
            "ps_muls0 8, 2, 0\n\t"
            "ps_madds1 8, 3, 0, 8\n\t"
            "ps_madds0 8, 4, 1, 8\n\t"
            "ps_add 8, 8, 5\n\t"
            "ps_mul 9, 6, 0\n\t"
            "ps_madd 9, 7, 1, 9\n\t"
            "ps_sum0 9, 9, 9, 9\n\t"
            "psq_st 8, 0(%2), 0, 0\n\t"
            "psq_st 9, 8(%2), 1, 0\n\t"
            :
            : "b"(p), "b"(m), "b"(o)
            : "memory", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9");
    }
}

void wl_skin_normals(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx) {
    static const float k[3] = {SKIN_EPS, 0.5f, 1.5f};
    for (uint32_t i = 0; i < n; i++) {
        const float *m = mtx[env[i]].c;
        const float *p = in + 3u * i;
        float *o = out + 3u * i;
        // f8 = (x' y'), f9 = (z' ...) after the 3x3; f11 = the squared length; f12.. the reciprocal square root with two
        // Newton steps (y = y * (1.5 - 0.5 * len2 * y * y)); f13 = eps, f14 = 0.5, f15 = 1.5.
        __asm__ volatile(
            "psq_l 0, 0(%0), 0, 0\n\t"
            "psq_l 1, 8(%0), 1, 0\n\t"
            "psq_l 2, 0(%1), 0, 0\n\t"
            "psq_l 3, 8(%1), 0, 0\n\t"
            "psq_l 4, 16(%1), 0, 0\n\t"
            "psq_l 6, 32(%1), 0, 0\n\t"
            "psq_l 7, 40(%1), 0, 0\n\t"
            "lfs 13, 0(%3)\n\t"
            "lfs 14, 4(%3)\n\t"
            "lfs 15, 8(%3)\n\t"
            "ps_muls0 8, 2, 0\n\t"
            "ps_madds1 8, 3, 0, 8\n\t"
            "ps_madds0 8, 4, 1, 8\n\t"
            "ps_mul 9, 6, 0\n\t"
            "ps_muls0 10, 7, 1\n\t"
            "ps_sum0 9, 9, 9, 9\n\t"
            "ps_add 9, 9, 10\n\t"
            "ps_mul 11, 8, 8\n\t"
            "ps_sum0 11, 11, 11, 11\n\t"
            "ps_madd 11, 9, 9, 11\n\t"
            "fsubs 12, 11, 13\n\t"
            "fsel 11, 12, 11, 13\n\t"
            "frsqrte 12, 11\n\t"
            "fmuls 16, 12, 12\n\t"
            "fmuls 16, 16, 11\n\t"
            "fnmsubs 16, 16, 14, 15\n\t"
            "fmuls 12, 12, 16\n\t"
            "fmuls 16, 12, 12\n\t"
            "fmuls 16, 16, 11\n\t"
            "fnmsubs 16, 16, 14, 15\n\t"
            "fmuls 12, 12, 16\n\t"
            "ps_muls0 8, 8, 12\n\t"
            "ps_muls0 9, 9, 12\n\t"
            "psq_st 8, 0(%2), 0, 0\n\t"
            "psq_st 9, 8(%2), 1, 0\n\t"
            :
            : "b"(p), "b"(m), "b"(o), "b"(k)
            : "memory", "fr0", "fr1", "fr2", "fr3", "fr4", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13",
              "fr14", "fr15", "fr16");
    }
}

#else

// ---- host: the same functions in plain C ------------------------------------------------------

bool wl_ps_available(void) {
    return false;
}

void wl_mtx_concat(WlMat34 *out, const WlMat34 *a, const WlMat34 *b) {
    wl_mat34_mul(out, a, b);
}

void wl_skin_points(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx) {
    wl_skin_points_ref(out, in, n, env, mtx);
}

void wl_skin_normals(float *out, const float *in, uint32_t n, const uint16_t *env, const WlSkinMtx *mtx) {
    wl_skin_normals_ref(out, in, n, env, mtx);
}

#endif
