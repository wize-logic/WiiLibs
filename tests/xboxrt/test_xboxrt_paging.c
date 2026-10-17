// SPDX-License-Identifier: MIT
// Demand paging of guest memory on the host / ppc-be backend under a frame limit: the Wii's model
// (paging.c's clock and swap file) through SIGSEGV and mprotect. Zero pages give their frames back
// for nothing, changed pages go to the swap file in batches and come back on their next touch,
// clean pages read back cost no write when they go again, changed pages cleared to zero go as zero
// pages, a file read over swapped pages does not read them back first, pages a file read filled
// whole go for nothing and come back from the file until they change or the file does, both EAs of
// a page see one page whatever happened to its frame, pinned pages keep theirs, relocations and
// frees carry the paging state, accesses across two pages and copies between two buffers larger
// than the pool work, a read back gives the guest lock up while the card works (another thread that
// touches the page waits for it), devices still fault, and the write watch sees the stores to a
// range (and only those: not reads, not a swap-out and read back) through either EA. Built twice:
// with a swap file, and without one (WL_XBRT_PAGING_NO_SWAP), where only zero and clean pages give
// frames back and a changed page that needs a frame is a guest crash.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/platform/host/fs_host.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/crash.h"
#include "wl/base/endian.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"

#define FRAMES 64u
#define PAGE WL_XB_PAGE

static bool g_up;
static char g_dir[256];

static void up(void) {
    if (g_up) {
        return;
    }
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/wl_xbrt_paging_XXXXXX", tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
    if (mkdtemp(g_dir) == NULL) {
        return;
    }
    wl_host_fs_set_root(WL_DEVICE_SD, g_dir);
    wl_platform_fs_mount(WL_DEVICE_SD);
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    cfg.ram_bytes = 16u << 20;
    cfg.frame_limit_bytes = FRAMES * PAGE;
#if defined(WL_XBRT_PAGING_NO_SWAP)
    cfg.swap_path = NULL;
#else
    cfg.swap_path = "sd:/pagefile.sys";
#endif
    if (wl_xb_mem_init(&cfg) == WL_OK && wl_xb_fault_install() == WL_OK) {
        g_up = true;
    }
}

static void cleanup(void) {
    char path[300];
    snprintf(path, sizeof path, "%s/pagefile.sys", g_dir);
    unlink(path);
    rmdir(g_dir);
}

static WlXbMemStats stats(void) {
    WlXbMemStats st;
    wl_xb_mem_stats(&st);
    return st;
}

// A word that tells page `i` of a pattern apart at offset `off`.
static uint32_t word(uint32_t seed, uint32_t i, uint32_t off) {
    return (seed * 0x9E3779B1u) ^ (i * 0x85EBCA6Bu) ^ (off * 0xC2B2AE35u);
}

WL_UNUSED static void fill(uint32_t va, uint32_t pages, uint32_t seed) {
    for (uint32_t i = 0; i < pages; i++) {
        for (uint32_t off = 0; off < PAGE; off += 1024u) {
            wl_xb_wr32(va + i * PAGE + off, word(seed, i, off));
        }
    }
}

WL_UNUSED static bool check(uint32_t va, uint32_t pages, uint32_t seed) {
    for (uint32_t i = 0; i < pages; i++) {
        for (uint32_t off = 0; off < PAGE; off += 1024u) {
            if (wl_xb_rd32(va + i * PAGE + off) != word(seed, i, off)) {
                fprintf(stderr, "page %u +0x%x: 0x%08x, not 0x%08x\n", (unsigned)i, (unsigned)off,
                        (unsigned)wl_xb_rd32(va + i * PAGE + off), (unsigned)word(seed, i, off));
                return false;
            }
        }
    }
    return true;
}

static uint32_t alloc(uint32_t pages) {
    uint32_t va = 0;
    return wl_xb_va_alloc(&va, pages * PAGE, 0x00400000u, WL_XB_PROT_READWRITE) == WL_OK ? va : 0u;
}

TEST paging_is_on(void) {
    up();
    ASSERT(g_up);
    WlXbMemStats st = stats();
    ASSERT_EQ(FRAMES, st.frames_total);
#if defined(WL_XBRT_PAGING_NO_SWAP)
    ASSERT_EQ(0u, st.swap_slots);
#else
    ASSERT_EQ((16u << 20) / PAGE, st.swap_slots);
#endif
    PASS();
}

