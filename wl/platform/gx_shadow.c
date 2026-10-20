// SPDX-License-Identifier: MIT
// The GX state last sent by the direct layer and the TEV (gx_shadow.h).
#include "wl/platform/gx_shadow.h"

#include <string.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/platform.h"

// (stages compared as bytes: no padding in them)
_Static_assert(sizeof(WlTevHalf) == 9u && sizeof(WlTevStage) == 7u + 2u * sizeof(WlTevHalf), "WlTevStage has padding");

// the GX calls each key's setter makes, and the bytes they put in the FIFO (the projection and the
// viewport XF loads of 7 and 6 words, the scissor two BP writes, the colour and alpha updates two, the
// cull mode its share of the generator mode at the next primitive, the others one BP write)
static const uint8_t k_calls[WL_GX_SHADOW_KEYS] = {1, 1, 1, 1, 1, 1, 1, 2, 1};
static const uint8_t k_bytes[WL_GX_SHADOW_KEYS] = {33, 29, 10, 5, 5, 5, 5, 10, 5};

#define BP_BYTES 5u
#define CP_BYTES 6u
#define XF_BYTES(words) (5u + 4u * (words))

static struct {
    uint8_t values[WL_GX_SHADOW_KEYS][WL_GX_SHADOW_VALUE_MAX];
    uint16_t known; // bit per key
    // the TEV as the last apply left it: each stage's fields (bit per stage of stages_known), the
    // konst colours, preloads and swap tables (bit per slot), the stage count
    WlTevStage stages[WL_TEV_MAX_STAGES];
    uint16_t stages_known;
    uint32_t konst[WL_TEV_KONST_SLOTS];
    uint32_t preload[WL_TEV_REG_COUNT];
    uint8_t swap[WL_TEV_SWAP_TABLES][4];
    uint8_t konst_known, preload_known, swaps_known;
    uint8_t stage_count;
    bool stage_count_known;
    uint32_t calls, sent;
    // the TLUT slots' palettes as loaded (bit per slot of tluts_known)
    uint8_t tlut[WL_GX_SHADOW_TLUT_SLOTS][WL_GX_SHADOW_TLUT_BYTES];
    uint16_t tlut_bytes[WL_GX_SHADOW_TLUT_SLOTS];
    uint8_t tlut_format[WL_GX_SHADOW_TLUT_SLOTS];
    uint16_t tluts_known;
    uint32_t tlut_calls, tlut_sent;
    // each map's texture registers as the last bind wrote them (bit per map of texmaps_known), with
    // the TLUT's when it had one (texmap_count 7)
    uint32_t texmap[WL_GX_SHADOW_TEXMAPS][WL_GX_TEXREGS];
    uint8_t texmap_count[WL_GX_SHADOW_TEXMAPS];
    uint8_t texmaps_known;
    uint32_t tex_binds, tex_changed, tex_loads, tex_writes;
    // the arrays' bases and strides as sent (bit per attribute of arrays_known)
    const void *array[WL_GX_SHADOW_ARRAYS];
    uint32_t array_stride[WL_GX_SHADOW_ARRAYS];
    uint16_t arrays_known;
    uint32_t array_calls, array_sent;
    uint64_t fifo[WL_GX_FIFO_KINDS];
    // Dolphin's batches, a model: a primitive after a write that changed GX's state (any kind but the
    // arrays') starts a new one (Dolphin flushes its vertices into a draw of the host's GPU there); the
    // kinds written since the last primitive (bit per kind), the batches, and for each kind the batches
    // it started and those it started alone
    uint32_t dirty;
    uint32_t primitives, batches;
    uint32_t breaks[WL_GX_FIFO_KINDS], alone[WL_GX_FIFO_KINDS];
    // the tokens (a batch's start too: TOKEN_DIRTY in dirty), whether a command reached the FIFO since the last
    uint32_t tokens, tokens_idle, token_breaks, token_alone, tex_invalidates;
    bool written;
    // a fence's token put off (wl_gx_shadow_token_defer), and the primitives since
    bool token_deferred;
    uint32_t token_late;
    // texture maps as a cache (gx_shadow.h): the map each logical map reads, as map ^ logical (zero: itself),
    // each map's texture (its key, bit per map of keys_known), when it was last bound (a count of binds),
    // the logical maps bound since all were forgotten, whether one moved since the TEV followed, the switch
    uint8_t map_x[WL_GX_SHADOW_TEXMAPS];
    uint32_t map_key[WL_GX_SHADOW_TEXMAPS][WL_GX_MAP_KEY_WORDS];
    uint32_t map_stamp[WL_GX_SHADOW_TEXMAPS], stamp;
    uint8_t keys_known, bound;
    bool maps_moved, maps_fixed;
    uint32_t map_hits, map_orders;
} s;

