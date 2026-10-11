// SPDX-License-Identifier: MIT
// SDAT reader with a synthetic archive built here (no game data): 2 sequences, a sequence archive
// with 2 sub-sequences, 1 bank holding every instrument type, 1 wave archive with PCM8/PCM16/ADPCM
// waves, 1 stream; symbols on and off; malformed inputs must fail cleanly, never crash.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/sdat/sdat.h"

#include "sdat_synth.h"

/* Opens the archive and runs every reader over everything it names; returns the open result, or
 * WL_ERR_FAILED when a reader returned a view outside its file (a bug). */
static WlResult touch_all(const uint8_t *d0, size_t n0) {
    WlSdat s;
    WlResult r = wl_sdat_open(&s, d0, n0);
    if (r != WL_OK) {
        return r;
    }
    WlSbnkInst *inst = malloc(sizeof *inst);
    for (uint32_t f = 0; f < s.file_count + 1u && inst != NULL; f++) {
        const uint8_t *d;
        size_t n;
        if (wl_sdat_file(&s, f, &d, &n) != WL_OK) {
            continue;
        }
        if (d < d0 || d + n > d0 + n0) {
            r = WL_ERR_FAILED;
        }
        WlSseq q;
        WlSsar ar;
        WlSbnk bk;
        WlSwar sw;
        if (wl_sseq_open(&q, d, n) == WL_OK && q.data + q.size > d + n) {
            r = WL_ERR_FAILED;
        }
        (void)wl_strm_check(d, n);
        if (wl_ssar_open(&ar, d, n) == WL_OK) {
            for (uint32_t i = 0; i < ar.count; i++) {
                WlSsarEntry e;
                (void)wl_ssar_entry(&ar, i, &e);
            }
        }
        if (wl_sbnk_open(&bk, d, n) == WL_OK) {
            for (uint32_t i = 0; i < bk.count; i++) {
                (void)wl_sbnk_inst(&bk, i, inst);
            }
        }
        if (wl_swar_open(&sw, d, n) == WL_OK) {
            for (uint32_t i = 0; i < sw.count; i++) {
                WlSwavInfo wi;
                if (wl_swar_wave(&sw, i, &wi) == WL_OK && wi.present && wi.data_off + wi.data_size > sw.size) {
                    r = WL_ERR_FAILED;
                }
            }
        }
    }
    for (uint32_t g = 0; g < s.counts[WL_SDAT_GROUP]; g++) {
        WlSdatGroupEntry e;
        for (uint32_t i = 0; s.group[g].present && i < s.group[g].count; i++) {
            (void)wl_sdat_group_entry(&s.group[g], i, &e);
        }
    }
    free(inst);
    wl_sdat_close(&s);
    return r;
}

TEST open_with_symbols(void) {
    W w = {0};
    build_sdat(&w, &GOOD);
    WlSdat s;
    ASSERT_EQ(WL_OK, wl_sdat_open(&s, w.p, w.len));
    ASSERT_EQ(6u, s.file_count);
    ASSERT_EQ(2u, s.counts[WL_SDAT_SEQ]);
    ASSERT(s.has_symbols);
    ASSERT_STR_EQ("SEQ_TWO", s.seq[1].name);
    ASSERT_EQ(1u, s.seq[1].file_id);
    ASSERT_EQ(91, s.seq[1].volume);
    ASSERT_EQ(0u, s.seq[1].bank);
    ASSERT_STR_EQ("SSAR_ONE", s.seqarc[0].name);
    ASSERT_EQ(2u, s.seqarc_sub_count[0]);
    ASSERT_STR_EQ("SUB_B", s.seqarc_sub_names[0][1]);
    ASSERT_STR_EQ("BANK_SE001", s.bank[0].name);
    ASSERT_EQ(WL_SDAT_NONE, s.bank[0].wavearc[1]);
    ASSERT_EQ(0u, s.bank[0].wavearc[0]);
    ASSERT_EQ(1u, s.counts[WL_SDAT_GROUP]);
    WlSdatGroupEntry ge;
    ASSERT_EQ(WL_OK, wl_sdat_group_entry(&s.group[0], 1, &ge));
    ASSERT_EQ(2, ge.type);
    ASSERT_EQ(WL_ERR_RANGE, wl_sdat_group_entry(&s.group[0], 2, &ge));
    ASSERT_EQ(16, s.player2[0].count);
    ASSERT_EQ(127, s.strm[0].volume);
    const uint8_t *d;
    size_t n;
    ASSERT_EQ(WL_OK, wl_sdat_file(&s, 0, &d, &n));
    WlSseq q;
    ASSERT_EQ(WL_OK, wl_sseq_open(&q, d, n));
    ASSERT_EQ(8u, q.size);
    ASSERT_EQ(0x11, q.data[6]);
    ASSERT_EQ(WL_ERR_RANGE, wl_sdat_file(&s, 6, &d, &n));
    wl_sdat_close(&s);
    free(w.p);
    PASS();
}

