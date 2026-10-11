// SPDX-License-Identifier: MIT
// wl_brsar_open over a file (host builds: reads through wl_host's random-access file API, so only
// the header, SYMB and INFO chunks and four bytes per group item are read).
#include "wl/brstm/brsar.h"

#include <string.h>

#include "wl/host/file.h"

static WlResult file_read(void *user, uint64_t off, void *dst, size_t n) {
    return wl_file_read_at((WlFile *)user, off, dst, n);
}

WlResult wl_brsar_open(const char *path, WlBrsar *out) {
    if (path == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    WlFile *f = NULL;
    uint64_t size = 0;
    WlResult r = wl_file_open_ro(path, &f, &size);
    if (r != WL_OK) {
        return r;
    }
    r = wl_brsar_open_cb(file_read, f, size, out);
    wl_file_close(f);
    return r;
}
