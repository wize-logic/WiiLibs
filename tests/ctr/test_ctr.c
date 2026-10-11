// SPDX-License-Identifier: MIT
// 3DS readers with synthetic images only: NCSD/NCCH/ExeFS/RomFS (nested directories, forced hash
// collisions, UTF-16 names), the encrypted-image refusal, GARC v4 with sub-file masks and LZ11,
// and malformed inputs (each must return an error, never crash).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"
#include "wl/nds/lz.h"
#include "greatest.h"

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* ---- synthetic RomFS tree ---- */
typedef struct {
    const char *name; /* UTF-8 */
    int parent;
} TDir;
typedef struct {
    const char *name;
    int parent;
    const char *data;
} TFile;

static const TDir DIRS[] = {{"", -1}, {"a", 0}, {"b", 1}, {"u", 0}};
static const TFile FILES[] = {{"root.bin", 0, "RR"},   {"x.bin", 1, "xxxx"},     {"y.bin", 1, "yy"},
                              {"deep.bin", 2, "DEEPDATA"}, {"\xc3\xa9.bin", 3, "accent"}, {"z.bin", 3, ""}};
#define ND ((int)(sizeof DIRS / sizeof DIRS[0]))
#define NF ((int)(sizeof FILES / sizeof FILES[0]))

static size_t to16(const char *s, uint16_t *u) { /* BMP only (test names are 1-3 byte UTF-8) */
    size_t n = 0;
    for (size_t i = 0; s[i] != '\0';) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) {
            u[n++] = b;
            i++;
        } else {
            u[n++] = (uint16_t)(((b & 0x1Fu) << 6) | ((uint8_t)s[i + 1] & 0x3Fu));
            i += 2;
        }
    }
    return n;
}

static uint32_t hashname(uint32_t parent, const char *name) {
    uint16_t u[64];
    size_t n = to16(name, u);
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < n; i++) {
        h = ((h >> 5) | (h << 27)) ^ u[i];
    }
    return h;
}

static size_t padded(size_t n) { return (n + 3u) & ~(size_t)3; }

#define IMG_SIZE 0x20000u
#define NCCH_OFF 0x4000u
#define ROMFS_OFF (NCCH_OFF + 0x1000u)
#define L3_OFF (ROMFS_OFF + 0x1000u)

