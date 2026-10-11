// SPDX-License-Identifier: MIT
// RomFS inside an NCCH: an IVFC wrapper (magic "IVFC", master hash, then the level-3 data) around
// the real file system. Level 3 starts with a header of directory/file hash tables and metadata
// tables, then the file data. Layout per 3dbrew.org "RomFS" and "IVFC". Hashes are not verified
// (integrity is not our job); the hash tables are used only for path lookup.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"

#define RFS_NONE 0xFFFFFFFFu
#define RFS_DIR_HDR 0x18u
#define RFS_FILE_HDR 0x20u
#define RFS_MAX_TABLE (64u * 1024u * 1024u)
#define RFS_MAX_DEPTH 64u
#define RFS_PATH_MAX 2048u
#define RFS_NAME_MAX 1024u /* bytes of UTF-16 */

static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)wl_rd_le32(p) | ((uint64_t)wl_rd_le32(p + 4) << 32);
}

void wl_romfs_close(WlRomfs *r) {
    if (r != NULL) {
        free(r->dir_hash);
        free(r->dir_meta);
        free(r->file_hash);
        free(r->file_meta);
        memset(r, 0, sizeof *r);
    }
}

static WlResult load_table(const WlRomfs *r, uint32_t off, uint32_t len, uint8_t **out) {
    if (len > RFS_MAX_TABLE || (len & 3u) != 0 || !wl_range_ok(off, len, r->l3_size)) {
        return WL_ERR_RANGE;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)len + 4u);
    if (b == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult res = wl_ctr_src_read(r->ncch->src, r->l3_off + off, b, len);
    if (res != WL_OK) {
        free(b);
        return res;
    }
    *out = b;
    return WL_OK;
}

WlResult wl_romfs_open(WlRomfs *r, const WlNcch *c) {
    if (r == NULL || c == NULL) {
        return WL_ERR_ARG;
    }
    memset(r, 0, sizeof *r);
    if (c->romfs_size < 0x1000) {
        return WL_ERR_FORMAT;
    }
    uint8_t h[0x60];
    WlResult res = wl_ctr_src_read(c->src, c->romfs_off, h, sizeof h);
    if (res != WL_OK) {
        return res;
    }
    if (memcmp(h, "IVFC", 4) != 0 || wl_rd_le32(h + 4) != 0x10000u) {
        return WL_ERR_FORMAT;
    }
    uint32_t master = wl_rd_le32(h + 8);
    uint64_t l3_size = rd_le64(h + 0x44);
    uint32_t bs_log2 = wl_rd_le32(h + 0x4C);
    if (bs_log2 < 4 || bs_log2 > 24) {
        return WL_ERR_FORMAT;
    }
    /* Level 3 follows the master hash, aligned up to its block size. */
    uint64_t align = (uint64_t)1 << bs_log2;
    uint64_t l3_rel = ((uint64_t)0x60 + master + align - 1u) & ~(align - 1u);
    if (!wl_range_ok(l3_rel, l3_size, c->romfs_size) || l3_size < 0x28) {
        return WL_ERR_RANGE;
    }
    r->ncch = c;
    r->l3_off = c->romfs_off + l3_rel;
    r->l3_size = l3_size;
    uint8_t l[0x28];
    res = wl_ctr_src_read(c->src, r->l3_off, l, sizeof l);
    if (res != WL_OK) {
        return res;
    }
    if (wl_rd_le32(l) != 0x28u) {
        return WL_ERR_FORMAT;
    }
    r->data_off = wl_rd_le32(l + 0x24);
    if (r->data_off > l3_size) {
        wl_romfs_close(r);
        return WL_ERR_RANGE;
    }
    uint32_t dm = wl_rd_le32(l + 0x10), fm = wl_rd_le32(l + 0x20);
    if ((res = load_table(r, wl_rd_le32(l + 0x04), wl_rd_le32(l + 0x08), &r->dir_hash)) != WL_OK ||
        (res = load_table(r, wl_rd_le32(l + 0x0C), dm, &r->dir_meta)) != WL_OK ||
        (res = load_table(r, wl_rd_le32(l + 0x14), wl_rd_le32(l + 0x18), &r->file_hash)) != WL_OK ||
        (res = load_table(r, wl_rd_le32(l + 0x1C), fm, &r->file_meta)) != WL_OK) {
        wl_romfs_close(r);
        return res;
    }
    r->dir_hash_n = wl_rd_le32(l + 0x08) / 4u;
    r->file_hash_n = wl_rd_le32(l + 0x18) / 4u;
    r->dir_meta_len = dm;
    r->file_meta_len = fm;
    if (dm < RFS_DIR_HDR) {
        wl_romfs_close(r);
        return WL_ERR_FORMAT;
    }
    return WL_OK;
}

