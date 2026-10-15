// SPDX-License-Identifier: MIT
// Replays a capture of the NV2A model (wl/xbox/nv2a_capture.h: a title's push buffers and the guest
// memory they read, a frame or a few) through the model on the host, and says what it drew: each
// clear, draw (primitive, vertices, program, combiners, textures, raster state), frame and flip, the
// model's counts (methods not handled, stencil and fixed-function draws, memory it could not read),
// and, with --png-gx, each frame the GX backend (wl/xbox/nv2a_gx.h) would show on the Wii, drawn by
// the host's software GX (wl/gxsoft) as a PNG.
//
// usage: xbox_nv2a_replay CAPTURE [-v | -vv] [--png-gx DIR] [--efb-after N DIR] [--gx-dump N]
//                          [--gx-textures DIR] [--psh-dump N] [--programs FILE] [--stream]
//   -v          a line for each clear, draw, frame and flip (with --png-gx, what each GX primitive
//               covered and wrote); -vv also each draw's raster state
//   --png-gx    DIR/frame_NNNN.png for each frame GX shows (the display copy of the EFB)
//   --efb-after the EFB after the GX backend's draw N (counted from 0 in the replay) as
//               DIR/efb_after_N.png, to see what one draw did (may be given several times)
//   --gx-dump   what GX is handed for its primitive N (counted as -v's "gx N" lines): the
//               projection, viewport, scissor, raster state, TEV program and vertices (may be given
//               several times)
//   --gx-textures DIR  with --gx-dump, each texture map of the primitive dumped as
//               DIR/gx_N_map_M.png (its first level, decoded from GX's format)
//   --psh-dump  the combiner words (psh.h's 57, in hex) of the replay's draw N, and what psh made of
//               them (may be given several times)
//   --programs  each vertex program drawn with, once, as tools/xbox_vsh_codegen.c's input (C text:
//               a header word with the instruction count in its high half, four words an
//               instruction), for compiling a title's programs ahead of time
//   --textures-mb N  the GX backend's converted textures in an arena of N MiB, as the runtime
//               keeps them (without it: the backend's default budget, from the heap)
//   --vertex-cache-kb N  the GX backend's vertex arrays kept for GX to read by index
//               (nv2a_vcache.h) in an arena of N KiB (without it: none kept)
//   --watch     the converted textures and vertex arrays checked through a write watch, as the
//               runtime's MMU gives it on the Wii: a page counts as stored to when one of the
//               capture's memory blocks lands in it (the blocks are what the title wrote, as the
//               capture read it); with --bench too
//   --cpu-transform  every vertex through the program on the processor (GX's transform unit not
//               used for the draws whose outputs are affine), to the bit as before it
//   --bench N   the capture replayed N times through the GX backend alone (no software GX, no
//               recorder): the processor's part of the frames, timed (the best run and the mean,
//               a frame), and the best run's parts: the model's (pusher, puller, 3D state), the
//               backend's draws, its clears and frames, the memory records put in
//   --xf-programs  with --bench: the draws and vertices of each way (wl_nv2a_gx_last_xf) by
//               vertex program (its hash), the most vertices first (each draw's program hashed:
//               not for timing)
//   --stream    the replay through wl_nv2a_replay_stream, as the runtime's capture bench makes it
//   --png-alpha the frames and EFB pictures written with the EFB's destination alpha (else opaque,
//               as a screen shows them; --gx-textures keeps the maps' alpha either way)
//   --survey    which of the NV2A's features the capture's draws use that GX takes approximately
//               (the combiners' blues to alpha and muxes, the texture stages' modes, alpha kill,
//               signed channels and LOD bias, blend equations and constant factors, psh's
//               approximations and refusals): how many draws, and the first of each
//               (wl/xboxrt/bench.h): the capture read a piece at a time (of an odd size, so records
//               straddle the pieces), into a guest memory given as the caller's, written a 4 KB
//               piece at a time where it differs; the output is the same as without
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wl/base/endian.h"
#include "wl/codec/gx_tex.h"
#include "wl/gxsoft/gxsoft.h"
#include "wl/gxtex/gxtex.h"
#include "wl/platform/host/gx_direct_host.h"
#include "wl/platform/host/gx_renderer_host.h"
#include "wl/platform/host/tev_host.h"
#include "wl/platform/platform.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_gx.h"
#include "wl/xbox/nv2a_pipeline.h"
#include "wl/xbox/nv2a_record.h"

enum { BACKENDS = 2, EFB_AFTER_MAX = 64 };

typedef struct Tee {
    WlNv2aBackend b[BACKENDS];
    unsigned count;
} Tee;

static FILE *g_programs;
static bool g_xf_programs;
static unsigned g_bench;
static bool g_stream;

// a title's vertex programs compiled ahead of time, when a build of this tool links them (the C
// wl_add_xbox_title generates, vsh_natives.c; weak: WiiLibs' own build has none): the GX backend
// runs them, the recorder's digest stays the interpreter's
extern const WlXboxVshNative *wl_xb_title_vsh_natives(unsigned long *count) __attribute__((weak));

// the GX backend's settings: main's (--textures-mb, --watch) and the compiled programs
static WlNv2aGxConfig g_gcfg;

static const WlNv2aGxConfig *gx_config(void) {
    if (wl_xb_title_vsh_natives != NULL) {
        g_gcfg.natives = wl_xb_title_vsh_natives(&g_gcfg.native_count);
        fprintf(stderr, "%lu vertex programs compiled ahead of time\n", g_gcfg.native_count);
    }
    return &g_gcfg;
}
static unsigned long g_program_hashes[1024];
static unsigned g_program_count;
static unsigned g_verbosity;
// --survey: each feature's name, its draws and the first of them
enum { SURVEY_MAX = 96 };
static bool g_survey;
static struct {
    char name[96];
    uint32_t draws, first;
} g_features[SURVEY_MAX];
static unsigned g_feature_count;
static const char *g_png_dir;
static unsigned g_png_count;
static WlNv2aRecorder g_recorder;
static WlNv2aGx *g_gx;
static uint32_t g_backend_draws;
// the write watch over the replay's guest memory (--watch): each page's epoch of its last block
static uint32_t *g_page_epoch;
static uint32_t g_epoch = 1, g_page_count;
static struct {
    uint32_t draw;
    const char *dir;
} g_efb_after[EFB_AFTER_MAX];
static unsigned g_efb_after_count;
static uint32_t g_gx_dump[EFB_AFTER_MAX];
static unsigned g_gx_dump_count;
static const char *g_gx_textures_dir;
static uint32_t g_gx_primitives;
static uint32_t g_psh_dump[EFB_AFTER_MAX];
static unsigned g_psh_dump_count;

