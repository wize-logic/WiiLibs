// SPDX-License-Identifier: MIT
// BRSTM reader with synthetic inputs only: RSTM files generated here from a pseudo-random source (no
// game data). De-interleaving and decoding are compared with an independent reference decoder sample
// for sample; loop state, partial last frames, mismatching headers, truncation and random corruption
// are covered.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/base/endian.h"
#include "wl/brstm/brstm.h"

static uint32_t g_seed = 987654321u;
static uint32_t rnd(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

/* Independent reference decoder: one sample at a time. */
static void ref_decode(const uint8_t *fr, size_t nframes, const int16_t *coef, int16_t *out) {
    int h1 = 0, h2 = 0;
    for (size_t f = 0; f < nframes; f++) {
        int pi = fr[f * 8] >> 4, sc = fr[f * 8] & 15;
        for (size_t k = 0; k < 14; k++) {
            int b = fr[f * 8 + 1 + k / 2];
            int nib = (k % 2 == 0) ? b >> 4 : b & 15;
            nib = nib >= 8 ? nib - 16 : nib;
            long v = ((long)nib * (2048L << sc) + 1024 + (long)coef[pi * 2] * h1 + (long)coef[pi * 2 + 1] * h2) >> 11;
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            out[f * 14 + k] = (int16_t)v;
            h2 = h1;
            h1 = (int)v;
        }
    }
}

/* ---- synthetic RSTM ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *file;
    size_t len;
    uint32_t samples, ch, bs;
    int16_t coef[2][16];
    uint8_t *chan[2]; /* reference frames per channel: the cut last frame is zero after its real bytes */
    uint32_t data_off, head_off, dsp_off[2]; /* positions in the file, for corruption tests */
} Rs;

static uint32_t last_bytes(uint32_t n) {
    uint32_t rest = n % 14u;
    return n / 14u * 8u + (rest ? 1u + (rest + 1u) / 2u : 0u);
}

