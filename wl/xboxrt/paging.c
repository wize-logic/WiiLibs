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
//   - swapped: no frame, a slot: its bytes are in the swap file, and a touch reads them back;
//   - a file copy: no frame, its bytes are a file's (the slot field's FILE_SLOT bit, the file's
//     index in g_src and its offset in g_foff): a file read into guest memory filled it whole
//     (wl_xb_pg_filed), and a touch reads it from the file again.
// A resident page keeps its slot or its file copy while it stays clean, so its frame goes back for
// nothing; a dirty one gets a new slot when it is written out. A file's copies are forgotten
// (wl_xb_pg_forget) before its bytes change or it is closed: a resident page is marked dirty, a
// page without a frame read back first.
//
// The clock goes round the frames. A pinned page, or one in I/O, is passed by. The page's R bits
// are harvested and cleared, and a page used since the last round is passed too (its second
// chance). A clean page gives its frame back at once, and so does a dirty one found zero again
// (a heap clears its blocks: nothing to write, and its touch zero-fills it; in the exception
// context only one with no copy to forget). Another dirty one is left: in the slow path the
// dirty pages passed are gathered, and when a look finds no clean page they are written in one
// write to consecutive slots and their frames taken (a batch: the SD card is slow at small
// writes). The fault's exception context (the Wii's DSI) only looks a bounded way ahead and never
// writes or reads; a touch that needs I/O goes to the slow path, in normal context with the guest
// lock held. A read back there gives the guest lock up while the card works when the touch is the
// thread's own (wl_xb_pg_touch, not inside a kernel call or a device's emulation): the page is in
// transit meanwhile (below). One made with the lock kept takes the file copies after it at the
// following offsets in the same read (up to RUN pages, with frames had without I/O): a device
// walks a buffer page after page, and the card's time is mostly its seek.
//
// The page cache (WlXbMemConfig.page_cache_bytes) is a share of the frames kept out of the pages:
// a page that gives its frame back clean with a copy (its file, a slot: the clock's clean pages, a
// batch's written ones) is packed into it too, and its next touch unpacks it instead of reading the
// card. Most of a level's paging is pages coming back soon after they went (a10's opening at the
// Wii's 24 MB: 89% of the pages read back), and their packed copies take about half a frame.
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
// KeTickCount, a PTE reload) sees them whole; the I/O runs outside, its pages marked IO. Every
// transfer on the swap file and the files is made under the card's lock (thread.h): the kernel's
// reads, and a read back without the guest lock, may be on the same handle at once.
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/thread.h"

#define TAG "xbpage"
#define BATCH 32u      // pages a swap write takes at most (128 KB)
#define FAST_LOOK 256u // frames the exception context's clock looks at before it gives up
#define SLOT_ALIGN 8u  // a batch starts on 8 slots (32 KB) when it can
#define PAGE WL_XB_PAGE
#define FILE_SLOT 0x8000u // in g_xb_pg_slot: a file copy, its source's index + 1 in the low bits
#define SOURCES 32u       // files with copies at once
#define RUN 8u            // file copies a read back with the guest lock kept takes at most (32 KB)

uint8_t *g_xb_pg_flags;
uint16_t *g_xb_pg_frame;
uint16_t *g_xb_pg_slot;
uint32_t g_xb_pg_frames;
uint32_t *g_xb_pg_wgen;
uint32_t g_xb_pg_epoch = 1; // (a test may move it to just before its wrap)

static uint16_t *g_owner;  // [frames + 1] the page holding each frame, + 1 (0: free)
static uint16_t *g_free;   // a stack of free frames
static uint32_t g_free_n;
static uint32_t g_hand = 1; // the clock: the next frame it looks at
static uint32_t g_pinned;

static WlPlatformFile *g_swap;
static uint32_t *g_slot_used; // a bit per slot
static uint32_t g_slots, g_slots_used, g_slot_cursor;
static uint8_t *g_io; // BATCH pages: the batch's bytes (guest lock held)
static uint8_t *g_in; // a page read back (the card's lock held): the page after g_io's
static uint16_t g_batch[BATCH];
static uint32_t g_batch_n;

static uint32_t *g_foff; // [ram pages] a file copy's offset in its file
// A diagnostic: the eviction count (its low 16 bits, 0 skipped) when each page last lost its frame, 0
// once it has one again or its contents went (a stamp from 65536 evictions back reads as recent)
static uint16_t *g_evstamp; // [ram pages]
static uint32_t g_evcount;
static struct {
    WlPlatformFile *file; // NULL: free
    uint32_t pages;       // file copies of it
    uint64_t lo, hi;      // its bytes they may hold: [lo, hi) (a forget elsewhere looks at no page)
} g_src[SOURCES];

static struct {
    uint32_t evictions, clean_evictions, fast_evictions;
    uint32_t swap_ins, swap_outs, swap_writes, zero_outs, discards;
    uint32_t file_copies, file_evictions, file_ins, file_ahead, file_forgets, file_forget_ins;
    uint64_t read_bytes, write_bytes, io_us, io_held_us;
    uint32_t io_held_max_us, io_unlocked, transit_waits;
    uint32_t zero_evictions, returns, refaults_swap, refaults_file, refaults_zero, ahead_unused;
    uint32_t z_puts, z_raw, z_hits;
} g_st;

// A transfer of `us` microseconds ends: its time counted, and with `held` (the
// guest lock kept through it) as time no other thread ran.
static void io_done(uint64_t us, bool held) {
    g_st.io_us += us;
    if (held) {
        g_st.io_held_us += us;
        if (us > g_st.io_held_max_us) {
            g_st.io_held_max_us = (uint32_t)(us > UINT32_MAX ? UINT32_MAX : us);
        }
    } else {
        g_st.io_unlocked++;
    }
}

