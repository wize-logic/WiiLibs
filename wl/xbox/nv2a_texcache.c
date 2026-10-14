// SPDX-License-Identifier: MIT
// The converted textures kept (nv2a_texcache.h): the structure of OpenCE-Wii's texture manager
// (buckets by address, the least recently drawn evicted for room, the evicted images freed once
// the GPU has drawn the frame), over guest memory read through the device and wl/codec's
// conversions (nv2a_texture.c).

#include "wl/xbox/nv2a_texcache.h"

#include <stdlib.h>
#include <string.h>

#include "wl/xbox/nv2a_capture.h"

#define TAG "nv2a"

static void *default_alloc(void *user, size_t bytes) {
    (void)user;
    return aligned_alloc(32, (bytes + 31u) & ~(size_t)31u);
}

static void default_release(void *user, void *memory) {
    (void)user;
    free(memory);
}

void wl_nv2a_texcache_init(WlNv2aTexCache *cache, const WlNv2aTexCacheConfig *cfg) {
    memset(cache, 0, sizeof *cache);
    if (cfg != NULL) {
        cache->cfg = *cfg;
    }
    if (cache->cfg.alloc == NULL || cache->cfg.release == NULL) {
        cache->cfg.alloc = default_alloc;
        cache->cfg.release = default_release;
    }
    if (cache->cfg.arena != NULL && cache->cfg.arena_bytes >= 4096u) {
        wl_heap_init(&cache->heap, cache->cfg.arena, cache->cfg.arena_bytes, "textures");
        cache->heap.best_fit = true; // (a small image does not cut into the gap a large one needs)
        if (cache->cfg.budget == 0 || cache->cfg.budget > cache->heap.size) {
            cache->cfg.budget = cache->heap.size;
        }
    } else {
        cache->cfg.arena = NULL;
    }
    if (cache->cfg.budget == 0) {
        cache->cfg.budget = 4u * 1024u * 1024u;
    }
    if (cache->cfg.watch == NULL || cache->cfg.written == NULL) {
        cache->cfg.watch = NULL;
        cache->cfg.written = NULL;
    }
    cache->frame = 1;
}

// ---- images -------------------------------------------------------------------------------------

static void *image_alloc(WlNv2aTexCache *cache, size_t bytes) {
    if (cache->cfg.arena != NULL) {
        return wl_heap_alloc(&cache->heap, bytes, 32, TAG, 0, 0);
    }
    return cache->cfg.alloc(cache->cfg.user, bytes);
}

static void image_free(WlNv2aTexCache *cache, void *image) {
    if (cache->cfg.arena != NULL) {
        wl_heap_free(&cache->heap, image);
    } else {
        cache->cfg.release(cache->cfg.user, image);
    }
}

static unsigned bucket_of(uint32_t phys, uint32_t format_word) {
    return (unsigned)(((phys >> 5) ^ format_word * 40503u) * 2654435761u >> 24) & (WL_NV2A_TEXCACHE_BUCKETS - 1u);
}

static void unlink_entry(WlNv2aTexCache *cache, WlNv2aTexEntry *e) {
    uint16_t *link = &cache->buckets[bucket_of(e->phys, e->format_word)];
    uint16_t self = (uint16_t)(e - cache->entries + 1);

    while (*link != 0) {
        if (*link == self) {
            *link = e->next;
            return;
        }
        link = &cache->entries[*link - 1].next;
    }
}

static void link_entry(WlNv2aTexCache *cache, WlNv2aTexEntry *e) {
    uint16_t *bucket = &cache->buckets[bucket_of(e->phys, e->format_word)];

    e->next = *bucket;
    *bucket = (uint16_t)(e - cache->entries + 1);
}

// an image the GPU may still read this frame: given back at its end
static bool release_image(WlNv2aTexCache *cache, void *image, bool used_now) {
    if (!used_now) {
        image_free(cache, image);
        return true;
    }
    if (cache->pending_count == WL_NV2A_TEXCACHE_PENDING) {
        return false;
    }
    cache->pending[cache->pending_count++] = image;
    return true;
}

