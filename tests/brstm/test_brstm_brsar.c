// SPDX-License-Identifier: MIT
// BRSAR index with a synthetic archive built here (no game data): sounds of every type, a bank,
// players, internal, external and unlisted files, groups with items and labels; truncation, header
// corruption and random corruption of the INFO and SYMB chunks must fail cleanly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/base/endian.h"
#include "wl/brstm/brsar.h"
#ifdef WL_BRSAR_FILE_TEST
#include "wl/host/file.h"
#endif

static uint32_t g_seed = 987654321u;
static uint32_t rnd(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

typedef struct {
    uint8_t *b;
    size_t n, cap;
} Buf;

static size_t bgrow(Buf *x, size_t n) { /* appends n zero bytes (4-aligned), returns the offset */
    size_t off = (x->n + 3u) & ~(size_t)3u;
    if (off + n > x->cap) {
        x->cap = (off + n) * 2u;
        x->b = (uint8_t *)realloc(x->b, x->cap);
    }
    memset(x->b + x->n, 0, off + n - x->n);
    x->n = off + n;
    return off;
}

static void bref(Buf *x, size_t at, uint32_t marker, size_t target) {
    wl_wr_be32(x->b + at, marker);
    wl_wr_be32(x->b + at + 4, (uint32_t)target);
}

typedef struct {
    uint8_t *file;
    size_t len;
    size_t info_off, info_size, symb_off, symb_size;
} Sar;

/* 4 sounds (seq, stream, two waves), 1 bank, 2 players, 4 files (RWSD, RSEQ, one external, one unlisted),
 * 2 groups (the first with the two internal files as items), 3 labels. */
static void make_sar(Sar *a) {
    Buf in = {0};
    size_t hdr = bgrow(&in, 0x30);
    (void)hdr;
    /* tables */
    size_t ts = bgrow(&in, 4 + 8 * 4), tb = bgrow(&in, 4 + 8), tp = bgrow(&in, 4 + 16), tf = bgrow(&in, 4 + 8 * 4),
           tg = bgrow(&in, 4 + 16);
    bref(&in, 0, 0x01000000u, ts);
    bref(&in, 8, 0x01000000u, tb);
    bref(&in, 16, 0x01000000u, tp);
    bref(&in, 24, 0x01000000u, tf);
    bref(&in, 32, 0x01000000u, tg);
    wl_wr_be32(in.b + ts, 4);
    wl_wr_be32(in.b + tb, 1);
    wl_wr_be32(in.b + tp, 2);
    wl_wr_be32(in.b + tf, 4);
    wl_wr_be32(in.b + tg, 2);
    /* sounds */
    static const uint8_t types[4] = {1, 2, 3, 3};
    for (uint32_t i = 0; i < 4; i++) {
        size_t e = bgrow(&in, 0x28), info = bgrow(&in, 16);
        bref(&in, ts + 4 + 8 * i, 0x01000000u, e);
        wl_wr_be32(in.b + e, i);
        wl_wr_be32(in.b + e + 4, i == 0 ? 1u : (i == 1 ? 2u : 0u));
        wl_wr_be32(in.b + e + 8, i % 2);
        in.b[e + 0x14] = (uint8_t)(50 + i);
        in.b[e + 0x15] = 64;
        in.b[e + 0x16] = types[i];
        bref(&in, e + 0x18, 0x01000000u | ((uint32_t)types[i] << 16), info);
        wl_wr_be32(in.b + info, 100 + i);
        wl_wr_be32(in.b + info + 4, 200 + i);
        wl_wr_be32(in.b + info + 8, 300 + i);
        in.b[info + 12] = 0x40;
    }
    /* bank, players */
    size_t be = bgrow(&in, 12);
    bref(&in, tb + 4, 0x01000000u, be);
    wl_wr_be32(in.b + be, 2);
    wl_wr_be32(in.b + be + 4, 1);
    for (uint32_t i = 0; i < 2; i++) {
        size_t e = bgrow(&in, 16);
        bref(&in, tp + 4 + 8 * i, 0x01000000u, e);
        wl_wr_be32(in.b + e, 10 + i);
        in.b[e + 4] = (uint8_t)(2 + i);
        wl_wr_be32(in.b + e + 8, 0x1000);
    }
    /* files */
    static const char *ext = "sound/syn_a.brstm";
    for (uint32_t i = 0; i < 4; i++) {
        size_t e = bgrow(&in, 0x1C);
        bref(&in, tf + 4 + 8 * i, 0x01000000u, e);
        wl_wr_be32(in.b + e, 0x20 + i);
        wl_wr_be32(in.b + e + 4, i == 0 ? 0x100u : 0u);
        wl_wr_be32(in.b + e + 8, 0xFFFFFFFFu);
        if (i == 2) {
            size_t sp = bgrow(&in, 24);
            memcpy(in.b + sp, ext, strlen(ext) + 1);
            bref(&in, e + 0x0C, 0x01000000u, sp);
        }
        if (i < 2) { /* listed by group 0: (group, index) */
            size_t lst = bgrow(&in, 4 + 8), pr = bgrow(&in, 8);
            bref(&in, e + 0x14, 0x01000000u, lst);
            wl_wr_be32(in.b + lst, 1);
            bref(&in, lst + 4, 0x01000000u, pr);
            wl_wr_be32(in.b + pr, 0);
            wl_wr_be32(in.b + pr + 4, i);
        }
    }
    /* groups */
    static const uint32_t gdata[2][4] = {{0, 0x40, 0x100, 0x100}, {0, 0, 0, 0}};
    size_t group0 = 0;
    for (uint32_t g = 0; g < 2; g++) {
        size_t e = bgrow(&in, 0x28);
        if (g == 0) {
            group0 = e;
        }
        bref(&in, tg + 4 + 8 * g, 0x01000000u, e);
        wl_wr_be32(in.b + e, 20 + g);
        wl_wr_be32(in.b + e + 4, 0xFFFFFFFFu);
        wl_wr_be32(in.b + e + 0x14, gdata[g][1]);
        wl_wr_be32(in.b + e + 0x18, gdata[g][2]);
        wl_wr_be32(in.b + e + 0x1C, gdata[g][3]);
        uint32_t cnt = g == 0 ? 2u : 0u;
        size_t lst = bgrow(&in, 4 + 8 * cnt);
        bref(&in, e + 0x20, 0x01000000u, lst);
        wl_wr_be32(in.b + lst, cnt);
        for (uint32_t k = 0; k < cnt; k++) {
            size_t it = bgrow(&in, 24);
            bref(&in, lst + 4 + 8 * k, 0x01000000u, it);
            wl_wr_be32(in.b + it, k); /* file id */
            wl_wr_be32(in.b + it + 4, 0x20u * k);
            wl_wr_be32(in.b + it + 8, 0x20);
            wl_wr_be32(in.b + it + 12, 0x80u * k);
            wl_wr_be32(in.b + it + 16, 0x80);
        }
    }
    /* SYMB: three labels */
    Buf sy = {0};
    size_t sh = bgrow(&sy, 0x1C);
    (void)sh;
    size_t st = bgrow(&sy, 4 + 12);
    wl_wr_be32(sy.b, 0x53594D42u);
    wl_wr_be32(sy.b + 8, (uint32_t)(st - 8));
    wl_wr_be32(sy.b + st, 3);
    static const char *lab[3] = {"SE_ONE", "BGM_TWO", "X"};
    for (uint32_t i = 0; i < 3; i++) {
        size_t o = bgrow(&sy, 12);
        memcpy(sy.b + o, lab[i], strlen(lab[i]) + 1);
        wl_wr_be32(sy.b + st + 4 + 4 * i, (uint32_t)(o - 8));
    }
    size_t sy_size = (sy.n + 31u) & ~(size_t)31u, in_size = ((in.n + 8u) + 31u) & ~(size_t)31u;
    size_t file_off = 0x40 + sy_size + in_size, total = file_off + 0x20 + 0x100;
    a->file = (uint8_t *)calloc(1, total);
    a->len = total;
    uint8_t *f = a->file;
    memcpy(f, "RSAR", 4);
    wl_wr_be16(f + 4, 0xFEFF);
    wl_wr_be16(f + 6, 0x0103);
    wl_wr_be32(f + 8, (uint32_t)total);
    wl_wr_be16(f + 12, 0x40);
    wl_wr_be16(f + 14, 3);
    wl_wr_be32(f + 0x10, 0x40);
    wl_wr_be32(f + 0x14, (uint32_t)sy_size);
    wl_wr_be32(f + 0x18, (uint32_t)(0x40 + sy_size));
    wl_wr_be32(f + 0x1C, (uint32_t)in_size);
    wl_wr_be32(f + 0x20, (uint32_t)file_off);
    wl_wr_be32(f + 0x24, 0x120);
    memcpy(f + 0x40, sy.b, sy.n);
    wl_wr_be32(f + 0x44, (uint32_t)sy_size);
    memcpy(f + 0x40 + sy_size, "INFO", 4);
    wl_wr_be32(f + 0x40 + sy_size + 4, (uint32_t)in_size);
    memcpy(f + 0x40 + sy_size + 8, in.b, in.n);
    memcpy(f + file_off, "FILE", 4);
    wl_wr_be32(f + file_off + 4, 0x120);
    /* group 0 starts at file_off + 0x20: item 0 an RWSD, item 1 an RSEQ */
    wl_wr_be32(f + file_off + 0x20, 0x52575344u);
    wl_wr_be32(f + file_off + 0x40, 0x52534551u);
    /* group offsets are absolute */
    wl_wr_be32(f + 0x40 + sy_size + 8 + group0 + 0x10, (uint32_t)(file_off + 0x20));
    a->symb_off = 0x40;
    a->symb_size = sy_size;
    a->info_off = 0x40 + sy_size;
    a->info_size = in_size;
    free(in.b);
    free(sy.b);
}

static WlResult open_sar(const uint8_t *file, size_t len, WlBrsar *out) {
    return wl_brsar_open_mem(file, len, out);
}

typedef struct {
    const uint8_t *p;
    size_t n;
    unsigned reads;
} MemSrc;

static WlResult mem_read(void *user, uint64_t off, void *dst, size_t n) {
    MemSrc *m = (MemSrc *)user;
    m->reads++;
    if (off > m->n || m->n - off < n) {
        return WL_ERR_RANGE;
    }
    memcpy(dst, m->p + off, n);
    return WL_OK;
}

TEST brsar_index(void) {
    Sar s;
    make_sar(&s);
    WlBrsar a;
    ASSERT_EQ(WL_OK, open_sar(s.file, s.len, &a));
    ASSERT_EQ(0x0103u, a.version);
    ASSERT_EQ((uint64_t)s.len, a.archive_size);
    ASSERT_EQ(4u, a.n_snd);
    ASSERT_EQ(1u, a.snd_seq);
    ASSERT_EQ(1u, a.snd_stream);
    ASSERT_EQ(2u, a.snd_wave);
    ASSERT_EQ(0u, a.info_type_mismatch);
    ASSERT_EQ(1u, a.n_bank);
    ASSERT_EQ(2u, a.bank[0].string_id);
    ASSERT_EQ(2u, a.n_player);
    ASSERT_EQ(3u, a.player[1].playable);
    ASSERT_EQ(0x1000u, a.player[1].heap);
    ASSERT_EQ(4u, a.n_file);
    ASSERT_EQ(1u, a.files_ext);
    ASSERT_EQ(1u, a.files_rwsd);
    ASSERT_EQ(1u, a.files_rseq);
    ASSERT_EQ(1u, a.files_unstored);
    ASSERT_EQ(2u, a.n_grp);
    ASSERT_EQ(2u, a.n_item);
    ASSERT_EQ(2u, a.grp[0].item_count);
    ASSERT_EQ(1u, a.wave_data_groups);
    ASSERT_EQ(0x100u, (unsigned)a.wave_data_bytes);
    ASSERT_EQ(3u, a.n_str);
    ASSERT_STR_EQ("BGM_TWO", wl_brsar_string(&a, 1));
    ASSERT_STR_EQ("X", wl_brsar_string(&a, 2));
    ASSERT(wl_brsar_string(&a, 3) == NULL);
    ASSERT_EQ(WL_BRSAR_SOUND_STREAM, a.snd[1].type);
    ASSERT_EQ(101u, a.snd[1].info0);
    ASSERT_EQ(WL_BRSAR_SOUND_SEQ, a.snd[0].type);
    ASSERT_EQ(200u, a.snd[0].info1);
    ASSERT_EQ(300u, a.snd[0].info2);
    ASSERT_EQ(0x40u, a.snd[0].chan_prio);
    ASSERT_EQ(WL_BRSAR_MAGIC_RWSD, a.file[0].magic);
    ASSERT_EQ(WL_BRSAR_MAGIC_RSEQ, a.file[1].magic);
    ASSERT_EQ(1u, a.file[2].ext);
    ASSERT_STR_EQ("sound/syn_a.brstm", a.file[2].path);
    uint32_t fid = 99, sid = 99;
    ASSERT(wl_brsar_find_ext(&a, "sound/syn_a.brstm", &fid, &sid));
    ASSERT_EQ(2u, fid);
    ASSERT_EQ(1u, sid); /* sound 1 plays file 2 */
    ASSERT(!wl_brsar_find_ext(&a, "sound/none.brstm", &fid, &sid));
    ASSERT_EQ(0u, a.file[0].first_group);
    ASSERT_EQ(1u, a.file[1].first_index);
    ASSERT_EQ(WL_BRSAR_NONE, a.file[3].first_group);
    /* where the files live: group 0 at FILE + 0x20, item 1 at +0x20 in it, wave data at 0x100 + 0x80 */
    uint64_t off, woff;
    uint32_t sz, wsz;
    ASSERT_EQ(WL_OK, wl_brsar_file_where(&a, 1, &off, &sz, &woff, &wsz));
    ASSERT_EQ(0x52534551u, wl_rd_be32(s.file + off));
    ASSERT_EQ(0x20u, sz);
    ASSERT_EQ(0x100u + 0x80u, (unsigned)woff);
    ASSERT_EQ(0x80u, wsz);
    ASSERT_EQ(WL_ERR_RANGE, wl_brsar_file_where(&a, 2, &off, &sz, &woff, &wsz)); /* external */
    ASSERT_EQ(WL_ERR_RANGE, wl_brsar_file_where(&a, 3, &off, &sz, &woff, &wsz)); /* in no group */
    ASSERT_EQ(WL_ERR_RANGE, wl_brsar_file_where(&a, 4, &off, &sz, &woff, &wsz));
    /* reading a file and its wave data through the callback */
    MemSrc ms = {s.file, s.len, 0};
    uint8_t *d, *w;
    size_t dn, wn;
    ASSERT_EQ(WL_OK, wl_brsar_read_file(&a, mem_read, &ms, 0, 1u << 20, &d, &dn, &w, &wn));
    ASSERT_EQ(0x20u, (unsigned)dn);
    ASSERT_EQ(0x80u, (unsigned)wn);
    ASSERT_MEM_EQ("RWSD", d, 4);
    ASSERT(w != NULL);
    free(d);
    free(w);
    ASSERT_EQ(WL_ERR_LIMIT, wl_brsar_read_file(&a, mem_read, &ms, 0, 0x10, &d, &dn, &w, &wn));
    ASSERT_EQ(WL_ERR_RANGE, wl_brsar_read_file(&a, mem_read, &ms, 2, 1u << 20, &d, &dn, &w, &wn));
    wl_brsar_free(&a);
    /* the callback opener reads only the header, SYMB, INFO and four bytes per item */
    ms.reads = 0;
    ASSERT_EQ(WL_OK, wl_brsar_open_cb(mem_read, &ms, s.len, &a));
    ASSERT_EQ(5u, ms.reads);
    ASSERT_EQ(4u, a.n_file);
    wl_brsar_free(&a);
    free(s.file);
    PASS();
}

TEST brsar_malformed(void) {
    Sar s;
    make_sar(&s);
    WlBrsar a;
    /* every truncation is refused */
    for (size_t n = 0; n < s.len; n += 1 + n / 8) {
        ASSERT(open_sar(s.file, n, &a) != WL_OK);
        ASSERT(a.snd == NULL && a.n_snd == 0u);
    }
    /* header corruption */
    const struct {
        size_t at;
        uint32_t v;
    } H[] = {{0, 0x52534152u + 1u}, {8, 7}, {0x10, 0xFFFFFF00u}, {0x14, 0}, {0x18, 0x20}, {0x1C, 4}, {0x20, 0xFFFFFFF0u},
             {0x44, 5}, {0x48, 0xFFFFFFF0u}};
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
        uint8_t *m = (uint8_t *)malloc(s.len);
        memcpy(m, s.file, s.len);
        wl_wr_be32(m + H[i].at, H[i].v);
        ASSERT(open_sar(m, s.len, &a) != WL_OK);
        free(m);
    }
    ASSERT_EQ(WL_ERR_ARG, wl_brsar_open_cb(NULL, NULL, s.len, &a));
    free(s.file);
    PASS();
}

