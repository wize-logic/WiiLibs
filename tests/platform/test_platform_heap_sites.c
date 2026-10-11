// SPDX-License-Identifier: MIT
// The heap's accounting by call site (wl/platform/heap_sites.c): sites, size classes, the table of
// live blocks under a long random run (against a plain model), and what happens when it is full.
// Byte-order independent: also runs on the big-endian build.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/heap_sites.h"

static _Alignas(8) uint8_t g_storage[64u * 1024u];

static uintptr_t fake(unsigned i) {
    return (uintptr_t)0x10000000u + (uintptr_t)i * 32u;
}

TEST init_lays_out_the_tables(void) {
    WlHeapSites s;
    ASSERT_EQ(WL_ERR_ARG, wl_heap_sites_init(&s, NULL, 100u, 4u, 1024u));
    ASSERT_EQ(WL_ERR_ARG, wl_heap_sites_init(&s, g_storage, 64u, 4u, 1024u));
    ASSERT_EQ(WL_OK, wl_heap_sites_init(&s, g_storage, sizeof g_storage, 64u, 1024u));
    ASSERT_EQ(64u, s.site_capacity);
    // a power of two that fits after the sites
    ASSERT_EQ(0u, s.block_capacity & (s.block_capacity - 1u));
    ASSERT(wl_heap_sites_storage_bytes(64u, s.block_capacity) <= sizeof g_storage);
    ASSERT(wl_heap_sites_storage_bytes(64u, s.block_capacity * 2u) > sizeof g_storage);
    PASS();
}

TEST blocks_are_counted_by_site_and_small_ones_by_size(void) {
    WlHeapSites s;
    WlHeapSite top[4];
    WlHeapSitesTotals t;
    ASSERT_EQ(WL_OK, wl_heap_sites_init(&s, g_storage, sizeof g_storage, 16u, 1024u));
    wl_heap_sites_alloc(&s, (void *)fake(1), 4096u, 0x80001000u, 0x80002000u);
    wl_heap_sites_alloc(&s, (void *)fake(2), 2048u, 0x80001000u, 0x80002000u);
    wl_heap_sites_alloc(&s, (void *)fake(3), 65536u, 0x80003000u, 0x80004000u);
    // the same first caller from elsewhere is another site
    wl_heap_sites_alloc(&s, (void *)fake(4), 1024u, 0x80001000u, 0x80009000u);
    wl_heap_sites_alloc(&s, (void *)fake(5), 32u, 0x80005000u, 0u);
    wl_heap_sites_alloc(&s, (void *)fake(6), 100u, 0x80005000u, 0u);
    wl_heap_sites_alloc(&s, (void *)fake(7), 1000u, 0x80005000u, 0u);
    ASSERT_EQ(3u, wl_heap_sites_top(&s, top, 4u));
    ASSERT_EQ(0x80003000u, top[0].caller[0]);
    ASSERT_EQ(65536u, top[0].bytes);
    ASSERT_EQ(0x80001000u, top[1].caller[0]);
    ASSERT_EQ(0x80002000u, top[1].caller[1]);
    ASSERT_EQ(6144u, top[1].bytes);
    ASSERT_EQ(2u, top[1].blocks);
    ASSERT_EQ(0x80009000u, top[2].caller[1]);
    wl_heap_sites_totals(&s, &t);
    ASSERT_EQ(65536u + 6144u + 1024u, t.tracked_bytes);
    ASSERT_EQ(4u, t.tracked_blocks);
    ASSERT_EQ(32u, t.small_bytes[0]);
    ASSERT_EQ(100u, t.small_bytes[1]);
    ASSERT_EQ(1000u, t.small_bytes[2]);
    ASSERT_EQ(1u, t.small_blocks[2]);

    wl_heap_sites_free(&s, (void *)fake(1), 4096u);
    wl_heap_sites_free(&s, (void *)fake(6), 100u);
    ASSERT_EQ(2u, wl_heap_sites_top(&s, top, 2u));
    ASSERT_EQ(2048u, top[1].bytes);
    ASSERT_EQ(6144u, top[1].peak_bytes);
    ASSERT_EQ(2u, top[1].allocs);
    wl_heap_sites_totals(&s, &t);
    ASSERT_EQ(0u, t.small_bytes[1]);
    ASSERT_EQ(0u, t.small_blocks[1]);
    // a free of a tracked size never seen changes nothing; a small one is only a count, which
    // stops at zero
    wl_heap_sites_free(&s, (void *)fake(99), 8192u);
    wl_heap_sites_free(&s, (void *)fake(98), 100u);
    wl_heap_sites_totals(&s, &t);
    ASSERT_EQ(65536u + 2048u + 1024u, t.tracked_bytes);
    ASSERT_EQ(0u, t.small_bytes[1]);
    ASSERT_EQ(0u, t.small_blocks[1]);
    wl_heap_sites_log(&s, 8u);
    PASS();
}

