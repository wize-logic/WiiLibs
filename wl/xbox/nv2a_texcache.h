// SPDX-License-Identifier: MIT
// The textures a backend has converted into GX's formats (nv2a_texcache.c), kept by what they are
// (their address, format word, size and, for a P8, palette) and checked against the guest's
// texels by a hash: a title rewrites some of its textures in place (a glyph cache) and reuses the
// memory of others. Within a memory budget: the least recently drawn go first, and their images
// are given back only once the GPU has finished the frame that may still read them
// (wl_nv2a_texcache_frame_done).
//
// The hash: a texture of at most WL_NV2A_TEXCACHE_FULL_HASH bytes is hashed whole, a larger one
// at spread samples. A texture is checked at its first use in each frame; one found changed once
// is checked at every use from then on.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_texture.h"

enum {
    WL_NV2A_TEXCACHE_ENTRIES = 512,
    WL_NV2A_TEXCACHE_BUCKETS = 256,
    WL_NV2A_TEXCACHE_PENDING = 256,
    WL_NV2A_TEXCACHE_FULL_HASH = 256 * 1024
};

typedef struct WlNv2aTexCacheConfig {
    // bytes of GX images kept (0: 4 MiB)
    size_t budget;
    // 32-byte aligned memory the GPU reads, and its release (NULL: aligned_alloc and free)
    void *(*alloc)(void *user, size_t bytes);
    void (*release)(void *user, void *memory);
    // an image written by the processor, to memory before the GPU reads it (the Wii's
    // DCFlushRange; NULL: nothing)
    void (*flush)(void *user, void *memory, size_t bytes);
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
    uint32_t serial; // changes when the image is made anew
    uint8_t dynamic; // found changed once: checked at every use
    uint16_t next;   // in its bucket, + 1
} WlNv2aTexEntry;

typedef struct WlNv2aTexCacheStats {
    uint32_t lookups, hits, converted, reconverted, evicted, failed, hashed;
    uint64_t hashed_bytes;
    size_t bytes; // images kept now
} WlNv2aTexCacheStats;

typedef struct WlNv2aTexCache {
    WlNv2aTexCacheConfig cfg;
    WlNv2aTexEntry entries[WL_NV2A_TEXCACHE_ENTRIES];
    uint16_t buckets[WL_NV2A_TEXCACHE_BUCKETS];
    void *pending[WL_NV2A_TEXCACHE_PENDING];
    unsigned pending_count;
    uint32_t frame, serial;
    WlNv2aTexCacheStats stats;
    // the decoders' scratch (grown as needed)
    void *scratch;
    size_t scratch_bytes;
} WlNv2aTexCache;

// An empty cache (the caller's: it is large) with its configuration (NULL: the defaults).
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
