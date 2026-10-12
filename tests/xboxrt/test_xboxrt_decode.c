// SPDX-License-Identifier: MIT
// The device-fault decoders against a fake device, as instruction encodings: the PowerPC
// load/store matrix (every form, both byte orders, update forms, sign extension, the refused
// ones) and the x86-64 forms GCC emits for the MEM macros (moves, extensions, the ALU and group-1
// read-modify-writes with their flags, inc/dec, xchg/cmpxchg/xadd, high-byte registers, SSE moves).
// Portable: the same results on every host.
#include <string.h>

#include "greatest.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/ppc_decode.h"
#include "wl/xboxrt/x86_decode.h"

#define DEV 0xFE100000u

static uint8_t g_mem[256]; // the device's registers, little-endian bytes
static uint32_t g_reads, g_writes;

static uint32_t dev_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    g_reads++;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        v |= (uint32_t)g_mem[(off + i) & 255u] << (8u * i);
    }
    return v;
}

static void dev_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    (void)user;
    g_writes++;
    for (unsigned i = 0; i < size; i++) {
        g_mem[(off + i) & 255u] = (uint8_t)(value >> (8u * i));
    }
}

static void setup(void) {
    wl_xb_mmio_reset();
    WlXbDevice d = {"fake", DEV, 0x100u, dev_read, dev_write, NULL};
    wl_xb_mmio_register(&d);
    memset(g_mem, 0, sizeof g_mem);
    for (int i = 0; i < 16; i++) {
        g_mem[i] = (uint8_t)(0x10 + i); // 10 11 12 13 14 ...
    }
    g_mem[0x20] = 0xF0; // a negative halfword: F0 FF
    g_mem[0x21] = 0xFF;
    g_reads = g_writes = 0;
}

static uint32_t le32(uint32_t off) {
    return (uint32_t)g_mem[off] | (uint32_t)g_mem[off + 1] << 8 | (uint32_t)g_mem[off + 2] << 16 |
           (uint32_t)g_mem[off + 3] << 24;
}

// ---- PowerPC ------------------------------------------------------------------------------------

static uint32_t d_form(uint32_t op, uint32_t rt, uint32_t ra, int16_t d) {
    return op << 26 | rt << 21 | ra << 16 | (uint16_t)d;
}

static uint32_t x_form(uint32_t xo, uint32_t rt, uint32_t ra, uint32_t rb) {
    return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1;
}

// Decodes and emulates `insn` with r4 = EA base (DEV + off_base), r5 = an index.
static bool run_ppc(uint32_t insn, uint32_t gpr[32]) {
    WlXbPpcAccess a;
    if (wl_xb_ppc_decode(insn, &a) != WL_XB_PPC_OK) {
        return false;
    }
    uint32_t ea = wl_xb_ppc_ea(&a, gpr);
    return wl_xb_ppc_emulate(&a, gpr, ea, ea); // EA = guest VA here (the Wii's case: base 0)
}

TEST ppc_loads(void) {
    setup();
    uint32_t g[32] = {0};
    struct {
        uint32_t insn;
        uint32_t want;
        const char *name;
    } cases[] = {
        {d_form(32, 3, 4, 0), 0x10111213u, "lwz"},       // a big-endian load sees the bytes swapped
        {d_form(34, 3, 4, 2), 0x12u, "lbz"},
        {d_form(40, 3, 4, 4), 0x1415u, "lhz"},
        {d_form(42, 3, 4, 0x20), 0xFFFFFFF0u + 0u, "lha"}, // F0 FF read big-endian is F0FF: -3841
        {x_form(534, 3, 4, 5), 0x13121110u, "lwbrx"},     // the little-endian value
        {x_form(790, 3, 4, 5), 0x1110u, "lhbrx"},
        {x_form(23, 3, 4, 5), 0x10111213u, "lwzx"},
        {x_form(87, 3, 4, 5), 0x10u, "lbzx"},
        {x_form(279, 3, 4, 5), 0x1011u, "lhzx"},
        {x_form(343, 3, 4, 6), 0xFFFFFFF0u, "lhax"},
    };
    cases[3].want = 0xFFFFF0FFu;
    cases[9].want = 0xFFFFF0FFu;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(g, 0, sizeof g);
        g[4] = DEV;
        g[5] = 0;
        g[6] = 0x20;
        ASSERT_FALSEm(cases[i].name, !run_ppc(cases[i].insn, g));
        ASSERT_EQ_FMTm(cases[i].name, cases[i].want, g[3], "%08x");
    }
    PASS();
}

