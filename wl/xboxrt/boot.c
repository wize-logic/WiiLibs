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
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/play.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/xbe.h"

#define TAG "xbboot"
#define HEAD_BYTES 0x1000u

void wl_xb_title_config_default(WlXbTitleConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->xbe_name = "default.xbe";
    cfg->ram_mb = 64;
    cfg->keep_free_bytes = 14u << 20; // the host side: GX textures converted from the guest's, caches
    cfg->htab_bytes = 256u << 10;
}

static void input_vblank(void *user) {
    (void)user;
    wl_xb_input_frame();
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
    WlResult mem_result = WL_OK; // logged once the log is open (it is not yet)
    if (!wl_mem_ready()) {
        // wl_mem takes only what its heaps need (MEM1's budgets and arenas, a small MEM2 asset
        // heap): the rest stays malloc's, for the guest's frames and the host-side buffers. Its
        // default would claim both regions whole and leave no room for the frame pools.
        static WlMemTagDef tags[WL_TAG_BUILTIN_COUNT];
        memcpy(tags, wl_mem_builtin_tags(), sizeof tags);
        tags[WL_TAG_ASSET].budget = 2u << 20;
        tags[WL_TAG_AUDIO_BANK].budget = 0;
        WlMemConfig mcfg;
        wl_mem_config_default(&mcfg);
        mcfg.tags = tags;
        mcfg.tag_count = WL_TAG_BUILTIN_COUNT;
        // (the per-frame arenas and the scene stack are a game engine's; the runtime does not use
        // them, and MEM1 beside a recompiled title's ~12 MB of code has ~7 MB to give)
        mcfg.frame_arena_size = 64u * 1024u;
        mcfg.scene_stack_size = 64u * 1024u;
        mcfg.claim[WL_MEM1] = WL_MEM_CLAIM_NEEDED;
        mcfg.claim[WL_MEM2] = WL_MEM_CLAIM_NEEDED;
        mem_result = wl_mem_init(&mcfg);
    }
    wl_platform_fs_mount(WL_DEVICE_SD);
    if (cfg->log_dir == NULL) {
        snprintf(log_dir, sizeof log_dir, "%s/logs", cfg->title_dir);
    } else {
        snprintf(log_dir, sizeof log_dir, "%s", cfg->log_dir);
    }
    wl_platform_log_init(log_dir[0] != '\0' ? log_dir : NULL);
    if (mem_result != WL_OK || !wl_mem_ready()) {
        WL_LOG_E(TAG, "wl_mem_init: %s (ready %d): GX and the heaps that need it will not start",
                 wl_result_name(mem_result), (int)wl_mem_ready());
    }
    wl_platform_crash_report_install();
    wl_platform_video_init();
#else
    (void)log_dir;
    wl_platform_fs_mount(WL_DEVICE_SD);
#endif
    WL_LOG_I(TAG, "title %s, data %s", xbe_path, data_dir);
    {
        char seeds[sizeof data_dir + 24];
        snprintf(seeds, sizeof seeds, "%s/recomp_seeds.json", data_dir);
        wl_xb_set_seed_file(seeds);
    }

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
        uint32_t want = cfg->frame_pool_bytes & ~(WL_XB_PAGE - 1u);
        void *pool[2] = {NULL, NULL};
        uint32_t bytes[2] = {0, 0};
        if (want != 0) {
            pool[0] = memalign(WL_XB_PAGE, want);
            bytes[0] = pool[0] != NULL ? want : 0;
        } else {
            // the two largest blocks the heap gives (MEM2's and what is left of MEM1's), held while
            // probing, then `keep_free_bytes` given back from the smaller one first
            for (unsigned i = 0; i < 2u; i++) {
                for (uint32_t t = 64u << 20; t >= (1u << 20) && pool[i] == NULL; t -= 1u << 20) {
                    pool[i] = memalign(WL_XB_PAGE, t);
                    bytes[i] = pool[i] != NULL ? t : 0;
                }
            }
            uint32_t give = cfg->keep_free_bytes;
            for (int i = 1; i >= 0 && give != 0; i--) {
                if (pool[i] == NULL) {
                    continue;
                }
                free(pool[i]);
                uint32_t keep = bytes[i] > give + (1u << 20) ? bytes[i] - give : 0;
                give -= bytes[i] - keep < give ? bytes[i] - keep : give;
                pool[i] = keep != 0 ? memalign(WL_XB_PAGE, keep) : NULL;
                bytes[i] = pool[i] != NULL ? keep : 0;
            }
        }
        if (pool[0] == NULL && pool[1] == NULL) {
            WL_LOG_E(TAG, "no memory for guest frames");
            return WL_ERR_NOMEM;
        }
        for (unsigned i = 0, n = 0; i < 2u; i++) {
            if (pool[i] != NULL) {
                mc.frame_pools[n].base = pool[i];
                mc.frame_pools[n].bytes = bytes[i];
                WL_LOG_I(TAG, "guest frames: %u KB at %p", (unsigned)(bytes[i] >> 10), pool[i]);
                n++;
            }
        }
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
    // the controller ports (the host's controllers on the Wii) before the USB frame tick reads them
#if defined(__wii__)
    wl_xb_input_reset(true);
#else
    wl_xb_input_reset(false);
#endif
    wl_xb_service_add_vblank_hook(input_vblank, NULL);
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
    // the play harness: the title's setting, or on the Wii a file on the card
    {
        const char *play = cfg->play;
#if defined(__wii__)
        static char line[160];
        if (play == NULL) {
            char path[WL_FS_PATH_MAX];
            WlPlatformFile *pf = NULL;
            snprintf(path, sizeof path, "%s/play.cfg", cfg->title_dir);
            if (wl_platform_fs_open(path, WL_FILE_READ, &pf) == WL_OK) {
                uint32_t got = 0;
                wl_platform_fs_read_some_at(pf, 0, line, sizeof line - 1u, &got);
                wl_platform_fs_close(pf);
                line[got] = '\0';
                line[strcspn(line, "\r\n")] = '\0';
                play = line;
            }
        }
#endif
        if (play != NULL && play[0] != '\0') {
            char out[WL_FS_PATH_MAX];
            if (cfg->play_out == NULL) {
                snprintf(out, sizeof out, "%s/play", cfg->title_dir);
                wl_platform_fs_mkdir(out);
            }
            r = wl_xb_play_start(play, cfg->title_dir, cfg->play_out != NULL ? cfg->play_out : out);
            if (r != WL_OK) {
                WL_LOG_E(TAG, "the play harness did not start (%s): %s", play, wl_result_name(r));
            }
        }
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
