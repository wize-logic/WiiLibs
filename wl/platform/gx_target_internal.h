// SPDX-License-Identifier: MIT
// The backend half of gx_target.h: what gx_target.c asks of the GPU (wii/gx_target_wii.c) or of
// the host stub (host/gx_target_host.c). Not for applications.
#pragma once

#include "wl/platform/gx_target.h"

typedef enum {
    WL_GX_TARGET_COPY_RGBA8 = 0, // the EFB's colour and alpha as an RGBA8 texture
    WL_GX_TARGET_COPY_RGB565,    // ...as RGB565
    WL_GX_TARGET_COPY_Z24X8      // the EFB's depth as a Z24X8 texture
} WlGxTargetCopy;

// Copies out of the EFB's origin, `w` by `h` pixels, unfiltered (the display copy's deflicker
// filter is switched off for a run of copies and back on after). The destination is 32-byte
// aligned memory of wl_gx_tex_bytes bytes; the texture cache is invalidated after the run.
void wl_gx_target_be_copy_begin(void);
void wl_gx_target_be_copy(void *dst, uint32_t w, uint32_t h, WlGxTargetCopy what);
void wl_gx_target_be_copy_end(void);
// The scissor: (0, 0, w, h).
void wl_gx_target_be_scissor(uint32_t w, uint32_t h);
// The parked colour (RGBA8) and depth (Z24X8), `w` by `h` from the EFB's origin, written back over the
// EFB by a quad of that part of a `back_w` by `back_h` back buffer, depth test always passing and
// writing. Leaves the viewport and projection over the whole back buffer, as a clear does.
void wl_gx_target_be_restore(const void *colour, const void *depth, uint32_t w, uint32_t h, uint32_t back_w,
                             uint32_t back_h);
// A clear quad, as wl_gx_target_clear describes (z already clamped); `w`, `h` the back buffer's size.
void wl_gx_target_be_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int rw, int rh,
                           uint32_t back_w, uint32_t back_h);

// The finished back buffer narrowed into columns [x, x + w) of itself with black around it: the EFB
// copied (unfiltered) into `colour` (a block of wl_gx_tex_bytes(RGBA8, back_w, back_h) bytes) and
// cleared to black by the same copy, then drawn back as one linearly filtered quad. Nothing of the
// back buffer is kept; for the end of the frame.
void wl_gx_target_be_squeeze(void *colour, uint32_t back_w, uint32_t back_h, uint32_t x, uint32_t w);

// The video mode's framebuffer width and EFB height, the default back buffer.
void wl_gx_target_be_default_size(uint32_t *w, uint32_t *h);

// Called by the GX layer's backends.
void wl_gx_target_frame_begun(void); // wl_gx_begin_frame: the back buffer owns the EFB, nothing drawn
void wl_gx_target_frame_ending(void); // wl_gx_end_frame: the back buffer is put back
void wl_gx_target_shutdown(void);    // wl_gx_shutdown: the parked blocks are released
