// SPDX-License-Identifier: MIT
// GX texture containers and a stored-deflate PNG writer. The texel encoders and decoders are the
// shared codec in wl/codec/gx_tex.c; this file wraps them in WlGxTex. Block layouts are the public
// GX ones (README.md).
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/base/hash.h"
#include "wl/codec/gx_tex.h"
#include "wl/gxtex/gxtex.h"

#define MAX_DIM 4096u

/* ---------------------------------------------------------------------------------------- */
/* colours                                                                                  */
/* ---------------------------------------------------------------------------------------- */

uint16_t wl_gx_rgb5a3(const uint8_t p[4]) {
    return wl_gxc_rgb5a3_encode(p);
}

void wl_gx_rgb5a3_to_rgba(uint16_t v, uint8_t out[4]) {
    wl_gxc_rgb5a3_decode(v, out);
}

/* The shared codec's format number for a GX texture format (0 when unsupported). */
static int gxc_format(uint32_t fmt, WlGxcFormat *out) {
    switch (fmt) {
    case WL_GX_TF_I4: *out = WL_GXC_I4; return 1;
    case WL_GX_TF_RGB565: *out = WL_GXC_RGB565; return 1;
    case WL_GX_TF_RGB5A3: *out = WL_GXC_RGB5A3; return 1;
    case WL_GX_TF_RGBA8: *out = WL_GXC_RGBA8; return 1;
    case WL_GX_TF_C4: *out = WL_GXC_C4; return 1;
    case WL_GX_TF_C8: *out = WL_GXC_C8; return 1;
    case WL_GX_TF_CMPR: *out = WL_GXC_CMPR; return 1;
    default: return 0;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* GX textures                                                                              */
/* ---------------------------------------------------------------------------------------- */

static uint32_t up(uint32_t v, uint32_t m) {
    return (v + m - 1u) / m * m;
}

void wl_gx_tex_free(WlGxTex *t) {
    if (t != NULL) {
        free(t->data);
        memset(t, 0, sizeof *t);
    }
}

/* Block width, height and bytes of a format; 0 when unknown. */
static int block_of(uint32_t fmt, uint32_t *bw, uint32_t *bh, uint32_t *bsz) {
    switch (fmt) {
    case WL_GX_TF_RGBA8: *bw = *bh = 4; *bsz = 64; return 1;
    case WL_GX_TF_RGB5A3:
    case WL_GX_TF_RGB565:
    case WL_GX_TF_IA8: *bw = *bh = 4; *bsz = 32; return 1;
    case WL_GX_TF_I4:
    case WL_GX_TF_C4:
    case WL_GX_TF_CMPR: *bw = *bh = 8; *bsz = 32; return 1;
    case WL_GX_TF_I8:
    case WL_GX_TF_IA4:
    case WL_GX_TF_C8: *bw = 8; *bh = 4; *bsz = 32; return 1;
    default: return 0;
    }
}

size_t wl_gx_tex_size(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t bw, bh, bsz;
    if (!block_of(fmt, &bw, &bh, &bsz)) {
        return 0;
    }
    return (size_t)(up(w, bw) / bw) * (up(h, bh) / bh) * bsz;
}

/* Fills `out` around `data` (already in GX block order; ownership moves to out). tlut may be NULL. */
static void tex_wrap(uint32_t fmt, uint32_t w, uint32_t h, uint8_t *data, size_t dsz,
                     const uint16_t *tlut, uint32_t ntl, WlGxTex *out) {
    memset(out, 0, sizeof *out);
    out->fmt = fmt;
    out->w = w;
    out->h = h;
    out->data = data;
    out->size = dsz;
    if (tlut != NULL) {
        out->tlut_fmt = WL_GX_TL_RGB5A3;
        out->ntlut = ntl;
        for (uint32_t i = 0; i < ntl; i++) {
            wl_wr_be16(out->tlut + 2u * i, tlut[i]);
        }
    }
}

WlResult wl_gx_tex_from_indexed(const uint8_t *idx, uint32_t w, uint32_t h, uint32_t bpp,
                                const uint8_t (*pal)[4], uint32_t ncol, WlGxTex *out) {
    if (idx == NULL || pal == NULL || out == NULL || w == 0 || h == 0 ||
        w > MAX_DIM || h > MAX_DIM || (bpp != 4 && bpp != 8) || ncol == 0 || ncol > (1u << bpp)) {
        return WL_ERR_ARG;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (idx[i] >= ncol) {
            return WL_ERR_RANGE;
        }
    }
    WlGxcFormat gf = bpp == 4 ? WL_GXC_C4 : WL_GXC_C8;
    size_t dsz = wl_gxc_tex_bytes(gf, w, h);
    uint8_t *data = calloc(dsz, 1);
    uint16_t tl[256];
    if (data == NULL) {
        return WL_ERR_NOMEM;
    }
    if (wl_gxc_encode_ci(gf, idx, w, h, data, dsz) != dsz) {
        free(data);
        return WL_ERR_ARG;
    }
    for (uint32_t i = 0; i < ncol; i++) {
        tl[i] = wl_gx_rgb5a3(pal[i]);
    }
    tex_wrap(bpp == 4 ? WL_GX_TF_C4 : WL_GX_TF_C8, w, h, data, dsz, tl, ncol, out);
    return WL_OK;
}

WlResult wl_gx_tex_from_i4(const uint8_t *lum, uint32_t w, uint32_t h, WlGxTex *out) {
    if (lum == NULL || out == NULL || w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) {
        return WL_ERR_ARG;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (lum[i] > 15u) {
            return WL_ERR_RANGE;
        }
    }
    size_t dsz = wl_gxc_tex_bytes(WL_GXC_I4, w, h);
    uint8_t *data = calloc(dsz, 1);
    if (data == NULL) {
        return WL_ERR_NOMEM;
    }
    if (wl_gxc_encode_ci(WL_GXC_I4, lum, w, h, data, dsz) != dsz) { /* 0..15 values pack like C4 indices */
        free(data);
        return WL_ERR_ARG;
    }
    tex_wrap(WL_GX_TF_I4, w, h, data, dsz, NULL, 0, out);
    return WL_OK;
}

WlResult wl_gx_tex_from_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t fmt,
                             WlGxTex *out) {
    if (rgba == NULL || out == NULL || w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) {
        return WL_ERR_ARG;
    }
    WlGxcFormat gf;
    if (!gxc_format(fmt, &gf) || (fmt != WL_GX_TF_RGBA8 && fmt != WL_GX_TF_RGB5A3 && fmt != WL_GX_TF_RGB565 &&
                                  fmt != WL_GX_TF_CMPR)) {
        return WL_ERR_UNSUPPORTED;
    }
    size_t dsz = wl_gxc_tex_bytes(gf, w, h);
    uint8_t *data = calloc(dsz, 1);
    if (data == NULL) {
        return WL_ERR_NOMEM;
    }
    if (wl_gxc_encode(gf, rgba, w, h, data, dsz) != dsz) {
        free(data);
        return WL_ERR_ARG;
    }
    tex_wrap(fmt, w, h, data, dsz, NULL, 0, out);
    return WL_OK;
}

WlResult wl_gx_decode(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *data, size_t size,
                      uint32_t tlut_fmt, const uint8_t *tlut, uint32_t ntl, WlImage *im) {
    if (im == NULL || data == NULL || w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) {
        return WL_ERR_ARG;
    }
    WlGxcFormat gf;
    if (!gxc_format(fmt, &gf)) {
        return WL_ERR_UNSUPPORTED;
    }
    int indexed = gf == WL_GXC_C4 || gf == WL_GXC_C8;
    if (indexed && (tlut == NULL || ntl == 0 || ntl > 256u || tlut_fmt != WL_GX_TL_RGB5A3)) {
        return WL_ERR_FORMAT;
    }
    if (wl_gxc_tex_bytes(gf, w, h) > size) {
        return WL_ERR_FORMAT;
    }
    uint8_t *rgba = calloc((size_t)w * h, 4);
    if (rgba == NULL) {
        return WL_ERR_NOMEM;
    }
    if (indexed) {
        uint8_t *ix = malloc((size_t)w * h);
        if (ix == NULL) {
            free(rgba);
            return WL_ERR_NOMEM;
        }
        wl_gxc_decode_ci(gf, data, size, w, h, ix);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            if (ix[i] >= ntl) {
                free(ix);
                free(rgba);
                return WL_ERR_FORMAT;
            }
            wl_gxc_rgb5a3_decode(wl_rd_be16(tlut + 2u * ix[i]), rgba + i * 4u);
        }
        free(ix);
    } else {
        wl_gxc_decode(gf, data, size, w, h, rgba);
    }
    im->w = w;
    im->h = h;
    im->rgba = rgba;
    return WL_OK;
}