TEST ppc_stores(void) {
    setup();
    uint32_t g[32] = {0};
    g[4] = DEV;
    g[5] = 0x40;
    g[3] = 0xAABBCCDDu;
    ASSERT(run_ppc(d_form(36, 3, 4, 0x40), g)); // stw: big-endian bytes AA BB CC DD
    ASSERT_EQ_FMT(0xDDCCBBAAu, le32(0x40), "%08x");
    ASSERT(run_ppc(x_form(662, 3, 4, 5), g)); // stwbrx: little-endian bytes
    ASSERT_EQ_FMT(0xAABBCCDDu, le32(0x40), "%08x");
    ASSERT(run_ppc(d_form(44, 3, 4, 0x48), g)); // sth: CC DD
    ASSERT_EQ(0xCC, g_mem[0x48]);
    ASSERT_EQ(0xDD, g_mem[0x49]);
    ASSERT(run_ppc(x_form(918, 3, 4, 5), g)); // sthbrx at 0x40
    ASSERT_EQ(0xDD, g_mem[0x40]);
    ASSERT_EQ(0xCC, g_mem[0x41]);
    ASSERT(run_ppc(d_form(38, 3, 4, 0x50), g)); // stb
    ASSERT_EQ(0xDD, g_mem[0x50]);
    ASSERT(run_ppc(x_form(151, 3, 4, 5), g)); // stwx
    ASSERT_EQ_FMT(0xDDCCBBAAu, le32(0x40), "%08x");
    ASSERT(run_ppc(x_form(215, 3, 4, 5), g)); // stbx
    ASSERT(run_ppc(x_form(407, 3, 4, 5), g)); // sthx
    PASS();
}

TEST ppc_update_forms(void) {
    setup();
    uint32_t g[32] = {0};
    g[4] = DEV;
    ASSERT(run_ppc(d_form(33, 3, 4, 8), g)); // lwzu: r4 gets the EA
    ASSERT_EQ_FMT(0x18191A1Bu, g[3], "%08x");
    ASSERT_EQ_FMT(DEV + 8u, g[4], "%08x");
    g[3] = 0x01020304u;
    ASSERT(run_ppc(d_form(37, 3, 4, 8), g)); // stwu
    ASSERT_EQ_FMT(DEV + 16u, g[4], "%08x");
    ASSERT_EQ_FMT(0x04030201u, le32(16), "%08x");
    g[5] = 4;
    ASSERT(run_ppc(x_form(55, 3, 4, 5), g)); // lwzux
    ASSERT_EQ_FMT(DEV + 20u, g[4], "%08x");
    ASSERT(run_ppc(d_form(35, 6, 4, 1), g)); // lbzu
    ASSERT(run_ppc(d_form(41, 6, 4, 1), g)); // lhzu
    ASSERT(run_ppc(d_form(43, 6, 4, 1), g)); // lhau
    ASSERT(run_ppc(d_form(39, 6, 4, 1), g)); // stbu
    ASSERT(run_ppc(d_form(45, 6, 4, 1), g)); // sthu
    ASSERT_EQ_FMT(DEV + 25u, g[4], "%08x");
    PASS();
}

TEST ppc_refused_and_unknown(void) {
    WlXbPpcAccess a;
    ASSERT_EQ(WL_XB_PPC_REFUSED, wl_xb_ppc_decode(d_form(46, 3, 4, 0), &a)); // lmw
    ASSERT_STR_EQ("lmw", a.name);
    ASSERT_EQ(WL_XB_PPC_REFUSED, wl_xb_ppc_decode(d_form(47, 3, 4, 0), &a)); // stmw
    ASSERT_EQ(WL_XB_PPC_REFUSED, wl_xb_ppc_decode(x_form(1014, 0, 4, 5), &a)); // dcbz
    ASSERT_STR_EQ("dcbz", a.name);
    ASSERT_EQ(WL_XB_PPC_REFUSED, wl_xb_ppc_decode(x_form(20, 3, 4, 5), &a)); // lwarx
    ASSERT_EQ(WL_XB_PPC_REFUSED, wl_xb_ppc_decode(d_form(48, 1, 4, 0), &a)); // lfs
    ASSERT_EQ(WL_XB_PPC_REFUSED, wl_xb_ppc_decode(d_form(54, 1, 4, 0), &a)); // stfd
    ASSERT_EQ(WL_XB_PPC_UNKNOWN, wl_xb_ppc_decode(0x7C0802A6u, &a));         // mflr
    ASSERT_EQ(WL_XB_PPC_UNKNOWN, wl_xb_ppc_decode(0x38600000u, &a));         // li
    // ra = 0 reads as zero in the EA
    uint32_t g[32] = {0};
    g[0] = 0x12345678u;
    ASSERT_EQ(WL_XB_PPC_OK, wl_xb_ppc_decode(d_form(32, 3, 0, 0x100), &a));
    ASSERT_EQ_FMT(0x100u, wl_xb_ppc_ea(&a, g), "%x");
    PASS();
}

