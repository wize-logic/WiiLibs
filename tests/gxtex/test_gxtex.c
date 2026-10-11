// SPDX-License-Identifier: MIT
// GX texture encoding and DS graphics conversion: synthetic NCLR / NCGR / NCER / NANR / TEX0 files
// are built in code, converted to sprite sheets and GX textures, decoded and compared pixel for
// pixel; wl/gfx's independent GX decoder cross-checks the encoded blocks; malformed and damaged
// inputs must fail cleanly. No game data is used here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/gfx/tex_codec.h"
#include "wl/gxtex/gxtex.h"
#include "greatest.h"

// ---------------------------------------------------------------------------------------------
// builders (little-endian DS files)
// ---------------------------------------------------------------------------------------------

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static void bput(Buf *b, const void *d, size_t n) {
    if (n == 0) {
        return;
    }
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b8(Buf *b, unsigned v) {
    uint8_t x = (uint8_t)v;
    bput(b, &x, 1);
}
static void b16(Buf *b, unsigned v) {
    b8(b, v & 0xFFu);
    b8(b, v >> 8);
}
static void b32(Buf *b, uint32_t v) {
    b16(b, v & 0xFFFFu);
    b16(b, v >> 16);
}
static void bfree(Buf *b) {
    free(b->p);
    memset(b, 0, sizeof *b);
}
static void set32(Buf *b, size_t at, uint32_t v) {
    b->p[at] = (uint8_t)v;
    b->p[at + 1] = (uint8_t)(v >> 8);
    b->p[at + 2] = (uint8_t)(v >> 16);
    b->p[at + 3] = (uint8_t)(v >> 24);
}

// A DS container with one block: magic, BOM bytes FF FE, version 1, size, header size 16, blocks.
static void wrap(Buf *out, const char *magic, const char *tag, const Buf *body) {
    bput(out, magic, 4);
    b16(out, 0xFEFF);
    b16(out, 0x0100);
    b32(out, (uint32_t)(16 + 8 + body->n));
    b16(out, 16);
    b16(out, 1);
    bput(out, tag, 4);
    b32(out, (uint32_t)(8 + body->n));
    bput(out, body->p, body->n);
}

static void make_nclr(Buf *out, const uint16_t *col, unsigned n, unsigned depth) {
    Buf b = {0};
    b32(&b, depth);
    b32(&b, 0);
    b32(&b, n * 2u);
    b32(&b, 0x10);
    for (unsigned i = 0; i < n; i++) {
        b16(&b, col[i]);
    }
    wrap(out, "RLCN", "TTLP", &b);
    bfree(&b);
}

static void make_ncgr(Buf *out, const uint8_t *pix, unsigned n, unsigned depth, int scan) {
    Buf b = {0};
    b16(&b, 0xFFFF);
    b16(&b, 0xFFFF);
    b32(&b, depth);
    b32(&b, 0);
    b32(&b, scan ? 1u : 0u);
    b32(&b, n);
    b32(&b, 0x18);
    bput(&b, pix, n);
    wrap(out, "RGCN", "RAHC", &b);
    bfree(&b);
}

typedef struct {
    int x, y, size, shape, tile, pal, hflip, vflip, c256, hidden;
} Oam;

// cells[i] has noam[i] objects taken in order from oams; vt (may be NULL) is {src,size} per cell.
static void make_ncer(Buf *out, unsigned ncells, const unsigned *noam, const Oam *oams, unsigned mapping,
                      const uint32_t (*vt)[2]) {
    Buf b = {0};
    unsigned total = 0;
    for (unsigned i = 0; i < ncells; i++) {
        total += noam[i];
    }
    size_t cells_at = 0x18;
    size_t oam_at = cells_at + 16u * ncells;
    size_t vt_at = oam_at + 6u * total;
    b16(&b, ncells);
    b16(&b, 1);
    b32(&b, 0x18);
    b32(&b, mapping);
    b32(&b, vt != NULL ? (uint32_t)vt_at : 0u);
    b32(&b, 0);
    b32(&b, 0);
    unsigned first = 0;
    for (unsigned i = 0; i < ncells; i++) {
        b16(&b, noam[i]);
        b16(&b, 0);
        b32(&b, 6u * first);
        b16(&b, 0);
        b16(&b, 0);
        b16(&b, 0);
        b16(&b, 0);
        first += noam[i];
    }
    for (unsigned i = 0; i < total; i++) {
        const Oam *o = &oams[i];
        unsigned a0 = (unsigned)(o->y & 0xFF) | ((unsigned)o->c256 << 13) | ((unsigned)o->shape << 14) |
                      ((unsigned)o->hidden << 9);
        unsigned a1 = (unsigned)(o->x & 0x1FF) | ((unsigned)o->hflip << 12) | ((unsigned)o->vflip << 13) |
                      ((unsigned)o->size << 14);
        unsigned a2 = (unsigned)o->tile | ((unsigned)o->pal << 12);
        b16(&b, a0);
        b16(&b, a1);
        b16(&b, a2);
    }
    if (vt != NULL) {
        b32(&b, 0);
        b32(&b, 8);
        for (unsigned i = 0; i < ncells; i++) {
            b32(&b, vt[i][0]);
            b32(&b, vt[i][1]);
        }
    }
    wrap(out, "RECN", "KBEC", &b);
    bfree(&b);
}

// seq: {frame count, play mode}; frames: {cell, duration}.
static void make_nanr(Buf *out, unsigned nseq, const unsigned (*seq)[2], unsigned nframes,
                      const unsigned (*frames)[2]) {
    Buf b = {0};
    size_t so = 0x18, fo = so + 16u * nseq, ro = fo + 8u * nframes;
    b16(&b, nseq);
    b16(&b, nframes);
    b32(&b, (uint32_t)so);
    b32(&b, (uint32_t)fo);
    b32(&b, (uint32_t)ro);
    b32(&b, 0);
    b32(&b, 0);
    unsigned first = 0;
    for (unsigned i = 0; i < nseq; i++) {
        b16(&b, seq[i][0]);
        b16(&b, 0);
        b16(&b, 0);
        b16(&b, 1);
        b32(&b, seq[i][1]);
        b32(&b, 8u * first);
        first += seq[i][0];
    }
    for (unsigned i = 0; i < nframes; i++) {
        b32(&b, 2u * i);
        b16(&b, frames[i][1]);
        b16(&b, 0xBEEF);
    }
    for (unsigned i = 0; i < nframes; i++) {
        b16(&b, frames[i][0]);
    }
    wrap(out, "RNAN", "KNBA", &b);
    bfree(&b);
}

// Reference decode of an encoded texture with wl/gfx's GX decoder (an independent implementation).
static int ref_decode(const WlGxTex *t, uint8_t *rgba) {
    static const struct {
        uint32_t gx;
        WlTexFormat f;
    } map[] = {{WL_GX_TF_I4, WL_TEX_I4}, {WL_GX_TF_RGB565, WL_TEX_RGB565}, {WL_GX_TF_RGB5A3, WL_TEX_RGB5A3},
               {WL_GX_TF_RGBA8, WL_TEX_RGBA8}, {WL_GX_TF_CMPR, WL_TEX_CMPR}, {WL_GX_TF_C4, WL_TEX_C4},
               {WL_GX_TF_C8, WL_TEX_C8}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].gx != t->fmt) {
            continue;
        }
        if (wl_tex_bytes(map[i].f, t->w, t->h) != t->size) {
            return 0;
        }
        if (map[i].f == WL_TEX_C4 || map[i].f == WL_TEX_C8) {
            uint8_t pal[256 * 4];
            uint8_t *idx = malloc((size_t)t->w * t->h);
            if (idx == NULL || t->ntlut == 0) {
                free(idx);
                return 0;
            }
            wl_tlut_decode(WL_TLUT_RGB5A3, t->tlut, t->ntlut, pal);
            wl_tex_decode_ci(map[i].f, t->data, t->w, t->h, idx);
            for (size_t k = 0; k < (size_t)t->w * t->h; k++) {
                if (idx[k] >= t->ntlut) {
                    free(idx);
                    return 0;
                }
                memcpy(rgba + 4 * k, pal + 4u * idx[k], 4);
            }
            free(idx);
        } else {
            wl_tex_decode(map[i].f, t->data, t->w, t->h, rgba);
        }
        return 1;
    }
    return 0;
}

// The reference decode agrees with ours on every pixel: alpha exactly, colour of visible pixels
// within tol. Both modules decode through wl/codec now, so every caller asks for 0; this checks
// gxtex against gfx, not two independent decoders (gx_cmpr_decode_matches_hardware_palette and
// tests/codec cover the codec itself).
static int ref_agrees(const WlGxTex *t, const WlImage *im, int tol) {
    uint8_t *ref = malloc((size_t)t->w * t->h * 4u);
    int ok = ref != NULL && ref_decode(t, ref);
    for (size_t k = 0; ok && k < (size_t)t->w * t->h; k++) {
        const uint8_t *a = ref + 4 * k, *b = im->rgba + 4 * k;
        ok = a[3] == b[3];
        for (int c = 0; ok && a[3] != 0 && c < 3; c++) {
            int d = (int)a[c] - (int)b[c];
            ok = d >= -tol && d <= tol;
        }
    }
    free(ref);
    return ok;
}

static unsigned rgb555(unsigned r, unsigned g, unsigned b) {
    return r | (g << 5) | (b << 10);
}
static void expect_rgba(unsigned c555, uint8_t out[4]) {
    unsigned r = c555 & 31u, g = (c555 >> 5) & 31u, b = (c555 >> 10) & 31u;
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 3) | (g >> 2));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
    out[3] = 255;
}

