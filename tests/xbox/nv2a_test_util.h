// SPDX-License-Identifier: MIT
// What the NV2A tests share: a guest RAM of 4 MiB (its last megabyte instance memory), push
// buffers written into it word by word, RAMHT entries and objects set up through PRAMIN as a
// driver writes them, and a device over it.
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_kelvin.h"

enum {
    T_RAM = 4u * 1024u * 1024u,
    T_RAMIN = T_RAM - 0x100000u, // instance offset 0's guest-physical address
    T_RAMHT = 0x10000u,          // RAMHT's instance offset (NV_PFIFO_RAMHT base 0x100)
    T_PB = 0x10000u,             // where the tests' push buffers start
    T_CHANNEL = 0
};

typedef struct TGuest {
    uint8_t *ram;
    WlNv2a *dev;
    uint32_t put; // the next push buffer word's address
} TGuest;

static void *t_phys(void *user, uint32_t phys, uint32_t bytes) {
    TGuest *g = user;
    if (phys >= T_RAM || bytes > T_RAM - phys) {
        return NULL;
    }
    return g->ram + phys;
}

static inline uint32_t t_f(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}

static inline void t_w32(TGuest *g, uint32_t phys, uint32_t value) {
    wl_wr_le32(g->ram + phys, value);
}

static inline uint32_t t_r32(TGuest *g, uint32_t phys) {
    return wl_rd_le32(g->ram + phys);
}

static inline void t_mmio(TGuest *g, uint32_t offset, uint32_t value) {
    wl_nv2a_mmio_write(g->dev, offset, value, 4);
}

static inline uint32_t t_mmio_r(TGuest *g, uint32_t offset) {
    return wl_nv2a_mmio_read(g->dev, offset, 4);
}

// push buffer words at g->put
static inline void t_word(TGuest *g, uint32_t word) {
    t_w32(g, g->put, word);
    g->put += 4;
}

// a method header (subchannel 0) and its words
static inline void t_method(TGuest *g, uint32_t method, uint32_t count) {
    t_word(g, (count << 18) | method);
}

static inline void t_m1(TGuest *g, uint32_t method, uint32_t value) {
    t_method(g, method, 1);
    t_word(g, value);
}

// DMA_PUT of the tests' channel = g->put: runs what was written since the last kick
static inline void t_kick(TGuest *g) {
    t_mmio(g, WL_NV2A_USER + T_CHANNEL * WL_NV2A_USER_STRIDE + WL_NV2A_USER_DMA_PUT, g->put);
}

static inline uint32_t t_get(TGuest *g) {
    return t_mmio_r(g, WL_NV2A_USER + T_CHANNEL * WL_NV2A_USER_STRIDE + WL_NV2A_USER_DMA_GET);
}

// instance memory written through PRAMIN, as a driver does
static inline void t_ramin(TGuest *g, uint32_t instance, uint32_t value) {
    t_mmio(g, WL_NV2A_PRAMIN + instance, value);
}

// a RAMHT entry for the channel (entry `slot`): handle -> object at `instance`, engine 1
static inline void t_ramht(TGuest *g, unsigned slot, uint32_t handle, uint32_t instance) {
    t_ramin(g, T_RAMHT + slot * 8, handle);
    t_ramin(g, T_RAMHT + slot * 8 + 4, 0x80000000u | (T_CHANNEL << 24) | (1u << 16) | (instance >> 4));
}

// a DMA object at `instance`: class 0x3D, base, limit
static inline void t_dma(TGuest *g, uint32_t instance, uint32_t base, uint32_t limit) {
    t_ramin(g, instance, 0x0000303Du | ((base & 0xFFFu) << 20));
    t_ramin(g, instance + 4, limit);
    t_ramin(g, instance + 8, (base & 0xFFFFF000u) | 3u);
    t_ramin(g, instance + 12, (base & 0xFFFFF000u) | 3u);
}

static inline WlNv2a *t_create(TGuest *g, const WlNv2aBackend *backend) {
    WlNv2aConfig cfg;

    memset(g, 0, sizeof *g);
    g->ram = calloc(1, T_RAM);
    memset(&cfg, 0, sizeof cfg);
    cfg.phys_ptr = t_phys;
    cfg.user = g;
    cfg.backend = backend;
    cfg.ram_bytes = T_RAM;
    g->dev = wl_nv2a_create(&cfg);
    g->put = T_PB;
    if (g->dev != NULL) {
        // RAMHT at instance 0x10000, 4 KiB; GET and PUT at the push buffer's start
        t_mmio(g, WL_NV2A_PFIFO_RAMHT, 0x100u);
        t_mmio(g, WL_NV2A_PFIFO_CACHE1_PUSH1, T_CHANNEL);
        t_mmio(g, WL_NV2A_PFIFO_CACHE1_DMA_GET, T_PB);
        t_mmio(g, WL_NV2A_PFIFO_CACHE1_DMA_PUT, T_PB);
    }
    return g->dev;
}

// the 3D object bound to subchannel 0 through RAMHT (handle 0x97000001 at instance 0x11000),
// with context DMAs over all of RAM (handles 0xD0000000 + n at 0x11100 + 16 n; n = 1 a page at
// 0x8000, the notifiers')
static inline void t_setup_3d(TGuest *g) {
    unsigned i;

    t_ramin(g, 0x11000, WL_NV2A_CLASS_KELVIN);
    t_ramht(g, 0, 0x97000001u, 0x11000);
    for (i = 0; i < 4; i++) {
        // (handle 1, the notifiers': a page at 0x8000)
        t_dma(g, 0x11100 + 16 * i, i == 1 ? 0x8000u : 0, i == 1 ? 0xFFFu : T_RAM - 1);
        t_ramht(g, 1 + i, 0xD0000000u + i, 0x11100 + 16 * i);
    }
    t_m1(g, 0, 0x97000001u);
    // A, B, colour, zeta, vertex A and B, semaphores and reports over RAM (handle 0)
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_A, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_B, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_COLOR, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_ZETA, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_A, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_VERTEX_B, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_SEMAPHORE, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_REPORT, 0xD0000000u);
    t_m1(g, WL_NV2A_K_SET_CONTEXT_DMA_NOTIFIES, 0xD0000001u);
    t_kick(g);
}

static inline void t_destroy(TGuest *g) {
    wl_nv2a_destroy(g->dev);
    free(g->ram);
    g->dev = NULL;
    g->ram = NULL;
}
