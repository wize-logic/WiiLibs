// SPDX-License-Identifier: MIT
// Booting a recompiled title (boot.h). The calling host thread does the set-up holding the guest
// lock as the "boot" thread, starts the entry point as a guest thread of its own (each guest
// thread has a large host stack: recompiled frames are C frames), gives the lock up and waits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__wii__)
#include <malloc.h>
#endif

#include "wl/base/endian.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "wl/xboxkrnl/xboxkrnl.h"
#include "wl/xboxrt/boot.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/xbe.h"

#define TAG "xbboot"
#define HEAD_BYTES 0x1000u

void wl_xb_title_config_default(WlXbTitleConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->xbe_name = "default.xbe";
    cfg->ram_mb = 64;
    cfg->keep_free_bytes = 8u << 20;
    cfg->htab_bytes = 256u << 10;
}

static WlResult read_file(void *user, uint32_t offset, void *dst, uint32_t n) {
    return wl_platform_fs_read_at(user, offset, dst, n);
}

int wl_xb_boot_run(const WlXbTitleConfig *cfg) {
    if (cfg == NULL || cfg->title_dir == NULL) {
        return WL_ERR_ARG;
    }
    char data_dir[192], xbe_path[256], log_dir[192];
    snprintf(data_dir, sizeof data_dir, "%s", cfg->data_dir != NULL ? cfg->data_dir : "");
    if (cfg->data_dir == NULL) {
        snprintf(data_dir, sizeof data_dir, "%s/hdd", cfg->title_dir);
    }
    snprintf(xbe_path, sizeof xbe_path, "%s/%s", cfg->title_dir, cfg->xbe_name != NULL ? cfg->xbe_name : "default.xbe");

    // ---- platform: the card, the log ------------------------------------------------------------
#if defined(__wii__)
    if (!wl_mem_ready()) {
        wl_mem_init(NULL);
    }
    wl_platform_fs_mount(WL_DEVICE_SD);
    if (cfg->log_dir == NULL) {
        snprintf(log_dir, sizeof log_dir, "%s/logs", cfg->title_dir);
    } else {
        snprintf(log_dir, sizeof log_dir, "%s", cfg->log_dir);
    }
    wl_platform_log_init(log_dir[0] != '\0' ? log_dir : NULL);
    wl_platform_crash_report_install();
    wl_platform_video_init();
#else
    (void)log_dir;
    wl_platform_fs_mount(WL_DEVICE_SD);
#endif
    WL_LOG_I(TAG, "title %s, data %s", xbe_path, data_dir);

    // ---- the XBE's headers ----------------------------------------------------------------------
    WlPlatformFile *file = NULL;
    WlResult r = wl_platform_fs_open(xbe_path, WL_FILE_READ, &file);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "cannot open %s: %s", xbe_path, wl_result_name(r));
        return r;
    }
    uint64_t file_size = wl_platform_fs_size(file);
    uint32_t head_bytes = file_size < HEAD_BYTES ? (uint32_t)file_size : HEAD_BYTES;
    uint8_t *head = malloc(HEAD_BYTES);
    WlXbe *xbe = calloc(1, sizeof *xbe);
    if (head == NULL || xbe == NULL || wl_platform_fs_read_at(file, 0, head, head_bytes) != WL_OK) {
        WL_LOG_E(TAG, "cannot read %s", xbe_path);
        return WL_ERR_IO;
    }
    r = wl_xbe_parse(head, head_bytes, xbe);
    if (r == WL_ERR_RANGE && head_bytes >= 0x10Cu) {
        uint32_t need = wl_rd_le32(head + 0x108);
        uint8_t *bigger = need <= file_size ? realloc(head, need) : NULL;
        if (bigger != NULL && wl_platform_fs_read_at(file, 0, bigger, need) == WL_OK) {
            head = bigger;
            r = wl_xbe_parse(head, need, xbe);
        }
    }
    free(head);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "%s is not an XBE we can load: %s", xbe_path, wl_result_name(r));
        return r;
    }

    // ---- guest memory -------------------------------------------------------------------------------
    WlXbMemConfig mc;
    wl_xb_mem_config_default(&mc);
    mc.ram_bytes = (cfg->ram_mb != 0 ? cfg->ram_mb : 64u) << 20;
    mc.htab_bytes = cfg->htab_bytes != 0 ? cfg->htab_bytes : mc.htab_bytes;
