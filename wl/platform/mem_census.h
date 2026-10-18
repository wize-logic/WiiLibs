// SPDX-License-Identifier: MIT
// Who holds the host's heap: a census of a program's subsystems, cheap enough to keep on.
//
// The program's heap is malloc's (newlib's on the Wii, over libogc's sbrk; the C library's on the
// host). A subsystem that holds a lasting part of it registers a reporter, a function the census
// calls when it is taken: the bytes it holds now on that heap (and, if it likes, a short note such
// as a count or a high-water mark). Nothing is counted on the allocation paths: the census asks.
// Taking it reads the heap's own figures (wl_platform_heap_info) beside the reporters', and what
// no reporter claims is "other": libraries, small blocks, the allocator's overhead.
//
// The heap's figures need care on the Wii: newlib's mallinfo counts only what it has had from
// sbrk, and it gives memory back to the arena whenever more than 128 KB lies free at its top, so
// its free figure (fordblks) stays small however much the arena still holds. The free memory is
// that plus what is left above sbrk's top (WlPlatformHeapInfo.top): one contiguous piece, the
// largest allocation the heap can still make.
//
// Thread safety: the registry is filled at start-up (before other threads take the census) and
// read by one thread at a time; reporters are called on the thread taking the census.
#pragma once

#include "wl/base/base.h"

// The heap's figures, from the platform (wii/mem_wii.c, host/heap_host.c).
typedef struct {
    bool known;         // false: the platform cannot say (all zero)
    size_t arena;       // bytes the allocator has taken from the system
    size_t in_use;      // of those, in live blocks (headers included)
    size_t free_inside; // of those, in free blocks
    // Bytes the allocator can still take from the system, contiguous above its top (the Wii: the
    // arena above sbrk's top). With a cap (the host's wl_platform_heap_cap): what the cap leaves.
    // SIZE_MAX: no limit the program can see (the host without a cap).
    size_t top;
    // The Wii while sbrk is still in MEM1: MEM2's arena, where it goes on when MEM1 is full (a piece
    // of its own: an allocation fits in the larger of `top` and this)
    size_t elsewhere;
    size_t free_total; // free_inside + top + elsewhere (SIZE_MAX when top is)
    // The Wii once sbrk has moved on to MEM2: the rest of MEM1's arena it left behind, which malloc
    // can no longer reach (libogc's sbrk does not go back while MEM2 holds anything). Neither in
    // use nor free: newlib counts it as in use, and in_use has it taken back out.
    size_t stranded;
    // the host's cap (0: none): the limit on what was allocated since it was set, and the requests
    // it refused
    size_t cap, cap_used, cap_peak;
    uint32_t cap_refused;
    size_t cap_largest_refused;
    // The high-water marks since wl_platform_heap_mark (else the program's start), kept as they
    // happen rather than sampled: what a census taken every few seconds misses is in them. No
    // cost but a compare where the allocator already counts: the host's malloc (here) on each
    // allocation, newlib's own sbrk high-water on the Wii (it keeps one; the mark resets it).
    bool peak_known;
    // The most the heap grew past what it held at the mark. The host: the live blocks' bytes (the
    // cap's exempt blocks left out). The Wii: newlib's arena (sbrk's reach), free blocks inside
    // it included: the most of the memory above the mark's top the heap ever took, which is what
    // a piece kept free for it (boot.c's keep_free_bytes) must hold.
    size_t peak;
    // The least `top` since the mark: peak's other side (SIZE_MAX where top is; the Wii before a mark)
    size_t low_top;
} WlPlatformHeapInfo;

void wl_platform_heap_info(WlPlatformHeapInfo *out);
// Starts the high-water marks (peak, low_top) again from the heap as it is now: after a program
// has taken its large blocks for good (xboxrt's boot: the guest's frames, the textures' arena), so
// that the marks measure what comes after. One thread, with no other allocating meanwhile.
void wl_platform_heap_mark(void);

// Host only (a no-op returning WL_ERR_UNSUPPORTED elsewhere): from now on, malloc and its relatives
// refuse a request (NULL, errno ENOMEM) that would take the bytes allocated since this call (less
// those freed) beyond `bytes`; 0 lifts the cap. To imitate the Wii's heap on the host, for a test
// of what runs out first. Only the byte count is imitated, not newlib's fragmentation. On a host
// whose C library cannot be interposed (not glibc) it is WL_ERR_UNSUPPORTED.
WlResult wl_platform_heap_cap(size_t bytes);
// Host only (nothing elsewhere): while on (it nests, per thread), the blocks the calling thread
// allocates are not counted against the cap, nor is their free, wherever it happens: for the
// host's own stand-ins that the Wii has not (the software GX behind the host's GX stubs, the
// recorders), so that the cap imitates what the Wii's heap holds.
void wl_platform_heap_exempt(bool on);

