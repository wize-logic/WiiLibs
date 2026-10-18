// SPDX-License-Identifier: MIT
// Block copies, fills and compares tuned for Broadway (wii/memfast_wii.c), and the link that makes
// every memcpy, memmove, memset and memcmp of a Wii program use them.
//
// devkitPPC's newlib has only the generic C string functions: memcpy moves 16 bytes a loop when
// both pointers are word aligned and a byte at a time otherwise, memset stores a word a loop,
// memcmp compares a byte at a time unless both pointers are word aligned. These:
//   - move 32 bytes (a cache line) a loop in 8 word loads and 8 word stores; a source that is
//     not word aligned is read with misaligned word loads (the 750 does them in hardware), so a
//     misaligned copy costs what an aligned one does;
//   - zero each whole destination line with dcbz before its stores (no line fill from memory for
//     a line that is about to be overwritten), and a zero fill is dcbz alone: 3 instructions a
//     line. dcbz is used only on the processor's cached RAM: the BAT views of MEM1
//     (0x80000000..0x817FFFFF) and MEM2 (0x90000000..0x93FFFFFF), and the EAs a program maps
//     through the page table below 0x1C000000 (the Xbox runtime's guest RAM, WIMG 0; its devices
//     sit above, and the uncached mirrors 0xC0000000 and up are never zeroed so);
//   - touch the source two lines ahead (dcbt);
//   - do up to 64 bytes in a few loads and stores (overlapping where the length is not a step),
//     without a loop.
// Every routine reads and writes only the bytes it is given (a misaligned head or tail is exact,
// by overlapping word stores inside the range, never by reading past it), and gives what the C
// library gives for every length and alignment (tests/platform/test_platform_memfast.c, lengths
// 0..300 at every alignment pair). wl_memfast_move and wl_memfast_copy both handle overlapping
// ranges as memmove does (copy may be given a memmove's arguments); the Xbox runtime's lifted
// `rep movs` copies a pattern itself where its ranges overlap and calls memcpy only for disjoint
// ones. wl_memfast_compare returns the difference of the first unequal bytes (unsigned), as
// newlib does. No floating-point register is used, so they are safe in an interrupt handler
// (libogc's do not save the FPRs).
//
// On the host and on the big-endian test build they are the same C (dcbz and dcbt on PowerPC
// only), so the tests and tools/qemu_icount.py count the code the Wii runs.
//
// The link (wii only by default, CMake option WL_MEMFAST_WRAP): wl_platform's link interface
// passes --wrap=memcpy,memmove,memset,memcmp, so every call in the program, libogc's and
// newlib's own included, reaches __wrap_* here, and -u pulls them out of libwl_platform.a. A call
// GCC expands inline (a small constant length) is not a call and stays inline. The C library's
// own functions stay reachable as __real_*. The switch: wl_memfast_set_enabled(false) (or a file
// "oldcopy" in a folder given to wl_memfast_options) sends the four back to newlib's, at the cost
// of a load and a branch a call. WL_MEMFAST_HIST (a CMake option of the same name) counts the
// calls by operation, length and alignment (wl_memfast_hist_write).
#ifndef WL_PLATFORM_MEMFAST_H
#define WL_PLATFORM_MEMFAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// memcpy (overlap allowed, as memmove), memmove, memset and memcmp
void *wl_memfast_copy(void *dst, const void *src, size_t n);
void *wl_memfast_move(void *dst, const void *src, size_t n);
void *wl_memfast_fill(void *dst, int c, size_t n);
int wl_memfast_compare(const void *a, const void *b, size_t n);

// The wrapped memcpy/memmove/memset/memcmp use these routines (true, the default) or the C
// library's (false). Changing it is safe at any time; the call in flight finishes the way it began.
void wl_memfast_set_enabled(bool on);
bool wl_memfast_enabled(void);
// Reads the switch from a title's folder: a file "oldcopy" there turns the routines off (the C
// library's, as before, for an A/B on the console). Read once, at boot.
void wl_memfast_options(const char *dir);

// Whether the program links the wrap (WL_MEMFAST_WRAP): its memcpy & co. are these.
bool wl_memfast_wrapped(void);

// The call counts of a WL_MEMFAST_HIST build (all zero, and nothing written, without it): for
// each of copy, move, fill, compare, the calls of each length 0..64, then of the lengths in each
// power-of-two step (65..128, ..., 2^19+1 and up), with how many had a pointer that is not word
// aligned, and the bytes in all.
#define WL_MEMFAST_OPS 4u
#define WL_MEMFAST_EXACT 65u   // lengths 0..64 counted one by one
#define WL_MEMFAST_BUCKETS 13u // 65..128, 129..256, ..., 2^18+1..2^19, then over 2^19
typedef struct {
    uint32_t exact[WL_MEMFAST_OPS][WL_MEMFAST_EXACT];
    uint32_t bucket[WL_MEMFAST_OPS][WL_MEMFAST_BUCKETS];
    uint32_t misaligned[WL_MEMFAST_OPS];
    uint64_t calls[WL_MEMFAST_OPS];
    uint64_t bytes[WL_MEMFAST_OPS];
} WlMemfastHist;
bool wl_memfast_hist_enabled(void);
const WlMemfastHist *wl_memfast_hist(void);
// One line an operation (calls, bytes, misaligned), then each length that holds at least 0.1% of
// its calls.
void wl_memfast_hist_write(FILE *out);

#ifdef __cplusplus
}
#endif

#endif
