// SPDX-License-Identifier: MIT
// A software reference of the NV2A backend on GX (nv2a_raster.c): it reads the same state the GX
// backend reads and draws it the way GX would be asked to, on the processor, into RGBA8 surfaces:
// the vertices through the same pipeline (nv2a_pipeline.h), the same converted textures
// (nv2a_texcache.h, decoded back from GX's formats), the combiners as the same TEV program
// (wl/xbox/psh, evaluated per pixel with wl_tev_eval), GX's blending, depth and alpha test. For the
// host: a capture replayed into pictures (tools/xbox_nv2a_replay.c) without a Wii, and tests of the
// whole path from push buffer to pixel.
//
// Not GX bit for bit (its interpolation and filtering are the textbook's, perspective-correct for
// a program that divides by w), and like the GX backend it has no stencil.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/xbox/nv2a.h"

typedef struct WlNv2aRaster WlNv2aRaster;

typedef struct WlNv2aRasterConfig {
    // a frame shown (FLIP_INCREMENT_WRITE's surface, or the display flipped to one): its pixels,
    // RGBA8 rows (may be NULL)
    void (*frame)(void *user, uint32_t phys, const uint8_t *rgba, uint32_t width, uint32_t height);
    void *user;
    // bilinear filtering where the sampler asks for it (else nearest everywhere)
    bool bilinear;
} WlNv2aRasterConfig;

typedef struct WlNv2aRasterStats {
    uint32_t draws, triangles, culled, pixels, clears, frames;
    uint32_t stencil_skipped, programs_unusable;
} WlNv2aRasterStats;

WlNv2aRaster *wl_nv2a_raster_create(const WlNv2aRasterConfig *cfg);
void wl_nv2a_raster_destroy(WlNv2aRaster *r);
void wl_nv2a_raster_attach(WlNv2aRaster *r, WlNv2a *dev);
void wl_nv2a_raster_backend(WlNv2aRaster *r, WlNv2aBackend *out);
// A surface's pixels (RGBA8 rows) by its colour address, NULL when nothing was drawn there.
const uint8_t *wl_nv2a_raster_surface(const WlNv2aRaster *r, uint32_t phys, uint32_t *width, uint32_t *height);
void wl_nv2a_raster_stats(const WlNv2aRaster *r, WlNv2aRasterStats *out);
