// SPDX-License-Identifier: MIT
// Genius Sonority containers with synthetic data only: LZSS round trip (a small compressor lives
// here), GSfsys.toc with both entry sizes, FSYS archives, type names, and malformed inputs
// (every one must return an error, never crash).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/gs/gs.h"
#include "greatest.h"

/* ---- test-side LZSS compressor: literals plus matches that do not overlap the write area ---- */
static size_t lz_compress(const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t ring[4096];
    memset(ring, 0, sizeof ring);
    uint32_t r = 4078;
    size_t i = 0, o = 0;
    while (i < n) {
        size_t flag_at = o++;
        uint8_t flags = 0;
        out[flag_at] = 0;
        for (int bit = 0; bit < 8 && i < n; bit++) {
            uint32_t best = 0, best_pos = 0;
            for (uint32_t pos = 0; pos < 4096; pos++) {
                uint32_t len = 0;
                while (len < 18 && i + len < n && ring[(pos + len) & 4095] == in[i + len] &&
                       ((pos + len - r) & 4095) >= 18) {
                    len++;
                }
                if (len > best) {
                    best = len;
                    best_pos = pos;
                }
            }
            if (best >= 3) {
                out[o++] = (uint8_t)(best_pos & 0xFF);
                out[o++] = (uint8_t)(((best_pos >> 4) & 0xF0) | (best - 3));
                for (uint32_t k = 0; k < best; k++) {
                    ring[r] = in[i++];
                    r = (r + 1) & 4095;
                }
            } else {
                flags |= (uint8_t)(1u << bit);
                out[o++] = in[i];
                ring[r] = in[i++];
                r = (r + 1) & 4095;
            }
        }
        out[flag_at] = flags;
    }
    return o;
}

static size_t make_lzss(const uint8_t *in, size_t n, uint8_t *out) {
    size_t c = lz_compress(in, n, out + 16);
    wl_wr_be32(out, WL_GS_LZSS_MAGIC);
    wl_wr_be32(out + 4, (uint32_t)n);
    wl_wr_be32(out + 8, (uint32_t)(c + 16));
    wl_wr_be32(out + 12, 0);
    return c + 16;
}

static void pattern(uint8_t *d, size_t n) {
    uint32_t x = 12345;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        d[i] = (i % 97 < 40) ? (uint8_t)(i % 7) : (uint8_t)(x >> 16 & 0x3F);
    }
}

TEST lzss_roundtrip(void) {
    static uint8_t in[3000], packed[8000], out[3000];
    pattern(in, sizeof in);
    size_t pn = make_lzss(in, sizeof in, packed);
    ASSERT(pn < sizeof in); /* the matches really compress */
    uint8_t *res = NULL;
    size_t len = 0;
    ASSERT_EQ(WL_OK, wl_gs_lzss_unpack(packed, pn, 1u << 20, &res, &len));
    ASSERT_EQ(sizeof in, len);
    ASSERT_MEM_EQ(in, res, sizeof in);
    ASSERT_EQ(0, res[len]);
    free(res);
    ASSERT_EQ(WL_OK, wl_gs_lzss_decode(packed + 16, pn - 16, out, sizeof out));
    ASSERT_MEM_EQ(in, out, sizeof in);
    PASS();
}

TEST lzss_malformed(void) {
    static uint8_t in[600], packed[2000];
    pattern(in, sizeof in);
    size_t pn = make_lzss(in, sizeof in, packed);
    uint8_t *res = NULL;
    size_t len = 0;
    for (size_t cut = 0; cut < pn; cut++) { /* every truncation fails cleanly */
        WlResult r = wl_gs_lzss_unpack(packed, cut, 1u << 20, &res, &len);
        ASSERT(r != WL_OK);
    }
    ASSERT_EQ(WL_ERR_RANGE, wl_gs_lzss_unpack(packed, pn, 100, &res, &len));
    uint8_t bad[2000];
    memcpy(bad, packed, pn);
    bad[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_lzss_unpack(bad, pn, 1u << 20, &res, &len));
    memcpy(bad, packed, pn);
    wl_wr_be32(bad + 8, 8); /* packed size below the header */
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_lzss_unpack(bad, pn, 1u << 20, &res, &len));
    memcpy(bad, packed, pn);
    wl_wr_be32(bad + 4, 0x7000); /* claims more output than the stream holds */
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_lzss_unpack(bad, pn, 1u << 20, &res, &len));
    PASS();
}

