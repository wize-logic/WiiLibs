// SPDX-License-Identifier: MIT
// RWSD wave sounds: a synthetic RWSD with two waves (a plain one and a looping one that starts
// mid-frame), built here (no game data), resolves to the right frames, DSP state and playback
// parameters, and its frames decode like an independent reference decoder; malformed and randomly
// corrupted RWSDs fail cleanly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/base/endian.h"
#include "wl/brstm/dsp.h"
#include "wl/brstm/rwsd.h"

static uint32_t g_seed = 24681357u;
static uint32_t rnd(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

/* Independent reference decoder (one sample at a time). */
static void ref_decode(const uint8_t *fr, size_t nframes, const int16_t *coef, int16_t *out) {
    int h1 = 0, h2 = 0;
    for (size_t f = 0; f < nframes; f++) {
        int pi = fr[f * 8] >> 4, sc = fr[f * 8] & 15;
        for (size_t k = 0; k < 14; k++) {
            int b = fr[f * 8 + 1 + k / 2];
            int nib = (k % 2 == 0) ? b >> 4 : b & 15;
            nib = nib >= 8 ? nib - 16 : nib;
            long v = ((long)nib * (2048L << sc) + 1024 + (long)coef[(pi & 7) * 2] * h1 + (long)coef[(pi & 7) * 2 + 1] * h2) >> 11;
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            out[f * 14 + k] = (int16_t)v;
            h2 = h1;
            h1 = (int)v;
        }
    }
}

typedef struct {
    uint8_t rwsd[0x400];
    size_t rwsd_len;
    uint8_t *wave;
    size_t wave_len;
    int16_t coef[2][16];
    uint32_t frames[2], dl[2];
} Syn;

static void f32(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    wl_wr_be32(p, u);
}
static void ref(uint8_t *p, uint32_t off) {
    wl_wr_be32(p, 0x01000000u);
    wl_wr_be32(p + 4, off);
}

/* Two wave sounds: wsd 0 -> wave 0 (plain, 9 frames, last frame 10 samples; pitch 2.0 x 1.0, pan 80 + 64), wsd 1 ->
 * wave 1 (loops from nibble 2 + 16*3 + 5, 4 frames; pitch 1.0). */
static void make_syn(Syn *s) {
    memset(s, 0, sizeof *s);
    uint8_t *r = s->rwsd;
    memcpy(r, "RWSD", 4);
    wl_wr_be16(r + 4, 0xFEFF);
    wl_wr_be16(r + 6, 0x0102);
    wl_wr_be16(r + 0x0C, 0x20);
    wl_wr_be16(r + 0x0E, 2);
    const uint32_t doff = 0x20, dsz = 0x160, woff = doff + dsz, wsz = 0x120;
    wl_wr_be32(r + 0x10, doff);
    wl_wr_be32(r + 0x14, dsz);
    wl_wr_be32(r + 0x18, woff);
    wl_wr_be32(r + 0x1C, wsz);
    s->rwsd_len = woff + wsz;
    wl_wr_be32(r + 8, (uint32_t)s->rwsd_len);
    /* DATA payload offsets (from doff + 8): count, two refs at 4 and 12; entries at 0x20 and 0x40; infos 0x60/0x80;
     * note tables 0xA0/0xC0 (count, ref); note infos 0xE0/0x110 */
    uint8_t *D = r + doff;
    memcpy(D, "DATA", 4);
    wl_wr_be32(D + 4, dsz);
    uint8_t *P = D + 8;
    wl_wr_be32(P, 2);
    ref(P + 4, 0x20);
    ref(P + 12, 0x40);
    for (int i = 0; i < 2; i++) {
        uint8_t *e = P + 0x20 + 0x20 * i;
        ref(e + 0, 0x60 + 0x20 * (uint32_t)i);
        ref(e + 8, 0x60 + 0x20 * (uint32_t)i); /* track table: not read */
        ref(e + 16, 0xA0 + 0x20 * (uint32_t)i);
        uint8_t *nt = P + 0xA0 + 0x20 * i;
        wl_wr_be32(nt, 1);
        ref(nt + 4, 0xE0 + 0x30 * (uint32_t)i);
        uint8_t *ni = P + 0xE0 + 0x30 * i;
        wl_wr_be32(ni, (uint32_t)i);
        memset(ni + 4, 0x7f, 4);
        ni[0x0C] = 60;
        ni[0x0D] = 127;
        ni[0x0E] = 64;
        f32(ni + 0x10, 1.0f);
        uint8_t *in = P + 0x60 + 0x20 * i;
        f32(in, i == 0 ? 2.0f : 1.0f);
        in[4] = i == 0 ? 80 : 64;
    }
    /* the second note carries pan 64 and volume 100, the first pan 64 */
    P[0xE0 + 0x30 + 0x0D] = 100;
    /* WAVE: count, offsets from the chunk start, wave infos of 0x6c bytes at 0x18 and 0x84 */
    uint8_t *W = r + woff;
    memcpy(W, "WAVE", 4);
    wl_wr_be32(W + 4, wsz);
    wl_wr_be32(W + 8, 2);
    wl_wr_be32(W + 12, 0x18);
    wl_wr_be32(W + 16, 0x84);
    const uint32_t frames[2] = {9, 4};
    uint32_t dl = 0;
    for (int i = 0; i < 2; i++) {
        uint8_t *wi = W + 0x18 + 0x6c * i;
        s->frames[i] = frames[i];
        s->dl[i] = dl;
        wi[0] = 2;
        wi[1] = (uint8_t)i; /* loop */
        wi[2] = 1;
        wi[3] = 0;
        wl_wr_be16(wi + 4, i == 0 ? 32000 : 22050);
        wl_wr_be32(wi + 8, i == 0 ? 2u : 2u + 16u * 3u + 5u);
        wl_wr_be32(wi + 0x0C, (frames[i] - 1u) * 16u + (i == 0 ? 11u : 15u)); /* last sample: 10 in the frame or the 14th */
        wl_wr_be32(wi + 0x10, 0x1C);
        wl_wr_be32(wi + 0x14, dl);
        wl_wr_be32(wi + 0x1C, 0x20);
        wl_wr_be32(wi + 0x24, 0x3C);
        for (int k = 0; k < 4; k++) {
            wl_wr_be32(wi + 0x28 + 4 * k, 0x01000000u);
        }
        uint8_t *ai = wi + 0x3C;
        for (int k = 0; k < 16; k++) {
            s->coef[i][k] = (int16_t)((int)(rnd() % 4096) - 2048);
            wl_wr_be16(ai + 2 * k, (uint16_t)s->coef[i][k]);
        }
        dl += (frames[i] * 8u + 31u) & ~31u;
    }
    s->wave_len = dl;
    s->wave = (uint8_t *)calloc(1, dl);
    for (int i = 0; i < 2; i++) {
        for (uint32_t f = 0; f < s->frames[i]; f++) {
            uint8_t *fr = s->wave + s->dl[i] + 8u * f;
            fr[0] = (uint8_t)(((rnd() % 8) << 4) | (rnd() % 9));
            for (int k = 1; k < 8; k++) {
                fr[k] = (uint8_t)rnd();
            }
        }
        uint8_t *ai = r + woff + 0x18 + 0x6C * i + 0x3C;
        wl_wr_be16(ai + 0x22, s->wave[s->dl[i]]);
        wl_wr_be16(ai + 0x28, s->wave[s->dl[i] + 3u * 8u]); /* loop frame header (frame 3) */
    }
}

static void free_syn(Syn *s) { free(s->wave); }

TEST wave_plain(void) {
    Syn s;
    make_syn(&s);
    WlRwsdWave o;
    ASSERT_EQ(WL_OK, wl_rwsd_wave_sound(s.rwsd, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    ASSERT_EQ(0u, o.wsd);
    ASSERT_EQ(0u, o.wave);
    ASSERT_EQ(32000u, o.rate);
    ASSERT_EQ(8u * 14u + 10u, o.samples);
    ASSERT(!o.loop);
    ASSERT_EQ(0u, o.loop_end);
    ASSERT_EQ(1200, o.pitch_cents); /* a pitch factor of 2.0 is one octave */
    ASSERT(o.pitch == 2.0);
    /* pan: (80 - 64) + (64 - 64) = 16 -> 16 * 127 / 64 = 31.75 -> 32 */
    ASSERT_EQ(32, o.pan);
    ASSERT_EQ(80u, o.info_pan);
    ASSERT_EQ(127u, o.note_volume);
    ASSERT_EQ(60u, o.note_key);
    ASSERT_EQ((120u * 127u * 256u + 8064u) / 16129u, wl_rwsd_volume(120, o.note_volume));
    ASSERT_EQ(4u, o.duration_ms); /* 122 samples at 32 kHz */
    ASSERT_EQ(9u, o.frame_count);
    ASSERT_EQ(72u, (unsigned)o.frames_size);
    ASSERT(o.frames == s.wave + s.dl[0]);
    ASSERT_MEM_EQ(s.coef[0], o.coef, sizeof o.coef);
    ASSERT_EQ(s.wave[s.dl[0]], o.init_ps);
    ASSERT(o.adpcm_info == s.rwsd + 0x20 + 0x160 + 0x18 + 0x3C);
    /* the frames decode like the reference */
    int16_t pcm[9 * 14], ref_out[9 * 14];
    int16_t h1 = o.init_hist[0], h2 = o.init_hist[1];
    wl_brstm_dsp_decode(o.frames, o.frame_count, o.coef, &h1, &h2, pcm);
    ref_decode(s.wave, 9, s.coef[0], ref_out);
    ASSERT_MEM_EQ(ref_out, pcm, (size_t)o.samples * sizeof(int16_t));
    free_syn(&s);
    PASS();
}

TEST wave_loop_mid_frame(void) {
    Syn s;
    make_syn(&s);
    WlRwsdWave o;
    ASSERT_EQ(WL_OK, wl_rwsd_wave_sound(s.rwsd, s.rwsd_len, 1, s.wave, s.wave_len, &o));
    ASSERT_EQ(22050u, o.rate);
    ASSERT_EQ(4u * 14u, o.samples);
    ASSERT(o.loop);
    ASSERT(o.loop_mid);
    ASSERT_EQ(3u * 14u + 5u, o.loop_start);
    ASSERT_EQ(4u * 14u - 1u, o.loop_end);
    ASSERT_EQ(0, o.pitch_cents);
    ASSERT_EQ(0, o.pan);
    ASSERT_EQ(100u, o.note_volume);
    ASSERT_EQ((127u * 100u * 256u + 8064u) / 16129u, wl_rwsd_volume(127, o.note_volume));
    ASSERT_EQ(256u, wl_rwsd_volume(127, 127));
    ASSERT_EQ(256u, wl_rwsd_volume(255, 200)); /* clamped */
    ASSERT_EQ(0u, wl_rwsd_volume(0, 127));
    ASSERT_EQ(s.wave[s.dl[1] + 3 * 8], o.loop_ps); /* the loop frame's header */
    ASSERT(o.frames == s.wave + s.dl[1]);
    ASSERT_EQ(4u, o.frame_count);
    free_syn(&s);
    PASS();
}

TEST reject_bad(void) {
    Syn s;
    make_syn(&s);
    WlRwsdWave o;
    ASSERT_EQ(WL_ERR_ARG, wl_rwsd_wave_sound(NULL, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    ASSERT_EQ(WL_ERR_RANGE, wl_rwsd_wave_sound(s.rwsd, s.rwsd_len, 2, s.wave, s.wave_len, &o));
    ASSERT_EQ(WL_ERR_RANGE, wl_rwsd_wave_sound(s.rwsd, s.rwsd_len, 0, s.wave, 40, &o)); /* wave data too short */
    ASSERT_EQ(WL_ERR_RANGE, wl_rwsd_wave_sound(s.rwsd, s.rwsd_len, 0, NULL, 0, &o));
    ASSERT_EQ(WL_ERR_FORMAT, wl_rwsd_wave_sound(s.rwsd, 0x10, 0, s.wave, s.wave_len, &o));
    uint8_t m[sizeof s.rwsd];
    memcpy(m, s.rwsd, sizeof m);
    m[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_rwsd_wave_sound(m, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    /* not DSP-ADPCM (PCM16) */
    memcpy(m, s.rwsd, sizeof m);
    m[0x20 + 0x160 + 0x18] = 1;
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_rwsd_wave_sound(m, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    /* stereo */
    memcpy(m, s.rwsd, sizeof m);
    m[0x20 + 0x160 + 0x18 + 2] = 2;
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_rwsd_wave_sound(m, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    /* a sample count whose end nibble is inside the frame header */
    memcpy(m, s.rwsd, sizeof m);
    wl_wr_be32(m + 0x20 + 0x160 + 0x18 + 0x0C, 8u * 16u + 1u);
    ASSERT_EQ(WL_ERR_FORMAT, wl_rwsd_wave_sound(m, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    /* a non-positive pitch */
    memcpy(m, s.rwsd, sizeof m);
    f32(m + 0x20 + 8 + 0x60, 0.0f);
    ASSERT_EQ(WL_ERR_FORMAT, wl_rwsd_wave_sound(m, s.rwsd_len, 0, s.wave, s.wave_len, &o));
    /* a loop start past the end */
    memcpy(m, s.rwsd, sizeof m);
    wl_wr_be32(m + 0x20 + 0x160 + 0x84 + 8, 100u * 16u + 2u);
    ASSERT_EQ(WL_ERR_FORMAT, wl_rwsd_wave_sound(m, s.rwsd_len, 1, s.wave, s.wave_len, &o));
    free_syn(&s);
    PASS();
}

TEST fuzz_rwsd(void) {
    Syn s;
    make_syn(&s);
    uint8_t m[sizeof s.rwsd];
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * 14u * 64u);
    ASSERT(pcm != NULL);
    for (int it = 0; it < 20000; it++) {
        memcpy(m, s.rwsd, sizeof m);
        int flips = 1 + (int)(rnd() % 4);
        for (int k = 0; k < flips; k++) {
            size_t at = it % 3 ? rnd() % s.rwsd_len : rnd() % 0x80;
            m[at] = (uint8_t)rnd();
        }
        size_t len = it % 7 == 0 ? rnd() % (s.rwsd_len + 1) : s.rwsd_len;
        WlRwsdWave o;
        if (wl_rwsd_wave_sound(m, len, (uint32_t)(it & 1), s.wave, s.wave_len, &o) == WL_OK) {
            ASSERT(o.samples > 0 && o.samples <= (1u << 24));
            ASSERT(o.frames >= s.wave && o.frames + o.frames_size <= s.wave + s.wave_len);
            ASSERT(o.adpcm_info >= m && o.adpcm_info + WL_RWSD_ADPCM_INFO <= m + len);
            ASSERT(o.pan >= -127 && o.pan <= 127);
            ASSERT(!o.loop || o.loop_start < o.samples);
            if (o.frame_count <= 64u) {
                int16_t h1 = 0, h2 = 0;
                wl_brstm_dsp_decode(o.frames, o.frame_count, o.coef, &h1, &h2, pcm);
            }
        }
    }
    free(pcm);
    free_syn(&s);
    PASS();
}

SUITE(rwsd_suite) {
    RUN_TEST(wave_plain);
    RUN_TEST(wave_loop_mid_frame);
    RUN_TEST(reject_bad);
    RUN_TEST(fuzz_rwsd);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(rwsd_suite);
    GREATEST_MAIN_END();
}