#define TOKEN_DIRTY (1u << WL_GX_FIFO_KINDS)

static void fifo_add(WlGxFifoKind kind, uint32_t bytes) {
    s.fifo[kind] += bytes;
    s.written = s.written || bytes != 0;
    if (kind == WL_GX_FIFO_VERTEX) {
        s.primitives++;
        if (s.dirty != 0) {
            unsigned k;

            s.batches++;
            for (k = 0; k < WL_GX_FIFO_KINDS; k++) {
                if (s.dirty & (1u << k)) {
                    s.breaks[k]++;
                    s.alone[k] += s.dirty == (1u << k) ? 1u : 0u;
                }
            }
            s.token_breaks += (s.dirty & TOKEN_DIRTY) != 0 ? 1u : 0u;
            s.token_alone += s.dirty == TOKEN_DIRTY ? 1u : 0u;
            s.dirty = 0;
        }
    } else if (kind != WL_GX_FIFO_ARRAY && bytes != 0) {
        s.dirty |= 1u << kind;
    }
}

bool wl_gx_shadow_differs(WlGxShadowKey key, const void *value, size_t bytes) {
    uint16_t bit = (uint16_t)(1u << key);

    s.calls += k_calls[key];
    if ((s.known & bit) != 0 && memcmp(s.values[key], value, bytes) == 0) {
        return false;
    }
    memcpy(s.values[key], value, bytes);
    s.known |= bit;
    s.sent += k_calls[key];
    fifo_add(WL_GX_FIFO_STATE, k_bytes[key]);
    return true;
}

static bool same_half(const WlTevHalf *a, const WlTevHalf *b) {
    return a->op == b->op && a->bias == b->bias && a->scale == b->scale && a->clamp == b->clamp && a->dest == b->dest;
}

// the calls a stage's fields need, against the stage GX holds (`known`)
static uint8_t stage_calls(const WlTevStage *now, const WlTevStage *was, bool known) {
    uint8_t send = 0;

    if (!known) {
        return WL_TEV_SEND_STAGE;
    }
    // (the common case first: the stage as it was, its bytes compared at once)
    if (memcmp(now, was, sizeof *now) == 0) {
        return 0;
    }
    if (now->texcoord != was->texcoord || now->texmap != was->texmap || now->channel != was->channel) {
        send |= WL_TEV_SEND_ORDER;
    }
    if (now->ras_swap != was->ras_swap || now->tex_swap != was->tex_swap) {
        send |= WL_TEV_SEND_SWAP;
    }
    if (now->kcsel != was->kcsel) {
        send |= WL_TEV_SEND_KCSEL;
    }
    if (now->kasel != was->kasel) {
        send |= WL_TEV_SEND_KASEL;
    }
    if (memcmp(now->color.in, was->color.in, sizeof now->color.in) != 0) {
        send |= WL_TEV_SEND_COLOR_IN;
    }
    if (!same_half(&now->color, &was->color)) {
        send |= WL_TEV_SEND_COLOR_OP;
    }
    if (memcmp(now->alpha.in, was->alpha.in, sizeof now->alpha.in) != 0) {
        send |= WL_TEV_SEND_ALPHA_IN;
    }
    if (!same_half(&now->alpha, &was->alpha)) {
        send |= WL_TEV_SEND_ALPHA_OP;
    }
    return send;
}

// (in place: the Wii's processor has no instruction for it, and __builtin_popcount is libgcc's call)
static unsigned bits(uint32_t v) {
    v -= (v >> 1) & 0x55555555u;
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (unsigned)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}

