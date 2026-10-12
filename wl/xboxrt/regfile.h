// SPDX-License-Identifier: MIT
// A sparse register file for device models: 32-bit words by offset, 0 until written, sub-word
// accesses merged into their word. Open addressing over a fixed table (internal to wl/xboxrt).
#pragma once

#include "wl/base/base.h"

typedef struct {
    uint32_t *keys; // offset | 1 (0: empty)
    uint32_t *vals;
    uint32_t cap;   // a power of two
    uint32_t used;
    bool full_warned;
    const char *name;
} WlXbRegFile;

bool wl_xb_regfile_init(WlXbRegFile *rf, uint32_t cap, const char *name);
uint32_t wl_xb_regfile_get32(const WlXbRegFile *rf, uint32_t offset); // offset & ~3
void wl_xb_regfile_set32(WlXbRegFile *rf, uint32_t offset, uint32_t value);
// Sized accesses at any alignment inside a word (size 1, 2, 4).
uint32_t wl_xb_regfile_read(const WlXbRegFile *rf, uint32_t offset, unsigned size);
void wl_xb_regfile_write(WlXbRegFile *rf, uint32_t offset, uint32_t value, unsigned size);
// Whether the word was ever written.
bool wl_xb_regfile_has(const WlXbRegFile *rf, uint32_t offset);
