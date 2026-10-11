// SPDX-License-Identifier: MIT
// The PWST stream player (format: wl/audio/README.md).
//
// Data path: the main thread (wl_audio_pump) keeps a ring of NBLK blocks per channel full by
// submitting reads to the IO thread (wl_fs_read_async) and marking a block READY when its reads
// complete; the mixer thread (wl_stream_pull) decodes DSP-ADPCM straight from the READY blocks as
// the mixer asks for frames and marks a block FREE when it has consumed it. Compressed data is
// what is buffered (32 KiB of ADPCM per channel is 1.8 s at 32 kHz), so 3 blocks x 2 channels is
// 192 KiB per stereo stream.
//
// Loops: the producer reads the "virtual stream": frames 0 .. frame(loop_end), then frame(loop_start)
// .. frame(loop_end) again and again; a block ends where the file region ends, so a block never
// straddles the loop. The decoder, at the sample after loop_end, switches to the loop frame with
// the channel's loop history and loop_ps and resumes at loop_start % 14 inside it (PWST flag bit 1:
// a mid-frame loop; the DSP hardware resumes the same way at a nibble address).
//
// Ownership: fields marked (M) belong to the main thread, (A) to the mixer thread; the block
// states and the atomics are the hand-over. A stream is freed only after its voice has left the
// mixer and every read has completed.
#include <string.h>

#include "wl/audio/adpcm.h"
#include "wl/audio/audio_internal.h"
#include "wl/platform/io.h"
#include "wl/platform/mem.h"

#define NBLK WL_AUDIO_STREAM_BLOCKS
#define BLK WL_AUDIO_STREAM_BLOCK
#define MIN_LOOP_BYTES 2048u // shorter loops belong to the sample player (one block per loop pass)

typedef enum { PH_FREE = 0, PH_HEADER, PH_BUFFERING, PH_PLAYING, PH_CLOSING } Phase;
enum { BLK_FREE = 0, BLK_LOADING = 1, BLK_READY = 2 };

typedef struct WlAudioStream WlAudioStream;

typedef struct {
    WlAudioStream *s;
    uint8_t kind; // 0 header, 1 block
    uint8_t blk;
} StreamRead;

typedef struct {
    uint8_t *data[2];
    uint32_t len; // bytes per channel in this block (M writes before READY)
    uint8_t pending; // (M) reads not yet completed
    atomic_uint state;
} Block;

struct WlAudioStream {
    // (M)
    uint8_t phase;
    bool failed, voice_started, hdr_ok;
    uint32_t handle;
    WlAudioPlay play;
    char path[WL_FS_PATH_MAX];
    uint64_t base;
    uint32_t size;
    _Alignas(32) uint8_t hdr[192];
    WlStreamInfo info;
    uint8_t *mem;
    Block blk[NBLK];
    StreamRead rd_hdr;
    StreamRead rd[NBLK];
    uint32_t reads_inflight;
    uint32_t prod_idx, fpos, end_bytes, loop_byte, chan_bytes;
    bool prod_eof;
    uint32_t play_seq, stop_seq;
    // (A)
    uint32_t cons_idx, cons_off, fr_pos, sample_pos;
    bool jump, done;
    WlDspState st[2];
    int16_t fr[2][WL_DSP_FRAME_SAMPLES];
    // shared
    atomic_bool abort;   // (M) sets: the voice must end
    atomic_bool closing; // (M) sets: do not count starvation
    atomic_bool paused;  // (M) sets: the mixer thread outputs silence and consumes nothing (music pause)
};

static WlAudioStream g_streams[WL_AUDIO_MAX_STREAMS];
static atomic_uint g_starved;
static uint32_t g_reads, g_errors;
static uint32_t g_last_failed; // handle of the most recent failed stream, for wl_audio_state

// ---------------------------------------------------------------------------------------------
// Mixer thread: decode on demand
// ---------------------------------------------------------------------------------------------

