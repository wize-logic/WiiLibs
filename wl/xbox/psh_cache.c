// SPDX-License-Identifier: MIT
// The translated pixel shaders, kept.
//
// A translation (wl_xbox_psh_translate) is long: it tries the combiners several ways, and a
// program beyond GX is tried every way before it is refused. A game may set many of its programs'
// constants for each object (a model's fog, from its distance, and its change colours), so a cache
// keyed on the whole of the combiner state translates such a program again for nearly every draw.
//
// The translation reads the constants only through what they look like to it: which channels are
// 0 or 1, which channels of one are equal, which constants are equal (konst registers are shared),
// and the products of constants it folds into one. The first three make each constant's
// signature; programs whose combiners, textures and signatures agree are one entry, the
// structure's. When a structure is first translated, it is translated again twice with other
// constants of the same signatures, chosen at random; if the program comes out the same each
// time, only the konst and preloaded register values that hold a constant changing with it, the
// structure is generic: for other constants of its signatures, those values are rewritten in
// place rather than the program translated. The first change of each generic program, and every
// AUDIT_INTERVAL-th after, is translated anyway and compared (the counts say how often they
// differed: a structure that differs is not generic from then on). A structure that is not
// generic is kept for each set of its constants, as before.
//
// A structure is not generic when its translation folded constants into one (the program says
// so: a dot product of two constants, worked out); a guess from the combiners alone (a dot
// product whose inputs might hold constants) took most of Halo's model shaders for such, and
// each object's colours were translated again. A program beyond the konst registers is not
// generic either (the nearest constant stands in for the rest, by value); its sets of
// constants, new for nearly every object, are translated the way the structure's translation
// took (wl_xbox_psh_translate_by_way), one try instead of every way. In a set, entries for
// constants are pushed out before structures.
//
// Every constant counts, whether or not a combiner names it (a recipe may read some no combiner
// input does): one a game left from another shader changes the program's constants, not its
// structure, while its signature holds.
//
// What the cache keeps: the combiner state (that of the stages beyond the combiner count left
// out), the caller's extra word, the textures as the translation is given them, and the
// constants' signatures; for each entry, its constants and the program.

#include "wl/xbox/psh_cache.h"

#include <string.h>

/* ---------- constants */

enum {
    /* the constants a program can read: c0 and c1 of each combiner stage,
    the final combiner's two, and the fog's colour */
    _slot_c0 = 0,
    _slot_c1 = 8,
    _slot_final_c0 = 16,
    _slot_final_c1 = 17,
    _slot_fog = 18,
    NUMBER_OF_SLOTS = WL_XBOX_PSH_CACHE_SLOTS,

    CACHE_SETS = WL_XBOX_PSH_CACHE_SETS,
    CACHE_WAYS = WL_XBOX_PSH_CACHE_WAYS,
    EVICTED_HASHES = WL_XBOX_PSH_CACHE_EVICTED_HASHES,
    /* the translations with other constants that make a structure generic */
    GENERIC_TRIALS = 2,
    /* a generic program's changes translated again to check them: the first,
    then one in this many (no audit has differed in Halo's captures, and at
    one in 32 they were most of what was translated once the programs were
    kept: a model's fog and change colours change for each object) */
    AUDIT_INTERVAL = 256
};

/* ---------- private code */

static const WlXboxPshRecipe *recipe_of(const WlXboxPshCache *cache) {
    return cache->has_recipe ? &cache->recipe : NULL;
}

/* the Xbox's ARGB as the TEV program's RGBA (as psh.c's) */
static uint32_t rgba(uint32_t color) {
    return ((uint32_t)color << 8) | ((uint32_t)color >> 24);
}

