// SPDX-License-Identifier: MIT
// Genius Sonority engine containers (GameCube/Wii era: Colosseum, XD, Battle Revolution):
// GSfsys.toc (archive index), FSYS archives and their LZSS entries. All big-endian. Written
// from the engine's field layouts (GStocHeader, GSfsysHeaderEx, GSlzssHeader, GSfsysEntry) as
// summarised in README.md; no code is copied. Inputs are untrusted: every offset, count and size
// is bounds-checked and decompression is capped. Portable: data comes from memory or a read
// callback (wl/gs/gs_file.h adapts a wl_host WlFile).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

/* ---- byte source (memory block or a range of a random-access medium) ---------------------- */
/* Exact-size read of n bytes at absolute offset off of the underlying medium. */
typedef WlResult (*WlGsReadFn)(void *ctx, uint64_t off, void *buf, size_t n);

typedef struct {
    const uint8_t *mem; /* when non-NULL the source is this block */
    WlGsReadFn read;    /* otherwise the range [base, base + size) read through this callback */
    void *ctx;
    uint64_t base, size;
} WlGsSrc;

void wl_gs_src_mem(WlGsSrc *s, const void *data, size_t len);
void wl_gs_src_cb(WlGsSrc *s, WlGsReadFn read, void *ctx, uint64_t base, uint64_t size);
/* Exact-size read; WL_ERR_RANGE when [off, off+n) leaves the source. */
WlResult wl_gs_src_read(const WlGsSrc *s, uint64_t off, void *buf, size_t n);

/* ---- LZSS ---------------------------------------------------------------------------------- */
#define WL_GS_LZSS_MAGIC 0x4C5A5353u /* "LZSS" */
#define WL_GS_LZSS_HDR 0x10u
#define WL_GS_MAX_ENTRY 0x10000000u /* 256 MiB cap on one decompressed entry */

/* Decodes a raw LZSS stream (no header): 4 KiB zero-filled ring starting at 4078, flag byte
 * read LSB first (1 = literal), a match is 12-bit ring position + 4-bit length + 3. Produces
 * exactly out_len bytes; WL_ERR_FORMAT when the input runs out first. Trailing input is
 * ignored. */
WlResult wl_gs_lzss_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

/* Decodes a 16-byte "LZSS" header followed by the stream: unpacked size at +4, packed size
 * (header included) at +8. Allocates *out (one extra NUL byte) of the header's unpacked size,
 * which must not exceed max. Errors: FORMAT (bad magic/sizes/stream), RANGE (over max). */
WlResult wl_gs_lzss_unpack(const uint8_t *in, size_t in_len, size_t max, uint8_t **out,
                           size_t *out_len);

/* ---- GSfsys.toc ---------------------------------------------------------------------------- */
#define WL_GS_TOC_MAGIC 0x474C4C41u /* "GLLA" */
#define WL_GS_TOC_NAME_MAX 64

typedef struct {
    uint32_t id;
    uint32_t name_off;
    uint32_t size, flags; /* 16-byte entries only, else 0 */
    char name[WL_GS_TOC_NAME_MAX];
} WlGsTocEntry;

typedef struct {
    uint32_t version;     /* 0x104 Colosseum, 0x201 XD, 0x251 Battle Revolution */
    uint32_t entry_size;  /* 8 (id, name offset) or 16 (id, name offset, size, flags) */
    uint32_t count;
    WlGsTocEntry *entries;
} WlGsToc;

/* Parses a whole GSfsys.toc image. The entry size follows the version (below 0x200: 8 bytes). */
WlResult wl_gs_toc_parse(WlGsToc *t, const uint8_t *data, size_t len);
void wl_gs_toc_free(WlGsToc *t);

/* ---- FSYS ---------------------------------------------------------------------------------- */
#define WL_GS_FSYS_MAGIC 0x46535953u /* "FSYS" */
#define WL_GS_FSYS_HDR 0x60u
#define WL_GS_FSYS_REC 0x28u         /* bytes of an entry record read */
#define WL_GS_FSYS_NAME_MAX 48
#define WL_GS_FLAG_COMPRESSED 0x80000000u /* entry flags bit 31 */

typedef struct {
    uint32_t index;
    uint32_t id;
    uint32_t offset;   /* absolute offset of the (packed) data in the archive */
    uint32_t usize;    /* unpacked size */
    uint32_t psize;    /* stored size */
    uint32_t flags;
    uint32_t type;     /* file type id, see wl_gs_type_name */
    uint32_t name_off;
    char name[WL_GS_FSYS_NAME_MAX];
    int is_lzss;       /* the stored data starts with the LZSS header */
} WlGsEntry;

typedef struct {
    WlGsSrc src;
    uint32_t count;
    uint32_t file_size_field; /* header +0x1C */
    WlGsEntry *entries;
} WlGsFsys;

/* Opens an archive: checks the magic, the entry table and every record. The source must stay
 * valid while the archive is used. */
WlResult wl_gs_fsys_open(WlGsFsys *f, const WlGsSrc *src);
void wl_gs_fsys_close(WlGsFsys *f);

/* Reads entry i's unpacked bytes into a malloc'd buffer (one extra NUL byte). LZSS entries
 * must decode to exactly usize bytes. */
WlResult wl_gs_fsys_read(const WlGsFsys *f, uint32_t i, uint8_t **data, size_t *len);
/* Same checks without keeping the bytes; for uncompressed entries only bounds are checked. */
WlResult wl_gs_fsys_verify(const WlGsFsys *f, uint32_t i);

/* ---- type dispatch ------------------------------------------------------------------------- */
#define WL_GS_TYPE_COUNT 0x1Bu /* XD's fsys_datatype table has 27 records (0x00-0x1A) */
typedef enum { WL_GS_GAME_UNKNOWN = 0, WL_GS_GAME_COLOSSEUM, WL_GS_GAME_XD, WL_GS_GAME_PBR } WlGsGame;
/* Game from a GSfsys.toc version (0x104, 0x201, 0x251). */
WlGsGame wl_gs_game_from_toc_version(uint32_t version);
/* A name for a FSYS file-type id. The id meanings differ per game, so names exist only where a
 * game's handler table is known (Battle Revolution: 2, 3, 4, 5, 6, 9, 18); every other id is
 * "type_NN" ("type_out_of_range" past the table). The returned pointer is
 * static for named ids; others use the caller's buffer (>= 24 bytes). */
const char *wl_gs_type_name(WlGsGame game, uint32_t type, char buf[24]);
