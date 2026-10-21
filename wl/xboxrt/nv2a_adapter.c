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

#include "wl/platform/gx_target.h"
#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/page_census.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/thread.h"

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_kelvin.h"
#include "wl/xbox/nv2a_gx.h"
#include "wl/xbox/nv2a_regs.h"

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
    wl_xb_census_phys(phys, bytes, WL_XB_CU_GPU);
    return wl_xb_phys_ptr(phys);
}

// the write watch (mem.h): a converted texture is only read again once its pages were stored to,
// so its pages are the first to give their frames back (armed as it is converted)
static uint32_t tex_watch(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    wl_xb_census_phys(phys, bytes, WL_XB_CU_TEX);
    uint32_t stamp = wl_xb_phys_watch(phys, bytes);
    if (stamp != 0) {
        wl_xb_phys_converted(phys, bytes);
    }
    return stamp;
}

// (the same for a vertex array the cache converted)
static uint32_t vtx_watch(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    wl_xb_census_phys(phys, bytes, WL_XB_CU_VTX);
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

// ---- GX's waits outside the guest lock -------------------------------------------------------------
//
// The holder that waits for GX (a frame's end, a token, a copy: gx_wii.c's waits, through
// wl_gx_set_wait_idle) gives the guest lock up once the wait's own work (the put-off textures) is
// done and someone else wants the lock, as the Xbox's other threads run while its GPU draws, and
// takes it back when GX is done (thread.h's wl_xb_gpu_wait_begin/end: at the next holder's next safe
// point). It waits in the middle of the model (a method's flip, a kick's draws), which is not made to
// be entered twice, nor GX to be given another thread's commands in the middle of a frame's end. So
// meanwhile the others' accesses that run the pusher or a flip (a channel's PUT, PGRAPH's FIFO access
// and interrupt acknowledge, PCRTC's START) and the vertical blank (which tries the stopped channels
// again) wait outside the lock for the waiter to leave the model, or, from a thread that cannot give
// the lock up (the service thread, a raised IRQL), queue for it in order: the waiter runs them once it
// is out. The GPU's interrupt is held off as long, so its ISR does not run again and again on an
// acknowledge that queued. Their other accesses (status reads, the interrupt registers' enables, the
// timer) go through: the model is at rest between two of its calls in the waiter.
// Off (a file "holdgxlock", WL_XB_HOLD_GX_LOCK on the host): the lock kept through every wait.

// how long a wait spins with the lock before it gives the lock up (a short sync costs less than two
// hand-offs), and only when another thread or interrupt work wants it (wl_xb_lock_wanted)
#define GX_SPIN_US 100u
#define GX_QUEUE 64u
#define GX_VBLANK 0xFFFFFFFFu // a queued vertical blank's offset

static bool g_hold_gx_lock;
static uint32_t g_gpu_depth;        // the holder's calls into the model under way
static volatile bool g_gx_given_up; // a waiter in the model is off the lock (read by the others)
static struct {
    WlXbThread *self;    // off the lock (NULL: holding it)
    uint64_t t0;         // this wait's first look (0: none yet)
    bool kept;           // this wait keeps the lock
    uint32_t mmio_depth; // mmio.c's, put back (zero meanwhile: the others' faults may give the lock up)
} g_gw;
static struct {
    uint32_t offset, value;
    uint8_t size;
} g_queue[GX_QUEUE];
static uint32_t g_queue_n;
static struct {
    uint32_t given_up, queued, vblanks, waited, overflow, irq_held;
} g_gxl;

void wl_xb_gpu_set_hold_gx_lock(bool on) {
    g_hold_gx_lock = on;
}

static void gx_wait_idle(void *user, bool done) {
    (void)user;
    if (done) {
        if (g_gw.self != NULL) {
            WlXbThread *self = g_gw.self;
            g_gw.self = NULL;
            wl_xb_gpu_wait_end(self);
            g_xb_mmio_depth = g_gw.mmio_depth;
            g_gx_given_up = false;
        }
        g_gw.t0 = 0;
        g_gw.kept = false;
        return;
    }
    if (g_gw.self != NULL) {
        wl_xb_gpu_wait_poll();
        return;
    }
    if (g_gw.kept) {
        return;
    }
    uint64_t now = wl_platform_time_us();
    if (g_gw.t0 == 0) {
        g_gw.t0 = now | 1u;
        return;
    }
    if (now - g_gw.t0 < GX_SPIN_US || !wl_xb_lock_wanted()) {
        return;
    }
    if (g_hold_gx_lock || g_gpu_depth == 0 || g_gx_given_up || !wl_xb_lock_held()) {
        g_gw.kept = true; // (a wait outside the model's calls, or not the holder's: nothing to keep apart)
        return;
    }
    g_gx_given_up = true; // (before the lock goes: the next holder sees it)
    g_gw.mmio_depth = g_xb_mmio_depth;
    g_xb_mmio_depth = 0;
    g_gw.self = wl_xb_gpu_wait_begin();
    if (g_gw.self == NULL) {
        g_gx_given_up = false; // kept (the deterministic mode, a raised IRQL, the service's work)
        g_xb_mmio_depth = g_gw.mmio_depth;
        g_gw.kept = true;
        return;
    }
    g_gxl.given_up++;
}

// Whether an access runs the pusher or a flip (see above).
static bool runs_gx(uint32_t offset) {
    offset &= WL_NV2A_MMIO_SIZE - 1u;
    if (offset >= WL_NV2A_USER) {
        return (offset & 0x1FFCu) == WL_NV2A_USER_DMA_PUT;
    }
    switch (offset & ~3u) {
    case WL_NV2A_PGRAPH_INTR:
    case WL_NV2A_PGRAPH_FIFO:
    case WL_NV2A_PCRTC_START:
        return true;
    default:
        return false;
    }
}

// Another thread's access, while the waiter is off the lock, that must not enter the model now: it
// waits outside the lock until the waiter is out (true: go on), or queues (false: done).
static bool wait_for_waiter(uint32_t offset, uint32_t value, unsigned size) {
    WlXbThread *self = offset != GX_VBLANK ? wl_xb_block_begin() : NULL;
    if (self != NULL) {
        g_gxl.waited++;
        while (g_gx_given_up) {
            wl_xb_gpu_wait_poll();
        }
        wl_xb_block_end(self); // (at the waiter's next safe point: out of the model)
        return true;
    }
    if (g_queue_n == GX_QUEUE) {
        g_gxl.overflow++; // (a few acknowledges and kicks a wait: not seen)
        return true;
    }
    if (offset != GX_VBLANK && g_queue_n != 0 && g_queue[g_queue_n - 1u].offset == offset &&
        (offset & (WL_NV2A_MMIO_SIZE - 1u)) >= WL_NV2A_USER) {
        g_queue[g_queue_n - 1u].value = value; // (a channel's PUT again: the later one)
        return false;
    }
    g_queue[g_queue_n].offset = offset;
    g_queue[g_queue_n].value = value;
    g_queue[g_queue_n].size = (uint8_t)size;
    g_queue_n++;
    if (offset == GX_VBLANK) {
        g_gxl.vblanks++;
    } else {
        g_gxl.queued++;
    }
    return false;
}

static void vblank_now(void *gpu);

// The waiter is out of the model: what the others queued meanwhile, in order (a wait in one of them
// may give the lock up again: what queues then runs in this loop too).
static void run_queued(void *gpu) {
    for (uint32_t i = 0; i < g_queue_n; i++) {
        uint32_t offset = g_queue[i].offset;
        g_gpu_depth++;
        if (offset == GX_VBLANK) {
            vblank_now(gpu);
        } else {
            wl_nv2a_mmio_write(gpu, offset, g_queue[i].value, g_queue[i].size);
        }
        g_gpu_depth--;
    }
    g_queue_n = 0;
}

static uint32_t a_read(void *gpu, uint32_t offset, unsigned size) {
    return wl_nv2a_mmio_read(gpu, offset, size);
}

static void a_write(void *gpu, uint32_t offset, uint32_t value, unsigned size) {
    while (g_gx_given_up && runs_gx(offset)) {
        if (!wait_for_waiter(offset, value, size)) {
            return;
        }
        if (g_queue_n == GX_QUEUE) {
            break; // (the queue full: in at once, as before)
        }
    }
    g_gpu_depth++;
    wl_nv2a_mmio_write(gpu, offset, value, size);
    if (--g_gpu_depth == 0 && g_queue_n != 0) {
        run_queued(gpu);
    }
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
    if (g_gxl.given_up != 0) {
        WL_LOG_I(TAG, "status: GX waits gave the guest lock up %u times; behind them %u accesses and %u vertical "
                 "blanks queued, %u threads waited, %u queue overflows, %u interrupt polls held off",
                 (unsigned)g_gxl.given_up, (unsigned)g_gxl.queued, (unsigned)g_gxl.vblanks, (unsigned)g_gxl.waited,
                 (unsigned)g_gxl.overflow, (unsigned)g_gxl.irq_held);
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
    // (the heaviest frame's textures: what the arena must hold)
    WL_LOG_I(TAG, "status: textures at the peak, KB by format: RGBA8 %u, CMPR %u (%u of it DXT3/5 alpha), C8 %u, "
             "RGB565 %u, RGB5A3 %u, IA8 %u, I8 %u",
             (unsigned)(ts.peak_by_format[WL_GXC_RGBA8] >> 10), (unsigned)(ts.peak_by_format[WL_GXC_CMPR] >> 10),
             (unsigned)(ts.peak_alpha >> 10), (unsigned)(ts.peak_by_format[WL_GXC_C8] >> 10),
             (unsigned)(ts.peak_by_format[WL_GXC_RGB565] >> 10), (unsigned)(ts.peak_by_format[WL_GXC_RGB5A3] >> 10),
             (unsigned)(ts.peak_by_format[WL_GXC_IA8] >> 10), (unsigned)(ts.peak_by_format[WL_GXC_I8] >> 10));
    WL_LOG_I(TAG, "status: textures failed: %u refused again, %u formats GX cannot take, %u unreadable, %u bad; %u "
             "stood in, %u draws hidden; not looked up: %u unread, %u disabled, %u unsampled; %u mip-map shares (%u given back)",
             (unsigned)ts.refused_again, (unsigned)ts.unknown, (unsigned)ts.unreadable, (unsigned)ts.bad_texels,
             (unsigned)gs.textures_stood_in, (unsigned)gs.standin_hidden, (unsigned)gs.textures_unread,
             (unsigned)gs.textures_disabled,
             (unsigned)gs.textures_unsampled, (unsigned)ts.mip_shared, (unsigned)ts.mip_replaced);
    // (the frame budget: textures put off, and the frames' time; spikes are what it is for)
    WL_LOG_I(TAG, "status: textures put off %u times in %u frames (%u in a row at most; %u made while GX drew); a frame's texture work at most %u ms "
             "(%u KB made), %u frames over 20 ms, %u over 50 ms; frames ms <34:%u <67:%u <100:%u <150:%u <250:%u "
             "more:%u max %u", (unsigned)ts.deferred, (unsigned)ts.frames_deferred,
             (unsigned)ts.deferred_run, (unsigned)ts.made_early, (unsigned)(ts.worst_frame_us / 1000u),
             (unsigned)(ts.worst_frame_made >> 10), (unsigned)ts.frames_over_20ms, (unsigned)ts.frames_over_50ms,
             (unsigned)gs.frames_by_time[0], (unsigned)gs.frames_by_time[1], (unsigned)gs.frames_by_time[2],
             (unsigned)gs.frames_by_time[3], (unsigned)gs.frames_by_time[4], (unsigned)gs.frames_by_time[5],
             (unsigned)(gs.frame_us_worst / 1000u));
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
    if (gs.made_draws != 0) {
        // (nv2a_gx.c's made vertices read by index: how often the ring went round, and waited for GX)
        WL_LOG_I(TAG, "status: made vertices read by index: %u draws, %u vertices sent, %u made; %u fences, %u waits "
                 "for GX, %u laps; %u draws (%u vertices) of five words or fewer", (unsigned)gs.made_draws,
                 (unsigned)gs.made_sent, (unsigned)gs.made_vertices, (unsigned)gs.made_fences, (unsigned)gs.made_waits,
                 (unsigned)gs.made_laps, (unsigned)gs.made_small_draws, (unsigned)gs.made_small_vertices);
    }
}

static void a_vblank(void *gpu) {
    if (g_gx_given_up) {
        // (the service's or a holder's interrupt work, which keeps the lock: owed to the waiter)
        (void)wait_for_waiter(GX_VBLANK, 0, 0);
        return;
    }
    g_gpu_depth++;
    vblank_now(gpu);
    if (--g_gpu_depth == 0 && g_queue_n != 0) {
        run_queued(gpu);
    }
}

static void vblank_now(void *gpu) {
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
    if (g_gx_given_up) {
        g_gxl.irq_held++;
        return false; // (held off until the waiter is out: an acknowledge would only queue)
    }
    return wl_nv2a_irq_pending(gpu);
}

static const WlXbGpuOps k_ops = {a_read, a_write, a_vblank, a_irq};

WlNv2aGx *wl_xb_gpu_nv2a_gx(void) {
    return g_gx;
}

static bool g_no_xf, g_no_xf_light, g_no_xf_skin, g_no_fused_xf, g_no_overlap;

void wl_xb_gpu_set_transform_unit(bool on) {
    g_no_xf = !on;
}

void wl_xb_gpu_set_transform_unit_light(bool on) {
    g_no_xf_light = !on;
}

void wl_xb_gpu_set_transform_unit_skin(bool on) {
    g_no_xf_skin = !on;
}

void wl_xb_gpu_set_fused_xf(bool on) {
    g_no_fused_xf = !on;
}

void wl_xb_gpu_set_frame_overlap(bool on) {
    g_no_overlap = !on;
}

static bool g_query_by_area, g_targets_on_heap;
static uint32_t g_texture_budget = WL_XB_TEXTURE_FRAME_BUDGET;

void wl_xb_gpu_set_targets_on_heap(bool on) {
    g_targets_on_heap = on;
}

void wl_xb_gpu_set_texture_budget(uint32_t bytes) {
    g_texture_budget = bytes;
}

void wl_xb_gpu_set_query_by_area(bool on) {
    g_query_by_area = on;
}

static void *g_vcache_arena;
static uint32_t g_vcache_bytes;
static bool g_vcache_carved; // the vertex arrays' arena is the end of the boot's textures' arena (of the heap)

void wl_xb_gpu_set_vertex_cache(void *arena, uint32_t bytes) {
    g_vcache_arena = arena;
    g_vcache_bytes = bytes;
}

void wl_xb_gpu_set_prefetch(bool on) {
    wl_nv2a_set_prefetch(on);
}

void wl_xb_gpu_set_old_push(bool on) {
#if !defined(__wii__)
    on = on || getenv("WL_XB_OLDPUSH") != NULL;
#endif
    if (on) {
        WL_LOG_I(TAG, "push buffer: the header loop as before the method table's run kinds (oldpush)");
    }
    wl_nv2a_set_old_push(on);
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

// The back buffer's park (gx_target.c: its colour, as large as the picture, and the depth once a
// target has written over it) in a free piece of the textures' arena when it has one, as the render
// targets (nv2a_gx.h targets_on_heap: "heaptargets" in the title's folder, both of the heap). Its
// pieces in the arena, for the census (the heap's figures leave them out).
#define PARK_PIECES 4
static void *g_park_piece[PARK_PIECES];
static size_t g_park_piece_bytes[PARK_PIECES];

static void *park_alloc(size_t bytes) {
    void *p = g_gx != NULL ? wl_nv2a_gx_spare_alloc(g_gx, bytes) : NULL;
    if (p != NULL) {
        for (unsigned i = 0; i < PARK_PIECES; i++) {
            if (g_park_piece[i] == NULL) {
                g_park_piece[i] = p;
                g_park_piece_bytes[i] = bytes;
                return p;
            }
        }
        (void)wl_nv2a_gx_spare_free(g_gx, p); // (no room to note it: the heap's)
    }
    return aligned_alloc(32, (bytes + 31u) & ~(size_t)31u);
}

static void park_free(void *memory) {
    for (unsigned i = 0; i < PARK_PIECES; i++) {
        if (memory != NULL && g_park_piece[i] == memory) {
            g_park_piece[i] = NULL;
            g_park_piece_bytes[i] = 0;
            (void)wl_nv2a_gx_spare_free(g_gx, memory);
            return;
        }
    }
    free(memory);
}

static void census_targets(void *user, WlMemCensusReport *out) {
    (void)user;
    size_t record, targets, park_arena = 0;
    unsigned count;
    wl_nv2a_gx_host_bytes(g_gx, &record, &targets, &count);
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    for (unsigned i = 0; i < PARK_PIECES; i++) {
        park_arena += g_park_piece_bytes[i];
    }
    park_arena = park_arena < st.held ? park_arena : st.held;
    out->bytes = targets + st.held - park_arena;
    snprintf(out->note, sizeof out->note, "%u, park %u", count, (unsigned)((st.held + 512u) >> 10));
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

// the vertex arrays' arena when it is the end of the textures' (the boot's heap block)
static void census_vertices(void *user, WlMemCensusReport *out) {
    (void)user;
    out->bytes = g_vcache_carved ? g_vcache_bytes : 0u;
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
            g_vcache_carved = true;
        }
        if (texture_arena != NULL) {
            gcfg.textures.arena = texture_arena;
            gcfg.textures.arena_bytes = bytes;
        } else {
            gcfg.textures.budget = bytes;
        }
        gcfg.textures.watch = tex_watch;
        gcfg.textures.frame_budget = g_texture_budget;
        gcfg.query_by_area = g_query_by_area;
        gcfg.targets_on_heap = g_targets_on_heap;
        gcfg.textures.written = tex_written;
        if (g_vcache_bytes != 0) {
            if (g_vcache_arena != NULL) {
                gcfg.vertices.arena = g_vcache_arena;
                gcfg.vertices.arena_bytes = g_vcache_bytes;
            } else {
                gcfg.vertices.budget = g_vcache_bytes;
            }
            gcfg.vertices.watch = vtx_watch;
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
        gcfg.no_xf_light = g_no_xf_light;
        gcfg.no_xf_skin = g_no_xf_skin;
        gcfg.no_fused_xf = g_no_fused_xf;
        gcfg.overlap = !g_no_overlap;
        if (g_no_overlap) {
            WL_LOG_I(TAG, "frames not overlapped: each frame's end waits for GX to finish it");
        }
        if (g_no_xf) {
            WL_LOG_I(TAG, "GX's transform unit not used: every vertex through the program on the processor");
        } else if (g_no_xf_light) {
            WL_LOG_I(TAG, "GX's transform unit lights no colour: a light's dot product made by the processor");
        }
        if (!g_no_xf && g_no_xf_skin) {
            WL_LOG_I(TAG, "GX's transform unit lights no skinned model: d0 made by the processor");
        }
        g_gx = wl_nv2a_gx_create(&gcfg);
        WL_LOG_I(TAG, "textures: %u KB %s; checked through the write watch where the guest is paged",
                 (unsigned)(bytes >> 10), texture_arena != NULL ? "in an arena of their own" : "of the heap");
        if (g_texture_budget != 0) {
            WL_LOG_I(TAG, "textures: %u KB of new ones made a frame at its draws, the rest while GX draws or "
                     "in a later frame", (unsigned)(g_texture_budget >> 10));
        } else {
            WL_LOG_I(TAG, "textures: every one made at its first draw (lazytex)");
        }
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
        if (!g_targets_on_heap) {
            wl_gx_target_set_allocator(park_alloc, park_free);
        }
        (void)wl_mem_census_add("targets", census_targets, NULL);
        (void)wl_mem_census_add("textures", census_textures, NULL);
        (void)wl_mem_census_add("tex scratch", census_scratch, NULL);
        (void)wl_mem_census_add("vertex arena", census_vertices, NULL);
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
    if (!g_hold_gx_lock) {
        wl_gx_set_wait_idle(gx_wait_idle, NULL);
    } else {
        WL_LOG_I(TAG, "holdgxlock: the guest lock kept through GX's waits");
    }
    WlResult r = wl_xb_gpu_attach(&k_ops, g_gpu);
    if (r == WL_OK) {
        // PFB's first page read as memory (mmio.h's mirrors): D3D's kick reads the write-combining
        // buffer's register twice around its flush; the flush itself answered at once
        static WlXbMmioFastStore wbc;
        wbc.offset = WL_NV2A_PFB_WBC & (WL_XB_PAGE - 1u);
        wbc.keep = ~(uint32_t)WL_NV2A_PFB_WBC_FLUSH;
        wbc.model = wl_nv2a_pfb_wbc_word(g_gpu);
        WlResult m = wl_xb_mmio_mirror(WL_XB_NV2A_BASE + WL_NV2A_PFB_PAGE, &wbc, 1u);
        if (m != WL_OK) {
            WL_LOG_W(TAG, "PFB's page not mirrored: %s", wl_result_name(m));
        }
    }
    return r;
}
