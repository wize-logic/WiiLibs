// SPDX-License-Identifier: MIT
// The recording backend and the descriptions (nv2a_record.h).

#include "wl/xbox/nv2a_record.h"

#include <stdio.h>
#include <string.h>

#include "wl/base/hash.h"
#include "wl/xbox/nv2a_texture.h"

static uint64_t digest_words(uint64_t hash, const WlNv2aKelvin *k, uint32_t first, uint32_t last) {
    uint32_t m;

    for (m = first; m <= last; m += 4) {
        uint32_t word = wl_nv2a_k(k, m);
        uint8_t bytes[4] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16), (uint8_t)(word >> 24)};

        hash = wl_fnv1a64_update(hash, bytes, 4);
    }
    return hash;
}

static uint64_t digest_u32(uint64_t hash, uint32_t word) {
    uint8_t bytes[4] = {(uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16), (uint8_t)(word >> 24)};

    return wl_fnv1a64_update(hash, bytes, 4);
}

static uint64_t digest_float(uint64_t hash, float value) {
    uint32_t bits;

    memcpy(&bits, &value, 4);
    return digest_u32(hash, bits);
}

uint64_t wl_nv2a_state_digest(const WlNv2aKelvin *k) {
    uint64_t hash = WL_FNV1A64_INIT;
    uint32_t words[WL_XBOX_PSH_WORDS];
    const uint32_t *program;
    unsigned long count, i;
    unsigned j;

    hash = digest_words(hash, k, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, WL_NV2A_K_SET_SURFACE_ZETA_OFFSET);
    hash = digest_words(hash, k, WL_NV2A_K_SET_CONTROL0, WL_NV2A_K_SET_FRONT_FACE);
    hash = digest_words(hash, k, WL_NV2A_K_SET_FOG_PARAMS, WL_NV2A_K_SET_FOG_PARAMS + 8);
    wl_nv2a_psh_words(k, words);
    for (j = 0; j < WL_XBOX_PSH_WORDS; j++) {
        hash = digest_u32(hash, words[j]);
    }
    hash = digest_words(hash, k, WL_NV2A_K_SET_TEXTURE_OFFSET, WL_NV2A_K_SET_TEXTURE_OFFSET + 0xFC);
    hash = digest_words(hash, k, WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE, WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE);
    hash = digest_words(hash, k, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 60);
    if (wl_nv2a_program_mode(k)) {
        wl_nv2a_program(k, &program, &count);
        for (i = 0; i < count * 4; i++) {
            hash = digest_u32(hash, program[i]);
        }
        for (i = 0; i < WL_NV2A_CONSTANTS; i++) {
            for (j = 0; j < 4; j++) {
                hash = digest_float(hash, k->constants[i][j]);
            }
        }
    } else {
        hash = digest_words(hash, k, WL_NV2A_K_SET_COMPOSITE_MATRIX, WL_NV2A_K_SET_COMPOSITE_MATRIX + 60);
    }
    return hash;
}

const char *wl_nv2a_primitive_name(uint32_t primitive) {
    static const char *const names[] = {"end",       "points",         "lines",        "line-loop",
                                        "line-strip", "triangles",      "tri-strip",    "tri-fan",
                                        "quads",      "quad-strip",     "polygon"};

    return primitive < sizeof names / sizeof names[0] ? names[primitive] : "?";
}

const char *wl_nv2a_texture_format_name(uint8_t color) {
    switch (color) {
    case WL_NV2A_TEX_SZ_Y8: return "Y8";
    case WL_NV2A_TEX_SZ_AY8: return "AY8";
    case WL_NV2A_TEX_SZ_A1R5G5B5: return "A1R5G5B5";
    case WL_NV2A_TEX_SZ_X1R5G5B5: return "X1R5G5B5";
    case WL_NV2A_TEX_SZ_A4R4G4B4: return "A4R4G4B4";
    case WL_NV2A_TEX_SZ_R5G6B5: return "R5G6B5";
    case WL_NV2A_TEX_SZ_A8R8G8B8: return "A8R8G8B8";
    case WL_NV2A_TEX_SZ_X8R8G8B8: return "X8R8G8B8";
    case WL_NV2A_TEX_SZ_I8_A8R8G8B8: return "P8";
    case WL_NV2A_TEX_L_DXT1: return "DXT1";
    case WL_NV2A_TEX_L_DXT23: return "DXT3";
    case WL_NV2A_TEX_L_DXT45: return "DXT5";
    case WL_NV2A_TEX_LU_A1R5G5B5: return "LIN_A1R5G5B5";
    case WL_NV2A_TEX_LU_R5G6B5: return "LIN_R5G6B5";
    case WL_NV2A_TEX_LU_A8R8G8B8: return "LIN_A8R8G8B8";
    case WL_NV2A_TEX_LU_Y8: return "LIN_Y8";
    case WL_NV2A_TEX_SZ_A8: return "A8";
    case WL_NV2A_TEX_SZ_A8Y8: return "A8Y8";
    case WL_NV2A_TEX_LU_AY8: return "LIN_AY8";
    case WL_NV2A_TEX_LU_X1R5G5B5: return "LIN_X1R5G5B5";
    case WL_NV2A_TEX_LU_A4R4G4B4: return "LIN_A4R4G4B4";
    case WL_NV2A_TEX_LU_X8R8G8B8: return "LIN_X8R8G8B8";
    case WL_NV2A_TEX_LU_A8: return "LIN_A8";
    case WL_NV2A_TEX_LU_A8Y8: return "LIN_A8Y8";
    case WL_NV2A_TEX_LC_CR8YB8CB8YA8: return "YUY2";
    case WL_NV2A_TEX_LC_YB8CR8YA8CB8: return "UYVY";
    default: return "other";
    }
}