void wl_tev_shadow_plan(const WlTevProgram *p, uint32_t flags, WlTevSend *out) {
    bool changes = (flags & WL_TEV_APPLY_CHANGES) != 0;
    uint32_t swaps_used = 0;
    unsigned i, calls = 0, sent = 0;

    memset(out, 0, sizeof *out);
    for (i = 0; i < p->stage_count; i++) {
        swaps_used |= (1u << p->stages[i].ras_swap) | (1u << p->stages[i].tex_swap);
    }
    if ((flags & WL_TEV_APPLY_SKIP_SWAPS) != 0) {
        s.swaps_known = 0; // (the caller's: set behind the shadow)
    } else {
        for (i = 0; i < WL_TEV_SWAP_TABLES; i++) {
            if ((swaps_used & (1u << i)) == 0) {
                continue;
            }
            calls++;
            if (!changes || (s.swaps_known & (1u << i)) == 0 || memcmp(s.swap[i], p->swap[i], 4) != 0) {
                out->swaps |= (uint8_t)(1u << i);
                memcpy(s.swap[i], p->swap[i], 4);
                s.swaps_known |= (uint8_t)(1u << i);
            }
        }
    }
    for (i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        if ((p->konst_mask & (1u << i)) == 0) {
            continue;
        }
        calls++;
        if (!changes || (s.konst_known & (1u << i)) == 0 || s.konst[i] != p->konst[i]) {
            out->konst |= (uint8_t)(1u << i);
            s.konst[i] = p->konst[i];
            s.konst_known |= (uint8_t)(1u << i);
        }
    }
    for (i = 0; i < WL_TEV_REG_COUNT; i++) {
        if ((p->preload_mask & (1u << i)) == 0) {
            continue;
        }
        calls++;
        if (!changes || (s.preload_known & (1u << i)) == 0 || s.preload[i] != p->preload[i]) {
            out->preload |= (uint8_t)(1u << i);
            s.preload[i] = p->preload[i];
            s.preload_known |= (uint8_t)(1u << i);
        }
    }
    calls++;
    if (!changes || !s.stage_count_known || s.stage_count != p->stage_count) {
        out->stage_count = true;
        s.stage_count = p->stage_count;
        s.stage_count_known = true;
    }
    for (i = 0; i < p->stage_count; i++) {
        const WlTevStage *now = &p->stages[i];
        WlTevStage moved;

        if (now->texmap < WL_GX_SHADOW_TEXMAPS && s.map_x[now->texmap] != 0) {
            // (the map its texture went to: gx_shadow.h's cache)
            moved = *now;
            moved.texmap = (uint8_t)(now->texmap ^ s.map_x[now->texmap]);
            now = &moved;
        }
        calls += 8u;
        out->stages[i] = changes ? stage_calls(now, &s.stages[i], (s.stages_known & (1u << i)) != 0)
                                 : (uint8_t)WL_TEV_SEND_STAGE;
        s.stages[i] = *now;
        s.stages_known |= (uint16_t)(1u << i);
        sent += bits(out->stages[i]);
    }
    s.maps_moved = false; // (the stages read the maps their textures are on)
    sent += bits(out->swaps) + bits(out->konst) + bits(out->preload) + (out->stage_count ? 1u : 0u);
    s.calls += calls;
    s.sent += sent;
    // (a stage's call one BP write; a swap table and a konst colour two, a preloaded register four:
    // GX_SetTevColor writes its second half three times)
    fifo_add(WL_GX_FIFO_TEV, BP_BYTES * (sent + bits(out->swaps) + bits(out->konst) + 3u * bits(out->preload)));
}

bool wl_gx_shadow_tlut_differs(uint32_t slot, const void *entries, uint32_t bytes, uint8_t format) {
    s.tlut_calls++;
    if (slot >= WL_GX_SHADOW_TLUT_SLOTS || bytes > WL_GX_SHADOW_TLUT_BYTES) {
        s.tlut_sent++;
        return true;
    }
    uint16_t bit = (uint16_t)(1u << slot);
    if ((s.tluts_known & bit) != 0 && s.tlut_bytes[slot] == bytes && s.tlut_format[slot] == format &&
        memcmp(s.tlut[slot], entries, bytes) == 0) {
        return false;
    }
    memcpy(s.tlut[slot], entries, bytes);
    s.tlut_bytes[slot] = (uint16_t)bytes;
    s.tlut_format[slot] = format;
    s.tluts_known |= bit;
    s.tlut_sent++;
    fifo_add(WL_GX_FIFO_TLUT, 4u * BP_BYTES); // (GX_LoadTlut's BP writes, around two texture-state flushes)
    return true;
}

