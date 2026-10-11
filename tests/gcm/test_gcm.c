// SPDX-License-Identifier: MIT
// GameCube image reader with synthetic images only: boot.bin fields, nested FST directories,
// file lookup, DOL size, and malformed images (every one must return an error, never crash).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#ifdef WL_GCM_TEST_FILE
#include "wl/host/file.h"
#endif
#include "wl/gcm/gcm.h"
#include "greatest.h"

#define IMG_SIZE 0x10000u
#define DOL_OFF 0x3000u
#define FST_OFF 0x4000u

typedef struct {
    int is_dir;
    const char *name;
    uint32_t a, b; /* dir: parent, end index; file: offset, size */
} Ent;

static void be32(uint8_t *p, uint32_t v) {
    wl_wr_be32(p, v);
}

/* Builds an image with the given entries (entry 0, the root, is generated). Returns the FST size. */
static uint32_t build(uint8_t *img, const Ent *e, uint32_t n) {
    memset(img, 0, IMG_SIZE);
    memcpy(img, "TSTE01", 6);
    be32(img + 0x1C, WL_GCM_MAGIC);
    memcpy(img + 0x20, "synthetic test image", 20);
    be32(img + 0x420, DOL_OFF);
    be32(img + 0x424, FST_OFF);
    /* apploader: size 0x10, trailer 0x20 */
    be32(img + WL_GCM_APPLOADER_OFF + 0x14, 0x10);
    be32(img + WL_GCM_APPLOADER_OFF + 0x18, 0x20);
    /* DOL: one text section at 0x100, 0x40 bytes */
    be32(img + DOL_OFF + 0x00, 0x100);
    be32(img + DOL_OFF + 0x90, 0x40);
    uint32_t count = n + 1;
    uint8_t *fst = img + FST_OFF;
    fst[0] = 1;
    be32(fst + 8, count);
    uint8_t *names = fst + (size_t)count * 12u;
    uint32_t noff = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = fst + (size_t)(i + 1) * 12u;
        p[0] = e[i].is_dir ? 1 : 0;
        p[1] = (uint8_t)(noff >> 16);
        p[2] = (uint8_t)(noff >> 8);
        p[3] = (uint8_t)noff;
        be32(p + 4, e[i].a);
        be32(p + 8, e[i].b);
        size_t l = strlen(e[i].name) + 1;
        memcpy(names + noff, e[i].name, l);
        noff += (uint32_t)l;
    }
    uint32_t fst_size = count * 12u + noff;
    be32(img + 0x428, fst_size);
    be32(img + 0x42C, fst_size);
    return fst_size;
}

/* a.bin, d1/{x.bin, d2/{y.bin}}, z.bin */
static const Ent TREE[] = {
    {0, "a.bin", 0x8000, 5},  {1, "d1", 0, 6},      {0, "x.bin", 0x8010, 7},
    {1, "d2", 2, 6},          {0, "y.bin", 0x8020, 3}, {0, "z.bin", 0x8030, 0},
};
#define NTREE ((uint32_t)(sizeof TREE / sizeof TREE[0]))
/* entry indices in the FST are 1-based here: a=1 d1=2 x=3 d2=4 y=5 z=6; end index of d1/d2 = 6
 * does not fit (z is index 6), so the table above uses the real end 6 for d1 and 6 for d2. */

/* The portable build reads the image from memory; the host build (WL_GCM_TEST_FILE) writes it
 * to a temporary file and goes through wl_gcm_open, so both entry points run the same cases. */
#ifdef WL_GCM_TEST_FILE
static char g_dir[512];

static WlResult open_img(WlGcm *g, const uint8_t *img, size_t len) {
    if (g_dir[0] == '\0') {
        WlResult r = wl_file_make_temp_dir(g_dir, sizeof g_dir, "wl-gcm-");
        if (r != WL_OK) {
            return r;
        }
    }
    char path[600];
    snprintf(path, sizeof path, "%s/t.gcm", g_dir);
    WlResult r = wl_file_write_all(path, img, len);
    return r != WL_OK ? r : wl_gcm_open(g, path);
}
#else
static WlResult open_img(WlGcm *g, const uint8_t *img, size_t len) {
    return wl_gcm_open_mem(g, img, len);
}
#endif