static const char *func_name(uint32_t f) {
    static const char *const names[8] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};

    return f >= WL_NV2A_FUNC_NEVER && f <= WL_NV2A_FUNC_ALWAYS ? names[f - WL_NV2A_FUNC_NEVER] : "?";
}

size_t wl_nv2a_describe_state(const WlNv2aKelvin *k, char *buf, size_t cap) {
    WlNv2aSurface s;
    int n;

    wl_nv2a_surface(k, &s);
    n = snprintf(buf, cap,
                 "surface %ux%u+%u+%u colour %08x fmt %u pitch %u zeta %08x fmt %u%s%s | blend %s %04x/%04x eq %04x | "
                 "z %s %s write %u | alpha %s %s %u | cull %s %03x front %03x | mask %08x | fog %u mode %04x | %s",
                 (unsigned)s.clip_width, (unsigned)s.clip_height, (unsigned)s.clip_x, (unsigned)s.clip_y,
                 (unsigned)s.color_phys, (unsigned)s.color_format, (unsigned)s.color_pitch, (unsigned)s.zeta_phys,
                 (unsigned)s.zeta_format, s.swizzled ? " swizzled" : "", s.samples_x > 1 ? " multisampled" : "",
                 wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_ENABLE) ? "on" : "off",
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_EQUATION),
                 wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_TEST_ENABLE) ? "on" : "off",
                 func_name(wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_FUNC)), (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK),
                 wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_TEST_ENABLE) ? "on" : "off",
                 func_name(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_FUNC)), (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF),
                 wl_nv2a_k(k, WL_NV2A_K_SET_CULL_FACE_ENABLE) ? "on" : "off",
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_CULL_FACE), (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_FRONT_FACE),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_MASK), (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_FOG_ENABLE),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_FOG_MODE),
                 wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_TEST_ENABLE) ? "STENCIL" : "no stencil");
    if (n >= 0 && (size_t)n < cap && wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_TEST_ENABLE)) {
        // (the test and what it writes: func ref&mask, write mask, ops fail/zfail/zpass)
        int more = snprintf(buf + n, cap - (size_t)n, " %s %u&%02x write %02x ops %04x/%04x/%04x",
                            func_name(wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_FUNC)),
                            (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_FUNC_REF),
                            (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_FUNC_MASK),
                            (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_MASK),
                            (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_OP_FAIL),
                            (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_OP_ZFAIL),
                            (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_STENCIL_OP_ZPASS));
        n = more < 0 ? n : n + more;
    }
    return n < 0 ? 0 : (size_t)n;
}

size_t wl_nv2a_describe_draw(const WlNv2aKelvin *k, const WlNv2aDraw *draw, char *buf, size_t cap) {
    static const char *const sources[3] = {"arrays", "inline", "immediate"};
    size_t used = 0;
    unsigned stage;
    int n;

#define APPEND(...)                                                                                       \
    do {                                                                                                  \
        n = snprintf(buf + used, used < cap ? cap - used : 0, __VA_ARGS__);                              \
        if (n > 0) {                                                                                      \
            used += (size_t)n;                                                                            \
        }                                                                                                 \
    } while (0)

    APPEND("draw %u: %s x%u %s", (unsigned)draw->serial, wl_nv2a_primitive_name(draw->primitive),
           (unsigned)draw->count, draw->source < 3 ? sources[draw->source] : "?");
    if (draw->source == WL_NV2A_SOURCE_ARRAYS) {
        APPEND(" [%u..%u]", (unsigned)draw->index_base, (unsigned)draw->index_last);
    }
    if (wl_nv2a_program_mode(k)) {
        const uint32_t *program;
        unsigned long count;

        wl_nv2a_program(k, &program, &count);
        APPEND(" | program %lu at %u (%08lx)", count, (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_TRANSFORM_PROGRAM_START),
               wl_xbox_vsh_hash(program, count));
    } else {
        APPEND(" | FIXED FUNCTION");
    }
    APPEND(" | combiners %u modes %05x", (unsigned)(wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_CONTROL) & 0xFFu),
           (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM));
    for (stage = 0; stage < WL_NV2A_TEXTURES; stage++) {
        WlNv2aTexture t;

        if (wl_nv2a_stage_program(k, stage) == 0 || !wl_nv2a_texture(k, stage, &t)) {
            continue;
        }
        APPEND(" | t%u %ux%u %s%s @%08x", stage, (unsigned)t.width, (unsigned)t.height,
               wl_nv2a_texture_format_name(t.color), t.cube ? " cube" : "", (unsigned)t.phys);
    }
    APPEND(" | blend %s z %s%s alpha %s cull %s", wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_ENABLE) ? "on" : "off",
           wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_TEST_ENABLE) ? func_name(wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_FUNC)) : "off",
           wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK) ? "+w" : "",
           wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_TEST_ENABLE) ? func_name(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_FUNC)) : "off",
           wl_nv2a_k(k, WL_NV2A_K_SET_CULL_FACE_ENABLE) ? "on" : "off");