// a vertex program not met before, written out
static void program_seen(const WlNv2aKelvin *k) {
    const uint32_t *words;
    unsigned long count, hash, i;

    if (g_programs == NULL || !wl_nv2a_program_mode(k)) {
        return;
    }
    wl_nv2a_program(k, &words, &count);
    if (count == 0) {
        return;
    }
    hash = wl_xbox_vsh_hash(words, count);
    for (i = 0; i < g_program_count; i++) {
        if (g_program_hashes[i] == hash) {
            return;
        }
    }
    if (g_program_count < sizeof g_program_hashes / sizeof g_program_hashes[0]) {
        g_program_hashes[g_program_count++] = hash;
    }
    fprintf(g_programs, "/* program %u: %lu instructions, hash %08lx */\n\t0x%08lX,\n", g_program_count - 1, count,
            hash, (count << 16) | 0x2078ul);
    for (i = 0; i < count; i++) {
        fprintf(g_programs, "\t0x%08X, 0x%08X, 0x%08X, 0x%08X,\n", (unsigned)words[4 * i], (unsigned)words[4 * i + 1],
                (unsigned)words[4 * i + 2], (unsigned)words[4 * i + 3]);
    }
}

// a picture of the screen or the EFB: opaque, as a screen shows it (the EFB's destination alpha,
// which blending reads, is not the picture), unless --png-alpha keeps it
static bool g_png_alpha;

static bool png_write_as(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, bool opaque) {
    uint8_t *png = NULL, *copy = NULL;
    size_t len = 0, bytes = (size_t)w * h * 4u, i;
    FILE *f;
    bool ok;
    WlResult encoded;

    if (opaque && !g_png_alpha) {
        copy = malloc(bytes);
        if (copy == NULL) {
            fprintf(stderr, "png: no memory for %s\n", path);
            return false;
        }
        memcpy(copy, rgba, bytes);
        for (i = 3; i < bytes; i += 4) {
            copy[i] = 0xFF;
        }
        rgba = copy;
    }
    encoded = wl_png_encode(rgba, w, h, &png, &len);
    free(copy);
    if (encoded != WL_OK) {
        fprintf(stderr, "png: cannot encode %s\n", path);
        return false;
    }
    f = fopen(path, "wb");
    ok = f != NULL && fwrite(png, 1, len, f) == len;
    if (!ok) {
        fprintf(stderr, "png: cannot write %s\n", path);
    }
    if (f != NULL) {
        fclose(f);
    }
    free(png);
    return ok;
}

static bool png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    return png_write_as(path, rgba, w, h, true);
}

static void tee_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    Tee *t = user;
    unsigned i;

    for (i = 0; i < t->count; i++) {
        t->b[i].clear(t->b[i].user, k, flags);
    }
}

static void survey_count(const char *name) {
    unsigned i;

    for (i = 0; i < g_feature_count; i++) {
        if (strcmp(g_features[i].name, name) == 0) {
            g_features[i].draws++;
            return;
        }
    }
    if (g_feature_count < SURVEY_MAX) {
        snprintf(g_features[g_feature_count].name, sizeof g_features[0].name, "%s", name);
        g_features[g_feature_count].draws = 1;
        g_features[g_feature_count++].first = g_backend_draws;
    }
}

// what of a draw's state GX takes approximately, counted by name
static void survey_draw(const WlNv2aKelvin *k) {
    static const char *const equations[] = {"add", "min", "max", "?", "subtract", "reverse subtract"};
    uint32_t words[WL_XBOX_PSH_WORDS];
    char name[96];
    unsigned stage, count;

    wl_nv2a_psh_words(k, words);
    count = words[WL_XBOX_PSH_COMBINER_COUNT] & 0xffu;
    for (stage = 0; stage < count && stage < 8; stage++) {
        uint32_t rgb = words[WL_XBOX_PSH_RGB_OUTPUTS0 + stage],
                 alpha = words[WL_XBOX_PSH_ALPHA_OUTPUTS0 + stage];

        if (rgb & 0xc0000u) {
            survey_count("combiners: a blue to alpha");
        }
        if ((rgb | alpha) & 0x4000u) {
            survey_count(words[WL_XBOX_PSH_COMBINER_COUNT] & 0x100u ? "combiners: a mux by the top bit"
                                                                   : "combiners: a mux by the low bit");
        }
    }
    for (stage = 0; stage < 4; stage++) {
        uint32_t mode = wl_nv2a_stage_program(k, stage), base = WL_NV2A_K_TEXTURE_STRIDE * stage;
        uint32_t filter = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_FILTER + base),
                 control0 = wl_nv2a_k(k, WL_NV2A_K_SET_TEXTURE_CONTROL0 + base);

        if (mode == WL_NV2A_STAGE_NONE) {
            continue;
        }
        if (mode > WL_NV2A_STAGE_CUBE_MAP) {
            snprintf(name, sizeof name, "texture stage mode %02x", (unsigned)mode);
            survey_count(name);
        }
        if (control0 & 4u) {
            survey_count("texture: alpha kill");
        }
        if (filter >> 28) {
            snprintf(name, sizeof name, "texture: signed channels %x", (unsigned)(filter >> 28));
            survey_count(name);
        }
        if (filter & 0x1fffu) {
            survey_count("texture: a LOD bias");
        }
    }
    if (wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_ENABLE)) {
        uint32_t equation = wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_EQUATION);
        uint32_t factors[2] = {wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR),
                               wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR)};
        unsigned side;

        if (equation != WL_NV2A_BLEND_EQ_ADD) {
            if (equation >= WL_NV2A_BLEND_EQ_ADD && equation <= WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT) {
                snprintf(name, sizeof name, "blend: %s", equations[equation - WL_NV2A_BLEND_EQ_ADD]);
            } else {
                snprintf(name, sizeof name, "blend: equation %04x", (unsigned)equation);
            }
            survey_count(name);
        }
        for (side = 0; side < 2; side++) {
            if (factors[side] >= WL_NV2A_BLEND_CONSTANT_COLOR &&
                factors[side] <= WL_NV2A_BLEND_ONE_MINUS_CONSTANT_ALPHA) {
                snprintf(name, sizeof name, "blend: a constant factor %04x/%04x, colour %08x",
                         (unsigned)factors[0], (unsigned)factors[1],
                         (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_COLOR));
                survey_count(name);
                break;
            }
        }
    }
    if (g_gx != NULL) {
        bool used;
        const WlXboxPshProgram *program = wl_nv2a_gx_last_program(g_gx, &used);

        if (program != NULL && !used) {
            snprintf(name, sizeof name, "psh: beyond GX (%s)", program->reason ? program->reason : "?");
            survey_count(name);
        } else if (program != NULL && program->approximate) {
            snprintf(name, sizeof name, "psh: approximated (%s)",
                     program->approximation ? program->approximation : "?");
            survey_count(name);
        }
    }
}

