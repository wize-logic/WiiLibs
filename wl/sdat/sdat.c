// SPDX-License-Identifier: MIT
// SDAT container and the SSEQ/SSAR/SBNK/SWAR sub-file readers. Layout summary (all little-endian):
//  SDAT header (0x40): "SDAT", BOM 0xFEFF, version, file size, header size, block count, then
//   (offset, size) pairs for SYMB, INFO, FAT, FILE.
//  SYMB: eight u32 list offsets (relative to the block) in the order seq, seqarc, bank, wavearc,
//   player, group, player2, strm; a list is a u32 count and count u32 offsets of NUL-terminated
//   names (0 = unnamed). A SEQARC entry points at a pair (name, sub-name list).
//  INFO: the same eight list offsets; a list entry is the offset of a record (0 = absent).
//  FAT: "FAT ", size, count, then 16-byte entries (offset from the SDAT start, size, 8 reserved).
// Reference: community SDAT format notes (read as documentation, no code copied).
#include "wl/sdat/sdat.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

#define HDR_SIZE 0x40u

static int tag_is(const uint8_t *p, const char *t) {
    return memcmp(p, t, 4) == 0;
}

/* Validated view of one block: [off, off+size) lies in the file and starts with the tag. */
typedef struct {
    size_t off, size;
} Blk;

static WlResult blk_get(const uint8_t *d, size_t n, size_t off, size_t size, const char *tag,
                        int optional, Blk *b) {
    b->off = 0;
    b->size = 0;
    if (off == 0 && size == 0 && optional) {
        return WL_OK;
    }
    if (size < 8 || !wl_range_ok(off, size, n) || !tag_is(d + off, tag)) {
        return WL_ERR_FORMAT;
    }
    /* Some archives record an unpadded size in the SDAT header; the block's own size field (padded)
     * is the extent, and it must still lie inside the file. */
    size_t own = wl_rd_le32(d + off + 4);
    if (own < size) {
        own = size;
    }
    if (own > size + 3u || !wl_range_ok(off, own, n)) {
        return WL_ERR_FORMAT;
    }
    b->off = off;
    b->size = own;
    return WL_OK;
}

/* A NUL-terminated string at block offset o, or NULL when out of range or unterminated. */
static const char *symb_str(const uint8_t *d, const Blk *b, uint32_t o) {
    if (o == 0 || o >= b->size) {
        return NULL;
    }
    const uint8_t *p = d + b->off + o;
    size_t room = b->size - o;
    return memchr(p, 0, room) != NULL ? (const char *)p : NULL;
}

/* The list of kind k in a SYMB/INFO block: count and the offset of its first u32. */
static WlResult list_get(const uint8_t *d, const Blk *b, unsigned k, uint32_t *count,
                         size_t *items) {
    if (b->size < 8u + 32u) {
        return WL_ERR_FORMAT;
    }
    uint32_t lo = wl_rd_le32(d + b->off + 8 + 4u * k);
    if (!wl_range_ok(lo, 4, b->size)) {
        return WL_ERR_RANGE;
    }
    uint32_t c = wl_rd_le32(d + b->off + lo);
    if (!wl_range_ok((uint64_t)lo + 4u, (uint64_t)c * 4u, b->size)) {
        return WL_ERR_RANGE;
    }
    *count = c;
    *items = b->off + lo + 4u;
    return WL_OK;
}

/* Record at the offset stored in item i: NULL when absent; error when it does not fit rec_size. */
static WlResult rec_get(const uint8_t *d, const Blk *info, size_t items, uint32_t i,
                        size_t rec_size, const uint8_t **rec) {
    uint32_t ro = wl_rd_le32(d + items + 4u * i);
    if (ro == 0) {
        *rec = NULL;
        return WL_OK;
    }
    if (!wl_range_ok(ro, rec_size, info->size)) {
        return WL_ERR_RANGE;
    }
    *rec = d + info->off + ro;
    return WL_OK;
}

