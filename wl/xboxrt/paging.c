// SPDX-License-Identifier: MIT
// Demand paging of guest memory (mem_internal.h): the pool's frames, the clock that takes them
// back and the swap file. Portable: the Wii's MMU backend (mem_wii.c) and the host's simulation of
// it (mem_posix.c with WlXbMemConfig.frame_limit_bytes) run this same code.
//
// A physical page is
//   - zero: no frame and no slot (never touched, freed, or its frame taken back while it was
//     zero); a touch gives it a zeroed frame;
//   - resident: a frame, mapped or not; DIRTY once it changed since its zero fill or its read back
//     (the MMU's C bits, harvested), REF when used since the clock last passed it (R bits);
//   - swapped: no frame, a slot: its bytes are in the swap file, and a touch reads them back.
// A resident page keeps its slot while it stays clean, so its frame goes back for nothing; a
// dirty one gets a new slot when it is written out.
//
// The clock goes round the frames. A pinned page, or one in I/O, is passed by. The page's R bits
// are harvested and cleared, and a page used since the last round is passed too (its second
// chance). A clean page gives its frame back at once. A dirty one is left: in the slow path the
// dirty pages passed are gathered, and when a look finds no clean page they are written in one
// write to consecutive slots and their frames taken (a batch: the SD card is slow at small
// writes). The fault's exception context (the Wii's DSI) only looks a bounded way ahead and never
// writes or reads; a touch that needs I/O goes to the slow path, in normal context with the guest
// lock held.
//
// The write watch (mem.h, wl_xb_phys_watch) rides on the same C bits: each page has a write epoch,
// set from a global count whenever a store to it is seen (a C bit harvested here or by the backend,
// the host's write fault) or its contents change otherwise (freed: zero; moved). Arming a watch
// harvests and clears the pages' C bits and takes the count as its stamp; a later look harvests
// and clears them again and compares the epochs with the stamp. A watched page's PTE never carries
// C ahead of a store (WL_XB_PG_WATCH), and a swap-out and read back keep its bytes and its epoch.
//
// Locking: the tables change with the guest lock held, and on the Wii inside wl_xb_mmu_lock
// (interrupts off) for each step, so a DSI on another thread (the service thread's pinned
// KeTickCount, a PTE reload) sees them whole; the I/O runs outside, its pages marked IO.
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/mem_internal.h"

#define TAG "xbpage"
#define BATCH 32u      // pages a swap write takes at most (128 KB)
#define FAST_LOOK 256u // frames the exception context's clock looks at before it gives up
#define SLOT_ALIGN 8u  // a batch starts on 8 slots (32 KB) when it can
#define PAGE WL_XB_PAGE

uint8_t *g_xb_pg_flags;
uint16_t *g_xb_pg_frame;
uint16_t *g_xb_pg_slot;
uint32_t g_xb_pg_frames;
uint32_t *g_xb_pg_wgen;
uint32_t g_xb_pg_epoch = 1;

static uint16_t *g_owner;  // [frames + 1] the page holding each frame, + 1 (0: free)
static uint16_t *g_free;   // a stack of free frames
static uint32_t g_free_n;
static uint32_t g_hand = 1; // the clock: the next frame it looks at
static uint32_t g_pinned;

static WlPlatformFile *g_swap;
static uint32_t *g_slot_used; // a bit per slot
static uint32_t g_slots, g_slots_used, g_slot_cursor;
static uint8_t *g_io;         // BATCH pages: the batch's bytes, a page read back
static uint16_t g_batch[BATCH];
static uint32_t g_batch_n;

static struct {
    uint32_t evictions, clean_evictions, fast_evictions;
    uint32_t swap_ins, swap_outs, swap_writes;
    uint64_t read_bytes, write_bytes, io_us;
} g_st;

// ---- Frames (exception-safe) --------------------------------------------------------------------

static uint32_t take_free(void) {
    return g_free_n != 0 ? g_free[--g_free_n] : 0u;
}

static void give_free(uint32_t f) {
    g_owner[f] = 0;
    g_free[g_free_n++] = (uint16_t)f;
}

