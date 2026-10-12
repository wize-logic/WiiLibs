// SPDX-License-Identifier: MIT
// The shared GX texture codec (gx_tex.h): layouts, decoders, colour quantisers and an integer CMPR
// block fit. The CMPR palette weights are the hardware's 5/8 : 3/8. Layouts are the public GX ones.
#include "wl/codec/gx_tex.h"

#include <string.h>

// Tile width, height and bytes per tile per format.
static const uint8_t k_tile[WL_GXC_FORMAT_COUNT][3] = {{8, 8, 32}, {8, 4, 32}, {8, 4, 32}, {4, 4, 32}, {4, 4, 32},
                                                       {4, 4, 32}, {4, 4, 64}, {8, 8, 32}, {8, 8, 32}, {8, 4, 32}};

size_t wl_gxc_tex_bytes(WlGxcFormat fmt, uint32_t w, uint32_t h) {
    if ((unsigned)fmt >= WL_GXC_FORMAT_COUNT || w == 0 || h == 0 || w > WL_GXC_MAX_DIM || h > WL_GXC_MAX_DIM) {
        return 0;
    }
    uint64_t tx = (w + k_tile[fmt][0] - 1u) / k_tile[fmt][0];
    uint64_t ty = (h + k_tile[fmt][1] - 1u) / k_tile[fmt][1];
    return (size_t)(tx * ty * k_tile[fmt][2]);
}

static uint32_t luma(const uint8_t *p) {
    return (p[0] * 77u + p[1] * 150u + p[2] * 29u + 128u) >> 8;
}

static uint32_t exp4(uint32_t v) {
    return (v << 4) | v;
}

static uint32_t exp3(uint32_t v) {
    return (v << 5) | (v << 2) | (v >> 1);
}

static uint32_t exp5(uint32_t v) {
    return (v << 3) | (v >> 2);
}

static uint32_t exp6(uint32_t v) {
    return (v << 2) | (v >> 4);
}

static void put_be16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint32_t get_be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

// Round-to-nearest quantiser from 8 bits to 0..max.
static uint32_t q(uint32_t v, uint32_t max) {
    return (v * max + 127u) / 255u;
}

uint16_t wl_gxc_rgb565_encode(const uint8_t p[4]) {
    return (uint16_t)((q(p[0], 31) << 11) | (q(p[1], 63) << 5) | q(p[2], 31));
}

void wl_gxc_rgb565_decode(uint32_t c, uint8_t o[4]) {
    o[0] = (uint8_t)exp5((c >> 11) & 31u);
    o[1] = (uint8_t)exp6((c >> 5) & 63u);
    o[2] = (uint8_t)exp5(c & 31u);
    o[3] = 255;
}

uint16_t wl_gxc_rgb5a3_encode(const uint8_t p[4]) {
    if (q(p[3], 7) == 7u) { // alpha that rounds to opaque keeps the better 555 colour
        return (uint16_t)(0x8000u | (q(p[0], 31) << 10) | (q(p[1], 31) << 5) | q(p[2], 31));
    }
    return (uint16_t)((q(p[3], 7) << 12) | (q(p[0], 15) << 8) | (q(p[1], 15) << 4) | q(p[2], 15));
}

void wl_gxc_rgb5a3_decode(uint32_t c, uint8_t o[4]) {
    if ((c & 0x8000u) != 0) {
        o[0] = (uint8_t)exp5((c >> 10) & 31u);
        o[1] = (uint8_t)exp5((c >> 5) & 31u);
        o[2] = (uint8_t)exp5(c & 31u);
        o[3] = 255;
    } else {
        o[0] = (uint8_t)exp4((c >> 8) & 15u);
        o[1] = (uint8_t)exp4((c >> 4) & 15u);
        o[2] = (uint8_t)exp4(c & 15u);
        o[3] = (uint8_t)exp3((c >> 12) & 7u);
    }
}

// ---- CMPR -------------------------------------------------------------------------------------

// The hardware's blend of two 8-bit endpoint values: weights 5/8 and 3/8 (not S3TC's 2/3 and 1/3).
static uint32_t blend(uint32_t a, uint32_t b) {
    return (a * 5u + b * 3u) >> 3;
}