// ---------------------------------------------------------------------------------------------
// NCLR, NCGR
// ---------------------------------------------------------------------------------------------

TEST nclr_parse(void) {
    uint16_t col[32];
    for (unsigned i = 0; i < 32; i++) {
        col[i] = (uint16_t)rgb555(i, 31 - i, (i * 7) & 31u);
    }
    Buf f = {0};
    make_nclr(&f, col, 32, 3);
    WlNclr p;
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, f.p, f.n));
    ASSERT_EQ(32u, p.count);
    ASSERT_EQ(4u, p.bpp == 4 ? 4u : 0u);
    for (unsigned i = 0; i < 32; i++) {
        uint8_t got[4], want[4];
        wl_nclr_rgba(&p, i, got);
        expect_rgba(col[i], want);
        ASSERT_EQ(0, memcmp(got, want, 4));
    }
    bfree(&f);
    // 8-bit depth, and a colour count in the high half of the depth field (seen in real files).
    make_nclr(&f, col, 32, 4u | (10u << 16));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, f.p, f.n));
    ASSERT_EQ(8u, p.bpp);
    // depth 7 is not a palette depth.
    bfree(&f);
    make_nclr(&f, col, 32, 7);
    ASSERT_EQ(WL_ERR_FORMAT, wl_nclr_open(&p, f.p, f.n));
    bfree(&f);
    PASS();
}

TEST ncgr_parse(void) {
    uint8_t pix[64];
    for (unsigned i = 0; i < 64; i++) {
        pix[i] = (uint8_t)i;
    }
    Buf f = {0};
    make_ncgr(&f, pix, 64, 3, 0);
    WlNcgr g;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, f.p, f.n));
    ASSERT_EQ(64u, g.size);
    ASSERT_EQ(4u, g.bpp);
    ASSERT_EQ(0, g.scan);
    ASSERT_EQ(0, memcmp(g.pix, pix, 64));
    // declared data size larger than the file
    set32(&f, 16 + 8 + 0x10, 4096);
    ASSERT_EQ(WL_ERR_FORMAT, wl_ncgr_open(&g, f.p, f.n));
    bfree(&f);
    PASS();
}

// ---------------------------------------------------------------------------------------------
// cells and sprites
// ---------------------------------------------------------------------------------------------

// A 16x16 object (size 1, square) from 4 tiles of 4bpp, colour index = tile number + 1, so the four
// quadrants are four colours. Tile data of tile t is filled with the nibble (t + 1).
static void quad_tiles(uint8_t *pix, unsigned ntiles, unsigned first_colour) {
    for (unsigned t = 0; t < ntiles; t++) {
        memset(pix + 32u * t, (int)(((first_colour + t) << 4) | (first_colour + t)), 32);
    }
}

TEST render_cell_quadrants_and_flips(void) {
    uint8_t pix[32 * 4];
    quad_tiles(pix, 4, 1);
    uint16_t col[16];
    memset(col, 0, sizeof col);
    for (unsigned i = 1; i < 16; i++) {
        col[i] = (uint16_t)rgb555(i * 2, 31 - i, i);
    }
    Buf fg = {0}, fp = {0}, fc = {0};
    make_ncgr(&fg, pix, sizeof pix, 3, 0);
    make_nclr(&fp, col, 16, 3);
    Oam o[2] = {{-8, -8, 1, 0, 0, 0, 0, 0, 0, 0}, {-8, -8, 1, 0, 0, 0, 1, 1, 0, 0}};
    unsigned noam[2] = {1, 1};
    make_ncer(&fc, 2, noam, o, 0, NULL);
    WlNcgr g;
    WlNclr p;
    WlNcer c;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, fg.p, fg.n));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, fp.p, fp.n));
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    ASSERT_EQ(2u, c.ncells);
    int x0, y0, x1, y1;
    ASSERT_EQ(WL_OK, wl_ds_cell_bounds(&c, 0, &x0, &y0, &x1, &y1));
    ASSERT_EQ(-8, x0);
    ASSERT_EQ(8, x1);
    uint8_t img[16 * 16 * 4];
    for (unsigned cell = 0; cell < 2; cell++) {
        memset(img, 0, sizeof img);
        ASSERT_EQ(WL_OK, wl_ds_render_cell(&g, &p, &c, cell, -1, -8, -8, img, 16, 16));
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) {
                unsigned sx = cell ? 15 - x : x, sy = cell ? 15 - y : y; // flipped copy mirrors both ways
                unsigned tile = (sy / 8) * 2 + sx / 8;
                uint8_t want[4];
                expect_rgba(col[tile + 1], want);
                ASSERT_EQ(0, memcmp(img + (y * 16 + x) * 4, want, 4));
            }
        }
    }
    // a bank override reads the same index from another bank
    uint16_t col32[32];
    memset(col32, 0, sizeof col32);
    memcpy(col32 + 16, col, sizeof col);
    col32[16 + 1] = (uint16_t)rgb555(1, 2, 3);
    Buf fp2 = {0};
    make_nclr(&fp2, col32, 32, 3);
    WlNclr p2;
    ASSERT_EQ(WL_OK, wl_nclr_open(&p2, fp2.p, fp2.n));
    memset(img, 0, sizeof img);
    ASSERT_EQ(WL_OK, wl_ds_render_cell(&g, &p2, &c, 0, 1, -8, -8, img, 16, 16));
    uint8_t want[4];
    expect_rgba(rgb555(1, 2, 3), want);
    ASSERT_EQ(0, memcmp(img, want, 4));
    wl_ncer_free(&c);
    bfree(&fg);
    bfree(&fp);
    bfree(&fp2);
    bfree(&fc);
    PASS();
}

TEST render_cell_rules(void) {
    uint8_t pix[32 * 4];
    quad_tiles(pix, 4, 1);
    uint16_t col[16];
    memset(col, 0x11, sizeof col);
    Buf fg = {0}, fp = {0}, fc = {0};
    make_ncgr(&fg, pix, sizeof pix, 3, 0);
    make_nclr(&fp, col, 16, 3);
    WlNcgr g;
    WlNclr p;
    WlNcer c;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, fg.p, fg.n));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, fp.p, fp.n));
    // a hidden object is skipped; a tile index past the data is an error
    Oam o[2] = {{0, 0, 1, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 1, 0, 40, 0, 0, 0, 0, 0}};
    unsigned noam[2] = {1, 1};
    make_ncer(&fc, 2, noam, o, 0, NULL);
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    uint8_t img[16 * 16 * 4];
    memset(img, 0, sizeof img);
    ASSERT_EQ(WL_OK, wl_ds_render_cell(&g, &p, &c, 0, -1, 0, 0, img, 16, 16));
    for (size_t i = 0; i < sizeof img; i++) {
        ASSERT_EQ(0, img[i]);
    }
    ASSERT_EQ(WL_ERR_RANGE, wl_ds_render_cell(&g, &p, &c, 1, -1, 0, 0, img, 16, 16));
    ASSERT_EQ(WL_ERR_ARG, wl_ds_render_cell(&g, &p, &c, 2, -1, 0, 0, img, 16, 16));
    wl_ncer_free(&c);
    bfree(&fc);
    // object shape 3 is invalid
    Oam bad = {0, 0, 1, 3, 0, 0, 0, 0, 0, 0};
    unsigned one = 1;
    make_ncer(&fc, 1, &one, &bad, 0, NULL);
    ASSERT_EQ(WL_ERR_FORMAT, wl_ncer_open(&c, fc.p, fc.n));
    bfree(&fc);
    // a rotation/scaling object is refused by the renderer
    Oam rot = {0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    make_ncer(&fc, 1, &one, &rot, 0, NULL);
    fc.p[16 + 8 + 0x28 + 1] = 0x01; // attr0 bit 8 of the first object
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_ds_render_cell(&g, &p, &c, 0, -1, 0, 0, img, 16, 16));
    wl_ncer_free(&c);
    bfree(&fc);
    bfree(&fg);
    bfree(&fp);
    PASS();
}