static void assign(uint32_t f, uint32_t p) {
    g_owner[f] = (uint16_t)(p + 1u);
    g_xb_pg_frame[p] = (uint16_t)f;
}

// A clean page `q` not used lately gives frame `f` back (q is unmapped).
static void evict_clean(uint32_t q, uint32_t f) {
    g_xb_pg_frame[q] = 0;
    g_owner[f] = 0;
    g_xb_pg_flags[q] &= (uint8_t)~WL_XB_PG_REF;
    wl_xb_mmu_frame_drop(q, f);
    g_st.evictions++;
    g_st.clean_evictions++;
}

// One step of the clock, at frame g_hand. Returns the frame a clean page gave back, or 0. With
// `collect`, an unused dirty page is unmapped, marked IO and added to g_batch (while it has room).
static uint32_t clock_step(uint32_t keep, bool collect) {
    uint32_t f = g_hand;
    g_hand = f >= g_xb_pg_frames ? 1u : f + 1u;
    uint32_t q1 = g_owner[f];
    if (q1 == 0 || q1 == keep) {
        return 0;
    }
    uint32_t q = q1 - 1u;
    if ((g_xb_pg_flags[q] & (WL_XB_PG_PINNED | WL_XB_PG_IO)) != 0) {
        return 0;
    }
    wl_xb_mmu_harvest(q, true);
    uint8_t fl = g_xb_pg_flags[q];
    if ((fl & WL_XB_PG_REF) != 0) {
        g_xb_pg_flags[q] = (uint8_t)(fl & ~WL_XB_PG_REF); // its second chance
        return 0;
    }
    if ((fl & WL_XB_PG_DIRTY) != 0) {
        if (collect && g_batch_n < BATCH) {
            wl_xb_mmu_unmap(q);
            g_xb_pg_flags[q] |= WL_XB_PG_IO;
            g_batch[g_batch_n++] = (uint16_t)q;
        }
        return 0;
    }
    wl_xb_mmu_unmap(q);
    if ((g_xb_pg_flags[q] & WL_XB_PG_DIRTY) != 0) {
        return 0; // changed after all (the unmap harvested a C bit): left for a write
    }
    evict_clean(q, f);
    return f;
}

uint32_t wl_xb_pg_frame_fast(uint32_t p, uint32_t keep) {
    uint32_t f = take_free();
    for (uint32_t n = 0; f == 0 && n < FAST_LOOK; n++) {
        f = clock_step(keep, false);
        if (f != 0) {
            g_st.fast_evictions++;
        }
    }
    if (f != 0) {
        assign(f, p);
    }
    return f;
}

// ---- The swap file ------------------------------------------------------------------------------

static bool slot_is_used(uint32_t s) {
    return (g_slot_used[s >> 5] >> (s & 31u)) & 1u;
}

static void slot_free(uint32_t s) {
    g_slot_used[s >> 5] &= ~(1u << (s & 31u));
    g_slots_used--;
}

// Takes the free slots from `s` on, at most `want`; how many.
static uint32_t slot_take(uint32_t s, uint32_t want) {
    uint32_t len = 0;
    while (len < want && s + len < g_slots && !slot_is_used(s + len)) {
        g_slot_used[(s + len) >> 5] |= 1u << ((s + len) & 31u);
        len++;
    }
    g_slots_used += len;
    g_slot_cursor = s + len >= g_slots ? 0 : s + len;
    return len;
}

// A run of free slots, at most `want` long, taken: its length (0: the file is full), its first slot
// in `*first`. From the cursor on, wrapping once; first at a free group of SLOT_ALIGN slots (32 KB:
// an SD card's FAT cluster, so that a batch's write goes to the card whole rather than through the
// file system's cache), else at the first free slot.
static uint32_t slot_run(uint32_t want, uint32_t *first) {
    uint32_t groups = g_slots / SLOT_ALIGN;
    uint32_t need = want < SLOT_ALIGN ? want : SLOT_ALIGN;
    for (uint32_t n = 0, s = g_slot_cursor & ~(SLOT_ALIGN - 1u); n < groups; n++, s += SLOT_ALIGN) {
        if (s >= groups * SLOT_ALIGN) {
            s = 0;
        }
        uint32_t k = 0;
        while (k < need && !slot_is_used(s + k)) {
            k++;
        }
        if (k == need) {
            *first = s;
            return slot_take(s, want);
        }
    }
    for (uint32_t scanned = 0, s = g_slot_cursor; scanned < g_slots; scanned++, s = s + 1u >= g_slots ? 0 : s + 1u) {
        if ((s & 31u) == 0 && g_slot_used[s >> 5] == 0xFFFFFFFFu && scanned + 32u <= g_slots) {
            scanned += 31u; // a full word
            s += 31u;
            continue;
        }
        if (slot_is_used(s)) {
            continue;
        }
        *first = s;
        return slot_take(s, want);
    }
    return 0;
}

