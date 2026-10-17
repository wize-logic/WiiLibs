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
    unlink(g_path);
    PASS();
}

SUITE(census) {
    RUN_TEST(off_until_asked);
    RUN_TEST(pages_and_seconds);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(census);
    GREATEST_MAIN_END();
}
