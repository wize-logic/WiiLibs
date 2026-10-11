// SPDX-License-Identifier: MIT
// Nitro (Nintendo DS) SDAT sound archive reader. SDAT is little-endian: a header, then SYMB (names),
// INFO (per-kind records), FAT (file extents) and FILE (the bytes); the files inside are SSEQ
// (sequences), SSAR (sequence archives), SBNK (instrument banks), SWAR (wave archives of SWAV waves)
// and STRM (streams). Layouts come from community format notes (GBATEK-style SDAT pages,
// vgmtrans/sdat2sseq style documentation; read as format documentation, no code copied); a summary
// is in README.md. All views are non-owning: the caller keeps the SDAT bytes alive. Inputs are
// untrusted: every offset and count is range checked. A canonical text index of an archive (host
// builds) is in index.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"

#define WL_SDAT_NONE 0xFFFFu /* "no wave archive in this slot" */

typedef enum {
    WL_SDAT_SEQ = 0, WL_SDAT_SEQARC, WL_SDAT_BANK, WL_SDAT_WAVEARC,
    WL_SDAT_PLAYER, WL_SDAT_GROUP, WL_SDAT_PLAYER2, WL_SDAT_STRM,
    WL_SDAT_KINDS
} WlSdatKind;

typedef struct {
    int present;
    uint32_t file_id;
    uint16_t bank;
    uint8_t volume, chan_prio, player_prio, player;
    const char *name; /* NULL without a SYMB block or without a name */
} WlSdatSeq;

typedef struct {
    int present;
    uint32_t file_id;
    const char *name;
} WlSdatSeqArc;

typedef struct {
    int present;
    uint32_t file_id;
    uint16_t wavearc[4]; /* WL_SDAT_NONE for an empty slot */
    const char *name;
} WlSdatBank;

typedef struct {
    int present;
    uint32_t file_id;
    const char *name;
} WlSdatWaveArc;

typedef struct {
    int present;
    uint16_t seq_max;
    uint32_t channel_mask, heap_size;
    const char *name;
} WlSdatPlayer;

typedef struct {
    uint8_t type;       /* 0 sequence, 1 sequence archive, 2 bank, 3 wave archive */
    uint16_t load_flags;
    uint32_t entry;     /* id in the matching kind */
} WlSdatGroupEntry;

typedef struct {
    int present;
    uint32_t count;
    const uint8_t *entries; /* count * 8 bytes inside the SDAT, decode with wl_sdat_group_entry */
    const char *name;
} WlSdatGroup;

typedef struct {
    int present;
    uint8_t count; /* stream channels in use (the player2 / stream player record) */
    const char *name;
} WlSdatPlayer2;

typedef struct {
    int present;
    uint32_t file_id;
    uint8_t volume, priority, player;
    const char *name;
} WlSdatStrm;

typedef struct {
    const uint8_t *data;
    size_t size;
    int has_symbols;
    uint32_t file_count;
    const uint8_t *fat; /* file_count * 16 bytes */
    uint32_t counts[WL_SDAT_KINDS];
    WlSdatSeq *seq;
    WlSdatSeqArc *seqarc;
    WlSdatBank *bank;
    WlSdatWaveArc *wavearc;
    WlSdatPlayer *player;
    WlSdatGroup *group;
    WlSdatPlayer2 *player2;
    WlSdatStrm *strm;
    /* SEQARC sub-sequence names (SYMB): per SEQARC, names[i] or NULL; owned. */
    const char ***seqarc_sub_names;
    uint32_t *seqarc_sub_count;
} WlSdat;

/* Parses the header, SYMB (optional), INFO and FAT; every record and extent is validated. */
WlResult wl_sdat_open(WlSdat *s, const uint8_t *data, size_t size);
void wl_sdat_close(WlSdat *s);
/* The bytes of FAT entry file_id. */
WlResult wl_sdat_file(const WlSdat *s, uint32_t file_id, const uint8_t **data, size_t *size);
WlResult wl_sdat_group_entry(const WlSdatGroup *g, uint32_t i, WlSdatGroupEntry *e);

