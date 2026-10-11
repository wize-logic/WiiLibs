// SPDX-License-Identifier: MIT
// 3DS container readers: NCSD (CCI), NCCH, ExeFS, RomFS (IVFC level 3) and GARC v4.
// References: 3dbrew.org pages NCSD, NCCH, ExeFS, RomFS and the IVFC structure; the GARC layout
// is summarised in README.md. Only NoCrypto (already decrypted) images are read; encrypted
// content is refused and this library holds no keys. All inputs are untrusted.
#pragma once

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

/* Same code (and the same definition) as wl/host/base.h, so the module stays portable. */
#ifndef WL_ERR_ENCRYPTED
#define WL_ERR_ENCRYPTED (WL_ERR_BASE_HOST - 1)   /* image content is encrypted */
#endif

/* ---- byte source: images are up to 2 GiB, so everything reads through offsets. Fill the
 * struct yourself to read from any medium (read must be exact-size; close may be NULL). ---- */
typedef struct WlCtrSrc {
    WlResult (*read)(void *ctx, uint64_t off, void *buf, size_t n);
    void (*close)(void *ctx);
    void *ctx;
    uint64_t size;
} WlCtrSrc;

WlResult wl_ctr_src_mem(WlCtrSrc *s, const uint8_t *data, size_t len); /* borrows data */
/* Host builds only (src_file.c, needs wl_host): a 64-bit-offset read-only file. */
WlResult wl_ctr_src_open(WlCtrSrc *s, const char *path);
void wl_ctr_src_close(WlCtrSrc *s);
WlResult wl_ctr_src_read(const WlCtrSrc *s, uint64_t off, void *buf, size_t n);

/* ---- NCSD ---- */
typedef struct {
    WlCtrSrc *src;
    uint64_t part_off[8], part_size[8]; /* bytes; size 0 = unused */
} WlNcsd;

WlResult wl_ncsd_open(WlNcsd *n, WlCtrSrc *src);

/* ---- NCCH ---- */
typedef struct {
    WlCtrSrc *src;
    uint64_t base, size;           /* absolute byte range of the partition */
    uint64_t exefs_off, exefs_size; /* absolute */
    uint64_t romfs_off, romfs_size; /* absolute */
} WlNcch;

/* WL_ERR_ENCRYPTED when the NoCrypto flag is not set. */
WlResult wl_ncch_open(WlNcch *c, WlCtrSrc *src, uint64_t off, uint64_t size);

/* ---- ExeFS ---- */
#define WL_EXEFS_MAX_FILES 10
typedef struct {
    char name[9];
    uint64_t off; /* absolute */
    uint32_t size;
} WlExefsFile;
typedef struct {
    const WlNcch *ncch;
    unsigned count;
    WlExefsFile files[WL_EXEFS_MAX_FILES];
} WlExefs;

WlResult wl_exefs_open(WlExefs *e, const WlNcch *c);
/* Reads one file into a malloc'd buffer. */
WlResult wl_exefs_read(const WlExefs *e, unsigned index, uint8_t **data, size_t *len);

/* ---- RomFS ---- */
typedef struct {
    const WlNcch *ncch;
    uint64_t l3_off;  /* absolute offset of the level-3 area */
    uint64_t l3_size;
    uint64_t data_off; /* offset of file data relative to l3_off */
    uint8_t *dir_hash, *dir_meta, *file_hash, *file_meta; /* tables, owned */
    uint32_t dir_hash_n, dir_meta_len, file_hash_n, file_meta_len; /* n = bucket counts */
} WlRomfs;

typedef struct {
    int is_dir;
    uint64_t size;
    uint64_t offset; /* absolute offset of the file data in the image */
} WlRomfsEntry;

/* Called for every directory and file in depth-first order; path is UTF-8 without a leading
 * slash. Return WL_OK to continue. */
typedef WlResult (*WlRomfsVisit)(void *ud, const char *path, const WlRomfsEntry *e);

WlResult wl_romfs_open(WlRomfs *r, const WlNcch *c);
void wl_romfs_close(WlRomfs *r);
WlResult wl_romfs_walk(const WlRomfs *r, WlRomfsVisit cb, void *ud);
/* Looks a file up by path ("a/0/0/1") through the IVFC hash table; WL_ERR_FAILED if absent. */
WlResult wl_romfs_find_file(const WlRomfs *r, const char *path, WlRomfsEntry *e);
WlResult wl_romfs_read(const WlRomfs *r, const WlRomfsEntry *e, uint64_t off, void *buf, size_t n);
/* Reads a whole file (at most max bytes) into a malloc'd buffer. */
WlResult wl_romfs_read_all(const WlRomfs *r, const WlRomfsEntry *e, size_t max, uint8_t **data,
                           size_t *len);

/* ---- GARC v4 ---- */
typedef struct {
    const uint8_t *buf;
    size_t len;
    uint32_t entries;
    uint32_t fatb_entries_off; /* start of the FATB entry area */
    uint32_t fatb_len;         /* bytes of FATB entry area */
    uint32_t data_off;         /* start of FIMB data */
    uint32_t data_len;
    const uint8_t *fato;       /* FATO table of u32 offsets */
} WlGarc;

int wl_garc_is_garc(const uint8_t *d, size_t n);
WlResult wl_garc_open(WlGarc *g, const uint8_t *buf, size_t len);
/* Bit mask of the sub-files present in an entry (bit i = sub-file i). */
WlResult wl_garc_mask(const WlGarc *g, uint32_t entry, uint32_t *mask);
/* Sub-file at bit index "bit" of an entry (WL_ERR_RANGE when that bit is not set). */
WlResult wl_garc_sub(const WlGarc *g, uint32_t entry, unsigned bit, const uint8_t **data, size_t *len);
/* True when the bytes look like an LZ11 stream (0x11 and a plausible size). */
int wl_garc_is_lz11(const uint8_t *d, size_t n);

/* ---- survey: a generic smoke test over a whole image (counts only) ---- */
typedef struct {
    uint32_t romfs_files, romfs_dirs, exefs_files;
    uint64_t romfs_bytes;
    uint32_t garcs, garc_entries, garc_subfiles, lz11_ok, lz11_fail, lookup_fail, failures;
} WlCtrSurvey;

/* Walks the image; list (may be NULL) receives "path size" lines. garcs: open and decode GARCs. */
WlResult wl_ctr_survey(WlCtrSrc *src, int garcs, FILE *list, WlCtrSurvey *s);