// ---- x86-64 ------------------------------------------------------------------------------------

static uint32_t run_x86(const uint8_t *code, WlXbX86Regs *r, uint32_t va) {
    const char *what = NULL;
    uint32_t len = wl_xb_x86_emulate(code, r, va, &what);
    return len;
}

TEST x86_moves(void) {
    setup();
    WlXbX86Regs r;
    memset(&r, 0, sizeof r);
    r.gpr[0] = 0xFFFFFFFFFFFFFFFFull;
    const uint8_t mov_load[] = {0x8B, 0x04, 0x02};             // mov eax, [rdx+rax]
    ASSERT_EQ(3u, run_x86(mov_load, &r, DEV));
    ASSERT_EQ(0x13121110ull, r.gpr[0]);                         // a 32-bit write clears the top
    const uint8_t mov_store[] = {0x89, 0x0E};                   // mov [rsi], ecx
    r.gpr[1] = 0xCAFEBABEu;
    ASSERT_EQ(2u, run_x86(mov_store, &r, DEV + 0x40u));
    ASSERT_EQ_FMT(0xCAFEBABEu, le32(0x40), "%08x");
    const uint8_t mov_imm[] = {0xC7, 0x47, 0x08, 0x78, 0x56, 0x34, 0x12}; // mov dword [rdi+8], 0x12345678
    ASSERT_EQ(7u, run_x86(mov_imm, &r, DEV + 0x48u));
    ASSERT_EQ_FMT(0x12345678u, le32(0x48), "%08x");
    const uint8_t mov8_imm[] = {0xC6, 0x00, 0x99};              // mov byte [rax], 0x99
    ASSERT_EQ(3u, run_x86(mov8_imm, &r, DEV + 0x50u));
    ASSERT_EQ(0x99, g_mem[0x50]);
    const uint8_t mov16[] = {0x66, 0x8B, 0x08};                 // mov cx, [rax]
    r.gpr[1] = 0x1111111111111111ull;
    ASSERT_EQ(3u, run_x86(mov16, &r, DEV));
    ASSERT_EQ(0x1111111111111110ull, r.gpr[1]); // a 16-bit write keeps the rest
    const uint8_t mov8_hi[] = {0x8A, 0x20};                     // mov ah, [rax]
    r.gpr[0] = 0;
    ASSERT_EQ(2u, run_x86(mov8_hi, &r, DEV + 3u));
    ASSERT_EQ(0x1300ull, r.gpr[0]);
    const uint8_t mov8_rex[] = {0x40, 0x8A, 0x30};              // mov sil, [rax]
    r.gpr[6] = 0xFF00;
    ASSERT_EQ(3u, run_x86(mov8_rex, &r, DEV + 2u));
    ASSERT_EQ(0xFF12ull, r.gpr[6]);
    const uint8_t movzx[] = {0x0F, 0xB7, 0x48, 0x04};           // movzx ecx, word [rax+4]
    ASSERT_EQ(4u, run_x86(movzx, &r, DEV + 4u));
    ASSERT_EQ(0x1514ull, r.gpr[1]);
    const uint8_t movsx[] = {0x0F, 0xBF, 0x08};                 // movsx ecx, word [rax]
    ASSERT_EQ(3u, run_x86(movsx, &r, DEV + 0x20u));
    ASSERT_EQ(0xFFFFFFF0ull, r.gpr[1]); // F0 FF: -16, in a 32-bit register
    const uint8_t movsxd[] = {0x48, 0x63, 0x08};                // movsxd rcx, dword [rax]
    g_mem[0x60] = 0xFE;
    g_mem[0x61] = 0xFF;
    g_mem[0x62] = 0xFF;
    g_mem[0x63] = 0xFF;
    ASSERT_EQ(3u, run_x86(movsxd, &r, DEV + 0x60u));
    ASSERT_EQ(0xFFFFFFFFFFFFFFFEull, r.gpr[1]);
    const uint8_t rex_r[] = {0x44, 0x8B, 0x00};                 // mov r8d, [rax]
    ASSERT_EQ(3u, run_x86(rex_r, &r, DEV));
    ASSERT_EQ(0x13121110ull, r.gpr[8]);
    const uint8_t sib_disp32[] = {0x8B, 0x84, 0x91, 0x00, 0x01, 0x00, 0x00}; // mov eax, [rcx+rdx*4+0x100]
    ASSERT_EQ(7u, run_x86(sib_disp32, &r, DEV));
    PASS();
}

