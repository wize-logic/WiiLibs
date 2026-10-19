// SPDX-License-Identifier: MIT
// wl/xboxrt/recomp/recomp_types.h, the header generated code from xboxrecomp's lifter includes:
// guest memory holds little-endian bytes on any host (the macros read and write them, stores of
// floats and doubles included), XBOX_PTR applies the guest-to-effective-address transform, the
// atomics work on little-endian words, the SIMD unions overlay as on x86, and the helpers WiiLibs'
// patches added (recomp_cvt_si32, recomp_ld80/st80) and the RECOMP_FP_PC fast path give the
// answers the x86 does. Runs on the host and big-endian (ppc-be), where it matters.
#define RECOMP_GENERATED_CODE 1
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "recomp_types.h"

// what the header declares and these tests touch
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F;

#define GUEST_SIZE 0x00200000u
static uint8_t g_guest[GUEST_SIZE];

static void guest_reset(void) {
    memset(g_guest, 0, sizeof g_guest);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_guest;
}

#include "recomp_simd_case.h"

TEST ea_transform(void) {
    // user space keeps its address; the contiguous window and its tiled alias land at 0x1xxxxxxx
    ASSERT_EQ_FMT(0x00010000u, WL_XB_EA(0x00010000u), "%08x");
    ASSERT_EQ_FMT(0x03FFFFFCu, WL_XB_EA(0x03FFFFFCu), "%08x");
    ASSERT_EQ_FMT(0x10061000u, WL_XB_EA(0x80061000u), "%08x");
    ASSERT_EQ_FMT(0x103A6000u, WL_XB_EA(0x803A6000u), "%08x");
    ASSERT_EQ_FMT(0x10061000u, WL_XB_EA(0xF0061000u), "%08x");
    ASSERT_EQ_FMT(0x1D000000u, WL_XB_EA(0xFD000000u), "%08x");
    ASSERT_EQ_FMT(0x1FFFFFFFu, WL_XB_EA(0xFFFFFFFFu), "%08x");
    // a value computed at run time takes the same path as a constant
    volatile uint32_t v = 0x80061234u;
    ASSERT_EQ_FMT(0x10061234u, WL_XB_EA(v), "%08x");
    v = 0x0FFFFFFFu;
    ASSERT_EQ_FMT(0x0FFFFFFFu, WL_XB_EA(v), "%08x");
    guest_reset();
    ASSERT_EQ((uintptr_t)(g_guest + 0x1234), XBOX_PTR(0x1234u));
    PASS();
}

TEST mem_reads_little_endian(void) {
    guest_reset();
    static const uint8_t bytes[] = {0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A};
    memcpy(g_guest + 0x100, bytes, sizeof bytes);
    ASSERT_EQ_FMT(0x78u, (unsigned)MEM8(0x100), "%x");
    ASSERT_EQ_FMT(0x5678u, (unsigned)MEM16(0x100), "%x");
    ASSERT_EQ_FMT(0x12345678u, (unsigned)MEM32(0x100), "%08x");
    ASSERT_EQ_FMT(0x3456u, (unsigned)MEM16(0x101), "%x");        // unaligned
    ASSERT_EQ_FMT(0xF0123456u, (unsigned)MEM32(0x101), "%08x");
    ASSERT_EQ(-16, SMEM8(0x104));
    ASSERT_EQ(-8464, SMEM16(0x104));                               // 0xDEF0
    ASSERT_EQ((int32_t)0x9ABCDEF0, SMEM32(0x104));
    ASSERT_EQ((int64_t)0x9ABCDEF012345678ull, SMEM64(0x100));
    PASS();
}

