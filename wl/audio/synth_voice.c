// SPDX-License-Identifier: MIT
// SSEQ synth voices - allocation and stealing, the ADSR envelope in decibel-like units,
// the LFO, pitch, per-frame gains, wave playback (PCM8, PCM16, IMA-ADPCM decoded on the fly, PSG
// square, noise) and the mix. Everything is integer and bit-identical on every platform.
#include <string.h>

#include "wl/audio/synth_internal.h"
#include "wl/base/endian.h"

// ---- envelope rates ---------------------------------------------------------------------------

// Attack: the envelope level is multiplied by (rate / 256) every frame, so it climbs from silence
// to 0 dB along a curve; 127 is instant, 0 the slowest. Rates for attack values 109..127 come from
// a table.
static uint32_t attack_mult(uint32_t a) {
    static const uint8_t tab[19] = {0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49, 0x54,
                                    0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F};
    if (a > 127u) {
        a = 127u;
    }
    return a >= 109u ? tab[127u - a] : 255u - a;
}

// Decay and release: level units subtracted per frame; 127 empties in two frames, 0 almost never.
static int32_t fall_rate(uint32_t d) {
    if (d >= 127u) {
        return 0xFFFF;
    }
    if (d == 126u) {
        return 0x3C00;
    }
    if (d < 50u) {
        return (int32_t)(d * 2u + 1u);
    }
    return (int32_t)(0x1E00u / (126u - d));
}

// ---- wave fetch ---------------------------------------------------------------------------------
// Each wave kind has an inlined fetch: the next sample of the voice's wave, handling the end of a
// one-shot wave (silence, counted in `tail`) and loops (back to the loop start, restoring the ADPCM
// decoder state saved there).

#define WL_INLINE static inline __attribute__((always_inline))

// False when the wave is exhausted (a one-shot wave past its end).
WL_INLINE bool wave_wrap(WlSynVoice *v) {
    if (v->rd >= v->nsamp) {
        if (!v->loop) {
            v->tail++;
            return false;
        }
        v->rd = v->loop_start;
        if (v->kind == WL_SYN_K_ADPCM) {
            v->ima = v->ima_loop;
        }
    }
    return true;
}

WL_INLINE int32_t fetch_pcm8(WlSynVoice *v) {
    if (!wave_wrap(v)) {
        return 0;
    }
    return (int32_t)(int8_t)v->data[v->rd++] * 256;
}

