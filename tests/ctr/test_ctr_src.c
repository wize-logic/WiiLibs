// SPDX-License-Identifier: MIT
// wl_ctr_src_open over a synthetic 3DS (NCSD + NCCH + ExeFS) image written to a temp file:
// reads through the file source, range and missing-file errors, and truncated/garbled headers.
// No game data: the image is built here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/host/file.h"
#include "wl/ctr/ctr.h"
#include "greatest.h"

#define IMG_SIZE 0x1000u
#define NCCH_OFF 0x400u

static char g_dir[256];

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void build(uint8_t *img) {
    memset(img, 0, IMG_SIZE);
    memcpy(img + 0x100, "NCSD", 4);
    put32(img + 0x104, IMG_SIZE / 0x200u);
    put32(img + 0x120, NCCH_OFF / 0x200u);
    put32(img + 0x124, (IMG_SIZE - NCCH_OFF) / 0x200u);
    uint8_t *c = img + NCCH_OFF;
    memcpy(c + 0x100, "NCCH", 4);
    c[0x188 + 7] = 0x4; /* NoCrypto */
    put32(c + 0x1A0, 1); /* ExeFS at +0x200, two units */
    put32(c + 0x1A4, 2);
    uint8_t *x = c + 0x200;
    memcpy(x, ".code", 5);
    put32(x + 8, 0);
    put32(x + 12, 4);
    memcpy(x + 16, "icon", 4);
    put32(x + 24, 0x10);
    put32(x + 28, 3);
    memcpy(x + 0x200, "CODE", 4);
    memcpy(x + 0x210, "ICO", 3);
}

