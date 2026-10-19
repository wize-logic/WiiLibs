// SPDX-License-Identifier: MIT
// wl/xbox/vsh: a fused batch (tools/xbox_vsh_pairs.c's, the Wii's), emulated on the host by
// vsh_ps_emulate.py as Broadway runs it (frsqrte's table estimate, stfs cutting a double's
// mantissa), against the general path: here a sphere map's stage and wl_nv2a_sphere_coordinates.
// The fused stage works in single precision (two roots of two Newton's steps each): its
// coordinates are within 2^-22 (a four-thousandth of a texel of a 1024-texel map) of the exact
// ones, which the general path's double gives too except nearly straight behind (where its
// L + z cancels), and the disc's centre and edge are the general path's to the bit.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/nv2a_texture.h"
#include "wl/xbox/vsh_native.h"

extern const WlXboxVshNative test_vsh_fused_natives[];
extern const unsigned long test_vsh_fused_natives_count;

enum { VERTICES = 64, ROUNDS = 64, WORDS = 5 };

static uint32_t g_seed = 0x2545f491u;

static uint32_t next_random(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

// a float in [-1, 1) scaled by 2^(-4 .. 3)
static float random_component(void) {
    float x = (float)(int32_t)next_random() / 2147483648.0f;
    return x * (float)(1u << (next_random() % 8u)) / 16.0f;
}

static void put_le_float(unsigned char *p, float x) {
    uint32_t bits;

    memcpy(&bits, &x, 4);
    p[0] = (unsigned char)bits;
    p[1] = (unsigned char)(bits >> 8);
    p[2] = (unsigned char)(bits >> 16);
    p[3] = (unsigned char)(bits >> 24);
}

static float get_le_float(const unsigned char *p) {
    uint32_t bits = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    float x;

    memcpy(&x, &bits, 4);
    return x;
}

// the coordinates in double without the cancellation: 1 + z is r / (L (L - z)) for z < 0
static void sphere_exact(const float d[3], double st[2]) {
    double x = d[0], y = d[1], z = d[2], r = x * x + y * y, length = sqrt(r + z * z);
    double m = sqrt(2.0 * (z >= 0.0 ? 1.0 + z / length : r / (length * (length - z))));

    st[0] = x / length / (2.0 * m) + 0.5;
    st[1] = y / length / (2.0 * m) + 0.5;
}

static uint32_t float_bits(float x) {
    uint32_t bits;

    memcpy(&bits, &x, 4);
    return bits;
}

static const WlXboxVshFusedForm *sphere_form(void) {
    unsigned long i;

    for (i = 0; i < test_vsh_fused_natives_count; i++) {
        if (test_vsh_fused_natives[i].hash == 0x9aee98b6UL && test_vsh_fused_natives[i].fused_count == 1u) {
            return &test_vsh_fused_natives[i].fused[0];
        }
    }
    return NULL;
}

TEST fused_sphere_stage_is_close_to_the_general_path(void) {
    static float constants[192][4];
    static WlXboxVshFused fused;
    static unsigned char positions[VERTICES * 12], directions[VERTICES * 12];
    static uint32_t elements[VERTICES], words[VERTICES][WORDS];
    static uint32_t *words_made[VERTICES];
    static const double sphere[WL_XBOX_VSH_FUSED_SPHERE] = {1.0e-24, 1.0e76, 1.5, 0.5, 2.5e-13, 2.0};
    const WlXboxVshFusedForm *form = sphere_form();
    unsigned round, v, r, differing = 0, checked = 0;
    char detail[160] = "";
    double largest = 0.0;

    ASSERT(form != NULL);
    ASSERT_EQ(WL_XBOX_VSH_FUSED_STAGE_SPHERE, form->key.stages[0] >> 2);
    for (r = 0; r < 4; r++) {
        constants[96 + r][r] = 1.0f;
    }
    memset(&fused, 0, sizeof fused);
    memcpy(fused.sphere, sphere, sizeof sphere);
    fused.pack[WL_XBOX_VSH_FUSED_SCALE] = fused.pack[WL_XBOX_VSH_FUSED_SCALE + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES] = fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_HALF] = fused.pack[WL_XBOX_VSH_FUSED_HALF + 1] = 0.5f;
    fused.pack[WL_XBOX_VSH_FUSED_DEPTH] = fused.pack[WL_XBOX_VSH_FUSED_DEPTH + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_EDGE] = 0.5f;
    fused.pack[WL_XBOX_VSH_FUSED_EDGE + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_ONES] = fused.pack[WL_XBOX_VSH_FUSED_ONES + 1] = 1.0f;
    for (r = 0; r < 2; r++) {
        for (v = 0; v < 16; v++) {
            fused.inputs[r][v][3] = 1.0f;
        }
    }
    fused.data[0] = positions;
    fused.stride[0] = 12;
    fused.data[9] = directions;
    fused.stride[9] = 12;
    fused.elements = elements;
    fused.words = words_made;
    for (round = 0; round < ROUNDS; round++) {
        for (v = 0; v < VERTICES; v++) {
            float d[3];

            for (r = 0; r < 3; r++) {
                put_le_float(&positions[12 * v + 4 * r], random_component());
                d[r] = random_component();
            }
            // (the edges: a direction too short, one straight behind)
            if (round == 0 && v == 1) {
                d[0] = d[1] = d[2] = 0.0f;
            } else if (round == 0 && v == 2) {
                d[0] = d[1] = 0.0f;
                d[2] = -0.75f;
            } else if (round == 0 && v == 3) {
                // (nearly behind, where L + z would cancel)
                d[0] = 3.0e-6f;
                d[1] = -1.0e-6f;
                d[2] = -2.0f;
            } else if (round == 0 && v == 4) {
                d[0] = -1.0e-3f;
                d[1] = 2.0e-3f;
                d[2] = -0.5f;
            }
            for (r = 0; r < 3; r++) {
                put_le_float(&directions[12 * v + 4 * r], d[r]);
            }
            elements[v] = v;
            words_made[v] = words[v];
        }
        memset(words, 0, sizeof words);
        form->run((const float (*)[4])constants, &fused, VERTICES, form->key.live);
        for (v = 0; v < VERTICES; v++) {
            float d[3], st[2];
            double exact[2];

            for (r = 0; r < 3; r++) {
                const unsigned char *p = &directions[12 * v + 4 * r];
                uint32_t bits = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;

                memcpy(&d[r], &bits, 4);
            }
            wl_nv2a_sphere_coordinates(d, st);
            sphere_exact(d, exact);
            checked++;
            for (r = 0; r < 2; r++) {
                float made;
                uint32_t bits = words[v][3 + r];
                double apart;

                memcpy(&made, &bits, 4);
                apart = round == 0 && (v == 1 || v == 2) ? 0.0 : fabs((double)made - exact[r]);
                if (apart > largest) {
                    largest = apart;
                }
            }
            if (round == 0 && (v == 1 || v == 2) &&
                (words[v][3] != float_bits(st[0]) || words[v][4] != float_bits(st[1]))) {
                if (differing++ == 0) {
                    snprintf(detail, sizeof detail, "round %u vertex %u: fused %08x %08x, general %08x %08x", round, v,
                             (unsigned)words[v][3], (unsigned)words[v][4], (unsigned)float_bits(st[0]),
                             (unsigned)float_bits(st[1]));
                }
            }
        }
    }
    if (differing != 0) {
        printf("  %u of %u edge vertices differ; the first %s\n", differing, checked, detail);
    }
    printf("  the coordinates %g apart at most\n", largest);
    ASSERT_EQ(0u, differing);
    ASSERT(largest <= 1.0 / 4194304.0);
    PASS();
}

