// SPDX-License-Identifier: MIT
// Who holds the host's heap (wl/platform/mem_census.c): the reporters, the rows' order, "other",
// and on a glibc host (host/heap_host.c) the heap's figures, the cap and the exempt blocks.
// Byte-order independent: also runs on the big-endian build.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/mem_census.h"

static void report_fixed(void *user, WlMemCensusReport *out) {
    out->bytes = *(const size_t *)user;
}

static void report_noted(void *user, WlMemCensusReport *out) {
    out->bytes = *(const size_t *)user;
    strcpy(out->note, "3 of them");
}

TEST reporters_rows_and_other(void) {
    static size_t small = 1000, large = 50000, replaced = 7;
    static WlMemCensus c;
    ASSERT_EQ(WL_ERR_ARG, wl_mem_census_add(NULL, report_fixed, &small));
    ASSERT_EQ(WL_ERR_ARG, wl_mem_census_add("x", NULL, &small));
    ASSERT_EQ(WL_OK, wl_mem_census_add("small", report_fixed, &small));
    ASSERT_EQ(WL_OK, wl_mem_census_add("large", report_noted, &large));
    // a name already there is replaced, not added
    ASSERT_EQ(WL_OK, wl_mem_census_add("small", report_fixed, &replaced));
    void *held = malloc(100000); // (something on the heap)
    ASSERT(held != NULL);
    wl_mem_census_take(&c);
    free(held);
    ASSERT_EQ(2u, c.count);
    ASSERT_STR_EQ("large", c.rows[0].name); // largest first
    ASSERT_STR_EQ("3 of them", c.rows[0].report.note);
    ASSERT_EQ(7u, c.rows[1].report.bytes);
    ASSERT_EQ(50007u, c.claimed);
    if (c.heap.known) {
        ASSERT(c.heap.in_use > 0);
        ASSERT_EQ(c.heap.in_use > c.claimed ? c.heap.in_use - c.claimed : 0u, c.other);
    } else {
        ASSERT_EQ(0u, c.other);
    }
    wl_mem_census_log("test", false);
    wl_mem_census_log("test", true);
    wl_mem_census_remove("large");
    wl_mem_census_remove("absent");
    wl_mem_census_take(&c);
    ASSERT_EQ(1u, c.count);
    ASSERT_STR_EQ("small", c.rows[0].name);
    wl_mem_census_remove("small");
    // the registry has a limit
    {
        static char names[WL_MEM_CENSUS_MAX + 1][8];
        unsigned i;
        for (i = 0; i < WL_MEM_CENSUS_MAX; i++) {
            names[i][0] = (char)('a' + i);
            ASSERT_EQ(WL_OK, wl_mem_census_add(names[i], report_fixed, &small));
        }
        ASSERT_EQ(WL_ERR_LIMIT, wl_mem_census_add("one more", report_fixed, &small));
        for (i = 0; i < WL_MEM_CENSUS_MAX; i++) {
            wl_mem_census_remove(names[i]);
        }
        wl_mem_census_take(&c);
        ASSERT_EQ(0u, c.count);
    }
    PASS();
}

TEST heap_text(void) {
    WlPlatformHeapInfo h;
    char buf[200];
    memset(&h, 0, sizeof h);
    wl_mem_census_heap_text(&h, buf, sizeof buf);
    ASSERT(strstr(buf, "not known") != NULL);
    h.known = true;
    h.in_use = 3u << 20;
    h.free_inside = 100u << 10;
    h.top = 2u << 20;
    h.elsewhere = 1u << 20;
    h.free_total = h.free_inside + h.top + h.elsewhere;
    wl_mem_census_heap_text(&h, buf, sizeof buf);
    ASSERT_STR_EQ("heap: 3072 KB in use, 3172 KB free (100 inside, 2048 above the top, 1024 in MEM2)", buf);
    h.elsewhere = 0;
    h.free_total = h.free_inside + h.top;
    h.cap = 4u << 20;
    h.cap_used = 1u << 20;
    h.cap_peak = 2u << 20;
    h.cap_refused = 3;
    h.cap_largest_refused = 5000;
    wl_mem_census_heap_text(&h, buf, sizeof buf);
    ASSERT_STR_EQ("heap: 3072 KB in use, 2148 KB free (100 inside, 2048 above the top); cap 4096 KB: 1024 used, "
                  "2048 at most, 3 refused (largest 5000 B)",
                  buf);
    // (a short buffer is cut, not overrun)
    wl_mem_census_heap_text(&h, buf, 16);
    ASSERT_EQ(15u, strlen(buf));
    // the Wii once sbrk has moved on to MEM2: MEM1's rest out of malloc's reach, said beside
    memset(&h, 0, sizeof h);
    h.known = true;
    h.in_use = 3u << 20;
    h.top = 2u << 20;
    h.free_total = h.top;
    h.stranded = 7u << 20;
    wl_mem_census_heap_text(&h, buf, sizeof buf);
    ASSERT_STR_EQ("heap: 3072 KB in use, 2048 KB free (0 inside, 2048 above the top); 7168 KB of MEM1 out of "
                  "malloc's reach",
                  buf);
    PASS();
}

