// SPDX-License-Identifier: MIT
#include "wl/base/utf8.h"

size_t wl_utf8_decode(const char *s, size_t len, uint32_t *cp) {
    const uint8_t *p = (const uint8_t *)s;
    if (len == 0) {
        return 0;
    }
    const uint8_t b0 = p[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    size_t n;
    uint32_t v;
    uint32_t min;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        n = 2;
        v = b0 & 0x1Fu;
        min = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        n = 3;
        v = b0 & 0x0Fu;
        min = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        n = 4;
        v = b0 & 0x07u;
        min = 0x10000;
    } else {
        return 0; // continuation byte, C0/C1 (always overlong), F5..FF
    }
    if (len < n) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            return 0;
        }
        v = (v << 6) | (p[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) {
        return 0;
    }
    *cp = v;
    return n;
}

size_t wl_utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return 0;
    }
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

WlResult wl_utf8_length(const char *s, size_t len, size_t *out_count) {
    size_t count = 0;
    size_t off = 0;
    while (off < len) {
        uint32_t cp;
        size_t n = wl_utf8_decode(s + off, len - off, &cp);
        if (n == 0) {
            return WL_ERR_FORMAT;
        }
        off += n;
        count++;
    }
    *out_count = count;
    return WL_OK;
}

bool wl_utf8_valid(const char *s, size_t len) {
    size_t count;
    return wl_utf8_length(s, len, &count) == WL_OK;
}
