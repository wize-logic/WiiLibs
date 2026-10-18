#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026 sp00nz
# and contributors: the output is xboxrecomp's templates/runtime/recomp_types.h with the edits below.
"""Makes WiiLibs' big-endian recomp_types.h from xboxrecomp's runtime header template.

Generated code from xboxrecomp's lifter includes "recomp_types.h": the register model, the guest
memory macros and the helpers the lifter names. Upstream's template assumes a little-endian host.
WiiLibs' copy (wl/xboxrt/recomp/recomp_types.h) is that template, with WiiLibs' patches applied
(third_party/xboxrecomp/patches/), and these edits, each anchored on the template's own text:

  1. a GCC-only guard: #error unless the compiler has scalar_storage_order (clang ignores the
     attribute, and would silently read guest memory in host order)
  2. RECOMP_TLS is empty: registers are plain globals; the runtime serialises guest threads
  3. atomics on little-endian guest words: byte-swap around a compare-and-swap loop
  4. XBOX_PTR is the guest-to-host transform: WL_XB_EA(a) = (a & 0x0FFFFFFF) | ((a >> 3) &
     0x10000000), plus g_xbox_mem_offset, or nothing with WL_XB_EA_BASE_ZERO (the Wii, where the
     effective address is the address); MEM8..MEMD read and write little-endian through
     scalar_storage_order("little-endian") structs, may_alias, volatile but for the float and
     double loads
  5. MEMF_SET/MEMD_SET store a float's integer bits (one byte-reversed store, not a float
     store, reload and swap), always inline
  6. RecompXmm and RecompMmx get little-endian storage, so their lane views agree as on x86; the
     packed float arithmetic (RECOMP_XMM_LANEWISE) works on host-order copies of the lanes, inline,
     and XMM_SHUFFLE moves lanes as bytes; XMM_SHUFFLE, the SIMD loads and stores and XMM_SCALAR are
     always inline
  7. RECOMP_FP_PC and RECOMP_FP_PC_CW (the control word as an argument): a fast path (round to
     float when the result is a normal float) before the exact slow path, which stays out of line
  8. the indirect-call trace ring and counter are compiled only with WL_RECOMP_ICALL_TRACE
  9. MMX_CVT_F2I rounds with recomp_cvt_si32 (nearest-even, not the host's rounding mode)
 10. RecompMmx and the g_mm0..7 declarations move out of the RECOMP_GENERATED_CODE block, so the
     runtime can define the MMX registers without the generated code's register-name macros
 12. RECOMP_PORT_IN / RECOMP_PORT_OUT (patch 0009 lifts in/out to them) are wl/xboxrt's
     wl_xb_port_in / wl_xb_port_out
 11. RECOMP_BACKEDGE (patch 0008 calls it before every backward jump) is wl/xboxrt's safe point:
     a load of g_xb_attention and a branch, and wl_xb_safe_point() when another guest thread or
     interrupt work is waiting (a loop that only polls memory would otherwise hold the guest lock
     forever)
 13. the indirect-call macros resolve their target with one call, RECOMP_RESOLVE (wl/xboxrt's
     wl_xb_lookup: manual, generated, kernel behind a direct-mapped cache), not three
 14. RECOMP_FCMP is an inline function: one evaluation of each operand, ordered tests first
 15. PUSH32 onto the guest's own esp (g_esp, how every direct call pushes its return address) is a
     call of one small function per unit in code built for size (-Os: the units the profile does
     not name hot), not eight inline instructions
 16. MEMxx_SP(esp, disp) and MEMF/MEMD_SET_SP (--stack-disp), PUSH32 and POP32 address the guest
     stack as esp + disp without WL_XB_EA: every guest stack is below WL_XB_USER_LIMIT, where the
     transform is the identity; the PC=24 fast path tests the exponent with one add and one mask
 17. on PowerPC the PC=24 slow path is a call of a stub, recomp_fp24_<d>_<s>, that keeps every
     register but d around recomp_fp_round24: an x87 operation's neighbours keep their values in
     volatile registers
 18. on PowerPC the PC=24 fast path is frsp. and a branch on FPSCR[FX] (CR1): an overflow or an
     inexact underflow sets it, and the slow path clears it; no store and reload of the bits
 19. on PowerPC, in code built for size, a non-constant guest address goes through a branch to a
     stub for the register pair (wl_xb_ea_<d>_<s>, a COMDAT group) instead of two inline
     instructions (RECOMP_NO_EA_STUB keeps them inline)
 20. in code built for size, a backward jump's safe point (RECOMP_BACKEDGE) is one call of a small
     function per unit, and an indirect call or jump (RECOMP_ICALL*, RECOMP_ITAIL: the code-range
     check, the lookup, the call and the failure path) is one call too (RECOMP_NO_BACKEDGE_CALL,
     RECOMP_NO_ICALL_CALL keep them inline)
 21. on PowerPC, in code built for size, a constant pushed onto g_esp (a call's return address)
     is a data word after a branch to recomp_push32_word, not lis, ori and a call
     (RECOMP_NO_PUSH32_WORD turns it off)
 22. the stack's integer accesses (MEMxx_SP, PUSH32, POP32) are not volatile: GCC forwards a
     stored slot to its reload and loads a slot once (RECOMP_STACK_VOLATILE makes them volatile)
 23. the SSE loads and stores map their address once, and the --mem-disp forms of the float stores
     and the SSE and MMX accesses (MEMF_SET_D, XMM_MEM_D, MMX_STORE_D, ...; patch 0025) map base
     once and add disp on the host side, as MEMxx_D
 24. the MMX loads and stores move the guest's 8 bytes as one uint64_t (a memcpy, then on a
     big-endian host a byte swap into the union's little-endian q): GCC keeps the register in
     registers, where a memcpy of the union itself went through a stack slot at every access

On a little-endian host the storage-order attribute does nothing and the byte swaps are
identities, so the same header serves host tests.

    gen_recomp_types.py            regenerate wl/xboxrt/recomp/recomp_types.h
    gen_recomp_types.py --check    exit 1 if it is not what the current template gives (a ctest)
    gen_recomp_types.py --template PATH --out PATH

Without --template the template is taken from a patched copy of the vendored pipeline made in a
temporary directory (tools/xbox_recomp.py's prepare). A failed anchor means upstream changed the
part of the template that edit replaces: read the change and update the edit.
"""

from __future__ import annotations

import argparse
import difflib
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
WL_ROOT = HERE.parent.parent.parent
OUT = HERE / "recomp_types.h"


class AnchorError(Exception):
    pass


def replace_once(src: str, old: str, new: str, what: str) -> str:
    n = src.count(old)
    if n != 1:
        raise AnchorError(f"edit '{what}': expected its anchor once in the template, found it {n} times:\n"
                          f"{old[:200]}")
    return src.replace(old, new)


def replace_between(src: str, start: str, end: str, new: str, what: str) -> str:
    """Replace from the start anchor up to (not including) the end anchor."""
    if src.count(start) != 1:
        raise AnchorError(f"edit '{what}': start anchor found {src.count(start)} times: {start[:120]}")
    i = src.index(start)
    j = src.find(end, i)
    if j < 0:
        raise AnchorError(f"edit '{what}': end anchor not found after the start: {end[:120]}")
    return src[:i] + new + src[j:]


HEADER = """/* SPDX-License-Identifier: MIT
 * Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026 sp00nz
 * and contributors: templates/runtime/recomp_types.h at {commit} ({release}), with WiiLibs'
 * patches ({patches}).
 *
 * GENERATED by wl/xboxrt/recomp/gen_recomp_types.py: do not edit by hand. Change the generator (or a
 * patch in third_party/xboxrecomp/patches/) and regenerate; ctest xbox_recomp_header fails while
 * this file and the generator disagree.
 *
 * WiiLibs' runtime header for xboxrecomp's generated code, for a big-endian host (the Wii's
 * Broadway, 32-bit PowerPC Linux under qemu-ppc) and equally for a little-endian one. Guest memory
 * holds the x86's little-endian bytes; every MEM* access byte-reverses on a big-endian host
 * (lwbrx/stwbrx on PowerPC). README.md beside this file lists the symbols the runtime defines.
 */
"""

