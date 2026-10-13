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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/mem.h"

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_gx.h"

#define TAG "xbgpu"

static WlNv2a *g_gpu;
static WlNv2aGx *g_gx;

// A capture of the push buffers and the guest memory they read (wl/xbox/nv2a_capture.h), for
// tools/xbox_nv2a_replay: set by wl_xb_gpu_capture, started after `after` vertical blanks.
static struct {
    char path[WL_FS_PATH_MAX];
    uint32_t frames, after, vblanks;
    WlPlatformFile *file;
    bool armed;
} g_cap;

static WlResult cap_write(void *user, const void *data, uint32_t bytes) {
    (void)user;
    return g_cap.file != NULL ? wl_platform_fs_write(g_cap.file, data, bytes) : WL_ERR_STATE;
}

void wl_xb_gpu_capture(const char *path, uint32_t frames, uint32_t after_vblanks) {
    snprintf(g_cap.path, sizeof g_cap.path, "%s", path != NULL ? path : "");
    g_cap.frames = frames != 0 ? frames : 1u;
    g_cap.after = after_vblanks;
    g_cap.armed = g_cap.path[0] != '\0';
}

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
    if (g_cap.armed && ++g_cap.vblanks >= g_cap.after) {
        g_cap.armed = false;
        if (wl_platform_fs_open(g_cap.path, WL_FILE_WRITE_NEW, &g_cap.file) == WL_OK) {
            WlNv2aCaptureSink sink = {NULL, cap_write};
            if (wl_nv2a_capture_frames(gpu, &sink, g_cap.frames) == WL_OK) {
                WL_LOG_I(TAG, "capturing %u frames to %s", (unsigned)g_cap.frames, g_cap.path);
            }
        } else {
            WL_LOG_E(TAG, "cannot write the capture %s", g_cap.path);
        }
    }
    if (g_cap.file != NULL && !g_cap.armed && !wl_nv2a_capture_active(gpu)) {
        wl_platform_fs_close(g_cap.file);
        g_cap.file = NULL;
        WL_LOG_I(TAG, "capture written: %s", g_cap.path);
    }
}

static bool a_irq(const void *gpu) {
    return wl_nv2a_irq_pending(gpu);
}

static const WlXbGpuOps k_ops = {a_read, a_write, a_vblank, a_irq};

WlResult wl_xb_gpu_attach_nv2a(void) {
    // the draws go to GX (over libogc on the Wii, the host's recorder elsewhere)
    if (!wl_gx_ready() && wl_gx_init() != WL_OK) {
        WL_LOG_E(TAG, "GX did not start: the title's frames are not drawn");
    }
    WlNv2aBackend backend;
    memset(&backend, 0, sizeof backend);
    if (wl_gx_ready()) {
        WlNv2aGxConfig gcfg;
        memset(&gcfg, 0, sizeof gcfg);
        gcfg.textures.budget = 8u << 20; // a title's menu alone keeps several MB of textures
        g_gx = wl_nv2a_gx_create(&gcfg);
        if (g_gx != NULL) {
            wl_nv2a_gx_backend(g_gx, &backend);
        }
    }
    WlNv2aConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.phys_ptr = phys_ptr;
    cfg.user = NULL;
    cfg.backend = g_gx != NULL ? &backend : NULL;
    g_gpu = wl_nv2a_create(&cfg);
    if (g_gpu == NULL) {
        return WL_ERR_NOMEM;
    }
    if (g_gx != NULL) {
        wl_nv2a_gx_attach(g_gx, g_gpu);
    }
#if !defined(__wii__)
    const char *cap = getenv("WL_XB_CAPTURE"); // path[,frames[,after vblanks]]
    if (cap != NULL && cap[0] != '\0') {
        char path[WL_FS_PATH_MAX];
        snprintf(path, sizeof path, "%s", cap);
        uint32_t frames = 2, after = 600;
        char *comma = strchr(path, ',');
        if (comma != NULL) {
            *comma = '\0';
            frames = (uint32_t)strtoul(comma + 1, &comma, 10);
            if (*comma == ',') {
                after = (uint32_t)strtoul(comma + 1, NULL, 10);
            }
        }
        wl_xb_gpu_capture(path, frames, after);
    }
#endif
    return wl_xb_gpu_attach(&k_ops, g_gpu);
}