TEST x86_alu_and_flags(void) {
    setup();
    WlXbX86Regs r;
    memset(&r, 0, sizeof r);
    const uint8_t or_imm32[] = {0x81, 0x08, 0x00, 0x00, 0x01, 0x00}; // or dword [rax], 0x10000
    ASSERT_EQ(6u, run_x86(or_imm32, &r, DEV + 0x40u));
    ASSERT_EQ_FMT(0x10000u, le32(0x40), "%08x");
    const uint8_t and_imm8[] = {0x83, 0x20, 0xFE};                   // and dword [rax], -2
    g_mem[0x40] = 0x3;
    ASSERT_EQ(3u, run_x86(and_imm8, &r, DEV + 0x40u));
    ASSERT_EQ_FMT(0x10002u, le32(0x40), "%08x");
    const uint8_t cmp_imm8[] = {0x83, 0x38, 0x05};                   // cmp dword [rax], 5
    g_mem[0x44] = 3;
    ASSERT_EQ(3u, run_x86(cmp_imm8, &r, DEV + 0x44u));
    ASSERT(r.rflags & WL_XB_X86_CF);   // 3 < 5 unsigned
    ASSERT(r.rflags & WL_XB_X86_SF);   // 3 - 5 negative
    ASSERT(!(r.rflags & WL_XB_X86_ZF));
    ASSERT(!(r.rflags & WL_XB_X86_OF));
    ASSERT_EQ(3, g_mem[0x44]);         // cmp writes nothing
    const uint8_t test_r[] = {0x85, 0x08};                           // test [rax], ecx
    r.gpr[1] = 0x4;
    ASSERT_EQ(2u, run_x86(test_r, &r, DEV + 0x44u));
    ASSERT(r.rflags & WL_XB_X86_ZF);
    const uint8_t test_imm[] = {0xF7, 0x00, 0x00, 0x00, 0x01, 0x00}; // test dword [rax], 0x10000
    ASSERT_EQ(6u, run_x86(test_imm, &r, DEV + 0x40u));
    ASSERT(!(r.rflags & WL_XB_X86_ZF));
    const uint8_t add_rm_r[] = {0x01, 0x08};                         // add [rax], ecx
    g_mem[0x48] = 0xFF;
    g_mem[0x49] = 0xFF;
    g_mem[0x4A] = 0xFF;
    g_mem[0x4B] = 0x7F;
    r.gpr[1] = 1;
    ASSERT_EQ(2u, run_x86(add_rm_r, &r, DEV + 0x48u));
    ASSERT_EQ_FMT(0x80000000u, le32(0x48), "%08x");
    ASSERT(r.rflags & WL_XB_X86_OF);   // signed overflow
    ASSERT(!(r.rflags & WL_XB_X86_CF));
    const uint8_t sub_r_rm[] = {0x2B, 0x08};                         // sub ecx, [rax]
    r.gpr[1] = 0x80000000u;
    ASSERT_EQ(2u, run_x86(sub_r_rm, &r, DEV + 0x48u));
    ASSERT_EQ(0ull, r.gpr[1]);
    ASSERT(r.rflags & WL_XB_X86_ZF);
    const uint8_t xor_r_rm[] = {0x33, 0x08};                         // xor ecx, [rax]
    r.gpr[1] = 0x0F;
    ASSERT_EQ(2u, run_x86(xor_r_rm, &r, DEV));
    ASSERT_EQ(0x1312111Full, r.gpr[1]);
    const uint8_t inc[] = {0xFF, 0x00};                              // inc dword [rax]
    r.rflags = WL_XB_X86_CF;
    ASSERT_EQ(2u, run_x86(inc, &r, DEV + 0x50u));
    ASSERT_EQ_FMT(1u, le32(0x50), "%08x");
    ASSERT(r.rflags & WL_XB_X86_CF); // inc keeps CF
    const uint8_t dec8[] = {0xFE, 0x08};                             // dec byte [rax]
    ASSERT_EQ(2u, run_x86(dec8, &r, DEV + 0x50u));
    ASSERT_EQ(0, g_mem[0x50]);
    ASSERT(r.rflags & WL_XB_X86_ZF);
    PASS();
}

