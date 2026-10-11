// SPDX-License-Identifier: MIT
// RWSD wave sound resolution. Layout notes: README.md.
#include "wl/brstm/rwsd.h"

#include <math.h>
#include <string.h>

#include "wl/base/endian.h"

#define MAX_RWSD (1u << 20)
#define MAX_SAMPLES (1u << 24)

/* A reference: first byte 1 (an offset), offset in the second word. */
static int ref_off(const uint8_t *d, size_t size, size_t pos, uint32_t *off) {
    if (pos > size || size - pos < 8u || d[pos] != 1u) {
        return 0;
    }
    *off = wl_rd_be32(d + pos + 4);
    return 1;
}

static float rd_f32(const uint8_t *p) {
    uint32_t u = wl_rd_be32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static int32_t cents_of(double p) {
    if (p == 1.0) {
        return 0;
    }
    double c = floor(1200.0 * log2(p) + 0.5);
    return (int32_t)(c < -32767.0 ? -32767.0 : (c > 32767.0 ? 32767.0 : c));
}

static uint32_t clamp127(uint32_t v) {
    return v > 127u ? 127u : v;
}

uint32_t wl_rwsd_volume(uint8_t sound_volume, uint8_t note_volume) {
    return (clamp127(sound_volume) * clamp127(note_volume) * 256u + 8064u) / 16129u;
}

WlResult wl_rwsd_wave_sound(const uint8_t *r, size_t rn, uint32_t wsd, const uint8_t *wave, size_t wn,
                            WlRwsdWave *o) {
    if (r == NULL || o == NULL) {
        return WL_ERR_ARG;
    }
    if (rn < 0x20 || rn > MAX_RWSD || memcmp(r, "RWSD", 4) != 0 || wl_rd_be16(r + 4) != 0xFEFFu ||
        r[6] != 1u || wl_rd_be16(r + 0x0C) != 0x20u || wl_rd_be16(r + 0x0E) != 2u || wl_rd_be32(r + 8) > rn) {
        return WL_ERR_FORMAT;
    }
    uint32_t doff = wl_rd_be32(r + 0x10), dsz = wl_rd_be32(r + 0x14), woff = wl_rd_be32(r + 0x18),
             wsz = wl_rd_be32(r + 0x1C);
    if (dsz < 0x10u || wsz < 0x10u || !wl_range_ok(doff, dsz, rn) || !wl_range_ok(woff, wsz, rn) ||
        memcmp(r + doff, "DATA", 4) != 0 || wl_rd_be32(r + doff + 4) != dsz || memcmp(r + woff, "WAVE", 4) != 0 ||
        wl_rd_be32(r + woff + 4) != wsz) {
        return WL_ERR_FORMAT;
    }
    /* DATA: offsets are from the start of its payload */
    const uint8_t *D = r + doff + 8;
    size_t dn = dsz - 8u;
    uint32_t cnt = wl_rd_be32(D), off;
    if (wsd >= cnt || (dn - 4u) / 8u < cnt || !ref_off(D, dn, 4u + 8u * (size_t)wsd, &off) ||
        !wl_range_ok(off, 24, dn)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *e = D + off;
    uint32_t info_off, note_tab;
    if (!ref_off(e, 24, 0, &info_off) || !ref_off(e, 24, 16, &note_tab) || !wl_range_ok(info_off, 5, dn) ||
        !wl_range_ok(note_tab, 12, dn)) {
        return WL_ERR_FORMAT;
    }
    const uint8_t *info = D + info_off;
    uint32_t notes = wl_rd_be32(D + note_tab), note_off;
    if (notes < 1u || !ref_off(D + note_tab, dn - note_tab, 4, &note_off) || !wl_range_ok(note_off, 0x14, dn)) {
        return WL_ERR_FORMAT;
    }
    const uint8_t *note = D + note_off;
    int32_t widx = (int32_t)wl_rd_be32(note);
    double pitch = (double)rd_f32(info) * (double)rd_f32(note + 0x10);
    if (!(pitch > 1e-6 && pitch < 1e6)) {
        return WL_ERR_FORMAT;
    }
    /* WAVE: offsets are from the start of the chunk */
    const uint8_t *W = r + woff;
    uint32_t wcnt = wl_rd_be32(W + 8), wio;
    if (widx < 0 || (uint32_t)widx >= wcnt || (wsz - 12u) / 4u < wcnt) {
        return WL_ERR_RANGE;
    }
    wio = wl_rd_be32(W + 12 + 4u * (uint32_t)widx);
    if (!wl_range_ok(wio, 0x18, wsz)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *wi = W + wio;
    size_t wavail = wsz - wio;
    if (wi[0] != 2u || wi[2] != 1u || wi[6] != 0u) { /* DSP-ADPCM, mono, data at an offset */
        return WL_ERR_UNSUPPORTED;
    }
    uint32_t rate = ((uint32_t)wi[3] << 16) | wl_rd_be16(wi + 4), ls = wl_rd_be32(wi + 8),
             le = wl_rd_be32(wi + 0x0C), chtab = wl_rd_be32(wi + 0x10), dl = wl_rd_be32(wi + 0x14);
    if (rate == 0u || rate > 96000u || !wl_range_ok(chtab, 4, wavail)) {
        return WL_ERR_FORMAT;
    }
    uint32_t ci = wl_rd_be32(wi + chtab), ai;
    if (!wl_range_ok(ci, 8, wavail)) {
        return WL_ERR_RANGE;
    }
    ai = wl_rd_be32(wi + ci + 4);
    if (!wl_range_ok(ai, WL_RWSD_ADPCM_INFO, wavail)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *adpcm = wi + ai;
    /* nibble addresses: frame * 16 + 2 + sample in the frame; the end is the last sample */
    uint32_t ef = le >> 4, en = le & 15u;
    if (en < 2u || ef >= MAX_SAMPLES / 14u) {
        return WL_ERR_FORMAT;
    }
    uint32_t samples = ef * 14u + en - 1u, frames = ef + 1u;
    size_t dbytes = (size_t)frames * 8u;
    if (wave == NULL || !wl_range_ok(dl, dbytes, wn)) {
        return WL_ERR_RANGE;
    }
    int loop = wi[1] != 0u, loop_mid = 0;
    uint32_t lstart = 0;
    if (loop) {
        uint32_t lf = ls >> 4, ln = ls & 15u;
        if (ln < 2u || lf > ef) {
            return WL_ERR_FORMAT;
        }
        lstart = lf * 14u + ln - 2u;
        if (lstart >= samples) {
            return WL_ERR_FORMAT;
        }
        loop_mid = ln != 2u;
    }
    int32_t pan_sum = ((int32_t)clamp127(info[4]) - 64) + ((int32_t)clamp127(note[0x0E]) - 64);
    int32_t pan = (pan_sum * 127 + (pan_sum >= 0 ? 32 : -32)) / 64;

    memset(o, 0, sizeof *o);
    o->wsd = wsd;
    o->wave = (uint32_t)widx;
    o->rate = rate;
    o->samples = samples;
    o->loop = loop;
    o->loop_mid = loop_mid;
    o->loop_start = loop ? lstart : 0u;
    o->loop_end = loop ? samples - 1u : 0u;
    o->pitch = pitch;
    o->pitch_cents = cents_of(pitch);
    o->info_pan = info[4];
    o->note_pan = note[0x0E];
    o->pan = pan < -127 ? -127 : (pan > 127 ? 127 : pan);
    o->note_volume = note[0x0D];
    o->note_key = note[0x0C];
    o->duration_ms = (uint32_t)(((uint64_t)samples * 1000u + rate / 2u) / rate);
    for (int k = 0; k < 16; k++) {
        o->coef[k] = (int16_t)wl_rd_be16(adpcm + 2 * k);
    }
    o->init_ps = wl_rd_be16(adpcm + 0x22);
    o->init_hist[0] = (int16_t)wl_rd_be16(adpcm + 0x24);
    o->init_hist[1] = (int16_t)wl_rd_be16(adpcm + 0x26);
    o->loop_ps = wl_rd_be16(adpcm + 0x28);
    o->loop_hist[0] = (int16_t)wl_rd_be16(adpcm + 0x2A);
    o->loop_hist[1] = (int16_t)wl_rd_be16(adpcm + 0x2C);
    o->adpcm_info = adpcm;
    o->frames = wave + dl;
    o->frame_count = frames;
    o->frames_size = dbytes;
    return WL_OK;
}