/* a constant as the translation reads it (the fog's colour opaque) */
static uint32_t slot_value(const uint32_t *state, const WlXboxPshTextures *textures, int slot) {
    if (slot < _slot_c1)
        return state[WL_XBOX_PSH_CONSTANT0_0 + slot];
    if (slot < _slot_final_c0)
        return state[WL_XBOX_PSH_CONSTANT1_0 + slot - _slot_c1];
    if (slot == _slot_final_c0)
        return state[WL_XBOX_PSH_FINAL_CONSTANT0];
    if (slot == _slot_final_c1)
        return state[WL_XBOX_PSH_FINAL_CONSTANT1];
    return textures->fog_color | 0xFF000000UL;
}

static void slot_set(uint32_t *state, WlXboxPshTextures *textures, int slot, uint32_t value) {
    if (slot < _slot_c1)
        state[WL_XBOX_PSH_CONSTANT0_0 + slot] = value;
    else if (slot < _slot_final_c0)
        state[WL_XBOX_PSH_CONSTANT1_0 + slot - _slot_c1] = value;
    else if (slot == _slot_final_c0)
        state[WL_XBOX_PSH_FINAL_CONSTANT0] = value;
    else if (slot == _slot_final_c1)
        state[WL_XBOX_PSH_FINAL_CONSTANT1] = value;
    else
        textures->fog_color = value & 0x00FFFFFFUL;
}

/* a constant's channels (alpha, red, green, blue): each 0, 255 or another
value (two bits each), and which of them are equal (bits 8 to 13); bit 31
set */
static uint32_t constant_signature(uint32_t value) {
    unsigned channels[4], i, j, bit = 8;
    uint32_t signature = 0x80000000u;

    channels[0] = (unsigned)(value >> 24);
    channels[1] = (unsigned)(value >> 16) & 0xff;
    channels[2] = (unsigned)(value >> 8) & 0xff;
    channels[3] = (unsigned)value & 0xff;
    for (i = 0; i < 4; i++)
        signature |= (uint32_t)(channels[i] == 0 ? 1 : channels[i] == 255 ? 2 : 3) << (2 * i);
    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++, bit++) {
            if (channels[i] == channels[j])
                signature |= 1u << bit;
        }
    }
    return signature;
}

/* the structure of a draw's pixel shader, and its constants (the slots
returned: all of them) */
static unsigned long structure_make(WlXboxPshCacheKey *key, uint32_t *constants,
                                    const uint32_t *state, uint32_t extra,
                                    const WlXboxPshTextures *textures) {
    unsigned long count = state[WL_XBOX_PSH_COMBINER_COUNT] & 0xff,
                  reads = (1UL << NUMBER_OF_SLOTS) - 1, stage;
    int slot, other;

    memset(key, 0, sizeof(*key));
    memset(constants, 0, NUMBER_OF_SLOTS * sizeof(uint32_t));
    memcpy(key->state, state, sizeof(key->state));
    key->extra = extra;
    if (count > 8)
        count = 8;
    for (stage = count; stage < 8; stage++) {
        key->state[WL_XBOX_PSH_RGB_INPUTS0 + stage] = 0;
        key->state[WL_XBOX_PSH_RGB_OUTPUTS0 + stage] = 0;
        key->state[WL_XBOX_PSH_ALPHA_INPUTS0 + stage] = 0;
        key->state[WL_XBOX_PSH_ALPHA_OUTPUTS0 + stage] = 0;
    }
    for (slot = 0; slot < 8; slot++) {
        key->state[WL_XBOX_PSH_CONSTANT0_0 + slot] = 0;
        key->state[WL_XBOX_PSH_CONSTANT1_0 + slot] = 0;
    }
    key->state[WL_XBOX_PSH_FINAL_CONSTANT0] = 0;
    key->state[WL_XBOX_PSH_FINAL_CONSTANT1] = 0;
    /* (as the caller has them, padding and all: the device clears them
    first) */
    memcpy(&key->textures, textures, sizeof(key->textures));
    key->textures.fog_color = 0;
    for (slot = 0; slot < NUMBER_OF_SLOTS; slot++) {
        if (reads & (1UL << slot))
            constants[slot] = slot_value(state, textures, slot);
    }
    for (slot = 0; slot < NUMBER_OF_SLOTS; slot++) {
        if (!(reads & (1UL << slot)))
            continue;
        /* (and the first constant equal to it) */
        for (other = 0;
             other < slot && (!(reads & (1UL << other)) || constants[other] != constants[slot]);
             other++)
            ;
        key->signatures[slot] = constant_signature(constants[slot]) | (uint32_t)(other + 1) << 16;
    }
    return reads;
}

