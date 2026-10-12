// SPDX-License-Identifier: MIT
// A backend that draws nothing and records everything (nv2a_record.c): each clear, draw, frame and
// flip folded into a digest (the draw's state and its vertices as the vertex program leaves them,
// or as they come when the programs are not run), counted, and described in a line of text for a
// log or a tool (tools/xbox_nv2a_replay.c). For tests on the host and ppc-be: two runs that give
// the same digest drew the same things.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_kelvin.h"
#include "wl/xbox/nv2a_pipeline.h"

typedef struct WlNv2aRecorder {
    // run each vertex through the program for the digest (else the vertices' inputs are digested)
    bool run_programs;
    // a line for each event (0: none, 1: clears, draws, frames and flips, 2: and each draw's state)
    unsigned verbosity;
    void (*print)(void *user, const char *line);
    void *print_user;
    uint64_t digest;
    uint32_t clears, draws, frames, flips;
    uint64_t vertices;
    // (the programs, when they are run)
    WlNv2aPipeline pipeline;
} WlNv2aRecorder;

// A recorder (the caller's: it holds the programs it compiles).
void wl_nv2a_recorder_init(WlNv2aRecorder *r);
// The backend that records into r.
void wl_nv2a_recorder_backend(WlNv2aRecorder *r, WlNv2aBackend *out);

// A digest of the state a draw is drawn with: the surface, the raster state, the combiners, the
// textures' registers, the transform mode, the program's words and the constants.
uint64_t wl_nv2a_state_digest(const WlNv2aKelvin *k);
// A line describing a draw: its primitive, vertex count and source, the program, the combiners'
// stage count and texture modes, each bound texture (size, format, address), blending, depth,
// alpha test and culling. Returns the length written (snprintf's).
size_t wl_nv2a_describe_draw(const WlNv2aKelvin *k, const WlNv2aDraw *draw, char *buf, size_t cap);
// The surface and the raster state, a line.
size_t wl_nv2a_describe_state(const WlNv2aKelvin *k, char *buf, size_t cap);
// names, for describing
const char *wl_nv2a_primitive_name(uint32_t primitive);
const char *wl_nv2a_texture_format_name(uint8_t color);