static uint32_t tee_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    Tee *t = user;
    uint32_t pixels = 0;
    unsigned i;

    program_seen(k);
    for (i = 0; i < t->count; i++) {
        pixels = t->b[i].draw(t->b[i].user, k, draw);
    }
    if (g_survey) {
        survey_draw(k);
    }
    for (i = 0; i < g_psh_dump_count; i++) {
        if (g_psh_dump[i] == g_backend_draws) {
            uint32_t words[WL_XBOX_PSH_WORDS];
            unsigned w;

            wl_nv2a_psh_words(k, words);
            printf("    psh dump (draw %u, stage programs %08x):", (unsigned)g_backend_draws,
                   (unsigned)wl_nv2a_k(k, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM));
            for (w = 0; w < WL_XBOX_PSH_WORDS; w++) {
                printf("%s%08x", w % 8u == 0 ? "\n      " : " ", (unsigned)words[w]);
            }
            printf("\n");
            if (g_gx != NULL) {
                bool used;
                const WlXboxPshProgram *program = wl_nv2a_gx_last_program(g_gx, &used);

                if (program != NULL) {
                    unsigned n;

                    printf("      used %u, %u stages, reason %s, approximation %s\n", (unsigned)used,
                           (unsigned)program->tev.stage_count, program->reason ? program->reason : "-",
                           program->approximation ? program->approximation : "-");
                    for (n = 0; n < program->tev.stage_count; n++) {
                        const WlTevStage *st = &program->tev.stages[n];

                        printf("        %u: tc %d map %d ch %d swap %u/%u k %02x/%02x | c %d %d %d %d op %u b %u s %u -> %u | "
                               "a %d %d %d %d op %u b %u s %u -> %u\n",
                               n, st->texcoord == 0xFF ? -1 : st->texcoord, st->texmap == 0xFF ? -1 : st->texmap,
                               st->channel == 0xFF ? -1 : st->channel, (unsigned)st->ras_swap, (unsigned)st->tex_swap,
                               (unsigned)st->kcsel, (unsigned)st->kasel, st->color.in[0], st->color.in[1],
                               st->color.in[2], st->color.in[3], (unsigned)st->color.op, (unsigned)st->color.bias,
                               (unsigned)st->color.scale, (unsigned)st->color.dest, st->alpha.in[0], st->alpha.in[1],
                               st->alpha.in[2], st->alpha.in[3], (unsigned)st->alpha.op, (unsigned)st->alpha.bias,
                               (unsigned)st->alpha.scale, (unsigned)st->alpha.dest);
                    }
                }
            }
        }
    }
    if (g_gx != NULL && g_verbosity != 0) {
        bool used, shared;
        const WlXboxPshProgram *program = wl_nv2a_gx_last_program(g_gx, &used);
        float a, b;

        if (wl_nv2a_gx_last_projection(g_gx, &a, &b, &shared)) {
            printf("    projection: depth %.9g + %.9g / w%s\n", (double)a, (double)b,
                   shared ? " (the fit kept for its transform)" : "");
        }

        if (program != NULL && !used) {
            printf("    psh: beyond GX (%s): texture 0 times the colour\n",
                   program->reason != NULL ? program->reason : "?");
        } else if (program != NULL && program->approximate) {
            printf("    psh: %u stages, approximated (%s)\n", (unsigned)program->tev.stage_count,
                   program->approximation != NULL ? program->approximation : "?");
        }
    }
    if (g_gx != NULL) {
        for (i = 0; i < g_efb_after_count; i++) {
            if (g_efb_after[i].draw == g_backend_draws) {
                char path[1024];
                const uint8_t *efb = wl_gxsoft_efb();

                snprintf(path, sizeof path, "%s/efb_after_%u.png", g_efb_after[i].dir, (unsigned)g_backend_draws);
                if (efb != NULL && png_write(path, efb, WL_GXSOFT_EFB_WIDTH, WL_GXSOFT_EFB_HEIGHT)) {
                    printf("  efb after draw %u -> %s\n", (unsigned)g_backend_draws, path);
                }
            }
        }
    }
    g_backend_draws++;
    return pixels;
}

static void tee_frame(void *user, const WlNv2aKelvin *k) {
    Tee *t = user;
    unsigned i;

    for (i = 0; i < t->count; i++) {
        t->b[i].frame_done(t->b[i].user, k);
    }
}

static void tee_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    Tee *t = user;
    unsigned i;

    for (i = 0; i < t->count; i++) {
        t->b[i].flip(t->b[i].user, k, phys);
    }
}

static void print_line(void *user, const char *line) {
    (void)user;
    printf("  %s\n", line);
}

// a frame GX showed (the software GX's display copy)
static void on_gx_frame(void *user, const uint8_t *rgba, uint32_t w, uint32_t h) {
    char path[1024];

    (void)user;
    if (g_png_dir == NULL) {
        return;
    }
    snprintf(path, sizeof path, "%s/frame_%04u.png", g_png_dir, g_png_count++);
    if (png_write(path, rgba, w, h)) {
        printf("  frame %u (GX, %ux%u) -> %s\n", g_png_count - 1, (unsigned)w, (unsigned)h, path);
    }
}