/* ---- SSEQ ---- */
typedef struct {
    const uint8_t *data; /* the sequence command bytes */
    size_t size;
} WlSseq;
WlResult wl_sseq_open(WlSseq *q, const uint8_t *file, size_t size);

/* ---- SSAR: sequence archive; each entry is a sub-sequence with its own bank ---- */
typedef struct {
    const uint8_t *file;
    size_t size;
    uint32_t count;
} WlSsar;
typedef struct {
    uint32_t offset; /* of the command bytes, from the start of the SSAR file */
    uint16_t bank;
    uint8_t volume, chan_prio, player_prio, player;
} WlSsarEntry;
WlResult wl_ssar_open(WlSsar *a, const uint8_t *file, size_t size);
WlResult wl_ssar_entry(const WlSsar *a, uint32_t i, WlSsarEntry *e);

/* ---- SBNK: instrument bank ---- */
typedef enum {
    WL_SBNK_EMPTY = 0,
    WL_SBNK_PCM = 1,      /* one wave (PCM8, PCM16 or ADPCM) */
    WL_SBNK_PSG = 2,      /* pulse; "wave" field holds the duty */
    WL_SBNK_NOISE = 3,
    WL_SBNK_DIRECT = 4,   /* direct PCM (rare, accepted) */
    WL_SBNK_NULL = 5,
    WL_SBNK_DRUMSET = 16, /* one note record per key in [low, high] */
    WL_SBNK_KEYSPLIT = 17 /* up to 8 key regions */
} WlSbnkType;

typedef struct {
    uint16_t type;     /* WlSbnkType value of the note record (1 PCM, 2 PSG, 3 noise, ...) */
    uint16_t wave;     /* SWAV index in the wave archive, or the PSG duty */
    uint16_t wave_slot; /* which of the bank's 4 wave archives */
    uint8_t key, attack, decay, sustain, release, pan;
} WlSbnkNote;

typedef struct {
    uint8_t type;      /* WlSbnkType of the instrument */
    uint8_t low, high; /* drum set key range */
    uint8_t split_end[8]; /* key split region upper keys */
    uint32_t note_count;  /* 1 for single, regions for key split, keys for drum set */
    WlSbnkNote notes[128];
} WlSbnkInst;

typedef struct {
    const uint8_t *file;
    size_t size;
    uint32_t count;
} WlSbnk;
WlResult wl_sbnk_open(WlSbnk *b, const uint8_t *file, size_t size);
/* Instrument program i; a type-0 slot returns WL_OK with type WL_SBNK_EMPTY. */
WlResult wl_sbnk_inst(const WlSbnk *b, uint32_t i, WlSbnkInst *out);

/* ---- SWAR: wave archive ---- */
typedef enum { WL_SWAV_PCM8 = 0, WL_SWAV_PCM16 = 1, WL_SWAV_ADPCM = 2 } WlSwavEncoding;
typedef struct {
    const uint8_t *file;
    size_t size;
    uint32_t count;
} WlSwar;
typedef struct {
    int present; /* offset 0 = unused slot */
    uint8_t encoding;
    uint8_t loop;
    uint16_t rate;
    uint16_t time;
    uint16_t loop_start; /* in 32-bit words, as stored (ADPCM counts its 4-byte header) */
    uint32_t loop_len;   /* in 32-bit words */
    size_t data_off;     /* from the start of the SWAR file */
    size_t data_size;    /* bytes */
} WlSwavInfo;
WlResult wl_swar_open(WlSwar *w, const uint8_t *file, size_t size);
WlResult wl_swar_wave(const WlSwar *w, uint32_t i, WlSwavInfo *out);

/* ---- STRM: only the signature and header size are checked (streams pass through) ---- */
WlResult wl_strm_check(const uint8_t *file, size_t size);