typedef struct {
    char lines[16][64];
    int n;
} Rec;

static WlResult rec_cb(void *ud, const char *path, int is_dir, uint64_t off, uint64_t size) {
    Rec *r = (Rec *)ud;
    if (r->n < 16) {
        snprintf(r->lines[r->n], sizeof r->lines[0], "%s%s %llx %llu", path, is_dir ? "/" : "",
                 (unsigned long long)off, (unsigned long long)size);
    }
    r->n++;
    return WL_OK;
}

static WlResult open_walk(uint8_t *img, WlGcm *g) {
    WlResult r = open_img(g, img, IMG_SIZE);
    if (r != WL_OK) {
        return r;
    }
    WlGcmCounts c;
    r = wl_gcm_count(g, &c);
    if (r != WL_OK) {
        wl_gcm_close(g);
    }
    return r;
}

TEST header_and_tree(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    build(img, TREE, NTREE);
    WlGcm g;
    ASSERT_EQ(WL_OK, open_img(&g, img, IMG_SIZE));
    ASSERT_STR_EQ("TSTE", g.game_code);
    ASSERT_STR_EQ("01", g.maker);
    ASSERT_STR_EQ("synthetic test image", g.title);
    ASSERT_EQ(DOL_OFF, g.dol_off);
    ASSERT_EQ(7u, g.entry_count);
    ASSERT_EQ(0x50u, g.apploader_size);
    Rec rec = {{{0}}, 0};
    ASSERT_EQ(WL_OK, wl_gcm_walk(&g, rec_cb, &rec));
    ASSERT_EQ(6, rec.n);
    ASSERT_STR_EQ("a.bin 8000 5", rec.lines[0]);
    ASSERT_STR_EQ("d1/ 0 0", rec.lines[1]);
    ASSERT_STR_EQ("d1/x.bin 8010 7", rec.lines[2]);
    ASSERT_STR_EQ("d1/d2/ 0 0", rec.lines[3]);
    ASSERT_STR_EQ("d1/d2/y.bin 8020 3", rec.lines[4]);
    ASSERT_STR_EQ("z.bin 8030 0", rec.lines[5]);
    WlGcmCounts c;
    ASSERT_EQ(WL_OK, wl_gcm_count(&g, &c));
    ASSERT_EQ(4u, c.files);
    ASSERT_EQ(2u, c.dirs);
    ASSERT_EQ(15u, (unsigned)c.bytes);
    uint64_t off, size;
    ASSERT_EQ(WL_OK, wl_gcm_find(&g, "d1/d2/y.bin", &off, &size));
    ASSERT_EQ(0x8020u, (unsigned)off);
    ASSERT_EQ(3u, (unsigned)size);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_gcm_find(&g, "d1", &off, &size)); /* directories are not files */
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_gcm_find(&g, "nope", &off, &size));
    wl_gcm_close(&g);
    free(img);
    PASS();
}

TEST read_file_and_dol(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    build(img, TREE, NTREE);
    memcpy(img + 0x8000, "hello", 5);
    memcpy(img + DOL_OFF + 0x100, "DOLBODY", 7);
    WlGcm g;
    ASSERT_EQ(WL_OK, open_img(&g, img, IMG_SIZE));
    uint8_t *d;
    size_t n;
    ASSERT_EQ(WL_OK, wl_gcm_read_range(&g, 0x8000, 5, 100, &d, &n));
    ASSERT_EQ(5u, (unsigned)n);
    ASSERT_MEM_EQ("hello", d, 5);
    free(d);
    ASSERT_EQ(WL_ERR_RANGE, wl_gcm_read_range(&g, 0x8000, 5, 4, &d, &n)); /* over max */
    ASSERT_EQ(WL_ERR_RANGE, wl_gcm_read_range(&g, IMG_SIZE - 2, 5, 100, &d, &n));
    uint32_t dsz;
    ASSERT_EQ(WL_OK, wl_gcm_dol_size(&g, &dsz));
    ASSERT_EQ(0x140u, dsz);
    ASSERT_EQ(WL_OK, wl_gcm_read_dol(&g, &d, &n));
    ASSERT_EQ(0x140u, (unsigned)n);
    ASSERT_MEM_EQ("DOLBODY", d + 0x100, 7);
    free(d);
    wl_gcm_close(&g);
    free(img);
    PASS();
}

