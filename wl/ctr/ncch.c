// SPDX-License-Identifier: MIT
// NCCH partition header. Layout per 3dbrew.org "NCCH": magic "NCCH" at 0x100, content flags at
// 0x188 (flags[7] bit 0x4 = NoCrypto), ExeFS and RomFS offset/size pairs (media units) at 0x1A0
// and 0x1B0. Encrypted partitions are refused: this project holds no keys.
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"

#define NCCH_MEDIA_UNIT 0x200u

WlResult wl_ncch_open(WlNcch *c, WlCtrSrc *src, uint64_t off, uint64_t size) {
    if (c == NULL || src == NULL) {
        return WL_ERR_ARG;
    }
    memset(c, 0, sizeof *c);
    if (!wl_range_ok(off, size, src->size) || size < 0x200) {
        return WL_ERR_RANGE;
    }
    uint8_t h[0x200];
    WlResult r = wl_ctr_src_read(src, off, h, sizeof h);
    if (r != WL_OK) {
        return r;
    }
    if (memcmp(h + 0x100, "NCCH", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    if ((h[0x188 + 7] & 0x4u) == 0) {
        return WL_ERR_ENCRYPTED;
    }
    uint64_t exefs_off = (uint64_t)wl_rd_le32(h + 0x1A0) * NCCH_MEDIA_UNIT;
    uint64_t exefs_size = (uint64_t)wl_rd_le32(h + 0x1A4) * NCCH_MEDIA_UNIT;
    uint64_t romfs_off = (uint64_t)wl_rd_le32(h + 0x1B0) * NCCH_MEDIA_UNIT;
    uint64_t romfs_size = (uint64_t)wl_rd_le32(h + 0x1B4) * NCCH_MEDIA_UNIT;
    if ((exefs_size != 0 && !wl_range_ok(exefs_off, exefs_size, size)) ||
        (romfs_size != 0 && !wl_range_ok(romfs_off, romfs_size, size))) {
        return WL_ERR_RANGE;
    }
    c->src = src;
    c->base = off;
    c->size = size;
    if (exefs_size != 0) {
        c->exefs_off = off + exefs_off;
        c->exefs_size = exefs_size;
    }
    if (romfs_size != 0) {
        c->romfs_off = off + romfs_off;
        c->romfs_size = romfs_size;
    }
    return WL_OK;
}