GUARD = """#define RECOMP_TYPES_H

/* WiiLibs: guest memory is little-endian, and this header reads it through GCC's
 * scalar_storage_order attribute. A compiler without it (clang accepts the spelling with only a
 * warning) would read guest memory in host order: wrong on a big-endian host, silently. */
#if defined(__has_attribute)
#  if !__has_attribute(scalar_storage_order)
#    error "recomp_types.h needs GCC's scalar_storage_order attribute (GCC 6 or later; not clang)"
#  endif
#elif !defined(__GNUC__) || __GNUC__ < 6
#  error "recomp_types.h needs GCC's scalar_storage_order attribute (GCC 6 or later)"
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define RECOMP_HOST_BIG_ENDIAN 1
#else
#  define RECOMP_HOST_BIG_ENDIAN 0
#endif
/* A guest (little-endian) 32-bit word as the host reads it, and back: the same operation. */
#if RECOMP_HOST_BIG_ENDIAN
#  define RECOMP_LE32(v) __builtin_bswap32((uint32_t)(v))
#else
#  define RECOMP_LE32(v) ((uint32_t)(v))
#endif
"""

TLS_OLD = """#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif"""

TLS_NEW = """/* WiiLibs: plain globals. libogc has no thread-local storage, and the runtime runs one guest
 * thread at a time under its guest lock, saving and restoring this register file when it hands
 * the lock over, so every guest thread still has registers of its own. */
#ifndef RECOMP_TLS
#define RECOMP_TLS
#endif"""

ATOMIC_START = "#if defined(_MSC_VER)\n#include <intrin.h>\n#define RECOMP_ATOMIC_ADD32"
ATOMIC_END = "#include <setjmp.h>"
ATOMIC_NEW = """/* WiiLibs: p is a host pointer to a little-endian guest word, so the arithmetic happens on the
 * byte-reversed value inside a compare-and-swap loop (lwarx/stwcx. on PowerPC; Broadway is single
 * core, so this is atomic against every other thread and interrupt). The word must be 4-aligned
 * on PowerPC, as lwarx requires; x86 allows a locked access anywhere, but compilers align them. */
static inline uint32_t recomp_atomic_add32(uintptr_t p, uint32_t v) {
    volatile uint32_t *w = (volatile uint32_t *)p;
    uint32_t old, sum;
    do {
        old = *w;
        sum = RECOMP_LE32(RECOMP_LE32(old) + v);
    } while (!__sync_bool_compare_and_swap(w, old, sum));
    return RECOMP_LE32(old);
}
static inline uint32_t recomp_atomic_cas32(uintptr_t p, uint32_t cmp, uint32_t val) {
    return RECOMP_LE32(__sync_val_compare_and_swap((volatile uint32_t *)p, RECOMP_LE32(cmp),
                                                   RECOMP_LE32(val)));
}
#define RECOMP_ATOMIC_ADD32(p, v)        recomp_atomic_add32((uintptr_t)(p), (uint32_t)(v))
#define RECOMP_ATOMIC_CAS32(p, cmp, val) recomp_atomic_cas32((uintptr_t)(p), (uint32_t)(cmp), (uint32_t)(val))

"""

