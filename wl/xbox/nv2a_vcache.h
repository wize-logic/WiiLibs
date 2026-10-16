// SPDX-License-Identifier: MIT
// Vertex arrays a backend has converted for GX to read by index (nv2a_vcache.c): a title's static
// geometry (a level's BSP, drawn in several passes every frame) converted once into big-endian arrays
// GX reads through 16-bit indices (wl_gx_direct_array), so that the processor sends two bytes an
// attribute a vertex instead of fetching, converting and sending the vertex. Kept by what they are
// (the guest-physical address of the attribute's element 0, its stride and format, what it is
// converted as) and the range of elements converted, within a memory budget or an arena of its own:
// the least recently drawn go first, and their memory is given back only once the GPU has finished
// the frame that may still read it (wl_nv2a_vcache_frame_done).
//
// The check, as the texture cache's (nv2a_texcache.h): an array is checked at its first use in each
// frame. With the write watch (the runtime's MMU) an array whose pages were not written since it was
// made is as it was, its source not read; otherwise (no watch, or a capture running) its source is
// hashed. An array found changed is converted again; one found changed twice is geometry the title
// rewrites (a skinned or animated mesh) and is not kept: its draws are sent as before.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl/platform/mem_heap.h"
#include "wl/xbox/nv2a.h"
#include "wl/xbox/nv2a_kelvin.h"

enum {
    WL_NV2A_VCACHE_ENTRIES = 512,
    WL_NV2A_VCACHE_BUCKETS = 256,
    WL_NV2A_VCACHE_PENDING = 256,
    // the elements of an array at most (GX's 16-bit indices)
    WL_NV2A_VCACHE_SPAN = 65536
};

// what an attribute is converted as: the position (an F3 array: three big-endian floats), a texture
// coordinate (any format of at most two components, as the fetch widens it: s and t, big-endian
// floats), a colour (any format, as the fetch widens it, as GX's bytes r, g, b, a: nv2a_color.h)
typedef enum {
    WL_NV2A_VCACHE_POSITION = 0,
    WL_NV2A_VCACHE_TEXCOORD,
    WL_NV2A_VCACHE_COLOR,
    WL_NV2A_VCACHE_NORMAL // x, y, z as three big-endian floats (any type, decoded)
} WlNv2aVCacheKind;

typedef struct WlNv2aVCacheConfig {
    // bytes of arrays kept (0: off, nothing kept; with an arena, at most its size)
    size_t budget;
    // an arena the arrays are kept in (the cache's alone); NULL: alloc/release
    void *arena;
    size_t arena_bytes;
    // 32-byte aligned memory the GPU reads, and its release (NULL: aligned_alloc and free)
    void *(*alloc)(void *user, size_t bytes);
    void (*release)(void *user, void *memory);
    // an array written by the processor, to memory before the GPU reads it (the Wii's DCFlushRange;
    // NULL: nothing)
    void (*flush)(void *user, void *memory, size_t bytes);
    // optional: the write watch over guest-physical memory (as WlNv2aTexCacheConfig's)
    uint32_t (*watch)(void *user, uint32_t phys, uint32_t bytes);
    bool (*written)(void *user, uint32_t phys, uint32_t bytes, uint32_t stamp);
    void *user;
    // the frames the GPU may still be drawing behind the one finished (1 under
    // wl_gx_set_frame_overlap; 0: each frame drawn by its end): an array drawn in one of them is
    // neither given back nor rewritten in place until the GPU is past it
    uint8_t gpu_lag;
} WlNv2aVCacheConfig;

typedef struct WlNv2aVCacheEntry {
    // what the array is: its element 0's guest-physical address (an address the attribute's offset
    // and stride make: the array may start at a later element), stride, format and kind
    uint32_t phys0, stride;
    uint8_t type, size, kind;
    // which of its elements are converted: [first, first + count)
    uint32_t first, count;
    // what they became (count elements of the kind's size), and the source's hash
    void *array;
    uint32_t bytes;
    uint64_t hash;
    // bookkeeping
    uint32_t used_frame, checked_frame;
    uint32_t stamp;   // the write watch's over the source since it was checked (0: none)
    uint8_t changes;  // times found changed (2: rewritten geometry, not kept: array NULL)
    uint16_t next;    // in its bucket, + 1
} WlNv2aVCacheEntry;

typedef struct WlNv2aVCacheStats {
    uint32_t lookups, hits, converted, reconverted, refused, evicted, failed;
    uint32_t checks, watched, hashed; // checks (an array's first use in a frame), by the watch, hashed
    uint64_t hashed_bytes, converted_elements;
    size_t bytes, peak_bytes;         // arrays kept now, at most
    size_t frame_bytes, peak_frame_bytes; // of those, drawn in the last frame finished, at most
} WlNv2aVCacheStats;

typedef struct WlNv2aVCache {
    WlNv2aVCacheConfig cfg;
    WlNv2aVCacheEntry entries[WL_NV2A_VCACHE_ENTRIES];
    uint16_t buckets[WL_NV2A_VCACHE_BUCKETS];
    void *pending[WL_NV2A_VCACHE_PENDING];
    uint32_t pending_frame[WL_NV2A_VCACHE_PENDING]; // the frame each was given back in
    unsigned pending_count;
    WlHeap heap; // over the arena, when there is one
    uint32_t frame;
    bool made;      // an array made since wl_nv2a_vcache_made last said so
    bool capturing; // a capture ran at the last lookup
    WlNv2aVCacheStats stats;
} WlNv2aVCache;

void wl_nv2a_vcache_init(WlNv2aVCache *cache, const WlNv2aVCacheConfig *cfg);
// every array given back (the GPU must be done with them)
void wl_nv2a_vcache_clear(WlNv2aVCache *cache);
// whether arrays are kept at all (a budget)
bool wl_nv2a_vcache_on(const WlNv2aVCache *cache);
// The array of `attrib` (a draw's, from arrays: its data and phys index `first`'s) converted as `kind`
// covering elements first .. last (the draw's least and greatest index): the address of element
// `first`'s conversion, which the draw's indices less `first` reach; NULL when it cannot be kept (no
// memory, a span beyond 16-bit indices, a format the kind does not take, rewritten geometry).
const void *wl_nv2a_vcache_get(WlNv2aVCache *cache, WlNv2a *dev, const WlNv2aAttrib *attrib, uint32_t first,
                               uint32_t last, WlNv2aVCacheKind kind);
// the bytes of an element of a kind
uint32_t wl_nv2a_vcache_element_bytes(WlNv2aVCacheKind kind);
// whether an array was made (converted, or memory reused) since the last call: GX's vertex cache must
// then be emptied (wl_gx_direct_arrays_changed)
bool wl_nv2a_vcache_made(WlNv2aVCache *cache);
// The GPU has drawn the frame: the arrays evicted during it go.
void wl_nv2a_vcache_frame_done(WlNv2aVCache *cache);
void wl_nv2a_vcache_stats(const WlNv2aVCache *cache, WlNv2aVCacheStats *out);