/* Builds the level-3 area at img + L3_OFF; returns its size. buckets forces collisions. */
static size_t build_l3(uint8_t *img, uint32_t buckets) {
    uint8_t *l3 = img + L3_OFF;
    uint32_t doff[8], foff[16];
    uint32_t dm = 0, fm = 0;
    for (int i = 0; i < ND; i++) {
        doff[i] = dm;
        uint16_t u[64];
        dm += 0x18u + (uint32_t)padded(to16(DIRS[i].name, u) * 2);
    }
    for (int i = 0; i < NF; i++) {
        foff[i] = fm;
        uint16_t u[64];
        fm += 0x20u + (uint32_t)padded(to16(FILES[i].name, u) * 2);
    }
    uint32_t dh_off = 0x28, dh_size = buckets * 4u;
    uint32_t dmeta_off = dh_off + dh_size, fh_off = dmeta_off + dm, fh_size = buckets * 4u;
    uint32_t fmeta_off = fh_off + fh_size, data_off = (fmeta_off + fm + 0x1Fu) & ~0x1Fu;
    put32(l3, 0x28);
    put32(l3 + 4, dh_off);
    put32(l3 + 8, dh_size);
    put32(l3 + 12, dmeta_off);
    put32(l3 + 16, dm);
    put32(l3 + 20, fh_off);
    put32(l3 + 24, fh_size);
    put32(l3 + 28, fmeta_off);
    put32(l3 + 32, fm);
    put32(l3 + 36, data_off);
    for (uint32_t b = 0; b < buckets; b++) {
        put32(l3 + dh_off + b * 4u, 0xFFFFFFFFu);
        put32(l3 + fh_off + b * 4u, 0xFFFFFFFFu);
    }
    for (int i = 0; i < ND; i++) {
        uint8_t *e = l3 + dmeta_off + doff[i];
        int sib = -1, cd = -1, cf = -1;
        for (int j = i + 1; j < ND && i > 0; j++) {
            if (DIRS[j].parent == DIRS[i].parent) {
                sib = j;
                break;
            }
        }
        for (int j = 1; j < ND; j++) {
            if (DIRS[j].parent == i) {
                cd = j;
                break;
            }
        }
        for (int j = 0; j < NF; j++) {
            if (FILES[j].parent == i) {
                cf = j;
                break;
            }
        }
        uint16_t u[64];
        size_t nu = to16(DIRS[i].name, u);
        put32(e, i == 0 ? 0 : doff[DIRS[i].parent]);
        put32(e + 4, sib < 0 ? 0xFFFFFFFFu : doff[sib]);
        put32(e + 8, cd < 0 ? 0xFFFFFFFFu : doff[cd]);
        put32(e + 12, cf < 0 ? 0xFFFFFFFFu : foff[cf]);
        put32(e + 20, (uint32_t)nu * 2u);
        for (size_t k = 0; k < nu; k++) {
            put16(e + 0x18 + k * 2, u[k]);
        }
        uint32_t par = i == 0 ? 0 : doff[DIRS[i].parent];
        if (i > 0) {
            uint8_t *hp = l3 + dh_off + (hashname(par, DIRS[i].name) % buckets) * 4u;
            put32(e + 16, wl_rd_le32(hp));
            put32(hp, doff[i]);
        } else {
            put32(e + 16, 0xFFFFFFFFu);
        }
    }
    uint64_t pos = 0;
    for (int i = 0; i < NF; i++) {
        uint8_t *e = l3 + fmeta_off + foff[i];
        int sib = -1;
        for (int j = i + 1; j < NF; j++) {
            if (FILES[j].parent == FILES[i].parent) {
                sib = j;
                break;
            }
        }
        uint16_t u[64];
        size_t nu = to16(FILES[i].name, u);
        size_t dl = strlen(FILES[i].data);
        put32(e, doff[FILES[i].parent]);
        put32(e + 4, sib < 0 ? 0xFFFFFFFFu : foff[sib]);
        put64(e + 8, pos);
        put64(e + 16, dl);
        put32(e + 28, (uint32_t)nu * 2u);
        for (size_t k = 0; k < nu; k++) {
            put16(e + 0x20 + k * 2, u[k]);
        }
        uint8_t *hp = l3 + fh_off + (hashname(doff[FILES[i].parent], FILES[i].name) % buckets) * 4u;
        put32(e + 24, wl_rd_le32(hp));
        put32(hp, foff[i]);
        memcpy(l3 + data_off + pos, FILES[i].data, dl);
        pos += (dl + 15u) & ~(uint64_t)15;
    }
    return (size_t)data_off + (size_t)pos;
}

/* ---- full image ---- */
static uint8_t g_img[IMG_SIZE];

static void build_image(uint32_t buckets, int nocrypto) {
    memset(g_img, 0, sizeof g_img);
    /* NCSD */
    memcpy(g_img + 0x100, "NCSD", 4);
    put32(g_img + 0x104, IMG_SIZE / 0x200u);
    put32(g_img + 0x120, NCCH_OFF / 0x200u);
    put32(g_img + 0x124, (IMG_SIZE - NCCH_OFF) / 0x200u);
    /* NCCH */
    uint8_t *c = g_img + NCCH_OFF;
    memcpy(c + 0x100, "NCCH", 4);
    c[0x188 + 7] = nocrypto ? 0x4 : 0x1;
    put32(c + 0x1A0, 1); /* ExeFS at +0x200, two units */
    put32(c + 0x1A4, 2);
    put32(c + 0x1B0, 8); /* RomFS at +0x1000 */
    put32(c + 0x1B4, (IMG_SIZE - ROMFS_OFF) / 0x200u);
    /* ExeFS: ".code" (4 bytes) and "icon" (3 bytes) */
    uint8_t *x = c + 0x200;
    memcpy(x, ".code", 5);
    put32(x + 8, 0);
    put32(x + 12, 4);
    memcpy(x + 16, "icon", 4);
    put32(x + 24, 0x10);
    put32(x + 28, 3);
    memcpy(x + 0x200, "CODE", 4);
    memcpy(x + 0x210, "ICO", 3);
    /* RomFS: IVFC header, master hash of 0x20 bytes, level 3 aligned to 0x1000 */
    uint8_t *r = g_img + ROMFS_OFF;
    memcpy(r, "IVFC", 4);
    put32(r + 4, 0x10000);
    put32(r + 8, 0x20);
    size_t l3 = build_l3(g_img, buckets);
    put64(r + 0x44, l3);
    put32(r + 0x4C, 12);
}

