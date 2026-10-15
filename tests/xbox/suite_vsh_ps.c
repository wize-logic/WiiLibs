// SPDX-License-Identifier: MIT
// wl/xbox/vsh: the synthetic programs of vsh_programs.inc compiled ahead of time with their
// paired-single code (tools/xbox_vsh_codegen.c, WL_XBOX_VSH_PS) against the interpreter, within a
// tolerance: the fused multiply-adds and the dot products' order of adding change the last bits.
// On the console the code is the Gekko's (tests/console); on the host the same code emulated in C
// (vsh_ps_emulate.py, test_xbox_vsh_ps.c). The values are sixteenths up to 4, whose products and
// sums are exact either way, so only what follows a reciprocal, a root, an exponent or a logarithm
// may differ, by an error that rounding explains (the tolerance), and a wrong lane, operand or row
// shows as a large one.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "tests/platform/check.h"
#include "wl/xbox/vsh_native.h"

void wlt_suite_vsh_ps(void);

extern const WlXboxVshNative test_vsh_ps_natives[];
extern const unsigned long test_vsh_ps_natives_count;

static const uint32_t ps_microcode[] = {
#include "vsh_programs.inc"
};

enum { PS_TRIALS = 300, PS_VERTICES = 3 };

static uint32_t g_ps_rng = 0x2468aceu;
static uint32_t ps_rnd(void) {
    g_ps_rng = g_ps_rng * 1664525u + 1013904223u;
    return g_ps_rng >> 8;
}

// -4 to 4 in sixteenths, now and then 0
static float ps_value(void) {
    uint32_t r = ps_rnd();
    if (r % 16 == 0) {
        return 0.0f;
    }
    return (float)((int)(ps_rnd() % 129) - 64) / 16.0f;
}

static float g_ps_constants[WL_XBOX_VSH_CONSTANT_COUNT][4];
static float g_ps_inputs[PS_VERTICES][WL_XBOX_VSH_INPUT_COUNT][4];
static WlXboxVshProgram g_ps_native, g_ps_interpreted;
static WlXboxVshWork g_ps_work;

// the floats of an output, position to point size (clip_captured compared alone)
static void ps_floats(const WlXboxVshOutput *o, float *out, unsigned *count) {
    unsigned n = 0;
    memcpy(out + n, o->position, sizeof o->position), n += 4;
    memcpy(out + n, o->clip, sizeof o->clip), n += 4;
    memcpy(out + n, o->d0, sizeof o->d0), n += 4;
    memcpy(out + n, o->d1, sizeof o->d1), n += 4;
    memcpy(out + n, o->b0, sizeof o->b0), n += 4;
    memcpy(out + n, o->b1, sizeof o->b1), n += 4;
    memcpy(out + n, o->t, sizeof o->t), n += 16;
    out[n++] = o->fog;
    out[n++] = o->point_size;
    *count = n;
}

void wlt_suite_vsh_ps(void) {
    unsigned long floats = 0, differ = 0, beyond = 0, programs = 0;
    size_t position = 0, words = sizeof ps_microcode / sizeof ps_microcode[0];
    double worst = 0.0;
    bool natives = true, clips = true;

    while (position < words) {
        unsigned long count = ps_microcode[position] >> 16;
        const uint32_t *code = &ps_microcode[position + 1];

        position += 1 + count * 4;
        programs++;
        wl_xbox_vsh_compile(code, count, test_vsh_ps_natives, test_vsh_ps_natives_count, &g_ps_native);
        wl_xbox_vsh_compile(code, count, NULL, 0, &g_ps_interpreted);
        if (!g_ps_native.native || !g_ps_native.native_batch) {
            natives = false;
            continue;
        }
        for (int trial = 0; trial < PS_TRIALS; trial++) {
            WlXboxVshOutput want[PS_VERTICES], got[PS_VERTICES];

            for (int i = 0; i < WL_XBOX_VSH_CONSTANT_COUNT; i++) {
                for (int c = 0; c < 4; c++) {
                    g_ps_constants[i][c] = ps_value();
                }
            }
            for (int m = 0; m < PS_VERTICES; m++) {
                for (int i = 0; i < WL_XBOX_VSH_INPUT_COUNT; i++) {
                    for (int c = 0; c < 4; c++) {
                        g_ps_inputs[m][i][c] = ps_value();
                    }
                }
                // (v1.x a node's index: rows c[a0 + 10] to c[a0 + 13] within the constants)
                g_ps_inputs[m][1][0] = (float)(ps_rnd() % 40);
            }
            // (now and then the vertex before's nodes and weights, which a batch reuses)
            if (trial % 4 == 0) {
                memcpy(g_ps_inputs[1], g_ps_inputs[0], sizeof g_ps_inputs[0]);
                g_ps_inputs[1][0][0] = ps_value();
            }
            memset(want, 0, sizeof want);
            memset(got, 0, sizeof got);
            for (int m = 0; m < PS_VERTICES; m++) {
                wl_xbox_vsh_run(&g_ps_interpreted, (const float (*)[4])g_ps_constants,
                                (const float (*)[4])g_ps_inputs[m], 2.5f, &want[m]);
            }
            wl_xbox_vsh_run_batch(&g_ps_native, (const float (*)[4])g_ps_constants,
                                  (const float (*)[WL_XBOX_VSH_INPUT_COUNT][4])g_ps_inputs, PS_VERTICES, 2.5f,
                                  got, WL_XBOX_VSH_LIVE_ALL, &g_ps_work);
            for (int m = 0; m < PS_VERTICES; m++) {
                float a[48], b[48];
                unsigned n;

                clips = clips && want[m].clip_captured == got[m].clip_captured;
                ps_floats(&want[m], a, &n);
                ps_floats(&got[m], b, &n);
                for (unsigned k = 0; k < n; k++) {
                    double error;

                    floats++;
                    if (memcmp(&a[k], &b[k], sizeof a[k]) == 0 || (isnan(a[k]) && isnan(b[k]))) {
                        continue;
                    }
                    differ++;
                    error = fabs((double)a[k] - (double)b[k]) / (1.0 + fabs((double)a[k]));
                    if (!(error <= 1e-4)) {
                        beyond++;
                    } else if (error > worst) {
                        worst = error;
                    }
                }
            }
        }
    }
    wlt_report("WLT vsh_ps: %lu programs, %lu floats, %lu differ, %lu beyond 1e-4, the worst %.3g (paired "
               "singles %s)",
               programs, floats, differ, beyond, worst, WL_XBOX_VSH_PS ? "compiled" : "emulated");
    wlt_check("vsh_ps_natives", natives && programs > 0, "every program has its function");
    wlt_check("vsh_ps_clip", clips, "the clip position captured where the interpreter's is");
    wlt_check("vsh_ps_tolerance", beyond == 0, "within 1e-4 of the interpreter (relative to 1 + |x|)");
}