// Host only (WL_ERR_UNSUPPORTED elsewhere, and where wl_platform_heap_cap is): the cap grows by
// `bytes`, its counts kept (memory a reclaimer gave the heap back, below, as the Wii's would).
WlResult wl_platform_heap_cap_add(size_t bytes);

// Host only (glibc, not a static link; WL_ERR_UNSUPPORTED elsewhere): who holds the heap from now
// on, by call site (heap_sites.h: the two callers above malloc, as offsets into the program for
// addr2line), in the caller's `storage`; blocks under `min_tracked` bytes counted by size only.
// The report (wl_platform_heap_log_sites) gives the sites holding the most now and at the peak.
WlResult wl_platform_heap_track_sites(void *storage, size_t bytes, uint32_t sites, size_t min_tracked);
void wl_platform_heap_log_sites(size_t count);

// ---- giving memory back on demand ------------------------------------------------------------
// A program that keeps memory of the heap's for itself while it is idle (xboxrt's guest frames
// lent by the heap) registers a reclaimer: when an allocation would fail (newlib's heap has no
// room on the Wii; the host's cap would be passed), the allocator calls it with the bytes wanted,
// and tries again while it gives something. It frees what it can (whole blocks or a block's tail)
// and returns the bytes it gave back, 0 when it has nothing more or cannot now. One call at a time:
// a failure while one runs (its own allocations, another thread's) is not reclaimed for.
// The allocators that call it: the host's (glibc, not a static link: heap_host.c) and the Wii's with
// the wrap linked in (wl_platform_heap_overflow in CMake: heap_wrap_wii.c).
typedef size_t (*WlHeapReclaimFn)(void *user, size_t want);
// NULL removes it. Returns whether this program's allocator calls it.
bool wl_platform_heap_set_reclaim(WlHeapReclaimFn fn, void *user);
// Whether this program's allocator calls a reclaimer (the platform's answer).
bool wl_platform_heap_reclaims(void);
// For the allocators: the reclaimer's answer for `want` bytes, 0 without one or while it runs.
size_t wl_platform_heap_reclaim(size_t want);
typedef struct {
    uint32_t calls;    // times an allocation asked
    uint32_t gave;     // of those, times it gave something
    uint32_t busy;     // asked while it ran (not called)
    size_t bytes;      // given back in all
    size_t largest_want;
} WlHeapReclaimStats;
void wl_platform_heap_reclaim_stats(WlHeapReclaimStats *out);

// ---- the registry ---------------------------------------------------------------------------

#define WL_MEM_CENSUS_MAX 24u
#define WL_MEM_CENSUS_NOTE 48u

typedef struct {
    size_t bytes;                 // held on the heap now
    char note[WL_MEM_CENSUS_NOTE]; // optional ("" for none): counts, high-water marks
} WlMemCensusReport;

// Fills `out` (zeroed before the call).
typedef void (*WlMemCensusFn)(void *user, WlMemCensusReport *out);

// Registers a reporter under `name` (a static string, short: it is printed in a list). A name
// already there is replaced (a subsystem that starts again). WL_ERR_LIMIT past WL_MEM_CENSUS_MAX.
WlResult wl_mem_census_add(const char *name, WlMemCensusFn fn, void *user);
// Removes the reporter of that name (nothing if there is none).
void wl_mem_census_remove(const char *name);

typedef struct {
    const char *name;
    WlMemCensusReport report;
} WlMemCensusRow;

typedef struct {
    WlPlatformHeapInfo heap;
    WlMemCensusRow rows[WL_MEM_CENSUS_MAX];
    uint32_t count;
    size_t claimed; // the rows' bytes
    size_t other;   // heap.in_use - claimed (0 when the reporters claim more, or the heap is unknown)
} WlMemCensus;

void wl_mem_census_take(WlMemCensus *out);

// The census through the log (INFO, `tag`): the heap's line, then the reporters (largest first)
// over as many lines as they need, each under 200 characters (Dolphin cuts a report at about 250).
// `heap_only`: the heap's line alone.
void wl_mem_census_log(const char *tag, bool heap_only);
// The heap's line into `buf` ("heap: ..."), for a status line of the caller's.
void wl_mem_census_heap_text(const WlPlatformHeapInfo *heap, char *buf, size_t size);
