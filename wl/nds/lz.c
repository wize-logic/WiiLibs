// SPDX-License-Identifier: MIT
// LZ10 / LZ11 / BLZ decoders written from the format descriptions (GBATEK LZ77 section and the
// DS footer layout); no third-party code. See lz.h.
#include "wl/nds/lz.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"

#define WL_LZ_MAX_DECLARED (256u * 1024u * 1024u)

int wl_lz_looks_valid(const uint8_t *src, size_t n) {
    if (n < 5 || (src[0] != 0x10 && src[0] != 0x11)) {
        return 0;
    }
    uint32_t size = wl_rd_le24(src + 1);
    if (size == 0) {
        if (n < 8) {
            return 0;
        }
        size = wl_rd_le32(src + 4);
    }
    /* A literal-only stream costs 9 bytes per 8, a match stream far less; cap the ratio. */
    return size != 0 && size <= WL_LZ_MAX_DECLARED && (uint64_t)size <= (uint64_t)n * 4096u;
}

WlResult wl_lz_decode(const uint8_t *src, size_t n, size_t max_out, uint8_t **out, size_t *out_len) {
    if (src == NULL || out == NULL || out_len == NULL) {
        return WL_ERR_ARG;
    }
    *out = NULL;
    *out_len = 0;
    if (n < 4 || (src[0] != 0x10 && src[0] != 0x11)) {
        return WL_ERR_FORMAT;
    }
    const int lz11 = src[0] == 0x11;
    uint64_t size = wl_rd_le24(src + 1);
    size_t pos = 4;
    if (size == 0) {
        if (n < 8) {
            return WL_ERR_FORMAT;
        }
        size = wl_rd_le32(src + 4);
        pos = 8;
    }
    if (size > max_out || size > WL_LZ_MAX_DECLARED) {
        return WL_ERR_RANGE;
    }
    uint8_t *dst = (uint8_t *)malloc(size ? (size_t)size : 1u);
    if (dst == NULL) {
        return WL_ERR_NOMEM;
    }
    size_t op = 0;
    const size_t total = (size_t)size;
    while (op < total) {
        if (pos >= n) {
            goto truncated;
        }
        unsigned flags = src[pos++];
        for (int bit = 0; bit < 8 && op < total; bit++, flags <<= 1) {
            if ((flags & 0x80u) == 0) {
                if (pos >= n) {
                    goto truncated;
                }
                dst[op++] = src[pos++];
                continue;
            }
            if (pos + 2 > n) {
                goto truncated;
            }
            size_t len, disp;
            unsigned b0 = src[pos];
            if (!lz11) {
                len = (b0 >> 4) + 3u;
                disp = (((size_t)(b0 & 15u) << 8) | src[pos + 1]) + 1u;
                pos += 2;
            } else if ((b0 >> 4) == 0) {
                if (pos + 3 > n) {
                    goto truncated;
                }
                len = (((size_t)(b0 & 15u) << 4) | (src[pos + 1] >> 4)) + 0x11u;
                disp = (((size_t)(src[pos + 1] & 15u) << 8) | src[pos + 2]) + 1u;
                pos += 3;
            } else if ((b0 >> 4) == 1) {
                if (pos + 4 > n) {
                    goto truncated;
                }
                len = (((size_t)(b0 & 15u) << 12) | ((size_t)src[pos + 1] << 4) |
                       (src[pos + 2] >> 4)) + 0x111u;
                disp = (((size_t)(src[pos + 2] & 15u) << 8) | src[pos + 3]) + 1u;
                pos += 4;
            } else {
                len = (b0 >> 4) + 1u;
                disp = (((size_t)(b0 & 15u) << 8) | src[pos + 1]) + 1u;
                pos += 2;
            }
            if (disp > op) {
                free(dst);
                return WL_ERR_FORMAT;
            }
            if (len > total - op) {
                len = total - op; /* the BIOS writes the same clamp implicitly */
            }
            for (size_t i = 0; i < len; i++, op++) {
                dst[op] = dst[op - disp];
            }
        }
    }
    *out = dst;
    *out_len = total;
    return WL_OK;
truncated:
    free(dst);
    return WL_ERR_FORMAT;
}

/* Footer (last 8 bytes): u32 at n-8 = compressed length (24 bits) | header length (top byte);
 * u32 at n-4 = how many bytes the data grows by. The first n-enc bytes are stored as-is. */
int wl_blz_footer_ok(const uint8_t *src, size_t n) {
    if (src == NULL || n < 8) {
        return 0;
    }
    uint32_t w = wl_rd_le32(src + n - 8);
    uint32_t inc = wl_rd_le32(src + n - 4);
    uint32_t enc = w & 0xFFFFFFu;
    uint32_t hdr = w >> 24;
    return inc != 0 && inc <= WL_LZ_MAX_DECLARED && hdr >= 8 && hdr <= enc && enc <= n;
}

WlResult wl_blz_decode(const uint8_t *src, size_t n, size_t max_out, uint8_t **out, size_t *out_len) {
    if (src == NULL || out == NULL || out_len == NULL) {
        return WL_ERR_ARG;
    }
    *out = NULL;
    *out_len = 0;
    if (!wl_blz_footer_ok(src, n)) {
        return WL_ERR_FORMAT;
    }
    uint32_t w = wl_rd_le32(src + n - 8);
    size_t enc = w & 0xFFFFFFu;
    size_t hdr = w >> 24;
    uint64_t total64 = (uint64_t)n + wl_rd_le32(src + n - 4);
    if (total64 > max_out) {
        return WL_ERR_RANGE;
    }
    size_t total = (size_t)total64;
    size_t stop = n - enc; /* start of the compressed region (same index in input and output) */
    uint8_t *dst = (uint8_t *)malloc(total);
    if (dst == NULL) {
        return WL_ERR_NOMEM;
    }
    memcpy(dst, src, stop);
    size_t sp = n - hdr, dp = total;
    while (sp > stop && dp > stop) {
        unsigned flags = src[--sp];
        for (int bit = 0; bit < 8 && dp > stop; bit++, flags <<= 1) {
            if ((flags & 0x80u) == 0) {
                if (sp <= stop) {
                    goto bad;
                }
                dst[--dp] = src[--sp];
                continue;
            }
            if (sp < stop + 2) {
                goto bad;
            }
            unsigned b0 = src[--sp];
            unsigned b1 = src[--sp];
            size_t len = (b0 >> 4) + 3u;
            size_t disp = (((size_t)(b0 & 15u) << 8) | b1) + 3u;
            if (len > dp - stop) {
                len = dp - stop; /* clamp, as the reference decoders do */
            }
            for (size_t i = 0; i < len; i++) {
                dp--;
                if (dp + disp >= total) {
                    goto bad;
                }
                dst[dp] = dst[dp + disp];
            }
        }
    }
    if (dp != stop) {
        goto bad;
    }
    *out = dst;
    *out_len = total;
    return WL_OK;
bad:
    free(dst);
    return WL_ERR_FORMAT;
}
