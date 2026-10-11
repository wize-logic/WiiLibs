// SPDX-License-Identifier: MIT
// TEV programs (wl/platform/tev.c): the builder and allocation helpers, the validator, the software
// evaluator checked numerically (blend, bias, scale, clamp, compares, swap tables, konst
// selections, register timing) and the fixed recipes through it. Integer only: runs the same on
// the big-endian ppc-be build.
#include "wl/platform/tev.h"

#include <string.h>

#include "greatest.h"
#include "wl/platform/host/tev_host.h"

// ---- helpers ------------------------------------------------------------------------------------

// A one-stage program with the given inputs, ADD, no bias, scale 1, clamped, into PREV. The stage
// reads texture map 0 and colour channel 0.
static WlTevStage *one_stage(WlTevProgram *p) {
    wl_tev_program_init(p);
    WlTevStage *s = wl_tev_add_stage(p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
    return s;
}

static void set_color(WlTevStage *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    wl_tev_stage_color(s, a, b, c, d, WL_TEV_ADD, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
}

static void set_alpha(WlTevStage *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    wl_tev_stage_alpha(s, a, b, c, d, WL_TEV_ADD, WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
}

static void pixel(WlTevPixel *px, const uint8_t tex[4], const uint8_t ras[4]) {
    memset(px, 0, sizeof *px);
    memcpy(px->tex[0], tex, 4);
    memcpy(px->ras[0], ras, 4);
}

static uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8 | a;
}

static WlTevRegValue eval_prev(const WlTevProgram *p, const WlTevPixel *px) {
    WlTevRegValue regs[WL_TEV_REG_COUNT];
    wl_tev_eval(p, px, regs);
    return regs[WL_TEV_PREV];
}

static bool has_code(const WlTevReport *r, WlTevCode code) {
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->issues[i].code == code) {
            return true;
        }
    }
    return false;
}

// ---- builder ------------------------------------------------------------------------------------

TEST program_init_and_stage_limit(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    ASSERT_EQ(0, p.stage_count);
    ASSERT_EQ(WL_TEV_SWAP_R, p.swap[0][0]);
    ASSERT_EQ(WL_TEV_SWAP_B, p.swap[0][2]);
    ASSERT_EQ(WL_TEV_SWAP_R, p.swap[1][2]);
    ASSERT_EQ(WL_TEV_SWAP_G, p.swap[2][0]);
    ASSERT_EQ(WL_TEV_SWAP_B, p.swap[3][1]);
    ASSERT_EQ(WL_TEV_SWAP_A, p.swap[3][3]);
    for (uint32_t i = 0; i < WL_TEV_MAX_STAGES; i++) {
        WlTevStage *s = wl_tev_add_stage(&p);
        ASSERT(s != NULL);
        ASSERT_EQ(WL_TEV_NONE, s->texmap);
        ASSERT_EQ(WL_TEV_CC_CPREV, s->color.in[3]);
        ASSERT_EQ(WL_TEV_CA_APREV, s->alpha.in[3]);
        ASSERT_EQ(WL_TEV_PREV, s->color.dest);
    }
    ASSERT(wl_tev_add_stage(&p) == NULL);
    ASSERT_EQ(16, p.stage_count);
    PASS();
}

TEST konst_selection_helpers(void) {
    ASSERT_EQ(0x0C, wl_tev_kcsel(0, WL_TEV_K_RGB));
    ASSERT_EQ(0x0F, wl_tev_kcsel(3, WL_TEV_K_RGB));
    ASSERT_EQ(0x11, wl_tev_kcsel(1, WL_TEV_K_R));
    ASSERT_EQ(0x16, wl_tev_kcsel(2, WL_TEV_K_G));
    ASSERT_EQ(0x1B, wl_tev_kcsel(3, WL_TEV_K_B));
    ASSERT_EQ(0x1C, wl_tev_kcsel(0, WL_TEV_K_A));
    ASSERT_EQ(0x1E, wl_tev_kasel(2, WL_TEV_K_A));
    ASSERT_EQ(0x10, wl_tev_kasel(0, WL_TEV_K_R));
    PASS();
}

TEST konst_slots(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    ASSERT_EQ(0, wl_tev_konst_slot(&p, 0x11223344u));
    ASSERT_EQ(1, wl_tev_konst_slot(&p, 0x55667788u));
    ASSERT_EQ(0, wl_tev_konst_slot(&p, 0x11223344u)); // the same colour, the same slot
    wl_tev_konst_external(&p, 2, 0xFF00FF00u);
    ASSERT_EQ(3, wl_tev_konst_slot(&p, 0x99AABBCCu)); // 2 is spoken for
    ASSERT_EQ(-1, wl_tev_konst_slot(&p, 0x01020304u));
    ASSERT_EQ(3, wl_tev_konst_slot(&p, 0x99AABBCCu));
    ASSERT_EQ(0x0B, p.konst_mask);
    ASSERT_EQ(0x04, p.konst_external);
    PASS();
}

TEST register_pool(void) {
    WlTevRegPool pool;
    wl_tev_pool_init(&pool);
    ASSERT_EQ(WL_TEV_REG0, wl_tev_pool_alloc(&pool, 0));
    ASSERT_EQ(WL_TEV_REG1, wl_tev_pool_alloc(&pool, 0));
    ASSERT_EQ(WL_TEV_REG2, wl_tev_pool_alloc(&pool, 0));
    ASSERT_EQ(WL_TEV_PREV, wl_tev_pool_alloc(&pool, 0)); // PREV last
    ASSERT_EQ(-1, wl_tev_pool_alloc(&pool, 0));
    wl_tev_pool_release(&pool, WL_TEV_REG1);
    ASSERT(wl_tev_pool_free(&pool, WL_TEV_REG1));
    ASSERT_EQ(-1, wl_tev_pool_alloc(&pool, 1u << WL_TEV_REG1)); // avoided
    ASSERT_EQ(WL_TEV_REG1, wl_tev_pool_alloc(&pool, 0));
    wl_tev_pool_retain(&pool, WL_TEV_REG1); // two holders
    wl_tev_pool_release(&pool, WL_TEV_REG1);
    ASSERT(!wl_tev_pool_free(&pool, WL_TEV_REG1));
    wl_tev_pool_release(&pool, WL_TEV_REG1);
    ASSERT(wl_tev_pool_free(&pool, WL_TEV_REG1));
    wl_tev_pool_release(&pool, WL_TEV_REG1); // below zero: stays free
    ASSERT(wl_tev_pool_free(&pool, WL_TEV_REG1));
    PASS();
}

