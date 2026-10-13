// SPDX-License-Identifier: MIT
// A stand-in for WiiLibs' Xbox runtime (wl/xboxrt), for the build test of wl_add_xbox_title only:
// it defines every symbol the generated code needs (wl/xboxrt/recomp/README.md) with no behaviour
// beyond plain storage, so a synthetic title links and its leaf functions can be called. Not a
// runtime: no guest memory map, no kernel, no threads.
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
int g_force_return;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
volatile uint32_t g_xb_attention; void wl_xb_safe_point(void); void wl_xb_safe_point(void) {} /* RECOMP_BACKEDGE (no other threads here) */
uint32_t wl_xb_port_in(uint32_t port, unsigned size); uint32_t wl_xb_port_in(uint32_t port, unsigned size) { (void)port; (void)size; return 0; } void wl_xb_port_out(uint32_t port, uint32_t value, unsigned size); void wl_xb_port_out(uint32_t port, uint32_t value, unsigned size) { (void)port; (void)value; (void)size; } /* RECOMP_PORT_IN/OUT */
RECOMP_TLS uint32_t g_ebp, g_seh_ebp, g_fs_base;
RECOMP_TLS int g_df;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F;
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;

recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}
recomp_func_t recomp_lookup_kernel(uint32_t va) {
    (void)va;
    return NULL;
}
void recomp_icall_fail_log(uint32_t va) { fprintf(stderr, "stub runtime: no function at %08x\n", (unsigned)va); }
void recomp_icall_not_code_log(uint32_t va) { fprintf(stderr, "stub runtime: %08x is not code\n", (unsigned)va); }
void recomp_debug_service(uint32_t service, uint32_t arg_va) {
    (void)service;
    (void)arg_va;
}
uint64_t xbox_ReadTimeStampCounter(void) { return 0; }
void recomp_unimpl(const char *text, uint32_t va) { fprintf(stderr, "stub runtime: unimplemented %s at %08x\n", text, (unsigned)va); }
void recomp_trace_enter(const char *name, uint32_t va) {
    (void)name;
    (void)va;
}
void recomp_trace_exit(const char *name, uint32_t va) {
    (void)name;
    (void)va;
}
void recomp_trace_esp(const char *name, const char *tag) {
    (void)name;
    (void)tag;
}
jmp_buf *recomp_setjmp_slot(uint32_t buf_va) {
    (void)buf_va;
    return NULL;
}
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value) {
    (void)buf_va;
    (void)value;
    return 0;
}
