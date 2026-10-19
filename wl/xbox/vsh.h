// SPDX-License-Identifier: MIT
// The Xbox's vertex programs (NV2A vertex microcode) run on the processor (vsh.c): GX has no
// programmable vertex stage, so a port of an Xbox game runs each draw's vertices through the
// game's program here, and GX draws the results in screen space.
//
// A program is decoded once (wl_xbox_vsh_compile) from its instructions, four words each as the
// Xbox's Direct3D takes a vertex shader's function after its header word, then run for each vertex
// (wl_xbox_vsh_run, or wl_xbox_vsh_run_live for the outputs a draw reads). Programs known ahead of
// time can be compiled to C (tools/xbox_vsh_codegen.c; vsh_native.h): compile then finds a
// program's function in the table given by a hash of its words, and runs that instead of
// interpreting it, to the bit the same. The analysis (wl_xbox_vsh_analyze*) says what each output
// depends on: which inputs, whether affinely (then GX's matrices can compute it), whether through
// the constants.
#pragma once

#include <stdint.h>

enum {
    WL_XBOX_VSH_CONSTANT_COUNT = 192,
    /* c[-96] is the first */
    WL_XBOX_VSH_CONSTANT_BIAS = 96,
    WL_XBOX_VSH_INPUT_COUNT = 16,
    WL_XBOX_VSH_MAX_INSTRUCTIONS = 136
};

typedef struct WlXboxVshOperand {
    unsigned char mux, index, negate;
    unsigned char swizzle[4];
} WlXboxVshOperand;

typedef struct WlXboxVshInstruction {
    unsigned char mac, ilu;
    unsigned char mac_mask, ilu_mask, output_mask;
    unsigned char temporary, output_address, output_is_register, output_from_ilu, relative;
    /* the screen-space conversion's reciprocal of the clip-space w: the
    position is kept as it was before it (clip_captured) */
    unsigned char captures_clip;
    unsigned char input, constant;
    WlXboxVshOperand operands[3];
} WlXboxVshInstruction;

typedef struct WlXboxVshOutput WlXboxVshOutput;

/* a program compiled ahead of time (vsh_native.h), for the outputs
in live (WL_XBOX_VSH_LIVE_...) */
typedef void (*WlXboxVshLiveProc)(const float (*constants)[4], const float (*inputs)[4],
                                  WlXboxVshOutput *output, unsigned live);
/* ... and its colours alone */
typedef void (*WlXboxVshColorsProc)(const float (*constants)[4], const float (*inputs)[4],
                                    WlXboxVshOutput *output);
/* ... and its position alone, for four values of v0 (points) at once, the
other inputs those given: each one's clip-space position (clip, as the
program captures it) and screen-space position, the same to the bit as
four runs for the position (wl_xbox_vsh_run_positions) */
typedef void (*WlXboxVshPositionsProc)(const float (*constants)[4], const float (*inputs)[4],
                                       const float (*points)[4], float (*clips)[4],
                                       float (*positions)[4]);

/* ... and the program for count vertices (inputs[n] into outputs[n]), the
point size not set */
typedef void (*WlXboxVshBatchProc)(const float (*constants)[4], const float (*inputs)[16][4],
                                   WlXboxVshOutput *outputs, unsigned count, unsigned live);

/* ... and fused (the Wii's, tools/xbox_vsh_pairs.c): count vertices read from
their arrays, run and packed as GX takes them (WlXboxVshFused), for one form of
the arrays and of GX's vertex (WlXboxVshFusedKey) */
typedef struct WlXboxVshFused WlXboxVshFused;
typedef void (*WlXboxVshFusedProc)(const float (*constants)[4], WlXboxVshFused *fused, unsigned count,
                                   unsigned live);

