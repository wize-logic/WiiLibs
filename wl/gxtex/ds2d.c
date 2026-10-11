// SPDX-License-Identifier: MIT
// NCLR, NCGR, NCER and NANR readers and the cell renderer. Layouts are summarised in README.md; the
// pret decompilation projects' tools/nitrogfx was read as documentation.
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/gxtex/gxtex.h"

#define MAX_CELLS 4096u
#define MAX_OAMS 65536u
#define MAX_SEQS 4096u
#define MAX_FRAMES 65536u

void wl_image_free(WlImage *im) {
    if (im != NULL) {
        free(im->rgba);
        im->rgba = NULL;
        im->w = im->h = 0;
    }
}

/* Container: magic (as stored, e.g. "RGCN"), BOM 0xFFFE, size, header size, block count; then
 * blocks of {tag, size}. Returns the block starting with `tag` (including its 8-byte header). */
static WlResult find_block(const uint8_t *d, size_t n, const char *magic, const char *tag,
                           const uint8_t **b, size_t *bn) {
    if (d == NULL || n < 0x18 || memcmp(d, magic, 4) != 0 || (wl_rd_le16(d + 4) != 0xFEFFu && wl_rd_le16(d + 4) != 0u)) {
        return WL_ERR_FORMAT;
    }
    size_t fsize = wl_rd_le32(d + 8);
    size_t hs = wl_rd_le16(d + 12);
    unsigned nb = wl_rd_le16(d + 14);
    if (hs < 16 || fsize > n || fsize < hs || nb == 0 || nb > 16) {
        return WL_ERR_FORMAT;
    }
    size_t pos = hs;
    for (unsigned i = 0; i < nb; i++) {
        if (!wl_range_ok(pos, 8, fsize)) {
            return WL_ERR_FORMAT;
        }
        size_t sz = wl_rd_le32(d + pos + 4);
        if (sz < 8 || !wl_range_ok(pos, sz, fsize)) {
            return WL_ERR_FORMAT;
        }
        if (memcmp(d + pos, tag, 4) == 0) {
            *b = d + pos;
            *bn = sz;
            return WL_OK;
        }
        pos += sz;
    }
    return WL_ERR_FORMAT;
}

/* ---- NCLR ---- */
WlResult wl_nclr_open(WlNclr *p, const uint8_t *d, size_t n) {
    if (p == NULL) {
        return WL_ERR_ARG;
    }
    memset(p, 0, sizeof *p);
    const uint8_t *b;
    size_t bn;
    WlResult r = find_block(d, n, "RLCN", "TTLP", &b, &bn);
    if (r != WL_OK) {
        return r;
    }
    if (bn < 0x18) {
        return WL_ERR_FORMAT;
    }
    uint32_t depth = wl_rd_le16(b + 8); /* the upper half carries a colour count in some files */
    size_t dsz = wl_rd_le32(b + 0x10);
    size_t off = wl_rd_le32(b + 0x14);
    if ((depth != 3 && depth != 4) || off > bn - 8 || 8 + off > bn) {
        return WL_ERR_FORMAT;
    }
    size_t avail = bn - 8 - off;
    if (dsz > avail || dsz == 0) {
        dsz = avail; /* some files declare more than the block holds, or nothing */
    }
    p->col = b + 8 + off;
    p->count = (uint32_t)(dsz / 2);
    p->bpp = depth == 3 ? 4u : 8u;
    return p->count > 0 ? WL_OK : WL_ERR_FORMAT;
}

void wl_nclr_rgba(const WlNclr *p, uint32_t i, uint8_t out[4]) {
    uint32_t v = i < p->count ? wl_rd_le16(p->col + 2 * (size_t)i) : 0u;
    unsigned r = v & 31u, g = (v >> 5) & 31u, b = (v >> 10) & 31u;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 3) | (g >> 2));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255;
}

/* ---- NCGR ---- */
WlResult wl_ncgr_open(WlNcgr *g, const uint8_t *d, size_t n) {
    if (g == NULL) {
        return WL_ERR_ARG;
    }
    memset(g, 0, sizeof *g);
    const uint8_t *b;
    size_t bn;
    WlResult r = find_block(d, n, "RGCN", "RAHC", &b, &bn);
    if (r != WL_OK) {
        return r;
    }
    if (bn < 0x20) {
        return WL_ERR_FORMAT;
    }
    uint32_t th = wl_rd_le16(b + 8), tw = wl_rd_le16(b + 10);
    uint32_t depth = wl_rd_le16(b + 12);
    size_t dsz = wl_rd_le32(b + 0x18);
    size_t doff = wl_rd_le32(b + 0x1C);
    /* One old file declares a few bytes more than its block holds; the file itself has them. */
    if ((depth != 3 && depth != 4) || doff > bn - 8 || !wl_range_ok((uint64_t)(b - d) + 8 + doff, dsz, n)) {
        return WL_ERR_FORMAT;
    }
    g->bpp = depth == 3 ? 4u : 8u;
    g->tiles_h = th == 0xFFFFu ? 0u : th;
    g->tiles_w = tw == 0xFFFFu ? 0u : tw;
    g->scan = (b[0x14] != 0);
    g->pix = b + 8 + doff;
    g->size = (uint32_t)dsz;
    return WL_OK;
}

