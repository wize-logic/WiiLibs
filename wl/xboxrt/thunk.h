// SPDX-License-Identifier: MIT
// The functions generated code calls into the runtime, and the kernel thunks.
//
// An import slot of the XBE's kernel thunk table holds, after loading, either a data export's VA
// or the synthetic code address 0xFE000000 + slot * 4. The generated code calls through the slot
// like any indirect call (push the return address, RECOMP_ICALL); recomp_lookup_kernel answers
// the synthetic address with the dispatcher, which pops the return address, makes the call a yield
// point (interrupts, the quantum), runs the slot's kernel function and pops its stdcall arguments.
// A kernel function reads its arguments with WL_XB_ARG(n) and returns in g_eax (g_edx for the high
// half of a 64-bit result); fastcall ones read g_ecx/g_edx.
#pragma once

#include "wl/base/base.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RECOMP_DISPATCH_H
typedef void (*recomp_func_t)(void);
#endif

// ---- What generated code calls (the list is in README.md) ----------------------------------------

recomp_func_t recomp_lookup(uint32_t xbox_va);        // generated (recomp_dispatch.c)
recomp_func_t recomp_lookup_manual(uint32_t xbox_va); // weak default here: the title's hook, else NULL
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va); // here: the thunk dispatcher for 0xFE000000 + slot * 4
void recomp_icall_fail_log(uint32_t va);
// Where the indirect-call targets that had no translated function are recorded (a JSON seed file for
// tools/xbox_recomp.py's "seed_functions": recompiling with it translates them); NULL or "": none.
// What an earlier run recorded there is kept.
void wl_xb_set_seed_file(const char *path);
void recomp_icall_not_code_log(uint32_t va);
void recomp_debug_service(uint32_t service, uint32_t arg_va);
uint64_t xbox_ReadTimeStampCounter(void);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
// (recomp_setjmp_slot returns a jmp_buf *: declared in thunk.c's own prototype list, as
// <setjmp.h>'s jmp_buf comes from the generated code's header)

// The title's manual overrides: what the default recomp_lookup_manual asks (NULL: none). A title
// may instead define recomp_lookup_manual itself (the default is weak); either must give the same
// answer for an address for as long as it is used, or call wl_xb_lookup_flush when that changes.
typedef recomp_func_t (*WlXbManualLookup)(uint32_t xbox_va);
void wl_xb_set_manual_lookup(WlXbManualLookup fn);

// The way an indirect call resolves `va`: manual, generated, kernel. NULL when nothing is there.
// Generated code calls it for every indirect call and jump (RECOMP_RESOLVE in recomp_types.h); the
// manual and generated answers are cached (direct-mapped, 4096 entries), the kernel's never are.
// Guest code only: called with the guest lock held.
recomp_func_t wl_xb_lookup(uint32_t va);
// Forgets the cached answers: for a title whose own recomp_lookup_manual changes what it answers
// (wl_xb_set_manual_lookup does it itself).
void wl_xb_lookup_flush(void);

// ---- Kernel thunk slots --------------------------------------------------------------------------

#define WL_XB_THUNK_BASE 0xFE000000u
#define WL_XB_MAX_THUNKS 512u

typedef void (*WlXbKernelFn)(void);

// The kernel function and stdcall argument bytes of slot `slot` (its synthetic VA is
// WL_XB_THUNK_BASE + slot * 4). `name` is kept (a static string) for logs.
void wl_xb_thunk_bind(uint32_t slot, uint32_t ordinal, WlXbKernelFn fn, uint32_t arg_bytes, const char *name);
uint32_t wl_xb_thunk_va(uint32_t slot);
// The ordinal and name bound to a synthetic VA (0 / NULL when none).
uint32_t wl_xb_thunk_ordinal(uint32_t va, const char **name);

// The current kernel call's arguments: after the dispatcher popped the return address, argument n
// is at esp + 4n. WL_XB_RET(v) sets eax.
#define WL_XB_ARG(n) wl_xb_rd32(g_esp + 4u * (uint32_t)(n))
#define WL_XB_RET(v) (g_eax = (uint32_t)(v))
// The guest return address of the current kernel call (for logs).
uint32_t wl_xb_kernel_caller(void);

// Per-call logging: every call (`all`), or each ordinal's first call only. Off by default.
void wl_xb_thunk_trace(bool all, bool first_calls);
typedef struct {
    uint64_t calls;
    uint32_t ordinals_used;
} WlXbThunkStats;
void wl_xb_thunk_stats(WlXbThunkStats *out);
// Calls of an ordinal so far.
uint64_t wl_xb_thunk_calls(uint32_t ordinal);

// The x86 port I/O the lifter may route here (in/out): the video field flag at 0x80C0 and
// nothing else answers. Values are as the x86 sees them.
uint32_t wl_xb_port_in(uint32_t port, unsigned size);
void wl_xb_port_out(uint32_t port, uint32_t value, unsigned size);

// The guest code's bounds (from the XBE's executable sections): RECOMP_ICALL_IS_CODE reads them.
void wl_xb_set_code_bounds(uint32_t lo, uint32_t hi);

#ifdef __cplusplus
}
#endif
