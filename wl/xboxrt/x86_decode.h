// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors: src/platform/mmio_decode.h, the decoder of the x86-64 instruction that
// faulted on a device register. Rewritten over a portable register file (so tests drive it with
// byte strings) and extended with what GCC emits for the generated code's MEM macros on an x86-64
// host: the ALU and group-1 read-modify-write forms with all six arithmetic flags, inc/dec,
// movsx/movsxd, xchg/cmpxchg/xadd, the high-byte registers, and the SSE moves.
//
// Only the x86-64 host build uses it (its SIGSEGV handler, fault_posix.c); it compiles everywhere.
#pragma once

#include "wl/base/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t gpr[16]; // rax rcx rdx rbx rsp rbp rsi rdi r8 .. r15 (the encoding's order)
    uint64_t rip;
    uint64_t rflags;
    uint8_t xmm[16][16]; // little-endian lanes, as the CPU keeps them
} WlXbX86Regs;

#define WL_XB_X86_CF 0x0001u
#define WL_XB_X86_PF 0x0004u
#define WL_XB_X86_AF 0x0010u
#define WL_XB_X86_ZF 0x0040u
#define WL_XB_X86_SF 0x0080u
#define WL_XB_X86_OF 0x0800u

// Emulates the instruction at `ip` (its bytes; `regs->rip` is not read) whose memory operand is
// the device register at guest VA `va` (wl_xb_mmio_read/write), updating the registers and flags.
// Returns the instruction's length (the caller advances rip), or 0 when it is not one this
// decoder emulates, or no device claimed the access; `*what` (may be NULL) then names the reason.
uint32_t wl_xb_x86_emulate(const uint8_t *ip, WlXbX86Regs *regs, uint32_t va, const char **what);

#ifdef __cplusplus
}
#endif