TEST malformed_header(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    WlGcm g;
    build(img, TREE, NTREE);
    be32(img + 0x1C, 0);
    ASSERT_EQ(WL_ERR_FORMAT, open_img(&g, img, IMG_SIZE)); /* no magic */

    build(img, TREE, NTREE);
    be32(img + 0x18, WL_GCM_WII_MAGIC);
    be32(img + 0x1C, 0);
    ASSERT_EQ(WL_ERR_FORMAT, open_img(&g, img, IMG_SIZE)); /* a Wii disc */

    build(img, TREE, NTREE);
    ASSERT_EQ(WL_ERR_FORMAT, open_img(&g, img, 0x2000)); /* too short */

    build(img, TREE, NTREE);
    be32(img + 0x424, IMG_SIZE - 8); /* FST runs past the image */
    ASSERT_EQ(WL_ERR_RANGE, open_img(&g, img, IMG_SIZE));

    build(img, TREE, NTREE);
    be32(img + 0x424, 0xFFFFFFF0u); /* offset overflow */
    ASSERT_EQ(WL_ERR_RANGE, open_img(&g, img, IMG_SIZE));

    build(img, TREE, NTREE);
    be32(img + 0x428, 0x40000000u); /* absurd FST size */
    ASSERT_EQ(WL_ERR_RANGE, open_img(&g, img, IMG_SIZE));

    build(img, TREE, NTREE);
    be32(img + 0x420, IMG_SIZE); /* DOL outside the image */
    ASSERT_EQ(WL_ERR_RANGE, open_img(&g, img, IMG_SIZE));

    build(img, TREE, NTREE);
    be32(img + FST_OFF + 8, 5000); /* root claims more entries than the table holds */
    ASSERT_EQ(WL_ERR_RANGE, open_img(&g, img, IMG_SIZE));

    build(img, TREE, NTREE);
    img[FST_OFF] = 0; /* root is a file */
    ASSERT_EQ(WL_ERR_FORMAT, open_img(&g, img, IMG_SIZE));

    build(img, TREE, NTREE);
    be32(img + DOL_OFF + 0x90, 0x7FFFFFF0u); /* DOL section far outside */
    ASSERT_EQ(WL_OK, open_img(&g, img, IMG_SIZE));
    uint32_t dsz;
    ASSERT_EQ(WL_ERR_FORMAT, wl_gcm_dol_size(&g, &dsz));
    wl_gcm_close(&g);
    free(img);
    PASS();
}

/* mutate entry k (1-based FST index) field at byte offset f */
static void poke(uint8_t *img, unsigned k, unsigned f, uint32_t v) {
    be32(img + FST_OFF + (size_t)k * 12u + f, v);
}

TEST malformed_fst(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    WlGcm g;
    build(img, TREE, NTREE);
    ASSERT_EQ(WL_OK, open_walk(img, &g)); /* the unmodified tree is fine */
    wl_gcm_close(&g);

    build(img, TREE, NTREE);
    poke(img, 3, 0, 0x00FFFFFFu); /* name offset out of range (flag byte 0, offset 0xFFFFFF) */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    img[FST_OFF + 3 * 12] = 7; /* bad type byte */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 5, 4, 0xFFFFFF00u); /* file offset past the image */
    ASSERT_EQ(WL_ERR_RANGE, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 5, 8, 0xFFFFFFFFu); /* file size overflows */
    ASSERT_EQ(WL_ERR_RANGE, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 2, 8, 2); /* directory end == its own index: would loop forever */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 2, 8, 1); /* directory end before its own index */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 2, 8, 99); /* directory end beyond the table */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 4, 8, 7); /* nested dir ends beyond its parent's end (6) */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 4, 4, 0); /* wrong parent index */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    build(img, TREE, NTREE);
    poke(img, 4, 4, 4); /* parent == self */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));
    free(img);
    PASS();
}