// Animated sprite: three cells with the same object list, different tile data per cell through the
// VRAM transfer table; a one-frame sequence and a three-frame sequence with different holds.
TEST sprite_vram_transfer_and_animation(void) {
    uint8_t pix[3 * 128]; // 3 cells x 4 tiles
    for (unsigned c = 0; c < 3; c++) {
        quad_tiles(pix + 128u * c, 4, 1 + c); // colours 1..4, 2..5, 3..6
    }
    uint16_t col[16];
    memset(col, 0, sizeof col);
    for (unsigned i = 1; i < 16; i++) {
        col[i] = (uint16_t)rgb555(i, 2 * i & 31u, 31 - i);
    }
    Oam o[3];
    for (int i = 0; i < 3; i++) {
        Oam t = {-8, -8, 1, 0, 0, 0, 0, 0, 0, 0};
        o[i] = t;
    }
    unsigned noam[3] = {1, 1, 1};
    uint32_t vt[3][2] = {{0, 128}, {128, 128}, {256, 128}};
    unsigned seq[2][2] = {{1, 2}, {3, 2}}; // loop both
    unsigned frames[4][2] = {{0, 4}, {0, 30}, {1, 4}, {2, 8}};
    Buf fg = {0}, fp = {0}, fc = {0}, fa = {0};
    make_ncgr(&fg, pix, sizeof pix, 3, 0);
    make_nclr(&fp, col, 16, 3);
    make_ncer(&fc, 3, noam, o, 0, vt);
    make_nanr(&fa, 2, seq, 4, frames);
    WlNcgr g;
    WlNclr p;
    WlNcer c;
    WlNanr a;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, fg.p, fg.n));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, fp.p, fp.n));
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    ASSERT_EQ(WL_OK, wl_nanr_open(&a, fa.p, fa.n));
    ASSERT_EQ(2u, a.nseqs);
    ASSERT_EQ(4u, a.nframes);
    ASSERT_EQ(1u, c.cells[1].has_vt);
    ASSERT_EQ(128u, c.cells[1].vt_off);
    WlDsSprite sp;
    ASSERT_EQ(WL_OK, wl_ds_sprite_build(&g, &p, &c, &a, -1, 2, &sp));
    ASSERT_EQ(3u, sp.ncells);
    ASSERT_EQ(3u, sp.nframes);
    ASSERT_EQ(0, sp.lossy);
    ASSERT_EQ(16u, sp.fw);
    ASSERT_EQ(16u, sp.fh);
    ASSERT_EQ(2u, sp.nanims);
    ASSERT(sp.hold != NULL);
    // Animation 0: the single frame, cell 0, 2 ticks (4 source ticks at 60 Hz = 2 at 30 Hz), loop.
    ASSERT_EQ(0, sp.anims[0].first);
    ASSERT_EQ(1, sp.anims[0].count);
    ASSERT_EQ(2, sp.anims[0].ticks);
    ASSERT_EQ(1, sp.anims[0].loop);
    // Animation 1: cells 0..2 in place, holds 15, 2, 4.
    ASSERT_EQ(0, sp.anims[1].first);
    ASSERT_EQ(3, sp.anims[1].count);
    ASSERT_EQ(15, sp.anims[1].ticks);
    ASSERT_EQ(15, sp.hold[0]);
    ASSERT_EQ(2, sp.hold[1]);
    ASSERT_EQ(4, sp.hold[2]);
    // Pixels: frame f quadrant q has colour index f + q + 1 of the source palette.
    for (unsigned f = 0; f < 3; f++) {
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) {
                unsigned q = (y / 8) * 2 + x / 8;
                uint8_t want[4];
                expect_rgba(col[f + q + 1], want);
                uint8_t v = sp.frames[(f * 16 + y) * 16 + x];
                ASSERT(v > 0 && v < sp.ncol);
                ASSERT_EQ(0, memcmp(sp.pal[v], want, 4));
            }
        }
    }
    // A frame goes to GX as a C4 texture with the sheet's palette.
    WlGxTex gt;
    ASSERT_EQ(WL_OK, wl_gx_tex_from_indexed(sp.frames + 256 * 2, 16, 16, 4, (const uint8_t(*)[4])sp.pal, sp.ncol, &gt));
    WlImage im;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &im));
    uint8_t want[4];
    expect_rgba(col[2 + 3 + 1], want);
    ASSERT_EQ(0, memcmp(im.rgba + (15 * 16 + 15) * 4, want, 4));
    ASSERT_EQ(1, ref_agrees(&gt, &im, 0));
    wl_image_free(&im);
    wl_gx_tex_free(&gt);
    // 60 Hz ticks are kept with tick_div 1.
    WlDsSprite s60;
    ASSERT_EQ(WL_OK, wl_ds_sprite_build(&g, &p, &c, &a, -1, 1, &s60));
    ASSERT_EQ(4, s60.anims[0].ticks);
    ASSERT_EQ(30, s60.hold[0]);
    ASSERT_EQ(8, s60.hold[2]);
    wl_ds_sprite_free(&s60);
    ASSERT_EQ(WL_ERR_ARG, wl_ds_sprite_build(&g, &p, &c, &a, -1, 0, &s60));
    wl_ds_sprite_free(&sp);
    wl_ncer_free(&c);
    wl_nanr_free(&a);
    bfree(&fg);
    bfree(&fp);
    bfree(&fc);
    bfree(&fa);
    PASS();
}

TEST sprite_conflicting_holds_duplicate_frames(void) {
    uint8_t pix[128];
    quad_tiles(pix, 4, 1);
    uint16_t col[16];
    for (unsigned i = 0; i < 16; i++) {
        col[i] = (uint16_t)rgb555(i, i, i);
    }
    Oam o[2] = {{-8, -8, 1, 0, 0, 0, 0, 0, 0, 0}, {-8, -8, 1, 0, 0, 0, 1, 0, 0, 0}};
    unsigned noam[2] = {1, 1};
    // two sequences over cells 0,1 with different durations: the second cannot reuse the holds
    unsigned seq[2][2] = {{2, 2}, {2, 2}};
    unsigned frames[4][2] = {{0, 4}, {1, 6}, {0, 8}, {1, 10}};
    Buf fg = {0}, fp = {0}, fc = {0}, fa = {0};
    make_ncgr(&fg, pix, sizeof pix, 3, 0);
    make_nclr(&fp, col, 16, 3);
    make_ncer(&fc, 2, noam, o, 0, NULL);
    make_nanr(&fa, 2, seq, 4, frames);
    WlNcgr g;
    WlNclr p;
    WlNcer c;
    WlNanr a;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, fg.p, fg.n));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, fp.p, fp.n));
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    ASSERT_EQ(WL_OK, wl_nanr_open(&a, fa.p, fa.n));
    WlDsSprite sp;
    ASSERT_EQ(WL_OK, wl_ds_sprite_build(&g, &p, &c, &a, -1, 2, &sp));
    ASSERT_EQ(2u, sp.ncells);
    ASSERT_EQ(4u, sp.nframes); // the second sequence got its own copies
    ASSERT_EQ(2u, sp.nanims);
    ASSERT_EQ(2, sp.anims[1].first); // second sequence starts at the copies
    ASSERT_EQ(0, memcmp(sp.frames + 2 * 256, sp.frames, 2 * 256)); // copies of cells 0 and 1
    ASSERT(sp.hold != NULL);
    ASSERT_EQ(2, sp.hold[0]);
    ASSERT_EQ(3, sp.hold[1]);
    ASSERT_EQ(4, sp.hold[2]);
    ASSERT_EQ(5, sp.hold[3]);
    wl_ds_sprite_free(&sp);
    wl_ncer_free(&c);
    wl_nanr_free(&a);
    bfree(&fg);
    bfree(&fp);
    bfree(&fc);
    bfree(&fa);
    PASS();
}

TEST sprite_more_than_15_colours_is_reduced(void) {
    // Two objects with different banks, 16 distinct colours each: 30 colours in total.
    uint8_t pix[64];
    for (unsigned t = 0; t < 2; t++) {
        for (unsigned i = 0; i < 32; i++) {
            pix[32 * t + i] = (uint8_t)((((i * 2 + 1) & 15u) << 4) | ((i * 2) & 15u));
        }
    }
    uint16_t col[32];
    for (unsigned i = 0; i < 32; i++) {
        col[i] = (uint16_t)rgb555(i, 31 - i, (i * 5) & 31u);
    }
    Oam o[2] = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {8, 0, 0, 0, 1, 1, 0, 0, 0, 0}};
    unsigned noam = 2;
    Buf fg = {0}, fp = {0}, fc = {0};
    make_ncgr(&fg, pix, sizeof pix, 3, 0);
    make_nclr(&fp, col, 32, 3);
    make_ncer(&fc, 1, &noam, o, 0, NULL);
    WlNcgr g;
    WlNclr p;
    WlNcer c;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, fg.p, fg.n));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, fp.p, fp.n));
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    WlDsSprite sp;
    ASSERT_EQ(WL_OK, wl_ds_sprite_build(&g, &p, &c, NULL, -1, 2, &sp));
    ASSERT_EQ(1, sp.lossy);
    ASSERT_EQ(16u, sp.ncol);
    ASSERT_EQ(0u, sp.nanims);
    ASSERT(sp.hold == NULL);
    for (size_t i = 0; i < (size_t)sp.fw * sp.fh * sp.nframes; i++) {
        ASSERT(sp.frames[i] < 16);
    }
    // The stacked-icon path stays exact and refuses instead.
    WlGxTex gt;
    ASSERT_EQ(WL_ERR_LIMIT, wl_ds_cells_tex(&g, &p, &c, -1, &gt));
    wl_ds_sprite_free(&sp);
    wl_ncer_free(&c);
    bfree(&fg);
    bfree(&fp);
    bfree(&fc);
    PASS();
}

