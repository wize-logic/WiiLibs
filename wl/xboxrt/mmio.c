// SPDX-License-Identifier: MIT
// Device registry and dispatch (mmio.h).
#include <string.h>

#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"

static WlXbDevice g_devices[WL_XB_MAX_DEVICES];
static uint32_t g_count;
static WlXbMmioStats g_stats;

WlResult wl_xb_mmio_register(const WlXbDevice *dev) {
    if (dev == NULL || dev->base < WL_XB_MMIO_BASE || dev->size == 0 ||
        (uint64_t)dev->base + dev->size > 0x100000000ull || dev->read == NULL || dev->write == NULL) {
        return WL_ERR_ARG;
    }
    if (g_count == WL_XB_MAX_DEVICES) {
        return WL_ERR_LIMIT;
    }
    // newest first, so a later registration over a range wins
    memmove(&g_devices[1], &g_devices[0], g_count * sizeof g_devices[0]);
    g_devices[0] = *dev;
    g_count++;
    return WL_OK;
}

void wl_xb_mmio_reset(void) {
    g_count = 0;
    memset(&g_stats, 0, sizeof g_stats);
}

const WlXbDevice *wl_xb_mmio_find(uint32_t va) {
    for (uint32_t i = 0; i < g_count; i++) {
        if (va - g_devices[i].base < g_devices[i].size) {
            return &g_devices[i];
        }
    }
    return NULL;
}

bool wl_xb_mmio_read(uint32_t va, unsigned size, uint64_t *value) {
    const WlXbDevice *d = wl_xb_mmio_find(va);
    if (d == NULL) {
        g_stats.unclaimed++;
        return false;
    }
    g_stats.reads++;
    if (size == 8) {
        uint64_t lo = d->read(d->user, va - d->base, 4);
        uint64_t hi = d->read(d->user, va + 4u - d->base, 4);
        *value = (lo & 0xFFFFFFFFu) | (hi << 32);
    } else {
        uint32_t v = d->read(d->user, va - d->base, size);
        *value = size == 4 ? v : size == 2 ? (v & 0xFFFFu) : (v & 0xFFu);
    }
    return true;
}

bool wl_xb_mmio_write(uint32_t va, uint64_t value, unsigned size) {
    const WlXbDevice *d = wl_xb_mmio_find(va);
    if (d == NULL) {
        g_stats.unclaimed++;
        return false;
    }
    g_stats.writes++;
    if (size == 8) {
        d->write(d->user, va - d->base, (uint32_t)value, 4);
        d->write(d->user, va + 4u - d->base, (uint32_t)(value >> 32), 4);
    } else {
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        d->write(d->user, va - d->base, (uint32_t)value & mask, size);
    }
    return true;
}

void wl_xb_mmio_stats(WlXbMmioStats *out) {
    *out = g_stats;
}
