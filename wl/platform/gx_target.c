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
    bool parked;         // its colour and depth are in `colour` and `depth`
    bool squeeze_warned; // the lack of memory for wl_gx_target_squeeze has been told
    void *colour, *depth;
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
    if (t.colour != NULL) {
        release(t.colour);
    }
    if (t.depth != NULL) {
        release(t.depth);
    }
    t.colour = t.depth = NULL;
    t.parked = false;
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

// The back buffer's colour and depth out of the EFB, into blocks asked for at the first park.
static void park(void) {
    uint32_t w, h;
    back_size(&w, &h);
    if (t.colour == NULL || t.depth == NULL) {
        size_t bytes = wl_gx_tex_bytes(WL_GX_TEX_RGBA8, w, h);
        WlGxTargetAlloc alloc = t.alloc != NULL ? t.alloc : default_alloc;
        WlGxTargetFree release = t.release != NULL ? t.release : default_free;
        // (the colour's block may be there already: wl_gx_target_squeeze's)
        if (t.colour == NULL) {
            t.colour = alloc(bytes);
        }
        if (t.depth == NULL) {
            t.depth = alloc(bytes);
        }
        if (t.colour == NULL || t.depth == NULL) {
            if (t.colour != NULL) {
                release(t.colour);
            }
            if (t.depth != NULL) {
                release(t.depth);
            }
            t.colour = t.depth = NULL;
            WL_LOG_W(TAG, "no memory to park the back buffer (%u bytes twice): a target's draws go over it",
                     (unsigned)bytes);
            return;
        }
    }
    wl_gx_target_be_copy_begin();
    wl_gx_target_be_copy(t.colour, w, h, WL_GX_TARGET_COPY_RGBA8);
    wl_gx_target_be_copy(t.depth, w, h, WL_GX_TARGET_COPY_Z24X8);
    wl_gx_target_be_copy_end();
    t.parked = true;
    t.stats.parks++;
}

// The back buffer put back over a target's pixels, if it was parked.
static void restore(void) {
    uint32_t w, h;
    back_size(&w, &h);
    wl_gx_target_be_scissor(w, h);
    if (!t.parked) {
        return;
    }
    wl_gx_target_be_restore(t.colour, t.depth, w, h);
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
                         t.owner.format == WL_GX_TEX_RGB565 ? WL_GX_TARGET_COPY_RGB565 : WL_GX_TARGET_COPY_RGBA8);
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
    // the one leaving
    if (!t.owner_is_target) {
        if (t.drawn && !t.parked) {
            park();
        }
    } else {
        resolved = resolve();
    }
    // the one coming
    if (target == NULL) {
        restore();
        t.owner_is_target = false;
    } else {
        uint32_t w, h, sw, sh;
        back_size(&w, &h);
        wl_gx_target_scissor_size(target, w, h, &sw, &sh);
        wl_gx_target_be_scissor(sw, sh);
        t.owner = *target;
        t.owner_is_target = true;
    }
    return resolved;
}

const WlGxTarget *wl_gx_target_current(void) {
    return t.owner_is_target ? &t.owner : NULL;
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
    if (t.colour == NULL) {
        // (the block a park would copy the colour into: free at the end of a frame)
        WlGxTargetAlloc alloc = t.alloc != NULL ? t.alloc : default_alloc;
        t.colour = alloc(wl_gx_tex_bytes(WL_GX_TEX_RGBA8, bw, bh));
        if (t.colour == NULL) {
            if (!t.squeeze_warned) {
                t.squeeze_warned = true;
                WL_LOG_W(TAG, "no memory to narrow the picture (%u bytes): it is shown the full width",
                         (unsigned)wl_gx_tex_bytes(WL_GX_TEX_RGBA8, bw, bh));
            }
            return false;
        }
    }
    t.parked = false;
    wl_gx_target_be_squeeze(t.colour, bw, bh, x, w);
    t.stats.squeezes++;
    return true;
}

void wl_gx_target_stats(WlGxTargetStats *out, bool reset) {
    *out = t.stats;
    if (reset) {
        memset(&t.stats, 0, sizeof t.stats);
    }
}