static bool next_frame(WlAudioStream *s) {
    Block *b = &s->blk[s->cons_idx];
    if (atomic_load_explicit(&b->state, memory_order_acquire) != BLK_READY) {
        return false;
    }
    const WlStreamInfo *in = &s->info;
    const uint32_t first = s->jump ? in->loop_start % WL_DSP_FRAME_SAMPLES : 0u;
    for (uint32_t ch = 0; ch < in->channels; ch++) {
        const uint8_t *fr = b->data[ch] + s->cons_off;
        WlDspState *st = &s->st[ch];
        if (s->jump) {
            st->h1 = in->ch[ch].loop_h1;
            st->h2 = in->ch[ch].loop_h2;
            wl_dsp_decode_frame_from(st, (uint8_t)in->ch[ch].loop_ps, fr, first, s->fr[ch]);
        } else {
            wl_dsp_decode_frame_from(st, fr[0], fr, 0, s->fr[ch]);
        }
    }
    s->fr_pos = first;
    s->jump = false;
    s->cons_off += WL_DSP_FRAME_BYTES;
    if (s->cons_off >= b->len) {
        s->cons_off = 0;
        atomic_store_explicit(&b->state, BLK_FREE, memory_order_release);
        s->cons_idx = (s->cons_idx + 1u) % NBLK;
    }
    return true;
}

void wl_stream_pull(void *user, int16_t *dst, uint32_t frames, bool *ended) {
    WlAudioStream *s = user;
    uint32_t i = 0;
    const bool abort = atomic_load_explicit(&s->abort, memory_order_relaxed);
    if (!abort && !s->done && atomic_load_explicit(&s->paused, memory_order_relaxed)) {
        memset(dst, 0, sizeof(int16_t) * 2u * frames); // frozen: the position stays where it is
        *ended = false;
        return;
    }
    if (!abort && !s->done) {
        const WlStreamInfo *in = &s->info;
        const bool loop = (in->flags & WL_PWST_FLAG_LOOP) != 0;
        while (i < frames) {
            if (s->fr_pos >= WL_DSP_FRAME_SAMPLES && !next_frame(s)) {
                break; // starved: the data has not arrived
            }
            const int16_t l = s->fr[0][s->fr_pos];
            dst[2u * i] = l;
            dst[2u * i + 1u] = in->channels == 2 ? s->fr[1][s->fr_pos] : l;
            i++;
            s->fr_pos++;
            s->sample_pos++;
            if (loop) {
                if (s->sample_pos > in->loop_end) { // played loop_end: back to loop_start
                    s->sample_pos = in->loop_start;
                    s->jump = true;
                    s->fr_pos = WL_DSP_FRAME_SAMPLES; // drop the rest of this frame
                }
            } else if (s->sample_pos >= in->samples) {
                s->done = true;
                break;
            }
        }
    }
    if (i < frames) {
        memset(dst + 2u * i, 0, sizeof(int16_t) * 2u * (frames - i));
        if (!s->done && !abort && !atomic_load_explicit(&s->closing, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&g_starved, frames - i, memory_order_relaxed);
        }
    }
    *ended = s->done || abort;
}

// ---------------------------------------------------------------------------------------------
// Main thread: reads and the state machine
// ---------------------------------------------------------------------------------------------

static void fail(WlAudioStream *s, const char *why) {
    if (!s->failed) {
        WL_LOG_W("AUDIO", "stream %u failed: %s", s->handle, why);
        g_errors++;
        g_last_failed = s->handle;
    }
    s->failed = true;
    atomic_store(&s->abort, true);
}

static void release_mem(WlAudioStream *s) {
    wl_mem_free(s->mem);
    s->mem = NULL;
}

static bool in_pool(const void *p) {
    const uintptr_t a = (uintptr_t)p;
    return a >= (uintptr_t)&g_streams[0] && a < (uintptr_t)&g_streams[WL_AUDIO_MAX_STREAMS];
}