TEST zero_pages_go_for_nothing(void) {
    up();
    uint32_t va = alloc(4u * FRAMES);
    ASSERT(va != 0);
    WlXbMemStats a = stats();
    for (uint32_t i = 0; i < 4u * FRAMES; i++) {
        ASSERT_EQ(0u, wl_xb_rd32(va + i * PAGE + 64u)); // read, never written: nothing to keep
    }
    WlXbMemStats b = stats();
    ASSERT(b.frames <= FRAMES);
    ASSERT(b.evictions - a.evictions >= 3u * FRAMES - 8u);
    ASSERT_EQ(b.evictions - a.evictions, b.clean_evictions - a.clean_evictions);
    ASSERT(b.fast_evictions - a.fast_evictions >= FRAMES); // the exception-safe path's (the Wii's DSI)
    ASSERT_EQ(a.swap_outs, b.swap_outs);
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST stores_are_watched(void) {
    up();
    uint32_t phys = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(4u * PAGE, 0, 0xFFFFFFFFu, 0, &phys));
    const uint32_t w = WL_XB_WINDOW | phys;
    fill(w, 4, 9);
    uint32_t stamp = wl_xb_phys_watch(phys, 4u * PAGE);
    ASSERT(stamp != 0);
    ASSERT_FALSE(wl_xb_phys_written(phys, 4u * PAGE, stamp));
    ASSERT(check(w, 4, 9)); // reads are not stores
    ASSERT_FALSE(wl_xb_phys_written(phys, 4u * PAGE, stamp));
    wl_xb_wr32(w + 2u * PAGE + 8u, 1u);
    ASSERT(wl_xb_phys_written(phys, 4u * PAGE, stamp));
    ASSERT_FALSE(wl_xb_phys_written(phys, 2u * PAGE, stamp)); // page granular
    ASSERT(wl_xb_phys_written(phys + 2u * PAGE + 100u, 4u, stamp));
    ASSERT(wl_xb_phys_written(phys, 4u * PAGE, 0)); // no stamp: written
    // armed again: nothing until the next store, which a page written before still shows
    stamp = wl_xb_phys_watch(phys, 4u * PAGE);
    ASSERT_FALSE(wl_xb_phys_written(phys, 4u * PAGE, stamp));
    wl_xb_wr32(w + 2u * PAGE + 12u, 2u);
    ASSERT(wl_xb_phys_written(phys + 2u * PAGE, PAGE, stamp));
    // and once looked at, a store after the look is still seen
    stamp = wl_xb_phys_watch(phys, 4u * PAGE);
    ASSERT_FALSE(wl_xb_phys_written(phys, 4u * PAGE, stamp));
    wl_xb_wr32(w + 3u * PAGE + 8u, 3u);
    ASSERT(wl_xb_phys_written(phys, 4u * PAGE, stamp));
    wl_xb_wr32(w + 3u * PAGE + 12u, 4u);
    ASSERT(wl_xb_phys_written(phys + 3u * PAGE, 4u, stamp));
    // a user page, stored to through its user EA, watched through its physical address
    uint32_t va = alloc(1), uphys = 0;
    ASSERT(va != 0);
    ASSERT(wl_xb_va_to_phys(va, &uphys));
    wl_xb_wr32(va, 5u);
    stamp = wl_xb_phys_watch(uphys, PAGE);
    ASSERT_EQ(5u, wl_xb_rd32(WL_XB_WINDOW | uphys));
    ASSERT_FALSE(wl_xb_phys_written(uphys, PAGE, stamp));
    wl_xb_wr32(va + 4u, 6u);
    ASSERT(wl_xb_phys_written(uphys, PAGE, stamp));
#if !defined(WL_XBRT_PAGING_NO_SWAP)
    {
        // out to the swap file and read back: no store
        stamp = wl_xb_phys_watch(phys, 4u * PAGE);
        const uint32_t n = 3u * FRAMES;
        uint32_t junk = alloc(n);
        ASSERT(junk != 0);
        fill(junk, n, 10);
        ASSERT_FALSE(wl_xb_mem_resident(w));
        ASSERT(check(w, 4, 9));
        ASSERT_FALSE(wl_xb_phys_written(phys, 4u * PAGE, stamp));
        // a store, then out to the swap file before the look: seen
        wl_xb_wr32(w + PAGE + 8u, 7u);
        fill(junk, n, 11);
        ASSERT_FALSE(wl_xb_mem_resident(w + PAGE));
        ASSERT(wl_xb_phys_written(phys + PAGE, PAGE, stamp));
        ASSERT_FALSE(wl_xb_phys_written(phys, PAGE, stamp));
        ASSERT_EQ(7u, wl_xb_rd32(w + PAGE + 8u));
        ASSERT_EQ(WL_OK, wl_xb_va_release(junk, NULL));
    }
#endif
    // a pinned page: the host keeps it open for writing, so it cannot be watched
    ASSERT_EQ(WL_OK, wl_xb_mem_pin(va, 4u));
    ASSERT_EQ(0u, wl_xb_phys_watch(uphys, PAGE));
    wl_xb_mem_unpin(va, 4u);
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    // freed: its bytes are zero now, which counts as written
    stamp = wl_xb_phys_watch(phys, 4u * PAGE);
    ASSERT(stamp != 0);
    ASSERT_EQ(WL_OK, wl_xb_phys_free(phys));
    ASSERT(wl_xb_phys_written(phys, PAGE, stamp));
    // outside the RAM: no watch
    ASSERT_EQ(0u, wl_xb_phys_watch(16u << 20, PAGE));
    ASSERT(wl_xb_phys_written(16u << 20, PAGE, 1u));
    PASS();
}

