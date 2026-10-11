// SPDX-License-Identifier: MIT
// Host-only byte source for the 3DS readers: a 64-bit-offset read-only file through wl_host.
#include "wl/ctr/ctr.h"
#include "wl/host/file.h"

static WlResult file_read(void *ctx, uint64_t off, void *buf, size_t n) {
    return wl_file_read_at((WlFile *)ctx, off, buf, n);
}

static void file_close(void *ctx) {
    wl_file_close((WlFile *)ctx);
}

WlResult wl_ctr_src_open(WlCtrSrc *s, const char *path) {
    if (s == NULL || path == NULL) {
        return WL_ERR_ARG;
    }
    WlFile *f;
    uint64_t size;
    WlResult r = wl_file_open_ro(path, &f, &size);
    if (r != WL_OK) {
        return r;
    }
    s->read = file_read;
    s->close = file_close;
    s->ctx = f;
    s->size = size;
    return WL_OK;
}