// The gathered pages from `from` on stay resident (dirty, unmapped: they fault back in cheaply).
static void batch_abandon(uint32_t from) {
    uint32_t lv = wl_xb_mmu_lock();
    for (uint32_t i = from; i < g_batch_n; i++) {
        g_xb_pg_flags[g_batch[i]] &= (uint8_t)~WL_XB_PG_IO;
    }
    g_batch_n = from;
    wl_xb_mmu_unlock(lv);
}

// Writes the gathered dirty pages (unmapped, marked IO) to consecutive slots and takes their
// frames. Lock not held.
static WlResult batch_write(void) {
    uint32_t n = g_batch_n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = g_batch[i];
        memcpy(g_io + i * PAGE, wl_xb_mmu_frame_ptr(p, g_xb_pg_frame[p]), PAGE);
        if (g_xb_pg_slot[p] != 0) {
            uint32_t lv = wl_xb_mmu_lock();
            slot_free(g_xb_pg_slot[p] - 1u); // its old copy is stale
            g_xb_pg_slot[p] = 0;
            wl_xb_mmu_unlock(lv);
        }
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t first = 0;
        uint32_t lv = wl_xb_mmu_lock();
        uint32_t len = slot_run(n - done, &first);
        for (uint32_t k = 0; k < len; k++) {
            g_xb_pg_slot[g_batch[done + k]] = (uint16_t)(first + k + 1u);
        }
        wl_xb_mmu_unlock(lv);
        if (len == 0) {
            WL_LOG_E(TAG, "the swap file is full (%u pages)", (unsigned)g_slots);
            batch_abandon(done); // what was written goes; the rest stays resident
            n = done;
            break;
        }
        uint64_t t0 = wl_platform_time_us();
        WlResult r = wl_platform_fs_write_at(g_swap, (uint64_t)first * PAGE, g_io + done * PAGE, len * PAGE);
        g_st.io_us += wl_platform_time_us() - t0;
        if (r != WL_OK) {
            WL_LOG_E(TAG, "swap file write of %u pages at slot %u: %s", (unsigned)len, (unsigned)first,
                     wl_result_name(r));
            return WL_ERR_IO;
        }
        g_st.swap_writes++;
        g_st.write_bytes += (uint64_t)len * PAGE;
        done += len;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = g_batch[i];
        uint32_t lv = wl_xb_mmu_lock();
        uint32_t f = g_xb_pg_frame[p];
        g_xb_pg_frame[p] = 0;
        g_xb_pg_flags[p] &= (uint8_t)~(WL_XB_PG_IO | WL_XB_PG_DIRTY | WL_XB_PG_REF);
        give_free(f);
        wl_xb_mmu_frame_drop(p, f);
        wl_xb_mmu_unlock(lv);
    }
    g_st.swap_outs += n;
    g_st.evictions += n;
    g_batch_n = 0;
    return n != 0 ? WL_OK : WL_ERR_NOMEM;
}