WL_INLINE int32_t fetch_pcm16(WlSynVoice *v) {
    if (!wave_wrap(v)) {
        return 0;
    }
    const uint8_t *p = v->data + 2u * v->rd++;
    return (int16_t)(uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

// IMA-ADPCM: 4-byte header, then nibbles, low first. The step is wl_ima_step inlined.
WL_INLINE int32_t fetch_adpcm(WlSynVoice *v) {
    if (!wave_wrap(v)) {
        return 0;
    }
    const uint32_t i = v->rd++;
    if (v->loop && !v->loop_saved && i == v->loop_start) {
        v->ima_loop = v->ima;
        v->loop_saved = true;
    }
    const uint32_t byte = v->data[4u + (i >> 1)];
    const uint32_t nib = (i & 1u) ? (byte >> 4) : (byte & 15u);
    const int32_t step = wl_ima_step_table[v->ima.index];
    int32_t diff = step >> 3;
    diff += (nib & 4u) ? step : 0;
    diff += (nib & 2u) ? (step >> 1) : 0;
    diff += (nib & 1u) ? (step >> 2) : 0;
    int32_t p = v->ima.pred + ((nib & 8u) ? -diff : diff);
    p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    int32_t idx = (int32_t)v->ima.index + wl_ima_index_table[nib & 7u];
    idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    v->ima.pred = (int16_t)p;
    v->ima.index = (uint8_t)idx;
    return p;
}

WL_INLINE int32_t fetch_psg(WlSynVoice *v) {
    const uint32_t i = v->rd++ & 7u;
    return i <= v->duty ? 32767 : -32767; // high for (duty + 1) of 8 steps
}

WL_INLINE int32_t fetch_noise(WlSynVoice *v) {
    v->rd++;
    const uint32_t lsb = v->lfsr & 1u;
    v->lfsr = (uint16_t)((v->lfsr >> 1) ^ (lsb ? 0x6000u : 0u));
    return lsb ? -32767 : 32767;
}

static int32_t fetch(WlSynVoice *v) {
    switch (v->kind) {
    case WL_SYN_K_PCM8:
        return fetch_pcm8(v);
    case WL_SYN_K_PCM16:
        return fetch_pcm16(v);
    case WL_SYN_K_ADPCM:
        return fetch_adpcm(v);
    case WL_SYN_K_PSG:
        return fetch_psg(v);
    default:
        return fetch_noise(v);
    }
}

// ---- allocation ---------------------------------------------------------------------------------

static int32_t voice_alloc(WlSynth *s, uint32_t prio) {
    uint32_t limit = s->par.voice_limit;
    if (limit == 0 || limit > WL_SYN_VOICES) {
        limit = WL_SYN_VOICES;
    }
    for (uint32_t i = 0; i < limit; i++) {
        if (s->voice[i].state == WL_SYN_V_OFF) {
            return (int32_t)i;
        }
    }
    // The cheapest victim: a releasing voice first, then the lowest priority, then the oldest.
    int32_t best = -1;
    for (uint32_t i = 0; i < limit; i++) {
        const WlSynVoice *v = &s->voice[i];
        if (best < 0) {
            best = (int32_t)i;
            continue;
        }
        const WlSynVoice *b = &s->voice[best];
        const bool vr = v->state == WL_SYN_V_RELEASE;
        const bool br = b->state == WL_SYN_V_RELEASE;
        if (vr != br) {
            if (vr) {
                best = (int32_t)i;
            }
        } else if (v->prio != b->prio) {
            if (v->prio < b->prio) {
                best = (int32_t)i;
            }
        } else if (v->order < b->order) {
            best = (int32_t)i;
        }
    }
    const WlSynVoice *b = &s->voice[best];
    if (b->state != WL_SYN_V_RELEASE && b->prio > prio) {
        return -1; // everything playing outranks the newcomer
    }
    s->stats.steals++;
    s->stats.steals_live += b->state != WL_SYN_V_RELEASE ? 1u : 0u;
    wl_syn_emit(s, WL_SYN_EV_STEAL, b->track, (uint32_t)best, 0, 0, 0);
    return best;
}

void wl_syn_voice_release(WlSynth *s, uint32_t vi) {
    WlSynVoice *v = &s->voice[vi];
    if (v->state != WL_SYN_V_OFF && v->state != WL_SYN_V_RELEASE) {
        v->state = WL_SYN_V_RELEASE;
    }
}

int32_t wl_syn_voice_start(WlSynth *s, uint32_t ti, const WlSynNote *nt, const WlSynWave *wave,
                           uint32_t key, uint32_t vel, int32_t len_ticks) {
    const WlSynTrack *t = &s->trk[ti];
    uint32_t prio = (uint32_t)t->prio + s->par.chan_priority;
    if (prio > 255u) {
        prio = 255u;
    }
    const int32_t vi = voice_alloc(s, prio);
    if (vi < 0) {
        return -1;
    }
    WlSynVoice *v = &s->voice[vi];
    memset(v, 0, sizeof *v);
    v->order = ++s->voice_order;
    v->track = (uint8_t)ti;
    v->key = (uint8_t)key;
    v->vel = (uint8_t)vel;
    v->prio = (uint8_t)prio;
    v->inst_pan = nt->pan;
    v->lfsr = 0x7FFFu;

    switch (nt->type) {
    case WL_SYN_NOTE_PCM:
        v->kind = wave->enc == 0 ? WL_SYN_K_PCM8 : wave->enc == 1 ? WL_SYN_K_PCM16 : WL_SYN_K_ADPCM;
        v->rate = wave->rate;
        v->nsamp = wave->nsamples;
        v->loop = wave->loop;
        v->loop_start = wave->loop_start;
        v->data = wave->data;
        v->base_key = nt->key;
        if (v->kind == WL_SYN_K_ADPCM) {
            const int16_t pred = (int16_t)wl_rd_le16(wave->data);
            wl_ima_init(&v->ima, pred, wave->data[2] & 127u);
        }
        break;
    case WL_SYN_NOTE_PSG:
        v->kind = WL_SYN_K_PSG;
        v->duty = (uint8_t)(nt->wave > 7u ? 7u : nt->wave);
        v->rate = 8u * 440u; // eight steps per cycle: a note of the key's frequency
        v->base_key = 69;
        break;
    default:
        v->kind = WL_SYN_K_NOISE;
        v->rate = 8u * 440u;
        v->base_key = 69;
        break;
    }
    v->base_pitch = ((int32_t)key - (int32_t)v->base_key) * 64;

    const uint32_t a = t->atk != 255 ? t->atk : nt->attack;
    const uint32_t d = t->dec != 255 ? t->dec : nt->decay;
    const uint32_t su = t->sus != 255 ? t->sus : nt->sustain;
    const uint32_t r = t->rel != 255 ? t->rel : nt->release;
    v->attack_mult = (uint8_t)attack_mult(a);
    v->decay_rate = fall_rate(d);
    v->release_rate = fall_rate(r);
    v->sustain_env = wl_syn_volume_db(su) * 128;
    v->env = WL_SYN_ENV_MIN;
    v->state = WL_SYN_V_ATTACK;
    v->len = len_ticks;
    v->tied = t->tie;

    // Pitch glide: the sweep command, and portamento from the previous key.
    v->sweep_pitch = t->sweep;
    v->sweep_start_tick = s->tick;
    v->sweep_ticks = (uint32_t)len_ticks;
    if (t->porta) {
        v->sweep_pitch += ((int32_t)t->porta_key - (int32_t)key) * 64;
        if (t->porta_time != 0) {
            const uint32_t m = (uint32_t)(v->sweep_pitch < 0 ? -v->sweep_pitch : v->sweep_pitch);
            v->sweep_frames = ((uint32_t)t->porta_time * t->porta_time * m) >> 11;
            if (v->sweep_frames == 0) {
                v->sweep_frames = 1;
            }
        }
    }

    // Prime the interpolator: samples 0 and 1.
    v->s0 = fetch(v);
    v->s1 = fetch(v);
    if (len_ticks <= 0) {
        v->state = WL_SYN_V_RELEASE;
    }
    return vi;
}

// ---- per-frame update ---------------------------------------------------------------------------

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void voice_frame(WlSynth *s, uint32_t vi, uint32_t flen) {
    WlSynVoice *v = &s->voice[vi];
    const WlSynTrack *t = &s->trk[v->track];

    switch (v->state) {
    case WL_SYN_V_ATTACK:
        v->env = (v->env * (int32_t)v->attack_mult) / 256;
        if (v->env >= 0) {
            v->env = 0;
            v->state = WL_SYN_V_DECAY;
        }
        break;
    case WL_SYN_V_DECAY:
        v->env -= v->decay_rate;
        if (v->env <= v->sustain_env) {
            v->env = v->sustain_env;
            v->state = WL_SYN_V_SUSTAIN;
        }
        break;
    case WL_SYN_V_RELEASE:
        v->env -= v->release_rate;
        if (v->env <= WL_SYN_ENV_MIN) {
            v->env = WL_SYN_ENV_MIN;
            v->state = WL_SYN_V_OFF;
        }
        break;
    default:
        break;
    }
    if (v->tail >= 2u && v->state != WL_SYN_V_OFF) {
        v->state = WL_SYN_V_OFF; // a one-shot wave reached its end
    }
    if (v->state == WL_SYN_V_OFF) {
        v->gl = v->gr = v->dgl = v->dgr = v->tgl = v->tgr = 0;
        wl_syn_emit(s, WL_SYN_EV_VOICE_OFF, v->track, vi, 0, 0, 0);
        return;
    }

    // LFO.
    int32_t lfo = 0;
    if (t->mod_depth != 0) {
        if (v->lfo_delay_cnt < t->mod_delay) {
            v->lfo_delay_cnt++;
        } else {
            v->lfo_phase = (v->lfo_phase + ((uint32_t)t->mod_speed << 6)) & 0x7FFFu;
            lfo = wl_syn_sin(v->lfo_phase >> 8) * (int32_t)t->mod_depth * (int32_t)t->mod_range;
        }
    }

    // Pitch: base, glide, bend and vibrato, in 1/64 semitones.
    int32_t sweep = 0;
    if (v->sweep_pitch != 0) {
        if (v->sweep_frames != 0) {
            if (v->age_frames < v->sweep_frames) {
                sweep = (int32_t)(((int64_t)v->sweep_pitch *
                                   (int64_t)(v->sweep_frames - v->age_frames)) /
                                  (int64_t)v->sweep_frames);
            }
        } else if (v->sweep_ticks != 0) {
            const int64_t total = (int64_t)v->sweep_ticks * 256;
            const int64_t el = (int64_t)(s->tick - v->sweep_start_tick) * 256 +
                               (int64_t)s->tempo_acc * 256 / (int64_t)WL_SYN_TICK_UNIT;
            if (el < total) {
                sweep = (int32_t)((int64_t)v->sweep_pitch * (total - el) / total);
            }
        }
    }
    v->age_frames++;
    int32_t pitch = v->base_pitch + sweep + ((int32_t)t->bend * (int32_t)t->bend_range) / 2;
    if (t->mod_type == 0) {
        pitch += clamp32(lfo / 256, -768 * 4, 768 * 4);
    }
    v->step = wl_syn_step(v->rate, pitch);

    // Level: velocity, track volume, expression, master and sequence volume are all "square law"
    // decibel values; the envelope and the LFO add in the finer units.
    int32_t db = wl_syn_volume_db(v->vel) + wl_syn_volume_db(t->vol) + wl_syn_volume_db(t->expr) +
                 wl_syn_volume_db(s->master) + wl_syn_volume_db(s->par.volume);
    int32_t level = db * 128 + v->env;
    if (t->mod_type == 1) {
        level += (clamp32((lfo * 60) / 16384, -723, 723)) * 128;
    }
    const uint32_t g = wl_syn_level_gain(level);
    int32_t pan = clamp32((int32_t)t->pan + (int32_t)v->inst_pan, -64, 63);
    if (t->mod_type == 2) {
        pan = clamp32(pan + lfo / 256, -64, 63);
    }
    const uint32_t pp = (uint32_t)(pan + 64); // 0 = left, 64 = centre, 127 = right
    const uint32_t gg = (uint32_t)(((uint64_t)g * s->par.gain_q8) >> 8);
    const int32_t tl = (int32_t)(((gg * (128u - pp)) >> 7) << 4);
    const int32_t tr = (int32_t)(((gg * pp) >> 7) << 4);
    // The previous frame's ramp ends exactly on its target (the per-sample step is truncated).
    v->gl = v->tgl;
    v->gr = v->tgr;
    v->dgl = (tl - v->gl) / (int32_t)flen;
    v->dgr = (tr - v->gr) / (int32_t)flen;
    v->tgl = tl;
    v->tgr = tr;
}

static void run_frame(WlSynth *s) {
    s->stats.frames++;
    s->frame_acc += WL_SYN_RATE;
    s->frame_len = s->frame_acc / WL_SYN_FRAME_HZ;
    s->frame_acc -= s->frame_len * WL_SYN_FRAME_HZ;
    s->frame_left = s->frame_len;

    if (wl_syn_tracks_active(s) != 0 || wl_syn_voices_active(s) != 0) {
        s->tempo_acc += ((uint32_t)s->tempo * s->par.tempo_q8) >> 8;
        while (s->tempo_acc >= WL_SYN_TICK_UNIT) {
            s->tempo_acc -= WL_SYN_TICK_UNIT;
            wl_syn_tick(s);
        }
    }
    uint32_t active = 0;
    for (uint32_t i = 0; i < WL_SYN_VOICES; i++) {
        if (s->voice[i].state != WL_SYN_V_OFF) {
            voice_frame(s, i, s->frame_len);
            if (s->voice[i].state != WL_SYN_V_OFF) {
                active++;
            }
        }
    }
    if (active > s->stats.voices_peak) {
        s->stats.voices_peak = active;
    }
    if (active == 0 && wl_syn_tracks_active(s) == 0) {
        s->done = true;
    }
}

// ---- mixing -------------------------------------------------------------------------------------

// The voice mixer, one instance per wave kind so that the fetch inlines into the sample loop.
#define WL_SYN_MIX_LOOP(FETCH, INTERP)                                                             \
    for (uint32_t i = 0; i < n; i++) {                                                             \
        const int32_t smp = INTERP;                                                                \
        acc[0] += (smp * (gl >> 8)) >> 12;                                                         \
        acc[1] += (smp * (gr >> 8)) >> 12;                                                         \
        acc += 2;                                                                                  \
        gl += dgl;                                                                                 \
        gr += dgr;                                                                                 \
        frac += step;                                                                              \
        while (frac >= 65536u) {                                                                   \
            frac -= 65536u;                                                                        \
            s0 = s1;                                                                               \
            s1 = FETCH(v);                                                                         \
        }                                                                                          \
    }

// The same with steady gains (the common case between envelope steps): the gain multipliers are
// hoisted.
#define WL_SYN_MIX_LOOP_STEADY(FETCH, INTERP)                                                      \
    {                                                                                              \
        const int32_t ml = gl >> 8;                                                                \
        const int32_t mr = gr >> 8;                                                                \
        for (uint32_t i = 0; i < n; i++) {                                                         \
            const int32_t smp = INTERP;                                                            \
            acc[0] += (smp * ml) >> 12;                                                            \
            acc[1] += (smp * mr) >> 12;                                                            \
            acc += 2;                                                                              \
            frac += step;                                                                          \
            while (frac >= 65536u) {                                                               \
                frac -= 65536u;                                                                    \
                s0 = s1;                                                                           \
                s1 = FETCH(v);                                                                     \
            }                                                                                      \
        }                                                                                          \
    }

#define WL_SYN_DEFINE_MIX(NAME, FETCH)                                                             \
    static void NAME(WlSynth *s, WlSynVoice *v, uint32_t n) {                                      \
        int32_t gl = v->gl;                                                                        \
        int32_t gr = v->gr;                                                                        \
        const int32_t dgl = v->dgl;                                                                \
        const int32_t dgr = v->dgr;                                                                \
        uint32_t frac = v->frac;                                                                   \
        const uint32_t step = v->step;                                                             \
        int32_t s0 = v->s0;                                                                        \
        int32_t s1 = v->s1;                                                                        \
        int32_t *acc = s->acc;                                                                     \
        if (dgl == 0 && dgr == 0) {                                                                \
            if (s->par.nearest) {                                                                  \
                WL_SYN_MIX_LOOP_STEADY(FETCH, s0)                                                  \
            } else {                                                                               \
                WL_SYN_MIX_LOOP_STEADY(FETCH, s0 + (((s1 - s0) * (int32_t)(frac >> 4)) >> 12))     \
            }                                                                                      \
        } else if (s->par.nearest) {                                                               \
            WL_SYN_MIX_LOOP(FETCH, s0)                                                             \
        } else {                                                                                   \
            WL_SYN_MIX_LOOP(FETCH, s0 + (((s1 - s0) * (int32_t)(frac >> 4)) >> 12))                \
        }                                                                                          \
        v->gl = gl < 0 ? 0 : gl;                                                                   \
        v->gr = gr < 0 ? 0 : gr;                                                                   \
        v->frac = frac;                                                                            \
        v->s0 = s0;                                                                                \
        v->s1 = s1;                                                                                \
    }

WL_SYN_DEFINE_MIX(mix_pcm8, fetch_pcm8)
WL_SYN_DEFINE_MIX(mix_pcm16, fetch_pcm16)
WL_SYN_DEFINE_MIX(mix_adpcm, fetch_adpcm)
WL_SYN_DEFINE_MIX(mix_psg, fetch_psg)
WL_SYN_DEFINE_MIX(mix_noise, fetch_noise)

static void mix_voice(WlSynth *s, WlSynVoice *v, uint32_t n) {
    switch (v->kind) {
    case WL_SYN_K_PCM8:
        mix_pcm8(s, v, n);
        break;
    case WL_SYN_K_PCM16:
        mix_pcm16(s, v, n);
        break;
    case WL_SYN_K_ADPCM:
        mix_adpcm(s, v, n);
        break;
    case WL_SYN_K_PSG:
        mix_psg(s, v, n);
        break;
    default:
        mix_noise(s, v, n);
        break;
    }
    s->stats.voice_samples += n;
}

void wl_syn_render(WlSynth *s, int16_t *out, uint32_t frames) {
    if (atomic_load_explicit(&s->paused, memory_order_relaxed)) {
        memset(out, 0, sizeof(int16_t) * 2u * frames);
        return;
    }
    uint32_t done = 0;
    while (done < frames) {
        if (s->frame_left == 0) {
            if (s->done) {
                memset(out + 2u * done, 0, sizeof(int16_t) * 2u * (frames - done));
                return;
            }
            run_frame(s);
        }
        uint32_t n = frames - done;
        if (n > s->frame_left) {
            n = s->frame_left;
        }
        if (n > WL_SYN_CHUNK) {
            n = WL_SYN_CHUNK;
        }
        memset(s->acc, 0, sizeof(int32_t) * 2u * n);
        for (uint32_t i = 0; i < WL_SYN_VOICES; i++) {
            if (s->voice[i].state != WL_SYN_V_OFF) {
                mix_voice(s, &s->voice[i], n);
            }
        }
        int16_t *o = out + 2u * done;
        for (uint32_t i = 0; i < 2u * n; i++) {
            int32_t v = s->acc[i];
            if (v > 32767) {
                v = 32767;
                s->stats.clipped++;
            } else if (v < -32768) {
                v = -32768;
                s->stats.clipped++;
            }
            o[i] = (int16_t)v;
        }
        s->frame_left -= n;
        done += n;
    }
}

void wl_syn_pull(void *user, int16_t *stereo, uint32_t frames, bool *ended) {
    WlSynth *s = user;
    wl_syn_render(s, stereo, frames);
    *ended = s->done;
}

// ---- setup and accessors ------------------------------------------------------------------------

void wl_syn_params_default(WlSynParams *p) {
    memset(p, 0, sizeof *p);
    p->volume = 127;
    p->tempo_q8 = 256;
    p->gain_q8 = 256;
}

WlResult wl_syn_init(WlSynth *s, const WlSynSeq *seq, const WlSynBank *bank,
                     const WlSynParams *par) {
    if (s == NULL || seq == NULL || bank == NULL || seq->data == NULL) {
        return WL_ERR_ARG;
    }
    if (seq->data_size == 0) {
        return WL_ERR_FORMAT;
    }
    memset(s, 0, sizeof *s);
    s->seq = *seq;
    s->bank = bank;
    if (par != NULL) {
        s->par = *par;
    } else {
        wl_syn_params_default(&s->par);
    }
    if (s->par.volume > 127u) {
        s->par.volume = 127;
    }
    if (s->par.tempo_q8 == 0) {
        s->par.tempo_q8 = 256;
    }
    if (s->par.gain_q8 > 1024u) {
        s->par.gain_q8 = 1024; // keeps the Q20 gains and sample products inside 32 bits
    }
    s->gvars = s->par.global_vars != NULL ? s->par.global_vars : &s->vars[16];
    s->rng = s->par.seed != 0 ? s->par.seed : WL_SYN_DEFAULT_SEED;
    s->tempo = WL_SYN_DEFAULT_TEMPO;
    s->master = 127;
    // The first tick fires in the first frame, whatever the tempo (so tick n is frame 2n - 1 at
    // tempo 120).
    const uint32_t inc = ((uint32_t)s->tempo * s->par.tempo_q8) >> 8;
    s->tempo_acc = inc >= WL_SYN_TICK_UNIT ? 0u : WL_SYN_TICK_UNIT - inc;
    atomic_init(&s->paused, false);
    for (uint32_t i = 0; i < WL_SYN_TRACKS; i++) {
        wl_syn_track_reset(&s->trk[i]);
    }
    s->trk[0].active = true;
    return WL_OK;
}

bool wl_syn_done(const WlSynth *s) {
    return s->done;
}

uint32_t wl_syn_voices_active(const WlSynth *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < WL_SYN_VOICES; i++) {
        n += s->voice[i].state != WL_SYN_V_OFF ? 1u : 0u;
    }
    return n;
}

uint32_t wl_syn_tracks_active(const WlSynth *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < WL_SYN_TRACKS; i++) {
        n += s->trk[i].active ? 1u : 0u;
    }
    return n;
}

void wl_syn_set_paused(WlSynth *s, bool paused) {
    atomic_store_explicit(&s->paused, paused, memory_order_relaxed);
}

int32_t wl_syn_var_get(const WlSynth *s, uint32_t idx) {
    if (idx < 16u) {
        return s->vars[idx];
    }
    return idx < WL_SYN_VARS ? s->gvars[idx - 16u] : 0;
}

void wl_syn_var_set(WlSynth *s, uint32_t idx, int32_t value) {
    const int16_t w = (int16_t)clamp32(value, -32768, 32767);
    if (idx < 16u) {
        s->vars[idx] = w;
    } else if (idx < WL_SYN_VARS) {
        s->gvars[idx - 16u] = w;
    }
}

void wl_syn_set_trace(WlSynth *s, WlSynTraceFn fn, void *user) {
    s->trace = fn;
    s->trace_user = user;
}

const WlSynStats *wl_syn_stats(const WlSynth *s) {
    return &s->stats;
}