// The census's rows wrap before the Wii's log cuts a line (log_wii.c: 160 characters, the level and
// tag included: "[I][xbirq] " before the text).
static unsigned g_lines, g_longest;
static void sink_lengths(void *user, WlLogLevel level, const char *tag, const char *line) {
    (void)user;
    (void)level;
    size_t n = 6u + strlen(tag) + strlen(line);
    g_lines++;
    g_longest = n > g_longest ? (unsigned)n : g_longest;
}

TEST rows_fit_the_wii_log(void) {
    static size_t bytes = 12345678u;
    static char names[WL_MEM_CENSUS_MAX][16];
    unsigned i;
    for (i = 0; i < WL_MEM_CENSUS_MAX; i++) {
        snprintf(names[i], sizeof names[i], "row number %u", i);
        ASSERT_EQ(WL_OK, wl_mem_census_add(names[i], report_noted, &bytes));
    }
    wl_log_set_sink(sink_lengths, NULL);
    wl_mem_census_log("xbirq", false);
    wl_log_set_sink(NULL, NULL);
    for (i = 0; i < WL_MEM_CENSUS_MAX; i++) {
        wl_mem_census_remove(names[i]);
    }
    ASSERT(g_lines > 3u);
    ASSERT(g_longest <= 160u);
    PASS();
}

TEST cap_and_exempt(void) {
    WlPlatformHeapInfo h;
    if (wl_platform_heap_cap(1u << 20) == WL_ERR_UNSUPPORTED) {
        SKIPm("no cap on this host");
    }
    wl_platform_heap_info(&h);
    ASSERT(h.known);
    ASSERT_EQ((size_t)1u << 20, h.cap);
    ASSERT_EQ(0u, h.cap_refused);
    // within the cap
    void *a = malloc(256u << 10);
    ASSERT(a != NULL);
    wl_platform_heap_info(&h);
    ASSERT(h.cap_used >= (256u << 10));
    ASSERT(h.top <= h.cap - (256u << 10));
    // beyond it: refused, counted, nothing changes
    size_t used = h.cap_used;
    void *b = malloc(900u << 10);
    ASSERT(b == NULL);
    ASSERT(calloc(900u, 1024u) == NULL);
    void *grown = realloc(a, 2u << 20);
    ASSERT(grown == NULL); // (a stays)
    wl_platform_heap_info(&h);
    ASSERT_EQ(3u, h.cap_refused);
    // (the largest: the realloc's growth, 2 MB less the block's 256 KB)
    ASSERT(h.cap_largest_refused >= (1700u << 10) && h.cap_largest_refused <= (2u << 20));
    ASSERT_EQ(used, h.cap_used);
    // exempt blocks are neither held to the cap nor counted, their free neither
    wl_platform_heap_exempt(true);
    b = malloc(2u << 20);
    wl_platform_heap_exempt(false);
    ASSERT(b != NULL);
    memset(b, 1, 2u << 20);
    wl_platform_heap_info(&h);
    ASSERT_EQ(used, h.cap_used);
    b = realloc(b, 3u << 20); // (still exempt, whoever grows it)
    ASSERT(b != NULL);
    free(b);
    wl_platform_heap_info(&h);
    ASSERT_EQ(used, h.cap_used);
    free(a);
    wl_platform_heap_info(&h);
    ASSERT(h.cap_used < used);
    ASSERT(h.cap_peak >= used);
    // lifted
    ASSERT_EQ(WL_OK, wl_platform_heap_cap(0));
    b = malloc(2u << 20);
    ASSERT(b != NULL);
    free(b);
    wl_platform_heap_info(&h);
    ASSERT_EQ(0u, h.cap);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(reporters_rows_and_other);
    RUN_TEST(heap_text);
    RUN_TEST(rows_fit_the_wii_log);
    RUN_TEST(cap_and_exempt);
    GREATEST_MAIN_END();
}