// a form of the two-node blend (683e337f), whose prefix (the blended rows) a vertex takes from
// the vertex before when its node indices' and weight's bytes are the same
static const WlXboxVshFusedForm *blend_form(void) {
    unsigned long i;

    for (i = 0; i < test_vsh_fused_natives_count; i++) {
        if (test_vsh_fused_natives[i].hash == 0x683e337fUL && test_vsh_fused_natives[i].fused_count == 1u) {
            return &test_vsh_fused_natives[i].fused[0];
        }
    }
    return NULL;
}

// a batch's words, the same to the bit as each vertex run alone (its prefix always made): runs of
// vertices sharing nodes and weights, a new weight under the same nodes, and the shorts -32768 and
// -32767 (other bytes, the same float)
TEST fused_blend_kept_from_the_vertex_before_is_the_same(void) {
    static float constants[192][4];
    static WlXboxVshFused fused;
    static unsigned char positions[VERTICES * 12], normals[VERTICES * 12], nodes[VERTICES * 2],
        weights[VERTICES * 2];
    static uint32_t elements[VERTICES], batch[VERTICES][WORDS], alone[VERTICES][WORDS];
    static uint32_t *words_made[VERTICES];
    const WlXboxVshFusedForm *form = blend_form();
    unsigned round, v, r, differing = 0;
    char detail[160] = "";

    ASSERT(form != NULL);
    for (r = 0; r < 192; r++) {
        unsigned c;

        for (c = 0; c < 4; c++) {
            constants[r][c] = random_component();
        }
    }
    constants[7][3] = 765.0f;
    memset(&fused, 0, sizeof fused);
    fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES] = fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_HALF] = fused.pack[WL_XBOX_VSH_FUSED_HALF + 1] = 0.5f;
    fused.pack[WL_XBOX_VSH_FUSED_DEPTH] = fused.pack[WL_XBOX_VSH_FUSED_DEPTH + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_ONES] = fused.pack[WL_XBOX_VSH_FUSED_ONES + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_SHORT] = fused.pack[WL_XBOX_VSH_FUSED_SHORT + 1] = 1.0f / 32767.0f;
    fused.pack[WL_XBOX_VSH_FUSED_BYTE] = fused.pack[WL_XBOX_VSH_FUSED_BYTE + 1] = 1.0f / 255.0f;
    fused.pack[WL_XBOX_VSH_FUSED_NEGATIVE] = fused.pack[WL_XBOX_VSH_FUSED_NEGATIVE + 1] = -1.0f;
    fused.data[0] = positions;
    fused.stride[0] = 12;
    fused.data[1] = normals;
    fused.stride[1] = 12;
    fused.data[5] = nodes;
    fused.stride[5] = 2;
    fused.data[6] = weights;
    fused.stride[6] = 2;
    fused.elements = elements;
    for (round = 0; round < ROUNDS; round++) {
        for (v = 0; v < VERTICES; v++) {
            int weight = (int)(next_random() % 65535u) - 32767;

            for (r = 0; r < 3; r++) {
                put_le_float(&positions[12 * v + 4 * r], random_component());
                put_le_float(&normals[12 * v + 4 * r], random_component());
            }
            if (v % 4u != 0u) {
                // (mostly the vertex before's nodes and weight)
                memcpy(&nodes[2 * v], &nodes[2 * (v - 1u)], 2);
                if (v % 8u != 3u) {
                    memcpy(&weights[2 * v], &weights[2 * (v - 1u)], 2);
                    weight = 0x10000;
                }
            } else {
                nodes[2 * v] = (unsigned char)(next_random() % 44u);
                nodes[2 * v + 1] = (unsigned char)(next_random() % 44u);
            }
            if (v % 16u == 6u) {
                weight = -32768;
            } else if (v % 16u == 7u) {
                weight = -32767;
            }
            if (weight != 0x10000) {
                weights[2 * v] = (unsigned char)weight;
                weights[2 * v + 1] = (unsigned char)((unsigned)weight >> 8);
            }
            elements[v] = v;
        }
        for (r = 0; r < 2; r++) {
            for (v = 0; v < 16; v++) {
                memset(fused.inputs[r][v], 0, sizeof fused.inputs[r][v]);
                fused.inputs[r][v][3] = 1.0f;
            }
        }
        for (v = 0; v < VERTICES; v++) {
            words_made[v] = batch[v];
        }
        fused.words = words_made;
        memset(batch, 0, sizeof batch);
        form->run((const float (*)[4])constants, &fused, VERTICES, form->key.live);
        memset(alone, 0, sizeof alone);
        for (v = 0; v < VERTICES; v++) {
            uint32_t *one = alone[v];

            fused.elements = &elements[v];
            fused.words = &one;
            form->run((const float (*)[4])constants, &fused, 1, form->key.live);
        }
        fused.elements = elements;
        for (v = 0; v < VERTICES; v++) {
            if (memcmp(batch[v], alone[v], sizeof batch[v]) != 0 && differing++ == 0) {
                snprintf(detail, sizeof detail, "round %u vertex %u: batch %08x %08x %08x %08x, alone %08x %08x %08x %08x",
                         round, v, (unsigned)batch[v][0], (unsigned)batch[v][1], (unsigned)batch[v][2],
                         (unsigned)batch[v][3], (unsigned)alone[v][0], (unsigned)alone[v][1],
                         (unsigned)alone[v][2], (unsigned)alone[v][3]);
            }
        }
    }
    if (differing != 0) {
        printf("  %u vertices differ; the first %s\n", differing, detail);
    }
    ASSERT_EQ(0u, differing);
    PASS();
}