typedef struct {
    int files, dirs;
    uint64_t bytes;
} Count;

static WlResult count_cb(void *ud, const char *path, const WlRomfsEntry *e) {
    Count *c = (Count *)ud;
    (void)path;
    if (e->is_dir) {
        c->dirs++;
    } else {
        c->files++;
        c->bytes += e->size;
    }
    return WL_OK;
}

static int open_all(WlCtrSrc *src, WlNcsd *n, WlNcch *c, WlRomfs *r) {
    if (wl_ctr_src_mem(src, g_img, sizeof g_img) != WL_OK || wl_ncsd_open(n, src) != WL_OK ||
        wl_ncch_open(c, src, n->part_off[0], n->part_size[0]) != WL_OK) {
        return 0;
    }
    return wl_romfs_open(r, c) == WL_OK;
}

static int check_tree(uint32_t buckets) {
    build_image(buckets, 1);
    WlCtrSrc src;
    WlNcsd n;
    WlNcch c;
    WlRomfs r;
    if (!open_all(&src, &n, &c, &r)) {
        return 0;
    }
    int ok = 1;
    Count cnt = {0, 0, 0};
    WlResult wr = wl_romfs_walk(&r, count_cb, &cnt);
    ok &= wr == WL_OK && cnt.files == NF && cnt.dirs == ND - 1;
    static const char *paths[] = {"root.bin", "a/x.bin", "a/y.bin", "a/b/deep.bin", "u/\xc3\xa9.bin",
                                  "u/z.bin"};
    for (int i = 0; i < NF; i++) {
        WlRomfsEntry e;
        uint8_t *d;
        size_t len;
        ok &= wl_romfs_find_file(&r, paths[i], &e) == WL_OK && e.size == strlen(FILES[i].data);
        ok &= wl_romfs_read_all(&r, &e, 1024, &d, &len) == WL_OK && len == strlen(FILES[i].data) &&
              memcmp(d, FILES[i].data, len) == 0;
        free(d);
    }
    WlRomfsEntry e;
    ok &= wl_romfs_find_file(&r, "a/missing.bin", &e) == WL_ERR_FAILED;
    ok &= wl_romfs_find_file(&r, "nodir/x.bin", &e) == WL_ERR_FAILED;
    ok &= wl_romfs_find_file(&r, "a/x.bin/", &e) != WL_OK;
    wl_romfs_close(&r);
    wl_ctr_src_close(&src);
    return ok;
}

TEST romfs_tree_and_lookup(void) {
    ASSERT(check_tree(7));
    PASS();
}

TEST romfs_all_hashes_collide(void) {
    ASSERT(check_tree(1)); /* one bucket: every entry is on one chain */
    ASSERT(check_tree(2));
    PASS();
}

TEST exefs_lists_and_reads(void) {
    build_image(3, 1);
    WlCtrSrc src;
    WlNcsd n;
    WlNcch c;
    WlExefs ex;
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, g_img, sizeof g_img));
    ASSERT_EQ(WL_OK, wl_ncsd_open(&n, &src));
    ASSERT_EQ(WL_OK, wl_ncch_open(&c, &src, n.part_off[0], n.part_size[0]));
    ASSERT_EQ(WL_OK, wl_exefs_open(&ex, &c));
    ASSERT_EQ(2u, ex.count);
    ASSERT_STR_EQ(".code", ex.files[0].name);
    ASSERT_STR_EQ("icon", ex.files[1].name);
    uint8_t *d;
    size_t len;
    ASSERT_EQ(WL_OK, wl_exefs_read(&ex, 1, &d, &len));
    ASSERT_EQ(3u, (unsigned)len);
    ASSERT_MEM_EQ("ICO", d, 3);
    free(d);
    wl_ctr_src_close(&src);
    PASS();
}

