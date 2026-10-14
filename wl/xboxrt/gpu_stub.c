// SPDX-License-Identifier: MIT
// The idle NV2A (devices.h): what Direct3D's initialisation, kick-off and vblank paths poll,
// answered as a GPU that consumes everything at once and draws nothing. Register offsets (from
// 0xFD000000) are the NV2x family's as envytools' rnndb names them: PMC 0x000000, PBUS 0x001000,
// PFIFO 0x002000 (CACHE1 0x003200), PTIMER 0x009000, PFB 0x100000, PGRAPH 0x400000, PCRTC
// 0x600000, PRAMIN 0x700000, the channels' USER areas 0x800000 + channel * 0x10000.
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/regfile.h"

#define PMC_BOOT_0 0x000000u
#define PMC_INTR_0 0x000100u
#define PMC_INTR_EN_0 0x000140u
#define PBUS_INTR_0 0x001100u
#define PFIFO_INTR_0 0x002100u
#define PFIFO_RUNOUT_STATUS 0x002400u
#define PFIFO_CACHE1_STATUS 0x003214u
#define PFIFO_CACHE1_DMA_PUT 0x003240u
#define PFIFO_CACHE1_DMA_GET 0x003244u
#define PFIFO_CACHE1_DMA_SUBROUTINE 0x00324Cu
#define PTIMER_INTR_0 0x009100u
#define PTIMER_TIME_0 0x009400u
#define PTIMER_TIME_1 0x009410u
#define PFB_CSTATUS 0x10020Cu
#define PFB_WBC 0x100410u
#define PGRAPH_INTR 0x400100u
#define PGRAPH_STATUS 0x400700u
#define PCRTC_INTR_0 0x600100u
#define PCRTC_INTR_EN_0 0x600140u
#define PCRTC_START 0x600800u
#define USER_BASE 0x800000u
#define USER_DMA_PUT 0x40u
#define USER_DMA_GET 0x44u

#define PMC_INTR_PCRTC 0x01000000u

struct WlXbGpuStub {
    WlXbRegFile regs;
    uint64_t start_us;
    WlXbGpuStubStats stats;
};