void wl_gxc_cmpr_palette(uint32_t c0, uint32_t c1, uint8_t pal[4][4]) {
    wl_gxc_rgb565_decode(c0, pal[0]);
    wl_gxc_rgb565_decode(c1, pal[1]);
    if (c0 > c1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)blend(pal[0][k], pal[1][k]);
            pal[3][k] = (uint8_t)blend(pal[1][k], pal[0][k]);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) >> 1);
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
}

static uint32_t dist3(const uint8_t *a, const uint8_t *b) {
    int dr = (int)a[0] - (int)b[0];
    int dg = (int)a[1] - (int)b[1];
    int db = (int)a[2] - (int)b[2];
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

void wl_gxc_cmpr_encode_block(const uint8_t px[16][4], uint8_t out[8]) {
    uint32_t seen[16];
    uint32_t nd = 0;
    int clear = 0;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] < 128) {
            clear = 1;
            continue;
        }
        uint32_t c = wl_gxc_rgb565_encode(px[i]);
        int dup = 0;
        for (uint32_t k = 0; k < nd; k++) {
            dup |= seen[k] == c;
        }
        if (!dup) {
            seen[nd++] = c;
        }
    }
    uint32_t c0 = 0;
    uint32_t c1 = 0;
    if (nd == 1) {
        c0 = c1 = seen[0];
    } else if (nd > 1) {
        uint32_t best = 0;
        for (uint32_t a = 0; a < nd; a++) {
            for (uint32_t b = a + 1; b < nd; b++) {
                uint8_t ca[4];
                uint8_t cb[4];
                wl_gxc_rgb565_decode(seen[a], ca);
                wl_gxc_rgb565_decode(seen[b], cb);
                uint32_t d = dist3(ca, cb);
                if (d > best) {
                    best = d;
                    c0 = seen[a];
                    c1 = seen[b];
                }
            }
        }
    }
    // c0 <= c1 selects the transparent mode, c0 > c1 the four-colour mode.
    if (clear ? c0 > c1 : c0 < c1) {
        uint32_t t = c0;
        c0 = c1;
        c1 = t;
    }
    uint8_t pal[4][4];
    wl_gxc_cmpr_palette(c0, c1, pal);
    uint32_t levels = c0 > c1 ? 4u : 3u;
    put_be16(out, c0);
    put_be16(out + 2, c1);
    for (int y = 0; y < 4; y++) {
        uint32_t row = 0;
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = px[y * 4 + x];
            uint32_t best = 3;
            if (p[3] >= 128) {
                uint32_t bd = 0xFFFFFFFFu;
                for (uint32_t k = 0; k < levels; k++) {
                    uint32_t d = dist3(p, pal[k]);
                    if (d < bd) {
                        bd = d;
                        best = k;
                    }
                }
            }
            row |= best << (6 - 2 * x);
        }
        out[4 + y] = (uint8_t)row;
    }
}

void wl_gxc_cmpr_decode_block(const uint8_t in[8], uint8_t px[16][4]) {
    uint8_t pal[4][4];
    wl_gxc_cmpr_palette(get_be16(in), get_be16(in + 2), pal);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            memcpy(px[y * 4 + x], pal[(in[4 + y] >> (6 - 2 * x)) & 3u], 4);
        }
    }
}

// ---- generic tiling ---------------------------------------------------------------------------

static int fmt_ok(WlGxcFormat fmt, int ci) {
    return (unsigned)fmt < WL_GXC_FORMAT_COUNT && ((fmt == WL_GXC_C4 || fmt == WL_GXC_C8) == (ci != 0));
}

// The source pixel with the coordinates clamped into the image (tile padding repeats the edge).
static const uint8_t *px_at(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t x, uint32_t y) {
    x = x < w ? x : w - 1u;
    y = y < h ? y : h - 1u;
    return rgba + ((size_t)y * w + x) * 4u;
}

