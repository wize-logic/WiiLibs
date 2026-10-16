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

SUITE(recomp_header) {
    RUN_TEST(ea_transform);
    RUN_TEST(mem_reads_little_endian);
    RUN_TEST(mem_writes_little_endian);
    RUN_TEST(float_memory);
    RUN_TEST(atomics);
    RUN_TEST(simd_lanes_overlay_as_on_x86);
    RUN_TEST(shufps_then_movhps);
    RUN_TEST(cvt_si32);
    RUN_TEST(extended_80bit);
    RUN_TEST(fp_pc24_matches_the_slow_path);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(recomp_header);
    GREATEST_MAIN_END();
}