WlResult wl_gx_tex_decode(const WlGxTex *t, WlImage *im) {
    if (t == NULL) {
        return WL_ERR_ARG;
    }
    return wl_gx_decode(t->fmt, t->w, t->h, t->data, t->size, t->tlut_fmt, t->tlut, t->ntlut, im);
}

/* ---------------------------------------------------------------------------------------- */
/* PNG (stored deflate)                                                                     */
/* ---------------------------------------------------------------------------------------- */

static void put_chunk(uint8_t *o, size_t *pos, const char *tag, const uint8_t *data, size_t n) {
    wl_wr_be32(o + *pos, (uint32_t)n);
    memcpy(o + *pos + 4, tag, 4);
    if (n > 0) {
        memcpy(o + *pos + 8, data, n);
    }
    uint32_t crc = wl_crc32(o + *pos + 4, n + 4);
    wl_wr_be32(o + *pos + 8 + n, crc);
    *pos += 12 + n;
}

WlResult wl_png_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t **out, size_t *len) {
    if (rgba == NULL || out == NULL || len == NULL || w == 0 || h == 0 || w > 65535u || h > 65535u) {
        return WL_ERR_ARG;
    }
    size_t raw = ((size_t)w * 4u + 1u) * h;
    size_t nblk = (raw + 65534u) / 65535u;
    size_t zlen = 2 + raw + 5 * nblk + 4;
    uint8_t *z = malloc(zlen);
    uint8_t *img = malloc(raw);
    if (z == NULL || img == NULL) {
        free(z);
        free(img);
        return WL_ERR_NOMEM;
    }
    for (uint32_t y = 0; y < h; y++) {
        img[(size_t)y * (w * 4u + 1u)] = 0;
        memcpy(img + (size_t)y * (w * 4u + 1u) + 1u, rgba + (size_t)y * w * 4u, (size_t)w * 4u);
    }
    size_t p = 0;
    z[p++] = 0x78;
    z[p++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) {
        a = (a + img[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < raw;) {
        size_t chunk = raw - off > 65535u ? 65535u : raw - off;
        z[p++] = (off + chunk == raw) ? 1u : 0u;
        z[p++] = (uint8_t)(chunk & 0xFFu);
        z[p++] = (uint8_t)(chunk >> 8);
        z[p++] = (uint8_t)(~chunk & 0xFFu);
        z[p++] = (uint8_t)((~chunk >> 8) & 0xFFu);
        memcpy(z + p, img + off, chunk);
        p += chunk;
        off += chunk;
    }
    wl_wr_be32(z + p, (b << 16) | a);
    p += 4;
    free(img);
    size_t total = 8 + 25 + 12 + p + 12;
    uint8_t *o = malloc(total);
    if (o == NULL) {
        free(z);
        return WL_ERR_NOMEM;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(o, sig, 8);
    size_t pos = 8;
    uint8_t ihdr[13];
    wl_wr_be32(ihdr, w);
    wl_wr_be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    put_chunk(o, &pos, "IHDR", ihdr, 13);
    put_chunk(o, &pos, "IDAT", z, p);
    put_chunk(o, &pos, "IEND", NULL, 0);
    free(z);
    *out = o;
    *len = pos;
    return WL_OK;
}
