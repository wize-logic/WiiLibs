// SPDX-License-Identifier: MIT
#include "wl/audio/fmt.h"

#include <string.h>

#include "wl/base/endian.h"

static void read_channel(const uint8_t *c, WlDspChannelInfo *o) {
    for (uint32_t k = 0; k < 16; k++) {
        o->coef[k] = (int16_t)wl_rd_be16(c + 2 * k);
    }
    o->init_ps = wl_rd_be16(c + 0x22);
    o->init_h1 = (int16_t)wl_rd_be16(c + 0x24);
    o->init_h2 = (int16_t)wl_rd_be16(c + 0x26);
    o->loop_ps = wl_rd_be16(c + 0x28);
    o->loop_h1 = (int16_t)wl_rd_be16(c + 0x2A);
    o->loop_h2 = (int16_t)wl_rd_be16(c + 0x2C);
}

WlResult wl_pwst_parse(const uint8_t *d, size_t n, uint64_t payload_size, WlStreamInfo *out) {
    if (n < WL_PWST_HEADER_BYTES || payload_size < WL_PWST_HEADER_BYTES) {
        return WL_ERR_FORMAT;
    }
    if (memcmp(d, "PWST", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    if (wl_rd_be16(d + 4) > 1u) {
        return WL_ERR_VERSION;
    }
    WlStreamInfo i;
    memset(&i, 0, sizeof i);
    i.flags = wl_rd_be16(d + 6);
    i.channels = d[8];
    if (i.channels < 1 || i.channels > 2 || d[9] != 0) {
        return WL_ERR_FORMAT;
    }
    i.rate = wl_rd_be32(d + 0xC);
    i.samples = wl_rd_be32(d + 0x10);
    i.loop_start = wl_rd_be32(d + 0x14);
    i.loop_end = wl_rd_be32(d + 0x18);
    i.data_off = wl_rd_be32(d + 0x1C);
    i.data_size = wl_rd_be32(d + 0x20);
    if (i.rate < 4000 || i.rate > 96000 || i.samples == 0 || i.samples % 14u != 0) {
        return WL_ERR_FORMAT;
    }
    const uint32_t blocks_end = WL_PWST_HEADER_BYTES + WL_PWST_CHANNEL_BYTES * i.channels;
    if (n < blocks_end || payload_size < blocks_end || (i.data_off & 31u) != 0 || i.data_off < blocks_end) {
        return WL_ERR_FORMAT;
    }
    if ((uint64_t)i.data_size != (uint64_t)i.samples / 14u * 8u * i.channels ||
        (uint64_t)i.data_off + i.data_size > payload_size) {
        return WL_ERR_FORMAT;
    }
    if (i.flags & WL_PWST_FLAG_LOOP) {
        if (i.loop_start >= i.loop_end || i.loop_end >= i.samples) {
            return WL_ERR_FORMAT;
        }
        if (!(i.flags & WL_PWST_FLAG_LOOP_MID_FRAME) && i.loop_start % 14u != 0) {
            return WL_ERR_FORMAT;
        }
    }
    for (uint32_t c = 0; c < i.channels; c++) {
        read_channel(d + WL_PWST_HEADER_BYTES + WL_PWST_CHANNEL_BYTES * c, &i.ch[c]);
    }
    *out = i;
    return WL_OK;
}

WlResult wl_pwsf_parse(const uint8_t *d, size_t n, WlSfxInfo *out) {
    if (n < WL_PWSF_PROBE || memcmp(d, "PWSF", 4) != 0) {
        return WL_ERR_FORMAT;
    }
    if (wl_rd_be16(d + 4) > 1u) {
        return WL_ERR_VERSION;
    }
    WlSfxInfo i;
    memset(&i, 0, sizeof i);
    i.flags = wl_rd_be16(d + 6);
    if (d[8] != 1 || d[9] > 1) {
        return WL_ERR_FORMAT;
    }
    i.codec = d[9];
    i.bus = d[0xA];
    i.priority = d[0xB];
    i.rate = wl_rd_be32(d + 0xC);
    i.samples = wl_rd_be32(d + 0x10);
    i.loop_start = wl_rd_be32(d + 0x14);
    i.loop_end = wl_rd_be32(d + 0x18);
    i.data_off = wl_rd_be32(d + 0x1C);
    i.data_size = wl_rd_be32(d + 0x20);
    i.pitch_cents = (int16_t)wl_rd_be16(d + 0x24);
    i.volume = wl_rd_be16(d + 0x26);
    i.pan = (int8_t)d[0x28];
    i.play_ms = wl_rd_be16(d + 0x2A);
    i.duration_ms = wl_rd_be32(d + 0x2C);
    if (i.bus > 3 || i.rate < 4000 || i.rate > 96000 || i.samples == 0 || i.samples > (1u << 24)) {
        return WL_ERR_FORMAT;
    }
    const uint64_t need = i.codec == 0 ? ((uint64_t)i.samples + 13u) / 14u * 8u : (uint64_t)i.samples * 2u;
    if (i.data_off < WL_PWSF_PROBE || (uint64_t)i.data_size < need ||
        (uint64_t)i.data_off + i.data_size > n) {
        return WL_ERR_FORMAT;
    }
    if (i.flags & 1u) {
        if (i.loop_start >= i.loop_end || i.loop_end >= i.samples) {
            return WL_ERR_FORMAT;
        }
    }
    read_channel(d + WL_PWSF_HEADER_BYTES, &i.ch);
    *out = i;
    return WL_OK;
}