TEST cells_tex_stacks_frames(void) {
    uint8_t pix[256]; // two 32x... no: two 16x16 frames of 4 tiles each at tile 0 and 4
    quad_tiles(pix, 4, 1);
    quad_tiles(pix + 128, 4, 5);
    uint16_t col[16];
    for (unsigned i = 0; i < 16; i++) {
        col[i] = (uint16_t)rgb555(i, 31 - i, 7);
    }
    Oam o[2] = {{-8, -8, 1, 0, 0, 0, 0, 0, 0, 0}, {-8, -8, 1, 0, 4, 0, 0, 0, 0, 0}};
    unsigned noam[2] = {1, 1};
    Buf fg = {0}, fp = {0}, fc = {0};
    make_ncgr(&fg, pix, sizeof pix, 3, 0);
    make_nclr(&fp, col, 16, 3);
    make_ncer(&fc, 2, noam, o, 0, NULL);
    WlNcgr g;
    WlNclr p;
    WlNcer c;
    ASSERT_EQ(WL_OK, wl_ncgr_open(&g, fg.p, fg.n));
    ASSERT_EQ(WL_OK, wl_nclr_open(&p, fp.p, fp.n));
    ASSERT_EQ(WL_OK, wl_ncer_open(&c, fc.p, fc.n));
    WlGxTex gt;
    ASSERT_EQ(WL_OK, wl_ds_cells_tex(&g, &p, &c, -1, &gt));
    ASSERT_EQ(WL_GX_TF_C4, (int)gt.fmt);
    ASSERT_EQ(16u, gt.w);
    ASSERT_EQ(32u, gt.h);
    ASSERT_EQ(wl_gx_tex_size(WL_GX_TF_C4, 16, 32), gt.size);
    WlImage im;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &im));
    for (unsigned y = 0; y < 32; y++) {
        for (unsigned x = 0; x < 16; x++) {
            unsigned frame = y / 16, q = ((y % 16) / 8) * 2 + x / 8;
            uint8_t want[4];
            expect_rgba(col[1 + 4 * frame + q], want);
            ASSERT_EQ(0, memcmp(im.rgba + (y * 16 + x) * 4, want, 4));
        }
    }
    ASSERT_EQ(1, ref_agrees(&gt, &im, 0)); // the wl/gfx decoder agrees
    wl_image_free(&im);
    wl_gx_tex_free(&gt);
    wl_ncer_free(&c);
    bfree(&fg);
    bfree(&fp);
    bfree(&fc);
    PASS();
}

// ---------------------------------------------------------------------------------------------
// GX encoders
// ---------------------------------------------------------------------------------------------

static void fill_pattern(uint8_t *rgba, unsigned w, unsigned h, unsigned two_level) {
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            uint8_t *p = rgba + ((size_t)y * w + x) * 4u;
            unsigned r = (x * 7 + y * 3) & 31u, g = (x * 5 + y * 11) & 31u, b = (x * 13 + y) & 31u;
            p[0] = (uint8_t)((r << 3) | (r >> 2));
            p[1] = (uint8_t)((g << 3) | (g >> 2));
            p[2] = (uint8_t)((b << 3) | (b >> 2));
            p[3] = (two_level && ((x + 2 * y) % 5 == 0)) ? 0 : 255;
        }
    }
}

static int same_pixels(const uint8_t *a, const uint8_t *b, size_t npx) {
    for (size_t i = 0; i < npx; i++) {
        if (a[4 * i + 3] != b[4 * i + 3]) {
            return 0;
        }
        if (a[4 * i + 3] != 0 && memcmp(a + 4 * i, b + 4 * i, 3) != 0) {
            return 0;
        }
    }
    return 1;
}

TEST gx_rgb5a3_and_rgba8_exact(void) {
    enum { W = 24, H = 16 };
    uint8_t src[W * H * 4];
    fill_pattern(src, W, H, 1);
    WlGxTex out;
    WlImage im;
    ASSERT_EQ(WL_OK, wl_gx_tex_from_rgba(src, W, H, WL_GX_TF_RGB5A3, &out));
    ASSERT_EQ(wl_gx_tex_size(WL_GX_TF_RGB5A3, W, H), out.size);
    ASSERT_EQ(0u, out.ntlut);
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&out, &im));
    ASSERT_EQ(1, same_pixels(src, im.rgba, W * H));
    ASSERT_EQ(1, ref_agrees(&out, &im, 0));
    wl_image_free(&im);
    wl_gx_tex_free(&out);
    // RGBA8 keeps everything, including translucent alpha and the colour under alpha 0.
    for (unsigned i = 0; i < W * H; i++) {
        src[4 * i + 3] = (uint8_t)(i * 37u);
    }
    ASSERT_EQ(WL_OK, wl_gx_tex_from_rgba(src, W, H, WL_GX_TF_RGBA8, &out));
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&out, &im));
    ASSERT_EQ(0, memcmp(src, im.rgba, sizeof src));
    ASSERT_EQ(1, ref_agrees(&out, &im, 0));
    wl_image_free(&im);
    wl_gx_tex_free(&out);
    // RGB565 drops alpha; values on the 565 grid are exact.
    fill_pattern(src, W, H, 0);
    for (unsigned i = 0; i < W * H; i++) {
        src[4 * i + 1] = (uint8_t)((src[4 * i + 1] & 0xFCu) | (src[4 * i + 1] >> 6));
    }
    ASSERT_EQ(WL_OK, wl_gx_tex_from_rgba(src, W, H, WL_GX_TF_RGB565, &out));
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&out, &im));
    ASSERT_EQ(0, memcmp(src, im.rgba, sizeof src));
    ASSERT_EQ(1, ref_agrees(&out, &im, 0));
    wl_image_free(&im);
    wl_gx_tex_free(&out);
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_gx_tex_from_rgba(src, W, H, WL_GX_TF_I8, &out));
    ASSERT_EQ(WL_ERR_ARG, wl_gx_tex_from_rgba(src, 0, H, WL_GX_TF_RGBA8, &out));
    PASS();
}

TEST gx_indexed_c4_c8(void) {
    enum { W = 20, H = 9 }; // not multiples of the block size: edges are padded
    uint8_t idx[W * H];
    uint8_t pal[256][4];
    for (unsigned i = 0; i < 256; i++) {
        pal[i][0] = (uint8_t)((i & 31u) << 3 | (i & 31u) >> 2);
        pal[i][1] = (uint8_t)(255 - i);
        pal[i][2] = (uint8_t)(i / 2u * 2u);
        pal[i][3] = i == 0 ? 0 : 255;
        uint8_t q[4] = {pal[i][0], pal[i][1], pal[i][2], pal[i][3]};
        uint16_t v = wl_gx_rgb5a3(q);
        wl_gx_rgb5a3_to_rgba(v, pal[i]); // make the entry exactly representable
    }
    for (unsigned bpp = 4; bpp <= 8; bpp += 4) {
        unsigned ncol = bpp == 4 ? 16u : 200u;
        for (unsigned i = 0; i < W * H; i++) {
            idx[i] = (uint8_t)((i * 7u + 3u) % ncol);
        }
        WlGxTex out;
        ASSERT_EQ(WL_OK, wl_gx_tex_from_indexed(idx, W, H, bpp, (const uint8_t(*)[4])pal, ncol, &out));
        ASSERT_EQ(0u, out.size % 32u);
        ASSERT_EQ(ncol, out.ntlut);
        ASSERT_EQ(WL_GX_TL_RGB5A3, (int)out.tlut_fmt);
        ASSERT_EQ(bpp == 4 ? WL_GX_TF_C4 : WL_GX_TF_C8, (int)out.fmt);
        WlImage im;
        ASSERT_EQ(WL_OK, wl_gx_tex_decode(&out, &im));
        ASSERT_EQ((uint32_t)W, im.w);
        for (unsigned i = 0; i < W * H; i++) {
            ASSERT_EQ(0, memcmp(im.rgba + 4 * i, pal[idx[i]], 4));
        }
        ASSERT_EQ(1, ref_agrees(&out, &im, 0));
        wl_image_free(&im);
        wl_gx_tex_free(&out);
    }
    // index outside the palette
    idx[0] = 16;
    WlGxTex out;
    ASSERT_EQ(WL_ERR_RANGE, wl_gx_tex_from_indexed(idx, W, H, 4, (const uint8_t(*)[4])pal, 16, &out));
    // I4 from 4-bit intensities.
    uint8_t lum[W * H];
    for (unsigned i = 0; i < W * H; i++) {
        lum[i] = (uint8_t)(i % 16u);
    }
    ASSERT_EQ(WL_OK, wl_gx_tex_from_i4(lum, W, H, &out));
    ASSERT_EQ(WL_GX_TF_I4, (int)out.fmt);
    WlImage im;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&out, &im));
    for (unsigned i = 0; i < W * H; i++) {
        ASSERT_EQ(lum[i] * 17u, im.rgba[4 * i]);
        ASSERT_EQ(lum[i] * 17u, im.rgba[4 * i + 3]);
    }
    ASSERT_EQ(1, ref_agrees(&out, &im, 0));
    wl_image_free(&im);
    wl_gx_tex_free(&out);
    lum[3] = 16;
    ASSERT_EQ(WL_ERR_RANGE, wl_gx_tex_from_i4(lum, W, H, &out));
    PASS();
}

// Independent of wl/codec: an 8x8 CMPR texture of four identical 4x4 blocks (4-colour mode,
// c0 > c1, every row using indices 0,1,2,3) decodes to the hardware palette computed here from
// first principles: RGB565 widened by bit replication, then c2 = (5 c0 + 3 c1) / 8 and
// c3 = (3 c0 + 5 c1) / 8.
TEST gx_cmpr_decode_matches_hardware_palette(void) {
    const uint16_t c0 = 0xF81F, c1 = 0x07E0; // magenta, green; c0 > c1 selects 4-colour mode
    uint8_t data[32];
    for (int b = 0; b < 4; b++) {
        uint8_t *blk = data + 8 * b;
        blk[0] = (uint8_t)(c0 >> 8), blk[1] = (uint8_t)c0, blk[2] = (uint8_t)(c1 >> 8), blk[3] = (uint8_t)c1;
        memset(blk + 4, 0x1B, 4); // 00 01 10 11 per row
    }
    uint8_t e[2][3];
    const uint16_t c[2] = {c0, c1};
    for (int i = 0; i < 2; i++) {
        unsigned r = (c[i] >> 11) & 31u, g = (c[i] >> 5) & 63u, bl = c[i] & 31u;
        e[i][0] = (uint8_t)((r << 3) | (r >> 2));
        e[i][1] = (uint8_t)((g << 2) | (g >> 4));
        e[i][2] = (uint8_t)((bl << 3) | (bl >> 2));
    }
    uint8_t pal[4][4];
    for (int k = 0; k < 3; k++) {
        pal[0][k] = e[0][k];
        pal[1][k] = e[1][k];
        pal[2][k] = (uint8_t)((5u * e[0][k] + 3u * e[1][k]) >> 3);
        pal[3][k] = (uint8_t)((3u * e[0][k] + 5u * e[1][k]) >> 3);
    }
    for (int i = 0; i < 4; i++) {
        pal[i][3] = 255;
    }
    uint8_t rgba[8 * 8 * 4];
    wl_tex_decode(WL_TEX_CMPR, data, 8, 8, rgba);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            ASSERT_MEM_EQ(pal[x % 4], rgba + 4 * (y * 8 + x), 4);
        }
    }
    PASS();
}

