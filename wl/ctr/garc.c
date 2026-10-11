// SPDX-License-Identifier: MIT
// GARC v4 container (little-endian), layout in README.md:
// header "CRAG", FATO "OTAF" (per-entry offsets), FATB "BTAF" (per-entry sub-file mask and
// start/end/length triples) and FIMB "BMIF" (data). Per-entry LZ11 is detected, not applied here.
#include <string.h>

#include "wl/base/endian.h"
#include "wl/ctr/ctr.h"
#include "wl/nds/lz.h"

#define GARC_HDR 0x1Cu

int wl_garc_is_garc(const uint8_t *d, size_t n) {
    return n >= 4 && memcmp(d, "CRAG", 4) == 0;
}

WlResult wl_garc_open(WlGarc *g, const uint8_t *buf, size_t len) {
    if (g == NULL || buf == NULL) {
        return WL_ERR_ARG;
    }
    memset(g, 0, sizeof *g);
    if (len < GARC_HDR + 12u || !wl_garc_is_garc(buf, len)) {
        return WL_ERR_FORMAT;
    }
    if (wl_rd_le32(buf + 4) != GARC_HDR || wl_rd_le16(buf + 8) != 0xFEFFu ||
        wl_rd_le16(buf + 10) != 0x0400u) {
        return WL_ERR_FORMAT; /* only version 4 is supported */
    }
    uint32_t data_off = wl_rd_le32(buf + 0x10), file_size = wl_rd_le32(buf + 0x14);
    if (file_size > len) {
        return WL_ERR_RANGE;
    }
    len = file_size;
    /* FATO */
    uint32_t fato_off = GARC_HDR;
    if (!wl_range_ok(fato_off, 12, len) || memcmp(buf + fato_off, "OTAF", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    uint32_t fato_size = wl_rd_le32(buf + fato_off + 4);
    uint32_t count = wl_rd_le16(buf + fato_off + 8);
    if (fato_size < 12u + 4u * count || !wl_range_ok(fato_off, fato_size, len)) {
        return WL_ERR_RANGE;
    }
    /* FATB */
    uint32_t fatb_off = fato_off + fato_size;
    if (!wl_range_ok(fatb_off, 12, len) || memcmp(buf + fatb_off, "BTAF", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    uint32_t fatb_size = wl_rd_le32(buf + fatb_off + 4);
    if (fatb_size < 12 || !wl_range_ok(fatb_off, fatb_size, len)) {
        return WL_ERR_RANGE;
    }
    /* FIMB */
    uint32_t fimb_off = fatb_off + fatb_size;
    if (!wl_range_ok(fimb_off, 12, len) || memcmp(buf + fimb_off, "BMIF", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    uint32_t data_len = wl_rd_le32(buf + fimb_off + 8);
    if (data_off != fimb_off + 12u || !wl_range_ok(data_off, data_len, len)) {
        return WL_ERR_RANGE;
    }
    g->buf = buf;
    g->len = len;
    g->entries = count;
    g->fato = buf + fato_off + 12;
    g->fatb_entries_off = fatb_off + 12u;
    g->fatb_len = fatb_size - 12u;
    g->data_off = data_off;
    g->data_len = data_len;
    return WL_OK;
}

WlResult wl_garc_mask(const WlGarc *g, uint32_t entry, uint32_t *mask) {
    if (g == NULL || mask == NULL || entry >= g->entries) {
        return WL_ERR_ARG;
    }
    uint32_t off = wl_rd_le32(g->fato + (size_t)entry * 4u);
    if (!wl_range_ok(off, 4, g->fatb_len)) {
        return WL_ERR_RANGE;
    }
    *mask = wl_rd_le32(g->buf + g->fatb_entries_off + off);
    return WL_OK;
}

WlResult wl_garc_sub(const WlGarc *g, uint32_t entry, unsigned bit, const uint8_t **data, size_t *len) {
    uint32_t mask;
    WlResult r = wl_garc_mask(g, entry, &mask);
    if (r != WL_OK) {
        return r;
    }
    if (data == NULL || len == NULL || bit >= 32 || (mask & (1u << bit)) == 0) {
        return WL_ERR_RANGE;
    }
    unsigned idx = 0;
    for (unsigned b = 0; b < bit; b++) {
        idx += (mask >> b) & 1u;
    }
    uint32_t off = wl_rd_le32(g->fato + (size_t)entry * 4u);
    uint64_t rec = (uint64_t)off + 4u + (uint64_t)idx * 12u;
    if (!wl_range_ok(rec, 12, g->fatb_len)) {
        return WL_ERR_RANGE;
    }
    const uint8_t *p = g->buf + g->fatb_entries_off + rec;
    uint32_t start = wl_rd_le32(p), end = wl_rd_le32(p + 4), size = wl_rd_le32(p + 8);
    if (end < start || size > end - start || !wl_range_ok(start, size, g->data_len)) {
        return WL_ERR_RANGE;
    }
    *data = g->buf + g->data_off + start;
    *len = size;
    return WL_OK;
}

int wl_garc_is_lz11(const uint8_t *d, size_t n) {
    return n >= 5 && d[0] == 0x11 && wl_lz_looks_valid(d, n);
}
