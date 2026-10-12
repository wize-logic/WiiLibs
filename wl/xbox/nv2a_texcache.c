// SPDX-License-Identifier: MIT
// The converted textures kept (nv2a_texcache.h): the structure of OpenCE-Wii's texture manager
// (buckets by address, the least recently drawn evicted for room, the evicted images freed once
// the GPU has drawn the frame), over guest memory read through the device and wl/codec's
// conversions (nv2a_texture.c).

#include "wl/xbox/nv2a_texcache.h"

#include <stdlib.h>
#include <string.h>

#include "wl/base/hash.h"

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
    if (cache->cfg.budget == 0) {
        cache->cfg.budget = 4u * 1024u * 1024u;
    }
    if (cache->cfg.alloc == NULL || cache->cfg.release == NULL) {
        cache->cfg.alloc = default_alloc;
        cache->cfg.release = default_release;
    }
    cache->frame = 1;
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
        cache->cfg.release(cache->cfg.user, image);
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
            cache->cfg.release(cache->cfg.user, cache->entries[i].image);
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
    while (cache->pending_count != 0) {
        cache->cfg.release(cache->cfg.user, cache->pending[--cache->pending_count]);
    }
    cache->frame++;
}

void wl_nv2a_texcache_stats(const WlNv2aTexCache *cache, WlNv2aTexCacheStats *out) {
    *out = cache->stats;
}

static uint64_t hash_texels(WlNv2aTexCache *cache, const uint8_t *src, size_t bytes) {
    uint64_t hash = WL_FNV1A64_INIT;

    cache->stats.hashed++;
    if (bytes <= WL_NV2A_TEXCACHE_FULL_HASH) {
        cache->stats.hashed_bytes += bytes;
        return wl_fnv1a64_update(hash, src, bytes);
    }
    // the first and last 4 KiB and 64 pieces of 256 bytes between
    {
        size_t step = (bytes - 8192u) / 64u, i;

        hash = wl_fnv1a64_update(hash, src, 4096);
        for (i = 0; i < 64; i++) {
            hash = wl_fnv1a64_update(hash, src + 4096u + i * step, 256);
        }
        hash = wl_fnv1a64_update(hash, src + bytes - 4096u, 4096);
        cache->stats.hashed_bytes += 8192u + 64u * 256u;
    }
    return hash;
}

static WlNv2aTexEntry *find(WlNv2aTexCache *cache, const WlNv2aTexture *t, bool mipmaps) {
    uint16_t link = cache->buckets[bucket_of(t->phys, t->format_word)];

    while (link != 0) {
        WlNv2aTexEntry *e = &cache->entries[link - 1];

        if (e->phys == t->phys && e->format_word == t->format_word && e->rect_word == t->rect_word &&
            e->palette_phys == t->palette_phys && e->mipmaps == (uint8_t)mipmaps) {
            return e;
        }
        link = e->next;
    }
    return NULL;
}

// a slot for a new entry: a free one, else the least recently drawn (evicted)
static WlNv2aTexEntry *victim(WlNv2aTexCache *cache) {
    WlNv2aTexEntry *oldest = NULL;
    unsigned i;

    for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
        WlNv2aTexEntry *e = &cache->entries[i];

        if (e->image == NULL) {
            return e;
        }
        if (oldest == NULL || e->used_frame < oldest->used_frame) {
            oldest = e;
        }
    }
    return oldest != NULL && evict(cache, oldest) ? oldest : NULL;
}

// room for `bytes` more: the least recently drawn go
static void make_room(WlNv2aTexCache *cache, size_t bytes, const WlNv2aTexEntry *keep) {
    while (cache->stats.bytes + bytes > cache->cfg.budget) {
        WlNv2aTexEntry *oldest = NULL;
        unsigned i;

        for (i = 0; i < WL_NV2A_TEXCACHE_ENTRIES; i++) {
            WlNv2aTexEntry *e = &cache->entries[i];

            if (e->image != NULL && e != keep && (oldest == NULL || e->used_frame < oldest->used_frame)) {
                oldest = e;
            }
        }
        if (oldest == NULL || !evict(cache, oldest)) {
            return;
        }
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

// the entry's image made from the texels (its layout already set)
static bool convert(WlNv2aTexCache *cache, WlNv2aTexEntry *e, const WlNv2aTexture *t, const uint8_t *src,
                    const uint8_t *palette) {
    size_t scratch = wl_nv2a_texture_scratch_bytes(t, &e->layout);

    if (scratch != 0 && !scratch_for(cache, scratch)) {
        return false;
    }
    if (!wl_nv2a_texture_convert(t, &e->layout, src, palette, e->image, scratch != 0 ? cache->scratch : NULL)) {
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
    uint64_t hash, palette_hash = 0;
    WlNv2aGxLayout layout;

    if (made != NULL) {
        *made = false;
    }
    cache->stats.lookups++;
    e = find(cache, t, mipmaps);
    if (e != NULL && e->checked_frame == cache->frame && !e->dynamic) {
        e->used_frame = cache->frame;
        cache->stats.hits++;
        return e;
    }
    src = wl_nv2a_phys(dev, t->phys, t->total_bytes);
    if (src == NULL) {
        cache->stats.failed++;
        return NULL;
    }
    if (t->palette_entries != 0) {
        palette = wl_nv2a_phys(dev, t->palette_phys, 4u * t->palette_entries);
        if (palette != NULL) {
            palette_hash = wl_fnv1a64(palette, 4u * t->palette_entries);
        }
    }
    hash = hash_texels(cache, src, t->total_bytes);
    if (e != NULL) {
        e->checked_frame = cache->frame;
        e->used_frame = cache->frame;
        if (e->hash == hash && e->palette_hash == palette_hash) {
            cache->stats.hits++;
            return e;
        }
        // rewritten since: made again, in place when the GPU cannot be reading it yet
        e->hash = hash;
        e->palette_hash = palette_hash;
        e->dynamic = 1;
        {
            void *image = cache->cfg.alloc(cache->cfg.user, e->layout.total_bytes);

            if (image == NULL || !release_image(cache, e->image, true)) {
                if (image != NULL) {
                    cache->cfg.release(cache->cfg.user, image);
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
    e->image = cache->cfg.alloc(cache->cfg.user, layout.total_bytes);
    if (e->image == NULL) {
        WL_LOG_W(TAG, "no memory for a texture of %u bytes (%u kept)", (unsigned)layout.total_bytes,
                 (unsigned)cache->stats.bytes);
        cache->stats.failed++;
        return NULL;
    }
    e->phys = t->phys;
    e->format_word = t->format_word;
    e->rect_word = t->rect_word;
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
    e->dynamic = 0;
    if (!convert(cache, e, t, src, palette)) {
        cache->cfg.release(cache->cfg.user, e->image);
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
