// SPDX-License-Identifier: MIT
// A benchmark of the service poll a device access makes (wl_xb_device_safe_point's
// wl_xb_service_poll_levels when nothing is due): KeTickCount brought up to date and the level
// sources asked, as a title has them (the USB host controllers, the GPU, the APU and the AC'97),
// none asserted. Prints the polls that found work (none) and the tick written.
//
//     bench_irq_poll [polls]
//
// Count instructions with tools/qemu_icount.py on a ppc-be build (--base with fewer polls for the
// cost of one).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"

static uint32_t g_reg[5][4]; // a device's enable and status words (each line's source looks at two)

static bool level(void *user) {
    const uint32_t *r = user;
    return (r[0] & r[1]) != 0;
}

int main(int argc, char **argv) {
    uint32_t polls = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 100000u;
    WlXbMemConfig cfg;
    wl_xb_mem_config_default(&cfg);
    uint32_t va = 0x00400000u;
    if (wl_xb_mem_init(&cfg) != WL_OK || wl_xb_va_reserve(&va, 0x10000u, 0, WL_XB_PROT_READWRITE) != WL_OK ||
        wl_xb_va_commit(va, WL_XB_PAGE, WL_XB_PROT_READWRITE) != WL_OK) {
        fprintf(stderr, "bench_irq_poll: no guest memory\n");
        return 2;
    }
    static const uint32_t lines[5] = {WL_XB_IRQ_USB0, WL_XB_IRQ_GPU, WL_XB_IRQ_APU, WL_XB_IRQ_AC97, WL_XB_IRQ_USB1};
    for (unsigned i = 0; i < 5; i++) {
        g_reg[i][0] = 0xFFu; // enabled, nothing raised
        wl_xb_irq_set_level(lines[i], level, g_reg[i]);
    }
    wl_xb_service_set_tick_va(va);
    for (uint32_t i = 0; i < polls; i++) {
        wl_xb_service_poll_levels();
    }
    uint32_t lines_now = wl_xb_irq_lines();
    printf("bench_irq_poll: %u polls, lines %08x, tick %u\n", polls, (unsigned)lines_now,
           (unsigned)wl_xb_rd32(va));
    return lines_now == 0 ? 0 : 1;
}