/* ---- UTF-16 / UTF-8 ---- */
static size_t utf16_to_utf8(const uint8_t *src, size_t units, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = wl_rd_le16(src + i * 2);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            uint32_t d = wl_rd_le16(src + (i + 1) * 2);
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (d - 0xDC00u);
                i++;
            }
        }
        if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        uint8_t t[4];
        size_t n;
        if (c < 0x80) {
            t[0] = (uint8_t)c;
            n = 1;
        } else if (c < 0x800) {
            t[0] = (uint8_t)(0xC0u | (c >> 6));
            t[1] = (uint8_t)(0x80u | (c & 0x3Fu));
            n = 2;
        } else if (c < 0x10000) {
            t[0] = (uint8_t)(0xE0u | (c >> 12));
            t[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
            t[2] = (uint8_t)(0x80u | (c & 0x3Fu));
            n = 3;
        } else {
            t[0] = (uint8_t)(0xF0u | (c >> 18));
            t[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
            t[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
            t[3] = (uint8_t)(0x80u | (c & 0x3Fu));
            n = 4;
        }
        if (o + n >= cap) {
            return (size_t)-1;
        }
        memcpy(dst + o, t, n);
        o += n;
    }
    dst[o] = '\0';
    return o;
}

/* Decodes UTF-8 to UTF-16 code units; returns the unit count or -1. */
static size_t utf8_to_utf16(const char *s, size_t n, uint16_t *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint8_t b = (uint8_t)s[i];
        uint32_t c;
        size_t len;
        if (b < 0x80) {
            c = b;
            len = 1;
        } else if ((b & 0xE0u) == 0xC0u) {
            c = b & 0x1Fu;
            len = 2;
        } else if ((b & 0xF0u) == 0xE0u) {
            c = b & 0x0Fu;
            len = 3;
        } else if ((b & 0xF8u) == 0xF0u) {
            c = b & 0x07u;
            len = 4;
        } else {
            return (size_t)-1;
        }
        if (i + len > n) {
            return (size_t)-1;
        }
        for (size_t k = 1; k < len; k++) {
            if (((uint8_t)s[i + k] & 0xC0u) != 0x80u) {
                return (size_t)-1;
            }
            c = (c << 6) | ((uint8_t)s[i + k] & 0x3Fu);
        }
        i += len;
        if (c >= 0x10000u) {
            if (o + 2 > cap) {
                return (size_t)-1;
            }
            c -= 0x10000u;
            dst[o++] = (uint16_t)(0xD800u + (c >> 10));
            dst[o++] = (uint16_t)(0xDC00u + (c & 0x3FFu));
        } else {
            if (o + 1 > cap) {
                return (size_t)-1;
            }
            dst[o++] = (uint16_t)c;
        }
    }
    return o;
}

/* ---- metadata access ---- */
typedef struct {
    uint32_t parent, sibling, child_dir, child_file, next_hash;
    uint64_t data_off, data_size;
    uint32_t name_len; /* bytes */
    const uint8_t *name;
} Meta;