static WlResult setup_buffers(WlAudioStream *s) {
    const WlStreamInfo *in = &s->info;
    s->chan_bytes = in->samples / 14u * WL_DSP_FRAME_BYTES;
    if (in->flags & WL_PWST_FLAG_LOOP) {
        s->loop_byte = in->loop_start / 14u * WL_DSP_FRAME_BYTES;
        s->end_bytes = (in->loop_end / 14u + 1u) * WL_DSP_FRAME_BYTES;
        if (s->end_bytes - s->loop_byte < MIN_LOOP_BYTES) {
            return WL_ERR_UNSUPPORTED;
        }
    } else {
        s->end_bytes = s->chan_bytes;
    }
    s->mem = wl_mem_alloc_at(WL_TAG_AUDIO, (size_t)in->channels * NBLK * BLK, WL_ARENA_ALIGN, __FILE__, __LINE__);
    if (s->mem == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t b = 0; b < NBLK; b++) {
        for (uint32_t ch = 0; ch < in->channels; ch++) {
            s->blk[b].data[ch] = s->mem + ((size_t)b * in->channels + ch) * BLK;
        }
        atomic_store(&s->blk[b].state, BLK_FREE);
    }
    for (uint32_t ch = 0; ch < in->channels; ch++) {
        memcpy(s->st[ch].coef, in->ch[ch].coef, sizeof s->st[ch].coef);
        s->st[ch].h1 = in->ch[ch].init_h1;
        s->st[ch].h2 = in->ch[ch].init_h2;
    }
    s->fr_pos = WL_DSP_FRAME_SAMPLES; // first call decodes frame 0
    return WL_OK;
}

WlResult wl_stream_start(const WlAudioSpan *span, const WlAudioPlay *play, uint32_t handle) {
    if (span->path == NULL || strlen(span->path) >= WL_FS_PATH_MAX || span->size < 0x60u) {
        return WL_ERR_ARG;
    }
    WlAudioStream *s = NULL;
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        if (g_streams[i].phase == PH_FREE) {
            s = &g_streams[i];
            break;
        }
    }
    if (s == NULL) {
        return WL_ERR_LIMIT;
    }
    memset(s, 0, sizeof *s);
    strcpy(s->path, span->path);
    s->base = span->offset;
    s->size = span->size;
    s->handle = handle;
    s->play = *play;
    s->rd_hdr.s = s;
    s->rd_hdr.kind = 0;
    for (uint32_t b = 0; b < NBLK; b++) {
        s->rd[b].s = s;
        s->rd[b].kind = 1;
        s->rd[b].blk = (uint8_t)b;
    }
    const uint32_t probe = span->size < WL_PWST_MAX_PROBE ? span->size : WL_PWST_MAX_PROBE;
    WlResult r = wl_fs_read_async(s->path, s->base, s->hdr, probe, &s->rd_hdr, NULL);
    if (r != WL_OK) {
        return r;
    }
    s->reads_inflight = 1;
    s->phase = PH_HEADER;
    return WL_OK;
}

bool wl_stream_handle_done(const WlFsDone *d) {
    if (!in_pool(d->user)) {
        return false;
    }
    StreamRead *rd = d->user;
    WlAudioStream *s = rd->s;
    if (s->reads_inflight > 0) {
        s->reads_inflight--;
    }
    g_reads++;
    if (d->result != WL_OK) {
        fail(s, "read error");
        return true;
    }
    if (rd->kind == 0) {
        const uint32_t probe = s->size < WL_PWST_MAX_PROBE ? s->size : WL_PWST_MAX_PROBE;
        WlResult r = wl_pwst_parse(s->hdr, probe, s->size, &s->info);
        if (r == WL_OK) {
            r = setup_buffers(s);
        }
        if (r != WL_OK) {
            fail(s, "bad PWST header or no memory");
        } else {
            s->hdr_ok = true;
            if (s->phase == PH_HEADER) {
                s->phase = PH_BUFFERING;
            }
        }
        return true;
    }
    Block *b = &s->blk[rd->blk];
    if (b->pending > 0 && --b->pending == 0) {
        atomic_store_explicit(&b->state, BLK_READY, memory_order_release);
    }
    return true;
}