// the lit program's forms (4fd1043b): lit 0 its own, 1 the one GX lights (perspective 0, the screen's);
// x: the one whose position is GX's transform unit's
static const WlXboxVshFusedForm *lit_form_of(unsigned lit, unsigned perspective) {
    unsigned long i, f;

    for (i = 0; i < test_vsh_fused_natives_count; i++) {
        if (test_vsh_fused_natives[i].hash == 0x4fd1043bUL) {
            for (f = 0; f < test_vsh_fused_natives[i].fused_count; f++) {
                if (test_vsh_fused_natives[i].fused[f].key.lit == lit &&
                    test_vsh_fused_natives[i].fused[f].key.perspective == perspective) {
                    return &test_vsh_fused_natives[i].fused[f];
                }
            }
        }
    }
    return NULL;
}

static const WlXboxVshFusedForm *lit_form(unsigned lit) {
    return lit_form_of(lit, 0);
}

// the lit form's words: its form's position, then the normal (the program's r11, v1 times c[12].w,
// to the bit), d0's alpha byte as its form packs it, and the texture coordinate
TEST fused_lit_form_sends_the_normal(void) {
    static float constants[192][4];
    static WlXboxVshFused fused;
    static unsigned char positions[VERTICES * 12], normals[VERTICES * 12], coordinates[VERTICES * 8];
    static uint32_t elements[VERTICES], plain[VERTICES][8], lit[VERTICES][10], xf[VERTICES][8];
    static uint32_t *words_made[VERTICES];
    const WlXboxVshFusedForm *form = lit_form(0), *lit_one = lit_form(1),
                             *xf_one = lit_form_of(0, WL_XBOX_VSH_FUSED_XF);
    unsigned v, r, c;

    ASSERT(form != NULL);
    ASSERT(lit_one != NULL);
    ASSERT(xf_one != NULL);
    ASSERT_EQ(0u, xf_one->key.live & WL_XBOX_VSH_LIVE_POSITION);
    for (r = 0; r < 192; r++) {
        for (c = 0; c < 4; c++) {
            constants[r][c] = random_component();
        }
    }
    constants[96 + 36][0] = 0.625f;
    memset(&fused, 0, sizeof fused);
    fused.pack[WL_XBOX_VSH_FUSED_SCALE] = fused.pack[WL_XBOX_VSH_FUSED_SCALE + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES] = fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_HALF] = fused.pack[WL_XBOX_VSH_FUSED_HALF + 1] = 0.5f;
    fused.pack[WL_XBOX_VSH_FUSED_255] = fused.pack[WL_XBOX_VSH_FUSED_255 + 1] = 255.0f;
    fused.pack[WL_XBOX_VSH_FUSED_DEPTH] = fused.pack[WL_XBOX_VSH_FUSED_DEPTH + 1] = 1.0f;
    fused.pack[WL_XBOX_VSH_FUSED_ONES] = fused.pack[WL_XBOX_VSH_FUSED_ONES + 1] = 1.0f;
    fused.data[0] = positions;
    fused.stride[0] = 12;
    fused.data[1] = normals;
    fused.stride[1] = 12;
    fused.data[4] = coordinates;
    fused.stride[4] = 8;
    fused.elements = elements;
    fused.words = words_made;
    for (v = 0; v < VERTICES; v++) {
        for (r = 0; r < 3; r++) {
            put_le_float(&positions[12 * v + 4 * r], random_component());
            put_le_float(&normals[12 * v + 4 * r], random_component());
        }
        put_le_float(&coordinates[8 * v], random_component());
        put_le_float(&coordinates[8 * v + 4], random_component());
        elements[v] = v;
    }
    for (r = 0; r < 2; r++) {
        for (v = 0; v < 16; v++) {
            fused.inputs[r][v][3] = 1.0f;
        }
    }
    for (v = 0; v < VERTICES; v++) {
        words_made[v] = plain[v];
    }
    form->run((const float (*)[4])constants, &fused, VERTICES, form->key.live);
    for (v = 0; v < VERTICES; v++) {
        words_made[v] = lit[v];
    }
    lit_one->run((const float (*)[4])constants, &fused, VERTICES, lit_one->key.live);
    for (v = 0; v < VERTICES; v++) {
        words_made[v] = xf[v];
    }
    xf_one->run((const float (*)[4])constants, &fused, VERTICES, xf_one->key.live);
    for (v = 0; v < VERTICES; v++) {
        // (x: the position's words v0's as the arrays hold it, the rest the form's own)
        for (r = 0; r < 3; r++) {
            ASSERT_EQ(float_bits(get_le_float(&positions[12 * v + 4 * r])), xf[v][r]);
        }
        for (r = 3; r < 6; r++) {
            ASSERT_EQ(plain[v][r], xf[v][r]);
        }
    }
    for (v = 0; v < VERTICES; v++) {
        for (r = 0; r < 3; r++) {
            float n;

            n = get_le_float(&normals[12 * v + 4 * r]);
            ASSERT_EQ(plain[v][r], lit[v][r]);
            ASSERT_EQ(float_bits(n * constants[96 + 12][3]), lit[v][3 + r]);
        }
        // (d0's alpha byte, 0.625 * 255 + 1/2, alone: GX lights its r, g and b)
        for (c = 0, r = 0; c < 4; c++) {
            if (((const unsigned char *)&lit[v][6])[c] != 0) {
                ASSERT_EQ(159, ((const unsigned char *)&lit[v][6])[c]);
                ASSERT_EQ(159, ((const unsigned char *)&plain[v][3])[c]);
                r++;
            }
        }
        ASSERT_EQ(1u, r);
        ASSERT_EQ(plain[v][4], lit[v][7]);
        ASSERT_EQ(plain[v][5], lit[v][8]);
    }
    PASS();
}

