// SPDX-License-Identifier: MIT
// NDS header, FAT, FNT and overlay tables. Layout per GBATEK "DS Cartridge Header" (offsets
// 0x00 title/code, 0x20..0x3C ARM9/ARM7, 0x40..0x5C FNT/FAT/overlay tables) and the 32-byte
// overlay entry (id, ram, size, bss, static-init start/end, file id, compressed word).
#include <string.h>

#include "wl/base/endian.h"
#include "wl/nds/fnt.h"
#include "wl/nds/nds.h"

#define NDS_HEADER_MIN 0x200u
#define NDS_OVL_ENTRY 32u

WlResult wl_nds_open(WlNds *n, const uint8_t *d, size_t size) {
    if (n == NULL || d == NULL) {
        return WL_ERR_ARG;
    }
    memset(n, 0, sizeof *n);
    if (size < NDS_HEADER_MIN) {
        return WL_ERR_FORMAT;
    }
    n->data = d;
    n->size = size;
    memcpy(n->game_code, d + 0x0C, 4);
    n->game_code[4] = '\0';
    n->arm9_off = wl_rd_le32(d + 0x20);
    n->arm9_entry = wl_rd_le32(d + 0x24);
    n->arm9_ram = wl_rd_le32(d + 0x28);
    n->arm9_size = wl_rd_le32(d + 0x2C);
    n->arm7_off = wl_rd_le32(d + 0x30);
    n->arm7_entry = wl_rd_le32(d + 0x34);
    n->arm7_ram = wl_rd_le32(d + 0x38);
    n->arm7_size = wl_rd_le32(d + 0x3C);
    n->fnt_off = wl_rd_le32(d + 0x40);
    n->fnt_size = wl_rd_le32(d + 0x44);
    n->fat_off = wl_rd_le32(d + 0x48);
    n->fat_size = wl_rd_le32(d + 0x4C);
    n->ovt9_off = wl_rd_le32(d + 0x50);
    n->ovt9_size = wl_rd_le32(d + 0x54);
    n->ovt7_off = wl_rd_le32(d + 0x58);
    n->ovt7_size = wl_rd_le32(d + 0x5C);
    if (!wl_range_ok(n->arm9_off, n->arm9_size, size) || !wl_range_ok(n->arm7_off, n->arm7_size, size) ||
        !wl_range_ok(n->fnt_off, n->fnt_size, size) || !wl_range_ok(n->fat_off, n->fat_size, size) ||
        !wl_range_ok(n->ovt9_off, n->ovt9_size, size) || !wl_range_ok(n->ovt7_off, n->ovt7_size, size)) {
        return WL_ERR_RANGE;
    }
    if (n->fat_size % 8u != 0 || n->ovt9_size % NDS_OVL_ENTRY != 0 || n->ovt7_size % NDS_OVL_ENTRY != 0) {
        return WL_ERR_FORMAT;
    }
    n->file_count = n->fat_size / 8u;
    n->ov9_count = n->ovt9_size / NDS_OVL_ENTRY;
    n->ov7_count = n->ovt7_size / NDS_OVL_ENTRY;
    return WL_OK;
}

WlResult wl_nds_file(const WlNds *n, uint32_t id, const uint8_t **data, size_t *size) {
    if (n == NULL || data == NULL || size == NULL) {
        return WL_ERR_ARG;
    }
    if (id >= n->file_count) {
        return WL_ERR_RANGE;
    }
    const uint8_t *e = n->data + n->fat_off + (size_t)id * 8u;
    uint32_t start = wl_rd_le32(e), end = wl_rd_le32(e + 4);
    if (end < start || !wl_range_ok(start, (uint64_t)end - start, n->size)) {
        return WL_ERR_RANGE;
    }
    *data = n->data + start;
    *size = (size_t)(end - start);
    return WL_OK;
}

WlResult wl_nds_overlay(const WlNds *n, int arm9, uint32_t i, WlNdsOverlay *ov) {
    if (n == NULL || ov == NULL) {
        return WL_ERR_ARG;
    }
    if (i >= (arm9 ? n->ov9_count : n->ov7_count)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *e = n->data + (arm9 ? n->ovt9_off : n->ovt7_off) + (size_t)i * NDS_OVL_ENTRY;
    ov->id = wl_rd_le32(e);
    ov->ram_addr = wl_rd_le32(e + 4);
    ov->ram_size = wl_rd_le32(e + 8);
    ov->bss_size = wl_rd_le32(e + 12);
    ov->sinit_start = wl_rd_le32(e + 16);
    ov->sinit_end = wl_rd_le32(e + 20);
    ov->file_id = wl_rd_le32(e + 24);
    uint32_t w = wl_rd_le32(e + 28);
    ov->comp_size = w & 0xFFFFFFu;
    ov->compressed = (w >> 24) & 1u;
    return WL_OK;
}

WlResult wl_nds_overlay_data(const WlNds *n, const WlNdsOverlay *ov, const uint8_t **data,
                             size_t *size) {
    if (ov == NULL) {
        return WL_ERR_ARG;
    }
    return wl_nds_file(n, ov->file_id, data, size);
}

typedef struct {
    const WlNds *n;
    WlNdsFileFn fn;
    void *ud;
} WalkCtx;

static WlResult walk_cb(void *ud, const char *path, uint32_t id) {
    WalkCtx *c = (WalkCtx *)ud;
    const uint8_t *d;
    size_t sz;
    WlResult r = wl_nds_file(c->n, id, &d, &sz);
    if (r != WL_OK) {
        return r;
    }
    return c->fn(c->ud, path, id, d, sz);
}

WlResult wl_nds_walk(const WlNds *n, WlNdsFileFn fn, void *ud) {
    if (n == NULL || fn == NULL) {
        return WL_ERR_ARG;
    }
    WalkCtx c = {n, fn, ud};
    return wl_fnt_walk(n->data + n->fnt_off, n->fnt_size, walk_cb, &c);
}