static const char *sym_name(const uint8_t *d, const Blk *sy, unsigned kind, uint32_t i) {
    uint32_t c;
    size_t it;
    if (sy->size == 0 || list_get(d, sy, kind, &c, &it) != WL_OK || i >= c) {
        return NULL;
    }
    uint32_t o = wl_rd_le32(d + it + 4u * i);
    if (kind == WL_SDAT_SEQARC) { /* the entry is a pair (name offset, sub-name list offset) */
        if (o == 0 || !wl_range_ok(o, 8, sy->size)) {
            return NULL;
        }
        o = wl_rd_le32(d + sy->off + o);
    }
    return symb_str(d, sy, o);
}

static void *zalloc(size_t count, size_t each) {
    return count == 0 ? NULL : calloc(count, each);
}

void wl_sdat_close(WlSdat *s) {
    if (s == NULL) {
        return;
    }
    free(s->seq);
    free(s->seqarc);
    free(s->bank);
    free(s->wavearc);
    free(s->player);
    free(s->group);
    free(s->player2);
    free(s->strm);
    if (s->seqarc_sub_names != NULL) {
        for (uint32_t i = 0; i < s->counts[WL_SDAT_SEQARC]; i++) {
            free((void *)s->seqarc_sub_names[i]);
        }
    }
    free((void *)s->seqarc_sub_names);
    free(s->seqarc_sub_count);
    memset(s, 0, sizeof *s);
}

/* SEQARC symbol entries: a pair (name offset, offset of a sub-name list). Best effort: a bad
 * entry just leaves that archive's sub-sequences unnamed. */
static WlResult load_seqarc_subs(WlSdat *s, const Blk *sy) {
    uint32_t n = s->counts[WL_SDAT_SEQARC];
    if (n == 0) {
        return WL_OK;
    }
    s->seqarc_sub_names = zalloc(n, sizeof(char **));
    s->seqarc_sub_count = zalloc(n, sizeof(uint32_t));
    if (s->seqarc_sub_names == NULL || s->seqarc_sub_count == NULL) {
        return WL_ERR_NOMEM;
    }
    uint32_t c;
    size_t it;
    if (sy->size == 0 || list_get(s->data, sy, WL_SDAT_SEQARC, &c, &it) != WL_OK) {
        return WL_OK;
    }
    for (uint32_t i = 0; i < n && i < c; i++) {
        uint32_t eo = wl_rd_le32(s->data + it + 4u * i);
        if (eo == 0 || !wl_range_ok(eo, 8, sy->size)) {
            continue;
        }
        uint32_t so = wl_rd_le32(s->data + sy->off + eo + 4);
        if (so == 0 || !wl_range_ok(so, 4, sy->size)) {
            continue;
        }
        uint32_t sc = wl_rd_le32(s->data + sy->off + so);
        if (!wl_range_ok((uint64_t)so + 4u, (uint64_t)sc * 4u, sy->size)) {
            continue;
        }
        const char **names = zalloc(sc, sizeof(char *));
        if (sc != 0 && names == NULL) {
            return WL_ERR_NOMEM;
        }
        for (uint32_t k = 0; k < sc; k++) {
            names[k] = symb_str(s->data, sy, wl_rd_le32(s->data + sy->off + so + 4u + 4u * k));
        }
        s->seqarc_sub_names[i] = names;
        s->seqarc_sub_count[i] = sc;
    }
    return WL_OK;
}