MEM_START = "#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)"
MEM_END = "/** Float/double stores. The lifter emits MEMF_SET"
# patch 0003's default MEMF_SET/MEMD_SET (plain assignments) give way to the ones above
SET_DEFAULTS_START = "/** Float/double stores. The lifter emits MEMF_SET"
SET_DEFAULTS_END = "/** x87 80-bit extended memory operands"
MEM_NEW = """/* WiiLibs: guest address -> host address.
 *
 * WL_XB_EA is the guest-to-effective-address transform the runtime maps guest memory with:
 * guest 0x0xxxxxxx (user space: the XBE image, stacks, heap) -> 0x0xxxxxxx, and the contiguous
 * window 0x8xxxxxxx and its tiled alias 0xFxxxxxxx (MMIO above 0xFC000000) -> 0x1xxxxxxx. Two
 * instructions on PowerPC (rlwinm + rlwimi). On the Wii the effective address is the address
 * (WL_XB_EA_BASE_ZERO, an MMU-mapped 512 MB at 0); on a host or ppc-be it is an offset into the
 * runtime's reservation, g_xbox_mem_offset. Mask to 32 bits first: guest arithmetic wraps.
 * A runtime that defines WL_XB_EA first (the same transform) is used instead. */
#ifndef WL_XB_EA
#if (defined(__PPC__) || defined(__powerpc__)) && !defined(WL_XB_EA_NO_ASM)
/* PowerPC: GCC spells the C form rlwinm, rlwinm, or; rlwinm then rlwimi is one fewer, on every
 * guest access. A constant address still folds to a constant. Always inline: at -Os GCC keeps an
 * out-of-line copy in each translation unit and calls it for every access (with the call's own
 * return and the moves around it, 9-10% of Halo's time on the Wii). Inline, an access takes one
 * instruction more room than the call (Halo's lifted code: about 8% more, 1 MB); WL_XB_EA_CALL
 * gives GCC the choice again, for a title short of memory. */
#ifdef WL_XB_EA_CALL
#define WL_XB_EA_INLINE static inline
#else
#define WL_XB_EA_INLINE static inline __attribute__((always_inline))
#endif
#if defined(__OPTIMIZE_SIZE__) && !defined(WL_XB_EA_CALL) && !defined(RECOMP_NO_EA_STUB) && !defined(__powerpc64__)
/* In code built for size (the units the profile does not name hot) the two instructions are a
 * branch to a stub for the register pair, wl_xb_ea_<d>_<s> (the same two instructions and a blr,
 * one copy in the program: a COMDAT group), that touches nothing else, so GCC keeps every value in
 * its register across it: one instruction instead of two at each guest access whose address is not
 * a constant, about 320 KB of Halo's MEM1, for a bl and a blr each. RECOMP_NO_EA_STUB keeps them
 * inline. */
__asm__(
    ".macro wl_xb_ea_stub d, s\\n"
    ".pushsection .text.wl_xb_ea_\\\\d\\\\()_\\\\s,\\"axG\\",@progbits,wl_xb_ea_\\\\d\\\\()_\\\\s,comdat\\n"
    ".weak wl_xb_ea_\\\\d\\\\()_\\\\s\\n"
    ".type wl_xb_ea_\\\\d\\\\()_\\\\s,@function\\n"
    "wl_xb_ea_\\\\d\\\\()_\\\\s:\\n"
    ".ifc \\\\d,\\\\s\\n"
    "  rlwimi \\\\d,\\\\s,29,3,3\\n  rlwinm \\\\d,\\\\d,0,3,31\\n"
    ".else\\n"
    "  rlwinm \\\\d,\\\\s,0,4,31\\n  rlwimi \\\\d,\\\\s,29,3,3\\n"
    ".endif\\n"
    "  blr\\n"
    ".size wl_xb_ea_\\\\d\\\\()_\\\\s,.-wl_xb_ea_\\\\d\\\\()_\\\\s\\n"
    ".popsection\\n"
    ".endm\\n");
static inline __attribute__((always_inline)) uint32_t wl_xb_ea_ppc(uint32_t guest) {
    uint32_t ea;
    if (__builtin_constant_p(guest))
        return (guest & 0x0FFFFFFFu) | ((guest >> 3) & 0x10000000u);
    __asm__("bl wl_xb_ea_%0_%1\\n.ifndef wl_xb_ea_%0_%1\\n\\twl_xb_ea_stub %0, %1\\n.endif"
            : "=r"(ea) : "r"(guest) : "lr");
    return ea;
}
#else
WL_XB_EA_INLINE uint32_t wl_xb_ea_ppc(uint32_t guest) {
    uint32_t ea;
    if (__builtin_constant_p(guest))
        return (guest & 0x0FFFFFFFu) | ((guest >> 3) & 0x10000000u);
    ea = guest & 0x0FFFFFFFu;
    __asm__("rlwimi %0,%1,29,3,3" : "+r"(ea) : "r"(guest));
    return ea;
}
#endif
#define WL_XB_EA(guest) wl_xb_ea_ppc((uint32_t)(guest))
#else
#define WL_XB_EA(guest) ((((uint32_t)(guest)) & 0x0FFFFFFFu) | ((((uint32_t)(guest)) >> 3) & 0x10000000u))
#endif
#endif
#ifdef WL_XB_EA_BASE_ZERO
#define XBOX_PTR(addr) ((uintptr_t)WL_XB_EA(addr))
#else
#define XBOX_PTR(addr) ((uintptr_t)WL_XB_EA(addr) + (uintptr_t)g_xbox_mem_offset)
#endif

/* Guest memory is little-endian. Each access goes through a one-member struct with
 * little-endian storage order, so GCC byte-reverses in the access itself (lwbrx, lhbrx, stwbrx,
 * sthbrx; two of them for 64 bits) and the macros stay lvalues. volatile keeps every access
 * (spin loops on flags other threads set, MMIO); may_alias stops GCC assuming that a 16-bit and a
 * 32-bit store to the same guest address are unrelated (it folds loads across them otherwise).
 * &MEM32(a) is not allowed (GCC refuses the address of a reverse-order scalar); the lifter never
 * takes it. Unaligned guest addresses compile to the same byte-reversing access. */
#define RECOMP_GUEST_LE __attribute__((scalar_storage_order("little-endian"), may_alias))
typedef struct RECOMP_GUEST_LE { volatile uint8_t  v; } recomp_le_u8;
typedef struct RECOMP_GUEST_LE { volatile uint16_t v; } recomp_le_u16;
typedef struct RECOMP_GUEST_LE { volatile uint32_t v; } recomp_le_u32;
typedef struct RECOMP_GUEST_LE { volatile int8_t   v; } recomp_le_s8;
typedef struct RECOMP_GUEST_LE { volatile int16_t  v; } recomp_le_s16;
typedef struct RECOMP_GUEST_LE { volatile int32_t  v; } recomp_le_s32;
typedef struct RECOMP_GUEST_LE { volatile int64_t  v; } recomp_le_s64;
/* The float and double loads alone are not volatile (x87 and SSE scalar operands: MEMF, MEMD;
 * their stores are MEMF_SET/MEMD_SET, integer and volatile). No guest spins on a float or reads
 * one from a device, and a byte-reversed float load is three instructions through the stack
 * (lwbrx, stw, lfs): volatile made each `fcomp [mem]` load its operand up to four times and every
 * `fmul [esp+x]` reload a value the block had just read. GCC still orders them after every
 * store that may alias and every call; only an access that hands the guest lock over (a device
 * read, or a page-in: paging gives the lock up while it reads the page, so another guest thread
 * may run in it) no longer forces a float read before it to be read again after it. */
typedef struct RECOMP_GUEST_LE { float  v; } recomp_le_f32;
typedef struct RECOMP_GUEST_LE { double v; } recomp_le_f64;

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (((recomp_le_u8  *)XBOX_PTR(addr))->v)
#define MEM16(addr)  (((recomp_le_u16 *)XBOX_PTR(addr))->v)
#define MEM32(addr)  (((recomp_le_u32 *)XBOX_PTR(addr))->v)

/** Signed memory reads. */
#define SMEM8(addr)  (((recomp_le_s8  *)XBOX_PTR(addr))->v)
#define SMEM16(addr) (((recomp_le_s16 *)XBOX_PTR(addr))->v)
#define SMEM32(addr) (((recomp_le_s32 *)XBOX_PTR(addr))->v)
#define SMEM64(addr) (((recomp_le_s64 *)XBOX_PTR(addr))->v)

/** Float/double memory loads. */
#define MEMF(addr)   (((recomp_le_f32 *)XBOX_PTR(addr))->v)
#define MEMD(addr)   (((recomp_le_f64 *)XBOX_PTR(addr))->v)

/** The same accesses at base + disp, disp a constant in [-0x8000, 0x8000) (the lifter's --mem-disp
 * writes them for MEMxx(base + disp), patch 0021): base is mapped and disp added on the host side, so
 * accesses through one base share its EA (two instructions each before, and an addi) and the
 * displacement lands in the access. For every address the guest may touch it is the same access: a
 * small displacement from a valid base stays in its 256 MB region, where the EA map is a plain
 * addition; only a wild access that leaves the region (one the Xbox faults on too) lands elsewhere. */
#define RECOMP_MEM_D(T, b, d) (((T *)(XBOX_PTR(b) + (uintptr_t)(intptr_t)(int32_t)(d)))->v)
#define MEM8_D(b, d)   RECOMP_MEM_D(recomp_le_u8, b, d)
#define MEM16_D(b, d)  RECOMP_MEM_D(recomp_le_u16, b, d)
#define MEM32_D(b, d)  RECOMP_MEM_D(recomp_le_u32, b, d)
#define SMEM8_D(b, d)  RECOMP_MEM_D(recomp_le_s8, b, d)
#define SMEM16_D(b, d) RECOMP_MEM_D(recomp_le_s16, b, d)
#define SMEM32_D(b, d) RECOMP_MEM_D(recomp_le_s32, b, d)
#define SMEM64_D(b, d) RECOMP_MEM_D(recomp_le_s64, b, d)
#define MEMF_D(b, d)   RECOMP_MEM_D(recomp_le_f32, b, d)
#define MEMD_D(b, d)   RECOMP_MEM_D(recomp_le_f64, b, d)

/* The same accesses at esp + disp (--stack-disp writes them for an access whose base is esp, disp 0
 * included), without WL_XB_EA: wl/xboxrt keeps every guest stack below WL_XB_USER_LIMIT
 * (0x10000000, checked when a thread is created), where the transform is the identity, so the
 * address is esp + disp itself (plus g_xbox_mem_offset on a host). GCC folds disp into the load and
 * keeps one register for esp, not one per esp the pushes and calls make (7% of the instructions
 * of Halo's hot units). */
#ifdef WL_XB_EA_BASE_ZERO
#define RECOMP_STACK_PTR(a) ((uintptr_t)(uint32_t)(a))
#else
#define RECOMP_STACK_PTR(a) ((uintptr_t)(uint32_t)(a) + (uintptr_t)g_xbox_mem_offset)
#endif
#define RECOMP_MEM_SP(T, b, d) (((T *)RECOMP_STACK_PTR((uint32_t)(b) + (uint32_t)(int32_t)(d)))->v)
/* The stack's integer accesses (MEMxx_SP, and so PUSH32 and POP32) are not volatile either
 * (RECOMP_STACK_VOLATILE makes them so again): GCC forwards a value the code stored to a stack
 * slot to the load that reads it back, and loads a slot once where nothing between can change it,
 * as the x86's own registers would have held it (1.1% of the instructions of Halo's hot unit). A
 * guest stack is its own thread's: no device writes it, interrupt work runs on a stack of its own,
 * and the guest lock passes to another guest thread only inside a call (the safe point's
 * included), which makes GCC read and write the stack again, or at a device access or a page-in in
 * the middle of a block, where no other thread's code reads or writes the running frame (on the
 * Xbox that would be a race with a thread that may run at any instruction). The order of the
 * stack's stores among the other guest accesses that may alias them is kept. */
#ifdef RECOMP_STACK_VOLATILE
#define RECOMP_SP_VOLATILE volatile
#else
#define RECOMP_SP_VOLATILE
#endif
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE uint8_t  v; } recomp_sp_u8;
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE uint16_t v; } recomp_sp_u16;
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE uint32_t v; } recomp_sp_u32;
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE int8_t   v; } recomp_sp_s8;
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE int16_t  v; } recomp_sp_s16;
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE int32_t  v; } recomp_sp_s32;
typedef struct RECOMP_GUEST_LE { RECOMP_SP_VOLATILE int64_t  v; } recomp_sp_s64;
#define MEM8_SP(b, d)   RECOMP_MEM_SP(recomp_sp_u8, b, d)
#define MEM16_SP(b, d)  RECOMP_MEM_SP(recomp_sp_u16, b, d)
#define MEM32_SP(b, d)  RECOMP_MEM_SP(recomp_sp_u32, b, d)
#define SMEM8_SP(b, d)  RECOMP_MEM_SP(recomp_sp_s8, b, d)
#define SMEM16_SP(b, d) RECOMP_MEM_SP(recomp_sp_s16, b, d)
#define SMEM32_SP(b, d) RECOMP_MEM_SP(recomp_sp_s32, b, d)
#define SMEM64_SP(b, d) RECOMP_MEM_SP(recomp_sp_s64, b, d)
#define MEMF_SP(b, d)   RECOMP_MEM_SP(recomp_le_f32, b, d)
#define MEMD_SP(b, d)   RECOMP_MEM_SP(recomp_le_f64, b, d)

/** Float/double stores, through the value's integer bits: a byte-reversed float store would be
 * a float store, an integer reload and a three-instruction swap; this is the float store, the
 * reload and one stwbrx (two for a double). Always inline, like WL_XB_EA: at -Os GCC calls an
 * out-of-line copy in each translation unit, with a stack frame of its own for the reload (1.9% of
 * Halo's time on the Wii, at 12,700 stores); inline it is two or three instructions more at each.
 * WL_XB_EA_CALL leaves GCC the choice, here too. */
#if defined(WL_XB_EA_CALL)
#define RECOMP_MEM_SET_INLINE static inline
#else
#define RECOMP_MEM_SET_INLINE static inline __attribute__((always_inline))
#endif
RECOMP_MEM_SET_INLINE void recomp_memf_set(uint32_t addr, float v) {
    union { float f; uint32_t u; } c;
    c.f = v;
    MEM32(addr) = c.u;
}
RECOMP_MEM_SET_INLINE void recomp_memd_set(uint32_t addr, double v) {
    union { double d; uint64_t u; } c;
    c.d = v;
    MEM32(addr) = (uint32_t)c.u;
    MEM32_D(addr, 4) = (uint32_t)(c.u >> 32);
}
#define MEMF_SET(addr, v) recomp_memf_set((uint32_t)(addr), (float)(v))
#define MEMD_SET(addr, v) recomp_memd_set((uint32_t)(addr), (double)(v))
RECOMP_MEM_SET_INLINE void recomp_memf_set_sp(uint32_t addr, float v) {
    union { float f; uint32_t u; } c;
    c.f = v;
    MEM32_SP(addr, 0) = c.u;
}
RECOMP_MEM_SET_INLINE void recomp_memd_set_sp(uint32_t addr, double v) {
    union { double d; uint64_t u; } c;
    c.d = v;
    MEM32_SP(addr, 0) = (uint32_t)c.u;
    MEM32_SP(addr, 4) = (uint32_t)(c.u >> 32);
}
#define MEMF_SET_SP(b, d, v) recomp_memf_set_sp((uint32_t)(b) + (uint32_t)(int32_t)(d), (float)(v))
#define MEMD_SET_SP(b, d, v) recomp_memd_set_sp((uint32_t)(b) + (uint32_t)(int32_t)(d), (double)(v))
/* The stores at base + disp (--mem-disp), and the SSE and MMX accesses below: base mapped, disp
 * added on the host side, as MEMxx_D. */
#define RECOMP_HOST_D(b, d) (XBOX_PTR(b) + (uintptr_t)(intptr_t)(int32_t)(d))
#define RECOMP_LE32_AT(p) (((recomp_le_u32 *)(p))->v)
RECOMP_MEM_SET_INLINE void recomp_memf_set_h(uintptr_t p, float v) {
    union { float f; uint32_t u; } c;
    c.f = v;
    RECOMP_LE32_AT(p) = c.u;
}
RECOMP_MEM_SET_INLINE void recomp_memd_set_h(uintptr_t p, double v) {
    union { double d; uint64_t u; } c;
    c.d = v;
    RECOMP_LE32_AT(p) = (uint32_t)c.u;
    RECOMP_LE32_AT(p + 4) = (uint32_t)(c.u >> 32);
}
#define MEMF_SET_D(b, d, v) recomp_memf_set_h(RECOMP_HOST_D(b, d), (float)(v))
#define MEMD_SET_D(b, d, v) recomp_memd_set_h(RECOMP_HOST_D(b, d), (double)(v))
"""

