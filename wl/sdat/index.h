// SPDX-License-Identifier: MIT
// Canonical text index of an SDAT (host builds only: needs wl_host for SHA-1). One line per wave
// archive, bank, sequence, sequence archive, sub-sequence and stream (id, name, file id, size,
// SHA-1 of the file), the closure of every sequence and sub-sequence (its bank and the wave
// archives that bank names, with the bytes they add up to), and every reference that does not
// resolve. Ordering is by id, so the text and its digest are stable: a quick way to inventory an
// archive, diff two of them, or check that a rebuild is unchanged.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/host/sha1.h"
#include "wl/sdat/sdat.h"

typedef struct {
    uint32_t seq, seqarc, subseq, bank, wavearc, strm, player, group, player2, files;
    uint64_t waves, wave_by_enc[3], wave_other;
    uint32_t unresolved;      /* references that point at nothing (file, bank, wave archive) */
    uint32_t inst_unresolved; /* instrument notes whose wave is missing from its archive */
    uint64_t seq_bytes, bank_bytes, wave_bytes, strm_bytes;
} WlSdatSummary;

typedef struct {
    WlSdatSummary sum;
    char *text;  /* canonical index text, NUL-terminated, owned */
    size_t text_len;
    uint8_t digest[WL_SHA1_SIZE]; /* SHA-1 over the canonical text */
} WlSdatIndex;

/* Builds the index of an opened SDAT. Unresolved references are listed and counted, not errors;
 * WL_ERR_ARG / WL_ERR_NOMEM only. */
WlResult wl_sdat_index_build(const WlSdat *s, WlSdatIndex *ix);
void wl_sdat_index_free(WlSdatIndex *ix);