#define LOAD(KIND, ARR, T)                                                                      \
    do {                                                                                        \
        uint32_t c_;                                                                            \
        size_t it_;                                                                             \
        r = list_get(data, &info, KIND, &c_, &it_);                                             \
        if (r != WL_OK) {                                                                       \
            goto fail;                                                                          \
        }                                                                                       \
        s->counts[KIND] = c_;                                                                   \
        s->ARR = zalloc(c_, sizeof(T));                                                         \
        if (c_ != 0 && s->ARR == NULL) {                                                        \
            r = WL_ERR_NOMEM;                                                               \
            goto fail;                                                                          \
        }                                                                                       \
        for (uint32_t i = 0; i < c_; i++) {                                                     \
            const uint8_t *p_;                                                                  \
            r = rec_get(data, &info, it_, i, REC_##KIND, &p_);                                  \
            if (r != WL_OK) {                                                                   \
                goto fail;                                                                      \
            }                                                                                   \
            if (p_ == NULL) {                                                                   \
                continue;                                                                       \
            }                                                                                   \
            s->ARR[i].present = 1;                                                              \
            s->ARR[i].name = sym_name(data, &symb, KIND, i);                                    \
            FILL_##KIND                                                                         \
        }                                                                                       \
    } while (0)

#define REC_WL_SDAT_SEQ 10u
#define REC_WL_SDAT_SEQARC 4u
#define REC_WL_SDAT_BANK 12u
#define REC_WL_SDAT_WAVEARC 4u
#define REC_WL_SDAT_PLAYER 12u
#define REC_WL_SDAT_GROUP 4u
#define REC_WL_SDAT_PLAYER2 17u
#define REC_WL_SDAT_STRM 8u

#define FILL_WL_SDAT_SEQ                                                                        \
    s->seq[i].file_id = wl_rd_le16(p_);                                                         \
    s->seq[i].bank = wl_rd_le16(p_ + 4);                                                        \
    s->seq[i].volume = p_[6];                                                                   \
    s->seq[i].chan_prio = p_[7];                                                                \
    s->seq[i].player_prio = p_[8];                                                              \
    s->seq[i].player = p_[9];
#define FILL_WL_SDAT_SEQARC s->seqarc[i].file_id = wl_rd_le16(p_);
#define FILL_WL_SDAT_BANK                                                                       \
    s->bank[i].file_id = wl_rd_le16(p_);                                                        \
    for (int w_ = 0; w_ < 4; w_++) {                                                            \
        s->bank[i].wavearc[w_] = wl_rd_le16(p_ + 4 + 2 * w_);                                   \
    }
#define FILL_WL_SDAT_WAVEARC s->wavearc[i].file_id = wl_rd_le16(p_);
#define FILL_WL_SDAT_PLAYER                                                                     \
    s->player[i].seq_max = wl_rd_le16(p_);                                                      \
    s->player[i].channel_mask = wl_rd_le32(p_ + 4);                                             \
    s->player[i].heap_size = wl_rd_le32(p_ + 8);
#define FILL_WL_SDAT_PLAYER2 s->player2[i].count = p_[0];
#define FILL_WL_SDAT_STRM                                                                       \
    s->strm[i].file_id = wl_rd_le16(p_);                                                        \
    s->strm[i].volume = p_[4];                                                                  \
    s->strm[i].priority = p_[5];                                                                \
    s->strm[i].player = p_[6];

WlResult wl_sdat_open(WlSdat *s, const uint8_t *data, size_t size) {
    if (s == NULL || data == NULL) {
        return WL_ERR_ARG;
    }
    memset(s, 0, sizeof *s);
    if (size < HDR_SIZE || !tag_is(data, "SDAT") || wl_rd_le16(data + 4) != 0xFEFFu) {
        return WL_ERR_FORMAT;
    }
    size_t fsize = wl_rd_le32(data + 8);
    if (fsize < HDR_SIZE || fsize > size || wl_rd_le16(data + 12) < 0x20) {
        return WL_ERR_FORMAT;
    }
    size_t n = fsize; /* nothing past the declared end is read */
    s->data = data;
    s->size = n;
    Blk symb, info, fat, file;
    WlResult r = blk_get(data, n, wl_rd_le32(data + 16), wl_rd_le32(data + 20), "SYMB", 1, &symb);
    if (r == WL_OK) {
        r = blk_get(data, n, wl_rd_le32(data + 24), wl_rd_le32(data + 28), "INFO", 0, &info);
    }
    if (r == WL_OK) {
        r = blk_get(data, n, wl_rd_le32(data + 32), wl_rd_le32(data + 36), "FAT ", 0, &fat);
    }
    if (r == WL_OK) {
        r = blk_get(data, n, wl_rd_le32(data + 40), wl_rd_le32(data + 44), "FILE", 0, &file);
    }
    if (r != WL_OK) {
        memset(s, 0, sizeof *s);
        return r;
    }
    s->has_symbols = symb.size != 0;
    if (fat.size < 12) {
        memset(s, 0, sizeof *s);
        return WL_ERR_FORMAT;
    }
    uint32_t fc = wl_rd_le32(data + fat.off + 8);
    if (!wl_range_ok(12, (uint64_t)fc * 16u, fat.size)) {
        memset(s, 0, sizeof *s);
        return WL_ERR_RANGE;
    }
    s->file_count = fc;
    s->fat = data + fat.off + 12;

    LOAD(WL_SDAT_SEQ, seq, WlSdatSeq);
    LOAD(WL_SDAT_SEQARC, seqarc, WlSdatSeqArc);
    LOAD(WL_SDAT_BANK, bank, WlSdatBank);
    LOAD(WL_SDAT_WAVEARC, wavearc, WlSdatWaveArc);
    LOAD(WL_SDAT_PLAYER, player, WlSdatPlayer);
    LOAD(WL_SDAT_PLAYER2, player2, WlSdatPlayer2);
    LOAD(WL_SDAT_STRM, strm, WlSdatStrm);
    {
        /* groups: record = u32 entry count, then count 8-byte entries */
        uint32_t c;
        size_t it;
        r = list_get(data, &info, WL_SDAT_GROUP, &c, &it);
        if (r != WL_OK) {
            goto fail;
        }
        s->counts[WL_SDAT_GROUP] = c;
        s->group = zalloc(c, sizeof(WlSdatGroup));
        if (c != 0 && s->group == NULL) {
            r = WL_ERR_NOMEM;
            goto fail;
        }
        for (uint32_t i = 0; i < c; i++) {
            const uint8_t *p;
            r = rec_get(data, &info, it, i, 4, &p);
            if (r != WL_OK) {
                goto fail;
            }
            if (p == NULL) {
                continue;
            }
            uint32_t ec = wl_rd_le32(p);
            size_t at = (size_t)(p - (data + info.off)) + 4u;
            if (!wl_range_ok(at, (uint64_t)ec * 8u, info.size)) {
                r = WL_ERR_RANGE;
                goto fail;
            }
            s->group[i].present = 1;
            s->group[i].count = ec;
            s->group[i].entries = p + 4;
            s->group[i].name = sym_name(data, &symb, WL_SDAT_GROUP, i);
        }
    }
    r = load_seqarc_subs(s, &symb);
    if (r != WL_OK) {
        goto fail;
    }
    return WL_OK;
fail:
    wl_sdat_close(s);
    return r;
}

WlResult wl_sdat_file(const WlSdat *s, uint32_t id, const uint8_t **data, size_t *size) {
    if (s == NULL || data == NULL || size == NULL) {
        return WL_ERR_ARG;
    }
    if (id >= s->file_count) {
        return WL_ERR_RANGE;
    }
    const uint8_t *e = s->fat + 16u * id;
    uint32_t off = wl_rd_le32(e), sz = wl_rd_le32(e + 4);
    if (!wl_range_ok(off, sz, s->size)) {
        return WL_ERR_RANGE;
    }
    *data = s->data + off;
    *size = sz;
    return WL_OK;
}

WlResult wl_sdat_group_entry(const WlSdatGroup *g, uint32_t i, WlSdatGroupEntry *e) {
    if (g == NULL || e == NULL || !g->present) {
        return WL_ERR_ARG;
    }
    if (i >= g->count) {
        return WL_ERR_RANGE;
    }
    const uint8_t *p = g->entries + 8u * i;
    e->type = p[0];
    e->load_flags = wl_rd_le16(p + 2);
    e->entry = wl_rd_le32(p + 4);
    return WL_OK;
}

/* ---- common sub-file header: tag, BOM, version, size, header size 16, block count ---- */
static WlResult sub_hdr(const uint8_t *f, size_t n, const char *tag, size_t *fsize) {
    if (f == NULL || n < 0x14 || !tag_is(f, tag) || wl_rd_le16(f + 4) != 0xFEFFu) {
        return WL_ERR_FORMAT;
    }
    size_t sz = wl_rd_le32(f + 8);
    if (sz < 0x14 || sz > n || wl_rd_le16(f + 12) < 16 || !tag_is(f + 16, "DATA")) {
        return WL_ERR_FORMAT;
    }
    *fsize = sz;
    return WL_OK;
}

WlResult wl_sseq_open(WlSseq *q, const uint8_t *f, size_t n) {
    size_t sz;
    if (q == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = sub_hdr(f, n, "SSEQ", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x1C) {
        return WL_ERR_FORMAT;
    }
    uint32_t doff = wl_rd_le32(f + 24);
    if (doff < 0x1Cu || doff > sz) {
        return WL_ERR_RANGE;
    }
    q->data = f + doff;
    q->size = sz - doff;
    return WL_OK;
}

WlResult wl_ssar_open(WlSsar *a, const uint8_t *f, size_t n) {
    size_t sz;
    if (a == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = sub_hdr(f, n, "SSAR", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x20) {
        return WL_ERR_FORMAT;
    }
    uint32_t c = wl_rd_le32(f + 28);
    if (!wl_range_ok(0x20, (uint64_t)c * 12u, sz)) {
        return WL_ERR_RANGE;
    }
    a->file = f;
    a->size = sz;
    a->count = c;
    return WL_OK;
}

WlResult wl_ssar_entry(const WlSsar *a, uint32_t i, WlSsarEntry *e) {
    if (a == NULL || e == NULL) {
        return WL_ERR_ARG;
    }
    if (i >= a->count) {
        return WL_ERR_RANGE;
    }
    const uint8_t *p = a->file + 0x20 + 12u * i;
    e->offset = wl_rd_le32(p);
    e->bank = wl_rd_le16(p + 4);
    e->volume = p[6];
    e->chan_prio = p[7];
    e->player_prio = p[8];
    e->player = p[9];
    if (e->offset >= a->size) {
        return WL_ERR_RANGE;
    }
    return WL_OK;
}

/* ---- SBNK ---- */
WlResult wl_sbnk_open(WlSbnk *b, const uint8_t *f, size_t n) {
    size_t sz;
    if (b == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = sub_hdr(f, n, "SBNK", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x3C) {
        return WL_ERR_FORMAT;
    }
    uint32_t c = wl_rd_le32(f + 0x38);
    if (!wl_range_ok(0x3C, (uint64_t)c * 4u, sz)) {
        return WL_ERR_RANGE;
    }
    b->file = f;
    b->size = sz;
    b->count = c;
    return WL_OK;
}

/* A note record without its leading u16 type: wave, wave slot, key, ADSR, pan (10 bytes). */
static void note_from(WlSbnkNote *nt, uint16_t type, const uint8_t *p) {
    nt->type = type;
    nt->wave = wl_rd_le16(p);
    nt->wave_slot = wl_rd_le16(p + 2);
    nt->key = p[4];
    nt->attack = p[5];
    nt->decay = p[6];
    nt->sustain = p[7];
    nt->release = p[8];
    nt->pan = p[9];
}

WlResult wl_sbnk_inst(const WlSbnk *b, uint32_t i, WlSbnkInst *out) {
    if (b == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (i >= b->count) {
        return WL_ERR_RANGE;
    }
    memset(out, 0, sizeof *out);
    const uint8_t *rec = b->file + 0x3C + 4u * i;
    uint8_t type = rec[0];
    uint32_t off = wl_rd_le16(rec + 1);
    out->type = type;
    if (type == WL_SBNK_EMPTY) {
        return WL_OK;
    }
    if (type >= 1 && type <= 5) {
        if (!wl_range_ok(off, 10, b->size)) {
            return WL_ERR_RANGE;
        }
        out->note_count = 1;
        note_from(&out->notes[0], type, b->file + off);
        return WL_OK;
    }
    if (type == WL_SBNK_DRUMSET) {
        if (!wl_range_ok(off, 2, b->size)) {
            return WL_ERR_RANGE;
        }
        out->low = b->file[off];
        out->high = b->file[off + 1];
        if (out->high < out->low || out->high > 127) {
            return WL_ERR_FORMAT;
        }
        uint32_t cnt = (uint32_t)out->high - out->low + 1u;
        if (!wl_range_ok(off + 2u, (uint64_t)cnt * 12u, b->size)) {
            return WL_ERR_RANGE;
        }
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t *p = b->file + off + 2u + 12u * k;
            note_from(&out->notes[k], wl_rd_le16(p), p + 2);
        }
        out->note_count = cnt;
        return WL_OK;
    }
    if (type == WL_SBNK_KEYSPLIT) {
        if (!wl_range_ok(off, 8, b->size)) {
            return WL_ERR_RANGE;
        }
        uint32_t cnt = 0;
        while (cnt < 8 && b->file[off + cnt] != 0) {
            out->split_end[cnt] = b->file[off + cnt];
            cnt++;
        }
        if (!wl_range_ok(off + 8u, (uint64_t)cnt * 12u, b->size)) {
            return WL_ERR_RANGE;
        }
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t *p = b->file + off + 8u + 12u * k;
            note_from(&out->notes[k], wl_rd_le16(p), p + 2);
        }
        out->note_count = cnt;
        return WL_OK;
    }
    return WL_ERR_FORMAT;
}

/* ---- SWAR ---- */
WlResult wl_swar_open(WlSwar *w, const uint8_t *f, size_t n) {
    size_t sz;
    if (w == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = sub_hdr(f, n, "SWAR", &sz);
    if (r != WL_OK) {
        return r;
    }
    if (sz < 0x3C) {
        return WL_ERR_FORMAT;
    }
    uint32_t c = wl_rd_le32(f + 0x38);
    if (!wl_range_ok(0x3C, (uint64_t)c * 4u, sz)) {
        return WL_ERR_RANGE;
    }
    w->file = f;
    w->size = sz;
    w->count = c;
    return WL_OK;
}

WlResult wl_swar_wave(const WlSwar *w, uint32_t i, WlSwavInfo *o) {
    if (w == NULL || o == NULL) {
        return WL_ERR_ARG;
    }
    if (i >= w->count) {
        return WL_ERR_RANGE;
    }
    memset(o, 0, sizeof *o);
    uint32_t off = wl_rd_le32(w->file + 0x3C + 4u * i);
    if (off == 0) {
        return WL_OK;
    }
    if (!wl_range_ok(off, 12, w->size)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *p = w->file + off;
    o->present = 1;
    o->encoding = p[0];
    o->loop = p[1];
    o->rate = wl_rd_le16(p + 2);
    o->time = wl_rd_le16(p + 4);
    o->loop_start = wl_rd_le16(p + 6);
    o->loop_len = wl_rd_le32(p + 8);
    uint64_t words = (uint64_t)o->loop_start + o->loop_len;
    if (o->encoding > WL_SWAV_ADPCM || !wl_range_ok(off + 12u, words * 4u, w->size)) {
        return o->encoding > WL_SWAV_ADPCM ? WL_ERR_FORMAT : WL_ERR_RANGE;
    }
    o->data_off = off + 12u;
    o->data_size = (size_t)(words * 4u);
    return WL_OK;
}

WlResult wl_strm_check(const uint8_t *f, size_t n) {
    if (f == NULL || n < 0x10 || !tag_is(f, "STRM") || wl_rd_le16(f + 4) != 0xFEFFu) {
        return WL_ERR_FORMAT;
    }
    size_t sz = wl_rd_le32(f + 8);
    return sz >= 0x10 && sz <= n ? WL_OK : WL_ERR_RANGE;
}