FPPC_OLD = """/* RECOMP_FP_PC_CW takes the control word as an argument: a function whose x87
 * state the lifter keeps in locals (--x87-locals) passes its local copy. */
#define RECOMP_FP_PC_CW(cw, x) (((cw) & 0x300u) ? (double)(x) : recomp_fp_round24(x))
#define RECOMP_FP_PC(x) RECOMP_FP_PC_CW(g_fp_control_word, x)"""
FPPC_NEW = """/* WiiLibs: the common case inline -- the result rounded to float is a normal float, so the
 * plain conversion is exact PC=24 rounding -- and only zero, denormal, overflowing, infinite and
 * NaN results take recomp_fp_round24, out of line. Upstream inlines the whole range test at
 * every x87 operation, which more than doubled the size of float-heavy functions on PowerPC. */
#if RECOMP_FP_ROUND24_KEEP
/* On PowerPC: frsp. rounds and copies FPSCR[FX] to CR1. FX is set when an instruction turns an
 * exception bit from 0 to 1, and the stub clears it (with OX and UX) after each slow path; XX
 * (inexact) stays set, so a normal float result leaves FX clear, and only an overflow or an
 * inexact underflow sets it -- the results a plain conversion gets wrong. An exact denormal or
 * zero is the x87's value as well. FX from other code (its own overflow, a thread's first
 * inexact result) sends one operation to the slow path, which is exact for every input. Two
 * instructions beside the frsp: a branch over, and a call of the stub for the register pair. */
static inline double recomp_fp_pc24(double x) {
    double r;
    __asm__("frsp. %0,%1\\n\\tbf+ 4,1f\\n\\tbl recomp_fp24_%0_%1\\n1:\\n"
            ".ifndef recomp_fp24_%0_%1\\n\\trecomp_fp24_stub %0, %1\\n.endif"
            : "=&f"(r) : "f"(x) : "lr", "cr1");
    return r;
}
#else
static inline double recomp_fp_pc24(double x) {
    float f = (float)x;
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    /* exponent 1..254: +1 then its top seven bits not all zero (addis, andis., one branch) */
    if (__builtin_expect(((bits + 0x00800000u) & 0x7F000000u) != 0, 1))
        return (double)f;
    return recomp_fp_round24(x);
}
#endif
/* RECOMP_FP_PC_CW takes the control word as an argument: a function whose x87
 * state the lifter keeps in locals (--x87-locals) passes its local copy. */
#define RECOMP_FP_PC_CW(cw, x) (((cw) & 0x300u) ? (double)(x) : recomp_fp_pc24(x))
#define RECOMP_FP_PC(x) RECOMP_FP_PC_CW(g_fp_control_word, x)"""

FCMP_OLD = "#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)"
FCMP_NEW = """/* WiiLibs: each operand evaluated once (it is often a guest memory load), and the ordered tests
 * first, so the compiler answers all four outcomes from one fcmpu; the two NaN tests first made it
 * three or four compares, and a test of the status word after it could not fold to one branch. */
static inline __attribute__((always_inline)) int recomp_fcmp(double a, double b) {
    return a < b ? -1 : a > b ? 1 : a == b ? 0 : 2;
}
#define RECOMP_FCMP(a, b)     recomp_fcmp((a), (b))"""