/* ---- GSfsys.toc ---------------------------------------------------------------------------- */
static size_t make_toc(uint8_t *b, uint32_t version, uint32_t esz) {
    static const char *const names[] = {"common", "bf_00", "m1_tr_00"};
    memset(b, 0, 512);
    wl_wr_be32(b, WL_GS_TOC_MAGIC);
    wl_wr_be32(b + 4, version);
    wl_wr_be32(b + 8, 3);
    uint32_t noff = 0x20, nlen[3];
    uint8_t *names_at[3];
    for (int i = 0; i < 3; i++) {
        nlen[i] = (uint32_t)strlen(names[i]) + 1;
        names_at[i] = b + noff;
        memcpy(b + noff, names[i], nlen[i]);
        noff += nlen[i];
    }
    uint32_t table = (noff + 15) & ~15u;
    wl_wr_be32(b + 0x10, table);
    wl_wr_be32(b + 0x14, 0x20);
    uint32_t cur = 0x20;
    for (uint32_t i = 0; i < 3; i++) {
        uint8_t *e = b + table + i * esz;
        wl_wr_be32(e, 10 + i);
        wl_wr_be32(e + 4, cur);
        if (esz == 16) {
            wl_wr_be32(e + 8, 0x1000 * (i + 1));
            wl_wr_be32(e + 12, 7);
        }
        cur += nlen[i];
        (void)names_at;
    }
    return table + 3 * esz;
}

TEST toc_both_sizes(void) {
    uint8_t b[512];
    uint32_t ver[2] = {0x104, 0x201}, sz[2] = {8, 16};
    for (int k = 0; k < 2; k++) {
        size_t n = make_toc(b, ver[k], sz[k]);
        WlGsToc t;
        ASSERT_EQ(WL_OK, wl_gs_toc_parse(&t, b, n));
        ASSERT_EQ(sz[k], t.entry_size);
        ASSERT_EQ(3u, t.count);
        ASSERT_EQ(11u, t.entries[1].id);
        ASSERT_STR_EQ("bf_00", t.entries[1].name);
        ASSERT_STR_EQ("m1_tr_00", t.entries[2].name);
        ASSERT_EQ(sz[k] == 16 ? 0x2000u : 0u, t.entries[1].size);
        ASSERT_EQ(sz[k] == 16 ? 7u : 0u, t.entries[1].flags);
        wl_gs_toc_free(&t);
    }
    PASS();
}

TEST toc_malformed(void) {
    uint8_t b[512], c[512];
    size_t n = make_toc(b, 0x251, 16);
    WlGsToc t;
    for (size_t cut = 0; cut < n; cut++) {
        ASSERT(wl_gs_toc_parse(&t, b, cut) != WL_OK);
    }
    memcpy(c, b, n);
    c[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_toc_parse(&t, c, n));
    memcpy(c, b, n);
    wl_wr_be32(c + 8, 0x10000000u); /* count * 16 overflows 32 bits */
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_toc_parse(&t, c, n));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x10, 0xFFFFFFF8u); /* table offset wraps */
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_toc_parse(&t, c, n));
    memcpy(c, b, n);
    uint32_t table = wl_rd_be32(c + 0x10);
    wl_wr_be32(c + table + 4, 4); /* name offset inside the header */
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_toc_parse(&t, c, n));
    memcpy(c, b, n);
    wl_wr_be32(c + table + 4, (uint32_t)n - 1); /* name starts on the last byte (7), no NUL */
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_toc_parse(&t, c, n));
    PASS();
}

/* ---- FSYS ---------------------------------------------------------------------------------- */
#define FS_MAX 8192
static uint8_t g_raw[100];
static uint8_t g_big[1500];