/* loop_start < 0: no loop. hdr_ok: the loop state in the header is the true one (else deliberately wrong). */
static void make_rs(Rs *s, uint32_t samples, uint32_t ch, uint32_t bs, int64_t loop_start, int hdr_ok) {
    memset(s, 0, sizeof *s);
    s->samples = samples;
    s->ch = ch;
    s->bs = bs;
    uint32_t bsamp = bs / 8u * 14u, blocks = (samples + bsamp - 1u) / bsamp;
    uint32_t lsamp = samples - (blocks - 1u) * bsamp, lsize = last_bytes(lsamp), lpad = (lsize + 31u) & ~31u;
    if (lpad > bs) {
        lpad = bs;
    }
    size_t frames = (samples + 13u) / 14u, per = frames * 8u;
    uint64_t total = ((uint64_t)(blocks - 1u) * bs + lpad) * ch;
    /* layout */
    uint32_t head_size = 0x100u, adpc_entries = (uint32_t)((((blocks - 1u) * bs + lsize) + 0x3FFu) / 0x400u);
    uint32_t adpc_size = (8u + adpc_entries * 4u * ch + 31u) & ~31u;
    uint32_t adpc_off = 0x40u + head_size, data_off = adpc_off + adpc_size;
    uint32_t data_size = (0x20u + (uint32_t)total + 31u) & ~31u;
    s->len = data_off + data_size;
    s->file = (uint8_t *)calloc(1, s->len);
    uint8_t *f = s->file;
    memcpy(f, "RSTM", 4);
    wl_wr_be16(f + 4, 0xFEFF);
    wl_wr_be16(f + 6, 0x0100);
    wl_wr_be32(f + 8, (uint32_t)s->len);
    wl_wr_be16(f + 12, 0x40);
    wl_wr_be16(f + 14, 2);
    wl_wr_be32(f + 0x10, 0x40);
    wl_wr_be32(f + 0x14, head_size);
    wl_wr_be32(f + 0x18, adpc_off);
    wl_wr_be32(f + 0x1C, adpc_size);
    wl_wr_be32(f + 0x20, data_off);
    wl_wr_be32(f + 0x24, data_size);
    /* HEAD */
    uint8_t *h = f + 0x40;
    memcpy(h, "HEAD", 4);
    wl_wr_be32(h + 4, head_size);
    uint8_t *pl = h + 8; /* payload; references are relative to it */
    wl_wr_be32(pl + 0, 0x01000000u);
    wl_wr_be32(pl + 4, 0x18);
    wl_wr_be32(pl + 8, 0x01000000u);
    wl_wr_be32(pl + 12, 0x4C);
    wl_wr_be32(pl + 16, 0x01000000u);
    wl_wr_be32(pl + 20, 0x5C);
    uint8_t *si = pl + 0x18;
    si[0] = 2;
    si[1] = loop_start >= 0 ? 1 : 0;
    si[2] = (uint8_t)ch;
    wl_wr_be16(si + 4, 32000);
    wl_wr_be32(si + 8, loop_start >= 0 ? (uint32_t)loop_start : 0u);
    wl_wr_be32(si + 0x0C, samples);
    wl_wr_be32(si + 0x10, data_off + 0x20u);
    wl_wr_be32(si + 0x14, blocks);
    wl_wr_be32(si + 0x18, bs);
    wl_wr_be32(si + 0x1C, bsamp);
    wl_wr_be32(si + 0x20, lsize);
    wl_wr_be32(si + 0x24, lsamp);
    wl_wr_be32(si + 0x28, lpad);
    wl_wr_be32(si + 0x2C, 0x400);
    wl_wr_be32(si + 0x30, 4);
    uint8_t *tr = pl + 0x4C; /* track table: one track */
    tr[0] = 1;
    wl_wr_be32(tr + 4, 0x01000000u);
    wl_wr_be32(tr + 8, 0x58);
    uint8_t *cn = pl + 0x5C; /* channel table: count, a reference per channel, then the channel references and DSP blocks */
    cn[0] = (uint8_t)ch;
    uint32_t refs_at = 0x5C + 4u, info_at = refs_at + 8u * ch, dsp_at = info_at + 8u * ch;
    for (uint32_t c = 0; c < ch; c++) {
        wl_wr_be32(pl + refs_at + 8u * c, 0x01000000u);
        wl_wr_be32(pl + refs_at + 8u * c + 4u, info_at + 8u * c);
        wl_wr_be32(pl + info_at + 8u * c, 0x01000000u);
        wl_wr_be32(pl + info_at + 8u * c + 4u, dsp_at + 0x30u * c);
        s->dsp_off[c] = 0x48u + dsp_at + 0x30u * c;
    }
    /* audio */
    for (size_t i = 0; i < total; i++) {
        f[data_off + 0x20u + i] = (uint8_t)rnd(); /* garbage everywhere, then the frame headers fixed */
    }
    memcpy(f + adpc_off, "ADPC", 4);
    wl_wr_be32(f + adpc_off + 4, adpc_size);
    memcpy(f + data_off, "DATA", 4);
    wl_wr_be32(f + data_off + 4, data_size);
    for (uint32_t c = 0; c < ch; c++) {
        uint8_t *dsp = f + s->dsp_off[c];
        for (int k = 0; k < 16; k++) {
            s->coef[c][k] = (int16_t)((int)(rnd() % 6000) - 3000);
            wl_wr_be16(dsp + 2 * k, (uint16_t)s->coef[c][k]);
        }
        s->chan[c] = (uint8_t *)calloc(1, per);
        size_t pos = 0;
        for (uint32_t blk = 0; blk < blocks; blk++) {
            int last = blk + 1u == blocks;
            size_t n = last ? lsize : bs;
            size_t src = data_off + 0x20u + ((size_t)blk * bs * ch) + (last ? (size_t)c * lpad : (size_t)c * bs);
            for (size_t p = 0; p + 0 < n; p += 8) {
                f[src + p] = (uint8_t)(((rnd() % 8) << 4) | (rnd() % 9));
            }
            memcpy(s->chan[c] + pos, f + src, n);
            pos += n;
            if (last && lsamp % 14u != 0u) {
                pos = (pos + 7u) & ~(size_t)7u;
            }
        }
        wl_wr_be16(dsp + 0x22, s->chan[c][0]);
        if (loop_start >= 0) {
            int16_t *pcm = (int16_t *)malloc(frames * 14 * sizeof(int16_t));
            ref_decode(s->chan[c], frames, s->coef[c], pcm);
            uint32_t ls = (uint32_t)loop_start;
            uint16_t ps = s->chan[c][ls / 14u * 8u];
            int16_t a = ls >= 1u ? pcm[ls - 1u] : 0, b = ls >= 2u ? pcm[ls - 2u] : 0;
            if (!hdr_ok) {
                ps = (uint16_t)(ps ^ 1u);
                a = (int16_t)(a + 1);
            }
            wl_wr_be16(dsp + 0x28, ps);
            wl_wr_be16(dsp + 0x2A, (uint16_t)a);
            wl_wr_be16(dsp + 0x2C, (uint16_t)b);
            free(pcm);
        }
        if (!hdr_ok) {
            wl_wr_be16(dsp + 0x22, (uint16_t)(s->chan[c][0] ^ 2u)); /* wrong initial predictor/scale */
        }
    }
    s->data_off = data_off;
    s->head_off = 0x40;
}

