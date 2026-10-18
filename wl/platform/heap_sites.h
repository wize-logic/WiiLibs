// SPDX-License-Identifier: MIT
// Who holds the heap: an accounting of an allocator's live blocks by the code that asked for them
// (two return addresses, for addr2line on the program's ELF) and, for the small ones, by size.
//
// A program that runs out of memory needs to know who has it. The allocator itself only knows
// sizes; this records, for each live block of at least `min_tracked` bytes, the call site that
// made it, and keeps per site the bytes and blocks live now, the peak and the allocations ever
// made. Blocks under `min_tracked` are only counted, by size class, so that the many small ones
// (strings, list nodes) cost no table room. The report lists the sites holding the most.
//
// It is fed by an allocator's wrapper (on the Wii, heap_wrap_wii.c, once
// wl_heap_overflow_track_sites is called) with the block's usable size at both ends, so that a
// block is taken off exactly what it was counted for. The storage is the caller's: one block cut
// into the site table and an open-addressed table of live blocks (backward-shift deletion: no
// tombstones, so a long session does not slow it down). A block that finds the table full is
// counted as untracked; its free is then not found and changes nothing (the counts of the site
// stay as they were, so `untracked` says how far to trust them). A small block is only counted,
// so its free is taken off its class whether its allocation was seen or not (a class stops at 0).
//
// Thread safety: every entry point takes wl_platform_mem_lock.
#pragma once

#include "wl/base/base.h"

// The size classes of the small blocks: under 64, 256 and 1024 bytes, then up to `min_tracked`.
#define WL_HEAP_SITES_CLASSES 4
// The sites kept at the peak of the bytes live, and how far the peak moves before they are kept again.
#define WL_HEAP_SITES_AT_PEAK 12
#define WL_HEAP_SITES_PEAK_STEP (64u * 1024u)

typedef struct {
    uintptr_t caller[2]; // the return addresses: into the allocator's caller, then into its caller
    size_t bytes;        // usable bytes live now
    size_t peak_bytes;
    uint32_t blocks; // live now
    uint32_t allocs; // ever
} WlHeapSite;

typedef struct {
    uintptr_t ptr; // 0: empty
    uint32_t size;
    uint16_t site;
    uint16_t reserved;
} WlHeapSiteBlock;

typedef struct {
    WlHeapSite *sites;
    uint32_t site_capacity, site_count;
    WlHeapSiteBlock *blocks;
    uint32_t block_capacity; // a power of two
    uint32_t block_count;
    size_t min_tracked;
    bool ready;
    // the small blocks, by class (WL_HEAP_SITES_CLASSES)
    size_t small_bytes[WL_HEAP_SITES_CLASSES];
    uint32_t small_blocks[WL_HEAP_SITES_CLASSES];
    // blocks of at least min_tracked that had no room: in the site table or the block table
    uint32_t untracked;
    size_t untracked_bytes;
    // the bytes live in all (every block seen made and not yet seen freed) and, at their peak,
    // the sites holding the most then (kept again each time the peak passes the last one kept by
    // WL_HEAP_SITES_PEAK_STEP: within that of the true peak)
    size_t live;
    size_t peak_live; // live when at_peak was kept
    uint32_t at_peak_count;
    WlHeapSite at_peak[WL_HEAP_SITES_AT_PEAK];
} WlHeapSites;

// Bytes of storage for `sites` call sites and `blocks` live blocks.
size_t wl_heap_sites_storage_bytes(uint32_t sites, uint32_t blocks);

// Lays the tables out in `storage` (aligned to 8): as many sites as `sites` asks (at least 1),
// and the largest power of two of live blocks the rest holds (at least 16, else WL_ERR_ARG).
// Blocks of fewer than `min_tracked` bytes are counted by class only.
WlResult wl_heap_sites_init(WlHeapSites *s, void *storage, size_t storage_bytes, uint32_t sites,
                            size_t min_tracked);

// A block of `size` usable bytes at `ptr`, asked for from caller0 (whose caller is caller1, or 0).
void wl_heap_sites_alloc(WlHeapSites *s, const void *ptr, size_t size, uintptr_t caller0,
                         uintptr_t caller1);
// The block at `ptr`, of `size` usable bytes (the same as at wl_heap_sites_alloc), freed.
void wl_heap_sites_free(WlHeapSites *s, const void *ptr, size_t size);

// Copies up to `count` sites, those holding the most bytes first; returns how many.
size_t wl_heap_sites_top(WlHeapSites *s, WlHeapSite *out, size_t count);

typedef struct {
    size_t tracked_bytes; // in the tracked blocks
    uint32_t tracked_blocks;
    uint32_t sites;
    size_t small_bytes[WL_HEAP_SITES_CLASSES];
    uint32_t small_blocks[WL_HEAP_SITES_CLASSES];
    uint32_t untracked;
    size_t untracked_bytes;
} WlHeapSitesTotals;

void wl_heap_sites_totals(WlHeapSites *s, WlHeapSitesTotals *out);

// The totals and the `count` sites holding the most, one INFO line each (tag "heap"), then the
// bytes live at the peak and the sites that held the most then.
void wl_heap_sites_log(WlHeapSites *s, size_t count);