ROUND24_OLD = "static inline double recomp_fp_round24(double x) {"
ROUND24_NEW = r"""/* WiiLibs: on PowerPC an x87 operation's slow path is a call of recomp_fp24_<d>_<s>, one small
 * stub for each pair of registers the compiler chose (d the result, s the value): it moves s to
 * f1, calls recomp_fp24_keep and moves f1 to d, keeping f1. recomp_fp24_keep saves every other
 * volatile register (r0, r3-r12, f0, f2-f13, CR, XER, CTR, LR) around recomp_fp_round24 and
 * clears FPSCR[OX, UX, FX] after it. A plain call clobbers them all, so each x87 operation made
 * GCC move a function's live values to non-volatile registers or spill them around a call that
 * almost never runs. One copy of each stub per program (COMDAT groups); recomp_fp24_keep calls
 * the recomp_fp_round24 of the unit its copy came from. */
#if (defined(__PPC__) || defined(__powerpc__)) && !defined(__powerpc64__) && !defined(WL_XB_EA_NO_ASM)
#define RECOMP_FP_ROUND24_KEEP 1
static __attribute__((used)) double recomp_fp_round24(double x);
__asm__(
    ".pushsection .text.recomp_fp24_keep,\"axG\",@progbits,recomp_fp24_keep,comdat\n"
    ".weak recomp_fp24_keep\n"
    ".type recomp_fp24_keep,@function\n"
    "recomp_fp24_keep:\n"
    "  stwu 1,-192(1)\n  stw 0,8(1)\n  mflr 0\n  stw 0,12(1)\n"
    "  stw 3,16(1)\n  stw 4,20(1)\n  stw 5,24(1)\n  stw 6,28(1)\n  stw 7,32(1)\n"
    "  stw 8,36(1)\n  stw 9,40(1)\n  stw 10,44(1)\n  stw 11,48(1)\n  stw 12,52(1)\n"
    "  mfcr 0\n  stw 0,56(1)\n  mfxer 0\n  stw 0,60(1)\n  mfctr 0\n  stw 0,64(1)\n"
    "  stfd 0,72(1)\n  stfd 2,80(1)\n  stfd 3,88(1)\n  stfd 4,96(1)\n  stfd 5,104(1)\n"
    "  stfd 6,112(1)\n  stfd 7,120(1)\n  stfd 8,128(1)\n  stfd 9,136(1)\n  stfd 10,144(1)\n"
    "  stfd 11,152(1)\n  stfd 12,160(1)\n  stfd 13,168(1)\n"
    "  bl recomp_fp_round24\n"
    "  mtfsb0 3\n  mtfsb0 4\n  mtfsb0 0\n"
    "  lfd 0,72(1)\n  lfd 2,80(1)\n  lfd 3,88(1)\n  lfd 4,96(1)\n  lfd 5,104(1)\n"
    "  lfd 6,112(1)\n  lfd 7,120(1)\n  lfd 8,128(1)\n  lfd 9,136(1)\n  lfd 10,144(1)\n"
    "  lfd 11,152(1)\n  lfd 12,160(1)\n  lfd 13,168(1)\n"
    "  lwz 0,64(1)\n  mtctr 0\n  lwz 0,60(1)\n  mtxer 0\n  lwz 0,56(1)\n  mtcrf 255,0\n"
    "  lwz 3,16(1)\n  lwz 4,20(1)\n  lwz 5,24(1)\n  lwz 6,28(1)\n  lwz 7,32(1)\n"
    "  lwz 8,36(1)\n  lwz 9,40(1)\n  lwz 10,44(1)\n  lwz 11,48(1)\n  lwz 12,52(1)\n"
    "  lwz 0,12(1)\n  mtlr 0\n  lwz 0,8(1)\n"
    "  addi 1,1,192\n  blr\n"
    ".size recomp_fp24_keep,.-recomp_fp24_keep\n"
    ".popsection\n"
    ".macro recomp_fp24_stub d, s\n"
    ".pushsection .text.recomp_fp24_\\d\\()_\\s,\"axG\",@progbits,recomp_fp24_\\d\\()_\\s,comdat\n"
    ".weak recomp_fp24_\\d\\()_\\s\n"
    ".type recomp_fp24_\\d\\()_\\s,@function\n"
    "recomp_fp24_\\d\\()_\\s:\n"
    "  stwu 1,-32(1)\n  stw 0,8(1)\n  mflr 0\n  stw 0,12(1)\n"
    "  stfd 1,16(1)\n  fmr 1,\\s\n"
    "  bl recomp_fp24_keep\n"
    "  stfd 1,24(1)\n  lfd 1,16(1)\n  lfd \\d,24(1)\n"
    "  lwz 0,12(1)\n  mtlr 0\n  lwz 0,8(1)\n"
    "  addi 1,1,32\n  blr\n"
    ".size recomp_fp24_\\d\\()_\\s,.-recomp_fp24_\\d\\()_\\s\n"
    ".popsection\n"
    ".endm\n");
#else
#define RECOMP_FP_ROUND24_KEEP 0
#endif
static __attribute__((noinline, cold, unused)) double recomp_fp_round24(double x) {"""

TRACE_SEQ = """    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \\
    g_icall_trace_idx++; \\
    g_icall_count++; \\
"""
TRACE_NEW = """    RECOMP_ICALL_TRACE(_va); \\
"""
TRACE_DEF_ANCHOR = "/** Total count of indirect calls executed. */\nextern volatile uint64_t g_icall_count;\n"
TRACE_DEF = TRACE_DEF_ANCHOR + """
/* WiiLibs: the ring and the count cost two loads, a 64-bit add and three stores on every indirect
 * call (the count is 64-bit on a 32-bit CPU), so they are compiled only with WL_RECOMP_ICALL_TRACE;
 * the runtime then defines the three variables above. */
#ifdef WL_RECOMP_ICALL_TRACE
#define RECOMP_ICALL_TRACE(va) do { \\
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = (va); \\
    g_icall_trace_idx++; \\
    g_icall_count++; \\
} while (0)
#else
#define RECOMP_ICALL_TRACE(va) ((void)0)
#endif
"""

CVT_START = "static inline int32_t MMX_CVT_F2I(float v, int truncate)\n{"
CVT_END = "static inline RecompMmx MMX_FROM_PS("
CVT_NEW = """static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
    /* WiiLibs: nearest-even (MXCSR's default) or truncation, independent of the host's rounding
     * mode, and 0x80000000 out of range: the same on every host. */
    return recomp_cvt_si32((double)v, truncate);
}

"""

XMM_HOST = """/* WiiLibs: the SSE loads and stores map their address once and add the lanes' offsets on the
 * host side, and the --mem-disp forms (XMM_MEM_D, ...) their displacement too, as MEMxx_D does: a
 * 16-byte access at an address the guest may touch stays in its 256 MB region. */
static inline __attribute__((always_inline)) RecompXmm recomp_xmm_mem_h(uintptr_t p) {
    RecompXmm r;
    r.u[0] = RECOMP_LE32_AT(p);     r.u[1] = RECOMP_LE32_AT(p + 4);
    r.u[2] = RECOMP_LE32_AT(p + 8); r.u[3] = RECOMP_LE32_AT(p + 12);
    return r;
}
static inline __attribute__((always_inline)) void recomp_xmm_store_h(uintptr_t p, RecompXmm v) {
    RECOMP_LE32_AT(p) = v.u[0];     RECOMP_LE32_AT(p + 4) = v.u[1];
    RECOMP_LE32_AT(p + 8) = v.u[2]; RECOMP_LE32_AT(p + 12) = v.u[3];
}
static inline __attribute__((always_inline)) void recomp_xmm_load_half_h(RecompXmm *dst, uintptr_t p, int high) {
    dst->u[high * 2]     = RECOMP_LE32_AT(p);
    dst->u[high * 2 + 1] = RECOMP_LE32_AT(p + 4);
}
static inline __attribute__((always_inline)) void recomp_xmm_store_half_h(uintptr_t p, RecompXmm src, int high) {
    RECOMP_LE32_AT(p)     = src.u[high * 2];
    RECOMP_LE32_AT(p + 4) = src.u[high * 2 + 1];
}
#define XMM_MEM_D(b, d)             recomp_xmm_mem_h(RECOMP_HOST_D(b, d))
#define XMM_STORE_D(b, d, v)        recomp_xmm_store_h(RECOMP_HOST_D(b, d), (v))
#define XMM_LOAD_LOW_D(dst, b, d)   recomp_xmm_load_half_h(&(dst), RECOMP_HOST_D(b, d), 0)
#define XMM_LOAD_HIGH_D(dst, b, d)  recomp_xmm_load_half_h(&(dst), RECOMP_HOST_D(b, d), 1)
#define XMM_STORE_LOW_D(b, d, src)  recomp_xmm_store_half_h(RECOMP_HOST_D(b, d), (src), 0)
#define XMM_STORE_HIGH_D(b, d, src) recomp_xmm_store_half_h(RECOMP_HOST_D(b, d), (src), 1)

"""
XMM_HOST_ANCHOR = "/* -- guest memory --\n * Addresses are guest VAs"
XMM_BODY_EDITS = [
    ("""    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
""", """    return recomp_xmm_mem_h(XBOX_PTR(addr));
"""),
    ("""    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
""", """    recomp_xmm_store_h(XBOX_PTR(addr), v);
"""),
    ("""    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
""", """    recomp_xmm_load_half_h(dst, XBOX_PTR(addr), high);
"""),
    ("""    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
""", """    recomp_xmm_store_half_h(XBOX_PTR(addr), src, high);
"""),
]
MMX_HOST_ANCHOR = "/* The same at base + disp (--mem-disp), as XMM_MEM_D. */\n#ifndef MMX_MEM_D"
MMX_HOST = """/* WiiLibs: the --mem-disp forms map base once and add disp on the host side. */
static inline __attribute__((always_inline)) RecompMmx recomp_mmx_mem_h(uintptr_t p) {
    RecompMmx r; uint64_t t;
    memcpy(&t, (const void *)p, 8);
    r.q = RECOMP_MMX_LE64(t);
    return r;
}
static inline __attribute__((always_inline)) void recomp_mmx_store_h(uintptr_t p, RecompMmx v) {
    uint64_t t = RECOMP_MMX_LE64(v.q);
    memcpy((void *)p, &t, 8);
}
#define MMX_MEM_D(b, d)      recomp_mmx_mem_h(RECOMP_HOST_D(b, d))
#define MMX_STORE_D(b, d, v) recomp_mmx_store_h(RECOMP_HOST_D(b, d), (v))

"""

