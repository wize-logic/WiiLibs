// SPDX-License-Identifier: MIT
// BRSAR (NW4R "RSAR") index. Layout notes: README.md.
#include "wl/brstm/brsar.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

#define RSAR_HDR 0x40u
#define MAX_ENTRIES (1u << 20)
#define MAX_ITEMS (1u << 22)
#define MAX_CHUNK (64u << 20)

/* The archive as a read callback. */
typedef struct {
    WlBrsarReadFn read;
    void *user;
} Src;

static WlResult src_read(const Src *f, uint64_t off, void *dst, size_t n) {
    return f->read(f->user, off, dst, n);
}

/* Everything the INFO chunk holds is addressed from the start of its payload (8 bytes after the chunk). */
typedef struct {
    const uint8_t *p;
    size_t size; /* payload bytes */
} Info;

/* A reference is a marker word (0x01000000 for an address, 0 for none; the second byte tells what it points
 * to) and an offset. Returns 1 and the target offset when it is an address with need bytes behind it, 0
 * for a null reference (*isnull set), -1 for a bad one. */
static int info_ref(const Info *v, uint64_t pos, size_t need, size_t *target, uint32_t *marker) {
    if (pos > v->size || v->size - pos < 8u) {
        return -1;
    }
    uint32_t m = wl_rd_be32(v->p + pos), off = wl_rd_be32(v->p + pos + 4);
    if (marker != NULL) {
        *marker = m;
    }
    if (m == 0u) {
        return 0;
    }
    if ((m >> 24) != 1u || off > v->size || v->size - off < need) {
        return -1;
    }
    *target = off;
    return 1;
}

/* A table: u32 count, then count references. */
static WlResult info_table(const Info *v, uint32_t ref_pos, uint32_t max, size_t *t, uint32_t *count) {
    if (info_ref(v, ref_pos, 4, t, NULL) != 1) {
        return WL_ERR_RANGE;
    }
    uint32_t n = wl_rd_be32(v->p + *t);
    if (n > max || (v->size - *t - 4u) / 8u < n) {
        return WL_ERR_RANGE;
    }
    *count = n;
    return WL_OK;
}

void wl_brsar_free(WlBrsar *a) {
    if (a == NULL) {
        return;
    }
    free(a->snd);
    free(a->file);
    free(a->grp);
    free(a->item);
    free(a->bank);
    free(a->player);
    free(a->strings);
    memset(a, 0, sizeof *a);
}