static bool evict(WlNv2aTexCache *cache, WlNv2aTexEntry *e) {
    if (e->image != NULL) {
        if (!release_image(cache, e->image, e->used_frame == cache->frame)) {
            return false;
        }
        cache->stats.bytes -= e->layout.total_bytes;
        unlink_entry(cache, e);
        cache->stats.evicted++;
    }
    memset(e, 0, sizeof *e);
    return true;
}

void wl_nv2a_texcache_clear(WlNv2aTexCache *cache) {
    unsigned i;

    wl_nv2a_texcache_frame_done(cache);
    for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
        if (cache->entries[i].image != NULL) {
            image_free(cache, cache->entries[i].image);
        }
        memset(&cache->entries[i], 0, sizeof cache->entries[i]);
    }
    memset(cache->buckets, 0, sizeof cache->buckets);
    cache->stats.bytes = 0;
    free(cache->scratch);
    cache->scratch = NULL;
    cache->scratch_bytes = 0;
}

void wl_nv2a_texcache_frame_done(WlNv2aTexCache *cache) {
    size_t drawn = 0;
    unsigned i;

    while (cache->pending_count != 0) {
        image_free(cache, cache->pending[--cache->pending_count]);
    }
    for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
        if (cache->entries[i].image != NULL && cache->entries[i].used_frame == cache->frame) {
            drawn += cache->entries[i].layout.total_bytes;
        }
    }
    cache->stats.frame_bytes = drawn;
    if (drawn > cache->stats.peak_frame_bytes) {
        cache->stats.peak_frame_bytes = drawn;
    }
    cache->frame++;
}

void wl_nv2a_texcache_stats(const WlNv2aTexCache *cache, WlNv2aTexCacheStats *out) {
    *out = cache->stats;
}

void wl_nv2a_texcache_memory(const WlNv2aTexCache *cache, WlNv2aTexCacheMemory *out) {
    unsigned i;

    memset(out, 0, sizeof *out);
    out->images = cache->stats.bytes;
    out->scratch = cache->scratch_bytes;
    if (cache->cfg.arena != NULL) {
        out->arena = cache->heap.size;
        out->arena_used = cache->heap.used;
        out->arena_largest = wl_heap_largest_free(&cache->heap);
        for (i = 0; i < cache->pending_count; i++) {
            out->pending += wl_heap_usable_size(&cache->heap, cache->pending[i]);
        }
    }
}

// ---- the check ----------------------------------------------------------------------------------

static inline uint32_t rotl32(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

static inline uint32_t load32(const uint8_t *p) {
    uint32_t v;

    memcpy(&v, p, 4);
    return v;
}

// XXH32's rounds (four lanes of 32-bit multiplies: cheap on a 32-bit processor, where FNV-1a's
// 64-bit multiply per byte is not), continued from `h`. In the host's byte order: a hash is only
// compared with one made the same way.
#define P1 2654435761u
#define P2 2246822519u
#define P3 3266489917u
#define P4 668265263u
#define P5 374761393u
static uint64_t hash_update(uint64_t h, const uint8_t *p, size_t bytes) {
    uint32_t seed = (uint32_t)h ^ (uint32_t)(h >> 32);
    uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1, acc;
    const uint8_t *end = p + bytes;

    while ((size_t)(end - p) >= 16u) {
        v1 = rotl32(v1 + load32(p) * P2, 13) * P1;
        v2 = rotl32(v2 + load32(p + 4) * P2, 13) * P1;
        v3 = rotl32(v3 + load32(p + 8) * P2, 13) * P1;
        v4 = rotl32(v4 + load32(p + 12) * P2, 13) * P1;
        p += 16;
    }
    acc = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18) + (uint32_t)bytes;
    while ((size_t)(end - p) >= 4u) {
        acc = rotl32(acc + load32(p) * P3, 17) * P4;
        p += 4;
    }
    while (p < end) {
        acc = rotl32(acc + *p++ * P5, 11) * P1;
    }
    acc ^= acc >> 15;
    acc *= P2;
    acc ^= acc >> 13;
    acc *= P3;
    acc ^= acc >> 16;
    return (h << 32 | h >> 32) ^ acc;
}

