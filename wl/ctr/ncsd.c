// SPDX-License-Identifier: MIT
// NCSD (the CCI cartridge image container). Layout per 3dbrew.org "NCSD": magic "NCSD" at 0x100,
// partition table of eight (offset, size) pairs at 0x120, both in 0x200-byte media units.
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"

#define NCSD_MEDIA_UNIT 0x200u

WlResult wl_ncsd_open(WlNcsd *n, WlCtrSrc *src) {
    if (n == NULL || src == NULL) {
        return WL_ERR_ARG;
    }
    memset(n, 0, sizeof *n);
    uint8_t h[0x160];
    WlResult r = wl_ctr_src_read(src, 0, h, sizeof h);
    if (r != WL_OK) {
        return r == WL_ERR_RANGE ? WL_ERR_FORMAT : r;
    }
    if (memcmp(h + 0x100, "NCSD", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    n->src = src;
    for (unsigned i = 0; i < 8; i++) {
        uint64_t off = (uint64_t)wl_rd_le32(h + 0x120 + i * 8) * NCSD_MEDIA_UNIT;
        uint64_t size = (uint64_t)wl_rd_le32(h + 0x124 + i * 8) * NCSD_MEDIA_UNIT;
        if (size == 0) {
            continue;
        }
        if (!wl_range_ok(off, size, src->size)) {
            return WL_ERR_RANGE;
        }
        n->part_off[i] = off;
        n->part_size[i] = size;
    }
    if (n->part_size[0] == 0) {
        return WL_ERR_FORMAT; /* the game executable partition is required */
    }
    return WL_OK;
}
