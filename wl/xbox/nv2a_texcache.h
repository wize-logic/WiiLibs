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
    WL_NV2A_TEXCACHE_REFUSED = 16
};

typedef struct WlNv2aTexCacheConfig {
    // bytes of GX images kept (0: 4 MiB, or the arena's size)
    size_t budget;
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
} WlNv2aTexCacheConfig;

typedef struct WlNv2aTexEntry {
    // what the texture is
    uint32_t phys, format_word, rect_word, palette_phys;
    uint32_t source_bytes;
    uint64_t hash, palette_hash;
    uint8_t mipmaps;
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
    uint16_t next;   // in its bucket, + 1
} WlNv2aTexEntry;

typedef struct WlNv2aTexCacheStats {
    uint32_t lookups, hits, converted, reconverted, evicted, failed, hashed;
    uint32_t checks;      // lookups that looked at the texture (not its frame's first check again)
    uint32_t watched;     // of those, answered by the write watch (unchanged, nothing read)
    uint32_t no_memory;   // of the failures, for want of memory
    uint64_t hashed_bytes;
    size_t bytes;         // images kept now
    size_t frame_bytes;   // of those, drawn in the last frame finished
    size_t peak_frame_bytes;
} WlNv2aTexCacheStats;

typedef struct WlNv2aTexCache {
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexEntry entries[WL_NV2A_TEXCACHE_ENTRIES];
    uint16_t buckets[WL_NV2A_TEXCACHE_BUCKETS];
    void *pending[WL_NV2A_TEXCACHE_PENDING];
    unsigned pending_count;
    WlHeap heap; // over the arena, when there is one
    struct {
        uint32_t phys, format_word, rect_word, frame;
    } refused[WL_NV2A_TEXCACHE_REFUSED];
    unsigned refused_next;
    uint32_t refused_logged;
    uint32_t frame, serial;
    WlNv2aTexCacheStats stats;
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