static void produce(WlAudioStream *s) {
    const uint32_t nch = s->info.channels;
    while (!s->prod_eof) {
        Block *b = &s->blk[s->prod_idx];
        if (atomic_load_explicit(&b->state, memory_order_acquire) != BLK_FREE) {
            return;
        }
        if (wl_io_outstanding() + nch > WL_IO_QUEUE_DEPTH) {
            return; // retry next pump
        }
        uint32_t len = s->end_bytes - s->fpos;
        if (len > BLK) {
            len = BLK;
        }
        b->len = len;
        b->pending = (uint8_t)nch;
        atomic_store_explicit(&b->state, BLK_LOADING, memory_order_relaxed);
        for (uint32_t ch = 0; ch < nch; ch++) {
            const uint64_t off = s->base + s->info.data_off + (uint64_t)ch * s->chan_bytes + s->fpos;
            WlResult r = wl_fs_read_async(s->path, off, b->data[ch], len, &s->rd[s->prod_idx], NULL);
            if (r != WL_OK) {
                b->pending = (uint8_t)(b->pending - (nch - ch)); // reads never submitted
                if (ch == 0) {
                    atomic_store_explicit(&b->state, BLK_FREE, memory_order_relaxed);
                } else if (b->pending == 0) {
                    atomic_store_explicit(&b->state, BLK_READY, memory_order_release);
                }
                fail(s, "read submit failed");
                return;
            }
            s->reads_inflight++;
        }
        s->fpos += len;
        if (s->fpos >= s->end_bytes) {
            if (s->info.flags & WL_PWST_FLAG_LOOP) {
                s->fpos = s->loop_byte;
            } else {
                s->prod_eof = true;
            }
        }
        s->prod_idx = (s->prod_idx + 1u) % NBLK;
    }
}

// Samples decoded-ready in READY blocks, for the one-second pre-roll.
static uint32_t buffered_samples(WlAudioStream *s) {
    uint32_t n = 0;
    for (uint32_t b = 0; b < NBLK; b++) {
        if (atomic_load_explicit(&s->blk[b].state, memory_order_acquire) == BLK_READY) {
            n += s->blk[b].len / WL_DSP_FRAME_BYTES * WL_DSP_FRAME_SAMPLES;
        }
    }
    return n;
}

static bool all_loaded(WlAudioStream *s) {
    for (uint32_t b = 0; b < NBLK; b++) {
        if (atomic_load_explicit(&s->blk[b].state, memory_order_acquire) == BLK_LOADING) {
            return false;
        }
    }
    return s->prod_eof;
}

static void begin_close(WlAudioStream *s, uint32_t fade_ms, bool send_stop) {
    atomic_store(&s->closing, true);
    if (s->voice_started && send_stop) {
        WlAudioCmd c;
        memset(&c, 0, sizeof c);
        c.op = WL_ACMD_STOP;
        c.id = s->handle;
        c.fade_frames = fade_ms * (WL_MIX_RATE / 1000u);
        for (uint32_t i = 0; i < 20u && !wl_audio_push(&c); i++) {
            wl_platform_thread_sleep_ms(1);
        }
    }
    s->stop_seq = wl_audio_cmds_issued();
    s->phase = PH_CLOSING;
}

static void start_voice(WlAudioStream *s) {
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_PLAY_STREAM;
    c.id = s->handle;
    c.ptr = s;
    c.rate = s->info.rate;
    c.pitch = s->play.pitch_cents;
    c.gain = s->play.gain;
    c.pan = s->play.pan;
    c.bus = s->play.bus;
    c.priority = s->play.priority != 0 ? s->play.priority : 64;
    c.fade_frames = s->play.fade_in_ms * (WL_MIX_RATE / 1000u);
    if (wl_audio_push(&c)) {
        s->voice_started = true;
        s->play_seq = wl_audio_cmds_issued();
        s->phase = PH_PLAYING;
    } // else retry next pump
}

