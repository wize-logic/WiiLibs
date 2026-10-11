// SPDX-License-Identifier: MIT
// Cells to palettised sprite sheets and stacked C4 cell textures (policy in README.md).
#include <stdlib.h>
#include <string.h>

#include "wl/gxtex/gxtex.h"

#define MAX_PAL 16u

static uint32_t up8(uint32_t v) {
    return (v + 7u) & ~7u;
}

/* Exact palettisation of RGBA frames (alpha 0 or 255): index 0 is transparent, the other colours
 * are numbered in order of first appearance. WL_ERR_LIMIT when there are more than 15. */
static WlResult palettize(const uint8_t *rgba, size_t npx, uint8_t *idx, uint8_t pal[MAX_PAL][4],
                          uint32_t *ncol) {
    uint32_t n = 1;
    memset(pal, 0, MAX_PAL * 4u);
    for (size_t i = 0; i < npx; i++) {
        const uint8_t *p = rgba + i * 4u;
        if (p[3] == 0) {
            idx[i] = 0;
            continue;
        }
        uint32_t k;
        for (k = 1; k < n; k++) {
            if (pal[k][0] == p[0] && pal[k][1] == p[1] && pal[k][2] == p[2]) {
                break;
            }
        }
        if (k == n) {
            if (n >= MAX_PAL) {
                return WL_ERR_LIMIT;
            }
            memcpy(pal[n], p, 3);
            pal[n][3] = 255;
            n++;
        }
        idx[i] = (uint8_t)k;
    }
    *ncol = n;
    return WL_OK;
}

/* Lossy fallback: the 15 most frequent RGB555 colours, every other colour to the nearest of them. */
static WlResult quantize(const uint8_t *rgba, size_t npx, uint8_t *idx, uint8_t pal[MAX_PAL][4], uint32_t *ncol) {
    uint32_t *count = calloc(32768u, sizeof *count);
    if (count == NULL) {
        return WL_ERR_NOMEM;
    }
    for (size_t i = 0; i < npx; i++) {
        const uint8_t *p = rgba + i * 4u;
        if (p[3] != 0) {
            count[((unsigned)p[0] >> 3) | (((unsigned)p[1] >> 3) << 5) | (((unsigned)p[2] >> 3) << 10)]++;
        }
    }
    uint32_t key[MAX_PAL];
    uint32_t n = 1;
    memset(pal, 0, MAX_PAL * 4u);
    while (n < MAX_PAL) {
        uint32_t best = 0, bc = 0;
        for (uint32_t k = 0; k < 32768u; k++) {
            if (count[k] > bc) {
                bc = count[k];
                best = k;
            }
        }
        if (bc == 0) {
            break;
        }
        key[n] = best;
        count[best] = 0;
        pal[n][0] = (uint8_t)(((best & 31u) << 3) | ((best & 31u) >> 2));
        pal[n][1] = (uint8_t)((((best >> 5) & 31u) << 3) | (((best >> 5) & 31u) >> 2));
        pal[n][2] = (uint8_t)((((best >> 10) & 31u) << 3) | (((best >> 10) & 31u) >> 2));
        pal[n][3] = 255;
        n++;
    }
    for (size_t i = 0; i < npx; i++) {
        const uint8_t *p = rgba + i * 4u;
        if (p[3] == 0) {
            idx[i] = 0;
            continue;
        }
        uint32_t bk = 1, bd = 0xFFFFFFFFu;
        for (uint32_t k = 1; k < n; k++) {
            int dr = (int)p[0] - pal[k][0], dg = (int)p[1] - pal[k][1], db = (int)p[2] - pal[k][2];
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < bd) {
                bd = d;
                bk = k;
            }
        }
        idx[i] = (uint8_t)bk;
    }
    (void)key;
    *ncol = n;
    free(count);
    return WL_OK;
}

/* Union bounding box over all cells; empty cells are skipped. */
static WlResult union_bounds(const WlNcer *c, int *x0, int *y0, int *x1, int *y1) {
    int have = 0;
    for (uint32_t i = 0; i < c->ncells; i++) {
        int a, b, d, e;
        if (wl_ds_cell_bounds(c, i, &a, &b, &d, &e) != WL_OK) {
            continue;
        }
        if (!have) {
            *x0 = a;
            *y0 = b;
            *x1 = d;
            *y1 = e;
            have = 1;
        } else {
            *x0 = a < *x0 ? a : *x0;
            *y0 = b < *y0 ? b : *y0;
            *x1 = d > *x1 ? d : *x1;
            *y1 = e > *y1 ? e : *y1;
        }
    }
    return have ? WL_OK : WL_ERR_RANGE;
}

