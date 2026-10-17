// SPDX-License-Identifier: MIT
// A guest page packed small (mem_internal.h): a byte-oriented LZ77 of the LZ4 kind, written here for
// a 4 KB page on a 729 MHz Broadway. A sequence is a token (its literals' count in the high nibble,
// its match's length - 4 in the low; 15 continues in bytes of 255 and a last byte below it), the
// literals, the match's distance (2 bytes, little-endian, 1..4095) and the rest of its length; the
// last sequence has literals only. A run of one word (a cleared or filled block) is one match.
// The packer's table is static: callers hold the MMU lock (one at a time).
#include <string.h>

#include "wl/xboxrt/mem_internal.h"

#define PAGE WL_XB_PAGE
#define HBITS 11u
#define MINMATCH 4u
#define TAIL 8u // the page's last bytes are literals (the match finder reads 4 bytes ahead)

static uint16_t g_ht[1u << HBITS]; // a position for each hash (stale ones are checked, not trusted)

static inline uint32_t load32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint32_t hash32(uint32_t v) {
    return (v * 2654435761u) >> (32u - HBITS);
}

// A length's continuation bytes: `n` already counted 15 in the token.
static inline uint32_t len_bytes(uint32_t n) {
    return n >= 15u ? (n - 15u) / 255u + 1u : 0u;
}

static uint8_t *put_len(uint8_t *op, uint32_t n) {
    for (n -= 15u; n >= 255u; n -= 255u) {
        *op++ = 255u;
    }
    *op++ = (uint8_t)n;
    return op;
}

static uint8_t *put_seq(uint8_t *op, const uint8_t *lit, uint32_t nlit, uint32_t dist, uint32_t mlen) {
    uint32_t ml = mlen != 0 ? mlen - MINMATCH : 0u;
    *op++ = (uint8_t)(((nlit < 15u ? nlit : 15u) << 4) | (ml < 15u ? ml : 15u));
    if (nlit >= 15u) {
        op = put_len(op, nlit);
    }
    memcpy(op, lit, nlit);
    op += nlit;
    if (mlen != 0) {
        *op++ = (uint8_t)dist;
        *op++ = (uint8_t)(dist >> 8);
        if (ml >= 15u) {
            op = put_len(op, ml);
        }
    }
    return op;
}

size_t wl_xb_page_pack(const uint8_t *src, uint8_t *dst, size_t cap) {
    uint8_t *op = dst;
    uint32_t ip = 1, anchor = 0, misses = 0;
    g_ht[hash32(load32(src))] = 0;
    while (ip + MINMATCH + TAIL <= PAGE) {
        uint32_t v = load32(src + ip);
        uint32_t h = hash32(v);
        uint32_t c = g_ht[h];
        g_ht[h] = (uint16_t)ip;
        if (c >= ip || load32(src + c) != v) {
            ip += 1u + (misses++ >> 5); // incompressible bytes are passed faster
            continue;
        }
        misses = 0;
        while (ip > anchor && c > 0 && src[ip - 1u] == src[c - 1u]) {
            ip--;
            c--;
        }
        uint32_t end = PAGE - TAIL, m = ip + MINMATCH, s = c + MINMATCH;
        while (m + 4u <= end && load32(src + m) == load32(src + s)) {
            m += 4u;
            s += 4u;
        }
        while (m < end && src[m] == src[s]) {
            m++;
            s++;
        }
        uint32_t nlit = ip - anchor, mlen = m - ip;
        size_t need = 1u + len_bytes(nlit) + nlit + 2u + len_bytes(mlen - MINMATCH);
        if ((size_t)(op - dst) + need > cap) {
            return 0;
        }
        op = put_seq(op, src + anchor, nlit, ip - c, mlen);
        ip = m;
        anchor = m;
        if (ip >= 2u && ip + MINMATCH <= PAGE) {
            g_ht[hash32(load32(src + ip - 2u))] = (uint16_t)(ip - 2u);
        }
    }
    uint32_t nlit = PAGE - anchor;
    if ((size_t)(op - dst) + 1u + len_bytes(nlit) + nlit > cap) {
        return 0;
    }
    op = put_seq(op, src + anchor, nlit, 0, 0);
    return (size_t)(op - dst);
}

// A length's continuation; false past the input.
static bool get_len(const uint8_t **ip, const uint8_t *end, uint32_t *n) {
    uint8_t b;
    do {
        if (*ip >= end) {
            return false;
        }
        b = *(*ip)++;
        *n += b;
    } while (b == 255u && *n < 2u * PAGE);
    return true;
}

bool wl_xb_page_unpack(const uint8_t *src, size_t n, uint8_t *dst) {
    const uint8_t *ip = src, *end = src + n;
    uint32_t op = 0;
    while (ip < end) {
        uint8_t tok = *ip++;
        uint32_t nlit = tok >> 4;
        if (nlit == 15u && !get_len(&ip, end, &nlit)) {
            return false;
        }
        if (nlit > (uint32_t)(end - ip) || nlit > PAGE - op) {
            return false;
        }
        memcpy(dst + op, ip, nlit);
        ip += nlit;
        op += nlit;
        if (ip == end) {
            break; // the last sequence
        }
        if (end - ip < 2) {
            return false;
        }
        uint32_t dist = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
        ip += 2;
        uint32_t mlen = tok & 15u;
        if (mlen == 15u && !get_len(&ip, end, &mlen)) {
            return false;
        }
        mlen += MINMATCH;
        if (dist == 0 || dist > op || mlen > PAGE - op) {
            return false;
        }
        uint8_t *d = dst + op;
        const uint8_t *s = d - dist;
        if (dist >= 8u) {
            uint32_t k = 0;
            for (; k + 8u <= mlen; k += 8u) {
                memcpy(d + k, s + k, 8);
            }
            for (; k < mlen; k++) {
                d[k] = s[k];
            }
        } else {
            for (uint32_t k = 0; k < mlen; k++) {
                d[k] = s[k];
            }
        }
        op += mlen;
    }
    return op == PAGE;
}
