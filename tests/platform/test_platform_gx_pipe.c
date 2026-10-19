// SPDX-License-Identifier: MIT
// The loops that write a vertex's words into the write-gather pipe (gx_pipe_words.h, used by
// wii/gx_direct_wii.c): against the plain loops they replaced, over a recorder in place of the pipe,
// every store compared in size, value and order, for every vertex size and layout shape.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"

// the pipe as the FIFO sees it: the bytes of each store, big-endian, in order (two 16-bit stores and one
// 32-bit store of the same four bytes are the same stream)
typedef struct {
    uint8_t byte[16384];
    unsigned count;
} Rec;

static void rec_push(Rec *r, unsigned size, uint32_t value) {
    unsigned i;

    for (i = 0; i < size; i++) {
        if (r->count < sizeof r->byte) {
            r->byte[r->count++] = (uint8_t)(value >> (8u * (size - 1u - i)));
        }
    }
}

#define WL_PIPE_T Rec *
#define WL_PIPE_W32(pipe, v) rec_push((pipe), 4, (v))
#define WL_PIPE_W16(pipe, v) rec_push((pipe), 2, (v))
#include "wl/platform/gx_pipe_words.h"

#define MAX_WORDS 48u

// a vertex's words: distinct, with a colour-like word and floats' bit patterns among them
static uint32_t vertex_words[8][MAX_WORDS];
static const uint32_t *vertex_ptr[8];

static void make_vertices(void) {
    unsigned v, i;

    for (v = 0; v < 8; v++) {
        for (i = 0; i < MAX_WORDS; i++) {
            vertex_words[v][i] = 0x80000000u ^ (v * 0x01010101u + i * 0x9e3779b9u + 0xff000001u);
        }
        vertex_ptr[v] = vertex_words[v];
    }
}

static bool same(const Rec *a, const Rec *b) {
    return a->count == b->count && memcmp(a->byte, b->byte, a->count) == 0;
}

TEST vertices_words_match_the_plain_loop(void) {
    static Rec got, want;
    uint32_t count, n, v, i;

    make_vertices();
    for (count = 0; count <= MAX_WORDS; count++) {
        for (n = 0; n <= 8; n++) {
            memset(&got, 0, sizeof got);
            memset(&want, 0, sizeof want);
            wl_gx_pipe_vertices(&got, vertex_ptr, n, count);
            for (v = 0; v < n; v++) {
                for (i = 0; i < count; i++) {
                    rec_push(&want, 4, vertex_ptr[v][i]);
                }
            }
            ASSERT_EQ_FMT(4u * n * count, got.count, "%u");
            ASSERT(same(&got, &want));
            memset(&got, 0, sizeof got);
            wl_gx_pipe_vertices64(&got, vertex_ptr, n, count);
            ASSERT(same(&got, &want));
        }
    }
    PASS();
}

TEST one_vertex_matches_the_plain_loop(void) {
    static Rec got, want;
    uint32_t count, i;

    make_vertices();
    for (count = 0; count <= MAX_WORDS; count++) {
        memset(&got, 0, sizeof got);
        memset(&want, 0, sizeof want);
        wl_gx_pipe_words(&got, vertex_words[3], count);
        for (i = 0; i < count; i++) {
            rec_push(&want, 4, vertex_words[3][i]);
        }
        ASSERT(same(&got, &want));
        memset(&got, 0, sizeof got);
        wl_gx_pipe_words64(&got, vertex_words[3], count);
        ASSERT(same(&got, &want));
    }
    PASS();
}

// the loop wii/gx_direct_wii.c had: an item at a time for each vertex
static void reference_index(Rec *r, const uint8_t *order, unsigned order_count, const uint32_t *indices,
                            uint32_t base, uint32_t n, const uint32_t *words, uint32_t count) {
    uint32_t v;

    for (v = 0; v < n; v++) {
        uint16_t index = (uint16_t)(indices != NULL ? indices[v] - base : v);
        const uint32_t *w = words;
        uint32_t left = count;
        unsigned i, k;

        for (i = 0; i < order_count; i++) {
            if (order[i] == 0) {
                rec_push(r, 2, index);
                continue;
            }
            for (k = 0; k < order[i] && left != 0; k++, left--) {
                rec_push(r, 4, *w++);
            }
        }
    }
}

TEST indexed_vertices_match_the_plain_loop(void) {
    static Rec got, want;
    static const uint8_t shapes[][12] = {
        {0},          {0, 0},       {0, 0, 0},    {0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 1},        {1, 0},
        {0, 2, 0},    {1, 0, 2, 0}, {0, 1, 0, 1}, {3},          {2, 0, 0, 5},    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 7, 0, 7, 0, 7, 0, 7, 0, 7, 0, 7},
    };
    static const unsigned shape_len[] = {1, 2, 3, 4, 5, 2, 2, 3, 4, 4, 1, 4, 12, 12};
    static const uint32_t indices[7] = {1000, 1001, 1005, 1002, 1000, 1999, 1003};
    unsigned s, count;
    uint32_t n, with_indices;

    make_vertices();
    for (s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        for (count = 0; count <= 40; count++) {
            for (n = 0; n <= 7; n++) {
                for (with_indices = 0; with_indices < 2; with_indices++) {
                    const uint32_t *ix = with_indices ? indices : NULL;
                    uint32_t base = with_indices ? 1000u : 0u;

                    memset(&got, 0, sizeof got);
                    memset(&want, 0, sizeof want);
                    wl_gx_pipe_vertices_index(&got, shapes[s], shape_len[s], ix, base, n, vertex_words[2], count);
                    reference_index(&want, shapes[s], shape_len[s], ix, base, n, vertex_words[2], count);
                    ASSERT(same(&got, &want));
                    memset(&got, 0, sizeof got);
                    wl_gx_pipe_vertices_index64(&got, shapes[s], shape_len[s], ix, base, n, vertex_words[2], count);
                    ASSERT(same(&got, &want));
                }
            }
        }
    }
    PASS();
}