/* a fused form: each input register's array format (WL_XBOX_VSH_FUSED_...,
as the fetch reads it; NONE: no array, the register keeps its start value),
the outputs read, and GX's vertex: its position (perspective: x w, y w, -w
where the program captured the clip-space position, else the screen-space
position and depth; WL_XBOX_VSH_FUSED_XF: v0's floats as fetched, which GX's
transform unit transforms, the program's position not made), a lit form's normal, its colours (count, and bit n: the
program gives dn, else 0), its texture coordinates in order (stage |
WL_XBOX_VSH_FUSED_STAGE_... << 2). A batch whose draw has another form (or fog, projected coordinates,
landings) runs the general way. */
enum {
    WL_XBOX_VSH_FUSED_NONE,
    WL_XBOX_VSH_FUSED_F1,
    WL_XBOX_VSH_FUSED_F2,
    WL_XBOX_VSH_FUSED_F3,
    WL_XBOX_VSH_FUSED_F4,
    WL_XBOX_VSH_FUSED_D3DCOLOR,
    WL_XBOX_VSH_FUSED_CMP,
    /* (normalized shorts and bytes, then shorts as they are: 1 to 4 of them) */
    WL_XBOX_VSH_FUSED_S1 = 7,
    WL_XBOX_VSH_FUSED_UB = 11,
    WL_XBOX_VSH_FUSED_S32K = 15,
    WL_XBOX_VSH_FUSED_FORMATS = 19
};
/* (perspective: 0 the screen's position, 1 perspective's, WL_XBOX_VSH_FUSED_XF the transform unit's: "o", "p"
and "x" in a form's text; WL_XBOX_VSH_FUSED_WORLD, "w": the x, y and z of the temporary the program's
clip-space position is made from (WlXboxVshWorldPosition), the clip-space w the program captures in place
of one of them (WlXboxVshFused's world_slot), which GX's position matrix (the four constants and the
viewport) transforms; the clip-space x, y and z not made) */
enum { WL_XBOX_VSH_FUSED_XF = 2, WL_XBOX_VSH_FUSED_WORLD = 3 };
enum {
    WL_XBOX_VSH_FUSED_STAGE_PLAIN,
    WL_XBOX_VSH_FUSED_STAGE_LINEAR,
    WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE,
    WL_XBOX_VSH_FUSED_STAGE_PROJECTIVE_LINEAR,
    WL_XBOX_VSH_FUSED_STAGE_SPHERE,
    /* (projected: (s, t, q) undivided, for GX to divide at each pixel, sent first after the position
    as GX's normal, binormal and tangent: three words for one, nine for two or three, the unused zero;
    "G", and "H" for a linear texture's) */
    WL_XBOX_VSH_FUSED_STAGE_PROJECTED,
    WL_XBOX_VSH_FUSED_STAGE_PROJECTED_LINEAR,
    /* (a sphere map's made by GX's transform unit from the direction of unit length the form sends,
    as a projected coordinate's (s, t, q) are, after a lit form's normal: WlGxDirectLayout's
    texcoords_sphere; "U") */
    WL_XBOX_VSH_FUSED_STAGE_SPHERE_XF,
    /* (made by GX's transform unit through a matrix from the one input register the program makes the
    coordinate from, affinely (wl_xbox_vsh_texgen_source), its x and y as the fetch made them: sent once,
    in the first such stage's coordinate, for every stage made from that register (the others not sent,
    GX's texgen reading the first's input); the stage's output not made, not in the form's live; "M") */
    WL_XBOX_VSH_FUSED_STAGE_INPUT
};
/* (a build whose forms may have sphere maps made by GX: its fused key's) */
#define WL_XBOX_VSH_FUSED_SPHERE_XF 1
typedef struct WlXboxVshFusedKey {
    unsigned short live;
    unsigned char format[16];
    unsigned char perspective, colors, channels, stage_count;
    unsigned char stages[4];
    /* d0's r, g and b lit by GX's transform unit (the program's light shape,
    wl_xbox_vsh_light_shape): its normal's three floats sent after the
    position, d0's word then its alpha alone (its r, g and b unread) */
    unsigned char lit;
    /* every array one stride (one vertex buffer's attributes): the element's offset made once, not
    once an array ("I" in a form's text). A draw of such arrays runs a form without it when it has none
    with it */
    unsigned char interleaved;
} WlXboxVshFusedKey;
typedef struct WlXboxVshFusedForm {
    WlXboxVshFusedKey key;
    WlXboxVshFusedProc run;
} WlXboxVshFusedForm;

