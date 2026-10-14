// SPDX-License-Identifier: MIT
// The capture bench (bench.h): wl_nv2a_replay_stream over the runtime's guest memory, through the
// GX backend nv2a_adapter.c made, with that backend wrapped to time its calls.
#include "wl/xboxrt/bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"

#define TAG "xbbench"

bool wl_xb_bench_setting(const char *play) {
    return play != NULL && strncmp(play, "replay:", 7) == 0;
}

#if defined(WL_XBRT_HAVE_NV2A)

#include <time.h>

#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_gx.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/mem.h"

#if defined(__wii__)
#include "wl/platform/timebase.h"
#endif

enum { DEFAULT_RUNS = 5, MAX_RUNS = 1000 };

// the clock: the time base on the Wii (a few cycles a read), nanoseconds elsewhere (C11 has no
// monotonic clock; a run is short)
static uint64_t now_ticks(void) {
#if defined(__wii__)
    return wl_timebase_read();
#else
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

static uint64_t ticks_us(uint64_t ticks) {
#if defined(__wii__)
    return wl_timebase_us(ticks);
#else
    return ticks / 1000u;
#endif
}

// what a run counts (clock ticks; PHASE_LOG: the frames' lines, left out); `frame_*` are the totals
// at the last frame's end
typedef enum { PHASE_DRAW, PHASE_CLEAR, PHASE_PRESENT, PHASE_READ, PHASE_LOG, PHASE_COUNT } Phase;

typedef struct {
    WlNv2aBackend gx;   // the backend the title draws with
    WlPlatformFile *file;
    uint64_t offset;    // the next byte of the file to read
    WlNv2aReplayInfo *info;
    unsigned run;
    uint64_t phase[PHASE_COUNT];
    uint32_t draws, frames;
    uint64_t start, frame_at, frame_phase[PHASE_COUNT], frame_memory;
    uint32_t frame_draws;
} Bench;

static Bench B;

// milliseconds in `ticks`, a share of `per`
static double ms(uint64_t ticks, uint32_t per) {
    return per != 0 ? (double)ticks_us(ticks) / 1000.0 / per : 0.0;
}

// ---- the backend, timed -------------------------------------------------------------------------

static void t_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    (void)user;
    uint64_t t0 = now_ticks();
    B.gx.clear(B.gx.user, k, flags);
    B.phase[PHASE_CLEAR] += now_ticks() - t0;
}

static uint32_t t_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    (void)user;
    uint64_t t0 = now_ticks();
    uint32_t pixels = B.gx.draw(B.gx.user, k, draw);
    B.phase[PHASE_DRAW] += now_ticks() - t0;
    B.draws++;
    return pixels;
}

static void t_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    (void)user;
    uint64_t t0 = now_ticks();
    B.gx.flip(B.gx.user, k, phys);
    B.phase[PHASE_PRESENT] += now_ticks() - t0;
}

// the end of a frame: its line
static void t_frame_done(void *user, const WlNv2aKelvin *k) {
    (void)user;
    uint64_t t0 = now_ticks();
    B.gx.frame_done(B.gx.user, k);
    uint64_t t1 = now_ticks();
    B.phase[PHASE_PRESENT] += t1 - t0;
    // the GPU's part of the frame: its time without reading the capture and putting memory in
    uint64_t wall = t1 - B.frame_at;
    uint64_t aside = (B.phase[PHASE_READ] - B.frame_phase[PHASE_READ]) + (B.info->memory_time - B.frame_memory);
    uint64_t gpu = wall > aside ? wall - aside : 0;
    uint64_t draws = B.phase[PHASE_DRAW] - B.frame_phase[PHASE_DRAW];
    uint64_t present = B.phase[PHASE_PRESENT] - B.frame_phase[PHASE_PRESENT];
    WL_LOG_I(TAG, "run %u frame %u: %.1f ms (draws %.1f, present %.1f; %u draws)", B.run, (unsigned)B.frames,
             ms(gpu, 1), ms(draws, 1), ms(present, 1), (unsigned)(B.draws - B.frame_draws));
    B.frames++;
    B.frame_draws = B.draws;
    memcpy(B.frame_phase, B.phase, sizeof B.phase);
    B.frame_memory = B.info->memory_time;
    B.frame_at = now_ticks();
    B.phase[PHASE_LOG] += B.frame_at - t1;
}

// ---- the capture, the memory, the hooks -----------------------------------------------------------