size_t wl_gxc_encode(WlGxcFormat fmt, const uint8_t *rgba, uint32_t w, uint32_t h, void *dst, size_t cap) {
    size_t total = fmt_ok(fmt, 0) ? wl_gxc_tex_bytes(fmt, w, h) : 0;
    if (total == 0 || rgba == NULL || dst == NULL || total > cap) {
        return 0;
    }
    memset(dst, 0, total);
    uint8_t *out = dst;
    uint32_t tw = k_tile[fmt][0];
    uint32_t th = k_tile[fmt][1];
    uint32_t tsz = k_tile[fmt][2];
    uint32_t tiles_x = (w + tw - 1u) / tw;
    uint32_t tiles_y = (h + th - 1u) / th;
    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint8_t *tile = out + ((size_t)ty * tiles_x + tx) * tsz;
            if (fmt == WL_GXC_CMPR) {
                for (uint32_t sub = 0; sub < 4; sub++) {
                    uint8_t px[16][4];
                    uint32_t bx = tx * 8u + (sub & 1u) * 4u;
                    uint32_t by = ty * 8u + (sub >> 1) * 4u;
                    for (uint32_t i = 0; i < 16; i++) {
                        memcpy(px[i], px_at(rgba, w, h, bx + (i & 3u), by + (i >> 2)), 4);
                    }
                    wl_gxc_cmpr_encode_block((const uint8_t(*)[4])px, tile + sub * 8u);
                }
                continue;
            }
            for (uint32_t y = 0; y < th; y++) {
                for (uint32_t x = 0; x < tw; x++) {
                    const uint8_t *p = px_at(rgba, w, h, tx * tw + x, ty * th + y);
                    uint32_t i = y * tw + x;
                    switch (fmt) {
                    case WL_GXC_I4: {
                        uint32_t v = (luma(p) + 8u) / 17u;
                        tile[i >> 1] = (uint8_t)(tile[i >> 1] | ((i & 1u) != 0 ? v : v << 4));
                        break;
                    }
                    case WL_GXC_I8:
                        tile[i] = (uint8_t)luma(p);
                        break;
                    case WL_GXC_IA4:
                        tile[i] = (uint8_t)((((p[3] + 8u) / 17u) << 4) | ((luma(p) + 8u) / 17u));
                        break;
                    case WL_GXC_IA8:
                        tile[i * 2] = p[3];
                        tile[i * 2 + 1] = (uint8_t)luma(p);
                        break;
                    case WL_GXC_RGB565:
                        put_be16(tile + i * 2, wl_gxc_rgb565_encode(p));
                        break;
                    case WL_GXC_RGB5A3:
                        put_be16(tile + i * 2, wl_gxc_rgb5a3_encode(p));
                        break;
                    default: // RGBA8: 16 AR pairs, then 16 GB pairs
                        tile[i * 2] = p[3];
                        tile[i * 2 + 1] = p[0];
                        tile[32 + i * 2] = p[1];
                        tile[32 + i * 2 + 1] = p[2];
                        break;
                    }
                }
            }
        }
    }
    return total;
}

size_t wl_gxc_encode_ci(WlGxcFormat fmt, const uint8_t *indices, uint32_t w, uint32_t h, void *dst, size_t cap) {
    // I4 values 0..15 pack exactly like C4 indices (same tile), so luminance-index data (wl/gxtex) shares this call.
    int is_i4 = fmt == WL_GXC_I4;
    size_t total = (fmt_ok(fmt, 1) || is_i4) ? wl_gxc_tex_bytes(fmt, w, h) : 0;
    if (total == 0 || indices == NULL || dst == NULL || total > cap) {
        return 0;
    }
    memset(dst, 0, total);
    uint8_t *out = dst;
    uint32_t tw = k_tile[fmt][0];
    uint32_t th = k_tile[fmt][1];
    uint32_t tsz = k_tile[fmt][2];
    uint32_t tiles_x = (w + tw - 1u) / tw;
    uint32_t tiles_y = (h + th - 1u) / th;
    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint8_t *tile = out + ((size_t)ty * tiles_x + tx) * tsz;
            for (uint32_t y = 0; y < th; y++) {
                for (uint32_t x = 0; x < tw; x++) {
                    uint32_t sx = tx * tw + x;
                    uint32_t sy = ty * th + y;
                    uint32_t v = (sx < w && sy < h) ? indices[(size_t)sy * w + sx] : 0u;
                    uint32_t i = y * tw + x;
                    if (fmt == WL_GXC_C8) {
                        tile[i] = (uint8_t)v;
                    } else {
                        v &= 15u;
                        tile[i >> 1] = (uint8_t)(tile[i >> 1] | ((i & 1u) != 0 ? v : v << 4));
                    }
                }
            }
        }
    }
    return total;
}

