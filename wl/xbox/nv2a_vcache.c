// SPDX-License-Identifier: MIT
// The converted vertex arrays kept (nv2a_vcache.h): the texture cache's structure (nv2a_texcache.c)
// over arrays: buckets by what an array is, the least recently drawn evicted for room, the evicted
// arrays freed once the GPU has drawn the frame.

#include "wl/xbox/nv2a_vcache.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/endian.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_color.h"
#include "wl/xbox/nv2a_texcache.h"

#define TAG "nv2a"

static void *default_alloc(void *user, size_t bytes) {
    (void)user;
    return aligned_alloc(32, (bytes + 31u) & ~(size_t)31u);
}

static void default_release(void *user, void *memory) {
    (void)user;
    free(memory);
}

void wl_nv2a_vcache_init(WlNv2aVCache *cache, const WlNv2aVCacheConfig *cfg) {
    memset(cache, 0, sizeof *cache);
    if (cfg != NULL) {
        cache->cfg = *cfg;
    }
    if (cache->cfg.alloc == NULL || cache->cfg.release == NULL) {
        cache->cfg.alloc = default_alloc;
        cache->cfg.release = default_release;
    }
    if (cache->cfg.arena != NULL && cache->cfg.arena_bytes >= 4096u) {
        wl_heap_init(&cache->heap, cache->cfg.arena, cache->cfg.arena_bytes, "vertex arrays");
        if (cache->cfg.budget == 0 || cache->cfg.budget > cache->heap.size) {
            cache->cfg.budget = cache->heap.size;
        }
    } else {
        cache->cfg.arena = NULL;
    }
    if (cache->cfg.watch == NULL || cache->cfg.written == NULL) {
        cache->cfg.watch = NULL;
        cache->cfg.written = NULL;
    }
    for (unsigned i = 0; i < WL_NV2A_VCACHE_ENTRIES; i++) {
        cache->entries[i].kind = 0xFFu;
    }
    cache->frame = 1;
}

bool wl_nv2a_vcache_on(const WlNv2aVCache *cache) {
    return cache->cfg.budget != 0;
}

uint32_t wl_nv2a_vcache_element_bytes(WlNv2aVCacheKind kind) {
    return kind == WL_NV2A_VCACHE_POSITION ? 12u : kind == WL_NV2A_VCACHE_TEXCOORD ? 8u : 4u;
}

// ---- memory -------------------------------------------------------------------------------------

// an array's memory, in whole 32-byte blocks (as the GPU's buffers are written back)
static size_t padded(size_t bytes) {
    return (bytes + 31u) & ~(size_t)31u;
}

static void *array_alloc(WlNv2aVCache *cache, size_t bytes) {
    if (cache->cfg.arena != NULL) {
        return wl_heap_alloc(&cache->heap, bytes, 32, TAG, 0, 0);
    }
    return cache->cfg.alloc(cache->cfg.user, bytes);
}

static void array_free(WlNv2aVCache *cache, void *array) {
    if (cache->cfg.arena != NULL) {
        wl_heap_free(&cache->heap, array);
    } else {
        cache->cfg.release(cache->cfg.user, array);
    }
}

// an array the GPU may still read this frame: given back at its end (false: no room to remember it)
static bool release_array(WlNv2aVCache *cache, void *array, bool used_now) {
    if (!used_now) {
        array_free(cache, array);
        return true;
    }
    if (cache->pending_count == WL_NV2A_VCACHE_PENDING) {
        return false;
    }
    cache->pending[cache->pending_count++] = array;
    return true;
}

static unsigned bucket_of(uint32_t phys0, uint32_t stride, unsigned kind) {
    return (unsigned)(((phys0 >> 4) ^ stride * 40503u ^ kind * 977u) * 2654435761u >> 24) &
           (WL_NV2A_VCACHE_BUCKETS - 1u);
}

static void unlink_entry(WlNv2aVCache *cache, WlNv2aVCacheEntry *e) {
    uint16_t *link = &cache->buckets[bucket_of(e->phys0, e->stride, e->kind)];
    uint16_t self = (uint16_t)(e - cache->entries + 1);

    while (*link != 0) {
        if (*link == self) {
            *link = e->next;
            return;
        }
        link = &cache->entries[*link - 1].next;
    }
}

static void link_entry(WlNv2aVCache *cache, WlNv2aVCacheEntry *e) {
    uint16_t *bucket = &cache->buckets[bucket_of(e->phys0, e->stride, e->kind)];

    e->next = *bucket;
    *bucket = (uint16_t)(e - cache->entries + 1);
}

