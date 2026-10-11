// SPDX-License-Identifier: MIT
// The software mixer: see mixer.h. Fixed point throughout so the output is bit-identical on the
// host and on the Wii (the synth regression digests rely on that).
#include "wl/audio/mixer.h"

#include <string.h>

#define G_ONE 16384 // Q14 unity

// 2^(i/96) in Q16 for i = 0..96 (one octave in 12.5-cent steps; linear interpolation in between
// is within 7e-6 of the true curve).
static const uint32_t k_pitch[97] = {
    65536,  66011,  66489,  66971,  67456,  67945,  68438,  68933,  69433,  69936,  70443,  70953,
    71468,  71985,  72507,  73032,  73562,  74095,  74632,  75172,  75717,  76266,  76819,  77375,
    77936,  78501,  79069,  79642,  80220,  80801,  81386,  81976,  82570,  83169,  83771,  84378,
    84990,  85606,  86226,  86851,  87480,  88114,  88752,  89396,  90043,  90696,  91353,  92015,
    92682,  93354,  94030,  94711,  95398,  96089,  96785,  97487,  98193,  98905,  99621,  100343,
    101070, 101803, 102540, 103283, 104032, 104786, 105545, 106310, 107080, 107856, 108638, 109425,
    110218, 111017, 111821, 112631, 113448, 114270, 115098, 115932, 116772, 117618, 118470, 119329,
    120194, 121065, 121942, 122825, 123715, 124612, 125515, 126425, 127341, 128263, 129193, 130129,
    131072};

// cos(i * pi / 254) in Q15, i = 0..127 (the left gain of the constant-power pan law; the right
// gain is the same table read backwards).
static const int16_t k_pan[128] = {
    32767, 32765, 32758, 32745, 32728, 32705, 32678, 32645, 32608, 32565, 32518, 32465, 32408, 32345,
    32278, 32206, 32128, 32046, 31959, 31867, 31771, 31669, 31562, 31451, 31335, 31214, 31088, 30958,
    30823, 30683, 30538, 30389, 30235, 30076, 29913, 29745, 29573, 29396, 29215, 29029, 28839, 28644,
    28445, 28242, 28034, 27822, 27606, 27386, 27161, 26932, 26699, 26462, 26221, 25976, 25727, 25474,
    25217, 24956, 24692, 24424, 24151, 23876, 23596, 23313, 23027, 22737, 22443, 22146, 21846, 21542,
    21235, 20925, 20611, 20294, 19975, 19652, 19326, 18997, 18666, 18331, 17994, 17654, 17311, 16965,
    16617, 16267, 15914, 15558, 15200, 14840, 14478, 14113, 13746, 13377, 13006, 12633, 12258, 11882,
    11503, 11123, 10741, 10357, 9972,  9585,  9196,  8807,  8416,  8023,  7630,  7235,  6839,  6442,
    6045,  5646,  5246,  4846,  4444,  4043,  3640,  3237,  2833,  2430,  2025,  1620,  1216,  810,
    405,   0};

uint32_t wl_mixer_step(uint32_t source_rate, int32_t cents) {
    if (cents > 4800) {
        cents = 4800;
    } else if (cents < -4800) {
        cents = -4800;
    }
    int32_t oct = cents / 1200;
    int32_t rem = cents - oct * 1200;
    if (rem < 0) {
        rem += 1200;
        oct -= 1;
    }
    const uint32_t i = (uint32_t)rem * 2u / 25u; // 12.5 cents per entry
    const uint32_t f = (uint32_t)rem * 2u % 25u; // 0..24 in 1/25 steps of an entry
    const uint64_t ratio = k_pitch[i] + ((uint64_t)(k_pitch[i + 1] - k_pitch[i]) * f) / 25u;
    uint64_t step = ((uint64_t)source_rate << 16) / WL_MIX_RATE;
    step = (step * ratio) >> 16;
    step = oct >= 0 ? step << oct : step >> (-oct);
    if (step < WL_MIX_STEP_MIN) {
        step = WL_MIX_STEP_MIN;
    } else if (step > WL_MIX_STEP_MAX) {
        step = WL_MIX_STEP_MAX;
    }
    return (uint32_t)step;
}

