// SPDX-License-Identifier: MIT
// The ring of made vertices GX reads by index (nv2a_gx.c's made_place, made_slots, made_flush), the
// file built into this test for its slots' bookkeeping. A batch's slots are written back to the next
// 32-byte line and the next batch starts at the first slot after it: with slots under 32 bytes (two,
// three or five words) that passes over up to three slots, all of them within the slots the draw
// took. Every batch of every draw ends within them (made.next <= made.limit), no write reaches past
// them, and the draw's slot indices stay GX's 16 bits, also where the slots end at the ring's end.
#include "wl/xbox/nv2a_gx.c"

#include "greatest.h"

enum { GUARD = 256, GUARD_BYTE = 0xA5 };

static uint8_t *g_space;
static WlNv2aGx *g_gx;

static void ring_begin(void) {
    g_space = malloc(MADE_RING_BYTES + GUARD + 32u);
    g_gx = calloc(1, sizeof *g_gx);
    g_gx->made.ring = (PackedVertex *)(void *)(((uintptr_t)g_space + 31u) & ~(uintptr_t)31u);
    g_gx->made.tried = true;
}

static void ring_end(void) {
    free(g_gx);
    free(g_space);
    g_gx = NULL;
    g_space = NULL;
}

// One draw of `count` vertices, slots of `stride` bytes, placed from `pos` (every chunk entered: no
// fence to wait for), batch by batch as vertices_make takes them; indexed, the n-th vertex's index is
// n * repeat / 4 (repeats kept by the memo: fewer new slots a batch). 0 when every batch stays within
// the draw's slots and every index within 16 bits, else the first batch that did not.
static int ring_draw(uint32_t pos, uint32_t stride, uint32_t count, unsigned repeat) {
    static uint32_t indices[1024];
    uint8_t *ring = (uint8_t *)g_gx->made.ring;
    WlNv2aDraw draw;
    uint32_t first, run, n, end;
    int batch = 1;

    memset(&draw, 0, sizeof draw);
    draw.count = count;
    if (repeat != 0) {
        for (n = 0; n < count; n++) {
            indices[n] = n * repeat / 4u;
        }
        draw.indices = indices;
        draw.source = WL_NV2A_SOURCE_ARRAYS;
    }
    g_gx->memo_stamp++;
    g_gx->made.pos = pos;
    g_gx->made.entered = ~0u;
    g_gx->made.pending = 0;
    if (!made_begin(g_gx, count)) {
        return -1;
    }
    made_place(g_gx, stride);
    if (g_gx->made.pos != pos || (uint64_t)g_gx->made.limit * stride > MADE_RING_BYTES) {
        return -2; // (the slots begun again from the ring's start, or past its end)
    }
    end = g_gx->made.limit * stride;
    memset(ring + end, GUARD_BYTE, GUARD);
    for (first = 0; first < count; first += run, batch++) {
        run = count - first < WL_XBOX_VSH_BATCH ? count - first : WL_XBOX_VSH_BATCH;
        (void)made_slots(g_gx, &draw, first, run, repeat != 0, first == 0);
        for (n = 0; n < run; n++) {
            if (g_gx->made.index[n] >= g_gx->made.limit || g_gx->made.index[n] > 0xFFFFu) {
                return batch;
            }
        }
        made_flush(g_gx);
        if (g_gx->made.next > g_gx->made.limit) {
            return batch;
        }
    }
    for (n = 0; n < GUARD; n++) {
        if (ring[end + n] != GUARD_BYTE) {
            return 1000 + (int)n;
        }
    }
    g_gx->made.ring_on = false;
    return 0;
}

// a three-word draw of four batches (51 vertices), from the ring's second line: its first slot 4 bytes
// into the line, each batch's last ending 4 bytes into one, three slots up to the next (one was kept a
// batch: past the draw's slots by the third batch)
TEST a_three_word_draw_of_four_batches_stays_within_its_slots(void) {
    ring_begin();
    ASSERT_EQ(0, ring_draw(32, 12, 51, 0));
    ASSERT_EQ(0, ring_draw(32, 12, 59, 0));
    ASSERT_EQ(0, ring_draw(32, 12, 35, 0));
    ring_end();
    PASS();
}

// every small stride (and some larger), from each place in the first lines, every count up to two
// hundred, the vertices made in order and indexed with repeats
TEST every_batch_of_every_small_slot_stays_within_its_draw(void) {
    static const uint32_t strides[] = {8, 12, 16, 20, 24, 28, 32, 44, 64, 88};
    unsigned s, repeat;
    uint32_t pos, count;

    ring_begin();
    for (s = 0; s < sizeof strides / sizeof strides[0]; s++) {
        for (pos = 0; pos < 32u * 12u; pos += 32u) {
            for (count = 1; count <= 200; count++) {
                for (repeat = 0; repeat <= 4; repeat++) {
                    int r = ring_draw(pos, strides[s], count, repeat);

                    if (r != 0) {
                        FAILm("a batch past its draw's slots");
                    }
                }
            }
        }
    }
    ring_end();
    PASS();
}

// a draw whose slots end at the ring's end: none written past it, its indices within 16 bits (8-byte
// slots: the ring's 65536)
TEST a_draw_at_the_rings_end_stays_within_it(void) {
    static const uint32_t strides[] = {8, 12, 20, 44};
    unsigned s;
    uint32_t count;

    ring_begin();
    for (s = 0; s < sizeof strides / sizeof strides[0]; s++) {
        for (count = 17; count <= 300; count++) {
            uint32_t pos = MADE_RING_BYTES - 32u;
            int r;

            // (the last place whose slots still fit, made_place's: from there on they begin again)
            while ((r = ring_draw(pos, strides[s], count, 0)) == -2) {
                pos -= 32u;
            }
            ASSERT_EQ(0, r);
            ASSERT_EQ(0, ring_draw(pos, strides[s], count, 3));
        }
    }
    ring_end();
    PASS();
}

SUITE(ring_suite) {
    RUN_TEST(a_three_word_draw_of_four_batches_stays_within_its_slots);
    RUN_TEST(every_batch_of_every_small_slot_stays_within_its_draw);
    RUN_TEST(a_draw_at_the_rings_end_stays_within_it);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(ring_suite);
    GREATEST_MAIN_END();
}