// an entry's array given back (pending while the GPU may read it this frame); false when it cannot
// be yet
static bool drop_array(WlNv2aVCache *cache, WlNv2aVCacheEntry *e) {
    if (e->array == NULL) {
        return true;
    }
    if (!release_array(cache, e->array, e->used_frame == cache->frame)) {
        return false;
    }
    cache->stats.bytes -= e->bytes;
    e->array = NULL;
    e->bytes = 0;
    return true;
}

static bool evict(WlNv2aVCache *cache, WlNv2aVCacheEntry *e) {
    if (!drop_array(cache, e)) {
        return false;
    }
    if (e->kind != 0xFFu) {
        unlink_entry(cache, e);
        cache->stats.evicted++;
    }
    memset(e, 0, sizeof *e);
    e->kind = 0xFFu;
    return true;
}

// the least recently drawn entry not drawn in this frame (`spare` aside), or NULL
static WlNv2aVCacheEntry *eviction_candidate(WlNv2aVCache *cache, const WlNv2aVCacheEntry *spare) {
    WlNv2aVCacheEntry *best = NULL;
    unsigned i;

    for (i = 0; i < WL_NV2A_VCACHE_ENTRIES; i++) {
        WlNv2aVCacheEntry *e = &cache->entries[i];

        if (e->kind == 0xFFu || e == spare || e->array == NULL || e->used_frame == cache->frame) {
            continue;
        }
        if (best == NULL || e->used_frame < best->used_frame) {
            best = e;
        }
    }
    return best;
}

// memory for an array of `bytes`, the least recently drawn evicted for it (within the budget)
static void *room(WlNv2aVCache *cache, size_t bytes, const WlNv2aVCacheEntry *spare) {
    for (;;) {
        void *array = NULL;
        WlNv2aVCacheEntry *victim;

        if (cache->stats.bytes + bytes <= cache->cfg.budget) {
            array = array_alloc(cache, bytes);
            if (array != NULL) {
                return array;
            }
        }
        victim = eviction_candidate(cache, spare);
        if (victim == NULL || !evict(cache, victim)) {
            return NULL;
        }
    }
}

static WlNv2aVCacheEntry *free_entry(WlNv2aVCache *cache, const WlNv2aVCacheEntry *spare) {
    WlNv2aVCacheEntry *victim = NULL;
    unsigned i;

    for (i = 0; i < WL_NV2A_VCACHE_ENTRIES; i++) {
        WlNv2aVCacheEntry *e = &cache->entries[i];

        if (e->kind == 0xFFu) {
            return e;
        }
        // (a refused one's place, the least recently met, before an array's)
        if (e != spare && e->array == NULL && (victim == NULL || e->used_frame < victim->used_frame)) {
            victim = e;
        }
    }
    if (victim == NULL) {
        victim = eviction_candidate(cache, spare);
    }
    return victim != NULL && evict(cache, victim) ? victim : NULL;
}

void wl_nv2a_vcache_clear(WlNv2aVCache *cache) {
    unsigned i;

    wl_nv2a_vcache_frame_done(cache);
    for (i = 0; i < WL_NV2A_VCACHE_ENTRIES; i++) {
        if (cache->entries[i].array != NULL) {
            array_free(cache, cache->entries[i].array);
        }
        memset(&cache->entries[i], 0, sizeof cache->entries[i]);
        cache->entries[i].kind = 0xFFu;
    }
    memset(cache->buckets, 0, sizeof cache->buckets);
    cache->stats.bytes = 0;
}

void wl_nv2a_vcache_frame_done(WlNv2aVCache *cache) {
    size_t drawn = 0;
    unsigned i;

    while (cache->pending_count != 0) {
        array_free(cache, cache->pending[--cache->pending_count]);
    }
    for (i = 0; i < WL_NV2A_VCACHE_ENTRIES; i++) {
        const WlNv2aVCacheEntry *e = &cache->entries[i];

        if (e->kind != 0xFFu && e->array != NULL && e->used_frame == cache->frame) {
            drawn += e->bytes;
        }
    }
    cache->stats.frame_bytes = drawn;
    if (drawn > cache->stats.peak_frame_bytes) {
        cache->stats.peak_frame_bytes = drawn;
    }
    cache->frame++;
}

void wl_nv2a_vcache_stats(const WlNv2aVCache *cache, WlNv2aVCacheStats *out) {
    *out = cache->stats;
}

bool wl_nv2a_vcache_made(WlNv2aVCache *cache) {
    bool made = cache->made;

    cache->made = false;
    return made;
}