/* ---- NCER ---- */
static const uint8_t k_obj_w[4][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {0, 0, 0, 0}};
static const uint8_t k_obj_h[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {0, 0, 0, 0}};

void wl_ncer_free(WlNcer *c) {
    if (c != NULL) {
        free(c->cells);
        free(c->oams);
        memset(c, 0, sizeof *c);
    }
}

WlResult wl_ncer_open(WlNcer *c, const uint8_t *d, size_t n) {
    if (c == NULL) {
        return WL_ERR_ARG;
    }
    memset(c, 0, sizeof *c);
    const uint8_t *b;
    size_t bn;
    WlResult r = find_block(d, n, "RECN", "KBEC", &b, &bn);
    if (r != WL_OK) {
        return r;
    }
    if (bn < 0x20) {
        return WL_ERR_FORMAT;
    }
    uint32_t nc = wl_rd_le16(b + 8);
    uint32_t attr = wl_rd_le16(b + 10);
    size_t coff = wl_rd_le32(b + 12);
    uint32_t mapping = wl_rd_le32(b + 0x10);
    size_t vtoff = wl_rd_le32(b + 0x14);
    size_t csz = (attr & 1u) ? 16u : 8u;
    if (nc == 0 || nc > MAX_CELLS || mapping > 255u || 8 + coff > bn ||
        !wl_range_ok(8 + coff, (size_t)nc * csz, bn)) {
        return WL_ERR_FORMAT;
    }
    size_t oam_base = 8 + coff + (size_t)nc * csz;
    c->cells = calloc(nc, sizeof *c->cells);
    if (c->cells == NULL) {
        return WL_ERR_NOMEM;
    }
    c->ncells = nc;
    c->mapping = mapping;
    /* First pass: total object count and range checks. */
    uint32_t total = 0;
    for (uint32_t i = 0; i < nc; i++) {
        const uint8_t *e = b + 8 + coff + (size_t)i * csz;
        uint32_t no = wl_rd_le16(e);
        size_t ooff = wl_rd_le32(e + 4);
        if (!wl_range_ok(oam_base, ooff, bn) || !wl_range_ok(oam_base + ooff, (size_t)no * 6, bn)) {
            wl_ncer_free(c);
            return WL_ERR_FORMAT;
        }
        c->cells[i].first = total;
        c->cells[i].count = no;
        c->cells[i].flip_h = (uint8_t)((wl_rd_le16(e + 2) >> 8) & 1u);
        c->cells[i].flip_v = (uint8_t)((wl_rd_le16(e + 2) >> 9) & 1u);
        total += no;
        if (total > MAX_OAMS) {
            wl_ncer_free(c);
            return WL_ERR_RANGE;
        }
    }
    if (vtoff != 0) { /* VRAM transfer table: u32 max size, u32 offset of the entries, {u32 src, u32 size} per cell */
        if (!wl_range_ok(8 + vtoff, 8, bn)) {
            wl_ncer_free(c);
            return WL_ERR_FORMAT;
        }
        size_t eoff = 8 + vtoff + wl_rd_le32(b + 8 + vtoff + 4);
        if (!wl_range_ok(eoff, (size_t)nc * 8u, bn)) {
            wl_ncer_free(c);
            return WL_ERR_FORMAT;
        }
        for (uint32_t i = 0; i < nc; i++) {
            c->cells[i].has_vt = 1;
            c->cells[i].vt_off = wl_rd_le32(b + eoff + 8u * i);
            c->cells[i].vt_size = wl_rd_le32(b + eoff + 8u * i + 4);
        }
    }
    c->oams = calloc(total ? total : 1u, sizeof *c->oams);
    if (c->oams == NULL) {
        wl_ncer_free(c);
        return WL_ERR_NOMEM;
    }
    c->noams = total;
    for (uint32_t i = 0; i < nc; i++) {
        const uint8_t *e = b + 8 + coff + (size_t)i * csz;
        size_t ooff = wl_rd_le32(e + 4);
        for (uint32_t k = 0; k < c->cells[i].count; k++) {
            const uint8_t *o = b + oam_base + ooff + (size_t)k * 6;
            unsigned a0 = wl_rd_le16(o), a1 = wl_rd_le16(o + 2), a2 = wl_rd_le16(o + 4);
            WlOam *m = &c->oams[c->cells[i].first + k];
            unsigned shape = (a0 >> 14) & 3u, size = (a1 >> 14) & 3u;
            if (shape == 3) {
                wl_ncer_free(c);
                return WL_ERR_FORMAT;
            }
            m->y = (int16_t)(int8_t)(a0 & 0xFFu);
            int x = (int)(a1 & 0x1FFu);
            m->x = (int16_t)(x >= 256 ? x - 512 : x);
            m->w = k_obj_w[shape][size];
            m->h = k_obj_h[shape][size];
            m->xform = (uint8_t)((a0 >> 8) & 1u);
            unsigned mode = (a0 >> 10) & 3u;
            m->hidden = (uint8_t)((!m->xform && ((a0 >> 9) & 1u)) || mode == 2u);
            m->color256 = (uint8_t)((a0 >> 13) & 1u);
            if (!m->xform) {
                m->flip_h = (uint8_t)((a1 >> 12) & 1u);
                m->flip_v = (uint8_t)((a1 >> 13) & 1u);
            }
            m->tile = (uint16_t)(a2 & 0x3FFu);
            m->pal = (uint8_t)(a2 >> 12);
        }
    }
    return WL_OK;
}