static void pump_one(WlAudioStream *s) {
    if (s->phase == PH_HEADER) {
        if (s->failed && s->reads_inflight == 0) {
            s->phase = PH_FREE;
        }
        return;
    }
    if (s->failed && s->phase != PH_CLOSING) {
        begin_close(s, 0, true);
    }
    switch (s->phase) {
    case PH_BUFFERING:
        produce(s);
        if (!s->failed && (buffered_samples(s) >= s->info.rate || (all_loaded(s) && buffered_samples(s) > 0))) {
            start_voice(s);
        }
        break;
    case PH_PLAYING:
        produce(s);
        if (wl_audio_cmds_done() >= s->play_seq && !wl_audio_voice_in_mixer(s->handle)) {
            begin_close(s, 0, false); // finished (or never started: the mixer was full)
        }
        break;
    case PH_CLOSING:
        if (s->reads_inflight == 0 &&
            (!s->voice_started ||
             (wl_audio_cmds_done() >= s->stop_seq && !wl_audio_voice_in_mixer(s->handle)))) {
            release_mem(s);
            s->phase = PH_FREE;
        }
        break;
    default:
        break;
    }
}

void wl_stream_pump(void) {
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        if (g_streams[i].phase != PH_FREE) {
            pump_one(&g_streams[i]);
        }
    }
}

void wl_stream_stop(uint32_t handle, uint32_t fade_ms) {
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        WlAudioStream *s = &g_streams[i];
        if (s->phase != PH_FREE && s->phase != PH_CLOSING && s->handle == handle) {
            begin_close(s, fade_ms, true);
            if (s->phase == PH_CLOSING && s->reads_inflight == 0 && !s->voice_started && s->mem == NULL) {
                s->phase = PH_FREE;
            }
        }
    }
}

bool wl_stream_set_paused(uint32_t handle, bool paused) {
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        WlAudioStream *s = &g_streams[i];
        if (s->phase != PH_FREE && s->phase != PH_CLOSING && s->handle == handle) {
            atomic_store_explicit(&s->paused, paused, memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void wl_stream_stop_all(void) {
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        WlAudioStream *s = &g_streams[i];
        if (s->phase != PH_FREE && s->phase != PH_CLOSING) {
            begin_close(s, 0, true);
        }
    }
}

bool wl_stream_owns(uint32_t handle) {
    if (handle == g_last_failed && handle != 0) {
        return true;
    }
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        if (g_streams[i].phase != PH_FREE && g_streams[i].handle == handle) {
            return true;
        }
    }
    return false;
}

WlAudioState wl_stream_state(uint32_t handle) {
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        const WlAudioStream *s = &g_streams[i];
        if (s->phase != PH_FREE && s->handle == handle) {
            if (s->failed) {
                return WL_AUDIO_FAILED;
            }
            switch (s->phase) {
            case PH_HEADER:
            case PH_BUFFERING:
                return WL_AUDIO_BUFFERING;
            case PH_PLAYING:
                return WL_AUDIO_PLAYING;
            default:
                return WL_AUDIO_GONE;
            }
        }
    }
    return handle == g_last_failed ? WL_AUDIO_FAILED : WL_AUDIO_GONE;
}

void wl_stream_stats(uint32_t *starved, uint32_t *reads, uint32_t *errors, uint32_t *active) {
    *starved = atomic_load_explicit(&g_starved, memory_order_relaxed);
    *reads = g_reads;
    *errors = g_errors;
    uint32_t n = 0;
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        n += g_streams[i].phase != PH_FREE ? 1u : 0u;
    }
    *active = n;
}

bool wl_stream_idle(void) {
    for (uint32_t i = 0; i < WL_AUDIO_MAX_STREAMS; i++) {
        if (g_streams[i].phase != PH_FREE) {
            return false;
        }
    }
    return true;
}

void wl_stream_reset(void) {
    memset(g_streams, 0, sizeof g_streams);
    atomic_store(&g_starved, 0);
    g_reads = g_errors = g_last_failed = 0;
}
