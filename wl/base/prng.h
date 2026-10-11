// SPDX-License-Identifier: MIT
// xoshiro128** 1.1 (Blackman and Vigna, public domain, https://prng.di.unimi.it/): the
// deterministic generator for deterministic simulation randomness. 32-bit operations only, so results
// are identical on Broadway and x86-64. Implementation: wl/base/prng.c.
#pragma once

#include <stdint.h>

// State is public so it can be embedded in saves and snapshots; treat as opaque otherwise.
// All-zero state is invalid (the generator would only output zeros).
typedef struct WlRng {
    uint32_t s[4];
} WlRng;

// Seeding: the 64-bit seed feeds SplitMix64 (Vigna, as recommended by the authors); two
// outputs a, b fill s = {lo(a), hi(a), lo(b), hi(b)}. Any seed, including 0, gives a valid state.
void wl_rng_seed(WlRng *rng, uint64_t seed);

// Next 32 random bits.
uint32_t wl_rng_next(WlRng *rng);

// Uniform in [0, bound), bound > 0 (returns 0 for bound 0). Unbiased: rejection sampling on
// the top of the 32-bit range, so the number of draws varies but the sequence is deterministic.
uint32_t wl_rng_below(WlRng *rng, uint32_t bound);