// ---- Frames (exception-safe) --------------------------------------------------------------------

static uint32_t take_free(void) {
    return g_free_n != 0 ? g_free[--g_free_n] : 0u;
}

static void give_free(uint32_t f) {
    g_owner[f] = 0;
    g_free[g_free_n++] = (uint16_t)f;
}

// ---- The page cache (exception-safe) --------------------------------------------------------------
// Frames kept out of the pages hold a ring of packed pages (page_pack.c): a page that gives its
// frame back clean with a copy elsewhere (its file, the swap file) is packed into it as well, and a
// touch that finds it there unpacks it instead of reading the card. Each entry is a header word
// (the page << 12 | its packed bytes) and the bytes, at a 4-byte boundary, across the frames' ends
// if need be; the oldest go as new ones come. A page's entry stands for its copy: it goes with it
// (copy_drop), and stays while the page is resident and clean (going again costs no packing).

#define ZMAX (PAGE * 3u / 4u) // a page packed larger is not kept (a fourth saved at least)

static uint8_t **g_zmem;       // [g_zframes] the cache's frames
static uint32_t g_zframes;     // 0: no cache
static uint32_t *g_zloc;       // [ram pages] its entry's place in the ring / 4 + 1, 0: none
static uint32_t g_zhead, g_ztail, g_zused; // the ring's next entry, its oldest, its bytes in use
static uint32_t g_zpages;                  // entries that stand for a page
static uint64_t g_zbytes;                  // their packed bytes
static uint8_t g_zpack[ZMAX];              // a page being packed (the MMU lock)
static uint8_t g_zspan[ZMAX];              // a page's entry being unpacked (the guest lock)

static uint32_t z_entry(uint32_t n) {
    return (4u + n + 3u) & ~3u;
}

static uint32_t z_word(uint32_t at) {
    uint32_t w;
    memcpy(&w, g_zmem[at / PAGE] + at % PAGE, 4); // (4-byte aligned: never across a frame)
    return w;
}

// Copies `n` bytes between the ring from `at` and `buf` (`out`: from the ring).
static void z_copy(uint32_t at, uint8_t *buf, uint32_t n, bool out) {
    uint32_t ring = g_zframes * PAGE;
    while (n != 0) {
        at %= ring;
        uint32_t k = PAGE - at % PAGE;
        k = k < n ? k : n;
        uint8_t *z = g_zmem[at / PAGE] + at % PAGE;
        memcpy(out ? buf : z, out ? z : buf, k);
        buf += k;
        at += k;
        n -= k;
    }
}

// Page `p` has no entry from now on (its bytes or its copy changed, or it went).
static void z_forget(uint32_t p) {
    uint32_t loc = g_zframes != 0 ? g_zloc[p] : 0u;
    if (loc != 0) {
        g_zloc[p] = 0;
        g_zpages--;
        g_zbytes -= z_word((loc - 1u) * 4u) & (PAGE - 1u);
    }
}

// Clean page `p`, whose copy is elsewhere, gives its frame back: its bytes packed into the cache.
static void z_put(uint32_t p, const uint8_t *bytes) {
    if (g_zframes == 0 || g_zloc[p] != 0 || (g_xb_pg_flags[p] & WL_XB_PG_RAW) != 0) {
        return; // no cache, already in it, or does not pack
    }
    uint32_t n = (uint32_t)wl_xb_page_pack(bytes, g_zpack, ZMAX);
    if (n == 0) {
        g_xb_pg_flags[p] |= WL_XB_PG_RAW;
        g_st.z_raw++;
        return;
    }
    uint32_t need = z_entry(n), ring = g_zframes * PAGE;
    while (ring - g_zused < need) {
        // the oldest entry goes
        uint32_t w = z_word(g_ztail);
        uint32_t q = w >> 12;
        if (g_zloc[q] == g_ztail / 4u + 1u) {
            z_forget(q);
        }
        g_zused -= z_entry(w & (PAGE - 1u));
        g_ztail = (g_ztail + z_entry(w & (PAGE - 1u))) % ring;
    }
    uint32_t w = p << 12 | n;
    memcpy(g_zmem[g_zhead / PAGE] + g_zhead % PAGE, &w, 4);
    z_copy(g_zhead + 4u, g_zpack, n, false);
    g_zloc[p] = g_zhead / 4u + 1u;
    g_zhead = (g_zhead + need) % ring;
    g_zused += need;
    g_zpages++;
    g_zbytes += n;
    g_st.z_puts++;
}

// Page `p` stood for in the cache: its packed bytes into g_zspan (MMU lock held); their count.
static uint32_t z_take(uint32_t p) {
    uint32_t at = (g_zloc[p] - 1u) * 4u;
    uint32_t n = z_word(at) & (PAGE - 1u);
    z_copy(at + 4u, g_zspan, n, true);
    return n;
}

// Page `p` lost its frame (its contents kept: a copy, or zero).
static void stamp_evicted(uint32_t p) {
    wl_xb_census_io(WL_XB_CIO_EVICT, p, false);
    uint16_t st = (uint16_t)++g_evcount;
    g_evstamp[p] = st != 0 ? st : 1u;
}