TEST needs_merge_and_conflicts(void) {
    WlTevNeeds a, b;
    wl_tev_needs_clear(&a);
    wl_tev_needs_clear(&b);
    ASSERT(wl_tev_needs_texture(&a, 0, 0, 0x0F));
    ASSERT(wl_tev_needs_texture(&a, 0, 0, 0x09)); // tables 0 and 3
    ASSERT_EQ(0x09, a.tex_swaps);
    ASSERT(!wl_tev_needs_texture(&a, 1, 1, 0x0F)); // another texture
    ASSERT(!wl_tev_needs_texture(&a, 0, 0, 0x06)); // no table in common
    ASSERT_EQ(0x09, a.tex_swaps);                   // (unchanged)
    ASSERT(wl_tev_needs_channel(&a, WL_TEV_CH_COLOR0A0, 0x01));
    ASSERT(!wl_tev_needs_channel(&a, WL_TEV_CH_COLOR1A1, 0x0F));

    ASSERT(wl_tev_needs_texture(&b, 0, 0, 0x08));
    ASSERT(wl_tev_needs_channel(&b, WL_TEV_CH_COLOR0A0, 0x0F));
    ASSERT(wl_tev_needs_merge(&a, &b));
    ASSERT_EQ(0x08, a.tex_swaps);
    ASSERT_EQ(0x01, a.ras_swaps);

    WlTevNeeds c;
    wl_tev_needs_clear(&c);
    ASSERT(wl_tev_needs_texture(&c, 2, 2, 0x0F));
    WlTevNeeds before = a;
    ASSERT(!wl_tev_needs_merge(&a, &c));
    ASSERT(memcmp(&a, &before, sizeof a) == 0);

    WlTevNeeds d; // a read through table 1 only, with a's texture on 0 or 3
    wl_tev_needs_clear(&d);
    ASSERT(wl_tev_needs_texture(&d, 0, 0, 0x02));
    ASSERT(!wl_tev_needs_merge(&a, &d));

    // konst selections: one per half, equal ones agree, a different one clashes
    WlTevNeeds k, k2;
    wl_tev_needs_clear(&k);
    ASSERT_EQ(WL_TEV_KSEL_NONE, k.kcsel);
    ASSERT_EQ(WL_TEV_KSEL_NONE, k.kasel);
    ASSERT(wl_tev_needs_konst_color(&k, wl_tev_kcsel(1, WL_TEV_K_RGB)));
    ASSERT(wl_tev_needs_konst_color(&k, wl_tev_kcsel(1, WL_TEV_K_RGB)));
    ASSERT(!wl_tev_needs_konst_color(&k, wl_tev_kcsel(1, WL_TEV_K_A)));
    ASSERT(!wl_tev_needs_konst_color(&k, WL_TEV_KSEL_1));
    ASSERT_EQ(wl_tev_kcsel(1, WL_TEV_K_RGB), k.kcsel);
    ASSERT(wl_tev_needs_konst_alpha(&k, WL_TEV_KSEL_1)); // 0 is a selection too
    ASSERT(!wl_tev_needs_konst_alpha(&k, WL_TEV_KSEL_1_2));
    ASSERT_EQ(WL_TEV_KSEL_1, k.kasel);
    wl_tev_needs_clear(&k2); // nothing asked for: merges with anything
    ASSERT(wl_tev_needs_merge(&k, &k2));
    ASSERT_EQ(wl_tev_kcsel(1, WL_TEV_K_RGB), k.kcsel);
    ASSERT(wl_tev_needs_konst_color(&k2, wl_tev_kcsel(1, WL_TEV_K_RGB)));
    ASSERT(wl_tev_needs_merge(&k, &k2)); // the same selection
    ASSERT(wl_tev_needs_merge(&k2, &k)); // into an empty one
    wl_tev_needs_clear(&k2);
    ASSERT(wl_tev_needs_konst_color(&k2, wl_tev_kcsel(2, WL_TEV_K_RGB)));
    WlTevNeeds k_before = k;
    ASSERT(!wl_tev_needs_merge(&k, &k2)); // the colour halves want K1 and K2
    ASSERT(memcmp(&k, &k_before, sizeof k) == 0);
    wl_tev_needs_clear(&k2);
    ASSERT(wl_tev_needs_konst_alpha(&k2, wl_tev_kasel(0, WL_TEV_K_A)));
    ASSERT(!wl_tev_needs_merge(&k, &k2)); // the alpha halves want 1 and K0.a
    ASSERT(memcmp(&k, &k_before, sizeof k) == 0);

    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    ASSERT_EQ(WL_TEV_KSEL_1, s->kcsel);
    wl_tev_stage_set_needs(s, &k);
    ASSERT_EQ(wl_tev_kcsel(1, WL_TEV_K_RGB), s->kcsel);
    ASSERT_EQ(WL_TEV_KSEL_1, s->kasel);
    s->kasel = WL_TEV_KSEL_1_4;
    wl_tev_needs_clear(&k2); // an unnamed selection leaves the stage's
    wl_tev_stage_set_needs(s, &k2);
    ASSERT_EQ(wl_tev_kcsel(1, WL_TEV_K_RGB), s->kcsel);
    ASSERT_EQ(WL_TEV_KSEL_1_4, s->kasel);
    wl_tev_program_init(&p);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_set_needs(s, &a);
    ASSERT_EQ(0, s->texcoord);
    ASSERT_EQ(0, s->texmap);
    ASSERT_EQ(WL_TEV_CH_COLOR0A0, s->channel);
    ASSERT_EQ(3, s->tex_swap);
    ASSERT_EQ(0, s->ras_swap);
    PASS();
}

TEST program_uses(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    ASSERT_EQ(0u, wl_tev_channels_used(&p));
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 3, 5, WL_TEV_CH_ZERO);
    ASSERT_EQ(1u, wl_tev_channels_used(&p));
    ASSERT_EQ(4u, wl_tev_texcoords_used(&p));
    ASSERT_EQ(1u << 5, wl_tev_texmaps_used(&p));
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 1, 2, WL_TEV_CH_COLOR1A1);
    ASSERT_EQ(2u, wl_tev_channels_used(&p));
    ASSERT_EQ(4u, wl_tev_texcoords_used(&p));
    ASSERT_EQ((1u << 5) | (1u << 2), wl_tev_texmaps_used(&p));
    PASS();
}

TEST program_use_masks(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    ASSERT_EQ(0u, wl_tev_channels_mask(&p));
    ASSERT_EQ(0u, wl_tev_texcoords_mask(&p));
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 4, 4, WL_TEV_NONE); // a fog's texcoord alone
    ASSERT_EQ(1u << 4, wl_tev_texcoords_mask(&p));
    ASSERT_EQ(5u, wl_tev_texcoords_used(&p)); // (the count is the highest + 1)
    ASSERT_EQ(0u, wl_tev_channels_mask(&p));
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR1A1);
    ASSERT_EQ(2u, wl_tev_channels_mask(&p)); // colour 1 alone
    ASSERT_EQ(2u, wl_tev_channels_used(&p));
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 1, WL_TEV_CH_ZERO);
    ASSERT_EQ(2u, wl_tev_channels_mask(&p)); // COLORZERO adds no bit ...
    ASSERT_EQ((1u << 4) | 1u, wl_tev_texcoords_mask(&p));
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 7, 0, WL_TEV_CH_COLOR0A0);
    ASSERT_EQ(3u, wl_tev_channels_mask(&p));
    ASSERT_EQ((1u << 7) | (1u << 4) | 1u, wl_tev_texcoords_mask(&p));
    // ... and on its own it is no channel in the mask, one in the count
    wl_tev_program_init(&p);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_ZERO);
    ASSERT_EQ(0u, wl_tev_channels_mask(&p));
    ASSERT_EQ(1u, wl_tev_channels_used(&p));
    // the recipes
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_MODULATE);
    ASSERT_EQ(1u, wl_tev_channels_mask(&p));
    ASSERT_EQ(1u, wl_tev_texcoords_mask(&p));
    wl_tev_recipe(&p, WL_GX_RECIPE_KONST);
    ASSERT_EQ(0u, wl_tev_channels_mask(&p));
    ASSERT_EQ(0u, wl_tev_texcoords_mask(&p));
    PASS();
}

TEST equal_and_hash(void) {
    WlTevProgram a, b;
    wl_tev_recipe(&a, WL_GX_RECIPE_TEX_TINT);
    wl_tev_recipe(&b, WL_GX_RECIPE_TEX_TINT);
    ASSERT(wl_tev_equal(&a, &b));
    ASSERT(wl_tev_hash(&a) == wl_tev_hash(&b));
    b.stages[1].color.scale = WL_TEV_SCALE_2;
    ASSERT(!wl_tev_equal(&a, &b));
    ASSERT(wl_tev_hash(&a) != wl_tev_hash(&b));
    b = a;
    b.konst[3] = 0x12345678u; // a slot the program does not own
    b.stages[5].color.dest = 2;      // a stage beyond the count
    ASSERT(wl_tev_equal(&a, &b));
    ASSERT(wl_tev_hash(&a) == wl_tev_hash(&b));
    wl_tev_konst_slot(&b, 0x12345678u);
    ASSERT(!wl_tev_equal(&a, &b));
    ASSERT(wl_tev_hash(&a) != wl_tev_hash(&b));
    wl_tev_recipe(&b, WL_GX_RECIPE_UI);
    ASSERT(wl_tev_hash(&a) != wl_tev_hash(&b));
    PASS();
}

// ---- validation ---------------------------------------------------------------------------------