void wl_gx_shadow_tlut_forget(uint32_t slot) {
    s.tluts_known &= slot >= WL_GX_SHADOW_TLUT_SLOTS ? (uint16_t)0 : (uint16_t)~(1u << slot);
}

void wl_gx_shadow_tlut_counts(uint32_t *calls, uint32_t *sent) {
    *calls = s.tlut_calls;
    *sent = s.tlut_sent;
}

uint32_t wl_gx_shadow_texture(uint32_t map, const uint32_t *regs, unsigned count, bool whole) {
    uint32_t answer = 0, *held;
    unsigned i;

    s.tex_binds++;
    if (map >= WL_GX_SHADOW_TEXMAPS || count > WL_GX_TEXREGS) {
        s.tex_changed++;
        s.tex_loads++;
        s.tex_writes += count;
        return WL_GX_TEXREG_LOAD | ((1u << count) - 1u);
    }
    held = s.texmap[map];
    if ((s.texmaps_known & (1u << map)) == 0 || s.texmap_count[map] != count) {
        answer = WL_GX_TEXREG_LOAD | ((1u << count) - 1u);
    } else {
        for (i = 0; i < count; i++) {
            answer |= held[i] != regs[i] ? 1u << i : 0u;
        }
        if (answer == 0 && !whole) {
            return 0;
        }
        if (whole || (answer & (1u << WL_GX_TEXREG_MODE0 | 1u << WL_GX_TEXREG_IMAGE0)) != 0) {
            answer = WL_GX_TEXREG_LOAD | ((1u << count) - 1u);
        }
    }
    memcpy(held, regs, count * sizeof *regs);
    s.texmap_count[map] = (uint8_t)count;
    s.texmaps_known |= (uint8_t)(1u << map);
    s.tex_changed++;
    s.tex_writes += bits(answer & 0xFFu);
    // (a whole load: libogc sends the coordinates' scale again at the next primitive, two BP writes for
    // the coordinate that reads the map, as a rule one)
    fifo_add(WL_GX_FIFO_TEXTURE,
             BP_BYTES * bits(answer & 0xFFu) + ((answer & WL_GX_TEXREG_LOAD) != 0 ? 2u * BP_BYTES : 0u));
    if ((answer & WL_GX_TEXREG_LOAD) != 0) {
        s.tex_loads++;
    }
    return answer;
}

void wl_gx_shadow_texture_kept(void) {
    s.tex_binds++;
}

bool wl_gx_shadow_texture_forget(uint32_t maps) {
    bool moved = false;
    unsigned m;

    s.texmaps_known &= (uint8_t)~maps;
    s.keys_known &= (uint8_t)~maps;
    if ((maps & 0xFFu) == 0xFFu) {
        s.bound = 0;
    }
    for (m = 0; m < WL_GX_SHADOW_TEXMAPS; m++) {
        if ((maps & (1u << m)) != 0 && s.map_x[m] != 0) {
            s.map_x[m] = 0; // (wl/platform's own draws read the map they loaded)
            moved = true;
        }
    }
    s.maps_moved = s.maps_moved || moved;
    return moved;
}

// ---- texture maps as a cache --------------------------------------------------------------------

void wl_gx_shadow_maps_cache(bool on) {
    if (s.maps_fixed != !on) {
        s.maps_fixed = !on;
        (void)wl_gx_shadow_texture_forget(0xFFu);
    }
}

bool wl_gx_shadow_maps_cache_on(void) {
    return !s.maps_fixed;
}

void wl_gx_shadow_maps_begin(void) {
    s.bound = 0;
}

// the maps a logical map's texture may go to: maps 0-5, and its own (maps 6 and 7's regions overlap maps 4
// and 5's in the console's texture memory: gx_direct_wii.c, wl_gx_wii_regions_apply)
#define CACHE_MAPS 0x3Fu

static bool map_holds(unsigned m, const uint32_t *key) {
    return (s.keys_known & (1u << m)) != 0 && memcmp(s.map_key[m], key, WL_GX_MAP_KEY_WORDS * sizeof *key) == 0;
}

