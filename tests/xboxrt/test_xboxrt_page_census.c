// SPDX-License-Identifier: MIT
// The page census (page_census.h) under a frame limit on the host: off until WL_XB_PAGE_CENSUS names
// a file (its hooks do nothing), then a page's allocation, what used it and the seconds it was
// touched in; a second closes the touched pages again so that a touch in the next one is seen, and
// the pages' records go to `<file>.pages`.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/xboxrt/crash.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/page_census.h"

#define PAGE WL_XB_PAGE

static char g_path[300];

static uint32_t phys_page(uint32_t va) {
    uint32_t phys = 0;
    return wl_xb_va_to_phys(va, &phys) ? phys >> WL_XB_PAGE_SHIFT : 0xFFFFFFFFu;
}

TEST off_until_asked(void) {
    ASSERT_FALSE(wl_xb_census_on());
    ASSERT_EQ(NULL, g_xb_census_use);
    wl_xb_census_phys(0, PAGE, WL_XB_CU_GPU); // (nothing: off)
    wl_xb_census_alloc(WL_XB_CA_CONTIG, 0x80000000u, PAGE, 0);
    ASSERT_EQ(0u, wl_xb_census_interval());
    PASS();
}

TEST pages_and_seconds(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_path, sizeof g_path, "%s/wl_xbrt_census_%d.txt", tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp",
             (int)getpid());
    setenv("WL_XB_PAGE_CENSUS", g_path, 1);
    setenv("WL_XB_PAGE_CENSUS_STACKS", "1", 1);
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    cfg.ram_bytes = 16u << 20;
    cfg.frame_limit_bytes = 64u * PAGE;
    cfg.swap_path = NULL;
    ASSERT_EQ(WL_OK, wl_xb_mem_init(&cfg));
    ASSERT_EQ(WL_OK, wl_xb_fault_install());
    ASSERT(wl_xb_census_on());

    uint32_t va = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_alloc(&va, 4u * PAGE, 0x00400000u, WL_XB_PROT_READWRITE));
    wl_xb_census_alloc(WL_XB_CA_VIRTUAL, va, 4u * PAGE, 0x00012345u);
    uint32_t p0 = phys_page(va), p1 = phys_page(va + PAGE), p2 = phys_page(va + 2u * PAGE);
    ASSERT(p0 != 0xFFFFFFFFu && p1 != 0xFFFFFFFFu && p2 != 0xFFFFFFFFu);

    // second 1: page 0 written, page 1 read
    volatile uint8_t *m = wl_xb_ptr(va);
    m[0] = 7;
    ASSERT_EQ(0, m[PAGE]);
    ASSERT(wl_xb_census_interval() >= 2u);
    // second 2: page 1 read again (closed by the second's end: the read faults once more)
    ASSERT_EQ(0, m[PAGE + 8u]);
    ASSERT(wl_xb_census_interval() >= 1u);
    // a device used page 2 without the processor touching it
    uint32_t phys2 = 0;
    ASSERT(wl_xb_va_to_phys(va + 2u * PAGE, &phys2));
    wl_xb_census_phys(phys2, 16, WL_XB_CU_TEX);

    uint8_t use;
    uint32_t first, last, seconds;
    WlXbCensusAlloc kind;
    wl_xb_census_page(p0, &use, &first, &last, &seconds, &kind);
    ASSERT_EQ(WL_XB_CU_WRITE, use);
    ASSERT_EQ(1u, first);
    ASSERT_EQ(1u, last);
    ASSERT_EQ(1u, seconds);
    ASSERT_EQ(WL_XB_CA_VIRTUAL, kind);
    wl_xb_census_page(p1, &use, &first, &last, &seconds, &kind);
    ASSERT_EQ(0u, use);
    ASSERT_EQ(1u, first);
    ASSERT_EQ(2u, last);
    ASSERT_EQ(2u, seconds);
    wl_xb_census_page(p2, &use, &first, &last, &seconds, &kind);
    ASSERT_EQ(WL_XB_CU_TEX, use);
    ASSERT_EQ(0u, first);
    ASSERT_EQ(WL_XB_CA_VIRTUAL, kind);

    // the file: a line a second, and the pages' records
    FILE *f = fopen(g_path, "r");
    ASSERT(f != NULL);
    char line[512];
    unsigned lines = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        lines += strncmp(line, "second ", 7) == 0;
    }
    fclose(f);
    ASSERT_EQ(2u, lines);
    wl_xb_census_dump();
    char pages[320];
    snprintf(pages, sizeof pages, "%s.pages", g_path);
    f = fopen(pages, "r");
    ASSERT(f != NULL);
    bool alloc = false, page = false;
    char want[64];
    snprintf(want, sizeof want, "page %x 1 1 1 1 1", (unsigned)p0);
    while (fgets(line, sizeof line, f) != NULL) {
        alloc = alloc || strncmp(line, "alloc 1 virtual ", 16) == 0;
        page = page || strncmp(line, want, strlen(want)) == 0;
    }
    fclose(f);
    ASSERT(alloc);
    ASSERT(page);
    unlink(pages);