static uint32_t sized(uint32_t word, uint32_t off, unsigned size) {
    uint32_t shift = (off & 3u) * 8u;
    return size >= 4 ? word : (word >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
}

static uint32_t stub_read(void *user, uint32_t off, unsigned size) {
    WlXbGpuStub *g = user;
    uint32_t reg = off & ~3u;
    uint32_t v;
    switch (reg) {
    case PMC_BOOT_0:
        v = 0x02A000A1u; // NV2A, revision A1
        break;
    case PMC_INTR_0: {
        uint32_t pcrtc = wl_xb_regfile_get32(&g->regs, PCRTC_INTR_0) & wl_xb_regfile_get32(&g->regs, PCRTC_INTR_EN_0);
        v = (pcrtc & 1u) ? PMC_INTR_PCRTC : 0u;
        break;
    }
    case PBUS_INTR_0:
    case PFIFO_INTR_0:
    case PTIMER_INTR_0:
    case PGRAPH_INTR:
    case PGRAPH_STATUS:
        v = 0; // nothing pending, the graph idle
        break;
    case PFIFO_RUNOUT_STATUS:
    case PFIFO_CACHE1_STATUS:
        v = 0x10u; // empty
        break;
    case PFIFO_CACHE1_DMA_GET:
    case PFIFO_CACHE1_DMA_SUBROUTINE:
        v = wl_xb_regfile_get32(&g->regs, PFIFO_CACHE1_DMA_PUT);
        break;
    case PTIMER_TIME_0:
    case PTIMER_TIME_1: {
        uint64_t ns = (wl_platform_time_us() - g->start_us) * 1000u;
        v = reg == PTIMER_TIME_0 ? (uint32_t)(ns << 5) & 0xFFFFFFE0u : (uint32_t)(ns >> 27) & 0x1FFFFFFFu;
        break;
    }
    case PFB_CSTATUS:
        v = 64u << 20; // the framebuffer's size: all of the RAM
        break;
    case PFB_WBC:
        v = wl_xb_regfile_get32(&g->regs, PFB_WBC) & ~0x10000u; // the flush is done
        break;
    default:
        if (reg >= USER_BASE && (reg & 0xFFFFu) == USER_DMA_GET) {
            v = wl_xb_regfile_get32(&g->regs, reg - USER_DMA_GET + USER_DMA_PUT); // consumed
        } else {
            v = wl_xb_regfile_get32(&g->regs, reg);
        }
        break;
    }
    return sized(v, off, size);
}

static void stub_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    WlXbGpuStub *g = user;
    uint32_t reg = off & ~3u;
    if (size < 4) {
        uint32_t shift = (off & 3u) * 8u;
        uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        value = (wl_xb_regfile_get32(&g->regs, reg) & ~mask) | ((value << shift) & mask);
    }
    switch (reg) {
    case PMC_INTR_0:
    case PBUS_INTR_0:
    case PFIFO_INTR_0:
    case PTIMER_INTR_0:
    case PGRAPH_INTR:
        return; // write-one-to-clear of bits that are never set
    case PCRTC_INTR_0: {
        uint32_t old = wl_xb_regfile_get32(&g->regs, PCRTC_INTR_0);
        if (old & value & 1u) {
            g->stats.irq_acks++;
        }
        wl_xb_regfile_set32(&g->regs, PCRTC_INTR_0, old & ~value);
        return;
    }
    case PCRTC_START:
        g->stats.flips++;
        g->stats.last_start = value;
        break;
    default:
        if (reg >= USER_BASE && (reg & 0xFFFFu) == USER_DMA_PUT) {
            g->stats.puts++;
            g->stats.last_put = value;
            wl_xb_regfile_set32(&g->regs, PFIFO_CACHE1_DMA_PUT, value);
        }
        break;
    }
    wl_xb_regfile_set32(&g->regs, reg, value);
}

static void stub_vblank(void *user) {
    WlXbGpuStub *g = user;
    g->stats.vblanks++;
    wl_xb_regfile_set32(&g->regs, PCRTC_INTR_0, wl_xb_regfile_get32(&g->regs, PCRTC_INTR_0) | 1u);
}

static bool stub_irq(const void *user) {
    const WlXbGpuStub *g = user;
    uint32_t pcrtc = wl_xb_regfile_get32(&g->regs, PCRTC_INTR_0) & wl_xb_regfile_get32(&g->regs, PCRTC_INTR_EN_0);
    return (pcrtc & 1u) && (wl_xb_regfile_get32(&g->regs, PMC_INTR_EN_0) & 1u);
}

static const WlXbGpuOps k_stub_ops = {stub_read, stub_write, stub_vblank, stub_irq};

WlXbGpuStub *wl_xb_gpu_stub_create(void) {
    WlXbGpuStub *g = calloc(1, sizeof *g);
    if (g == NULL || !wl_xb_regfile_init(&g->regs, 16384, "nv2a stub")) {
        free(g);
        return NULL;
    }
    g->start_us = wl_platform_time_us();
    return g;
}

const WlXbGpuOps *wl_xb_gpu_stub_ops(void) {
    return &k_stub_ops;
}

void wl_xb_gpu_stub_stats(const WlXbGpuStub *gpu, WlXbGpuStubStats *out) {
    *out = gpu->stats;
}

#if !defined(WL_XBRT_HAVE_NV2A)
// without wl/xbox's model there is nothing to capture
void wl_xb_gpu_capture(const char *path, uint32_t frames, uint32_t after_vblanks) {
    (void)path;
    (void)frames;
    (void)after_vblanks;
}

bool wl_xb_gpu_capture_busy(void) {
    return false;
}

void wl_xb_gpu_set_program_list(const char *path) {
    (void)path;
}

void wl_xb_gpu_set_transform_unit(bool on) {
    (void)on;
}

void wl_xb_gpu_set_vertex_cache(void *arena, uint32_t bytes) {
    (void)arena;
    (void)bytes;
}
#endif