TEST x86_exchange_and_sse(void) {
    setup();
    WlXbX86Regs r;
    memset(&r, 0, sizeof r);
    const uint8_t xchg[] = {0x87, 0x08}; // xchg [rax], ecx
    r.gpr[1] = 0xAAAAAAAAu;
    ASSERT_EQ(2u, run_x86(xchg, &r, DEV));
    ASSERT_EQ(0x13121110ull, r.gpr[1]);
    ASSERT_EQ_FMT(0xAAAAAAAAu, le32(0), "%08x");
    const uint8_t cmpxchg[] = {0xF0, 0x0F, 0xB1, 0x0A}; // lock cmpxchg [rdx], ecx
    r.gpr[0] = 0xAAAAAAAAu;
    r.gpr[1] = 0x55u;
    ASSERT_EQ(4u, run_x86(cmpxchg, &r, DEV));
    ASSERT(r.rflags & WL_XB_X86_ZF);
    ASSERT_EQ_FMT(0x55u, le32(0), "%08x");
    r.gpr[0] = 0x1;
    ASSERT_EQ(4u, run_x86(cmpxchg, &r, DEV));
    ASSERT(!(r.rflags & WL_XB_X86_ZF));
    ASSERT_EQ(0x55ull, r.gpr[0]);
    const uint8_t xadd[] = {0x0F, 0xC1, 0x08}; // xadd [rax], ecx
    r.gpr[1] = 3;
    ASSERT_EQ(3u, run_x86(xadd, &r, DEV));
    ASSERT_EQ(0x55ull, r.gpr[1]);
    ASSERT_EQ_FMT(0x58u, le32(0), "%08x");
    const uint8_t movss_load[] = {0xF3, 0x0F, 0x10, 0x00}; // movss xmm0, [rax]
    float f = 2.5f;
    memcpy(&g_mem[0x70], &f, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint32_t fb;
    memcpy(&fb, &f, 4);
    for (int i = 0; i < 4; i++) {
        g_mem[0x70 + i] = (uint8_t)(fb >> (8 * i));
    }
#endif
    memset(r.xmm[0], 0xEE, 16);
    ASSERT_EQ(4u, run_x86(movss_load, &r, DEV + 0x70u));
    ASSERT_EQ(0, r.xmm[0][4]); // the upper lanes cleared
    uint32_t bits = (uint32_t)r.xmm[0][0] | (uint32_t)r.xmm[0][1] << 8 | (uint32_t)r.xmm[0][2] << 16 |
                    (uint32_t)r.xmm[0][3] << 24;
    ASSERT_EQ_FMT(0x40200000u, bits, "%08x");
    const uint8_t movd_store[] = {0x66, 0x0F, 0x7E, 0x08}; // movd [rax], xmm1
    r.xmm[1][0] = 0x44;
    r.xmm[1][1] = 0x33;
    r.xmm[1][2] = 0x22;
    r.xmm[1][3] = 0x11;
    ASSERT_EQ(4u, run_x86(movd_store, &r, DEV + 0x80u));
    ASSERT_EQ_FMT(0x11223344u, le32(0x80), "%08x");
    const uint8_t movups[] = {0x0F, 0x10, 0x10}; // movups xmm2, [rax]
    ASSERT_EQ(3u, run_x86(movups, &r, DEV));
    ASSERT_EQ(0x58, r.xmm[2][0]);
    ASSERT_EQ(0x1F, r.xmm[2][15]);
    // not emulated: a call through a device register, and a register operand
    const uint8_t call_mem[] = {0xFF, 0x10};
    ASSERT_EQ(0u, run_x86(call_mem, &r, DEV));
    const uint8_t reg_op[] = {0x89, 0xC8};
    ASSERT_EQ(0u, run_x86(reg_op, &r, DEV));
    PASS();
}

SUITE(decode_suite) {
    RUN_TEST(ppc_loads);
    RUN_TEST(ppc_stores);
    RUN_TEST(ppc_update_forms);
    RUN_TEST(ppc_refused_and_unknown);
    RUN_TEST(x86_moves);
    RUN_TEST(x86_alu_and_flags);
    RUN_TEST(x86_exchange_and_sse);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(decode_suite);
    GREATEST_MAIN_END();
}