// One texel of a tile-ordered image (w wide, tiles_x tiles per row) as RGBA: the one place that knows
// where a texel sits and what it expands to, shared by the whole-image decoder and the texel read.
static void texel_rgba(WlGxcFormat fmt, const uint8_t *in, uint32_t tiles_x, uint32_t x, uint32_t y, uint8_t *o) {
    uint32_t tw = k_tile[fmt][0];
    uint32_t th = k_tile[fmt][1];
    const uint8_t *tile = in + ((size_t)(y / th) * tiles_x + (x / tw)) * k_tile[fmt][2];
    uint32_t lx = x % tw;
    uint32_t ly = y % th;
    uint32_t i = ly * tw + lx;
    switch (fmt) {
    case WL_GXC_I4: {
        uint32_t v = (i & 1u) != 0 ? tile[i >> 1] & 15u : tile[i >> 1] >> 4;
        o[0] = o[1] = o[2] = o[3] = (uint8_t)exp4(v);
        break;
    }
    case WL_GXC_I8:
        o[0] = o[1] = o[2] = o[3] = tile[i];
        break;
    case WL_GXC_IA4:
        o[0] = o[1] = o[2] = (uint8_t)exp4(tile[i] & 15u);
        o[3] = (uint8_t)exp4(tile[i] >> 4);
        break;
    case WL_GXC_IA8:
        o[0] = o[1] = o[2] = tile[i * 2 + 1];
        o[3] = tile[i * 2];
        break;
    case WL_GXC_RGB565:
        wl_gxc_rgb565_decode(get_be16(tile + i * 2), o);
        break;
    case WL_GXC_RGB5A3:
        wl_gxc_rgb5a3_decode(get_be16(tile + i * 2), o);
        break;
    case WL_GXC_RGBA8:
        o[3] = tile[i * 2];
        o[0] = tile[i * 2 + 1];
        o[1] = tile[32 + i * 2];
        o[2] = tile[32 + i * 2 + 1];
        break;
    default: { // CMPR
        const uint8_t *blk = tile + ((ly >> 2) * 2u + (lx >> 2)) * 8u;
        uint8_t pal[4][4];
        wl_gxc_cmpr_palette(get_be16(blk), get_be16(blk + 2), pal);
        uint32_t idx = (blk[4 + (ly & 3u)] >> (6 - 2 * (lx & 3u))) & 3u;
        memcpy(o, pal[idx], 4);
        break;
    }
    }
}

size_t wl_gxc_decode(WlGxcFormat fmt, const void *src, size_t src_len, uint32_t w, uint32_t h, uint8_t *rgba_out) {
    size_t total = fmt_ok(fmt, 0) ? wl_gxc_tex_bytes(fmt, w, h) : 0;
    if (total == 0 || src == NULL || rgba_out == NULL || src_len < total) {
        return 0;
    }
    uint32_t tiles_x = (w + k_tile[fmt][0] - 1u) / k_tile[fmt][0];
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            texel_rgba(fmt, src, tiles_x, x, y, rgba_out + ((size_t)y * w + x) * 4u);
        }
    }
    return total;
}

bool wl_gxc_texel_read(WlGxcFormat fmt, const void *src, size_t src_len, uint32_t w, uint32_t h, uint32_t x,
                       uint32_t y, uint8_t rgba[4]) {
    size_t total = fmt_ok(fmt, 0) ? wl_gxc_tex_bytes(fmt, w, h) : 0;
    if (total == 0 || src == NULL || rgba == NULL || src_len < total || x >= w || y >= h) {
        return false;
    }
    texel_rgba(fmt, src, (w + k_tile[fmt][0] - 1u) / k_tile[fmt][0], x, y, rgba);
    return true;
}