/* Entries: 0 raw type 6 named "(null)", 1 LZSS type 2 unnamed, 2 raw type 40 named "x". */
static size_t make_fsys(uint8_t *b) {
    memset(b, 0, FS_MAX);
    wl_wr_be32(b, WL_GS_FSYS_MAGIC);
    wl_wr_be32(b + 0x0C, 3);
    wl_wr_be32(b + 0x40, 0x60);
    wl_wr_be32(b + 0x44, 0x200);
    memcpy(b + 0x200, "(null)", 7);
    memcpy(b + 0x210, "x", 2);
    pattern(g_raw, sizeof g_raw);
    pattern(g_big, sizeof g_big);
    uint32_t data = 0x300;
    memcpy(b + data, g_raw, sizeof g_raw);
    uint32_t d1 = data + 0x80;
    size_t pn = make_lzss(g_big, sizeof g_big, b + d1);
    uint32_t d2 = (uint32_t)((d1 + pn + 15) & ~15u);
    memcpy(b + d2, g_raw, 50);
    const uint32_t rec[3] = {0x100, 0x140, 0x180};
    for (int i = 0; i < 3; i++) {
        wl_wr_be32(b + 0x60 + 4 * i, rec[i]);
    }
    uint8_t *r = b + rec[0];
    wl_wr_be32(r, 1); wl_wr_be32(r + 4, data); wl_wr_be32(r + 8, sizeof g_raw);
    wl_wr_be32(r + 0x14, sizeof g_raw); wl_wr_be32(r + 0x20, 6); wl_wr_be32(r + 0x24, 0x200);
    r = b + rec[1];
    wl_wr_be32(r, 2); wl_wr_be32(r + 4, d1); wl_wr_be32(r + 8, sizeof g_big);
    wl_wr_be32(r + 0x0C, WL_GS_FLAG_COMPRESSED); wl_wr_be32(r + 0x14, (uint32_t)pn);
    wl_wr_be32(r + 0x20, 2);
    r = b + rec[2];
    wl_wr_be32(r, 3); wl_wr_be32(r + 4, d2); wl_wr_be32(r + 8, 50);
    wl_wr_be32(r + 0x14, 50); wl_wr_be32(r + 0x20, 40); wl_wr_be32(r + 0x24, 0x210);
    return d2 + 50;
}

static WlResult cb_read(void *ctx, uint64_t off, void *buf, size_t n) {
    memcpy(buf, (const uint8_t *)ctx + off, n);
    return WL_OK;
}

TEST fsys_open_read(void) {
    static uint8_t b[FS_MAX];
    size_t n = make_fsys(b);
    WlGsSrc s;
    wl_gs_src_mem(&s, b, n);
    WlGsFsys f;
    ASSERT_EQ(WL_OK, wl_gs_fsys_open(&f, &s));
    ASSERT_EQ(3u, f.count);
    ASSERT_STR_EQ("(null)", f.entries[0].name);
    ASSERT_STR_EQ("", f.entries[1].name);
    ASSERT_STR_EQ("x", f.entries[2].name);
    ASSERT_EQ(6u, f.entries[0].type);
    ASSERT(!f.entries[0].is_lzss);
    ASSERT(f.entries[1].is_lzss);
    uint8_t *d = NULL;
    size_t len = 0;
    ASSERT_EQ(WL_OK, wl_gs_fsys_read(&f, 0, &d, &len));
    ASSERT_EQ(sizeof g_raw, len);
    ASSERT_MEM_EQ(g_raw, d, len);
    free(d);
    ASSERT_EQ(WL_OK, wl_gs_fsys_read(&f, 1, &d, &len));
    ASSERT_EQ(sizeof g_big, len);
    ASSERT_MEM_EQ(g_big, d, len);
    free(d);
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(WL_OK, wl_gs_fsys_verify(&f, i));
    }
    ASSERT_EQ(WL_ERR_ARG, wl_gs_fsys_verify(&f, 3));
    wl_gs_fsys_close(&f);
    /* the same archive as a sub-range of a larger medium, read through the callback source */
    static uint8_t big[FS_MAX + 100];
    memset(big, 0xEE, sizeof big);
    memcpy(big + 100, b, n);
    WlGsSrc cs;
    wl_gs_src_cb(&cs, cb_read, big, 100, n);
    ASSERT_EQ(WL_OK, wl_gs_fsys_open(&f, &cs));
    ASSERT_EQ(3u, f.count);
    ASSERT_STR_EQ("x", f.entries[2].name);
    ASSERT_EQ(WL_OK, wl_gs_fsys_read(&f, 1, &d, &len));
    ASSERT_EQ(sizeof g_big, len);
    ASSERT_MEM_EQ(g_big, d, len);
    free(d);
    wl_gs_fsys_close(&f);
    wl_gs_src_cb(&cs, NULL, NULL, 0, n);
    ASSERT_EQ(WL_ERR_ARG, wl_gs_src_read(&cs, 0, big, 4));
    PASS();
}