static void free_rs(Rs *s) {
    free(s->file);
    free(s->chan[0]);
    free(s->chan[1]);
}

/* The de-interleaved frames are the source's, and both they and the straight decode give the reference. */
static int check_exact(const Rs *s, const WlBrstm *b) {
    size_t frames = WL_BRSTM_FRAMES(s->samples);
    uint8_t *fr = (uint8_t *)malloc(frames * 8u);
    int16_t *ref = (int16_t *)malloc(frames * 14u * 2u), *a = (int16_t *)malloc(frames * 14u * 2u),
            *c2 = (int16_t *)malloc((size_t)s->samples * 2u);
    int ok = 1;
    for (uint32_t c = 0; c < s->ch && ok; c++) {
        ref_decode(s->chan[c], frames, s->coef[c], ref);
        int16_t h1 = 0, h2 = 0;
        ok = wl_brstm_channel_frames(b, s->file, s->len, c, fr) == WL_OK && memcmp(fr, s->chan[c], frames * 8u) == 0;
        if (ok) {
            wl_brstm_dsp_decode(fr, frames, b->coef[c], &h1, &h2, a);
            ok = wl_brstm_decode(b, s->file, s->len, c, c2) == WL_OK &&
                 memcmp(ref, a, (size_t)s->samples * 2) == 0 && memcmp(ref, c2, (size_t)s->samples * 2) == 0;
        }
    }
    free(fr);
    free(ref);
    free(a);
    free(c2);
    return ok;
}

TEST stereo_loop(void) {
    Rs s;
    make_rs(&s, 5000, 2, 0x100, 1400, 1); /* blocks of 448 samples, loop on a frame boundary */
    WlBrstm b;
    ASSERT_EQ(WL_OK, wl_brstm_parse(s.file, s.len, &b));
    ASSERT_EQ(2u, b.channels);
    ASSERT_EQ(5000u, b.samples);
    ASSERT_EQ(12u, b.blocks);
    ASSERT_EQ(32000u, b.rate);
    ASSERT(b.loop);
    ASSERT_EQ(1400u, b.loop_start);
    WlBrstmState st;
    WlBrstmNotes nt;
    ASSERT_EQ(WL_OK, wl_brstm_state(&b, s.file, s.len, &st, &nt));
    ASSERT(!nt.ps_fixed && !nt.loop_ps_bad && !nt.loop_hist_bad);
    ASSERT_EQ(s.chan[1][0], st.init_ps[1]);
    ASSERT_EQ(s.chan[0][1400 / 14 * 8], st.loop_ps[0]);
    ASSERT(check_exact(&s, &b));
    free_rs(&s);
    PASS();
}

