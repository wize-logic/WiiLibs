// SPDX-License-Identifier: MIT
// The MCPX's idle devices and the GPU attachment (devices.h), and the sparse register file the
// models keep their registers in (regfile.h).
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/regfile.h"
#include "wl/xboxrt/usb.h"

#define TAG "xbdev"

// ---- Register file -----------------------------------------------------------------------------

bool wl_xb_regfile_init(WlXbRegFile *rf, uint32_t cap, const char *name) {
    memset(rf, 0, sizeof *rf);
    rf->keys = calloc(cap, sizeof *rf->keys);
    rf->vals = calloc(cap, sizeof *rf->vals);
    rf->cap = cap;
    rf->name = name;
    return rf->keys != NULL && rf->vals != NULL;
}

static uint32_t slot_of(const WlXbRegFile *rf, uint32_t offset, bool *found) {
    uint32_t key = (offset & ~3u) | 1u;
    uint32_t i = ((offset >> 2) * 2654435761u) & (rf->cap - 1u);
    for (uint32_t n = 0; n < rf->cap; n++) {
        if (rf->keys[i] == key) {
            *found = true;
            return i;
        }
        if (rf->keys[i] == 0) {
            *found = false;
            return i;
        }
        i = (i + 1u) & (rf->cap - 1u);
    }
    *found = false;
    return rf->cap;
}

uint32_t wl_xb_regfile_get32(const WlXbRegFile *rf, uint32_t offset) {
    bool found;
    uint32_t i = slot_of(rf, offset, &found);
    return found ? rf->vals[i] : 0;
}

bool wl_xb_regfile_has(const WlXbRegFile *rf, uint32_t offset) {
    bool found;
    slot_of(rf, offset, &found);
    return found;
}

void wl_xb_regfile_set32(WlXbRegFile *rf, uint32_t offset, uint32_t value) {
    bool found;
    uint32_t i = slot_of(rf, offset, &found);
    if (!found) {
        if (i == rf->cap || rf->used + 1u > rf->cap * 3u / 4u) {
            if (!rf->full_warned) {
                rf->full_warned = true;
                WL_LOG_W(TAG, "%s: register file full, writes from 0x%x on are dropped", rf->name, (unsigned)offset);
            }
            return;
        }
        rf->keys[i] = (offset & ~3u) | 1u;
        rf->used++;
    }
    rf->vals[i] = value;
}

uint32_t wl_xb_regfile_read(const WlXbRegFile *rf, uint32_t offset, unsigned size) {
    uint32_t w = wl_xb_regfile_get32(rf, offset);
    uint32_t shift = (offset & 3u) * 8u;
    return size >= 4 ? w : (w >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
}

void wl_xb_regfile_write(WlXbRegFile *rf, uint32_t offset, uint32_t value, unsigned size) {
    if (size >= 4) {
        wl_xb_regfile_set32(rf, offset, value);
        return;
    }
    uint32_t shift = (offset & 3u) * 8u;
    uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
    uint32_t w = wl_xb_regfile_get32(rf, offset);
    wl_xb_regfile_set32(rf, offset, (w & ~mask) | ((value << shift) & mask));
}

// ---- A generic idle device ---------------------------------------------------------------------

typedef struct Dev Dev;
typedef bool (*DevRead)(Dev *d, uint32_t off, unsigned size, uint32_t *out);  // true: handled
typedef bool (*DevWrite)(Dev *d, uint32_t off, uint32_t value, unsigned size); // true: handled

struct Dev {
    const char *name;
    WlXbRegFile regs;
    DevRead read;
    DevWrite write;
    uint64_t start_us;
    uint32_t seen[32]; // offsets already logged
    uint32_t seen_count;
};

static void note(Dev *d, uint32_t off, bool write, uint32_t value) {
    for (uint32_t i = 0; i < d->seen_count; i++) {
        if (d->seen[i] == (off | (write ? 1u : 0u))) {
            return;
        }
    }
    if (d->seen_count < WL_ARRAY_COUNT(d->seen)) {
        d->seen[d->seen_count++] = off | (write ? 1u : 0u);
        WL_LOG_D(TAG, "%s: %s at +0x%05x%s", d->name, write ? "write" : "read", (unsigned)(off & ~1u),
                 write ? "" : " (kept value)");
    }
    (void)value;
}

static uint32_t dev_read(void *user, uint32_t off, unsigned size) {
    Dev *d = user;
    uint32_t v = 0;
    if (d->read != NULL && d->read(d, off, size, &v)) {
        return v;
    }
    note(d, off & ~1u, false, 0);
    return wl_xb_regfile_read(&d->regs, off, size);
}

static void dev_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    Dev *d = user;
    if (d->write != NULL && d->write(d, off, value, size)) {
        return;
    }
    note(d, off & ~1u, true, value);
    wl_xb_regfile_write(&d->regs, off, value, size);
}

