// SPDX-License-Identifier: MIT
// Nintendo DS cartridge image reader: header, NitroFS (FNT/FAT), ARM9/ARM7 binaries and overlay
// tables, plus NARC archives. References: GBATEK "DS Cartridge Header" and "DS Cartridge Nitro
// File System"; the overlay-table entry layout is in README.md. All views are non-owning: the caller keeps the ROM buffer alive. Inputs are untrusted.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "wl/base/base.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    char game_code[5];
    uint32_t arm9_off, arm9_size, arm9_ram, arm9_entry;
    uint32_t arm7_off, arm7_size, arm7_ram, arm7_entry;
    uint32_t fnt_off, fnt_size, fat_off, fat_size;
    uint32_t ovt9_off, ovt9_size, ovt7_off, ovt7_size;
    uint32_t file_count;   /* FAT entries */
    uint32_t ov9_count, ov7_count;
} WlNds;

typedef struct {
    uint32_t id, ram_addr, ram_size, bss_size, sinit_start, sinit_end;
    uint32_t file_id;
    uint32_t comp_size; /* low 24 bits of the last word */
    int compressed;     /* bit 24 of the last word */
} WlNdsOverlay;

/* Validates the header and the FAT/FNT/overlay-table ranges. */
WlResult wl_nds_open(WlNds *n, const uint8_t *data, size_t size);
/* The bytes of FAT entry file_id. */
WlResult wl_nds_file(const WlNds *n, uint32_t file_id, const uint8_t **data, size_t *size);
/* Overlay entry i of the ARM9 (arm9 != 0) or ARM7 table. */
WlResult wl_nds_overlay(const WlNds *n, int arm9, uint32_t i, WlNdsOverlay *ov);
WlResult wl_nds_overlay_data(const WlNds *n, const WlNdsOverlay *ov, const uint8_t **data,
                             size_t *size);

/* Called for every named file; a non-WL_OK return stops the walk and is returned. */
typedef WlResult (*WlNdsFileFn)(void *ud, const char *path, uint32_t file_id,
                                const uint8_t *data, size_t size);
WlResult wl_nds_walk(const WlNds *n, WlNdsFileFn fn, void *ud);

/* ---- NARC (BTAF / BTNF / GMIF) ---- */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint32_t count;
    size_t btaf_off, btnf_off, btnf_size, gmif_off, gmif_size;
    int has_names;
} WlNarc;

int wl_narc_is_narc(const uint8_t *data, size_t size);
WlResult wl_narc_open(WlNarc *a, const uint8_t *data, size_t size);
WlResult wl_narc_member(const WlNarc *a, uint32_t i, const uint8_t **data, size_t *size);
typedef WlResult (*WlNarcNameFn)(void *ud, const char *path, uint32_t index, const uint8_t *data,
                                 size_t size);
/* Walks the named members (a no-op for an archive without names). */
WlResult wl_narc_walk(const WlNarc *a, WlNarcNameFn fn, void *ud);

/* ---- survey: opens everything, prints only paths, sizes and counts ---- */
typedef struct {
    uint32_t files, narcs, narc_members, narc_failed;
    uint32_t overlays, blz_ok, blz_fail, arm9_blz;
    uint32_t lz_candidates, lz_ok, lz_fail;
    uint32_t failures; /* counted errors (NARC, overlay, ARM9, FAT) */
    uint64_t bytes;
} WlNdsSurvey;

/* list (may be NULL) receives one "path size" line per file. open_narcs also opens every NARC
 * and its members; decode also decompresses the ARM9 and all overlays and trial-decodes LZ
 * candidates. Generic: recognises no particular game. Returns WL_ERR_FAILED when failures > 0. */
WlResult wl_nds_survey(const WlNds *n, int open_narcs, int decode, FILE *list, WlNdsSurvey *s);
