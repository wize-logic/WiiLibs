// SPDX-License-Identifier: MIT
// FNV-1a 64 and CRC-32 (see hash.h).
#include "wl/base/hash.h"

uint64_t wl_fnv1a64_update(uint64_t hash, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

uint64_t wl_fnv1a64(const void *data, size_t len) {
    return wl_fnv1a64_update(WL_FNV1A64_INIT, data, len);
}

// Nibble-at-a-time table: 64 bytes of rodata instead of 1 KiB, fast enough for saves and archives.
static const uint32_t k_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
    0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

uint32_t wl_crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 0xFu];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 0xFu];
    }
    return ~crc;
}

uint32_t wl_crc32(const void *data, size_t len) {
    return wl_crc32_update(0, data, len);
}
