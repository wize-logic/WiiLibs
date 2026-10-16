// SPDX-License-Identifier: MIT
// TEV programs as data: a description of the GX texture environment (up to 16 stages of
// colour/alpha inputs a b c d, op, bias, scale, clamp and output register, plus the texture,
// texture-coordinate and colour-channel order, swap tables and konst selections), a builder with
// the allocation helpers a translator needs (resource merging, konst slots, a register pool),
// a validator for what the hardware would reject or what is probably a mistake, a software
// evaluator of a program on one pixel, and an apply step that writes the program to GX.
//
// Everything but wl_tev_apply* is portable C with no GX and no platform calls: programs are built,
// checked and evaluated on the host and in big-endian tests. The Wii backend is
// wl/platform/wii/tev_wii.c; the host backend (host/tev_host.c) records the last applied program.
// All enum values equal libogc's GX_* values (asserted when the Wii backend is compiled), so a
// stage maps to the hardware without a table.
//
// One TEV stage computes, for the colour (three channels at once) and for the alpha apart,
//     out = (d (+|-) ((1 - c) * a + c * b) + bias) * scale
// clamped to 0..255 (clamp on) or to -1024..1023 (clamp off), and writes it to one of four
// registers (PREV, REG0..REG2). The inputs a, b and c are 8 bits (a register's low 8 bits); d is
// the register's full signed 11-bit value, so an unclamped result can feed d and go on. The
// compare ops replace the blend with
//     out = d + (a > b (or ==) ? c : 0)
// Colour inputs are a register's rgb, a register's alpha in all three channels, the texel or the
// rasterised colour (rgb or alpha in all three), ONE, HALF, the stage's konst selection or ZERO.
// Alpha inputs are the registers' alphas, the texel's or rasteriser's alpha, the konst selection
// or ZERO. A stage reads every input before it writes its outputs.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"
#include "wl/platform/platform.h"

#define WL_TEV_MAX_STAGES 16u
#define WL_TEV_KONST_SLOTS 4u
#define WL_TEV_SWAP_TABLES 4u
#define WL_TEV_TEXMAPS 8u
#define WL_TEV_TEXCOORDS 8u
#define WL_TEV_CHANNELS 2u
#define WL_TEV_NONE 0xFFu // no texcoord, no texmap, no colour channel

// ---- inputs, ops, registers ---------------------------------------------------------------------

// Colour inputs (GX_CC_*). C* is a register's rgb, A* the register's alpha in all three channels.
enum {
    WL_TEV_CC_CPREV = 0, WL_TEV_CC_APREV, WL_TEV_CC_C0, WL_TEV_CC_A0, WL_TEV_CC_C1, WL_TEV_CC_A1,
    WL_TEV_CC_C2, WL_TEV_CC_A2, WL_TEV_CC_TEXC, WL_TEV_CC_TEXA, WL_TEV_CC_RASC, WL_TEV_CC_RASA,
    WL_TEV_CC_ONE, WL_TEV_CC_HALF, WL_TEV_CC_KONST, WL_TEV_CC_ZERO,
    WL_TEV_CC_COUNT
};
// Alpha inputs (GX_CA_*).
enum {
    WL_TEV_CA_APREV = 0, WL_TEV_CA_A0, WL_TEV_CA_A1, WL_TEV_CA_A2, WL_TEV_CA_TEXA, WL_TEV_CA_RASA,
    WL_TEV_CA_KONST, WL_TEV_CA_ZERO,
    WL_TEV_CA_COUNT
};
// Ops (GX_TEV_*). The compare ops take bias ZERO and scale 1. In the alpha half only the A8
// compares exist (the same codes as RGB8). R8 compares the red of a and b and applies the result
// to all three channels; GR16 and BGR24 compare green:red and blue:green:red as 16 and 24 bit
// numbers; RGB8 compares each channel by itself.
enum {
    WL_TEV_ADD = 0, WL_TEV_SUB = 1,
    WL_TEV_COMP_R8_GT = 8, WL_TEV_COMP_R8_EQ, WL_TEV_COMP_GR16_GT, WL_TEV_COMP_GR16_EQ,
    WL_TEV_COMP_BGR24_GT, WL_TEV_COMP_BGR24_EQ, WL_TEV_COMP_RGB8_GT, WL_TEV_COMP_RGB8_EQ,
    WL_TEV_COMP_A8_GT = WL_TEV_COMP_RGB8_GT, WL_TEV_COMP_A8_EQ = WL_TEV_COMP_RGB8_EQ
};
enum { WL_TEV_BIAS_ZERO = 0, WL_TEV_BIAS_ADDHALF, WL_TEV_BIAS_SUBHALF, WL_TEV_BIAS_COUNT };
// DIVIDE_2 halves the whole sum, bias included: (d +- lerp + bias) / 2. A bias with DIVIDE_2 is
// accepted: it is what Dolphin's TEV computes ((d + bias) << shift + lerp) >> 1, and what an exact
// signed dot product (lerp - 1/2) / 2 needs. Real hardware has not been checked.
enum { WL_TEV_SCALE_1 = 0, WL_TEV_SCALE_2, WL_TEV_SCALE_4, WL_TEV_DIVIDE_2, WL_TEV_SCALE_COUNT };
// Output registers (GX_TEVPREV, GX_TEVREG0..2). The program's result is PREV after the last stage.
enum { WL_TEV_PREV = 0, WL_TEV_REG0, WL_TEV_REG1, WL_TEV_REG2, WL_TEV_REG_COUNT };

