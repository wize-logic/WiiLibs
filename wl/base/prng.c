// SPDX-License-Identifier: MIT
// xoshiro128** and SplitMix64 (see prng.h). Algorithms from the public-domain reference code.
#include "wl/base/prng.h"

static uint32_t rotl32(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void wl_rng_seed(WlRng *rng, uint64_t seed) {
    uint64_t sm = seed;
    uint64_t a = splitmix64(&sm);
    uint64_t b = splitmix64(&sm);
    rng->s[0] = (uint32_t)a;
    rng->s[1] = (uint32_t)(a >> 32);
    rng->s[2] = (uint32_t)b;
    rng->s[3] = (uint32_t)(b >> 32);
}

uint32_t wl_rng_next(WlRng *rng) {
    uint32_t *s = rng->s;
    const uint32_t result = rotl32(s[1] * 5u, 7) * 9u;
    const uint32_t t = s[1] << 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl32(s[3], 11);
    return result;
}

uint32_t wl_rng_below(WlRng *rng, uint32_t bound) {
    if (bound == 0) {
        return 0;
    }
    // Reject the first (2^32 mod bound) values so the remainder is uniform.
    const uint32_t threshold = (uint32_t)(0u - bound) % bound;
    for (;;) {
        uint32_t r = wl_rng_next(rng);
        if (r >= threshold) {
            return r % bound;
        }
    }
}