bool wl_gxc_texel_raw(WlGxcFormat fmt, const void *src, size_t src_len, uint32_t w, uint32_t h, uint32_t x,
                      uint32_t y, uint32_t *raw) {
    size_t total = (unsigned)fmt < WL_GXC_FORMAT_COUNT ? wl_gxc_tex_bytes(fmt, w, h) : 0;
    if (total == 0 || src == NULL || raw == NULL || src_len < total || x >= w || y >= h) {
        return false;
    }
    uint32_t tw = k_tile[fmt][0];
    uint32_t th = k_tile[fmt][1];
    uint32_t tiles_x = (w + tw - 1u) / tw;
    const uint8_t *tile = (const uint8_t *)src + ((size_t)(y / th) * tiles_x + (x / tw)) * k_tile[fmt][2];
    uint32_t lx = x % tw;
    uint32_t ly = y % th;
    uint32_t i = ly * tw + lx;
    switch (fmt) {
    case WL_GXC_I4:
    case WL_GXC_C4:
        *raw = (i & 1u) != 0 ? tile[i >> 1] & 15u : tile[i >> 1] >> 4;
        break;
    case WL_GXC_I8:
    case WL_GXC_IA4:
    case WL_GXC_C8:
        *raw = tile[i];
        break;
    case WL_GXC_IA8:
    case WL_GXC_RGB565:
    case WL_GXC_RGB5A3:
        *raw = get_be16(tile + i * 2);
        break;
    case WL_GXC_RGBA8:
        *raw = ((uint32_t)tile[i * 2] << 24) | ((uint32_t)tile[i * 2 + 1] << 16) | ((uint32_t)tile[32 + i * 2] << 8) |
               tile[32 + i * 2 + 1];
        break;
    default: { // CMPR: the texel's 2-bit palette index
        const uint8_t *blk = tile + ((ly >> 2) * 2u + (lx >> 2)) * 8u;
        *raw = (blk[4 + (ly & 3u)] >> (6 - 2 * (lx & 3u))) & 3u;
        break;
    }
    }
    return true;
}

size_t wl_gxc_decode_ci(WlGxcFormat fmt, const void *src, size_t src_len, uint32_t w, uint32_t h, uint8_t *indices_out) {
    size_t total = fmt_ok(fmt, 1) ? wl_gxc_tex_bytes(fmt, w, h) : 0;
    if (total == 0 || src == NULL || indices_out == NULL || src_len < total) {
        return 0;
    }
    const uint8_t *in = src;
    uint32_t tw = k_tile[fmt][0];
    uint32_t th = k_tile[fmt][1];
    uint32_t tsz = k_tile[fmt][2];
    uint32_t tiles_x = (w + tw - 1u) / tw;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *tile = in + ((size_t)(y / th) * tiles_x + (x / tw)) * tsz;
            uint32_t i = (y % th) * tw + (x % tw);
            indices_out[(size_t)y * w + x] =
                fmt == WL_GXC_C4 ? ((i & 1u) != 0 ? (uint8_t)(tile[i >> 1] & 15u) : (uint8_t)(tile[i >> 1] >> 4)) : tile[i];
        }
    }
    return total;
}

void wl_gxc_tlut_encode(WlGxcTlutFormat fmt, const uint8_t *rgba, uint32_t count, void *dst) {
    uint8_t *out = dst;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        if (fmt == WL_GXC_TLUT_IA8) {
            out[i * 2] = p[3];
            out[i * 2 + 1] = (uint8_t)luma(p);
        } else if (fmt == WL_GXC_TLUT_RGB565) {
            put_be16(out + i * 2, wl_gxc_rgb565_encode(p));
        } else {
            put_be16(out + i * 2, wl_gxc_rgb5a3_encode(p));
        }
    }
}

void wl_gxc_tlut_decode(WlGxcTlutFormat fmt, const void *src, uint32_t count, uint8_t *rgba_out) {
    const uint8_t *in = src;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *o = rgba_out + (size_t)i * 4u;
        if (fmt == WL_GXC_TLUT_IA8) {
            o[0] = o[1] = o[2] = in[i * 2 + 1];
            o[3] = in[i * 2];
        } else if (fmt == WL_GXC_TLUT_RGB565) {
            wl_gxc_rgb565_decode(get_be16(in + i * 2), o);
        } else {
            wl_gxc_rgb5a3_decode(get_be16(in + i * 2), o);
        }
    }
}
