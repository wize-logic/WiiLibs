// SPDX-License-Identifier: MIT
// The GX state gx_direct.h's setters and wl_tev_apply last sent, so that a setter given the value GX
// holds already sends nothing (the TEV's only with WL_TEV_APPLY_CHANGES). The same bookkeeping on
// the console and the host, where nothing is skipped but the calls are counted alike
// (WlGxDirectStats.state_calls / state_sent). Forgotten wherever GX may have been set behind it:
// wl_gx_invalidate_shadow (and so wl_gx_direct_begin), and wl_gx's own shadowed setters.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/platform/tev.h"

typedef enum {
    WL_GX_SHADOW_PROJECTION = 0,
    WL_GX_SHADOW_VIEWPORT,
    WL_GX_SHADOW_SCISSOR,
    WL_GX_SHADOW_BLEND,
    WL_GX_SHADOW_ZMODE,
    WL_GX_SHADOW_ZEARLY,
    WL_GX_SHADOW_ALPHA_COMPARE,
    WL_GX_SHADOW_COLOR_UPDATE, // (two calls: the colour's and the alpha's)
    WL_GX_SHADOW_CULL,
    WL_GX_SHADOW_KEYS
} WlGxShadowKey;

// the largest value a key keeps (a projection: 4 x 4 floats and its kind)
#define WL_GX_SHADOW_VALUE_MAX 68u

// Whether `value` (`bytes` <= WL_GX_SHADOW_VALUE_MAX, no padding in it) is not what `key` last sent,
// or nothing is known: then remembered, and the setter sends it. false: GX holds it already.
bool wl_gx_shadow_differs(WlGxShadowKey key, const void *value, size_t bytes);

// What wl_tev_apply_unchecked_ex sends of a program: per stage, the calls (WL_TEV_SEND_*); the
// konst colours, preloaded registers and swap tables by mask; the stage count.
enum {
    WL_TEV_SEND_ORDER = 1u << 0,
    WL_TEV_SEND_SWAP = 1u << 1,
    WL_TEV_SEND_KCSEL = 1u << 2,
    WL_TEV_SEND_KASEL = 1u << 3,
    WL_TEV_SEND_COLOR_IN = 1u << 4,
    WL_TEV_SEND_COLOR_OP = 1u << 5,
    WL_TEV_SEND_ALPHA_IN = 1u << 6,
    WL_TEV_SEND_ALPHA_OP = 1u << 7,
    WL_TEV_SEND_STAGE = 0xFFu
};
typedef struct {
    uint8_t stages[WL_TEV_MAX_STAGES];
    uint8_t konst, preload, swaps;
    bool stage_count;
} WlTevSend;
// The calls for `p` (flags: wl_tev_apply's; with WL_TEV_APPLY_CHANGES only those GX does not hold
// already), the shadow then holding `p`. `p` is valid (wl_tev_validate).
void wl_tev_shadow_plan(const WlTevProgram *p, uint32_t flags, WlTevSend *out);

// The TLUT slots (palettes in GX's texture memory, 256 entries each), compared by their bytes: whether
// `bytes` (<= WL_GX_SHADOW_TLUT_BYTES) of `entries` in `format` are not what `slot` holds, or nothing
// is known (then remembered, and the load is sent); false: the slot holds that palette already, whatever
// memory it was loaded from. A slot >= WL_GX_SHADOW_TLUT_SLOTS is always sent. Forgotten by
// wl_gx_shadow_tlut_forget (not by wl_gx_shadow_forget: GX's setters leave the slots as they are) wherever a
// slot may be loaded behind it: wl_gx_tlut_load, wl_gx_tlut_invalidate, wl_gx_init and a change of the
// texture memory's regions (the quads of the targets load no TLUT, so wl_gx_invalidate_shadow keeps them).
#define WL_GX_SHADOW_TLUT_SLOTS 16u
#define WL_GX_SHADOW_TLUT_BYTES 512u
bool wl_gx_shadow_tlut_differs(uint32_t slot, const void *entries, uint32_t bytes, uint8_t format);
// Slot `slot` forgotten (WL_GX_SHADOW_TLUT_SLOTS or more: all of them).
void wl_gx_shadow_tlut_forget(uint32_t slot);
// The TLUT loads asked for since the start, and of those the ones sent.
void wl_gx_shadow_tlut_counts(uint32_t *calls, uint32_t *sent);

