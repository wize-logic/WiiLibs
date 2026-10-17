// SPDX-License-Identifier: MIT
// The page packer's cost (page_pack.c), for tools/qemu_icount.py on ppc-be:
//   bench_page_pack <pages> pack|unpack [file]
// packs (or packs once and unpacks) <pages> pages: the 4 KB pages of `file` in turn (pages saved
// from a run, kept out of the repository), or synthetic ones (records, floats, sparse, random).
// Prints the packed bytes per page; the cost per page is the difference of two counts:
//   qemu_icount.py --per 900 --base "./bench_page_pack 100 pack f" ./bench_page_pack 1000 pack f
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/xboxrt/mem_internal.h"

#define PAGE WL_XB_PAGE
#define SYNTH 16u

static uint32_t g_rng = 99u;

static uint32_t rnd(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    return g_rng >> 8;
}

static void synth(uint8_t *page, uint32_t k) {
    switch (k % 4u) {
    case 0:
        for (uint32_t i = 0; i < PAGE; i += 32u) {
            uint32_t w[8] = {i / 32u, 0x00010000u, 0x80123400u + (rnd() & 0xff0u), 0, 0xffffffffu,
                             0x80200000u + i * 4u, 0, 0x3f800000u};
            memcpy(page + i, w, 32);
        }
        break;
    case 1:
        for (uint32_t i = 0; i < PAGE; i += 16u) {
            float v[4] = {(float)(i / 16u) * 0.25f, (float)(rnd() & 63u), 1.0f, 0.0f};
            memcpy(page + i, v, 16);
        }
        break;
    case 2:
        memset(page, 0, PAGE);
        for (uint32_t i = 0; i < 64u; i++) {
            page[rnd() % PAGE] = (uint8_t)rnd();
        }
        break;
    default:
        for (uint32_t i = 0; i < PAGE; i++) {
            page[i] = (uint8_t)rnd();
        }
        break;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: bench_page_pack <pages> pack|unpack [file]\n");
        return 2;
    }
    uint32_t pages = (uint32_t)strtoul(argv[1], NULL, 0);
    int unpack = strcmp(argv[2], "unpack") == 0;
    uint32_t have = SYNTH;
    uint8_t *src = NULL;
    if (argc > 3) {
        FILE *f = fopen(argv[3], "rb");
        if (f == NULL) {
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        have = size > 0 ? (uint32_t)((unsigned long)size / PAGE) : 0u;
        fseek(f, 0, SEEK_SET);
        src = malloc((size_t)have * PAGE);
        if (have == 0 || src == NULL || fread(src, PAGE, have, f) != have) {
            return 1;
        }
        fclose(f);
    } else {
        src = malloc(SYNTH * PAGE);
        for (uint32_t k = 0; k < SYNTH; k++) {
            synth(src + (size_t)k * PAGE, k);
        }
    }
    uint8_t *packed = malloc((size_t)have * PAGE);
    uint32_t *len = malloc(have * sizeof *len);
    static uint8_t back[PAGE];
    uint64_t total = 0;
    if (unpack) {
        for (uint32_t k = 0; k < have; k++) {
            len[k] = (uint32_t)wl_xb_page_pack(src + (size_t)k * PAGE, packed + (size_t)k * PAGE, PAGE);
        }
    }
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t k = i % have;
        if (!unpack) {
            size_t n = wl_xb_page_pack(src + (size_t)k * PAGE, packed, PAGE);
            total += n != 0 ? n : PAGE;
        } else if (len[k] != 0) {
            if (!wl_xb_page_unpack(packed + (size_t)k * PAGE, len[k], back)) {
                return 1;
            }
            total += len[k];
        } else {
            total += PAGE;
        }
    }
    printf("%u pages, %u bytes a page packed\n", (unsigned)pages, (unsigned)(pages ? total / pages : 0));
    return 0;
}
