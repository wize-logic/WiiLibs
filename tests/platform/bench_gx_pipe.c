// SPDX-License-Identifier: MIT
// The instructions a vertex's words cost on their way into the write-gather pipe: the plain loops
// wii/gx_direct_wii.c had ("old") and the ones it has (gx_pipe_words.h, "new"), over a pipe that is
// one word of memory. tools/qemu_icount.py counts them on ppc-be, a cost per vertex by the
// difference of two sizes:
//   qemu_icount.py --per 4096 --base "./bench_gx_pipe new_words 6 4096" ./bench_gx_pipe new_words 6 8192
// usage: bench_gx_pipe <old_words|new_words|old_index|new_index|old_index_all|new_index_all> <words> <vertices>
// (`words` is a vertex's words, or the colour words of an indexed vertex: order 0, words, 0)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_gx_pipe.h"

#define BATCH 64u
#define WORDS 24u

#define WL_PIPE_T BenchPipe *
#define WL_PIPE_W32(pipe, v) ((pipe)->U32 = (v))
#define WL_PIPE_W16(pipe, v) ((pipe)->U16 = (v))
#include "wl/platform/gx_pipe_words.h"

static uint32_t storage[BATCH][WORDS];
static const uint32_t *batch[BATCH];
static uint32_t indices[BATCH];

static void old_words(const uint32_t *const *vertices, uint32_t n, uint32_t count) {
    uint32_t v, i;

    for (v = 0; v < n; v++) {
        const uint32_t *words = vertices[v];

        for (i = 0; i < count; i++) {
            bench_pipe->U32 = words[i];
        }
    }
}

static void old_vertex_index(uint16_t index, const uint8_t *order, unsigned order_count, const uint32_t *words,
                             uint32_t count) {
    unsigned i, n;

    for (i = 0; i < order_count; i++) {
        if (order[i] == 0) {
            bench_pipe->U16 = index;
            continue;
        }
        for (n = 0; n < order[i] && count != 0; n++, count--) {
            bench_pipe->U32 = *words++;
        }
    }
}

static void old_index(const uint8_t *order, unsigned order_count, const uint32_t *ix, uint32_t base, uint32_t n,
                      const uint32_t *words, uint32_t count) {
    uint32_t v;
    unsigned k, words_sent = 0;

    for (k = 0; k < order_count; k++) {
        words_sent += order[k];
    }
    if (count == 0 && words_sent == 0) {
        for (v = 0; v < n; v++) {
            uint16_t index = (uint16_t)(ix != NULL ? ix[v] - base : v);
            unsigned i;

            for (i = 0; i < order_count; i++) {
                bench_pipe->U16 = index;
            }
        }
        return;
    }
    for (v = 0; v < n; v++) {
        old_vertex_index((uint16_t)(ix != NULL ? ix[v] - base : v), order, order_count, words, count);
    }
}

int main(int argc, char **argv) {
    static const uint8_t mixed[3] = {0, 8, 0}, all[3] = {0, 0, 0};
    static const char *const modes[] = {"old_words", "new_words", "old_index", "new_index", "old_index_all", "new_index_all"};
    unsigned words, vertices, v, i, done, mode;

    if (argc != 4) {
        fprintf(stderr, "usage: bench_gx_pipe mode words vertices\n");
        return 2;
    }
    words = (unsigned)atoi(argv[2]);
    vertices = (unsigned)atoi(argv[3]);
    if (words > WORDS) {
        return 2;
    }
    for (v = 0; v < BATCH; v++) {
        batch[v] = storage[v];
        indices[v] = 4000u + v * 3u;
        for (i = 0; i < WORDS; i++) {
            storage[v][i] = v * 0x01000193u + i;
        }
    }
    for (mode = 0; mode < sizeof modes / sizeof modes[0] && strcmp(argv[1], modes[mode]) != 0; mode++) {
    }
    if (mode == sizeof modes / sizeof modes[0]) {
        return 2;
    }
    for (done = 0; done < vertices; done += BATCH) {
        uint32_t n = vertices - done < BATCH ? vertices - done : BATCH;

        switch (mode) {
        case 0:
            old_words(batch, n, words);
            break;
        case 1:
            wl_gx_pipe_vertices(bench_pipe, batch, n, words);
            break;
        case 2:
            old_index(mixed, 3, indices, 4000, n, storage[0], words);
            break;
        case 3:
            wl_gx_pipe_vertices_index(bench_pipe, mixed, 3, indices, 4000, n, storage[0], words);
            break;
        case 4:
            old_index(all, 3, indices, 4000, n, storage[0], 0);
            break;
        default:
            wl_gx_pipe_vertices_index(bench_pipe, all, 3, indices, 4000, n, storage[0], 0);
            break;
        }
    }
    return 0;
}
