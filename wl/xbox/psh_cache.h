// SPDX-License-Identifier: MIT
// The translated pixel shaders, kept (psh_cache.c): a program is translated (wl_xbox_psh_translate)
// once for each set of combiners and textures, and once again only when its constants change in a
// way the translation could see. A device that draws an Xbox game's pixel shaders asks it for each
// draw's program.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/xbox/psh.h"

enum {
    /* the constants a program can read: c0 and c1 of each combiner stage,
    the final combiner's two, and the fog's colour */
    WL_XBOX_PSH_CACHE_SLOTS = 19,
    /* 512 programs (about 1 KB each): a scene of Halo's draws with up to 150
    structures and constant sets, and 32 sets of 4 pushed them out of each
    other (an evicted structure is translated three times again, its trials
    with it) */
    WL_XBOX_PSH_CACHE_SETS = 64,
    WL_XBOX_PSH_CACHE_WAYS = 8,
    /* the structures pushed out of the cache, remembered by their hash to
    count those translated again */
    WL_XBOX_PSH_CACHE_EVICTED_HASHES = 256
};

typedef struct WlXboxPshCacheCounts {
    /* programs translated (wl_xbox_psh_translate called for a draw), and of
    them those beyond GX */
    unsigned long translated, unusable;
    /* draws whose program was kept as it was, and those whose program was
    kept with its constants changed */
    unsigned long found, patched;
    /* the translations made to tell whether a program's constants can be
    changed in place (two a set of combiners), those that came out otherwise,
    and the sets found to be so */
    unsigned long tests, tests_differ, generic;
    /* changed programs translated again to check them, and those that came
    out otherwise (then translated for each set of constants) */
    unsigned long audited, audits_differ;
    /* structures pushed out of the cache by others, and structures
    translated that had been (by their hash, of the last 256 pushed out) */
    unsigned long evicted, again;
} WlXboxPshCacheCounts;

/* (the cache's own: a program's structure, hashed and compared whole) */
typedef struct WlXboxPshCacheKey {
    /* the combiner state, the constants and the stages past the count cleared */
    uint32_t state[WL_XBOX_PSH_WORDS];
    uint32_t extra;
    WlXboxPshTextures textures;
    /* each constant's signature (constant_signature) */
    uint32_t signatures[WL_XBOX_PSH_CACHE_SLOTS];
} WlXboxPshCacheKey;

typedef struct WlXboxPshCacheEntry {
    WlXboxPshCacheKey key;
    /* the constants the program holds */
    uint32_t constants[WL_XBOX_PSH_CACHE_SLOTS];
    uint32_t hash;
    /* when it was last used; 0: empty */
    unsigned long stamp;
    /* the structure's own entry (in the set of its structure's hash), else
    one for these constants (in the set of their hash too) */
    unsigned char structural;
    unsigned char generic;
    /* the constant each konst colour and preloaded register holds (its
    slot), -1 for a value of the translation's own */
    signed char konst_slots[WL_TEV_KONST_SLOTS];
    signed char preload_slots[WL_TEV_REG_COUNT];
    unsigned long changes;
    bool usable;
    WlXboxPshProgram program;
} WlXboxPshCacheEntry;

/* A cache: the caller's (static: it is large), set up by wl_xbox_psh_cache_init. One thread at a
time. */
typedef struct WlXboxPshCache {
    WlXboxPshRecipe recipe;
    bool has_recipe;
    WlXboxPshCacheEntry entries[WL_XBOX_PSH_CACHE_SETS][WL_XBOX_PSH_CACHE_WAYS];
    uint32_t evicted_hashes[WL_XBOX_PSH_CACHE_EVICTED_HASHES];
    unsigned evicted_next;
    unsigned long stamp;
    WlXboxPshCacheCounts counts;
    uint32_t random_state;
    /* the last call's combiner state, extra word and textures, and its entry
    (unchanged since, as nothing else has been asked) */
    uint32_t last_state[WL_XBOX_PSH_WORDS], last_extra;
    WlXboxPshTextures last_textures;
    WlXboxPshCacheEntry *last_entry;
    /* (the calls' own: the key looked up, the programs compared) */
    WlXboxPshCacheKey key;
    WlXboxPshProgram expected, trial, audit;
    uint32_t trial_state[WL_XBOX_PSH_WORDS];
} WlXboxPshCache;

/* an empty cache whose programs are translated through the recipe (NULL:
none; copied) */
void wl_xbox_psh_cache_init(WlXboxPshCache *cache, const WlXboxPshRecipe *recipe);
/* the program for the combiner state and the textures (as wl_xbox_psh_translate takes them);
`extra` is anything else the caller tells programs apart by (its texture modes, say; 0 if
nothing), kept in the key. *usable false when it is beyond GX, *translated true when it was
translated for this call. The program is kept until the next call. */
const WlXboxPshProgram *wl_xbox_psh_cache_program(WlXboxPshCache *cache, const uint32_t *state,
                                                  uint32_t extra, const WlXboxPshTextures *textures,
                                                  bool *usable, bool *translated);
/* the counts since wl_xbox_psh_cache_init */
void wl_xbox_psh_cache_counts(const WlXboxPshCache *cache, WlXboxPshCacheCounts *counts);
