// SPDX-License-Identifier: MIT
// Host-only entry point of the GameCube reader: opens an image file through wl_host.
#include "wl/gcm/gcm.h"
#include "wl/host/file.h"

static WlResult file_read(void *ctx, uint64_t off, void *buf, size_t n) {
    return wl_file_read_at((WlFile *)ctx, off, buf, n);
}

static void file_close(void *ctx) {
    wl_file_close((WlFile *)ctx);
}

WlResult wl_gcm_open(WlGcm *g, const char *path) {
    if (g == NULL || path == NULL) {
        return WL_ERR_ARG;
    }
    WlFile *f;
    uint64_t size;
    WlResult r = wl_file_open_ro(path, &f, &size);
    if (r != WL_OK) {
        *g = (WlGcm){0};
        return r;
    }
    return wl_gcm_open_src(g, file_read, file_close, f, size);
}