uint32_t wl_gx_shadow_map_bind(uint32_t logical, const uint32_t key[WL_GX_MAP_KEY_WORDS], bool *held) {
    unsigned here, m, best = WL_GX_SHADOW_TEXMAPS, l;
    uint32_t pinned = 0, best_age = 0;

    *held = false;
    if (s.maps_fixed || logical >= WL_GX_SHADOW_TEXMAPS) {
        return logical;
    }
    here = logical ^ s.map_x[logical];
    s.stamp++;
    s.bound |= (uint8_t)(1u << logical);
    if (map_holds(here, key)) {
        s.map_stamp[here] = s.stamp;
        *held = true;
        return here;
    }
    for (m = 0; m < WL_GX_SHADOW_TEXMAPS; m++) {
        if (m != here && ((CACHE_MAPS | 1u << logical) & (1u << m)) != 0 && map_holds(m, key)) {
            // (another map holds it: read there, nothing loaded)
            best = m;
            s.map_hits++;
            *held = true;
            break;
        }
    }
    if (best == WL_GX_SHADOW_TEXMAPS) {
        // (the maps the other logical maps read are kept; of the others the one never given a texture or
        // forgotten, else the one bound longest ago, `here` first among equals)
        pinned = ~(CACHE_MAPS | 1u << logical);
        for (l = 0; l < WL_GX_SHADOW_TEXMAPS; l++) {
            if (l != logical && (s.bound & (1u << l)) != 0) {
                pinned |= 1u << (l ^ s.map_x[l]);
            }
        }
        for (m = 0; m < WL_GX_SHADOW_TEXMAPS; m++) {
            // (binds since its last; one holding nothing the oldest)
            uint32_t age = (s.keys_known & (1u << m)) == 0 ? UINT32_MAX : s.stamp - s.map_stamp[m];

            if ((pinned & (1u << m)) != 0) {
                continue;
            }
            if (best == WL_GX_SHADOW_TEXMAPS || age > best_age || (age == best_age && m == here)) {
                best = m;
                best_age = age;
            }
        }
        if (best == WL_GX_SHADOW_TEXMAPS) {
            // (every map another logical map reads: seven of them bound and their separate alphas: its own
            // map, the logical maps that read it to be bound again)
            best = here;
            for (l = 0; l < WL_GX_SHADOW_TEXMAPS; l++) {
                if (l != logical && (l ^ s.map_x[l]) == best) {
                    s.bound &= (uint8_t)~(1u << l);
                }
            }
        }
        memcpy(s.map_key[best], key, WL_GX_MAP_KEY_WORDS * sizeof *key);
        s.keys_known |= (uint8_t)(1u << best);
    }
    s.map_stamp[best] = s.stamp;
    if (best != here) {
        s.map_x[logical] = (uint8_t)(best ^ logical);
        s.maps_moved = true;
    }
    return best;
}

uint32_t wl_gx_shadow_map_of(uint32_t logical) {
    return logical < WL_GX_SHADOW_TEXMAPS ? logical ^ s.map_x[logical] : logical;
}

bool wl_gx_shadow_maps_moved(void) {
    return s.maps_moved;
}

void wl_gx_shadow_maps_followed(void) {
    s.maps_moved = false;
}

bool wl_tev_shadow_order_moved(unsigned stage, uint8_t texcoord, uint8_t texmap, uint8_t channel) {
    WlTevStage *was;
    bool known;

    if (stage >= WL_TEV_MAX_STAGES) {
        return false;
    }
    was = &s.stages[stage];
    known = (s.stages_known & (1u << stage)) != 0;
    if (known && was->texmap == texmap && was->texcoord == texcoord && was->channel == channel) {
        return false;
    }
    if (known) {
        was->texcoord = texcoord;
        was->texmap = texmap;
        was->channel = channel;
    }
    s.map_orders++;
    s.calls++;
    s.sent++;
    fifo_add(WL_GX_FIFO_TEV, BP_BYTES);
    return true;
}

void wl_gx_shadow_texture_counts(uint32_t *binds, uint32_t *changed, uint32_t *loads, uint32_t *writes) {
    *binds = s.tex_binds;
    *changed = s.tex_changed;
    *loads = s.tex_loads;
    *writes = s.tex_writes;
}

