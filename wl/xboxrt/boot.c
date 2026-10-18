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
#include "wl/platform/mem_census.h"
#include "wl/platform/net_stream.h"
#include "wl/platform/platform.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/hwcount.h"
#include "wl/platform/profile_arm.h"
#include "wl/xboxkrnl/xboxkrnl.h"
#include "wl/xboxrt/bench.h"
#include "wl/xboxrt/boot.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/play.h"
#include "wl/xboxrt/runopts.h"
#include "wl/xboxrt/thread.h"
#include "wl/xboxrt/xbe.h"

#define TAG "xbboot"
#define HEAD_BYTES 0x1000u

void wl_xb_title_config_default(WlXbTitleConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->xbe_name = "default.xbe";
    cfg->ram_mb = 64;
    // the host side: caches, buffers, render targets (9 MB: the census's exact high-water mark, "peak
    // +N KB", was 6.7 MB in a real-Wii run of a10's opening, shots included, and 6.7-6.8 MB on the
    // host in a10, b30 and four level loads; 2 MB stay over it for levels not measured, more render
    // targets, fragmentation and the freeze profile's 320 KB; the rest goes to the guest's frames), and the GX
    // textures converted from the guest's in an arena of their own (15 MB, the vertex arrays' 1 MB in its end: a10's opening
    // draws up to 14.2 MB of them in a frame; an arena too small for a frame's textures keeps them
    // smaller, or refuses them and the draws lose them)
    cfg->keep_free_bytes = 9u << 20;
    cfg->texture_bytes = 15u << 20;
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

#if defined(__wii__)
// The largest page-aligned block memalign gives now, of at least 1 MB: tried down from `room` (the
// heap's figure for the piece sbrk can still take, less what newlib adds to a request) in 64 KB
// steps; room 0 (not known): from 64 MB down in 1 MB steps.
static void *largest_block(size_t room, uint32_t *bytes) {
    const uint32_t margin = 64u << 10;
    uint32_t step = room != 0 ? 64u << 10 : 1u << 20;
    uint32_t t = 64u << 20;
    if (room != 0) {
        t = room < (1u << 20) + margin ? 0 : room - margin < t ? (uint32_t)(room - margin) & ~(step - 1u) : t;
    }
    for (; t >= (1u << 20); t -= step) {
        void *p = memalign(WL_XB_PAGE, t);
        if (p != NULL) {
            *bytes = t;
            return p;
        }
    }
    *bytes = 0;
    return NULL;
}
#endif

// Guest memory (the Wii's frames from the heap), the GPU's textures' arena (when `want_textures`)
// and the guest threads with the calling thread as the boot thread, holding the guest lock: what
// the title and the capture bench both need.
static WlResult guest_memory(const WlXbTitleConfig *cfg, const char *data_dir, bool want_textures, void **arena,
                             uint32_t *arena_bytes, WlXbThread **boot) {
    WlXbMemConfig mc;
    wl_xb_mem_config_default(&mc);
    mc.ram_bytes = (cfg->ram_mb != 0 ? cfg->ram_mb : 64u) << 20;
    mc.htab_bytes = cfg->htab_bytes != 0 ? cfg->htab_bytes : mc.htab_bytes;
    // the swap file the paging writes changed pages to when the frames run out (the title's hard
    // disk folder: on the card on the Wii)
    char swap_path[WL_FS_PATH_MAX + 16];
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
            // the two largest blocks the heap gives, held while probing: MEM1's rest first, while
            // sbrk is still there (libogc's sbrk moves on to MEM2 for good at the first request
            // MEM1 cannot hold, and MEM1's rest is then out of malloc's reach: a probe that began
            // with MEM2's block left ~7 MB of MEM1 to nobody), then MEM2's
            WlPlatformHeapInfo h;
            wl_platform_heap_info(&h);
            bool mem1_first = h.known && h.elsewhere != 0;
            pool[0] = largest_block(h.known ? h.top : 0, &bytes[0]);
            wl_platform_heap_info(&h);
            if (mem1_first && h.elsewhere == 0) {
                WL_LOG_W(TAG, "guest frames: MEM1's rest was not had (sbrk moved on to MEM2)");
            }
            pool[1] = largest_block(mem1_first ? (h.elsewhere != 0 ? h.elsewhere : h.top) : 0, &bytes[1]);
            // then `keep_free_bytes` and the textures' arena given back from the larger block first
            // (MEM2's): the host side keeps one piece at MEM2's top, the frames MEM1's faster memory
            uint32_t give = cfg->keep_free_bytes + cfg->texture_bytes;
            unsigned larger = bytes[0] >= bytes[1] ? 0u : 1u;
            for (unsigned k = 0; k < 2u && give != 0; k++) {
                unsigned i = k == 0 ? larger : 1u - larger;
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
    if (want_textures && texture_bytes != 0) {
#if defined(__wii__)
        texture_arena = memalign(32, texture_bytes);
#else
        texture_arena = aligned_alloc(32, texture_bytes);
#endif
        if (texture_arena == NULL) {
            WL_LOG_W(TAG, "no %u KB for the textures' arena: they share the heap", (unsigned)(texture_bytes >> 10));
        }
    }
    *arena = texture_arena;
    *arena_bytes = texture_bytes;
#if !defined(__wii__)
    {
        // the Wii's heap on the host: what is allocated from here on (the guest memory's tables, the
        // threads' stacks, the GPU's and everyone's buffers: on the Wii all of it comes out of
        // keep_free_bytes, the frames and the textures' arena being had already) capped, so that a
        // run shows what would fail first. The host's own stand-ins (the software GX of the
        // screenshots, the GX recorders) are not counted (mem_census.h). WL_XB_HEAP_MB=12 is the Wii's.
        const char *mb = getenv("WL_XB_HEAP_MB");
        const char *kb = getenv("WL_XB_HEAP_KB");
        size_t cap = 0;
        if (kb != NULL && kb[0] != '\0') {
            cap = (size_t)strtoul(kb, NULL, 10) << 10;
        } else if (mb != NULL && mb[0] != '\0') {
            cap = (size_t)strtoul(mb, NULL, 10) << 20;
        }
        if (cap != 0) {
            WlResult capped = wl_platform_heap_cap(cap);
            WL_LOG_I(TAG, "host heap: %u KB from here on, as the Wii's (%s)", (unsigned)(cap >> 10),
                     capped == WL_OK ? "capped" : wl_result_name(capped));
        }
    }
#endif
    // the heap's high-water marks from here on (mem_census.h): the host side's own use, which
    // keep_free_bytes holds on the Wii, exact (the census and the exit print them)
    wl_platform_heap_mark();
    WlResult r = wl_xb_mem_init(&mc);
    if (r == WL_OK) {
        r = wl_xb_fault_install();
    }
    if (r == WL_OK) {
        WlXbThreadConfig tc;
        wl_xb_thread_config_default(&tc);
        // a file "deterministic" in the title's folder (WL_XB_DETERMINISTIC on the host): the virtual
        // clock on the console too, for a run to compare with the host's
        char det_flag[WL_FS_PATH_MAX];
        snprintf(det_flag, sizeof det_flag, "%s/deterministic", cfg->title_dir);
        tc.deterministic = cfg->deterministic || wl_platform_fs_exists(det_flag);
#if !defined(__wii__)
        // the deterministic mode's costs: WL_XB_DET_NS=<backward jump>,<kernel call>,<device access>
        const char *det_ns = getenv("WL_XB_DET_NS");
        if (det_ns != NULL && det_ns[0] != '\0') {
            unsigned be = 0, call = 0, dev = 0;
            if (sscanf(det_ns, "%u,%u,%u", &be, &call, &dev) >= 1) {
                tc.det_backedge_ns = be;
                tc.det_call_ns = call != 0 ? call : tc.det_call_ns;
                tc.det_device_ns = dev != 0 ? dev : tc.det_device_ns;
            }
        }
#endif
        r = wl_xb_threads_init(&tc, boot);
    }
    if (r == WL_OK) {
        r = wl_xb_thread_setup_guest(*boot, 0x10000u, 0, 0, 0);
    }
    if (r != WL_OK) {
        WL_LOG_E(TAG, "guest memory: %s", wl_result_name(r));
    }
    return r;
}

// The play harness's setting (play.h): the title's, or on the Wii the first line of play.cfg in the
// title's folder when there is one.
static const char *play_setting(const WlXbTitleConfig *cfg) {
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
    return play;
}

// The switches that leave work out of what GX gets (gx_direct.h, WlGxBisect), to find what hangs a
// console's GPU by halving: files of the title's folder (or switch=), each off when absent. "gxnotex":
// every texture one fixed texture; "gxnotlut": the colour-indexed ones only; "gxtlutmem1": the TLUTs
// loaded from copies in MEM1; "gxmaxdraws" (its text N): only a frame's first N primitives;
// "gxnoquad": no quad of the targets; "gxnozrestore": the back buffer's restore without its depth;
// "gxnocopy": no texture copy out of the EFB; "gxtexgap": the vertex's texture coordinates in the
// descriptor's slots of their own names, gaps and all (the layout that hung); "gxlatez": no early depth
// test; "gxvtxinv": GX's vertex cache invalidated before every primitive.
static bool title_flag(const WlXbTitleConfig *cfg, const char *name, uint32_t *value) {
    char flag[WL_FS_PATH_MAX];
    WlPlatformFile *f = NULL;
    char text[16];
    uint32_t got = 0;

    snprintf(flag, sizeof flag, "%s/%s", cfg->title_dir, name);
    if (!wl_platform_fs_exists(flag)) {
        return false;
    }
    if (value != NULL && wl_platform_fs_open(flag, WL_FILE_READ, &f) == WL_OK) {
        wl_platform_fs_read_some_at(f, 0, text, sizeof text - 1u, &got);
        wl_platform_fs_close(f);
        text[got] = '\0';
        *value = (uint32_t)strtoul(text, NULL, 10);
    }
    return true;
}

static void gx_bisect_options(const WlXbTitleConfig *cfg) {
    WlGxBisect b;

    memset(&b, 0, sizeof b);
    b.no_textures = title_flag(cfg, "gxnotex", NULL);
    b.no_tlut = title_flag(cfg, "gxnotlut", NULL);
    b.tlut_mem1 = title_flag(cfg, "gxtlutmem1", NULL);
    title_flag(cfg, "gxmaxdraws", &b.max_draws);
    b.no_quads = title_flag(cfg, "gxnoquad", NULL);
    b.no_z_restore = title_flag(cfg, "gxnozrestore", NULL);
    b.no_copies = title_flag(cfg, "gxnocopy", NULL);
    b.texcoord_gaps = title_flag(cfg, "gxtexgap", NULL);
    b.late_z = title_flag(cfg, "gxlatez", NULL);
    b.vtx_inval = title_flag(cfg, "gxvtxinv", NULL);
    if (b.no_textures || b.no_tlut || b.tlut_mem1 || b.max_draws != 0 || b.no_quads || b.no_z_restore ||
        b.no_copies || b.texcoord_gaps || b.late_z || b.vtx_inval) {
        WL_LOG_W(TAG, "GX bisection:%s%s%s max draws %u%s%s%s%s%s%s", b.no_textures ? " gxnotex" : "",
                 b.no_tlut ? " gxnotlut" : "", b.tlut_mem1 ? " gxtlutmem1" : "", (unsigned)b.max_draws,
                 b.no_quads ? " gxnoquad" : "", b.no_z_restore ? " gxnozrestore" : "",
                 b.no_copies ? " gxnocopy" : "", b.texcoord_gaps ? " gxtexgap" : "", b.late_z ? " gxlatez" : "",
                 b.vtx_inval ? " gxvtxinv" : "");
    }
    wl_gx_bisect_set(&b);
}

// The GPU's options from the title's folder: a file "noxf\" (or WL_XB_NO_XF on the host) keeps GX's
// transform unit out of the vertices, for an A/B on the console; the processor goes on with the next
// frame while GX draws (the caches keep what GX may still read one frame longer), and a file
// "nooverlap" (or WL_XB_NOOVERLAP) makes each frame's end wait for GX to finish it, as before (a
// file "overlap" or WL_XB_OVERLAP is still read, and does nothing). The static geometry's vertex
// arrays are kept for GX to read by index, 1 MB of them in the end of the textures' arena (a10 kept
// at most 843 KB on the Wii); a file "vcache" sets the KB (its text, 1024 when empty), a file
// "novcache" turns them off (WL_XB_VCACHE_KB on the host, 0 for off).
static void gpu_options(const WlXbTitleConfig *cfg) {
    char flag[WL_FS_PATH_MAX];
    bool no_xf, overlap;
    uint32_t vcache_kb = 1024u;
    WlPlatformFile *vf = NULL;

    // earlier behaviours put back, for a bisection on the console (read once, before the guest runs):
    // files "oldapc", "oldyield", "oldvblank" (thread.h's WL_XB_COMPAT_*) and "oldoccl" (occlusion
    // queries counted by their area). The old APC and yield are the default (card run 12: the new two
    // together crash a10's load, a read of guest 0x00007974); files "newapc" and "newyield" take them away.
    {
        static const struct {
            const char *name;
            uint32_t bit;
        } k_compat[] = {{"oldapc", WL_XB_COMPAT_OLDAPC},
                        {"oldyield", WL_XB_COMPAT_OLDYIELD},
                        {"oldvblank", WL_XB_COMPAT_OLDVBLANK},
                        {"oldoccl", 0u}};
        char in_force[64] = "";
        bool old_occl = false;
        g_xb_compat = WL_XB_COMPAT_OLDAPC | WL_XB_COMPAT_OLDYIELD;
        snprintf(flag, sizeof flag, "%s/newapc", cfg->title_dir);
        if (wl_platform_fs_exists(flag)) {
            g_xb_compat &= ~WL_XB_COMPAT_OLDAPC;
        }
        snprintf(flag, sizeof flag, "%s/newyield", cfg->title_dir);
        if (wl_platform_fs_exists(flag)) {
            g_xb_compat &= ~WL_XB_COMPAT_OLDYIELD;
        }
        for (size_t i = 0; i < sizeof k_compat / sizeof k_compat[0]; i++) {
            snprintf(flag, sizeof flag, "%s/%s", cfg->title_dir, k_compat[i].name);
            if (wl_platform_fs_exists(flag)) {
                g_xb_compat |= k_compat[i].bit;
                old_occl = old_occl || k_compat[i].bit == 0u;
            }
            if (k_compat[i].bit != 0u ? (g_xb_compat & k_compat[i].bit) != 0u : old_occl) {
                strncat(in_force, " ", sizeof in_force - strlen(in_force) - 1u);
                strncat(in_force, k_compat[i].name, sizeof in_force - strlen(in_force) - 1u);
            }
        }
        wl_xb_gpu_set_query_by_area(old_occl);
        WL_LOG_I(TAG, "old behaviours in force:%s", in_force[0] != '\0' ? in_force : " none");
    }

    snprintf(flag, sizeof flag, "%s/noxf", cfg->title_dir);
    no_xf = wl_platform_fs_exists(flag);
#if !defined(__wii__)
    no_xf = no_xf || getenv("WL_XB_NO_XF") != NULL;
#endif
    wl_xb_gpu_set_transform_unit(!no_xf);
    snprintf(flag, sizeof flag, "%s/nooverlap", cfg->title_dir);
    overlap = !wl_platform_fs_exists(flag);
#if !defined(__wii__)
    overlap = overlap && getenv("WL_XB_NOOVERLAP") == NULL;
#endif
    wl_xb_gpu_set_frame_overlap(overlap);
    snprintf(flag, sizeof flag, "%s/vcache", cfg->title_dir);
    if (wl_platform_fs_open(flag, WL_FILE_READ, &vf) == WL_OK) {
        char text[16];
        uint32_t got = 0;

        wl_platform_fs_read_some_at(vf, 0, text, sizeof text - 1u, &got);
        wl_platform_fs_close(vf);
        text[got] = '\0';
        vcache_kb = (uint32_t)strtoul(text, NULL, 10);
        vcache_kb = vcache_kb != 0 ? vcache_kb : 1024u;
    }
    snprintf(flag, sizeof flag, "%s/novcache", cfg->title_dir);
    if (wl_platform_fs_exists(flag)) {
        vcache_kb = 0;
    }
#if !defined(__wii__)
    const char *kb = getenv("WL_XB_VCACHE_KB");
    if (kb != NULL && kb[0] != '\0') { // (set and empty: the default)
        vcache_kb = (uint32_t)strtoul(kb, NULL, 10);
    }
#endif
    wl_xb_gpu_set_vertex_cache(NULL, vcache_kb << 10);
    // (a file "noprefetch": no dcbt ahead of the vertex data and push buffer, for an A/B on the console)
    snprintf(flag, sizeof flag, "%s/noprefetch", cfg->title_dir);
    wl_xb_gpu_set_prefetch(!wl_platform_fs_exists(flag));
    gx_bisect_options(cfg);
}

// The capture bench (bench.h) in place of the title: guest memory and the GPU as the title has
// them, then the replays; no image, kernel, devices or service thread.
static int bench_boot(const WlXbTitleConfig *cfg, const char *data_dir, const char *play) {
    void *texture_arena = NULL;
    uint32_t texture_bytes = 0;
    WlXbThread *boot = NULL;
    WL_LOG_I(TAG, "the capture bench: %s", play);
    WlResult r = guest_memory(cfg, data_dir, true, &texture_arena, &texture_bytes, &boot);
    if (r != WL_OK) {
        return r;
    }
    gpu_options(cfg);
    r = wl_xb_gpu_attach_nv2a(texture_arena, texture_bytes);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "the GPU: %s (the bench needs wl/xbox's NV2A)", wl_result_name(r));
        return r;
    }
    g_boot_us = wl_platform_time_us();
    profile_arm(cfg->title_dir);
    int code = wl_xb_bench_run(play, cfg->title_dir);
    WL_LOG_I(TAG, "the capture bench ended with %d", code);
#if defined(__wii__)
    wl_platform_log_flush();
#endif
    return code;
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
        // wl_mem takes only what its heaps need (MEM1's budgets and arenas): the rest stays
        // malloc's, for the guest's frames and the host-side buffers. Its default would claim both
        // regions whole and leave no room for the frame pools. The runtime's only users of wl_mem's
        // heaps are the GX FIFO, three thread stacks (the IO thread's 32 KB, the shutdown watch's
        // 16 KB, the audio thread's 24 KB) and the audio's DMA blocks (five of 4 KB at most): the
        // budgets are those with room to spare (the built-in ones, 4 MB of MEM1 and 4 MB of MEM2,
        // held 6.8 MB that nothing used, out of malloc's reach; the census logs each heap's peak:
        // over ~800 reports of Halo's levels at most stack 25 KB, audio 5 KB, hot, frame and scene
        // 0, so their 512 KB of 2026-10 is now 144, and MEM1's rest, the frames', 496 KB more).
        static WlMemTagDef tags[WL_TAG_BUILTIN_COUNT];
        memcpy(tags, wl_mem_builtin_tags(), sizeof tags);
        tags[WL_TAG_STACK].budget = 96u << 10;
        tags[WL_TAG_HOT].budget = 0;
        tags[WL_TAG_AUDIO].budget = 48u << 10;
        tags[WL_TAG_NET].budget = 0;
        tags[WL_TAG_ASSET].budget = 0;
        tags[WL_TAG_AUDIO_BANK].budget = 0;
        tags[WL_TAG_IO].budget = 0;
        WlMemConfig mcfg;
        wl_mem_config_default(&mcfg);
        mcfg.tags = tags;
        mcfg.tag_count = WL_TAG_BUILTIN_COUNT;
        // (the per-frame arenas and the scene stack are a game engine's; the runtime does not use
        // them: none)
        mcfg.frame_arena_size = 0;
        mcfg.scene_stack_size = 0;
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
    // ---- the run's options (runopts.h): the command line and netrun.txt choose the play setting and
    // flag files as if they were in the title folder; net= opens the remote log, which also sends
    // what the run writes in the title folder and its play folder
    WlXbRunOptions run;
    wl_xb_run_options_load(cfg->title_dir, cfg->arg_count, cfg->args, &run);
    if (run.net[0] != '\0') {
        wl_platform_log_init(NULL); // (the platform's sink feeds the stream; the host has not installed it)
        WlResult nr = wl_net_stream_open(run.net, run.name);
        if (nr == WL_OK) {
            (void)wl_net_stream_watch_files(cfg->title_dir, "play");
        } else {
            WL_LOG_E(TAG, "net=%s: %s", run.net, wl_result_name(nr));
        }
    }
    WL_LOG_I(TAG, "title %s, data %s", xbe_path, data_dir);
    if (run.virtual_files > 0u || run.play[0] != '\0') {
        WL_LOG_I(TAG, "run options: %u files, play \"%s\"", (unsigned)run.virtual_files, run.play);
    }
    const char *play = run.play[0] != '\0' ? run.play : play_setting(cfg);
    if (wl_xb_bench_setting(play)) {
        return bench_boot(cfg, data_dir, play);
    }
    {
        char seeds[sizeof data_dir + 24];
        snprintf(seeds, sizeof seeds, "%s/recomp_seeds.json", data_dir);
        wl_xb_set_seed_file(seeds);
        // (the vertex programs drawn with, for the title's build to compile ahead of time)
        snprintf(seeds, sizeof seeds, "%s/vsh_programs.inc", data_dir);
        wl_xb_gpu_set_program_list(seeds);
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
    void *texture_arena = NULL;
    uint32_t texture_bytes = 0;
    WlXbThread *boot = NULL;
    r = guest_memory(cfg, data_dir, !cfg->gpu_stub, &texture_arena, &texture_bytes, &boot);
    if (r != WL_OK) {
        return r;
    }

    // ---- the image, the kernel, the devices -----------------------------------------------------
    // the sections' whole pages stay backed by the file (its x86 code is evicted without a swap
    // write and read from it again), so it stays open for the image's life
    r = wl_xbe_load_backed(xbe, read_file, file, file);
    if (r != WL_OK) {
        wl_platform_fs_close(file);
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
    {
        // a file "noreadahead" in the title's folder (WL_XB_NO_READAHEAD on the host): no host thread
        // reads a map ahead of the title's sequential reads, for an A/B of a level's first load
        char flag[WL_FS_PATH_MAX];
        snprintf(flag, sizeof flag, "%s/noreadahead", cfg->title_dir);
        kc.no_read_ahead = wl_platform_fs_exists(flag);
#if !defined(__wii__)
        kc.no_read_ahead = kc.no_read_ahead || getenv("WL_XB_NO_READAHEAD") != NULL;
#endif
        if (kc.no_read_ahead) {
            WL_LOG_I(TAG, "read-ahead: off");
        }
    }
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
    gpu_options(cfg);
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
    // the play harness (its setting read above)
    {
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
    wl_mem_census_log(TAG, false); // (the heap's peak over the whole run, and who holds it now)
    // the guest stopped before the platform winds down: its other threads (and the service
    // thread's interrupts and vertical blanks) would go on running the title's code while the
    // card is unmounted and the audio and video are stopped. The service thread first (it may be
    // waiting for the lock), then the lock is the boot thread's for good: no guest code runs again.
    wl_xb_service_stop();
    wl_xb_lock(boot);
    wl_xk_files_stop(true); // a guest thread's card transfer outside the guest lock goes no further
    wl_xk_files_flush();    // the files the title left open (its Z: cache slots) whole on the card
#if defined(__wii__)
    wl_platform_log_flush();
#endif
    return code;
}
