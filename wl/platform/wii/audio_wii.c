// SPDX-License-Identifier: MIT
// Wii audio output: AESND (libogc's audio engine) with ASND as the
// fallback, fed by one stereo PCM16 voice at 32 kHz that the DSP resamples to 48 kHz. Only this
// file touches aesndlib/asndlib.
//
// Design: a ring of N blocks (3 for AESND, 4 for ASND). The audio thread (WL_THREAD_PRIO_AUDIO)
// sleeps on a semaphore, and whenever a block is FREE it calls the mixer's fill function into it
// and marks it READY. The voice callback, which libogc runs in the DSP/DMA interrupt, only moves
// pointers: it frees the block that just finished, hands the next READY block to the voice (or a
// silent block, counting an underrun) and posts the semaphore. No mixing, no locks and no
// allocation happen in interrupt context.
//
// AESND runs a stream voice in 2 ms steps of the DSP mixer; it calls the voice callback with
// VOICE_STATE_STREAM when the current buffer has been copied out entirely (read from
// libaesnd's disassembly: the callback fires from the DSP request handler, once the playback
// position reaches the end of the buffer, and a buffer set inside the callback is used in the same
// step, so a prepared block gives a gapless handover; the voice must be "looped", or the engine
// stops it at the end of the first buffer). It copies the buffer with the CPU in
// 1152-byte pieces, so a block that is playing must not be rewritten: a block is FREE only after
// the callback that replaced it.
//
// Block size. The copy step is 1152 bytes (2 ms at the engine's maximum rate of 144 kHz); a buffer
// whose length is not a multiple of it is followed by silence up to the next multiple, which at a
// 1024-byte (256-frame) block is 128 zero bytes every block: a gap of 32 frames every 8 ms that
// also slows the stream by 11 % (measured in Dolphin: 2236 callbacks in 20 s instead of 2500; the
// Dolphin audio dump showed the tone cut). So the hardware block is 288 stereo frames = 1152
// bytes (9 ms), and the audio thread builds it from fill calls of at most `block_frames`
// (two of 144 for the engine's 256).
//
// ASND (the fallback) calls its voice callback only once per 1024 output samples (21.3 ms,
// measured: 288-frame buffers were consumed 2.4 at a time and the output stuttered at 43 % of the
// rate), and takes one queued buffer per call, so its hardware block is 1024 frames (32 ms; four
// fill calls of 256) and its ring has four blocks.
#include <aesndlib.h>
#include <asndlib.h>
#include <stdatomic.h>

#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#define MAX_SLOTS 4u
#define AESND_BLOCK_FRAMES 288u  // 1152 bytes of stereo PCM16: see "Block size" above
#define ASND_BLOCK_FRAMES 1024u  // ASND only runs its voices once per 1024 output samples (21.3 ms, measured)
#define AUDIO_STACK_SIZE (24u * 1024u)
#define FALLBACK_WAIT_MS 600u

enum { S_FREE = 0, S_READY = 1, S_PLAYING = 2 };

static struct {
    bool running;
    WlAudioBackend prefer;
    WlAudioBackend active; // AESND or ASND while running
    uint32_t rate, frames, bytes, n_slots; // frames/bytes: one hardware block
    uint32_t chunk;                        // frames per fill call (frames / chunks)
    uint32_t req_frames;                   // the engine's largest chunk (block_frames of start)
    WlAudioFillFn fill;
    void *user;
    int16_t *buf[MAX_SLOTS];
    int16_t *mute;
    atomic_uint state[MAX_SLOTS];
    uint32_t fill_idx; // audio thread
    uint32_t play_idx; // voice callback: the next block to hand out
    int32_t flight[2]; // ASND: blocks owned by the hardware (oldest first), -1 = the mute block
    uint32_t flight_n;
    int32_t playing;   // AESND: the block the voice is reading
    atomic_uint underruns, played, filled, fill_us_max, fill_us_total;
    atomic_bool quit;
    WlPlatformThread thread;
    WlPlatformSemaphore sem;
    void *stack;
    AESNDPB *pb;
} g;

