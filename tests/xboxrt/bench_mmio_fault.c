// SPDX-License-Identifier: MIT
// A benchmark of a device fault's own work in C, as the Wii's wl_xb_dsi_slow does it for a push
// buffer kick (a stwbrx of PUT to the GPU's user area): the EA to the guest VA, the device looked
// up, the instruction decoded, its EA formed and the store emulated into a device whose write
// handler only keeps the value (the pusher's work is not counted). The exception entry, the
// trampoline and the returns are assembly (dsi_wii.c): count those by hand.
//
//     bench_mmio_fault [faults] [wbc | mirror]
//
// `wbc`: a read of the write-combining flush register (lwbrx of PFB 0x100410, the kick's poll) the
// same way; `mirror`: that read answered by the mirrored page (mmio.h's wl_xb_ppc_fast: what the
// Wii's DSI answers in exception context where the MMU cannot map the copy read-only).
//
// Count instructions with tools/qemu_icount.py on a ppc-be build (--base with fewer faults for the
// cost of one).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/ppc_decode.h"

#define GPU 0xFD000000u
#define PUT 0x00800040u // channel 0's DMA_PUT in the user area

static volatile uint32_t g_put;

static uint32_t dev_read(void *user, uint32_t off, unsigned size) {
    (void)user;
    (void)off;
    (void)size;
    return 0;
}

static void dev_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    (void)user;
    (void)size;
    if (off == PUT) {
        g_put = value;
    }
}

int main(int argc, char **argv) {
    uint32_t faults = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 100000u;
    const char *mode = argc > 2 ? argv[2] : "";
    bool wbc = strcmp(mode, "wbc") == 0, mirror = strcmp(mode, "mirror") == 0;
    if (mirror) {
        WlXbMemConfig mc;
        wl_xb_mem_config_default(&mc);
        if (wl_xb_mem_init(&mc) != WL_OK) {
            return 1;
        }
    }
    wl_xb_mmio_reset();
    // the devices a title has (the GPU last, as wl_xbox_gpu registers it after the others)
    static const struct {
        const char *name;
        uint32_t base, size;
    } others[] = {{"apu", 0xFE800000u, 0x80000u}, {"ac97", 0xFEC00000u, 0x1000u}, {"usb0", 0xFED00000u, 0x1000u},
                  {"usb1", 0xFED08000u, 0x1000u}, {"nic", 0xFEF00000u, 0x400u}};
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; i++) {
        WlXbDevice d = {others[i].name, others[i].base, others[i].size, dev_read, dev_write, NULL};
        wl_xb_mmio_register(&d);
    }
    WlXbDevice gpu = {"nv2a", GPU, 0x01000000u, dev_read, dev_write, NULL};
    wl_xb_mmio_register(&gpu);
    if (mirror && wl_xb_mmio_mirror(GPU + 0x100000u, NULL, 0) != WL_OK) {
        return 1;
    }
    // stwbrx r4, r3, r5: r3 = the user area's EA (as the Wii's DSI gives it), r5 = 0; or lwbrx r4, r3, r5
    // of the PFB's flush register
    const uint32_t insn = 31u << 26 | 4u << 21 | 3u << 16 | 5u << 11 | (wbc || mirror ? 534u : 662u) << 1;
    const uint32_t reg = wbc || mirror ? 0x00100410u : PUT;
    uint32_t gpr[32] = {0};
    uint32_t ok = 0;
    for (uint32_t i = 0; i < faults; i++) {
        gpr[3] = (GPU + reg) & (WL_XB_EA_SPAN - 1u);
        gpr[4] = i * 4u;
        uint32_t ea = gpr[3] + gpr[5];
        uint32_t va = wl_xb_ea_to_va(ea);
        if (va >= WL_XB_MMIO_BASE && wl_xb_mmio_find(va) != NULL) {
            WlXbPpcAccess a;
            if (wl_xb_ppc_decode(insn, &a) == WL_XB_PPC_OK) {
                uint32_t acc = wl_xb_ppc_ea(&a, gpr);
                ok += (mirror ? wl_xb_ppc_fast(&a, gpr, acc, va) : wl_xb_ppc_emulate(&a, gpr, acc, va)) ? 1u : 0u;
            }
        }
    }
    printf("bench_mmio_fault: %u of %u emulated, PUT %08x\n", ok, faults, g_put);
    return ok == faults ? 0 : 1;
}