#define HASH_INIT 0x9E3779B97F4A7C15ull

static uint64_t hash_texels(WlNv2aTexCache *cache, const uint8_t *src, size_t bytes) {
    uint64_t hash = HASH_INIT;

    cache->stats.hashed++;
    if (bytes <= WL_NV2A_TEXCACHE_FULL_HASH) {
        cache->stats.hashed_bytes += bytes;
        return hash_update(hash, src, bytes);
    }
    // the first and last 4 KiB and 64 pieces of 256 bytes between
    {
        size_t step = (bytes - 8192u) / 64u, i;

        hash = hash_update(hash, src, 4096);
        for (i = 0; i < 64; i++) {
            hash = hash_update(hash, src + 4096u + i * step, 256);
        }
        hash = hash_update(hash, src + bytes - 4096u, 4096);
        cache->stats.hashed_bytes += 8192u + 64u * 256u;
    }
    return hash;
}

// whether the texture's checks may use the write watch now (a capture must see the texels read)
static bool watching(const WlNv2aTexCache *cache, const WlNv2a *dev) {
    return cache->cfg.watch != NULL && !wl_nv2a_capture_active(dev);
}

// The watch armed over the texture's texels and palette (before they are read): its stamp, 0 when
// there is none.
static uint32_t watch_arm(WlNv2aTexCache *cache, const WlNv2aTexture *t) {
    uint32_t stamp, palette_stamp;

    if (cache->cfg.watch == NULL) {
        return 0;
    }
    stamp = cache->cfg.watch(cache->cfg.user, t->phys, t->total_bytes);
    if (stamp != 0 && t->palette_entries != 0) {
        palette_stamp = cache->cfg.watch(cache->cfg.user, t->palette_phys, 4u * t->palette_entries);
        // (the earlier stamp: whatever was stored to the texels while the palette's was armed counts)
        stamp = palette_stamp != 0 ? stamp : 0u;
    }
    return stamp;
}

static bool watch_written(WlNv2aTexCache *cache, const WlNv2aTexEntry *e, const WlNv2aTexture *t) {
    return cache->cfg.written(cache->cfg.user, t->phys, t->total_bytes, e->stamp) ||
           (t->palette_entries != 0 &&
            cache->cfg.written(cache->cfg.user, t->palette_phys, 4u * t->palette_entries, e->stamp));
}

// ---- entries ------------------------------------------------------------------------------------

// The image rectangle only sizes a linear texture: a swizzled one's register may hold anything (the
// last linear texture's), and is no part of what it is.
static uint32_t rect_of(const WlNv2aTexture *t) {
    return t->linear ? t->rect_word : 0u;
}

static WlNv2aTexEntry *find(WlNv2aTexCache *cache, const WlNv2aTexture *t, bool mipmaps) {
    uint16_t link = cache->buckets[bucket_of(t->phys, t->format_word)];

    while (link != 0) {
        WlNv2aTexEntry *e = &cache->entries[link - 1];

        if (e->phys == t->phys && e->format_word == t->format_word && e->rect_word == rect_of(t) &&
            e->palette_phys == t->palette_phys && e->mipmaps == (uint8_t)mipmaps) {
            return e;
        }
        link = e->next;
    }
    return NULL;
}

// the least recently drawn entry with an image, other than `keep`; with `idle`, only one not drawn
// in this frame (whose image goes back at once)
static WlNv2aTexEntry *oldest(WlNv2aTexCache *cache, const WlNv2aTexEntry *keep, bool idle) {
    WlNv2aTexEntry *found = NULL;
    unsigned i;

    for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
        WlNv2aTexEntry *e = &cache->entries[i];

        if (e->image != NULL && e != keep && (!idle || e->used_frame != cache->frame) &&
            (found == NULL || e->used_frame < found->used_frame)) {
            found = e;
        }
    }
    return found;
}

// a slot for a new entry: a free one, else the least recently drawn (evicted)
static WlNv2aTexEntry *victim(WlNv2aTexCache *cache) {
    WlNv2aTexEntry *e;
    unsigned i;

    for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
        if (cache->entries[i].image == NULL) {
            return &cache->entries[i];
        }
    }
    e = oldest(cache, NULL, false);
    return e != NULL && evict(cache, e) ? e : NULL;
}

