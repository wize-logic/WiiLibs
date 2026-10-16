// SPDX-License-Identifier: MIT
// A case of recomp_types.h's SSE helpers that a compiler got wrong: rows of a 3x3 matrix loaded as SSE
// code loads them (movss, then movhps for the other two), each scaled by a broadcast element (shufps 0)
// and summed (mulps, addps), the sum's lanes put back in order (shufps) and stored with movhps and
// movss. devkitPPC's GCC 16.1 (-Os and -O2) miscompiled XMM_SHUFFLE when it moved the lanes as bytes
// into the little-endian union: the lanes stored after it came out byte-reversed. The host and ppc-be
// run it in test_recomp_header.c, the console (devkitPPC's compiler) in suite_recomp_header.c.
// The including file defines what recomp_types.h needs and points g_xbox_mem_offset at guest memory
// of at least 0x400 bytes.
#pragma once

#include <stdint.h>

// the case's addresses: the matrix, the scales, the result
#define RECOMP_CASE_M 0x100u
#define RECOMP_CASE_V 0x200u
#define RECOMP_CASE_OUT 0x300u

static void recomp_case_shufps_movhps_run(uint32_t out, uint32_t m, uint32_t v) {
    RecompXmm xmm0, xmm1, xmm2, xmm3, xmm4, xmm5;
    xmm0 = XMM_SCALAR(MEMF(m + 0x00));
    XMM_LOAD_HIGH(xmm0, m + 0x04);
    xmm1 = XMM_SCALAR(MEMF(m + 0x0C));
    XMM_LOAD_HIGH(xmm1, m + 0x10);
    xmm2 = XMM_SCALAR(MEMF(m + 0x20));
    XMM_LOAD_HIGH(xmm2, m + 0x18);
    xmm2 = XMM_SHUFFLE(xmm2, xmm2, 0x36);
    xmm3 = XMM_SCALAR(MEMF(v + 0x00));
    xmm3 = XMM_SHUFFLE(xmm3, xmm3, 0);
    xmm4 = XMM_SCALAR(MEMF(v + 0x04));
    xmm4 = XMM_SHUFFLE(xmm4, xmm4, 0);
    xmm5 = XMM_SCALAR(MEMF(v + 0x08));
    xmm5 = XMM_SHUFFLE(xmm5, xmm5, 0);
    xmm3 = XMM_MUL(xmm3, xmm0);
    xmm4 = XMM_MUL(xmm4, xmm1);
    xmm5 = XMM_MUL(xmm5, xmm2);
    xmm3 = XMM_ADD(xmm3, xmm4);
    xmm3 = XMM_ADD(xmm3, xmm5);
    xmm3 = XMM_SHUFFLE(xmm3, xmm3, 0x8F);
    XMM_STORE_HIGH(out + 0x00, xmm3);
    MEMF_SET(out + 0x08, xmm3.f[0]);
}

// Runs the case on 1..9 scaled by 1, 2 and 4: the rows are (1, 0, 2, 3), (4, 0, 5, 6) and, after
// the shufps, (7, 0, 8, 9), so the sum is (37, 0, 44, 51) and the stores give 37, 44, 51 as
// little-endian floats. The number of the three words that are wrong.
static int recomp_case_shufps_movhps(void) {
    for (uint32_t i = 0; i < 9u; i++) {
        MEMF_SET(RECOMP_CASE_M + 4u * i, (float)(i + 1u));
    }
    MEMF_SET(RECOMP_CASE_V + 0u, 1.0f);
    MEMF_SET(RECOMP_CASE_V + 4u, 2.0f);
    MEMF_SET(RECOMP_CASE_V + 8u, 4.0f);
    recomp_case_shufps_movhps_run(RECOMP_CASE_OUT, RECOMP_CASE_M, RECOMP_CASE_V);
    static const uint32_t want[3] = {0x42140000u, 0x42300000u, 0x424C0000u}; // 37, 44, 51
    int wrong = 0;
    for (uint32_t i = 0; i < 3u; i++) {
        const uint8_t *p = (const uint8_t *)XBOX_PTR(RECOMP_CASE_OUT + 4u * i);
        uint32_t got = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        wrong += got != want[i];
    }
    return wrong;
}
