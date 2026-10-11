// SPDX-License-Identifier: MIT
// A heap that spills over: an allocator (the primary, normally newlib's malloc in MEM1) is tried
// first, and when it is full the request is served from a tagged heap of wl_mem (normally in
// MEM2) instead. free and realloc find the heap a block came from by its address, so a program
// that mallocs more than MEM1 has left it needs no change besides the link flags.
//
// This is the portable core: it knows the primary only through WlOverflowPrimary, so it is
// tested on the host and on the big-endian build with a fake one. On the Wii the primary is
// newlib's allocator, reached through the linker's --wrap (heap_wrap_wii.c; see "On the Wii").
//
// Blocks of the overflow heap carry a 32-byte header in front of the pointer (size, capacity, a
// check value, the offset to the block start), so:
//   - every overflow pointer is 32-byte aligned (GPU/DMA safe), and any power-of-two alignment
//     is served (over-allocating beyond 32);
//   - realloc knows the old size without asking the heap, and shrinks and grows within the
//     block in place;
//   - free checks the header and refuses (logs, counts in stats.bad_frees) a pointer that is not
//     a live overflow block, as wl_mem_free does.
// Each overflow block costs the header plus wl_mem's own 32 bytes.
//
// Rules shared by all entry points:
//   - the primary is always tried first, so memory freed in it is used again;
//   - a request of size 0 is never moved to the overflow heap (the primary's answer stands);
//   - realloc(NULL, n) is malloc(n); realloc(p, 0) frees p and returns NULL;
//   - realloc of a primary block that cannot grow moves it to the overflow heap, and the old
//     block is freed only once the copy is made; if both heaps are full it returns NULL with the
//     block untouched;
//   - realloc of an overflow block that fits its capacity keeps the pointer (any shrink does);
//     one that has to grow moves, to the primary heap if that has room now, else to the overflow
//     heap (a block of a larger alignment than malloc's loses it, as with any realloc);
//   - alignment is a power of two, else the call fails with NULL.
// Thread safety: the counters and the one-time notice are guarded by wl_platform_mem_lock; the
// primary and wl_mem do their own locking.
#pragma once

#include "wl/base/base.h"
#include "wl/platform/heap_sites.h"
#include "wl/platform/mem.h"

// The allocator being backed up. Only `alloc`, `free` and `usable_size` are required.
typedef struct WlOverflowPrimary {
    void *ctx; // passed back as the first argument of each function
    // malloc (align 0) or memalign; NULL when it has no room
    void *(*alloc)(void *ctx, size_t align, size_t size);
    // calloc; NULL: alloc + memset
    void *(*calloc)(void *ctx, size_t count, size_t size);
    // realloc of a block of this heap; NULL when it cannot be done (the block is untouched).
    // NULL here: emulated with alloc, copy and free
    void *(*realloc)(void *ctx, void *ptr, size_t size);
    void (*free)(void *ctx, void *ptr);
    // bytes a block of this heap holds (at least what was asked)
    size_t (*usable_size)(void *ctx, void *ptr);
    // bytes in use and free in this heap, for the report; the function may be NULL
    void (*usage)(void *ctx, size_t *used, size_t *free_bytes);
} WlOverflowPrimary;

typedef struct {
    uint32_t live; // overflow blocks live now
    size_t bytes;  // the sizes asked for, summed over the live blocks
    size_t peak_bytes;
    uint32_t allocs; // blocks served from the overflow heap, ever
    uint32_t frees;
    uint32_t fails;     // requests both heaps refused
    uint32_t bad_frees; // frees of a pointer in the overflow heap that was not a live block
    uint32_t moved_in;  // reallocs of a primary block that moved to the overflow heap
    uint32_t moved_out; // reallocs of an overflow block that moved (to either heap)
} WlOverflowStats;

// Called when both heaps refused a request: `what` is "malloc", "calloc", "memalign" or
// "realloc". Without a hook the failure is logged (error level, tag "heap").
typedef void (*WlOverflowFailFn)(void *user, const char *what, size_t size, size_t align);

// One per primary heap. All zero = not armed: every call then goes to the primary alone.
typedef struct WlOverflow {
    WlMemTag tag;
    uintptr_t lo, hi; // the tag's heap
    bool armed;
    bool announced; // the one-time "primary is full" notice was logged
    bool failing;   // inside the failure report (logging may allocate)
    WlOverflowFailFn on_fail;
    void *fail_user;
    WlOverflowStats stats;
} WlOverflow;

// Arms the overflow with tag's heap. wl_mem must be initialised and the tag enabled (budget > 0),
// else WL_ERR_STATE; WL_ERR_ARG for a bad tag. A second call re-arms only after wl_overflow_stop.
WlResult wl_overflow_init(WlOverflow *o, WlMemTag tag);
// Disarms (call before wl_mem_shutdown). Overflow blocks still live become foreign pointers.
void wl_overflow_stop(WlOverflow *o);
void wl_overflow_set_fail_hook(WlOverflow *o, WlOverflowFailFn fn, void *user);

