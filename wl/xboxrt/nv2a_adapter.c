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

#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/thread.h"

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_kelvin.h"
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
    // (the capture's blocks of guest memory come as host pointers into it, which may be paged out)
    return g_cap.file != NULL ? wl_xb_file_write(g_cap.file, data, bytes) : WL_ERR_STATE;
}

void wl_xb_gpu_capture(const char *path, uint32_t frames, uint32_t after_vblanks) {
    snprintf(g_cap.path, sizeof g_cap.path, "%s", path != NULL ? path : "");
    g_cap.frames = frames != 0 ? frames : 1u;
    g_cap.after = after_vblanks;
    g_cap.armed = g_cap.path[0] != '\0';
}

uint32_t wl_xb_gpu_flips(void) {
    if (g_gpu == NULL) {
        return 0;
    }
    WlNv2aStats st;
    wl_nv2a_stats(g_gpu, &st);
    return st.flips;
}

bool wl_xb_gpu_capture_busy(void) {
    return g_cap.armed || g_cap.file != NULL;
}

// The vertex programs the title draws with, listed for compiling ahead of time
// (wl_xb_gpu_set_program_list): those the file holds, by their hash, and the file
static struct {
    char path[WL_FS_PATH_MAX];
    unsigned long hashes[512];
    unsigned count;
} g_programs;

// the title's vertex programs compiled ahead of time (wl_add_xbox_title's vertex_programs: the
// generated table's accessor; weak, so a title without them links)
extern const WlXboxVshNative *wl_xb_title_vsh_natives(unsigned long *count) __attribute__((weak));