static WlResult read_chunk(const Src *f, uint64_t size, uint32_t off, uint32_t csize, const char *magic, uint8_t **out) {
    if (csize < 8u || csize > MAX_CHUNK || off < RSAR_HDR || (uint64_t)off + csize > size) {
        return WL_ERR_RANGE;
    }
    uint8_t *b = (uint8_t *)malloc(csize);
    if (b == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = src_read(f, off, b, csize);
    if (r == WL_OK && (memcmp(b, magic, 4) != 0 || wl_rd_be32(b + 4) != csize)) {
        r = WL_ERR_FORMAT;
    }
    if (r != WL_OK) {
        free(b);
        return r;
    }
    *out = b;
    return WL_OK;
}

typedef struct {
    uint32_t symb_off, symb_size, info_off, info_size, file_off, file_size;
} Layout;

static WlResult read_header(const Src *f, uint64_t size, Layout *l, uint32_t *version) {
    uint8_t h[RSAR_HDR];
    if (size < RSAR_HDR) {
        return WL_ERR_RANGE;
    }
    WlResult r = src_read(f, 0, h, sizeof h);
    if (r != WL_OK) {
        return r;
    }
    if (memcmp(h, "RSAR", 4) != 0 || wl_rd_be16(h + 4) != 0xFEFFu || h[6] != 1u || wl_rd_be32(h + 8) != size ||
        wl_rd_be16(h + 12) != RSAR_HDR || wl_rd_be16(h + 14) != 3u) {
        return WL_ERR_FORMAT;
    }
    *version = wl_rd_be16(h + 6);
    l->symb_off = wl_rd_be32(h + 0x10);
    l->symb_size = wl_rd_be32(h + 0x14);
    l->info_off = wl_rd_be32(h + 0x18);
    l->info_size = wl_rd_be32(h + 0x1C);
    l->file_off = wl_rd_be32(h + 0x20);
    l->file_size = wl_rd_be32(h + 0x24);
    if ((uint64_t)l->file_off + l->file_size > size || l->file_off < RSAR_HDR) {
        return WL_ERR_RANGE;
    }
    return WL_OK;
}

/* ---- strings (SYMB) --------------------------------------------------------------------------- */
static WlResult index_strings(const uint8_t *sy, size_t n, WlBrsar *a) {
    /* SYMB: tag, size, then (from +8) the offset of the string table and four lookup-tree offsets; the
     * table is a count and one offset (from +8) per string. */
    if (n < 0x20u) {
        return WL_ERR_RANGE;
    }
    uint32_t to = wl_rd_be32(sy + 8);
    if (to > n - 8u || n - 8u - to < 4u) {
        return WL_ERR_RANGE;
    }
    uint32_t cnt = wl_rd_be32(sy + 8 + to);
    if (cnt > MAX_ENTRIES || (n - 8u - to - 4u) / 4u < cnt) {
        return WL_ERR_RANGE;
    }
    a->strings = (uint8_t *)calloc((size_t)cnt + 1u, WL_BRSAR_STR_MAX);
    if (a->strings == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t o = wl_rd_be32(sy + 8 + to + 4 + 4u * i);
        if (o >= n - 8u) {
            return WL_ERR_RANGE;
        }
        const uint8_t *s = sy + 8 + o;
        size_t max = n - 8u - o, len = 0;
        while (len < max && s[len] != 0u) {
            len++;
        }
        if (len == max) {
            return WL_ERR_FORMAT; /* no terminator */
        }
        if (len > a->str_max) {
            a->str_max = (uint32_t)len;
        }
        size_t keep = len;
        if (keep >= WL_BRSAR_STR_MAX) {
            keep = WL_BRSAR_STR_MAX - 1u;
            a->str_truncated++;
        }
        memcpy(a->strings + (size_t)i * WL_BRSAR_STR_MAX, s, keep);
    }
    a->n_str = cnt;
    return WL_OK;
}

/* ---- INFO ------------------------------------------------------------------------------------- */
static WlResult index_sounds(const Info *v, uint32_t ref_pos, WlBrsar *a) {
    size_t t;
    uint32_t n;
    WlResult r = info_table(v, ref_pos, MAX_ENTRIES, &t, &n);
    if (r != WL_OK) {
        return r;
    }
    a->snd = (WlBrsarSound *)calloc((size_t)n + 1u, sizeof *a->snd);
    if (a->snd == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        size_t e, in;
        uint32_t mk = 0;
        if (info_ref(v, t + 4u + 8u * i, 0x20, &e, NULL) != 1) {
            return WL_ERR_RANGE;
        }
        const uint8_t *p = v->p + e;
        WlBrsarSound *s = &a->snd[i];
        s->string_id = wl_rd_be32(p);
        s->file_id = wl_rd_be32(p + 4);
        s->player_id = wl_rd_be32(p + 8);
        s->volume = p[0x14];
        s->priority = p[0x15];
        s->type = p[0x16];
        s->remote_filter = p[0x17];
        size_t need = s->type == WL_BRSAR_SOUND_SEQ ? 14u : (s->type == WL_BRSAR_SOUND_STREAM ? 8u : 10u);
        if (info_ref(v, e + 0x18, need, &in, &mk) != 1) {
            return WL_ERR_RANGE;
        }
        if (((mk >> 16) & 0xFFu) != s->type) {
            a->info_type_mismatch++;
        }
        const uint8_t *q = v->p + in;
        switch (s->type) {
        case WL_BRSAR_SOUND_SEQ:
            s->info0 = wl_rd_be32(q);
            s->info1 = wl_rd_be32(q + 4);
            s->info2 = wl_rd_be32(q + 8);
            s->chan_prio = q[12];
            s->rel_prio = q[13];
            a->snd_seq++;
            break;
        case WL_BRSAR_SOUND_STREAM:
            s->info0 = wl_rd_be32(q);
            s->info1 = wl_rd_be32(q + 4);
            a->snd_stream++;
            break;
        case WL_BRSAR_SOUND_WAVE:
            s->info0 = wl_rd_be32(q);
            s->info1 = wl_rd_be32(q + 4);
            s->chan_prio = q[8];
            s->rel_prio = q[9];
            a->snd_wave++;
            break;
        default:
            a->snd_other++;
            break;
        }
    }
    a->n_snd = n;
    return WL_OK;
}

static WlResult index_files(const Info *v, uint32_t ref_pos, WlBrsar *a) {
    size_t t;
    uint32_t n;
    WlResult r = info_table(v, ref_pos, MAX_ENTRIES, &t, &n);
    if (r != WL_OK) {
        return r;
    }
    a->file = (WlBrsarFile *)calloc((size_t)n + 1u, sizeof *a->file);
    if (a->file == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        size_t e, sp, lp;
        if (info_ref(v, t + 4u + 8u * i, 0x1C, &e, NULL) != 1) {
            return WL_ERR_RANGE;
        }
        WlBrsarFile *f = &a->file[i];
        const uint8_t *p = v->p + e;
        f->size = wl_rd_be32(p);
        f->wave_size = wl_rd_be32(p + 4);
        f->first_group = f->first_index = WL_BRSAR_NONE;
        int sr = info_ref(v, e + 0x0C, 1, &sp, NULL);
        if (sr < 0) {
            return WL_ERR_RANGE;
        }
        if (sr == 1) { /* external file path */
            size_t max = v->size - sp, len = 0;
            while (len < max && v->p[sp + len] != 0u) {
                len++;
            }
            if (len == max || len >= WL_BRSAR_PATH_MAX) {
                return WL_ERR_FORMAT;
            }
            memcpy(f->path, v->p + sp, len);
            f->ext = 1;
            a->files_ext++;
        }
        int lr = info_ref(v, e + 0x14, 4, &lp, NULL);
        if (lr < 0) {
            return WL_ERR_RANGE;
        }
        if (lr == 1) { /* the groups that list the file: (group id, item index) pairs */
            uint32_t cnt = wl_rd_be32(v->p + lp);
            if (cnt > MAX_ENTRIES || (v->size - lp - 4u) / 8u < cnt) {
                return WL_ERR_RANGE;
            }
            f->group_count = cnt;
            if (cnt > 0u) {
                size_t pr;
                if (info_ref(v, lp + 4u, 8, &pr, NULL) != 1) {
                    return WL_ERR_RANGE;
                }
                f->first_group = wl_rd_be32(v->p + pr);
                f->first_index = wl_rd_be32(v->p + pr + 4);
            }
        }
    }
    a->n_file = n;
    return WL_OK;
}

static WlResult index_groups(const Info *v, uint32_t ref_pos, WlBrsar *a) {
    size_t t;
    uint32_t n;
    WlResult r = info_table(v, ref_pos, MAX_ENTRIES, &t, &n);
    if (r != WL_OK) {
        return r;
    }
    a->grp = (WlBrsarGroup *)calloc((size_t)n + 1u, sizeof *a->grp);
    if (a->grp == NULL) {
        return WL_ERR_NOMEM;
    }
    size_t cap = 0, used = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t e, lp;
        if (info_ref(v, t + 4u + 8u * i, 0x28, &e, NULL) != 1) {
            return WL_ERR_RANGE;
        }
        WlBrsarGroup *g = &a->grp[i];
        const uint8_t *p = v->p + e;
        g->string_id = wl_rd_be32(p);
        g->offset = wl_rd_be32(p + 0x10);
        g->size = wl_rd_be32(p + 0x14);
        g->wave_offset = wl_rd_be32(p + 0x18);
        g->wave_size = wl_rd_be32(p + 0x1C);
        g->first_item = (uint32_t)used;
        if (info_ref(v, e + 0x20, 4, &lp, NULL) != 1) {
            return WL_ERR_RANGE;
        }
        uint32_t cnt = wl_rd_be32(v->p + lp);
        if (cnt > MAX_ITEMS || (v->size - lp - 4u) / 8u < cnt || used + cnt > MAX_ITEMS) {
            return WL_ERR_RANGE;
        }
        g->item_count = cnt;
        if (used + cnt > cap) {
            size_t nc = cap ? cap : 1024u;
            while (nc < used + cnt) {
                nc *= 2u;
            }
            WlBrsarItem *ni = (WlBrsarItem *)realloc(a->item, nc * sizeof *ni);
            if (ni == NULL) {
                return WL_ERR_NOMEM;
            }
            a->item = ni;
            cap = nc;
        }
        for (uint32_t k = 0; k < cnt; k++) {
            size_t ip;
            if (info_ref(v, lp + 4u + 8u * k, 24, &ip, NULL) != 1) {
                return WL_ERR_RANGE;
            }
            WlBrsarItem *it = &a->item[used++];
            it->group = i;
            it->file = wl_rd_be32(v->p + ip);
            it->offset = wl_rd_be32(v->p + ip + 4);
            it->size = wl_rd_be32(v->p + ip + 8);
            it->wave_offset = wl_rd_be32(v->p + ip + 12);
            it->wave_size = wl_rd_be32(v->p + ip + 16);
        }
        if (g->wave_size != 0u) {
            a->wave_data_groups++;
            a->wave_data_bytes += g->wave_size;
        }
    }
    a->n_grp = n;
    a->n_item = (uint32_t)used;
    return WL_OK;
}

