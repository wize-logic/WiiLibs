// SPDX-License-Identifier: MIT
// GameCube disc image (GCM/ISO) reader: boot.bin header, bi2, apploader, main.dol and the file
// system table (FST). Big-endian. Reference: the GameCube disc layout documented by community
// pages (Yet Another GameCube Documentation "Disc image", WiiBrew "Wii disc" for the shared
// boot.bin fields); written from the field layout only. Inputs are untrusted: every offset,
// size and name is bounds-checked and directory nesting is validated, so a hostile FST cannot
// loop or read outside the image. Wii discs are not read here: wl/wiidisc extracts them with
// dolphin-tool (Dolphin does the decryption; this library holds no keys).
// The image is read through a callback (wl_gcm_open_src) or a borrowed buffer, so the reader is
// portable; wl_gcm_open (a path) exists on host builds only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_GCM_MAGIC 0xC2339F3Du      /* boot.bin offset 0x1C */
#define WL_GCM_WII_MAGIC 0x5D1C9EA3u  /* boot.bin offset 0x18 */
#define WL_GCM_BOOT_SIZE 0x440u
#define WL_GCM_BI2_OFF 0x440u
#define WL_GCM_BI2_SIZE 0x2000u
#define WL_GCM_APPLOADER_OFF 0x2440u
#define WL_GCM_MAX_DEPTH 64u
#define WL_GCM_PATH_MAX 1024u

/* Exact-size read of n bytes at off (the reader checks the range first). */
typedef WlResult (*WlGcmReadFn)(void *ctx, uint64_t off, void *buf, size_t n);
typedef void (*WlGcmCloseFn)(void *ctx);

typedef struct WlGcm {
    WlGcmReadFn read;
    WlGcmCloseFn close;         /* may be NULL */
    void *ctx;
    uint64_t size;              /* image size in bytes */
    char game_code[5];          /* e.g. "GXXE" */
    char maker[3];
    uint8_t disc_number, version;
    char title[65];             /* boot.bin game name (NUL-terminated copy) */
    uint32_t dol_off;
    uint32_t fst_off, fst_size, fst_max;
    uint32_t apploader_size;    /* header + body + trailer, 0 when the header is implausible */
    uint8_t *fst;               /* owned copy of the FST */
    uint32_t entry_count;       /* FST entries including the root */
} WlGcm;

/* Opens a GameCube image: checks the magic, reads boot.bin and the FST, validates ranges.
 * WL_ERR_FORMAT for a non-GameCube image (a Wii disc is reported as such to the log). */
WlResult wl_gcm_open_src(WlGcm *g, WlGcmReadFn read, WlGcmCloseFn close, void *ctx,
                         uint64_t size);
/* Borrows data (it must outlive g). */
WlResult wl_gcm_open_mem(WlGcm *g, const uint8_t *data, size_t size);
/* Host builds only (gcm_file.c, needs wl_host): opens the image file at path. */
WlResult wl_gcm_open(WlGcm *g, const char *path);
/* Calls the source's close callback (also on a failed open) and clears g. */
void wl_gcm_close(WlGcm *g);

/* Exact-size read at an absolute image offset (WL_ERR_RANGE past the end). */
WlResult wl_gcm_read(const WlGcm *g, uint64_t off, void *buf, size_t n);

/* Called for every FST entry below the root in FST order, directories before their children.
 * path is relative to the root with '/' separators and no leading slash. For a directory,
 * off and size are 0. A non-WL_OK return stops the walk and is returned. */
typedef WlResult (*WlGcmWalkFn)(void *ud, const char *path, int is_dir, uint64_t off,
                                uint64_t size);
WlResult wl_gcm_walk(const WlGcm *g, WlGcmWalkFn fn, void *ud);

typedef struct {
    uint32_t files, dirs; /* the root is not counted */
    uint64_t bytes;       /* sum of file sizes */
} WlGcmCounts;
WlResult wl_gcm_count(const WlGcm *g, WlGcmCounts *c);

/* Finds a file by path (case-sensitive, '/' separators); WL_ERR_NOT_FOUND when absent. */
WlResult wl_gcm_find(const WlGcm *g, const char *path, uint64_t *off, uint64_t *size);

/* Reads [off, off+size) into a malloc'd buffer (one extra NUL byte); size must be <= max. */
WlResult wl_gcm_read_range(const WlGcm *g, uint64_t off, uint64_t size, size_t max, uint8_t **data,
                           size_t *len);

/* Size of the DOL executable at dol_off, computed from its section table. */
WlResult wl_gcm_dol_size(const WlGcm *g, uint32_t *size);
/* Reads main.dol into a malloc'd buffer (at most 16 MiB). */
WlResult wl_gcm_read_dol(const WlGcm *g, uint8_t **data, size_t *len);