// Left and right Q15 gains for a pan position.
static void pan_gains(int pan, int32_t *l, int32_t *r) {
    int p = pan < -127 ? -127 : pan > 127 ? 127 : pan;
    const int idx2 = p + 127; // 0..254: half-steps of the 127-step table
    const int i = idx2 / 2;
    const int half = idx2 & 1;
    int32_t a = k_pan[i];
    int32_t b = k_pan[i + 1 > 127 ? 127 : i + 1];
    *l = half ? (a + b) / 2 : a;
    const int ri = 254 - idx2; // mirrored position
    const int j = ri / 2;
    a = k_pan[j];
    b = k_pan[j + 1 > 127 ? 127 : j + 1];
    *r = (ri & 1) ? (a + b) / 2 : a;
}

void wl_mixer_init(WlMixer *m) {
    memset(m, 0, sizeof *m);
    for (uint32_t b = 0; b < WL_BUS_COUNT; b++) {
        m->bus_gain[b] = 256;
    }
    m->bus_gain[WL_BUS_MUSIC] = 205; // 80 %: music sits under effects and speech
    m->bus_gain[WL_BUS_UI] = 179;    // 70 %
    m->master = 256;
    m->duck_explicit = 256;
    m->speech_duck = 128; // -6 dB
    m->duck_cur = G_ONE;
    for (uint32_t b = 0; b < WL_BUS_COUNT; b++) {
        m->bus_prev[b] = (int32_t)m->bus_gain[b] * 64;
    }
}

// ---------------------------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------------------------

static WlMixVoice *find(WlMixer *m, uint32_t id) {
    if (id == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
        if (m->voices[i].active && m->voices[i].id == id) {
            return &m->voices[i];
        }
    }
    return NULL;
}

bool wl_mixer_voice_active(const WlMixer *m, uint32_t id) {
    return find((WlMixer *)(uintptr_t)m, id) != NULL;
}

int wl_mixer_voice_start(WlMixer *m, const WlMixVoiceDesc *d) {
    if (d->id == 0 || d->bus >= WL_BUS_COUNT) {
        return -1;
    }
    const bool is_pull = d->pull != NULL;
    if (is_pull ? false : (d->pcm == NULL || d->frames == 0)) {
        return -1;
    }
    if (!is_pull && d->loop && (d->loop_end > d->frames || d->loop_start >= d->loop_end)) {
        return -1;
    }
    int slot = -1;
    for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
        if (!m->voices[i].active) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) { // steal: lowest priority, then oldest; only from voices not above the newcomer
        int victim = -1;
        for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
            const WlMixVoice *v = &m->voices[i];
            if (v->priority > d->priority) {
                continue;
            }
            if (victim < 0 || v->priority < m->voices[victim].priority ||
                (v->priority == m->voices[victim].priority && v->seq < m->voices[victim].seq)) {
                victim = (int)i;
            }
        }
        if (victim < 0) {
            m->stats.drops++;
            return -1;
        }
        m->stats.steals++;
        slot = victim;
    }
    WlMixVoice *v = &m->voices[slot];
    memset(v, 0, sizeof *v);
    v->id = d->id;
    v->seq = ++m->seq;
    v->active = true;
    v->is_pull = is_pull;
    v->bus = d->bus;
    v->priority = d->priority;
    v->gain = d->gain > 511 ? 511 : d->gain;
    v->pan = d->pan;
    v->rate = d->rate != 0 ? d->rate : WL_MIX_RATE;
    v->step = wl_mixer_step(v->rate, d->pitch_cents);
    v->pcm = d->pcm;
    v->frames = d->frames;
    v->stereo = is_pull ? true : d->stereo;
    v->loop = d->loop;
    v->loop_start = d->loop_start;
    v->loop_end = d->loop_end;
    v->pull = d->pull;
    v->pull_user = d->pull_user;
    v->gl = -1; // "no previous block": the first block starts at its target gain
    v->fade = G_ONE << 8;
    v->fade_goal = G_ONE << 8;
    if (d->fade_in_frames > 0) {
        v->fade = 0;
        v->fade_step = (int32_t)(((int64_t)G_ONE << 8) / (int64_t)d->fade_in_frames);
        if (v->fade_step < 1) {
            v->fade_step = 1;
        }
    }
    m->stats.voices_started++;
    return slot;
}

