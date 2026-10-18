// SPDX-License-Identifier: MIT
// Guest frames lent by the heap (frame_loan.h), on the host's simulation of the Wii's paging: frames
// leave the paging with the guest's bytes kept (changed pages moved, clean and zero ones dropped),
// the paging goes on in the frames left, a page held in place makes a retirement fail whole, and
// with the heap's cap standing for the Wii's heap, allocations past it get the frames back and none
// fails.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/platform/host/fs_host.h"
#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/frame_loan.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"

#define FRAMES 256u // the paging's (1 MB)
#define PAGE WL_XB_PAGE

static bool g_up;
static char g_dir[256];

static void up(void) {
    if (g_up) {
        return;
    }
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/wl_xbrt_loan_XXXXXX", tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
    if (mkdtemp(g_dir) == NULL) {
        return;
    }
    wl_host_fs_set_root(WL_DEVICE_SD, g_dir);
    wl_platform_fs_mount(WL_DEVICE_SD);
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    cfg.ram_bytes = 16u << 20;
    cfg.frame_limit_bytes = FRAMES * PAGE;
    cfg.page_cache_bytes = 0;
    cfg.swap_path = "sd:/pagefile.sys";
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

static uint32_t word(uint32_t seed, uint32_t i, uint32_t off) {
    return (seed * 0x9E3779B1u) ^ (i * 0x85EBCA6Bu) ^ (off * 0xC2B2AE35u) ^ 1u; // never zero together
}

static void fill(uint32_t va, uint32_t pages, uint32_t seed) {
    for (uint32_t i = 0; i < pages; i++) {
        for (uint32_t off = 0; off < PAGE; off += 512u) {
            wl_xb_wr32(va + i * PAGE + off, word(seed, i, off));
        }
    }
}

static bool check(uint32_t va, uint32_t pages, uint32_t seed) {
    for (uint32_t i = 0; i < pages; i++) {
        for (uint32_t off = 0; off < PAGE; off += 512u) {
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

// Pages with frames now: the pool's less the free ones and the retired.
static uint32_t resident(uint32_t retired) {
    WlXbMemStats st = stats();
    return st.frames_total - st.frames_free - retired;
}

static uint32_t g_retired;

TEST frames_leave_with_the_bytes_kept(void) {
    up();
    ASSERT(g_up);
    // every frame taken: changed pages, zero pages read (zero-filled, clean), changed pages zero again
    uint32_t va = alloc(FRAMES + 64u);
    ASSERT(va != 0);
    fill(va, FRAMES - 64u, 1u);
    uint32_t zva = va + (FRAMES - 64u) * PAGE;
    for (uint32_t i = 0; i < 32u; i++) {
        ASSERT_EQ(0u, wl_xb_rd32(zva + i * PAGE));
        wl_xb_wr32(zva + (32u + i) * PAGE, 0u); // changed, still zero
    }
    ASSERT_EQ(0u, stats().frames_free);
    // the top 64 frames, then the next 64
    uint32_t moved = 0;
    ASSERT(wl_xb_pg_retire(FRAMES - 63u, 64u, &moved));
    ASSERT(wl_xb_pg_retire(FRAMES - 127u, 64u, &moved));
    g_retired = 128u;
    ASSERT(moved > 0u);
    ASSERT(resident(g_retired) <= FRAMES - g_retired);
    ASSERT(check(va, FRAMES - 64u, 1u));
    for (uint32_t i = 0; i < 64u; i++) {
        ASSERT_EQ(0u, wl_xb_rd32(zva + i * PAGE));
    }
    // the paging goes on in the frames left: three times as many pages as frames, all kept
    uint32_t vb = alloc(3u * (FRAMES - g_retired));
    ASSERT(vb != 0);
    fill(vb, 3u * (FRAMES - g_retired), 2u);
    ASSERT(check(vb, 3u * (FRAMES - g_retired), 2u));
    ASSERT(check(va, FRAMES - 64u, 1u));
    ASSERT_EQ(0u, stats().frames_free); // (no retired frame came back)
    ASSERT(resident(g_retired) <= FRAMES - g_retired);
    PASS();
}

TEST a_held_page_fails_it_whole(void) {
    up();
    ASSERT(g_up);
    uint32_t va = alloc(4u);
    ASSERT(va != 0);
    fill(va, 4u, 3u);
    ASSERT_EQ(WL_OK, wl_xb_mem_pin(va, PAGE));
    WlXbMemStats before = stats();
    // all the frames left: a pinned page among them, and nowhere to move the others
    ASSERT_FALSE(wl_xb_pg_retire(1u, FRAMES - g_retired, NULL));
    WlXbMemStats after = stats();
    ASSERT(after.frames_free >= before.frames_free); // (none kept)
    ASSERT(check(va, 4u, 3u));
    wl_xb_mem_unpin(va, PAGE);
    // the frames all still the paging's: as many pages as frames fit at once
    uint32_t vc = alloc(FRAMES - g_retired);
    ASSERT(vc != 0);
    fill(vc, FRAMES - g_retired, 4u);
    ASSERT(check(vc, FRAMES - g_retired, 4u));
    PASS();
}

static uint32_t g_left_now;
static size_t g_given;

static void give(void *user, uint32_t left) {
    (void)user;
    size_t bytes = (size_t)(g_left_now - left) * PAGE;
    g_left_now = left;
    g_given += bytes;
    (void)wl_platform_heap_cap_add(bytes);
}

TEST the_heap_gets_frames_back_and_nothing_fails(void) {
    up();
    ASSERT(g_up);
    if (wl_platform_heap_cap(1u << 30) != WL_OK) {
        SKIPm("no heap cap here (a static link)");
    }
    // the frames still the paging's lent: all but 32, every one holding a changed page
    uint32_t lent = FRAMES - g_retired - 32u;
    uint32_t va = alloc(FRAMES - g_retired);
    ASSERT(va != 0);
    fill(va, FRAMES - g_retired, 5u);
    g_left_now = lent;
    ASSERT_EQ(WL_OK, wl_xb_loan_start(33u, lent, give, NULL)); // (the top's retired above)
    // a heap of 64 KB from here on, then 360 KB asked of it in blocks of 36 KB (the loan: 384 KB)
    ASSERT_EQ(WL_OK, wl_platform_heap_cap(64u << 10));
    enum { BLOCKS = 10 };
    void *b[BLOCKS];
    for (int i = 0; i < BLOCKS; i++) {
        b[i] = malloc(36u << 10);
        if (b[i] == NULL) {
            WlXbLoanStats d;
            wl_xb_loan_stats(&d);
            WlHeapReclaimStats r;
            wl_platform_heap_reclaim_stats(&r);
            fprintf(stderr, "block %d: loan left %u refusals %u gives %u; reclaim calls %u busy %u\n", i,
                    (unsigned)d.left, (unsigned)d.refusals, (unsigned)d.gives, (unsigned)r.calls, (unsigned)r.busy);
        }
        ASSERT(b[i] != NULL);
        memset(b[i], i + 1, 36u << 10);
    }
    WlPlatformHeapInfo h;
    wl_platform_heap_info(&h);
    ASSERT_EQ(0u, h.cap_refused);
    WlXbLoanStats ls;
    wl_xb_loan_stats(&ls);
    ASSERT_EQ(lent, ls.lent);
    ASSERT(ls.left < lent);
    ASSERT(ls.gives >= 2u);
    ASSERT(ls.moved > 0u);
    ASSERT_EQ((size_t)(lent - ls.left) * PAGE, g_given);
    ASSERT(g_given >= (360u - 64u) << 10);
    WlHeapReclaimStats rs;
    wl_platform_heap_reclaim_stats(&rs);
    ASSERT_EQ(g_given, rs.bytes);
    // the guest's bytes kept, and its paging within what is left
    ASSERT(check(va, FRAMES - g_retired, 5u));
    ASSERT(resident(g_retired + (lent - ls.left)) <= FRAMES - g_retired - (lent - ls.left));
    // past the whole loan: refused as before, nothing more given
    void *big = malloc((size_t)(ls.left + 64u) * PAGE + (1u << 20));
    ASSERT_EQ(NULL, big);
    wl_xb_loan_stats(&ls);
    ASSERT_EQ(0u, ls.left);
    wl_platform_heap_info(&h);
    ASSERT(h.cap_refused >= 1u);
    for (int i = 0; i < BLOCKS; i++) {
        free(b[i]);
    }
    (void)wl_platform_heap_cap(0);
    (void)wl_platform_heap_set_reclaim(NULL, NULL);
    ASSERT(check(va, FRAMES - g_retired, 5u));
    PASS();
}

SUITE(frame_loan) {
    RUN_TEST(frames_leave_with_the_bytes_kept);
    RUN_TEST(a_held_page_fails_it_whole);
    RUN_TEST(the_heap_gets_frames_back_and_nothing_fails);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(frame_loan);
    cleanup();
    GREATEST_MAIN_END();
}
