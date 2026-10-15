// SPDX-License-Identifier: MIT
// The NV2A's register combiners with their signed sums made unsigned, for a translation (psh.h)
// whose registers hold [0, 1] only. A program that averages half-biased inputs and puts the half
// back afterwards (Halo's blur of the screen: ((t0 - 1/2) + (t1 - 1/2)) / 2, the same of t2 and t3,
// their mean, plus 1/2) has registers below 0 in between, which GX's clamped registers lose (the
// picture comes out a flat grey). Each colour register is followed as an unsigned value plus an
// offset of its own (its true value is the two's sum): a half bias's -1/2, a constant 1/2, a signed
// read of a register with an offset, a halving output or a biased one add to the offset rather than
// to the value, and the program is written again with the unsigned values alone, once every read
// that needs the true value finds the offset 0 again. A program it cannot follow so exactly is
// left as it is.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wl/xbox/psh.h"

// `out` (WL_XBOX_PSH_WORDS words) the combiner state `state` with its offsets taken out; true when
// it differs from `state` and gives every result the NV2A's combiners give `state` (signed
// registers clamped to [-1, 1]), in a translation that clamps every register and mapping to
// [0, 1]. false, `out` undefined, otherwise.
bool wl_xbox_psh_unbias(const uint32_t *state, uint32_t *out);