// A frame for the slow path: a free one, a clean page's, or a batch written out. Lock not held.
static WlResult frame_slow(uint32_t keep, uint32_t *out) {
    uint32_t lv = wl_xb_mmu_lock();
    uint32_t f = take_free();
    wl_xb_mmu_unlock(lv);
    bool collect = g_swap != NULL;
    // two rounds clear every R bit and come back to the pages not used since
    for (uint32_t n = 0; f == 0 && n < 2u * g_xb_pg_frames + 2u; n++) {
        lv = wl_xb_mmu_lock();
        f = clock_step(keep, collect);
        wl_xb_mmu_unlock(lv);
        if (g_batch_n == BATCH) {
            break;
        }
    }
    if (f != 0) {
        batch_abandon(0); // a clean page was had: nothing written
    } else if (g_batch_n != 0) {
        WlResult r = batch_write();
        if (r == WL_ERR_IO) {
            return r;
        }
        lv = wl_xb_mmu_lock();
        f = take_free();
        wl_xb_mmu_unlock(lv);
    }
    *out = f;
    return f != 0 ? WL_OK : WL_ERR_NOMEM;
}

WlResult wl_xb_pg_fault(uint32_t p, uint32_t keep) {
    if (g_xb_pg_frames == 0 || p >= g_xb_ram_pages) {
        return WL_ERR_STATE;
    }
    if ((g_xb_pg_flags[p] & WL_XB_PG_IO) != 0) {
        return WL_ERR_STATE;
    }
    if (g_xb_pg_frame[p] != 0) {
        return WL_OK;
    }
    uint32_t f = 0;
    WlResult r = frame_slow(keep, &f);
    if (r != WL_OK) {
        return r;
    }
    uint32_t lv = wl_xb_mmu_lock();
    assign(f, p);
    uint32_t slot = g_xb_pg_slot[p];
    if (slot != 0) {
        g_xb_pg_flags[p] |= WL_XB_PG_IO;
    }
    wl_xb_mmu_unlock(lv);
    if (slot == 0) {
        wl_xb_mmu_frame_zero(p, f);
    } else {
        uint64_t t0 = wl_platform_time_us();
        r = wl_platform_fs_read_at(g_swap, (uint64_t)(slot - 1u) * PAGE, g_io, PAGE);
        g_st.io_us += wl_platform_time_us() - t0;
        if (r != WL_OK) {
            WL_LOG_E(TAG, "swap file read of slot %u: %s", (unsigned)(slot - 1u), wl_result_name(r));
            return WL_ERR_IO;
        }
        memcpy(wl_xb_mmu_frame_ptr(p, f), g_io, PAGE);
        g_st.swap_ins++;
        g_st.read_bytes += PAGE;
    }
    lv = wl_xb_mmu_lock();
    g_xb_pg_flags[p] = (uint8_t)((g_xb_pg_flags[p] & ~(WL_XB_PG_IO | WL_XB_PG_DIRTY)) | WL_XB_PG_REF);
    wl_xb_mmu_unlock(lv);
    return WL_OK;
}

// ---- Bookkeeping ---------------------------------------------------------------------------------

void wl_xb_pg_release(uint32_t p, uint32_t pages) {
    if (g_xb_pg_frames == 0) {
        return;
    }
    for (uint32_t k = p; k < p + pages && k < g_xb_ram_pages; k++) {
        uint32_t lv = wl_xb_mmu_lock();
        uint32_t f = g_xb_pg_frame[k];
        if (f != 0) {
            wl_xb_mmu_unmap(k);
            g_xb_pg_frame[k] = 0;
            give_free(f);
            wl_xb_mmu_frame_drop(k, f);
        }
        if (g_xb_pg_slot[k] != 0) {
            slot_free(g_xb_pg_slot[k] - 1u);
            g_xb_pg_slot[k] = 0;
        }
        if ((g_xb_pg_flags[k] & WL_XB_PG_PINNED) != 0) {
            g_pinned--;
        }
        g_xb_pg_flags[k] = 0;
        wl_xb_pg_wrote(k); // (zero from now on)
        wl_xb_mmu_unlock(lv);
    }
}