void wl_mixer_voice_stop(WlMixer *m, uint32_t id, uint32_t fade_frames) {
    WlMixVoice *v = find(m, id);
    if (v == NULL) {
        return;
    }
    if (fade_frames < WL_MIX_BLOCK) {
        fade_frames = WL_MIX_BLOCK;
    }
    v->stopping = true;
    v->fade_goal = 0;
    // From wherever the fade is now to silence in `fade_frames`.
    int32_t step = (int32_t)((int64_t)v->fade / (int64_t)fade_frames);
    v->fade_step = -(step < 1 ? 1 : step);
}

void wl_mixer_voice_set(WlMixer *m, uint32_t id, uint16_t gain, int8_t pan, int16_t pitch_cents) {
    WlMixVoice *v = find(m, id);
    if (v == NULL) {
        return;
    }
    v->gain = gain > 511 ? 511 : gain;
    v->pan = pan;
    v->step = wl_mixer_step(v->rate, pitch_cents);
}

void wl_mixer_stop_pcm(WlMixer *m, const int16_t *pcm) {
    for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
        if (m->voices[i].active && !m->voices[i].is_pull && m->voices[i].pcm == pcm) {
            m->voices[i].active = false;
        }
    }
}

void wl_mixer_stop_all(WlMixer *m) {
    for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
        m->voices[i].active = false;
    }
}

void wl_mixer_bus_set(WlMixer *m, WlBus bus, uint16_t gain_q8) {
    if ((unsigned)bus < WL_BUS_COUNT) {
        m->bus_gain[bus] = gain_q8 > 511 ? 511 : gain_q8;
    }
}

void wl_mixer_master_set(WlMixer *m, uint16_t gain_q8) {
    m->master = gain_q8 > 511 ? 511 : gain_q8;
}

void wl_mixer_duck_set(WlMixer *m, uint16_t music_factor_q8) {
    m->duck_explicit = music_factor_q8 > 256 ? 256 : music_factor_q8;
}

// ---------------------------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------------------------

// Target Q14 gains of a voice at the end of this block (fade already advanced).
static void voice_targets(const WlMixVoice *v, int32_t fade14, int32_t *gl, int32_t *gr) {
    const int32_t gain = (int32_t)v->gain * 64; // Q8 -> Q14
    int32_t l, r;
    if (v->stereo) {
        const int p = v->pan < -127 ? -127 : v->pan;
        l = p > 0 ? 32768 * (127 - p) / 127 : 32768;
        r = p < 0 ? 32768 * (127 + p) / 127 : 32768;
    } else {
        pan_gains(v->pan, &l, &r);
    }
    int32_t a = (int32_t)(((int64_t)gain * l) >> 15);
    int32_t b = (int32_t)(((int64_t)gain * r) >> 15);
    *gl = (int32_t)(((int64_t)a * fade14) >> 14);
    *gr = (int32_t)(((int64_t)b * fade14) >> 14);
}