UNION_EDITS = [
    ("typedef union RecompXmm {",
     "/* WiiLibs: little-endian storage, so f[i]/u[i]/d[]/q[] overlay as on x86 (and a memcpy of the\n"
     " * register is the guest's byte image). */\n"
     "typedef union __attribute__((scalar_storage_order(\"little-endian\"))) RecompXmm {"),
    ("typedef union RecompMmx {",
     "/* WiiLibs: little-endian storage: ub[0] is the low byte of q, as on x86, so MMX_FROM32, the\n"
     " * unpacks and MMX_MEM's raw copy of guest bytes agree with the hardware. */\n"
     "typedef union __attribute__((scalar_storage_order(\"little-endian\"))) RecompMmx {"),
]

# Edit 24: MMX_MEM and MMX_STORE copy the guest's bytes through a uint64_t, not the union: with
# memcpy into the union (reverse storage order, an aggregate) GCC kept every MMX register that
# passed through a load or a store in a stack slot (sub_001842D0, Halo's MMX copy loop, stored and
# reloaded each of them). memcpy into an integer and a byte swap is defined C, and the two swaps of
# a load and a store cancel. (A raw copy of the union's words through a pointer cast is punning
# between storage orders, undefined, and GCC 13 miscompiled it.)
MMX_LE64_ANCHOR = "#ifndef RECOMP_MMX_DEFINED\n"
MMX_LE64 = """/* WiiLibs: an integer holding 8 guest bytes in host order (a memcpy of them) as RecompMmx's q,
 * which holds them little-endian, and back: the MMX loads and stores (edit 24). */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RECOMP_MMX_LE64(t) __builtin_bswap64(t)
#else
#define RECOMP_MMX_LE64(t) (t)
#endif
"""
MMX_BODY_EDITS = [
    ("""    RecompMmx r;
    memcpy(&r, (const void *)XBOX_PTR(addr), 8);
    return r;
""", """    RecompMmx r; uint64_t t;
    memcpy(&t, (const void *)XBOX_PTR(addr), 8);
    r.q = RECOMP_MMX_LE64(t);
    return r;
"""),
    ("""    memcpy((void *)XBOX_PTR(addr), &v, 8);
""", """    uint64_t t = RECOMP_MMX_LE64(v.q);
    memcpy((void *)XBOX_PTR(addr), &t, 8);
"""),
]


LANEWISE_OLD = """#define RECOMP_XMM_LANEWISE(name, expr)                                   \\
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \\
        RecompXmm r; int i;                                               \\
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.f[i] = (expr); }    \\
        return r;                                                         \\
    }
"""
LANEWISE_NEW = """/* WiiLibs: RecompXmm's lanes are little-endian, and on a big-endian host a float read from
 * reversed storage is an lwbrx, a store and a reload, and one written back a store, a reload and a
 * three-instruction swap, lane after lane, each reload waiting on the store before it (XMM_MUL and
 * XMM_ADD out of line took 1.3% of Halo's time on the Wii). Here the operands' lanes go to host-order
 * copies first, the arithmetic runs on those, and the results go back with one stwbrx each; inline,
 * as the operands are the register globals and a call made a copy of each. `a` and `b` are the
 * host-order copies, so `expr` is upstream's. */
#define RECOMP_XMM_LANEWISE(name, expr)                                   \\
    static inline __attribute__((always_inline)) RecompXmm name(RecompXmm a_, RecompXmm b_) { \\
        union { float f[4]; uint32_t u[4]; } a, b, h;                     \\
        RecompXmm r; int i;                                               \\
        _Pragma("GCC unroll 4")                                           \\
        for (i = 0; i < 4; ++i) { a.u[i] = a_.u[i]; b.u[i] = b_.u[i]; }   \\
        _Pragma("GCC unroll 4")                                           \\
        for (i = 0; i < 4; ++i) { (void)a; (void)b; h.f[i] = (expr); }    \\
        _Pragma("GCC unroll 4")                                           \\
        for (i = 0; i < 4; ++i) r.u[i] = h.u[i];                          \\
        return r;                                                         \\
    }
"""

# The SIMD helpers the generated code calls at each site: plain `static inline` left GCC an
# out-of-line copy with a frame of its own in each unit at -Os (MMX_MEM, MMX_STORE, XMM_SCALAR,
# XMM_SHUFFLE and the movlps/movhps halves were each 0.2-0.4% of Halo's time on the Wii).
ALWAYS_INLINE = [
    ("XMM_SCALAR", "RecompXmm XMM_SCALAR(float v) {"),
    ("XMM_MEM", "RecompXmm XMM_MEM(uint32_t addr) {"),
    ("XMM_STORE", "void XMM_STORE(uint32_t addr, RecompXmm v) {"),
    ("recomp_xmm_load_half", "void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,"),
    ("recomp_xmm_store_half", "void recomp_xmm_store_half(uint32_t addr, RecompXmm src,"),
    ("MMX_MEM", "RecompMmx MMX_MEM(uint32_t addr) {"),
    ("MMX_STORE", "void MMX_STORE(uint32_t addr, RecompMmx v) {"),
]

SHUFFLE_OLD = """static inline RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}
"""
SHUFFLE_NEW = """static inline __attribute__((always_inline)) RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    /* WiiLibs: lane by lane, as upstream. Lanes moved as bytes (memcpy into the little-endian
     * union) were miscompiled by GCC 16's scalar replacement of aggregates (devkitPPC 16.1, -Os and
     * -O2): a lane read back after the copy kept its bytes reversed, so a movhps after a shufps
     * stored the floats big-endian. This form is also the smaller one there. */
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}
"""


MMX_START = "#ifndef RECOMP_MMX_DEFINED\n"
MMX_END = "extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;\n"
GENCODE_START = "#ifdef RECOMP_GENERATED_CODE\n#define eax g_eax\n"


def hoist_mmx(src: str) -> str:
    """Move the RecompMmx type and the g_mm* declarations ahead of the generated-code-only block."""
    for anchor in (MMX_START, MMX_END, GENCODE_START):
        if src.count(anchor) != 1:
            raise AnchorError(f"edit 'hoist RecompMmx': anchor found {src.count(anchor)} times: {anchor!r}")
    i = src.index(MMX_START)
    j = src.index(MMX_END) + len(MMX_END)
    block = src[i:j]
    src = src[:i] + src[j:]
    k = src.index(GENCODE_START)
    if k > i:
        raise AnchorError("edit 'hoist RecompMmx': the MMX type is no longer inside the generated-code block")
    note = ("/* WiiLibs: the MMX register type and storage are declared for every includer (upstream has\n"
            " * them only under RECOMP_GENERATED_CODE), so the runtime can define g_mm0..7 without\n"
            " * the register-name macros below. */\n")
    return src[:k] + note + block + "\n" + src[k:]


