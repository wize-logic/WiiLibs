// SPDX-License-Identifier: MIT
// SPSC ring (wl/platform/ring.c): argument checks, FIFO order, full/empty, peek/discard,
// wraparound across the 2^32 index wrap, and a two-thread stress run with raw pthreads.
#include "wl/platform/ring.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"

TEST init_validates_arguments(void) {
    uint32_t store[8];
    WlSpscRing r;
    ASSERT_EQ(WL_ERR_ARG, wl_ring_init(NULL, store, 4, 8));
    ASSERT_EQ(WL_ERR_ARG, wl_ring_init(&r, NULL, 4, 8));
    ASSERT_EQ(WL_ERR_ARG, wl_ring_init(&r, store, 0, 8));
    ASSERT_EQ(WL_ERR_ARG, wl_ring_init(&r, store, 4, 1));
    ASSERT_EQ(WL_ERR_ARG, wl_ring_init(&r, store, 4, 12));
    ASSERT_EQ(WL_ERR_ARG, wl_ring_init(&r, (uint8_t *)store + 1, 4, 8));
    ASSERT_EQ(WL_OK, wl_ring_init(&r, store, 4, 8));
    PASS();
}

TEST fifo_full_empty_and_peek(void) {
    uint16_t store[4];
    WlSpscRing r;
    ASSERT_EQ(WL_OK, wl_ring_init(&r, store, 2, 4));
    uint16_t v;
    ASSERT(!wl_ring_pop(&r, &v));
    ASSERT(wl_ring_peek(&r) == NULL);
    for (uint16_t i = 0; i < 4; i++) {
        v = (uint16_t)(10 + i);
        ASSERT(wl_ring_push(&r, &v));
    }
    v = 99;
    ASSERT(!wl_ring_push(&r, &v));
    ASSERT_EQ(4u, wl_ring_count(&r));
    ASSERT_EQ(0u, wl_ring_free_slots(&r));
    const uint16_t *p = wl_ring_peek(&r);
    ASSERT(p != NULL && *p == 10);
    ASSERT(wl_ring_peek(&r) == p); // peek does not consume
    wl_ring_discard(&r);
    ASSERT_EQ(3u, wl_ring_count(&r));
    for (uint16_t i = 1; i < 4; i++) {
        ASSERT(wl_ring_pop(&r, &v));
        ASSERT_EQ(10 + i, v);
    }
    ASSERT(!wl_ring_pop(&r, &v));
    PASS();
}

TEST indices_survive_the_u32_wrap(void) {
    uint32_t store[8];
    WlSpscRing r;
    ASSERT_EQ(WL_OK, wl_ring_init(&r, store, 4, 8));
    atomic_store(&r.head, 0xFFFFFFFCu); // 4 pushes before the free-running index wraps
    atomic_store(&r.tail, 0xFFFFFFFCu);
    uint32_t in = 0;
    uint32_t out = 0;
    uint32_t v;
    for (int i = 0; i < 64; i++) {
        while (wl_ring_push(&r, &in)) {
            in++;
        }
        ASSERT_EQ(8u, wl_ring_count(&r));
        for (int k = 0; k < 5; k++) {
            ASSERT(wl_ring_pop(&r, &v));
            ASSERT_EQ(out, v);
            out++;
        }
    }
    PASS();
}

typedef struct {
    WlSpscRing *ring;
    uint32_t n;
    uint32_t bad;
} Stress;

static void *stress_consumer(void *arg) {
    Stress *s = arg;
    for (uint32_t expect = 0; expect < s->n;) {
        uint64_t v;
        if (wl_ring_pop(s->ring, &v)) {
            if (v != ((uint64_t)expect << 32 | (expect ^ 0xA5A5A5A5u))) {
                s->bad++;
            }
            expect++;
        }
    }
    return NULL;
}

TEST two_threads_one_million_items(void) {
    static uint64_t store[64];
    WlSpscRing r;
    ASSERT_EQ(WL_OK, wl_ring_init(&r, store, 8, 64));
    Stress s = {&r, 1000000u, 0};
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, stress_consumer, &s));
    for (uint32_t i = 0; i < s.n; i++) {
        uint64_t v = (uint64_t)i << 32 | (i ^ 0xA5A5A5A5u);
        while (!wl_ring_push(&r, &v)) {
        }
    }
    pthread_join(th, NULL);
    ASSERT_EQ(0u, s.bad);
    ASSERT_EQ(0u, wl_ring_count(&r));
    PASS();
}

SUITE(ring_suite) {
    RUN_TEST(init_validates_arguments);
    RUN_TEST(fifo_full_empty_and_peek);
    RUN_TEST(indices_survive_the_u32_wrap);
    RUN_TEST(two_threads_one_million_items);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(ring_suite);
    GREATEST_MAIN_END();
}
