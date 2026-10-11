// SPDX-License-Identifier: MIT
// TEX0 (NSBTX / the texture block of NSBMD) reader, exact decoders for the seven DS texture
// formats and the conversion to GX formats. Format notes in README.md; the pret decompilation
// projects' tools/nitrobtx was read as documentation.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/gxtex/gxtex.h"

#define NAME_LEN 16u
#define MAX_ENTRIES 1024u

enum { F_A3I5 = 1, F_PAL4 = 2, F_PAL16 = 3, F_PAL256 = 4, F_COMP = 5, F_A5I3 = 6, F_DIRECT = 7 };

/* Dictionary: u8 revision, u8 count, u16 size, ...; at +6 the offset of the data section:
 * u16 unit size, u16 name offset, entries, names. Returns the entry array offset and unit. */
static WlResult dict_open(const uint8_t *d, size_t n, size_t base, uint32_t *count, size_t *ents,
                          size_t *names, uint32_t want_unit) {
    if (!wl_range_ok(base, 8, n)) {
        return WL_ERR_FORMAT;
    }
    uint32_t cnt = d[base + 1];
    size_t data = base + wl_rd_le16(d + base + 6);
    if (cnt > MAX_ENTRIES || !wl_range_ok(data, 4, n)) {
        return WL_ERR_FORMAT;
    }
    uint32_t unit = wl_rd_le16(d + data);
    size_t nofs = wl_rd_le16(d + data + 2);
    if (unit != want_unit || !wl_range_ok(data + 4, (size_t)cnt * unit, n) ||
        !wl_range_ok(data + nofs, (size_t)cnt * NAME_LEN, n)) {
        return WL_ERR_FORMAT;
    }
    *count = cnt;
    *ents = data + 4;
    *names = data + nofs;
    return WL_OK;
}

WlResult wl_tex0_open(WlTex0 *t, const uint8_t *d, size_t n) {
    if (t == NULL || d == NULL) {
        return WL_ERR_ARG;
    }
    memset(t, 0, sizeof *t);
    size_t base;
    if (n >= 8 && memcmp(d, "TEX0", 4) == 0) {
        base = 0;
    } else {
        if (n < 0x14 || (memcmp(d, "BTX0", 4) != 0 && memcmp(d, "BMD0", 4) != 0) ||
            wl_rd_le16(d + 4) != 0xFEFFu) {
            return WL_ERR_FORMAT;
        }
        unsigned nb = wl_rd_le16(d + 0xE);
        size_t hs = wl_rd_le16(d + 0xC);
        if (hs < 0x10 || nb == 0 || nb > 8 || !wl_range_ok(0x10, 4u * nb, n)) {
            return WL_ERR_FORMAT;
        }
        base = (size_t)-1;
        for (unsigned i = 0; i < nb; i++) {
            size_t off = wl_rd_le32(d + 0x10 + 4u * i);
            if (wl_range_ok(off, 8, n) && memcmp(d + off, "TEX0", 4) == 0) {
                base = off;
                break;
            }
        }
        if (base == (size_t)-1) {
            return WL_ERR_NOT_FOUND;
        }
    }
    size_t bsz = wl_rd_le32(d + base + 4);
    if (bsz < 0x3C || !wl_range_ok(base, bsz, n)) {
        return WL_ERR_FORMAT;
    }
    t->d = d;
    t->n = n;
    t->base = base;
    t->tex_size = (size_t)wl_rd_le16(d + base + 0x0C) << 3;
    t->tex_data = base + wl_rd_le32(d + base + 0x14);
    t->comp_size = (size_t)wl_rd_le16(d + base + 0x1C) << 3;
    t->comp_data = base + wl_rd_le32(d + base + 0x24);
    t->comp_idx = base + wl_rd_le32(d + base + 0x28);
    t->pal_size = (size_t)wl_rd_le16(d + base + 0x30) << 3;
    t->pal_data = base + wl_rd_le32(d + base + 0x38);
    t->tex_dict = base + wl_rd_le16(d + base + 0x0E);
    t->pal_dict = base + wl_rd_le32(d + base + 0x34);
    if (!wl_range_ok(t->tex_data, t->tex_size, n) || !wl_range_ok(t->comp_data, t->comp_size, n) ||
        !wl_range_ok(t->comp_idx, t->comp_size / 2u, n) || !wl_range_ok(t->pal_data, t->pal_size, n)) {
        return WL_ERR_FORMAT;
    }
    size_t e, nm;
    WlResult r = dict_open(d, n, t->tex_dict, &t->ntex, &e, &nm, 8);
    if (r == WL_OK) {
        r = dict_open(d, n, t->pal_dict, &t->npal, &e, &nm, 4);
    }
    return r;
}

