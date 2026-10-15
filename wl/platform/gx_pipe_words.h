// SPDX-License-Identifier: MIT
// The loops that put a vertex's words into the write-gather pipe (wii/gx_direct_wii.c), kept apart
// from libogc so that a host test checks the words and their order and ppc-be counts the
// instructions. The includer defines, before this file, the pipe and how one store reaches it:
//     WL_PIPE_T              the pipe's type, held in a register for a whole call (the Wii: a pointer
//                            to libogc's WGPipe, read once; a test: a recorder)
//     WL_PIPE_W32(pipe, v)   a 32-bit store of v      WL_PIPE_W16(pipe, v)   a 16-bit store of v
// The pipe takes a store as it comes, so the loops only keep the order and spend as little else as
// they can: the pointer read once, a vertex of a size known at compile time written without a loop
// counter, four words read before they are written (a load's latency does not stall the stores
// behind it).
#ifndef WL_PLATFORM_GX_PIPE_WORDS_H
#define WL_PLATFORM_GX_PIPE_WORDS_H

#include <stddef.h>
#include <stdint.h>

#ifndef WL_PIPE_T
#error "define WL_PIPE_T, WL_PIPE_W32 and WL_PIPE_W16 before gx_pipe_words.h"
#endif

// `count` words, a constant after inlining: four read, four written, then the rest
static inline __attribute__((always_inline)) void wl_pipe_fixed(WL_PIPE_T pipe, const uint32_t *w,
                                                                const unsigned count) {
    unsigned i = 0;

#pragma GCC unroll 8
    for (; i + 4u <= count; i += 4u) {
        uint32_t a = w[i], b = w[i + 1u], c = w[i + 2u], e = w[i + 3u];

        WL_PIPE_W32(pipe, a);
        WL_PIPE_W32(pipe, b);
        WL_PIPE_W32(pipe, c);
        WL_PIPE_W32(pipe, e);
    }
    if (count - i == 3u) {
        uint32_t a = w[i], b = w[i + 1u], c = w[i + 2u];

        WL_PIPE_W32(pipe, a);
        WL_PIPE_W32(pipe, b);
        WL_PIPE_W32(pipe, c);
    } else if (count - i == 2u) {
        uint32_t a = w[i], b = w[i + 1u];

        WL_PIPE_W32(pipe, a);
        WL_PIPE_W32(pipe, b);
    } else if (count - i == 1u) {
        uint32_t a = w[i];

        WL_PIPE_W32(pipe, a);
    }
}

// `count` words of any number: the same, with the count read at run time
static inline void wl_gx_pipe_words(WL_PIPE_T pipe, const uint32_t *w, uint32_t count) {
    while (count >= 4u) {
        uint32_t a = w[0], b = w[1], c = w[2], e = w[3];

        WL_PIPE_W32(pipe, a);
        WL_PIPE_W32(pipe, b);
        WL_PIPE_W32(pipe, c);
        WL_PIPE_W32(pipe, e);
        w += 4;
        count -= 4u;
    }
    while (count != 0) {
        WL_PIPE_W32(pipe, *w);
        w++;
        count--;
    }
}

// the largest vertex written by the loops of a size known at compile time (the rest by the loop above)
#define WL_PIPE_FIXED_MAX 16u

// `n` vertices of `count` words each (vertices[v] their words), in order
static inline void wl_gx_pipe_vertices(WL_PIPE_T pipe, const uint32_t *const *vertices, uint32_t n,
                                       uint32_t count) {
    uint32_t v;

    switch (count) {
#define WL_PIPE_CASE(N)                                                 \
    case N:                                                             \
        for (v = 0; v < n; v++) {                                       \
            wl_pipe_fixed(pipe, vertices[v], N);                        \
        }                                                               \
        return;
        WL_PIPE_CASE(1u)
        WL_PIPE_CASE(2u)
        WL_PIPE_CASE(3u)
        WL_PIPE_CASE(4u)
        WL_PIPE_CASE(5u)
        WL_PIPE_CASE(6u)
        WL_PIPE_CASE(7u)
        WL_PIPE_CASE(8u)
        WL_PIPE_CASE(9u)
        WL_PIPE_CASE(10u)
        WL_PIPE_CASE(11u)
        WL_PIPE_CASE(12u)
        WL_PIPE_CASE(13u)
        WL_PIPE_CASE(14u)
        WL_PIPE_CASE(15u)
        WL_PIPE_CASE(16u)
#undef WL_PIPE_CASE
    default:
        for (v = 0; v < n; v++) {
            wl_gx_pipe_words(pipe, vertices[v], count);
        }
        return;
    }
}

// An indexed layout's vertex is a list of items, each a 16-bit index or a run of words (order[i]: 0 an
// index, else that many words, taken in turn from `words` while `count` of them last), as
// WlGxDirect describes them. `n` vertices: vertex v's index is indices[v] - base (indices NULL: v).
#define WL_PIPE_ITEMS_MAX 32u

// `count` (a constant) 16-bit copies of an index, pairs of them as one 32-bit store
static inline __attribute__((always_inline)) void wl_pipe_index_run(WL_PIPE_T pipe, uint16_t index,
                                                                    const unsigned count) {
    const uint32_t pair = (uint32_t)index * 0x10001u;
    unsigned i = 0;

    for (; i + 2u <= count; i += 2u) {
        WL_PIPE_W32(pipe, pair);
    }
    if (i < count) {
        WL_PIPE_W16(pipe, index);
    }
}

