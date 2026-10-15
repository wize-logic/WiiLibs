// SPDX-License-Identifier: MIT
// Demand paging of guest memory on the host / ppc-be backend under a frame limit: the Wii's model
// (paging.c's clock and swap file) through SIGSEGV and mprotect. Zero pages give their frames back
// for nothing, changed pages go to the swap file in batches and come back on their next touch,
// clean pages read back cost no write when they go again, both EAs of a page see one page
// whatever happened to its frame, pinned pages keep theirs, relocations and frees carry the
// paging state, accesses across two pages and copies between two buffers larger than the pool
// work, devices still fault, and the write watch sees the stores to a range (and only those: not
// reads, not a swap-out and read back) through either EA. Built twice: with a swap file, and without one
// (WL_XBRT_PAGING_NO_SWAP), where only zero and clean pages give frames back and a changed page
// that needs a frame is a guest crash.
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
#if !defined(WL_XBRT_PAGING_NO_SWAP)
    RUN_TEST(changed_pages_swap_out_and_back);
    RUN_TEST(clean_pages_read_back_go_for_nothing);
    RUN_TEST(both_eas_see_one_page);
    RUN_TEST(pinned_pages_keep_their_frames);
    RUN_TEST(relocation_carries_swapped_pages);
    RUN_TEST(freed_pages_come_back_zero);
    RUN_TEST(across_pages_and_big_copies);
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
