// SPDX-License-Identifier: MIT
// Device registry and dispatch (mmio.h).
#include <string.h>

#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/base/base.h"

static WlXbDevice g_devices[WL_XB_MAX_DEVICES];
static uint32_t g_count;
static WlXbMmioStats g_stats;
uint32_t g_xb_mmio_depth;
uint32_t g_xb_mmio_last_write;
// The accesses by 256-byte block of registers (a run's report at the exit: where a title keeps going
// back to, each a fault on the Wii): a small table of (device, block, kind), the first comers kept.
// The host's only.
#define HOT_SLOTS 512u
static struct {
    uint32_t key; // device index << 24 | offset << 1 | write, + 1 (0: free)
    uint32_t count;
} g_hot[HOT_SLOTS];

static void note(const WlXbDevice *d, uint32_t off, uint32_t write) {
#if !defined(__wii__)
    uint32_t key = ((uint32_t)(d - g_devices) << 24 | (off >> 8 & 0x7FFFFu) << 1 | write) + 1u;
    uint32_t h = (key * 2654435761u) >> 23;
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t k = g_hot[(h + i) % HOT_SLOTS].key;
        if (k == key || k == 0u) {
            g_hot[(h + i) % HOT_SLOTS].key = key;
            g_hot[(h + i) % HOT_SLOTS].count++;
            return;
        }
    }
#else
    (void)d; // (the host's report: nothing added to the console's faults)
    (void)off;
    (void)write;
#endif
}

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
    g_xb_mmio_last_write = 0;
    note(d, va - d->base, 0u);
    g_xb_mmio_depth++;
    if (size == 8) {
        uint64_t lo = d->read(d->user, va - d->base, 4);
        uint64_t hi = d->read(d->user, va + 4u - d->base, 4);
        *value = (lo & 0xFFFFFFFFu) | (hi << 32);
    } else {
        uint32_t v = d->read(d->user, va - d->base, size);
        *value = size == 4 ? v : size == 2 ? (v & 0xFFFFu) : (v & 0xFFu);
    }
    g_xb_mmio_depth--;
    return true;
}

bool wl_xb_mmio_write(uint32_t va, uint64_t value, unsigned size) {
    const WlXbDevice *d = wl_xb_mmio_find(va);
    if (d == NULL) {
        g_stats.unclaimed++;
        return false;
    }
    g_stats.writes++;
    g_xb_mmio_last_write = 1;
    note(d, va - d->base, 1u);
    g_xb_mmio_depth++;
    if (size == 8) {
        d->write(d->user, va - d->base, (uint32_t)value, 4);
        d->write(d->user, va + 4u - d->base, (uint32_t)(value >> 32), 4);
    } else {
        uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        d->write(d->user, va - d->base, (uint32_t)value & mask, size);
    }
    g_xb_mmio_depth--;
    return true;
}

static uintptr_t g_fault_pc;

void wl_xb_mmio_set_fault_pc(uintptr_t pc) {
    g_fault_pc = pc;
}

uintptr_t wl_xb_mmio_fault_pc(void) {
    return g_fault_pc;
}

void wl_xb_mmio_stats(WlXbMmioStats *out) {
    *out = g_stats;
}

void wl_xb_mmio_log_hot(const char *tag, uint32_t top) {
    static bool used[HOT_SLOTS];
    memset(used, 0, sizeof used);
    for (uint32_t n = 0; n < top; n++) {
        uint32_t best = HOT_SLOTS;
        for (uint32_t i = 0; i < HOT_SLOTS; i++) {
            if (!used[i] && g_hot[i].key != 0u && (best == HOT_SLOTS || g_hot[i].count > g_hot[best].count)) {
                best = i;
            }
        }
        if (best == HOT_SLOTS) {
            break;
        }
        used[best] = true;
        uint32_t key = g_hot[best].key - 1u;
        uint32_t dev = key >> 24;
        WL_LOG_I(tag, "device accesses: %s+0x%06x..+0xff %s %u times", dev < g_count ? g_devices[dev].name : "?",
                 (unsigned)(((key >> 1) & 0x7FFFFu) << 8), (key & 1u) ? "written" : "read", (unsigned)g_hot[best].count);
    }
}
