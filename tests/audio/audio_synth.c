// SPDX-License-Identifier: MIT
#include "tests/audio/audio_synth.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/base/hash.h"
#include "tests/audio/dsp_fixture.h"

// ---------------------------------------------------------------------------------------------
// Melody
// ---------------------------------------------------------------------------------------------

static int16_t g_tab[65];
static bool g_tab_ready;

static void tab_init(void) {
    if (!g_tab_ready) {
        fx_sine_pcm(g_tab, 64); // 64-sample period, amplitude 12000
        g_tab[64] = g_tab[0];
        g_tab_ready = true;
    }
}

static int32_t sine(uint32_t phase) { // phase: full circle = 2^32
    const uint32_t i = phase >> 26;
    const int32_t f = (int32_t)((phase >> 10) & 0xFFFFu);
    return g_tab[i] + (((g_tab[i + 1] - g_tab[i]) * f) >> 16);
}

static uint32_t lcg(uint32_t x) {
    return x * 1664525u + 1013904223u;
}

// Phase increments (2^32 * f / 32000) for a C-major pentatonic over two octaves, indexed 0..9.
static const uint32_t k_note_hz[10] = {262, 294, 330, 392, 440, 524, 588, 660, 784, 880};

void fxa_tone(int16_t *out, uint32_t n, uint32_t rate, uint32_t melody, uint32_t channel) {
    tab_init();
    const uint32_t note_len = rate / 4u;
    uint32_t phase = 0;
    uint32_t cur = 0xFFFFFFFFu;
    uint32_t note = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t k = i / note_len;
        if (k != cur) {
            cur = k;
            note = lcg(lcg(melody * 7919u + k) ^ (channel * 0x9E3779B9u)) >> 16;
            note = note % 10u;
        }
        const uint32_t pos = i - k * note_len;
        const uint64_t inc = ((uint64_t)k_note_hz[note] << 32) / rate;
        phase += (uint32_t)inc;
        int32_t s = sine(phase) + sine(phase * 2u) / 2;
        // Envelope: 5 ms attack, linear decay to 40 %.
        int32_t env = 1000 - (int32_t)(600u * pos / note_len);
        const uint32_t atk = rate / 200u;
        if (pos < atk) {
            env = (int32_t)((uint32_t)env * pos / atk);
        }
        out[i] = (int16_t)(s * env / 1000 * 3 / 4);
    }
}

// ---------------------------------------------------------------------------------------------
// PWST and PWSF
// ---------------------------------------------------------------------------------------------

static void put_channel(uint8_t *b, const uint8_t *adpcm, const int16_t *ref, uint32_t loop_start, bool loop) {
    for (int i = 0; i < 16; i++) {
        wl_wr_be16(b + 2 * i, (uint16_t)FX_DSP_COEF[i]);
    }
    wl_wr_be16(b + 0x20, 0);             // gain
    wl_wr_be16(b + 0x22, adpcm[0]);      // initial ps
    wl_wr_be16(b + 0x24, 0);             // hist1
    wl_wr_be16(b + 0x26, 0);             // hist2
    if (loop) {
        wl_wr_be16(b + 0x28, adpcm[loop_start / 14u * 8u]);
        wl_wr_be16(b + 0x2A, loop_start >= 1 ? (uint16_t)ref[loop_start - 1] : 0);
        wl_wr_be16(b + 0x2C, loop_start >= 2 ? (uint16_t)ref[loop_start - 2] : 0);
    }
}

