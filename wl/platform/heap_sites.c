// SPDX-License-Identifier: MIT
// The heap's accounting by call site (see heap_sites.h).
#include "wl/platform/heap_sites.h"

#include <string.h>

#include "wl/platform/mem.h"

#define LOG_TAG "heap"
#define MIN_BLOCKS 16u
#define NO_SITE 0xFFFFu
#define MAX_SITES 0xFFFEu
// the most sites the report copies out at once
#define LOG_MAX 32u

static const size_t k_class_limits[WL_HEAP_SITES_CLASSES - 1] = {64u, 256u, 1024u};

size_t wl_heap_sites_storage_bytes(uint32_t sites, uint32_t blocks) {
    return (size_t)sites * sizeof(WlHeapSite) + (size_t)blocks * sizeof(WlHeapSiteBlock) + 8u;
}

WlResult wl_heap_sites_init(WlHeapSites *s, void *storage, size_t storage_bytes, uint32_t sites,
                            size_t min_tracked) {
    if (s == NULL || storage == NULL) {
        return WL_ERR_ARG;
    }
    memset(s, 0, sizeof *s);
    if (sites == 0u) {
        sites = 1u;
    }
    if (sites > MAX_SITES) {
        sites = MAX_SITES;
    }
    uintptr_t start = ((uintptr_t)storage + 7u) & ~(uintptr_t)7u;
    uintptr_t end = (uintptr_t)storage + storage_bytes;
    size_t site_bytes = (size_t)sites * sizeof(WlHeapSite);
    if (end < start || end - start < site_bytes) {
        return WL_ERR_ARG;
    }
    size_t room = (end - start - site_bytes) / sizeof(WlHeapSiteBlock);
    uint32_t capacity = MIN_BLOCKS;
    if (room < MIN_BLOCKS) {
        return WL_ERR_ARG;
    }
    while ((size_t)capacity * 2u <= room && capacity < 0x40000000u) {
        capacity *= 2u;
    }
    s->sites = (WlHeapSite *)start;
    s->site_capacity = sites;
    s->blocks = (WlHeapSiteBlock *)(start + site_bytes);
    s->block_capacity = capacity;
    s->min_tracked = min_tracked;
    memset(s->sites, 0, site_bytes);
    memset(s->blocks, 0, (size_t)capacity * sizeof(WlHeapSiteBlock));
    s->ready = true;
    return WL_OK;
}

static uint32_t slot_of(const WlHeapSites *s, uintptr_t ptr) {
    // (blocks are at least 8-aligned: the low bits say nothing)
    uint32_t h = (uint32_t)(ptr >> 3) * 2654435761u;
    return h & (s->block_capacity - 1u);
}

static unsigned class_of(size_t size) {
    unsigned c = 0;
    while (c < WL_HEAP_SITES_CLASSES - 1u && size >= k_class_limits[c]) {
        c++;
    }
    return c;
}

// The site of these callers, made if new; NO_SITE when the table is full.
static uint16_t site_of(WlHeapSites *s, uintptr_t caller0, uintptr_t caller1) {
    uint32_t i;
    for (i = 0; i < s->site_count; i++) {
        if (s->sites[i].caller[0] == caller0 && s->sites[i].caller[1] == caller1) {
            return (uint16_t)i;
        }
    }
    if (s->site_count == s->site_capacity) {
        return NO_SITE;
    }
    WlHeapSite *site = &s->sites[s->site_count];
    memset(site, 0, sizeof *site);
    site->caller[0] = caller0;
    site->caller[1] = caller1;
    return (uint16_t)s->site_count++;
}

void wl_heap_sites_alloc(WlHeapSites *s, const void *ptr, size_t size, uintptr_t caller0,
                         uintptr_t caller1) {
    if (s == NULL || ptr == NULL) {
        return;
    }
    wl_platform_mem_lock();
    if (!s->ready) {
        wl_platform_mem_unlock();
        return;
    }
    if (size < s->min_tracked) {
        unsigned c = class_of(size);
        s->small_bytes[c] += size;
        s->small_blocks[c]++;
        wl_platform_mem_unlock();
        return;
    }
    // (the table kept under seven eighths full, so that the runs stay short)
    uint16_t site = s->block_count < s->block_capacity - s->block_capacity / 8u
                        ? site_of(s, caller0, caller1)
                        : NO_SITE;
    if (site == NO_SITE || size > 0xFFFFFFFFu) {
        s->untracked++;
        s->untracked_bytes += size;
        wl_platform_mem_unlock();
        return;
    }
    uint32_t slot = slot_of(s, (uintptr_t)ptr);
    while (s->blocks[slot].ptr != 0u && s->blocks[slot].ptr != (uintptr_t)ptr) {
        slot = (slot + 1u) & (s->block_capacity - 1u);
    }
    if (s->blocks[slot].ptr == (uintptr_t)ptr) {
        // (a block recorded as live, given out again: its free was not seen; it is replaced)
        WlHeapSite *old = &s->sites[s->blocks[slot].site];
        old->bytes -= s->blocks[slot].size;
        old->blocks--;
    } else {
        s->block_count++;
    }
    s->blocks[slot].ptr = (uintptr_t)ptr;
    s->blocks[slot].size = (uint32_t)size;
    s->blocks[slot].site = site;
    WlHeapSite *record = &s->sites[site];
    record->bytes += size;
    record->blocks++;
    record->allocs++;
    if (record->bytes > record->peak_bytes) {
        record->peak_bytes = record->bytes;
    }
    wl_platform_mem_unlock();
}