// ---- texture maps: the registers a bind writes ----------------------------------------------------
// GX_LoadTexObj writes six BP registers for a map (its mode 0 and 1, image 0: the size and format, the
// cache region's even and odd halves, image 3: the address) and a seventh for a colour-indexed texture
// (its TLUT's), whatever the map held; libogc then sets the texture coordinates' scale again at the next
// primitive (two more a coordinate). Here each map's last registers are kept: a bind writes only those
// that differ, unless the mode or the size and format do (libogc keeps those two for the coordinates'
// scale: then the whole load, GX_LoadTexObj's), or nothing is known of the map.
enum {
    WL_GX_TEXREG_MODE0 = 0,
    WL_GX_TEXREG_MODE1,
    WL_GX_TEXREG_IMAGE0,
    WL_GX_TEXREG_EVEN,
    WL_GX_TEXREG_ODD,
    WL_GX_TEXREG_IMAGE3,
    WL_GX_TEXREG_TLUT,
    WL_GX_TEXREGS
};
#define WL_GX_TEXREG_LOAD 0x100u // (in wl_gx_shadow_texture's answer: the whole load)
#define WL_GX_SHADOW_TEXMAPS 8u
// Map `map`'s bind of `regs` (`count`: 6, or 7 with the TLUT's): 0 when the map holds them all already;
// WL_GX_TEXREG_LOAD | the registers when the whole load is needed (nothing known, the mode or image 0
// changed, or `whole`: the switch gxtexload, every bind loaded whole as before); else bit n for each
// register n to write. The shadow then holds `regs`. Counted (binds, writes).
uint32_t wl_gx_shadow_texture(uint32_t map, const uint32_t *regs, unsigned count, bool whole);
// A bind counted that its caller found the map holding already (the same object as its last bind).
void wl_gx_shadow_texture_kept(void);
// The maps of the mask `maps` forgotten (set behind the shadow: wl/platform's quads, the ramp, ...): with
// the cache, those maps are their own logical maps again (wl/platform's own draws read the map they load),
// and all of them (0xFF) frees every map. Whether a logical map moved (the TEV to follow it at once:
// wl/platform's quads may have applied it before their load).
bool wl_gx_shadow_texture_forget(uint32_t maps);
// The binds asked for since the start, the ones that wrote something, of those the whole loads, and
// the BP writes they made.
void wl_gx_shadow_texture_counts(uint32_t *binds, uint32_t *changed, uint32_t *loads, uint32_t *writes);
// A host's stand-in for a bind's registers (the host has no libogc): numbers that are equal exactly where
// the console's registers are: `address` the image's, `format` WlGxTexFormat, `mips` its levels, the
// wraps and filters as bound (after the clamps of a size not a power of two and of no mipmaps), the LOD
// bias the object holds, `tlut_slot` a colour-indexed texture's, `region` wl_gx_shadow_texture_region's.
void wl_gx_shadow_texture_model(uint32_t regs[WL_GX_TEXREGS], uintptr_t address, uint32_t format, uint32_t width,
                                uint32_t height, uint32_t mips, uint32_t wrap_s, uint32_t wrap_t, uint32_t min_filter,
                                uint32_t mag_filter, float lod_bias, uint32_t tlut_slot, uint32_t region);
// The region set the console's callback (wl_gx_wii_regions_apply) picks for a texture: 0, 1 or 2.
uint32_t wl_gx_shadow_texture_region(uint32_t format, uint32_t mips, bool regions_libogc);