WlResult fxa_build_pwst(const FxAStreamSpec *sp, uint8_t **out, size_t *size, int16_t *ref[2]) {
    const uint32_t n = sp->samples;
    const uint32_t frames = n / 14u;
    const uint32_t nch = sp->channels;
    if (n == 0 || n % 14u != 0 || nch < 1 || nch > 2) {
        return WL_ERR_ARG;
    }
    const uint32_t blocks_end = 0x30u + 48u * nch;
    const uint32_t data_off = (blocks_end + 31u) & ~31u;
    const uint32_t chan_bytes = frames * 8u;
    const size_t total = ((size_t)data_off + (size_t)chan_bytes * nch + 31u) & ~(size_t)31u;
    uint8_t *buf = calloc(1, total);
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    // Without `ref` one scratch buffer serves both channels (the console has little malloc room).
    int16_t *dec[2] = {malloc(sizeof(int16_t) * n), (nch > 1 && ref != NULL) ? malloc(sizeof(int16_t) * n) : NULL};
    if (buf == NULL || pcm == NULL || dec[0] == NULL || (nch > 1 && ref != NULL && dec[1] == NULL)) {
        free(buf);
        free(pcm);
        free(dec[0]);
        free(dec[1]);
        return WL_ERR_NOMEM;
    }
    memcpy(buf, "PWST", 4);
    wl_wr_be16(buf + 4, 1);
    uint16_t flags = 0;
    if (sp->loop) {
        flags |= 1u;
        if (sp->loop_start % 14u != 0) {
            flags |= 2u;
        }
    }
    wl_wr_be16(buf + 6, flags);
    buf[8] = (uint8_t)nch;
    buf[9] = 0;
    wl_wr_be32(buf + 0xC, sp->rate);
    wl_wr_be32(buf + 0x10, n);
    wl_wr_be32(buf + 0x14, sp->loop ? sp->loop_start : 0);
    wl_wr_be32(buf + 0x18, sp->loop ? sp->loop_end : 0);
    wl_wr_be32(buf + 0x1C, data_off);
    wl_wr_be32(buf + 0x20, chan_bytes * nch);
    for (uint32_t ch = 0; ch < nch; ch++) {
        fxa_tone(pcm, n, sp->rate, sp->melody, ch);
        uint8_t *adpcm = buf + data_off + (size_t)ch * chan_bytes;
        fx_dsp_encode(pcm, n, FX_DSP_COEF, adpcm);
        int16_t hist[2] = {0, 0};
        int16_t *d = dec[ch] != NULL ? dec[ch] : dec[0];
        fx_dsp_decode(adpcm, frames, FX_DSP_COEF, d, hist);
        put_channel(buf + 0x30 + 48u * ch, adpcm, d, sp->loop_start, sp->loop);
    }
    free(pcm);
    if (ref != NULL) {
        ref[0] = dec[0];
        ref[1] = dec[1];
    } else {
        free(dec[0]);
        free(dec[1]);
    }
    *out = buf;
    *size = total;
    return WL_OK;
}

WlResult fxa_build_pwsf(const FxASfxSpec *sp, uint8_t **out, size_t *size, int16_t **ref) {
    const uint32_t n = sp->samples;
    if (n == 0 || (sp->codec == 0 && n % 14u != 0)) {
        return WL_ERR_ARG;
    }
    const uint32_t data_size = sp->codec == 0 ? n / 14u * 8u : n * 2u;
    const size_t total = ((size_t)0x80 + data_size + 31u) & ~(size_t)31u;
    uint8_t *buf = calloc(1, total);
    int16_t *pcm = malloc(sizeof(int16_t) * n);
    int16_t *dec = malloc(sizeof(int16_t) * n);
    if (buf == NULL || pcm == NULL || dec == NULL) {
        free(buf);
        free(pcm);
        free(dec);
        return WL_ERR_NOMEM;
    }
    fxa_tone(pcm, n, sp->rate, sp->melody, 0);
    memcpy(buf, "PWSF", 4);
    wl_wr_be16(buf + 4, 1);
    wl_wr_be16(buf + 6, sp->loop ? 1u : 0u);
    buf[8] = 1;
    buf[9] = sp->codec;
    buf[0xA] = sp->bus;
    buf[0xB] = sp->priority;
    wl_wr_be32(buf + 0xC, sp->rate);
    wl_wr_be32(buf + 0x10, n);
    wl_wr_be32(buf + 0x14, sp->loop ? sp->loop_start : 0);
    wl_wr_be32(buf + 0x18, sp->loop ? sp->loop_end : 0);
    wl_wr_be32(buf + 0x1C, 0x80);
    wl_wr_be32(buf + 0x20, data_size);
    wl_wr_be16(buf + 0x24, (uint16_t)sp->pitch_cents);
    wl_wr_be16(buf + 0x26, sp->volume);
    buf[0x28] = (uint8_t)sp->pan;
    wl_wr_be16(buf + 0x2A, sp->play_ms);
    wl_wr_be32(buf + 0x2C, (uint32_t)((uint64_t)n * 1000u / sp->rate));
    if (sp->codec == 0) {
        uint8_t *adpcm = buf + 0x80;
        fx_dsp_encode(pcm, n, FX_DSP_COEF, adpcm);
        int16_t hist[2] = {0, 0};
        fx_dsp_decode(adpcm, n / 14u, FX_DSP_COEF, dec, hist);
        put_channel(buf + 0x40, adpcm, dec, 0, false);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            wl_wr_be16(buf + 0x80 + 2u * i, (uint16_t)pcm[i]);
            dec[i] = pcm[i];
        }
    }
    free(pcm);
    if (ref != NULL) {
        *ref = dec;
    } else {
        free(dec);
    }
    *out = buf;
    *size = total;
    return WL_OK;
}
