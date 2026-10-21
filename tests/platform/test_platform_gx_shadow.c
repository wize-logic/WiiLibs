// SPDX-License-Identifier: MIT
// The shadow of a texture map's registers and of the vertex arrays (wl/platform/gx_shadow.c): what a
// bind writes (nothing, the registers that differ, the whole load), the switches for the old ways,
// forgetting, and the counts; Dolphin's batches (a model) and the fences' tokens put off, also through
// wl_gx_fence (sent at a cut, at the latest primitive, at a poll; at once with gxfenceeager).
#include "wl/platform/gx_shadow.h"

#include <string.h>

#include "greatest.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/platform.h"
#include "wl/platform/tev.h"

static void regs_of(uint32_t regs[WL_GX_TEXREGS], uintptr_t address, uint32_t width, uint32_t wrap) {
    wl_gx_shadow_texture_model(regs, address, WL_GX_TEX_CMPR, width, 256, 9, wrap, wrap, WL_GX_FILTER_LINEAR_MIP_LINEAR,
                               WL_GX_FILTER_LINEAR, 0.0f, 0, wl_gx_shadow_texture_region(WL_GX_TEX_CMPR, 9, false));
}

TEST texture_binds_write_what_differs(void) {
    uint32_t a[WL_GX_TEXREGS], b[WL_GX_TEXREGS], c[WL_GX_TEXREGS], binds, changed, loads, writes;

    wl_gx_shadow_texture_forget(0xFFu);
    regs_of(a, 0x10000, 256, WL_GX_WRAP_REPEAT);
    regs_of(b, 0x20000, 256, WL_GX_WRAP_REPEAT); // (another address: image 3 alone)
    regs_of(c, 0x30000, 128, WL_GX_WRAP_REPEAT); // (another size: the whole load)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, a, 6, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture(2, a, 6, false));
    ASSERT_EQ(1u << WL_GX_TEXREG_IMAGE3, wl_gx_shadow_texture(2, b, 6, false));
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, c, 6, false));
    regs_of(c, 0x30000, 128, WL_GX_WRAP_CLAMP); // (another mode 0: the whole load)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, c, 6, false));
    // (another map knows nothing yet; the switch gxtexload loads whole at every bind)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(3, c, 6, false));
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(3, c, 6, true));
    // (forgotten: loaded whole again)
    wl_gx_shadow_texture_forget(1u << 2);
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, c, 6, false));
    wl_gx_shadow_texture_kept();
    wl_gx_shadow_texture_counts(&binds, &changed, &loads, &writes);
    ASSERT(binds >= 9u && changed + 2u <= binds && loads + 1u <= changed);
    PASS();
}

TEST texture_tlut_register(void) {
    uint32_t a[WL_GX_TEXREGS], b[WL_GX_TEXREGS];

    wl_gx_shadow_texture_forget(0xFFu);
    wl_gx_shadow_texture_model(a, 0x10000, WL_GX_TEX_C8, 64, 64, 1, 0, 0, 1, 1, 0.0f, 3,
                               wl_gx_shadow_texture_region(WL_GX_TEX_C8, 1, false));
    memcpy(b, a, sizeof b);
    b[WL_GX_TEXREG_TLUT] = 4;
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x7Fu, wl_gx_shadow_texture(0, a, 7, false));
    ASSERT_EQ(1u << WL_GX_TEXREG_TLUT, wl_gx_shadow_texture(0, b, 7, false));
    // (a texture without a TLUT after one with: the whole load)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(0, b, 6, false));
    PASS();
}