/* ---- NANR ---- */
void wl_nanr_free(WlNanr *a) {
    if (a != NULL) {
        free(a->seqs);
        free(a->frames);
        memset(a, 0, sizeof *a);
    }
}

WlResult wl_nanr_open(WlNanr *a, const uint8_t *d, size_t n) {
    if (a == NULL) {
        return WL_ERR_ARG;
    }
    memset(a, 0, sizeof *a);
    const uint8_t *b;
    size_t bn;
    WlResult r = find_block(d, n, "RNAN", "KNBA", &b, &bn);
    if (r != WL_OK) {
        return r;
    }
    if (bn < 0x1C) {
        return WL_ERR_FORMAT;
    }
    uint32_t ns = wl_rd_le16(b + 8), nf = wl_rd_le16(b + 10);
    size_t so = wl_rd_le32(b + 12), fo = wl_rd_le32(b + 16), ro = wl_rd_le32(b + 20);
    if (ns == 0 || ns > MAX_SEQS || nf > MAX_FRAMES || 8 + so > bn || 8 + fo > bn || 8 + ro > bn ||
        !wl_range_ok(8 + so, (size_t)ns * 16, bn) || !wl_range_ok(8 + fo, (size_t)nf * 8, bn)) {
        return WL_ERR_FORMAT;
    }
    a->seqs = calloc(ns, sizeof *a->seqs);
    a->frames = calloc(nf ? nf : 1u, sizeof *a->frames);
    if (a->seqs == NULL || a->frames == NULL) {
        wl_nanr_free(a);
        return WL_ERR_NOMEM;
    }
    a->nseqs = ns;
    a->nframes = nf;
    size_t res_avail = bn - 8 - ro;
    for (uint32_t i = 0; i < nf; i++) {
        const uint8_t *e = b + 8 + fo + (size_t)i * 8;
        size_t roff = wl_rd_le32(e);
        if (!wl_range_ok(roff, 2, res_avail)) {
            wl_nanr_free(a);
            return WL_ERR_FORMAT;
        }
        a->frames[i].cell = wl_rd_le16(b + 8 + ro + roff);
        a->frames[i].duration = wl_rd_le16(e + 4);
    }
    for (uint32_t i = 0; i < ns; i++) {
        const uint8_t *e = b + 8 + so + (size_t)i * 16;
        uint32_t cnt = wl_rd_le16(e);
        uint32_t type = wl_rd_le16(e + 6);
        size_t foff = wl_rd_le32(e + 12);
        if (type != 1u || foff % 8 != 0 || foff / 8 > nf || cnt > nf - foff / 8) {
            wl_nanr_free(a);
            return type != 1u ? WL_ERR_UNSUPPORTED : WL_ERR_FORMAT;
        }
        a->seqs[i].first = (uint32_t)(foff / 8);
        a->seqs[i].count = cnt;
        a->seqs[i].loop_start = (uint16_t)wl_rd_le16(e + 2);
        a->seqs[i].element = (uint16_t)wl_rd_le16(e + 4);
        a->seqs[i].play_mode = (uint16_t)wl_rd_le32(e + 8);
    }
    return WL_OK;
}

/* ---- cells ---- */
/* The object as drawn: a mirrored cell mirrors every position about the cell origin and toggles
 * the object's own flip. */
