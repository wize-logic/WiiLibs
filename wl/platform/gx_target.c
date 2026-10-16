// SPDX-License-Identifier: MIT
// Render targets on GX's one EFB (gx_target.h): who owns the EFB, when the back buffer is parked and
// put back, when a target's pixels are copied out. Portable: the GPU's part is the backend's
// (gx_target_internal.h).
#include <stdlib.h>
#include <string.h>

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
    bool parked;         // its colour and depth are in `colour` and `depth`, (0, 0) to (parked_w, parked_h)
    bool squeeze_warned; // the lack of memory for wl_gx_target_squeeze has been told
    uint32_t parked_w, parked_h;
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

void wl_gx_target_frame_begun(void) {
    t.owner_is_target = false;
    t.drawn = false;
    t.parked = false;
}

void wl_gx_target_shutdown(void) {
    free_parked();
    memset(&t.owner, 0, sizeof t.owner);
    t.owner_is_target = false;
    t.drawn = false;
}

// The part of the back buffer a target of scissor `w` by `h` can draw over, which a park keeps: from
// the EFB's origin, in whole 4 by 4 tiles of the copies, within the back buffer.
static void park_size(uint32_t w, uint32_t h, uint32_t *pw, uint32_t *ph) {
    uint32_t bw, bh;
    back_size(&bw, &bh);
    w = (w + 3u) & ~3u;
    h = (h + 3u) & ~3u;
    *pw = w < bw ? w : bw;
    *ph = h < bh ? h : bh;
}

// The back buffer's colour and depth, `w` by `h` from the EFB's origin (what the targets coming can
// draw over: park_size), out of the EFB, into blocks asked for at the first park and again, larger,
// when a park needs more.
static void park(uint32_t w, uint32_t h) {
    size_t bytes = wl_gx_tex_bytes(WL_GX_TEX_RGBA8, w, h);
    // (the colour's block may be there already, and larger: wl_gx_target_squeeze's)
    if (!block_at_least(&t.colour, &t.colour_bytes, &t.colour_clean, bytes) ||
        !block_at_least(&t.depth, &t.depth_bytes, &t.depth_clean, bytes)) {
        free_parked();
        WL_LOG_W(TAG, "no memory to park the back buffer (%u bytes twice): a target's draws go over it",
                 (unsigned)bytes);
        return;
    }
    wl_gx_target_be_copy_begin();
    wl_gx_target_be_copy(t.colour, w, h, WL_GX_TARGET_COPY_RGBA8, t.colour_clean < bytes);
    wl_gx_target_be_copy(t.depth, w, h, WL_GX_TARGET_COPY_Z24X8, t.depth_clean < bytes);
    t.colour_clean = t.colour_clean > bytes ? t.colour_clean : bytes;
    t.depth_clean = t.depth_clean > bytes ? t.depth_clean : bytes;
    wl_gx_target_be_copy_end();
    t.parked = true;
    t.parked_w = w;
    t.parked_h = h;
    t.stats.parks++;
}

// The back buffer put back over a target's pixels, if it was parked: the part parked.
static void restore(void) {
    uint32_t w, h;
    back_size(&w, &h);
    wl_gx_target_be_scissor(w, h);
    if (!t.parked) {
        return;
    }
    wl_gx_target_be_restore(t.colour, t.depth, t.parked_w, t.parked_h, w, h);
    t.parked = false;
    t.stats.restores++;
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
    wl_gx_target_be_copy(t.owner.memory, t.owner.width, t.owner.height,
                         t.owner.format == WL_GX_TEX_RGB565 ? WL_GX_TARGET_COPY_RGB565 : WL_GX_TARGET_COPY_RGBA8,
                         !t.owner.resident);
    wl_gx_target_be_copy_end();
    t.stats.resolves++;
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
        if (t.parked && (sw > t.parked_w || sh > t.parked_h)) {
            // a target larger than the part parked: the back buffer put back (the last target's
            // pixels are resolved), then parked again, the larger part
            park_size(sw > t.parked_w ? sw : t.parked_w, sh > t.parked_h ? sh : t.parked_h, &pw, &ph);
            restore();
            park(pw, ph);
        }
        wl_gx_target_be_scissor(sw, sh);
        t.owner = *target;
        t.owner_is_target = true;
    }
    return resolved;
}

const WlGxTarget *wl_gx_target_current(void) {
    return t.owner_is_target ? &t.owner : NULL;
}

const void *wl_gx_target_parked_colour(uint32_t *w, uint32_t *h) {
    if (!t.parked || !t.owner_is_target || t.colour == NULL) {
        return NULL;
    }
    *w = t.parked_w;
    *h = t.parked_h;
    return t.colour;
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
}

void wl_gx_target_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int w, int h) {
    uint32_t bw, bh;
    back_size(&bw, &bh);
    z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    wl_gx_target_be_clear(colour, depth, rgba, z, x, y, w, h, bw, bh);
    t.stats.clears++;
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
    wl_gx_target_be_squeeze(t.colour, bw, bh, x, w, t.colour_clean < squeeze_bytes);
    t.colour_clean = t.colour_clean > squeeze_bytes ? t.colour_clean : squeeze_bytes;
    t.stats.squeezes++;
    return true;
}

void wl_gx_target_stats(WlGxTargetStats *out, bool reset) {
    *out = t.stats;
    if (reset) {
        memset(&t.stats, 0, sizeof t.stats);
    }
}
