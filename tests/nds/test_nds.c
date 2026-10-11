// SPDX-License-Identifier: MIT
// NDS readers with synthetic images only: header, NitroFS tree, overlays, NARC, LZ10/LZ11, BLZ,
// and malformed inputs (every one must return an error, never crash).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/nds/lz.h"
#include "wl/nds/nds.h"
#include "greatest.h"

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}

/* ---- test-side compressors (the library only decodes) ---- */
static size_t lz_encode(int lz11, const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, i = 0;
    out[o++] = lz11 ? 0x11 : 0x10;
    out[o++] = (uint8_t)n;
    out[o++] = (uint8_t)(n >> 8);
    out[o++] = (uint8_t)(n >> 16);
    const size_t maxlen = lz11 ? 0x10110u : 18u;
    while (i < n) {
        size_t fpos = o++;
        unsigned flags = 0;
        for (int b = 0; b < 8 && i < n; b++) {
            size_t best = 0, bdisp = 0;
            for (size_t d = 1; d <= 4096 && d <= i; d++) {
                size_t l = 0;
                while (l < maxlen && i + l < n && in[i + l - d] == in[i + l]) {
                    l++;
                }
                if (l > best) {
                    best = l;
                    bdisp = d;
                }
            }
            if (best >= 3) {
                flags |= 0x80u >> b;
                size_t dd = bdisp - 1;
                if (!lz11) {
                    out[o++] = (uint8_t)(((best - 3) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                } else if (best <= 16) {
                    out[o++] = (uint8_t)(((best - 1) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                } else if (best <= 0x110) {
                    size_t v = best - 0x11;
                    out[o++] = (uint8_t)(v >> 4);
                    out[o++] = (uint8_t)(((v & 15) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                } else {
                    size_t v = best - 0x111;
                    out[o++] = (uint8_t)(0x10 | (v >> 12));
                    out[o++] = (uint8_t)(v >> 4);
                    out[o++] = (uint8_t)(((v & 15) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                }
                i += best;
            } else {
                out[o++] = in[i++];
            }
        }
        out[fpos] = (uint8_t)flags;
    }
    return o;
}

static size_t make_sample(uint8_t *d, size_t n) {
    uint32_t x = 12345;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        if (i < 600) {
            d[i] = (uint8_t)(x >> 24); /* noise */
        } else if (i < 5600) {
            d[i] = 0; /* long run: exercises the long LZ11 forms */
        } else {
            d[i] = (uint8_t)("wiilibs"[i % 7]); /* short period */
        }
    }
    return n;
}

static int lz_roundtrip(int lz11) {
    enum { N = 9000 };
    uint8_t in[N], *enc = (uint8_t *)malloc(N * 2), *dec = NULL;
    size_t dl = 0;
    make_sample(in, N);
    size_t el = lz_encode(lz11, in, N, enc);
    WlResult r = wl_lz_decode(enc, el, 1u << 20, &dec, &dl);
    int ok = r == WL_OK && dl == N && memcmp(dec, in, N) == 0 && el < N;
    /* truncation at every length below the full stream must fail, not crash */
    for (size_t t = 0; ok && t < el - 4; t += 7) {
        uint8_t *x = NULL;
        size_t xl = 0;
        if (wl_lz_decode(enc, t, 1u << 20, &x, &xl) == WL_OK) {
            ok = 0;
        }
        free(x);
    }
    free(dec);
    free(enc);
    return ok;
}

TEST lz10_round_trip(void) {
    ASSERT(lz_roundtrip(0));
    PASS();
}
TEST lz11_round_trip(void) {
    ASSERT(lz_roundtrip(1));
    PASS();
}

TEST lz_extended_size_and_limits(void) {
    /* LZ10, 24-bit size 0 means a 32-bit size follows: 4 literals */
    const uint8_t s[] = {0x10, 0, 0, 0, 4, 0, 0, 0, 0x00, 'a', 'b', 'c', 'd'};
    uint8_t *o = NULL;
    size_t ol = 0;
    ASSERT_EQ(WL_OK, wl_lz_decode(s, sizeof s, 100, &o, &ol));
    ASSERT_EQ(4, (int)ol);
    ASSERT_EQ(0, memcmp(o, "abcd", 4));
    free(o);
    ASSERT_EQ(WL_ERR_RANGE, wl_lz_decode(s, sizeof s, 3, &o, &ol)); /* over max_out */
    const uint8_t bad_type[] = {0x30, 4, 0, 0, 0, 'a', 'b', 'c', 'd'};
    ASSERT_EQ(WL_ERR_FORMAT, wl_lz_decode(bad_type, sizeof bad_type, 100, &o, &ol));
    /* a match whose displacement reaches before the start of the output */
    const uint8_t bad_disp[] = {0x10, 8, 0, 0, 0x80, 0x00, 0x05};
    ASSERT_EQ(WL_ERR_FORMAT, wl_lz_decode(bad_disp, sizeof bad_disp, 100, &o, &ol));
    ASSERT_EQ(WL_ERR_FORMAT, wl_lz_decode(s, 3, 100, &o, &ol));
    ASSERT(!wl_lz_looks_valid((const uint8_t *)"abcdefgh", 8));
    PASS();
}

/* Hand-built BLZ: 3 stored bytes, then (read from the end) flags, 3 literals, one match.
 * Output has 24 bytes: "xyz" followed by the period-3 pattern ending in "abc". */
static size_t make_blz(uint8_t *b) {
    const uint8_t rd[] = {0x10 /* flags: L L L M */, 'c', 'b', 'a', 0xF0, 0x00 /* len 18, disp 3 */};
    size_t o = 0;
    b[o++] = 'x';
    b[o++] = 'y';
    b[o++] = 'z';
    for (size_t i = sizeof rd; i-- > 0;) { /* the stream is consumed backward */
        b[o++] = rd[i];
    }
    put32(b + o, (uint32_t)(sizeof rd + 8)); /* compressed length incl. footer, header len 0 */
    b[o + 3] = 8;                           /* header (footer) length in the top byte */
    put32(b + o + 4, 24u - (uint32_t)(o + 8));
    return o + 8;
}

TEST blz_hand_built(void) {
    uint8_t b[64];
    size_t n = make_blz(b);
    ASSERT_EQ(17, (int)n);
    ASSERT(wl_blz_footer_ok(b, n));
    uint8_t *o = NULL;
    size_t ol = 0;
    ASSERT_EQ(WL_OK, wl_blz_decode(b, n, 1000, &o, &ol));
    ASSERT_EQ(24, (int)ol);
    ASSERT_EQ(0, memcmp(o, "xyz", 3));
    for (size_t i = 3; i < 24; i++) {
        ASSERT_EQ((uint8_t)"abc"[i % 3], o[i]);
    }
    free(o);
    ASSERT_EQ(WL_ERR_RANGE, wl_blz_decode(b, n, 10, &o, &ol));
    /* corrupt: a size increment that does not match the stream, and truncation */
    b[n - 4]++;
    ASSERT_EQ(WL_ERR_FORMAT, wl_blz_decode(b, n, 1000, &o, &ol));
    ASSERT(!wl_blz_footer_ok(b, 4));
    for (size_t t = 0; t < n; t++) {
        uint8_t c[64];
        memcpy(c, b, t);
        if (wl_blz_decode(c, t, 1000, &o, &ol) == WL_OK) {
            free(o);
        }
    }
    PASS();
}

/* ---- ROM builder ---- */
typedef struct {
    uint8_t *d;
    size_t size;
    size_t fnt_off, fat_off;
} Rom;

static const char *g_file_body[4] = {"AAAA", "BBBBBB", "CC", "ZZZZZZZZ"};

/* root: a.bin(0) sub/ z.txt(1);  sub: b.bin(2) deep/;  deep: c.bin(3). Optionally one overlay. */
static void build_rom(Rom *r, int with_overlay) {
    r->size = 0x4000;
    r->d = (uint8_t *)calloc(1, r->size);
    uint8_t *d = r->d;
    memcpy(d + 0x0C, "TEST", 4);
    r->fnt_off = 0x200;
    r->fat_off = 0x400;
    uint8_t *f = d + r->fnt_off;
    /* main table: 3 directories; sub-tables follow at 24 (root), 43 (sub) and 57 (deep) */
    put32(f + 0, 24); put16(f + 4, 0); put16(f + 6, 3);
    put32(f + 8, 43); put16(f + 12, 2); put16(f + 14, 0xF000);
    put32(f + 16, 57); put16(f + 20, 3); put16(f + 22, 0xF001);
    size_t p = 24;
    f[p++] = 5; memcpy(f + p, "a.bin", 5); p += 5;
    f[p++] = 0x83; memcpy(f + p, "sub", 3); p += 3; put16(f + p, 0xF001); p += 2;
    f[p++] = 5; memcpy(f + p, "z.txt", 5); p += 5;
    f[p++] = 0;
    f[p++] = 5; memcpy(f + p, "b.bin", 5); p += 5;
    f[p++] = 0x84; memcpy(f + p, "deep", 4); p += 4; put16(f + p, 0xF002); p += 2;
    f[p++] = 0;
    f[p++] = 5; memcpy(f + p, "c.bin", 5); p += 5;
    f[p++] = 0;
    put32(d + 0x40, (uint32_t)r->fnt_off); put32(d + 0x44, (uint32_t)p);
    /* FAT: 4 files + 1 overlay file at 0x1000.. */
    uint32_t pos = 0x1000;
    for (int i = 0; i < 4; i++) {
        size_t l = strlen(g_file_body[i]);
        memcpy(d + pos, g_file_body[i], l);
        put32(d + r->fat_off + (size_t)i * 8, pos);
        put32(d + r->fat_off + (size_t)i * 8 + 4, pos + (uint32_t)l);
        pos += 0x20;
    }
    put32(d + 0x48, (uint32_t)r->fat_off);
    put32(d + 0x4C, with_overlay ? 40 : 32);
    if (with_overlay) {
        uint8_t blz[64];
        size_t bl = make_blz(blz);
        memcpy(d + 0x2000, blz, bl);
        put32(d + r->fat_off + 32, 0x2000);
        put32(d + r->fat_off + 36, 0x2000 + (uint32_t)bl);
        uint8_t *ov = d + 0x600;
        put32(ov, 0); put32(ov + 4, 0x02000000); put32(ov + 8, 24); put32(ov + 24, 4);
        put32(ov + 28, (uint32_t)bl | 0x01000000u);
        put32(d + 0x50, 0x600);
        put32(d + 0x54, 32);
    }
    put32(d + 0x20, 0x800); put32(d + 0x2C, 0x100); /* arm9 */
    put32(d + 0x30, 0xA00); put32(d + 0x3C, 0x100); /* arm7 */
}

typedef struct {
    int n;
    char path[8][64];
    uint32_t id[8];
    size_t size[8];
    char body[8][16];
} Seen;

static WlResult collect(void *ud, const char *path, uint32_t id, const uint8_t *d, size_t sz) {
    Seen *s = (Seen *)ud;
    if (s->n >= 8) {
        return WL_ERR_FAILED;
    }
    snprintf(s->path[s->n], 64, "%s", path);
    s->id[s->n] = id;
    s->size[s->n] = sz;
    memcpy(s->body[s->n], d, sz < 15 ? sz : 15);
    s->n++;
    return WL_OK;
}

TEST rom_walk_nested(void) {
    Rom r;
    build_rom(&r, 0);
    WlNds n;
    ASSERT_EQ(WL_OK, wl_nds_open(&n, r.d, r.size));
    ASSERT_EQ(4, (int)n.file_count);
    ASSERT_STR_EQ("TEST", n.game_code);
    Seen s = {0};
    ASSERT_EQ(WL_OK, wl_nds_walk(&n, collect, &s));
    ASSERT_EQ(4, s.n);
    ASSERT_STR_EQ("a.bin", s.path[0]);
    ASSERT_STR_EQ("sub/b.bin", s.path[1]);
    ASSERT_STR_EQ("sub/deep/c.bin", s.path[2]);
    ASSERT_STR_EQ("z.txt", s.path[3]);
    ASSERT_EQ(2, (int)s.id[1]);
    ASSERT_EQ(3, (int)s.id[2]);
    ASSERT_EQ(1, (int)s.id[3]);
    ASSERT_EQ(2, (int)s.size[1]); /* id 2 */
    ASSERT_EQ(0, memcmp(s.body[3], "BBBBBB", 6)); /* z.txt is id 1 */
    free(r.d);
    PASS();
}

TEST rom_survey_with_overlay(void) {
    Rom r;
    build_rom(&r, 1);
    WlNds n;
    ASSERT_EQ(WL_OK, wl_nds_open(&n, r.d, r.size));
    ASSERT_EQ(1, (int)n.ov9_count);
    WlNdsOverlay ov;
    ASSERT_EQ(WL_OK, wl_nds_overlay(&n, 1, 0, &ov));
    ASSERT(ov.compressed);
    ASSERT_EQ(4, (int)ov.file_id);
    ASSERT_EQ(WL_ERR_RANGE, wl_nds_overlay(&n, 1, 1, &ov));
    WlNdsSurvey sv;
    ASSERT_EQ(WL_OK, wl_nds_survey(&n, 1, 1, NULL, &sv));
    ASSERT_EQ(4, (int)sv.files);
    ASSERT_EQ(1, (int)sv.overlays);
    ASSERT_EQ(1, (int)sv.blz_ok);
    ASSERT_EQ(0, (int)sv.failures);
    free(r.d);
    PASS();
}

TEST rom_malformed(void) {
    Rom r;
    build_rom(&r, 0);
    WlNds n;
    ASSERT_EQ(WL_ERR_FORMAT, wl_nds_open(&n, r.d, 0x100));      /* shorter than a header */
    ASSERT_EQ(WL_ERR_RANGE, wl_nds_open(&n, r.d, 0x300));       /* FAT beyond the image */
    put32(r.d + 0x4C, 33);                                          /* FAT size not a multiple of 8 */
    ASSERT_EQ(WL_ERR_FORMAT, wl_nds_open(&n, r.d, r.size));
    put32(r.d + 0x4C, 32);
    put32(r.d + r.fat_off + 12, 0xFFFFFFF0u);                       /* file end out of range */
    ASSERT_EQ(WL_OK, wl_nds_open(&n, r.d, r.size));
    Seen s = {0};
    ASSERT_EQ(WL_ERR_RANGE, wl_nds_walk(&n, collect, &s));
    put32(r.d + r.fat_off + 12, 0x1000 + 4);
    put32(r.d + r.fat_off + 8, 0x1010);                             /* end before start */
    s.n = 0;
    ASSERT_EQ(WL_ERR_RANGE, wl_nds_walk(&n, collect, &s));
    ASSERT_EQ(WL_ERR_RANGE, wl_nds_file(&n, 99, &(const uint8_t *){0}, &(size_t){0}));
    free(r.d);
    PASS();
}

TEST fnt_cycle_and_depth(void) {
    /* a directory that lists itself */
    uint8_t cyc[40] = {0};
    put32(cyc, 8); put16(cyc + 4, 0); put16(cyc + 6, 1);
    cyc[8] = 0x81; cyc[9] = 'd'; put16(cyc + 10, 0xF000); cyc[12] = 0;
    uint8_t rom[0x400] = {0};
    memcpy(rom + 0x200, cyc, sizeof cyc);
    put32(rom + 0x40, 0x200); put32(rom + 0x44, sizeof cyc);
    put32(rom + 0x48, 0x300); put32(rom + 0x4C, 0);
    WlNds n;
    ASSERT_EQ(WL_OK, wl_nds_open(&n, rom, sizeof rom));
    Seen s = {0};
    ASSERT_EQ(WL_ERR_FORMAT, wl_nds_walk(&n, collect, &s));
    /* a chain deeper than the cap: 40 nested directories */
    enum { N = 40 };
    static uint8_t big[0x2000];
    memset(big, 0, sizeof big);
    uint8_t *f = big + 0x200;
    size_t sub = (size_t)N * 8;
    for (int i = 0; i < N; i++) {
        put32(f + (size_t)i * 8, (uint32_t)sub);
        put16(f + (size_t)i * 8 + 4, 0);
        put16(f + (size_t)i * 8 + 6, i == 0 ? N : (unsigned)(0xF000 + i - 1));
        if (i + 1 < N) {
            f[sub++] = 0x81; f[sub++] = 'd'; put16(f + sub, (unsigned)(0xF000 + i + 1)); sub += 2;
        }
        f[sub++] = 0;
    }
    put32(big + 0x40, 0x200); put32(big + 0x44, (uint32_t)sub);
    put32(big + 0x48, 0x1800); put32(big + 0x4C, 0);
    ASSERT_EQ(WL_OK, wl_nds_open(&n, big, sizeof big));
    ASSERT_EQ(WL_ERR_FORMAT, wl_nds_walk(&n, collect, &s));
    /* a directory id outside the table */
    put16(cyc + 10, 0xF005);
    memcpy(rom + 0x200, cyc, sizeof cyc);
    ASSERT_EQ(WL_OK, wl_nds_open(&n, rom, sizeof rom));
    ASSERT_EQ(WL_ERR_FORMAT, wl_nds_walk(&n, collect, &s));
    PASS();
}

/* ---- NARC builder ---- */
static size_t make_narc(uint8_t *b, int named) {
    const char *body[3] = {"one", "twotwo", "3"};
    size_t o = 16;
    memcpy(b, "NARC", 4);
    put16(b + 4, 0xFFFE); put16(b + 6, 0x0100); put16(b + 12, 16); put16(b + 14, 3);
    memcpy(b + o, "BTAF", 4); put32(b + o + 4, 12 + 24); put16(b + o + 8, 3);
    uint32_t pos = 0;
    for (int i = 0; i < 3; i++) {
        put32(b + o + 12 + (size_t)i * 8, pos);
        pos += (uint32_t)strlen(body[i]);
        put32(b + o + 12 + (size_t)i * 8 + 4, pos);
        pos = (pos + 3u) & ~3u;
    }
    o += 36;
    size_t bt = o;
    memcpy(b + o, "BTNF", 4);
    o += 8;
    if (!named) {
        put32(b + o, 4); put16(b + o + 4, 0); put16(b + o + 6, 1);
        o += 8;
    } else {
        put32(b + o, 8); put16(b + o + 4, 0); put16(b + o + 6, 1);
        size_t q = o + 8;
        const char *nm[3] = {"x.a", "yy.b", "z"};
        for (int i = 0; i < 3; i++) {
            b[q++] = (uint8_t)strlen(nm[i]);
            memcpy(b + q, nm[i], strlen(nm[i]));
            q += strlen(nm[i]);
        }
        b[q++] = 0;
        while ((q - bt) % 4) { b[q++] = 0; }
        o = q;
    }
    put32(b + bt + 4, (uint32_t)(o - bt));
    size_t gm = o;
    memcpy(b + o, "GMIF", 4);
    o += 8;
    pos = 0;
    for (int i = 0; i < 3; i++) {
        memcpy(b + o + pos, body[i], strlen(body[i]));
        pos = (pos + (uint32_t)strlen(body[i]) + 3u) & ~3u;
    }
    o += pos;
    put32(b + gm + 4, (uint32_t)(o - gm));
    put32(b + 8, (uint32_t)o);
    return o;
}

TEST narc_nameless(void) {
    uint8_t b[256] = {0};
    size_t n = make_narc(b, 0);
    WlNarc a;
    ASSERT_EQ(WL_OK, wl_narc_open(&a, b, n));
    ASSERT_EQ(3, (int)a.count);
    ASSERT(!a.has_names);
    const uint8_t *d;
    size_t sz;
    ASSERT_EQ(WL_OK, wl_narc_member(&a, 1, &d, &sz));
    ASSERT_EQ(6, (int)sz);
    ASSERT_EQ(0, memcmp(d, "twotwo", 6));
    ASSERT_EQ(WL_ERR_RANGE, wl_narc_member(&a, 3, &d, &sz));
    Seen s = {0};
    ASSERT_EQ(WL_OK, wl_narc_walk(&a, collect, &s));
    ASSERT_EQ(0, s.n);
    PASS();
}

TEST narc_named(void) {
    uint8_t b[256] = {0};
    size_t n = make_narc(b, 1);
    WlNarc a;
    ASSERT_EQ(WL_OK, wl_narc_open(&a, b, n));
    ASSERT(a.has_names);
    Seen s = {0};
    ASSERT_EQ(WL_OK, wl_narc_walk(&a, collect, &s));
    ASSERT_EQ(3, s.n);
    ASSERT_STR_EQ("yy.b", s.path[1]);
    ASSERT_EQ(6, (int)s.size[1]);
    ASSERT_EQ(0, memcmp(s.body[2], "3", 1));
    PASS();
}

TEST narc_malformed(void) {
    uint8_t b[256] = {0}, c[256];
    size_t n = make_narc(b, 0);
    WlNarc a;
    for (size_t t = 0; t < n; t++) { /* every truncation is an error or at least safe */
        memcpy(c, b, t);
        if (wl_narc_open(&a, c, t) == WL_OK) {
            const uint8_t *d;
            size_t sz;
            for (uint32_t i = 0; i < a.count; i++) {
                ASSERT_EQ(WL_OK, wl_narc_member(&a, i, &d, &sz));
            }
        }
    }
    ASSERT_EQ(WL_ERR_FORMAT, wl_narc_open(&a, b, 10));
    memcpy(c, b, n);
    put16(c + 4, 0x1234); /* bad byte-order mark */
    ASSERT_EQ(WL_ERR_FORMAT, wl_narc_open(&a, c, n));
    memcpy(c, b, n);
    put32(c + 16 + 12 + 4, 0x7000); /* member end beyond the data section */
    ASSERT_EQ(WL_ERR_RANGE, wl_narc_open(&a, c, n));
    memcpy(c, b, n);
    put32(c + 16 + 4, 0xFFFFFFF0u); /* section size overflows */
    ASSERT_EQ(WL_ERR_RANGE, wl_narc_open(&a, c, n));
    memcpy(c, b, n);
    put16(c + 16 + 8, 0xFFFF); /* count larger than the section */
    ASSERT_EQ(WL_ERR_RANGE, wl_narc_open(&a, c, n));
    PASS();
}

SUITE(nds_suite) {
    RUN_TEST(lz10_round_trip);
    RUN_TEST(lz11_round_trip);
    RUN_TEST(lz_extended_size_and_limits);
    RUN_TEST(blz_hand_built);
    RUN_TEST(rom_walk_nested);
    RUN_TEST(rom_survey_with_overlay);
    RUN_TEST(rom_malformed);
    RUN_TEST(fnt_cycle_and_depth);
    RUN_TEST(narc_nameless);
    RUN_TEST(narc_named);
    RUN_TEST(narc_malformed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(nds_suite);
    GREATEST_MAIN_END();
}