void wl_gx_shadow_texture_model(uint32_t regs[WL_GX_TEXREGS], uintptr_t address, uint32_t format, uint32_t width,
                                uint32_t height, uint32_t mips, uint32_t wrap_s, uint32_t wrap_t, uint32_t min_filter,
                                uint32_t mag_filter, float lod_bias, uint32_t tlut_slot, uint32_t region) {
    int32_t bias;

    // (GX_InitTexObjLODBias's: clamped to -4 .. 3.99, in 32nds, eight bits)
    if (!(lod_bias == lod_bias)) {
        lod_bias = 0.0f;
    }
    lod_bias = lod_bias < -4.0f ? -4.0f : lod_bias >= 4.0f ? 3.99f : lod_bias;
    bias = (int32_t)(32.0f * lod_bias);
    regs[WL_GX_TEXREG_MODE0] = (wrap_s & 3u) | (wrap_t & 3u) << 2 | (mag_filter & 1u) << 4 | (min_filter & 7u) << 5 |
                               ((uint32_t)bias & 0xFFu) << 9;
    regs[WL_GX_TEXREG_MODE1] = mips;
    regs[WL_GX_TEXREG_IMAGE0] = (width - 1u) | (height - 1u) << 10 | format << 20;
    regs[WL_GX_TEXREG_EVEN] = region + 1u;
    regs[WL_GX_TEXREG_ODD] = region + 1u;
    regs[WL_GX_TEXREG_IMAGE3] = (uint32_t)(address >> 5);
    regs[WL_GX_TEXREG_TLUT] = tlut_slot;
}

uint32_t wl_gx_shadow_texture_region(uint32_t format, uint32_t mips, bool regions_libogc) {
    bool ci = format == WL_GX_TEX_C4 || format == WL_GX_TEX_C8, cmpr = format == WL_GX_TEX_CMPR;

    if (!regions_libogc) {
        // (the even bank alone for colour-indexed textures and CMPR without mipmaps)
        return ci || (cmpr && mips <= 1u) ? 1u : 0u;
    }
    // (libogc's three sets: colour-indexed and CMPR without mipmaps, the others without, mipmapped)
    return ci ? 0u : mips > 1u ? 2u : cmpr ? 0u : 1u;
}

bool wl_gx_shadow_array(unsigned attribute, const void *data, uint32_t stride, bool always) {
    s.array_calls++;
    if (attribute < WL_GX_SHADOW_ARRAYS) {
        uint16_t bit = (uint16_t)(1u << attribute);

        if (!always && (s.arrays_known & bit) != 0 && s.array[attribute] == data && s.array_stride[attribute] == stride) {
            return false;
        }
        s.array[attribute] = data;
        s.array_stride[attribute] = stride;
        s.arrays_known |= bit;
    }
    s.array_sent++;
    fifo_add(WL_GX_FIFO_ARRAY, 2u * CP_BYTES);
    return true;
}

void wl_gx_shadow_array_forget(void) {
    s.arrays_known = 0;
}

void wl_gx_shadow_array_counts(uint32_t *calls, uint32_t *sent) {
    *calls = s.array_calls;
    *sent = s.array_sent;
}

void wl_gx_shadow_fifo(WlGxFifoKind kind, uint32_t bytes) {
    fifo_add(kind, bytes);
}

void wl_gx_shadow_fifo_counts(uint64_t *out) {
    memcpy(out, s.fifo, sizeof s.fifo);
}

uint32_t wl_gx_shadow_layout_bytes(unsigned texgens, unsigned chans, bool lit) {
    // (the descriptor's two CP writes and the transform unit's vertex spec, the format's three CP
    // writes, each generator's two XF registers, the counts' XF writes and the generator mode, the
    // channels' controls: two, or four when lit)
    (void)chans;
    return 2u * CP_BYTES + XF_BYTES(1u) + 3u * CP_BYTES + texgens * 2u * XF_BYTES(1u) + 2u * XF_BYTES(1u) + BP_BYTES +
           (lit ? 4u : 2u) * XF_BYTES(1u);
}

