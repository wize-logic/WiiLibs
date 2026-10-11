// SPDX-License-Identifier: MIT
// Non-cryptographic hashes: FNV-1a 64 (ids, keys) and CRC-32 (integrity of files and saves).
// Implementation: wl/base/hash.c. Results do not depend on host endianness.
#pragma once

#include <stddef.h>
#include <stdint.h>

// FNV-1a, 64 bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3). Incremental use:
// start from WL_FNV1A64_INIT and feed pieces to wl_fnv1a64_update.
#define WL_FNV1A64_INIT 0xcbf29ce484222325ull
uint64_t wl_fnv1a64_update(uint64_t hash, const void *data, size_t len);
uint64_t wl_fnv1a64(const void *data, size_t len);

// CRC-32 as in IEEE 802.3, zlib and PNG (reflected, polynomial 0xEDB88320, init and final XOR
// 0xFFFFFFFF). wl_crc32_update continues a previous result: start with crc = 0, and
// wl_crc32_update(wl_crc32_update(0, a, n), b, m) == wl_crc32 of a followed by b.
uint32_t wl_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t wl_crc32(const void *data, size_t len);
