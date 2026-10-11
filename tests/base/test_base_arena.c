// SPDX-License-Identifier: MIT
#include "wl/base/arena.h"

#include <stdint.h>

#include "greatest.h"

typedef struct {
    uint64_t a;
    uint8_t b;
} Pair;

TEST alloc_aligns_and_tracks(void) {
    _Alignas(64) uint8_t mem[256];
    WlArena a;
    wl_arena_init(&a, mem, sizeof mem, "test");
    ASSERT_EQ(256u, wl_arena_remaining(&a));
    uint8_t *p1 = wl_arena_alloc(&a, 3, 1);
    ASSERT(p1 == mem);
    uint8_t *p2 = wl_arena_alloc(&a, 4, 8);
    ASSERT(p2 == mem + 8);
    uint8_t *p3 = wl_arena_alloc(&a, 1, WL_ARENA_ALIGN_GPU);
    ASSERT(p3 == mem + 32);
    ASSERT_EQ(33u, a.used);
    ASSERT_EQ(33u, a.peak);
    ASSERT_EQ(223u, wl_arena_remaining(&a));
    PASS();
}

TEST alignment_uses_real_address(void) {
    _Alignas(64) uint8_t mem[128];
    WlArena a;
    wl_arena_init(&a, mem + 1, 100, NULL); // deliberately misaligned block
    uint8_t *p = wl_arena_alloc(&a, 4, 16);
    ASSERT(p != NULL);
    ASSERT_EQ(0u, (unsigned)((uintptr_t)p % 16u));
    ASSERT(p == mem + 16);
    PASS();
}

TEST exhaustion_returns_null(void) {
    uint8_t mem[16];
    WlArena a;
    wl_arena_init(&a, mem, sizeof mem, NULL);
    ASSERT(wl_arena_alloc(&a, 17, 1) == NULL);
    ASSERT(wl_arena_alloc(&a, SIZE_MAX, 1) == NULL);
    ASSERT(wl_arena_alloc(&a, SIZE_MAX - 3, 8) == NULL);
    ASSERT(wl_arena_alloc(&a, 16, 1) != NULL);
    ASSERT(wl_arena_alloc(&a, 1, 1) == NULL);
    ASSERT_EQ(0u, wl_arena_remaining(&a));
    PASS();
}

TEST zero_fill_and_typed_helpers(void) {
    uint8_t mem[128];
    for (size_t i = 0; i < sizeof mem; i++) {
        mem[i] = 0xAA;
    }
    WlArena a;
    wl_arena_init(&a, mem, sizeof mem, "zero");
    Pair *p = WL_ARENA_NEW(&a, Pair);
    ASSERT(p != NULL && p->a == 0 && p->b == 0);
    ASSERT_EQ(0u, (unsigned)((uintptr_t)p % _Alignof(Pair)));
    uint16_t *arr = WL_ARENA_ARRAY(&a, uint16_t, 5);
    ASSERT(arr != NULL);
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(0u, arr[i]);
    }
    ASSERT(WL_ARENA_ARRAY(&a, uint64_t, 1000) == NULL);
    PASS();
}

TEST mark_reset_and_peak(void) {
    uint8_t mem[64];
    WlArena a;
    wl_arena_init(&a, mem, sizeof mem, NULL);
    ASSERT(wl_arena_alloc(&a, 8, 1) != NULL);
    WlArenaMark m = wl_arena_mark(&a);
    ASSERT(wl_arena_alloc(&a, 40, 1) != NULL);
    ASSERT_EQ(48u, a.peak);
    wl_arena_reset_to(&a, m);
    ASSERT_EQ(8u, a.used);
    ASSERT_EQ(48u, a.peak); // peak survives rollback
    ASSERT(wl_arena_alloc(&a, 56, 1) != NULL);
    wl_arena_reset(&a);
    ASSERT_EQ(0u, a.used);
    ASSERT_EQ(64u, wl_arena_remaining(&a));
    PASS();
}

TEST null_memory_is_empty(void) {
    WlArena a;
    wl_arena_init(&a, NULL, 100, NULL);
    ASSERT(wl_arena_alloc(&a, 1, 1) == NULL);
    PASS();
}

SUITE(arena_suite) {
    RUN_TEST(alloc_aligns_and_tracks);
    RUN_TEST(alignment_uses_real_address);
    RUN_TEST(exhaustion_returns_null);
    RUN_TEST(zero_fill_and_typed_helpers);
    RUN_TEST(mark_reset_and_peak);
    RUN_TEST(null_memory_is_empty);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(arena_suite);
    GREATEST_MAIN_END();
}