// Rasterised colour channels of a stage's order (GX_COLOR0A0, GX_COLOR1A1, GX_COLORZERO).
// GX_ALPHA_BUMP and GX_ALPHA_BUMPN need indirect texturing, which this layer does not model.
enum { WL_TEV_CH_COLOR0A0 = 4, WL_TEV_CH_COLOR1A1 = 5, WL_TEV_CH_ZERO = 6 };

// Source channels of a swap table entry (GX_CH_*).
enum { WL_TEV_SWAP_R = 0, WL_TEV_SWAP_G, WL_TEV_SWAP_B, WL_TEV_SWAP_A };

// Konst selections (GX_TEV_KCSEL_*, GX_TEV_KASEL_*): a constant fraction n/8 or a channel of one
// of the four konst colours. The colour half's can read rgb (or one channel in all three), the
// alpha half's one channel. Use wl_tev_kcsel / wl_tev_kasel for the K registers.
// WL_TEV_KSEL_NONE is not a selection: WlTevNeeds uses it for "not asked for".
#define WL_TEV_KSEL_NONE 0xFFu
enum {
    WL_TEV_KSEL_1 = 0, WL_TEV_KSEL_7_8, WL_TEV_KSEL_3_4, WL_TEV_KSEL_5_8, WL_TEV_KSEL_1_2,
    WL_TEV_KSEL_3_8, WL_TEV_KSEL_1_4, WL_TEV_KSEL_1_8
};
typedef enum { WL_TEV_K_RGB = 0, WL_TEV_K_R, WL_TEV_K_G, WL_TEV_K_B, WL_TEV_K_A } WlTevKComp;

// The colour half's selection of konst `slot` (0..3): RGB as it is, or one channel in all three.
static inline uint8_t wl_tev_kcsel(uint32_t slot, WlTevKComp comp) {
    static const uint8_t base[5] = {0x0C, 0x10, 0x14, 0x18, 0x1C};
    return (uint8_t)(base[comp] + slot);
}
// The alpha half's selection: one channel (WL_TEV_K_RGB reads as alpha).
static inline uint8_t wl_tev_kasel(uint32_t slot, WlTevKComp comp) {
    static const uint8_t base[5] = {0x1C, 0x10, 0x14, 0x18, 0x1C};
    return (uint8_t)(base[comp] + slot);
}

// ---- the program --------------------------------------------------------------------------------

// One half (colour or alpha) of a stage.
typedef struct {
    uint8_t in[4]; // a, b, c, d: WL_TEV_CC_* (colour half) or WL_TEV_CA_* (alpha half)
    uint8_t op;    // WL_TEV_ADD, WL_TEV_SUB or a compare
    uint8_t bias;  // WL_TEV_BIAS_*
    uint8_t scale; // WL_TEV_SCALE_*
    uint8_t clamp; // 1: clamp to 0..255; 0: to -1024..1023
    uint8_t dest;  // WL_TEV_PREV, WL_TEV_REG0..2
} WlTevHalf;

typedef struct {
    uint8_t texcoord; // texture coordinate (0..7) or WL_TEV_NONE
    uint8_t texmap;   // texture map (0..7) or WL_TEV_NONE; both are set, or neither
    uint8_t channel;  // WL_TEV_CH_* or WL_TEV_NONE
    uint8_t ras_swap; // swap table (0..3) the rasterised colour is read through
    uint8_t tex_swap; // swap table the texel is read through
    uint8_t kcsel;    // konst selection the colour half's KONST reads
    uint8_t kasel;    // konst selection the alpha half's KONST reads
    WlTevHalf color;
    WlTevHalf alpha;
} WlTevStage;