// ---- texture maps as a cache ------------------------------------------------------------------------
// A bind (wl_gx_direct_texture) names the map its TEV stages read (the logical map). With the cache on (the
// default; the switch gxmapsfixed turns it off), GX's eight maps keep their last textures: the bind goes to
// whichever map holds that texture already, or else to the map bound longest ago (one never given a texture,
// or forgotten, first) that no other logical map reads, and the TEV's stages read the map it went to
// (wl_tev_shadow_plan's orders, and tev_wii.c's at the next primitive when only the binds moved). A texture
// is known by `key`: its registers that do not depend on the map (mode 0 and 1, image 0 and 3, its TLUT and
// region set), equal exactly where the registers are. Off, every map is its logical map, as before.
#define WL_GX_MAP_KEY_WORDS 5u
void wl_gx_shadow_maps_cache(bool on);
bool wl_gx_shadow_maps_cache_on(void);
// wl_gx_direct_begin's: the maps keep their textures (wl/platform's own loads forget the maps they load), and
// no logical map is read until it is bound again (its map free for the others').
void wl_gx_shadow_maps_begin(void);
// Logical map `logical`'s bind of the texture `key`: the map it goes to; *held: that map holds it already
// (nothing to load: counted kept), else the caller loads it there (the key is remembered). Off: `logical`,
// *held false.
uint32_t wl_gx_shadow_map_bind(uint32_t logical, const uint32_t key[WL_GX_MAP_KEY_WORDS], bool *held);
// The map logical map `logical` (< 8) reads (itself when the cache is off).
uint32_t wl_gx_shadow_map_of(uint32_t logical);
// Whether a logical map has moved to another map since the TEV last followed (wl_gx_shadow_maps_followed).
bool wl_gx_shadow_maps_moved(void);
void wl_gx_shadow_maps_followed(void);
// TEV stage `stage`'s order moved to map `texmap` (its coordinate and channel as applied): whether it is to
// be sent (the stage's map not known to be `texmap`); the shadow then holds it. Counted as a TEV write.
bool wl_tev_shadow_order_moved(unsigned stage, uint8_t texcoord, uint8_t texmap, uint8_t channel);
// (each backend's tev_*.c) The last program applied made to read the maps its logical maps read now, where
// one moved since (wl_gx_shadow_maps_moved): the direct layer's primitives call it, and a forget that moved one.
void wl_tev_maps_follow(void);

// ---- the vertex arrays: GX_SetArray's base and stride (two CP writes) ------------------------------
// Whether attribute `attribute`'s (< WL_GX_SHADOW_ARRAYS) array is not what GX holds, or nothing is known:
// then remembered (sent); false: GX holds it (`always`, the switch gxarrayall: sent all the same).
// Forgotten by wl_gx_shadow_forget and wl_gx_shadow_array_forget (GX_SetArray called behind it).
// Counted (calls, sent). (GX's vertex cache is not emptied by a new base: wl_gx_direct_arrays_changed.)
#define WL_GX_SHADOW_ARRAYS 12u
bool wl_gx_shadow_array(unsigned attribute, const void *data, uint32_t stride, bool always);
void wl_gx_shadow_array_forget(void);
void wl_gx_shadow_array_counts(uint32_t *calls, uint32_t *sent);