static uint32_t words_hash(uint32_t hash, const void *data, size_t size) {
    const uint32_t *word = data;
    size_t index;

    for (index = 0; index < size / sizeof(uint32_t); index++) {
        hash = (hash ^ word[index]) * 0x9E3779B1u;
        hash ^= hash >> 15;
    }
    return hash;
}

static unsigned random_channel(WlXboxPshCache *cache) {
    cache->random_state ^= cache->random_state << 13;
    cache->random_state ^= cache->random_state >> 17;
    cache->random_state ^= cache->random_state << 5;
    return 1 + (cache->random_state >> 8) % 254;
}

/* a constant of the same signature: its channels of 0 and 255 kept, the
others chosen at random (1 to 254), equal where the constant's are */
static uint32_t constant_perturbed(WlXboxPshCache *cache, uint32_t value) {
    unsigned channels[4], out[4], i, j;
    bool taken;

    channels[0] = (unsigned)(value >> 24);
    channels[1] = (unsigned)(value >> 16) & 0xff;
    channels[2] = (unsigned)(value >> 8) & 0xff;
    channels[3] = (unsigned)value & 0xff;
    for (i = 0; i < 4; i++) {
        if (channels[i] == 0 || channels[i] == 255) {
            out[i] = channels[i];
            continue;
        }
        for (j = 0; j < i && channels[j] != channels[i]; j++)
            ;
        if (j < i) {
            out[i] = out[j];
            continue;
        }
        do {
            out[i] = random_channel(cache);
            for (taken = false, j = 0; j < i; j++)
                taken |= out[j] == out[i];
        } while (taken);
    }
    return (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 8 |
           (uint32_t)out[3];
}

/* whether a constant has a channel other than 0 and 255 (else it is its
signature's only one) */
static bool constant_free(uint32_t value) {
    int shift;

    for (shift = 0; shift < 32; shift += 8) {
        unsigned channel = (unsigned)(value >> shift) & 0xff;

        if (channel != 0 && channel != 255)
            return true;
    }
    return false;
}

/* other constants of the same signatures (equal where they are); false
when none could be found */
static bool constants_perturbed(WlXboxPshCache *cache, unsigned long reads,
                                const uint32_t *constants, uint32_t *perturbed) {
    int slot, other, attempt;

    memset(perturbed, 0, NUMBER_OF_SLOTS * sizeof(uint32_t));
    for (slot = 0; slot < NUMBER_OF_SLOTS; slot++) {
        if (!(reads & (1UL << slot)))
            continue;
        for (other = 0;
             other < slot && (!(reads & (1UL << other)) || constants[other] != constants[slot]);
             other++)
            ;
        if (other < slot) {
            perturbed[slot] = perturbed[other];
            continue;
        }
        for (attempt = 0; attempt < 64; attempt++) {
            perturbed[slot] = constant_perturbed(cache, constants[slot]);
            if (slot == _slot_fog)
                perturbed[slot] |= 0xFF000000UL;
            for (other = 0;
                 other < slot && (!(reads & (1UL << other)) || perturbed[other] != perturbed[slot]);
                 other++)
                ;
            if (other == slot)
                break;
        }
        if (attempt == 64)
            return false;
    }
    return true;
}

/* the constant each of the program's konst colours and preloaded registers
holds */
static void slots_find(WlXboxPshCacheEntry *entry, unsigned long reads) {
    const WlTevProgram *tev = &entry->program.tev;
    int index, slot;

    for (index = 0; index < (int)WL_TEV_KONST_SLOTS; index++) {
        entry->konst_slots[index] = -1;
        for (slot = 0; slot < NUMBER_OF_SLOTS && (tev->konst_mask & (1u << index)); slot++) {
            if ((reads & (1UL << slot)) && rgba(entry->constants[slot]) == tev->konst[index]) {
                entry->konst_slots[index] = (signed char)slot;
                break;
            }
        }
    }
    for (index = 0; index < WL_TEV_REG_COUNT; index++) {
        entry->preload_slots[index] = -1;
        for (slot = 0; slot < NUMBER_OF_SLOTS && (tev->preload_mask & (1u << index)); slot++) {
            if ((reads & (1UL << slot)) && rgba(entry->constants[slot]) == tev->preload[index]) {
                entry->preload_slots[index] = (signed char)slot;
                break;
            }
        }
    }
}

static void constants_patch(const WlXboxPshCacheEntry *entry, WlXboxPshProgram *program,
                            const uint32_t *constants) {
    int index;

    for (index = 0; index < (int)WL_TEV_KONST_SLOTS; index++) {
        if (entry->konst_slots[index] >= 0)
            program->tev.konst[index] = rgba(constants[(int)entry->konst_slots[index]]);
    }
    for (index = 0; index < WL_TEV_REG_COUNT; index++) {
        if (entry->preload_slots[index] >= 0)
            program->tev.preload[index] = rgba(constants[(int)entry->preload_slots[index]]);
    }
}

/* whether the program changes with its constants only where it holds them */
static bool structure_generic(WlXboxPshCache *cache, const WlXboxPshCacheEntry *entry,
                              unsigned long reads, const uint32_t *state,
                              const WlXboxPshTextures *textures) {
    uint32_t *trial_state = cache->trial_state;
    WlXboxPshProgram *expected = &cache->expected, *trial = &cache->trial;
    WlXboxPshTextures trial_textures;
    uint32_t perturbed[NUMBER_OF_SLOTS];
    bool usable, free = false;
    int attempt, slot, way;

    /* (no constants, or none but 0s and 1s: no others of their signatures) */
    for (slot = 0; slot < NUMBER_OF_SLOTS; slot++)
        free |= (reads & (1UL << slot)) && constant_free(entry->constants[slot]);
    if (!free)
        return true;
    if (entry->program.folded) {
        cache->counts.folded++;
        return false;
    }
    for (attempt = 0; attempt < GENERIC_TRIALS; attempt++) {
        if (!constants_perturbed(cache, reads, entry->constants, perturbed))
            return false;
        memcpy(trial_state, state, sizeof(cache->trial_state));
        memcpy(&trial_textures, textures, sizeof(trial_textures));
        for (slot = 0; slot < NUMBER_OF_SLOTS; slot++) {
            if (reads & (1UL << slot))
                slot_set(trial_state, &trial_textures, slot, perturbed[slot]);
        }
        way = entry->way;
        usable = wl_xbox_psh_translate_by_way(trial_state, &trial_textures, recipe_of(cache), trial,
                                              &way);
        cache->counts.tests++;
        memcpy(expected, &entry->program, sizeof(*expected));
        constants_patch(entry, expected, perturbed);
        if (usable != entry->usable || memcmp(expected, trial, sizeof(*expected))) {
            cache->counts.tests_differ++;
            return false;
        }
    }
    return true;
}

/* the set's entry used longest ago: an empty one first, then one for a set
of constants (of a program whose constants change all the time, each is
mostly drawn once), then a structure (translated three times again when it
comes back, its trials with it) */
static WlXboxPshCacheEntry *set_victim(WlXboxPshCacheEntry *set) {
    WlXboxPshCacheEntry *victim = &set[0];
    int way;

    for (way = 1; way < CACHE_WAYS; way++) {
        const WlXboxPshCacheEntry *entry = &set[way];
        int rank = !entry->stamp ? 0 : entry->structural ? 2 : 1,
            victim_rank = !victim->stamp ? 0 : victim->structural ? 2 : 1;

        if (rank < victim_rank || (rank == victim_rank && entry->stamp < victim->stamp))
            victim = &set[way];
    }
    return victim;
}

/* ---------- public code */

void wl_xbox_psh_cache_init(WlXboxPshCache *cache, const WlXboxPshRecipe *recipe) {
    memset(cache, 0, sizeof(*cache));
    if (recipe) {
        cache->recipe = *recipe;
        cache->has_recipe = true;
    }
    cache->random_state = 0x2545F491u;
}

const WlXboxPshProgram *wl_xbox_psh_cache_program(WlXboxPshCache *cache,
                                                  const uint32_t *render_state, uint32_t extra,
                                                  const WlXboxPshTextures *textures, bool *usable,
                                                  bool *translated) {
    WlXboxPshCacheKey *key = &cache->key;
    WlXboxPshProgram *audit = &cache->audit;
    WlXboxPshCacheCounts *counts = &cache->counts;
    uint32_t constants[NUMBER_OF_SLOTS];
    WlXboxPshCacheEntry *set, *entry = NULL, *structural = NULL;
    unsigned long reads;
    uint32_t hash, exact_hash = 0;
    int way;

    *translated = false;
    /* (as the last call: its program, still as it was left) */
    if (cache->last_entry && !memcmp(cache->last_state, render_state, sizeof(cache->last_state)) &&
        cache->last_extra == extra &&
        !memcmp(&cache->last_textures, textures, sizeof(cache->last_textures))) {
        counts->found++;
        *usable = cache->last_entry->usable;
        return &cache->last_entry->program;
    }
    cache->stamp++;
    reads = structure_make(key, constants, render_state, extra, textures);
    hash = words_hash(0x811C9DC5u, key, sizeof(*key));
    set = cache->entries[hash % CACHE_SETS];
    for (way = 0; way < CACHE_WAYS && !structural; way++) {
        if (set[way].stamp && set[way].structural && set[way].hash == hash &&
            !memcmp(&set[way].key, key, sizeof(*key)))
            structural = &set[way];
    }
    if (structural && !memcmp(structural->constants, constants, sizeof(constants))) {
        entry = structural;
        counts->found++;
    } else if (structural && structural->generic) {
        entry = structural;
        memcpy(entry->constants, constants, sizeof(constants));
        constants_patch(entry, &entry->program, constants);
        entry->version++;
        counts->patched++;
        if (entry->changes++ % AUDIT_INTERVAL == 0) {
            bool audit_usable =
                wl_xbox_psh_translate(render_state, textures, recipe_of(cache), audit);

            counts->audited++;
            if (audit_usable != entry->usable || memcmp(audit, &entry->program, sizeof(*audit))) {
                counts->audits_differ++;
                counts->translated++;
                counts->unusable += !audit_usable;
                *translated = true;
                entry->generic = false;
                entry->usable = audit_usable;
                memcpy(&entry->program, audit, sizeof(*audit));
                entry->version++;
            }
        }
    } else if (structural) {
        /* (not generic: an entry of its own for these constants) */
        exact_hash = words_hash(hash, constants, sizeof(constants));
        set = cache->entries[exact_hash % CACHE_SETS];
        for (way = 0; way < CACHE_WAYS && !entry; way++) {
            if (set[way].stamp && !set[way].structural && set[way].hash == exact_hash &&
                !memcmp(set[way].constants, constants, sizeof(constants)) &&
                !memcmp(&set[way].key, key, sizeof(*key))) {
                entry = &set[way];
                counts->found++;
            }
        }
    }
    if (!entry) {
        unsigned which;

        entry = set_victim(set);
        if (entry->stamp && entry->structural) {
            counts->evicted++;
            cache->evicted_hashes[cache->evicted_next++ % EVICTED_HASHES] = entry->hash;
        } else if (entry->stamp)
            counts->constant_sets_evicted++;
        if (!structural)
            for (which = 0; which < EVICTED_HASHES; which++)
                if (cache->evicted_hashes[which] == hash) {
                    counts->again++;
                    break;
                }
        memcpy(&entry->key, key, sizeof(*key));
        memcpy(entry->constants, constants, sizeof(constants));
        entry->version++;
        entry->hash = structural ? exact_hash : hash;
        entry->structural = !structural;
        entry->generic = false;
        entry->changes = 0;
        /* (a structure's other constants: the way the structure's translation
        took) */
        entry->way = structural && !structural->program.folded ? structural->way : -1;
        entry->usable = wl_xbox_psh_translate_by_way(render_state, textures, recipe_of(cache),
                                                     &entry->program, &entry->way);
        counts->translated++;
        counts->unusable += !entry->usable;
        counts->constant_sets += structural != NULL;
        *translated = true;
        /* (a structure first seen: whether it is generic) */
        if (!structural) {
            slots_find(entry, reads);
            entry->generic =
                (unsigned char)structure_generic(cache, entry, reads, render_state, textures);
            counts->generic += entry->generic;
        }
    }
    entry->stamp = cache->stamp;
    memcpy(cache->last_state, render_state, sizeof(cache->last_state));
    cache->last_extra = extra;
    memcpy(&cache->last_textures, textures, sizeof(cache->last_textures));
    cache->last_entry = entry;
    *usable = entry->usable;
    return &entry->program;
}

/* the slot of the caller's words in the recent calls, and their hash */
static unsigned recent_slot(const uint32_t *words, uint32_t extra, uint32_t *hash_out) {
    uint32_t hash = extra * 0x9E3779B1u;
    unsigned index;

    for (index = 0; index < WL_XBOX_PSH_WORDS; index++)
        hash = (hash ^ words[index]) * 0x01000193u;
    hash ^= hash >> 16;
    *hash_out = hash;
    return hash % WL_XBOX_PSH_CACHE_RECENT;
}

const WlXboxPshProgram *wl_xbox_psh_cache_recent(WlXboxPshCache *cache, const uint32_t *words, uint32_t extra,
                                                 const WlXboxPshTextures *textures, bool *usable) {
    uint32_t hash;
    unsigned slot = recent_slot(words, extra, &hash);
    WlXboxPshCacheEntry *entry = cache->recent[slot].entry;

    if (!entry || cache->recent[slot].hash != hash || cache->recent[slot].version != entry->version ||
        cache->recent[slot].extra != extra ||
        memcmp(cache->recent[slot].words, words, sizeof(cache->recent[slot].words)) ||
        memcmp(&cache->recent[slot].textures, textures, sizeof(*textures)))
        return NULL;
    /* (as wl_xbox_psh_cache_program finds it: used now; the last call's
    state, which the caller's words do not give, no longer the last) */
    cache->stamp++;
    entry->stamp = cache->stamp;
    cache->last_entry = NULL;
    cache->counts.found++;
    cache->counts.recent++;
    *usable = entry->usable;
    return &entry->program;
}

void wl_xbox_psh_cache_remember(WlXboxPshCache *cache, const uint32_t *words, uint32_t extra,
                                const WlXboxPshTextures *textures) {
    uint32_t hash;
    unsigned slot = recent_slot(words, extra, &hash);

    if (!cache->last_entry)
        return;
    cache->recent[slot].hash = hash;
    cache->recent[slot].version = cache->last_entry->version;
    cache->recent[slot].extra = extra;
    cache->recent[slot].entry = cache->last_entry;
    memcpy(cache->recent[slot].words, words, sizeof(cache->recent[slot].words));
    memcpy(&cache->recent[slot].textures, textures, sizeof(*textures));
}

void wl_xbox_psh_cache_counts(const WlXboxPshCache *cache, WlXboxPshCacheCounts *out) {
    *out = cache->counts;
}