TEST fsys_malformed(void) {
    static uint8_t b[FS_MAX], c[FS_MAX];
    size_t n = make_fsys(b);
    WlGsFsys f;
    WlGsSrc s;
    for (size_t cut = 0; cut < n; cut += 7) { /* truncations: open or verify may fail, never crash */
        wl_gs_src_mem(&s, b, cut);
        if (wl_gs_fsys_open(&f, &s) == WL_OK) {
            for (uint32_t i = 0; i < f.count; i++) {
                (void)wl_gs_fsys_verify(&f, i);
            }
            wl_gs_fsys_close(&f);
        }
    }
    memcpy(c, b, n);
    c[0] = 'X';
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_open(&f, &s));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x0C, 0x40000000u); /* count*4 overflows */
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_open(&f, &s));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x40, (uint32_t)n); /* table past the end */
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_open(&f, &s));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x60, 0xFFFFFFF0u); /* record offset out of range */
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_open(&f, &s));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x100 + 4, 0xFFFFFFF0u); /* data offset wraps */
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_open(&f, &s));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x100 + 0x24, (uint32_t)n + 5); /* name offset outside */
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_open(&f, &s));
    memcpy(c, b, n);
    wl_wr_be32(c + 0x140 + 8, 1499); /* unpacked size disagrees with the LZSS header */
    wl_gs_src_mem(&s, c, n);
    ASSERT_EQ(WL_OK, wl_gs_fsys_open(&f, &s));
    ASSERT_EQ(WL_ERR_FORMAT, wl_gs_fsys_verify(&f, 1));
    wl_gs_fsys_close(&f);
    /* flipping every byte of the header and tables must never crash */
    for (size_t i = 0; i < 0x1C0; i++) {
        memcpy(c, b, n);
        c[i] ^= 0xA5;
        wl_gs_src_mem(&s, c, n);
        if (wl_gs_fsys_open(&f, &s) == WL_OK) {
            for (uint32_t k = 0; k < f.count; k++) {
                (void)wl_gs_fsys_verify(&f, k);
            }
            wl_gs_fsys_close(&f);
        }
    }
    PASS();
}

TEST type_names(void) {
    char buf[24];
    ASSERT_STR_EQ("scene", wl_gs_type_name(WL_GS_GAME_PBR, 2, buf));
    ASSERT_STR_EQ("texture", wl_gs_type_name(WL_GS_GAME_PBR, 6, buf));
    ASSERT_STR_EQ("type_02", wl_gs_type_name(WL_GS_GAME_XD, 2, buf));
    ASSERT_STR_EQ("type_26", wl_gs_type_name(WL_GS_GAME_PBR, 26, buf));
    ASSERT_STR_EQ("type_out_of_range", wl_gs_type_name(WL_GS_GAME_PBR, 27, buf));
    ASSERT_EQ(WL_GS_GAME_COLOSSEUM, wl_gs_game_from_toc_version(0x104));
    ASSERT_EQ(WL_GS_GAME_XD, wl_gs_game_from_toc_version(0x201));
    ASSERT_EQ(WL_GS_GAME_PBR, wl_gs_game_from_toc_version(0x251));
    PASS();
}

SUITE(gs_suite) {
    RUN_TEST(lzss_roundtrip);
    RUN_TEST(lzss_malformed);
    RUN_TEST(toc_both_sizes);
    RUN_TEST(toc_malformed);
    RUN_TEST(fsys_open_read);
    RUN_TEST(fsys_malformed);
    RUN_TEST(type_names);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gs_suite);
    GREATEST_MAIN_END();
}