TEST mid_frame_loop(void) {
    Rs s;
    make_rs(&s, 2000, 2, 0x80, 301, 1); /* 301 = 21 * 14 + 7 */
    WlBrstm b;
    ASSERT_EQ(WL_OK, wl_brstm_parse(s.file, s.len, &b));
    WlBrstmState st;
    WlBrstmNotes nt;
    ASSERT_EQ(WL_OK, wl_brstm_state(&b, s.file, s.len, &st, &nt));
    ASSERT(!nt.loop_ps_bad && !nt.loop_hist_bad);
    ASSERT_EQ(s.chan[0][21 * 8], st.loop_ps[0]);
    ASSERT(check_exact(&s, &b));
    free_rs(&s);
    PASS();
}

TEST mono_partial_last_frame(void) {
    const uint32_t cases[][3] = {{449, 1, 0x100}, {448, 1, 0x100}, {1, 1, 0x20}, {14, 1, 0x20}, {15, 2, 0x20},
                                 {896, 2, 0x100}, {897, 2, 0x100}, {1000, 1, 0x40}, {13, 2, 0x20}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Rs s;
        make_rs(&s, cases[i][0], cases[i][1], cases[i][2], -1, 1);
        WlBrstm b;
        ASSERT_EQ(WL_OK, wl_brstm_parse(s.file, s.len, &b));
        ASSERT_EQ(cases[i][1], b.channels);
        ASSERT(!b.loop);
        WlBrstmState st;
        ASSERT_EQ(WL_OK, wl_brstm_state(&b, s.file, s.len, &st, NULL));
        ASSERT_EQ(0u, st.loop_ps[0]);
        ASSERT(check_exact(&s, &b));
        free_rs(&s);
    }
    PASS();
}

TEST header_disagrees_with_data(void) {
    Rs s;
    make_rs(&s, 3000, 2, 0x100, 700, 0); /* wrong loop predictor, wrong loop history, wrong initial predictor */
    WlBrstm b;
    ASSERT_EQ(WL_OK, wl_brstm_parse(s.file, s.len, &b));
    WlBrstmState st;
    WlBrstmNotes nt;
    ASSERT_EQ(WL_OK, wl_brstm_state(&b, s.file, s.len, &st, &nt));
    ASSERT(nt.ps_fixed && nt.loop_ps_bad && nt.loop_hist_bad);
    ASSERT(check_exact(&s, &b));
    /* the loop history is the one of the audio */
    size_t frames = WL_BRSTM_FRAMES(s.samples);
    int16_t *ref = (int16_t *)malloc(frames * 14u * 2u);
    ref_decode(s.chan[0], frames, s.coef[0], ref);
    ASSERT_EQ(ref[699], st.loop_hist[0][0]);
    ASSERT_EQ(ref[698], st.loop_hist[0][1]);
    ASSERT_EQ(s.chan[0][700 / 14 * 8], st.loop_ps[0]);
    free(ref);
    free_rs(&s);
    PASS();
}

TEST dsp_decoder_history(void) {
    /* Decoding frame by frame with the history carried gives the same as one call. */
    uint8_t fr[8 * 6];
    int16_t coef[16], one[6 * 14], step[6 * 14];
    for (size_t i = 0; i < sizeof fr; i++) {
        fr[i] = (uint8_t)rnd();
    }
    for (int f = 0; f < 6; f++) {
        fr[f * 8] = (uint8_t)(((rnd() % 8) << 4) | (rnd() % 12));
    }
    for (int k = 0; k < 16; k++) {
        coef[k] = (int16_t)((int)(rnd() % 8000) - 4000);
    }
    ASSERT_EQ(WL_OK, wl_brstm_dsp_frames_valid(fr, 6));
    int16_t h1 = 0, h2 = 0;
    wl_brstm_dsp_decode(fr, 6, coef, &h1, &h2, one);
    int16_t g1 = 0, g2 = 0;
    for (size_t f = 0; f < 6; f++) {
        wl_brstm_dsp_decode(fr + 8 * f, 1, coef, &g1, &g2, step + 14 * f);
    }
    ASSERT_MEM_EQ(one, step, sizeof one);
    ASSERT_EQ(one[6 * 14 - 1], h1);
    ASSERT_EQ(one[6 * 14 - 2], h2);
    int16_t ref[6 * 14];
    ref_decode(fr, 6, coef, ref);
    ASSERT_MEM_EQ(ref, one, sizeof one);
    fr[3 * 8] = 0x85;
    ASSERT_EQ(WL_ERR_FORMAT, wl_brstm_dsp_frames_valid(fr, 6));
    PASS();
}