/* Renders every cell into frames[ncells][fw*fh*4]. */
static WlResult render_all(const WlNcgr *g, const WlNclr *p, const WlNcer *c, int pal, int ox,
                           int oy, uint32_t fw, uint32_t fh, uint8_t *rgba) {
    for (uint32_t i = 0; i < c->ncells; i++) {
        WlResult r = wl_ds_render_cell(g, p, c, i, pal, ox, oy, rgba + (size_t)i * fw * fh * 4u, fw, fh);
        if (r != WL_OK) {
            return r;
        }
    }
    return WL_OK;
}

static uint8_t ticks_of(uint32_t d, uint32_t div) {
    uint32_t t = (d + div / 2u) / div;
    return (uint8_t)(t < 1u ? 1u : (t > 255u ? 255u : t));
}

void wl_ds_sprite_free(WlDsSprite *s) {
    if (s != NULL) {
        free(s->frames);
        free(s->anims);
        free(s->hold);
        memset(s, 0, sizeof *s);
    }
}

WlResult wl_ds_sprite_build(const WlNcgr *g, const WlNclr *p, const WlNcer *c, const WlNanr *a,
                            int pal, uint32_t tick_div, WlDsSprite *out) {
    if (g == NULL || p == NULL || c == NULL || out == NULL || tick_div == 0) {
        return WL_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    if (c->ncells > 200u) {
        return WL_ERR_LIMIT;
    }
    int x0, y0, x1, y1;
    WlResult r = union_bounds(c, &x0, &y0, &x1, &y1);
    if (r != WL_OK) {
        return r;
    }
    uint32_t fw = up8((uint32_t)(x1 - x0)), fh = up8((uint32_t)(y1 - y0));
    if (fw > 512u || fh > 512u) {
        return WL_ERR_LIMIT;
    }
    size_t fpx = (size_t)fw * fh;
    uint8_t *rgba = calloc(fpx * c->ncells, 4);
    uint8_t *idx = malloc(fpx * c->ncells);
    uint8_t *frames = NULL; /* final index frames */
    WlSpriteAnim *anim = NULL;
    uint8_t *hold = NULL;
    uint16_t *map = NULL;
    if (rgba == NULL || idx == NULL) {
        r = WL_ERR_NOMEM;
        goto done;
    }
    r = render_all(g, p, c, pal, x0, y0, fw, fh, rgba);
    if (r != WL_OK) {
        goto done;
    }
    uint8_t cpal[MAX_PAL][4];
    uint32_t ncol;
    r = palettize(rgba, fpx * c->ncells, idx, cpal, &ncol);
    int lossy = 0;
    if (r == WL_ERR_LIMIT) {
        r = quantize(rgba, fpx * c->ncells, idx, cpal, &ncol);
        lossy = 1;
    }
    if (r != WL_OK) {
        goto done;
    }
    /* Stored frames: the cells in order, then copies for sequences that cannot use them. */
    uint32_t cap = 256u, nstore = c->ncells, nanim = 0;
    map = malloc(cap * sizeof *map);
    hold = calloc(cap, 1);
    anim = calloc(a != NULL && a->nseqs ? a->nseqs : 1u, sizeof *anim);
    if (map == NULL || hold == NULL || anim == NULL) {
        r = WL_ERR_NOMEM;
        goto done;
    }
    for (uint32_t i = 0; i < c->ncells; i++) {
        map[i] = (uint16_t)i;
    }
    int any_hold = 0;
    for (uint32_t s = 0; a != NULL && s < a->nseqs; s++) {
        const WlNanrSeq *q = &a->seqs[s];
        if (q->count == 0) {
            continue;
        }
        int rev = q->play_mode == 3 || q->play_mode == 4;
        int loop = q->play_mode == 2 || q->play_mode == 4;
        uint32_t cells[256], dur[256];
        if (q->count > 255u) {
            r = WL_ERR_LIMIT;
            goto done;
        }
        for (uint32_t k = 0; k < q->count; k++) {
            const WlNanrFrame *f = &a->frames[q->first + (rev ? q->count - 1u - k : k)];
            if (f->cell >= c->ncells) {
                r = WL_ERR_RANGE;
                goto done;
            }
            cells[k] = f->cell;
            dur[k] = f->duration;
        }
        uint8_t first;
        if (q->count == 1) {
            first = (uint8_t)cells[0];
        } else {
            int fits = 1;
            for (uint32_t k = 0; k < q->count && fits; k++) {
                fits = cells[k] == cells[0] + k && (hold[cells[0] + k] == 0 || hold[cells[0] + k] == ticks_of(dur[k], tick_div));
            }
            if (fits) {
                for (uint32_t k = 0; k < q->count; k++) {
                    hold[cells[0] + k] = ticks_of(dur[k], tick_div);
                }
                first = (uint8_t)cells[0];
            } else {
                if (nstore + q->count > 255u) {
                    r = WL_ERR_LIMIT;
                    goto done;
                }
                first = (uint8_t)nstore;
                for (uint32_t k = 0; k < q->count; k++) {
                    map[nstore] = (uint16_t)cells[k];
                    hold[nstore] = ticks_of(dur[k], tick_div);
                    nstore++;
                }
            }
            any_hold = 1;
        }
        anim[nanim].first = first;
        anim[nanim].count = (uint8_t)q->count;
        anim[nanim].ticks = ticks_of(dur[0], tick_div);
        anim[nanim].loop = loop ? 1u : 0u;
        nanim++;
    }
    frames = malloc(fpx * nstore);
    if (frames == NULL) {
        r = WL_ERR_NOMEM;
        goto done;
    }
    for (uint32_t f = 0; f < nstore; f++) {
        memcpy(frames + fpx * f, idx + fpx * map[f], fpx);
    }
    /* Hold tables cover the stored frames only. */
    if (any_hold) {
        uint8_t *h = realloc(hold, nstore);
        if (h == NULL) {
            r = WL_ERR_NOMEM;
            goto done;
        }
        hold = h;
    }
    out->fw = fw;
    out->fh = fh;
    out->nframes = nstore;
    out->frames = frames;
    memcpy(out->pal, cpal, sizeof cpal);
    out->ncol = ncol;
    out->anims = anim;
    out->nanims = nanim;
    out->hold = any_hold ? hold : NULL;
    out->ncells = c->ncells;
    out->lossy = lossy;
    frames = NULL;
    anim = NULL;
    if (any_hold) {
        hold = NULL;
    }
done:
    free(rgba);
    free(idx);
    free(frames);
    free(anim);
    free(hold);
    free(map);
    return r;
}

WlResult wl_ds_cells_tex(const WlNcgr *g, const WlNclr *p, const WlNcer *c, int pal, WlGxTex *out) {
    if (g == NULL || p == NULL || c == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    int x0, y0, x1, y1;
    WlResult r = union_bounds(c, &x0, &y0, &x1, &y1);
    if (r != WL_OK) {
        return r;
    }
    uint32_t fw = up8((uint32_t)(x1 - x0)), fh = up8((uint32_t)(y1 - y0));
    if (fw > 256u || fh * c->ncells > 2048u) {
        return WL_ERR_LIMIT;
    }
    size_t fpx = (size_t)fw * fh;
    uint8_t *rgba = calloc(fpx * c->ncells, 4);
    uint8_t *idx = malloc(fpx * c->ncells);
    if (rgba == NULL || idx == NULL) {
        free(rgba);
        free(idx);
        return WL_ERR_NOMEM;
    }
    uint8_t cpal[MAX_PAL][4];
    uint32_t ncol;
    r = render_all(g, p, c, pal, x0, y0, fw, fh, rgba);
    if (r == WL_OK) {
        r = palettize(rgba, fpx * c->ncells, idx, cpal, &ncol);
    }
    if (r == WL_OK) {
        /* Frames are stored one after the other, which is the same as one tall image. */
        r = wl_gx_tex_from_indexed(idx, fw, fh * c->ncells, 4, (const uint8_t(*)[4])cpal, ncol, out);
    }
    free(rgba);
    free(idx);
    return r;
}