TEST subfiles(void) {
    W w = {0};
    build_sdat(&w, &GOOD);
    WlSdat s;
    ASSERT_EQ(WL_OK, wl_sdat_open(&s, w.p, w.len));
    const uint8_t *d;
    size_t n;
    /* bank */
    ASSERT_EQ(WL_OK, wl_sdat_file(&s, 2, &d, &n));
    WlSbnk bk;
    ASSERT_EQ(WL_OK, wl_sbnk_open(&bk, d, n));
    ASSERT_EQ(5u, bk.count);
    WlSbnkInst *in = malloc(sizeof *in);
    ASSERT_EQ(WL_OK, wl_sbnk_inst(&bk, 0, in));
    ASSERT_EQ(WL_SBNK_PCM, in->type);
    ASSERT_EQ(1u, in->note_count);
    ASSERT_EQ(60, in->notes[0].key);
    ASSERT_EQ(WL_OK, wl_sbnk_inst(&bk, 1, in));
    ASSERT_EQ(WL_SBNK_PSG, in->type);
    ASSERT_EQ(3u, in->notes[0].wave);
    ASSERT_EQ(WL_OK, wl_sbnk_inst(&bk, 2, in));
    ASSERT_EQ(WL_SBNK_NOISE, in->type);
    ASSERT_EQ(WL_OK, wl_sbnk_inst(&bk, 3, in));
    ASSERT_EQ(WL_SBNK_DRUMSET, in->type);
    ASSERT_EQ(2u, in->note_count);
    ASSERT_EQ(60, in->low);
    ASSERT_EQ(2u, in->notes[1].wave);
    ASSERT_EQ(WL_OK, wl_sbnk_inst(&bk, 4, in));
    ASSERT_EQ(WL_SBNK_KEYSPLIT, in->type);
    ASSERT_EQ(2u, in->note_count);
    ASSERT_EQ(127, in->split_end[1]);
    ASSERT_EQ(WL_ERR_RANGE, wl_sbnk_inst(&bk, 5, in));
    free(in);
    /* waves */
    ASSERT_EQ(WL_OK, wl_sdat_file(&s, 3, &d, &n));
    WlSwar sw;
    ASSERT_EQ(WL_OK, wl_swar_open(&sw, d, n));
    ASSERT_EQ(3u, sw.count);
    WlSwavInfo wi;
    ASSERT_EQ(WL_OK, wl_swar_wave(&sw, 0, &wi));
    ASSERT_EQ(WL_SWAV_PCM8, wi.encoding);
    ASSERT_EQ(8000, wi.rate);
    ASSERT_EQ(16u, wi.data_size);
    ASSERT_EQ(WL_OK, wl_swar_wave(&sw, 1, &wi));
    ASSERT_EQ(WL_SWAV_PCM16, wi.encoding);
    ASSERT(wi.loop);
    ASSERT_EQ(32u, wi.data_size);
    ASSERT_EQ(WL_OK, wl_swar_wave(&sw, 2, &wi));
    ASSERT_EQ(WL_SWAV_ADPCM, wi.encoding);
    ASSERT_EQ(22050, wi.rate);
    ASSERT_EQ(WL_ERR_RANGE, wl_swar_wave(&sw, 3, &wi));
    /* sequence archive */
    ASSERT_EQ(WL_OK, wl_sdat_file(&s, 4, &d, &n));
    WlSsar ar;
    ASSERT_EQ(WL_OK, wl_ssar_open(&ar, d, n));
    ASSERT_EQ(2u, ar.count);
    WlSsarEntry e;
    ASSERT_EQ(WL_OK, wl_ssar_entry(&ar, 1, &e));
    ASSERT_EQ(0x39u, e.offset);
    ASSERT_EQ(100, e.volume);
    ASSERT_EQ(WL_OK, wl_sdat_file(&s, 5, &d, &n));
    ASSERT_EQ(WL_OK, wl_strm_check(d, n));
    wl_sdat_close(&s);
    free(w.p);
    PASS();
}