// The epoch is 32 bits and wraps: a store just after the wrap is still seen by a watch armed just
// before it, and a watch older than the comparison's window counts as written.
TEST watch_survives_the_epoch_wrap(void) {
    up();
    uint32_t phys = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(2u * PAGE, 0, 0xFFFFFFFFu, 0, &phys));
    const uint32_t w = WL_XB_WINDOW | phys;
    fill(w, 2, 11);
    g_xb_pg_epoch = 0xFFFFFFF0u;
    uint32_t stamp = wl_xb_phys_watch(phys, 2u * PAGE);
    ASSERT(stamp >= 0xFFFFFFF0u);
    ASSERT_FALSE(wl_xb_phys_written(phys, 2u * PAGE, stamp));
    for (uint32_t i = 0; i < 40u; i++) { // past the wrap: stores to the second page, one watch
        wl_xb_wr32(w + PAGE + 4u * i, i);
        ASSERT(wl_xb_phys_written(phys + PAGE, PAGE, stamp));
        (void)wl_xb_phys_watch(phys + PAGE, PAGE); // re-armed: the epoch moves on
    }
    ASSERT(g_xb_pg_epoch < 0x1000u); // wrapped
    ASSERT_FALSE(wl_xb_phys_written(phys, PAGE, stamp)); // the first page: not stored to
    uint32_t now = wl_xb_phys_watch(phys, 2u * PAGE);
    ASSERT_FALSE(wl_xb_phys_written(phys, 2u * PAGE, now));
    wl_xb_wr32(w + 8u, 7u);
    ASSERT(wl_xb_phys_written(phys, PAGE, now));
    // a stamp from more than the window ago: written, whatever the pages say
    ASSERT(wl_xb_phys_written(phys + PAGE, PAGE, now - WL_XB_PG_EPOCH_WINDOW));
    ASSERT_EQ(WL_OK, wl_xb_phys_free(phys));
    PASS();
}

// The freeze watch's reads (wl_xb_mem_peek, without the guest lock): a resident page's bytes, and
// nothing (no fault, no swap-in) for a page swapped out or in swap I/O.
TEST peeks_never_fault(void) {
    up();
    const uint32_t n = 4u;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    fill(va, n, 9);
    uint8_t b[4];
    for (uint32_t i = 0; i < n; i++) {
        ASSERT(wl_xb_mem_peek(va + i * PAGE + 1024u, b, 4));
        ASSERT_EQ_FMT(word(9, i, 1024u), wl_rd_le32(b), "%08x");
    }
    ASSERT(!wl_xb_mem_peek(va + PAGE - 2u, b, 4)); // across two pages: refused
    uint32_t phys = 0;
    ASSERT(wl_xb_va_to_phys(va, &phys));
    uint32_t p = phys >> WL_XB_PAGE_SHIFT;
    g_xb_pg_flags[p] |= WL_XB_PG_IO; // as while its frame is written to the swap file
    ASSERT(!wl_xb_mem_peek(va, b, 4));
    g_xb_pg_flags[p] &= (uint8_t)~WL_XB_PG_IO;
    ASSERT(wl_xb_mem_peek(va, b, 4));
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
#if !defined(WL_XBRT_PAGING_NO_SWAP)
    // more pages than frames: the ones swapped out read as nothing, and the peeks bring none back
    const uint32_t m = 4u * FRAMES;
    va = alloc(m);
    ASSERT(va != 0);
    fill(va, m, 10);
    WlXbMemStats a = stats();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < m; i++) {
        bool res = wl_xb_mem_resident(va + i * PAGE);
        bool ok = wl_xb_mem_peek(va + i * PAGE, b, 4);
        ASSERT_EQ(res, ok);
        if (ok) {
            ASSERT_EQ_FMT(word(10, i, 0), wl_rd_le32(b), "%08x");
            seen++;
        }
    }
    WlXbMemStats c = stats();
    ASSERT_EQ(a.swap_ins, c.swap_ins);
    ASSERT(seen > 0 && seen <= FRAMES);
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
#endif
    PASS();
}

#if !defined(WL_XBRT_PAGING_NO_SWAP)

TEST changed_pages_swap_out_and_back(void) {
    up();
    const uint32_t n = 8u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    WlXbMemStats a = stats();
    fill(va, n, 1);
    ASSERT(check(va, n, 1));
    ASSERT(check(va, n, 1));
    WlXbMemStats b = stats();
    ASSERT(b.frames <= FRAMES);
    ASSERT(b.swap_outs - a.swap_outs >= n - FRAMES);
    ASSERT(b.swap_ins - a.swap_ins >= n - FRAMES);
    ASSERT(b.swap_writes - a.swap_writes < (b.swap_outs - a.swap_outs) / 4u); // batched
    ASSERT_EQ((uint64_t)(b.swap_outs - a.swap_outs) * PAGE, b.swap_write_bytes - a.swap_write_bytes);
    ASSERT_EQ((uint64_t)(b.swap_ins - a.swap_ins) * PAGE, b.swap_read_bytes - a.swap_read_bytes);
    ASSERT(b.swap_slots_used >= n - FRAMES);
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    WlXbMemStats c = stats();
    ASSERT(c.swap_slots_used + n - FRAMES <= b.swap_slots_used + FRAMES); // the slots came back
    PASS();
}