TEST texture_regions(void) {
    // (the even bank alone for colour-indexed textures and CMPR without mipmaps; libogc's three sets)
    ASSERT_EQ(1u, wl_gx_shadow_texture_region(WL_GX_TEX_C4, 1, false));
    ASSERT_EQ(1u, wl_gx_shadow_texture_region(WL_GX_TEX_CMPR, 1, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture_region(WL_GX_TEX_CMPR, 4, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture_region(WL_GX_TEX_RGBA8, 1, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture_region(WL_GX_TEX_C8, 1, true));
    ASSERT_EQ(2u, wl_gx_shadow_texture_region(WL_GX_TEX_RGBA8, 3, true));
    ASSERT_EQ(1u, wl_gx_shadow_texture_region(WL_GX_TEX_RGBA8, 1, true));
    PASS();
}

TEST arrays_sent_when_they_differ(void) {
    static uint8_t data[64];
    uint32_t calls, sent, calls0, sent0;

    wl_gx_shadow_forget();
    wl_gx_shadow_array_counts(&calls0, &sent0);
    ASSERT(wl_gx_shadow_array(0, data, 12, false));
    ASSERT_FALSE(wl_gx_shadow_array(0, data, 12, false));
    ASSERT(wl_gx_shadow_array(0, data, 16, false));
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, false));
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, true)); // (gxarrayall)
    wl_gx_shadow_array_forget();
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, false));
    wl_gx_shadow_forget();
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, false));
    wl_gx_shadow_array_counts(&calls, &sent);
    ASSERT_EQ(7u, calls - calls0);
    ASSERT_EQ(6u, sent - sent0);
    PASS();
}

static void stats_now(WlGxDirectStats *out) {
    memset(out, 0, sizeof *out);
    wl_gx_shadow_stats(out);
}

// The transform unit's flush after every token (gx_direct.h): a point through the host's direct layer, which
// counts what the console sends; if a token was sent since the point before, the point is flushed first.
static WlGxDirectStats g_last;

static int flushed_point(void) {
    const uint32_t words[3] = {0, 0, 0};
    WlGxDirectStats now;
    int flushes;

    wl_gx_direct_primitive_begin(WL_GX_DIRECT_POINTS, 1);
    wl_gx_direct_vertex_words(words, 3);
    wl_gx_direct_primitive_end();
    wl_gx_direct_stats(&now);
    flushes = (int)(now.flushes - g_last.flushes);
    if (now.tokens != g_last.tokens && flushes == 0) {
        flushes = -1; // (a token, put off or not, and no flush after it)
    }
    g_last = now;
    return flushes;
}

// Every token is followed by the flush before the next direct primitive: a fence's token put off past the
// flush its caller asked for (sent at a change of state, at the latest primitive, at a poll or a wait), one
// sent at once (the switch gxfenceeager), wl_gx_sync's.
TEST every_token_followed_by_the_flush(void) {
    WlGxDirectStats a, b;
    WlGxDirectLayout layout;
    WlGxBisect bisect;
    const uint8_t blend[3] = {1, 2, 3}, blend2[3] = {3, 2, 1};
    uint32_t f;
    unsigned n;

    ASSERT_EQ(WL_OK, wl_gx_init());
    memset(&bisect, 0, sizeof bisect);
    wl_gx_bisect_set(&bisect);
    memset(&layout, 0, sizeof layout);
    wl_gx_direct_begin();
    wl_gx_direct_layout(&layout);
    wl_gx_direct_stats(&g_last);
    ASSERT(flushed_point() >= 0); // (the flush after begin)
    // (a fence put off, the caller's flush spent by the next point; the token at a change of state, flushed)
    (void)wl_gx_fence();
    wl_gx_direct_flush_after(); // (as wl/xbox's made_fence asks)
    ASSERT(wl_gx_shadow_token_deferred());
    wl_gx_direct_stats(&a);
    ASSERT(flushed_point() >= 0);
    ASSERT(flushed_point() >= 0);
    ASSERT(wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, blend, sizeof blend));
    ASSERT(flushed_point() >= 0);
    wl_gx_direct_stats(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_EQ(2u, b.flushes - a.flushes); // (the caller's, and the one after the token)
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    // (at the latest primitive of the same state)
    (void)wl_gx_fence();
    wl_gx_direct_flush_after();
    wl_gx_direct_stats(&a);
    for (n = 0; n < WL_GX_SHADOW_TOKEN_LATE; n++) {
        ASSERT(flushed_point() >= 0);
    }
    wl_gx_direct_stats(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_EQ(2u, b.flushes - a.flushes);
    // (a poll, and a wait: the token sent then, the next point flushed)
    f = wl_gx_fence();
    ASSERT(flushed_point() >= 0);
    ASSERT(wl_gx_fence_passed(f));
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    ASSERT_EQ(1, flushed_point());
    f = wl_gx_fence();
    ASSERT(flushed_point() >= 0);
    ASSERT(wl_gx_fence_wait(f));
    ASSERT_EQ(1, flushed_point());
    ASSERT_EQ(0, flushed_point());
    // (wl_gx_sync's token)
    ASSERT(wl_gx_sync());
    ASSERT_EQ(1, flushed_point());
    // (the switch gxfenceeager: at once, flushed after)
    bisect.fence_eager = true;
    wl_gx_bisect_set(&bisect);
    (void)wl_gx_fence();
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    ASSERT(wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, blend2, sizeof blend2));
    ASSERT_EQ(1, flushed_point());
    bisect.fence_eager = false;
    wl_gx_bisect_set(&bisect);
    wl_gx_shutdown();
    PASS();
}

