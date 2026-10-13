// SPDX-License-Identifier: MIT
// The runtime's half of the generated code's ABI against the generated code's own header
// (wl/xboxrt/recomp/recomp_types.h). Both sides declare every data symbol (recomp_symbols.inc
// through regs.h, the header's externs) and every function the runtime provides (thunk.h, the
// header's prototypes) in this one file, so a type that differs is a compile error rather than a
// silent mismatch at link time; thread.h's back-edge hook too, once the header declares it. At run
// time: the register unions' little-endian lanes, the header's address transform against the
// runtime's, and its MEM macros against the runtime's accessors.
//
// The runtime's transform is defined first, token for token as mem.h has it: the header uses a
// WL_XB_EA that is already there, and mem.h's identical definition is then no redefinition.
#define WL_XB_EA(va) ((((uint32_t)(va)) & 0x0FFFFFFFu) | ((((uint32_t)(va)) >> 3) & 0x10000000u))
#include "recomp_types.h"

#include "greatest.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/regs.h"
#include "wl/xboxrt/thread.h" // g_xb_attention, wl_xb_safe_point: the back-edge hook
#include "wl/xboxrt/thunk.h"

// The header's register unions are the ones regs.c stores: the same lanes, little-endian.
_Static_assert(sizeof(RecompXmm) == 16, "RecompXmm is 16 bytes");
_Static_assert(sizeof(RecompMmx) == 8, "RecompMmx is 8 bytes");
_Static_assert(sizeof(((WlXbCpu *)0)->g_fp_stack) == 8 * sizeof(double), "eight x87 registers");

// Every function the runtime gives generated code, taken by address with the header's type.
static void *const k_runtime_fns[] = {
    (void *)recomp_lookup_kernel, (void *)recomp_lookup_manual,  (void *)recomp_icall_fail_log,
    (void *)recomp_icall_not_code_log, (void *)recomp_debug_service, (void *)xbox_ReadTimeStampCounter,
    (void *)recomp_setjmp_slot,   (void *)recomp_guest_longjmp,  (void *)recomp_unimpl,
    (void *)recomp_trace_enter,   (void *)recomp_trace_exit,     (void *)recomp_trace_esp,
    (void *)wl_xb_safe_point,
};

TEST unions_are_little_endian(void) {
    RecompXmm x;
    memset(&x, 0, sizeof x);
    x.u[0] = 0x03020100u;
    x.u[1] = 0x07060504u;
    uint8_t b[16];
    memcpy(b, &x, sizeof b);
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_FMT(i, (int)b[i], "%d"); // the guest's byte image
    }
    ASSERT_EQ_FMT(0x0706050403020100ull, (unsigned long long)x.q[0], "%llx");
    x.f[2] = 1.0f;
    ASSERT_EQ_FMT(0x3F800000u, x.u[2], "%08x");
    RecompMmx m;
    m.q = 0x8877665544332211ull;
    ASSERT_EQ_FMT(0x11u, (unsigned)m.ub[0], "%02x");
    ASSERT_EQ_FMT(0x4433u, (unsigned)m.uw[1], "%04x");
    ASSERT_EQ_FMT(0x88776655u, m.ud[1], "%08x");
    PASS();
}

TEST registers_save_and_load(void) {
    WlXbCpu saved, reset;
    g_eax = 0x12345678u;
    g_esp = 0x00F00000u;
    g_xmm3.u[3] = 0xCAFEF00Du;
    g_mm5.q = 0x0102030405060708ull;
    g_fp_stack[7] = 2.5;
    g_fp_control_word = 0x027Fu;
    wl_xb_cpu_save(&saved);
    wl_xb_cpu_reset(&reset);
    wl_xb_cpu_load(&reset);
    ASSERT_EQ_FMT(0u, g_eax, "%08x");
    ASSERT_EQ_FMT(0x037Fu, (unsigned)g_fp_control_word, "%04x"); // the x87 state at reset
    ASSERT_EQ_FMT(0x4000u, (unsigned)g_fp_cc, "%04x");
    wl_xb_cpu_load(&saved);
    ASSERT_EQ_FMT(0x12345678u, g_eax, "%08x");
    ASSERT_EQ_FMT(0x00F00000u, g_esp, "%08x");
    ASSERT_EQ_FMT(0xCAFEF00Du, g_xmm3.u[3], "%08x");
    ASSERT_EQ_FMT(0x0102030405060708ull, (unsigned long long)g_mm5.q, "%llx");
    ASSERT(g_fp_stack[7] == 2.5);
    PASS();
}

TEST memory_macros_agree(void) {
    static bool up;
    if (!up) {
        WlXbMemConfig mc;
        wl_xb_mem_config_default(&mc);
        ASSERT_EQ(WL_OK, wl_xb_mem_init(&mc));
        up = true;
    }
    static const uint32_t vas[] = {0x00010000u, 0x0BFFF000u, 0x80000000u, 0x83FFF000u, 0xF0001000u, 0xFD000000u};
    for (size_t i = 0; i < sizeof vas / sizeof vas[0]; i++) {
        ASSERT_EQ_FMT((unsigned long long)(uintptr_t)wl_xb_ptr(vas[i]), (unsigned long long)XBOX_PTR(vas[i]), "%llx");
    }
    uint32_t va = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_alloc(&va, WL_XB_PAGE, 0x00100000u, WL_XB_PROT_READWRITE));
    MEM32(va) = 0x11223344u;
    ASSERT_EQ_FMT(0x44u, (unsigned)wl_xb_rd8(va), "%02x"); // little-endian in guest memory
    ASSERT_EQ_FMT(0x11223344u, wl_xb_rd32(va), "%08x");
    wl_xb_wr16(va + 6u, 0xBEEFu);
    ASSERT_EQ_FMT(0xBEEFu, (unsigned)MEM16(va + 6u), "%04x");
    ASSERT_EQ_FMT(0xEFu, (unsigned)MEM8(va + 6u), "%02x");
    MEMF_SET(va + 8u, 1.5f);
    ASSERT(wl_xb_rdf(va + 8u) == 1.5f);
    wl_xb_wr16(va + 12u, 0x8001u);
    ASSERT_EQ_FMT(-32767, (int)SMEM16(va + 12u), "%d");
    uint32_t phys = 0;
    ASSERT(wl_xb_va_to_phys(va, &phys));
    ASSERT_EQ_FMT(0x11223344u, MEM32(WL_XB_WINDOW | phys), "%08x"); // the window: the same page
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST runtime_functions_exist(void) {
    for (size_t i = 0; i < sizeof k_runtime_fns / sizeof k_runtime_fns[0]; i++) {
        ASSERT(k_runtime_fns[i] != NULL);
    }
    ASSERT(recomp_lookup_kernel(0x00010000u) == NULL); // not a thunk slot
    PASS();
}

SUITE(abi_suite) {
    RUN_TEST(unions_are_little_endian);
    RUN_TEST(registers_save_and_load);
    RUN_TEST(memory_macros_agree);
    RUN_TEST(runtime_functions_exist);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(abi_suite);
    GREATEST_MAIN_END();
}