static size_t tex_entry(const WlTex0 *t, uint32_t i) {
    size_t data = t->tex_dict + wl_rd_le16(t->d + t->tex_dict + 6);
    return data + 4 + (size_t)i * 8u;
}
static size_t tex_name(const WlTex0 *t, uint32_t i) {
    size_t data = t->tex_dict + wl_rd_le16(t->d + t->tex_dict + 6);
    return data + wl_rd_le16(t->d + data + 2) + (size_t)i * NAME_LEN;
}
static size_t pal_entry(const WlTex0 *t, uint32_t i) {
    size_t data = t->pal_dict + wl_rd_le16(t->d + t->pal_dict + 6);
    return data + 4 + (size_t)i * 4u;
}
static size_t pal_name(const WlTex0 *t, uint32_t i) {
    size_t data = t->pal_dict + wl_rd_le16(t->d + t->pal_dict + 6);
    return data + wl_rd_le16(t->d + data + 2) + (size_t)i * NAME_LEN;
}

WlResult wl_tex0_info(const WlTex0 *t, uint32_t i, WlTexInfo *info) {
    if (t == NULL || info == NULL || i >= t->ntex) {
        return WL_ERR_ARG;
    }
    uint32_t param = wl_rd_le32(t->d + tex_entry(t, i));
    WlTexInfo x;
    x.off = (param & 0xFFFFu) << 3;
    x.w = 8u << ((param >> 20) & 7u);
    x.h = 8u << ((param >> 23) & 7u);
    x.fmt = (param >> 26) & 7u;
    x.transp = (int)((param >> 29) & 1u);
    static const uint8_t bits[8] = {0, 8, 2, 4, 8, 2, 8, 16};
    if (x.fmt == 0 || x.w > 1024u || x.h > 1024u) {
        return WL_ERR_FORMAT;
    }
    size_t len = (size_t)x.w * x.h * bits[x.fmt] / 8u;
    if (x.fmt == F_COMP) {
        if (!wl_range_ok(t->comp_data + x.off, len, t->n) ||
            !wl_range_ok(t->comp_idx + x.off / 2u, (size_t)x.w * x.h / 8u, t->n)) {
            return WL_ERR_FORMAT;
        }
    } else if (x.off + len > t->tex_size) {
        return WL_ERR_FORMAT;
    }
    *info = x;
    return WL_OK;
}

WlResult wl_tex0_palette(const WlTex0 *t, uint32_t i, const uint8_t **col, uint32_t *count) {
    if (t == NULL || i >= t->npal) {
        return WL_ERR_ARG;
    }
    size_t start = (size_t)wl_rd_le16(t->d + pal_entry(t, i)) << 3;
    size_t end = t->pal_size;
    for (uint32_t k = 0; k < t->npal; k++) {
        size_t s = (size_t)wl_rd_le16(t->d + pal_entry(t, k)) << 3;
        if (s > start && s < end) {
            end = s;
        }
    }
    if (start > t->pal_size) {
        return WL_ERR_FORMAT;
    }
    *col = t->d + t->pal_data + start;
    *count = (uint32_t)((end - start) / 2u);
    return WL_OK;
}

static int name_eq(const WlTex0 *t, size_t a, size_t b, const char *suffix) {
    char x[NAME_LEN + 4], y[NAME_LEN + 4];
    memset(x, 0, sizeof x);
    memset(y, 0, sizeof y);
    memcpy(x, t->d + a, NAME_LEN);
    memcpy(y, t->d + b, NAME_LEN);
    size_t lx = 0;
    while (lx < NAME_LEN && x[lx] != 0) {
        lx++;
    }
    if (suffix != NULL) {
        size_t ls = strlen(suffix);
        if (lx + ls > NAME_LEN) {
            return 0;
        }
        memcpy(x + lx, suffix, ls);
        x[lx + ls] = 0;
    } else {
        x[lx] = 0;
    }
    y[NAME_LEN] = 0;
    return strcmp(x, y) == 0;
}