TEST gx_cmpr_two_colour_blocks_exact_and_general_blocks_bounded(void) {
    enum { W = 16, H = 16 };
    uint8_t src[W * H * 4];
    // 4x4 blocks: even blocks two colours (+ transparency on some), odd blocks a smooth ramp.
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            uint8_t *p = src + ((size_t)y * W + x) * 4;
            unsigned bx = x / 4, by = y / 4;
            if ((bx + by) % 2 == 0) {
                int on = (int)((x + y) & 1u);
                unsigned c = on ? 20u : 5u;
                p[0] = (uint8_t)((c << 3) | (c >> 2));
                unsigned gg = c * 3 / 2u, bb = 31 - c;
                p[1] = (uint8_t)((gg << 3) | (gg >> 2));
                p[2] = (uint8_t)((bb << 3) | (bb >> 2));
                p[3] = (bx == 0 && by == 0 && x == 1) ? 0 : 255;
                if (p[3] == 0) {
                    p[0] = p[1] = p[2] = 0;
                }
            } else {
                p[0] = (uint8_t)(x * 16u);
                p[1] = (uint8_t)(y * 16u);
                p[2] = (uint8_t)(x * 8u + y * 8u);
                p[3] = 255;
            }
        }
    }
    WlGxTex out;
    ASSERT_EQ(WL_OK, wl_gx_tex_from_rgba(src, W, H, WL_GX_TF_CMPR, &out));
    WlImage im;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&out, &im));
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            const uint8_t *a = src + ((size_t)y * W + x) * 4, *b = im.rgba + ((size_t)y * W + x) * 4;
            ASSERT_EQ(a[3], b[3]);
            if (a[3] == 0) {
                continue;
            }
            unsigned bx = x / 4, by = y / 4;
            int tol = (bx + by) % 2 == 0 ? 3 : 56; // two-colour blocks: only the 565 rounding
            for (int k = 0; k < 3; k++) {
                int d = (int)a[k] - (int)b[k];
                ASSERT(d >= -tol && d <= tol);
            }
        }
    }
    ASSERT_EQ(1, ref_agrees(&out, &im, 0)); // the wl/gfx decoder agrees exactly (same blend weights)
    wl_image_free(&im);
    wl_gx_tex_free(&out);
    PASS();
}

// ---------------------------------------------------------------------------------------------
// TEX0
// ---------------------------------------------------------------------------------------------

typedef struct {
    unsigned fmt, w, h, transp;
    const uint8_t *data;
    size_t size;           // texel bytes
    const uint8_t *idx;    // 4x4: palette index words (size/2 bytes)
    const char *name;
} Tex;

static void dict(Buf *b, unsigned count, unsigned unit, const uint8_t *ents, const char *const *names) {
    size_t base = b->n;
    b8(b, 0);
    b8(b, count);
    b16(b, (unsigned)(8 + 4 * (count + 1) + 4 + unit * count + 16 * count));
    b16(b, 8);
    b16(b, (unsigned)(8 + 4 * (count + 1)));
    for (unsigned i = 0; i < count + 1; i++) {
        b32(b, 0);
    }
    b16(b, unit);
    b16(b, 4 + unit * count);
    bput(b, ents, (size_t)unit * count);
    for (unsigned i = 0; i < count; i++) {
        char nm[16] = {0};
        strncpy(nm, names[i], 15);
        bput(b, nm, 16);
    }
    (void)base;
}

// Builds a BTX0 with the given textures and palettes (each palette: bytes, name).
static void make_btx0(Buf *out, unsigned ntex, const Tex *tex, unsigned npal, const uint8_t *const *pal,
                      const size_t *pal_size, const char *const *pal_names) {
    Buf texdata = {0}, compdata = {0}, compidx = {0}, paldata = {0};
    uint8_t ents[16][8];
    const char *tnames[16];
    for (unsigned i = 0; i < ntex; i++) {
        const Tex *t = &tex[i];
        size_t off;
        if (t->fmt == 5) {
            off = compdata.n;
            bput(&compdata, t->data, t->size);
            bput(&compidx, t->idx, t->size / 2);
        } else {
            off = texdata.n;
            bput(&texdata, t->data, t->size);
        }
        while (texdata.n % 8) {
            b8(&texdata, 0);
        }
        while (compdata.n % 8) {
            b8(&compdata, 0);
        }
        unsigned lw = 0, lh = 0;
        while ((8u << lw) < t->w) {
            lw++;
        }
        while ((8u << lh) < t->h) {
            lh++;
        }
        uint32_t param = (uint32_t)(off >> 3) | (lw << 20) | (lh << 23) | (t->fmt << 26) | (t->transp << 29);
        wl_wr_be32(ents[i], 0); // placeholder, rewritten little-endian below
        ents[i][0] = (uint8_t)param;
        ents[i][1] = (uint8_t)(param >> 8);
        ents[i][2] = (uint8_t)(param >> 16);
        ents[i][3] = (uint8_t)(param >> 24);
        memset(ents[i] + 4, 0, 4);
        tnames[i] = t->name;
    }
    uint8_t pents[8][4];
    for (unsigned i = 0; i < npal; i++) {
        size_t off = paldata.n;
        bput(&paldata, pal[i], pal_size[i]);
        while (paldata.n % 8) {
            b8(&paldata, 0);
        }
        pents[i][0] = (uint8_t)(off >> 3);
        pents[i][1] = (uint8_t)(off >> 11);
        pents[i][2] = pents[i][3] = 0;
    }
    Buf td = {0}, pd = {0};
    dict(&td, ntex, 8, (const uint8_t *)ents, tnames);
    dict(&pd, npal, 4, (const uint8_t *)pents, pal_names);
    Buf blk = {0};
    size_t hdr = 0x3C;
    size_t tex_dict_off = hdr, pal_dict_off = hdr + td.n;
    size_t tex_off = pal_dict_off + pd.n;
    size_t comp_off = tex_off + texdata.n;
    size_t cidx_off = comp_off + compdata.n;
    size_t pal_off = cidx_off + compidx.n;
    size_t total = pal_off + paldata.n;
    bput(&blk, "TEX0", 4);
    b32(&blk, (uint32_t)total);
    b32(&blk, 0);
    b16(&blk, (unsigned)(texdata.n >> 3));
    b16(&blk, (unsigned)tex_dict_off);
    b32(&blk, 0);
    b32(&blk, (uint32_t)tex_off);
    b32(&blk, 0);
    b16(&blk, (unsigned)(compdata.n >> 3));
    b16(&blk, (unsigned)tex_dict_off);
    b32(&blk, 0);
    b32(&blk, (uint32_t)comp_off);
    b32(&blk, (uint32_t)cidx_off);
    b32(&blk, 0);
    b16(&blk, (unsigned)(paldata.n >> 3));
    b16(&blk, 0x8000);
    b32(&blk, (uint32_t)pal_dict_off);
    b32(&blk, (uint32_t)pal_off);
    bput(&blk, td.p, td.n);
    bput(&blk, pd.p, pd.n);
    bput(&blk, texdata.p, texdata.n);
    bput(&blk, compdata.p, compdata.n);
    bput(&blk, compidx.p, compidx.n);
    bput(&blk, paldata.p, paldata.n);
    bput(out, "BTX0", 4);
    b16(out, 0xFEFF);
    b16(out, 0x0001);
    b32(out, (uint32_t)(0x14 + blk.n));
    b16(out, 16);
    b16(out, 1);
    b32(out, 0x14);
    bput(out, blk.p, blk.n);
    bfree(&texdata);
    bfree(&compdata);
    bfree(&compidx);
    bfree(&paldata);
    bfree(&td);
    bfree(&pd);
    bfree(&blk);
}

static void pal_bytes(uint8_t *out, const uint16_t *col, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        out[2 * i] = (uint8_t)col[i];
        out[2 * i + 1] = (uint8_t)(col[i] >> 8);
    }
}

// Expected RGBA of a palette entry; entry transparent when t is set.
static void exp_pal(uint16_t c, int transparent, uint8_t out[4]) {
    expect_rgba(c, out);
    out[3] = transparent ? 0 : 255;
}

