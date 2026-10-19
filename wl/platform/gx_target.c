// SPDX-License-Identifier: MIT
// Render targets on GX's one EFB (gx_target.h): who owns the EFB, when the back buffer is parked and
// put back, when a target's pixels are copied out. Portable: the GPU's part is the backend's
// (gx_target_internal.h).
#include <stdlib.h>
#include <string.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_target_internal.h"

#define TAG "TARGET"

static struct {
    bool size_set;
    uint32_t back_w, back_h;
    WlGxTargetAlloc alloc;
    WlGxTargetFree release;
    // the surface the EFB holds: a target (the copy of its descriptor), or the back buffer
    bool owner_is_target;
    WlGxTarget owner;
    WlGxTarget resolved; // the last one wl_gx_target_use returned
    bool drawn;          // the back buffer has had something drawn or cleared this frame
    bool parked;         // its colour and depth are in `colour` and `depth`, (parked_x, 0) to (+parked_w, parked_h)
    bool depth_parked;   // ...its depth too (always, unless lazy_depth: then from wl_gx_target_depth_write)
    bool lazy_depth;     // wl_gx_target_set_lazy_depth
    bool park_whole;     // wl_gx_target_set_park_whole
    bool squeeze_warned; // the lack of memory for wl_gx_target_squeeze has been told
    uint32_t parked_x, parked_w, parked_h;
    // the part of the EFB a target can draw over that the park covers, from the EFB's origin (the
    // whole back buffer for a whole park): a larger target has it parked again
    uint32_t cover_w, cover_h;
    // the picture's columns (wl_gx_target_set_picture; pic_w 0: all of the back buffer), and whether
    // its bars may hold anything but black (a target drew there, the frame's clear was not black)
    uint32_t pic_x, pic_w;
    bool bars_dirty;
    bool bars_hidden; // wl_gx_target_set_bars_shown(false): the columns beside the picture are not shown
    // the part of it the targets since the park can have drawn over: their scissors' bounds (a target draws
    // within its scissor), all a restore puts back (the switch restore_whole: the part parked)
    uint32_t spill_w, spill_h;
    void *colour, *depth;
    size_t colour_bytes, depth_bytes; // the blocks' sizes
    // the bytes of each block from its start that the GPU has been copied into: the processor does not
    // touch the blocks, so what a copy has invalidated stays out of its cache
    size_t colour_clean, depth_clean;
    WlGxTargetStats stats;
} t;

static void *default_alloc(size_t bytes) {
    return aligned_alloc(32, (bytes + 31u) & ~(size_t)31u);
}

static void default_free(void *memory) {
    free(memory);
}

static void free_parked(void) {
    WlGxTargetFree release = t.release != NULL ? t.release : default_free;
    if (t.colour != NULL || t.depth != NULL) {
        (void)wl_gx_sync(); // (the GPU may still copy into them or read them: frame overlap)
    }
    if (t.colour != NULL) {
        release(t.colour);
    }
    if (t.depth != NULL) {
        release(t.depth);
    }
    t.colour = t.depth = NULL;
    t.colour_bytes = t.depth_bytes = 0;
    t.colour_clean = t.depth_clean = 0;
    t.parked = false;
}

// A block of at least `bytes` in *block: the one there, or a new one (the old given back). False,
// and *block NULL, when the allocator has none.
static bool block_at_least(void **block, size_t *size, size_t *clean, size_t bytes) {
    WlGxTargetAlloc alloc = t.alloc != NULL ? t.alloc : default_alloc;
    WlGxTargetFree release = t.release != NULL ? t.release : default_free;
    if (*block != NULL && *size >= bytes) {
        return true;
    }
    if (*block != NULL) {
        (void)wl_gx_sync(); // (the GPU may still copy into it or read it: frame overlap)
        release(*block);
    }
    *block = alloc(bytes);
    *size = *block != NULL ? bytes : 0;
    *clean = 0;
    return *block != NULL;
}