uint32_t wl_gx_shadow_layout_vertex_bytes(const WlGxDirectLayout *layout) {
    // (the attributes the vertex sends, as the Wii's layout lists them, with their words: the position,
    // the normal of a lit layout, the projected coordinates' normal (or normal, binormal and tangent), the
    // colours not taken from the register, each coordinate sent that is not projected; an indexed one sends two bytes)
    uint32_t words[16] = {0}, bytes = 0, projected = bits(layout->texcoords_projected), n;

    words[WL_GX_DIRECT_ARRAY_POSITION] = 3;
    words[WL_GX_DIRECT_ARRAY_NORMAL] = layout->lit && projected == 0 ? 3u : 0u;
    // (a lit layout's normal with projected coordinates: the first of the normal, binormal and tangent)
    projected += layout->lit && projected != 0 ? 1u : 0u;
    words[WL_GX_DIRECT_ARRAY_COLOR0] = layout->colors >= 1 && !(layout->colors_register & 1u) ? 1u : 0u;
    words[WL_GX_DIRECT_ARRAY_COLOR0 + 1] = layout->colors >= 2 && !(layout->colors_register & 2u) ? 1u : 0u;
    for (n = 0; n < 8; n++) {
        if ((layout->texcoords & (uint8_t)~layout->texcoords_projected) & (1u << n)) {
            words[WL_GX_DIRECT_ARRAY_TEXCOORD0 + n] = (layout->texcoord_s & (1u << n)) ? 1u : 2u;
        }
    }
    for (n = 0; n < 16; n++) {
        bytes += words[n] == 0 ? 0u : (layout->indexed & (1u << n)) ? 2u : 4u * words[n];
    }
    // (the normal, binormal and tangent that carry them: three or nine floats, or one index for all)
    if (projected != 0 && (layout->indexed & (1u << WL_GX_DIRECT_ARRAY_NORMAL))) {
        return bytes + 2u;
    }
    return bytes + 4u * (projected == 0 ? 0u : projected == 1u ? 3u : 9u);
}

void wl_gx_shadow_token(void) {
    s.token_deferred = false;
    s.tokens++;
    s.tokens_idle += s.written ? 0u : 1u;
    s.written = false;
    s.dirty |= TOKEN_DIRTY;
}

void wl_gx_shadow_token_defer(void) {
    if (!s.token_deferred) {
        s.token_deferred = true;
        s.token_late = 0;
    }
}

bool wl_gx_shadow_token_deferred(void) {
    return s.token_deferred;
}

bool wl_gx_shadow_token_due(void) {
    // (its batch cut by the state given since the last primitive already, or put off long enough)
    return s.token_deferred && (s.dirty != 0 || ++s.token_late >= WL_GX_SHADOW_TOKEN_LATE);
}

void wl_gx_shadow_texture_invalidate(void) {
    s.tex_invalidates++;
    fifo_add(WL_GX_FIFO_TEXTURE, 4u * BP_BYTES);
}

void wl_gx_shadow_lighting(const WlGxDirectLighting *l) {
    unsigned extra = bits((uint32_t)(l->extra[0] | l->extra[1] | l->extra[2] | l->extra[3]) & 0x0Fu);

    fifo_add(WL_GX_FIFO_MATRIX, XF_BYTES(9u) + (4u + extra) * XF_BYTES(16u) + 4u * XF_BYTES(1u));
}

void wl_gx_shadow_forget(void) {
    s.known = 0;
    s.arrays_known = 0;
    s.stages_known = 0;
    s.konst_known = s.preload_known = s.swaps_known = 0;
    s.stage_count_known = false;
}

void wl_gx_shadow_counts(uint32_t *calls, uint32_t *sent) {
    *calls = s.calls;
    *sent = s.sent;
}

_Static_assert(WL_GX_FIFO_KINDS == sizeof(((WlGxDirectStats *)0)->fifo) / sizeof(uint64_t),
               "WlGxDirectStats.fifo holds each kind");

void wl_gx_shadow_stats(WlGxDirectStats *out) {
    wl_gx_shadow_counts(&out->state_calls, &out->state_sent);
    wl_gx_shadow_tlut_counts(&out->tlut_calls, &out->tlut_sent);
    wl_gx_shadow_texture_counts(&out->tex_binds, &out->tex_changed, &out->tex_loads, &out->tex_writes);
    wl_gx_shadow_array_counts(&out->array_calls, &out->array_sent);
    wl_gx_shadow_fifo_counts(out->fifo);
    out->tokens = s.tokens;
    out->tokens_idle = s.tokens_idle;
    out->token_breaks = s.token_breaks;
    out->token_alone = s.token_alone;
    out->tex_invalidates = s.tex_invalidates;
    out->tex_map_hits = s.map_hits;
    out->tex_map_orders = s.map_orders;
    out->primitives = s.primitives;
    out->batches = s.batches;
    memcpy(out->batch_breaks, s.breaks, sizeof s.breaks);
    memcpy(out->batch_alone, s.alone, sizeof s.alone);
}
