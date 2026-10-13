// SPDX-License-Identifier: MIT
// wl/xbox's NV2A model behind the runtime's GPU interface (devices.h). Built only when wl/xbox
// provides wl/xbox/nv2a.h (CMake defines WL_XBRT_HAVE_NV2A). Written against the interface the
// plan gives the model:
//
//     WlNv2a  *wl_nv2a_create(const WlNv2aConfig *cfg);   // cfg->phys_ptr(user, phys, bytes), cfg->user
//     uint32_t wl_nv2a_mmio_read(WlNv2a *, uint32_t offset, unsigned size);
//     void     wl_nv2a_mmio_write(WlNv2a *, uint32_t offset, uint32_t value, unsigned size);
//     void     wl_nv2a_vblank(WlNv2a *);
//     bool     wl_nv2a_irq_pending(const WlNv2a *);
//
// phys_ptr is the guest's window: the host pointer of guest-physical bytes, contiguous in the
// host's address space (on the Wii a page-table address: for the CPU only, never for GX or DMA).
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/mem.h"

#include "wl/xbox/nv2a.h"

static void *phys_ptr(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    if ((uint64_t)phys + bytes > wl_xb_ram_bytes()) {
        return NULL;
    }
    return wl_xb_phys_ptr(phys);
}

static uint32_t a_read(void *gpu, uint32_t offset, unsigned size) {
    return wl_nv2a_mmio_read(gpu, offset, size);
}

static void a_write(void *gpu, uint32_t offset, uint32_t value, unsigned size) {
    wl_nv2a_mmio_write(gpu, offset, value, size);
}

static void a_vblank(void *gpu) {
    wl_nv2a_vblank(gpu);
}

static bool a_irq(const void *gpu) {
    return wl_nv2a_irq_pending(gpu);
}

static const WlXbGpuOps k_ops = {a_read, a_write, a_vblank, a_irq};

WlResult wl_xb_gpu_attach_nv2a(void) {
    WlNv2aConfig cfg = {0};
    cfg.phys_ptr = phys_ptr;
    cfg.user = NULL;
    WlNv2a *gpu = wl_nv2a_create(&cfg);
    if (gpu == NULL) {
        return WL_ERR_NOMEM;
    }
    return wl_xb_gpu_attach(&k_ops, gpu);
}