static WlResult add_device(const char *name, uint32_t base, uint32_t size, DevRead rd, DevWrite wr, uint32_t regs) {
    Dev *d = calloc(1, sizeof *d);
    if (d == NULL || !wl_xb_regfile_init(&d->regs, regs, name)) {
        return WL_ERR_NOMEM;
    }
    d->name = name;
    d->read = rd;
    d->write = wr;
    d->start_us = wl_platform_time_us();
    WlXbDevice dev = {name, base, size, dev_read, dev_write, d};
    return wl_xb_mmio_register(&dev);
}

// APU: the global processor's sample counter (DirectSound waits for it to move) runs at 48 kHz.
static bool apu_read(Dev *d, uint32_t off, unsigned size, uint32_t *out) {
    (void)size;
    if ((off & ~3u) == 0x20010u) {
        *out = (uint32_t)((wl_platform_time_us() - d->start_us) * 48u / 1000u);
        return true;
    }
    return false;
}

// AC'97 (ACI): no primary codec ready (GLOB_STA 0x130 bit 8 clear), so the audio driver gives up;
// the bus masters' reset-registers bit (CR bit 1, at 0x10B, 0x11B, 0x12B, 0x17B) clears at once.
static bool ac97_read(Dev *d, uint32_t off, unsigned size, uint32_t *out) {
    (void)size;
    if ((off & ~3u) == 0x130u) {
        *out = 0; // GLOB_STA: nothing ready
        return true;
    }
    (void)d;
    return false;
}

static bool ac97_write(Dev *d, uint32_t off, uint32_t value, unsigned size) {
    if (off >= 0x100u && off < 0x180u) {
        uint32_t cr = (off & ~0xFu) | 0xBu; // the control register byte of this bus master
        if (off <= cr && cr < off + size) {
            uint32_t shift = (cr - off) * 8u;
            value &= ~(0x2u << shift); // RR self-clears
            wl_xb_regfile_write(&d->regs, off, value, size);
            return true;
        }
    }
    return false;
}

// The flash: reads 0, writes ignored.
static bool flash_read(Dev *d, uint32_t off, unsigned size, uint32_t *out) {
    (void)d;
    (void)off;
    (void)size;
    *out = 0;
    return true;
}

static bool flash_write(Dev *d, uint32_t off, uint32_t value, unsigned size) {
    (void)d;
    (void)off;
    (void)value;
    (void)size;
    return true;
}

WlResult wl_xb_devices_register_mcpx_usb(const WlXbUsbConfig *usb) {
    WlResult r = add_device("apu", WL_XB_APU_BASE, WL_XB_APU_SIZE, apu_read, NULL, 4096);
    if (r == WL_OK) {
        r = add_device("ac97", WL_XB_AC97_BASE, WL_XB_AC97_SIZE, ac97_read, ac97_write, 256);
    }
    if (r == WL_OK) {
        r = wl_xb_usb_register(usb); // the host controllers and the gamepads (usb.h)
    }
    if (r == WL_OK) {
        r = add_device("nic", WL_XB_NIC_BASE, WL_XB_NIC_SIZE, NULL, NULL, 256);
    }
    if (r == WL_OK) {
        r = add_device("flash", WL_XB_FLASH_BASE, WL_XB_FLASH_SIZE, flash_read, flash_write, 16);
    }
    return r;
}

WlResult wl_xb_devices_register_mcpx(void) {
    return wl_xb_devices_register_mcpx_usb(NULL);
}

// ---- The GPU -----------------------------------------------------------------------------------

static const WlXbGpuOps *g_gpu_ops;
static void *g_gpu;

static uint32_t gpu_read(void *user, uint32_t off, unsigned size) {
    return g_gpu_ops->mmio_read(user, off, size);
}

static void gpu_write(void *user, uint32_t off, uint32_t value, unsigned size) {
    g_gpu_ops->mmio_write(user, off, value, size);
}

static void gpu_vblank(void *user) {
    (void)user;
    if (g_gpu_ops->vblank != NULL) {
        g_gpu_ops->vblank(g_gpu);
    }
}

static bool gpu_irq(void *user) {
    (void)user;
    return g_gpu_ops->irq_pending != NULL && g_gpu_ops->irq_pending(g_gpu);
}

WlResult wl_xb_gpu_attach(const WlXbGpuOps *ops, void *gpu) {
    if (ops == NULL || ops->mmio_read == NULL || ops->mmio_write == NULL) {
        return WL_ERR_ARG;
    }
    g_gpu_ops = ops;
    g_gpu = gpu;
    WlXbDevice dev = {"nv2a", WL_XB_NV2A_BASE, WL_XB_NV2A_SIZE, gpu_read, gpu_write, gpu};
    WlResult r = wl_xb_mmio_register(&dev);
    if (r != WL_OK) {
        return r;
    }
    wl_xb_service_set_vblank(gpu_vblank, NULL);
    wl_xb_irq_set_level(WL_XB_IRQ_GPU, gpu_irq, NULL);
    return WL_OK;
}

#if !defined(WL_XBRT_HAVE_NV2A)
WlResult wl_xb_gpu_attach_nv2a(void) {
    return WL_ERR_UNSUPPORTED;
}
#endif