/* the outputs a draw reads (wl_xbox_vsh_run_live): the others are left as they
were, or hold what the instructions computed on the way */
enum {
    /* (position, clip and clip_captured) */
    WL_XBOX_VSH_LIVE_POSITION = 1 << 0,
    WL_XBOX_VSH_LIVE_D0 = 1 << 1,
    WL_XBOX_VSH_LIVE_D1 = 1 << 2,
    /* (b0 and b1) */
    WL_XBOX_VSH_LIVE_BACK = 1 << 3,
    WL_XBOX_VSH_LIVE_POINT_SIZE = 1 << 4,
    /* (t[n]: WL_XBOX_VSH_LIVE_T0 << n) */
    WL_XBOX_VSH_LIVE_T0 = 1 << 5,
    WL_XBOX_VSH_LIVE_FOG = 1 << 9,
    WL_XBOX_VSH_LIVE_ALL = (1 << 10) - 1
};

/* a program compiled ahead of time, as tools/xbox_vsh_codegen.c lists them:
found by its words' hash (wl_xbox_vsh_hash) and instruction count, and
their second hash (wl_xbox_vsh_check), so that another program of the same
hash and count is not taken for it */
typedef struct WlXboxVshNative {
    unsigned long hash, count, check;
    /* (for the outputs a draw reads: WL_XBOX_VSH_LIVE_...) */
    WlXboxVshLiveProc run;
    /* its colours (d0, d1) and fog alone */
    WlXboxVshColorsProc colors;
    /* its position for four values of v0 (NULL: none) */
    WlXboxVshPositionsProc positions;
    /* (for count vertices at once; NULL: run a vertex at a time) */
    WlXboxVshBatchProc batch;
    /* its fused forms (none off the Wii) */
    const WlXboxVshFusedForm *fused;
    unsigned fused_count;
} WlXboxVshNative;

/* an instruction as the interpreter runs it, its fields resolved once by
wl_xbox_vsh_compile: each operand's register file (WL_XBOX_VSH_FILE_...,
with WL_XBOX_VSH_NEGATE), its index (a temporary 0..12, 12 the position;
an input; a constant, kept in range unless relative) and its swizzle (two
bits a component, x's lowest); the registers the units write (12 the
position) with their masks (0: no write); the output written
(WL_XBOX_VSH_TO_...) and its mask; and the outputs its results reach
(WL_XBOX_VSH_LIVE_...: a draw that reads none of them skips it) */
enum {
    WL_XBOX_VSH_FILE_ZERO,
    WL_XBOX_VSH_FILE_TEMPORARY,
    WL_XBOX_VSH_FILE_INPUT,
    WL_XBOX_VSH_FILE_CONSTANT,
    WL_XBOX_VSH_FILE_RELATIVE,
    WL_XBOX_VSH_NEGATE = 0x80
};
enum {
    WL_XBOX_VSH_TO_NONE,
    WL_XBOX_VSH_TO_POSITION,
    WL_XBOX_VSH_TO_D0,
    WL_XBOX_VSH_TO_D1,
    WL_XBOX_VSH_TO_B0,
    WL_XBOX_VSH_TO_B1,
    WL_XBOX_VSH_TO_FOG,
    WL_XBOX_VSH_TO_POINT_SIZE,
    WL_XBOX_VSH_TO_T0
};
enum {
    /* (flags) */
    WL_XBOX_VSH_STEP_CAPTURES_CLIP = 1,
    WL_XBOX_VSH_STEP_ARL = 2,
    WL_XBOX_VSH_STEP_FROM_ILU = 4
};