// the program to the screen (e53b834b, its clip-space position captured) and its form of perspective
// (perspective 1) or of the screen (0)
static const WlXboxVshFusedForm *screen_form(unsigned perspective, const WlXboxVshNative **native) {
    unsigned long i, f;

    for (i = 0; i < test_vsh_fused_natives_count; i++) {
        if (test_vsh_fused_natives[i].hash == 0xe53b834bUL) {
            *native = &test_vsh_fused_natives[i];
            for (f = 0; f < test_vsh_fused_natives[i].fused_count; f++) {
                if (test_vsh_fused_natives[i].fused[f].key.perspective == perspective) {
                    return &test_vsh_fused_natives[i].fused[f];
                }
            }
        }
    }
    return NULL;
}

// the screen program's forms for a target of 2 x 4 samples a pixel: the runtime gives the viewport's
// scale and offset already times 1 / the samples (nv2a_gx.c's fused pack), so the perspective form's
// x w and y w are positions_pack's ((clip scale + offset w) / samples + w / 2) to the bit, scaled once
// (not again by 1 / the samples, which a target of one sample a pixel cannot tell), and the screen
// form's x and y are (position / samples + 1/2)
TEST fused_forms_scale_by_the_samples_once(void) {
    static float constants[192][4];
    static WlXboxVshFused fused;
    static unsigned char positions[VERTICES * 12];
    static uint32_t elements[VERTICES], clipped[VERTICES][4], screen[VERTICES][4];
    static uint32_t *words_made[VERTICES];
    static const float scale[2] = {320.0f, -240.0f}, offset[2] = {320.5f, 240.25f}, to[2] = {0.5f, 0.25f};
    const WlXboxVshNative *native = NULL;
    const WlXboxVshFusedForm *form = screen_form(1, &native), *plain = screen_form(0, &native);
    unsigned v, r, c;

    ASSERT(form != NULL);
    ASSERT(plain != NULL);
    ASSERT(native != NULL);
    for (r = 0; r < 192; r++) {
        for (c = 0; c < 4; c++) {
            constants[r][c] = random_component();
        }
    }
    memset(&fused, 0, sizeof fused);
    for (c = 0; c < 2; c++) {
        fused.pack[WL_XBOX_VSH_FUSED_SCALE + c] = scale[c] * to[c];
        fused.pack[WL_XBOX_VSH_FUSED_OFFSET + c] = offset[c] * to[c];
        fused.pack[WL_XBOX_VSH_FUSED_TO_SAMPLES + c] = to[c];
        fused.pack[WL_XBOX_VSH_FUSED_HALF + c] = 0.5f;
        fused.pack[WL_XBOX_VSH_FUSED_ONES + c] = 1.0f;
    }
    fused.pack[WL_XBOX_VSH_FUSED_DEPTH] = fused.pack[WL_XBOX_VSH_FUSED_DEPTH + 1] = 1.0f;
    fused.data[0] = positions;
    fused.stride[0] = 12;
    fused.elements = elements;
    fused.words = words_made;
    for (v = 0; v < VERTICES; v++) {
        for (r = 0; r < 3; r++) {
            put_le_float(&positions[12 * v + 4 * r], random_component());
        }
        elements[v] = v;
    }
    for (r = 0; r < 2; r++) {
        for (v = 0; v < 16; v++) {
            fused.inputs[r][v][3] = 1.0f;
        }
    }
    for (v = 0; v < VERTICES; v++) {
        words_made[v] = clipped[v];
    }
    form->run((const float (*)[4])constants, &fused, VERTICES, form->key.live);
    for (v = 0; v < VERTICES; v++) {
        words_made[v] = screen[v];
    }
    plain->run((const float (*)[4])constants, &fused, VERTICES, plain->key.live);
    for (v = 0; v < VERTICES; v++) {
        float inputs[16][4];
        WlXboxVshOutput out;

        memset(inputs, 0, sizeof inputs);
        memset(&out, 0, sizeof out);
        for (r = 0; r < 16; r++) {
            inputs[r][3] = 1.0f;
        }
        for (r = 0; r < 3; r++) {
            inputs[0][r] = get_le_float(&positions[12 * v + 4 * r]);
        }
        native->run((const float (*)[4])constants, (const float (*)[4])inputs, &out, WL_XBOX_VSH_LIVE_POSITION);
        ASSERT_EQ(1, out.clip_captured);
        for (c = 0; c < 2; c++) {
            // (each step rounded as a float, as positions_pack's: volatile against a fused multiply-add)
            volatile float cw = out.clip[3], product = out.clip[c] * scale[c], moved = offset[c] * cw;
            volatile float sum = product + moved, samples = sum * to[c], half = 0.5f * cw;
            volatile float position = out.position[c] * to[c];

            ASSERT_EQ(float_bits(samples + half), clipped[v][c]);
            ASSERT_EQ(float_bits(position + 0.5f), screen[v][c]);
        }
        ASSERT_EQ(float_bits(-out.clip[3]), clipped[v][2]);
    }
    PASS();
}

