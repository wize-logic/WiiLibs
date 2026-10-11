// SPDX-License-Identifier: MIT
// Genius Sonority LZSS (Okumura variant), see wl/gs/gs.h. Written from the format description;
// no code is copied.
#include <stdlib.h>
#include <string.h>

#include "wl/gs/gs.h"
#include "wl/base/endian.h"

#define RING 4096u
#define RING_START 4078u

WlResult wl_gs_lzss_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if ((in == NULL && in_len != 0) || (out == NULL && out_len != 0)) {
        return WL_ERR_ARG;
    }
    uint8_t ring[RING];
    memset(ring, 0, sizeof ring);
    uint32_t r = RING_START;
    size_t i = 0, o = 0;
    uint32_t flags = 0;
    while (o < out_len) {
        flags >>= 1;
        if ((flags & 0x100u) == 0) {
            if (i >= in_len) {
                return WL_ERR_FORMAT;
            }
            flags = (uint32_t)in[i++] | 0xFF00u;
        }
        if (flags & 1u) {
            if (i >= in_len) {
                return WL_ERR_FORMAT;
            }
            uint8_t c = in[i++];
            out[o++] = c;
            ring[r] = c;
            r = (r + 1) & (RING - 1);
        } else {
            if (in_len - i < 2) {
                return WL_ERR_FORMAT;
            }
            uint32_t a = in[i], b = in[i + 1];
            i += 2;
            uint32_t pos = a | ((b & 0xF0u) << 4);
            uint32_t n = (b & 0x0Fu) + 3;
            for (uint32_t k = 0; k < n && o < out_len; k++) {
                uint8_t c = ring[(pos + k) & (RING - 1)];
                out[o++] = c;
                ring[r] = c;
                r = (r + 1) & (RING - 1);
            }
        }
    }
    return WL_OK;
}

WlResult wl_gs_lzss_unpack(const uint8_t *in, size_t in_len, size_t max, uint8_t **out,
                           size_t *out_len) {
    if (in == NULL || out == NULL || out_len == NULL) {
        return WL_ERR_ARG;
    }
    if (in_len < WL_GS_LZSS_HDR || wl_rd_be32(in) != WL_GS_LZSS_MAGIC) {
        return WL_ERR_FORMAT;
    }
    uint32_t usize = wl_rd_be32(in + 4), psize = wl_rd_be32(in + 8);
    if (psize < WL_GS_LZSS_HDR || psize > in_len) {
        return WL_ERR_FORMAT;
    }
    if (usize > max) {
        return WL_ERR_RANGE;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)usize + 1);
    if (buf == NULL) {
        return WL_ERR_NOMEM;
    }
    WlResult r = wl_gs_lzss_decode(in + WL_GS_LZSS_HDR, psize - WL_GS_LZSS_HDR, buf, usize);
    if (r != WL_OK) {
        free(buf);
        return r;
    }
    buf[usize] = 0;
    *out = buf;
    *out_len = usize;
    return WL_OK;
}
