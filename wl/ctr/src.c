// SPDX-License-Identifier: MIT
// Byte sources for the 3DS readers: a borrowed memory buffer. The file source is in src_file.c.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"

typedef struct {
    const uint8_t *data;
    size_t len;
} MemCtx;

static WlResult mem_read(void *ctx, uint64_t off, void *buf, size_t n) {
    const MemCtx *m = (const MemCtx *)ctx;
    if (!wl_range_ok(off, n, m->len)) {
        return WL_ERR_RANGE;
    }
    if (n != 0) {
        memcpy(buf, m->data + off, n);
    }
    return WL_OK;
}

static void mem_close(void *ctx) {
    free(ctx);
}

WlResult wl_ctr_src_mem(WlCtrSrc *s, const uint8_t *data, size_t len) {
    if (s == NULL || (data == NULL && len != 0)) {
        return WL_ERR_ARG;
    }
    MemCtx *m = (MemCtx *)malloc(sizeof *m);
    if (m == NULL) {
        return WL_ERR_NOMEM;
    }
    m->data = data;
    m->len = len;
    s->read = mem_read;
    s->close = mem_close;
    s->ctx = m;
    s->size = len;
    return WL_OK;
}

void wl_ctr_src_close(WlCtrSrc *s) {
    if (s != NULL && s->close != NULL) {
        s->close(s->ctx);
        s->close = NULL;
    }
}

WlResult wl_ctr_src_read(const WlCtrSrc *s, uint64_t off, void *buf, size_t n) {
    if (!wl_range_ok(off, n, s->size)) {
        return WL_ERR_RANGE;
    }
    return s->read(s->ctx, off, buf, n);
}
