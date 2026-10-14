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
#include "wl/platform/hwcount.h"
#include "wl/platform/profile_arm.h"
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
    // the host side: caches, buffers, render targets (12 MB: what the 20 MB kept before held beside
    // 8 MB of textures), and the GX textures converted from the guest's in an arena of their own
    // (14 MB: a10's opening draws ~11.6 MB of them in each frame; an arena too small for a frame's
    // textures converts them again in every frame)
    cfg->keep_free_bytes = 12u << 20;
    cfg->texture_bytes = 14u << 20;
    cfg->htab_bytes = 256u << 10;
}

// The sampling profiler, armed by profile.txt in the title's folder (profile_arm.h): a profile
// begins once the title has run `after` seconds; the dumps go beside the control file.
static WlProfileArm g_profile_arm;
static uint64_t g_boot_us;

static bool profile_ready(void *user, const WlProfileArmSettings *s, uint32_t run, char *note, size_t size) {
    (void)user;
    uint64_t up_s = (wl_platform_time_us() - g_boot_us) / 1000000u;
    if (up_s < (uint64_t)s->after + (uint64_t)run * s->seconds) {
        return false;
    }
    snprintf(note, size, "uptime %u s", (unsigned)up_s);
    return true;
}

static void profile_issue(void *user, WlProfileArmIssue issue, const char *key, uint32_t line) {
    (void)user;
    WL_LOG_W(TAG, "profile.txt line %u: %s %s", (unsigned)line,
             issue == WL_PROFILE_ARM_UNKNOWN_KEY ? "no such setting" : "no number for", key);
}

static void profile_arm(const char *folder) {
    (void)wl_hwcount_arm(folder); // hwcount.txt (not found: not armed)
    WlProfileArmConfig config;
    if (wl_profile_arm_config_load(&config, folder, profile_issue, NULL) != WL_OK) {
        return;
    }
    config.hooks.ready = profile_ready;
    wl_profile_arm_launch(&g_profile_arm, &config);
}

static void input_vblank(void *user) {
    (void)user;
    wl_xb_input_frame();
}

static WlResult read_file(void *user, uint32_t offset, void *dst, uint32_t n) {
    return wl_platform_fs_read_at(user, offset, dst, n);
}

// The title config's XBE patches (tools/xbox_recomp.py writes the table into the title's own
// code; weak, so a runtime built without one links): the code was lifted from the patched bytes,
// and the loaded image gets the same, each checked against what it replaces.
typedef struct {
    uint32_t va, bytes;
    const uint8_t *from, *to;
} WlXbTitlePatch;
extern const WlXbTitlePatch wl_xb_title_patches[] __attribute__((weak));
extern const uint32_t wl_xb_title_patch_count __attribute__((weak));