typedef struct WlXboxVshStepOperand {
    unsigned char file, index, swizzle;
} WlXboxVshStepOperand;

typedef struct WlXboxVshStep {
    unsigned char mac, ilu, flags;
    unsigned char mac_register, mac_mask, ilu_register, ilu_mask;
    unsigned char output, output_mask;
    unsigned short reaches;
    WlXboxVshStepOperand operands[3];
} WlXboxVshStep;

/* A d0 GX's lighting can make (wl_xbox_vsh_light_shape): its r, g and b the
ambient c[ambient].xyz plus, for each light, c[color].xyz times
max(dot(n, s c[direction].xyz), 0) (s -1 where direction_negate), or with a
back term max(dot, b dot, 0), b = c[back].component (negated where
back_negate); n the xyz of temporary `normal` as the program ends (no
instruction writes it after the dot products read it), and the 0 of the
maximum an input's component no array gives (v[zero].zero_component, 0 at
every vertex where the register's array has fewer components: a draw's to
check). Constants by their register index (c[n - 96] in Direct3D's terms). */
enum { WL_XBOX_VSH_LIGHTS = 4 };
typedef struct WlXboxVshLight {
    unsigned char direction, direction_negate, color;
    unsigned char back, back_negate, back_component, has_back;
} WlXboxVshLight;
typedef struct WlXboxVshLightShape {
    /* the lights (0: d0 is not of the shape) */
    unsigned char count;
    unsigned char normal, ambient, zero, zero_component;
    WlXboxVshLight lights[WL_XBOX_VSH_LIGHTS];
} WlXboxVshLightShape;

/* A clip-space position GX's position matrix can make from a temporary
(wl_xbox_vsh_world_position): each of its x, y, z and w, as the program
captures it, a dph of the temporary (unswizzled, unnegated) and the constant
c[constant[n]] (c[n - 96] in Direct3D's terms; unswizzled, unnegated, not
relative), the temporary's x, y and z the same from the first of the four to
the capture (to_end: to the program's end, no instruction writing them after) */
typedef struct WlXboxVshWorldPosition {
    /* (0: the position is not of the shape) */
    unsigned char found, to_end;
    unsigned char temporary;
    unsigned char constant[4];
} WlXboxVshWorldPosition;

typedef struct WlXboxVshProgram {
    unsigned long count;
    /* (wl_xbox_vsh_hash: the generated programs' names) */
    unsigned long hash;
    /* (0 from wl_xbox_vsh_compile; its owner's to set, for caches keyed by
    the program's address to tell it from another compiled in its place:
    the pipeline's compile count) */
    uint32_t serial;
    WlXboxVshLiveProc native;
    /* (the compiled program's colours and fog alone; NULL on the
    interpreter) */
    WlXboxVshColorsProc native_colors;
    /* (its position for four values of v0; NULL on the interpreter) */
    WlXboxVshPositionsProc native_positions;
    /* (for many vertices at once; NULL on the interpreter) */
    WlXboxVshBatchProc native_batch;
    /* (its fused forms) */
    const WlXboxVshFusedForm *native_fused;
    unsigned native_fused_count;
    /* the temporaries read before they are written (bit n: r[n]), which
    the interpreter clears for each vertex */
    unsigned short exposed;
    /* an instruction captures the clip-space position (the run of any
    vertex for its position sets clip_captured) */
    unsigned char captures_clip;
    /* its d0's light shape, as wl_xbox_vsh_light_shape finds it */
    WlXboxVshLightShape light;
    /* its clip-space position's shape, as wl_xbox_vsh_world_position finds it */
    WlXboxVshWorldPosition world;
    WlXboxVshInstruction instructions[WL_XBOX_VSH_MAX_INSTRUCTIONS];
    WlXboxVshStep steps[WL_XBOX_VSH_MAX_INSTRUCTIONS];
} WlXboxVshProgram;