BACKEDGE_OLD = """#ifndef RECOMP_BACKEDGE
#define RECOMP_BACKEDGE() ((void)0)
#endif
"""
BACKEDGE_NEW = """/* WiiLibs: a backward jump is a safe point of wl/xboxrt's guest lock (wl/xboxrt/thread.h): one
 * load and a branch, unless another guest thread or interrupt work waits for the lock. A loop
 * that only polls memory (a flip counter, a fence word) would otherwise never let them run. */
extern volatile uint32_t g_xb_attention;
void wl_xb_safe_point(void);
#ifndef RECOMP_BACKEDGE
/* WiiLibs: in code built for size (-Os) the check is a call of one small function per unit: the
 * load, compare and the branch around the call (four or five instructions at each of Halo's
 * ~6,300 loops) become one branch, for a call and a return each time a cold loop goes round. */
#if defined(__OPTIMIZE_SIZE__) && !defined(RECOMP_NO_BACKEDGE_CALL)
static __attribute__((noinline, unused)) void recomp_backedge(void) {
    if (__builtin_expect(g_xb_attention != 0, 0)) wl_xb_safe_point();
}
#define RECOMP_BACKEDGE() recomp_backedge()
#else
#define RECOMP_BACKEDGE() do { if (__builtin_expect(g_xb_attention != 0, 0)) wl_xb_safe_point(); } while (0)
#endif
#endif
"""


RESOLVE_SEQ = """    recomp_func_t _fn = recomp_lookup_manual(_va); \\
    if (!_fn) _fn = recomp_lookup(_va); \\
    if (!_fn) _fn = recomp_lookup_kernel(_va); \\
"""
RESOLVE_NEW = """    recomp_func_t _fn = RECOMP_RESOLVE(_va); \\
"""
RESOLVE_DEF_ANCHOR = "/**\n * RECOMP_ICALL - Indirect call through the dispatch table.\n"
RESOLVE_DEF = """/* WiiLibs: an indirect call or jump resolves its target with one call, wl/xboxrt's wl_xb_lookup
 * (thunk.h): the title's manual functions, the generated table and the kernel thunks in that order,
 * as the three calls upstream makes here, behind a direct-mapped cache of the answers (the generated
 * table is a binary search: 3.8% of Halo's time on the Wii went to it). One call instead of three
 * also makes each of a title's indirect call sites about 30 bytes smaller. (recomp_dispatch.c
 * defines RECOMP_DISPATCH_H and its own recomp_func_t after this header: it makes no calls.) */
#ifndef RECOMP_DISPATCH_H
recomp_func_t wl_xb_lookup(uint32_t xbox_va);
#endif
#ifndef RECOMP_RESOLVE
#define RECOMP_RESOLVE(va) wl_xb_lookup(va)
#endif

""" + RESOLVE_DEF_ANCHOR


PUSH_OLD = """#define PUSH32(sp, val) do { \\
    uint32_t _pv = (uint32_t)(val); \\
    (sp) -= 4; \\
    MEM32(sp) = _pv; \\
} while(0)
"""
PUSH_NEW = """#define RECOMP_PUSH32_INLINE(sp, val) do { \\
    uint32_t _pv = (uint32_t)(val); \\
    (sp) -= 4; \\
    MEM32_SP(sp, 0) = _pv; \\
} while(0)
/* WiiLibs: in code built for size (-Os, the units the profile does not name hot) a push onto the
 * guest's own esp, g_esp, is a call of recomp_push32_esp: load, decrement, store, the address
 * transform and the byte-reversed store (about eight instructions) become one branch. Every direct
 * call pushes its return address so, about 14,000 sites in Halo: 348 KB of the Wii's MEM1, for
 * two branches per push run. A push onto a local copy of esp stays inline (the local is a register
 * there). */
#if defined(__OPTIMIZE_SIZE__) && !defined(RECOMP_NO_PUSH32_CALL)
static __attribute__((noinline, unused)) void recomp_push32_esp(uint32_t v) {
    RECOMP_PUSH32_INLINE(g_esp, v);
}
#if (defined(__PPC__) || defined(__powerpc__)) && !defined(__powerpc64__) && !defined(WL_XB_EA_NO_ASM) && \\
    !defined(RECOMP_NO_PUSH32_WORD)
/* On PowerPC a constant (a call's return address, the common case) follows the branch as a data
 * word: recomp_push32_word reads it at the link register, returns past it, and pushes it through
 * recomp_push32_esp_w. Two words at the site instead of three (lis, ori, bl); to the code around
 * it the same call (every volatile register clobbered). RECOMP_NO_PUSH32_WORD turns it off. */
__attribute__((weak, noinline, used)) void recomp_push32_esp_w(uint32_t v) {
    RECOMP_PUSH32_INLINE(g_esp, v);
}
__asm__(
    ".pushsection .text.recomp_push32_word,\\"axG\\",@progbits,recomp_push32_word,comdat\\n"
    ".weak recomp_push32_word\\n"
    ".type recomp_push32_word,@function\\n"
    "recomp_push32_word:\\n"
    "  mflr 12\\n  lwz 3,0(12)\\n  addi 12,12,4\\n  mtlr 12\\n  b recomp_push32_esp_w\\n"
    ".size recomp_push32_word,.-recomp_push32_word\\n"
    ".popsection\\n");
#define RECOMP_PUSH32_ESP(val) do { \\
    if (__builtin_constant_p(val)) \\
        __asm__ __volatile__("bl recomp_push32_word\\n\\t.long %0" : : "i"((uint32_t)(val)) \\
            : "r0", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "ctr", "lr", \\
              "cr0", "cr1", "cr5", "cr6", "cr7", "xer", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", \\
              "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13", "memory"); \\
    else recomp_push32_esp((uint32_t)(val)); \\
} while(0)
#else
#define RECOMP_PUSH32_ESP(val) recomp_push32_esp((uint32_t)(val))
#endif
#define PUSH32(sp, val) do { \\
    if (&(sp) == &g_esp) RECOMP_PUSH32_ESP(val); \\
    else RECOMP_PUSH32_INLINE(sp, val); \\
} while(0)
#else
#define PUSH32(sp, val) RECOMP_PUSH32_INLINE(sp, val)
#endif
"""


POP_OLD = """#define POP32(sp, dst) do { \\
    (dst) = MEM32(sp); \\"""
POP_NEW = POP_OLD.replace("MEM32(sp)", "MEM32_SP(sp, 0)")
ICALL_ANCHOR = """/* ================================================================
 * Register name aliases for generated code
"""
ICALL_NEW = """/* WiiLibs: in code built for size (-Os, the units the profile does not name hot) an indirect call
 * or jump is a call of one small function per unit: the code-range check, the lookup, the call
 * (a tail call there) and both failure paths are about 25 instructions at each of Halo's ~2,100
 * sites. Only when the macros' hooks are off (no trace, feedback, ABI check or call profile):
 * then they are the same steps in the same order. The pushed return address is the caller's;
 * saved_esp is read at the call instead of at a failure, the same value (nothing between changes
 * it). RECOMP_NO_ICALL_CALL keeps them inline. */
#if defined(__OPTIMIZE_SIZE__) && !defined(RECOMP_NO_ICALL_CALL) && !defined(WL_RECOMP_ICALL_TRACE) && \\
    !defined(RECOMP_ICALL_FEEDBACK) && !defined(RECOMP_ABI_CHECK) && !defined(RECOMP_CALL_PROFILE) && \\
    !defined(RECOMP_DISPATCH_H)
static __attribute__((noinline, unused)) void recomp_icall_pop(uint32_t va) {
    recomp_func_t fn;
    if (!RECOMP_ICALL_IS_CODE(va)) { recomp_icall_not_code_log(va); g_esp += 4; g_eax = 0; return; }
    fn = RECOMP_RESOLVE(va);
    if (fn) { fn(); return; }
    recomp_icall_fail_log(va); g_esp += 4; g_eax = 0;
}
static __attribute__((noinline, unused)) void recomp_icall_to(uint32_t va, uint32_t saved_esp) {
    recomp_func_t fn;
    if (!RECOMP_ICALL_IS_CODE(va)) { recomp_icall_not_code_log(va); g_esp = saved_esp; g_eax = 0; return; }
    fn = RECOMP_RESOLVE(va);
    if (fn) { fn(); return; }
    recomp_icall_fail_log(va); g_esp = saved_esp; g_eax = 0;
}
static __attribute__((noinline, unused)) void recomp_itail(uint32_t va) {
    recomp_func_t fn = RECOMP_RESOLVE(va);
    if (fn) { fn(); return; }
    recomp_icall_fail_log(va); g_esp += 4; g_eax = 0;
}
#undef RECOMP_ICALL
#undef RECOMP_ICALL_SAFE
#undef RECOMP_ICALL_SAFE_CC
#undef RECOMP_ICALL_SAFE_AT
#undef RECOMP_ICALL_SAFE_AT_CC
#undef RECOMP_ITAIL
#define RECOMP_ICALL(xbox_va) recomp_icall_pop((uint32_t)(xbox_va))
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) recomp_icall_to((uint32_t)(xbox_va), (uint32_t)(saved_esp))
#define RECOMP_ICALL_SAFE_CC(xbox_va, saved_esp) do { \\
    (void)(saved_esp); recomp_icall_pop((uint32_t)(xbox_va)); } while (0)
#define RECOMP_ICALL_SAFE_AT(xbox_va, saved_esp, site) \\
    recomp_icall_to((uint32_t)(xbox_va), (uint32_t)(saved_esp))
#define RECOMP_ICALL_SAFE_AT_CC(xbox_va, saved_esp, site) do { \\
    (void)(saved_esp); recomp_icall_pop((uint32_t)(xbox_va)); } while (0)
#define RECOMP_ITAIL(xbox_va) recomp_itail((uint32_t)(xbox_va))
#endif

""" + ICALL_ANCHOR