void wl_heap_sites_free(WlHeapSites *s, const void *ptr, size_t size) {
    if (s == NULL || ptr == NULL) {
        return;
    }
    wl_platform_mem_lock();
    if (!s->ready) {
        wl_platform_mem_unlock();
        return;
    }
    if (size < s->min_tracked) {
        unsigned c = class_of(size);
        s->small_bytes[c] -= size <= s->small_bytes[c] ? size : s->small_bytes[c];
        s->small_blocks[c] -= s->small_blocks[c] != 0u;
        wl_platform_mem_unlock();
        return;
    }
    uint32_t mask = s->block_capacity - 1u;
    uint32_t slot = slot_of(s, (uintptr_t)ptr);
    while (s->blocks[slot].ptr != 0u && s->blocks[slot].ptr != (uintptr_t)ptr) {
        slot = (slot + 1u) & mask;
    }
    if (s->blocks[slot].ptr == 0u) {
        // (one that had no room, or was made before the accounting began)
        wl_platform_mem_unlock();
        return;
    }
    WlHeapSite *record = &s->sites[s->blocks[slot].site];
    record->bytes -= s->blocks[slot].size;
    record->blocks--;
    s->block_count--;
    // backward-shift deletion: each following entry of the run moves into the hole if its home
    // slot is not between the hole and it
    uint32_t hole = slot;
    uint32_t next = (hole + 1u) & mask;
    while (s->blocks[next].ptr != 0u) {
        uint32_t home = slot_of(s, s->blocks[next].ptr);
        bool stays = hole <= next ? (home > hole && home <= next) : (home > hole || home <= next);
        if (!stays) {
            s->blocks[hole] = s->blocks[next];
            hole = next;
        }
        next = (next + 1u) & mask;
    }
    memset(&s->blocks[hole], 0, sizeof s->blocks[hole]);
    wl_platform_mem_unlock();
}

size_t wl_heap_sites_top(WlHeapSites *s, WlHeapSite *out, size_t count) {
    size_t n = 0;
    if (s == NULL || out == NULL) {
        return 0;
    }
    wl_platform_mem_lock();
    if (s->ready) {
        uint32_t i;
        for (i = 0; i < s->site_count; i++) {
            const WlHeapSite *site = &s->sites[i];
            size_t at;
            if (site->bytes == 0u && site->blocks == 0u) {
                continue;
            }
            // insertion into the sorted list (most bytes first), the last dropped when full
            if (n == count) {
                if (count == 0u || out[count - 1u].bytes >= site->bytes) {
                    continue;
                }
                n--;
            }
            at = n;
            while (at > 0u && out[at - 1u].bytes < site->bytes) {
                out[at] = out[at - 1u];
                at--;
            }
            out[at] = *site;
            n++;
        }
    }
    wl_platform_mem_unlock();
    return n;
}

void wl_heap_sites_totals(WlHeapSites *s, WlHeapSitesTotals *out) {
    memset(out, 0, sizeof *out);
    if (s == NULL) {
        return;
    }
    wl_platform_mem_lock();
    if (s->ready) {
        uint32_t i;
        for (i = 0; i < s->site_count; i++) {
            out->tracked_bytes += s->sites[i].bytes;
        }
        out->tracked_blocks = s->block_count;
        out->sites = s->site_count;
        memcpy(out->small_bytes, s->small_bytes, sizeof out->small_bytes);
        memcpy(out->small_blocks, s->small_blocks, sizeof out->small_blocks);
        out->untracked = s->untracked;
        out->untracked_bytes = s->untracked_bytes;
    }
    wl_platform_mem_unlock();
}

void wl_heap_sites_log(WlHeapSites *s, size_t count) {
    WlHeapSite top[LOG_MAX];
    WlHeapSitesTotals totals;
    size_t n, i;
    if (s == NULL || !s->ready) {
        return;
    }
    // (copied out first: logging may allocate, and so come back here)
    wl_heap_sites_totals(s, &totals);
    n = wl_heap_sites_top(s, top, count < LOG_MAX ? count : LOG_MAX);
    WL_LOG_I(LOG_TAG,
             "by call site: %u KiB in %u blocks of %u bytes or more, from %u sites; %u blocks "
             "(%u KiB) without room to be tracked",
             (unsigned)(totals.tracked_bytes / 1024u), (unsigned)totals.tracked_blocks,
             (unsigned)s->min_tracked, (unsigned)totals.sites, (unsigned)totals.untracked,
             (unsigned)(totals.untracked_bytes / 1024u));
    WL_LOG_I(LOG_TAG,
             "smaller blocks: under 64 bytes %u KiB (%u), under 256 %u KiB (%u), under 1024 %u "
             "KiB (%u), the rest %u KiB (%u)",
             (unsigned)(totals.small_bytes[0] / 1024u), (unsigned)totals.small_blocks[0],
             (unsigned)(totals.small_bytes[1] / 1024u), (unsigned)totals.small_blocks[1],
             (unsigned)(totals.small_bytes[2] / 1024u), (unsigned)totals.small_blocks[2],
             (unsigned)(totals.small_bytes[3] / 1024u), (unsigned)totals.small_blocks[3]);
    for (i = 0; i < n; i++) {
        WL_LOG_I(LOG_TAG, "%6u KiB in %4u blocks (peak %6u KiB, %6u made) from %08lx %08lx",
                 (unsigned)(top[i].bytes / 1024u), (unsigned)top[i].blocks,
                 (unsigned)(top[i].peak_bytes / 1024u), (unsigned)top[i].allocs,
                 (unsigned long)top[i].caller[0], (unsigned long)top[i].caller[1]);
    }
}