#if defined(__wii__)
    {
        uint32_t want = cfg->frame_pool_bytes;
        void *pool = NULL;
        if (want == 0) {
            // the largest block the heap gives, less what everything else needs
            uint32_t largest = 0;
            for (uint32_t try_bytes = 64u << 20; try_bytes >= (8u << 20) && largest == 0; try_bytes -= 2u << 20) {
                void *probe = memalign(WL_XB_PAGE, try_bytes);
                if (probe != NULL) {
                    largest = try_bytes;
                    free(probe);
                }
            }
            want = largest > cfg->keep_free_bytes + (8u << 20) ? largest - cfg->keep_free_bytes : largest;
        }
        if (want != 0) {
            pool = memalign(WL_XB_PAGE, want);
        }
        if (pool == NULL) {
            WL_LOG_E(TAG, "no memory for guest frames");
            return WL_ERR_NOMEM;
        }
        mc.frame_pools[0].base = pool;
        mc.frame_pools[0].bytes = want & ~(WL_XB_PAGE - 1u);
    }
#endif
    r = wl_xb_mem_init(&mc);
    if (r == WL_OK) {
        r = wl_xb_fault_install();
    }
    WlXbThread *boot = NULL;
    if (r == WL_OK) {
        WlXbThreadConfig tc;
        wl_xb_thread_config_default(&tc);
        r = wl_xb_threads_init(&tc, &boot);
    }
    if (r == WL_OK) {
        r = wl_xb_thread_setup_guest(boot, 0x10000u, 0, 0, 0);
    }
    if (r != WL_OK) {
        WL_LOG_E(TAG, "guest memory: %s", wl_result_name(r));
        return r;
    }

    // ---- the image, the kernel, the devices -----------------------------------------------------
    r = wl_xbe_load(xbe, read_file, file);
    wl_platform_fs_close(file);
    if (r != WL_OK) {
        return r;
    }
    WlXkConfig kc;
    wl_xk_config_default(&kc);
    kc.title_dir = cfg->title_dir;
    kc.data_dir = data_dir;
    kc.mounts = cfg->mounts;
    kc.mount_count = cfg->mount_count;
    kc.title_id = xbe->title_id;
    kc.inline_first_thread = cfg->inline_first_thread;
    kc.async_io_pending = cfg->async_io_pending;
    kc.trace_calls = cfg->trace_kernel_calls;
    if (cfg->kernel_config != NULL) {
        cfg->kernel_config(&kc, cfg->user);
    }
    r = wl_xk_init(&kc);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "kernel: %s", wl_result_name(r));
        return r;
    }
    uint32_t thunks = 0;
    r = wl_xbe_resolve_thunks(xbe, wl_xk_resolve, NULL, cfg->ordinal_remap, cfg->ordinal_remap_count,
                              cfg->thunk_look_behind, &thunks);
    if (r != WL_OK) {
        return r;
    }
    wl_xb_devices_register_mcpx();
    bool real_gpu = !cfg->gpu_stub && wl_xb_gpu_attach_nv2a() == WL_OK;
    if (!real_gpu) {
        WlXbGpuStub *stub = wl_xb_gpu_stub_create();
        if (stub != NULL) {
            wl_xb_gpu_attach(wl_xb_gpu_stub_ops(), stub);
        }
    }
    WL_LOG_I(TAG, "GPU: %s", real_gpu ? "wl/xbox's NV2A" : "the idle stub (nothing is drawn)");
    if (cfg->manual_lookup != NULL) {
        wl_xb_set_manual_lookup(cfg->manual_lookup);
    }
    WlXbServiceConfig sc;
    wl_xb_service_config_default(&sc);
    r = wl_xb_service_start(&sc);
    if (r != WL_OK) {
        return r;
    }
    if (cfg->before_entry != NULL) {
        cfg->before_entry(cfg->user);
    }

    // ---- the entry point on a guest thread of its own -------------------------------------------
    WlXbThread *entry = NULL;
    uint32_t stack = xbe->stack_commit != 0 ? xbe->stack_commit : 0x10000u;
    r = wl_xb_thread_create(xbe->entry, 0, 0, stack, WL_XB_PRIORITY_NORMAL, false, &entry);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "cannot start the entry point: %s", wl_result_name(r));
        return r;
    }
    snprintf(entry->name, sizeof entry->name, "entry");
    WL_LOG_I(TAG, "entry point 0x%08x on thread %u (stack 0x%x)", (unsigned)xbe->entry, (unsigned)entry->id,
             (unsigned)stack);
    free(xbe);
    wl_xb_unlock(boot);
    int code = wl_xb_process_wait();
    WL_LOG_I(TAG, "the title left with %d", code);
#if defined(__wii__)
    wl_platform_log_flush();
#endif
    return code;
}