TEST fuzz_rsar(void) {
    Sar s;
    make_sar(&s);
    uint8_t *m = (uint8_t *)malloc(s.len);
    for (int it = 0; it < 1500; it++) {
        memcpy(m, s.file, s.len);
        int flips = 1 + (int)(rnd() % 4);
        for (int k = 0; k < flips; k++) {
            size_t at = rnd() % (s.info_off + s.info_size);
            m[at] = (uint8_t)rnd();
        }
        WlBrsar a;
        WlResult r = open_sar(m, s.len, &a);
        if (r == WL_OK) {
            MemSrc ms = {m, s.len, 0};
            for (uint32_t f = 0; f < a.n_file + 1u; f++) {
                uint8_t *d, *w;
                size_t dn, wn;
                if (wl_brsar_read_file(&a, mem_read, &ms, f, 1u << 16, &d, &dn, &w, &wn) == WL_OK) {
                    free(d);
                    free(w);
                }
            }
            for (uint32_t i = 0; i < a.n_str; i++) {
                ASSERT(strlen(wl_brsar_string(&a, i)) < WL_BRSAR_STR_MAX);
            }
            wl_brsar_free(&a);
        }
    }
    free(m);
    free(s.file);
    PASS();
}

#ifdef WL_BRSAR_FILE_TEST
static char g_dir[512];
static void cleanup_dir(void) {
    if (g_dir[0]) {
        (void)wl_file_remove_tree(g_dir);
    }
}