// True if `ptr` lies in the overflow heap.
bool wl_overflow_owns(const WlOverflow *o, const void *ptr);

void *wl_overflow_malloc(WlOverflow *o, const WlOverflowPrimary *p, size_t size);
void *wl_overflow_calloc(WlOverflow *o, const WlOverflowPrimary *p, size_t count, size_t size);
// memalign and aligned_alloc (the same here)
void *wl_overflow_memalign(WlOverflow *o, const WlOverflowPrimary *p, size_t align, size_t size);
void *wl_overflow_realloc(WlOverflow *o, const WlOverflowPrimary *p, void *ptr, size_t size);
void wl_overflow_free(WlOverflow *o, const WlOverflowPrimary *p, void *ptr);
// Bytes a block of either heap holds; 0 for NULL.
size_t wl_overflow_usable_size(const WlOverflow *o, const WlOverflowPrimary *p, void *ptr);

// For a layer that calls the primary itself (heap_wrap_wii.c's reentrant entry points): the
// overflow half alone. alloc: a block from the overflow heap (align 0 = malloc's), logging the
// first use; NULL if disarmed or full (count it with wl_overflow_failed). free_owned: frees `ptr`
// if it is an overflow block and returns true, else false.
void *wl_overflow_alloc(WlOverflow *o, size_t align, size_t size);
bool wl_overflow_free_owned(WlOverflow *o, void *ptr);
// Records and reports a request that both heaps refused.
void wl_overflow_failed(WlOverflow *o, const char *what, size_t size, size_t align);

void wl_overflow_stats(const WlOverflow *o, WlOverflowStats *out);

// The report: the primary heap's bytes (when it can say) and the overflow heap's, from wl_mem.
typedef struct {
    bool primary_known;
    size_t primary_used, primary_free;
    bool overflow_armed;
    size_t overflow_used; // heap bytes in use, headers included
    size_t overflow_peak;
    size_t overflow_budget;
    size_t overflow_free; // budget - used
    uint32_t overflow_live;
    size_t free_total; // primary_free + overflow_free: what a program can still get
} WlOverflowUsage;

void wl_overflow_usage(const WlOverflow *o, const WlOverflowPrimary *p, WlOverflowUsage *out);
// One INFO line each for the primary and the overflow heap.
void wl_overflow_log_usage(const WlOverflow *o, const WlOverflowPrimary *p);

// ---- On the Wii -------------------------------------------------------------------------------
//
// wii/heap_wrap_wii.c wraps newlib's malloc, calloc, realloc, free, memalign, aligned_alloc and
// their _r forms, with the primary being newlib's own. It is opt-in, like the other wraps: from
// CMake call wl_platform_heap_overflow(<executable>) (it adds the --wrap flags); a build without
// CMake passes the flags in WL_HEAP_OVERFLOW_WRAPS (cmake/WiiLibs.cmake) itself. Then, after
// wl_mem_init with a MEM2 tag whose budget is the overflow's size:
//     wl_heap_overflow_enable(MY_TAG);
// Before that call (and without the link flags) nothing changes: malloc is newlib's alone.
// Declared on every platform for the headers' sake; defined only in that file.

// Arms the overflow with `tag`. WL_ERR_STATE if wl_mem is not initialised or the tag has no heap.
WlResult wl_heap_overflow_enable(WlMemTag tag);
// Disarms it (before wl_mem_shutdown); malloc is newlib's alone again.
void wl_heap_overflow_disable(void);
// Counters and the report of the process-wide overflow (newlib's malloc is the primary).
void wl_heap_overflow_stats(WlOverflowStats *out);
void wl_heap_overflow_usage(WlOverflowUsage *out);
void wl_heap_overflow_log_usage(void);
// True if `ptr` lies in the overflow heap (a pointer from the wrapped malloc in MEM2).
bool wl_heap_overflow_owns(const void *ptr);
// Who holds the heap (heap_sites.h): from this call on, every block of either heap made through
// the wrapped names is counted, those of at least `min_tracked` bytes by the two return addresses
// above the wrapper (for addr2line), the rest by size. `storage` holds the tables (see
// wl_heap_sites_init); blocks made before the call are not counted. WL_ERR_STATE if already on.
WlResult wl_heap_overflow_track_sites(void *storage, size_t bytes, uint32_t sites,
                                      size_t min_tracked);
// The accounting's totals and the `count` sites holding the most, one INFO line each; nothing
// if it is off.
void wl_heap_overflow_log_sites(size_t count);
// Its totals (all zero if it is off).
void wl_heap_overflow_site_totals(WlHeapSitesTotals *out);
