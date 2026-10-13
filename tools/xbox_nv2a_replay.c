// SPDX-License-Identifier: MIT
// Replays a capture of the NV2A model (wl/xbox/nv2a_capture.h: a title's push buffers and the guest
// memory they read, a frame or a few) through the model on the host, and says what it drew: each
// clear, draw (primitive, vertices, program, combiners, textures, raster state), frame and flip, the
// model's counts (methods not handled, stencil and fixed-function draws, memory it could not read),
// and, with --png-gx, each frame the GX backend (wl/xbox/nv2a_gx.h) would show on the Wii, drawn by
// the host's software GX (wl/gxsoft) as a PNG.
//
// usage: xbox_nv2a_replay CAPTURE [-v | -vv] [--png-gx DIR] [--efb-after N DIR] [--gx-dump N]
//                          [--psh-dump N] [--programs FILE]
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
//   --watch     the converted textures checked through a write watch, as the runtime's MMU gives
//               it on the Wii: a page counts as stored to when one of the capture's memory blocks
//               lands in it (the blocks are what the title wrote, as the capture read it)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static unsigned long g_program_hashes[1024];
static unsigned g_program_count;
static unsigned g_verbosity;
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

static bool png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint8_t *png = NULL;
    size_t len = 0;
    FILE *f;
    bool ok;

    if (wl_png_encode(rgba, w, h, &png, &len) != WL_OK) {
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

static void tee_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    Tee *t = user;
    unsigned i;

    for (i = 0; i < t->count; i++) {
        t->b[i].clear(t->b[i].user, k, flags);
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
                    if (png_write(path, rgba, t->width, t->height)) {
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

static int usage(void) {
    fprintf(stderr, "usage: xbox_nv2a_replay CAPTURE [-v | -vv] [--png-gx DIR] [--efb-after N DIR] [--gx-dump N] "
                    "[--psh-dump N] [--programs FILE] [--textures-mb N] [--watch]\n");
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
    WlNv2aGxConfig gcfg;
    void *arena = NULL;
    bool watch = false;

    memset(&gcfg, 0, sizeof gcfg);
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
        } else if (strcmp(argv[i], "--programs") == 0 && i + 1 < argc) {
            g_programs = fopen(argv[++i], "w");
            if (g_programs == NULL) {
                fprintf(stderr, "cannot write %s\n", argv[i]);
                return 1;
            }
            fprintf(g_programs, "/* NV2A vertex programs a capture drew with (xbox_nv2a_replay --programs) */\n");
        } else if (strcmp(argv[i], "--textures-mb") == 0 && i + 1 < argc) {
            gcfg.textures.arena_bytes = (size_t)strtoul(argv[++i], NULL, 0) << 20;
            arena = gcfg.textures.arena_bytes != 0 ? malloc(gcfg.textures.arena_bytes) : NULL;
            gcfg.textures.arena = arena;
        } else if (strcmp(argv[i], "--watch") == 0) {
            watch = true;
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
        if (watch) {
            gcfg.textures.watch = watch_arm;
            gcfg.textures.written = watch_written;
        }
        if (wl_gx_init() != WL_OK || wl_gxsoft_attach(&scfg) != WL_OK || (g_gx = wl_nv2a_gx_create(&gcfg)) == NULL) {
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
    if (watch && size >= 16) {
        g_page_count = wl_rd_le32(data + 12) >> 12; // (the header's RAM size)
        g_page_epoch = calloc(g_page_count != 0 ? g_page_count : 1u, sizeof *g_page_epoch);
        hooks.memory = on_memory;
    }
    result = wl_nv2a_replay(data, (size_t)size, &backend, &hooks, &info);
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
        {
            WlNv2aTexCacheStats ts;

            wl_nv2a_texcache_stats(wl_nv2a_gx_textures(g_gx), &ts);
            printf("textures: %u lookups, %u checks (%u by the write watch), %u hashes (%llu bytes), %u converted, "
                   "%u made again, %u evicted, %u failed (%u for memory); %u KB kept, %u KB drawn in a frame at most\n",
                   (unsigned)ts.lookups, (unsigned)ts.checks, (unsigned)ts.watched, (unsigned)ts.hashed,
                   (unsigned long long)ts.hashed_bytes, (unsigned)ts.converted, (unsigned)ts.reconverted,
                   (unsigned)ts.evicted, (unsigned)ts.failed, (unsigned)ts.no_memory, (unsigned)(ts.bytes >> 10),
                   (unsigned)(ts.peak_frame_bytes >> 10));
        }
        wl_gxsoft_stats(&ss);
        printf("gx backend: %u draws, %u perspective (%u fits made, %u shared), %u into targets (%u without one), "
               "%u stencil skipped, %u programs beyond GX, %u approximated, %u clears (%u quads), %u frames\n",
               (unsigned)gs.draws, (unsigned)gs.perspective_draws, (unsigned)gs.fits_made, (unsigned)gs.fits_shared,
               (unsigned)gs.target_draws,
               (unsigned)gs.target_skipped, (unsigned)gs.stencil_skipped, (unsigned)gs.programs_unusable,
               (unsigned)gs.approximations, (unsigned)gs.clears, (unsigned)gs.quad_clears, (unsigned)gs.frames);
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
    if (g_programs != NULL) {
        printf("programs: %u distinct vertex programs written\n", g_program_count);
        fclose(g_programs);
    }
    free(data);
    free(arena);
    free(g_page_epoch);
    return 0;
}