// what a GX primitive covered and wrote
static void on_gx_draw(void *user, const WlGxSoftDraw *d) {
    unsigned bit;

    (void)user;
    if (g_verbosity == 0) {
        return;
    }
    printf("    gx %u: %u triangles (%u culled, %u clipped), %u pixels, %u written", (unsigned)d->index,
           (unsigned)d->triangles, (unsigned)d->culled, (unsigned)d->clipped, (unsigned)d->pixels,
           (unsigned)d->written);
    if (d->z_failed != 0 || d->alpha_failed != 0) {
        printf(" (%u failed z, %u failed alpha)", (unsigned)d->z_failed, (unsigned)d->alpha_failed);
    }
    if (d->written != 0) {
        printf(" in (%.0f, %.0f)-(%.0f, %.0f)", (double)d->x0, (double)d->y0, (double)d->x1, (double)d->y1);
    }
    for (bit = 0; bit < WL_GXSOFT_ISSUES; bit++) {
        if (d->issues & (1u << bit)) {
            printf("; %s", wl_gxsoft_issue_name(bit));
        }
    }
    printf("\n");
}

// GX's primitive as it is handed over (the host's direct layer hook, before it is drawn)
static void on_gx_primitive(void *user, WlGxDirectPrim prim, const WlGxDirectVertex *v, uint32_t count) {
    const WlHostGxDirect *st = wl_host_gx_direct();
    const WlTevProgram *tev = wl_host_tev_last();
    uint32_t index = g_gx_primitives++, i;
    unsigned d, n;
    bool dump = false;

    (void)user;
    for (d = 0; d < g_gx_dump_count; d++) {
        dump = dump || g_gx_dump[d] == index;
    }
    if (!dump) {
        return;
    }
    printf("    gx %u dump: primitive %u, %u vertices; layout colours %u texcoords %02x (s-only %02x)\n", (unsigned)index,
           (unsigned)prim, (unsigned)count, (unsigned)st->layout.colors, (unsigned)st->layout.texcoords,
           (unsigned)st->layout.texcoord_s);
    printf("      projection %s:", st->orthographic ? "orthographic" : "perspective");
    for (i = 0; i < 4; i++) {
        printf(" [%g %g %g %g]", (double)st->projection[i][0], (double)st->projection[i][1],
               (double)st->projection[i][2], (double)st->projection[i][3]);
    }
    printf("\n      viewport %g %g %g %g, scissor %u %u %u %u, cull %u, blend %u %u %u, z %u/%u/%u early %u, alpha %u %u, "
           "update %u %u\n",
           (double)st->viewport[0], (double)st->viewport[1], (double)st->viewport[2], (double)st->viewport[3],
           (unsigned)st->scissor[0], (unsigned)st->scissor[1], (unsigned)st->scissor[2], (unsigned)st->scissor[3],
           (unsigned)st->cull, (unsigned)st->blend_mode, (unsigned)st->blend_source, (unsigned)st->blend_destination,
           (unsigned)st->z_test, (unsigned)st->z_func, (unsigned)st->z_write, (unsigned)st->z_early,
           (unsigned)st->alpha_func, (unsigned)st->alpha_ref, (unsigned)st->color_update, (unsigned)st->alpha_update);
    for (n = 0; n < 8; n++) {
        if (st->map_kind[n] == WL_HOST_GX_MAP_TEXTURE) {
            const WlGxTexObj *t = &st->map_objects[n];

            if (g_gx_textures_dir != NULL && t->format != WL_GX_TEX_C4 && t->format != WL_GX_TEX_C8) {
                const void *image = wl_host_gx_tex_image(t);
                uint8_t *rgba = malloc((size_t)t->width * t->height * 4u);
                size_t bytes = wl_gxc_tex_bytes((WlGxcFormat)t->format, t->width, t->height);
                char path[1024];

                if (image != NULL && rgba != NULL &&
                    wl_gxc_decode((WlGxcFormat)t->format, image, bytes, t->width, t->height, rgba) != 0) {
                    snprintf(path, sizeof path, "%s/gx_%u_map_%u.png", g_gx_textures_dir, (unsigned)index, n);
                    if (png_write_as(path, rgba, t->width, t->height, false)) {
                        printf("      map %u -> %s\n", n, path);
                    }
                }
                free(rgba);
            }
            printf("      map %u: %ux%u format %u mips %u wrap %u %u filter %u %u\n", n, (unsigned)t->width,
                   (unsigned)t->height, (unsigned)t->format, (unsigned)t->mips, (unsigned)t->sampler.wrap_s,
                   (unsigned)t->sampler.wrap_t, (unsigned)t->sampler.min_filter, (unsigned)t->sampler.mag_filter);
        } else if (st->map_kind[n] == WL_HOST_GX_MAP_RAMP) {
            printf("      map %u: the ramp\n", n);
        }
    }
    if (tev != NULL) {
        printf("      TEV: %u stages, konst %08x %08x %08x %08x (mask %x), preload %08x %08x %08x (mask %x)\n",
               (unsigned)tev->stage_count, (unsigned)tev->konst[0], (unsigned)tev->konst[1], (unsigned)tev->konst[2],
               (unsigned)tev->konst[3], (unsigned)tev->konst_mask, (unsigned)tev->preload[1], (unsigned)tev->preload[2],
               (unsigned)tev->preload[3], (unsigned)tev->preload_mask);
        for (n = 0; n < tev->stage_count; n++) {
            const WlTevStage *s = &tev->stages[n];

            printf("        %u: tc %d map %d ch %d swap %u/%u k %02x/%02x | c %d %d %d %d op %u b %u s %u cl %u -> %u | a %d "
                   "%d %d %d op %u b %u s %u cl %u -> %u\n",
                   n, s->texcoord == 0xFF ? -1 : s->texcoord, s->texmap == 0xFF ? -1 : s->texmap,
                   s->channel == 0xFF ? -1 : s->channel, (unsigned)s->ras_swap, (unsigned)s->tex_swap,
                   (unsigned)s->kcsel, (unsigned)s->kasel, s->color.in[0], s->color.in[1], s->color.in[2],
                   s->color.in[3], (unsigned)s->color.op, (unsigned)s->color.bias, (unsigned)s->color.scale,
                   (unsigned)s->color.clamp, (unsigned)s->color.dest, s->alpha.in[0], s->alpha.in[1], s->alpha.in[2],
                   s->alpha.in[3], (unsigned)s->alpha.op, (unsigned)s->alpha.bias, (unsigned)s->alpha.scale,
                   (unsigned)s->alpha.clamp, (unsigned)s->alpha.dest);
        }
    }
    for (i = 0; i < count && i < 12u; i++) {
        printf("      v%u: (%g, %g, %g) c0 %02x%02x%02x%02x c1 %02x%02x%02x%02x t0 (%g, %g) t1 (%g, %g) t2 (%g, %g) t4 %g\n",
               (unsigned)i, (double)v[i].position[0], (double)v[i].position[1], (double)v[i].position[2],
               v[i].color[0][0], v[i].color[0][1], v[i].color[0][2], v[i].color[0][3], v[i].color[1][0],
               v[i].color[1][1], v[i].color[1][2], v[i].color[1][3], (double)v[i].texcoord[0][0],
               (double)v[i].texcoord[0][1], (double)v[i].texcoord[1][0], (double)v[i].texcoord[1][1],
               (double)v[i].texcoord[2][0], (double)v[i].texcoord[2][1], (double)v[i].texcoord[4][0]);
    }
}