static int rejects(const Rs *s, uint8_t *mut, size_t at, size_t n, const void *val) {
    uint8_t save[8];
    memcpy(save, mut + at, n);
    memcpy(mut + at, val, n);
    WlBrstm b;
    int bad = wl_brstm_parse(mut, s->len, &b) != WL_OK;
    memcpy(mut + at, save, n);
    return bad;
}

TEST malformed_rstm(void) {
    Rs s;
    make_rs(&s, 3000, 2, 0x100, 700, 1);
    uint8_t *f = s.file;
    const size_t si = 0x40 + 8 + 0x18; /* stream info */
    const uint8_t z32[4] = {0, 0, 0, 0}, big[4] = {0xFF, 0xFF, 0xFF, 0xF0}, one[4] = {0, 0, 0, 1};
    const uint8_t two = 2, three = 3, zero = 0, bad = 7;
    ASSERT(rejects(&s, f, 0, 4, "RSTN"));
    ASSERT(rejects(&s, f, 4, 2, "\xFF\xFE"));
    ASSERT(rejects(&s, f, 6, 1, &two)); /* version */
    ASSERT(rejects(&s, f, 8, 4, big));  /* file size field */
    ASSERT(rejects(&s, f, 12, 2, "\x00\x41"));
    ASSERT(rejects(&s, f, 0x10, 4, big)); /* HEAD offset */
    ASSERT(rejects(&s, f, 0x14, 4, z32)); /* HEAD size */
    ASSERT(rejects(&s, f, 0x18, 4, big)); /* ADPC offset */
    ASSERT(rejects(&s, f, 0x20, 4, big)); /* DATA offset */
    ASSERT(rejects(&s, f, 0x24, 4, big)); /* DATA size */
    ASSERT(rejects(&s, f, 0x40, 4, "HEAT"));
    ASSERT(rejects(&s, f, si + 0, 1, &zero));                  /* codec PCM8 */
    ASSERT(rejects(&s, f, si + 0, 1, &(uint8_t){1}));          /* codec PCM16 */
    ASSERT(rejects(&s, f, si + 1, 1, &two));                   /* loop flag */
    ASSERT(rejects(&s, f, si + 2, 1, &three));                 /* channels */
    ASSERT(rejects(&s, f, si + 2, 1, &zero));
    ASSERT(rejects(&s, f, si + 4, 2, "\x00\x10"));             /* rate */
    ASSERT(rejects(&s, f, si + 0x0C, 4, z32));                 /* samples */
    ASSERT(rejects(&s, f, si + 0x0C, 4, big));
    ASSERT(rejects(&s, f, si + 0x0C, 4, one));                 /* contradicts the block structure */
    ASSERT(rejects(&s, f, si + 0x10, 4, big));                 /* data offset */
    ASSERT(rejects(&s, f, si + 0x10, 4, z32));
    ASSERT(rejects(&s, f, si + 0x14, 4, z32));                 /* blocks */
    ASSERT(rejects(&s, f, si + 0x14, 4, big));
    ASSERT(rejects(&s, f, si + 0x18, 4, z32));                 /* block size */
    ASSERT(rejects(&s, f, si + 0x18, 4, one));
    ASSERT(rejects(&s, f, si + 0x1C, 4, one));                 /* block samples */
    ASSERT(rejects(&s, f, si + 0x20, 4, z32));                 /* last block size */
    ASSERT(rejects(&s, f, si + 0x20, 4, big));
    ASSERT(rejects(&s, f, si + 0x24, 4, z32));                 /* last block samples */
    ASSERT(rejects(&s, f, si + 0x24, 4, big));
    ASSERT(rejects(&s, f, si + 0x28, 4, big));                 /* last block padded */
    ASSERT(rejects(&s, f, si + 0x28, 4, z32));
    ASSERT(rejects(&s, f, si + 0x08, 4, big));                 /* loop start */
    ASSERT(rejects(&s, f, si + 0x30, 4, z32));                 /* ADPC entry size */
    /* references */
    ASSERT(rejects(&s, f, 0x48, 4, z32));                      /* marker of the stream info reference */
    ASSERT(rejects(&s, f, 0x4C, 4, big));
    ASSERT(rejects(&s, f, 0x58, 4, big));                      /* channel table offset */
    ASSERT(rejects(&s, f, 0x48 + 0x5C, 1, &one[3]));           /* channel count 1 vs 2 */
    ASSERT(rejects(&s, f, 0x48 + 0x5C, 1, &bad));
    ASSERT(rejects(&s, f, 0x48 + 0x5C + 4, 4, z32));           /* first channel reference marker */
    ASSERT(rejects(&s, f, 0x48 + 0x5C + 8, 4, big));
    ASSERT(rejects(&s, f, s.dsp_off[0] + 0x20, 2, "\x00\x01")); /* gain */
    /* every truncation is refused */
    for (size_t n = 0; n < s.len; n += 1 + n / 16) {
        WlBrstm b;
        ASSERT(wl_brstm_parse(s.file, n, &b) != WL_OK);
    }
    /* a frame with a predictor above 7 parses but does not convert */
    WlBrstm b;
    ASSERT_EQ(WL_OK, wl_brstm_parse(s.file, s.len, &b));
    s.file[s.data_off + 0x20 + 5 * 8] = 0x80;
    WlBrstmState st;
    ASSERT_EQ(WL_ERR_FORMAT, wl_brstm_state(&b, s.file, s.len, &st, NULL));
    int16_t *out = (int16_t *)malloc(3000 * 2);
    ASSERT_EQ(WL_ERR_FORMAT, wl_brstm_decode(&b, s.file, s.len, 0, out));
    free(out);
    free_rs(&s);
    PASS();
}