WlResult wl_tex0_pair_palette(const WlTex0 *t, uint32_t i, uint32_t *pal, int *how) {
    if (t == NULL || i >= t->ntex || pal == NULL) {
        return WL_ERR_ARG;
    }
    if (t->npal == 0) {
        return WL_ERR_NOT_FOUND;
    }
    for (uint32_t k = 0; k < t->npal; k++) {
        if (name_eq(t, tex_name(t, i), pal_name(t, k), "_pl")) {
            *pal = k;
            if (how) {
                *how = 0;
            }
            return WL_OK;
        }
    }
    for (uint32_t k = 0; k < t->npal; k++) {
        if (name_eq(t, tex_name(t, i), pal_name(t, k), NULL)) {
            *pal = k;
            if (how) {
                *how = 1;
            }
            return WL_OK;
        }
    }
    if (t->npal == t->ntex) {
        *pal = i;
        if (how) {
            *how = 2;
        }
        return WL_OK;
    }
    *pal = 0;
    if (how) {
        *how = 3;
    }
    return WL_OK;
}

static void bgr555(uint32_t v, uint8_t out[4], int opaque) {
    unsigned r = v & 31u, g = (v >> 5) & 31u, b = (v >> 10) & 31u;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 3) | (g >> 2));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = opaque ? 255u : 0u;
}

typedef struct {
    const uint8_t *col; /* palette bytes */
    uint32_t count;
} Pal;

static uint32_t pal_at(const Pal *p, uint32_t i) {
    return i < p->count ? wl_rd_le16(p->col + 2u * i) : 0u;
}

static WlResult load_pal(const WlTex0 *t, uint32_t pal, Pal *p) {
    if (t->npal == 0) {
        p->col = (const uint8_t *)"";
        p->count = 0;
        return WL_OK;
    }
    return wl_tex0_palette(t, pal, &p->col, &p->count);
}

/* Palette indices of a paletted texture (formats 1..4, 6) into idx[w*h] and alpha[w*h]. */
static void unpack_indices(const WlTex0 *t, const WlTexInfo *x, uint8_t *idx, uint8_t *alpha) {
    const uint8_t *s = t->d + t->tex_data + x->off;
    size_t npx = (size_t)x->w * x->h;
    for (size_t k = 0; k < npx; k++) {
        unsigned v;
        uint8_t a = 255;
        switch (x->fmt) {
        case F_A3I5: {
            unsigned b = s[k];
            v = b & 31u;
            unsigned a3 = b >> 5;
            a = (uint8_t)((a3 << 5) | (a3 << 2) | (a3 >> 1));
            break;
        }
        case F_PAL4:
            v = (s[k / 4u] >> (2u * (k % 4u))) & 3u;
            break;
        case F_PAL16:
            v = (s[k / 2u] >> (4u * (k % 2u))) & 15u;
            break;
        case F_PAL256:
            v = s[k];
            break;
        default: { /* A5I3 */
            unsigned b = s[k];
            v = b & 7u;
            unsigned a5 = b >> 3;
            a = (uint8_t)((a5 << 3) | (a5 >> 2));
            break;
        }
        }
        if (x->transp && v == 0 && (x->fmt == F_PAL4 || x->fmt == F_PAL16 || x->fmt == F_PAL256)) {
            a = 0;
        }
        idx[k] = (uint8_t)v;
        alpha[k] = a;
    }
}

static uint32_t chan_mix(uint32_t c0, uint32_t c1, uint32_t wa, uint32_t wb, uint32_t div) {
    uint32_t out = 0;
    for (unsigned s = 0; s < 15; s += 5) {
        uint32_t x = (c0 >> s) & 31u, y = (c1 >> s) & 31u;
        out |= ((x * wa + y * wb) / div) << s;
    }
    return out;
}