static void back_size(uint32_t *w, uint32_t *h) {
    if (!t.size_set) {
        wl_gx_target_be_default_size(&t.back_w, &t.back_h);
        t.size_set = true;
    }
    *w = t.back_w;
    *h = t.back_h;
}

void wl_gx_target_set_back_buffer_size(uint32_t width, uint32_t height) {
    if (t.size_set && t.back_w == width && t.back_h == height) {
        return;
    }
    free_parked();
    t.back_w = width;
    t.back_h = height;
    t.size_set = true;
}

void wl_gx_target_set_allocator(WlGxTargetAlloc alloc, WlGxTargetFree release) {
    free_parked();
    t.alloc = alloc;
    t.release = release;
}

size_t wl_gx_target_bytes(const WlGxTarget *target) {
    if (target == NULL || (target->format != WL_GX_TEX_RGBA8 && target->format != WL_GX_TEX_RGB565)) {
        return 0;
    }
    return wl_gx_tex_bytes((WlGxTexFormat)target->format, target->width, target->height);
}

bool wl_gx_target_resolvable(const WlGxTarget *target, uint32_t back_w, uint32_t back_h) {
    return target != NULL && target->memory != NULL && ((uintptr_t)target->memory & 31u) == 0 &&
           wl_gx_target_bytes(target) != 0 && target->width != 0 && target->height != 0 &&
           target->width <= back_w && target->height <= back_h && (target->width & 3u) == 0 &&
           (target->height & 3u) == 0;
}

void wl_gx_target_scissor_size(const WlGxTarget *target, uint32_t back_w, uint32_t back_h, uint32_t *w, uint32_t *h) {
    *w = target->width < back_w ? target->width : back_w;
    *h = target->height < back_h ? target->height : back_h;
}

void wl_gx_target_frame_begun(uint32_t clear_rgba) {
    t.owner_is_target = false;
    t.drawn = false;
    t.parked = false;
    t.depth_parked = false;
    // (the bars are cleared with the rest of the EFB: to the frame's colour)
    t.bars_dirty = t.bars_dirty || (clear_rgba >> 8) != 0;
}

// the picture's columns: [*x, *x + *w) of a back buffer `bw` wide
static void picture_columns(uint32_t bw, uint32_t *x, uint32_t *w) {
    if (t.pic_w == 0u || t.pic_x + t.pic_w > bw) {
        *x = 0;
        *w = bw;
    } else {
        *x = t.pic_x;
        *w = t.pic_w;
    }
}

void wl_gx_target_set_picture(uint32_t x, uint32_t w) {
    // (from an even column, whole tiles wide: GX's copy rectangle)
    t.pic_x = w != 0u ? x & ~1u : 0u;
    t.pic_w = w != 0u ? (w + 3u) & ~3u : 0u;
    t.bars_dirty = true;
}

void wl_gx_target_set_bars_shown(bool shown) {
    t.bars_hidden = !shown;
}

bool wl_gx_target_picture(uint32_t *x, uint32_t *w) {
    uint32_t bw, bh;
    back_size(&bw, &bh);
    picture_columns(bw, x, w);
    return *w < bw;
}

// The bars of a picture drawn narrow blackened when something may have drawn over them (at the end of
// the frame, once the back buffer has the EFB again)
static void bars_blacken(void) {
    uint32_t bw, bh, x, w;
    back_size(&bw, &bh);
    picture_columns(bw, &x, &w);
    if (w >= bw || !t.bars_dirty || t.bars_hidden) {
        return;
    }
    wl_gx_target_be_scissor(0, bw, bh);
    if (x != 0u) {
        wl_gx_target_be_clear(true, false, 0x000000FFu, 1.0f, 0, 0, (int)x, (int)bh, bw, bh);
    }
    if (x + w < bw) {
        wl_gx_target_be_clear(true, false, 0x000000FFu, 1.0f, (int)(x + w), 0, (int)(bw - x - w), (int)bh, bw, bh);
    }
    t.bars_dirty = false;
    t.stats.bar_clears++;
}