#if defined(__GLIBC__) && defined(__x86_64__)
    // who touched (WL_XB_PAGE_CENSUS_STACKS): each touch's second, page and host stack id
    char stacks[320], touch[320];
    snprintf(stacks, sizeof stacks, "%s.stacks", g_path);
    snprintf(touch, sizeof touch, "%s.touch", g_path);
    f = fopen(stacks, "r");
    ASSERT(f != NULL);
    bool anchor = false, stack = false;
    while (fgets(line, sizeof line, f) != NULL) {
        anchor = anchor || strncmp(line, "anchor ", 7) == 0;
        stack = stack || strncmp(line, "stack ", 6) == 0;
    }
    fclose(f);
    ASSERT(anchor);
    ASSERT(stack);
    f = fopen(touch, "rb");
    ASSERT(f != NULL);
    uint32_t rec[3];
    bool p0_1 = false, p1_1 = false, p1_2 = false;
    while (fread(rec, sizeof rec, 1, f) == 1) {
        ASSERT(rec[2] != 0u);
        p0_1 = p0_1 || (rec[0] == 1u && rec[1] == p0);
        p1_1 = p1_1 || (rec[0] == 1u && rec[1] == p1);
        p1_2 = p1_2 || (rec[0] == 2u && rec[1] == p1);
    }
    fclose(f);
    ASSERT(p0_1 && p1_1 && p1_2);
    unlink(stacks);
    unlink(touch);
#endif
    PASS();
}

// The card's traffic (the census's io and card lines): counted by kind, with the pages a file read
// filled and no device used since ("fresh"), started again each second.
TEST card_traffic(void) {
    ASSERT(wl_xb_census_on());
    uint32_t va = 0;
    ASSERT_EQ(WL_OK, wl_xb_va_alloc(&va, 2u * PAGE, 0x00800000u, WL_XB_PROT_READWRITE));
    uint32_t p0 = phys_page(va), p1 = phys_page(va + PAGE);
    ASSERT(p0 != 0xFFFFFFFFu && p1 != 0xFFFFFFFFu);
    (void)wl_xb_census_interval(); // (a second of its own)

    uint32_t ops, pages, fresh;

    // two pages filled by a map read; p1 then read by the GPU: only p0 stays fresh
    wl_xb_census_va(va, 2u * PAGE, WL_XB_CU_MAP);
    wl_xb_census_phys(p1 << WL_XB_PAGE_SHIFT, 16, WL_XB_CU_TEX);
    wl_xb_census_io(WL_XB_CIO_FILE_IN, p0, true);
    wl_xb_census_io(WL_XB_CIO_AHEAD, p1, false);
    wl_xb_census_io_now(WL_XB_CIO_FILE_IN, &ops, &pages, &fresh);
    ASSERT_EQ(1u, ops);
    ASSERT_EQ(1u, pages);
    ASSERT_EQ(1u, fresh);
    wl_xb_census_io_now(WL_XB_CIO_AHEAD, &ops, &pages, &fresh);
    ASSERT_EQ(0u, ops);
    ASSERT_EQ(1u, pages);
    ASSERT_EQ(0u, fresh);
    wl_xb_census_card(WL_XB_CCARD_READ, true, 8192);
    wl_xb_census_card(WL_XB_CCARD_AHEAD, false, 4096);
    uint64_t bytes;
    wl_xb_census_card_now(WL_XB_CCARD_READ, &ops, &bytes);
    ASSERT_EQ(1u, ops);
    ASSERT_EQ(8192u, (unsigned)bytes);

    // the second's end writes its lines and starts the counts again
    (void)wl_xb_census_interval();
    wl_xb_census_io_now(WL_XB_CIO_FILE_IN, &ops, &pages, &fresh);
    ASSERT_EQ(0u, pages);
    wl_xb_census_card_now(WL_XB_CCARD_READ, &ops, &bytes);
    ASSERT_EQ(0u, ops);
    FILE *f = fopen(g_path, "r");
    ASSERT(f != NULL);
    char line[512];
    bool io = false, card = false;
    while (fgets(line, sizeof line, f) != NULL) {
        io = io || (strstr(line, " file_in ops 1 pages 1 map 1 fresh 1 ") != NULL && strstr(line, " @0 1/1") != NULL);
        card = card || strstr(line, " read 1 8 0 0 read_held 0 0 0 0 ahead 0 0 1 4 write 0 0 0 0") != NULL;
    }
    fclose(f);
    ASSERT(io);
    ASSERT(card);
    unlink(g_path);
    PASS();
}

SUITE(census) {
    RUN_TEST(off_until_asked);
    RUN_TEST(pages_and_seconds);
    RUN_TEST(card_traffic);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(census);
    GREATEST_MAIN_END();
}