TEST open_without_symbols(void) {
    W w = {0};
    Opt o = GOOD;
    o.symbols = 0;
    build_sdat(&w, &o);
    WlSdat s;
    ASSERT_EQ(WL_OK, wl_sdat_open(&s, w.p, w.len));
    ASSERT(!s.has_symbols);
    ASSERT(s.seq[0].name == NULL);
    ASSERT(s.bank[0].name == NULL);
    ASSERT_EQ(2u, s.counts[WL_SDAT_SEQ]);
    ASSERT_EQ(WL_OK, touch_all(w.p, w.len));
    wl_sdat_close(&s);
    free(w.p);
    PASS();
}

TEST malformed(void) {
    W w = {0};
    build_sdat(&w, &GOOD);
    size_t n = w.len;
    uint8_t *c = malloc(n);
    WlSdat s;
    memcpy(c, w.p, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_sdat_open(&s, c, 0x3F)); /* too short */
    c[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_sdat_open(&s, c, n));
    memcpy(c, w.p, n);
    p32(&(W){c, n, n}, 8, (uint32_t)n + 100u); /* declared size beyond the buffer */
    ASSERT_EQ(WL_ERR_FORMAT, wl_sdat_open(&s, c, n));
    memcpy(c, w.p, n);
    p32(&(W){c, n, n}, 24, 0xFFFFFFF0u); /* INFO offset out of range */
    ASSERT_EQ(WL_ERR_FORMAT, wl_sdat_open(&s, c, n));
    memcpy(c, w.p, n);
    p32(&(W){c, n, n}, 20, 0xFFFFFFF0u); /* SYMB size out of range */
    ASSERT(wl_sdat_open(&s, c, n) != WL_OK);
    /* a list count too large for the INFO block */
    memcpy(c, w.p, n);
    WlSdat ok;
    ASSERT_EQ(WL_OK, wl_sdat_open(&ok, c, n));
    uint32_t info_off = 0x40u;
    info_off += (uint32_t)(c[20] | (c[21] << 8) | (c[22] << 16));
    uint32_t lo = (uint32_t)(c[info_off + 8] | (c[info_off + 9] << 8));
    wl_sdat_close(&ok);
    p32(&(W){c, n, n}, info_off + lo, 0x7FFFFFFFu);
    ASSERT_EQ(WL_ERR_RANGE, wl_sdat_open(&s, c, n));
    /* a record offset past the block */
    memcpy(c, w.p, n);
    p32(&(W){c, n, n}, info_off + lo + 4, 0x00FFFFFFu);
    ASSERT_EQ(WL_ERR_RANGE, wl_sdat_open(&s, c, n));
    /* FAT count too large */
    memcpy(c, w.p, n);
    uint32_t fat_off = (uint32_t)(c[32] | (c[33] << 8) | (c[34] << 16));
    p32(&(W){c, n, n}, fat_off + 8, 0x0FFFFFFFu);
    ASSERT_EQ(WL_ERR_RANGE, wl_sdat_open(&s, c, n));
    /* FAT extent outside the file: opens, but the file is unreachable */
    memcpy(c, w.p, n);
    p32(&(W){c, n, n}, fat_off + 12 + 16 * 2, 0xFFFFFF00u);
    ASSERT_EQ(WL_OK, wl_sdat_open(&s, c, n));
    const uint8_t *d;
    size_t dn;
    ASSERT_EQ(WL_ERR_RANGE, wl_sdat_file(&s, 2, &d, &dn));
    wl_sdat_close(&s);

    /* sub-file corruption */
    WlSdat g;
    ASSERT_EQ(WL_OK, wl_sdat_open(&g, w.p, n));
    ASSERT_EQ(WL_OK, wl_sdat_file(&g, 2, &d, &dn));
    uint8_t sub[256];
    ASSERT(dn <= sizeof sub);
    WlSbnk bk;
    WlSbnkInst *in = malloc(sizeof *in);
    memcpy(sub, d, dn);
    p32(&(W){sub, dn, dn}, 0x38, 0x10000000u); /* instrument count too large */
    ASSERT_EQ(WL_ERR_RANGE, wl_sbnk_open(&bk, sub, dn));
    memcpy(sub, d, dn);
    sub[0x3C + 3 * 4 + 1] = 0xFF; sub[0x3C + 3 * 4 + 2] = 0xFF; /* drum set offset out of range */
    ASSERT_EQ(WL_OK, wl_sbnk_open(&bk, sub, dn));
    ASSERT_EQ(WL_ERR_RANGE, wl_sbnk_inst(&bk, 3, in));
    memcpy(sub, d, dn);
    sub[0x3C + 4 * 4] = 99; /* unknown instrument type */
    ASSERT_EQ(WL_OK, wl_sbnk_open(&bk, sub, dn));
    ASSERT_EQ(WL_ERR_FORMAT, wl_sbnk_inst(&bk, 4, in));
    free(in);
    ASSERT_EQ(WL_OK, wl_sdat_file(&g, 3, &d, &dn));
    WlSwar sw;
    WlSwavInfo wi;
    memcpy(sub, d, dn);
    p32(&(W){sub, dn, dn}, 0x3C + 4, 0x00F00000u); /* wave offset out of range */
    ASSERT_EQ(WL_OK, wl_swar_open(&sw, sub, dn));
    ASSERT_EQ(WL_ERR_RANGE, wl_swar_wave(&sw, 1, &wi));
    memcpy(sub, d, dn);
    p32(&(W){sub, dn, dn}, 0x3C + 8 + 8, 0x00FFFFFFu); /* wave length beyond the file */
    uint32_t off2 = (uint32_t)(sub[0x3C + 8] | (sub[0x3C + 9] << 8));
    p32(&(W){sub, dn, dn}, off2 + 8, 0x00FFFFFFu);
    ASSERT_EQ(WL_OK, wl_swar_open(&sw, sub, dn));
    ASSERT_EQ(WL_ERR_RANGE, wl_swar_wave(&sw, 2, &wi));
    memcpy(sub, d, dn);
    sub[0x3C + 4 + 0] = 0; /* wave offset 0 = unused slot (still fine) */
    sub[0x3C + 4 + 1] = 0; sub[0x3C + 4 + 2] = 0;
    ASSERT_EQ(WL_OK, wl_swar_open(&sw, sub, dn));
    ASSERT_EQ(WL_OK, wl_swar_wave(&sw, 1, &wi));
    ASSERT(!wi.present);
    ASSERT_EQ(WL_ERR_FORMAT, wl_swar_open(&sw, sub, 8));
    ASSERT_EQ(WL_OK, wl_sdat_file(&g, 4, &d, &dn));
    WlSsar ar;
    memcpy(sub, d, dn);
    p32(&(W){sub, dn, dn}, 28, 0x01000000u); /* sub-sequence count too large */
    ASSERT_EQ(WL_ERR_RANGE, wl_ssar_open(&ar, sub, dn));
    ASSERT_EQ(WL_ERR_FORMAT, wl_sseq_open(&(WlSseq){0}, sub, 4));
    wl_sdat_close(&g);

    /* every single-byte mutation of the headers and tables must end in a result, not a crash */
    size_t lim = n < 0x300 ? n : 0x300;
    for (size_t i = 0; i < lim; i++) {
        for (unsigned v = 0; v < 2; v++) {
            memcpy(c, w.p, n);
            c[i] = v ? 0xFF : 0x00;
            WlResult r = touch_all(c, n);
            ASSERT(r != WL_ERR_FAILED);
        }
    }
    free(c);
    free(w.p);
    PASS();
}

SUITE(sdat_suite) {
    RUN_TEST(open_with_symbols);
    RUN_TEST(open_without_symbols);
    RUN_TEST(subfiles);
    RUN_TEST(malformed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(sdat_suite);
    GREATEST_MAIN_END();
}
