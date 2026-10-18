// SPDX-License-Identifier: MIT
// The textures a backend has converted into GX's formats (nv2a_texcache.c), kept by what they are
// (their address, format word, size and, for a P8, palette) and checked against the guest's
// texels: a title rewrites some of its textures in place (a glyph cache) and reuses the memory of
// others. Within a memory budget (or an arena of its own): the least recently drawn go first, and
// their images are given back only once the GPU has finished the frame that may still read them
// (wl_nv2a_texcache_frame_done).
//
// The check. A texture is checked at its first use in each frame; one found changed once is
// checked at every use from then on. With a write watch (the configuration's `watch`/`written`:
// the runtime's MMU, whose changed bits say which guest pages were stored to), a texture whose
// pages were not written since it was made is as it was, without its texels read; one whose pages
// were is hashed (a large one is made again). Without a watch, or while a capture runs (it must see
// the texels read), the texels are hashed: a texture of at most WL_NV2A_TEXCACHE_FULL_HASH bytes
// whole, a larger one at spread samples.
//
// Memory. A texture that finds no room even with the images not drawn in the frame given back is
// kept smaller (wl_nv2a_texture_layout_reduced: its largest levels left out, a level at a time, up
// to WL_NV2A_TEXCACHE_REDUCE_MAX) rather than not at all, and made whole again at its first use in
// a later frame (one in WL_NV2A_TEXCACHE_RESTORE_FRAMES) when there is room for it: free memory, or
// the images of textures not drawn for as many frames given back (never one the frames draw now).
//
// Time. With a frame budget (the configuration's `frame_budget`), a frame makes at most that many
// guest bytes of textures at their lookups: a texture new to the cache looked up once the frame
// has made as many is put off (the lookup answers NULL, the backend stands in for it) and kept in a
// short list, made when the processor would only wait for the GPU (wl_nv2a_texcache_work, from the
// backend's end of frame) or else at its first lookup in a later frame, so that a burst of new
// textures (a level's area, a cutscene) is spread over frames instead of stalling one. A texture
// rewritten or kept smaller is never put off (its old image is drawn meanwhile); a capture puts off
// nothing.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/platform/mem_heap.h"
#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_texture.h"

enum {
    WL_NV2A_TEXCACHE_ENTRIES = 512,
    WL_NV2A_TEXCACHE_BUCKETS = 256,
    WL_NV2A_TEXCACHE_PENDING = 256,
    WL_NV2A_TEXCACHE_FULL_HASH = 256 * 1024,
    // textures that found no memory, not tried again in the same frame
    WL_NV2A_TEXCACHE_REFUSED = 16,
    // the decoders' scratch kept between conversions at most (a larger one is had for one)
    WL_NV2A_TEXCACHE_SCRATCH_KEPT = 256 * 1024,
    // a texture kept smaller for want of memory: levels left out at most, and how often it tries
    // to be made whole again
    WL_NV2A_TEXCACHE_REDUCE_MAX = 3,
    WL_NV2A_TEXCACHE_RESTORE_FRAMES = 60,
    // textures put off by the frame budget, kept to be made while the processor waits
    WL_NV2A_TEXCACHE_DEFERRED = 64
};

typedef struct WlNv2aTexCacheConfig {
    // bytes of GX images kept (0: 4 MiB, or the arena's size)
    size_t budget;
    // a light's falloff cube made its lit face (wl_nv2a_texture_spot), for a backend whose texture
    // coordinates read it through wl_nv2a_spot_coordinates (entry->layout.spot); false: every cube
    // map a sphere map
    bool spots;
    // an arena the images are kept in (the cache's alone: a heap of its own over it, so that a
    // texture finds room by evicting others, and nothing else fragments it); NULL: alloc/release
    void *arena;
    size_t arena_bytes;
    // 32-byte aligned memory the GPU reads, and its release (NULL: aligned_alloc and free); the
    // images' without an arena (a backend's other images, render targets, use them either way)
    void *(*alloc)(void *user, size_t bytes);
    void (*release)(void *user, void *memory);
    // an image written by the processor, to memory before the GPU reads it (the Wii's
    // DCFlushRange; NULL: nothing)
    void (*flush)(void *user, void *memory, size_t bytes);
    // optional: the write watch over guest-physical memory. watch() arms it over a range: a stamp
    // (0: the stores to the range cannot be seen); written() says whether part of the range may
    // have been stored to since the stamp was had.
    uint32_t (*watch)(void *user, uint32_t phys, uint32_t bytes);
    bool (*written)(void *user, uint32_t phys, uint32_t bytes, uint32_t stamp);
    void *user;
    // the frames the GPU may still be drawing behind the one finished (1 under
    // wl_gx_set_frame_overlap; 0: each frame drawn by its end): an image drawn in one of them is
    // neither given back nor rewritten in place until the GPU is past it
    uint8_t gpu_lag;
    // optional, with gpu_lag: waits for the GPU to finish what it was given (wl_gx_sync). Called
    // at most once a frame, when an image finds no room while frames the GPU may still be drawing
    // hold the memory (a level's first frames): the images only those frames drew go back then,
    // instead of the new texture being kept smaller.
    void (*gpu_wait)(void);
    // optional: the guest bytes of textures a frame makes at its lookups before new textures are
    // put off to a later frame or the GPU's wait (wl_nv2a_texcache_work); 0: no limit. The texture
    // that crosses it is made whole, so a frame always makes one.
    size_t frame_budget;
} WlNv2aTexCacheConfig;