void wl_gx_target_shutdown(void) {
    free_parked();
    memset(&t.owner, 0, sizeof t.owner);
    t.owner_is_target = false;
    t.drawn = false;
}

// The part of the EFB a target of scissor `w` by `h` can draw over (from the EFB's origin), which a
// park covers: in whole 4 by 4 tiles of the copies, within the back buffer.
static void park_size(uint32_t w, uint32_t h, uint32_t *cw, uint32_t *ch) {
    uint32_t bw, bh;
    back_size(&bw, &bh);
    if (t.park_whole) {
        w = bw;
        h = bh;
    }
    w = (w + 3u) & ~3u;
    h = (h + 3u) & ~3u;
    *cw = w < bw ? w : bw;
    *ch = h < bh ? h : bh;
}

// The back buffer's colour and depth where targets of `cw` by `ch` from the EFB's origin can draw over
// its picture (park_size; the picture's columns: wl_gx_target_set_picture), out of the EFB, into blocks
// asked for at the first park and again, larger, when a park needs more (with lazy depth, the depth's
// at its first copy). Targets over the bars alone park nothing.
static void park(uint32_t cw, uint32_t ch) {
    uint32_t bw, bh, x, pw, w, h = ch;
    back_size(&bw, &bh);
    picture_columns(bw, &x, &pw);
    w = cw > x ? cw - x : 0u;
    w = w < pw ? (w + 3u) & ~3u : pw;
    w = x + w > bw ? bw - x : w;
    t.cover_w = cw;
    t.cover_h = ch;
    if (w == 0u || h == 0u) {
        t.parked = true;
        t.depth_parked = true; // (nothing of the picture to keep)
        t.parked_x = x;
        t.parked_w = t.parked_h = 0;
        t.spill_w = t.spill_h = 0;
        t.stats.bars_parks++;
        return;
    }
    size_t bytes = wl_gx_tex_bytes(WL_GX_TEX_RGBA8, w, h);
    // (the colour's block may be there already, and larger: wl_gx_target_squeeze's)
    if (!block_at_least(&t.colour, &t.colour_bytes, &t.colour_clean, bytes) ||
        (!t.lazy_depth && !block_at_least(&t.depth, &t.depth_bytes, &t.depth_clean, bytes))) {
        free_parked();
        WL_LOG_W(TAG, "no memory to park the back buffer (%u bytes twice): a target's draws go over it",
                 (unsigned)bytes);
        return;
    }
    wl_gx_target_be_copy_begin();
    wl_gx_target_be_copy(t.colour, x, w, h, WL_GX_TARGET_COPY_RGBA8, t.colour_clean < bytes);
    t.stats.park_bytes += bytes;
    if (!t.lazy_depth) {
        wl_gx_target_be_copy(t.depth, x, w, h, WL_GX_TARGET_COPY_Z24X8, t.depth_clean < bytes);
        t.depth_clean = t.depth_clean > bytes ? t.depth_clean : bytes;
        t.stats.depths++;
        t.stats.depth_bytes += bytes;
    }
    t.colour_clean = t.colour_clean > bytes ? t.colour_clean : bytes;
    wl_gx_target_be_copy_end();
    t.parked = true;
    t.depth_parked = !t.lazy_depth;
    t.parked_x = x;
    t.parked_w = w;
    t.parked_h = h;
    t.spill_w = t.spill_h = 0;
    t.stats.parks++;
}