static void on_flip(void *user, WlNv2a *dev, uint32_t index) {
    WlNv2aStats s;

    (void)user;
    wl_nv2a_stats(dev, &s);
    printf("flip %u: %u draws, %u vertices, %u clears so far; digest %016llx\n", (unsigned)index, (unsigned)s.draws,
           (unsigned)s.vertices, (unsigned)s.clears, (unsigned long long)g_recorder.digest);
}

// (the GX backend reads guest memory through the model the replay makes)
static void on_memory(void *user, uint32_t phys, uint32_t bytes) {
    uint32_t p;

    (void)user;
    if (g_page_epoch == NULL || bytes == 0) {
        return;
    }
    g_epoch++;
    for (p = phys >> 12; p <= (phys + bytes - 1u) >> 12 && p < g_page_count; p++) {
        g_page_epoch[p] = g_epoch;
    }
}

static uint32_t watch_arm(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    (void)bytes;
    return (phys >> 12) < g_page_count ? g_epoch : 0u;
}

static bool watch_written(void *user, uint32_t phys, uint32_t bytes, uint32_t stamp) {
    uint32_t p;

    (void)user;
    if (stamp == 0 || bytes == 0) {
        return true;
    }
    for (p = phys >> 12; p <= (phys + bytes - 1u) >> 12; p++) {
        if (p >= g_page_count || g_page_epoch[p] > stamp) {
            return true;
        }
    }
    return false;
}

static void on_created(void *user, WlNv2a *dev) {
    (void)user;
    if (g_gx != NULL) {
        wl_nv2a_gx_attach(g_gx, dev);
    }
}

// --stream: the capture's bytes a piece at a time, and a guest memory of the tool's own given as
// the caller's
static struct {
    const uint8_t *data;
    size_t size, at;
    uint8_t *ram;
    uint32_t ram_bytes;
} g_src;

static WlResult stream_read(void *user, void *dst, uint32_t bytes, uint32_t *got) {
    size_t n = g_src.size - g_src.at;

    (void)user;
    n = n < bytes ? n : bytes;
    n = n < 65521u ? n : 65521u;
    memcpy(dst, g_src.data + g_src.at, n);
    g_src.at += n;
    *got = (uint32_t)n;
    return WL_OK;
}

static void *stream_phys(void *user, uint32_t phys, uint32_t bytes) {
    (void)user;
    if (phys >= g_src.ram_bytes || bytes > g_src.ram_bytes - phys) {
        return NULL;
    }
    return g_src.ram + phys;
}

static WlResult replay(const uint8_t *data, size_t size, const WlNv2aBackend *backend, const WlNv2aReplayHooks *hooks,
                       WlNv2aReplayInfo *info) {
    WlNv2aReplaySource source = {NULL, stream_read};
    WlNv2aReplayMemory memory = {NULL, stream_phys, 0};
    WlResult result;

    if (!g_stream) {
        return wl_nv2a_replay(data, size, backend, hooks, info);
    }
    g_src.data = data;
    g_src.size = size;
    g_src.at = 0;
    g_src.ram_bytes = size >= 16 ? wl_rd_le32(data + 12) : 0u; // (the header's RAM size)
    g_src.ram = calloc(1, g_src.ram_bytes != 0 ? g_src.ram_bytes : 1u);
    if (g_src.ram == NULL) {
        return WL_ERR_NOMEM;
    }
    memory.ram_bytes = g_src.ram_bytes;
    result = wl_nv2a_replay_stream(&source, &memory, backend, hooks, info);
    free(g_src.ram);
    g_src.ram = NULL;
    return result;
}

// --bench: the replay through the GX backend alone, `runs` times (its textures kept from one run to
// the next, as a title's are from one frame to the next)
static double seconds_now(void) {
    struct timespec t;

    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec * 1.0e-9;
}

// (the bench's clock in microseconds, and the backend's calls timed: what is left is the model's)
static uint64_t micros_now(void *user) {
    (void)user;
    return (uint64_t)(seconds_now() * 1.0e6);
}

static struct {
    WlNv2aBackend inner;
    double draws, others;
} g_timed;

// the ways' names (WlNv2aGxXfWhy), and the draws of each way by program (--xf-programs)
static const char *const g_xf_whys[WL_NV2A_GX_XF_WHYS + 1] = {
    "xf, indexed",           "xf, sent: not arrays",     "xf, sent: span",
    "xf, sent: array",       "pos: fog/point/back",      "pos: colour not affine", "pos: colour affine, mixed",
    "pos: coordinate",       "pos: sphere map",          "pos: projective",
    "pos: coordinate input", "cpu: xf off",              "cpu: fixed function",
    "cpu: counting pixels",  "cpu: too few vertices",    "cpu: immediate",
    "cpu: position not affine in v0", "cpu: position not F3", "cpu: clip w not captured",
    "cpu: screen w/depth",   "not drawn"};

enum { XF_PROGRAM_ROWS = 512 };

typedef struct XfRow {
    unsigned long hash;
    unsigned why;
    uint64_t draws, vertices;
} XfRow;

static XfRow g_xf_rows[XF_PROGRAM_ROWS];
static unsigned g_xf_row_count;

static void xf_program_note(const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    const uint32_t *words;
    unsigned long count = 0, hash = 0;
    unsigned why = wl_nv2a_gx_last_xf(g_gx), i;

    if (why >= WL_NV2A_GX_XF_WHYS) {
        return;
    }
    if (wl_nv2a_program_mode(k)) {
        wl_nv2a_program(k, &words, &count);
        hash = count != 0 ? wl_xbox_vsh_hash(words, count) : 0;
    }
    for (i = 0; i < g_xf_row_count; i++) {
        if (g_xf_rows[i].hash == hash && g_xf_rows[i].why == why) {
            break;
        }
    }
    if (i == g_xf_row_count) {
        if (i == XF_PROGRAM_ROWS) {
            return;
        }
        g_xf_rows[g_xf_row_count++].hash = hash;
        g_xf_rows[i].why = why;
    }
    g_xf_rows[i].draws++;
    g_xf_rows[i].vertices += draw->count;
}

