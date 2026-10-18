// SPDX-License-Identifier: MIT
// The block operations' cost for tools/qemu_icount.py on the big-endian build:
//     bench_memfast <copy|fill|zero|compare> <fast|libc|newlib> <bytes> <dst offset> <src offset> <calls>
// makes <calls> calls on one pair of buffers (the destination and source at those offsets from a
// 32-byte line). Two runs at different <calls> give the instructions a call:
//     qemu_icount.py --per 1000 --base "./bench_memfast copy fast 64 0 1 10" ./bench_memfast copy fast 64 0 1 1010
// "libc" is the program's C library (glibc on ppc-be). "newlib" is devkitPPC's newlib objects
// renamed nl_* (a build with BENCH_NEWLIB that links them, as ~/wl-work/fast-copy notes say).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/memfast.h"

#if defined(BENCH_NEWLIB)
void *nl_memmove(void *dst, const void *src, size_t n);
void *nl_memcpy(void *dst, const void *src, size_t n);
void *nl_memset(void *dst, int c, size_t n);
int nl_memcmp(const void *a, const void *b, size_t n);
#endif

static _Alignas(32) uint8_t g_a[16384 + 64], g_b[16384 + 64];

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s <copy|fill|zero|compare> <fast|libc|newlib> <bytes> <dst off> <src off> <calls>\n",
                argv[0]);
        return 2;
    }
    const char *op = argv[1], *impl = argv[2];
    size_t n = strtoul(argv[3], NULL, 10), doff = strtoul(argv[4], NULL, 10), soff = strtoul(argv[5], NULL, 10);
    unsigned long calls = strtoul(argv[6], NULL, 10);
    if (n > 16384u || doff > 63u || soff > 63u) {
        return 2;
    }
    void *(*volatile copy)(void *, const void *, size_t) = memcpy;
    void *(*volatile fill)(void *, int, size_t) = memset;
    int (*volatile cmp)(const void *, const void *, size_t) = memcmp;
    if (strcmp(impl, "fast") == 0) {
        copy = wl_memfast_copy;
        fill = wl_memfast_fill;
        cmp = wl_memfast_compare;
    }
#if defined(BENCH_NEWLIB)
    else if (strcmp(impl, "newlib") == 0) {
        copy = nl_memcpy;
        fill = nl_memset;
        cmp = nl_memcmp;
    }
#endif
    else if (strcmp(impl, "libc") != 0) {
        return 2;
    }
    uint8_t *d = g_a + doff, *s = g_b + soff;
    for (size_t i = 0; i < sizeof g_b; i++) {
        g_b[i] = (uint8_t)(i * 7u);
    }
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i]; // a compare runs to the end: equal blocks
    }
    unsigned sum = 0;
    for (unsigned long i = 0; i < calls; i++) {
        if (op[0] == 'c' && op[1] == 'o' && op[2] == 'p') {
            copy(d, s, n);
        } else if (op[0] == 'f') {
            fill(d, 0x5A, n);
        } else if (op[0] == 'z') {
            fill(d, 0, n);
        } else {
            sum += (unsigned)cmp(d, s, n);
        }
    }
    printf("%u %u\n", (unsigned)d[n / 2u], sum);
    return 0;
}
