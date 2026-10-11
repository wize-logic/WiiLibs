// SPDX-License-Identifier: MIT
// ExeFS: a 0x200-byte header of ten (name[8], offset, size) records, file data follows the header.
// Layout per 3dbrew.org "ExeFS". The ".code" file may be backward-LZ compressed; not handled here.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"

#define EXEFS_HDR 0x200u

WlResult wl_exefs_open(WlExefs *e, const WlNcch *c) {
    if (e == NULL || c == NULL) {
        return WL_ERR_ARG;
    }
    memset(e, 0, sizeof *e);
    if (c->exefs_size < EXEFS_HDR) {
        return WL_ERR_FORMAT;
    }
    uint8_t h[EXEFS_HDR];
    WlResult r = wl_ctr_src_read(c->src, c->exefs_off, h, sizeof h);
    if (r != WL_OK) {
        return r;
    }
    e->ncch = c;
    for (unsigned i = 0; i < WL_EXEFS_MAX_FILES; i++) {
        const uint8_t *rec = h + i * 16;
        if (rec[0] == 0) {
            continue;
        }
        uint32_t off = wl_rd_le32(rec + 8), size = wl_rd_le32(rec + 12);
        if (!wl_range_ok((uint64_t)EXEFS_HDR + off, size, c->exefs_size)) {
            return WL_ERR_RANGE;
        }
        WlExefsFile *f = &e->files[e->count++];
        memcpy(f->name, rec, 8);
        f->name[8] = '\0';
        f->off = c->exefs_off + EXEFS_HDR + off;
        f->size = size;
    }
    return WL_OK;
}

WlResult wl_exefs_read(const WlExefs *e, unsigned index, uint8_t **data, size_t *len) {
    if (e == NULL || data == NULL || len == NULL || index >= e->count) {
        return WL_ERR_ARG;
    }
    const WlExefsFile *f = &e->files[index];
    uint8_t *b = (uint8_t *)malloc((size_t)f->size + 1u);
    if (b == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_ctr_src_read(e->ncch->src, f->off, b, f->size);
    if (r != WL_OK) {
        free(b);
        return r;
    }
    b[f->size] = 0;
    *data = b;
    *len = f->size;
    return WL_OK;
}
