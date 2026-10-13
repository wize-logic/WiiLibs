// SPDX-License-Identifier: MIT
// Synthetic NV2A frames the backend tests share (the host's GX recorder, the GX backend drawn by the
// host's software GX, the Wii console test): push buffers as a title's Direct3D would write them for a 640x480 A8R8G8B8 back
// buffer with a Z24S8 depth, vertex programs (assembled with tests/xbox/vsh_programs.py's assembler),
// combiner programs, textures in guest memory, immediate vertices. Needs nv2a_test_util.h first.
#pragma once

#include <stdint.h>

// mov oPos, v0; mov oD0, v3; mov oT0, v9: vertices in screen space
static const uint32_t f_screen_program[][4] = {
    {0x00000000, 0x0020001B, 0x0836006C, 0x0000F800}, // mov oPos, v0
    {0x00000000, 0x0020061B, 0x0836006C, 0x0000F818}, // mov oD0, v3
    {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849}, // mov oT0, v9
};
// the position by c[0..3], captured, divided by w and moved to the viewport (c[-38], c[-37]), as
// Direct3D's programs end; the colour and coordinates passed
static const uint32_t f_clip_program[][4] = {
    {0x00000000, 0x00EC001B, 0x0836186C, 0x08C00000}, // dp4 r12.x, v0, c[0]
    {0x00000000, 0x00EC201B, 0x0836186C, 0x04C00000}, // dp4 r12.y, v0, c[1]
    {0x00000000, 0x00EC401B, 0x0836186C, 0x02C00000}, // dp4 r12.z, v0, c[2]
    {0x00000000, 0x00EC601B, 0x0836186C, 0x01C00000}, // dp4 r12.w, v0, c[3]
    {0x00000000, 0x0600001B, 0x003603FF, 0x10180000}, // ircc r1.x, r12.w
    {0x00000000, 0x0040001B, 0xC400286C, 0x0EC00000}, // mul r12.xyz, r12, r1.x
    {0x00000000, 0x0027601B, 0x0C36006C, 0x0F200000}, // mov r2, c[-37]
    {0x00000000, 0x0087401B, 0xC436186C, 0x9000E800}, // mad oPos.xyz, r12, c[-38], r2
    {0x00000000, 0x0020061B, 0x0836006C, 0x0000F818}, // mov oD0, v3
    {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849}, // mov oT0, v9
};

enum {
    F_TEXTURE = 0x60000,   // a 4x4 A8R8G8B8 texture (grey, or what a test writes)
    F_TEXTURE2 = 0x61000,  // a 16x16 A4R4G4B4 texture (a glyph cache's format)
    F_COLOR = 0x100000,    // the back buffer's address (the backends never read it)
    F_ZETA = 0x200000
};

static inline void f_program(TGuest *g, uint32_t slot, const uint32_t (*program)[4], unsigned count) {
    unsigned i, w;
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, slot);
    for (i = 0; i < count; i++) {
        t_method(g, WL_NV2A_K_SET_TRANSFORM_PROGRAM, 4);
        for (w = 0; w < 4; w++) {
            t_word(g, program[i][w]);
        }
    }
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_START, slot);
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE, WL_NV2A_XF_PROGRAM);
}

static inline void f_constant(TGuest *g, uint32_t index, float x, float y, float z, float w) {
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, index);
    t_method(g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, 4);
    t_word(g, t_f(x));
    t_word(g, t_f(y));
    t_word(g, t_f(z));
    t_word(g, t_f(w));
}

// Direct3D's viewport for the 640x480 back buffer (the constants c[58], c[59] through the methods)
static inline void f_viewport(TGuest *g) {
    t_method(g, WL_NV2A_K_SET_VIEWPORT_SCALE, 4);
    t_word(g, t_f(320.0f));
    t_word(g, t_f(-240.0f));
    t_word(g, t_f(16777215.0f));
    t_word(g, t_f(0.0f));
    t_method(g, WL_NV2A_K_SET_VIEWPORT_OFFSET, 4);
    t_word(g, t_f(320.0f));
    t_word(g, t_f(240.0f));
    t_word(g, t_f(0.0f));
    t_word(g, t_f(0.0f));
}

// one combiner stage r0 = t0 * v0 (colour and alpha), the final combiner's output r0
static inline void f_combiner_texture_times_colour(TGuest *g) {
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_ICW, 0x08040000u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_OCW, 0x000000C0u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_ICW, 0x18140000u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_OCW, 0x000000C0u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0, 0x0000000Cu);
    t_m1(g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001C00u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_CONTROL, 1);
}

// two stages: r0 = t0 * c0, r1 = r0 + v0 * c1 (c0 and c1 the stage's constants, ARGB); output r1
static inline void f_combiner_two_stages(TGuest *g, uint32_t c0, uint32_t c1) {
    // stage 0: A = t0, B = c0 -> r0 (AB); stage 1: A = r0, B = 1 (zero inverted), C = v0, D = c1 ->
    // the sum to r1
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_ICW, 0x08010000u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_OCW, 0x000000C0u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_ICW, 0x18110000u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_OCW, 0x000000C0u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_ICW + 4, 0x0C200402u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_OCW + 4, 0x00000D00u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_ICW + 4, 0x1C301412u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_OCW + 4, 0x00000D00u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_FACTOR0, c0);
    t_m1(g, WL_NV2A_K_SET_COMBINER_FACTOR1 + 4, c1);
    t_m1(g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0, 0x0000000Du);
    t_m1(g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001D00u);
    // two stages, each its own constants (factor 0 and 1 per stage)
    t_m1(g, WL_NV2A_K_SET_COMBINER_CONTROL, 2u | 1u << 12 | 1u << 16);
}

