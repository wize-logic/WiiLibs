// SPDX-License-Identifier: MIT
// Heap core (wl/platform/mem_heap.c): alignment, coalescing, determinism, corruption checks.
#include "wl/platform/mem_heap.h"

#include <stdint.h>
#include <string.h>

#include "greatest.h"

static _Alignas(64) uint8_t g_mem[64 * 1024];

static bool count_cb(void *user, const WlHeapLive *b) {
    (void)b;
    (*(uint32_t *)user)++;
    return true;
}

TEST alloc_is_32_aligned_and_disjoint(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, sizeof g_mem, "t");
    void *p[20];
    for (int i = 0; i < 20; i++) {
        p[i] = wl_heap_alloc(&h, (size_t)(1 + i * 7), (size_t)1 << (i % 6), "a.c", 1, 0);
        ASSERT(p[i] != NULL);
        ASSERT_EQ(0u, (unsigned)((uintptr_t)p[i] & 31u));
        ASSERT(wl_heap_usable_size(&h, p[i]) >= (size_t)(1 + i * 7));
        memset(p[i], i, (size_t)(1 + i * 7));
    }
    for (int i = 0; i < 20; i++) { // no overlap: contents intact
        for (int j = 0; j < 1 + i * 7; j++) {
            ASSERT_EQ(i, ((uint8_t *)p[i])[j]);
        }
    }
    ASSERT(wl_heap_check(&h));
    for (int i = 0; i < 20; i++) {
        ASSERT(wl_heap_free(&h, p[i]));
    }
    ASSERT_EQ(0u, h.used);
    ASSERT_EQ(0u, h.live);
    ASSERT(wl_heap_check(&h));
    PASS();
}

TEST unaligned_base_is_trimmed(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem + 5, 4000, "t");
    ASSERT_EQ(0u, (unsigned)((uintptr_t)h.base & 31u));
    ASSERT_EQ(0u, h.size % 32u);
    void *p = wl_heap_alloc(&h, 100, 8, "a.c", 1, 0);
    ASSERT(p != NULL && wl_heap_owns(&h, p));
    PASS();
}

TEST coalesces_back_to_one_block(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, sizeof g_mem, "t");
    size_t whole = wl_heap_largest_free(&h);
    void *a = wl_heap_alloc(&h, 1000, 8, "a.c", 1, 0);
    void *b = wl_heap_alloc(&h, 2000, 8, "a.c", 2, 0);
    void *c = wl_heap_alloc(&h, 3000, 8, "a.c", 3, 0);
    ASSERT(a && b && c);
    ASSERT(wl_heap_free(&h, b)); // hole in the middle
    ASSERT(wl_heap_check(&h));
    ASSERT(wl_heap_free(&h, a)); // merges with the hole (next)
    ASSERT(wl_heap_free(&h, c)); // merges both sides and the tail
    ASSERT(wl_heap_check(&h));
    ASSERT_EQ(whole, wl_heap_largest_free(&h));
    PASS();
}

TEST exhaustion_and_budget(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, 4096, "t");
    ASSERT(wl_heap_alloc(&h, 8192, 8, "a.c", 1, 0) == NULL);
    ASSERT_EQ(1u, h.fails);
    void *p = wl_heap_alloc(&h, wl_heap_largest_free(&h), 8, "a.c", 2, 0);
    ASSERT(p != NULL);
    ASSERT(wl_heap_alloc(&h, 1, 8, "a.c", 3, 0) == NULL);
    ASSERT_EQ(2u, h.fails);
    ASSERT_EQ(h.size, h.peak);
    ASSERT(wl_heap_free(&h, p));
    ASSERT(wl_heap_alloc(&h, 1, 8, "a.c", 3, 0) != NULL);
    PASS();
}

TEST zero_size_and_null_free(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, 4096, "t");
    void *p = wl_heap_alloc(&h, 0, 1, "a.c", 1, 0);
    ASSERT(p != NULL);
    ASSERT(wl_heap_free(&h, NULL));
    ASSERT(wl_heap_free(&h, p));
    PASS();
}