static WlResult source_read(void *user, void *dst, uint32_t bytes, uint32_t *got) {
    (void)user;
    uint64_t t0 = now_ticks();
    // (whole sectors from the second read on: the file's offset kept on a 512-byte boundary)
    uint32_t n = bytes;
    if (n > 4096u) {
        n -= (uint32_t)((B.offset + n) & 511u);
    }
    WlResult r = wl_platform_fs_read_some_at(B.file, B.offset, dst, n, got);
    if (r == WL_OK) {
        B.offset += *got;
    }
    B.phase[PHASE_READ] += now_ticks() - t0;
    return r;
}

static void *phys_ptr(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    if ((uint64_t)phys + bytes > wl_xb_ram_bytes()) {
        return NULL;
    }
    return wl_xb_phys_ptr(phys);
}

static void on_created(void *user, WlNv2a *dev) {
    wl_nv2a_gx_attach(user, dev);
}

static uint64_t on_clock(void *user) {
    (void)user;
    return now_ticks();
}

// ---- the runs -------------------------------------------------------------------------------------

static void phase_line(char *out, size_t cap, const uint64_t *phase, uint64_t model, uint64_t memory,
                       uint32_t frames) {
    snprintf(out, cap, "pusher %.1f, draws %.1f, clears %.1f, present %.1f; besides: memory %.1f, reading %.1f ms a "
             "frame", ms(model, frames), ms(phase[PHASE_DRAW], frames), ms(phase[PHASE_CLEAR], frames),
             ms(phase[PHASE_PRESENT], frames), ms(memory, frames), ms(phase[PHASE_READ], frames));
}