TEST mem_writes_little_endian(void) {
    guest_reset();
    MEM32(0x200) = 0x11223344u;
    MEM16(0x204) = 0xAABBu;
    MEM8(0x206) = 0xCC;
    static const uint8_t want[] = {0x44, 0x33, 0x22, 0x11, 0xBB, 0xAA, 0xCC, 0x00};
    ASSERT_MEM_EQ(want, g_guest + 0x200, sizeof want);
    // read-modify-write as the lifter emits it
    MEM32(0x200) = MEM32(0x200) + 1u;
    ASSERT_EQ(0x45, g_guest[0x200]);
    // a 16-bit store between two 32-bit loads of the same word is seen (may_alias)
    uint32_t before = MEM32(0x200);
    MEM16(0x200) = 0xFFFF;
    ASSERT(before != MEM32(0x200));
    ASSERT_EQ_FMT(0x1122FFFFu, (unsigned)MEM32(0x200), "%08x");
    PASS();
}

TEST float_memory(void) {
    guest_reset();
    MEMF_SET(0x300, 1.5f);
    static const uint8_t f15[] = {0x00, 0x00, 0xC0, 0x3F};
    ASSERT_MEM_EQ(f15, g_guest + 0x300, 4);
    ASSERT_EQ(1.5f, MEMF(0x300));
    MEMD_SET(0x308, -2.0);
    static const uint8_t dm2[] = {0, 0, 0, 0, 0, 0, 0x00, 0xC0};
    ASSERT_MEM_EQ(dm2, g_guest + 0x308, 8);
    ASSERT_EQ(-2.0, MEMD(0x308));
    MEMD_SET(0x311, 3.141592653589793);                            // unaligned
    ASSERT_EQ(3.141592653589793, MEMD(0x311));
    MEMF_SET(0x320, (double)0.1f);                                  // a double argument narrows
    ASSERT_EQ(0.1f, MEMF(0x320));
    PASS();
}

TEST atomics(void) {
    guest_reset();
    MEM32(0x400) = 0x000000FFu;
    uint32_t old = RECOMP_ATOMIC_ADD32(XBOX_PTR(0x400), 1u);
    ASSERT_EQ_FMT(0xFFu, (unsigned)old, "%x");
    ASSERT_EQ_FMT(0x100u, (unsigned)MEM32(0x400), "%x");          // the carry went into byte 1
    ASSERT_EQ(0x01, g_guest[0x401]);
    old = RECOMP_ATOMIC_CAS32(XBOX_PTR(0x400), 0x100u, 0xCAFEBABEu);
    ASSERT_EQ_FMT(0x100u, (unsigned)old, "%x");
    ASSERT_EQ_FMT(0xCAFEBABEu, (unsigned)MEM32(0x400), "%08x");
    old = RECOMP_ATOMIC_CAS32(XBOX_PTR(0x400), 0x1u, 0x2u);         // compare fails: unchanged
    ASSERT_EQ_FMT(0xCAFEBABEu, (unsigned)old, "%08x");
    ASSERT_EQ_FMT(0xCAFEBABEu, (unsigned)MEM32(0x400), "%08x");
    ASSERT_EQ_FMT(0x12345678u, (unsigned)RECOMP_LE32(RECOMP_LE32(0x12345678u)), "%08x");
    PASS();
}