// a form's text read back the same: interleaved arrays ("I"), projected stages ("G", "H"), sphere maps
// GX makes ("U") and shorts as they are ("SK")
TEST fused_form_text_round_trips(void) {
    static const char *const texts[] = {"23 p 1 1I 0G,1P 0:UB.3,8:SK.1,9:UB.3",
                                        "1e7 p 2 3LI 2H,0P,1P,3S 0:F3,1:CMP,4:S1.2,5:UB.2,6:SK.4",
                                        "1e7 x 2 3LI 0P,1U,3U 0:F3,1:CMP,4:S1.2",
                                        "106 w 2 3I 0M,1M,2M,3U 0:F3,1:CMP,4:S1.2"};
    WlXboxVshFusedKey key;
    char text[200];
    unsigned n;

    for (n = 0; n < sizeof texts / sizeof texts[0]; n++) {
        ASSERT(wl_xbox_vsh_fused_parse(texts[n], &key) > 0);
        ASSERT_EQ(1u, key.interleaved);
        wl_xbox_vsh_fused_text(&key, text, sizeof text);
        ASSERT_STR_EQ(texts[n], text);
    }
    ASSERT(wl_xbox_vsh_fused_parse(texts[0], &key) > 0);
    ASSERT_EQ(WL_XBOX_VSH_FUSED_STAGE_PROJECTED, key.stages[0] >> 2);
    ASSERT_EQ(WL_XBOX_VSH_FUSED_S32K, key.format[8]);
    ASSERT(wl_xbox_vsh_fused_parse(texts[3], &key) > 0);
    ASSERT_EQ(WL_XBOX_VSH_FUSED_STAGE_INPUT, key.stages[1] >> 2);
    PASS();
}

SUITE(xbox_vsh_fused) {
    RUN_TEST(fused_form_text_round_trips);
    RUN_TEST(fused_sphere_stage_is_close_to_the_general_path);
    RUN_TEST(fused_blend_kept_from_the_vertex_before_is_the_same);
    RUN_TEST(fused_lit_form_sends_the_normal);
    RUN_TEST(fused_forms_scale_by_the_samples_once);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(xbox_vsh_fused);
    GREATEST_MAIN_END();
}