TEST encrypted_ncch_is_refused(void) {
    build_image(3, 0);
    WlCtrSrc src;
    WlNcsd n;
    WlNcch c;
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, g_img, sizeof g_img));
    ASSERT_EQ(WL_OK, wl_ncsd_open(&n, &src));
    ASSERT_EQ(WL_ERR_ENCRYPTED, wl_ncch_open(&c, &src, n.part_off[0], n.part_size[0]));
    WlCtrSurvey s;
    ASSERT_EQ(WL_ERR_ENCRYPTED, wl_ctr_survey(&src, 1, NULL, &s));
    wl_ctr_src_close(&src);
    PASS();
}

TEST survey_counts(void) {
    build_image(5, 1);
    WlCtrSrc src;
    WlCtrSurvey s;
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, g_img, sizeof g_img));
    ASSERT_EQ(WL_OK, wl_ctr_survey(&src, 1, NULL, &s));
    ASSERT_EQ((uint32_t)NF, s.romfs_files);
    ASSERT_EQ((uint32_t)(ND - 1), s.romfs_dirs);
    ASSERT_EQ(2u, s.exefs_files);
    ASSERT_EQ(0u, s.lookup_fail);
    ASSERT_EQ(0u, s.failures);
    wl_ctr_src_close(&src);
    PASS();
}

TEST malformed_images(void) {
    WlCtrSrc src;
    WlNcsd n;
    WlNcch c;
    WlRomfs r;
    /* too short and wrong magic */
    uint8_t tiny[16] = {0};
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, tiny, sizeof tiny));
    ASSERT(wl_ncsd_open(&n, &src) != WL_OK);
    wl_ctr_src_close(&src);
    build_image(3, 1);
    g_img[0x100] = 'X';
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, g_img, sizeof g_img));
    ASSERT_EQ(WL_ERR_FORMAT, wl_ncsd_open(&n, &src));
    wl_ctr_src_close(&src);
    /* partition past the end of the image */
    build_image(3, 1);
    put32(g_img + 0x124, 0x7FFFFFFFu);
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, g_img, sizeof g_img));
    ASSERT_EQ(WL_ERR_RANGE, wl_ncsd_open(&n, &src));
    wl_ctr_src_close(&src);
    /* NCCH with a RomFS past its partition */
    build_image(3, 1);
    put32(g_img + NCCH_OFF + 0x1B4, 0x7FFFFFFu);
    ASSERT_EQ(WL_OK, wl_ctr_src_mem(&src, g_img, sizeof g_img));
    ASSERT_EQ(WL_OK, wl_ncsd_open(&n, &src));
    ASSERT_EQ(WL_ERR_RANGE, wl_ncch_open(&c, &src, n.part_off[0], n.part_size[0]));
    wl_ctr_src_close(&src);
    /* bad IVFC magic */
    build_image(3, 1);
    g_img[ROMFS_OFF] = 'X';
    ASSERT_EQ(0, open_all(&src, &n, &c, &r));
    wl_ctr_src_close(&src);
    /* level-3 header length wrong */
    build_image(3, 1);
    put32(g_img + L3_OFF, 0x30);
    ASSERT_EQ(0, open_all(&src, &n, &c, &r));
    wl_ctr_src_close(&src);
    /* table past level 3 */
    build_image(3, 1);
    put32(g_img + L3_OFF + 0x1C, 0x7FFFFF00u);
    ASSERT_EQ(0, open_all(&src, &n, &c, &r));
    wl_ctr_src_close(&src);
    PASS();
}

/* Opens a patched image and returns the walk result. The first file entry sits at the start of
 * the file metadata table; patch(off in file meta, value) is applied before opening. */