TEST valid_program_has_no_issues(void) {
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_ZERO);
    WlTevReport r;
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.errors);
    ASSERT_EQ(0u, r.warnings);
    ASSERT(wl_tev_validate(&p, NULL));
    PASS();
}

TEST stage_count_errors(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevReport r;
    ASSERT(!wl_tev_validate(&p, &r));
    ASSERT(has_code(&r, WL_TEV_E_NO_STAGES));
    ASSERT(!wl_tev_validate(&p, NULL));
    one_stage(&p);
    p.stage_count = 17;
    ASSERT(!wl_tev_validate(&p, &r));
    ASSERT(has_code(&r, WL_TEV_E_NO_STAGES));
    PASS();
}

#define EXPECT_ERROR(setup, code)                \
    do {                                         \
        WlTevProgram p_;                         \
        WlTevStage *s = one_stage(&p_);          \
        set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC); \
        set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_RASA); \
        setup;                                   \
        WlTevReport r_;                          \
        ASSERT(!wl_tev_validate(&p_, &r_));      \
        ASSERT_EQm(#code, 1, has_code(&r_, code)); \
        ASSERT(!wl_tev_validate(&p_, NULL));     \
        WlTevPixel px_ = {0};                    \
        ASSERT_EQ(0u, wl_tev_eval_rgba(&p_, &px_)); \
    } while (0)

TEST each_error_is_found(void) {
    EXPECT_ERROR(s->color.in[2] = 16, WL_TEV_E_INPUT);
    EXPECT_ERROR(s->alpha.in[0] = 8, WL_TEV_E_INPUT);
    EXPECT_ERROR(s->color.op = 2, WL_TEV_E_OP);
    EXPECT_ERROR(s->color.op = 16, WL_TEV_E_OP);
    EXPECT_ERROR(s->alpha.op = WL_TEV_COMP_R8_GT, WL_TEV_E_OP); // alpha has only the A8 compares
    EXPECT_ERROR(s->color.bias = 3, WL_TEV_E_BIAS_SCALE);
    EXPECT_ERROR(s->color.scale = 4, WL_TEV_E_BIAS_SCALE);
    EXPECT_ERROR(s->color.clamp = 2, WL_TEV_E_BIAS_SCALE);
    EXPECT_ERROR((s->color.op = WL_TEV_COMP_R8_GT, s->color.scale = WL_TEV_DIVIDE_2), WL_TEV_E_BIAS_SCALE);
    EXPECT_ERROR((s->alpha.op = WL_TEV_COMP_A8_GT, s->alpha.scale = WL_TEV_SCALE_2), WL_TEV_E_BIAS_SCALE);
    EXPECT_ERROR((s->color.op = WL_TEV_COMP_RGB8_EQ, s->color.bias = WL_TEV_BIAS_SUBHALF), WL_TEV_E_BIAS_SCALE);
    EXPECT_ERROR(s->color.dest = 4, WL_TEV_E_DEST);
    EXPECT_ERROR(s->alpha.dest = 9, WL_TEV_E_DEST);
    EXPECT_ERROR(s->texcoord = 8, WL_TEV_E_ORDER);
    EXPECT_ERROR(s->texmap = 8, WL_TEV_E_ORDER);
    EXPECT_ERROR((s->texmap = WL_TEV_NONE), WL_TEV_E_ORDER); // a coordinate without a map
    EXPECT_ERROR(s->channel = 3, WL_TEV_E_ORDER);
    EXPECT_ERROR(s->channel = 7, WL_TEV_E_CHANNEL);
    EXPECT_ERROR(s->channel = 8, WL_TEV_E_CHANNEL);
    EXPECT_ERROR(s->ras_swap = 4, WL_TEV_E_SWAP);
    EXPECT_ERROR(s->tex_swap = 7, WL_TEV_E_SWAP);
    EXPECT_ERROR(p_.swap[2][1] = 4, WL_TEV_E_SWAP);
    EXPECT_ERROR(s->kcsel = 8, WL_TEV_E_KSEL);
    EXPECT_ERROR(s->kcsel = 0x20, WL_TEV_E_KSEL);
    EXPECT_ERROR(s->kasel = 0x0C, WL_TEV_E_KSEL); // the alpha half has no K RGB
    EXPECT_ERROR(s->kasel = 9, WL_TEV_E_KSEL);
    EXPECT_ERROR((s->color.in[0] = WL_TEV_CC_TEXC, s->texcoord = WL_TEV_NONE, s->texmap = WL_TEV_NONE),
                 WL_TEV_E_TEXTURE_UNBOUND);
    EXPECT_ERROR((s->alpha.in[1] = WL_TEV_CA_TEXA, s->texcoord = WL_TEV_NONE, s->texmap = WL_TEV_NONE),
                 WL_TEV_E_TEXTURE_UNBOUND);
    EXPECT_ERROR(s->channel = WL_TEV_NONE, WL_TEV_E_RAS_UNBOUND);
    EXPECT_ERROR(wl_tev_preload(&p_, WL_TEV_PREV, 0), WL_TEV_E_PRELOAD);
    PASS();
}

TEST a_ras_read_with_the_zero_channel_is_legal(void) {
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    s->channel = WL_TEV_CH_ZERO;
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_RASA);
    ASSERT(wl_tev_validate(&p, NULL));
    WlTevPixel px = {0};
    memset(px.ras, 0x77, sizeof px.ras);
    ASSERT_EQ(0u, wl_tev_eval_rgba(&p, &px));
    PASS();
}

TEST reports_are_capped_but_counted(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    for (uint32_t i = 0; i < WL_TEV_MAX_STAGES; i++) {
        WlTevStage *s = wl_tev_add_stage(&p);
        s->color.in[0] = s->color.in[1] = 99;
    }
    WlTevReport r;
    ASSERT(!wl_tev_validate(&p, &r));
    ASSERT_EQ(32u, r.errors);
    ASSERT_EQ(WL_TEV_MAX_ISSUES, r.count);
    PASS();
}

TEST dead_write_warning(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ONE, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_REG0); // written, never read
    s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_HALF, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV); // PREV written ...
    s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ONE, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV); // ... and overwritten unread
    WlTevReport r;
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.errors);
    uint32_t dead = 0;
    for (uint32_t i = 0; i < r.count; i++) {
        if (r.issues[i].code == WL_TEV_W_DEAD_WRITE) {
            dead++;
            ASSERT_EQ(0, r.issues[i].alpha);
        }
    }
    ASSERT_EQ(2u, dead); // REG0 from stage 0, PREV from stage 1
    // (the colour is written to PREV; the alpha half only ever passes PREV through, so it has no result)
    uint32_t none = 0;
    for (uint32_t i = 0; i < r.count; i++) {
        if (r.issues[i].code == WL_TEV_W_NO_RESULT) {
            none++;
            ASSERT_EQ(1, r.issues[i].alpha);
        }
    }
    ASSERT_EQ(1u, none);
    PASS();
}

TEST no_result_warning(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ONE, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_REG0);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_REG0);
    WlTevReport r;
    ASSERT(wl_tev_validate(&p, &r));
    uint32_t none = 0;
    for (uint32_t i = 0; i < r.count; i++) {
        none += r.issues[i].code == WL_TEV_W_NO_RESULT;
    }
    ASSERT_EQ(2u, none); // one for each half
    PASS();
}

TEST read_before_write_and_konst_warnings(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    s->kcsel = wl_tev_kcsel(2, WL_TEV_K_RGB);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_C1, WL_TEV_CC_KONST, WL_TEV_CC_ZERO, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_A0, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    WlTevReport r;
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT(has_code(&r, WL_TEV_W_READ_UNWRITTEN));
    ASSERT(has_code(&r, WL_TEV_W_KONST_UNSET));
    ASSERT_EQ(3u, r.warnings); // C1, A0, K2
    // a preload and a konst slot silence them
    wl_tev_preload(&p, WL_TEV_REG1, 0);
    wl_tev_preload(&p, WL_TEV_REG0, 0);
    wl_tev_konst_external(&p, 2, 0);
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.warnings);
    // a K fraction needs no register
    p.stages[0].kcsel = WL_TEV_KSEL_1_2;
    p.konst_external = 0;
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.warnings);
    PASS();
}