static int xf_row_order(const void *a, const void *b) {
    uint64_t va = ((const XfRow *)a)->vertices, vb = ((const XfRow *)b)->vertices;

    return va < vb ? 1 : va > vb ? -1 : 0;
}

static uint32_t timed_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    double t0 = seconds_now();
    uint32_t pixels = g_timed.inner.draw(g_timed.inner.user, k, draw);

    (void)user;
    g_timed.draws += seconds_now() - t0;
    if (g_xf_programs) {
        xf_program_note(k, draw);
    }
    return pixels;
}

static void timed_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    double t0 = seconds_now();

    (void)user;
    g_timed.inner.clear(g_timed.inner.user, k, flags);
    g_timed.others += seconds_now() - t0;
}

static void timed_frame(void *user, const WlNv2aKelvin *k) {
    double t0 = seconds_now();

    (void)user;
    g_timed.inner.frame_done(g_timed.inner.user, k);
    g_timed.others += seconds_now() - t0;
}

static void timed_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    double t0 = seconds_now();

    (void)user;
    g_timed.inner.flip(g_timed.inner.user, k, phys);
    g_timed.others += seconds_now() - t0;
}

// the vertex cache's work and memory, when it is on
static void vertex_cache_line(const char *prefix) {
    WlNv2aVCacheStats vs;
    WlNv2aGxStats gs;

    if (!wl_nv2a_vcache_on(wl_nv2a_gx_vertex_cache(g_gx))) {
        return;
    }
    wl_nv2a_vcache_stats(wl_nv2a_gx_vertex_cache(g_gx), &vs);
    wl_nv2a_gx_stats(g_gx, &gs);
    printf("%svertex arrays: %u draws (%u vertices) sent as indices; %u lookups, %u checks (%u by the write watch), "
           "%u hashes (%llu bytes), %u converted (%llu elements), %u made again, %u refused, %u evicted, %u failed; "
           "%u KB kept (%u KB at most), %u KB drawn in a frame at most\n",
           prefix, (unsigned)gs.indexed_draws, (unsigned)gs.indexed_vertices, (unsigned)vs.lookups, (unsigned)vs.checks,
           (unsigned)vs.watched, (unsigned)vs.hashed, (unsigned long long)vs.hashed_bytes, (unsigned)vs.converted,
           (unsigned long long)vs.converted_elements, (unsigned)vs.reconverted, (unsigned)vs.refused,
           (unsigned)vs.evicted, (unsigned)vs.failed, (unsigned)(vs.bytes >> 10), (unsigned)(vs.peak_bytes >> 10),
           (unsigned)(vs.peak_frame_bytes >> 10));
}

// the texture cache's work and memory
static void texture_cache_line(const char *prefix) {
    WlNv2aTexCacheStats ts;

    wl_nv2a_texcache_stats(wl_nv2a_gx_textures(g_gx), &ts);
    printf("%stextures: %u lookups, %u checks (%u by the write watch), %u hashes (%llu bytes), %u converted, "
           "%u made again, %u evicted, %u failed (%u for memory), %u kept smaller (%u made whole again); %u KB kept, "
           "%u KB drawn in a frame at most\n",
           prefix, (unsigned)ts.lookups, (unsigned)ts.checks, (unsigned)ts.watched, (unsigned)ts.hashed,
           (unsigned long long)ts.hashed_bytes, (unsigned)ts.converted, (unsigned)ts.reconverted,
           (unsigned)ts.evicted, (unsigned)ts.failed, (unsigned)ts.no_memory, (unsigned)ts.reduced,
           (unsigned)ts.restored, (unsigned)(ts.bytes >> 10), (unsigned)(ts.peak_frame_bytes >> 10));
}