TEST simd_lanes_overlay_as_on_x86(void) {
    RecompMmx m;
    m.q = 0x0807060504030201ull;
    ASSERT_EQ(0x01, m.ub[0]);
    ASSERT_EQ(0x08, m.ub[7]);
    ASSERT_EQ(0x0201, m.uw[0]);
    ASSERT_EQ_FMT(0x04030201u, (unsigned)m.ud[0], "%08x");
    ASSERT_EQ_FMT(0x08070605u, (unsigned)m.ud[1], "%08x");
    uint8_t image[8];
    memcpy(image, &m, 8);                     // the register's bytes are the guest's byte image
    ASSERT_EQ(0x01, image[0]);
    m = MMX_FROM32(0xAABBCCDDu);
    ASSERT_EQ(0xDD, m.ub[0]);
    ASSERT_EQ(0ull, m.q >> 32);

    guest_reset();
    MEM32(0x500) = 0x3F800000u;               // 1.0f
    MEM32(0x504) = 0x40000000u;               // 2.0f
    MEM32(0x508) = 0x11223344u;
    MEM32(0x50C) = 0x55667788u;
    RecompXmm x = XMM_MEM(0x500);
    ASSERT_EQ(1.0f, x.f[0]);
    ASSERT_EQ(2.0f, x.f[1]);
    ASSERT_EQ_FMT(0x400000003F800000ull, (unsigned long long)x.q[0], "%llx");
    ASSERT_EQ_FMT(0x5566778811223344ull, (unsigned long long)x.q[1], "%llx");
    XMM_STORE(0x600, x);
    ASSERT_MEM_EQ(g_guest + 0x500, g_guest + 0x600, 16);
    RecompMmx r = MMX_MEM(0x500);
    ASSERT_EQ_FMT(0x3F800000u, (unsigned)r.ud[0], "%08x");
    MMX_STORE(0x700, r);
    ASSERT_MEM_EQ(g_guest + 0x500, g_guest + 0x700, 8);
    PASS();
}

TEST shufps_then_movhps(void) {
    // the case devkitPPC's GCC 16 got wrong (recomp_simd_case.h; the console runs it too)
    guest_reset();
    ASSERT_EQ(0, recomp_case_shufps_movhps());
    PASS();
}

TEST cvt_si32(void) {
    ASSERT_EQ(2, recomp_cvt_si32(1.5, 0));
    ASSERT_EQ(2, recomp_cvt_si32(2.5, 0));
    ASSERT_EQ(-2, recomp_cvt_si32(-2.5, 0));
    ASSERT_EQ(1, recomp_cvt_si32(1.5, 1));
    ASSERT_EQ(-1, recomp_cvt_si32(-1.9, 1));
    ASSERT_EQ(INT32_MIN, recomp_cvt_si32(3.0e9, 1));
    ASSERT_EQ(INT32_MIN, recomp_cvt_si32(-3.0e9, 0));
    ASSERT_EQ(INT32_MIN, recomp_cvt_si32(NAN, 0));
    ASSERT_EQ(INT32_MAX, recomp_cvt_si32(2147483647.4, 0));
    ASSERT_EQ(INT32_MIN, recomp_cvt_si32(2147483647.5, 0));
    ASSERT_EQ(2, MMX_CVT_F2I(2.5f, 0) + MMX_CVT_F2I(0.5f, 0) + MMX_CVT_F2I(-0.5f, 1));
    PASS();
}

// fist and frndint: the integer path for values under 2^30 against the libm path, every rounding mode and
// width, at the halves, the integers beside them, the ends of the fast range and random values of every size
TEST fist_and_frndint_match_the_wide_path(void) {
    static const double edge[] = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999999999999994, 32767.5,
                                  -32768.5, 32768.0, -32769.0, 1073741823.5, -1073741823.5, 1073741824.0,
                                  -1073741824.0, 2147483647.5, -2147483648.5, 4.6e18, -9.3e18, INFINITY, NAN};
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    unsigned long differ = 0;
    for (int i = 0; i < 400000; i++) {
        double v;
        if (i < (int)(sizeof edge / sizeof edge[0])) {
            v = edge[i];
        } else {
            rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
            v = ldexp((double)(int64_t)(rng >> 11) / 9007199254740992.0, (int)(rng % 40)); // |v| < 2^40
            if (i % 3 == 0) {
                v = floor(v * 2.0) / 2.0; // a half or an integer
            }
            if (i % 5 == 0) {
                v = -v;
            }
        }
        for (uint16_t mode = 0; mode < 4; mode++) {
            uint16_t cw = (uint16_t)(0x037F | mode << 10);
            for (unsigned bits = 16; bits <= 64; bits *= 2) {
                if (recomp_fist(v, cw, bits) != recomp_fist_wide(v, cw, bits)) {
                    differ++;
                }
            }
            double a = recomp_frndint(v, cw), b = recomp_frndint_wide(v, cw);
            if (memcmp(&a, &b, sizeof a) != 0 && !(a != a && b != b)) {
                differ++;
            }
        }
    }
    ASSERT_EQ_FMT(0ul, differ, "%lu");
    ASSERT_EQ(-3, recomp_fist(-2.5, 0x077F, 32)); // down
    ASSERT_EQ(-2, recomp_fist(-2.5, 0x037F, 32)); // nearest, even
    ASSERT_EQ(-32768, recomp_fist(32768.0, 0x0F7F, 16)); // out of range: the indefinite
    PASS();
}