// room for `bytes` more: the least recently drawn go (in an arena only those not drawn in this
// frame: an image drawn in it is only given back at its end, so evicting it makes no room now). An
// arena whose budget is its size needs none of this: its heap says when there is no room, and
// image_for then empties the window that makes it (evicting by age here would throw away images
// wherever they lie first).
static void make_room(WlNv2aTexCache *cache, size_t bytes, const WlNv2aTexEntry *keep) {
    if (cache->cfg.arena != NULL && cache->cfg.budget >= cache->heap.size) {
        return;
    }
    while (cache->stats.bytes + bytes > cache->cfg.budget) {
        WlNv2aTexEntry *e = oldest(cache, keep, cache->cfg.arena != NULL);

        if (e == NULL || !evict(cache, e)) {
            return;
        }
    }
}

// ---- room in the arena --------------------------------------------------------------------------
//
// The arena's free memory is in pieces between the images kept, and a large image may find no piece
// its size while the pieces add up to far more. Evicting the least recently drawn until it fits
// throws away textures wherever they lie, most of them nowhere near a gap they would widen, and
// often all of them without making the room. Instead the arena's blocks are looked at in address
// order: a run of blocks that may go (the images not drawn in this frame) with the free pieces
// around them is a window, and the window wide enough for the image whose newest image was drawn
// longest ago (then: with the fewest bytes) is emptied. Blocks that may not go (an image drawn in
// this frame, one evicted in it and waiting for its end, the one being made) bound the windows.

typedef struct {
    uint32_t start, end; // the block, header included, as offsets in the arena
    uint32_t used_frame;
    uint16_t entry;      // + 1; 0: a block that may not go
} ArenaBlock;

typedef struct {
    WlNv2aTexCache *cache;
    const WlNv2aTexEntry *keep;
    ArenaBlock *blocks;
    unsigned count, cap;
    const uint16_t *by_image; // entries with an image in the arena, by the image's address
    unsigned images, next;
} ArenaWalk;

static bool arena_block(void *user, const WlHeapLive *live) {
    ArenaWalk *w = user;
    WlNv2aTexCache *cache = w->cache;
    const uint8_t *base = cache->heap.base;
    ArenaBlock *b;

    if (w->count == w->cap) {
        return false;
    }
    b = &w->blocks[w->count++];
    b->start = (uint32_t)((const uint8_t *)live->ptr - base) - WL_HEAP_GRANULE;
    b->end = (uint32_t)((const uint8_t *)live->ptr - base) + (uint32_t)wl_heap_usable_size(&cache->heap, live->ptr);
    b->entry = 0;
    b->used_frame = 0;
    // (both in address order: the entries' images are met as the walk passes them)
    while (w->next < w->images && (const uint8_t *)cache->entries[w->by_image[w->next]].image < (const uint8_t *)live->ptr) {
        w->next++;
    }
    if (w->next < w->images && cache->entries[w->by_image[w->next]].image == live->ptr) {
        const WlNv2aTexEntry *e = &cache->entries[w->by_image[w->next]];

        if (e != w->keep && e->used_frame != cache->frame) {
            b->entry = (uint16_t)(w->by_image[w->next] + 1u);
            b->used_frame = e->used_frame;
        }
    }
    return true;
}

static void sort_images(WlNv2aTexCache *cache, uint16_t *list, unsigned n) {
    unsigned i, j;

    // (insertion sort: a few hundred entries, met when an image finds no room)
    for (i = 1; i < n; i++) {
        uint16_t v = list[i];
        const uint8_t *image = cache->entries[v].image;

        for (j = i; j > 0 && (const uint8_t *)cache->entries[list[j - 1]].image > image; j--) {
            list[j] = list[j - 1];
        }
        list[j] = v;
    }
}