static WlResult dir_at(const WlRomfs *r, uint32_t off, Meta *m) {
    if (off % 4u != 0 || !wl_range_ok(off, RFS_DIR_HDR, r->dir_meta_len)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *p = r->dir_meta + off;
    m->parent = wl_rd_le32(p);
    m->sibling = wl_rd_le32(p + 4);
    m->child_dir = wl_rd_le32(p + 8);
    m->child_file = wl_rd_le32(p + 12);
    m->next_hash = wl_rd_le32(p + 16);
    m->name_len = wl_rd_le32(p + 20);
    if (m->name_len > RFS_NAME_MAX || (m->name_len & 1u) != 0 ||
        !wl_range_ok((uint64_t)off + RFS_DIR_HDR, m->name_len, r->dir_meta_len)) {
        return WL_ERR_RANGE;
    }
    m->name = p + RFS_DIR_HDR;
    return WL_OK;
}

static WlResult file_at(const WlRomfs *r, uint32_t off, Meta *m) {
    if (off % 4u != 0 || !wl_range_ok(off, RFS_FILE_HDR, r->file_meta_len)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *p = r->file_meta + off;
    m->parent = wl_rd_le32(p);
    m->sibling = wl_rd_le32(p + 4);
    m->data_off = rd_le64(p + 8);
    m->data_size = rd_le64(p + 16);
    m->next_hash = wl_rd_le32(p + 24);
    m->name_len = wl_rd_le32(p + 28);
    if (m->name_len > RFS_NAME_MAX || (m->name_len & 1u) != 0 ||
        !wl_range_ok((uint64_t)off + RFS_FILE_HDR, m->name_len, r->file_meta_len)) {
        return WL_ERR_RANGE;
    }
    m->name = p + RFS_FILE_HDR;
    m->child_dir = m->child_file = RFS_NONE;
    return WL_OK;
}

static WlResult file_entry(const WlRomfs *r, const Meta *m, WlRomfsEntry *e) {
    if (m->data_off > r->l3_size || m->data_size > r->l3_size ||
        !wl_range_ok(r->data_off + m->data_off, m->data_size, r->l3_size)) {
        return WL_ERR_RANGE;
    }
    e->is_dir = 0;
    e->size = m->data_size;
    e->offset = r->l3_off + r->data_off + m->data_off;
    return WL_OK;
}

typedef struct {
    const WlRomfs *r;
    WlRomfsVisit cb;
    void *ud;
    uint32_t dirs_left, files_left;
    char path[RFS_PATH_MAX];
} Walk;

static WlResult walk_dir(Walk *w, uint32_t doff, size_t plen, unsigned depth) {
    if (depth > RFS_MAX_DEPTH) {
        return WL_ERR_FORMAT;
    }
    Meta d;
    WlResult res = dir_at(w->r, doff, &d);
    if (res != WL_OK) {
        return res;
    }
    for (uint32_t f = d.child_file; f != RFS_NONE;) {
        Meta fm;
        if (w->files_left-- == 0) {
            return WL_ERR_FORMAT; /* a sibling cycle */
        }
        if ((res = file_at(w->r, f, &fm)) != WL_OK) {
            return res;
        }
        size_t o = plen;
        size_t nl = utf16_to_utf8(fm.name, fm.name_len / 2u, w->path + o, RFS_PATH_MAX - o);
        if (nl == (size_t)-1) {
            return WL_ERR_RANGE;
        }
        WlRomfsEntry e;
        if ((res = file_entry(w->r, &fm, &e)) != WL_OK || (res = w->cb(w->ud, w->path, &e)) != WL_OK) {
            return res;
        }
        f = fm.sibling;
    }
    for (uint32_t c = d.child_dir; c != RFS_NONE;) {
        Meta cm;
        if (w->dirs_left-- == 0) {
            return WL_ERR_FORMAT;
        }
        if ((res = dir_at(w->r, c, &cm)) != WL_OK) {
            return res;
        }
        size_t nl = utf16_to_utf8(cm.name, cm.name_len / 2u, w->path + plen, RFS_PATH_MAX - plen);
        if (nl == (size_t)-1 || plen + nl + 2 > RFS_PATH_MAX) {
            return WL_ERR_RANGE;
        }
        WlRomfsEntry e = {1, 0, 0};
        if ((res = w->cb(w->ud, w->path, &e)) != WL_OK) {
            return res;
        }
        w->path[plen + nl] = '/';
        w->path[plen + nl + 1] = '\0';
        if ((res = walk_dir(w, c, plen + nl + 1, depth + 1)) != WL_OK) {
            return res;
        }
        c = cm.sibling;
    }
    return WL_OK;
}

WlResult wl_romfs_walk(const WlRomfs *r, WlRomfsVisit cb, void *ud) {
    if (r == NULL || r->dir_meta == NULL || cb == NULL) {
        return WL_ERR_ARG;
    }
    Walk w;
    w.r = r;
    w.cb = cb;
    w.ud = ud;
    w.dirs_left = r->dir_meta_len / RFS_DIR_HDR;
    w.files_left = r->file_meta_len / RFS_FILE_HDR;
    w.path[0] = '\0';
    return walk_dir(&w, 0, 0, 0);
}

/* ---- hash lookup (3dbrew "RomFS": rotate-xor over the parent offset and UTF-16 units) ---- */
static uint32_t name_hash(uint32_t parent, const uint16_t *u, size_t n) {
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < n; i++) {
        h = ((h >> 5) | (h << 27)) ^ (uint32_t)u[i];
    }
    return h;
}

static int name_eq(const uint8_t *name, uint32_t name_len, const uint16_t *u, size_t n) {
    if (name_len != n * 2u) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (wl_rd_le16(name + i * 2) != u[i]) {
            return 0;
        }
    }
    return 1;
}