/* the vertices the interpreter runs together (wl_xbox_vsh_run_batch): each
instruction is decoded for all of them at once, and its registers are
this scratch, the caller's (for a thread whose stack is small) */
enum { WL_XBOX_VSH_BATCH = 16 };
typedef struct WlXboxVshWork {
    float r[WL_XBOX_VSH_BATCH][13][4];
    float mac[WL_XBOX_VSH_BATCH][4], ilu[WL_XBOX_VSH_BATCH][4];
    /* (the operands A, B and C, swizzled and signed) */
    float operands[3][WL_XBOX_VSH_BATCH][4];
    int a0[WL_XBOX_VSH_BATCH];
} WlXboxVshWork;

/* what a vertex leaves: the screen-space position the program computes
(pixels, and depth scaled by the viewport), the clip-space position before
its conversion if the program converts it, the colors, the texture
coordinates and the fog */
struct WlXboxVshOutput {
    float position[4];
    float clip[4];
    int clip_captured;
    float d0[4], d1[4], b0[4], b1[4];
    float t[4][4];
    float fog;
    float point_size;
};

/* a fused batch's (WlXboxVshFusedProc) state: what it reads, its scratch, where
it writes. The floats a pair at an even index (8 bytes aligned) */
enum {
    /* pack: (each a pair) the viewport's scale and offset in samples (each
    times 1 / the samples), 1 / the samples, 1/2,
    255, the depth's divisor (the depth's maximum, 1 for none) and 1, 1e-12
    and 0, each stage's 1 / its texture's size (1 when it is not linear),
    1 / 32767, 1 / 255, 1/2 and 1, 1 and 1, -1 and -1 */
    WL_XBOX_VSH_FUSED_SCALE = 0,
    WL_XBOX_VSH_FUSED_OFFSET = 2,
    WL_XBOX_VSH_FUSED_TO_SAMPLES = 4,
    WL_XBOX_VSH_FUSED_HALF = 6,
    WL_XBOX_VSH_FUSED_255 = 8,
    WL_XBOX_VSH_FUSED_DEPTH = 10,
    WL_XBOX_VSH_FUSED_TINY = 12,
    WL_XBOX_VSH_FUSED_TO_TEXELS = 14,
    WL_XBOX_VSH_FUSED_SHORT = 22,
    WL_XBOX_VSH_FUSED_BYTE = 24,
    WL_XBOX_VSH_FUSED_EDGE = 26,
    WL_XBOX_VSH_FUSED_ONES = 28,
    WL_XBOX_VSH_FUSED_NEGATIVE = 30,
    WL_XBOX_VSH_FUSED_PACK = 32,
    /* sphere: the sphere map's doubles: 1e-24, 1e76, 1.5, 0.5, 2.5e-13, 2 */
    WL_XBOX_VSH_FUSED_SPHERE = 6
};
struct WlXboxVshFused {
    double sphere[WL_XBOX_VSH_FUSED_SPHERE];
    float pack[WL_XBOX_VSH_FUSED_PACK];
    /* the inputs, the vertex before's and this one's: each register's start
    value (a register no array gives: the fetch's current value; one an array
    gives: (0, 0, 0, 1), its components then written over) */
    float inputs[2][16][4];
    WlXboxVshOutput output;
    uint32_t stride[16];
    uint32_t saved[2];
    /* each array's element 0 (by input register) */
    const unsigned char *data[16];
    /* the fetch's tables (a byte normalized; the packed normal's 11- and
    10-bit fields) */
    const float *byte_float, *cmp11, *cmp10;
    /* the vertices' elements, and where each one's words go */
    const uint32_t *elements;
    uint32_t *const *words;
    /* (a "w" form) the byte offset in the vertex of the world position's
    component the clip-space w is sent in place of (4 k: GX's position matrix
    recovers component k from w) */
    uint32_t world_slot;
};