// ---- the arrays ---------------------------------------------------------------------------------

// the guest-physical range an entry's elements are read from
static uint32_t source_phys(const WlNv2aVCacheEntry *e) {
    return e->phys0 + e->first * e->stride;
}

static uint32_t source_bytes(const WlNv2aVCacheEntry *e) {
    return (e->count - 1u) * e->stride + wl_nv2a_attrib_bytes(e->type, e->size);
}

static bool watching(const WlNv2aVCache *cache, const WlNv2a *dev) {
    return cache->cfg.watch != NULL && !wl_nv2a_capture_active(dev);
}

// the elements converted from `src` (element `first`'s attribute) into `array`, as the fetch makes
// them (wl_nv2a_attrib_decode), to the bit
static void convert(WlNv2aVCache *cache, const WlNv2aVCacheEntry *e, const uint8_t *src, void *array) {
    uint8_t *out = array;
    uint32_t i;

    if (e->kind == WL_NV2A_VCACHE_POSITION) {
        for (i = 0; i < e->count; i++, src += e->stride, out += 12) {
            wl_wr_be32(out, wl_rd_le32(src));
            wl_wr_be32(out + 4, wl_rd_le32(src + 4));
            wl_wr_be32(out + 8, wl_rd_le32(src + 8));
        }
    } else if (e->kind == WL_NV2A_VCACHE_COLOR) {
        for (i = 0; i < e->count; i++, src += e->stride, out += 4) {
            float value[4];

            wl_nv2a_attrib_decode(e->type, e->size, src, value);
            wl_wr_be32(out, wl_nv2a_color_word(value));
        }
    } else {
        for (i = 0; i < e->count; i++, src += e->stride, out += 8) {
            float value[4];
            uint32_t bits[2];

            wl_nv2a_attrib_decode(e->type, e->size, src, value);
            memcpy(bits, value, sizeof bits);
            wl_wr_be32(out, bits[0]);
            wl_wr_be32(out + 4, bits[1]);
        }
    }
    if (cache->cfg.flush != NULL) {
        cache->cfg.flush(cache->cfg.user, array, padded(e->bytes));
    }
    cache->stats.converted_elements += e->count;
    cache->made = true;
}

static uint64_t hash_source(WlNv2aVCache *cache, const uint8_t *src, uint32_t bytes) {
    cache->stats.hashed++;
    cache->stats.hashed_bytes += bytes;
    return wl_nv2a_texcache_hash(0x9E3779B97F4A7C15ull, src, bytes);
}

// whether the entry's source is as it was converted; the entry checked (its watch armed again)
static bool unchanged(WlNv2aVCache *cache, WlNv2a *dev, WlNv2aVCacheEntry *e, const uint8_t **src_out,
                      uint64_t *hash_out) {
    uint32_t phys = source_phys(e), bytes = source_bytes(e);
    const uint8_t *src;
    uint64_t hash;

    cache->stats.checks++;
    if (e->stamp != 0 && watching(cache, dev) && !cache->cfg.written(cache->cfg.user, phys, bytes, e->stamp)) {
        cache->stats.watched++;
        return true;
    }
    src = wl_nv2a_phys(dev, phys, bytes);
    if (src == NULL) {
        return false;
    }
    // (the watch armed before the source is read: a store from here on is seen at the next check)
    e->stamp = watching(cache, dev) ? cache->cfg.watch(cache->cfg.user, phys, bytes) : 0u;
    hash = hash_source(cache, src, bytes);
    *src_out = src;
    *hash_out = hash;
    return hash == e->hash;
}

// a new entry for [first, first + count) of the array, converted
static WlNv2aVCacheEntry *make(WlNv2aVCache *cache, WlNv2a *dev, const WlNv2aVCacheEntry *key, uint32_t first,
                               uint32_t count, const WlNv2aVCacheEntry *spare) {
    WlNv2aVCacheEntry *e = free_entry(cache, spare);
    const uint8_t *src;

    if (e == NULL) {
        return NULL;
    }
    *e = *key;
    e->first = first;
    e->count = count;
    e->bytes = (uint32_t)padded(count * wl_nv2a_vcache_element_bytes((WlNv2aVCacheKind)key->kind));
    src = wl_nv2a_phys(dev, source_phys(e), source_bytes(e));
    e->array = src != NULL ? room(cache, padded(e->bytes), spare) : NULL;
    if (e->array == NULL) {
        memset(e, 0, sizeof *e);
        e->kind = 0xFFu;
        return NULL;
    }
    e->stamp = watching(cache, dev) ? cache->cfg.watch(cache->cfg.user, source_phys(e), source_bytes(e)) : 0u;
    e->hash = hash_source(cache, src, source_bytes(e));
    e->checked_frame = cache->frame;
    convert(cache, e, src, e->array);
    cache->stats.bytes += e->bytes;
    if (cache->stats.bytes > cache->stats.peak_bytes) {
        cache->stats.peak_bytes = cache->stats.bytes;
    }
    cache->stats.converted++;
    link_entry(cache, e);
    return e;
}