// Empties the best window for an image of `bytes`; false when no window is wide enough.
static bool arena_make_room(WlNv2aTexCache *cache, size_t bytes, const WlNv2aTexEntry *keep) {
    ArenaBlock blocks[WL_NV2A_TEXCACHE_ENTRIES + 16];
    uint16_t by_image[WL_NV2A_TEXCACHE_ENTRIES];
    ArenaWalk w;
    size_t need = ((bytes + WL_HEAP_GRANULE - 1u) & ~(size_t)(WL_HEAP_GRANULE - 1u)) + WL_HEAP_GRANULE;
    unsigned i, j, best_i = 0, best_j = 0, images = 0;
    bool found = false;
    uint32_t best_frame = 0;
    size_t best_bytes = 0;

    if (need > cache->heap.size) {
        return false;
    }
    for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
        if (cache->entries[i].image != NULL && wl_heap_owns(&cache->heap, cache->entries[i].image)) {
            by_image[images++] = (uint16_t)i;
        }
    }
    sort_images(cache, by_image, images);
    memset(&w, 0, sizeof w);
    w.cache = cache;
    w.keep = keep;
    w.blocks = blocks;
    w.cap = sizeof blocks / sizeof blocks[0];
    w.by_image = by_image;
    w.images = images;
    wl_heap_walk_live(&cache->heap, arena_block, &w);
    if (w.count == w.cap) {
        return false; // (more blocks than an arena of images holds: not ours to judge)
    }
    // windows [i, j] of blocks that may go: from the end of the block before i to the start of the
    // one after j
    for (i = 0; i < w.count; i++) {
        uint32_t newest = 0;
        size_t evicted = 0;
        size_t from = i > 0 ? blocks[i - 1].end : 0;

        if (blocks[i].entry == 0) {
            continue;
        }
        for (j = i; j < w.count && blocks[j].entry != 0; j++) {
            size_t to = j + 1 < w.count ? blocks[j + 1].start : cache->heap.size;

            newest = blocks[j].used_frame > newest ? blocks[j].used_frame : newest;
            evicted += blocks[j].end - blocks[j].start;
            if (found && newest > best_frame) {
                break; // (only worse from here)
            }
            if (to - from >= need) {
                if (!found || newest < best_frame || (newest == best_frame && evicted < best_bytes)) {
                    found = true;
                    best_i = i;
                    best_j = j;
                    best_frame = newest;
                    best_bytes = evicted;
                }
                break;
            }
        }
    }
    if (!found) {
        return false;
    }
    for (i = best_i; i <= best_j; i++) {
        evict(cache, &cache->entries[blocks[i].entry - 1u]);
    }
    return true;
}

// An image of `bytes`: when the memory has no room, entries not drawn in this frame give theirs back
// until it has (an image drawn in this frame is only given back at its end): in an arena those of
// the window arena_make_room finds, elsewhere the least recently drawn. NULL when even that does not
// make room.
static void *image_for(WlNv2aTexCache *cache, size_t bytes, const WlNv2aTexEntry *keep) {
    void *image = image_alloc(cache, bytes);

    if (image == NULL && cache->cfg.arena != NULL) {
        if (arena_make_room(cache, bytes, keep)) {
            image = image_alloc(cache, bytes);
        }
        return image;
    }
    while (image == NULL) {
        WlNv2aTexEntry *e = oldest(cache, keep, true);

        if (e == NULL) {
            return NULL;
        }
        evict(cache, e);
        image = image_alloc(cache, bytes);
    }
    return image;
}

// a texture that found no memory in this frame: not tried again before the next one
static bool refused_now(const WlNv2aTexCache *cache, const WlNv2aTexture *t) {
    unsigned i;

    for (i = 0; i < WL_NV2A_TEXCACHE_REFUSED; i++) {
        if (cache->refused[i].frame == cache->frame && cache->refused[i].phys == t->phys &&
            cache->refused[i].format_word == t->format_word && cache->refused[i].rect_word == rect_of(t)) {
            return true;
        }
    }
    return false;
}