static inline void wl_gx_pipe_vertices_index(WL_PIPE_T pipe, const uint8_t *order, unsigned order_count,
                                             const uint32_t *indices, uint32_t base, uint32_t n,
                                             const uint32_t *words, uint32_t count) {
    // (the vertex's stores flattened once: the same words follow every vertex's indices)
    uint32_t item[WL_PIPE_ITEMS_MAX];
    uint8_t is_index[WL_PIPE_ITEMS_MAX];
    unsigned i, k, items = 0, indexes = 0, remaining = count, sends_words = 0;
    uint32_t v;

    for (i = 0; i < order_count; i++) {
        if (order[i] == 0) {
            indexes++;
        } else if (count != 0) {
            sends_words = 1;
        }
    }
    if (!sends_words && indexes <= 4u) {
        // (a few indices alone: nothing to flatten)
        items = indexes;
        order_count = 0;
    }
    for (i = 0; i < order_count; i++) {
        if (order[i] == 0) {
            if (items < WL_PIPE_ITEMS_MAX) {
                is_index[items] = 1;
                item[items] = 0;
            }
            items++;
            continue;
        }
        for (k = 0; k < order[i] && remaining != 0; k++, remaining--) {
            if (items < WL_PIPE_ITEMS_MAX) {
                is_index[items] = 0;
                item[items] = *words;
            }
            items++;
            words++;
        }
    }
    if (items == indexes) {
        // (every attribute indexed: the vertex's index for each)
        // (two indices side by side are the four bytes of one 32-bit store, the stream the same)
        switch (items) {
#define WL_PIPE_CASE(N)                                                          \
    case N:                                                                      \
        if (indices == NULL) {                                                   \
            for (v = 0; v < n; v++) {                                            \
                wl_pipe_index_run(pipe, (uint16_t)v, N);                         \
            }                                                                    \
        } else {                                                                 \
            for (v = 0; v < n; v++) {                                            \
                wl_pipe_index_run(pipe, (uint16_t)(indices[v] - base), N);       \
            }                                                                    \
        }                                                                        \
        return;
            WL_PIPE_CASE(1u)
            WL_PIPE_CASE(2u)
            WL_PIPE_CASE(3u)
            WL_PIPE_CASE(4u)
#undef WL_PIPE_CASE
        default:
            break;
        }
    }
    if (items <= WL_PIPE_ITEMS_MAX) {
        // (the usual shape: A indices, one or two words, B indices, of constant sizes in a loop of their own)
        unsigned a = 0, w = 0, b = 0;

        while (a < items && is_index[a]) {
            a++;
        }
        while (a + w < items && !is_index[a + w]) {
            w++;
        }
        while (a + w + b < items && is_index[a + w + b]) {
            b++;
        }
        if (a + w + b == items && a >= 1u && a <= 2u && w >= 1u && w <= 2u && b <= 3u) {
            const uint32_t w0 = item[a], w1 = item[a + w - 1u];

            switch (a << 8 | w << 4 | b) {
#define WL_PIPE_SHAPE(A, W, B)                                                       \
    case (A << 8 | W << 4 | B):                                                      \
        for (v = 0; v < n; v++) {                                                    \
            uint16_t index = (uint16_t)(indices != NULL ? indices[v] - base : v);    \
            wl_pipe_index_run(pipe, index, A);                                       \
            WL_PIPE_W32(pipe, w0);                                                   \
            if (W == 2) {                                                            \
                WL_PIPE_W32(pipe, w1);                                               \
            }                                                                        \
            wl_pipe_index_run(pipe, index, B);                                       \
        }                                                                            \
        return;
                WL_PIPE_SHAPE(1u, 1u, 0u)
                WL_PIPE_SHAPE(1u, 1u, 1u)
                WL_PIPE_SHAPE(1u, 1u, 2u)
                WL_PIPE_SHAPE(1u, 1u, 3u)
                WL_PIPE_SHAPE(1u, 2u, 0u)
                WL_PIPE_SHAPE(1u, 2u, 1u)
                WL_PIPE_SHAPE(1u, 2u, 2u)
                WL_PIPE_SHAPE(1u, 2u, 3u)
                WL_PIPE_SHAPE(2u, 1u, 0u)
                WL_PIPE_SHAPE(2u, 1u, 1u)
                WL_PIPE_SHAPE(2u, 1u, 2u)
                WL_PIPE_SHAPE(2u, 1u, 3u)
                WL_PIPE_SHAPE(2u, 2u, 0u)
                WL_PIPE_SHAPE(2u, 2u, 1u)
                WL_PIPE_SHAPE(2u, 2u, 2u)
                WL_PIPE_SHAPE(2u, 2u, 3u)
#undef WL_PIPE_SHAPE
            default:
                break;
            }
        }
        for (v = 0; v < n; v++) {
            uint16_t index = (uint16_t)(indices != NULL ? indices[v] - base : v);

            for (i = 0; i < items; i++) {
                if (is_index[i]) {
                    WL_PIPE_W16(pipe, index);
                } else {
                    WL_PIPE_W32(pipe, item[i]);
                }
            }
        }
        return;
    }
    // (a vertex of more items than the table holds: the layout's items walked for each)
    for (v = 0; v < n; v++) {
        uint16_t index = (uint16_t)(indices != NULL ? indices[v] - base : v);
        const uint32_t *w = words - (count - remaining);
        unsigned left = count;

        for (i = 0; i < order_count; i++) {
            if (order[i] == 0) {
                WL_PIPE_W16(pipe, index);
                continue;
            }
            for (k = 0; k < order[i] && left != 0; k++, left--) {
                WL_PIPE_W32(pipe, *w++);
            }
        }
    }
}

#endif