bool wl_platform_audio_threaded(void) {
    return true;
}

void wl_platform_audio_prefer(WlAudioBackend backend) {
    g.prefer = backend;
}

uint32_t wl_platform_audio_underruns(void) {
    return atomic_load(&g.underruns);
}

void wl_platform_audio_stats(WlPlatformAudioStats *out) {
    out->backend = g.running ? (g.active == WL_AUDIO_BACKEND_ASND ? 2u : 1u) : 0u;
    out->hw_block_frames = g.running ? g.frames : 0u;
    out->blocks_filled = atomic_load(&g.filled);
    out->blocks_played = atomic_load(&g.played);
    out->underruns = atomic_load(&g.underruns);
    out->fill_us_max = atomic_load(&g.fill_us_max);
    out->fill_us_total = atomic_load(&g.fill_us_total);
}

// ---------------------------------------------------------------------------------------------
// The mixer thread
// ---------------------------------------------------------------------------------------------

static void *audio_thread(void *arg) {
    (void)arg;
    for (;;) {
        wl_platform_sem_wait(g.sem);
        if (atomic_load(&g.quit)) {
            break;
        }
        while (atomic_load_explicit(&g.state[g.fill_idx], memory_order_acquire) == S_FREE) {
            int16_t *dst = g.buf[g.fill_idx];
            const uint64_t t0 = wl_platform_fs_clock_us();
            for (uint32_t done = 0; done < g.frames; done += g.chunk) {
                g.fill(g.user, dst + 2u * done, g.chunk);
            }
            wl_gpu_buffer_commit(dst, g.bytes);
            const uint32_t dt = (uint32_t)(wl_platform_fs_clock_us() - t0);
            atomic_fetch_add(&g.fill_us_total, dt);
            if (dt > atomic_load_explicit(&g.fill_us_max, memory_order_relaxed)) {
                atomic_store_explicit(&g.fill_us_max, dt, memory_order_relaxed);
            }
            atomic_fetch_add(&g.filled, 1u);
            atomic_store_explicit(&g.state[g.fill_idx], S_READY, memory_order_release);
            g.fill_idx = (g.fill_idx + 1u) % g.n_slots;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// Interrupt side: pointer moves only
// ---------------------------------------------------------------------------------------------

static void release_slot(int32_t slot) {
    if (slot >= 0) {
        atomic_store_explicit(&g.state[slot], S_FREE, memory_order_release);
        wl_platform_sem_post(g.sem);
    }
}

// Takes the next READY block for the hardware: its index, or -1 (and an underrun) when the mixer
// is late. The mute block is returned through *buf.
static int32_t take_next(const int16_t **buf) {
    const uint32_t i = g.play_idx;
    if (atomic_load_explicit(&g.state[i], memory_order_acquire) == S_READY) {
        atomic_store_explicit(&g.state[i], S_PLAYING, memory_order_relaxed);
        g.play_idx = (i + 1u) % g.n_slots;
        atomic_fetch_add(&g.played, 1u);
        *buf = g.buf[i];
        return (int32_t)i;
    }
    atomic_fetch_add(&g.underruns, 1u);
    *buf = g.mute;
    return -1;
}

static void aesnd_voice_cb(AESNDPB *pb, u32 state, void *arg) {
    (void)arg;
    if (state != VOICE_STATE_STREAM) {
        return;
    }
    const int16_t *next;
    const int32_t finished = g.playing;
    g.playing = take_next(&next);
    AESND_SetVoiceBuffer(pb, next, g.bytes);
    release_slot(finished);
}

static void asnd_voice_cb(s32 voice) {
    if (g.flight_n >= 2u) { // the oldest buffer has ended: its block is free again
        release_slot(g.flight[0]);
        g.flight[0] = g.flight[1];
        g.flight_n = 1;
    }
    const int16_t *next;
    const int32_t slot = take_next(&next);
    if (ASND_AddVoice(voice, (void *)(uintptr_t)next, (s32)g.bytes) != SND_OK) {
        // Second slot still busy (spurious call): give the block back.
        if (slot >= 0) {
            g.play_idx = (uint32_t)slot;
            atomic_store_explicit(&g.state[slot], S_READY, memory_order_relaxed);
            atomic_fetch_sub(&g.played, 1u);
        } else {
            atomic_fetch_sub(&g.underruns, 1u);
        }
        return;
    }
    g.flight[g.flight_n++] = slot;
}

// ---------------------------------------------------------------------------------------------
// Start and stop
// ---------------------------------------------------------------------------------------------

static void free_buffers(void) {
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        wl_mem_free(g.buf[i]);
        g.buf[i] = NULL;
    }
    wl_mem_free(g.mute);
    g.mute = NULL;
    wl_mem_free(g.stack);
    g.stack = NULL;
}

static void hw_stop(WlAudioBackend which) {
    if (which == WL_AUDIO_BACKEND_AESND) {
        if (g.pb != NULL) {
            AESND_SetVoiceStop(g.pb, true);
            AESND_RegisterVoiceCallbackWithArg(g.pb, NULL, NULL);
            AESND_FreeVoice(g.pb);
            g.pb = NULL;
        }
        AESND_Pause(true);
    } else if (which == WL_AUDIO_BACKEND_ASND) {
        ASND_StopVoice(0);
        ASND_Pause(1);
    }
}

static void thread_stop(void) {
    atomic_store(&g.quit, true);
    wl_platform_sem_post(g.sem);
    wl_platform_thread_join(g.thread, NULL);
    wl_platform_sem_destroy(g.sem);
}

// Brings the hardware path `which` up with blocks 0..n-1 prefilled; block 0 is the first played.
static void hw_start(WlAudioBackend which) {
    for (uint32_t i = 0; i < g.n_slots; i++) {
        atomic_store(&g.state[i], i == 0 ? S_PLAYING : S_READY);
    }
    g.play_idx = 1;
    g.fill_idx = 0;
    g.flight_n = 1;
    g.flight[0] = 0;
    g.playing = 0;
    atomic_fetch_add(&g.played, 1u);
    if (which == WL_AUDIO_BACKEND_AESND) {
        AESND_Init();
        g.pb = AESND_AllocateVoiceWithArg(aesnd_voice_cb, NULL);
        if (g.pb != NULL) {
            AESND_SetVoiceStream(g.pb, true);
            AESND_SetVoiceVolume(g.pb, 255, 255);
            // looped = true: the engine stops a one-shot voice when its buffer ends, even when the callback
            // has just queued the next one; a looping stream voice restarts at the buffer the callback set.
            AESND_PlayVoice(g.pb, VOICE_STEREO16, g.buf[0], g.bytes, (f32)g.rate, 0, true);
        }
        AESND_Pause(false);
    } else {
        ASND_Init();
        ASND_Pause(0);
        ASND_SetVoice(0, VOICE_STEREO_16BIT, (s32)g.rate, 0, g.buf[0], (s32)g.bytes, 255, 255,
                      asnd_voice_cb);
    }
}

static WlResult start_with(WlAudioBackend which) {
    for (uint32_t i = 0; i < g.n_slots; i++) {
        for (uint32_t done = 0; done < g.frames; done += g.chunk) { // prime on the caller's thread: the audio thread is not up yet
            g.fill(g.user, g.buf[i] + 2u * done, g.chunk);
        }
        wl_gpu_buffer_commit(g.buf[i], g.bytes);
    }
    atomic_store(&g.quit, false);
    if (wl_platform_sem_create(&g.sem, 0, MAX_SLOTS) != WL_OK ||
        wl_platform_thread_create(&g.thread, audio_thread, NULL, g.stack, AUDIO_STACK_SIZE,
                                  WL_THREAD_PRIO_AUDIO) != WL_OK) {
        return WL_ERR_FAILED;
    }
    g.active = which;
    hw_start(which);
    return WL_OK;
}

WlResult wl_platform_audio_start(uint32_t sample_rate, uint32_t block_frames, WlAudioFillFn fill,
                                 void *user) {
    if (g.running) {
        return WL_ERR_STATE;
    }
    if (fill == NULL || block_frames == 0 || (block_frames & 7u) != 0 || sample_rate < 8000u ||
        sample_rate > 48000u || block_frames * 4u > 4096u || block_frames < 36u) {
        return WL_ERR_ARG;
    }
    g.rate = sample_rate;
    g.req_frames = block_frames;
    g.fill = fill;
    g.user = user;
    atomic_store(&g.underruns, 0);
    atomic_store(&g.played, 0);
    atomic_store(&g.filled, 0);
    atomic_store(&g.fill_us_max, 0);
    atomic_store(&g.fill_us_total, 0);

    WlAudioBackend which = g.prefer == WL_AUDIO_BACKEND_ASND ? WL_AUDIO_BACKEND_ASND : WL_AUDIO_BACKEND_AESND;
    for (int attempt = 0; attempt < 2; attempt++) {
        g.n_slots = which == WL_AUDIO_BACKEND_ASND ? 4u : 3u;
        g.frames = which == WL_AUDIO_BACKEND_ASND ? ASND_BLOCK_FRAMES : AESND_BLOCK_FRAMES;
        g.bytes = g.frames * 4u;
        g.chunk = g.frames;
        while (g.chunk > g.req_frames) { // the engine's largest chunk: split the hardware block in equal parts
            g.chunk /= 2u;
        }
        for (uint32_t i = 0; i < g.n_slots; i++) {
            g.buf[i] = WL_MEM_ALLOC_GPU(WL_TAG_AUDIO, g.bytes);
        }
        g.mute = WL_MEM_ALLOC_GPU(WL_TAG_AUDIO, g.bytes);
        g.stack = WL_MEM_ALLOC_GPU(WL_TAG_STACK, AUDIO_STACK_SIZE);
        bool ok = g.mute != NULL && g.stack != NULL;
        for (uint32_t i = 0; i < g.n_slots; i++) {
            ok = ok && g.buf[i] != NULL;
        }
        if (!ok) {
            free_buffers();
            return WL_ERR_NOMEM;
        }
        for (uint32_t i = 0; i < g.frames * 2u; i++) {
            g.mute[i] = 0;
        }
        wl_gpu_buffer_commit(g.mute, g.bytes);
        WlResult r = start_with(which);
        if (r != WL_OK) {
            free_buffers();
            return r;
        }
        g.running = true;
        if (g.prefer != WL_AUDIO_BACKEND_AUTO || which == WL_AUDIO_BACKEND_ASND) {
            WL_LOG_I("AUDIO", "output started: %s", which == WL_AUDIO_BACKEND_ASND ? "ASND" : "AESND");
            return WL_OK;
        }
        // AUTO with AESND: it must start consuming blocks, otherwise fall back to ASND.
        for (uint32_t waited = 0; waited < FALLBACK_WAIT_MS; waited += 5u) {
            if (atomic_load(&g.played) >= 3u) {
                WL_LOG_I("AUDIO", "output started: AESND");
                return WL_OK;
            }
            wl_platform_thread_sleep_ms(5);
        }
        WL_LOG_W("AUDIO", "AESND consumed no blocks in %u ms, falling back to ASND", FALLBACK_WAIT_MS);
        wl_platform_audio_stop();
        which = WL_AUDIO_BACKEND_ASND;
    }
    return WL_ERR_FAILED;
}

void wl_platform_audio_stop(void) {
    if (!g.running) {
        return;
    }
    hw_stop(g.active); // after this no callback runs
    thread_stop();
    free_buffers();
    g.running = false;
}