static void assign(uint32_t f, uint32_t p) {
    g_owner[f] = (uint16_t)(p + 1u);
    g_xb_pg_frame[p] = (uint16_t)f;
    uint16_t st = g_evstamp[p];
    if (st != 0) {
        // back after losing its frame: a refault when that was within a pool's worth of evictions
        g_evstamp[p] = 0;
        g_st.returns++;
        if ((uint16_t)((uint16_t)g_evcount - st) < g_xb_pg_frames) {
            uint16_t s = g_xb_pg_slot[p];
            if (s == 0) {
                g_st.refaults_zero++;
            } else if ((s & FILE_SLOT) != 0) {
                g_st.refaults_file++;
            } else {
                g_st.refaults_swap++;
            }
        }
    }
}

// A clean page `q` not used lately gives frame `f` back (q is unmapped).
static void evict_clean(uint32_t q, uint32_t f) {
    if (g_xb_pg_slot[q] != 0) {
        z_put(q, wl_xb_mmu_frame_ptr(q, f)); // (before the host's frame drop lets its bytes go)
    }
    g_xb_pg_frame[q] = 0;
    g_owner[f] = 0;
    g_xb_pg_flags[q] &= (uint8_t)~WL_XB_PG_REF;
    wl_xb_mmu_frame_drop(q, f);
    stamp_evicted(q);
    g_st.evictions++;
    g_st.clean_evictions++;
    if ((g_xb_pg_flags[q] & WL_XB_PG_AHEAD) != 0) {
        g_xb_pg_flags[q] &= (uint8_t)~WL_XB_PG_AHEAD;
        g_st.ahead_unused++;
        wl_xb_census_io(WL_XB_CIO_AHEAD_UNUSED, q, false);
    }
    if ((g_xb_pg_slot[q] & FILE_SLOT) != 0) {
        g_st.file_evictions++;
    } else if (g_xb_pg_slot[q] == 0) {
        g_st.zero_evictions++;
    }
}

// Whether a page's bytes are all zero (a frame: page aligned).
static bool page_is_zero(const uint8_t *bytes) {
    const uint64_t *w = (const uint64_t *)(const void *)bytes;
    uint64_t any = 0;
    for (uint32_t i = 0; i < PAGE / 8u; i += 4u) {
        any |= w[i] | w[i + 1] | w[i + 2] | w[i + 3];
        if (any != 0) {
            return false;
        }
    }
    return true;
}

static void copy_drop(uint32_t p);

// A changed page `q` (unmapped) found zero gives frame `f` back: a zero page again, with its
// write epoch as it is (its bytes are).
static void evict_zero(uint32_t q, uint32_t f) {
    copy_drop(q); // (stale)
    g_xb_pg_frame[q] = 0;
    g_owner[f] = 0;
    g_xb_pg_flags[q] &= (uint8_t)~(WL_XB_PG_DIRTY | WL_XB_PG_REF | WL_XB_PG_AHEAD);
    wl_xb_mmu_frame_drop(q, f);
    stamp_evicted(q);
    g_st.evictions++;
    g_st.zero_outs++;
}