// every vertex of A indices, W words, B indices (the loops of constant sizes and their neighbours)
TEST indexed_vertices_of_every_small_shape(void) {
    static Rec got, want;
    static const uint32_t indices[5] = {70, 71, 90, 70, 72};
    uint8_t order[10];
    unsigned a, w, b, count, order_count, i;

    make_vertices();
    for (a = 0; a <= 4; a++) {
        for (w = 0; w <= 3; w++) {
            for (b = 0; b <= 5; b++) {
                for (count = 0; count <= 4; count++) {
                    order_count = 0;
                    for (i = 0; i < a; i++) {
                        order[order_count++] = 0;
                    }
                    if (w != 0) {
                        order[order_count++] = (uint8_t)w;
                    }
                    for (i = 0; i < b; i++) {
                        order[order_count++] = 0;
                    }
                    if (order_count == 0) {
                        continue;
                    }
                    memset(&got, 0, sizeof got);
                    memset(&want, 0, sizeof want);
                    wl_gx_pipe_vertices_index(&got, order, order_count, indices, 70, 5, vertex_words[4], count);
                    reference_index(&want, order, order_count, indices, 70, 5, vertex_words[4], count);
                    ASSERT(same(&got, &want));
                    memset(&got, 0, sizeof got);
                    wl_gx_pipe_vertices_index64(&got, order, order_count, indices, 70, 5, vertex_words[4], count);
                    ASSERT(same(&got, &want));
                    // (and an even number of vertices, and none: the pairs of lone indices)
                    memset(&got, 0, sizeof got);
                    memset(&want, 0, sizeof want);
                    wl_gx_pipe_vertices_index64(&got, order, order_count, indices, 70, 4, vertex_words[4], count);
                    reference_index(&want, order, order_count, indices, 70, 4, vertex_words[4], count);
                    ASSERT(same(&got, &want));
                }
            }
        }
    }
    PASS();
}

// more items than the flattened table holds: the layout walked for each vertex
TEST indexed_vertex_of_many_items_is_walked(void) {
    static Rec got, want;
    uint8_t order[12];
    unsigned i;
    static const uint32_t indices[3] = {5, 9, 7};

    make_vertices();
    for (i = 0; i < 12; i++) {
        order[i] = (uint8_t)(i & 1u ? 8 : 0);
    }
    memset(&got, 0, sizeof got);
    memset(&want, 0, sizeof want);
    wl_gx_pipe_vertices_index(&got, order, 12, indices, 5, 3, vertex_words[1], 48);
    reference_index(&want, order, 12, indices, 5, 3, vertex_words[1], 48);
    ASSERT(got.count > 4u * WL_PIPE_ITEMS_MAX);
    ASSERT(same(&got, &want));
    memset(&got, 0, sizeof got);
    wl_gx_pipe_vertices_index64(&got, order, 12, indices, 5, 3, vertex_words[1], 48);
    ASSERT(same(&got, &want));
    PASS();
}

// every attribute indexed, one to twelve of them (a renderer's made vertices read back by index), the
// indices given or the vertices' own numbers
TEST all_indexed_vertices_of_every_size(void) {
    static Rec got, want;
    static const uint32_t indices[5] = {300, 301, 377, 300, 302};
    uint8_t order[12] = {0};
    unsigned items, pass;

    make_vertices();
    for (items = 1; items <= 12; items++) {
        for (pass = 0; pass < 2; pass++) {
            const uint32_t *ix = pass == 0 ? indices : NULL;

            memset(&got, 0, sizeof got);
            memset(&want, 0, sizeof want);
            wl_gx_pipe_vertices_index(&got, order, items, ix, 300, 5, NULL, 0);
            reference_index(&want, order, items, ix, 300, 5, NULL, 0);
            ASSERT(same(&got, &want));
            memset(&got, 0, sizeof got);
            wl_gx_pipe_vertices_index64(&got, order, items, ix, 300, 5, NULL, 0);
            ASSERT(same(&got, &want));
        }
    }
    PASS();
}

SUITE(gx_pipe_suite) {
    RUN_TEST(vertices_words_match_the_plain_loop);
    RUN_TEST(one_vertex_matches_the_plain_loop);
    RUN_TEST(indexed_vertices_match_the_plain_loop);
    RUN_TEST(indexed_vertices_of_every_small_shape);
    RUN_TEST(indexed_vertex_of_many_items_is_walked);
    RUN_TEST(all_indexed_vertices_of_every_size);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gx_pipe_suite);
    GREATEST_MAIN_END();
}
