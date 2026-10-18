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
// The maps of the mask `maps` forgotten (set behind the shadow: wl/platform's quads, the ramp, ...).
void wl_gx_shadow_texture_forget(uint32_t maps);
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

// Everything forgotten: the next value of each key is sent.
void wl_gx_shadow_forget(void);
// The GX calls asked for since the start, and of those the ones sent.
void wl_gx_shadow_counts(uint32_t *calls, uint32_t *sent);
// The counts above into the direct layer's stats (wl_gx_direct_stats).
struct WlGxDirectStats;
void wl_gx_shadow_stats(struct WlGxDirectStats *out);