// Accumulates one source frame pair into the bus buffers with the running Q22 gains.
#define ACCUM(al, ar, sl, sr, i, gl22, gr22)                                                       \
    do {                                                                                           \
        (al)[i] += ((int32_t)(sl) * ((gl22) >> 8)) >> 14;                                          \
        (ar)[i] += ((int32_t)(sr) * ((gr22) >> 8)) >> 14;                                          \
    } while (0)

#define INLINE static inline __attribute__((always_inline))

// The sample-voice loop, expanded once per combination of constant flags so the per-frame
// branches on stereo/loop/ramp disappear (the mixer's hot loop: 32 voices x 288 frames per block).
INLINE void sample_core(WlMixVoice *v, int32_t *al, int32_t *ar, uint32_t n, int32_t gl22, int32_t dl,
                        int32_t gr22, int32_t dr, const bool stereo, const bool loop, const bool ramp) {
    const int16_t *s = v->pcm;
    uint32_t pos = v->pos;
    uint32_t frac = v->frac;
    const uint32_t step = v->step;
    const uint32_t end = loop ? v->loop_end : v->frames;
    const uint32_t loop_start = v->loop_start;
    for (uint32_t i = 0; i < n; i++) {
        if (pos >= end) {
            v->active = false;
            break;
        }
        if (ramp) { // the block's last frame lands exactly on the end gain
            gl22 += dl;
            gr22 += dr;
        }
        uint32_t nxt = pos + 1;
        if (nxt >= end) {
            nxt = loop ? loop_start : pos;
        }
        const int32_t f15 = (int32_t)(frac >> 1);
        int32_t xl, xr;
        if (stereo) {
            const int32_t a0 = s[2 * pos], a1 = s[2 * pos + 1];
            const int32_t b0 = s[2 * nxt], b1 = s[2 * nxt + 1];
            xl = a0 + (((b0 - a0) * f15) >> 15);
            xr = a1 + (((b1 - a1) * f15) >> 15);
        } else {
            const int32_t a0 = s[pos], b0 = s[nxt];
            xl = xr = a0 + (((b0 - a0) * f15) >> 15);
        }
        ACCUM(al, ar, xl, xr, i, gl22, gr22);
        frac += step;
        pos += frac >> 16;
        frac &= 0xFFFFu;
        if (loop) {
            while (pos >= end) {
                pos = loop_start + (pos - end);
            }
        }
    }
    v->pos = pos;
    v->frac = frac;
}

static void mix_sample_voice(WlMixVoice *v, int32_t *al, int32_t *ar, uint32_t n, int32_t gl22, int32_t dl,
                             int32_t gr22, int32_t dr) {
#define CASE(st, lp, rp) sample_core(v, al, ar, n, gl22, dl, gr22, dr, st, lp, rp)
    const bool ramp = (dl | dr) != 0;
    if (v->stereo) {
        if (v->loop) {
            ramp ? CASE(true, true, true) : CASE(true, true, false);
        } else {
            ramp ? CASE(true, false, true) : CASE(true, false, false);
        }
    } else {
        if (v->loop) {
            ramp ? CASE(false, true, true) : CASE(false, true, false);
        } else {
            ramp ? CASE(false, false, true) : CASE(false, false, false);
        }
    }
#undef CASE
}

INLINE void pull_core(const int16_t *w, uint32_t *frac_io, int32_t *al, int32_t *ar, uint32_t n, uint32_t step,
                      int32_t gl22, int32_t dl, int32_t gr22, int32_t dr, const bool ramp, const bool unity) {
    uint32_t frac = *frac_io;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ramp) {
            gl22 += dl;
            gr22 += dr;
        }
        const int32_t a0 = w[2 * pos], a1 = w[2 * pos + 1];
        int32_t xl = a0, xr = a1;
        if (!unity && frac != 0) {
            const int32_t f15 = (int32_t)(frac >> 1);
            const int32_t b0 = w[2 * pos + 2], b1 = w[2 * pos + 3];
            xl = a0 + (((b0 - a0) * f15) >> 15);
            xr = a1 + (((b1 - a1) * f15) >> 15);
        }
        ACCUM(al, ar, xl, xr, i, gl22, gr22);
        frac += step;
        pos += frac >> 16;
        frac &= 0xFFFFu;
    }
    *frac_io = frac;
}