static WlResult index_banks_players(const Info *v, WlBrsar *a) {
    size_t t;
    uint32_t n;
    WlResult r = info_table(v, 8, MAX_ENTRIES, &t, &n); /* banks: second reference of INFO */
    if (r != WL_OK) {
        return r;
    }
    a->bank = (WlBrsarBank *)calloc((size_t)n + 1u, sizeof *a->bank);
    if (a->bank == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        size_t e;
        if (info_ref(v, t + 4u + 8u * i, 12, &e, NULL) != 1) {
            return WL_ERR_RANGE;
        }
        a->bank[i].string_id = wl_rd_be32(v->p + e);
        a->bank[i].file_id = wl_rd_be32(v->p + e + 4);
        a->bank[i].aux = wl_rd_be32(v->p + e + 8);
    }
    a->n_bank = n;
    r = info_table(v, 16, MAX_ENTRIES, &t, &n); /* players: third */
    if (r != WL_OK) {
        return r;
    }
    a->player = (WlBrsarPlayer *)calloc((size_t)n + 1u, sizeof *a->player);
    if (a->player == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < n; i++) {
        size_t e;
        if (info_ref(v, t + 4u + 8u * i, 12, &e, NULL) != 1) {
            return WL_ERR_RANGE;
        }
        a->player[i].string_id = wl_rd_be32(v->p + e);
        a->player[i].playable = v->p[e + 4];
        a->player[i].heap = wl_rd_be32(v->p + e + 8);
    }
    a->n_player = n;
    return WL_OK;
}