static WlResult walk_patched(uint32_t file_meta_field, uint32_t value, uint32_t dir_meta_field,
                             int use_dir) {
    build_image(3, 1);
    uint8_t *l3 = g_img + L3_OFF;
    if (use_dir) {
        put32(l3 + wl_rd_le32(l3 + 12) + dir_meta_field, value);
    } else {
        put32(l3 + wl_rd_le32(l3 + 28) + file_meta_field, value);
    }
    WlCtrSrc src;
    WlNcsd n;
    WlNcch c;
    WlRomfs r;
    if (!open_all(&src, &n, &c, &r)) {
        wl_ctr_src_close(&src);
        return WL_ERR_FORMAT;
    }
    Count cnt = {0, 0, 0};
    WlResult res = wl_romfs_walk(&r, count_cb, &cnt);
    wl_romfs_close(&r);
    wl_ctr_src_close(&src);
    return res;
}

TEST malformed_romfs_tables(void) {
    ASSERT(walk_patched(4, 0, 0, 0) != WL_OK);          /* file sibling loops to the first file */
    ASSERT(walk_patched(28, 0xFFFFFFF0u, 0, 0) != WL_OK); /* name length past the table */
    ASSERT(walk_patched(28, 3, 0, 0) != WL_OK);           /* odd UTF-16 name length */
    ASSERT(walk_patched(16, 0xFFFFFFFFu, 0, 0) != WL_OK); /* file size beyond level 3 */
    ASSERT(walk_patched(4, 0x7FFFFFF0u, 0, 0) != WL_OK);  /* sibling offset out of range */
    ASSERT(walk_patched(0, 0, 8, 1) != WL_OK);
    ASSERT(walk_patched(0, 2, 8, 1) != WL_OK);            /* misaligned directory offset */
    PASS();
}

/* ---- GARC v4 ---- */
static size_t build_garc(uint8_t *g, size_t cap) {
    /* data: entry0 sub0 = LZ11 literal stream for "abc"; entry1 subs 0 and 2; entry2 sub 1 */
    static const uint8_t lz[] = {0x11, 3, 0, 0, 0x00, 'a', 'b', 'c'};
    memset(g, 0, cap);
    const uint32_t entries = 3;
    uint32_t fato = 0x1C, fato_size = 12 + 4 * entries + 0; /* 24 */
    uint32_t fatb = fato + fato_size;
    /* FATB entry area: e0 = mask + 1 triple (16), e1 = mask + 2 triples (28), e2 = 16 => 60 */
    uint32_t fatb_size = 12 + 60;
    uint32_t fimb = fatb + fatb_size, data = fimb + 12;
    memcpy(g, "CRAG", 4);
    put32(g + 4, 0x1C);
    put16(g + 8, 0xFEFF);
    put16(g + 10, 0x0400);
    put32(g + 12, 4);
    put32(g + 16, data);
    memcpy(g + fato, "OTAF", 4);
    put32(g + fato + 4, fato_size);
    put16(g + fato + 8, entries);
    put16(g + fato + 10, 0xFFFF);
    put32(g + fato + 12, 0);
    put32(g + fato + 16, 16);
    put32(g + fato + 20, 44);
    memcpy(g + fatb, "BTAF", 4);
    put32(g + fatb + 4, fatb_size);
    put32(g + fatb + 8, entries);
    uint8_t *e = g + fatb + 12;
    put32(e, 1); /* entry 0 */
    put32(e + 4, 0);
    put32(e + 8, 8);
    put32(e + 12, 8);
    put32(e + 16, 5); /* entry 1: sub 0 and sub 2 */
    put32(e + 20, 8);
    put32(e + 24, 12);
    put32(e + 28, 4);
    put32(e + 32, 12);
    put32(e + 36, 16);
    put32(e + 40, 4);
    put32(e + 44, 2); /* entry 2: sub 1 */
    put32(e + 48, 16);
    put32(e + 52, 20);
    put32(e + 56, 4);
    memcpy(g + fimb, "BMIF", 4);
    put32(g + fimb + 4, 12);
    put32(g + fimb + 8, 20);
    memcpy(g + data, lz, 8);
    memcpy(g + data + 8, "AAAA", 4);
    memcpy(g + data + 12, "BBBB", 4);
    memcpy(g + data + 16, "CCCC", 4);
    uint32_t total = data + 20;
    put32(g + 20, total);
    return total;
}