typedef struct {
    uint8_t stage_count;
    WlTevStage stages[WL_TEV_MAX_STAGES];
    // Swap tables: entry [t][i] is the source channel (WL_TEV_SWAP_*) of the texel or colour's
    // channel i (r, g, b, a). wl_tev_program_init gives GX's defaults: RGBA, RRRA, GGGA, BBBA.
    uint8_t swap[WL_TEV_SWAP_TABLES][4];
    // Konst colours 0xRRGGBBAA. konst_mask: slots the program owns (apply writes them);
    // konst_external: slots whose value is set elsewhere (the application's GX state), where
    // konst[] holds the value the evaluator assumes. A K selection of a slot in neither is
    // flagged by the validator.
    uint32_t konst[WL_TEV_KONST_SLOTS];
    uint8_t konst_mask;
    uint8_t konst_external;
    // Values REG0..REG2 start with (apply sets them with GX_SetTevColor, which cannot set PREV),
    // by register bit (1 << WL_TEV_REG0 ...); other registers start at 0 in the evaluator.
    uint32_t preload[WL_TEV_REG_COUNT];
    uint8_t preload_mask;
} WlTevProgram;

// An empty program (no stages) with the default swap tables.
void wl_tev_program_init(WlTevProgram *p);
// Adds a stage that does nothing: no order, swap tables 0, konst 1, and both halves pass PREV
// through (d = PREV, dest PREV). NULL when the program has WL_TEV_MAX_STAGES.
WlTevStage *wl_tev_add_stage(WlTevProgram *p);
// Fills a stage's order (texcoord, texmap, channel; WL_TEV_NONE for none).
void wl_tev_stage_order(WlTevStage *s, uint8_t texcoord, uint8_t texmap, uint8_t channel);
// Fills a half: inputs a b c d, op, bias, scale, clamp, dest.
void wl_tev_stage_color(WlTevStage *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t op, uint8_t bias,
                        uint8_t scale, bool clamp, uint8_t dest);
void wl_tev_stage_alpha(WlTevStage *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t op, uint8_t bias,
                        uint8_t scale, bool clamp, uint8_t dest);