const void *wl_nv2a_vcache_get(WlNv2aVCache *cache, WlNv2a *dev, const WlNv2aAttrib *attrib, uint32_t first,
                               uint32_t last, WlNv2aVCacheKind kind) {
    WlNv2aVCacheEntry key, *e = NULL, *near = NULL;
    uint32_t esize = wl_nv2a_vcache_element_bytes(kind);
    uint16_t link;

    if (!wl_nv2a_vcache_on(cache) || attrib->data == NULL || last < first) {
        return NULL;
    }
    cache->stats.lookups++;
    if ((kind == WL_NV2A_VCACHE_POSITION && (attrib->type != WL_NV2A_VTX_F || attrib->size != 3)) ||
        (kind == WL_NV2A_VCACHE_TEXCOORD && (attrib->size == 0 || attrib->size > 2)) ||
        wl_nv2a_attrib_bytes(attrib->type, attrib->size) == 0 || last - first >= WL_NV2A_VCACHE_SPAN) {
        cache->stats.refused++;
        return NULL;
    }
    memset(&key, 0, sizeof key);
    key.phys0 = attrib->phys - first * attrib->stride;
    key.stride = attrib->stride;
    key.type = attrib->type;
    key.size = attrib->size;
    key.kind = (uint8_t)kind;
    for (link = cache->buckets[bucket_of(key.phys0, key.stride, key.kind)]; link != 0;
         link = cache->entries[link - 1].next) {
        WlNv2aVCacheEntry *c = &cache->entries[link - 1];

        if (c->phys0 != key.phys0 || c->stride != key.stride || c->type != key.type || c->size != key.size ||
            c->kind != key.kind) {
            continue;
        }
        if (c->changes >= 2) {
            // (rewritten geometry: sent as before)
            c->used_frame = cache->frame;
            cache->stats.refused++;
            return NULL;
        }
        if (c->array != NULL && c->first <= first && last < c->first + c->count) {
            e = c;
            break;
        }
        if (c->array != NULL) {
            near = c;
        }
    }
    if (e == NULL && near != NULL) {
        // (the array met with another range: one entry over both, when 16-bit indices reach)
        uint32_t lo = near->first < first ? near->first : first;
        uint32_t hi = near->first + near->count - 1u > last ? near->first + near->count - 1u : last;

        if (hi - lo < WL_NV2A_VCACHE_SPAN) {
            if (near->used_frame != cache->frame) {
                // (not drawn this frame: its memory free for the one over both)
                evict(cache, near);
                e = make(cache, dev, &key, lo, hi - lo + 1u, NULL);
            } else {
                e = make(cache, dev, &key, lo, hi - lo + 1u, near);
                if (e != NULL) {
                    evict(cache, near);
                }
            }
        }
    }
    if (e == NULL) {
        e = make(cache, dev, &key, first, last - first + 1u, NULL);
        if (e == NULL) {
            cache->stats.failed++;
            return NULL;
        }
    } else if (e->checked_frame != cache->frame) {
        const uint8_t *src = NULL;
        uint64_t hash = 0;

        if (!unchanged(cache, dev, e, &src, &hash)) {
            if (src == NULL || ++e->changes >= 2) {
                // (rewritten twice, or no longer guest RAM: not kept)
                drop_array(cache, e);
                e->changes = 2;
                e->used_frame = cache->frame;
                cache->stats.refused++;
                return NULL;
            }
            if (e->used_frame == cache->frame) {
                // (the GPU may still read the old one this frame: the new one elsewhere)
                void *array = room(cache, padded(e->bytes), e);

                if (array == NULL || !release_array(cache, e->array, true)) {
                    if (array != NULL) {
                        array_free(cache, array);
                    }
                    cache->stats.failed++;
                    return NULL;
                }
                e->array = array;
            }
            e->hash = hash;
            convert(cache, e, src, e->array);
            cache->stats.reconverted++;
        }
        e->checked_frame = cache->frame;
    }
    e->used_frame = cache->frame;
    cache->stats.hits++;
    return (const uint8_t *)e->array + (size_t)(first - e->first) * esize;
}