void wl_xb_pg_move(uint32_t from, uint32_t to) {
    if (g_xb_pg_frames == 0) {
        return;
    }
    wl_xb_pg_release(to, 1); // a free page touched through the window may hold a frame
    uint32_t lv = wl_xb_mmu_lock();
    wl_xb_mmu_unmap(from);
    uint32_t f = g_xb_pg_frame[from];
    if (f != 0) {
        wl_xb_mmu_frame_move(from, to, f);
        g_xb_pg_frame[from] = 0;
        assign(f, to);
    }
    g_xb_pg_slot[to] = g_xb_pg_slot[from];
    g_xb_pg_slot[from] = 0;
    g_xb_pg_flags[to] = g_xb_pg_flags[from];
    g_xb_pg_flags[from] = 0;
    wl_xb_pg_wrote(from);
    wl_xb_pg_wrote(to);
    if ((g_xb_pg_flags[to] & WL_XB_PG_PINNED) != 0) {
        wl_xb_mmu_pinned(to);
    }
    wl_xb_mmu_unlock(lv);
}

WlResult wl_xb_pg_pin(uint32_t p, bool pin) {
    if (g_xb_pg_frames == 0) {
        return WL_OK; // the OS keeps every page
    }
    if (p >= g_xb_ram_pages) {
        return WL_ERR_RANGE;
    }
    bool pinned = (g_xb_pg_flags[p] & WL_XB_PG_PINNED) != 0;
    if (!pin) {
        if (pinned) {
            uint32_t lv = wl_xb_mmu_lock();
            // its changes were not watched while pinned (the host maps it writable): written out
            // when it goes
            g_xb_pg_flags[p] = (uint8_t)((g_xb_pg_flags[p] & ~WL_XB_PG_PINNED) | WL_XB_PG_DIRTY);
            g_pinned--;
            wl_xb_mmu_unlock(lv);
        }
        return WL_OK;
    }
    if (pinned) {
        return WL_OK;
    }
    if (g_pinned + 1u > g_xb_pg_frames / 2u) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_xb_pg_fault(p, 0);
    if (r != WL_OK) {
        return r;
    }
    uint32_t lv = wl_xb_mmu_lock();
    g_xb_pg_flags[p] |= WL_XB_PG_PINNED;
    g_pinned++;
    wl_xb_mmu_pinned(p);
    wl_xb_mmu_unlock(lv);
    return WL_OK;
}

uint32_t wl_xb_pg_watch(uint32_t p, uint32_t pages) {
    if (g_xb_pg_frames == 0 || pages == 0 || p >= g_xb_ram_pages || pages > g_xb_ram_pages - p) {
        return 0;
    }
    bool seen = true;
    uint32_t lv = wl_xb_mmu_lock();
    for (uint32_t k = p; k < p + pages; k++) {
        g_xb_pg_flags[k] |= WL_XB_PG_WATCH;
        seen = wl_xb_mmu_watch(k) && seen;
    }
    uint32_t stamp = g_xb_pg_epoch;
    wl_xb_mmu_unlock(lv);
    return seen ? stamp : 0u;
}

bool wl_xb_pg_written(uint32_t p, uint32_t pages, uint32_t stamp) {
    if (stamp == 0 || g_xb_pg_frames == 0 || pages == 0 || p >= g_xb_ram_pages || pages > g_xb_ram_pages - p) {
        return true;
    }
    bool written = false;
    uint32_t lv = wl_xb_mmu_lock();
    for (uint32_t k = p; k < p + pages && !written; k++) {
        g_xb_pg_flags[k] |= WL_XB_PG_WATCH; // (a page freed since lost it)
        written = !wl_xb_mmu_watch(k) || g_xb_pg_wgen[k] > stamp;
    }
    wl_xb_mmu_unlock(lv);
    return written;
}

void wl_xb_pg_stats(WlXbMemStats *out) {
    out->frames_total = g_xb_pg_frames;
    out->frames_free = g_free_n;
    out->pinned_pages = g_pinned;
    out->evictions = g_st.evictions;
    out->clean_evictions = g_st.clean_evictions;
    out->fast_evictions = g_st.fast_evictions;
    out->swap_ins = g_st.swap_ins;
    out->swap_outs = g_st.swap_outs;
    out->swap_writes = g_st.swap_writes;
    out->swap_read_bytes = g_st.read_bytes;
    out->swap_write_bytes = g_st.write_bytes;
    out->swap_us = g_st.io_us;
    out->swap_slots = g_slots;
    out->swap_slots_used = g_slots_used;
}

// ---- Set-up ------------------------------------------------------------------------------------