// The back buffer put back over a target's pixels, if it was parked: the part the targets drew over, within
// a scissor of its size (a park of the whole back buffer, wl_gx_target_set_park_whole, is 640 by 480 where
// the targets drew 64 by 64 of it: the restore's quad no longer fills and textures the rest, which holds
// the back buffer still), or with the switch restore_whole the part parked.
static void restore(void) {
    uint32_t w, h, rw, rh;
    back_size(&w, &h);
    if (!t.parked) {
        wl_gx_target_be_scissor(0, w, h);
        return;
    }
    rw = t.parked_w;
    rh = t.parked_h;
    if (!wl_gx_bisect()->restore_whole) {
        // (the targets drew over columns [0, spill_w): of the part parked, from its column parked_x)
        uint32_t spill = t.spill_w > t.parked_x ? t.spill_w - t.parked_x : 0u;
        rw = spill < rw ? spill : rw;
        rh = t.spill_h < rh ? t.spill_h : rh;
    }
    // (the quad over the part parked, within the scissor of the part to put back: the back buffer's
    // when that is all of it)
    bool all = rw == t.parked_w && rh == t.parked_h;
    wl_gx_target_be_scissor(all ? 0u : t.parked_x, all ? w : rw, all ? h : rh);
    if (rw != 0 && rh != 0) {
        // (a depth not parked is the back buffer's still: no target wrote it)
        wl_gx_target_be_restore(t.colour, t.depth_parked ? t.depth : NULL, t.parked_x, t.parked_w, t.parked_h, w,
                                h);
        t.stats.restored_pixels += (uint64_t)rw * rh;
        t.stats.restores++;
    }
    if (!all) {
        wl_gx_target_be_scissor(0, w, h);
    }
    t.parked = false;
    t.depth_parked = false;
}

// The owning target's pixels into its memory; the target when it was copied.
static const WlGxTarget *resolve(void) {
    uint32_t w, h;
    back_size(&w, &h);
    if (!wl_gx_target_resolvable(&t.owner, w, h)) {
        t.stats.skipped++;
        return NULL;
    }
    wl_gx_target_be_copy_begin();
    wl_gx_target_be_copy(t.owner.memory, 0, t.owner.width, t.owner.height,
                         t.owner.format == WL_GX_TEX_RGB565 ? WL_GX_TARGET_COPY_RGB565 : WL_GX_TARGET_COPY_RGBA8,
                         !t.owner.resident);
    wl_gx_target_be_copy_end();
    t.stats.resolves++;
    t.stats.resolve_bytes += wl_gx_target_bytes(&t.owner);
    t.resolved = t.owner;
    return &t.resolved;
}

static const void *key_of(const WlGxTarget *target) {
    return target->id != NULL ? target->id : target->memory;
}

const WlGxTarget *wl_gx_target_use(const WlGxTarget *target) {
    const WlGxTarget *resolved = NULL;
    if (target == NULL ? !t.owner_is_target : (t.owner_is_target && key_of(&t.owner) == key_of(target))) {
        if (target != NULL) {
            t.owner = *target; // (the descriptor as the caller has it now)
        }
        return NULL;
    }
    uint32_t w, h, sw = 0, sh = 0, pw, ph;
    back_size(&w, &h);
    if (target != NULL) {
        wl_gx_target_scissor_size(target, w, h, &sw, &sh);
    }
    // the one leaving (the back buffer leaves only for a target)
    if (!t.owner_is_target) {
        if (t.drawn && !t.parked) {
            park_size(sw, sh, &pw, &ph);
            park(pw, ph);
        }
    } else {
        resolved = resolve();
    }
    // the one coming
    if (target == NULL) {
        restore();
        t.owner_is_target = false;
    } else {
        park_size(sw, sh, &pw, &ph);
        if (t.parked && (pw > t.cover_w || ph > t.cover_h)) {
            // a target larger than the part parked (or one with whole parks asked for since the
            // park): the back buffer put back (the last target's pixels are resolved), then parked
            // again, the larger part
            park_size(pw > t.cover_w ? pw : t.cover_w, ph > t.cover_h ? ph : t.cover_h, &pw, &ph);
            restore();
            park(pw, ph);
        }
        wl_gx_target_be_scissor(0, sw, sh);
        t.bars_dirty = true;
        t.spill_w = sw > t.spill_w ? sw : t.spill_w;
        t.spill_h = sh > t.spill_h ? sh : t.spill_h;
        t.owner = *target;
        t.owner_is_target = true;
    }
    return resolved;
}