WlResult wl_tex0_decode(const WlTex0 *t, uint32_t i, uint32_t pal, WlImage *im) {
    WlTexInfo x;
    WlResult r = wl_tex0_info(t, i, &x);
    if (r != WL_OK) {
        return r;
    }
    if (im == NULL) {
        return WL_ERR_ARG;
    }
    Pal p = {NULL, 0};
    if (x.fmt != F_DIRECT && (r = load_pal(t, pal, &p)) != WL_OK) {
        return r;
    }
    size_t npx = (size_t)x.w * x.h;
    uint8_t *rgba = calloc(npx, 4);
    if (rgba == NULL) {
        return WL_ERR_NOMEM;
    }
    if (x.fmt == F_DIRECT) {
        const uint8_t *s = t->d + t->tex_data + x.off;
        for (size_t k = 0; k < npx; k++) {
            uint32_t v = wl_rd_le16(s + 2 * k);
            bgr555(v, rgba + 4 * k, (v & 0x8000u) != 0);
        }
    } else if (x.fmt == F_COMP) {
        const uint8_t *s = t->d + t->comp_data + x.off;
        const uint8_t *ix = t->d + t->comp_idx + x.off / 2u;
        uint32_t bw = x.w / 4u;
        for (uint32_t by = 0; by < x.h / 4u; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t bi = (size_t)by * bw + bx;
                uint32_t texels = wl_rd_le32(s + 4 * bi);
                uint32_t pidx = wl_rd_le16(ix + 2 * bi);
                uint32_t mode = pidx >> 14, b = (pidx & 0x3FFFu) * 2u;
                uint32_t c[4];
                int opaque[4] = {1, 1, 1, 1};
                c[0] = pal_at(&p, b);
                c[1] = pal_at(&p, b + 1);
                switch (mode) {
                case 0:
                    c[2] = pal_at(&p, b + 2);
                    c[3] = 0;
                    opaque[3] = 0;
                    break;
                case 1:
                    c[2] = chan_mix(c[0], c[1], 1, 1, 2);
                    c[3] = 0;
                    opaque[3] = 0;
                    break;
                case 2:
                    c[2] = pal_at(&p, b + 2);
                    c[3] = pal_at(&p, b + 3);
                    break;
                default:
                    c[2] = chan_mix(c[0], c[1], 5, 3, 8);
                    c[3] = chan_mix(c[0], c[1], 3, 5, 8);
                    break;
                }
                for (uint32_t k = 0; k < 16; k++) {
                    uint32_t v = (texels >> (2u * k)) & 3u;
                    size_t o = ((size_t)(by * 4u + k / 4u) * x.w + bx * 4u + k % 4u) * 4u;
                    bgr555(c[v], rgba + o, opaque[v]);
                }
            }
        }
    } else {
        uint8_t *idx = malloc(npx), *alpha = malloc(npx);
        if (idx == NULL || alpha == NULL) {
            free(idx);
            free(alpha);
            free(rgba);
            return WL_ERR_NOMEM;
        }
        unpack_indices(t, &x, idx, alpha);
        for (size_t k = 0; k < npx; k++) {
            bgr555(pal_at(&p, idx[k]), rgba + 4 * k, 1);
            rgba[4 * k + 3] = alpha[k];
        }
        free(idx);
        free(alpha);
    }
    im->w = x.w;
    im->h = x.h;
    im->rgba = rgba;
    return WL_OK;
}

/* The colour of a fully transparent texel is whatever the artist left under it (magenta, lime, yellow:
 * the DS never samples across texels, GX's bilinear filter does and pulls that colour into every cutout
 * edge: magenta rings round windows, lime strips on hedges). Each transparent texel takes the average
 * colour of the nearest opaque texels (up to 8 passes of the 8-neighbourhood); a texture with no opaque
 * texel is left alone. Only the converter's output changes: wl_tex0_decode stays exact. */
static void bleed_transparent(uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h;
    int any = 0, have = 0;
    for (size_t i = 0; i < n; i++) {
        any |= rgba[4 * i + 3] == 0;
        have |= rgba[4 * i + 3] != 0;
    }
    if (!any || !have) {
        return;
    }
    uint8_t *done = (uint8_t *)malloc(n), *next = (uint8_t *)malloc(n);
    uint8_t *out = (uint8_t *)malloc(n * 3u);
    if (done == NULL || next == NULL || out == NULL) {
        free(done);
        free(next);
        free(out);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        done[i] = rgba[4 * i + 3] != 0;
    }
    for (int pass = 0; pass < 8; pass++) {
        int changed = 0;
        memcpy(next, done, n);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                if (done[i]) {
                    continue;
                }
                uint32_t sum[3] = {0, 0, 0}, cnt = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = (int)x + dx, ny = (int)y + dy;
                        if (nx < 0 || ny < 0 || nx >= (int)w || ny >= (int)h) {
                            continue;
                        }
                        size_t j = (size_t)ny * w + (size_t)nx;
                        if (done[j]) {
                            if (rgba[4 * j + 3] != 0) {
                                sum[0] += rgba[4 * j], sum[1] += rgba[4 * j + 1], sum[2] += rgba[4 * j + 2];
                            } else {
                                sum[0] += out[3 * j], sum[1] += out[3 * j + 1], sum[2] += out[3 * j + 2];
                            }
                            cnt++;
                        }
                    }
                }
                if (cnt != 0) {
                    out[3 * i] = (uint8_t)(sum[0] / cnt);
                    out[3 * i + 1] = (uint8_t)(sum[1] / cnt);
                    out[3 * i + 2] = (uint8_t)(sum[2] / cnt);
                    next[i] = 1;
                    changed = 1;
                }
            }
        }
        memcpy(done, next, n);
        if (!changed) {
            break;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (rgba[4 * i + 3] == 0 && done[i]) {
            rgba[4 * i] = out[3 * i];
            rgba[4 * i + 1] = out[3 * i + 1];
            rgba[4 * i + 2] = out[3 * i + 2];
        }
    }
    free(done);
    free(next);
    free(out);
}