TEST brsar_open_file(void) {
    ASSERT_EQ(WL_OK, wl_file_make_temp_dir(g_dir, sizeof g_dir, "wl-brsar-"));
    (void)atexit(cleanup_dir);
    Sar s;
    make_sar(&s);
    char path[600];
    (void)snprintf(path, sizeof path, "%s/t.brsar", g_dir);
    ASSERT_EQ(WL_OK, wl_file_write_all(path, s.file, s.len));
    WlBrsar a;
    ASSERT_EQ(WL_OK, wl_brsar_open(path, &a));
    ASSERT_EQ(4u, a.n_snd);
    ASSERT_EQ(WL_BRSAR_MAGIC_RWSD, a.file[0].magic);
    wl_brsar_free(&a);
    ASSERT_EQ(WL_OK, wl_file_write_all(path, s.file, s.len / 2));
    ASSERT(wl_brsar_open(path, &a) != WL_OK);
    (void)snprintf(path, sizeof path, "%s/missing.brsar", g_dir);
    ASSERT(wl_brsar_open(path, &a) != WL_OK);
    free(s.file);
    PASS();
}
#endif

SUITE(brsar_suite) {
    RUN_TEST(brsar_index);
    RUN_TEST(brsar_malformed);
    RUN_TEST(fuzz_rsar);
#ifdef WL_BRSAR_FILE_TEST
    RUN_TEST(brsar_open_file);
#endif
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(brsar_suite);
    GREATEST_MAIN_END();
}