TEST tex0_paletted_formats_exact(void) {
    // 16-colour 16x8, 4-colour 8x8 (colour 0 transparent), 256-colour 8x8, A3I5 8x8, A5I3 8x8.
    uint16_t pal16[16], pal4[4], pal256[256], pal32[32], pal8[8];
    for (unsigned i = 0; i < 256; i++) {
        pal256[i] = (uint16_t)rgb555(i & 31u, (i >> 3) & 31u, (i * 3) & 31u);
    }
    memcpy(pal16, pal256 + 40, sizeof pal16);
    memcpy(pal4, pal256 + 90, sizeof pal4);
    memcpy(pal32, pal256 + 100, sizeof pal32);
    memcpy(pal8, pal256 + 150, sizeof pal8);
    uint8_t t16[16 * 8 / 2], t4[8 * 8 / 4], t256[64], ta[64], tb[64];
    for (unsigned i = 0; i < 16 * 8; i++) { // pixel i has index (i*5+1) % 16, two per byte, low nibble first
        unsigned v = (i * 5 + 1) % 16u;
        t16[i / 2] = (uint8_t)((i % 2) ? (t16[i / 2] | (v << 4)) : v);
    }
    for (unsigned i = 0; i < 64; i++) {
        unsigned v = (i * 3) % 4u;
        t4[i / 4] = (uint8_t)((i % 4) ? (t4[i / 4] | (v << (2 * (i % 4)))) : v);
        t256[i] = (uint8_t)(i * 4 + 1);
        ta[i] = (uint8_t)(((i % 8) << 5) | (i % 32));  // alpha 0..7, index 0..31
        tb[i] = (uint8_t)(((i % 32) << 3) | (i % 8));  // alpha 0..31, index 0..7
    }
    Tex tex[5] = {
        {3, 16, 8, 0, t16, sizeof t16, NULL, "tex16"},
        {2, 8, 8, 1, t4, sizeof t4, NULL, "tex4"},
        {4, 8, 8, 0, t256, sizeof t256, NULL, "tex256"},
        {1, 8, 8, 0, ta, sizeof ta, NULL, "texa3i5"},
        {6, 8, 8, 0, tb, sizeof tb, NULL, "texa5i3"},
    };
    uint8_t pb[5][512];
    pal_bytes(pb[0], pal16, 16);
    pal_bytes(pb[1], pal4, 4);
    pal_bytes(pb[2], pal256, 256);
    pal_bytes(pb[3], pal32, 32);
    pal_bytes(pb[4], pal8, 8);
    const uint8_t *pals[5] = {pb[0], pb[1], pb[2], pb[3], pb[4]};
    size_t psz[5] = {32, 8, 512, 64, 16};
    const char *pn[5] = {"tex16_pl", "tex4", "tex256_pl", "texa3i5_pl", "texa5i3_pl"};
    Buf f = {0};
    make_btx0(&f, 5, tex, 5, pals, psz, pn);
    WlTex0 t;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, f.p, f.n));
    ASSERT_EQ(5u, t.ntex);
    ASSERT_EQ(5u, t.npal);
    for (unsigned i = 0; i < 5; i++) {
        uint32_t pal;
        int how;
        ASSERT_EQ(WL_OK, wl_tex0_pair_palette(&t, i, &pal, &how));
        ASSERT_EQ(i, pal);
        ASSERT_EQ(i == 1 ? 1 : 0, how); // "tex4" matches by stem, the rest by "_pl"
        WlImage im;
        ASSERT_EQ(WL_OK, wl_tex0_decode(&t, i, pal, &im));
        for (unsigned p = 0; p < tex[i].w * tex[i].h; p++) {
            uint8_t want[4];
            switch (tex[i].fmt) {
            case 3:
                exp_pal(pal16[(p * 5 + 1) % 16u], 0, want);
                break;
            case 2:
                exp_pal(pal4[(p * 3) % 4u], ((p * 3) % 4u) == 0, want);
                break;
            case 4:
                exp_pal(pal256[(p * 4 + 1) & 255u], 0, want);
                break;
            case 1: {
                exp_pal(pal32[p % 32u], 0, want);
                unsigned a = p % 8u;
                want[3] = (uint8_t)((a << 5) | (a << 2) | (a >> 1));
                break;
            }
            default: {
                exp_pal(pal8[p % 8u], 0, want);
                unsigned a = p % 32u;
                want[3] = (uint8_t)((a << 3) | (a >> 2));
                break;
            }
            }
            ASSERT_EQ(0, memcmp(im.rgba + 4 * p, want, 4));
        }
        // And through the GX conversion.
        WlGxTex gt;
        WlTex0Policy pol = {0};
        ASSERT_EQ(WL_OK, wl_tex0_to_gx(&t, i, pal, &pol, &gt));
        int want_fmt = tex[i].fmt == 3 || tex[i].fmt == 2 ? WL_GX_TF_C4
                       : tex[i].fmt == 4                  ? WL_GX_TF_C8
                                                          : WL_GX_TF_RGBA8; // partial alpha
        ASSERT_EQ(want_fmt, (int)gt.fmt);
        WlImage g;
        ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &g));
        ASSERT_EQ(1, same_pixels(im.rgba, g.rgba, (size_t)im.w * im.h));
        ASSERT_EQ(1, ref_agrees(&gt, &g, 0));
        wl_image_free(&g);
        wl_image_free(&im);
        wl_gx_tex_free(&gt);
    }
    bfree(&f);
    PASS();
}

TEST tex0_a3i5_two_level_alpha_becomes_rgb5a3(void) {
    uint16_t pal32[32];
    for (unsigned i = 0; i < 32; i++) {
        pal32[i] = (uint16_t)rgb555(i, 31 - i, i / 2);
    }
    uint8_t ta[64];
    for (unsigned i = 0; i < 64; i++) {
        ta[i] = (uint8_t)(((i % 3 == 0 ? 0u : 7u) << 5) | (i % 32));
    }
    Tex tex = {1, 8, 8, 0, ta, sizeof ta, NULL, "a"};
    uint8_t pb[64];
    pal_bytes(pb, pal32, 32);
    const uint8_t *pals[1] = {pb};
    size_t psz[1] = {64};
    const char *pn[1] = {"a_pl"};
    Buf f = {0};
    make_btx0(&f, 1, &tex, 1, pals, psz, pn);
    WlTex0 t;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, f.p, f.n));
    WlGxTex gt;
    WlTex0Policy pol = {0};
    ASSERT_EQ(WL_OK, wl_tex0_to_gx(&t, 0, 0, &pol, &gt));
    ASSERT_EQ(WL_GX_TF_RGB5A3, (int)gt.fmt);
    WlImage im, g;
    ASSERT_EQ(WL_OK, wl_tex0_decode(&t, 0, 0, &im));
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &g));
    ASSERT_EQ(1, same_pixels(im.rgba, g.rgba, 64));
    wl_image_free(&im);
    wl_image_free(&g);
    wl_gx_tex_free(&gt);
    bfree(&f);
    PASS();
}

// A transparent texel keeps whatever colour the artist left under it (magenta here); bilinear filtering pulls it into every
// cutout edge, so the GX texture carries the average of the opaque texels it borders instead (the exact decode is unchanged).
TEST tex0_transparent_texels_take_the_neighbours_colour(void) {
    uint16_t pal4[4] = {(uint16_t)rgb555(31, 0, 31), (uint16_t)rgb555(0, 31, 0), (uint16_t)rgb555(0, 0, 31), 0}; // 0 = magenta, transparent
    uint8_t ti[16]; // 8 x 8, 2 bits a texel: a 4 x 4 block of index 1 inside index 0, the rest index 0
    memset(ti, 0, sizeof ti);
    for (unsigned y = 2; y < 6; y++) {
        for (unsigned x = 2; x < 6; x++) {
            unsigned k = y * 8 + x;
            ti[k / 4] = (uint8_t)(ti[k / 4] | (1u << (2 * (k % 4))));
        }
    }
    uint8_t pb[8];
    pal_bytes(pb, pal4, 4);
    Tex tex = {2, 8, 8, 1, ti, sizeof ti, NULL, "k"}; // fmt 2 = 4 colours, transp 1
    const uint8_t *pals[1] = {pb};
    size_t psz[1] = {8};
    const char *pn[1] = {"k_pl"};
    Buf f = {0};
    make_btx0(&f, 1, &tex, 1, pals, psz, pn);
    WlTex0 t;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, f.p, f.n));
    WlImage im;
    ASSERT_EQ(WL_OK, wl_tex0_decode(&t, 0, 0, &im));
    ASSERT_EQ(0, im.rgba[3]);
    ASSERT_EQ(255, im.rgba[0]); // the exact decode keeps the magenta
    ASSERT_EQ(255, im.rgba[2]);
    WlGxTex gt;
    WlTex0Policy pol = {0};
    ASSERT_EQ(WL_OK, wl_tex0_to_gx(&t, 0, 0, &pol, &gt));
    WlImage g;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &g));
    ASSERT_EQ(1, same_pixels(im.rgba, g.rgba, 64)); // alpha and the opaque texels exact
    for (unsigned p = 0; p < 64; p++) {
        if (g.rgba[4 * p + 3] == 0) {
            ASSERT_EQ(0, g.rgba[4 * p]); // the opaque neighbours are green: no magenta left
            ASSERT_EQ(255, g.rgba[4 * p + 1]);
            ASSERT_EQ(0, g.rgba[4 * p + 2]);
        }
    }
    wl_image_free(&g);
    wl_image_free(&im);
    wl_gx_tex_free(&gt);
    bfree(&f);
    PASS();
}

