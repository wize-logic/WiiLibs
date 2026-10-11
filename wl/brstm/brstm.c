// SPDX-License-Identifier: MIT
// BRSTM (NW4R "RSTM") parsing, de-interleaving and decoding. Layout notes: README.md. No code from
// any decomp or tool.
#include "wl/brstm/brstm.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

/* RSTM file header (big-endian). */
#define F_BOM 4
#define F_SIZE 8
#define F_HDRSIZE 0x0C
#define F_CHUNKS 0x0E
#define F_HEAD_OFF 0x10
#define F_HEAD_SIZE 0x14
#define F_ADPC_OFF 0x18
#define F_ADPC_SIZE 0x1C
#define F_DATA_OFF 0x20
#define F_DATA_SIZE 0x24
#define RSTM_HDR 0x40u

/* Stream info (HEAD part 1), from its start. */
#define S_CODEC 0x00
#define S_LOOP 0x01
#define S_CHANNELS 0x02
#define S_RATE 0x04
#define S_LOOP_START 0x08
#define S_SAMPLES 0x0C
#define S_DATA_OFF 0x10
#define S_BLOCKS 0x14
#define S_BLOCK_SIZE 0x18
#define S_BLOCK_SAMPLES 0x1C
#define S_LAST_SIZE 0x20
#define S_LAST_SAMPLES 0x24
#define S_LAST_PADDED 0x28
#define S_ADPC_INTERVAL 0x2C
#define S_ADPC_ENTRY 0x30
#define S_INFO_SIZE 0x34

#define CODEC_DSP 2u

/* Where a reference inside HEAD points (references are 8 bytes: a marker word 0x01000000 and an offset
 * from the start of the HEAD payload, which is 8 bytes after the chunk start). need bytes must fit. */
typedef struct {
    const uint8_t *f;
    size_t base, end; /* payload start, chunk end (absolute) */
} Head;

static int head_ref(const Head *h, size_t pos, size_t need, size_t *target) {
    if (pos > h->end || h->end - pos < 8u || wl_rd_be32(h->f + pos) != 0x01000000u) {
        return 0;
    }
    uint64_t t = (uint64_t)h->base + wl_rd_be32(h->f + pos + 4);
    if (t > h->end || h->end - t < need) {
        return 0;
    }
    *target = (size_t)t;
    return 1;
}

static uint32_t last_bytes(uint32_t last_samples) {
    uint32_t whole = last_samples / 14u, rest = last_samples % 14u;
    return whole * 8u + (rest != 0u ? 1u + (rest + 1u) / 2u : 0u);
}