int wl_xb_bench_run(const char *setting, const char *base_dir) {
    if (!wl_xb_bench_setting(setting)) {
        return WL_ERR_ARG;
    }
    // "replay:<file> [runs]"
    char arg[WL_FS_PATH_MAX], path[WL_FS_PATH_MAX + 64];
    snprintf(arg, sizeof arg, "%s", setting + 7);
    size_t len = strcspn(arg, "\r\n");
    while (len > 0 && (arg[len - 1] == ' ' || arg[len - 1] == '\t')) {
        len--;
    }
    arg[len] = '\0';
    unsigned runs = DEFAULT_RUNS;
    char *space = strrchr(arg, ' ');
    if (space != NULL && space[1] != '\0' && strspn(space + 1, "0123456789") == strlen(space + 1)) {
        runs = (unsigned)strtoul(space + 1, NULL, 10);
        while (space > arg && (space[-1] == ' ' || space[-1] == '\t')) {
            space--;
        }
        *space = '\0';
    }
    runs = runs == 0 ? 1u : runs > MAX_RUNS ? MAX_RUNS : runs;
    if (strchr(arg, ':') != NULL || arg[0] == '/' || base_dir == NULL || base_dir[0] == '\0') {
        snprintf(path, sizeof path, "%s", arg);
    } else {
        snprintf(path, sizeof path, "%s/%s", base_dir, arg);
    }

    struct WlNv2aGx *gx = wl_xb_gpu_nv2a_gx();
    if (gx == NULL) {
        WL_LOG_E(TAG, "no GX backend (GX did not start, or the GPU is the idle stub): nothing to replay through");
        return WL_ERR_STATE;
    }
    memset(&B, 0, sizeof B);
    WlResult r = wl_platform_fs_open(path, WL_FILE_READ, &B.file);
    if (r != WL_OK) {
        WL_LOG_E(TAG, "cannot open the capture %s: %s", path, wl_result_name(r));
        return r;
    }
    WL_LOG_I(TAG, "%s (%u KB), %u runs", path, (unsigned)(wl_platform_fs_size(B.file) >> 10), runs);

    wl_nv2a_gx_backend(gx, &B.gx);
    WlNv2aBackend timed = {NULL, t_clear, t_draw, t_frame_done, t_flip};
    WlNv2aReplaySource source = {NULL, source_read};
    WlNv2aReplayMemory memory = {NULL, phys_ptr, wl_xb_ram_bytes()};
    WlNv2aReplayHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.user = gx;
    hooks.created = on_created;
    hooks.clock = on_clock;

    uint64_t best = 0, total = 0, best_wall = 0, total_wall = 0;
    uint64_t sums[PHASE_COUNT] = {0}, sum_model = 0, sum_memory = 0;
    uint32_t frames = 0, draws = 0;
    unsigned done = 0;
    WlNv2aReplayInfo info;
    for (unsigned run = 1; run <= runs; run++) {
        memset(B.phase, 0, sizeof B.phase);
        memset(B.frame_phase, 0, sizeof B.frame_phase);
        B.offset = 0;
        B.info = &info;
        B.run = run;
        B.draws = B.frames = B.frame_draws = 0;
        B.frame_memory = 0;
        B.start = B.frame_at = now_ticks();
        r = wl_nv2a_replay_stream(&source, &memory, &timed, &hooks, &info);
        uint64_t wall = now_ticks() - B.start;
        if (r != WL_OK) {
            WL_LOG_E(TAG, "run %u: the replay failed: %s (after %u memory records, %u writes)", run, wl_result_name(r),
                     (unsigned)info.blocks, (unsigned)info.writes);
            break;
        }
        frames = info.stats.frames != 0 ? info.stats.frames : 1u;
        draws = info.stats.draws;
        uint64_t backend = B.phase[PHASE_DRAW] + B.phase[PHASE_CLEAR] + B.phase[PHASE_PRESENT];
        uint64_t aside = B.phase[PHASE_READ] + B.phase[PHASE_LOG] + info.memory_time;
        uint64_t gpu = wall > aside ? wall - aside : 0;
        uint64_t model = gpu > backend ? gpu - backend : 0;
        char line[200];
        phase_line(line, sizeof line, B.phase, model, info.memory_time, frames);
        WL_LOG_I(TAG, "run %u: %u frames, %u draws, %.1f ms a frame (%s)", run, (unsigned)frames, (unsigned)draws,
                 ms(gpu, frames), line);
        WL_LOG_I(TAG, "run %u: %u memory records (%u KB, %u KB of it changed guest memory), %u writes, %u vertices",
                 run, (unsigned)info.blocks, (unsigned)(info.block_bytes >> 10), (unsigned)(info.changed_bytes >> 10),
                 (unsigned)info.writes, (unsigned)info.stats.vertices);
        best = done == 0 || gpu < best ? gpu : best;
        best_wall = done == 0 || wall < best_wall ? wall : best_wall;
        total += gpu;
        total_wall += wall;
        for (unsigned p = 0; p < PHASE_COUNT; p++) {
            sums[p] += B.phase[p];
        }
        sum_model += model;
        sum_memory += info.memory_time;
        done++;
    }
    wl_platform_fs_close(B.file);
    B.file = NULL;
    if (done == 0) {
        return r != WL_OK ? r : WL_ERR_FORMAT;
    }
    // the last frame on the screen before the summary
    wl_nv2a_gx_present(gx);
    (void)wl_platform_video_wait_shown();
    {
        char line[200];
        for (unsigned p = 0; p < PHASE_COUNT; p++) {
            sums[p] /= done;
        }
        phase_line(line, sizeof line, sums, sum_model / done, sum_memory / done, frames);
        WL_LOG_I(TAG, "summary: %u runs of %u frames (%u draws): best %.1f ms a frame, mean %.1f ms a frame (the "
                 "whole run: best %.1f, mean %.1f)", done, (unsigned)frames, (unsigned)draws, ms(best, frames),
                 ms(total, frames * done), ms(best_wall, frames), ms(total_wall, frames * done));
        WL_LOG_I(TAG, "summary: mean %s", line);
    }
    {
        WlNv2aGxStats gs;
        WlNv2aTexCacheStats ts;
        WlXbMemStats ms;
        wl_nv2a_gx_stats(gx, &gs);
        wl_nv2a_texcache_stats(wl_nv2a_gx_textures(gx), &ts);
        wl_xb_mem_stats(&ms);
        WL_LOG_I(TAG, "textures: %u converted, %u made again, %u evicted, %u failed (%u for memory), %u kept smaller "
                 "(%u made whole again); %u checks, %u by the write watch, %u hashes (%u KB); %u KB kept",
                 (unsigned)ts.converted, (unsigned)ts.reconverted, (unsigned)ts.evicted, (unsigned)ts.failed,
                 (unsigned)ts.no_memory, (unsigned)ts.reduced, (unsigned)ts.restored, (unsigned)ts.checks,
                 (unsigned)ts.watched, (unsigned)ts.hashed, (unsigned)(ts.hashed_bytes >> 10),
                 (unsigned)(ts.bytes >> 10));
        WL_LOG_I(TAG, "guest memory: %u frames (%u free), %u evictions, %u swap-ins, %u swap-outs, %u ms in the swap "
                 "file", (unsigned)ms.frames_total, (unsigned)ms.frames_free, (unsigned)ms.evictions,
                 (unsigned)ms.swap_ins, (unsigned)ms.swap_outs, (unsigned)(ms.swap_us / 1000u));
    }
    return done == runs ? 0 : (r != WL_OK ? r : WL_ERR_FORMAT);
}

#else

int wl_xb_bench_run(const char *setting, const char *base_dir) {
    (void)setting;
    (void)base_dir;
    WL_LOG_E(TAG, "this runtime was built without wl/xbox's NV2A model: no bench");
    return WL_ERR_UNSUPPORTED;
}

#endif
