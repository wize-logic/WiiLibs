// SPDX-License-Identifier: MIT
// wl/xbox/psh_cache: every program the cache gives is the one a fresh translation makes, byte for
// byte: for random combiner programs drawn with their constants as they were, changed within their
// signatures (a channel's 0s and 1s, and the equal channels, kept: the program patched in place)
// and changed outside them; for more structures than the cache holds; with the caller's extra word
// and a recipe.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/psh_cache.h"

static uint32_t g_rng = 0xC0FFEEu;
static uint32_t rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

static WlXboxPshCache g_cache;
static WlXboxPshProgram g_fresh;

static unsigned rand_input(void) {
    static const unsigned regs[] = {0, 1, 2, 3, 4, 5, 8, 9, 12, 13};
    static const unsigned maps[] = {0x00, 0x20, 0x40, 0x80, 0xc0};
    return regs[rnd() % 10u] | (rnd() % 3u == 0 ? 0x10u : 0u) | maps[rnd() % 5u];
}

static unsigned rand_output(void) {
    static const unsigned names[] = {0, 0, 12, 13, 8, 4};
    return names[rnd() % 6u];
}

// a constant whose channels are 0, 255 or something else, some equal
static uint32_t rand_constant(void) {
    uint32_t value = 0;
    for (int ch = 0; ch < 4; ch++) {
        unsigned kind = rnd() % 4u, channel = kind == 0 ? 0u : kind == 1 ? 255u : 1u + rnd() % 254u;
        value = value << 8 | channel;
    }
    return value;
}

static void program_random(uint32_t *state, WlXboxPshTextures *textures) {
    memset(state, 0, WL_XBOX_PSH_WORDS * sizeof *state);
    unsigned count = 1 + rnd() % 3u;
    state[WL_XBOX_PSH_COMBINER_COUNT] = count | (rnd() % 2u ? 0x1000u : 0u);
    for (unsigned st = 0; st < count; st++) {
        for (int portion = 0; portion < 2; portion++) {
            uint32_t in = 0;
            for (int k = 0; k < 4; k++) {
                in = in << 8 | rand_input();
            }
            unsigned ab = rand_output(), cd = rand_output();
            if (ab && ab == cd) {
                cd = 0;
            }
            state[(portion ? WL_XBOX_PSH_ALPHA_INPUTS0 : WL_XBOX_PSH_RGB_INPUTS0) + st] = in;
            state[(portion ? WL_XBOX_PSH_ALPHA_OUTPUTS0 : WL_XBOX_PSH_RGB_OUTPUTS0) + st] =
                ab << 4 | cd;
        }
    }
    for (int i = 0; i < 8; i++) {
        state[WL_XBOX_PSH_CONSTANT0_0 + i] = rand_constant();
        state[WL_XBOX_PSH_CONSTANT1_0 + i] = rand_constant();
    }
    state[WL_XBOX_PSH_FINAL_CONSTANT0] = rand_constant();
    state[WL_XBOX_PSH_FINAL_CONSTANT1] = rand_constant();
    memset(textures, 0, sizeof *textures);
    textures->present = 1u + rnd() % 7u;
    textures->fog = (unsigned char)(rnd() % 2u);
    textures->fog_color = rand_constant() & 0x00FFFFFFu;
}