TEST batches_cut_by_state_and_tokens(void) {
    WlGxDirectStats a, b;
    static uint8_t data[16];
    const uint8_t blend[3] = {1, 2, 3};

    wl_gx_shadow_forget();
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u); // (whatever came before: a primitive of its own)
    stats_now(&a);
    // (two primitives of the same state: one batch; the arrays' writes cut none)
    ASSERT(wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, blend, sizeof blend));
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    ASSERT(wl_gx_shadow_array(1, data, 12, false));
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    // (a token between two primitives cuts the batch, alone)
    wl_gx_shadow_token();
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    stats_now(&b);
    ASSERT_EQ(3u, b.primitives - a.primitives);
    ASSERT_EQ(2u, b.batches - a.batches);
    ASSERT_EQ(1u, b.batch_breaks[WL_GX_FIFO_STATE] - a.batch_breaks[WL_GX_FIFO_STATE]);
    ASSERT_EQ(1u, b.batch_alone[WL_GX_FIFO_STATE] - a.batch_alone[WL_GX_FIFO_STATE]);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_EQ(1u, b.token_alone - a.token_alone);
    PASS();
}

TEST tokens_put_off_to_a_cut(void) {
    WlGxDirectStats a, b;
    const uint8_t blend[3] = {4, 5, 6};
    unsigned n;

    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    stats_now(&a);
    // (put off: not due before a primitive of the same state, due before one after a change of state)
    wl_gx_shadow_token_defer();
    wl_gx_shadow_token_defer(); // (two fences, one token)
    ASSERT(wl_gx_shadow_token_deferred());
    ASSERT_FALSE(wl_gx_shadow_token_due());
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    ASSERT(wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, blend, sizeof blend));
    ASSERT(wl_gx_shadow_token_due());
    wl_gx_shadow_token();
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    stats_now(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_EQ(1u, b.batches - a.batches); // (the token and the state cut the same batch)
    ASSERT_EQ(0u, b.token_alone - a.token_alone);
    // (due at the latest before the WL_GX_SHADOW_TOKEN_LATE'th primitive of one batch)
    wl_gx_shadow_token_defer();
    for (n = 1; n < WL_GX_SHADOW_TOKEN_LATE; n++) {
        ASSERT_FALSE(wl_gx_shadow_token_due());
        wl_gx_shadow_fifo(WL_GX_FIFO_VERTEX, 3u);
    }
    ASSERT(wl_gx_shadow_token_due());
    wl_gx_shadow_token();
    // (a token sent for another reason takes the one put off with it)
    wl_gx_shadow_token_defer();
    wl_gx_shadow_token();
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    PASS();
}

// A point through the direct layer (the host's, which counts what the console's sends)
static void fence_point(void) {
    const uint32_t words[3] = {0, 0, 0};

    wl_gx_direct_primitive_begin(WL_GX_DIRECT_POINTS, 1);
    wl_gx_direct_vertex_words(words, 3);
    wl_gx_direct_primitive_end();
}

