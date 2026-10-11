// SPDX-License-Identifier: MIT
// FSYS archive reader. Header (GSfsysHeaderEx): "FSYS", u32 entry count at +0x0C, u32 offset of the entry
// pointer table at +0x40. The table holds one u32 per entry, pointing at a record: id +0x00,
// data offset +0x04, unpacked size +0x08, flags +0x0C (bit 31 = compressed), packed size
// +0x14, file type +0x20, name offset +0x24 (NUL-terminated string, 0 = none).
#include <stdlib.h>
#include <string.h>

#include "wl/gs/gs.h"
#include "wl/base/endian.h"

void wl_gs_src_mem(WlGsSrc *s, const void *data, size_t len) {
    memset(s, 0, sizeof *s);
    s->mem = (const uint8_t *)data;
    s->size = len;
}

void wl_gs_src_cb(WlGsSrc *s, WlGsReadFn read, void *ctx, uint64_t base, uint64_t size) {
    memset(s, 0, sizeof *s);
    s->read = read;
    s->ctx = ctx;
    s->base = base;
    s->size = size;
}

WlResult wl_gs_src_read(const WlGsSrc *s, uint64_t off, void *buf, size_t n) {
    if (!wl_range_ok(off, n, s->size)) {
        return WL_ERR_RANGE;
    }
    if (n == 0) {
        return WL_OK;
    }
    if (s->mem != NULL) {
        memcpy(buf, s->mem + off, n);
        return WL_OK;
    }
    if (s->read == NULL) {
        return WL_ERR_ARG;
    }
    return s->read(s->ctx, s->base + off, buf, n);
}

void wl_gs_fsys_close(WlGsFsys *f) {
    if (f != NULL) {
        free(f->entries);
        memset(f, 0, sizeof *f);
    }
}

static WlResult read_name(const WlGsSrc *s, uint32_t off, char out[WL_GS_FSYS_NAME_MAX]) {
    out[0] = 0;
    if (off == 0) {
        return WL_OK;
    }
    if (off >= s->size) {
        return WL_ERR_FORMAT;
    }
    size_t n = WL_GS_FSYS_NAME_MAX;
    if (s->size - off < n) {
        n = (size_t)(s->size - off);
    }
    uint8_t tmp[WL_GS_FSYS_NAME_MAX];
    WlResult r = wl_gs_src_read(s, off, tmp, n);
    if (r != WL_OK) {
        return r;
    }
    size_t k = 0;
    while (k < n && tmp[k] != 0) {
        k++;
    }
    if (k == n) {
        return WL_ERR_FORMAT; /* unterminated or too long */
    }
    memcpy(out, tmp, k);
    out[k] = 0;
    return WL_OK;
}

WlResult wl_gs_fsys_open(WlGsFsys *f, const WlGsSrc *src) {
    if (f == NULL || src == NULL) {
        return WL_ERR_ARG;
    }
    memset(f, 0, sizeof *f);
    f->src = *src;
    uint8_t h[WL_GS_FSYS_HDR];
    WlResult r = wl_gs_src_read(src, 0, h, sizeof h);
    if (r != WL_OK) {
        return r == WL_ERR_RANGE ? WL_ERR_FORMAT : r;
    }
    if (wl_rd_be32(h) != WL_GS_FSYS_MAGIC) {
        return WL_ERR_FORMAT;
    }
    uint32_t count = wl_rd_be32(h + 0x0C), table = wl_rd_be32(h + 0x40);
    if (!wl_range_ok(table, (uint64_t)count * 4, src->size)) {
        return WL_ERR_FORMAT;
    }
    uint8_t *offs = (uint8_t *)malloc((size_t)count * 4 + 1);
    f->entries = (WlGsEntry *)calloc(count ? count : 1, sizeof *f->entries);
    if (offs == NULL || f->entries == NULL) {
        free(offs);
        wl_gs_fsys_close(f);
        return WL_ERR_NOMEM;
    }
    r = wl_gs_src_read(src, table, offs, (size_t)count * 4);
    for (uint32_t i = 0; r == WL_OK && i < count; i++) {
        uint32_t ro = wl_rd_be32(offs + (size_t)i * 4);
        uint8_t rec[WL_GS_FSYS_REC];
        if (wl_gs_src_read(src, ro, rec, sizeof rec) != WL_OK) {
            r = WL_ERR_FORMAT;
            break;
        }
        WlGsEntry *e = &f->entries[i];
        e->index = i;
        e->id = wl_rd_be32(rec);
        e->offset = wl_rd_be32(rec + 0x04);
        e->usize = wl_rd_be32(rec + 0x08);
        e->flags = wl_rd_be32(rec + 0x0C);
        e->psize = wl_rd_be32(rec + 0x14);
        e->type = wl_rd_be32(rec + 0x20);
        e->name_off = wl_rd_be32(rec + 0x24);
        if (!wl_range_ok(e->offset, e->psize, src->size)) {
            r = WL_ERR_FORMAT;
            break;
        }
        r = read_name(src, e->name_off, e->name);
        if (r == WL_OK && e->psize >= 4) {
            uint8_t m[4];
            r = wl_gs_src_read(src, e->offset, m, 4);
            e->is_lzss = r == WL_OK && wl_rd_be32(m) == WL_GS_LZSS_MAGIC;
        }
    }
    free(offs);
    if (r != WL_OK) {
        wl_gs_fsys_close(f);
        return r == WL_ERR_NOMEM ? r : WL_ERR_FORMAT;
    }
    f->count = count;
    f->file_size_field = wl_rd_be32(h + 0x1C);
    return WL_OK;
}

WlResult wl_gs_fsys_read(const WlGsFsys *f, uint32_t i, uint8_t **data, size_t *len) {
    if (f == NULL || data == NULL || len == NULL || i >= f->count) {
        return WL_ERR_ARG;
    }
    const WlGsEntry *e = &f->entries[i];
    if (e->psize > WL_GS_MAX_ENTRY || e->usize > WL_GS_MAX_ENTRY) {
        return WL_ERR_RANGE;
    }
    uint8_t *raw = (uint8_t *)malloc((size_t)e->psize + 1);
    if (raw == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_gs_src_read(&f->src, e->offset, raw, e->psize);
    if (r != WL_OK) {
        free(raw);
        return r;
    }
    raw[e->psize] = 0;
    if (!e->is_lzss) {
        *data = raw;
        *len = e->psize;
        return WL_OK;
    }
    uint8_t *out = NULL;
    size_t olen = 0;
    r = wl_gs_lzss_unpack(raw, e->psize, WL_GS_MAX_ENTRY, &out, &olen);
    free(raw);
    if (r != WL_OK) {
        return r;
    }
    if (olen != e->usize) {
        free(out);
        return WL_ERR_FORMAT;
    }
    *data = out;
    *len = olen;
    return WL_OK;
}

WlResult wl_gs_fsys_verify(const WlGsFsys *f, uint32_t i) {
    if (f == NULL || i >= f->count) {
        return WL_ERR_ARG;
    }
    if (!f->entries[i].is_lzss) {
        return WL_OK; /* bounds were checked at open */
    }
    uint8_t *d = NULL;
    size_t n = 0;
    WlResult r = wl_gs_fsys_read(f, i, &d, &n);
    free(d);
    return r;
}
