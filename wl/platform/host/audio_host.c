// SPDX-License-Identifier: MIT
// Host stub of the audio output; see audio_host.h.
#include "wl/platform/host/audio_host.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/platform.h"

static struct {
    bool started;
    uint32_t rate, frames;
    WlAudioFillFn fill;
    void *user;
    atomic_uint underruns, blocks;
    atomic_bool quit;
    bool thread_up;
    WlPlatformThread thread;
    void *stack;
} g;

bool wl_platform_audio_threaded(void) {
    return g.thread_up;
}

void wl_platform_audio_prefer(WlAudioBackend backend) {
    (void)backend;
}

WlResult wl_platform_audio_start(uint32_t sample_rate, uint32_t block_frames, WlAudioFillFn fill,
                                 void *user) {
    if (g.started) {
        return WL_ERR_STATE;
    }
    if (fill == NULL || block_frames == 0 || sample_rate == 0) {
        return WL_ERR_ARG;
    }
    g.rate = sample_rate;
    g.frames = block_frames;
    g.fill = fill;
    g.user = user;
    atomic_store(&g.underruns, 0);
    atomic_store(&g.blocks, 0);
    g.started = true;
    return WL_OK;
}

void wl_platform_audio_stop(void) {
    wl_host_audio_thread_stop();
    g.started = false;
    g.fill = NULL;
}

uint32_t wl_platform_audio_underruns(void) {
    return atomic_load(&g.underruns);
}

void wl_platform_audio_stats(WlPlatformAudioStats *out) {
    memset(out, 0, sizeof *out);
    out->backend = g.started ? 3u : 0u;
    out->hw_block_frames = g.started ? g.frames : 0u;
    out->blocks_filled = atomic_load(&g.blocks);
    out->blocks_played = out->blocks_filled;
    out->underruns = atomic_load(&g.underruns);
}

uint32_t wl_host_audio_render(int16_t *out, uint32_t frames) {
    if (!g.started || g.thread_up) {
        return 0;
    }
    int16_t *discard = NULL;
    uint32_t done = 0;
    while (done < frames) {
        int16_t *dst = out;
        if (dst == NULL) {
            if (discard == NULL) {
                discard = malloc((size_t)g.frames * 4u);
            }
            dst = discard;
        } else {
            dst = out + (size_t)done * 2u;
        }
        g.fill(g.user, dst, g.frames);
        atomic_fetch_add(&g.blocks, 1u);
        done += g.frames;
    }
    free(discard);
    return done;
}

void wl_host_audio_inject_underruns(uint32_t n) {
    atomic_fetch_add(&g.underruns, n);
}

static void *host_thread(void *arg) {
    (void)arg;
    int16_t *buf = malloc((size_t)g.frames * 4u);
    while (!atomic_load(&g.quit)) {
        g.fill(g.user, buf, g.frames);
        atomic_fetch_add(&g.blocks, 1u);
        wl_platform_thread_sleep_ms(1);
    }
    free(buf);
    return NULL;
}

WlResult wl_host_audio_thread_start(void) {
    if (!g.started || g.thread_up) {
        return WL_ERR_STATE;
    }
    g.stack = aligned_alloc(32, 8192);
    atomic_store(&g.quit, false);
    if (g.stack == NULL ||
        wl_platform_thread_create(&g.thread, host_thread, NULL, g.stack, 8192, WL_THREAD_PRIO_AUDIO) != WL_OK) {
        free(g.stack);
        return WL_ERR_FAILED;
    }
    g.thread_up = true;
    return WL_OK;
}

void wl_host_audio_thread_stop(void) {
    if (!g.thread_up) {
        return;
    }
    atomic_store(&g.quit, true);
    wl_platform_thread_join(g.thread, NULL);
    free(g.stack);
    g.stack = NULL;
    g.thread_up = false;
}