TEST pass_through_halves_raise_no_warnings(void) {
    WlTevReport r;
    WlTevProgram p;
    // a colour-only program: the alpha halves wl_tev_add_stage leaves read and write nothing
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_NONE);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC);
    s = wl_tev_add_stage(&p); // (a stage with both halves passing)
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.errors);
    ASSERT_EQ(1u, r.warnings); // only that no stage wrote PREV's alpha
    ASSERT(has_code(&r, WL_TEV_W_NO_RESULT));
    ASSERT_EQ(1, r.issues[0].alpha);
    // the same with both halves written once: clean, however many pass-through stages surround it
    wl_tev_program_init(&p);
    wl_tev_add_stage(&p);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_NONE);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA);
    wl_tev_add_stage(&p);
    wl_tev_add_stage(&p);
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.warnings);
    // a program of pass-through stages alone writes nothing: both halves are reported
    wl_tev_program_init(&p);
    wl_tev_add_stage(&p);
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(2u, r.warnings);
    // a pass-through with the clamp off, or one a real stage does not follow, is still one
    wl_tev_program_init(&p);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_NONE);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_ADD, 0, 0, false,
                       WL_TEV_PREV);
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT_EQ(0u, r.warnings);
    // but a half that does something else with PREV is checked as before: PREV's alpha unwritten
    wl_tev_program_init(&p);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_alpha(s, WL_TEV_CA_APREV, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV); // a, not d: reads PREV
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ONE);
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT(has_code(&r, WL_TEV_W_READ_UNWRITTEN));
    wl_tev_program_init(&p);
    s = wl_tev_add_stage(&p);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ONE);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_ADD, 0, 1, true,
                       WL_TEV_PREV); // scale 2: not a pass-through
    ASSERT(wl_tev_validate(&p, &r));
    ASSERT(has_code(&r, WL_TEV_W_READ_UNWRITTEN));
    // the fixed recipes (some with a pass-through half) stay clean
    for (int rc = 0; rc < WL_GX_RECIPE_COUNT; rc++) {
        wl_tev_recipe(&p, (WlGxRecipe)rc);
        ASSERT(wl_tev_validate(&p, &r));
        ASSERT_EQ(0u, r.warnings);
    }
    PASS();
}

TEST names_exist(void) {
    for (int c = WL_TEV_OK; c <= WL_TEV_W_NO_RESULT; c++) {
        const char *n = wl_tev_code_name((WlTevCode)c);
        ASSERT(n != NULL && n[0] != '\0' && strcmp(n, "unknown") != 0);
    }
    ASSERT(strcmp(wl_tev_code_name((WlTevCode)99), "unknown") == 0);
    PASS();
}

// ---- evaluation ---------------------------------------------------------------------------------

TEST blend_modulate_and_replace(void) {
    const uint8_t tex[4] = {200, 100, 50, 255};
    const uint8_t ras[4] = {255, 0, 128, 255};
    WlTevPixel px;
    pixel(&px, tex, ras);
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO); // tex * ras
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_ZERO);
    WlTevRegValue v = eval_prev(&p, &px);
    ASSERT_EQ(200, v.r);       // ras 255 is a full factor: exact
    ASSERT_EQ(0, v.g);         // ras 0
    ASSERT_EQ((50 * 129 + 128) >> 8, v.b); // ras 128 is the factor 129/256: 25
    ASSERT_EQ(255, v.a);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC); // replace
    v = eval_prev(&p, &px);
    ASSERT_EQ(200, v.r);
    ASSERT_EQ(100, v.g);
    ASSERT_EQ(50, v.b);
    PASS();
}

TEST blend_lerp_and_sum(void) {
    const uint8_t tex[4] = {200, 0, 0, 0};
    const uint8_t ras[4] = {40, 0, 0, 0};
    WlTevPixel px;
    pixel(&px, tex, ras);
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    // lerp: (1 - c) * a + c * b with c = ONE picks b, c = ZERO picks a
    set_color(s, WL_TEV_CC_RASC, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_ZERO);
    ASSERT_EQ(200, eval_prev(&p, &px).r);
    set_color(s, WL_TEV_CC_RASC, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO);
    ASSERT_EQ(40, eval_prev(&p, &px).r);
    // HALF is the factor 129/256 of the two
    set_color(s, WL_TEV_CC_RASC, WL_TEV_CC_TEXC, WL_TEV_CC_HALF, WL_TEV_CC_ZERO);
    ASSERT_EQ((40 * (256 - 129) + 200 * 129 + 128) >> 8, eval_prev(&p, &px).r);
    // a + d (c = ZERO): 40 + 200
    set_color(s, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC);
    ASSERT_EQ(240, eval_prev(&p, &px).r);
    // clamped at 255
    set_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC);
    ASSERT_EQ(255, eval_prev(&p, &px).r);
    // unclamped: 400
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_ADD, 0, 0, false,
                       WL_TEV_PREV);
    ASSERT_EQ(400, eval_prev(&p, &px).r);
    // d - a: 200 - 40
    wl_tev_stage_color(s, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_SUB, 0, 0, true,
                       WL_TEV_PREV);
    ASSERT_EQ(160, eval_prev(&p, &px).r);
    // 40 - 200 clamps to 0, or stays -160 unclamped
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC, WL_TEV_SUB, 0, 0, true,
                       WL_TEV_PREV);
    ASSERT_EQ(0, eval_prev(&p, &px).r);
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC, WL_TEV_SUB, 0, 0, false,
                       WL_TEV_PREV);
    ASSERT_EQ(-160, eval_prev(&p, &px).r);
    PASS();
}

TEST bias_and_scale(void) {
    const uint8_t tex[4] = {100, 50, 0, 0};
    WlTevPixel px;
    const uint8_t ras[4] = {0, 0, 0, 0};
    pixel(&px, tex, ras);
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    struct {
        uint8_t bias, scale;
        bool clamp;
        int expect_r, expect_g;
    } c[] = {
        {WL_TEV_BIAS_ZERO, WL_TEV_SCALE_2, true, 200, 100},
        {WL_TEV_BIAS_ZERO, WL_TEV_SCALE_4, true, 255, 200},
        {WL_TEV_BIAS_ZERO, WL_TEV_SCALE_4, false, 400, 200},
        {WL_TEV_BIAS_ZERO, WL_TEV_DIVIDE_2, true, 50, 25},
        {WL_TEV_BIAS_ADDHALF, WL_TEV_SCALE_1, true, 228, 178},
        {WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_1, true, 0, 0},
        {WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_1, false, -28, -78},
        {WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_2, false, -56, -156}, // the bias is scaled too
        {WL_TEV_BIAS_ADDHALF, WL_TEV_SCALE_2, false, 456, 356},
        {WL_TEV_BIAS_ADDHALF, WL_TEV_SCALE_4, false, 912, 712},
        {WL_TEV_BIAS_ZERO, WL_TEV_SCALE_4, false, 400, 200},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_ADD, c[i].bias,
                           c[i].scale, c[i].clamp, WL_TEV_PREV);
        WlTevRegValue v = eval_prev(&p, &px);
        ASSERT_EQ_FMT(c[i].expect_r, (int)v.r, "%d");
        ASSERT_EQ_FMT(c[i].expect_g, (int)v.g, "%d");
    }
    // the unclamped range ends at 1023 and -1024
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_ADD,
                       WL_TEV_BIAS_ADDHALF, WL_TEV_SCALE_4, false, WL_TEV_PREV);
    ASSERT_EQ(1023, eval_prev(&p, &px).r); // (100 + 100 + 128) * 4 = 1312
    const uint8_t big[4] = {200, 0, 0, 0};
    pixel(&px, big, ras);
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_SUB,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_4, false, WL_TEV_PREV);
    ASSERT_EQ(-1024, eval_prev(&p, &px).r); // (0 - 200 - 128) * 4 = -1312
    PASS();
}

