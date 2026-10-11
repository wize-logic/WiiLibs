// SPDX-License-Identifier: MIT
// BRSAR index: the NW4R sound archive "RSAR" (big-endian) with three chunks, SYMB (the labels), INFO
// (sounds, banks, players, files, groups) and FILE (the sound data). Index only: no audio is
// converted here (wave sounds: rwsd.h; stream sounds name external BRSTM files: brstm.h). Reading
// touches only the header, SYMB and INFO plus four bytes per group item, so a large archive can be
// indexed through a read callback without loading it. Layout notes: README.md. Inputs are
// untrusted: every offset, count and size is checked. No code from any decomp or tool.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_BRSAR_SOUND_SEQ 1u    /* RSEQ sequence (music-style, played by a bank) */
#define WL_BRSAR_SOUND_STREAM 2u /* streamed: an external BRSTM */
#define WL_BRSAR_SOUND_WAVE 3u   /* a wave of an RWSD */

#define WL_BRSAR_PATH_MAX 40 /* external file path field, NUL padded */
#define WL_BRSAR_STR_MAX 32  /* label field of the strings table, NUL padded (longer labels are cut) */
#define WL_BRSAR_NONE 0xFFFFFFFFu

/* File kinds (the first four bytes of a file stored in the archive). */
#define WL_BRSAR_MAGIC_RWSD 0x52575344u
#define WL_BRSAR_MAGIC_RSEQ 0x52534551u
#define WL_BRSAR_MAGIC_RBNK 0x52424E4Bu
#define WL_BRSAR_MAGIC_RWAR 0x52574152u

typedef struct {
    uint32_t string_id, file_id, player_id;
    uint8_t type, volume, priority, remote_filter, chan_prio, rel_prio;
    uint32_t info0, info1, info2; /* seq: data id, bank id, track alloc; stream: start, u16|u16; wave: wave sound index (in the RWSD), track alloc */
} WlBrsarSound;

typedef struct {
    uint32_t size, wave_size;
    uint32_t magic;       /* first four bytes of the file inside the archive (RWSD, RSEQ, RBNK, RWAR...); 0 when not stored there */
    uint32_t ext;         /* 1 when the file is external (path set) */
    uint32_t group_count; /* groups listing it */
    uint32_t first_group, first_index; /* the first (group id, item index) pair, WL_BRSAR_NONE when none */
    char path[WL_BRSAR_PATH_MAX];
} WlBrsarFile;

typedef struct {
    uint32_t string_id, item_count, first_item;
    uint32_t offset, size, wave_offset, wave_size; /* absolute offsets in the archive */
} WlBrsarGroup;

typedef struct {
    uint32_t group, file, offset, size, wave_offset, wave_size; /* offsets relative to the group's */
} WlBrsarItem;

typedef struct {
    uint32_t string_id, file_id, aux;
} WlBrsarBank;

typedef struct {
    uint32_t string_id, playable, heap;
} WlBrsarPlayer;

typedef struct {
    uint64_t archive_size;
    uint32_t version; /* e.g. 0x0103 */
    WlBrsarSound *snd;
    WlBrsarFile *file;
    WlBrsarGroup *grp;
    WlBrsarItem *item; /* every group's items, group after group */
    WlBrsarBank *bank;
    WlBrsarPlayer *player;
    uint8_t *strings; /* n_str rows of WL_BRSAR_STR_MAX bytes, row = string id */
    uint32_t n_snd, n_file, n_grp, n_item, n_bank, n_player, n_str;
    /* counts */
    uint32_t snd_seq, snd_stream, snd_wave, snd_other;
    uint32_t files_ext, files_rwsd, files_rseq, files_rbnk, files_rwar, files_other, files_unstored;
    uint32_t info_type_mismatch; /* sounds whose info reference tag differs from the sound type */
    uint32_t str_truncated, str_max, bad_items;
    uint32_t wave_data_groups;   /* groups with wave data */
    uint64_t wave_data_bytes;    /* sum of the groups' wave data sizes */
} WlBrsar;

/* Reads n bytes at off of the archive into dst; WL_OK or an error that is passed through. */
typedef WlResult (*WlBrsarReadFn)(void *user, uint64_t off, void *dst, size_t n);

/* Opens and indexes an archive of `size` bytes read through `read`. WL_ERR_FORMAT / RANGE for a
 * malformed archive, WL_ERR_NOMEM. On failure *out is cleared. */
WlResult wl_brsar_open_cb(WlBrsarReadFn read, void *user, uint64_t size, WlBrsar *out);
/* The same over an archive in memory (the bytes need not outlive the call). */
WlResult wl_brsar_open_mem(const uint8_t *data, size_t size, WlBrsar *out);
/* The same over a file (host builds only: needs wl_host). */
WlResult wl_brsar_open(const char *path, WlBrsar *out);
void wl_brsar_free(WlBrsar *a);

/* The label of string id i (NUL-terminated, at most WL_BRSAR_STR_MAX - 1 characters), or NULL. */
const char *wl_brsar_string(const WlBrsar *a, uint32_t i);

/* The external file named path (e.g. "sound/<stem>.brstm"); *file_id and *sound_id (the first sound
 * that plays it, WL_BRSAR_NONE when none) are set. Returns 1 when found. */
int wl_brsar_find_ext(const WlBrsar *a, const char *path, uint32_t *file_id, uint32_t *sound_id);

/* Where file `file_id` lives (the first group that lists it): the absolute offset and size of the
 * file and of its wave data in the archive. WL_ERR_RANGE for an external file or one in no group. */
WlResult wl_brsar_file_where(const WlBrsar *a, uint32_t file_id, uint64_t *off, uint32_t *size,
                             uint64_t *woff, uint32_t *wsize);

/* Reads file `file_id` (its bytes and its wave data, as wl_brsar_file_where finds them) into malloc'd
 * buffers; each size must be at most max bytes (WL_ERR_LIMIT otherwise). *wave is NULL when the file
 * has no wave data. For a wave sound: the RWSD and the wave data to pass to wl_rwsd_wave_sound. */
WlResult wl_brsar_read_file(const WlBrsar *a, WlBrsarReadFn read, void *user, uint32_t file_id, size_t max,
                            uint8_t **data, size_t *size, uint8_t **wave, size_t *wave_size);