/* what a program's outputs depend on, found from its instructions alone
(wl_xbox_vsh_analyze): each component's input registers (bit n: v[n]), and
WL_XBOX_VSH_NOT_AFFINE when it is not an affine function of them (a product of
two inputs, a reciprocal, a minimum, a lookup by an input's index, ...).
An affine output is A x + b for the draw's constants, which GX's matrices
can compute. */
enum { WL_XBOX_VSH_NOT_AFFINE = 1 << 16 };
/* (wl_xbox_vsh_analyze_constants: a component that reads a constant register,
c[n] or c[a0 + n], on the way; one without it is the vertex's alone, the
same for any constants) */
enum { WL_XBOX_VSH_READS_CONSTANT = 1 << 17 };

typedef struct WlXboxVshDependencies {
    unsigned long clip[4], position[4];
    unsigned long d0[4], d1[4], b0[4], b1[4];
    unsigned long t[4][4];
    unsigned long fog, point_size;
    int clip_captured;
    /* the input registers any instruction reads */
    unsigned long inputs;
    /* the constant registers it reads (bit n of word n / 32: c[n - 96]),
    all of them when it reads one by a0 */
    unsigned long constants[WL_XBOX_VSH_CONSTANT_COUNT / 32];
    int relative;
} WlXboxVshDependencies;

/* a program decoded from its instructions (four words each; at most
WL_XBOX_VSH_MAX_INSTRUCTIONS, the last the one with its end bit), with its
compiled functions when the table has them (natives: NULL, or native_count
of them) */
void wl_xbox_vsh_compile(const uint32_t *instructions, unsigned long instruction_count,
                         const WlXboxVshNative *natives, unsigned long native_count,
                         WlXboxVshProgram *program);
/* a program's words hashed (FNV-1a), which finds its function */
unsigned long wl_xbox_vsh_hash(const uint32_t *instructions, unsigned long instruction_count);
/* the words hashed a second way (MurmurHash3's, 32 bits, independent of
FNV-1a), which a compiled function's table entry carries too */
unsigned long wl_xbox_vsh_check(const uint32_t *instructions, unsigned long instruction_count);

/* the light shape of a program's d0 (WlXboxVshLightShape): 1 and *shape
filled, or 0 (shape->count 0) */
int wl_xbox_vsh_light_shape(const WlXboxVshProgram *program, WlXboxVshLightShape *shape);
/* the shape of a program's clip-space position (WlXboxVshWorldPosition): 1
and *world filled, or 0 (world->found 0) */
int wl_xbox_vsh_world_position(const WlXboxVshProgram *program, WlXboxVshWorldPosition *world);
/* a copy of the program whose d0's r, g and b no instruction writes (the
form that leaves them to GX's lighting: the instructions that made them then
reach nothing) */
void wl_xbox_vsh_light_unlit(const WlXboxVshProgram *program, WlXboxVshProgram *unlit);

/* a fused form as text, as tools/xbox_vsh_codegen.c's --fused file lists them
after the program's hash: "<live, hex> <p, o, x or w> <colours> <channels>[L]
<stages> <formats>" (L: lit), the stages "-" or each "<n><N, L, P, Q, S, G, H or U>" (plain, linear,
projective, both, sphere, projected, linear projected, sphere by GX), the formats "-" or each "<register>:<F1 ... F4,
D3D, CMP, S1.n, UB.n>", comma-separated. _text gives the length (snprintf's),
_parse the characters read (0: not a form; the key's every byte set) */
/* the input register a fused form's stage of kind WL_XBOX_VSH_FUSED_STAGE_INPUT is made from: the one
register (of at most two components in the form) its s and t read, affinely; -1 for none */
int wl_xbox_vsh_texgen_source(const WlXboxVshDependencies *dependencies, const WlXboxVshFusedKey *key,
                              unsigned stage);