// each constant's channels changed where they are neither 0 nor 255, equal channels kept equal
// (within its signature), or (outside) any channel
static void constants_change(uint32_t *state, WlXboxPshTextures *textures, bool within) {
    static const int words[] = {WL_XBOX_PSH_CONSTANT0_0, WL_XBOX_PSH_CONSTANT1_0};
    uint32_t *slots[19];
    int n = 0;
    for (int w = 0; w < 2; w++) {
        for (int i = 0; i < 8; i++) {
            slots[n++] = &state[words[w] + i];
        }
    }
    slots[n++] = &state[WL_XBOX_PSH_FINAL_CONSTANT0];
    slots[n++] = &state[WL_XBOX_PSH_FINAL_CONSTANT1];
    slots[n++] = &textures->fog_color;
    uint32_t before[19];
    for (int s = 0; s < n; s++) {
        before[s] = *slots[s];
    }
    for (int s = 0; s < n; s++) {
        uint32_t value = *slots[s];
        int same;
        // (a constant equal to one before it stays so: its signature says so)
        for (same = 0; same < s && before[same] != value; same++) {
        }
        if (within && same < s) {
            *slots[s] = *slots[same];
            continue;
        }
        if (!within) {
            *slots[s] = rand_constant() & (s == n - 1 ? 0x00FFFFFFu : 0xFFFFFFFFu);
            continue;
        }
        // (a new value for each distinct middle channel, the same for equal ones)
        unsigned from[4], to[4], used = 0;
        uint32_t out = 0;
        for (int ch = 3; ch >= 0; ch--) {
            unsigned channel = (value >> (8 * ch)) & 255u, now = channel;
            if (channel != 0 && channel != 255) {
                unsigned u;
                for (u = 0; u < used && from[u] != channel; u++) {
                }
                if (u == used) {
                    bool taken;
                    from[used] = channel;
                    do {
                        to[used] = 1u + rnd() % 254u;
                        taken = false;
                        for (unsigned v = 0; v < used; v++) {
                            taken |= to[v] == to[used];
                        }
                    } while (taken);
                    used++;
                }
                now = to[u];
            }
            out = out << 8 | now;
        }
        *slots[s] = out;
    }
}

static void compare(const uint32_t *state, const WlXboxPshTextures *textures, uint32_t extra,
                    unsigned long *differ) {
    bool usable, translated;
    const WlXboxPshProgram *cached =
        wl_xbox_psh_cache_program(&g_cache, state, extra, textures, &usable, &translated);
    bool fresh_usable = wl_xbox_psh_translate(state, textures, NULL, &g_fresh);
    if (usable != fresh_usable || (usable && memcmp(cached, &g_fresh, sizeof g_fresh))) {
        ++*differ;
    }
}