const WlGxTarget *wl_gx_target_current(void) {
    return t.owner_is_target ? &t.owner : NULL;
}

const void *wl_gx_target_parked_colour(uint32_t *w, uint32_t *h) {
    if (!t.parked || !t.owner_is_target || t.colour == NULL || t.parked_w == 0u) {
        return NULL;
    }
    *w = t.parked_w;
    *h = t.parked_h;
    return t.colour;
}

void wl_gx_target_set_lazy_depth(bool on) {
    t.lazy_depth = on;
}

void wl_gx_target_set_park_whole(bool on) {
    t.park_whole = on;
}

void wl_gx_target_depth_write(void) {
    size_t bytes;
    if (!t.owner_is_target || !t.parked || t.depth_parked) {
        return;
    }
    // (the EFB's depth there is still the back buffer's, copied before the draw that is to write it,
    // into a block asked for now)
    bytes = wl_gx_tex_bytes(WL_GX_TEX_RGBA8, t.parked_w, t.parked_h);
    if (!block_at_least(&t.depth, &t.depth_bytes, &t.depth_clean, bytes)) {
        WL_LOG_W(TAG, "no memory to park the back buffer's depth (%u bytes): a target's draws go over it",
                 (unsigned)bytes);
        return;
    }
    wl_gx_target_be_copy_begin();
    wl_gx_target_be_copy(t.depth, t.parked_x, t.parked_w, t.parked_h, WL_GX_TARGET_COPY_Z24X8, t.depth_clean < bytes);
    wl_gx_target_be_copy_end();
    t.depth_clean = t.depth_clean > bytes ? t.depth_clean : bytes;
    t.depth_parked = true;
    t.stats.depths++;
    t.stats.depth_bytes += bytes;
}

void wl_gx_target_drawn(void) {
    if (!t.owner_is_target) {
        t.drawn = true;
    }
}

void wl_gx_target_frame_ending(void) {
    if (t.owner_is_target) {
        (void)wl_gx_target_use(NULL);
    }
    bars_blacken();
}

void wl_gx_target_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int w, int h) {
    uint32_t bw, bh;
    back_size(&bw, &bh);
    z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    if (depth) {
        wl_gx_target_depth_write();
    }
    wl_gx_target_be_clear(colour, depth, rgba, z, x, y, w, h, bw, bh);
    t.stats.clears++;
    t.stats.clear_pixels += (uint64_t)(w > 0 ? w : 0) * (uint64_t)(h > 0 ? h : 0);
}

bool wl_gx_target_squeeze(uint32_t x, uint32_t w) {
    uint32_t bw, bh;
    back_size(&bw, &bh);
    if (w == 0u || x + w > bw) {
        return false;
    }
    // (the block a park copies the colour into, free at the end of a frame: made as large as the
    // back buffer where a park's is smaller)
    size_t squeeze_bytes = wl_gx_tex_bytes(WL_GX_TEX_RGBA8, bw, bh);
    if (!block_at_least(&t.colour, &t.colour_bytes, &t.colour_clean, squeeze_bytes)) {
        if (!t.squeeze_warned) {
            t.squeeze_warned = true;
            WL_LOG_W(TAG, "no memory to narrow the picture (%u bytes): it is shown the full width",
                     (unsigned)wl_gx_tex_bytes(WL_GX_TEX_RGBA8, bw, bh));
        }
        return false;
    }
    t.parked = false;
    t.depth_parked = false;
    wl_gx_target_be_squeeze(t.colour, bw, bh, x, w, t.colour_clean < squeeze_bytes);
    t.colour_clean = t.colour_clean > squeeze_bytes ? t.colour_clean : squeeze_bytes;
    t.stats.squeezes++;
    t.stats.squeeze_bytes += squeeze_bytes;
    return true;
}

void wl_gx_target_stats(WlGxTargetStats *out, bool reset) {
    *out = t.stats;
    out->held = t.colour_bytes + t.depth_bytes;
    if (reset) {
        memset(&t.stats, 0, sizeof t.stats);
    }
}