TEST extended_80bit(void) {
    guest_reset();
    recomp_st80(0x800, 1.0);
    static const uint8_t one[] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F};
    ASSERT_MEM_EQ(one, g_guest + 0x800, 10);
    ASSERT_EQ(1.0, recomp_ld80(0x800));
    static const double values[] = {-2.5, 1e300, -1e-300, 5e-324, 3.141592653589793, 0.0};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        recomp_st80(0x810, values[i]);
        ASSERT_EQ(values[i], recomp_ld80(0x810));
    }
    recomp_st80(0x820, -0.0);
    ASSERT(signbit(recomp_ld80(0x820)));
    recomp_st80(0x830, INFINITY);
    ASSERT(isinf(recomp_ld80(0x830)));
    recomp_st80(0x840, NAN);
    ASSERT(isnan(recomp_ld80(0x840)));
    PASS();
}

// The fast path agrees with the exact rounding for every class of value.
TEST fp_pc24_matches_the_slow_path(void) {
    static const double edge[] = {0.0, -0.0, 1.0, 1.0 / 3.0, 3.4028234663852886e38, 3.4028235677973366e38,
                                  3.5e38, -1e39, 1.1754943508222875e-38, 1.1754942e-38, 1e-40, -1e-45,
                                  1e-300, 1e300, 16777217.0, INFINITY, -INFINITY};
    g_fp_control_word = 0x007F;   // PC = 24
    for (size_t i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        double fast = RECOMP_FP_PC(edge[i]), slow = recomp_fp_round24(edge[i]);
        ASSERT_MEM_EQ(&slow, &fast, sizeof fast);
    }
    uint32_t s = 12345u;
    for (int i = 0; i < 200000; i++) {
        s = s * 1664525u + 1013904223u;
        uint64_t bits = ((uint64_t)s << 32) ^ (uint64_t)(s * 2654435761u);
        double x;
        memcpy(&x, &bits, sizeof x);
        if (x != x) continue;
        double fast = RECOMP_FP_PC(x), slow = recomp_fp_round24(x);
        ASSERT_MEM_EQ(&slow, &fast, sizeof fast);
    }
    double nan_in = NAN;
    ASSERT(isnan(RECOMP_FP_PC(nan_in)));
    g_fp_control_word = 0x027F;   // PC = 53: unchanged
    ASSERT_EQ(1.0 / 3.0, RECOMP_FP_PC(1.0 / 3.0));
    PASS();
}