static void swap_open(const WlXbMemConfig *cfg) {
    if (cfg->swap_path == NULL || cfg->swap_path[0] == '\0') {
        WL_LOG_W(TAG, "no swap file: only pages with nothing to write give their frames back");
        return;
    }
    uint32_t bytes = cfg->swap_bytes != 0 ? cfg->swap_bytes : cfg->ram_bytes;
    bytes &= ~(PAGE - 1u);
    g_slots = bytes / PAGE;
    if (g_slots > 0xFFFFu) {
        g_slots = 0xFFFFu; // slot + 1 is 16 bits
    }
    g_slot_used = calloc((g_slots + 31u) / 32u, sizeof *g_slot_used);
    g_io = aligned_alloc(64, BATCH * PAGE);
    WlPlatformFile *f = NULL;
    WlResult r = g_slot_used != NULL && g_io != NULL ? wl_platform_fs_open(cfg->swap_path, WL_FILE_READ_WRITE, &f)
                                                     : WL_ERR_NOMEM;
    uint64_t have = r == WL_OK ? wl_platform_fs_size(f) : 0;
    if (r == WL_OK && have < (uint64_t)g_slots * PAGE) {
        // grown once and kept: the card allocates the clusters now, not in the middle of a game
        WL_LOG_I(TAG, "making the swap file %s: %u MB", cfg->swap_path, (unsigned)((g_slots * PAGE) >> 20));
        uint64_t t0 = wl_platform_time_us();
        r = wl_platform_fs_truncate(f, (uint64_t)g_slots * PAGE);
        WL_LOG_I(TAG, "  in %u ms", (unsigned)((wl_platform_time_us() - t0) / 1000u));
    }
    if (r != WL_OK) {
        WL_LOG_W(TAG, "swap file %s: %s; only pages with nothing to write give their frames back", cfg->swap_path,
                 wl_result_name(r));
        if (f != NULL) {
            wl_platform_fs_close(f);
        }
        g_slots = 0;
        return;
    }
    g_swap = f;
    WL_LOG_I(TAG, "swap file %s: %u pages", cfg->swap_path, (unsigned)g_slots);
}

size_t wl_xb_pg_host_bytes(void) {
    if (g_xb_pg_frames == 0) {
        return 0;
    }
    size_t pages = g_xb_ram_pages;
    return pages * (sizeof *g_xb_pg_flags + sizeof *g_xb_pg_frame + sizeof *g_xb_pg_slot + sizeof *g_xb_pg_wgen) +
           (g_xb_pg_frames + 1u) * sizeof *g_owner + g_xb_pg_frames * sizeof *g_free +
           (g_slot_used != NULL ? (g_slots + 31u) / 32u * sizeof *g_slot_used : 0u) + (g_io != NULL ? BATCH * PAGE : 0u);
}

WlResult wl_xb_pg_init(const WlXbMemConfig *cfg, uint32_t frames) {
    if (g_xb_pg_frames != 0 || frames == 0 || frames > 0xFFFFu) {
        return WL_ERR_ARG;
    }
    uint32_t pages = cfg->ram_bytes >> WL_XB_PAGE_SHIFT;
    g_xb_pg_flags = calloc(pages, 1);
    g_xb_pg_frame = calloc(pages, sizeof *g_xb_pg_frame);
    g_xb_pg_slot = calloc(pages, sizeof *g_xb_pg_slot);
    g_owner = calloc(frames + 1u, sizeof *g_owner);
    g_free = calloc(frames, sizeof *g_free);
    g_xb_pg_wgen = calloc(pages, sizeof *g_xb_pg_wgen);
    if (g_xb_pg_flags == NULL || g_xb_pg_frame == NULL || g_xb_pg_slot == NULL || g_owner == NULL || g_free == NULL ||
        g_xb_pg_wgen == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t f = frames; f >= 1u; f--) {
        g_free[g_free_n++] = (uint16_t)f; // frame 1 comes first
    }
    swap_open(cfg);
    g_xb_pg_frames = frames;
    WL_LOG_I(TAG, "demand paging over %u frames (%u KB)", (unsigned)frames, (unsigned)(frames * (PAGE >> 10)));
    return WL_OK;
}
