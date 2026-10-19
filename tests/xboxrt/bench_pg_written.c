// SPDX-License-Identifier: MIT
// A benchmark of the write watch's look (wl_xb_phys_written) as the GPU's caches make it, on the
// host's paged backend: 64 ranges of guest RAM sized as the capture bench's checks are (d40: a
// tenth of one page, a fifth of 2-4, three tenths of 5-16, two fifths of 17-64, a few of 65-256),
// each armed and read through the window as a cache converts it. A frame stores to four of them,
// then looks 2048 times over all of them (the caches' checks of a frame), arming again and reading
// what was written. Prints the looks and how many answered written, so two builds can show they
// agree.
//
//     bench_pg_written [frames] [old]
//
// "old" looks at every page through the MMU (WlXbMemConfig.old_watch). Count instructions with
// tools/qemu_icount.py on a ppc-be build (--base with fewer frames for the cost of a frame).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"

#define RANGES 64u
#define LOOKS 2048u

int main(int argc, char **argv) {
    uint32_t frames = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 10u;
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    cfg.frame_limit_bytes = 16u << 20; // paged (the watch needs it), no swap file
    cfg.page_cache_bytes = 0;
    cfg.old_watch = argc > 2 && strcmp(argv[2], "old") == 0;
    if (wl_xb_mem_init(&cfg) != WL_OK || wl_xb_fault_install() != WL_OK) {
        fprintf(stderr, "bench_pg_written: no guest memory\n");
        return 2;
    }
    static const uint32_t sizes[8] = {1, 3, 10, 40, 10, 40, 3, 40}; // pages; 128 for two ranges
    uint32_t phys[RANGES], bytes[RANGES], stamp[RANGES];
    for (uint32_t i = 0; i < RANGES; i++) {
        uint32_t pages = i == 13u || i == 51u ? 128u : sizes[i & 7u];
        bytes[i] = pages * WL_XB_PAGE - (i & 1u ? 0x100u : 0u);
        if (wl_xb_phys_alloc(pages * WL_XB_PAGE, 0, 0xFFFFFFFFu, 0, &phys[i]) != WL_OK) {
            fprintf(stderr, "bench_pg_written: allocation failed\n");
            return 2;
        }
        for (uint32_t k = 0; k < pages; k++) {
            wl_xb_wr32(WL_XB_WINDOW | (phys[i] + k * WL_XB_PAGE), i ^ k); // (the guest makes it)
        }
    }
    uint32_t looks = 0, written = 0, sum = 0, x = 12345u;
    for (uint32_t i = 0; i < RANGES; i++) {
        stamp[i] = wl_xb_phys_watch(phys[i], bytes[i]);
    }
    for (uint32_t f = 0; f < frames; f++) {
        for (uint32_t d = 0; d < 4u; d++) { // the guest rewrites a few arrays
            uint32_t i = (f * 7u + d * 17u) % RANGES;
            for (uint32_t off = 0; off < bytes[i]; off += WL_XB_PAGE) {
                wl_xb_wr32(WL_XB_WINDOW | (phys[i] + off), f);
            }
        }
        for (uint32_t n = 0; n < LOOKS; n++) {
            x = x * 1664525u + 1013904223u;
            uint32_t i = (x >> 16) % RANGES;
            looks++;
            if (wl_xb_phys_written(phys[i], bytes[i], stamp[i])) {
                written++;
                stamp[i] = wl_xb_phys_watch(phys[i], bytes[i]); // armed, then read (converted)
                for (uint32_t off = 0; off < bytes[i]; off += WL_XB_PAGE) {
                    sum += wl_xb_rd32(WL_XB_WINDOW | (phys[i] + off));
                }
            }
        }
    }
    printf("bench_pg_written: %u looks, %u written (%u)\n", (unsigned)looks, (unsigned)written, (unsigned)(sum & 0xFFu));
    return 0;
}