#undef APPEND
    return used;
}

// ---- the backend ------------------------------------------------------------------------------

static void say(WlNv2aRecorder *r, const char *line) {
    if (r->print != NULL) {
        r->print(r->print_user, line);
    }
}

static void record_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    WlNv2aRecorder *r = user;
    char line[256];

    r->clears++;
    r->digest = digest_u32(r->digest, 0xC1EA0000u | flags);
    r->digest = digest_words(r->digest, k, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL);
    r->digest = digest_words(r->digest, k, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, WL_NV2A_K_SET_SURFACE_ZETA_OFFSET);
    if (r->verbosity >= 1) {
        snprintf(line, sizeof line, "clear %02x colour %08x z %08x rect %08x %08x", (unsigned)flags,
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_COLOR_CLEAR_VALUE),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL),
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL));
        say(r, line);
    }
}

static uint32_t record_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    WlNv2aRecorder *r = user;
    float inputs[WL_NV2A_ATTRIBS][4];
    bool run = r->run_programs && wl_nv2a_pipeline_begin(&r->pipeline, k);
    uint32_t v;
    unsigned a, c;

    r->draws++;
    r->vertices += draw->count;
    r->digest = digest_u32(r->digest, 0xD0000000u | (uint32_t)draw->primitive << 8 | draw->source);
    r->digest = digest_u32(r->digest, draw->count);
    r->digest ^= wl_nv2a_state_digest(k);
    for (v = 0; v < draw->count; v++) {
        wl_nv2a_draw_fetch(draw, k, v, inputs);
        if (run) {
            WlXboxVshOutput out;

            memset(&out, 0, sizeof out);
            wl_nv2a_pipeline_vertex(&r->pipeline, k, (const float (*)[4])inputs, WL_XBOX_VSH_LIVE_ALL, &out);
            for (c = 0; c < 4; c++) {
                r->digest = digest_float(r->digest, out.position[c]);
                r->digest = digest_float(r->digest, out.d0[c]);
                r->digest = digest_float(r->digest, out.d1[c]);
                for (a = 0; a < 4; a++) {
                    r->digest = digest_float(r->digest, out.t[a][c]);
                }
            }
            r->digest = digest_float(r->digest, out.fog);
        } else {
            for (a = 0; a < WL_NV2A_ATTRIBS; a++) {
                if (draw->streamed & (1u << a)) {
                    for (c = 0; c < 4; c++) {
                        r->digest = digest_float(r->digest, inputs[a][c]);
                    }
                }
            }
        }
    }
    if (r->verbosity >= 1) {
        char line[512];

        wl_nv2a_describe_draw(k, draw, line, sizeof line);
        say(r, line);
        if (r->verbosity >= 2) {
            wl_nv2a_describe_state(k, line, sizeof line);
            say(r, line);
        }
    }
    return 0;
}

static void record_frame(void *user, const WlNv2aKelvin *k) {
    WlNv2aRecorder *r = user;
    char line[96];

    r->frames++;
    r->digest = digest_u32(r->digest, 0xF4A3E000u);
    if (r->verbosity >= 1) {
        snprintf(line, sizeof line, "frame %u done (colour %08x)", (unsigned)r->frames,
                 (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET));
        say(r, line);
    }
}

static void record_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    WlNv2aRecorder *r = user;
    char line[96];

    (void)k;
    r->flips++;
    r->digest = digest_u32(r->digest, 0xF1100000u);
    r->digest = digest_u32(r->digest, phys);
    if (r->verbosity >= 1) {
        snprintf(line, sizeof line, "flip %u to %08x", (unsigned)r->flips, (unsigned)phys);
        say(r, line);
    }
}

void wl_nv2a_recorder_init(WlNv2aRecorder *r) {
    memset(r, 0, sizeof *r);
    r->digest = WL_FNV1A64_INIT;
    r->run_programs = true;
    wl_nv2a_pipeline_init(&r->pipeline, NULL, 0);
}

void wl_nv2a_recorder_backend(WlNv2aRecorder *r, WlNv2aBackend *out) {
    memset(out, 0, sizeof *out);
    out->user = r;
    out->clear = record_clear;
    out->draw = record_draw;
    out->frame_done = record_frame;
    out->flip = record_flip;
}