/* the constant registers (c[n - 96]) the instructions that reach the outputs `live`
(WL_XBOX_VSH_LIVE_...) read, at most `most` of them into registers: their count, -1 when one
is read by a0 or there are more (what those outputs are, for any vertex, is a function of them) */
int wl_xbox_vsh_live_constants(const WlXboxVshProgram *program, unsigned live, unsigned char *registers, int most);
int wl_xbox_vsh_fused_text(const WlXboxVshFusedKey *key, char *text, unsigned long size);
int wl_xbox_vsh_fused_parse(const char *text, WlXboxVshFusedKey *key);

void wl_xbox_vsh_run(const WlXboxVshProgram *program, const float (*constants)[4],
                     const float (*inputs)[4], float point_size, WlXboxVshOutput *output);
/* wl_xbox_vsh_run for the outputs in live alone (WL_XBOX_VSH_LIVE_...): those are
the same, bit for bit, the others undefined */
void wl_xbox_vsh_run_live(const WlXboxVshProgram *program, const float (*constants)[4],
                          const float (*inputs)[4], float point_size, WlXboxVshOutput *output,
                          unsigned live);
/* wl_xbox_vsh_run_live for count vertices (inputs[n], outputs[n]), on the
interpreter WL_XBOX_VSH_BATCH at a time: the same results, to the bit */
void wl_xbox_vsh_run_batch(const WlXboxVshProgram *program, const float (*constants)[4],
                           const float (*inputs)[WL_XBOX_VSH_INPUT_COUNT][4], unsigned count,
                           float point_size, WlXboxVshOutput *outputs, unsigned live,
                           WlXboxVshWork *work);
/* the input registers the instructions that reach the outputs in live read
(bit n: v[n]): the others may hold anything for wl_xbox_vsh_run_live and
wl_xbox_vsh_run_batch */
unsigned long wl_xbox_vsh_inputs_read(const WlXboxVshProgram *program, unsigned live);
/* the outputs each instruction's results reach (needs[n] for instruction n,
WL_XBOX_VSH_LIVE_...), found backwards from the end a register's
component at a time */
void wl_xbox_vsh_live_needs(const WlXboxVshProgram *program, unsigned *needs);
/* a vertex's colours (d0, d1) and fog alone: the compiled colours function
where there is one, else all of the program */
void wl_xbox_vsh_run_colors(const WlXboxVshProgram *program, const float (*constants)[4],
                            const float (*inputs)[4], WlXboxVshOutput *output);
/* the clip-space and screen-space positions for four values of v0 (points;
the other inputs as given), as four wl_xbox_vsh_run_live for the position give
them, to the bit: 0 (nothing done) for a program without its compiled
function */
int wl_xbox_vsh_run_positions(const WlXboxVshProgram *program, const float (*constants)[4],
                              const float (*inputs)[4], const float (*points)[4], float (*clips)[4],
                              float (*positions)[4]);

/* the input registers the outputs in live (WL_XBOX_VSH_LIVE_...) read, as the
analysis found them (bit n: v[n]): the others may hold anything */
unsigned long wl_xbox_vsh_live_inputs(const WlXboxVshDependencies *dependencies, unsigned live);
void wl_xbox_vsh_analyze(const WlXboxVshProgram *program, WlXboxVshDependencies *dependencies);
/* wl_xbox_vsh_analyze for a draw whose input registers in uniform (bit n:
v[n]) hold the same values for every vertex */
void wl_xbox_vsh_analyze_uniform(const WlXboxVshProgram *program, unsigned long uniform,
                                 WlXboxVshDependencies *dependencies);
/* wl_xbox_vsh_analyze, with WL_XBOX_VSH_READS_CONSTANT in each component that
reads a constant register */
void wl_xbox_vsh_analyze_constants(const WlXboxVshProgram *program,
                                   WlXboxVshDependencies *dependencies);