/* The first four bytes of every file that sits inside the archive tell its kind. */
static WlResult classify_files(const Src *f, WlBrsar *a) {
    for (uint32_t i = 0; i < a->n_item; i++) {
        const WlBrsarItem *it = &a->item[i];
        const WlBrsarGroup *g = &a->grp[it->group];
        if (it->file >= a->n_file) {
            a->bad_items++;
            continue;
        }
        WlBrsarFile *fl = &a->file[it->file];
        if (fl->magic != 0u) {
            continue;
        }
        uint64_t loc = (uint64_t)g->offset + it->offset;
        if (it->size < 4u || g->size == 0u || (uint64_t)it->offset + it->size > g->size ||
            loc + 4u > a->archive_size) {
            a->bad_items++;
            continue;
        }
        uint8_t m[4];
        WlResult r = src_read(f, loc, m, 4);
        if (r != WL_OK) {
            return r;
        }
        fl->magic = wl_rd_be32(m);
    }
    for (uint32_t i = 0; i < a->n_file; i++) {
        const WlBrsarFile *fl = &a->file[i];
        if (fl->ext) {
            continue;
        }
        switch (fl->magic) {
        case 0x52575344u: a->files_rwsd++; break; /* RWSD */
        case 0x52534551u: a->files_rseq++; break; /* RSEQ */
        case 0x52424E4Bu: a->files_rbnk++; break; /* RBNK */
        case 0x52574152u: a->files_rwar++; break; /* RWAR */
        case 0u: a->files_unstored++; break;
        default: a->files_other++; break;
        }
    }
    return WL_OK;
}

WlResult wl_brsar_open_cb(WlBrsarReadFn read, void *user, uint64_t size, WlBrsar *a) {
    if (a == NULL) {
        return WL_ERR_ARG;
    }
    memset(a, 0, sizeof *a);
    if (read == NULL) {
        return WL_ERR_ARG;
    }
    Src src = {read, user};
    const Src *f = &src;
    Layout l;
    uint8_t *sy = NULL, *in = NULL;
    WlResult r = read_header(f, size, &l, &a->version);
    if (r == WL_OK) {
        a->archive_size = size;
        r = read_chunk(f, size, l.symb_off, l.symb_size, "SYMB", &sy);
    }
    if (r == WL_OK) {
        r = read_chunk(f, size, l.info_off, l.info_size, "INFO", &in);
    }
    if (r == WL_OK) {
        r = index_strings(sy, l.symb_size, a);
    }
    if (r == WL_OK) {
        Info v = {in + 8, (size_t)l.info_size - 8u};
        if (v.size < 0x30u) {
            r = WL_ERR_RANGE;
        }
        if (r == WL_OK) { /* the references of INFO: sounds, banks, players, files, groups, player info */
            r = index_sounds(&v, 0, a);
        }
        if (r == WL_OK) {
            r = index_banks_players(&v, a);
        }
        if (r == WL_OK) {
            r = index_files(&v, 24, a);
        }
        if (r == WL_OK) {
            r = index_groups(&v, 32, a);
        }
        if (r == WL_OK) {
            r = classify_files(f, a);
        }
    }
    free(sy);
    free(in);
    if (r != WL_OK) {
        wl_brsar_free(a);
    }
    return r;
}