PORT_OLD = """#ifndef RECOMP_PORT_IN
#define RECOMP_PORT_IN(port, size) ((void)(port), (void)(size), 0u)
#endif
#ifndef RECOMP_PORT_OUT
#define RECOMP_PORT_OUT(port, value, size) ((void)(port), (void)(value), (void)(size))
#endif
"""
PORT_NEW = """/* WiiLibs: the console's x86 I/O ports are wl/xboxrt's (thunk.h) */
uint32_t wl_xb_port_in(uint32_t port, unsigned size);
void wl_xb_port_out(uint32_t port, uint32_t value, unsigned size);
#ifndef RECOMP_PORT_IN
#define RECOMP_PORT_IN(port, size) wl_xb_port_in((port), (size))
#endif
#ifndef RECOMP_PORT_OUT
#define RECOMP_PORT_OUT(port, value, size) wl_xb_port_out((port), (value), (size))
#endif
"""


def transform(src: str, commit: str, release: str, patches: list[str]) -> str:
    src = replace_once(src, "#define RECOMP_TYPES_H\n", GUARD, "compiler guard")
    src = replace_once(src, TLS_OLD, TLS_NEW, "RECOMP_TLS")
    src = replace_between(src, ATOMIC_START, ATOMIC_END, ATOMIC_NEW, "atomics")
    src = replace_between(src, MEM_START, MEM_END, MEM_NEW + "\n", "memory macros")
    src = replace_between(src, SET_DEFAULTS_START, SET_DEFAULTS_END, "", "MEMF_SET defaults")
    src = replace_once(src, ROUND24_OLD, ROUND24_NEW, "recomp_fp_round24 out of line")
    src = replace_once(src, FPPC_OLD, FPPC_NEW, "RECOMP_FP_PC fast path")
    src = replace_once(src, FCMP_OLD, FCMP_NEW, "RECOMP_FCMP")
    n = src.count(TRACE_SEQ)
    if n < 5:
        raise AnchorError(f"edit 'icall trace': expected the trace sequence in at least 5 macros, found {n}")
    src = src.replace(TRACE_SEQ, TRACE_NEW)
    src = replace_once(src, TRACE_DEF_ANCHOR, TRACE_DEF, "icall trace macro")
    n = src.count(RESOLVE_SEQ)
    if n < 6:
        raise AnchorError(f"edit 'icall resolve': expected the lookup sequence in at least 6 macros, found {n}")
    src = src.replace(RESOLVE_SEQ, RESOLVE_NEW)
    src = replace_once(src, RESOLVE_DEF_ANCHOR, RESOLVE_DEF, "RECOMP_RESOLVE")
    src = replace_between(src, CVT_START, CVT_END, CVT_NEW, "MMX_CVT_F2I")
    for old, new in UNION_EDITS:
        src = replace_once(src, old, new, old)
    src = replace_once(src, LANEWISE_OLD, LANEWISE_NEW, "RECOMP_XMM_LANEWISE")
    src = replace_once(src, SHUFFLE_OLD, SHUFFLE_NEW, "XMM_SHUFFLE")
    src = replace_once(src, XMM_HOST_ANCHOR, XMM_HOST + XMM_HOST_ANCHOR, "SSE accesses on the host side")
    for old, new in XMM_BODY_EDITS:
        src = replace_once(src, old, new, "SSE access: " + old.split("\n")[1].strip())
    src = replace_once(src, MMX_HOST_ANCHOR, MMX_HOST + MMX_HOST_ANCHOR, "MMX accesses at base + disp")
    for old, new in MMX_BODY_EDITS:
        src = replace_once(src, old, new, "MMX access: " + old.split("\n")[1].strip())
    for name, sig in ALWAYS_INLINE:
        src = replace_once(src, "static inline " + sig, "static inline __attribute__((always_inline)) " + sig,
                           "always inline: " + name)
    src = hoist_mmx(src)
    src = replace_once(src, MMX_LE64_ANCHOR, MMX_LE64 + MMX_LE64_ANCHOR, "RECOMP_MMX_LE64")
    src = replace_once(src, BACKEDGE_OLD, BACKEDGE_NEW, "RECOMP_BACKEDGE safe point")
    src = replace_once(src, PORT_OLD, PORT_NEW, "RECOMP_PORT_IN/OUT")
    src = replace_once(src, PUSH_OLD, PUSH_NEW, "PUSH32 onto g_esp out of line")
    src = replace_once(src, POP_OLD, POP_NEW, "POP32 at esp without the address map")
    src = replace_once(src, ICALL_ANCHOR, ICALL_NEW, "indirect calls out of line")
    for needle in ("__thread", "_InterlockedExchangeAdd", "(*(volatile uint32_t *)XBOX_PTR"):
        if needle in src:
            raise AnchorError(f"after the edits the header still has {needle!r}: an edit missed a new copy")
    return HEADER.format(commit=commit[:12], release=release or "unreleased",
                         patches=", ".join(patches) or "none") + src


def from_pipeline(tmp: Path) -> tuple[str, str, str, list[str]]:
    sys.path.insert(0, str(WL_ROOT / "tools"))
    import xbox_recomp  # noqa: E402
    pipe = xbox_recomp.prepare_pipeline(tmp / "pipeline", quiet=True)
    with open(xbox_recomp.STATE, encoding="utf-8") as f:
        state = json.load(f)
    tpl = (pipe / "templates" / "runtime" / "recomp_types.h").read_text(encoding="utf-8")
    names = [p.name.split("-", 1)[0] for p in xbox_recomp.patch_files()]
    return tpl, state["vendored"]["commit"], state["vendored"].get("release", ""), names


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[1:]))
    ap.add_argument("--template", help="a patched template (default: patch the vendored one)")
    ap.add_argument("--commit", default="unknown", help="with --template: the upstream commit it is from")
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--check", action="store_true", help="compare with --out instead of writing it")
    args = ap.parse_args(argv)
    try:
        with tempfile.TemporaryDirectory(prefix="wl-recomp-types-") as td:
            if args.template:
                tpl, commit, release, names = Path(args.template).read_text(encoding="utf-8"), args.commit, "", []
            else:
                tpl, commit, release, names = from_pipeline(Path(td))
            text = transform(tpl, commit, release, names)
    except AnchorError as e:
        print(f"gen_recomp_types: {e}", file=sys.stderr)
        return 2
    out = Path(args.out)
    if args.check:
        have = out.read_text(encoding="utf-8") if out.exists() else ""
        if have == text:
            print(f"{out}: up to date")
            return 0
        sys.stdout.writelines(list(difflib.unified_diff(
            have.splitlines(True), text.splitlines(True), str(out), "regenerated", n=2))[:200])
        print(f"\n{out} is not what the template gives: run {Path(__file__).name} and review the diff",
              file=sys.stderr)
        return 1
    out.write_text(text, encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
