// SPDX-License-Identifier: MIT
// Paired-single routines against their plain C twins (on the host and ppc-be both run the C path, so
// this checks the routines' contract; on the console the same test compares the assembly).
#include "wl/gfx/gfx_ps.h"

#include <math.h>
#include <string.h>

#include "greatest.h"

#define NEAR_EQ(a, b, eps) ASSERT_IN_RANGE((double)(b), (double)(a), (double)(eps))

static int mats_close(const WlMat34 *a, const WlMat34 *b, double eps) {
    for (int i = 0; i < 12; i++) {
        if (fabs((double)a->m[i] - (double)b->m[i]) > eps) {
            return 0;
        }
    }
    return 1;
}

TEST skin_routines_match_their_plain_c_twins(void) {
    WlMat34 a, b, c1, c2;
    uint32_t seed = 3;
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 12; i++) {
            seed = seed * 1664525u + 1013904223u;
            a.m[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 8.0f;
            seed = seed * 1664525u + 1013904223u;
            b.m[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 8.0f;
        }
        wl_mtx_concat(&c1, &a, &b);
        wl_mtx_concat_ref(&c2, &a, &b);
        wl_mat34_mul(&c2, &a, &b);
        ASSERT(mats_close(&c1, &c2, 1e-4));
        // Aliasing: out = a, out = b.
        WlMat34 t = a;
        wl_mtx_concat(&t, &t, &b);
        ASSERT(mats_close(&t, &c2, 1e-4));
        t = b;
        wl_mtx_concat(&t, &a, &t);
        ASSERT(mats_close(&t, &c2, 1e-4));
    }
    // Vertex loops over mixed envelopes.
    enum { N = 97, E = 5 };
    WlMat34 skin[4];
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 12; i++) {
            seed = seed * 1664525u + 1013904223u;
            skin[k].m[i] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 3.0f;
        }
    }
    WlSkinMtx mtx[E];
    const uint16_t bones[E][4] = {{0, 0, 0, 0}, {0, 1, 0, 0}, {1, 2, 3, 0}, {3, 2, 1, 0}, {2, 0, 0, 0}};
    const float w[E][4] = {{1, 0, 0, 0}, {0.25f, 0.75f, 0, 0}, {0.5f, 0.25f, 0.25f, 0}, {0.1f, 0.2f, 0.7f, 0}, {1, 0, 0, 0}};
    const uint32_t cnt[E] = {1, 2, 3, 3, 1};
    for (int e = 0; e < E; e++) {
        wl_skinmtx_blend(&mtx[e], skin, bones[e], w[e], cnt[e]);
    }
    float in[3 * N], out1[3 * N], out2[3 * N];
    uint16_t env[N];
    for (int i = 0; i < N; i++) {
        env[i] = (uint16_t)(i % E);
        for (int c = 0; c < 3; c++) {
            seed = seed * 1664525u + 1013904223u;
            in[3 * i + c] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 4.0f;
        }
    }
    in[3 * 7] = in[3 * 7 + 1] = in[3 * 7 + 2] = 0.0f; // a zero normal stays zero
    wl_skin_points(out1, in, N, env, mtx);
    wl_skin_points_ref(out2, in, N, env, mtx);
    for (int i = 0; i < 3 * N; i++) {
        NEAR_EQ(out2[i], out1[i], 1e-4 * (1.0 + fabs(out2[i])));
    }
    wl_skin_normals(out1, in, N, env, mtx);
    wl_skin_normals_ref(out2, in, N, env, mtx);
    for (int i = 0; i < 3 * N; i++) {
        ASSERT(isfinite(out1[i]));
        NEAR_EQ(out2[i], out1[i], 1e-4);
    }
    for (int c = 0; c < 3; c++) {
        NEAR_EQ(0.0, out1[3 * 7 + c], 1e-9);
    }
    // The blended matrix equals the weighted sum of the bone matrices applied to a point.
    WlVec3 pt = wl_vec3(0.5f, -1.0f, 2.0f);
    float one[3], pin[3] = {pt.x, pt.y, pt.z};
    uint16_t e1 = 2;
    wl_skin_points_ref(one, pin, 1, &e1, mtx);
    float sum[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        WlVec3 q = wl_mat34_transform_point(&skin[bones[2][k]], pt);
        sum[0] += w[2][k] * q.x;
        sum[1] += w[2][k] * q.y;
        sum[2] += w[2][k] * q.z;
    }
    for (int c = 0; c < 3; c++) {
        NEAR_EQ(sum[c], one[c], 1e-4);
    }
    PASS();
}

SUITE(gfx_ps_suite) {
    RUN_TEST(skin_routines_match_their_plain_c_twins);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gfx_ps_suite);
    GREATEST_MAIN_END();
}