TEST clean_pages_read_back_go_for_nothing(void) {
    up();
    const uint32_t n = 6u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    fill(va, n, 2);
    ASSERT(check(va, n, 2)); // every page read back once: clean copies of their slots
    WlXbMemStats a = stats();
    ASSERT(check(va, n, 2)); // only clean pages to take frames from: no write
    WlXbMemStats b = stats();
    ASSERT_EQ(a.swap_outs, b.swap_outs);
    ASSERT(b.swap_ins - a.swap_ins >= n - FRAMES);
    ASSERT(b.clean_evictions - a.clean_evictions >= n - FRAMES);
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST both_eas_see_one_page(void) {
    up();
    const uint32_t n = 4u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    fill(va, n, 3);
    // through the window, after most of them went to the swap file
    for (uint32_t i = 0; i < n; i++) {
        uint32_t phys = 0;
        ASSERT(wl_xb_va_to_phys(va + i * PAGE, &phys));
        ASSERT_EQ(word(3, i, 1024u), wl_xb_rd32(WL_XB_WINDOW | (phys + 1024u)));
        wl_xb_wr32(WL_XB_TILED | (phys + 2048u), 0xA5000000u | i); // and back the other way
    }
    for (uint32_t i = 0; i < n; i++) {
        ASSERT_EQ(0xA5000000u | i, wl_xb_rd32(va + i * PAGE + 2048u));
        ASSERT_EQ(word(3, i, 3072u), wl_xb_rd32(va + i * PAGE + 3072u));
    }
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST pinned_pages_keep_their_frames(void) {
    up();
    uint32_t pin = alloc(2);
    ASSERT(pin != 0);
    ASSERT_EQ(WL_OK, wl_xb_mem_pin(pin, PAGE + 8u)); // two pages
    ASSERT(wl_xb_mem_resident(pin) && wl_xb_mem_resident(pin + PAGE));
    ASSERT_EQ(2u, stats().pinned_pages);
    wl_xb_wr32(pin + 16u, 0x5EED0001u);
    const uint32_t n = 4u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    fill(va, n, 4);
    ASSERT(wl_xb_mem_resident(pin) && wl_xb_mem_resident(pin + PAGE));
    ASSERT(check(va, n, 4));
    ASSERT(wl_xb_mem_resident(pin));
    ASSERT_EQ(0x5EED0001u, wl_xb_rd32(pin + 16u));
    // the window of a pinned page is open too (a host thread may write it without the lock)
    uint32_t phys = 0;
    ASSERT(wl_xb_va_to_phys(pin, &phys));
    ASSERT_EQ(0x5EED0001u, wl_xb_rd32(WL_XB_WINDOW | (phys + 16u)));
    // at most half the frames
    uint32_t big = alloc(FRAMES);
    ASSERT(big != 0);
    ASSERT_EQ(WL_ERR_NOMEM, wl_xb_mem_pin(big, FRAMES * PAGE));
    wl_xb_mem_unpin(big, FRAMES * PAGE);
    wl_xb_mem_unpin(pin, 2u * PAGE);
    ASSERT_EQ(0u, stats().pinned_pages);
    ASSERT(check(va, n, 4)); // the unpinned pages may go now, their bytes kept
    ASSERT_EQ(0x5EED0001u, wl_xb_rd32(pin + 16u));
    ASSERT_EQ(WL_ERR_RANGE, wl_xb_mem_pin(0x0F000000u, 4)); // not committed
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    ASSERT_EQ(WL_OK, wl_xb_va_release(big, NULL));
    ASSERT_EQ(WL_OK, wl_xb_va_release(pin, NULL));
    PASS();
}

TEST relocation_carries_swapped_pages(void) {
    up();
    uint32_t va = alloc(1);
    ASSERT(va != 0);
    wl_xb_wr32(va + 0x100u, 0xCAFEF00Du);
    uint32_t p0 = 0;
    ASSERT(wl_xb_va_to_phys(va, &p0));
    // pushed out to the swap file
    const uint32_t n = 3u * FRAMES;
    uint32_t junk = alloc(n);
    ASSERT(junk != 0);
    fill(junk, n, 5);
    ASSERT(!wl_xb_mem_resident(va));
    // a pinned range over its physical page: the page moves, its slot with it
    uint32_t got = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(PAGE, p0, p0 + PAGE - 1u, 0, &got));
    ASSERT_EQ_FMT(p0, got, "%x");
    uint32_t p1 = 0;
    ASSERT(wl_xb_va_to_phys(va, &p1));
    ASSERT(p1 != p0);
    ASSERT_EQ_FMT(0xCAFEF00Du, wl_xb_rd32(va + 0x100u), "%08x");
    ASSERT_EQ_FMT(0xCAFEF00Du, wl_xb_rd32(WL_XB_WINDOW | (p1 + 0x100u)), "%08x");
    ASSERT_EQ_FMT(0u, wl_xb_rd32(WL_XB_WINDOW | (p0 + 0x100u)), "%08x"); // the pinned page is clean
    // and a resident one moves too
    wl_xb_wr32(va + 0x200u, 0x12345678u);
    ASSERT(wl_xb_mem_resident(va));
    uint32_t got2 = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(PAGE, p1, p1 + PAGE - 1u, 0, &got2));
    ASSERT_EQ_FMT(0x12345678u, wl_xb_rd32(va + 0x200u), "%08x");
    ASSERT_EQ_FMT(0xCAFEF00Du, wl_xb_rd32(va + 0x100u), "%08x");
    ASSERT(check(junk, n, 5));
    ASSERT_EQ(WL_OK, wl_xb_phys_free(got));
    ASSERT_EQ(WL_OK, wl_xb_phys_free(got2));
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    ASSERT_EQ(WL_OK, wl_xb_va_release(junk, NULL));
    PASS();
}

