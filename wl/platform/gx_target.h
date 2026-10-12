// SPDX-License-Identifier: MIT
// Render targets on GX's one frame buffer. GX draws into a single EFB; the back buffer has it for
// the frame, and a draw into any other surface (a texture to render into, a reflection, a
// screen-in-the-world) takes it over:
//
//  - the back buffer, if anything was drawn into it, is parked first: its colour and its depth are
//    copied out of the EFB into memory of the layer's own;
//  - the target is drawn at the EFB's origin, within a scissor of its size;
//  - when another surface takes the EFB, the target's pixels are copied into its own memory in
//    GX's texture format (resolved), so that a texture object over that memory shows them;
//  - the back buffer is put back by a quad that writes the parked colour and, through a Z texture,
//    the parked depth.
//
// Once the frame has begun, a clear of part of the EFB is a quad too (wl_gx_target_clear): GX's
// own clear belongs to the display copy.
//
// The layer decides nothing about what a target is for; the caller says which surface it draws
// into (wl_gx_target_use) before each draw, and the layer does the rest. The target descriptors
// are the caller's: the layer keeps a copy of the owner's.
//
// Not allowed with a display list recording. The functions that draw bypass the wl_gx state
// shadow and call wl_gx_invalidate_shadow themselves; what they leave in GX, for a caller that
// draws with GX itself, is listed with wl_gx_target_use. The host backend keeps the same
// bookkeeping and records what the GPU would have been asked (host/gx_target_host.h).
#pragma once

#include "wl/platform/platform.h"

typedef struct {
    const void *id;    // what makes a surface the same surface: any address the caller owns (never the back buffer's: that is NULL)
    void *memory;      // where a resolve writes the image: 32-byte aligned, wl_gx_tex_bytes(format, width, height) bytes; NULL: not resolved
    uint16_t width, height; // pixels: multiples of 4, within the back buffer's size to be resolved (a larger target can still be drawn)
    uint8_t format;    // WlGxTexFormat: WL_GX_TEX_RGBA8 or WL_GX_TEX_RGB565 (anything else is not resolved)
    uintptr_t user;    // the caller's own word, handed back with the target when it is resolved
} WlGxTarget;

typedef struct {
    uint32_t parks;     // back buffers parked
    uint32_t restores;  // ...and put back (a restore with nothing parked is not counted)
    uint32_t resolves;  // targets copied into their memory
    uint32_t skipped;   // targets left without a copy (no memory, a format or size that has none)
    uint32_t clears;    // wl_gx_target_clear quads
    uint32_t squeezes;  // wl_gx_target_squeeze: finished pictures narrowed
} WlGxTargetStats;

// Whether a target can be resolved at all in a back buffer of `back_w` by `back_h` pixels, and the
// bytes its image takes (0 when it cannot). Pure.
bool wl_gx_target_resolvable(const WlGxTarget *target, uint32_t back_w, uint32_t back_h);
size_t wl_gx_target_bytes(const WlGxTarget *target);
// The scissor a target is drawn within: its size, but no larger than the back buffer.
void wl_gx_target_scissor_size(const WlGxTarget *target, uint32_t back_w, uint32_t back_h, uint32_t *w, uint32_t *h);

// The part of the EFB the back buffer is: parked and put back, the bounds of a target's copy, the
// coordinates of the quads. Default: the video mode's framebuffer width by its EFB height.
void wl_gx_target_set_back_buffer_size(uint32_t width, uint32_t height);

// Where the parked colour and depth come from: two blocks of wl_gx_tex_bytes(WL_GX_TEX_RGBA8, w, h)
// bytes (the back buffer's size), 32-byte aligned, asked for at the first park. `alloc` returns NULL
// when it has none: the back buffer is then not parked (a target's draws go over it) and the next
// park asks again. Default: aligned_alloc and free. The blocks are given back to `release` by
// wl_gx_shutdown and when the back buffer's size changes.
typedef void *(*WlGxTargetAlloc)(size_t bytes);
typedef void (*WlGxTargetFree)(void *memory);
void wl_gx_target_set_allocator(WlGxTargetAlloc alloc, WlGxTargetFree release);

// The surface the next draws (and clears) are into: `target`, or the back buffer for NULL. A
// target that is not the owner already takes the EFB: the owner leaves it (a back buffer with
// something drawn into it is parked; a target is resolved) and `target` comes (the back buffer
// is put back, with the scissor of the whole of it; a target gets a scissor of its size). The
// owner at the start of a frame is the back buffer (wl_gx_begin_frame), and wl_gx_end_frame puts
// it back if a target has the EFB.
//
// Returns the target that was resolved by the change (the layer's copy of its descriptor, valid
// until the next call), or NULL when none was: the owner was the back buffer, was already the
// owner, or had nothing to copy to (counted in the stats' `skipped`).
//
// What this leaves in GX, for a caller that draws with GX itself: the scissor; and, after a back
// buffer is put back, everything wl_gx_target_clear leaves. Nothing else changes.
const WlGxTarget *wl_gx_target_use(const WlGxTarget *target);
// The owner (the layer's copy of its descriptor), or NULL for the back buffer.
const WlGxTarget *wl_gx_target_current(void);
// The caller is drawing into the owner now. Only the back buffer's is recorded: it decides whether
// a target taking the EFB parks it.
void wl_gx_target_drawn(void);

// Clears a rectangle of the owner (pixels from its top left, within its size) with a quad: its
// colour and alpha (`rgba`, 0xRRGGBBAA) where `colour`, its depth (`z`, 0 near to 1 far, clamped)
// where `depth`. The frame must have begun.
//
// Leaves in GX: the projection (an orthographic one over the back buffer's pixels), the viewport
// (all of the back buffer), the position matrix 0 (the identity, current), blending off, the alpha
// test always passing, culling off, the colour and alpha update as asked, the depth test always
// passing and writing only where `depth`, the vertex descriptor and format 0 (positions, and
// texture coordinate 0 for a restore), the texture coordinate generator 0, no colour channels, one
// or two TEV stages, and no indirect stages. The caller sets again what it needs from that.
void wl_gx_target_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int w, int h);

// The finished back buffer narrowed into columns [x, x + w) of itself, black on each side: the
// picture of a 4:3 game on a 16:9 screen (video_mode.h). For the end of the frame, once the back
// buffer owns the EFB again (wl_gx_end_frame does it before the display copy); the EFB's contents
// are replaced and its depth cleared. It borrows the memory a park keeps for the back buffer's
// colour (asked from the allocator at the first call if no park has; the same block, so a program
// that parks pays the 1.2 MiB once). Returns false, leaving the EFB as it was, for a bad range or
// when there is no memory. Leaves in GX what wl_gx_target_clear does, and the texture map 0.
bool wl_gx_target_squeeze(uint32_t x, uint32_t w);

void wl_gx_target_stats(WlGxTargetStats *out, bool reset);