TEST tex0_direct_colour(void) {
    uint8_t td[8 * 8 * 2];
    uint16_t want[64];
    for (unsigned i = 0; i < 64; i++) {
        want[i] = (uint16_t)(rgb555(i & 31u, (i * 3) & 31u, (i * 5) & 31u) | ((i % 4) ? 0x8000u : 0u));
        td[2 * i] = (uint8_t)want[i];
        td[2 * i + 1] = (uint8_t)(want[i] >> 8);
    }
    Tex tex = {7, 8, 8, 0, td, sizeof td, NULL, "d"};
    Buf f = {0};
    make_btx0(&f, 1, &tex, 0, NULL, NULL, NULL);
    WlTex0 t;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, f.p, f.n));
    WlImage im;
    ASSERT_EQ(WL_OK, wl_tex0_decode(&t, 0, 0, &im));
    for (unsigned i = 0; i < 64; i++) {
        uint8_t e[4];
        expect_rgba(want[i], e);
        e[3] = (want[i] & 0x8000u) ? 255 : 0;
        ASSERT_EQ(e[3], im.rgba[4 * i + 3]);
        if (e[3]) {
            ASSERT_EQ(0, memcmp(e, im.rgba + 4 * i, 3));
        }
    }
    WlGxTex gt;
    WlTex0Policy pol = {0};
    ASSERT_EQ(WL_OK, wl_tex0_to_gx(&t, 0, 0, &pol, &gt));
    ASSERT_EQ(WL_GX_TF_RGB5A3, (int)gt.fmt);
    WlImage g;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &g));
    ASSERT_EQ(1, same_pixels(im.rgba, g.rgba, 64));
    wl_image_free(&g);
    wl_image_free(&im);
    wl_gx_tex_free(&gt);
    bfree(&f);
    PASS();
}

// 4x4-compressed texture, 16x8 pixels = 8 blocks, one per palette-index mode and a mix.
TEST tex0_compressed_4x4_all_modes(void) {
    enum { W = 16, H = 8, NB = 8 };
    uint16_t pal[16];
    for (unsigned i = 0; i < 16; i++) {
        pal[i] = (uint16_t)rgb555((i * 2) & 31u, 31u - i, (i * 5 + 3) & 31u);
    }
    // block k: mode m = k % 4, palette offset (in colour pairs) o = k % 3 * 2 (so base colour 2*o)
    uint32_t texels[NB];
    uint16_t pidx[NB];
    for (unsigned k = 0; k < NB; k++) {
        unsigned m = k % 4u, o = k % 3u;
        pidx[k] = (uint16_t)((m << 14) | o);
        uint32_t v = 0;
        for (unsigned i = 0; i < 16; i++) {
            v |= (uint32_t)((i + k) % 4u) << (2 * i);
        }
        texels[k] = v;
    }
    uint8_t td[NB * 4], ti[NB * 2];
    for (unsigned k = 0; k < NB; k++) {
        for (unsigned j = 0; j < 4; j++) {
            td[4 * k + j] = (uint8_t)(texels[k] >> (8 * j));
        }
        ti[2 * k] = (uint8_t)pidx[k];
        ti[2 * k + 1] = (uint8_t)(pidx[k] >> 8);
    }
    Tex tex = {5, W, H, 0, td, sizeof td, ti, "c"};
    uint8_t pb[32];
    pal_bytes(pb, pal, 16);
    const uint8_t *pals[1] = {pb};
    size_t psz[1] = {32};
    const char *pn[1] = {"c_pl"};
    Buf f = {0};
    make_btx0(&f, 1, &tex, 1, pals, psz, pn);
    WlTex0 t;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, f.p, f.n));
    WlImage im;
    ASSERT_EQ(WL_OK, wl_tex0_decode(&t, 0, 0, &im));
    unsigned avail_transparent = 0, avail_blend = 0;
    for (unsigned k = 0; k < NB; k++) {
        unsigned bx = k % (W / 4), by = k / (W / 4);
        unsigned m = k % 4u, b = (k % 3u) * 2u;
        uint8_t c[4][4];
        unsigned c0 = pal[b], c1 = pal[b + 1];
        exp_pal((uint16_t)c0, 0, c[0]);
        exp_pal((uint16_t)c1, 0, c[1]);
        for (int q = 2; q < 4; q++) {
            memset(c[q], 0, 4);
        }
        unsigned mix2 = 0, mix3 = 0;
        for (unsigned s = 0; s < 15; s += 5) {
            unsigned x = (c0 >> s) & 31u, y = (c1 >> s) & 31u;
            mix2 |= ((x + y) / 2u) << s;
            mix3 |= ((x * 3 + y * 5) / 8u) << s;
        }
        unsigned mix5 = 0;
        for (unsigned s = 0; s < 15; s += 5) {
            mix5 |= (((c0 >> s) & 31u) * 5u + ((c1 >> s) & 31u) * 3u) / 8u << s;
        }
        if (m == 0) {
            exp_pal(pal[b + 2], 0, c[2]);
        } else if (m == 1) {
            exp_pal((uint16_t)mix2, 0, c[2]);
        } else if (m == 2) {
            exp_pal(pal[b + 2], 0, c[2]);
            exp_pal(pal[b + 3], 0, c[3]);
        } else {
            exp_pal((uint16_t)mix5, 0, c[2]);
            exp_pal((uint16_t)mix3, 0, c[3]);
        }
        if (m < 2) {
            c[3][3] = 0; // transparent
        }
        for (unsigned i = 0; i < 16; i++) {
            unsigned v = (i + k) % 4u;
            const uint8_t *g = im.rgba + ((size_t)(by * 4 + i / 4) * W + bx * 4 + i % 4) * 4;
            ASSERT_EQ(c[v][3], g[3]);
            if (c[v][3]) {
                ASSERT_EQ(0, memcmp(c[v], g, 3));
            } else {
                avail_transparent++;
            }
            avail_blend += (m == 1 || m == 3) && v >= 2;
        }
    }
    ASSERT(avail_transparent > 0 && avail_blend > 0);
    // Exact policy: RGB5A3 with the same pixels.
    WlGxTex gt;
    WlTex0Policy ex = {0}, cm = {1};
    ASSERT_EQ(WL_OK, wl_tex0_to_gx(&t, 0, 0, &ex, &gt));
    ASSERT_EQ(WL_GX_TF_RGB5A3, (int)gt.fmt);
    WlImage g;
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &g));
    ASSERT_EQ(1, same_pixels(im.rgba, g.rgba, W * H));
    wl_image_free(&g);
    wl_gx_tex_free(&gt);
    // Compact policy: CMPR, transparency preserved, colours close.
    ASSERT_EQ(WL_OK, wl_tex0_to_gx(&t, 0, 0, &cm, &gt));
    ASSERT_EQ(WL_GX_TF_CMPR, (int)gt.fmt);
    ASSERT_EQ(WL_OK, wl_gx_tex_decode(&gt, &g));
    for (unsigned i = 0; i < W * H; i++) {
        ASSERT_EQ(im.rgba[4 * i + 3], g.rgba[4 * i + 3]);
        if (im.rgba[4 * i + 3]) {
            for (int k = 0; k < 3; k++) {
                int d = (int)im.rgba[4 * i + (unsigned)k] - (int)g.rgba[4 * i + (unsigned)k];
                ASSERT(d >= -80 && d <= 80);
            }
        }
    }
    wl_image_free(&g);
    wl_image_free(&im);
    wl_gx_tex_free(&gt);
    bfree(&f);
    PASS();
}

TEST tex0_rejects_malformed(void) {
    uint16_t pal16[16] = {0};
    uint8_t t16[32] = {0};
    Tex tex = {3, 8, 8, 0, t16, sizeof t16, NULL, "x"};
    uint8_t pb[32];
    pal_bytes(pb, pal16, 16);
    const uint8_t *pals[1] = {pb};
    size_t psz[1] = {32};
    const char *pn[1] = {"x_pl"};
    Buf f = {0};
    make_btx0(&f, 1, &tex, 1, pals, psz, pn);
    WlTex0 t;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, f.p, f.n));
    // every truncation fails or yields a consistent reader; none crashes
    for (size_t cut = 0; cut < f.n; cut += 7) {
        WlResult r = wl_tex0_open(&t, f.p, cut);
        if (r == WL_OK) {
            WlImage im;
            WlResult d = wl_tex0_decode(&t, 0, 0, &im);
            if (d == WL_OK) {
                wl_image_free(&im);
            }
        }
    }
    // damaged magic, damaged dictionary count, texture offset past the data
    uint8_t bad[512];
    ASSERT(f.n <= sizeof bad);
    memcpy(bad, f.p, f.n);
    bad[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_tex0_open(&t, bad, f.n));
    memcpy(bad, f.p, f.n);
    bad[0x14 + 0x3C + 1] = 200; // texture dictionary count
    ASSERT(wl_tex0_open(&t, bad, f.n) != WL_OK);
    memcpy(bad, f.p, f.n);
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, bad, f.n));
    WlTexInfo ti;
    ASSERT_EQ(WL_OK, wl_tex0_info(&t, 0, &ti));
    ASSERT_EQ(WL_ERR_ARG, wl_tex0_info(&t, 5, &ti));
    // set the texture's data offset field to a huge value
    size_t ent = (size_t)0x14 + 0x3C + 8 + 4 * 2 + 4;
    bad[ent + 1] = 0xFF;
    ASSERT_EQ(WL_OK, wl_tex0_open(&t, bad, f.n));
    ASSERT_EQ(WL_ERR_FORMAT, wl_tex0_info(&t, 0, &ti));
    bfree(&f);
    // not a texture file at all
    ASSERT_EQ(WL_ERR_FORMAT, wl_tex0_open(&t, (const uint8_t *)"NARC", 4));
    PASS();
}

// ---------------------------------------------------------------------------------------------
// damaged inputs and PNG
// ---------------------------------------------------------------------------------------------

static uint32_t g_rng = 12345;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

