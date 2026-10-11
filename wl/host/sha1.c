// SPDX-License-Identifier: MIT
// SHA-1 written from FIPS 180-4 section 6.1 (public standard).
#include "wl/host/sha1.h"

#include <string.h>

static uint32_t rotl(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

static void compress(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (size_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (size_t i = 16; i < 80; i++) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (size_t i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void wl_sha1_init(WlSha1 *ctx) {
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->length = 0;
    ctx->fill = 0;
}

void wl_sha1_update(WlSha1 *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->length += len;
    if (ctx->fill > 0) {
        size_t take = 64 - ctx->fill;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->block + ctx->fill, p, take);
        ctx->fill += take;
        p += take;
        len -= take;
        if (ctx->fill < 64) {
            return;
        }
        compress(ctx->h, ctx->block);
        ctx->fill = 0;
    }
    while (len >= 64) {
        compress(ctx->h, p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(ctx->block, p, len);
        ctx->fill = len;
    }
}

void wl_sha1_final(WlSha1 *ctx, uint8_t digest[WL_SHA1_SIZE]) {
    uint64_t bits = ctx->length * 8u;
    uint8_t pad = 0x80;
    uint8_t zero = 0;
    uint8_t len_be[8];
    wl_sha1_update(ctx, &pad, 1);
    while (ctx->fill != 56) {
        wl_sha1_update(ctx, &zero, 1);
    }
    for (size_t i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    wl_sha1_update(ctx, len_be, 8);
    for (size_t i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

void wl_sha1_buf(const void *data, size_t len, uint8_t digest[WL_SHA1_SIZE]) {
    WlSha1 ctx;
    wl_sha1_init(&ctx);
    wl_sha1_update(&ctx, data, len);
    wl_sha1_final(&ctx, digest);
}

void wl_sha1_to_hex(const uint8_t digest[WL_SHA1_SIZE], char out[WL_SHA1_HEX_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < WL_SHA1_SIZE; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15u];
    }
    out[40] = '\0';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

WlResult wl_sha1_from_hex(const char *hex, uint8_t digest[WL_SHA1_SIZE]) {
    if (hex == NULL || strlen(hex) != 40) {
        return WL_ERR_FORMAT;
    }
    for (size_t i = 0; i < WL_SHA1_SIZE; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return WL_ERR_FORMAT;
        }
        digest[i] = (uint8_t)(hi * 16 + lo);
    }
    return WL_OK;
}