TEST fuzz_rstm(void) {
    Rs s;
    make_rs(&s, 1500, 2, 0x80, 140, 1);
    uint8_t *m = (uint8_t *)malloc(s.len);
    for (int it = 0; it < 4000; it++) {
        memcpy(m, s.file, s.len);
        int flips = 1 + (int)(rnd() % 4);
        for (int k = 0; k < flips; k++) {
            size_t at = it % 2 ? rnd() % 0x200 : rnd() % s.len; /* half the runs hit the headers */
            if (at < s.len) {
                m[at] = (uint8_t)rnd();
            }
        }
        WlBrstm b;
        if (wl_brstm_parse(m, s.len, &b) == WL_OK) {
            WlBrstmState st;
            WlResult r = wl_brstm_state(&b, m, s.len, &st, NULL);
            ASSERT(r == WL_OK || r == WL_ERR_FORMAT);
            int16_t *pcm = (int16_t *)malloc((size_t)b.samples * 2u);
            ASSERT(pcm != NULL);
            r = wl_brstm_decode(&b, m, s.len, (uint32_t)(it & 1) % b.channels, pcm);
            ASSERT(r == WL_OK || r == WL_ERR_FORMAT || r == WL_ERR_RANGE);
            free(pcm);
        }
    }
    free(m);
    free_rs(&s);
    PASS();
}

SUITE(brstm_suite) {
    RUN_TEST(stereo_loop);
    RUN_TEST(mid_frame_loop);
    RUN_TEST(mono_partial_last_frame);
    RUN_TEST(header_disagrees_with_data);
    RUN_TEST(dsp_decoder_history);
    RUN_TEST(malformed_rstm);
    RUN_TEST(fuzz_rstm);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(brstm_suite);
    GREATEST_MAIN_END();
}
