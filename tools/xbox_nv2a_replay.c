// SPDX-License-Identifier: MIT
// Replays a capture of the NV2A model (wl/xbox/nv2a_capture.h: a title's push buffers and the guest
// memory they read, a frame or a few) through the model on the host, and says what it drew: each
// clear, draw (primitive, vertices, program, combiners, textures, raster state), frame and flip, the
// model's counts (methods not handled, stencil and fixed-function draws, memory it could not read),
// and, with --png, each frame drawn by the software reference of the GX backend
// (wl/xbox/nv2a_raster.h) as a PNG.
//
// usage: xbox_nv2a_replay CAPTURE [-v | -vv] [--png DIR] [--bilinear] [--programs FILE]
//   -v          a line for each clear, draw, frame and flip; -vv also each draw's raster state
//   --png       DIR/frame_NNNN.png for each frame shown
//   --programs  each vertex program drawn with, once, as tools/xbox_vsh_codegen.c's input (C text:
//               a header word with the instruction count in its high half, four words an
//               instruction), for compiling a title's programs ahead of time
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/gxtex/gxtex.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_raster.h"
#include "wl/xbox/nv2a_record.h"

typedef struct Tee {
    WlNv2aBackend a, b;
} Tee;

static void tee_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    Tee *t = user;
    t->a.clear(t->a.user, k, flags);
    t->b.clear(t->b.user, k, flags);
}

static FILE *g_programs;
static unsigned long g_program_hashes[1024];
static unsigned g_program_count;

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

static uint32_t tee_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    Tee *t = user;
    program_seen(k);
    (void)t->a.draw(t->a.user, k, draw);
    return t->b.draw(t->b.user, k, draw);
}

static void tee_frame(void *user, const WlNv2aKelvin *k) {
    Tee *t = user;
    t->a.frame_done(t->a.user, k);
    t->b.frame_done(t->b.user, k);
}

static void tee_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    Tee *t = user;
    t->a.flip(t->a.user, k, phys);
    t->b.flip(t->b.user, k, phys);
}

static void print_line(void *user, const char *line) {
    (void)user;
    printf("  %s\n", line);
}

static const char *g_png_dir;
static unsigned g_png_count;
static WlNv2aRecorder g_recorder;
static WlNv2aRaster *g_raster;

static void on_frame(void *user, uint32_t phys, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint8_t *png = NULL;
    size_t len = 0;
    char path[1024];
    FILE *f;

    (void)user;
    if (g_png_dir == NULL) {
        return;
    }
    if (wl_png_encode(rgba, w, h, &png, &len) != WL_OK) {
        fprintf(stderr, "png: cannot encode the frame\n");
        return;
    }
    snprintf(path, sizeof path, "%s/frame_%04u.png", g_png_dir, g_png_count++);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(png, 1, len, f) != len) {
        fprintf(stderr, "png: cannot write %s\n", path);
    } else {
        printf("  frame %u (surface %08x, %ux%u) -> %s\n", g_png_count - 1, (unsigned)phys, (unsigned)w, (unsigned)h,
               path);
    }
    if (f != NULL) {
        fclose(f);
    }
    free(png);
}

static void on_flip(void *user, WlNv2a *dev, uint32_t index) {
    WlNv2aStats s;

    (void)user;
    wl_nv2a_stats(dev, &s);
    printf("flip %u: %u draws, %u vertices, %u clears so far; digest %016llx\n", (unsigned)index, (unsigned)s.draws,
           (unsigned)s.vertices, (unsigned)s.clears, (unsigned long long)g_recorder.digest);
}

// (the software rasterizer reads guest memory through the model the replay makes)
static void on_created(void *user, WlNv2a *dev) {
    (void)user;
    wl_nv2a_raster_attach(g_raster, dev);
}

static int usage(void) {
    fprintf(stderr, "usage: xbox_nv2a_replay CAPTURE [-v | -vv] [--png DIR] [--bilinear] [--programs FILE]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    unsigned verbosity = 0;
    bool bilinear = false;
    uint8_t *data;
    long size;
    FILE *f;
    int i;
    Tee tee;
    WlNv2aBackend backend;
    WlNv2aRasterConfig rcfg;
    WlNv2aReplayHooks hooks;
    WlNv2aReplayInfo info;
    WlNv2aRasterStats rs;
    WlResult result;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbosity = 1;
        } else if (strcmp(argv[i], "-vv") == 0) {
            verbosity = 2;
        } else if (strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
            g_png_dir = argv[++i];
        } else if (strcmp(argv[i], "--bilinear") == 0) {
            bilinear = true;
        } else if (strcmp(argv[i], "--programs") == 0 && i + 1 < argc) {
            g_programs = fopen(argv[++i], "w");
            if (g_programs == NULL) {
                fprintf(stderr, "cannot write %s\n", argv[i]);
                return 1;
            }
            fprintf(g_programs, "/* NV2A vertex programs a capture drew with (xbox_nv2a_replay --programs) */\n");
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

    wl_nv2a_recorder_init(&g_recorder);
    g_recorder.verbosity = verbosity;
    g_recorder.print = print_line;
    memset(&rcfg, 0, sizeof rcfg);
    rcfg.frame = on_frame;
    rcfg.bilinear = bilinear;
    g_raster = wl_nv2a_raster_create(&rcfg);
    if (g_raster == NULL) {
        fprintf(stderr, "no memory\n");
        return 1;
    }
    wl_nv2a_recorder_backend(&g_recorder, &tee.a);
    wl_nv2a_raster_backend(g_raster, &tee.b);
    memset(&backend, 0, sizeof backend);
    backend.user = &tee;
    backend.clear = tee_clear;
    backend.draw = tee_draw;
    backend.frame_done = tee_frame;
    backend.flip = tee_flip;
    memset(&hooks, 0, sizeof hooks);
    hooks.created = on_created;
    hooks.flip = on_flip;
    result = wl_nv2a_replay(data, (size_t)size, &backend, &hooks, &info);
    if (result != WL_OK) {
        fprintf(stderr, "%s: %s\n", path, wl_result_name(result));
        return 1;
    }
    wl_nv2a_raster_stats(g_raster, &rs);
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
    printf("raster: %u draws, %u triangles (%u culled), %u pixels, %u frames, %u combiner programs beyond GX\n",
           (unsigned)rs.draws, (unsigned)rs.triangles, (unsigned)rs.culled, (unsigned)rs.pixels, (unsigned)rs.frames,
           (unsigned)rs.programs_unusable);
    if (g_programs != NULL) {
        printf("programs: %u distinct vertex programs written\n", g_program_count);
        fclose(g_programs);
    }
    wl_nv2a_raster_destroy(g_raster);
    free(data);
    return 0;
}