TEST cached_programs_are_fresh_translations(void) {
    static uint32_t state[WL_XBOX_PSH_WORDS], first[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures, first_textures;
    unsigned long differ = 0;
    WlXboxPshCacheCounts counts;
    wl_xbox_psh_cache_init(&g_cache, NULL);
    for (int n = 0; n < 400; n++) {
        program_random(state, &textures);
        memcpy(first, state, sizeof state);
        first_textures = textures;
        compare(state, &textures, 0, &differ);
        compare(state, &textures, 0, &differ);
        for (int change = 0; change < 6; change++) {
            constants_change(state, &textures, change < 4);
            compare(state, &textures, 0, &differ);
        }
        compare(first, &first_textures, 0, &differ);
    }
    wl_xbox_psh_cache_counts(&g_cache, &counts);
    ASSERT_EQ(0ul, differ);
    ASSERT_EQ(0ul, counts.audits_differ);
    // (more structures than the cache holds: some pushed out)
    ASSERT(counts.evicted > 0);
    ASSERT(counts.patched > 0);
    ASSERT(counts.generic > 0);
    ASSERT(counts.found >= 400);
    ASSERT(counts.translated < 400 * 9);
    PASS();
}

TEST the_last_call_and_the_extra_word(void) {
    static uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshCacheCounts before, after;
    bool usable, translated;
    wl_xbox_psh_cache_init(&g_cache, NULL);
    program_random(state, &textures);
    const WlXboxPshProgram *one =
        wl_xbox_psh_cache_program(&g_cache, state, 5, &textures, &usable, &translated);
    ASSERT(translated);
    wl_xbox_psh_cache_counts(&g_cache, &before);
    const WlXboxPshProgram *two =
        wl_xbox_psh_cache_program(&g_cache, state, 5, &textures, &usable, &translated);
    ASSERT(!translated);
    ASSERT_EQ(one, two);
    wl_xbox_psh_cache_counts(&g_cache, &after);
    ASSERT_EQ(before.found + 1, after.found);
    // another extra word: another structure
    wl_xbox_psh_cache_program(&g_cache, state, 6, &textures, &usable, &translated);
    ASSERT(translated);
    wl_xbox_psh_cache_program(&g_cache, state, 5, &textures, &usable, &translated);
    ASSERT(!translated);
    PASS();
}

static int g_recipe_calls;
static bool recipe_make(const uint32_t *state, const WlXboxPshTextures *textures,
                        WlXboxPshProgram *program, void *user) {
    (void)state;
    (void)textures;
    (void)program;
    ++*(int *)user;
    return false;
}

TEST the_cache_translates_through_its_recipe(void) {
    static uint32_t state[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures;
    WlXboxPshRecipe recipe = {recipe_make, &g_recipe_calls};
    bool usable, translated;
    wl_xbox_psh_cache_init(&g_cache, &recipe);
    g_recipe_calls = 0;
    program_random(state, &textures);
    wl_xbox_psh_cache_program(&g_cache, state, 0, &textures, &usable, &translated);
    ASSERT(translated);
    ASSERT(g_recipe_calls >= 1);
    PASS();
}

// a translation by the way one of the same structure took is the translation of every way
// (programs folded aside: the cache does not ask it of them)
TEST a_translation_by_way_is_the_translation(void) {
    static uint32_t state[WL_XBOX_PSH_WORDS];
    static WlXboxPshProgram first, by_way;
    WlXboxPshTextures textures;
    unsigned long tried = 0, differ = 0;
    for (int n = 0; n < 300; n++) {
        int way = -1, again;
        program_random(state, &textures);
        bool usable = wl_xbox_psh_translate_by_way(state, &textures, NULL, &first, &way);
        bool fresh_usable = wl_xbox_psh_translate(state, &textures, NULL, &g_fresh);
        ASSERT_EQ(fresh_usable, usable);
        ASSERT(!usable || !memcmp(&first, &g_fresh, sizeof first));
        if (!usable || first.folded)
            continue;
        ASSERT(way >= 0);
        for (int change = 0; change < 4; change++) {
            constants_change(state, &textures, true);
            again = way;
            usable = wl_xbox_psh_translate_by_way(state, &textures, NULL, &by_way, &again);
            fresh_usable = wl_xbox_psh_translate(state, &textures, NULL, &g_fresh);
            tried++;
            if (usable != fresh_usable || (usable && memcmp(&by_way, &g_fresh, sizeof g_fresh)))
                differ++;
        }
    }
    ASSERT(tried > 100);
    ASSERT_EQ(0ul, differ);
    PASS();
}

// the caller's own words for a combiner state (any function of them: here the state's words turned)
static void words_of(const uint32_t *state, uint32_t *words) {
    for (int i = 0; i < WL_XBOX_PSH_WORDS; i++) {
        words[i] = state[i] ^ 0x5A5A5A5Au;
    }
}

// wl_xbox_psh_cache_recent: the program a remembered call found, for the same words, extra word and
// textures, while its entry is unchanged; NULL for other words, another extra word or other textures,
// and once its entry was patched (its constants changed within the signature: the program the entry
// holds is another state's) or evicted (the entry another program's)
TEST the_recent_calls_found_by_the_callers_words(void) {
    static uint32_t state[WL_XBOX_PSH_WORDS], changed[WL_XBOX_PSH_WORDS], words[WL_XBOX_PSH_WORDS],
        other[WL_XBOX_PSH_WORDS], flood[WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures, changed_textures, flood_textures, other_textures;
    WlXboxPshCacheCounts before, after;
    const WlXboxPshProgram *program, *found;
    bool usable, found_usable, translated;
    int n, patched = 0;

    wl_xbox_psh_cache_init(&g_cache, NULL);
    // (a program whose structure the cache patches: a constant change within its signature)
    for (n = 0; n < 200 && !patched; n++) {
        program_random(state, &textures);
        words_of(state, words);
        program = wl_xbox_psh_cache_program(&g_cache, state, 3, &textures, &usable, &translated);
        wl_xbox_psh_cache_remember(&g_cache, words, 3, &textures);
        // a hit: the program the call found, counted
        wl_xbox_psh_cache_counts(&g_cache, &before);
        found_usable = !usable;
        found = wl_xbox_psh_cache_recent(&g_cache, words, 3, &textures, &found_usable);
        ASSERT(found == program);
        ASSERT_EQ(usable, found_usable);
        wl_xbox_psh_cache_counts(&g_cache, &after);
        ASSERT_EQ(before.recent + 1ul, after.recent);
        ASSERT_EQ(before.found + 1ul, after.found);
        ASSERT(wl_xbox_psh_translate(state, &textures, NULL, &g_fresh) == usable);
        ASSERT(!usable || !memcmp(found, &g_fresh, sizeof g_fresh));
        // a miss: other words, another extra word, other textures
        memcpy(other, words, sizeof other);
        other[WL_XBOX_PSH_COMBINER_COUNT] ^= 1u;
        ASSERT(wl_xbox_psh_cache_recent(&g_cache, other, 3, &textures, &found_usable) == NULL);
        ASSERT(wl_xbox_psh_cache_recent(&g_cache, words, 4, &textures, &found_usable) == NULL);
        other_textures = textures;
        other_textures.fog ^= 1u;
        ASSERT(wl_xbox_psh_cache_recent(&g_cache, words, 3, &other_textures, &found_usable) == NULL);
        // the entry patched for other constants: a miss for the first state's words
        memcpy(changed, state, sizeof changed);
        changed_textures = textures;
        constants_change(changed, &changed_textures, true);
        if (!memcmp(changed, state, sizeof changed) && !memcmp(&changed_textures, &textures, sizeof textures)) {
            continue;
        }
        wl_xbox_psh_cache_counts(&g_cache, &before);
        (void)wl_xbox_psh_cache_program(&g_cache, changed, 3, &changed_textures, &usable, &translated);
        wl_xbox_psh_cache_counts(&g_cache, &after);
        if (after.patched == before.patched) {
            continue;
        }
        patched = 1;
        ASSERT(wl_xbox_psh_cache_recent(&g_cache, words, 3, &textures, &found_usable) == NULL);
    }
    ASSERT(patched);
    // remembered again, then more structures than the cache holds: the entry evicted, a miss
    program = wl_xbox_psh_cache_program(&g_cache, state, 3, &textures, &usable, &translated);
    wl_xbox_psh_cache_remember(&g_cache, words, 3, &textures);
    ASSERT(wl_xbox_psh_cache_recent(&g_cache, words, 3, &textures, &found_usable) == program);
    wl_xbox_psh_cache_counts(&g_cache, &before);
    for (n = 0; n < 2 * WL_XBOX_PSH_CACHE_SETS * WL_XBOX_PSH_CACHE_WAYS; n++) {
        program_random(flood, &flood_textures);
        (void)wl_xbox_psh_cache_program(&g_cache, flood, 3, &flood_textures, &usable, &translated);
    }
    wl_xbox_psh_cache_counts(&g_cache, &after);
    ASSERT(after.evicted > before.evicted + WL_XBOX_PSH_CACHE_SETS * WL_XBOX_PSH_CACHE_WAYS);
    ASSERT(wl_xbox_psh_cache_recent(&g_cache, words, 3, &textures, &found_usable) == NULL);
    // (and the state itself made again, as wl_xbox_psh_cache_program gives it)
    program = wl_xbox_psh_cache_program(&g_cache, state, 3, &textures, &usable, &translated);
    ASSERT(translated);
    PASS();
}

// the caller's words as the device has them for the memo: the structure's words a function of the state's
// (here turned), the constants as the state holds them
static void memo_words_of(const uint32_t *state, uint32_t *words) {
    for (int i = 0; i < WL_XBOX_PSH_WORDS; i++) {
        bool constant = (i >= WL_XBOX_PSH_CONSTANT0_0 && i < WL_XBOX_PSH_CONSTANT1_0 + 8) ||
                        i == WL_XBOX_PSH_FINAL_CONSTANT0 || i == WL_XBOX_PSH_FINAL_CONSTANT1;
        words[i] = constant ? state[i] : state[i] ^ 0x5A5A5A5Au;
    }
}

static WlXboxPshCache g_plain;

// a device's call: the memo, else the cache and the call remembered
static const WlXboxPshProgram *memo_call(WlXboxPshCache *cache, const uint32_t *state,
                                         const WlXboxPshTextures *textures, bool *usable) {
    uint32_t words[WL_XBOX_PSH_WORDS];
    const WlXboxPshProgram *program;
    bool translated;

    memo_words_of(state, words);
    program = wl_xbox_psh_cache_recent(cache, words, 3, textures, usable);
    if (program == NULL) {
        program = wl_xbox_psh_cache_program(cache, state, 3, textures, usable, &translated);
        wl_xbox_psh_cache_remember(cache, words, 3, textures);
    }
    return program;
}

// wl_xbox_psh_cache_set_memo: the calls remembered by their structure give what the cache gives without
// them (a cache of its own: the same programs, byte for byte, and fresh translations), for programs drawn
// in turn with their constants changed within their signatures (patched by the memo) and outside them,
// with the fog's colour changed, and after more structures than the cache holds
TEST the_memo_gives_what_the_cache_gives(void) {
    static uint32_t states[3][WL_XBOX_PSH_WORDS];
    WlXboxPshTextures textures[3];
    WlXboxPshCacheCounts counts, plain;
    unsigned long differ = 0, fresh_differ = 0, calls = 0;

    wl_xbox_psh_cache_init(&g_cache, NULL);
    wl_xbox_psh_cache_init(&g_plain, NULL);
    wl_xbox_psh_cache_set_memo(&g_cache, true);
    for (int n = 0; n < 900; n++) {
        int which = n % 3;
        bool usable, plain_usable, translated;
        const WlXboxPshProgram *program, *expected;

        if (n % 30 < 3) {
            program_random(states[which], &textures[which]);
        } else if (rnd() % 4u == 0) {
            constants_change(states[which], &textures[which], rnd() % 4u == 0 ? false : true);
        }
        if (n >= 600 && n % 60 == 0) {
            // (more structures than the cache holds: the remembered calls' entries pushed out)
            for (int flood = 0; flood < WL_XBOX_PSH_CACHE_SETS * WL_XBOX_PSH_CACHE_WAYS; flood++) {
                static uint32_t other[WL_XBOX_PSH_WORDS];
                WlXboxPshTextures other_textures;

                program_random(other, &other_textures);
                (void)memo_call(&g_cache, other, &other_textures, &usable);
                (void)wl_xbox_psh_cache_program(&g_plain, other, 3, &other_textures, &usable, &translated);
            }
        }
        program = memo_call(&g_cache, states[which], &textures[which], &usable);
        expected = wl_xbox_psh_cache_program(&g_plain, states[which], 3, &textures[which], &plain_usable, &translated);
        calls++;
        if (usable != plain_usable || (usable && memcmp(program, expected, sizeof *program))) {
            differ++;
        }
        if (n % 7 == 0) {
            bool fresh_usable = wl_xbox_psh_translate(states[which], &textures[which], NULL, &g_fresh);

            if (usable != fresh_usable || (usable && memcmp(program, &g_fresh, sizeof g_fresh))) {
                fresh_differ++;
            }
        }
    }
    wl_xbox_psh_cache_counts(&g_cache, &counts);
    wl_xbox_psh_cache_counts(&g_plain, &plain);
    ASSERT_EQ(0ul, differ);
    ASSERT_EQ(0ul, fresh_differ);
    // (found by the memo, some of them patched; fewer translations than calls)
    ASSERT(counts.recent > calls / 4);
    ASSERT(counts.patched > 0);
    ASSERT(counts.evicted > 0);
    ASSERT_EQ(plain.audits_differ, counts.audits_differ);
    PASS();
}

SUITE(psh_cache_suite) {
    RUN_TEST(a_translation_by_way_is_the_translation);
    RUN_TEST(cached_programs_are_fresh_translations);
    RUN_TEST(the_last_call_and_the_extra_word);
    RUN_TEST(the_cache_translates_through_its_recipe);
    RUN_TEST(the_recent_calls_found_by_the_callers_words);
    RUN_TEST(the_memo_gives_what_the_cache_gives);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(psh_cache_suite);
    GREATEST_MAIN_END();
}