TEST malformed_names(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    WlGcm g;
    static const Ent slash[] = {{0, "a/b", 0x8000, 1}};
    uint32_t n = build(img, slash, 1);
    (void)n;
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    static const Ent empty[] = {{0, "", 0x8000, 1}};
    build(img, empty, 1);
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    static const Ent one[] = {{0, "abc", 0x8000, 1}};
    uint32_t fst_size = build(img, one, 1);
    img[FST_OFF + fst_size - 1] = 'x'; /* the name table loses its terminator */
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    static char longname[300];
    memset(longname, 'n', sizeof longname - 1);
    static Ent lng[1];
    lng[0] = (Ent){0, longname, 0x8000, 1};
    build(img, lng, 1);
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g)); /* name longer than 255 */
    free(img);
    PASS();
}

TEST deep_nesting_is_refused(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    enum { DEPTH = 70 };
    static Ent chain[DEPTH + 1];
    static char names[DEPTH + 1][8];
    for (int i = 0; i < DEPTH; i++) {
        snprintf(names[i], sizeof names[i], "d%d", i);
        /* entry i+1 is a directory whose parent is i (0 = root) and which spans to the end */
        chain[i] = (Ent){1, names[i], (uint32_t)i, DEPTH + 2};
    }
    snprintf(names[DEPTH], sizeof names[DEPTH], "f");
    chain[DEPTH] = (Ent){0, names[DEPTH], 0x8000, 1};
    build(img, chain, DEPTH + 1);
    WlGcm g;
    ASSERT_EQ(WL_ERR_FORMAT, open_walk(img, &g));

    /* 60 levels is allowed */
    enum { OK_DEPTH = 60 };
    for (int i = 0; i < OK_DEPTH; i++) {
        chain[i] = (Ent){1, names[i], (uint32_t)i, OK_DEPTH + 2};
    }
    chain[OK_DEPTH] = (Ent){0, "f", 0x8000, 1};
    build(img, chain, OK_DEPTH + 1);
    ASSERT_EQ(WL_OK, open_walk(img, &g));
    wl_gcm_close(&g);
    free(img);
    PASS();
}

static WlResult stop_cb(void *ud, const char *path, int is_dir, uint64_t off, uint64_t size) {
    int *left = (int *)ud;
    (void)path;
    (void)is_dir;
    (void)off;
    (void)size;
    return --(*left) == 0 ? WL_ERR_FAILED : WL_OK;
}

TEST walk_stops_on_callback_error(void) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);
    build(img, TREE, NTREE);
    WlGcm g;
    ASSERT_EQ(WL_OK, open_img(&g, img, IMG_SIZE));
    int left = 3;
    ASSERT_EQ(WL_ERR_FAILED, wl_gcm_walk(&g, stop_cb, &left));
    ASSERT_EQ(0, left);
    ASSERT_EQ(WL_ERR_ARG, wl_gcm_walk(&g, NULL, NULL));
    wl_gcm_close(&g);
    free(img);
    PASS();
}

SUITE(gcm_suite) {
    RUN_TEST(header_and_tree);
    RUN_TEST(read_file_and_dol);
    RUN_TEST(malformed_header);
    RUN_TEST(malformed_fst);
    RUN_TEST(malformed_names);
    RUN_TEST(deep_nesting_is_refused);
    RUN_TEST(walk_stops_on_callback_error);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gcm_suite);
#ifdef WL_GCM_TEST_FILE
    if (g_dir[0] != '\0') {
        (void)wl_file_remove_tree(g_dir);
    }
#endif
    GREATEST_MAIN_END();
}