TEST registers_and_timing(void) {
    const uint8_t tex[4] = {10, 20, 30, 40};
    const uint8_t ras[4] = {1, 2, 3, 4};
    WlTevPixel px;
    pixel(&px, tex, ras);
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
    // stage 0: REG0.rgb = tex, REG1.a = ras.a, PREV = ras (the colour half and the alpha half write
    // different registers)
    wl_tev_stage_color(s, 15, 15, 15, WL_TEV_CC_TEXC, WL_TEV_ADD, 0, 0, true, WL_TEV_REG0);
    wl_tev_stage_alpha(s, 7, 7, 7, WL_TEV_CA_RASA, WL_TEV_ADD, 0, 0, true, WL_TEV_REG1);
    // stage 1: a stage reads before it writes: PREV = C0 + A1-as-colour while REG0 takes RASC
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR0A0);
    wl_tev_stage_color(s, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_A1, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_stage_alpha(s, 7, 7, 7, WL_TEV_CA_A1, WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
    // stage 2: REG0 (tex) gets overwritten by the rasterised colour in the same stage that reads it
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR0A0);
    wl_tev_stage_color(s, WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_REG0);
    wl_tev_stage_alpha(s, 7, 7, 7, WL_TEV_CA_APREV, WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
    // stage 3: PREV = REG0 (now PREV + C0 of the old value)
    s = wl_tev_add_stage(&p);
    wl_tev_stage_color(s, WL_TEV_CC_C0, 15, 15, 15, WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
    wl_tev_stage_alpha(s, 7, 7, 7, WL_TEV_CA_APREV, WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
    WlTevRegValue regs[WL_TEV_REG_COUNT];
    wl_tev_eval(&p, &px, regs);
    // stage 1 PREV.rgb = C0 (tex) + A1 (ras.a = 4) = (14, 24, 34); alpha = 4
    // stage 2 REG0 = C0 (tex) + CPREV (14, 24, 34) = (24, 44, 64)
    // stage 3 PREV = C0 = (24, 44, 64)
    ASSERT_EQ(24, regs[WL_TEV_PREV].r);
    ASSERT_EQ(44, regs[WL_TEV_PREV].g);
    ASSERT_EQ(64, regs[WL_TEV_PREV].b);
    ASSERT_EQ(4, regs[WL_TEV_PREV].a);
    ASSERT_EQ(24, regs[WL_TEV_REG0].r);
    ASSERT_EQ(4, regs[WL_TEV_REG1].a);
    ASSERT_EQ(0, regs[WL_TEV_REG1].r);
    ASSERT_EQ(0, regs[WL_TEV_REG2].r);
    PASS();
}

TEST inputs_a_b_c_are_eight_bits(void) {
    const uint8_t tex[4] = {50, 0, 0, 0};
    const uint8_t ras[4] = {0, 0, 0, 0};
    WlTevPixel px;
    pixel(&px, tex, ras);
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_NONE);
    // REG0 = 50 - 128 = -78, unclamped
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_1, false, WL_TEV_REG0);
    wl_tev_stage_alpha(s, 7, 7, 7, 7, WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
    // as a, b or c the register is -78 & 255 = 178
    static const struct {
        uint8_t in[4];
        int expect;
    } c[] = {
        {{WL_TEV_CC_C0, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO}, 178},  // a
        {{WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_CC_ONE, WL_TEV_CC_ZERO}, 178},   // b (a full factor)
        {{WL_TEV_CC_ZERO, WL_TEV_CC_ONE, WL_TEV_CC_C0, WL_TEV_CC_ZERO}, (255 * 179 + 128) >> 8}, // c: 178 + (178 >> 7) = 179
    };
    s = wl_tev_add_stage(&p);
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        wl_tev_stage_color(s, c[i].in[0], c[i].in[1], c[i].in[2], c[i].in[3], WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, 7, 7, 7, 7, WL_TEV_ADD, 0, 0, true, WL_TEV_PREV);
        WlTevRegValue regs[WL_TEV_REG_COUNT];
        wl_tev_eval(&p, &px, regs);
        ASSERT_EQ(-78, regs[WL_TEV_REG0].r);
        ASSERT_EQ_FMT(c[i].expect, (int)regs[WL_TEV_PREV].r, "%d");
    }
    PASS();
}

// d is the register's signed value: an unclamped stage feeds it, and what follows can go on from it.
TEST input_d_is_the_full_signed_register(void) {
    const uint8_t tex[4] = {50, 0, 0, 40};
    const uint8_t ras[4] = {200, 0, 0, 0};
    WlTevPixel px;
    pixel(&px, tex, ras);
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
    // REG0 = tex - 128 = -78 (colour), tex.a - 128 = -88 (alpha); both unclamped
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_1, false, WL_TEV_REG0);
    wl_tev_stage_alpha(s, WL_TEV_CA_TEXA, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_1, false, WL_TEV_REG0);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
    WlTevRegValue regs[WL_TEV_REG_COUNT];

    // d alone, unclamped: -78, not 178; the alpha -88
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_ADD, 0, 0, false,
                       WL_TEV_PREV);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A0, WL_TEV_ADD, 0, 0, false,
                       WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-78, regs[WL_TEV_PREV].r);
    ASSERT_EQ(-88, regs[WL_TEV_PREV].a);
    ASSERT_EQ(-78, regs[WL_TEV_REG0].r);
    // ... and clamped by this stage's own clamp: 0
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(0, regs[WL_TEV_PREV].r);
    // d + a: -78 + 200 (the rasterised colour as a with c = ZERO) = 122, which the 8-bit view would make 378 -> 255
    wl_tev_stage_color(s, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(122, regs[WL_TEV_PREV].r);
    // d - a, unclamped: -78 - 200 = -278
    wl_tev_stage_color(s, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_SUB, 0, 0, false,
                       WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-278, regs[WL_TEV_PREV].r);
    // with a scale: (d + a) * 2 = (-78 + 200) * 2 = 244
    wl_tev_stage_color(s, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_ADD, 0, WL_TEV_SCALE_2,
                       false, WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(244, regs[WL_TEV_PREV].r);
    // a blend: d + lerp(a=0, b=200, c=ONE) = -78 + 200
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_RASC, WL_TEV_CC_ONE, WL_TEV_CC_C0, WL_TEV_ADD, 0, 0, false,
                       WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(122, regs[WL_TEV_PREV].r);
    // alpha: d = A0 (-88) + a (TEXA 40) = -48
    wl_tev_stage_alpha(s, WL_TEV_CA_TEXA, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A0, WL_TEV_ADD, 0, 0, false,
                       WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-48, regs[WL_TEV_PREV].a);
    // a compare adds c to the full d: -78 + 200 when 200 > 50 (R8), nothing otherwise
    wl_tev_stage_color(s, WL_TEV_CC_RASC, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_C0, WL_TEV_COMP_R8_GT, 0, 0,
                       false, WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(122, regs[WL_TEV_PREV].r);
    wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_RASC, WL_TEV_CC_C0, WL_TEV_COMP_R8_GT, 0, 0,
                       false, WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-78, regs[WL_TEV_PREV].r);
    // the alpha compare too: -88 + 40 (50 -> TEXA 40 > RASA 0)
    wl_tev_stage_alpha(s, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_TEXA, WL_TEV_CA_A0, WL_TEV_COMP_A8_GT, 0, 0,
                       false, WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-48, regs[WL_TEV_PREV].a);
    // d with a bias and a scale: (-78 - 128) * 4 = -824
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_4, false, WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-824, regs[WL_TEV_PREV].r);
    // (SUB negates the blend, not d)
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C0, WL_TEV_SUB,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_SCALE_4, false, WL_TEV_PREV);
    wl_tev_eval(&p, &px, regs);
    ASSERT_EQ(-824, regs[WL_TEV_PREV].r);
    PASS();
}