TEST damaged_2d_resources_never_crash(void) {
    uint8_t pix[128];
    quad_tiles(pix, 4, 1);
    uint16_t col[16] = {0};
    Oam o[2] = {{-8, -8, 1, 0, 0, 0, 0, 0, 0, 0}, {-8, -8, 1, 0, 0, 0, 1, 1, 0, 0}};
    unsigned noam[2] = {1, 1};
    uint32_t vt[2][2] = {{0, 128}, {0, 128}};
    unsigned seq[1][2] = {{2, 2}};
    unsigned frames[2][2] = {{0, 4}, {1, 4}};
    Buf f[4] = {{0}, {0}, {0}, {0}};
    make_ncgr(&f[0], pix, sizeof pix, 3, 0);
    make_nclr(&f[1], col, 16, 3);
    make_ncer(&f[2], 2, noam, o, 0, vt);
    make_nanr(&f[3], 1, seq, 2, frames);
    for (int k = 0; k < 4; k++) {
        for (int round = 0; round < 400; round++) {
            Buf c = {0};
            bput(&c, f[k].p, f[k].n);
            size_t n = c.n;
            unsigned flips = 1 + rnd() % 3u;
            for (unsigned i = 0; i < flips; i++) {
                c.p[rnd() % c.n] = (uint8_t)rnd();
            }
            if (round % 5 == 0) {
                n = rnd() % c.n;
            }
            WlNcgr g;
            WlNclr p;
            WlNcer e;
            WlNanr a;
            uint8_t img[16 * 16 * 4] = {0};
            switch (k) {
            case 0:
                (void)wl_ncgr_open(&g, c.p, n);
                break;
            case 1:
                (void)wl_nclr_open(&p, c.p, n);
                break;
            case 2:
                if (wl_ncer_open(&e, c.p, n) == WL_OK) {
                    WlNcgr g2;
                    WlNclr p2;
                    if (wl_ncgr_open(&g2, f[0].p, f[0].n) == WL_OK && wl_nclr_open(&p2, f[1].p, f[1].n) == WL_OK) {
                        for (uint32_t cell = 0; cell < e.ncells; cell++) {
                            (void)wl_ds_render_cell(&g2, &p2, &e, cell, -1, -8, -8, img, 16, 16);
                        }
                    }
                    wl_ncer_free(&e);
                }
                break;
            default:
                if (wl_nanr_open(&a, c.p, n) == WL_OK) {
                    wl_nanr_free(&a);
                }
                break;
            }
            bfree(&c);
        }
        bfree(&f[k]);
    }
    PASS();
}

TEST damaged_gx_data_never_crash(void) {
    // Random texel data, sizes, formats and palettes: the decoder either succeeds or refuses.
    static const uint32_t fmts[] = {WL_GX_TF_I4, WL_GX_TF_RGB565, WL_GX_TF_RGB5A3, WL_GX_TF_RGBA8,
                                    WL_GX_TF_C4, WL_GX_TF_C8, WL_GX_TF_CMPR, WL_GX_TF_IA8, 0x7, 0xFF};
    uint8_t data[1024], tlut[512];
    for (int round = 0; round < 500; round++) {
        for (size_t i = 0; i < sizeof data; i++) {
            data[i] = (uint8_t)rnd();
        }
        for (size_t i = 0; i < sizeof tlut; i++) {
            tlut[i] = (uint8_t)rnd();
        }
        uint32_t fmt = fmts[rnd() % (sizeof fmts / sizeof fmts[0])];
        uint32_t w = rnd() % 40u, h = rnd() % 40u;
        size_t size = rnd() % (sizeof data + 1);
        uint32_t ntl = rnd() % 260u;
        uint32_t tlf = round % 5 == 0 ? WL_GX_TL_IA8 : WL_GX_TL_RGB5A3;
        WlImage im;
        WlResult r = wl_gx_decode(fmt, w, h, data, size, tlf, ntl <= 256 ? tlut : NULL, ntl, &im);
        if (r == WL_OK) {
            ASSERT(wl_gx_tex_size(fmt, w, h) <= size);
            wl_image_free(&im);
        }
    }
    WlImage im;
    ASSERT_EQ(WL_ERR_ARG, wl_gx_decode(WL_GX_TF_RGBA8, 4, 4, NULL, 64, 0, NULL, 0, &im));
    ASSERT_EQ(WL_ERR_FORMAT, wl_gx_decode(WL_GX_TF_RGBA8, 4, 4, data, 63, 0, NULL, 0, &im));
    ASSERT_EQ(WL_ERR_FORMAT, wl_gx_decode(WL_GX_TF_C8, 8, 4, data, 32, WL_GX_TL_RGB5A3, NULL, 0, &im));
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_gx_decode(0x7, 8, 8, data, 64, 0, NULL, 0, &im));
    // An index past the TLUT is refused.
    memset(data, 0xFF, 32);
    ASSERT_EQ(WL_ERR_FORMAT, wl_gx_decode(WL_GX_TF_C8, 8, 4, data, 32, WL_GX_TL_RGB5A3, tlut, 255, &im));
    ASSERT_EQ(WL_OK, wl_gx_decode(WL_GX_TF_C8, 8, 4, data, 32, WL_GX_TL_RGB5A3, tlut, 256, &im));
    wl_image_free(&im);
    PASS();
}

static int png_inflate_stored(const uint8_t *z, size_t n, uint8_t *out, size_t cap, size_t *outn) {
    size_t p = 2, o = 0;
    for (;;) {
        if (p + 5 > n) {
            return 0;
        }
        int last = z[p] & 1;
        unsigned len = z[p + 1] | (unsigned)(z[p + 2] << 8);
        unsigned nlen = z[p + 3] | (unsigned)(z[p + 4] << 8);
        if ((len ^ nlen) != 0xFFFFu || p + 5 + len > n || o + len > cap) {
            return 0;
        }
        memcpy(out + o, z + p + 5, len);
        o += len;
        p += 5u + len;
        if (last) {
            break;
        }
    }
    *outn = o;
    return 1;
}

TEST png_encode_is_valid(void) {
    // 300 x 100 RGBA: the raw size exceeds 65535, so several stored blocks are needed.
    enum { W = 300, H = 100 };
    static uint8_t px[W * H * 4];
    for (size_t i = 0; i < sizeof px; i++) {
        px[i] = (uint8_t)(i * 31u + (i >> 8));
    }
    uint8_t *png;
    size_t n;
    ASSERT_EQ(WL_OK, wl_png_encode(px, W, H, &png, &n));
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    ASSERT_EQ(0, memcmp(png, sig, 8));
    ASSERT_EQ(0, memcmp(png + 12, "IHDR", 4));
    ASSERT_EQ((uint32_t)W, wl_rd_be32(png + 16));
    ASSERT_EQ((uint32_t)H, wl_rd_be32(png + 20));
    ASSERT_EQ(6, png[25]); // colour type RGBA
    // IDAT: inflate the stored blocks and compare with the filtered rows
    size_t idat = 8 + 25;
    uint32_t zlen = wl_rd_be32(png + idat);
    ASSERT_EQ(0, memcmp(png + idat + 4, "IDAT", 4));
    static uint8_t raw[(W * 4 + 1) * H + 16];
    size_t rn;
    ASSERT_EQ(1, png_inflate_stored(png + idat + 8, zlen, raw, sizeof raw, &rn));
    ASSERT_EQ((size_t)(W * 4 + 1) * H, rn);
    for (unsigned y = 0; y < H; y++) {
        ASSERT_EQ(0, raw[(size_t)y * (W * 4 + 1)]);
        ASSERT_EQ(0, memcmp(raw + (size_t)y * (W * 4 + 1) + 1, px + (size_t)y * W * 4, W * 4));
    }
    ASSERT_EQ(0, memcmp(png + n - 12 + 4, "IEND", 4));
    // the CRC of IHDR (ISO 3309) over type + data
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 12; i < 12 + 4 + 13; i++) {
        crc ^= png[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    ASSERT_EQ(~crc, wl_rd_be32(png + 8 + 8 + 13));
    free(png);
    ASSERT_EQ(WL_ERR_ARG, wl_png_encode(px, 0, 4, &png, &n));
    PASS();
}

SUITE(gxtex_suite) {
    RUN_TEST(nclr_parse);
    RUN_TEST(ncgr_parse);
    RUN_TEST(render_cell_quadrants_and_flips);
    RUN_TEST(render_cell_rules);
    RUN_TEST(sprite_vram_transfer_and_animation);
    RUN_TEST(sprite_conflicting_holds_duplicate_frames);
    RUN_TEST(sprite_more_than_15_colours_is_reduced);
    RUN_TEST(cells_tex_stacks_frames);
    RUN_TEST(gx_rgb5a3_and_rgba8_exact);
    RUN_TEST(gx_indexed_c4_c8);
    RUN_TEST(gx_cmpr_decode_matches_hardware_palette);
    RUN_TEST(gx_cmpr_two_colour_blocks_exact_and_general_blocks_bounded);
    RUN_TEST(tex0_paletted_formats_exact);
    RUN_TEST(tex0_a3i5_two_level_alpha_becomes_rgb5a3);
    RUN_TEST(tex0_transparent_texels_take_the_neighbours_colour);
    RUN_TEST(tex0_direct_colour);
    RUN_TEST(tex0_compressed_4x4_all_modes);
    RUN_TEST(tex0_rejects_malformed);
    RUN_TEST(damaged_2d_resources_never_crash);
    RUN_TEST(damaged_gx_data_never_crash);
    RUN_TEST(png_encode_is_valid);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gxtex_suite);
    GREATEST_MAIN_END();
}
