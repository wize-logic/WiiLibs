// SPDX-License-Identifier: MIT
// Host adaptor: a WlGsSrc over a range of a wl_host WlFile. Header-only, so wl_gs itself does not
// depend on wl_host; include it from host code that links wl_host.
#pragma once

#include "wl/gs/gs.h"
#include "wl/host/file.h"

static inline WlResult wl_gs_file_read_(void *ctx, uint64_t off, void *buf, size_t n) {
    return wl_file_read_at((WlFile *)ctx, off, buf, n);
}

/* The range [base, base + size) of f; f must stay open while the source is used. */
static inline void wl_gs_src_file(WlGsSrc *s, WlFile *f, uint64_t base, uint64_t size) {
    wl_gs_src_cb(s, wl_gs_file_read_, f, base, size);
}