typedef struct WlNv2aTexEntry {
    // what the texture is
    uint32_t phys, format_word, rect_word, palette_phys;
    uint32_t source_bytes;
    uint64_t hash, palette_hash;
    uint8_t mipmaps; // (a request without mip-maps takes it with them, never converting it twice)
    uint8_t signs;   // WlNv2aTexture.signs
    // what it became
    WlNv2aGxLayout layout;
    void *image; // layout.total_bytes: the image, then its alpha (DXT3/5), then its TLUT (P8)
    uint8_t contents, linear, sphere;
    uint16_t width, height; // the guest texture's (a linear one's coordinates are in its texels)
    // bookkeeping
    uint32_t used_frame, checked_frame;
    uint32_t stamp;  // the write watch's over its texels and palette since it was checked (0: none)
    uint32_t serial; // changes when the image is made anew
    uint8_t dynamic; // found changed once: checked at every use
    uint8_t reduced; // levels left out for want of memory (0: whole)
    uint16_t next;   // in its bucket, + 1
    uint32_t restore_frame; // a reduced one's last try to be made whole
} WlNv2aTexEntry;

typedef struct WlNv2aTexCacheStats {
    uint32_t lookups, hits, converted, reconverted, evicted, failed, hashed;
    uint32_t checks;      // lookups that looked at the texture (not its frame's first check again)
    uint32_t watched;     // of those, answered by the write watch (unchanged, nothing read)
    uint32_t no_memory;   // of the failures, for want of memory
    uint32_t refused_again; // ...lookups of a texture refused so in their frame
    uint32_t unknown;     // ...a format or size GX cannot take
    uint32_t unreadable;  // ...texels outside guest memory
    uint32_t bad_texels;  // ...a conversion that failed
    uint32_t mip_shared;  // lookups without mip-maps answered by the texture with them
    uint32_t mip_replaced; // textures without mip-maps given back when made with them
    uint32_t reduced;     // textures kept smaller for want of memory (converted or made again so)
    uint32_t restored;    // of those, made whole again later
    uint32_t gpu_waits;   // frames whose images waited for the GPU (cfg.gpu_wait) to find room
    uint32_t deferred;    // lookups of a new texture put off by the frame budget (the backend stood in)
    uint32_t made_early;  // of the textures put off, made by wl_nv2a_texcache_work (while the GPU drew)
    uint32_t frames_deferred, deferred_run; // frames that put a texture off, and the most of them in a row
    // the texture work of a frame at its lookups (checks, hashes, conversions; the processor's
    // clock): the longest, its guest bytes made, and the frames that took 20 and 50 ms or more
    uint32_t worst_frame_us, worst_frame_made, frames_over_20ms, frames_over_50ms;
    uint64_t hashed_bytes;
    size_t bytes;         // images kept now
    size_t frame_bytes;   // of those, drawn in the last frame finished
    uint32_t frame_reduced; // textures drawn in the last frame finished kept smaller (detail missing now)
    size_t peak_frame_bytes;
    // the frame with peak_frame_bytes: its bytes by GX format (a C8's TLUT and a DXT3/5's alpha
    // included), and of those a DXT3/5's alpha images
    size_t peak_by_format[WL_GXC_FORMAT_COUNT];
    size_t peak_alpha;
} WlNv2aTexCacheStats;

typedef struct WlNv2aTexCache {
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexEntry entries[WL_NV2A_TEXCACHE_ENTRIES];
    uint16_t buckets[WL_NV2A_TEXCACHE_BUCKETS];
    void *pending[WL_NV2A_TEXCACHE_PENDING];
    uint32_t pending_frame[WL_NV2A_TEXCACHE_PENDING]; // the frame each was given back in
    unsigned pending_count;
    WlHeap heap; // over the arena, when there is one
    struct {
        uint32_t phys, format_word, rect_word, frame;
    } refused[WL_NV2A_TEXCACHE_REFUSED];
    unsigned refused_next;
    uint32_t refused_logged;
    uint32_t unknown_logged[8]; // the format codes logged as unknown (a bit each)
    uint32_t frame, serial;
    uint32_t waited_frame; // the frame cfg.gpu_wait was last called in (the GPU past every frame before it)
    bool capturing; // a capture ran at the last lookup
    WlNv2aTexCacheStats stats;
    // the frame budget's: guest bytes made in this frame, the texture work's time, and the textures
    // put off (made by wl_nv2a_texcache_work, or forgotten at the frame's end)
    size_t frame_made;
    uint32_t frame_us;
    struct {
        WlNv2aTexture texture;
        bool mipmaps;
    } deferred[WL_NV2A_TEXCACHE_DEFERRED];
    unsigned deferred_count;
    bool frame_deferred;  // a texture put off in this frame
    uint32_t deferred_run; // frames in a row that put one off, this one's before it
    bool working; // in wl_nv2a_texcache_work: nothing put off, and no wait for the GPU
    // the decoders' scratch (grown as needed)
    void *scratch;
    size_t scratch_bytes;
} WlNv2aTexCache;