static WlOam oam_eff(const WlCell *ce, const WlOam *o) {
    WlOam e = *o;
    if (ce->flip_h) {
        e.x = (int16_t)(-(o->x + (int)o->w));
        e.flip_h = (uint8_t)!o->flip_h;
    }
    if (ce->flip_v) {
        e.y = (int16_t)(-(o->y + (int)o->h));
        e.flip_v = (uint8_t)!o->flip_v;
    }
    return e;
}

WlResult wl_ds_cell_bounds(const WlNcer *c, uint32_t cell, int *x0, int *y0, int *x1, int *y1) {
    if (c == NULL || cell >= c->ncells) {
        return WL_ERR_RANGE;
    }
    const WlCell *ce = &c->cells[cell];
    int have = 0, ax = 0, ay = 0, bx = 0, by = 0;
    for (uint32_t k = 0; k < ce->count; k++) {
        WlOam oe = oam_eff(ce, &c->oams[ce->first + k]);
        const WlOam *o = &oe;
        if (o->hidden) {
            continue;
        }
        int ex = o->x + o->w, ey = o->y + o->h;
        if (!have) {
            ax = o->x;
            ay = o->y;
            bx = ex;
            by = ey;
            have = 1;
        } else {
            ax = o->x < ax ? o->x : ax;
            ay = o->y < ay ? o->y : ay;
            bx = ex > bx ? ex : bx;
            by = ey > by ? ey : by;
        }
    }
    if (!have) {
        return WL_ERR_RANGE;
    }
    *x0 = ax;
    *y0 = ay;
    *x1 = bx;
    *y1 = by;
    return WL_OK;
}

WlResult wl_ds_render_cell(const WlNcgr *g, const WlNclr *p, const WlNcer *c, uint32_t cell,
                           int pal, int ox, int oy, uint8_t *rgba, uint32_t w, uint32_t h) {
    if (g == NULL || p == NULL || c == NULL || rgba == NULL || cell >= c->ncells) {
        return WL_ERR_ARG;
    }
    if (g->scan || c->mapping > 3u) {
        return WL_ERR_UNSUPPORTED;
    }
    size_t unit = (size_t)32u << c->mapping;
    const WlCell *ce = &c->cells[cell];
    size_t pbase = 0, plimit = g->size;
    if (ce->has_vt) {
        if (ce->vt_off > g->size || ce->vt_size > g->size - ce->vt_off) {
            return WL_ERR_RANGE;
        }
        pbase = ce->vt_off;
        plimit = ce->vt_size;
    }
    for (uint32_t k = ce->count; k-- > 0;) {
        WlOam oe = oam_eff(ce, &c->oams[ce->first + k]);
        const WlOam *o = &oe;
        if (o->hidden) {
            continue;
        }
        if (o->xform) {
            return WL_ERR_UNSUPPORTED;
        }
        int bpp = o->color256 ? 8 : 4;
        size_t tbytes = bpp == 4 ? 32u : 64u;
        size_t tw = o->w / 8u;
        size_t base = (size_t)o->tile * unit;
        uint32_t bank = pal >= 0 ? (uint32_t)pal : o->pal;
        if ((bpp == 4 && g->bpp != 4) || (bpp == 8 && g->bpp != 8)) {
            return WL_ERR_UNSUPPORTED;
        }
        for (unsigned py = 0; py < o->h; py++) {
            unsigned sy = o->flip_v ? (unsigned)o->h - 1u - py : py;
            for (unsigned px = 0; px < o->w; px++) {
                unsigned sx = o->flip_h ? (unsigned)o->w - 1u - px : px;
                size_t toff = base + ((sy / 8u) * tw + sx / 8u) * tbytes;
                size_t within = (sy % 8u) * (bpp == 4 ? 4u : 8u) + (sx % 8u) / (bpp == 4 ? 2u : 1u);
                if (!wl_range_ok(toff + within, 1, plimit)) {
                    return WL_ERR_RANGE;
                }
                unsigned v = g->pix[pbase + toff + within];
                if (bpp == 4) {
                    v = (sx % 2u) ? (v >> 4) : (v & 15u);
                }
                if (v == 0) {
                    continue;
                }
                int cx = o->x - ox + (int)px, cy = o->y - oy + (int)py;
                if (cx < 0 || cy < 0 || (uint32_t)cx >= w || (uint32_t)cy >= h) {
                    continue;
                }
                uint32_t ci = bpp == 4 ? bank * 16u + v : v;
                if (ci >= p->count) {
                    return WL_ERR_RANGE;
                }
                wl_nclr_rgba(p, ci, rgba + ((size_t)cy * w + (uint32_t)cx) * 4u);
            }
        }
    }
    return WL_OK;
}
