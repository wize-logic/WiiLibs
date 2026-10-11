// SPDX-License-Identifier: MIT
// NARC reader. Layout (little-endian): "NARC", BOM 0xFFFE, version, file size, header size (16),
// section count; then sections "BTAF" (u16 count + start/end pairs relative to the GMIF data),
// "BTNF" (a file-name table in the same format as the ROM FNT) and "GMIF" (the member bytes).
// Reference: community format notes (e.g. Tinke); summary in README.md.
#include <string.h>

#include "wl/base/endian.h"
#include "wl/nds/fnt.h"
#include "wl/nds/nds.h"

int wl_narc_is_narc(const uint8_t *d, size_t n) {
    return d != NULL && n >= 16 && memcmp(d, "NARC", 4) == 0;
}

WlResult wl_narc_open(WlNarc *a, const uint8_t *d, size_t size) {
    if (a == NULL || d == NULL) {
        return WL_ERR_ARG;
    }
    memset(a, 0, sizeof *a);
    if (!wl_narc_is_narc(d, size) || wl_rd_le16(d + 4) != 0xFFFEu) {
        return WL_ERR_FORMAT;
    }
    size_t hdr = wl_rd_le16(d + 12);
    unsigned sections = wl_rd_le16(d + 14);
    if (hdr < 16 || hdr > size || sections < 3 || sections > 16) {
        return WL_ERR_FORMAT;
    }
    a->data = d;
    a->size = size;
    size_t pos = hdr;
    int have_btaf = 0, have_btnf = 0, have_gmif = 0;
    for (unsigned s = 0; s < sections; s++) {
        if (!wl_range_ok(pos, 8, size)) {
            return WL_ERR_FORMAT;
        }
        uint32_t sz = wl_rd_le32(d + pos + 4);
        if (sz < 8 || !wl_range_ok(pos, sz, size)) {
            return WL_ERR_RANGE;
        }
        const uint8_t *tag = d + pos;
        if (memcmp(tag, "BTAF", 4) == 0 && !have_btaf) {
            if (sz < 12) {
                return WL_ERR_FORMAT;
            }
            a->count = wl_rd_le16(d + pos + 8);
            if (!wl_range_ok(12, (uint64_t)a->count * 8u, sz)) {
                return WL_ERR_RANGE;
            }
            a->btaf_off = pos + 12;
            have_btaf = 1;
        } else if (memcmp(tag, "BTNF", 4) == 0 && !have_btnf) {
            a->btnf_off = pos + 8;
            a->btnf_size = sz - 8u;
            have_btnf = 1;
        } else if (memcmp(tag, "GMIF", 4) == 0 && !have_gmif) {
            a->gmif_off = pos + 8;
            a->gmif_size = sz - 8u;
            have_gmif = 1;
        }
        pos += sz;
    }
    if (!have_btaf || !have_btnf || !have_gmif) {
        return WL_ERR_FORMAT;
    }
    /* A name table is present when the root entry points at real entries: a nameless archive
     * has an empty root directory list, i.e. a first byte of zero at its sub-table offset. */
    a->has_names = 0;
    if (a->btnf_size >= 9) {
        const uint8_t *t = d + a->btnf_off;
        size_t sub = wl_rd_le32(t);
        a->has_names = sub < a->btnf_size && t[sub] != 0;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        const uint8_t *e = d + a->btaf_off + (size_t)i * 8u;
        uint32_t st = wl_rd_le32(e), en = wl_rd_le32(e + 4);
        if (en < st || !wl_range_ok(st, (uint64_t)en - st, a->gmif_size)) {
            return WL_ERR_RANGE;
        }
    }
    return WL_OK;
}

WlResult wl_narc_member(const WlNarc *a, uint32_t i, const uint8_t **data, size_t *size) {
    if (a == NULL || data == NULL || size == NULL) {
        return WL_ERR_ARG;
    }
    if (i >= a->count) {
        return WL_ERR_RANGE;
    }
    const uint8_t *e = a->data + a->btaf_off + (size_t)i * 8u;
    uint32_t st = wl_rd_le32(e), en = wl_rd_le32(e + 4);
    if (en < st || !wl_range_ok(st, (uint64_t)en - st, a->gmif_size)) {
        return WL_ERR_RANGE;
    }
    *data = a->data + a->gmif_off + st;
    *size = (size_t)(en - st);
    return WL_OK;
}

typedef struct {
    const WlNarc *a;
    WlNarcNameFn fn;
    void *ud;
} NameCtx;

static WlResult name_cb(void *ud, const char *path, uint32_t id) {
    NameCtx *c = (NameCtx *)ud;
    const uint8_t *d;
    size_t sz;
    WlResult r = wl_narc_member(c->a, id, &d, &sz);
    if (r != WL_OK) {
        return r;
    }
    return c->fn(c->ud, path, id, d, sz);
}

WlResult wl_narc_walk(const WlNarc *a, WlNarcNameFn fn, void *ud) {
    if (a == NULL || fn == NULL) {
        return WL_ERR_ARG;
    }
    if (!a->has_names) {
        return WL_OK;
    }
    NameCtx c = {a, fn, ud};
    return wl_fnt_walk(a->data + a->btnf_off, a->btnf_size, name_cb, &c);
}