// An empty cache (the caller's: it is large) with its configuration (NULL: the defaults).
// the hash the texels are checked by (XXH32's rounds, in the host's byte order), continued from
// `hash`: the vertex cache's too
uint64_t wl_nv2a_texcache_hash(uint64_t hash, const uint8_t *bytes, size_t count);
void wl_nv2a_texcache_init(WlNv2aTexCache *cache, const WlNv2aTexCacheConfig *cfg);
// Gives every image back (the GPU must be done with them).
void wl_nv2a_texcache_clear(WlNv2aTexCache *cache);
// The GX image of a texture, converted now if it is not kept or its texels changed (*made true
// then), reading its texels through wl_nv2a_phys. NULL when it cannot be shown (its memory not in
// guest RAM, a format or size GX has not, no memory).
const WlNv2aTexEntry *wl_nv2a_texcache_get(WlNv2aTexCache *cache, WlNv2a *dev, const WlNv2aTexture *texture,
                                           bool mipmaps, bool *made);
// A frame is drawn and the GPU has finished it: the images evicted during it are given back,
// and the next frame checks each texture again.
void wl_nv2a_texcache_frame_done(WlNv2aTexCache *cache);
void wl_nv2a_texcache_stats(const WlNv2aTexCache *cache, WlNv2aTexCacheStats *out);
// Makes the next texture the frame budget put off in this frame (between lookups, before
// wl_nv2a_texcache_frame_done: the backend calls it while it waits for the GPU at the frame's end).
// *made true when it made an image (the GPU's texture cache must be invalidated before the next
// draw). Returns whether more are left.
bool wl_nv2a_texcache_work(WlNv2aTexCache *cache, WlNv2a *dev, bool *made);
// Whether guest-physical [phys, phys + bytes) is converted and unchanged: the texels of a texture
// the cache keeps lie on part of it, and every texture kept whose texels lie on part of it is
// under the write watch with no store to its pages since it was checked (and is not kept smaller,
// which is made whole from its texels). Such bytes are not read by the cache again until they are
// stored to or the texture is evicted: a pager may let them go (and read them back when the
// watch's stores, or an eviction's conversion, need them). False without a watch. Asked between
// lookups (the GPU's thread, or under its lock); it walks every entry.
bool wl_nv2a_texcache_converted(const WlNv2aTexCache *cache, uint32_t phys, uint32_t bytes);

// The memory the cache holds, for a census of the host's heap.
typedef struct WlNv2aTexCacheMemory {
    size_t images;        // the images kept (stats.bytes)
    size_t pending;       // images evicted in this frame, given back at its end
    size_t scratch;       // the decoders' scratch (of the heap)
    size_t arena;         // the arena's bytes (0: the images are the heap's)
    size_t arena_used;    // of those, in blocks (headers included)
    size_t arena_largest; // its largest free block (the largest image that fits without evicting)
} WlNv2aTexCacheMemory;
void wl_nv2a_texcache_memory(const WlNv2aTexCache *cache, WlNv2aTexCacheMemory *out);

// Memory for the GPU's other pictures (render targets, the back buffer's park) from a free piece of
// the arena, 32-aligned, no image evicted for it: NULL without an arena or a free piece that large.
// The piece stays the caller's until wl_nv2a_texcache_spare_free (an image that wants room later
// goes around it). The arena is the GPU thread's, as the cache's other calls.
void *wl_nv2a_texcache_spare_alloc(WlNv2aTexCache *cache, size_t bytes);
// Whether `memory` is a piece of the arena (wl_nv2a_texcache_spare_alloc's).
bool wl_nv2a_texcache_spare_owns(const WlNv2aTexCache *cache, const void *memory);
// Gives a piece back; false (nothing done) when `memory` is not the arena's: the caller frees it.
bool wl_nv2a_texcache_spare_free(WlNv2aTexCache *cache, void *memory);

// What the images kept are, for a census of the arena: bytes and count by GX format (a C8's TLUT
// and a DXT3/5's alpha included), the bytes of linear textures (pictures of the screen's size
// among them), the guest's bytes they were made from, and the largest image.
typedef struct WlNv2aTexCacheKinds {
    size_t by_format[WL_GXC_FORMAT_COUNT];
    uint32_t count_by_format[WL_GXC_FORMAT_COUNT];
    size_t linear, source, largest;
} WlNv2aTexCacheKinds;
void wl_nv2a_texcache_kinds(const WlNv2aTexCache *cache, WlNv2aTexCacheKinds *out);