// Sets swap table `table` (0..3): output channel r, g, b, a reads source channel WL_TEV_SWAP_*.
void wl_tev_set_swap(WlTevProgram *p, uint32_t table, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// ---- konst slots, register pool, resource merging (what a translator allocates) -------------------

// The slot holding `rgba`, adding it when it is not there yet. -1 when all four slots are taken.
int wl_tev_konst_slot(WlTevProgram *p, uint32_t rgba);
// Marks `slot` as set elsewhere; the evaluator assumes `assumed_rgba` there.
void wl_tev_konst_external(WlTevProgram *p, uint32_t slot, uint32_t assumed_rgba);
// Gives REG0..REG2 a value before the first stage (GX_SetTevColor). PREV cannot be preloaded.
void wl_tev_preload(WlTevProgram *p, uint8_t reg, uint32_t rgba);

// A pool of the four registers, with a holder count each (a value that two names share holds
// the register twice).
typedef struct {
    uint8_t holders[WL_TEV_REG_COUNT];
} WlTevRegPool;
void wl_tev_pool_init(WlTevRegPool *pool);
// A free register, REG0..REG2 first and PREV last (the program's result lives in PREV), or -1.
// `avoid` is a mask of registers (1 << reg) not to hand out.
int wl_tev_pool_alloc(WlTevRegPool *pool, uint32_t avoid);
void wl_tev_pool_retain(WlTevRegPool *pool, uint8_t reg);
void wl_tev_pool_release(WlTevRegPool *pool, uint8_t reg);
bool wl_tev_pool_free(const WlTevRegPool *pool, uint8_t reg);

// What the inputs of a stage (or of several operations that want to share one) need from the
// stage's order: one texture (coordinate and map), one colour channel, the swap tables that
// would serve them, as sets (bit t = table t), and one konst selection for each half (a stage has
// one kcsel and one kasel). Two operations share a stage when their needs merge.
#define WL_TEV_SWAPS_ANY 0x0Fu
typedef struct {
    uint8_t texcoord, texmap; // WL_TEV_NONE: no texture
    uint8_t channel;          // WL_TEV_NONE: no colour channel
    uint8_t tex_swaps;        // swap tables the texel can be read through
    uint8_t ras_swaps;
    uint8_t kcsel, kasel;     // WL_TEV_KSEL_NONE: that half's KONST is not read; else the selection it needs
} WlTevNeeds;
void wl_tev_needs_clear(WlTevNeeds *n);
// Adds a texture read; false (and n unchanged) when n already names another texture or no swap
// table serves both.
bool wl_tev_needs_texture(WlTevNeeds *n, uint8_t texcoord, uint8_t texmap, uint8_t swaps);
bool wl_tev_needs_channel(WlTevNeeds *n, uint8_t channel, uint8_t swaps);
// Adds a read of the colour half's (alpha half's) KONST through `sel` (a wl_tev_kcsel / wl_tev_kasel
// value or a WL_TEV_KSEL_n/8 fraction); false (and n unchanged) when n already names another selection.
bool wl_tev_needs_konst_color(WlTevNeeds *n, uint8_t sel);
bool wl_tev_needs_konst_alpha(WlTevNeeds *n, uint8_t sel);
// Merges `other` into `into`; false (and into unchanged) when they conflict.
bool wl_tev_needs_merge(WlTevNeeds *into, const WlTevNeeds *other);
// Sets the stage's order, swap tables (the lowest table of each set) and konst selections from `n`;
// a selection `n` does not name is left as the stage has it.
void wl_tev_stage_set_needs(WlTevStage *s, const WlTevNeeds *n);

// ---- validation ---------------------------------------------------------------------------------

typedef enum {
    WL_TEV_OK = 0,
    // errors: the hardware would reject the program or compute something else
    WL_TEV_E_NO_STAGES,       // stage_count is 0 or above WL_TEV_MAX_STAGES
    WL_TEV_E_INPUT,           // an input code beyond its enum
    WL_TEV_E_OP,              // an op that does not exist (or an R8/GR16/BGR24 compare in the alpha half)
    WL_TEV_E_BIAS_SCALE,      // bias or scale out of range, or a compare with a bias or scale
    WL_TEV_E_DEST,            // a destination register out of range
    WL_TEV_E_ORDER,           // texcoord, texmap or channel out of range, or texcoord without texmap
    WL_TEV_E_CHANNEL,         // a channel the layer does not model (alpha bump)
    WL_TEV_E_SWAP,            // a swap table out of range, or a table entry that is not a channel
    WL_TEV_E_KSEL,            // a konst selection with no meaning
    WL_TEV_E_TEXTURE_UNBOUND, // the stage reads TEXC/TEXA without a texmap
    WL_TEV_E_RAS_UNBOUND,     // the stage reads RASC/RASA without a colour channel
    WL_TEV_E_PRELOAD,         // a preload of PREV (GX_SetTevColor takes REG0..REG2 only)
    // warnings: legal, but probably a mistake
    WL_TEV_W_KONST_UNSET,     // KONST reads a K register the program neither sets nor marks external
    WL_TEV_W_READ_UNWRITTEN,  // a register is read before any stage wrote it and without a preload: its value is whatever GX holds (0 here)
    WL_TEV_W_DEAD_WRITE,      // a register written and overwritten without a read, or never read
    WL_TEV_W_NO_RESULT        // no stage writes PREV's colour (or alpha): the result's half is 0
} WlTevCode;

#define WL_TEV_MAX_ISSUES 24u
typedef struct {
    WlTevCode code;
    uint8_t stage;  // the stage (the one that wrote it, for a dead write)
    uint8_t alpha;  // 0: colour half, 1: alpha half
    uint8_t detail; // the input index, register or field the issue is about
} WlTevIssue;
typedef struct {
    WlTevIssue issues[WL_TEV_MAX_ISSUES];
    uint32_t count;    // issues kept (at most WL_TEV_MAX_ISSUES)
    uint32_t errors;   // all errors found
    uint32_t warnings; // all warnings found
} WlTevReport;

// True when the program has no errors. `report` (may be NULL) lists errors and warnings. The
// dataflow warnings are only worked out for a program without errors. A half that only passes PREV
// through (a b c ZERO, d PREV, ADD, bias ZERO, scale 1, dest PREV; the clamp does not matter), as
// wl_tev_add_stage leaves it, neither reads nor writes for the warnings: a stage that sets one half
// does not make the other read an unwritten PREV or count as the program's result.
bool wl_tev_validate(const WlTevProgram *p, WlTevReport *report);
const char *wl_tev_code_name(WlTevCode code); // static, never NULL

// ---- what a program uses ------------------------------------------------------------------------

uint32_t wl_tev_channels_used(const WlTevProgram *p);  // colour channels GX_SetNumChans must enable: 0..2
uint32_t wl_tev_texcoords_used(const WlTevProgram *p); // highest texcoord + 1, 0 for none
// The same as sets, by the stages' orders. Channels: bit 0 COLOR0A0, bit 1 COLOR1A1 (COLORZERO adds
// no bit, though wl_tev_channels_used counts it as needing one channel). Texcoords: bit n is texcoord n.
uint32_t wl_tev_channels_mask(const WlTevProgram *p);
uint32_t wl_tev_texcoords_mask(const WlTevProgram *p);
uint32_t wl_tev_texmaps_used(const WlTevProgram *p);   // mask of texture maps read
bool wl_tev_equal(const WlTevProgram *a, const WlTevProgram *b); // the same stages, tables, konsts, preloads
uint64_t wl_tev_hash(const WlTevProgram *p);                      // equal programs hash equal (not padding)

// ---- software evaluation ------------------------------------------------------------------------

// What the pixel pipeline hands the TEV: the texel of each texture map and the rasteriser's two
// colours, as r, g, b, a bytes.
typedef struct {
    uint8_t tex[WL_TEV_TEXMAPS][4];
    uint8_t ras[WL_TEV_CHANNELS][4];
} WlTevPixel;

typedef struct {
    int16_t r, g, b, a; // register contents: -1024..1023 (11 bits signed); a, b, c read the low 8 bits, d all
} WlTevRegValue;

// Runs the stages on one pixel (a, b and c read a register's low 8 bits, d its signed value). regs[WL_TEV_PREV] is the result before the pipeline's clamp.
// Registers start at the preloads (0 otherwise). Rounds the blend at 8 fractional bits as the
// hardware does (the lerp factor c is taken as c + (c >> 7), 0..256), so a blend with a full
// factor is exact; it is not guaranteed bit-exact with hardware in the last unit for other
// factors. The program should have passed wl_tev_validate; an invalid one evaluates to 0s.
void wl_tev_eval(const WlTevProgram *p, const WlTevPixel *px, WlTevRegValue regs[WL_TEV_REG_COUNT]);
// The stages on one pixel as GX's TEV runs them, each stage reading its own texel: stage_tex[i]
// is what stage i's order samples (its texture map at its texture coordinate; GX gives every stage
// its own lookup, so one map read at two coordinates is two texels), read when the stage orders a
// map; ras the two colour channels (r, g, b, a bytes). No validation, for a renderer that checked
// the program once: every code is taken within its field's range, a stage reading a texel or a
// colour it does not order reads 0, a konst selection with no meaning reads 0. NULL stage_tex or
// ras: all 0.
void wl_tev_eval_stages(const WlTevProgram *p, const uint8_t (*stage_tex)[4], const uint8_t (*ras)[4],
                        WlTevRegValue regs[WL_TEV_REG_COUNT]);
// PREV clamped to 0..255 per channel, packed 0xRRGGBBAA.
uint32_t wl_tev_eval_rgba(const WlTevProgram *p, const WlTevPixel *px);

// ---- the fixed recipes --------------------------------------------------------------------------

// The program of a WlGxRecipe (see platform.h). Konst slot 0 is external (white as far as the
// evaluator knows): wl_gx_set_konst sets it.
void wl_tev_recipe(WlTevProgram *p, WlGxRecipe recipe);

// ---- applying -----------------------------------------------------------------------------------

// Writes the program to GX: the swap tables the stages use, the konsts and preloads it owns,
// GX_SetNumTevStages and every stage. Does not touch texture coordinate generators, the number
// of colour channels or textures (see wl_tev_channels_used / wl_tev_texcoords_used), and
// bypasses the wl_gx shadow: call wl_gx_invalidate_shadow afterwards unless the caller is
// wl_gx itself. wl_tev_apply validates first and writes nothing for a program with errors
// (WL_ERR_ARG); the _unchecked form is for programs already validated. The host backend records
// the program (host/tev_host.h) and writes nothing.
// The _ex forms take flags: WL_TEV_APPLY_SKIP_SWAPS leaves the swap tables alone (the caller set
// them once with GX_SetTevSwapModeTable; the program's own p->swap is not written).
#define WL_TEV_APPLY_SKIP_SWAPS 0x01u
// WL_TEV_APPLY_CHANGES sends only what differs from the TEV as the last apply left it (gx_shadow.h:
// forgotten by wl_gx_invalidate_shadow, which whatever sets the TEV behind it calls): a stage's
// fields by call, the konsts, preloads, swap tables and stage count by value.
#define WL_TEV_APPLY_CHANGES 0x02u
WlResult wl_tev_apply(const WlTevProgram *p);
WlResult wl_tev_apply_ex(const WlTevProgram *p, uint32_t flags);
void wl_tev_apply_unchecked(const WlTevProgram *p);
void wl_tev_apply_unchecked_ex(const WlTevProgram *p, uint32_t flags); // the backends' one entry point
