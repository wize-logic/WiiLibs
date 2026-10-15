// SPDX-License-Identifier: MIT
// A benchmark of wl_xb_range_ok as a title's own code and the kernel call it: small reads (4 and
// 16 bytes) of objects spread over a handful of committed user pages, now and then one in the RAM's
// window or one that fails. Prints how many ranges passed, so two builds can show they agree.
//
//     bench_range_ok [calls] [expected passes|-] [slow]
//
// With an expected count, exits 1 when the count differs (ctest runs it so). "slow" calls only
// wl_xb_range_ok_slow, the whole check without the inline memo test in front. Count instructions
// with tools/qemu_icount.py on a ppc-be build (--base with fewer calls for the cost of a call).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/xboxrt/mem.h"

int main(int argc, char **argv) {
    uint32_t calls = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1000000u;
    bool slow = argc > 3 && strcmp(argv[3], "slow") == 0; // only the out-of-line half (all there was before)
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    uint32_t va = 0x00400000u;
    if (wl_xb_mem_init(&cfg) != WL_OK || wl_xb_va_reserve(&va, 0x100000u, 0, WL_XB_PROT_READWRITE) != WL_OK) {
        fprintf(stderr, "bench_range_ok: no guest memory\n");
        return 2;
    }
    static const uint32_t pages[8] = {0x00, 0x13, 0x27, 0x35, 0x41, 0x5C, 0x6E, 0xF8}; // (pages of va: apart in the memo)
    for (unsigned k = 0; k < 8; k++) {
        if (wl_xb_va_commit(va + (pages[k] << WL_XB_PAGE_SHIFT), WL_XB_PAGE, WL_XB_PROT_READWRITE) != WL_OK) {
            fprintf(stderr, "bench_range_ok: commit failed\n");
            return 2;
        }
    }
    uint32_t passed = 0, x = 12345u;
    for (uint32_t i = 0; i < calls; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t at = va + (pages[x >> 29] << WL_XB_PAGE_SHIFT) + ((x >> 8) & 0xFF0u);
        uint32_t n = (x & 0x10u) ? 16u : 4u;
        if ((x & 0x3F00u) == 0) {
            at = 0x80010000u + (x & 0xFFF0u); // the window
        } else if ((x & 0x3F00u) == 0x100u) {
            at = va + 0x1000u; // reserved, not committed
        }
        passed += (slow ? wl_xb_range_ok_slow(at, n) : wl_xb_range_ok(at, n)) ? 1u : 0u;
    }
    printf("bench_range_ok: %u calls, %u passed\n", (unsigned)calls, (unsigned)passed);
    if (argc > 2 && strcmp(argv[2], "-") != 0 && passed !=(uint32_t)strtoul(argv[2], NULL, 0)) {
        fprintf(stderr, "bench_range_ok: expected %s passes\n", argv[2]);
        return 1;
    }
    return 0;
}