static void mix_pull_voice(WlMixer *m, WlMixVoice *v, int32_t *al, int32_t *ar, uint32_t n,
                           int32_t gl22, int32_t dl, int32_t gr22, int32_t dr) {
    int16_t *w = m->scratch; // the window: carry frames, then newly pulled frames
    const uint32_t step = v->step;
    const uint32_t frac0 = v->frac;
    const uint32_t last = (frac0 + step * (n - 1)) >> 16; // highest index read as `a`
    const uint32_t consumed = (frac0 + step * n) >> 16;
    uint32_t total = last + 2; // frames 0..last+1 are read
    if (consumed + 1 > total) {
        total = consumed + 1; // keep at least one frame to carry
    }
    memcpy(w, v->carry, sizeof(int16_t) * 2u * v->carry_frames);
    if (total > v->carry_frames) {
        bool ended = false;
        v->pull(v->pull_user, w + 2u * v->carry_frames, total - v->carry_frames, &ended);
        if (ended) {
            v->pull_ended = true;
        }
    }
    uint32_t frac = frac0;
    const bool ramp = (dl | dr) != 0;
    if (step == WL_MIX_STEP_UNITY && frac0 == 0) { // the common case: no resampling
        ramp ? pull_core(w, &frac, al, ar, n, step, gl22, dl, gr22, dr, true, true)
             : pull_core(w, &frac, al, ar, n, step, gl22, dl, gr22, dr, false, true);
    } else {
        ramp ? pull_core(w, &frac, al, ar, n, step, gl22, dl, gr22, dr, true, false)
             : pull_core(w, &frac, al, ar, n, step, gl22, dl, gr22, dr, false, false);
    }
    v->frac = frac;
    v->carry_frames = total - consumed; // 1 or 2
    memcpy(v->carry, w + 2u * consumed, sizeof(int16_t) * 2u * v->carry_frames);
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void wl_mixer_mix(WlMixer *m, int16_t *out, uint32_t n) {
    if (n == 0 || n > WL_MIX_BLOCK) {
        return;
    }
    uint32_t bus_used = 0; // buses a voice mixed into this block; the others stay untouched (and unzeroed)
    bool speech_playing = false;
    for (uint32_t k = 0; k < WL_MIX_MAX_VOICES; k++) {
        WlMixVoice *v = &m->voices[k];
        if (!v->active) {
            continue;
        }
        if (v->bus == WL_BUS_SPEECH) {
            speech_playing = true;
        }
        // Advance the fade across this block.
        const int32_t f0 = v->fade;
        int32_t f1 = f0 + v->fade_step * (int32_t)n;
        if (v->fade_step > 0) {
            f1 = f1 > v->fade_goal ? v->fade_goal : f1;
        } else if (v->fade_step < 0) {
            f1 = f1 < v->fade_goal ? v->fade_goal : f1;
        }
        v->fade = f1;
        int32_t gl_end, gr_end;
        voice_targets(v, f1 >> 8, &gl_end, &gr_end);
        int32_t gl_start = v->gl, gr_start = v->gr;
        if (gl_start < 0) { // first block: begin at the fade's own starting level
            int32_t a, b;
            voice_targets(v, f0 >> 8, &a, &b);
            gl_start = a;
            gr_start = b;
        }
        const int32_t dl = (int32_t)(((int64_t)(gl_end - gl_start) * 256) / (int64_t)n);
        const int32_t dr = (int32_t)(((int64_t)(gr_end - gr_start) * 256) / (int64_t)n);
        if (!(bus_used & (1u << v->bus))) {
            memset(m->acc[v->bus], 0, sizeof m->acc[v->bus]);
            bus_used |= 1u << v->bus;
        }
        int32_t *al = m->acc[v->bus][0];
        int32_t *ar = m->acc[v->bus][1];
        if (v->is_pull) {
            mix_pull_voice(m, v, al, ar, n, gl_start << 8, dl, gr_start << 8, dr);
        } else {
            mix_sample_voice(v, al, ar, n, gl_start << 8, dl, gr_start << 8, dr);
        }
        v->gl = gl_end;
        v->gr = gr_end;
        if (v->pull_ended || (v->stopping && f1 == v->fade_goal && v->fade_goal == 0)) {
            v->active = false;
        }
    }

    // Buses: effective gain = bus * master (* duck for music), ramped over the block.
    uint32_t duck_q8 = m->duck_explicit;
    if (speech_playing && m->speech_duck < duck_q8) {
        duck_q8 = m->speech_duck;
    }
    const int32_t duck_target = (int32_t)duck_q8 * 64;
    int32_t duck_next = duck_target;
    if (duck_target < m->duck_cur) { // duck fast: a quarter of the range per block
        duck_next = clamp_i32(m->duck_cur - G_ONE / 4, duck_target, G_ONE);
    } else if (duck_target > m->duck_cur) { // recover slowly: 1/32 of the range per block
        duck_next = clamp_i32(m->duck_cur + G_ONE / 32, 0, duck_target);
    }
    int32_t sum_l[WL_MIX_BLOCK];
    int32_t sum_r[WL_MIX_BLOCK];
    memset(sum_l, 0, sizeof(int32_t) * n);
    memset(sum_r, 0, sizeof(int32_t) * n);
    for (uint32_t b = 0; b < WL_BUS_COUNT; b++) {
        int32_t g1 = (int32_t)(((int64_t)m->bus_gain[b] * 64 * m->master) >> 8);
        if (b == WL_BUS_MUSIC) {
            g1 = (int32_t)(((int64_t)g1 * duck_next) >> 14);
        }
        const int32_t g0 = m->bus_prev[b];
        if (!(bus_used & (1u << b))) { // silent bus: nothing to scale, only the gain state moves on
            m->bus_prev[b] = g1;
            continue;
        }
        const int32_t d = (int32_t)(((int64_t)(g1 - g0) * 256) / (int64_t)n);
        int32_t g22 = g0 << 8;
        const int32_t *al = m->acc[b][0];
        const int32_t *ar = m->acc[b][1];
        for (uint32_t i = 0; i < n; i++) {
            g22 += d;
            const int64_t g = g22 >> 8;
            sum_l[i] += (int32_t)(((int64_t)al[i] * g) >> 14);
            sum_r[i] += (int32_t)(((int64_t)ar[i] * g) >> 14);
        }
        m->bus_prev[b] = g1;
    }
    m->duck_cur = duck_next;

    uint32_t clipped = 0;
    uint32_t peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = sum_l[i];
        int32_t r = sum_r[i];
        if (l > 32767 || l < -32768) {
            clipped++;
            l = l > 0 ? 32767 : -32768;
        }
        if (r > 32767 || r < -32768) {
            clipped++;
            r = r > 0 ? 32767 : -32768;
        }
        const uint32_t al = (uint32_t)(l < 0 ? -l : l);
        const uint32_t ar = (uint32_t)(r < 0 ? -r : r);
        peak = al > peak ? al : peak;
        peak = ar > peak ? ar : peak;
        out[2 * i] = (int16_t)l;
        out[2 * i + 1] = (int16_t)r;
    }
    uint32_t active = 0;
    for (uint32_t k = 0; k < WL_MIX_MAX_VOICES; k++) {
        active += m->voices[k].active ? 1u : 0u;
    }
    m->stats.blocks++;
    m->stats.frames += n;
    m->stats.clipped += clipped;
    m->stats.active = active;
    m->stats.peak = peak;
}