// ---- the FIFO's bytes, as the console's calls write them (a model counted alike on the host) -------
// BP and CP writes 5 and 6 bytes, an XF load of n words 5 + 4n; libogc's lazy state (the vertex
// descriptor, the formats, the generators) as its next primitive sends it.
typedef enum {
    WL_GX_FIFO_STATE = 0, // the setters' (wl_gx_shadow_differs)
    WL_GX_FIFO_TEV,       // wl_tev_apply's
    WL_GX_FIFO_TEXTURE,   // the binds' (and the coordinates' scale a whole load sends again)
    WL_GX_FIFO_TLUT,      // the TLUT loads'
    WL_GX_FIFO_ARRAY,     // the arrays'
    WL_GX_FIFO_MATRIX,    // the position and texture matrices' and the generators'
    WL_GX_FIFO_LAYOUT,    // the vertex layouts' (descriptor, format, generators, channels)
    WL_GX_FIFO_VERTEX,    // the primitives' (their command, vertices, the transform unit's flush)
    WL_GX_FIFO_KINDS
} WlGxFifoKind;
void wl_gx_shadow_fifo(WlGxFifoKind kind, uint32_t bytes);
// The bytes of each kind since the start (`out` WL_GX_FIFO_KINDS of them).
void wl_gx_shadow_fifo_counts(uint64_t *out);
// The bytes a layout change sends: `texgens` coordinates generated, `chans` colour channels, `lit`
// channels lit (four controls, not two).
uint32_t wl_gx_shadow_layout_bytes(unsigned texgens, unsigned chans, bool lit);
// the position matrix's XF load (12 words), a texture matrix's (8), a generator's two XF registers
#define WL_GX_FIFO_POSITION_BYTES 53u
#define WL_GX_FIFO_TEXMTX_BYTES 37u
#define WL_GX_FIFO_TEXGEN_BYTES 18u
// a channel's material colour (one XF register)
#define WL_GX_FIFO_MATERIAL_BYTES 9u
// The bytes a vertex of `layout` sends: an index for each attribute read from an array, the others' words
// (counted once a primitive: begin's 3 bytes and its vertices, not at each vertex).
struct WlGxDirectLayout;
uint32_t wl_gx_shadow_layout_vertex_bytes(const struct WlGxDirectLayout *layout);

// ---- the lighting's loads ------------------------------------------------------------------------
// wl_gx_direct_lighting's loads of a changed lighting counted (WL_GX_FIFO_MATRIX): the normal matrix (an XF load
// of 9 words), lights 0-3 and the extra ones a channel adds (16 words each), the channels' ambient colours and
// their materials made white (an XF register each). (Halo's change all at once: its lights' directions are the
// model's, its ambients each draw's, so they are not shadowed part by part.)
struct WlGxDirectLighting;
void wl_gx_shadow_lighting(const struct WlGxDirectLighting *lighting);

// ---- the tokens and the texture cache's invalidations ----------------------------------------------
// A draw-sync token sent (wl_gx_fence, wl_gx_sync, a frame's end: two BP writes): counted, and whether anything
// reached the FIFO since the token before it. GX's texture cache invalidated (GX_InvalidateTexAll: four BP
// writes). Dolphin flushes its batch at both, whatever their values: a batch of draws of the same state is cut
// in two by a token between them.
void wl_gx_shadow_token(void);
void wl_gx_shadow_texture_invalidate(void);
// A fence's token put off (wl_gx_fence; not the switch gxfenceeager): sent where Dolphin cuts its batch anyway,
// before the next primitive after a change of GX's state, or before the WL_GX_SHADOW_TOKEN_LATE'th primitive
// at the latest (wl_gx_shadow_token_due), or when the fence is asked about or a token is sent for another
// reason. The fence is the next token's number: GX past it is past everything given before the fence.
#define WL_GX_SHADOW_TOKEN_LATE 8u
void wl_gx_shadow_token_defer(void);
// Whether a token is put off.
bool wl_gx_shadow_token_deferred(void);
// Before a primitive (its state given, its command not): whether the token put off is to be sent now.
bool wl_gx_shadow_token_due(void);

// Everything forgotten: the next value of each key is sent.
void wl_gx_shadow_forget(void);
// The GX calls asked for since the start, and of those the ones sent.
void wl_gx_shadow_counts(uint32_t *calls, uint32_t *sent);
// The counts above into the direct layer's stats (wl_gx_direct_stats).
struct WlGxDirectStats;
void wl_gx_shadow_stats(struct WlGxDirectStats *out);