static bool program_listed(unsigned long hash) {
    for (unsigned i = 0; i < g_programs.count; i++) {
        if (g_programs.hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

static void program_note(unsigned long hash) {
    if (g_programs.count < sizeof g_programs.hashes / sizeof g_programs.hashes[0]) {
        g_programs.hashes[g_programs.count++] = hash;
    }
}

// the programs of the list's text (tools/xbox_vsh_codegen.c's input: hexadecimal words outside
// comments, each program a header word with its instruction count in the high half, then four
// words an instruction), noted by their hash
static void programs_parse(const char *text) {
    uint32_t words[1 + WL_XBOX_VSH_MAX_INSTRUCTIONS * 4];
    unsigned have = 0, want = 0;
    bool comment = false;

    for (const char *c = text; *c != '\0';) {
        if (comment) {
            const char *end = strstr(c, "*/");
            if (end == NULL) {
                break;
            }
            comment = false;
            c = end + 2;
        } else if (c[0] == '/' && c[1] == '*') {
            comment = true;
            c += 2;
        } else if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) {
            char *end;
            uint32_t word = (uint32_t)strtoul(c, &end, 16);
            c = end;
            if (have == 0) {
                want = 1u + (word >> 16) * 4u;
                if (want == 1u || want > sizeof words / sizeof words[0]) {
                    want = 0;
                    continue;
                }
            }
            words[have++] = word;
            if (have == want) {
                program_note(wl_xbox_vsh_hash(words + 1, (word = words[0] >> 16)));
                have = 0;
            }
        } else {
            c++;
        }
    }
}

void wl_xb_gpu_set_program_list(const char *path) {
    snprintf(g_programs.path, sizeof g_programs.path, "%s", path != NULL ? path : "");
    g_programs.count = 0;
    WlPlatformFile *f = NULL;
    if (g_programs.path[0] == '\0' || wl_platform_fs_open(g_programs.path, WL_FILE_READ, &f) != WL_OK) {
        return;
    }
    uint64_t size = wl_platform_fs_size(f);
    char *text = size < (16u << 20) ? malloc((size_t)size + 1u) : NULL;
    if (text != NULL && wl_platform_fs_read_at(f, 0, text, (uint32_t)size) == WL_OK) {
        text[size] = '\0';
        programs_parse(text);
    }
    free(text);
    wl_platform_fs_close(f);
}

// a program the pipeline decoded: one neither compiled into the title nor listed is appended
static void program_decoded(void *user, const uint32_t *words, unsigned long count, bool native) {
    (void)user;
    unsigned long hash = wl_xbox_vsh_hash(words, count);
    if (native || g_programs.path[0] == '\0' || program_listed(hash)) {
        return;
    }
    program_note(hash);
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(g_programs.path, WL_FILE_READ_WRITE, &f) != WL_OK) {
        WL_LOG_E(TAG, "cannot write the vertex program list %s", g_programs.path);
        return;
    }
    static const char heading[] = "/* NV2A vertex programs the title drew with (wl/xboxrt), for "
                                  "tools/xbox_vsh_codegen.c */\n";
    uint64_t at = wl_platform_fs_size(f);
    char line[96];
    WlResult r = at == 0 ? wl_platform_fs_write_at(f, 0, heading, (uint32_t)(sizeof heading - 1u)) : WL_OK;
    int n = snprintf(line, sizeof line, "/* %lu instructions, hash %08lx */\n\t0x%08lX,\n", count, hash,
                     (count << 16) | 0x2078ul);
    if (r == WL_OK) {
        r = wl_platform_fs_write_at(f, at == 0 ? sizeof heading - 1u : at, line, (uint32_t)n);
    }
    for (unsigned long i = 0; i < count && r == WL_OK; i++) {
        n = snprintf(line, sizeof line, "\t0x%08X, 0x%08X, 0x%08X, 0x%08X,\n", (unsigned)words[4 * i],
                     (unsigned)words[4 * i + 1], (unsigned)words[4 * i + 2], (unsigned)words[4 * i + 3]);
        r = wl_platform_fs_write(f, line, (uint32_t)n);
    }
    wl_platform_fs_close(f);
    WL_LOG_I(TAG, "vertex program %08lx (%lu instructions) added to %s: rebuild the title to compile it",
             hash, count, g_programs.path);
}

static void *phys_ptr(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    if ((uint64_t)phys + bytes > wl_xb_ram_bytes()) {
        return NULL;
    }
    return wl_xb_phys_ptr(phys);
}

// the write watch (mem.h): a converted texture is only read again once its pages were stored to
static uint32_t tex_watch(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    return wl_xb_phys_watch(phys, bytes);
}

static bool tex_written(void *user, uint32_t phys, uint32_t bytes, uint32_t stamp) {
    (void)user;
    return wl_xb_phys_written(phys, bytes, stamp);
}

// PTIMER's time: the guest's clock (the virtual one in the deterministic mode)
static uint64_t gpu_time_ns(void *user) {
    (void)user;
    return wl_xb_time_ns();
}

static uint32_t a_read(void *gpu, uint32_t offset, unsigned size) {
    return wl_nv2a_mmio_read(gpu, offset, size);
}

static void a_write(void *gpu, uint32_t offset, uint32_t value, unsigned size) {
    wl_nv2a_mmio_write(gpu, offset, value, size);
}

// every 10 s (600 vertical blanks): what the GPU did, for the log
static void gpu_status(void) {
    if (g_gx == NULL) {
        return;
    }
    if (g_gpu != NULL) {
        WlNv2aStats ns;
        wl_nv2a_stats(g_gpu, &ns);
        // the pusher and the display: whether the title's push buffers still come in and flip (an
        // unmet acquire: the channel waited on a semaphore)
        WL_LOG_I(TAG, "status: pusher %u kicks, %u words, %u methods, %u errors, %u unmet acquires; %u flips, %u "
                 "frames, %u vblanks, %u notifies, %u semaphores", (unsigned)ns.kicks, (unsigned)ns.words,
                 (unsigned)ns.methods, (unsigned)ns.pusher_errors, (unsigned)ns.acquires_unmet, (unsigned)ns.flips,
                 (unsigned)ns.frames, (unsigned)ns.vblanks, (unsigned)ns.notifies, (unsigned)ns.semaphores);
    }
    WlNv2aGxStats gs;
    wl_nv2a_gx_stats(g_gx, &gs);
    WlNv2aTexCacheStats ts;
    wl_nv2a_texcache_stats(wl_nv2a_gx_textures(g_gx), &ts);
    WL_LOG_I(TAG, "status: %u frames, %u draws (%u perspective, %u into targets, %u target-less, %u stencil "
             "only (skipped), %u stencil ignored, %u programs beyond GX, %u approximated); %u frames kept as a texture (%u "
             "reads, %u of an older one)", (unsigned)gs.frames, (unsigned)gs.draws,
             (unsigned)gs.perspective_draws, (unsigned)gs.target_draws, (unsigned)gs.target_skipped,
             (unsigned)gs.stencil_skipped, (unsigned)gs.stencil_ignored, (unsigned)gs.programs_unusable,
             (unsigned)gs.approximations, (unsigned)gs.frames_kept, (unsigned)gs.kept_frame_reads,
             (unsigned)gs.kept_frame_misses);
    WlXboxPshCacheCounts pc;
    wl_nv2a_gx_psh_counts(g_gx, &pc);
    // (the pixel programs' cache: a translation is long, and a program should be translated about once)
    WL_LOG_I(TAG, "status: pixel programs %lu translated, %lu found, %lu patched; %lu trials (%lu differed, %lu "
             "generic, %lu folded), %lu audits (%lu differed), %lu evicted, %lu translated again after eviction; "
             "%lu for constants of structures not generic (%lu of them evicted)",
             pc.translated, pc.found, pc.patched, pc.tests, pc.tests_differ, pc.generic, pc.folded, pc.audited,
             pc.audits_differ, pc.evicted, pc.again, pc.constant_sets, pc.constant_sets_evicted);
    WL_LOG_I(TAG, "status: depth fits %u made, %u shared; textures %u KB kept (%u KB drawn in the last frame, "
             "%u KB at most), %u converted, %u made again, %u failed (%u for memory), %u kept smaller (%u made whole "
             "again), %u evicted; %u checks, %u by the write watch, %u hashes (%u KB); %u drawn smaller in the last frame",
             (unsigned)gs.fits_made, (unsigned)gs.fits_shared, (unsigned)(ts.bytes / 1024u),
             (unsigned)(ts.frame_bytes / 1024u), (unsigned)(ts.peak_frame_bytes / 1024u), (unsigned)ts.converted,
             (unsigned)ts.reconverted, (unsigned)ts.failed, (unsigned)ts.no_memory, (unsigned)ts.reduced,
             (unsigned)ts.restored, (unsigned)ts.evicted,
             (unsigned)ts.checks, (unsigned)ts.watched, (unsigned)ts.hashed, (unsigned)(ts.hashed_bytes >> 10),
             (unsigned)ts.frame_reduced);
    WlNv2aTexCacheKinds tk;
    wl_nv2a_texcache_kinds(wl_nv2a_gx_textures(g_gx), &tk);
    // (what fills the arena: KB by GX format, count in brackets)
    WL_LOG_I(TAG, "status: textures KB by format: RGBA8 %u (%u), CMPR %u (%u), C8 %u (%u), RGB565 %u (%u), "
             "RGB5A3 %u (%u), IA8 %u (%u), I8 %u (%u); %u linear; made from %u KB of the guest's; largest %u",
             (unsigned)(tk.by_format[WL_GXC_RGBA8] >> 10), (unsigned)tk.count_by_format[WL_GXC_RGBA8],
             (unsigned)(tk.by_format[WL_GXC_CMPR] >> 10), (unsigned)tk.count_by_format[WL_GXC_CMPR],
             (unsigned)(tk.by_format[WL_GXC_C8] >> 10), (unsigned)tk.count_by_format[WL_GXC_C8],
             (unsigned)(tk.by_format[WL_GXC_RGB565] >> 10), (unsigned)tk.count_by_format[WL_GXC_RGB565],
             (unsigned)(tk.by_format[WL_GXC_RGB5A3] >> 10), (unsigned)tk.count_by_format[WL_GXC_RGB5A3],
             (unsigned)(tk.by_format[WL_GXC_IA8] >> 10), (unsigned)tk.count_by_format[WL_GXC_IA8],
             (unsigned)(tk.by_format[WL_GXC_I8] >> 10), (unsigned)tk.count_by_format[WL_GXC_I8],
             (unsigned)(tk.linear >> 10), (unsigned)(tk.source >> 10),
             (unsigned)(tk.largest >> 10));
    if (wl_nv2a_vcache_on(wl_nv2a_gx_vertex_cache(g_gx))) {
        WlNv2aVCacheStats vs;
        wl_nv2a_vcache_stats(wl_nv2a_gx_vertex_cache(g_gx), &vs);
        WL_LOG_I(TAG, "status: %u draws (%u vertices) sent as indices; vertex arrays %u KB kept (%u KB drawn in the "
                 "last frame, %u KB at most), %u converted, %u made again, %u refused, %u failed, %u evicted; %u "
                 "checks, %u by the write watch",
                 (unsigned)gs.indexed_draws, (unsigned)gs.indexed_vertices, (unsigned)(vs.bytes / 1024u),
                 (unsigned)(vs.frame_bytes / 1024u), (unsigned)(vs.peak_frame_bytes / 1024u), (unsigned)vs.converted,
                 (unsigned)vs.reconverted, (unsigned)vs.refused, (unsigned)vs.failed, (unsigned)vs.evicted,
                 (unsigned)vs.checks, (unsigned)vs.watched);
    }
}

static void a_vblank(void *gpu) {
    wl_nv2a_vblank(gpu);
    static uint32_t vblanks;
    if (++vblanks % 600u == 0) {
        gpu_status();
    }
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

WlNv2aGx *wl_xb_gpu_nv2a_gx(void) {
    return g_gx;
}

static bool g_no_xf, g_no_overlap = true;

void wl_xb_gpu_set_transform_unit(bool on) {
    g_no_xf = !on;
}

void wl_xb_gpu_set_frame_overlap(bool on) {
    g_no_overlap = !on;
}

static void *g_vcache_arena;
static uint32_t g_vcache_bytes;

void wl_xb_gpu_set_vertex_cache(void *arena, uint32_t bytes) {
    g_vcache_arena = arena;
    g_vcache_bytes = bytes;
}

void wl_xb_gpu_set_prefetch(bool on) {
    wl_nv2a_set_prefetch(on);
}

// What the GPU holds of the host's heap (mem_census.h): the device and the backend's records, the
// render targets' images, the texture decoders' scratch, and the textures' arena (the boot's, of the
// heap too) or, without one, the images kept on the heap.
static void census_gpu(void *user, WlMemCensusReport *out) {
    (void)user;
    size_t record = 0, targets = 0;
    unsigned count = 0;
    if (g_gx != NULL) {
        wl_nv2a_gx_host_bytes(g_gx, &record, &targets, &count);
    }
    out->bytes = record + (g_gpu != NULL ? wl_nv2a_host_bytes(g_gpu) : 0u);
}

static void census_targets(void *user, WlMemCensusReport *out) {
    (void)user;
    size_t record, targets;
    unsigned count;
    wl_nv2a_gx_host_bytes(g_gx, &record, &targets, &count);
    out->bytes = targets;
    snprintf(out->note, sizeof out->note, "%u", count);
}

static void census_textures(void *user, WlMemCensusReport *out) {
    (void)user;
    WlNv2aTexCacheMemory m;
    wl_nv2a_texcache_memory(wl_nv2a_gx_textures(g_gx), &m);
    if (m.arena != 0) {
        out->bytes = m.arena;
        snprintf(out->note, sizeof out->note, "arena: %u used, largest free %u", (unsigned)(m.arena_used >> 10),
                 (unsigned)(m.arena_largest >> 10));
    } else {
        out->bytes = m.images;
        snprintf(out->note, sizeof out->note, "of the heap");
    }
}

static void census_scratch(void *user, WlMemCensusReport *out) {
    (void)user;
    WlNv2aTexCacheMemory m;
    wl_nv2a_texcache_memory(wl_nv2a_gx_textures(g_gx), &m);
    out->bytes = m.scratch;
}

WlResult wl_xb_gpu_attach_nv2a(void *texture_arena, uint32_t texture_bytes) {
    // the draws go to GX (over libogc on the Wii, the host's recorder elsewhere)
    if (!wl_gx_ready() && wl_gx_init() != WL_OK) {
        WL_LOG_E(TAG, "GX did not start: the title's frames are not drawn");
    }
    WlNv2aBackend backend;
    memset(&backend, 0, sizeof backend);
    if (wl_gx_ready()) {
        WlNv2aGxConfig gcfg;
        memset(&gcfg, 0, sizeof gcfg);
        // the textures in an arena of their own (the boot's): a texture makes room by evicting
        // others, and the host's other memory is not fragmented by them
        size_t bytes = texture_bytes != 0 ? texture_bytes : 8u << 20;
        // (the vertex arrays without an arena of their own: the end of the textures' when it
        // leaves them 8 MB, so the heap, short during a level, is not asked)
        uint32_t vbytes = (g_vcache_bytes + 31u) & ~31u;
        if (vbytes != 0 && g_vcache_arena == NULL && texture_arena != NULL && bytes >= vbytes + (8u << 20)) {
            bytes -= vbytes;
            g_vcache_arena = (uint8_t *)texture_arena + bytes;
            g_vcache_bytes = vbytes;
        }
        if (texture_arena != NULL) {
            gcfg.textures.arena = texture_arena;
            gcfg.textures.arena_bytes = bytes;
        } else {
            gcfg.textures.budget = bytes;
        }
        gcfg.textures.watch = tex_watch;
        gcfg.textures.written = tex_written;
        if (g_vcache_bytes != 0) {
            if (g_vcache_arena != NULL) {
                gcfg.vertices.arena = g_vcache_arena;
                gcfg.vertices.arena_bytes = g_vcache_bytes;
            } else {
                gcfg.vertices.budget = g_vcache_bytes;
            }
            gcfg.vertices.watch = tex_watch;
            gcfg.vertices.written = tex_written;
            WL_LOG_I(TAG, "vertex arrays: %u KB %s for GX to read by index", (unsigned)(g_vcache_bytes >> 10),
                     g_vcache_arena != NULL ? "in an arena of their own" : "of the heap");
        }
        if (wl_xb_title_vsh_natives != NULL) {
            gcfg.natives = wl_xb_title_vsh_natives(&gcfg.native_count);
            WL_LOG_I(TAG, "%lu vertex programs compiled ahead of time", gcfg.native_count);
        }
        gcfg.program_decoded = program_decoded;
        gcfg.no_xf = g_no_xf;
        gcfg.overlap = !g_no_overlap;
        if (g_no_overlap) {
            WL_LOG_I(TAG, "frames not overlapped: each frame's end waits for GX to finish it");
        }
        if (g_no_xf) {
            WL_LOG_I(TAG, "GX's transform unit not used: every vertex through the program on the processor");
        }
        g_gx = wl_nv2a_gx_create(&gcfg);
        WL_LOG_I(TAG, "textures: %u KB %s; checked through the write watch where the guest is paged",
                 (unsigned)(bytes >> 10), texture_arena != NULL ? "in an arena of their own" : "of the heap");
        if (g_gx != NULL) {
            wl_nv2a_gx_backend(g_gx, &backend);
        }
    }
    WlNv2aConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.phys_ptr = phys_ptr;
    cfg.user = NULL;
    cfg.time_ns = gpu_time_ns;
    cfg.backend = g_gx != NULL ? &backend : NULL;
    g_gpu = wl_nv2a_create(&cfg);
    if (g_gpu == NULL) {
        return WL_ERR_NOMEM;
    }
    if (g_gx != NULL) {
        wl_nv2a_gx_attach(g_gx, g_gpu);
        (void)wl_mem_census_add("targets", census_targets, NULL);
        (void)wl_mem_census_add("textures", census_textures, NULL);
        (void)wl_mem_census_add("tex scratch", census_scratch, NULL);
    }
    (void)wl_mem_census_add("gpu", census_gpu, NULL);
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