TEST bad_free_is_rejected(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, 8192, "t");
    uint8_t other[64];
    void *p = wl_heap_alloc(&h, 100, 8, "a.c", 1, 0);
    ASSERT(!wl_heap_free(&h, other));          // foreign
    ASSERT(!wl_heap_free(&h, (uint8_t *)p + 8)); // interior (unaligned)
    ASSERT(wl_heap_free(&h, p));
    ASSERT(!wl_heap_free(&h, p)); // double free
    ASSERT(wl_heap_check(&h));
    PASS();
}

TEST check_detects_corruption(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, 8192, "t");
    uint8_t *p = wl_heap_alloc(&h, 100, 8, "a.c", 1, 0);
    uint8_t *q = wl_heap_alloc(&h, 100, 8, "a.c", 2, 0);
    ASSERT(p && q && wl_heap_check(&h));
    memset(p, 0xFF, 160); // runs past p's payload into q's header
    ASSERT(!wl_heap_check(&h));
    PASS();
}

TEST live_walk_reports_sites_and_scene(void) {
    WlHeap h;
    wl_heap_init(&h, g_mem, 8192, "t");
    void *a = wl_heap_alloc(&h, 10, 8, "a.c", 11, 7);
    void *b = wl_heap_alloc(&h, 20, 8, "b.c", 22, 8);
    uint32_t n = 0;
    wl_heap_walk_live(&h, count_cb, &n);
    ASSERT_EQ(2u, n);
    ASSERT(wl_heap_free(&h, a));
    n = 0;
    wl_heap_walk_live(&h, count_cb, &n);
    ASSERT_EQ(1u, n);
    ASSERT(wl_heap_free(&h, b));
    PASS();
}

// Same operations, same addresses (offsets from the heap base).
static uint32_t run_script(uint8_t *mem, uint32_t *offsets, int cap) {
    WlHeap h;
    wl_heap_init(&h, mem, 32768, "t");
    void *live[32] = {0};
    uint32_t state = 12345;
    int n = 0;
    for (int i = 0; i < 400; i++) {
        state = state * 1664525u + 1013904223u;
        unsigned slot = (state >> 16) % 32u;
        if (live[slot] != NULL) {
            wl_heap_free(&h, live[slot]);
            live[slot] = NULL;
        } else {
            live[slot] = wl_heap_alloc(&h, (state >> 8) % 700u, 8, "a.c", 1, 0);
            if (n < cap) {
                offsets[n++] = live[slot] != NULL ? (uint32_t)((uint8_t *)live[slot] - h.base) : 0xFFFFFFFFu;
            }
        }
        if (!wl_heap_check(&h)) {
            return 0xFFFFFFFFu;
        }
    }
    return (uint32_t)n;
}

TEST deterministic_and_stress(void) {
    static _Alignas(64) uint8_t other[32768 + 64];
    uint32_t o1[400], o2[400];
    uint32_t n1 = run_script(g_mem, o1, 400);
    uint32_t n2 = run_script(other + 32, o2, 400); // different absolute address
    ASSERT(n1 != 0xFFFFFFFFu && n1 == n2);
    ASSERT_MEM_EQ(o1, o2, n1 * sizeof o1[0]);
    PASS();
}

SUITE(mem_heap_suite) {
    RUN_TEST(alloc_is_32_aligned_and_disjoint);
    RUN_TEST(unaligned_base_is_trimmed);
    RUN_TEST(coalesces_back_to_one_block);
    RUN_TEST(exhaustion_and_budget);
    RUN_TEST(zero_size_and_null_free);
    RUN_TEST(bad_free_is_rejected);
    RUN_TEST(check_detects_corruption);
    RUN_TEST(live_walk_reports_sites_and_scene);
    RUN_TEST(deterministic_and_stress);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mem_heap_suite);
    GREATEST_MAIN_END();
}