static int two_level_alpha(const uint8_t *rgba, size_t npx) {
    for (size_t k = 0; k < npx; k++) {
        if (rgba[4 * k + 3] != 0 && rgba[4 * k + 3] != 255) {
            return 0;
        }
    }
    return 1;
}

WlResult wl_tex0_to_gx(const WlTex0 *t, uint32_t i, uint32_t pal, const WlTex0Policy *pol,
                       WlGxTex *out) {
    WlTexInfo x;
    WlResult r = wl_tex0_info(t, i, &x);
    if (r != WL_OK) {
        return r;
    }
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    size_t npx = (size_t)x.w * x.h;
    uint32_t f;
    if (x.fmt == F_PAL4 || x.fmt == F_PAL16 || x.fmt == F_PAL256) {
        Pal p;
        if ((r = load_pal(t, pal, &p)) != WL_OK) {
            return r;
        }
        uint32_t ncol = x.fmt == F_PAL4 ? 4u : (x.fmt == F_PAL16 ? 16u : 256u);
        uint8_t (*pc)[4] = malloc((size_t)ncol * 4u);
        uint8_t *idx = malloc(npx), *alpha = malloc(npx);
        if (pc == NULL || idx == NULL || alpha == NULL) {
            free(pc);
            free(idx);
            free(alpha);
            return WL_ERR_NOMEM;
        }
        for (uint32_t k = 0; k < ncol; k++) {
            bgr555(pal_at(&p, k), pc[k], !(k == 0 && x.transp));
        }
        unpack_indices(t, &x, idx, alpha);
        if (x.transp) {
            /* the transparent entry takes the average colour of the opaque texels it borders */
            uint32_t sum[3] = {0, 0, 0}, cnt = 0;
            for (uint32_t y = 0; y < x.h; y++) {
                for (uint32_t xx = 0; xx < x.w; xx++) {
                    if (idx[(size_t)y * x.w + xx] != 0) {
                        continue;
                    }
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = (int)xx + dx, ny = (int)y + dy;
                            if (nx < 0 || ny < 0 || nx >= (int)x.w || ny >= (int)x.h) {
                                continue;
                            }
                            uint8_t v = idx[(size_t)ny * x.w + (size_t)nx];
                            if (v != 0) {
                                sum[0] += pc[v][0], sum[1] += pc[v][1], sum[2] += pc[v][2];
                                cnt++;
                            }
                        }
                    }
                }
            }
            if (cnt != 0) {
                pc[0][0] = (uint8_t)(sum[0] / cnt);
                pc[0][1] = (uint8_t)(sum[1] / cnt);
                pc[0][2] = (uint8_t)(sum[2] / cnt);
            }
        }
        r = wl_gx_tex_from_indexed(idx, x.w, x.h, x.fmt == F_PAL256 ? 8u : 4u, (const uint8_t(*)[4])pc, ncol, out);
        free(pc);
        free(idx);
        free(alpha);
    } else {
        WlImage im;
        r = wl_tex0_decode(t, i, pal, &im);
        if (r != WL_OK) {
            return r;
        }
        if (x.fmt == F_COMP) {
            f = (pol != NULL && pol->cmpr) ? WL_GX_TF_CMPR : WL_GX_TF_RGB5A3;
        } else if (x.fmt == F_DIRECT || two_level_alpha(im.rgba, npx)) {
            f = WL_GX_TF_RGB5A3;
        } else {
            f = WL_GX_TF_RGBA8;
        }
        bleed_transparent(im.rgba, x.w, x.h);
        r = wl_gx_tex_from_rgba(im.rgba, x.w, x.h, f, out);
        wl_image_free(&im);
    }
    return r;
}