// Values live across the slow path survive it. On PowerPC it is a call that GCC is told clobbers
// only LR, so the accumulators below sit in volatile registers across it; a plain call of
// recomp_fp_round24 is the reference.
static __attribute__((noinline)) void pc24_accumulate(int keep, const double *din, const uint32_t *iin,
                                                      double *dout, uint32_t *iout) {
    double a0 = din[0], a1 = din[1], a2 = din[2], a3 = din[3], a4 = din[4], a5 = din[5];
    uint32_t b0 = iin[0], b1 = iin[1], b2 = iin[2], b3 = iin[3], b4 = iin[4], b5 = iin[5];
    for (int k = 1; k <= 64; k++) {
        double x = (k % 3 == 0 ? 0.0 : k & 1 ? 1e-40 : -3e39) * k;   // zero, denormal as a float, past its range
        double v = keep ? RECOMP_FP_PC(x) : recomp_fp_round24(x);
        a0 = a0 * 1.25 + v * 1e30; a1 -= a0 * 0.5; a2 = a2 * a1 + 1.0; a3 += v; a4 = a4 * 0.75 - a3;
        a5 += (double)k;
        b0 = b0 * 3u + (uint32_t)k; b1 ^= b0; b2 += b1 >> 3; b3 = b3 * 7u ^ b2; b4 -= b3; b5 += b4 | 1u;
    }
    dout[0] = a0; dout[1] = a1; dout[2] = a2; dout[3] = a3; dout[4] = a4; dout[5] = a5;
    iout[0] = b0; iout[1] = b1; iout[2] = b2; iout[3] = b3; iout[4] = b4; iout[5] = b5;
}

TEST fp_pc24_slow_path_keeps_registers(void) {
    static const double din[6] = {0.5, -1.5, 0.25, 2.0, 3.0, -4.0};
    static const uint32_t iin[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    double dk[6], dr[6];
    uint32_t ik[6], ir[6];
    g_fp_control_word = 0x007F;   // PC = 24
    pc24_accumulate(1, din, iin, dk, ik);
    pc24_accumulate(0, din, iin, dr, ir);
    ASSERT_MEM_EQ(dr, dk, sizeof dk);
    ASSERT_MEM_EQ(ir, ik, sizeof ik);
    PASS();
}

#if RECOMP_FP_ROUND24_KEEP
static uint32_t pc24_fpscr(void) {
    union { double d; uint64_t u; } c;
    __asm__ volatile("mffs %0" : "=f"(c.d));
    return (uint32_t)c.u;
}
#endif

// On PowerPC the fast path is frsp. and a branch on FPSCR[FX]: an overflow or an inexact underflow
// takes the slow path, which clears FX, OX and UX; FX left set by other code (an overflow of its
// own here) costs one slow path and never a wrong result.
TEST fp_pc24_exception_summary(void) {
    static const double seq[] = {1.5, 3.5e38, 0.1, 1e-40, 2.0 / 3.0, 5e-324, -7.25, -1e39, 1e-30, 0.0};
    volatile double big = 1e300;
    g_fp_control_word = 0x007F;   // PC = 24
    for (int round = 0; round < 3; round++) {
        if (round == 1) {
            volatile float f = (float)big;   // an overflow outside the x87 path: FX, OX set
            (void)f;
        }
        for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
            double x = seq[i] * (round == 2 ? -1.0 : 1.0);
            double fast = RECOMP_FP_PC(x);
#if RECOMP_FP_ROUND24_KEEP
            __asm__ volatile("" : "+f"(fast));
            ASSERT_EQ(0u, pc24_fpscr() & 0x18000000u);   // OX and UX never left set
#endif
            double slow = recomp_fp_round24(x);
            ASSERT_MEM_EQ(&slow, &fast, sizeof fast);
        }
    }
    PASS();
}

SUITE(recomp_header) {
    RUN_TEST(ea_transform);
    RUN_TEST(mem_reads_little_endian);
    RUN_TEST(mem_writes_little_endian);
    RUN_TEST(float_memory);
    RUN_TEST(atomics);
    RUN_TEST(simd_lanes_overlay_as_on_x86);
    RUN_TEST(shufps_then_movhps);
    RUN_TEST(cvt_si32);
    RUN_TEST(fist_and_frndint_match_the_wide_path);
    RUN_TEST(extended_80bit);
    RUN_TEST(fp_pc24_matches_the_slow_path);
    RUN_TEST(fp_pc24_slow_path_keeps_registers);
    RUN_TEST(fp_pc24_exception_summary);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(recomp_header);
    GREATEST_MAIN_END();
}