TEST freed_pages_come_back_zero(void) {
    up();
    const uint32_t n = 3u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    fill(va, n, 6);
    WlXbMemStats a = stats();
    ASSERT_EQ(WL_OK, wl_xb_va_decommit(va, n * PAGE));
    WlXbMemStats b = stats();
    ASSERT(b.swap_slots_used + (n - FRAMES) <= a.swap_slots_used + 8u);
    ASSERT(b.frames_free > a.frames_free);
    ASSERT_EQ(WL_OK, wl_xb_va_commit(va, n * PAGE, WL_XB_PROT_READWRITE));
    for (uint32_t i = 0; i < n; i++) {
        ASSERT_EQ(0u, wl_xb_rd32(va + i * PAGE));
    }
    // contiguous memory too
    uint32_t phys = 0;
    ASSERT_EQ(WL_OK, wl_xb_phys_alloc(n * PAGE, 0, 0xFFFFFFFFu, 0, &phys));
    fill(WL_XB_WINDOW | phys, n, 7);
    ASSERT(check(WL_XB_WINDOW | phys, n, 7));
    ASSERT_EQ(WL_OK, wl_xb_phys_free(phys));
    ASSERT_EQ(0u, wl_xb_rd32(WL_XB_WINDOW | (phys + PAGE)));
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST cleared_pages_go_out_as_zero(void) {
    up();
    const uint32_t n = 3u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    WlXbMemStats a = stats();
    for (uint32_t i = 0; i < n; i++) {
        wl_xb_wr32(va + i * PAGE + 128u, 0x5A5A5A5Au); // changed, then zero again
        wl_xb_wr32(va + i * PAGE + 128u, 0u);
    }
    WlXbMemStats b = stats();
    ASSERT(b.zero_outs - a.zero_outs >= n - FRAMES - 8u); // their frames back with nothing written
    for (uint32_t i = 0; i < n; i++) {
        ASSERT_EQ(0u, wl_xb_rd32(va + i * PAGE + 128u));
    }
    WlXbMemStats c = stats();
    ASSERT_EQ(b.swap_ins, c.swap_ins); // nothing to read back either
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

TEST file_reads_spare_swap_ins(void) {
    up();
    const uint32_t n = 2u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    fill(va, n, 8); // the first pages are in the swap file now
    static uint8_t src[4u * PAGE];
    for (uint32_t i = 0; i < sizeof src; i++) {
        src[i] = (uint8_t)(i * 7u + 1u);
    }
    // whole pages: not read back; the partial pages at either end keep their other bytes
    WlXbMemStats a = stats();
    wl_xb_copy_in(va + 100u, src, 3u * PAGE);
    WlXbMemStats b = stats();
    ASSERT_EQ(2u, b.swap_ins_spared - a.swap_ins_spared);
    ASSERT_EQ(2u, b.swap_ins - a.swap_ins);
    static uint8_t got[3u * PAGE];
    wl_xb_copy_out(got, va + 100u, sizeof got);
    ASSERT_MEM_EQ(src, got, sizeof got);
    ASSERT_EQ(word(8, 0, 0), wl_xb_rd32(va));
    ASSERT_EQ(word(8, 3, 1024), wl_xb_rd32(va + 3u * PAGE + 1024u));
    for (uint32_t i = 4; i < n; i++) { // the rest of the buffer as it was
        ASSERT_EQ(word(8, i, 2048), wl_xb_rd32(va + i * PAGE + 2048u));
    }
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}

#if !defined(WL_XBRT_PAGING_NO_SWAP)
// A file of `pages` pages of pattern `seed` as the guest reads it (little-endian words), open
// read-write.
static WlPlatformFile *pattern_file(const char *path, uint32_t pages, uint32_t seed) {
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(path, WL_FILE_READ_WRITE, &f) != WL_OK) {
        return NULL;
    }
    static uint8_t page[PAGE];
    for (uint32_t i = 0; i < pages; i++) {
        memset(page, 0, sizeof page);
        for (uint32_t off = 0; off < PAGE; off += 1024u) {
            wl_wr_le32(page + off, word(seed, i, off));
        }
        if (wl_platform_fs_write_at(f, (uint64_t)i * PAGE, page, PAGE) != WL_OK) {
            wl_platform_fs_close(f);
            return NULL;
        }
    }
    return f;
}

// A file read as the kernel's: `pages` pages of `f` from page `at` to `va` in 64 KB chunks, then
// the whole pages remember the file.
static void file_read(uint32_t va, WlPlatformFile *f, uint32_t at, uint32_t pages) {
    static uint8_t buf[16u * PAGE];
    for (uint32_t i = 0; i < pages; i += 16u) {
        uint32_t k = pages - i < 16u ? pages - i : 16u;
        if (wl_platform_fs_read_at(f, (uint64_t)(at + i) * PAGE, buf, k * PAGE) == WL_OK) {
            wl_xb_copy_in(va + i * PAGE, buf, k * PAGE);
        }
    }
    wl_xb_file_pages(va, pages * PAGE, f, (uint64_t)at * PAGE);
}

TEST file_pages_need_no_swap_out(void) {
    up();
    const uint32_t n = 2u * FRAMES;
    WlPlatformFile *f = pattern_file("sd:/filed.bin", n, 21);
    ASSERT(f != NULL);
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    WlXbMemStats a = stats();
    file_read(va, f, 0, n); // (pages taken back during the read are written out, then forget it)
    WlXbMemStats b = stats();
    ASSERT_EQ(n, b.file_copies - a.file_copies);
    // twice the pool read twice: each page comes back from the file and goes with nothing written
    ASSERT(check(va, n, 21));
    ASSERT(check(va, n, 21));
    WlXbMemStats c = stats();
    ASSERT_EQ(0u, c.swap_outs - b.swap_outs);
    ASSERT_EQ(0u, c.swap_ins - b.swap_ins);
    ASSERT((c.file_ins + c.file_ahead) - (b.file_ins + b.file_ahead) >= n);
    ASSERT(c.file_ahead > b.file_ahead); // read in order: the pages after each come in its read
    ASSERT(c.file_evictions - b.file_evictions >= n);
    // a read over pages that went: not read back from the file to be overwritten
    file_read(va, f, 0, n);
    WlXbMemStats d = stats();
    ASSERT_EQ(c.file_ins, d.file_ins);
    ASSERT_EQ(c.file_ahead, d.file_ahead);
    ASSERT(check(va, n, 21));
    // freed: the file's pages are gone with the memory, nothing is left to forget
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    WlXbMemStats e = stats();
    ASSERT_EQ(WL_OK, wl_xb_file_forget(f, 0, UINT64_MAX));
    WlXbMemStats g = stats();
    ASSERT_EQ(e.file_forgets, g.file_forgets);
    wl_platform_fs_close(f);
    wl_platform_fs_remove("sd:/filed.bin");
    PASS();
}

TEST file_pages_forget_their_file(void) {
    up();
    const uint32_t n = 2u * FRAMES, half = FRAMES;
    WlPlatformFile *f = pattern_file("sd:/forget.bin", n, 22);
    ASSERT(f != NULL);
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    file_read(va, f, 0, n);
    // a store: that page is no longer the file's (written to the swap file if it goes)
    wl_xb_wr32(va + 5u * PAGE + 8u, 0xC0FFEEu);
    ASSERT(check(va, n, 22)); // (every page taken back once)
    WlXbMemStats b = stats();
    ASSERT_EQ(0xC0FFEEu, wl_xb_rd32(va + 5u * PAGE + 8u));
    // its first half written over: forgotten first, the guest keeps the bytes it read
    ASSERT_EQ(WL_OK, wl_xb_file_forget(f, 0, (uint64_t)half * PAGE));
    WlXbMemStats c = stats();
    ASSERT(c.file_forgets - b.file_forgets >= half - 1u);
    ASSERT(c.file_forget_ins > b.file_forget_ins);
    static uint8_t page[PAGE];
    memset(page, 0x5A, sizeof page);
    for (uint32_t i = 0; i < half; i++) {
        ASSERT_EQ(WL_OK, wl_platform_fs_write_at(f, (uint64_t)i * PAGE, page, PAGE));
    }
    ASSERT(check(va, n, 22));
    ASSERT_EQ(0xC0FFEEu, wl_xb_rd32(va + 5u * PAGE + 8u));
    // a forget of a range that starts inside a page takes that page too
    ASSERT_EQ(WL_OK, wl_xb_file_forget(f, (uint64_t)(half + 3u) * PAGE + 100u, 1));
    WlXbMemStats d = stats();
    ASSERT_EQ(1u, d.file_forgets - c.file_forgets);
    // cut short past the half and three pages: what it held stays
    ASSERT_EQ(WL_OK, wl_xb_file_forget(f, (uint64_t)(half + 4u) * PAGE, UINT64_MAX));
    ASSERT_EQ(WL_OK, wl_platform_fs_truncate(f, (uint64_t)(half + 4u) * PAGE));
    ASSERT(check(va, n, 22));
    // closed (and gone): every page forgotten, none reads from it again
    ASSERT_EQ(WL_OK, wl_xb_file_forget(f, 0, UINT64_MAX));
    wl_platform_fs_close(f);
    wl_platform_fs_remove("sd:/forget.bin");
    WlXbMemStats e = stats();
    ASSERT(check(va, n, 22));
    ASSERT(check(va, n, 22));
    ASSERT_EQ(0xC0FFEEu, wl_xb_rd32(va + 5u * PAGE + 8u));
    WlXbMemStats g = stats();
    ASSERT_EQ(e.file_ins, g.file_ins);
    ASSERT(g.swap_ins > e.swap_ins);
    ASSERT_EQ(WL_OK, wl_xb_va_release(va, NULL));
    PASS();
}
#endif

TEST across_pages_and_big_copies(void) {
    up();
    const uint32_t n = 2u * FRAMES; // each buffer twice the pool
    uint32_t a = alloc(n), b = alloc(n);
    ASSERT(a != 0 && b != 0);
    fill(a, n, 8);
    for (uint32_t i = 1; i < n; i += 7u) {
        wl_xb_wr64(a + i * PAGE - 4u, 0x0102030405060708ull ^ i); // across two pages
    }
    // the host's own memcpy between two guest buffers, each larger than the frames
    memcpy(wl_xb_ptr(b), wl_xb_ptr(a), n * PAGE);
    for (uint32_t i = 1; i < n; i += 7u) {
        ASSERT_EQ(0x0102030405060708ull ^ i, wl_xb_rd64(b + i * PAGE - 4u));
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = (i % 7u == 1u) ? 1024u : 0u; // (the words the 64-bit stores did not cover)
        ASSERT_EQ(word(8, i, off), wl_xb_rd32(b + i * PAGE + off));
    }
    ASSERT(memcmp(wl_xb_ptr(a), wl_xb_ptr(b), n * PAGE) == 0);
    ASSERT_EQ(WL_OK, wl_xb_va_release(a, NULL));
    ASSERT_EQ(WL_OK, wl_xb_va_release(b, NULL));
    PASS();
}

// A file written straight from guest memory (the GPU capture's blocks), its pages swapped out: the
// bytes go through a bounce buffer, as the OS's write of a page without a frame fails instead of
// faulting it in.
TEST files_written_from_paged_out_memory(void) {
    up();
    const uint32_t n = 2u * FRAMES;
    uint32_t a = alloc(n);
    ASSERT(a != 0);
    fill(a, n, 9);
    ASSERT(!wl_xb_mem_resident(a)); // the first pages went to the swap file for the last ones
    WlPlatformFile *f = NULL;
    ASSERT_EQ(WL_OK, wl_platform_fs_open("sd:/capture.bin", WL_FILE_WRITE_NEW, &f));
    ASSERT_EQ(WL_OK, wl_xb_file_write(f, wl_xb_ptr(a), n * PAGE));
    uint32_t host_word = 0x5A5AA5A5u;
    ASSERT_EQ(WL_OK, wl_xb_file_write(f, &host_word, 4)); // host memory as it is
    wl_platform_fs_close(f);
    char path[300];
    snprintf(path, sizeof path, "%s/capture.bin", g_dir);
    FILE *h = fopen(path, "rb");
    ASSERT(h != NULL);
    uint8_t buf[PAGE];
    bool same = true;
    for (uint32_t i = 0; i < n && same; i++) {
        same = fread(buf, 1, PAGE, h) == PAGE;
        for (uint32_t off = 0; same && off < PAGE; off += 1024u) {
            same = wl_rd_le32(buf + off) == word(9, i, off);
        }
    }
    uint32_t tail = 0;
    same = same && fread(&tail, 1, 4, h) == 4 && tail == host_word;
    fclose(h);
    unlink(path);
    ASSERT(same);
    ASSERT_EQ(WL_OK, wl_xb_va_release(a, NULL));
    PASS();
}

// A thread's read back of a page gives the guest lock up while the card works (a real Wii's card
// held every thread, the vertical blank and the audio off for seconds while a10's opening cutscene
// paged its map in): here another host thread holds the card for 150 ms while the boot thread
// touches a page that went back to its file. A second guest thread runs meanwhile and touches the
// same page: it waits for that read rather than reading it again or taking its frame.
static volatile uint32_t g_card_state, g_go, g_seen_card, g_toucher_ok, g_toucher_done;
static uint32_t g_touch_va, g_touch_want;
static _Alignas(32) uint8_t g_card_stack[64 * 1024];

static void *card_holder(void *arg) {
    (void)arg;
    wl_xb_card_lock();
    g_card_state = 1;
    wl_platform_thread_sleep_ms(150);
    g_card_state = 2;
    wl_xb_card_unlock();
    return NULL;
}

static void toucher(WlXbThread *self, void *user) {
    (void)user;
    while (g_go == 0) {
        (void)wl_xb_wait(self, 1);
    }
    g_seen_card = g_card_state; // 1: it ran while the boot thread waited for the card
    g_toucher_ok = wl_xb_rd32(g_touch_va) == g_touch_want ? 1u : 0u;
    g_toucher_done = 1;
}

TEST read_back_gives_the_lock_up(void) {
    up();
    WlXbThread *boot = NULL;
    WlXbThreadConfig tc;
    wl_xb_thread_config_default(&tc);
    ASSERT_EQ(WL_OK, wl_xb_threads_init(&tc, &boot));
    ASSERT_EQ(WL_OK, wl_xb_thread_setup_guest(boot, 0x10000u, 0, 0, 0));
    const uint32_t n = 2u * FRAMES;
    WlPlatformFile *f = pattern_file("sd:/transit.bin", n, 33);
    ASSERT(f != NULL);
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    file_read(va, f, 0, n);
    ASSERT(check(va, n, 33)); // the first pages went back to the file for the last ones
    ASSERT(!wl_xb_mem_resident(va));
    WlXbThread *t = NULL;
    ASSERT_EQ(WL_OK, wl_xb_thread_create_host("toucher", toucher, NULL, 0x4000u,
                                              WL_XB_PRIORITY_NORMAL, &t));
    g_touch_va = va;
    g_touch_want = word(33, 0, 0);
    WlPlatformThread holder;
    ASSERT_EQ(WL_OK, wl_platform_thread_create(&holder, card_holder, NULL, g_card_stack,
                                               sizeof g_card_stack, WL_THREAD_PRIO_MAIN));
    while (g_card_state == 0) {
        wl_platform_thread_sleep_ms(1);
    }
    WlXbMemStats a = stats();
    g_go = 1;
    uint32_t got =
        wl_xb_rd32(va); // read back from the file: the lock given up while the card is held
    WlXbMemStats b = stats();
    ASSERT_EQ_FMT(g_touch_want, got, "%08x");
    ASSERT_EQ_FMT(2u, g_card_state, "%u"); // the read waited for the card
    uint64_t t0 = wl_platform_time_us();
    while (g_toucher_done == 0 && wl_platform_time_us() - t0 < 3000000u) {
        (void)wl_xb_wait(boot, 1);
    }
    ASSERT_EQ_FMT(1u, (unsigned)g_toucher_done, "%u");
    ASSERT_EQ_FMT(1u, (unsigned)g_seen_card, "%u"); // the other thread ran meanwhile
    ASSERT_EQ_FMT(1u, (unsigned)g_toucher_ok, "%u");
    ASSERT_EQ_FMT(1u, b.file_ins - a.file_ins, "%u");           // read once
    ASSERT_EQ_FMT(1u, b.transit_waits - a.transit_waits, "%u"); // the other thread waited for it
    ASSERT(b.io_unlocked > a.io_unlocked);
    ASSERT_EQ(b.io_held_us, a.io_held_us);
    ASSERT(check(va, n, 33));
    (void)wl_platform_thread_join(holder, NULL);
    wl_platform_fs_close(f);
    wl_platform_fs_remove("sd:/transit.bin");
    PASS();
}

#else // no swap file

static sigjmp_buf g_crash_jmp;
static WlXbCrash g_crash;

static void crash_stop(const WlXbCrash *c) {
    g_crash = *c;
    siglongjmp(g_crash_jmp, 1);
}

TEST changed_pages_need_a_swap_file(void) {
    up();
    const uint32_t n = 2u * FRAMES;
    uint32_t va = alloc(n);
    ASSERT(va != 0);
    wl_xb_crash_set_stop(crash_stop);
    memset(&g_crash, 0, sizeof g_crash);
    volatile uint32_t reached = 0;
    if (sigsetjmp(g_crash_jmp, 1) == 0) {
        for (uint32_t i = 0; i < n; i++) {
            wl_xb_wr32(va + i * PAGE, i + 1u);
            reached = i + 1u;
        }
        FAILm("every page got a frame");
    }
    ASSERT(reached >= FRAMES - 4u && reached <= FRAMES);
    ASSERT(g_crash.reason != NULL && strstr(g_crash.reason, "no frame left") != NULL);
    wl_xb_crash_set_stop(NULL);
    PASS();
}

#endif

SUITE(paging) {
    RUN_TEST(paging_is_on);
    RUN_TEST(zero_pages_go_for_nothing);
    RUN_TEST(stores_are_watched);
    RUN_TEST(peeks_never_fault);
    RUN_TEST(watch_survives_the_epoch_wrap);
#if !defined(WL_XBRT_PAGING_NO_SWAP)
    RUN_TEST(changed_pages_swap_out_and_back);
    RUN_TEST(clean_pages_read_back_go_for_nothing);
    RUN_TEST(both_eas_see_one_page);
    RUN_TEST(pinned_pages_keep_their_frames);
    RUN_TEST(relocation_carries_swapped_pages);
    RUN_TEST(freed_pages_come_back_zero);
    RUN_TEST(cleared_pages_go_out_as_zero);
    RUN_TEST(file_reads_spare_swap_ins);
    RUN_TEST(file_pages_need_no_swap_out);
    RUN_TEST(file_pages_forget_their_file);
    RUN_TEST(across_pages_and_big_copies);
    RUN_TEST(files_written_from_paged_out_memory);
    RUN_TEST(read_back_gives_the_lock_up); // (last: the boot thread becomes a guest thread)
#else
    RUN_TEST(changed_pages_need_a_swap_file);
#endif
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(paging);
    cleanup();
    GREATEST_MAIN_END();
}
