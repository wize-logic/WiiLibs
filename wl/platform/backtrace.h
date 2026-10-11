// SPDX-License-Identifier: MIT
// Backtraces and where-from logging for the 32-bit PowerPC EABI/SysV stack: each frame holds the
// caller's frame at offset 0, and the frame above it keeps the return address one word in (the
// callee stores its link register in its caller's frame). Addresses are the 32-bit values to give
// addr2line (or the linker map), so the same report works from a log line on the Wii and from a
// big-endian Linux test.
//
// The walker (wl_backtrace_walk) is portable: it reads the stack through a callback, so it runs on
// any host against a synthetic memory image. wl_platform_backtrace walks the running thread's own
// stack: on the Wii (MEM1 and MEM2 only, so a damaged chain ends the walk instead of faulting), on
// the big-endian ppc-be test build (the thread's stack), and not at all on the other hosts (it
// returns 0). Needs no allocation and is safe to call from a crash or log path.
#pragma once

#include "wl/base/base.h"

// Reads the 32-bit word at `address` into *value; false when the address cannot be read.
typedef bool (*WlBacktraceRead)(void *user, uint32_t address, uint32_t *value);

// Walks the frame chain from the frame at `sp`. If `lr` is not 0 it is the first address reported
// (the interrupted code's own link register, for a crash report). Each frame then gives the return
// address kept in the frame above it. The walk ends at `max` addresses, at a frame that is not word
// aligned or cannot be read, at a back chain that does not rise (the stack's first frame holds 0)
// and at a return address of 0 (the first frames of a thread have none). The first `skip` addresses are not stored. Returns the number stored in `out`.
uint32_t wl_backtrace_walk(WlBacktraceRead read, void *user, uint32_t sp, uint32_t lr, uint32_t skip,
                           uint32_t *out, uint32_t max);

// The return addresses up the calling thread's stack: out[0] is the address in the caller of this
// function that the call returns to (where wl_platform_backtrace was called from), out[1] the one
// in its caller, and so on, like __builtin_return_address(0), (1) ... one level up. `skip` drops
// that many from the start. Returns the count stored (0 where unsupported).
uint32_t wl_platform_backtrace(uint32_t *out, uint32_t max, uint32_t skip);

// The same for another context: the stack pointer and link register of an exception (the crash
// report), or of a switched-out thread. The Wii backend reads only MEM1 and MEM2.
uint32_t wl_platform_backtrace_from(uint32_t sp, uint32_t lr, uint32_t *out, uint32_t max);

// Logs "<what> from 80001234 80005678 ..." at error level under `tag` (up to 18 addresses) and
// flushes the log, so the line survives what follows. The first address is where the calling
// function called this one, as wl_platform_backtrace's out[0] is; `skip` drops that many from the
// front (1 in a wrapper, to start at its caller).
void wl_platform_log_backtrace(const char *tag, const char *what, uint32_t skip);

// Opt-in logging of every way out of the program: exit, _exit, abort and SYS_ResetSystem log
// "exit(0) from ..." (tag "exit") before they go on. Wii only. Call
// wl_platform_trace_exits(target) from CMake, or pass the linker
//   --wrap=exit --wrap=_exit --wrap=abort --wrap=SYS_ResetSystem
// yourself (a build that does not use CMake). The wrappers are in wii/trace_wii.c and are linked
// only when those flags send calls to them.
