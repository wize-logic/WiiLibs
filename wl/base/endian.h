// SPDX-License-Identifier: MIT
// Byte-wise endian readers and writers.
//
// Never cast a byte buffer to a struct. Packs are big-endian; the wire and saves are
// little-endian. These helpers load and store one byte at a time, so they work at any
// alignment on Broadway and on x86-64; GCC turns them into single loads where it can.
#pragma once

#include <stdint.h>

static inline uint16_t wl_rd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t wl_rd_le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint32_t wl_rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t wl_rd_le64(const uint8_t *p) {
    return (uint64_t)wl_rd_le32(p) | ((uint64_t)wl_rd_le32(p + 4) << 32);
}

static inline uint16_t wl_rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t wl_rd_be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static inline uint32_t wl_rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t wl_rd_be64(const uint8_t *p) {
    return ((uint64_t)wl_rd_be32(p) << 32) | (uint64_t)wl_rd_be32(p + 4);
}

static inline void wl_wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void wl_wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void wl_wr_le64(uint8_t *p, uint64_t v) {
    wl_wr_le32(p, (uint32_t)v);
    wl_wr_le32(p + 4, (uint32_t)(v >> 32));
}

static inline void wl_wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wl_wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void wl_wr_be64(uint8_t *p, uint64_t v) {
    wl_wr_be32(p, (uint32_t)(v >> 32));
    wl_wr_be32(p + 4, (uint32_t)v);
}