TEST garc_entries_and_masks(void) {
    uint8_t g[512];
    size_t n = build_garc(g, sizeof g);
    WlGarc a;
    ASSERT(wl_garc_is_garc(g, n));
    ASSERT_EQ(WL_OK, wl_garc_open(&a, g, n));
    ASSERT_EQ(3u, a.entries);
    uint32_t mask;
    const uint8_t *d;
    size_t len;
    ASSERT_EQ(WL_OK, wl_garc_mask(&a, 1, &mask));
    ASSERT_EQ(5u, mask);
    ASSERT_EQ(WL_OK, wl_garc_sub(&a, 1, 0, &d, &len));
    ASSERT_EQ(4u, (unsigned)len);
    ASSERT_MEM_EQ("AAAA", d, 4);
    ASSERT_EQ(WL_OK, wl_garc_sub(&a, 1, 2, &d, &len));
    ASSERT_MEM_EQ("BBBB", d, 4);
    ASSERT_EQ(WL_ERR_RANGE, wl_garc_sub(&a, 1, 1, &d, &len)); /* bit not set */
    ASSERT_EQ(WL_OK, wl_garc_sub(&a, 2, 1, &d, &len));
    ASSERT_MEM_EQ("CCCC", d, 4);
    ASSERT_EQ(WL_ERR_ARG, wl_garc_mask(&a, 3, &mask));
    /* entry 0 is an LZ11 stream and decodes */
    ASSERT_EQ(WL_OK, wl_garc_sub(&a, 0, 0, &d, &len));
    ASSERT(wl_garc_is_lz11(d, len));
    uint8_t *out;
    size_t olen;
    ASSERT_EQ(WL_OK, wl_lz_decode(d, len, 1024, &out, &olen));
    ASSERT_EQ(3u, (unsigned)olen);
    ASSERT_MEM_EQ("abc", out, 3);
    free(out);
    ASSERT(!wl_garc_is_lz11((const uint8_t *)"AAAA", 4));
    PASS();
}

TEST garc_malformed(void) {
    uint8_t g[512], t[512];
    size_t n = build_garc(g, sizeof g);
    WlGarc a;
    for (size_t cut = 0; cut < n; cut += 7) { /* every truncation must be rejected or consistent */
        WlResult r = wl_garc_open(&a, g, cut);
        ASSERT(r != WL_OK);
    }
    memcpy(t, g, n);
    put16(t + 10, 0x0600); /* other version */
    ASSERT_EQ(WL_ERR_FORMAT, wl_garc_open(&a, t, n));
    memcpy(t, g, n);
    t[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_garc_open(&a, t, n));
    memcpy(t, g, n);
    put32(t + 0x1C + 12 + 4, 0x7FFFFFF0u); /* FATO entry 1 offset out of range */
    ASSERT_EQ(WL_OK, wl_garc_open(&a, t, n));
    uint32_t mask;
    ASSERT_EQ(WL_ERR_RANGE, wl_garc_mask(&a, 1, &mask));
    memcpy(t, g, n);
    {
        uint32_t fatb = 0x1C + 24;
        put32(t + fatb + 12 + 8, 0x7FFFFFFFu); /* entry 0 end past data */
        put32(t + fatb + 12 + 12, 0x7FFFFFFFu);
    }
    ASSERT_EQ(WL_OK, wl_garc_open(&a, t, n));
    const uint8_t *d;
    size_t len;
    ASSERT_EQ(WL_ERR_RANGE, wl_garc_sub(&a, 0, 0, &d, &len));
    PASS();
}

SUITE(ctr_suite) {
    RUN_TEST(romfs_tree_and_lookup);
    RUN_TEST(romfs_all_hashes_collide);
    RUN_TEST(exefs_lists_and_reads);
    RUN_TEST(encrypted_ncch_is_refused);
    RUN_TEST(survey_counts);
    RUN_TEST(malformed_images);
    RUN_TEST(malformed_romfs_tables);
    RUN_TEST(garc_entries_and_masks);
    RUN_TEST(garc_malformed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(ctr_suite);
    GREATEST_MAIN_END();
}