static void refuse(WlNv2aTexCache *cache, const WlNv2aTexture *t, size_t bytes) {
    unsigned i = cache->refused_next++ % WL_NV2A_TEXCACHE_REFUSED;

    cache->refused[i].phys = t->phys;
    cache->refused[i].format_word = t->format_word;
    cache->refused[i].rect_word = rect_of(t);
    cache->refused[i].frame = cache->frame;
    cache->stats.no_memory++;
    // (logged now and then: a texture refused once is refused in every frame it is drawn)
    if (cache->refused_logged == 0 || cache->frame - cache->refused_logged >= 600u) {
        cache->refused_logged = cache->frame;
        WL_LOG_W(TAG, "no memory for a texture of %u bytes (%u kept; %u refused so far)", (unsigned)bytes,
                 (unsigned)cache->stats.bytes, (unsigned)cache->stats.no_memory);
    }
}

static bool scratch_for(WlNv2aTexCache *cache, size_t bytes) {
    void *grown;

    if (bytes <= cache->scratch_bytes) {
        return true;
    }
    grown = realloc(cache->scratch, bytes);
    if (grown == NULL) {
        return false;
    }
    cache->scratch = grown;
    cache->scratch_bytes = bytes;
    return true;
}

// The decoders' scratch for one conversion: the cache's own, grown as needed up to
// WL_NV2A_TEXCACHE_SCRATCH_KEPT and kept; a larger one (a big texture GX has no codec for, a cube
// map made a sphere: megabytes) is had for the conversion alone, from the arena like an image (the
// textures not drawn in this frame making room) or else the heap, and given back right after, so
// that the heap does not keep the largest scratch ever wanted.
static void *scratch_take(WlNv2aTexCache *cache, size_t bytes, const WlNv2aTexEntry *keep) {
    void *p;

    if (bytes <= WL_NV2A_TEXCACHE_SCRATCH_KEPT) {
        return scratch_for(cache, bytes) ? cache->scratch : NULL;
    }
    p = cache->cfg.arena != NULL ? image_for(cache, bytes, keep) : NULL;
    return p != NULL ? p : malloc(bytes);
}

static void scratch_give(WlNv2aTexCache *cache, void *p) {
    if (p == NULL || p == cache->scratch) {
        return;
    }
    if (cache->cfg.arena != NULL && wl_heap_owns(&cache->heap, p)) {
        image_free(cache, p);
    } else {
        free(p);
    }
}

// the entry's image made from the texels (its layout already set)
static bool convert(WlNv2aTexCache *cache, WlNv2aTexEntry *e, const WlNv2aTexture *t, const uint8_t *src,
                    const uint8_t *palette) {
    size_t scratch_bytes = wl_nv2a_texture_scratch_bytes(t, &e->layout);
    void *scratch = NULL;
    bool done;

    if (scratch_bytes != 0 && (scratch = scratch_take(cache, scratch_bytes, e)) == NULL) {
        return false;
    }
    done = wl_nv2a_texture_convert(t, &e->layout, src, palette, e->image, scratch);
    scratch_give(cache, scratch);
    if (!done) {
        return false;
    }
    if (cache->cfg.flush != NULL) {
        cache->cfg.flush(cache->cfg.user, e->image, e->layout.total_bytes);
    }
    e->serial = ++cache->serial;
    return true;
}