WlResult wl_brstm_parse(const uint8_t *f, size_t len, WlBrstm *out) {
    if (f == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (len < RSTM_HDR) {
        return WL_ERR_RANGE;
    }
    if (wl_rd_be32(f) != WL_BRSTM_MAGIC || wl_rd_be16(f + F_BOM) != 0xFEFFu) {
        return WL_ERR_FORMAT;
    }
    if (f[6] != 1u /* major version */ || wl_rd_be32(f + F_SIZE) != len || wl_rd_be16(f + F_HDRSIZE) != RSTM_HDR ||
        wl_rd_be16(f + F_CHUNKS) < 2u) {
        return WL_ERR_FORMAT;
    }
    uint32_t ho = wl_rd_be32(f + F_HEAD_OFF), hs = wl_rd_be32(f + F_HEAD_SIZE);
    uint32_t ao = wl_rd_be32(f + F_ADPC_OFF), as = wl_rd_be32(f + F_ADPC_SIZE);
    uint32_t dof = wl_rd_be32(f + F_DATA_OFF), ds = wl_rd_be32(f + F_DATA_SIZE);
    if (hs < 0x20u || as < 8u || ds < 8u || !wl_range_ok(ho, hs, len) || !wl_range_ok(ao, as, len) ||
        !wl_range_ok(dof, ds, len)) {
        return WL_ERR_RANGE;
    }
    if (memcmp(f + ho, "HEAD", 4) != 0 || memcmp(f + ao, "ADPC", 4) != 0 || memcmp(f + dof, "DATA", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    Head h = {f, (size_t)ho + 8u, (size_t)ho + hs};
    size_t si, tr, ci;
    if (!head_ref(&h, h.base, S_INFO_SIZE, &si) || !head_ref(&h, h.base + 8u, 4, &tr) ||
        !head_ref(&h, h.base + 16u, 4, &ci)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *s = f + si;
    WlBrstm b;
    memset(&b, 0, sizeof b);
    uint32_t loop = s[S_LOOP];
    b.channels = s[S_CHANNELS];
    b.rate = wl_rd_be16(s + S_RATE);
    b.loop_start = wl_rd_be32(s + S_LOOP_START);
    b.samples = wl_rd_be32(s + S_SAMPLES);
    b.data_off = wl_rd_be32(s + S_DATA_OFF);
    b.blocks = wl_rd_be32(s + S_BLOCKS);
    b.block_size = wl_rd_be32(s + S_BLOCK_SIZE);
    b.block_samples = wl_rd_be32(s + S_BLOCK_SAMPLES);
    b.last_size = wl_rd_be32(s + S_LAST_SIZE);
    b.last_samples = wl_rd_be32(s + S_LAST_SAMPLES);
    b.last_padded = wl_rd_be32(s + S_LAST_PADDED);
    uint32_t interval = wl_rd_be32(s + S_ADPC_INTERVAL), entry = wl_rd_be32(s + S_ADPC_ENTRY);
    if (s[S_CODEC] != CODEC_DSP || loop > 1u || b.channels < 1u || b.channels > 2u || b.rate < 8000u ||
        b.rate > 96000u || b.samples == 0u || b.samples >= WL_BRSTM_MAX_SAMPLES || b.block_size < 8u ||
        b.block_size > (1u << 24) || b.block_size % 8u != 0u || b.block_samples != b.block_size / 8u * 14u ||
        b.blocks == 0u) {
        return WL_ERR_FORMAT;
    }
    b.loop = (int)loop;
    /* block structure: all blocks full but the last, which holds the remaining samples */
    uint64_t full = (uint64_t)(b.blocks - 1u) * b.block_samples;
    if (full >= b.samples || b.last_samples == 0u || b.last_samples > b.block_samples ||
        full + b.last_samples != b.samples) {
        return WL_ERR_FORMAT;
    }
    if (b.last_size < last_bytes(b.last_samples) || b.last_size > b.last_padded || b.last_padded > b.block_size) {
        return WL_ERR_FORMAT;
    }
    if (b.loop && b.loop_start + 1u >= b.samples) {
        return WL_ERR_FORMAT;
    }
    /* the audio lies inside the DATA chunk */
    uint64_t total = ((uint64_t)(b.blocks - 1u) * b.block_size + b.last_padded) * b.channels;
    if (b.data_off < (uint64_t)dof + 8u || b.data_off + total > (uint64_t)dof + ds || b.data_off + total > len) {
        return WL_ERR_RANGE;
    }
    /* seek table: size only */
    if (entry != 4u || interval == 0u || (as - 8u) % (entry * b.channels) != 0u) {
        return WL_ERR_FORMAT;
    }
    b.adpc_entries = (as - 8u) / (entry * b.channels);
    /* channel table: HEAD part 3, a count, then a reference per channel to a reference to the DSP block */
    if (f[ci] != b.channels) {
        return WL_ERR_FORMAT;
    }
    for (uint32_t c = 0; c < b.channels; c++) {
        size_t e, d;
        if (!head_ref(&h, ci + 4u + 8u * c, 8, &e) || !head_ref(&h, e, 0x30, &d)) {
            return WL_ERR_RANGE;
        }
        const uint8_t *p = f + d;
        for (int k = 0; k < 16; k++) {
            b.coef[c][k] = (int16_t)wl_rd_be16(p + 2 * k);
        }
        if (wl_rd_be16(p + 0x20) != 0u) {
            return WL_ERR_FORMAT;
        }
        b.init_ps[c] = wl_rd_be16(p + 0x22);
        b.init_hist[c][0] = (int16_t)wl_rd_be16(p + 0x24);
        b.init_hist[c][1] = (int16_t)wl_rd_be16(p + 0x26);
        b.loop_ps[c] = wl_rd_be16(p + 0x28);
        b.loop_hist[c][0] = (int16_t)wl_rd_be16(p + 0x2A);
        b.loop_hist[c][1] = (int16_t)wl_rd_be16(p + 0x2C);
    }
    *out = b;
    (void)tr; /* the track table is not needed: every channel pair is one stereo track */
    return WL_OK;
}

WlResult wl_brstm_channel_frames(const WlBrstm *b, const uint8_t *f, size_t len, uint32_t ch, uint8_t *out) {
    if (ch >= b->channels) {
        return WL_ERR_ARG;
    }
    size_t need = (size_t)WL_BRSTM_FRAMES(b->samples) * 8u, pos = 0;
    memset(out, 0, need);
    for (uint32_t blk = 0; blk < b->blocks; blk++) {
        int last = blk + 1u == b->blocks;
        uint64_t src = (uint64_t)b->data_off + (uint64_t)blk * b->block_size * b->channels +
                       (last ? (uint64_t)ch * b->last_padded : (uint64_t)ch * b->block_size);
        size_t n = last ? last_bytes(b->last_samples) : b->block_size;
        if (n > need - pos || src > len || len - src < n) {
            return WL_ERR_RANGE;
        }
        memcpy(out + pos, f + src, n);
        pos += n;
        if (last && b->last_samples % 14u != 0u) {
            /* the cut frame: it sits at the end of what was copied, up to the next frame boundary */
            pos = (pos + 7u) & ~(size_t)7u;
        }
    }
    return pos == need ? WL_OK : WL_ERR_FORMAT;
}

WlResult wl_brstm_decode(const WlBrstm *b, const uint8_t *f, size_t len, uint32_t ch, int16_t *out) {
    if (ch >= b->channels) {
        return WL_ERR_ARG;
    }
    int16_t h1 = b->init_hist[ch][0], h2 = b->init_hist[ch][1];
    size_t done = 0;
    for (uint32_t blk = 0; blk < b->blocks; blk++) {
        int last = blk + 1u == b->blocks;
        uint64_t src = (uint64_t)b->data_off + (uint64_t)blk * b->block_size * b->channels +
                       (last ? (uint64_t)ch * b->last_padded : (uint64_t)ch * b->block_size);
        uint32_t samples = last ? b->last_samples : b->block_samples;
        uint32_t whole = samples / 14u, rest = samples % 14u;
        if (src > len || len - src < last_bytes(samples)) {
            return WL_ERR_RANGE;
        }
        if (wl_brstm_dsp_frames_valid(f + src, whole) != WL_OK) {
            return WL_ERR_FORMAT;
        }
        wl_brstm_dsp_decode(f + src, whole, b->coef[ch], &h1, &h2, out + done);
        done += (size_t)whole * 14u;
        if (rest != 0u) { /* the cut frame: header and the bytes of real samples, as a whole frame */
            uint8_t fr[8];
            int16_t tmp[14];
            memset(fr, 0, sizeof fr);
            memcpy(fr, f + src + (size_t)whole * 8u, last_bytes(rest));
            if ((fr[0] >> 4) > 7u) {
                return WL_ERR_FORMAT;
            }
            wl_brstm_dsp_decode(fr, 1, b->coef[ch], &h1, &h2, tmp);
            memcpy(out + done, tmp, (size_t)rest * sizeof(int16_t));
            done += rest;
        }
    }
    return done == b->samples ? WL_OK : WL_ERR_FORMAT;
}

WlResult wl_brstm_state(const WlBrstm *b, const uint8_t *f, size_t len, WlBrstmState *out,
                        WlBrstmNotes *notes) {
    if (b == NULL || f == NULL || out == NULL || b->channels < 1u || b->channels > 2u) {
        return WL_ERR_ARG;
    }
    WlBrstmNotes nt;
    memset(&nt, 0, sizeof nt);
    memset(out, 0, sizeof *out);
    size_t frames = WL_BRSTM_FRAMES(b->samples), per = frames * 8u;
    uint8_t *fr = (uint8_t *)malloc(per);
    int16_t *pcm = b->loop ? (int16_t *)malloc(frames * 14u * sizeof(int16_t)) : NULL;
    if (fr == NULL || (b->loop && pcm == NULL)) {
        free(fr);
        free(pcm);
        return WL_ERR_NOMEM;
    }
    WlResult r = WL_OK;
    for (uint32_t c = 0; c < b->channels; c++) {
        r = wl_brstm_channel_frames(b, f, len, c, fr);
        if (r != WL_OK) {
            break;
        }
        if (wl_brstm_dsp_frames_valid(fr, frames) != WL_OK) {
            r = WL_ERR_FORMAT;
            break;
        }
        out->init_ps[c] = fr[0];
        nt.ps_fixed |= fr[0] != b->init_ps[c];
        if (b->loop) {
            int16_t h1 = b->init_hist[c][0], h2 = b->init_hist[c][1];
            wl_brstm_dsp_decode(fr, frames, b->coef[c], &h1, &h2, pcm);
            uint32_t ls = b->loop_start;
            out->loop_ps[c] = fr[(size_t)(ls / 14u) * 8u];
            out->loop_hist[c][0] = ls >= 1u ? pcm[ls - 1u] : b->init_hist[c][0];
            out->loop_hist[c][1] = ls >= 2u ? pcm[ls - 2u] : (ls == 1u ? b->init_hist[c][0] : b->init_hist[c][1]);
            nt.loop_ps_bad |= out->loop_ps[c] != b->loop_ps[c];
            nt.loop_hist_bad |= out->loop_hist[c][0] != b->loop_hist[c][0] ||
                                out->loop_hist[c][1] != b->loop_hist[c][1];
        }
    }
    free(fr);
    free(pcm);
    if (notes != NULL) {
        *notes = nt;
    }
    return r;
}