// A bias with DIVIDE_2 halves the whole sum: the signed dot product's (lerp - 1/2) / 2.
TEST bias_with_divide_2(void) {
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    WlTevPixel px;
    const uint8_t ras0[4] = {0, 0, 0, 0};
    const uint8_t tex[4] = {200, 50, 0, 255};
    pixel(&px, tex, ras0);
    // the validator takes the combination, in both halves, with a bias of either sign and with any clamp
    for (uint8_t bias = 0; bias < WL_TEV_BIAS_COUNT; bias++) {
        for (int clamp = 0; clamp < 2; clamp++) {
            wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_ADD, bias,
                               WL_TEV_DIVIDE_2, clamp != 0, WL_TEV_PREV);
            wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_ADD, bias,
                               WL_TEV_DIVIDE_2, clamp != 0, WL_TEV_PREV);
            WlTevReport r;
            ASSERT(wl_tev_validate(&p, &r));
            ASSERT_EQ(0u, r.errors);
        }
    }
    // (lerp - 1/2) / 2 with lerp = b (c = ONE, a = ZERO): (200 - 128) / 2 = 36, (50 - 128) / 2 = -39
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_DIVIDE_2, false, WL_TEV_PREV);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO);
    WlTevRegValue v = eval_prev(&p, &px);
    ASSERT_EQ(36, v.r);
    ASSERT_EQ(-39, v.g);
    ASSERT_EQ(-64, v.b); // (0 - 128) / 2: exact
    // clamped: the negatives go to 0
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_DIVIDE_2, true, WL_TEV_PREV);
    v = eval_prev(&p, &px);
    ASSERT_EQ(36, v.r);
    ASSERT_EQ(0, v.g);
    // + 1/2: (200 + 128) / 2 = 164, (50 + 128) / 2 = 89
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_ADDHALF, WL_TEV_DIVIDE_2, true, WL_TEV_PREV);
    v = eval_prev(&p, &px);
    ASSERT_EQ(164, v.r);
    ASSERT_EQ(89, v.g);
    // d joins the halved sum: (d + lerp - 1/2) / 2 = (100 + 200 - 128) / 2 = 86
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_HALF, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_DIVIDE_2, true, WL_TEV_PREV);
    v = eval_prev(&p, &px);
    ASSERT_EQ((128 + 200 - 128) / 2, v.r);
    // SUB: (d - lerp + 1/2) / 2 with d = HALF (128), lerp = 200: (128 - 200 + 128) / 2 = 28
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_ONE, WL_TEV_CC_HALF, WL_TEV_SUB,
                       WL_TEV_BIAS_ADDHALF, WL_TEV_DIVIDE_2, true, WL_TEV_PREV);
    v = eval_prev(&p, &px);
    ASSERT_EQ(28, v.r);
    // the alpha half too: (255 - 128) / 2 = 63.5 -> 64
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_ADD,
                       WL_TEV_BIAS_SUBHALF, WL_TEV_DIVIDE_2, true, WL_TEV_PREV);
    v = eval_prev(&p, &px);
    ASSERT_EQ(64, v.a);
    // the evaluator no longer returns zeros for it: a program the validator takes is evaluated
    ASSERT(wl_tev_eval_rgba(&p, &px) != 0u);
    PASS();
}

TEST compares(void) {
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    // a = tex, b = ras, c = konst 0 (set below), d = REG0 (preloaded)
    wl_tev_konst_external(&p, 0, rgba(7, 8, 9, 10));
    wl_tev_preload(&p, WL_TEV_REG0, rgba(1, 2, 3, 4));
    s->kcsel = wl_tev_kcsel(0, WL_TEV_K_RGB);
    s->kasel = wl_tev_kasel(0, WL_TEV_K_A);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO);
    struct {
        uint8_t op;
        uint8_t tex[3], ras[3];
        int r, g, b; // the result: d, or d + c in the channels the compare accepts
    } c[] = {
        // R8: only red decides, for all channels
        {WL_TEV_COMP_R8_GT, {10, 0, 0}, {5, 255, 255}, 8, 10, 12},
        {WL_TEV_COMP_R8_GT, {5, 255, 255}, {5, 0, 0}, 1, 2, 3},
        {WL_TEV_COMP_R8_EQ, {5, 0, 255}, {5, 9, 9}, 8, 10, 12},
        {WL_TEV_COMP_R8_EQ, {6, 0, 0}, {5, 0, 0}, 1, 2, 3},
        // GR16: green:red as 16 bits
        {WL_TEV_COMP_GR16_GT, {0, 1, 0}, {255, 0, 9}, 8, 10, 12},   // 256 > 255
        {WL_TEV_COMP_GR16_GT, {255, 0, 0}, {0, 1, 0}, 1, 2, 3},     // 255 > 256: no
        {WL_TEV_COMP_GR16_EQ, {3, 4, 0}, {3, 4, 99}, 8, 10, 12},    // blue is ignored
        {WL_TEV_COMP_GR16_EQ, {3, 4, 0}, {4, 4, 0}, 1, 2, 3},
        // BGR24
        {WL_TEV_COMP_BGR24_GT, {0, 0, 1}, {255, 255, 0}, 8, 10, 12},
        {WL_TEV_COMP_BGR24_GT, {255, 255, 0}, {0, 0, 1}, 1, 2, 3},
        {WL_TEV_COMP_BGR24_EQ, {1, 2, 3}, {1, 2, 3}, 8, 10, 12},
        {WL_TEV_COMP_BGR24_EQ, {1, 2, 3}, {1, 2, 4}, 1, 2, 3},
        // RGB8: each channel by itself
        {WL_TEV_COMP_RGB8_GT, {9, 1, 9}, {8, 2, 8}, 8, 2, 12},
        {WL_TEV_COMP_RGB8_EQ, {9, 1, 9}, {9, 2, 8}, 8, 2, 3},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        wl_tev_stage_color(s, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_KONST, WL_TEV_CC_C0, c[i].op, 0, 0, true,
                           WL_TEV_PREV);
        const uint8_t tex[4] = {c[i].tex[0], c[i].tex[1], c[i].tex[2], 0};
        const uint8_t ras[4] = {c[i].ras[0], c[i].ras[1], c[i].ras[2], 0};
        WlTevPixel px;
        pixel(&px, tex, ras);
        WlTevRegValue v = eval_prev(&p, &px);
        ASSERT_EQ_FMT(c[i].r, (int)v.r, "%d");
        ASSERT_EQ_FMT(c[i].g, (int)v.g, "%d");
        ASSERT_EQ_FMT(c[i].b, (int)v.b, "%d");
    }
    // the compare's sum clamps like the blend's
    wl_tev_stage_color(s, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_CC_ONE, WL_TEV_CC_ONE, WL_TEV_COMP_R8_GT, 0, 0, true,
                       WL_TEV_PREV);
    WlTevPixel px = {0};
    ASSERT_EQ(255, eval_prev(&p, &px).r);
    wl_tev_stage_color(s, WL_TEV_CC_ONE, WL_TEV_CC_ZERO, WL_TEV_CC_ONE, WL_TEV_CC_ONE, WL_TEV_COMP_R8_GT, 0, 0, false,
                       WL_TEV_PREV);
    ASSERT_EQ(510, eval_prev(&p, &px).r);

    // the alpha half's A8 compares
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO);
    for (int eq = 0; eq < 2; eq++) {
        for (int tex_a = 0; tex_a < 3; tex_a++) {
            // a = TEXA, b = RASA (1), c = K0.a (10), d = A0 (4)
            wl_tev_stage_alpha(s, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_KONST, WL_TEV_CA_A0,
                               eq ? WL_TEV_COMP_A8_EQ : WL_TEV_COMP_A8_GT, 0, 0, true, WL_TEV_PREV);
            const uint8_t tex[4] = {0, 0, 0, (uint8_t)tex_a};
            const uint8_t ras[4] = {0, 0, 0, 1};
            pixel(&px, tex, ras);
            bool cond = eq ? tex_a == 1 : tex_a > 1;
            ASSERT_EQ_FMT(cond ? 14 : 4, (int)eval_prev(&p, &px).a, "%d");
        }
    }
    PASS();
}

