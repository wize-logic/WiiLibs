// SPDX-License-Identifier: MIT
// Streamed SHA-1 (FIPS 180-4). Used only to identify dumps and to hash outputs, never for security.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/host/base.h"

#define WL_SHA1_SIZE 20
#define WL_SHA1_HEX_SIZE 41 /* 40 digits and the terminator */

typedef struct {
    uint32_t h[5];
    uint64_t length; /* bytes processed so far */
    uint8_t block[64];
    size_t fill;
} WlSha1;

void wl_sha1_init(WlSha1 *ctx);
void wl_sha1_update(WlSha1 *ctx, const void *data, size_t len);
void wl_sha1_final(WlSha1 *ctx, uint8_t digest[WL_SHA1_SIZE]);
void wl_sha1_buf(const void *data, size_t len, uint8_t digest[WL_SHA1_SIZE]);
void wl_sha1_to_hex(const uint8_t digest[WL_SHA1_SIZE], char out[WL_SHA1_HEX_SIZE]);
/* Exactly 40 hex digits, either case. */
WlResult wl_sha1_from_hex(const char *hex, uint8_t digest[WL_SHA1_SIZE]);