TEST a_full_table_counts_what_it_cannot_hold(void) {
    WlHeapSites s;
    WlHeapSitesTotals t;
    // two sites and the smallest table of blocks (16, kept under 14 live)
    size_t bytes = wl_heap_sites_storage_bytes(2u, 16u);
    ASSERT_EQ(WL_OK, wl_heap_sites_init(&s, g_storage, bytes, 2u, 16u));
    ASSERT_EQ(16u, s.block_capacity);
    wl_heap_sites_alloc(&s, (void *)fake(1), 100u, 1u, 0u);
    wl_heap_sites_alloc(&s, (void *)fake(2), 100u, 2u, 0u);
    wl_heap_sites_alloc(&s, (void *)fake(3), 100u, 3u, 0u); // no site left
    wl_heap_sites_totals(&s, &t);
    ASSERT_EQ(1u, t.untracked);
    ASSERT_EQ(100u, t.untracked_bytes);
    for (unsigned i = 4; i < 40u; i++) {
        wl_heap_sites_alloc(&s, (void *)fake(i), 100u, 1u, 0u);
    }
    wl_heap_sites_totals(&s, &t);
    ASSERT_EQ(14u, t.tracked_blocks);
    ASSERT_EQ(1u + 24u, t.untracked);
    // freeing them all (tracked or not) leaves nothing tracked
    for (unsigned i = 1; i < 40u; i++) {
        wl_heap_sites_free(&s, (void *)fake(i), 100u);
    }
    wl_heap_sites_totals(&s, &t);
    ASSERT_EQ(0u, t.tracked_blocks);
    ASSERT_EQ(0u, t.tracked_bytes);
    PASS();
}

TEST a_long_random_run_matches_a_plain_model(void) {
    enum { SLOTS = 600, SITES = 24 };
    static size_t model_size[SLOTS];
    static uint8_t model_site[SLOTS];
    WlHeapSites s;
    WlHeapSite top[SITES];
    uint32_t rng = 12345u;
    size_t expected[SITES];
    ASSERT_EQ(WL_OK, wl_heap_sites_init(&s, g_storage, sizeof g_storage, SITES, 256u));
    ASSERT(s.block_capacity >= 1024u);
    memset(model_size, 0, sizeof model_size);
#define NEXT() (rng = rng * 1664525u + 1013904223u, rng >> 8)
    for (unsigned step = 0; step < 200000u; step++) {
        unsigned slot = NEXT() % SLOTS;
        if (model_size[slot] == 0u) {
            size_t size = 256u + NEXT() % 5000u;
            unsigned site = NEXT() % SITES;
            wl_heap_sites_alloc(&s, (void *)fake(slot), size, 0x80000000u + site * 4u, 0u);
            model_size[slot] = size;
            model_site[slot] = (uint8_t)site;
        } else {
            wl_heap_sites_free(&s, (void *)fake(slot), model_size[slot]);
            model_size[slot] = 0u;
        }
        if (step % 9973u == 0u || step == 199999u) {
            WlHeapSitesTotals t;
            size_t n, i, sum = 0;
            uint32_t live = 0;
            memset(expected, 0, sizeof expected);
            for (i = 0; i < SLOTS; i++) {
                if (model_size[i] != 0u) {
                    expected[model_site[i]] += model_size[i];
                    sum += model_size[i];
                    live++;
                }
            }
            wl_heap_sites_totals(&s, &t);
            ASSERT_EQ(sum, t.tracked_bytes);
            ASSERT_EQ(live, t.tracked_blocks);
            ASSERT_EQ(0u, t.untracked);
            n = wl_heap_sites_top(&s, top, SITES);
            for (i = 0; i < n; i++) {
                unsigned site = (unsigned)((top[i].caller[0] - 0x80000000u) / 4u);
                ASSERT_EQ(expected[site], top[i].bytes);
                if (i > 0u) {
                    ASSERT(top[i - 1u].bytes >= top[i].bytes);
                }
            }
        }
    }
#undef NEXT
    PASS();
}

SUITE(heap_sites_suite) {
    RUN_TEST(init_lays_out_the_tables);
    RUN_TEST(blocks_are_counted_by_site_and_small_ones_by_size);
    RUN_TEST(a_full_table_counts_what_it_cannot_hold);
    RUN_TEST(a_long_random_run_matches_a_plain_model);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(heap_sites_suite);
    GREATEST_MAIN_END();
}