// One step of the clock, at frame g_hand. Returns the frame a clean page, or a changed page found
// zero, gave back, or 0. With `collect`, an unused dirty page is unmapped, marked IO and added to
// g_batch (while it has room).
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
        g_xb_pg_flags[q] = (uint8_t)(fl & ~(WL_XB_PG_REF | WL_XB_PG_AHEAD)); // its second chance
        return 0;
    }
    if ((fl & WL_XB_PG_DIRTY) != 0) {
        // changed, but zero again (cleared, as a heap's fresh blocks are): goes as a zero page, with
        // nothing to write or read back (the exception context: only a page with no copy to forget)
        if ((collect || g_xb_pg_slot[q] == 0) && page_is_zero(wl_xb_mmu_frame_ptr(q, f))) {
            wl_xb_mmu_unmap(q);
            if (!page_is_zero(wl_xb_mmu_frame_ptr(q, f))) {
                return 0; // stored to before the unmap (left unmapped: its next touch maps it again)
            }
            evict_zero(q, f);
            return f;
        }
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

// Page `p` forgets its copy: its slot, or its file's bytes. Lock held.
static void copy_drop(uint32_t p) {
    z_forget(p);
    g_xb_pg_flags[p] &= (uint8_t)~WL_XB_PG_RAW;
    uint32_t s = g_xb_pg_slot[p];
    if (s == 0) {
        return;
    }
    if ((s & FILE_SLOT) != 0) {
        g_src[(s & ~FILE_SLOT) - 1u].pages--;
    } else {
        slot_free(s - 1u);
    }
    g_xb_pg_slot[p] = 0;
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
    uint32_t n = 0, zeroed = 0;
    for (uint32_t i = 0; i < g_batch_n; i++) {
        uint32_t p = g_batch[i];
        const uint8_t *bytes = wl_xb_mmu_frame_ptr(p, g_xb_pg_frame[p]);
        uint32_t lv = wl_xb_mmu_lock();
        copy_drop(p); // its old copy is stale
        if (page_is_zero(bytes)) {
            // zero again (cleared after a store): a zero page, with nothing to write or read back;
            // its bytes and its write epoch stay as they are
            uint32_t f = g_xb_pg_frame[p];
            g_xb_pg_frame[p] = 0;
            g_xb_pg_flags[p] &= (uint8_t)~(WL_XB_PG_IO | WL_XB_PG_DIRTY | WL_XB_PG_REF);
            give_free(f);
            wl_xb_mmu_frame_drop(p, f);
            stamp_evicted(p);
            wl_xb_mmu_unlock(lv);
            zeroed++;
            continue;
        }
        wl_xb_mmu_unlock(lv);
        memcpy(g_io + n * PAGE, bytes, PAGE);
        g_batch[n++] = (uint16_t)p;
    }
    g_batch_n = n;
    g_st.zero_outs += zeroed;
    g_st.evictions += zeroed;
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
        wl_xb_card_lock(); // (another thread's read back may be on the handle, outside the guest lock)
        WlResult r = wl_platform_fs_write_at(g_swap, (uint64_t)first * PAGE, g_io + done * PAGE, len * PAGE);
        wl_xb_card_unlock();
        io_done(wl_platform_time_us() - t0, true); // (frame_slow: the guest lock held)
        if (r != WL_OK) {
            WL_LOG_E(TAG, "swap file write of %u pages at slot %u: %s", (unsigned)len, (unsigned)first,
                     wl_result_name(r));
            return WL_ERR_IO;
        }
        g_st.swap_writes++;
        g_st.write_bytes += (uint64_t)len * PAGE;
        for (uint32_t k = 0; k < len; k++) {
            wl_xb_census_io(WL_XB_CIO_SWAP_OUT, g_batch[done + k], k == 0);
        }
        done += len;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = g_batch[i];
        uint32_t lv = wl_xb_mmu_lock();
        uint32_t f = g_xb_pg_frame[p];
        z_put(p, wl_xb_mmu_frame_ptr(p, f)); // (clean now, its copy in the swap file)
        g_xb_pg_frame[p] = 0;
        g_xb_pg_flags[p] &= (uint8_t)~(WL_XB_PG_IO | WL_XB_PG_DIRTY | WL_XB_PG_REF);
        give_free(f);
        wl_xb_mmu_frame_drop(p, f);
        stamp_evicted(p);
        wl_xb_mmu_unlock(lv);
    }
    g_st.swap_outs += n;
    g_st.evictions += n;
    g_batch_n = 0;
    return n + zeroed != 0 ? WL_OK : WL_ERR_NOMEM;
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

// A page read back with the guest lock given up (wl_xb_pg_touch) is in transit: unmapped, marked
// IO, its frame its own. Its record is taken and freed by its reader with the guest lock held;
// `done` and `r` are set under the card's lock once its bytes are in the frame. Another thread that
// meets the page (a touch, a free, a move, a pin, a forget: the guest lock held) waits for `done`
// and ends the read for it (transit_wait): the page is read once and its frame handed out once.
#define TRANSITS 8u // pages in transit at once (a thread each); with none free the lock is kept
static struct {
    uint32_t p1;        // the page + 1, 0: free
    bool ended;         // its flags are a read page's (by its reader or a waiter)
    volatile bool done; // (under the card's lock)
    WlResult r;
} g_transit[TRANSITS];
static bool g_giving_up; // in wl_xb_block_begin for a read back (the lock holder's)

// The physical page under guest VA `va`, + 1 (0: none): user space through its table, the window
// and its tiled alias as they are.
static uint32_t phys_page(uint32_t va) {
    if (va < WL_XB_USER_LIMIT) {
        return g_xb_va2phys != NULL ? g_xb_va2phys[va >> WL_XB_PAGE_SHIFT] : 0u;
    }
    uint32_t base = va >= WL_XB_TILED ? WL_XB_TILED : WL_XB_WINDOW;
    uint32_t p = (va - base) >> WL_XB_PAGE_SHIFT;
    return va >= WL_XB_WINDOW && p < g_xb_ram_pages ? p + 1u : 0u;
}

static uint32_t transit_of(uint32_t p) {
    for (uint32_t t = 0; t < TRANSITS; t++) {
        if (g_transit[t].p1 == p + 1u && !g_transit[t].ended) {
            return t + 1u;
        }
    }
    return 0;
}

// Page p's read is over: its flags as a page just read (left as they are after a failure: a crash).
static void read_end(uint32_t p, WlResult r) {
    if (r != WL_OK) {
        return;
    }
    uint32_t lv = wl_xb_mmu_lock();
    g_xb_pg_flags[p] =
        (uint8_t)((g_xb_pg_flags[p] & ~(WL_XB_PG_IO | WL_XB_PG_DIRTY)) | WL_XB_PG_REF);
    wl_xb_mmu_unlock(lv);
}

// Waits for transit record t1 - 1's read and ends it, the guest lock kept: its reader needs only
// the card's lock (and the one waiting may be a DPC, which cannot give the guest lock up).
static WlResult transit_wait(uint32_t t1) {
    uint32_t t = t1 - 1u;
    for (;;) {
        wl_xb_card_lock();
        bool done = g_transit[t].done;
        wl_xb_card_unlock();
        if (done) {
            break;
        }
        wl_platform_thread_sleep_ms(1); // its reader has not reached the card yet
    }
    g_st.transit_waits++;
    if (!g_transit[t].ended) {
        g_transit[t].ended = true;
        read_end(g_transit[t].p1 - 1u, g_transit[t].r);
    }
    return g_transit[t].r != WL_OK ? WL_ERR_IO : WL_OK;
}

// A page in transit is waited for (guest lock held): its tables are the lock's again.
static WlResult settle(uint32_t p) {
    if ((g_xb_pg_flags[p] & WL_XB_PG_IO) == 0) {
        return WL_OK;
    }
    uint32_t t1 = transit_of(p);
    return t1 != 0 ? transit_wait(t1) : WL_ERR_STATE;
}

// The file copies after page p (slot `slot`) at the following offsets, at pages p + step, p +
// 2 step, ...: given frames had without I/O and marked IO, up to RUN pages with p. Returns the
// run's length (1: p alone). Guest lock held.
static uint32_t read_ahead(uint32_t p, uint32_t slot, uint32_t step, uint32_t keep) {
    uint32_t run = 1;
    while (run < RUN) {
        uint32_t q = p + run * step;
        if (q >= g_xb_ram_pages || g_xb_pg_slot[q] != slot || g_foff[q] != g_foff[p] + run * PAGE ||
            g_xb_pg_frame[q] != 0 || (g_xb_pg_flags[q] & (WL_XB_PG_IO | WL_XB_PG_PINNED)) != 0) {
            break;
        }
        uint32_t lv = wl_xb_mmu_lock();
        uint32_t fq = wl_xb_pg_frame_fast(q, keep);
        if (fq != 0) {
            g_xb_pg_flags[q] |= WL_XB_PG_IO;
        }
        wl_xb_mmu_unlock(lv);
        if (fq == 0) {
            break;
        }
        run++;
    }
    return run;
}

static WlResult fault(uint32_t p, uint32_t keep, bool give_up) {
    if (g_xb_pg_frames == 0 || p >= g_xb_ram_pages) {
        return WL_ERR_STATE;
    }
    if ((g_xb_pg_flags[p] & WL_XB_PG_IO) != 0) {
        return settle(p); // another thread reads it back
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
    // in the page cache: its entry copied out here (an eviction in exception context may write over
    // it), unpacked below with the frame the page's own while it is IO
    uint32_t zn = slot != 0 && g_zframes != 0 && g_zloc[p] != 0 ? z_take(p) : 0u;
    wl_xb_mmu_unlock(lv);
    if (slot == 0) {
        wl_xb_mmu_frame_zero(p, f);
        read_end(p, WL_OK);
        return WL_OK;
    }
    if (zn != 0) {
        if (wl_xb_page_unpack(g_zspan, zn, wl_xb_mmu_frame_ptr(p, f))) {
            g_st.z_hits++;
            wl_xb_census_io(WL_XB_CIO_ZHIT, p, false);
            read_end(p, WL_OK);
            return WL_OK;
        }
        WL_LOG_E(TAG, "page %u's entry in the page cache does not unpack: read instead", (unsigned)p);
        lv = wl_xb_mmu_lock();
        z_forget(p);
        wl_xb_mmu_unlock(lv);
    }
    bool file = (slot & FILE_SLOT) != 0;
    WlPlatformFile *from = file ? g_src[(slot & ~FILE_SLOT) - 1u].file : g_swap;
    uint64_t at = file ? g_foff[p] : (uint64_t)(slot - 1u) * PAGE;
    // the card's work without the guest lock where it can be given up: the others run meanwhile
    // (not inside a kernel call or a device's emulation: their C code expects the lock throughout)
    // (nor for the thread's own KPCR page: wl_xb_block_begin reads the IRQL there, and with the page
    // in transit that read would wait for its own read, which comes only after it: a hang with the
    // lock held, seen when a thread's first look at its KPCR came after that page went to swap)
    WlXbThread *cur = wl_xb_current();
    give_up = give_up && !g_giving_up && cur != NULL && cur->kernel_depth == 0 &&
              g_xb_mmio_depth == 0 && phys_page(cur->kpcr) != p + 1u;
    uint32_t t1 = 0;
    for (uint32_t t = 0; give_up && t1 == 0 && t < TRANSITS; t++) {
        if (g_transit[t].p1 == 0) {
            t1 = t + 1u;
        }
    }
    WlXbThread *self = NULL;
    if (t1 != 0) {
        g_transit[t1 - 1u].p1 = p + 1u;
        g_transit[t1 - 1u].ended = false;
        g_transit[t1 - 1u].done = false;
        g_giving_up = true; // (its look at the IRQL reads the KPCR, which may fault: that read
                            // keeps the lock)
        self = wl_xb_block_begin();
        g_giving_up = false;
        if (self == NULL) {
            g_transit[t1 - 1u].p1 = 0; // the lock kept (the deterministic mode, a raised IRQL)
            t1 = 0;
        }
    }
    // With the lock kept (a device's or the kernel's own access: the NV2A walks a buffer page after
    // page), the file copies after it at the following offsets come in the same read, as far as
    // frames are at hand without I/O: the card's time is mostly its seek, not its bytes.
    // (the pages above it, or below it: a buffer's pages may come in either order)
    uint32_t run = 1, step = 1;
    for (uint32_t dir = 0; t1 == 0 && file && run == 1u && dir < 2u; dir++) {
        step = dir == 0 ? 1u : (uint32_t)-1;
        run = read_ahead(p, slot, step, keep);
    }
    // the card's lock: the kernel may read the same handle outside the guest lock, and g_in is one
    // (g_io, for a run, is the lock holder's)
    uint8_t *buf = run > 1u ? g_io : g_in;
    uint64_t t0 =
        wl_platform_time_us(); // (a wait for the card counts: with the lock kept, it was held)
    wl_xb_card_lock();
    r = wl_platform_fs_read_at(from, at, buf, run * PAGE);
    uint64_t t9 = wl_platform_time_us();
    if (r == WL_OK) {
        // (its frames: no one else's while they are IO)
        for (uint32_t k = 0; k < run; k++) {
            uint32_t q = p + k * step;
            memcpy(wl_xb_mmu_frame_ptr(q, g_xb_pg_frame[q]), buf + k * PAGE, PAGE);
        }
    }
    if (t1 != 0) {
        g_transit[t1 - 1u].r = r;
        g_transit[t1 - 1u].done = true;
    }
    wl_xb_card_unlock();
    wl_xb_block_end(self);
    io_done(t9 - t0, self == NULL);
    if (r != WL_OK) {
        if (file) {
            WL_LOG_E(TAG, "file read of page %u (at %u): %s", (unsigned)p, (unsigned)at,
                     wl_result_name(r));
        } else {
            WL_LOG_E(TAG, "swap file read of slot %u: %s", (unsigned)(slot - 1u),
                     wl_result_name(r));
        }
    } else if (file) {
        g_st.file_ins++;
        g_st.file_ahead += run - 1u;
        wl_xb_census_io(WL_XB_CIO_FILE_IN, p, true);
        for (uint32_t k = 1; k < run; k++) {
            wl_xb_census_io(WL_XB_CIO_AHEAD, p + k * step, false);
        }
    } else {
        g_st.swap_ins++;
        g_st.read_bytes += PAGE;
        wl_xb_census_io(WL_XB_CIO_SWAP_IN, p, true);
    }
    if (t1 != 0) {
        if (!g_transit[t1 - 1u].ended) {
            g_transit[t1 - 1u].ended = true;
            read_end(p, r);
        }
        g_transit[t1 - 1u].p1 = 0;
    } else {
        read_end(p, r);
    }
    if (run > 1u) {
        // the pages read ahead: clean, and not used yet (the clock's first choice if they stay so);
        // after a failure they go back to their file
        lv = wl_xb_mmu_lock();
        for (uint32_t k = 1; k < run; k++) {
            uint32_t q = p + k * step;
            g_xb_pg_flags[q] &= (uint8_t)~(WL_XB_PG_IO | WL_XB_PG_DIRTY | WL_XB_PG_REF);
            g_xb_pg_flags[q] |= WL_XB_PG_AHEAD;
            if (r != WL_OK) {
                uint32_t fq = g_xb_pg_frame[q];
                g_xb_pg_frame[q] = 0;
                give_free(fq);
                wl_xb_mmu_frame_drop(q, fq);
            }
        }
        wl_xb_mmu_unlock(lv);
    }
    return r != WL_OK ? WL_ERR_IO : WL_OK;
}

WlResult wl_xb_pg_fault(uint32_t p, uint32_t keep) {
    return fault(p, keep, false);
}

WlResult wl_xb_pg_touch(uint32_t p, uint32_t keep) {
    return fault(p, keep, true);
}

// ---- Bookkeeping ---------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------

void wl_xb_pg_release(uint32_t p, uint32_t pages) {
    if (g_xb_pg_frames == 0) {
        return;
    }
    for (uint32_t k = p; k < p + pages && k < g_xb_ram_pages; k++) {
        (void)settle(k); // a read back of it ends first (its frame goes with it)
        uint32_t lv = wl_xb_mmu_lock();
        uint32_t f = g_xb_pg_frame[k];
        if (f != 0) {
            wl_xb_mmu_unmap(k);
            g_xb_pg_frame[k] = 0;
            give_free(f);
            wl_xb_mmu_frame_drop(k, f);
        }
        copy_drop(k);
        if ((g_xb_pg_flags[k] & WL_XB_PG_PINNED) != 0) {
            g_pinned--;
        }
        g_xb_pg_flags[k] = 0;
        g_evstamp[k] = 0;
        wl_xb_pg_wrote(k); // (zero from now on)
        wl_xb_mmu_unlock(lv);
    }
}

void wl_xb_pg_discard(uint32_t p, uint32_t pages) {
    if (g_xb_pg_frames == 0) {
        return;
    }
    for (uint32_t k = p; k < p + pages && k < g_xb_ram_pages; k++) {
        uint32_t lv = wl_xb_mmu_lock();
        if (g_xb_pg_frame[k] == 0 && g_xb_pg_slot[k] != 0 && (g_xb_pg_flags[k] & WL_XB_PG_IO) == 0) {
            copy_drop(k);
            wl_xb_pg_wrote(k); // (zero until the caller's stores)
            g_st.discards++;
            g_evstamp[k] = 0;
        }
        wl_xb_mmu_unlock(lv);
    }
}

void wl_xb_pg_move(uint32_t from, uint32_t to) {
    if (g_xb_pg_frames == 0) {
        return;
    }
    wl_xb_pg_release(to, 1); // a free page touched through the window may hold a frame
    (void)settle(from);
    uint32_t lv = wl_xb_mmu_lock();
    z_forget(from); // (its entry's header names it)
    wl_xb_mmu_unmap(from);
    g_evstamp[to] = g_evstamp[from];
    g_evstamp[from] = 0;
    uint32_t f = g_xb_pg_frame[from];
    if (f != 0) {
        wl_xb_mmu_frame_move(from, to, f);
        g_xb_pg_frame[from] = 0;
        assign(f, to);
    }
    g_xb_pg_slot[to] = g_xb_pg_slot[from];
    g_xb_pg_slot[from] = 0;
    g_foff[to] = g_foff[from];
    g_xb_pg_flags[to] = g_xb_pg_flags[from];
    g_xb_pg_flags[from] = 0;
    wl_xb_pg_wrote(from);
    wl_xb_pg_wrote(to);
    if ((g_xb_pg_flags[to] & WL_XB_PG_PINNED) != 0) {
        wl_xb_mmu_pinned(to);
    }
    wl_xb_mmu_unlock(lv);
}

// ---- File copies ---------------------------------------------------------------------------------

uint32_t wl_xb_pg_source(WlPlatformFile *file) {
    if (g_xb_pg_frames == 0 || g_swap == NULL || file == NULL) {
        return 0; // (without a swap file nothing could be read back when a file's copies are forgotten)
    }
    uint32_t free_at = 0;
    for (uint32_t i = 0; i < SOURCES; i++) {
        if (g_src[i].file == file) {
            return i + 1u;
        }
        if (free_at == 0 && (g_src[i].file == NULL || g_src[i].pages == 0)) {
            free_at = i + 1u;
        }
    }
    if (free_at != 0) {
        g_src[free_at - 1u].file = file;
        g_src[free_at - 1u].lo = UINT64_MAX;
        g_src[free_at - 1u].hi = 0;
    }
    return free_at;
}

void wl_xb_pg_filed(uint32_t p, uint32_t source, uint32_t offset) {
    if (g_xb_pg_frames == 0 || p >= g_xb_ram_pages || source == 0 || source > SOURCES) {
        return;
    }
    uint32_t lv = wl_xb_mmu_lock();
    uint8_t fl = g_xb_pg_flags[p];
    // a pinned page's stores are not seen; a zero page (no frame, no slot) stays one
    if ((fl & (WL_XB_PG_PINNED | WL_XB_PG_IO)) == 0 && (g_xb_pg_frame[p] != 0 || g_xb_pg_slot[p] != 0)) {
        if (g_xb_pg_frame[p] != 0) {
            wl_xb_mmu_unmap(p); // the copy's stores harvested; the next store is seen
        }
        copy_drop(p);
        g_xb_pg_flags[p] &= (uint8_t)~WL_XB_PG_DIRTY;
        g_xb_pg_slot[p] = (uint16_t)(FILE_SLOT | source);
        g_foff[p] = offset;
        g_src[source - 1u].pages++;
        if (offset < g_src[source - 1u].lo) {
            g_src[source - 1u].lo = offset;
        }
        if (offset + (uint64_t)PAGE > g_src[source - 1u].hi) {
            g_src[source - 1u].hi = offset + (uint64_t)PAGE;
        }
        g_st.file_copies++;
    }
    wl_xb_mmu_unlock(lv);
}

WlResult wl_xb_pg_forget(WlPlatformFile *file, uint64_t offset, uint64_t bytes) {
    if (g_xb_pg_frames == 0 || file == NULL) {
        return WL_OK;
    }
    uint32_t i = 0;
    while (i < SOURCES && g_src[i].file != file) {
        i++;
    }
    if (i == SOURCES) {
        return WL_OK;
    }
    uint64_t end = bytes > UINT64_MAX - offset ? UINT64_MAX : offset + bytes;
    bool all = offset == 0 && end == UINT64_MAX;
    if (g_src[i].pages == 0 || end <= g_src[i].lo || offset >= g_src[i].hi) {
        if (all) {
            g_src[i].file = NULL;
        }
        return WL_OK;
    }
    uint16_t want = (uint16_t)(FILE_SLOT | (i + 1u));
    WlResult r = WL_OK;
    for (uint32_t k = 0; k < g_xb_ram_pages && g_src[i].pages != 0; k++) {
        if (g_xb_pg_slot[k] != want || g_foff[k] + (uint64_t)PAGE <= offset || g_foff[k] >= end) {
            continue;
        }
        if (settle(k) != WL_OK) { // read back by another thread: its read ends first
            r = WL_ERR_IO;
        }
        if (g_xb_pg_frame[k] == 0) {
            // read back from the file while it still holds the bytes (the clock may take another of
            // its pages meanwhile: one after k is met later, one before it was dirty already)
            r = wl_xb_pg_fault(k, 0);
            if (r != WL_OK) {
                WL_LOG_E(TAG, "page %u lost with its file's copy: %s", (unsigned)k, wl_result_name(r));
            } else {
                g_st.file_forget_ins++;
            }
        }
        uint32_t lv = wl_xb_mmu_lock();
        copy_drop(k);
        g_xb_pg_flags[k] |= WL_XB_PG_DIRTY; // written to the swap file when it goes
        wl_xb_mmu_unlock(lv);
        g_st.file_forgets++;
        wl_xb_census_io(WL_XB_CIO_FORGET, k, false);
    }
    if (all && g_src[i].pages == 0) {
        g_src[i].file = NULL; // (the file may be closed and its handle reused)
    }
    return r;
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
        if ((uint32_t)(g_xb_pg_epoch - g_xb_pg_wgen[k]) >= WL_XB_PG_EPOCH_WINDOW) {
            g_xb_pg_wgen[k] = g_xb_pg_epoch; // too old to compare across the wrap: the present
        }
    }
    uint32_t stamp = g_xb_pg_epoch;
    wl_xb_mmu_unlock(lv);
    return seen ? stamp : 0u;
}

bool wl_xb_pg_written(uint32_t p, uint32_t pages, uint32_t stamp) {
    if (stamp == 0 || g_xb_pg_frames == 0 || pages == 0 || p >= g_xb_ram_pages || pages > g_xb_ram_pages - p) {
        return true;
    }
    uint32_t lv = wl_xb_mmu_lock();
    bool written = (uint32_t)(g_xb_pg_epoch - stamp) >= WL_XB_PG_EPOCH_WINDOW; // too old: may have been
    for (uint32_t k = p; k < p + pages && !written; k++) {
        g_xb_pg_flags[k] |= WL_XB_PG_WATCH; // (a page freed since lost it)
        written = !wl_xb_mmu_watch(k) || (int32_t)(g_xb_pg_wgen[k] - stamp) > 0;
    }
    wl_xb_mmu_unlock(lv);
    return written;
}

void wl_xb_pg_zero_census(uint32_t *dirty_zero, uint32_t *clean_zero) {
    *dirty_zero = *clean_zero = 0;
    for (uint32_t f = 1; f <= g_xb_pg_frames; f++) {
        uint32_t q1 = g_owner[f];
        if (q1 == 0 || (g_xb_pg_flags[q1 - 1u] & WL_XB_PG_IO) != 0) {
            continue;
        }
        uint32_t q = q1 - 1u;
        bool dirty = (g_xb_pg_flags[q] & WL_XB_PG_DIRTY) != 0;
        if ((dirty || g_xb_pg_slot[q] == 0) && page_is_zero(wl_xb_mmu_frame_ptr(q, f))) {
            *(dirty ? dirty_zero : clean_zero) += 1u;
        }
    }
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
    out->zero_outs = g_st.zero_outs;
    out->swap_ins_spared = g_st.discards;
    out->file_copies = g_st.file_copies;
    out->file_evictions = g_st.file_evictions;
    out->file_ins = g_st.file_ins;
    out->file_ahead = g_st.file_ahead;
    out->file_forgets = g_st.file_forgets;
    out->file_forget_ins = g_st.file_forget_ins;
    out->swap_read_bytes = g_st.read_bytes;
    out->swap_write_bytes = g_st.write_bytes;
    out->swap_us = g_st.io_us;
    out->io_held_us = g_st.io_held_us;
    out->io_held_max_us = g_st.io_held_max_us;
    out->io_unlocked = g_st.io_unlocked;
    out->transit_waits = g_st.transit_waits;
    out->swap_slots = g_slots;
    out->swap_slots_used = g_slots_used;
    out->zero_evictions = g_st.zero_evictions;
    out->returns = g_st.returns;
    out->refaults_swap = g_st.refaults_swap;
    out->refaults_file = g_st.refaults_file;
    out->refaults_zero = g_st.refaults_zero;
    out->ahead_unused = g_st.ahead_unused;
    out->zcache_frames = g_zframes;
    out->zcache_puts = g_st.z_puts;
    out->zcache_raw = g_st.z_raw;
    out->zcache_hits = g_st.z_hits;
    out->zcache_pages = g_zpages;
    out->zcache_bytes = g_zbytes;
    out->resident_dirty = out->resident_file = 0;
    for (uint32_t f = 1; f <= g_xb_pg_frames; f++) {
        uint32_t q1 = g_owner[f];
        if (q1 != 0) {
            out->resident_dirty += (g_xb_pg_flags[q1 - 1u] & WL_XB_PG_DIRTY) != 0;
            out->resident_file += (g_xb_pg_slot[q1 - 1u] & FILE_SLOT) != 0;
        }
    }
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
    if (g_slots > FILE_SLOT - 1u) {
        g_slots = FILE_SLOT - 1u; // slot + 1 is 15 bits (FILE_SLOT marks a file copy)
    }
    g_slot_used = calloc((g_slots + 31u) / 32u, sizeof *g_slot_used);
    g_io = aligned_alloc(64, (BATCH + 1u) * PAGE);
    g_in = g_io != NULL ? g_io + BATCH * PAGE : NULL;
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
    return pages * (sizeof *g_xb_pg_flags + sizeof *g_xb_pg_frame + sizeof *g_xb_pg_slot + sizeof *g_xb_pg_wgen +
                    sizeof *g_foff + sizeof *g_evstamp) +
           (g_xb_pg_frames + 1u) * sizeof *g_owner + g_xb_pg_frames * sizeof *g_free +
           (g_slot_used != NULL ? (g_slots + 31u) / 32u * sizeof *g_slot_used : 0u) +
           (g_io != NULL ? (BATCH + 1u) * PAGE : 0u) +
           (g_zframes != 0 ? pages * sizeof *g_zloc + g_zframes * sizeof *g_zmem : 0u);
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
    g_foff = calloc(pages, sizeof *g_foff);
    g_evstamp = calloc(pages, sizeof *g_evstamp);
    if (g_evstamp == NULL || g_xb_pg_flags == NULL || g_xb_pg_frame == NULL || g_xb_pg_slot == NULL || g_owner == NULL || g_free == NULL ||
        g_xb_pg_wgen == NULL || g_foff == NULL) {
        return WL_ERR_NOMEM;
    }
    // the page cache's share: the last frames
    uint32_t zbytes = cfg->page_cache_bytes;
    const char *env = getenv("WL_XB_PAGE_CACHE_KB");
    if (env != NULL && env[0] != '\0') {
        zbytes = (uint32_t)strtoul(env, NULL, 10) << 10;
    }
    uint32_t z = zbytes == WL_XB_PAGE_CACHE_AUTO ? (frames >= (16u << 20) / PAGE ? frames / 12u : 0u) : zbytes / PAGE;
    z = z < frames / 4u ? z : frames / 4u;
    if (z != 0) {
        g_zmem = calloc(z, sizeof *g_zmem);
        g_zloc = calloc(pages, sizeof *g_zloc);
        for (uint32_t i = 0; g_zmem != NULL && i < z; i++) {
            g_zmem[i] = wl_xb_mmu_frame_ptr(WL_XB_PG_NO_PAGE, frames - z + 1u + i);
            if (g_zmem[i] == NULL) {
                free(g_zmem);
                g_zmem = NULL;
            }
        }
        if (g_zmem == NULL || g_zloc == NULL) {
            return WL_ERR_NOMEM;
        }
        g_zframes = z;
        frames -= z;
    }
    for (uint32_t f = frames; f >= 1u; f--) {
        g_free[g_free_n++] = (uint16_t)f; // frame 1 comes first
    }
    swap_open(cfg);
    g_xb_pg_frames = frames;
    WL_LOG_I(TAG, "demand paging over %u frames (%u KB), a page cache of %u KB", (unsigned)frames,
             (unsigned)(frames * (PAGE >> 10)), (unsigned)(z * (PAGE >> 10)));
    return WL_OK;
}
