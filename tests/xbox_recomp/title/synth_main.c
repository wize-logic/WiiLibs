// SPDX-License-Identifier: MIT
// The synthetic title of the wl_add_xbox_title test: xboxrecomp's tools/conformance/mkxbe.py writes
// an XBE with five x86 leaf functions in .text at 0x11000 (add3, submul, shifts, cmpsel, bits);
// the build ran the pipeline on it and compiled the C. This calls each through the generated
// dispatch table with its arguments on the guest stack, as guest code would, and checks eax.
// On the host and ppc-be it runs (ctest); on the Wii it is only built (it has no guest memory map).
#include <stdio.h>
#include <stdlib.h>

#include "recomp_funcs.h"

static int g_failures;

static uint32_t call(uint32_t va, int n, const uint32_t *args) {
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) {
        printf("no recompiled function at %08x\n", (unsigned)va);
        g_failures++;
        return 0;
    }
    uint32_t esp0 = 0x01F00000u;
    g_esp = esp0;
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0xDEADBEEFu);   // the return address the call pushes
    fn();
    if (g_esp != esp0 - 4u * (uint32_t)n) {
        printf("%08x left esp at %08x\n", (unsigned)va, (unsigned)g_esp);
        g_failures++;
    }
    return g_eax;
}

static void expect(const char *what, uint32_t va, int n, const uint32_t *args, uint32_t want) {
    uint32_t got = call(va, n, args);
    printf("%-22s %08x %s\n", what, (unsigned)got, got == want ? "ok" : "WRONG");
    if (got != want) g_failures++;
}

int main(void) {
    uint8_t *guest = calloc(0x02000000u, 1);
    if (!guest) return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    g_xbox_code_lo = 0x00011000u;
    g_xbox_code_hi = 0x00011050u;

    const uint32_t a3[] = {1, 2, 3}, sm[] = {10, 4}, sh1[] = {1, 33}, sh2[] = {3, 32}, sh3[] = {1, 5};
    const uint32_t c1[] = {5, 3}, c2[] = {0xFFFFFFFFu, 2}, b1[] = {6, 3};
    expect("add3(1, 2, 3)", 0x00011000u, 3, a3, 6);
    expect("submul(10, 4)", 0x00011010u, 2, sm, 18);
    expect("shifts(1, 33)", 0x00011020u, 2, sh1, 2);       // x86 masks the count: 1 << 1
    expect("shifts(3, 32)", 0x00011020u, 2, sh2, 3);
    expect("shifts(1, 5)", 0x00011020u, 2, sh3, 32);
    expect("cmpsel(5, 3)", 0x00011030u, 2, c1, 3);
    expect("cmpsel(-1, 2)", 0x00011030u, 2, c2, 0xFFFFFFFFu);
    expect("bits(6, 3)", 0x00011040u, 2, b1, 9);
    free(guest);
    printf("%s\n", g_failures ? "FAILED" : "all ok");
    return g_failures ? 1 : 0;
}
