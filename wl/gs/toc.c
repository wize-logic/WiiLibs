// SPDX-License-Identifier: MIT
// GSfsys.toc reader. Header (GStocHeader): "GLLA", u32
// version (+4), u32 entry count (+8), u32 index offset (+0x10), u32 name-area offset (+0x14).
// Names are NUL-terminated at absolute offsets. Entries: Colosseum (version 0x104) 8 bytes
// (id, name offset); XD (0x201) and Battle Revolution (0x251) 16 bytes (id, name offset, size,
// flags). Confirmed against the three discs' headers.
#include <stdlib.h>
#include <string.h>

#include "wl/gs/gs.h"
#include "wl/base/endian.h"

void wl_gs_toc_free(WlGsToc *t) {
    if (t != NULL) {
        free(t->entries);
        memset(t, 0, sizeof *t);
    }
}

WlResult wl_gs_toc_parse(WlGsToc *t, const uint8_t *data, size_t len) {
    if (t == NULL || (data == NULL && len != 0)) {
        return WL_ERR_ARG;
    }
    memset(t, 0, sizeof *t);
    if (len < 0x20 || wl_rd_be32(data) != WL_GS_TOC_MAGIC) {
        return WL_ERR_FORMAT;
    }
    uint32_t version = wl_rd_be32(data + 4), count = wl_rd_be32(data + 8);
    uint32_t table = wl_rd_be32(data + 0x10), names = wl_rd_be32(data + 0x14);
    uint32_t esz = version < 0x200u ? 8u : 16u;
    if (names < 0x20 || names > len || !wl_range_ok(table, (uint64_t)count * esz, len)) {
        return WL_ERR_FORMAT;
    }
    t->entries = (WlGsTocEntry *)calloc(count ? count : 1, sizeof *t->entries);
    if (t->entries == NULL) {
        return WL_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = data + table + (size_t)i * esz;
        WlGsTocEntry *e = &t->entries[i];
        e->id = wl_rd_be32(p);
        e->name_off = wl_rd_be32(p + 4);
        if (esz == 16) {
            e->size = wl_rd_be32(p + 8);
            e->flags = wl_rd_be32(p + 12);
        }
        if (e->name_off < names || e->name_off >= len) {
            wl_gs_toc_free(t);
            return WL_ERR_FORMAT;
        }
        size_t n = 0;
        while (e->name_off + n < len && data[e->name_off + n] != 0) {
            if (n + 1 >= WL_GS_TOC_NAME_MAX) {
                wl_gs_toc_free(t);
                return WL_ERR_FORMAT;
            }
            e->name[n] = (char)data[e->name_off + n];
            n++;
        }
        if (e->name_off + n >= len) { /* unterminated */
            wl_gs_toc_free(t);
            return WL_ERR_FORMAT;
        }
        e->name[n] = 0;
    }
    t->version = version;
    t->entry_size = esz;
    t->count = count;
    return WL_OK;
}
