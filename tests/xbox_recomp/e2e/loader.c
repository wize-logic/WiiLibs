/* SPDX-License-Identifier: MIT */
/* The oracle for tests/xbox_recomp/e2e.py: maps a test image where it was linked (0x10000) in a
 * 32-bit x86 process and calls its entry point, so the CPU runs exactly the bytes the lifter read.
 * The x87 runs at precision control 53 bits, which is what the lifted model's binary64 stack holds.
 *
 *     loader IMAGE.bin ENTRY OUT NOUT SEED     (numbers in hex or decimal)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define BASE 0x00010000u
#define SPAN 0x00100000u

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: loader IMAGE ENTRY OUT NOUT SEED\n");
        return 2;
    }
    uint32_t entry = (uint32_t)strtoul(argv[2], 0, 0), outp = (uint32_t)strtoul(argv[3], 0, 0);
    int nout = atoi(argv[4]);
    uint32_t seed = (uint32_t)strtoul(argv[5], 0, 0);
    void *m = mmap((void *)(uintptr_t)BASE, SPAN, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) {
        perror("mmap at 0x10000");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    size_t n = fread(m, 1, SPAN, f);
    fclose(f);
    if (n == 0) return 2;
    unsigned short cw = 0x027F;
    __asm__ __volatile__("fldcw %0" ::"m"(cw));
    uint32_t r = ((uint32_t (*)(uint32_t))(uintptr_t)entry)(seed);
    cw = 0x037F;
    __asm__ __volatile__("fldcw %0" ::"m"(cw));
    printf("ret %08x\n", r);
    for (int i = 0; i < nout; i++) printf("out%02d %08x\n", i, ((volatile uint32_t *)(uintptr_t)outp)[i]);
    return 0;
}