TEST swap_tables(void) {
    const uint8_t tex[4] = {10, 20, 30, 40};
    const uint8_t ras[4] = {50, 60, 70, 80};
    WlTevPixel px;
    pixel(&px, tex, ras);
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA);
    s->tex_swap = 3; // default table 3: BBBA
    WlTevRegValue v = eval_prev(&p, &px);
    ASSERT_EQ(30, v.r);
    ASSERT_EQ(30, v.g);
    ASSERT_EQ(30, v.b);
    ASSERT_EQ(40, v.a);
    s->tex_swap = 1;
    v = eval_prev(&p, &px);
    ASSERT_EQ(10, v.g);
    wl_tev_set_swap(&p, 1, WL_TEV_SWAP_A, WL_TEV_SWAP_B, WL_TEV_SWAP_G, WL_TEV_SWAP_R); // reverse
    v = eval_prev(&p, &px);
    ASSERT_EQ(40, v.r);
    ASSERT_EQ(30, v.g);
    ASSERT_EQ(20, v.b);
    ASSERT_EQ(10, v.a);
    // the rasterised colour has its own table; TEXA in the colour half is the alpha in all channels
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC);
    s->tex_swap = 0;
    s->ras_swap = 2; // GGGA
    v = eval_prev(&p, &px);
    ASSERT_EQ(60, v.r);
    ASSERT_EQ(60, v.b);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASA);
    s->ras_swap = 0;
    ASSERT_EQ(80, eval_prev(&p, &px).g);
    // channel 1
    s->channel = WL_TEV_CH_COLOR1A1;
    pixel(&px, tex, ras);
    px.ras[1][0] = 5;
    px.ras[1][3] = 9;
    ASSERT_EQ(9, eval_prev(&p, &px).g);
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC);
    ASSERT_EQ(5, eval_prev(&p, &px).r);
    PASS();
}

TEST konst_selections(void) {
    WlTevPixel px = {0};
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    s->channel = WL_TEV_NONE;
    s->texcoord = s->texmap = WL_TEV_NONE;
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_KONST);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_KONST);
    ASSERT_EQ(0, wl_tev_konst_slot(&p, rgba(11, 22, 33, 44)));
    ASSERT_EQ(1, wl_tev_konst_slot(&p, rgba(55, 66, 77, 88)));
    static const int fraction[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    for (uint8_t f = 0; f < 8; f++) {
        s->kcsel = f;
        s->kasel = f;
        WlTevRegValue v = eval_prev(&p, &px);
        ASSERT_EQ(fraction[f], v.r);
        ASSERT_EQ(fraction[f], v.b);
        ASSERT_EQ(fraction[f], v.a);
    }
    s->kcsel = wl_tev_kcsel(1, WL_TEV_K_RGB);
    s->kasel = wl_tev_kasel(1, WL_TEV_K_B);
    WlTevRegValue v = eval_prev(&p, &px);
    ASSERT_EQ(55, v.r);
    ASSERT_EQ(66, v.g);
    ASSERT_EQ(77, v.b);
    ASSERT_EQ(77, v.a);
    s->kcsel = wl_tev_kcsel(0, WL_TEV_K_G);
    s->kasel = wl_tev_kasel(0, WL_TEV_K_R);
    v = eval_prev(&p, &px);
    ASSERT_EQ(22, v.r);
    ASSERT_EQ(22, v.g);
    ASSERT_EQ(22, v.b);
    ASSERT_EQ(11, v.a);
    s->kcsel = wl_tev_kcsel(0, WL_TEV_K_A);
    s->kasel = wl_tev_kasel(1, WL_TEV_K_A);
    v = eval_prev(&p, &px);
    ASSERT_EQ(44, v.g);
    ASSERT_EQ(88, v.a);
    PASS();
}

TEST op_with_preloaded_register(void) {
    WlTevPixel px = {0};
    WlTevProgram p;
    WlTevStage *s = one_stage(&p);
    s->channel = WL_TEV_NONE;
    s->texcoord = s->texmap = WL_TEV_NONE;
    wl_tev_preload(&p, WL_TEV_REG2, rgba(10, 20, 30, 40));
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C2);
    set_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_A2);
    ASSERT_EQ(rgba(10, 20, 30, 40), wl_tev_eval_rgba(&p, &px));
    set_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_A2); // the alpha in all three
    ASSERT_EQ(rgba(40, 40, 40, 40), wl_tev_eval_rgba(&p, &px));
    PASS();
}

// ---- the recipes through the evaluator ----------------------------------------------------------

TEST every_recipe_is_a_clean_program(void) {
    for (int r = 0; r < WL_GX_RECIPE_COUNT; r++) {
        WlTevProgram p;
        wl_tev_recipe(&p, (WlGxRecipe)r);
        WlTevReport rep;
        ASSERT_EQm("recipe validates", 1, wl_tev_validate(&p, &rep));
        ASSERT_EQ_FMT(0u, rep.warnings, "%u");
        ASSERT(p.stage_count >= 1 && p.stage_count <= 2);
        ASSERT_EQ(0x01, p.konst_external); // konst 0 is wl_gx_set_konst's
        ASSERT_EQ(0, p.konst_mask);
    }
    PASS();
}

TEST recipes_compute_what_platform_h_says(void) {
    WlTevPixel px;
    const uint8_t tex[4] = {200, 100, 50, 120};
    const uint8_t ras[4] = {255, 0, 255, 255};
    WlTevProgram p;
    pixel(&px, tex, ras);

    wl_tev_recipe(&p, WL_GX_RECIPE_VERTEX_COLOR);
    ASSERT_EQ(rgba(255, 0, 255, 255), wl_tev_eval_rgba(&p, &px));
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_REPLACE);
    ASSERT_EQ(rgba(200, 100, 50, 120), wl_tev_eval_rgba(&p, &px));
    // ras 255 is a full factor, so these are exact: tex * ras
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_MODULATE);
    ASSERT_EQ(rgba(200, 0, 50, 120), wl_tev_eval_rgba(&p, &px));
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_LIT);
    ASSERT_EQ(rgba(200, 0, 50, 120), wl_tev_eval_rgba(&p, &px));

    // tint: times konst (255, 0, 255 and alpha 255: exact; then a half alpha factor, 129/256)
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_TINT);
    p.konst[0] = rgba(255, 255, 0, 255);
    ASSERT_EQ(rgba(200, 0, 0, 120), wl_tev_eval_rgba(&p, &px));
    p.konst[0] = rgba(255, 255, 255, 128);
    ASSERT_EQ(rgba(200, 0, 50, (120 * 129 + 128) >> 8), wl_tev_eval_rgba(&p, &px));

    // additive: tex * ras * ras.a, alpha 1 whatever the texture's
    wl_tev_recipe(&p, WL_GX_RECIPE_ADDITIVE);
    ASSERT_EQ(rgba(200, 0, 50, 255), wl_tev_eval_rgba(&p, &px));
    const uint8_t faded[4] = {255, 255, 255, 0};
    pixel(&px, tex, faded);
    ASSERT_EQ(rgba(0, 0, 0, 255), wl_tev_eval_rgba(&p, &px)); // fully faded: black, still opaque
    const uint8_t half[4] = {255, 255, 255, 128};
    pixel(&px, tex, half);
    {
        // the vertex alpha 128 is the factor 129/256 of tex (255 ras * tex, then * ras.a)
        uint32_t c = wl_tev_eval_rgba(&p, &px);
        ASSERT_EQ((200 * 129 + 128) >> 8, (int)(c >> 24));
        ASSERT_EQ(255, (int)(c & 0xFF));
    }

    // ui: tex * ras, alpha also times konst alpha
    pixel(&px, tex, ras);
    wl_tev_recipe(&p, WL_GX_RECIPE_UI);
    p.konst[0] = rgba(1, 2, 3, 255);
    ASSERT_EQ(rgba(200, 0, 50, 120), wl_tev_eval_rgba(&p, &px));
    p.konst[0] = rgba(1, 2, 3, 0);
    ASSERT_EQ(rgba(200, 0, 50, 0), wl_tev_eval_rgba(&p, &px));

    // text: rgb from the vertex colour, alpha = glyph coverage * vertex alpha
    wl_tev_recipe(&p, WL_GX_RECIPE_TEXT);
    ASSERT_EQ(rgba(255, 0, 255, 120), wl_tev_eval_rgba(&p, &px));

    // konst: a flat colour, no texture and no vertex colour read
    wl_tev_recipe(&p, WL_GX_RECIPE_KONST);
    p.konst[0] = rgba(9, 8, 7, 6);
    ASSERT_EQ(rgba(9, 8, 7, 6), wl_tev_eval_rgba(&p, &px));
    ASSERT_EQ(0u, wl_tev_channels_used(&p));
    ASSERT_EQ(0u, wl_tev_texcoords_used(&p));
    PASS();
}