// a 2D texture on stage 0 (format word's colour, log2 sizes), sampled nearest
static inline void f_texture(TGuest *g, uint32_t offset, uint32_t color, uint32_t log_w, uint32_t log_h) {
    t_m1(g, WL_NV2A_K_SET_TEXTURE_OFFSET, offset);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_FORMAT, 1u | 2u << 4 | color << 8 | 1u << 16 | log_w << 20 | log_h << 24);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_CONTROL0, 1u << 30);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_ADDRESS, WL_NV2A_WRAP_CLAMP_TO_EDGE | WL_NV2A_WRAP_CLAMP_TO_EDGE << 8);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_FILTER, WL_NV2A_FILTER_NEAREST << 16 | WL_NV2A_FILTER_NEAREST << 24);
    t_m1(g, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM, WL_NV2A_STAGE_2D_PROJECTIVE);
}

// the back buffer, the texture-times-colour combiner, the depth test
static inline void f_frame_setup(TGuest *g) {
    unsigned i;

    for (i = 0; i < 16; i++) {
        t_w32(g, F_TEXTURE + 4 * i, 0xFF808080u);
    }
    t_m1(g, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, 640u << 16);
    t_m1(g, WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL, 480u << 16);
    t_m1(g, WL_NV2A_K_SET_SURFACE_FORMAT,
         WL_NV2A_SURFACE_COLOR_A8R8G8B8 | WL_NV2A_SURFACE_ZETA_Z24S8 << 4 | WL_NV2A_SURFACE_TYPE_PITCH << 8);
    t_m1(g, WL_NV2A_K_SET_SURFACE_PITCH, 2560u << 16 | 2560u);
    t_m1(g, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET, F_COLOR);
    t_m1(g, WL_NV2A_K_SET_SURFACE_ZETA_OFFSET, F_ZETA);
    f_combiner_texture_times_colour(g);
    f_texture(g, F_TEXTURE, WL_NV2A_TEX_SZ_A8R8G8B8, 2, 2);
    t_m1(g, WL_NV2A_K_SET_COLOR_MASK, 0x01010101u);
    t_m1(g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 1);
    t_m1(g, WL_NV2A_K_SET_DEPTH_FUNC, WL_NV2A_FUNC_LEQUAL);
    t_m1(g, WL_NV2A_K_SET_DEPTH_MASK, 1);
    t_m1(g, WL_NV2A_K_SET_BLEND_ENABLE, 0);
    t_m1(g, WL_NV2A_K_SET_CULL_FACE_ENABLE, 0);
    f_viewport(g);
}

// a clear of all of the back buffer: colour (ARGB) and depth (far)
static inline void f_clear(TGuest *g, uint32_t argb) {
    t_m1(g, WL_NV2A_K_SET_COLOR_CLEAR_VALUE, argb);
    t_m1(g, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00u);
    t_m1(g, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL, 639u << 16);
    t_m1(g, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL, 479u << 16);
    t_m1(g, WL_NV2A_K_CLEAR_SURFACE, WL_NV2A_CLEAR_COLOR | WL_NV2A_CLEAR_Z | WL_NV2A_CLEAR_STENCIL);
}

// an immediate vertex: texture coordinate (2F), colour (4UB: bytes r, g, b, a in the word's order
// from its low byte), position (4F, completes it)
static inline void f_vertex(TGuest *g, float x, float y, float z, float w, float s, float t, uint32_t abgr) {
    t_method(g, WL_NV2A_K_SET_VERTEX_DATA2F_M + 8 * 9, 2);
    t_word(g, t_f(s));
    t_word(g, t_f(t));
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA4UB + 4 * 3, abgr);
    t_method(g, WL_NV2A_K_SET_VERTEX_DATA4F_M, 4);
    t_word(g, t_f(x));
    t_word(g, t_f(y));
    t_word(g, t_f(z));
    t_word(g, t_f(w));
}

// a screen-space quad (x0, y0)-(x1, y1) at depth z (0..1 of the depth range), one colour, the
// texture over it
static inline void f_quad(TGuest *g, float x0, float y0, float x1, float y1, float z, uint32_t abgr) {
    float depth = z * 16777215.0f;

    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
    f_vertex(g, x0, y0, depth, 1, 0, 0, abgr);
    f_vertex(g, x1, y0, depth, 1, 1, 0, abgr);
    f_vertex(g, x1, y1, depth, 1, 1, 1, abgr);
    f_vertex(g, x0, y1, depth, 1, 0, 1, abgr);
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
}

// the clip program's transform: clip = (x, y, z / 2, z), w the depth along z
static inline void f_perspective(TGuest *g) {
    f_program(g, 8, f_clip_program, 10);
    f_constant(g, 96, 1, 0, 0, 0);
    f_constant(g, 97, 0, 1, 0, 0);
    f_constant(g, 98, 0, 0, 0.5f, 0);
    f_constant(g, 99, 0, 0, 1, 0);
}