static void path_of(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static WlResult write_img(const char *name, size_t len, char *path, size_t cap) {
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    if (img == NULL) {
        return WL_ERR_NOMEM;
    }
    build(img);
    path_of(path, cap, name);
    WlResult r = wl_file_write_all(path, img, len);
    free(img);
    return r;
}

TEST open_and_read_container(void) {
    char path[300];
    ASSERT_EQ(WL_OK, write_img("ok.3ds", IMG_SIZE, path, sizeof path));
    WlCtrSrc src;
    ASSERT_EQ(WL_OK, wl_ctr_src_open(&src, path));
    ASSERT_EQ((uint64_t)IMG_SIZE, src.size);

    char magic[4];
    ASSERT_EQ(WL_OK, wl_ctr_src_read(&src, 0x100, magic, 4));
    ASSERT_MEM_EQ("NCSD", magic, 4);

    WlNcsd n;
    WlNcch c;
    WlExefs e;
    ASSERT_EQ(WL_OK, wl_ncsd_open(&n, &src));
    ASSERT_EQ((uint64_t)NCCH_OFF, n.part_off[0]);
    ASSERT_EQ(WL_OK, wl_ncch_open(&c, &src, n.part_off[0], n.part_size[0]));
    ASSERT_EQ(WL_OK, wl_exefs_open(&e, &c));
    ASSERT_EQ(2u, e.count);
    uint8_t *d;
    size_t len;
    ASSERT_EQ(WL_OK, wl_exefs_read(&e, 0, &d, &len));
    ASSERT_EQ((size_t)4, len);
    ASSERT_MEM_EQ("CODE", d, 4);
    free(d);
    ASSERT_EQ(WL_OK, wl_exefs_read(&e, 1, &d, &len));
    ASSERT_EQ((size_t)3, len);
    ASSERT_MEM_EQ("ICO", d, 3);
    free(d);

    /* a zero-length read at the very end is in range, one byte past is not */
    uint8_t b;
    ASSERT_EQ(WL_OK, wl_ctr_src_read(&src, IMG_SIZE, &b, 0));
    ASSERT_EQ(WL_OK, wl_ctr_src_read(&src, IMG_SIZE - 1, &b, 1));
    ASSERT_EQ(WL_ERR_RANGE, wl_ctr_src_read(&src, IMG_SIZE, &b, 1));
    ASSERT_EQ(WL_ERR_RANGE, wl_ctr_src_read(&src, IMG_SIZE - 1, &b, 2));
    ASSERT_EQ(WL_ERR_RANGE, wl_ctr_src_read(&src, UINT64_MAX, &b, 1));
    ASSERT_EQ(WL_ERR_RANGE, wl_ctr_src_read(&src, 1, &b, (size_t)-1));

    wl_ctr_src_close(&src);
    wl_ctr_src_close(&src); /* a second close is a no-op */
    PASS();
}

TEST missing_file_and_bad_args(void) {
    char path[300];
    path_of(path, sizeof path, "does-not-exist.3ds");
    WlCtrSrc src;
    memset(&src, 0, sizeof src);
    ASSERT(wl_ctr_src_open(&src, path) != WL_OK);
    ASSERT_EQ(WL_ERR_ARG, wl_ctr_src_open(NULL, path));
    ASSERT_EQ(WL_ERR_ARG, wl_ctr_src_open(&src, NULL));
    /* a directory is not an image */
    ASSERT(wl_ctr_src_open(&src, g_dir) != WL_OK);
    wl_ctr_src_close(&src); /* nothing was opened; must not crash */
    PASS();
}

TEST empty_and_truncated_files(void) {
    char path[300];
    WlCtrSrc src;
    WlNcsd n;
    /* empty file: opens with size 0, every read and the NCSD parse fail cleanly */
    path_of(path, sizeof path, "empty.3ds");
    ASSERT_EQ(WL_OK, wl_file_write_all(path, "", 0));
    ASSERT_EQ(WL_OK, wl_ctr_src_open(&src, path));
    ASSERT_EQ((uint64_t)0, src.size);
    uint8_t b;
    ASSERT_EQ(WL_ERR_RANGE, wl_ctr_src_read(&src, 0, &b, 1));
    ASSERT_EQ(WL_ERR_FORMAT, wl_ncsd_open(&n, &src));
    wl_ctr_src_close(&src);

    /* header cut before the partition table ends */
    ASSERT_EQ(WL_OK, write_img("cut-header.3ds", 0x150, path, sizeof path));
    ASSERT_EQ(WL_OK, wl_ctr_src_open(&src, path));
    ASSERT_EQ((uint64_t)0x150, src.size);
    ASSERT_EQ(WL_ERR_FORMAT, wl_ncsd_open(&n, &src));
    wl_ctr_src_close(&src);

    /* header intact, body truncated: the partition runs past the end of the file */
    ASSERT_EQ(WL_OK, write_img("cut-body.3ds", NCCH_OFF + 0x300, path, sizeof path));
    ASSERT_EQ(WL_OK, wl_ctr_src_open(&src, path));
    ASSERT_EQ(WL_ERR_RANGE, wl_ncsd_open(&n, &src));
    wl_ctr_src_close(&src);
    PASS();
}

TEST bad_magic_and_encrypted(void) {
    char path[300];
    WlCtrSrc src;
    WlNcsd n;
    WlNcch c;
    uint8_t *img = (uint8_t *)malloc(IMG_SIZE);
    ASSERT(img != NULL);

    build(img);
    img[0x100] = 'X';
    path_of(path, sizeof path, "badmagic.3ds");
    ASSERT_EQ(WL_OK, wl_file_write_all(path, img, IMG_SIZE));
    ASSERT_EQ(WL_OK, wl_ctr_src_open(&src, path));
    ASSERT_EQ(WL_ERR_FORMAT, wl_ncsd_open(&n, &src));
    wl_ctr_src_close(&src);

    build(img);
    img[NCCH_OFF + 0x188 + 7] = 0x1; /* NoCrypto flag cleared */
    path_of(path, sizeof path, "crypt.3ds");
    ASSERT_EQ(WL_OK, wl_file_write_all(path, img, IMG_SIZE));
    ASSERT_EQ(WL_OK, wl_ctr_src_open(&src, path));
    ASSERT_EQ(WL_OK, wl_ncsd_open(&n, &src));
    ASSERT_EQ(WL_ERR_ENCRYPTED, wl_ncch_open(&c, &src, n.part_off[0], n.part_size[0]));
    wl_ctr_src_close(&src);
    free(img);
    PASS();
}

SUITE(ctr_src_suite) {
    RUN_TEST(open_and_read_container);
    RUN_TEST(missing_file_and_bad_args);
    RUN_TEST(empty_and_truncated_files);
    RUN_TEST(bad_magic_and_encrypted);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    if (wl_file_make_temp_dir(g_dir, sizeof g_dir, "wl-ctrsrc-") != WL_OK) {
        fprintf(stderr, "cannot make a temp dir\n");
        return 1;
    }
    RUN_SUITE(ctr_src_suite);
    wl_file_remove_tree(g_dir);
    GREATEST_MAIN_END();
}