typedef struct {
    const uint8_t *p;
    size_t n;
} Mem;

static WlResult mem_read(void *user, uint64_t off, void *dst, size_t n) {
    const Mem *m = (const Mem *)user;
    if (!wl_range_ok(off, n, m->n)) {
        return WL_ERR_RANGE;
    }
    memcpy(dst, m->p + off, n);
    return WL_OK;
}

WlResult wl_brsar_open_mem(const uint8_t *data, size_t size, WlBrsar *out) {
    if (data == NULL && size != 0u) {
        return WL_ERR_ARG;
    }
    Mem m = {data, size};
    return wl_brsar_open_cb(mem_read, &m, size, out);
}

const char *wl_brsar_string(const WlBrsar *a, uint32_t i) {
    if (a == NULL || a->strings == NULL || i >= a->n_str) {
        return NULL;
    }
    return (const char *)a->strings + (size_t)i * WL_BRSAR_STR_MAX;
}

int wl_brsar_find_ext(const WlBrsar *a, const char *path, uint32_t *file_id, uint32_t *sound_id) {
    for (uint32_t i = 0; i < a->n_file; i++) {
        if (a->file[i].ext && strcmp(a->file[i].path, path) == 0) {
            *file_id = i;
            *sound_id = WL_BRSAR_NONE;
            for (uint32_t s = 0; s < a->n_snd; s++) {
                if (a->snd[s].file_id == i) {
                    *sound_id = s;
                    break;
                }
            }
            return 1;
        }
    }
    return 0;
}

WlResult wl_brsar_file_where(const WlBrsar *a, uint32_t file_id, uint64_t *off, uint32_t *size, uint64_t *woff, uint32_t *wsize) {
    if (file_id >= a->n_file) {
        return WL_ERR_RANGE;
    }
    const WlBrsarFile *fl = &a->file[file_id];
    if (fl->first_group >= a->n_grp || fl->first_index == WL_BRSAR_NONE) {
        return WL_ERR_RANGE;
    }
    const WlBrsarGroup *g = &a->grp[fl->first_group];
    if (fl->first_index >= g->item_count || (uint64_t)g->first_item + fl->first_index >= a->n_item) {
        return WL_ERR_RANGE;
    }
    const WlBrsarItem *it = &a->item[g->first_item + fl->first_index];
    if (it->file != file_id) {
        return WL_ERR_RANGE;
    }
    *off = (uint64_t)g->offset + it->offset;
    *size = it->size;
    *woff = (uint64_t)g->wave_offset + it->wave_offset;
    *wsize = it->wave_size;
    if (*off + *size > a->archive_size || *woff + *wsize > a->archive_size) {
        return WL_ERR_RANGE;
    }
    return WL_OK;
}

WlResult wl_brsar_read_file(const WlBrsar *a, WlBrsarReadFn read, void *user, uint32_t file_id, size_t max,
                            uint8_t **data, size_t *size, uint8_t **wave, size_t *wave_size) {
    if (a == NULL || read == NULL || data == NULL || size == NULL || wave == NULL || wave_size == NULL) {
        return WL_ERR_ARG;
    }
    *data = *wave = NULL;
    *size = *wave_size = 0;
    uint64_t off, woff;
    uint32_t n, wn;
    WlResult r = wl_brsar_file_where(a, file_id, &off, &n, &woff, &wn);
    if (r != WL_OK) {
        return r;
    }
    if (n > max || wn > max) {
        return WL_ERR_LIMIT;
    }
    uint8_t *d = (uint8_t *)malloc(n ? n : 1u);
    uint8_t *w = wn ? (uint8_t *)malloc(wn) : NULL;
    if (d == NULL || (wn != 0u && w == NULL)) {
        free(d);
        free(w);
        return WL_ERR_NOMEM;
    }
    r = n ? read(user, off, d, n) : WL_OK;
    if (r == WL_OK && wn != 0u) {
        r = read(user, woff, w, wn);
    }
    if (r != WL_OK) {
        free(d);
        free(w);
        return r;
    }
    *data = d;
    *size = n;
    *wave = w;
    *wave_size = wn;
    return WL_OK;
}