// A fence (wl_gx_fence) through the GX layer: its token put off, sent before the next primitive after a
// change of state, before the WL_GX_SHADOW_TOKEN_LATE'th primitive at the latest, and at once when the
// fence (or a later one) is polled or waited for, not for an older one; with the switch gxfenceeager
// (WlGxBisect.fence_eager) at once.
TEST a_fence_token_waits_for_a_cut_or_a_poll(void) {
    WlGxDirectStats a, b;
    WlGxDirectLayout layout;
    WlGxBisect bisect;
    const uint8_t blend_a[3] = {7, 8, 9};
    uint32_t f1, f2, f3, f4;
    unsigned n;

    ASSERT_EQ(WL_OK, wl_gx_init());
    memset(&bisect, 0, sizeof bisect);
    wl_gx_bisect_set(&bisect);
    memset(&layout, 0, sizeof layout);
    wl_gx_direct_layout(&layout);
    fence_point();
    // (the next primitive of the same state: still put off; one after a change of state: sent before it)
    stats_now(&a);
    f1 = wl_gx_fence();
    ASSERT(f1 != 0u);
    ASSERT(wl_gx_shadow_token_deferred());
    fence_point();
    stats_now(&b);
    ASSERT_EQ(0u, b.tokens - a.tokens);
    ASSERT(wl_gx_shadow_differs(WL_GX_SHADOW_BLEND, blend_a, sizeof blend_a));
    fence_point();
    stats_now(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    // (at the latest before the WL_GX_SHADOW_TOKEN_LATE'th primitive of the same state)
    stats_now(&a);
    (void)wl_gx_fence();
    for (n = 1; n < WL_GX_SHADOW_TOKEN_LATE; n++) {
        fence_point();
    }
    stats_now(&b);
    ASSERT_EQ(0u, b.tokens - a.tokens);
    fence_point();
    stats_now(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    // (polled: at once, one token for two fences; a poll of an older fence, or of one already sent, sends none)
    stats_now(&a);
    f2 = wl_gx_fence();
    f3 = wl_gx_fence();
    ASSERT(wl_gx_fence_passed(f1));
    stats_now(&b);
    ASSERT_EQ(0u, b.tokens - a.tokens);
    ASSERT(wl_gx_shadow_token_deferred());
    ASSERT(wl_gx_fence_passed(f2));
    stats_now(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    ASSERT(wl_gx_fence_passed(f3));
    stats_now(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    // (waited for: at once too)
    f4 = wl_gx_fence();
    ASSERT(wl_gx_fence_wait(f4));
    stats_now(&b);
    ASSERT_EQ(2u, b.tokens - a.tokens);
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    // (the switch gxfenceeager: the token at once)
    bisect.fence_eager = true;
    wl_gx_bisect_set(&bisect);
    stats_now(&a);
    (void)wl_gx_fence();
    stats_now(&b);
    ASSERT_EQ(1u, b.tokens - a.tokens);
    ASSERT_FALSE(wl_gx_shadow_token_deferred());
    bisect.fence_eager = false;
    wl_gx_bisect_set(&bisect);
    wl_gx_shutdown();
    PASS();
}

// GX's maps as a cache: a texture found where it is, a new one put where nothing another stage reads is, the
// TEV's orders following the maps, wl/platform's own loads making their maps their own again, and the switch off.
TEST maps_as_a_cache(void) {
    static const uint32_t ka[WL_GX_MAP_KEY_WORDS] = {1, 2, 3, 0x100, 0xFFFFFF};
    static const uint32_t kb[WL_GX_MAP_KEY_WORDS] = {1, 2, 3, 0x200, 0xFFFFFF};
    static const uint32_t kc[WL_GX_MAP_KEY_WORDS] = {1, 2, 3, 0x300, 0xFFFFFF};
    WlTevProgram p;
    WlTevSend send;
    WlGxDirectStats st;
    bool held = true;

    wl_gx_shadow_maps_cache(true);
    ASSERT(wl_gx_shadow_maps_cache_on());
    (void)wl_gx_shadow_texture_forget(0xFFu);
    wl_gx_shadow_forget();
    wl_gx_shadow_maps_followed();
    // (map 0 empty: A there; B then to an empty map, A's kept)
    ASSERT_EQ(0u, wl_gx_shadow_map_bind(0, ka, &held));
    ASSERT_FALSE(held);
    ASSERT_FALSE(wl_gx_shadow_maps_moved());
    ASSERT_EQ(1u, wl_gx_shadow_map_bind(0, kb, &held));
    ASSERT_FALSE(held);
    ASSERT_EQ(1u, wl_gx_shadow_map_of(0));
    ASSERT(wl_gx_shadow_maps_moved());
    // (a program applied reads the map its logical map went to: its order held, the maps followed)
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_MODULATE);
    ASSERT_EQ(0, p.stages[0].texmap);
    wl_tev_shadow_plan(&p, WL_TEV_APPLY_CHANGES, &send);
    ASSERT_FALSE(wl_gx_shadow_maps_moved());
    ASSERT_FALSE(wl_tev_shadow_order_moved(0, p.stages[0].texcoord, 1, p.stages[0].channel));
    // (A again: held in map 0, the order moved back, sent once)
    wl_gx_shadow_stats(&st);
    ASSERT_EQ(0u, wl_gx_shadow_map_bind(0, ka, &held));
    ASSERT(held);
    ASSERT(wl_gx_shadow_maps_moved());
    ASSERT(wl_tev_shadow_order_moved(0, p.stages[0].texcoord, 0, p.stages[0].channel));
    ASSERT_FALSE(wl_tev_shadow_order_moved(0, p.stages[0].texcoord, 0, p.stages[0].channel));
    {
        WlGxDirectStats after;

        wl_gx_shadow_stats(&after);
        ASSERT_EQ(1u, after.tex_map_hits - st.tex_map_hits);
        ASSERT_EQ(1u, after.tex_map_orders - st.tex_map_orders);
    }
    // (another logical map's bind of A shares map 0; its C then goes to an empty map, not over map 0 that
    // logical map 0 reads, nor over map 1)
    ASSERT_EQ(0u, wl_gx_shadow_map_bind(1, ka, &held));
    ASSERT(held);
    ASSERT_EQ(2u, wl_gx_shadow_map_bind(1, kc, &held));
    ASSERT_FALSE(held);
    // (B found in map 1, which no logical map read)
    ASSERT_EQ(1u, wl_gx_shadow_map_bind(2, kb, &held));
    ASSERT(held);
    // (the empty maps filled, then the one bound longest ago that no other logical map reads; maps 6 and 7
    // never another logical map's)
    {
        uint32_t k[WL_GX_MAP_KEY_WORDS] = {1, 2, 3, 0x400, 0xFFFFFF};

        ASSERT_EQ(3u, wl_gx_shadow_map_bind(3, k, &held));
        k[3] = 0x500;
        ASSERT_EQ(4u, wl_gx_shadow_map_bind(3, k, &held));
        k[3] = 0x600;
        ASSERT_EQ(5u, wl_gx_shadow_map_bind(3, k, &held));
        k[3] = 0x700;
        ASSERT_EQ(3u, wl_gx_shadow_map_bind(3, k, &held));
        ASSERT_FALSE(held);
    }
    // (wl/platform's own loads: the maps they load their own logical maps' again; all forgotten frees all)
    ASSERT(wl_gx_shadow_texture_forget(0xFFu));
    ASSERT_EQ(1u, wl_gx_shadow_map_of(1));
    ASSERT_EQ(5u, wl_gx_shadow_map_bind(5, ka, &held));
    ASSERT_FALSE(held);
    // (off: every texture on its logical map)
    wl_gx_shadow_maps_cache(false);
    ASSERT_EQ(4u, wl_gx_shadow_map_bind(4, ka, &held));
    ASSERT_FALSE(held);
    ASSERT_EQ(2u, wl_gx_shadow_map_of(2));
    wl_gx_shadow_maps_cache(true);
    PASS();
}

// The TEV's plan (wl_tev_shadow_plan, its stages compared as words): each stage's calls those of the fields
// that differ from the stage GX holds, field by field as before (texture maps moved by the cache included),
// every call for a stage GX does not hold, for random programs changed a few fields at a time.
static uint32_t g_plan_rng = 0x1234567u;
static unsigned plan_rnd(unsigned n) {
    g_plan_rng = g_plan_rng * 1664525u + 1013904223u;
    return (g_plan_rng >> 8) % n;
}

static uint8_t plan_reference(const WlTevStage *now, const WlTevStage *was) {
    uint8_t send = 0;

    if (now->texcoord != was->texcoord || now->texmap != was->texmap || now->channel != was->channel) {
        send |= WL_TEV_SEND_ORDER;
    }
    if (now->ras_swap != was->ras_swap || now->tex_swap != was->tex_swap) {
        send |= WL_TEV_SEND_SWAP;
    }
    send |= now->kcsel != was->kcsel ? WL_TEV_SEND_KCSEL : 0;
    send |= now->kasel != was->kasel ? WL_TEV_SEND_KASEL : 0;
    send |= memcmp(now->color.in, was->color.in, 4) ? WL_TEV_SEND_COLOR_IN : 0;
    send |= memcmp(&now->color.op, &was->color.op, 5) ? WL_TEV_SEND_COLOR_OP : 0;
    send |= memcmp(now->alpha.in, was->alpha.in, 4) ? WL_TEV_SEND_ALPHA_IN : 0;
    send |= memcmp(&now->alpha.op, &was->alpha.op, 5) ? WL_TEV_SEND_ALPHA_OP : 0;
    return send;
}

static void plan_byte_change(WlTevStage *st) {
    uint8_t *b = (uint8_t *)st;
    unsigned at = plan_rnd(sizeof *st);

    b[at] = at == 1 ? (uint8_t)(plan_rnd(3) == 0 ? WL_TEV_NONE : plan_rnd(WL_TEV_TEXMAPS)) : (uint8_t)plan_rnd(256);
}

TEST tev_plan_sends_the_fields_that_differ(void) {
    static const uint32_t ka[WL_GX_MAP_KEY_WORDS] = {9, 2, 3, 0x100, 0xFFFFFF};
    static const uint32_t kb[WL_GX_MAP_KEY_WORDS] = {9, 2, 3, 0x200, 0xFFFFFF};
    WlTevStage held[WL_TEV_MAX_STAGES];
    uint32_t known = 0;
    WlTevProgram p;
    WlTevSend send;
    bool kept;

    // (logical map 0 on map 1: the stages that read it held as reading map 1)
    wl_gx_shadow_maps_cache(true);
    (void)wl_gx_shadow_texture_forget(0xFFu);
    wl_gx_shadow_forget();
    (void)wl_gx_shadow_map_bind(0, ka, &kept);
    (void)wl_gx_shadow_map_bind(0, kb, &kept);
    ASSERT_EQ(1u, wl_gx_shadow_map_of(0));
    memset(&p, 0, sizeof p);
    p.stage_count = WL_TEV_MAX_STAGES;
    for (unsigned i = 0; i < WL_TEV_MAX_STAGES; i++) {
        for (unsigned n = 0; n < 8; n++) {
            plan_byte_change(&p.stages[i]);
        }
        p.stages[i].ras_swap &= 3u;
        p.stages[i].tex_swap &= 3u;
    }
    for (unsigned round = 0; round < 2000; round++) {
        p.stage_count = (uint8_t)(1u + plan_rnd(WL_TEV_MAX_STAGES));
        for (unsigned n = plan_rnd(4); n > 0; n--) {
            WlTevStage *st = &p.stages[plan_rnd(p.stage_count)];

            plan_byte_change(st);
            st->ras_swap &= 3u;
            st->tex_swap &= 3u;
        }
        wl_tev_shadow_plan(&p, WL_TEV_APPLY_CHANGES, &send);
        for (unsigned i = 0; i < p.stage_count; i++) {
            WlTevStage moved = p.stages[i];
            uint8_t expect;

            if (moved.texmap < WL_TEV_TEXMAPS) {
                moved.texmap = (uint8_t)wl_gx_shadow_map_of(moved.texmap);
            }
            expect = (known & (1u << i)) ? plan_reference(&moved, &held[i]) : (uint8_t)WL_TEV_SEND_STAGE;
            ASSERT_EQ(expect, send.stages[i]);
            if (expect != 0) {
                held[i] = moved;
                known |= 1u << i;
            }
        }
    }
    PASS();
}

SUITE(gx_shadow_suite) {
    RUN_TEST(tev_plan_sends_the_fields_that_differ);
    RUN_TEST(batches_cut_by_state_and_tokens);
    RUN_TEST(tokens_put_off_to_a_cut);
    RUN_TEST(a_fence_token_waits_for_a_cut_or_a_poll);
    RUN_TEST(texture_binds_write_what_differs);
    RUN_TEST(texture_tlut_register);
    RUN_TEST(texture_regions);
    RUN_TEST(arrays_sent_when_they_differ);
    RUN_TEST(maps_as_a_cache);
    RUN_TEST(every_token_followed_by_the_flush);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gx_shadow_suite);
    GREATEST_MAIN_END();
}