TEST recipes_name_their_channels_and_coordinates(void) {
    static const struct {
        WlGxRecipe recipe;
        uint32_t chans, texcoords;
    } expect[] = {
        {WL_GX_RECIPE_VERTEX_COLOR, 1, 0}, {WL_GX_RECIPE_TEX_REPLACE, 0, 1}, {WL_GX_RECIPE_TEX_MODULATE, 1, 1},
        {WL_GX_RECIPE_TEX_LIT, 1, 1},      {WL_GX_RECIPE_TEX_TINT, 1, 1},    {WL_GX_RECIPE_ADDITIVE, 1, 1},
        {WL_GX_RECIPE_UI, 1, 1},           {WL_GX_RECIPE_TEXT, 1, 1},        {WL_GX_RECIPE_KONST, 0, 0},
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        WlTevProgram p;
        wl_tev_recipe(&p, expect[i].recipe);
        ASSERT_EQ(expect[i].chans, wl_tev_channels_used(&p));
        ASSERT_EQ(expect[i].texcoords, wl_tev_texcoords_used(&p));
    }
    PASS();
}

// ---- apply (host: recorded) ---------------------------------------------------------------------

TEST apply_checks_then_records(void) {
    wl_host_tev_reset();
    ASSERT(wl_host_tev_last() == NULL);
    WlTevProgram p;
    wl_tev_recipe(&p, WL_GX_RECIPE_UI);
    ASSERT_EQ(WL_OK, wl_tev_apply(&p));
    ASSERT_EQ(1u, wl_host_tev_applies());
    ASSERT(wl_host_tev_last() != NULL && wl_tev_equal(wl_host_tev_last(), &p));
    p.stages[0].color.dest = 9; // invalid: refused, the last program stays
    ASSERT_EQ(WL_ERR_ARG, wl_tev_apply(&p));
    ASSERT_EQ(1u, wl_host_tev_applies());
    ASSERT_EQ(0u, wl_host_tev_last_flags());
    wl_tev_apply_unchecked(&p);
    ASSERT_EQ(2u, wl_host_tev_applies());
    // the flags reach the backend, through every entry point
    wl_tev_recipe(&p, WL_GX_RECIPE_UI);
    ASSERT_EQ(WL_OK, wl_tev_apply_ex(&p, WL_TEV_APPLY_SKIP_SWAPS));
    ASSERT_EQ(3u, wl_host_tev_applies());
    ASSERT_EQ(WL_TEV_APPLY_SKIP_SWAPS, wl_host_tev_last_flags());
    ASSERT_EQ(WL_OK, wl_tev_apply(&p));
    ASSERT_EQ(0u, wl_host_tev_last_flags());
    wl_tev_apply_unchecked_ex(&p, WL_TEV_APPLY_SKIP_SWAPS);
    ASSERT_EQ(WL_TEV_APPLY_SKIP_SWAPS, wl_host_tev_last_flags());
    wl_tev_apply_unchecked(&p);
    ASSERT_EQ(0u, wl_host_tev_last_flags());
    p.stages[0].color.dest = 9;
    ASSERT_EQ(WL_ERR_ARG, wl_tev_apply_ex(&p, WL_TEV_APPLY_SKIP_SWAPS));
    ASSERT_EQ(0u, wl_host_tev_last_flags());
    PASS();
}

// ---- a translation, the way a client would use the pieces ----------------------------------------

// A client's Xbox-style combiner "a*b + c*d" built with the pool, needs and konst helpers: two
// products, each a stage, the sum a third. Texture 1 times the vertex colour, plus texture 0
// times a constant.
TEST a_two_product_sum_built_from_the_helpers(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    WlTevRegPool pool;
    wl_tev_pool_init(&pool);
    int slot = wl_tev_konst_slot(&p, rgba(128, 128, 128, 255));
    ASSERT_EQ(0, slot);

    int r0 = wl_tev_pool_alloc(&pool, 0);
    WlTevStage *s = wl_tev_add_stage(&p);
    WlTevNeeds n;
    wl_tev_needs_clear(&n);
    ASSERT(wl_tev_needs_texture(&n, 1, 1, WL_TEV_SWAPS_ANY));
    ASSERT(wl_tev_needs_channel(&n, WL_TEV_CH_COLOR0A0, WL_TEV_SWAPS_ANY));
    wl_tev_stage_set_needs(s, &n);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_ADD, 0, 0, true,
                       (uint8_t)r0);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_ADD, 0, 0, true,
                       (uint8_t)r0);

    s = wl_tev_add_stage(&p);
    wl_tev_needs_clear(&n);
    ASSERT(wl_tev_needs_texture(&n, 0, 0, WL_TEV_SWAPS_ANY));
    wl_tev_stage_set_needs(s, &n);
    s->kcsel = wl_tev_kcsel((uint32_t)slot, WL_TEV_K_RGB);
    // PREV = (1 - 0) * 0 + ... : tex0 * konst + the first product (d), into PREV
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_KONST, (uint8_t)(r0 * 2), WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_pool_release(&pool, (uint8_t)r0);

    WlTevReport rep;
    ASSERT(wl_tev_validate(&p, &rep));
    ASSERT_EQ(0u, rep.errors);
    WlTevPixel px = {0};
    px.tex[0][0] = 100;
    px.tex[1][0] = 60;
    px.ras[0][0] = 255;
    // 60 * 255/255 + 100 * 128/256-ish (c = 128 -> factor 129/256): 60 + ((100 * 129 + 128) >> 8)
    ASSERT_EQ(60 + ((100 * 129 + 128) >> 8), (int)(wl_tev_eval_rgba(&p, &px) >> 24));
    PASS();
}

SUITE(tev_suite) {
    RUN_TEST(program_init_and_stage_limit);
    RUN_TEST(konst_selection_helpers);
    RUN_TEST(konst_slots);
    RUN_TEST(register_pool);
    RUN_TEST(needs_merge_and_conflicts);
    RUN_TEST(program_uses);
    RUN_TEST(program_use_masks);
    RUN_TEST(equal_and_hash);
    RUN_TEST(valid_program_has_no_issues);
    RUN_TEST(stage_count_errors);
    RUN_TEST(each_error_is_found);
    RUN_TEST(a_ras_read_with_the_zero_channel_is_legal);
    RUN_TEST(reports_are_capped_but_counted);
    RUN_TEST(dead_write_warning);
    RUN_TEST(no_result_warning);
    RUN_TEST(read_before_write_and_konst_warnings);
    RUN_TEST(pass_through_halves_raise_no_warnings);
    RUN_TEST(names_exist);
    RUN_TEST(blend_modulate_and_replace);
    RUN_TEST(blend_lerp_and_sum);
    RUN_TEST(bias_and_scale);
    RUN_TEST(registers_and_timing);
    RUN_TEST(inputs_a_b_c_are_eight_bits);
    RUN_TEST(input_d_is_the_full_signed_register);
    RUN_TEST(bias_with_divide_2);
    RUN_TEST(compares);
    RUN_TEST(swap_tables);
    RUN_TEST(konst_selections);
    RUN_TEST(op_with_preloaded_register);
    RUN_TEST(every_recipe_is_a_clean_program);
    RUN_TEST(recipes_compute_what_platform_h_says);
    RUN_TEST(recipes_name_their_channels_and_coordinates);
    RUN_TEST(apply_checks_then_records);
    RUN_TEST(a_two_product_sum_built_from_the_helpers);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(tev_suite);
    GREATEST_MAIN_END();
}
