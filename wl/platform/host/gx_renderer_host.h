// SPDX-License-Identifier: MIT
// A renderer behind the host's GX stubs. The stubs record what was asked of GX (gx_host.h,
// gx_direct_host.h, tev_host.h, gx_target_host.h) and draw nothing; a renderer set here is also
// handed every operation that changes the EFB, so that it can draw what GX would have drawn:
// wl/gxsoft's software GX is one. It reads the state a draw is made with from the recorders (the
// direct layer's wl_host_gx_direct(), the TEV program's wl_host_tev_last(), a texture object's
// image wl_host_gx_tex_image()) and is told of the rest here.
//
// What the host stubs keep as GX does, so that a renderer sees the state GX would draw with: the
// render targets' quads (a clear, a parked back buffer put back) leave the state the console's
// leave (gx_target.h: an orthographic projection over the back buffer, its viewport, blending off,
// the alpha test passing, no culling, the depth test as the quad set it), and their scissor is
// the direct layer's scissor, GX having one.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/platform.h"

// a copy's format: a WlGxTexFormat (RGBA8, RGB565, RGB5A3, I4, I8, IA8), or the EFB's depth as a
// Z24X8 texture (the layout of RGBA8: alpha 255, then the depth's high, middle and low bytes)
#define WL_HOST_GX_COPY_Z24X8 0x100u

typedef struct WlHostGxRenderer {
    void *user;
    // wl_gx_begin_frame: the EFB cleared to `clear_rgba` (0xRRGGBBAA) and the far depth
    void (*frame_begin)(void *user, uint32_t clear_rgba);
    // wl_gx_end_frame: the display copy of the EFB's top left `width` by `height` pixels
    void (*frame_end)(void *user, uint32_t width, uint32_t height);
    // a primitive in the direct layer's layout (the state: wl_host_gx_direct(), wl_host_tev_last())
    void (*draw)(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *vertices, uint32_t count);
    // a copy out of the EFB: its rectangle (x, y) `w` by `h` into `dst` as a texture of `format`
    // (tile-ordered, wl_gx_tex_bytes long), the rectangle cleared afterwards to the frame's clear
    // colour and the far depth when `clear`
    void (*copy)(void *user, int x, int y, uint32_t w, uint32_t h, void *dst, unsigned format, bool clear);
    // a parked colour (RGBA8) and depth (Z24X8), `w` by `h`, written back at the EFB's origin
    void (*restore)(void *user, const void *colour, const void *depth, uint32_t w, uint32_t h);
    // a clear quad over (x, y) `w` by `h`: colour and alpha where `colour`, depth where `depth`
    // (within the scissor)
    void (*clear)(void *user, bool colour, bool depth, uint32_t rgba, float z, int x, int y, int w, int h);
    // the finished back buffer narrowed into columns [x, x + w), black around it (gx_target.h)
    void (*squeeze)(void *user, uint32_t back_w, uint32_t back_h, uint32_t x, uint32_t w);
    // wl_gx_peek_rgba
    uint32_t (*peek)(void *user, int x, int y);
    // wl_gx_set_efb_alpha: the EFB with a destination alpha (RGBA6_Z24) or without (RGB8_Z24)
    void (*efb_alpha)(void *user, bool on);
    // GX's texture cache emptied (wl_gx_direct_texture_invalidate, wl_gx_tex_invalidate)
    void (*texture_invalidate)(void *user);
} WlHostGxRenderer;

// Sets the renderer (copied), or none for NULL. Any member may be NULL.
void wl_host_gx_set_renderer(const WlHostGxRenderer *renderer);
// The renderer set, NULL for none.
const WlHostGxRenderer *wl_host_gx_renderer(void);

// The image a texture object was made over (wl_gx_tex_init_ex's desc.image), NULL for none.
const void *wl_host_gx_tex_image(const WlGxTexObj *tex);

// (the host stubs' own: a texture map loaded with the ramp, wl_gx_ramp_bind)
void wl_host_gx_direct_ramp(uint32_t map);