static int bench(const uint8_t *data, size_t size, unsigned runs) {
    WlNv2aBackend backend;
    WlNv2aReplayHooks hooks;
    WlNv2aReplayInfo info;
    double best = 0.0, total = 0.0, best_draws = 0.0, best_others = 0.0, best_memory = 0.0;
    unsigned run, frames = 0;

    if (wl_gx_init() != WL_OK || (g_gx = wl_nv2a_gx_create(gx_config())) == NULL) {
        fprintf(stderr, "no memory for the GX backend\n");
        return 1;
    }
    wl_nv2a_gx_backend(g_gx, &g_timed.inner);
    memset(&backend, 0, sizeof backend);
    backend.draw = timed_draw;
    backend.clear = timed_clear;
    backend.frame_done = timed_frame;
    backend.flip = timed_flip;
    memset(&hooks, 0, sizeof hooks);
    hooks.created = on_created;
    hooks.clock = micros_now;
    if (g_page_epoch != NULL) {
        hooks.memory = on_memory;
    }
    for (run = 0; run < runs; run++) {
        double start = seconds_now(), took;

        g_timed.draws = g_timed.others = 0.0;
        if (replay(data, size, &backend, &hooks, &info) != WL_OK) {
            fprintf(stderr, "the replay failed\n");
            return 1;
        }
        took = seconds_now() - start;
        if (run == 0 || took < best) {
            best = took;
            best_draws = g_timed.draws;
            best_others = g_timed.others;
            best_memory = (double)info.memory_time * 1.0e-6;
        }
        total += took;
        frames = info.stats.frames != 0 ? info.stats.frames : 1u;
    }
    printf("bench: %u runs of %u frames (%u draws, %u vertices): best %.3f ms a frame, mean %.3f ms a frame\n",
           runs, frames, (unsigned)info.stats.draws, (unsigned)info.stats.vertices, best * 1000.0 / frames,
           total * 1000.0 / runs / frames);
    printf("bench: a run's model: %u kicks, %u words, %u methods\n", (unsigned)info.stats.kicks,
           (unsigned)info.stats.words, (unsigned)info.stats.methods);
    {
        WlNv2aGxStats gs;

        wl_nv2a_gx_stats(g_gx, &gs);
        printf("bench: %u draws (%u vertices) through GX's transform unit in all runs, %u more for their position "
               "alone\n", (unsigned)gs.xf_draws, (unsigned)gs.xf_vertices, (unsigned)gs.xf_position_draws);
        vertex_cache_line("bench: ");
        texture_cache_line("bench: ");
        {
            // (where each draw's vertices were transformed, a frame)
            double per = 1.0 / ((double)runs * frames);
            unsigned w;

            printf("bench: the draws' ways a frame (draws, vertices, vertices a draw):\n");
            for (w = 0; w < WL_NV2A_GX_XF_WHYS; w++) {
                if (gs.xf_why_draws[w] != 0) {
                    printf("bench:   %-32s %9.1f %10.1f %7.1f\n", g_xf_whys[w], gs.xf_why_draws[w] * per,
                           gs.xf_why_vertices[w] * per, (double)gs.xf_why_vertices[w] / gs.xf_why_draws[w]);
                }
            }
        }
        if (g_xf_programs) {
            double per = 1.0 / ((double)runs * frames);
            unsigned i;

            qsort(g_xf_rows, g_xf_row_count, sizeof g_xf_rows[0], xf_row_order);
            printf("bench: by program a frame (hash, way, draws, vertices):\n");
            for (i = 0; i < g_xf_row_count; i++) {
                printf("bench:   %08lx %-32s %9.1f %10.1f\n", g_xf_rows[i].hash, g_xf_whys[g_xf_rows[i].why],
                       (double)g_xf_rows[i].draws * per, (double)g_xf_rows[i].vertices * per);
            }
        }
    }
    printf("bench: the best run a frame: pusher %.3f ms, draws %.3f, clears and frames %.3f, memory records %.3f\n",
           (best - best_draws - best_others - best_memory) * 1000.0 / frames, best_draws * 1000.0 / frames,
           best_others * 1000.0 / frames, best_memory * 1000.0 / frames);
    wl_nv2a_gx_destroy(g_gx);
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: xbox_nv2a_replay CAPTURE [-v | -vv] [--png-gx DIR] [--efb-after N DIR] [--gx-dump N] "
                    "[--gx-textures DIR] [--psh-dump N] [--programs FILE] [--textures-mb N] [--vertex-cache-kb N] [--watch] [--bench N] [--xf-programs] "
                    "[--stream] [--cpu-transform] [--png-alpha] [--survey]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    bool gx = false;
    uint8_t *data;
    long size;
    FILE *f;
    int i;
    Tee tee;
    WlNv2aBackend backend;
    WlNv2aReplayHooks hooks;
    WlNv2aReplayInfo info;
    WlResult result;
    void *arena = NULL;
    bool watch = false;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbosity = 1;
        } else if (strcmp(argv[i], "-vv") == 0) {
            g_verbosity = 2;
        } else if (strcmp(argv[i], "--png-gx") == 0 && i + 1 < argc) {
            g_png_dir = argv[++i];
            gx = true;
        } else if (strcmp(argv[i], "--efb-after") == 0 && i + 2 < argc && g_efb_after_count < EFB_AFTER_MAX) {
            g_efb_after[g_efb_after_count].draw = (uint32_t)strtoul(argv[++i], NULL, 0);
            g_efb_after[g_efb_after_count++].dir = argv[++i];
            gx = true;
        } else if (strcmp(argv[i], "--gx-textures") == 0 && i + 1 < argc) {
            g_gx_textures_dir = argv[++i];
        } else if (strcmp(argv[i], "--gx-dump") == 0 && i + 1 < argc && g_gx_dump_count < EFB_AFTER_MAX) {
            g_gx_dump[g_gx_dump_count++] = (uint32_t)strtoul(argv[++i], NULL, 0);
            gx = true;
        } else if (strcmp(argv[i], "--psh-dump") == 0 && i + 1 < argc && g_psh_dump_count < EFB_AFTER_MAX) {
            g_psh_dump[g_psh_dump_count++] = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--png-alpha") == 0) {
            g_png_alpha = true;
        } else if (strcmp(argv[i], "--survey") == 0) {
            g_survey = true;
            gx = true;
        } else if (strcmp(argv[i], "--xf-programs") == 0) {
            g_xf_programs = true;
        } else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            g_bench = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--programs") == 0 && i + 1 < argc) {
            g_programs = fopen(argv[++i], "w");
            if (g_programs == NULL) {
                fprintf(stderr, "cannot write %s\n", argv[i]);
                return 1;
            }
            fprintf(g_programs, "/* NV2A vertex programs a capture drew with (xbox_nv2a_replay --programs) */\n");
        } else if (strcmp(argv[i], "--textures-mb") == 0 && i + 1 < argc) {
            g_gcfg.textures.arena_bytes = (size_t)strtoul(argv[++i], NULL, 0) << 20;
            arena = g_gcfg.textures.arena_bytes != 0 ? malloc(g_gcfg.textures.arena_bytes) : NULL;
            g_gcfg.textures.arena = arena;
        } else if (strcmp(argv[i], "--vertex-cache-kb") == 0 && i + 1 < argc) {
            g_gcfg.vertices.arena_bytes = (size_t)strtoul(argv[++i], NULL, 0) << 10;
            g_gcfg.vertices.arena = g_gcfg.vertices.arena_bytes != 0 ? malloc(g_gcfg.vertices.arena_bytes) : NULL;
        } else if (strcmp(argv[i], "--watch") == 0) {
            watch = true;
        } else if (strcmp(argv[i], "--cpu-transform") == 0) {
            g_gcfg.no_xf = true;
        } else if (strcmp(argv[i], "--stream") == 0) {
            g_stream = true;
        } else if (argv[i][0] == '-' || path != NULL) {
            return usage();
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        return usage();
    }
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    data = malloc((size_t)size + 1u);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    fclose(f);
    if (watch && size >= 16) {
        g_gcfg.textures.watch = watch_arm;
        g_gcfg.textures.written = watch_written;
        g_gcfg.vertices.watch = watch_arm;
        g_gcfg.vertices.written = watch_written;
        g_page_count = wl_rd_le32(data + 12) >> 12; // (the header's RAM size)
        g_page_epoch = calloc(g_page_count != 0 ? g_page_count : 1u, sizeof *g_page_epoch);
    }
    if (g_bench != 0) {
        return bench(data, (size_t)size, g_bench);
    }

    memset(&tee, 0, sizeof tee);
    wl_nv2a_recorder_init(&g_recorder);
    g_recorder.verbosity = g_verbosity;
    g_recorder.print = print_line;
    wl_nv2a_recorder_backend(&g_recorder, &tee.b[tee.count++]);
    if (gx) {
        WlGxSoftConfig scfg;

        memset(&scfg, 0, sizeof scfg);
        scfg.frame = on_gx_frame;
        scfg.draw = on_gx_draw;
        if (wl_gx_init() != WL_OK || wl_gxsoft_attach(&scfg) != WL_OK || (g_gx = wl_nv2a_gx_create(gx_config())) == NULL) {
            fprintf(stderr, "no memory for the GX backend\n");
            return 1;
        }
        wl_nv2a_gx_backend(g_gx, &tee.b[tee.count++]);
        if (g_gx_dump_count != 0) {
            wl_host_gx_direct_hook(on_gx_primitive, NULL);
        }
    }
    memset(&backend, 0, sizeof backend);
    backend.user = &tee;
    backend.clear = tee_clear;
    backend.draw = tee_draw;
    backend.frame_done = tee_frame;
    backend.flip = tee_flip;
    memset(&hooks, 0, sizeof hooks);
    hooks.created = on_created;
    hooks.flip = on_flip;
    if (g_page_epoch != NULL) {
        hooks.memory = on_memory;
    }
    result = replay(data, (size_t)size, &backend, &hooks, &info);
    if (result != WL_OK) {
        fprintf(stderr, "%s: %s\n", path, wl_result_name(result));
        return 1;
    }
    printf("\n%s: RAM %u MiB; %u MMIO writes, %u vertical blanks, %u memory blocks (%llu bytes), %u flips\n", path,
           (unsigned)(info.ram_bytes >> 20), (unsigned)info.writes, (unsigned)info.vblanks, (unsigned)info.blocks,
           (unsigned long long)info.block_bytes, (unsigned)info.flips);
    printf("model: %u kicks, %u words, %u methods (%u not handled), %u jumps, %u calls, %u pusher errors\n",
           (unsigned)info.stats.kicks, (unsigned)info.stats.words, (unsigned)info.stats.methods,
           (unsigned)info.stats.unhandled, (unsigned)info.stats.jumps, (unsigned)info.stats.calls,
           (unsigned)info.stats.pusher_errors);
    printf("       %u draws, %u vertices, %u clears, %u frames, %u flips; %u semaphores, %u notifies, %u reports\n",
           (unsigned)info.stats.draws, (unsigned)info.stats.vertices, (unsigned)info.stats.clears,
           (unsigned)info.stats.frames, (unsigned)info.stats.flips, (unsigned)info.stats.semaphores,
           (unsigned)info.stats.notifies, (unsigned)info.stats.reports);
    printf("       %u stencil draws, %u fixed-function draws, %u memory misses, %u unmet acquires\n",
           (unsigned)info.stats.stencil_draws, (unsigned)info.stats.fixed_function_draws,
           (unsigned)info.stats.memory_misses, (unsigned)info.stats.acquires_unmet);
    printf("recorder: digest %016llx\n", (unsigned long long)g_recorder.digest);
    if (g_gx != NULL) {
        WlNv2aGxStats gs;
        WlGxSoftStats ss;
        unsigned bit;

        wl_nv2a_gx_present(g_gx);
        wl_nv2a_gx_stats(g_gx, &gs);
        vertex_cache_line("");
        texture_cache_line("");
        wl_gxsoft_stats(&ss);
        printf("gx backend: %u draws, %u perspective (%u fits made, %u shared), %u into targets (%u without one), "
               "%u stencil skipped, %u programs beyond GX, %u approximated, %u clears (%u quads), %u frames\n",
               (unsigned)gs.draws, (unsigned)gs.perspective_draws, (unsigned)gs.fits_made, (unsigned)gs.fits_shared,
               (unsigned)gs.target_draws,
               (unsigned)gs.target_skipped, (unsigned)gs.stencil_skipped, (unsigned)gs.programs_unusable,
               (unsigned)gs.approximations, (unsigned)gs.clears, (unsigned)gs.quad_clears, (unsigned)gs.frames);
        printf("            %u programs' signed sums made unsigned, %u textures with border addressing clamped, "
               "%u targets stored to after their image\n",
               (unsigned)gs.programs_unbiased, (unsigned)gs.border_clamped, (unsigned)gs.targets_written);
        printf("            %u draws (%u vertices) through GX's transform unit, %u more for their position alone; "
               "%u with a projective coordinate GX divides at each pixel; %u vertices of indexed draws reused\n",
               (unsigned)gs.xf_draws, (unsigned)gs.xf_vertices, (unsigned)gs.xf_position_draws,
               (unsigned)gs.projected_draws, (unsigned)gs.vertices_reused);
        printf("            %u draws counted without drawing (they write nothing), %u skipped; %u kept the raster "
               "state, %u the textures, %u the pixel program\n",
               (unsigned)gs.unwritten_counted, (unsigned)gs.unwritten_skipped, (unsigned)gs.raster_kept,
               (unsigned)gs.textures_kept, (unsigned)gs.combiners_kept);
        printf("software GX: %u primitives, %u triangles (%u culled, %u clipped), %u pixels, %u written, %u copies, "
               "%u restores, %u clears, %u frames, %u textures decoded\n",
               (unsigned)ss.draws, (unsigned)ss.triangles, (unsigned)ss.culled, (unsigned)ss.clipped,
               (unsigned)ss.pixels, (unsigned)ss.written, (unsigned)ss.copies, (unsigned)ss.restores,
               (unsigned)ss.clears, (unsigned)ss.frames, (unsigned)ss.textures_decoded);
        for (bit = 0; bit < WL_GXSOFT_ISSUES; bit++) {
            if (ss.issue_draws[bit] != 0) {
                printf("             %u primitives: %s\n", (unsigned)ss.issue_draws[bit], wl_gxsoft_issue_name(bit));
            }
        }
        wl_nv2a_gx_destroy(g_gx);
        wl_gxsoft_detach();
    }
    if (g_survey) {
        unsigned n;

        printf("survey: %u features GX takes approximately\n", g_feature_count);
        for (n = 0; n < g_feature_count; n++) {
            printf("  %6u draws (first %u): %s\n", (unsigned)g_features[n].draws, (unsigned)g_features[n].first,
                   g_features[n].name);
        }
    }
    if (g_programs != NULL) {
        printf("programs: %u distinct vertex programs written\n", g_program_count);
        fclose(g_programs);
    }
    free(data);
    free(arena);
    free(g_page_epoch);
    return 0;
}