static void apply_title_patches(void) {
    // (the count's address alone: GCC holds an array's address non-null even when weak)
    const uint32_t *count = &wl_xb_title_patch_count;
    __asm__("" : "+r"(count)); // not folded: weak, so it may be null
    if (count == NULL) {
        return;
    }
    for (uint32_t i = 0; i < wl_xb_title_patch_count; i++) {
        const WlXbTitlePatch *p = &wl_xb_title_patches[i];
        uint8_t now[64];
        if (p->bytes == 0 || p->bytes > sizeof now || !wl_xb_range_ok(p->va, p->bytes)) {
            WL_LOG_E(TAG, "patch %u at 0x%08x: bad size or address, skipped", (unsigned)i, (unsigned)p->va);
            continue;
        }
        wl_xb_copy_out(now, p->va, p->bytes);
        if (memcmp(now, p->from, p->bytes) != 0) {
            WL_LOG_E(TAG, "patch %u at 0x%08x: the image does not have the expected bytes, skipped",
                     (unsigned)i, (unsigned)p->va);
            continue;
        }
        wl_xb_copy_in(p->va, p->to, p->bytes);
    }
    if (wl_xb_title_patch_count != 0) {
        WL_LOG_I(TAG, "%u title patches applied", (unsigned)wl_xb_title_patch_count);
    }
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
    // the swap file the paging writes changed pages to when the frames run out (the title's hard
    // disk folder: on the card on the Wii)
    char swap_path[sizeof data_dir + 16];
    snprintf(swap_path, sizeof swap_path, "%s/pagefile.sys", data_dir);
    mc.swap_path = cfg->swap_path != NULL ? cfg->swap_path : swap_path;
    mc.swap_bytes = cfg->swap_bytes;
#if !defined(__wii__)
    {
        // a frame limit: the host pages as the Wii does (for a title run at the Wii's size)
        mc.frame_limit_bytes = cfg->frame_pool_bytes & ~(WL_XB_PAGE - 1u);
        const char *mb = getenv("WL_XB_FRAME_MB");
        const char *kb = getenv("WL_XB_FRAME_KB");
        if (kb != NULL && kb[0] != '\0') {
            mc.frame_limit_bytes = (uint32_t)strtoul(kb, NULL, 10) << 10;
        } else if (mb != NULL && mb[0] != '\0') {
            mc.frame_limit_bytes = (uint32_t)strtoul(mb, NULL, 10) << 20;
        }
        mc.frame_limit_bytes &= ~(WL_XB_PAGE - 1u);
        if (mc.frame_limit_bytes != 0) {
            WL_LOG_I(TAG, "guest frames: %u KB (a frame limit: paged as on the Wii)", (unsigned)(mc.frame_limit_bytes >> 10));
        }
    }
#endif
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
            // probing, then `keep_free_bytes` and the textures' arena given back from the smaller
            // one first
            for (unsigned i = 0; i < 2u; i++) {
                for (uint32_t t = 64u << 20; t >= (1u << 20) && pool[i] == NULL; t -= 1u << 20) {
                    pool[i] = memalign(WL_XB_PAGE, t);
                    bytes[i] = pool[i] != NULL ? t : 0;
                }
            }
            uint32_t give = cfg->keep_free_bytes + cfg->texture_bytes;
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
    // the GPU's converted textures: an arena of their own, had now, while the heap's free memory is
    // still in one piece (the Wii: what the frame pools gave back for it)
    uint32_t texture_bytes = cfg->texture_bytes;
#if !defined(__wii__)
    {
        const char *mb = getenv("WL_XB_TEXTURE_MB"); // the Wii's arena on the host
        if (mb != NULL && mb[0] != '\0') {
            texture_bytes = (uint32_t)strtoul(mb, NULL, 10) << 20;
        }
    }
#endif
    texture_bytes &= ~31u;
    void *texture_arena = NULL;
    if (!cfg->gpu_stub && texture_bytes != 0) {
#if defined(__wii__)
        texture_arena = memalign(32, texture_bytes);
#else
        texture_arena = aligned_alloc(32, texture_bytes);
#endif
        if (texture_arena == NULL) {
            WL_LOG_W(TAG, "no %u KB for the textures' arena: they share the heap", (unsigned)(texture_bytes >> 10));
        }
    }
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
    apply_title_patches();
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
    WlXbAudioConfig audio;
    wl_xb_audio_config_default(&audio);
    audio.off = audio.off || cfg->audio_off;
    {
        // a file "noaudio" in the title's folder turns the models off too, for an A/B on the console
        char flag[sizeof data_dir + 32];
        snprintf(flag, sizeof flag, "%s/noaudio", cfg->title_dir);
        if (wl_platform_fs_exists(flag)) {
            audio.off = true;
            WL_LOG_I(TAG, "audio: off (%s)", flag);
        }
    }
    r = wl_xb_audio_attach(&audio);
    if (r != WL_OK && r != WL_ERR_UNSUPPORTED) {
        WL_LOG_W(TAG, "audio: %s (the title runs silent)", wl_result_name(r));
    }
    bool real_gpu = !cfg->gpu_stub && wl_xb_gpu_attach_nv2a(texture_arena, texture_bytes) == WL_OK;
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
    g_boot_us = wl_platform_time_us();
    profile_arm(cfg->title_dir);

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