WlResult wl_romfs_find_file(const WlRomfs *r, const char *path, WlRomfsEntry *e) {
    if (r == NULL || path == NULL || e == NULL || r->dir_meta == NULL) {
        return WL_ERR_ARG;
    }
    uint32_t parent = 0;
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    for (;;) {
        const char *end = strchr(p, '/');
        size_t clen = end ? (size_t)(end - p) : strlen(p);
        uint16_t u[RFS_NAME_MAX / 2];
        size_t un = utf8_to_utf16(p, clen, u, RFS_NAME_MAX / 2);
        if (clen == 0 || un == (size_t)-1) {
            return WL_ERR_ARG;
        }
        uint32_t h = name_hash(parent, u, un);
        int last = (end == NULL);
        const uint8_t *tab = last ? r->file_hash : r->dir_hash;
        uint32_t nb = last ? r->file_hash_n : r->dir_hash_n;
        if (nb == 0) {
            return WL_ERR_FAILED;
        }
        uint32_t cur = wl_rd_le32(tab + (size_t)(h % nb) * 4u);
        uint32_t steps = (last ? r->file_meta_len / RFS_FILE_HDR : r->dir_meta_len / RFS_DIR_HDR) + 1u;
        WlResult res = WL_ERR_FAILED;
        while (cur != RFS_NONE && steps-- > 0) {
            Meta m;
            res = last ? file_at(r, cur, &m) : dir_at(r, cur, &m);
            if (res != WL_OK) {
                return res;
            }
            if (m.parent == parent && name_eq(m.name, m.name_len, u, un)) {
                if (last) {
                    return file_entry(r, &m, e);
                }
                parent = cur;
                res = WL_OK;
                break;
            }
            cur = m.next_hash;
            res = WL_ERR_FAILED;
        }
        if (res != WL_OK) {
            return WL_ERR_FAILED;
        }
        p = end + 1;
    }
}

WlResult wl_romfs_read(const WlRomfs *r, const WlRomfsEntry *e, uint64_t off, void *buf, size_t n) {
    if (r == NULL || e == NULL || e->is_dir || !wl_range_ok(off, n, e->size)) {
        return WL_ERR_RANGE;
    }
    return wl_ctr_src_read(r->ncch->src, e->offset + off, buf, n);
}

WlResult wl_romfs_read_all(const WlRomfs *r, const WlRomfsEntry *e, size_t max, uint8_t **data,
                           size_t *len) {
    if (r == NULL || e == NULL || data == NULL || len == NULL || e->is_dir) {
        return WL_ERR_ARG;
    }
    if (e->size > max) {
        return WL_ERR_RANGE;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)e->size + 1u);
    if (b == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult res = wl_romfs_read(r, e, 0, b, (size_t)e->size);
    if (res != WL_OK) {
        free(b);
        return res;
    }
    b[e->size] = 0;
    *data = b;
    *len = (size_t)e->size;
    return WL_OK;
}