const WlNv2aTexEntry *wl_nv2a_texcache_get(WlNv2aTexCache *cache, WlNv2a *dev, const WlNv2aTexture *t,
                                           bool mipmaps, bool *made) {
    WlNv2aTexEntry *e;
    const uint8_t *src, *palette = NULL;
    uint64_t hash = 0, palette_hash = 0;
    uint32_t stamp;
    WlNv2aGxLayout layout;
    bool large;

    if (made != NULL) {
        *made = false;
    }
    cache->stats.lookups++;
    mipmaps = mipmaps && t->levels > 1; // (one level: the same image either way)
    e = find(cache, t, mipmaps);
    if (e != NULL && e->checked_frame == cache->frame && !e->dynamic) {
        e->used_frame = cache->frame;
        cache->stats.hits++;
        return e;
    }
    cache->stats.checks++;
    if (e != NULL && e->stamp != 0 && watching(cache, dev) && !watch_written(cache, e, t)) {
        // no store to its pages since it was checked: as it was
        e->checked_frame = cache->frame;
        e->used_frame = cache->frame;
        cache->stats.hits++;
        cache->stats.watched++;
        return e;
    }
    if (e == NULL && refused_now(cache, t)) {
        cache->stats.failed++;
        return NULL;
    }
    src = wl_nv2a_phys(dev, t->phys, t->total_bytes);
    if (src == NULL) {
        cache->stats.failed++;
        return NULL;
    }
    if (t->palette_entries != 0) {
        palette = wl_nv2a_phys(dev, t->palette_phys, 4u * t->palette_entries);
        if (palette != NULL) {
            palette_hash = hash_update(HASH_INIT, palette, 4u * t->palette_entries);
        }
    }
    if (e != NULL) {
        bool drawn_now = e->used_frame == cache->frame;

        // (the watch armed before the texels are read: a store from here on is seen at the next
        // check; a large texture under the watch is not hashed, its samples would not see every
        // store, so a store to its pages has it made again)
        stamp = watching(cache, dev) ? watch_arm(cache, t) : 0u;
        large = stamp != 0 && t->total_bytes > WL_NV2A_TEXCACHE_FULL_HASH;

        e->checked_frame = cache->frame;
        e->used_frame = cache->frame;
        e->stamp = stamp;
        if (!large) {
            hash = hash_texels(cache, src, t->total_bytes);
            if (e->hash == hash && e->palette_hash == palette_hash) {
                cache->stats.hits++;
                return e;
            }
        }
        // rewritten since: made again, in place when the GPU cannot be reading it yet
        e->hash = hash;
        e->palette_hash = palette_hash;
        e->dynamic = 1;
        if (drawn_now) {
            // (the GPU may read the old image in this frame: a new one, the old given back at its end)
            void *image = image_for(cache, e->layout.total_bytes, e);

            if (image == NULL || !release_image(cache, e->image, true)) {
                if (image != NULL) {
                    image_free(cache, image);
                }
                // (no room to keep the old image for the GPU: converted where it is)
                image = NULL;
            }
            if (image != NULL) {
                e->image = image;
            }
        }
        if (!convert(cache, e, t, src, palette)) {
            evict(cache, e);
            cache->stats.failed++;
            return NULL;
        }
        cache->stats.reconverted++;
        if (made != NULL) {
            *made = true;
        }
        return e;
    }
    if (!t->known || !wl_nv2a_texture_layout(t, mipmaps, &layout)) {
        cache->stats.failed++;
        return NULL;
    }
    make_room(cache, layout.total_bytes, NULL);
    e = victim(cache);
    if (e == NULL) {
        cache->stats.failed++;
        return NULL;
    }
    e->image = image_for(cache, layout.total_bytes, NULL);
    if (e->image == NULL) {
        refuse(cache, t, layout.total_bytes);
        cache->stats.failed++;
        return NULL;
    }
    stamp = watching(cache, dev) ? watch_arm(cache, t) : 0u;
    large = stamp != 0 && t->total_bytes > WL_NV2A_TEXCACHE_FULL_HASH;
    if (!large) {
        hash = hash_texels(cache, src, t->total_bytes);
    }
    e->phys = t->phys;
    e->format_word = t->format_word;
    e->rect_word = rect_of(t);
    e->palette_phys = t->palette_phys;
    e->source_bytes = t->total_bytes;
    e->hash = hash;
    e->palette_hash = palette_hash;
    e->mipmaps = (uint8_t)mipmaps;
    e->layout = layout;
    e->contents = t->contents;
    e->linear = t->linear;
    e->sphere = layout.sphere;
    e->width = (uint16_t)t->width;
    e->height = (uint16_t)t->height;
    e->used_frame = e->checked_frame = cache->frame;
    e->stamp = stamp;
    e->dynamic = 0;
    if (!convert(cache, e, t, src, palette)) {
        image_free(cache, e->image);
        memset(e, 0, sizeof *e);
        cache->stats.failed++;
        return NULL;
    }
    link_entry(cache, e);
    cache->stats.bytes += layout.total_bytes;
    cache->stats.converted++;
    if (made != NULL) {
        *made = true;
    }
    return e;
}
