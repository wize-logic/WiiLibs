# wl/sdat: Nintendo DS SDAT sound archive reader

Target `wl_sdat` (alias `WiiLibs::sdat`). Portable C11, big-endian safe (every field is read with
`wl_rd_le*`, no struct casts). Links `wl_base`; on host builds also `wl_host`, for the canonical
index (`index.h`, SHA-1).

| Header | What it provides |
|---|---|
| `sdat.h` | SDAT container (SYMB, INFO, FAT, FILE) and readers for SSEQ, SSAR, SBNK, SWAR/SWAV and a STRM check |
| `index.h` | host only: a canonical, stable text index of an archive with per-file SHA-1 and sequence closures |

All views are non-owning: they point into the SDAT bytes, which the caller keeps alive. Every input
is untrusted: offsets and counts are range checked, and malformed data gives `WL_ERR_FORMAT` /
`WL_ERR_RANGE`, never a crash (the tests mutate every header byte). Layouts come from community
SDAT format notes (GBATEK-style pages, vgmtrans/sdat2sseq documentation), read as documentation; no
code copied.

## API overview

```c
WlSdat s;
wl_sdat_open(&s, data, size);           /* header, SYMB (optional), INFO, FAT */
s.counts[WL_SDAT_SEQ]; s.seq[i].file_id; s.seq[i].bank; s.seq[i].name;  /* name NULL without SYMB */
s.bank[i].wavearc[0..3];                /* WL_SDAT_NONE for an empty slot */
s.seqarc_sub_names[i][k];               /* SSAR sub-sequence names from SYMB */

const uint8_t *f; size_t n;
wl_sdat_file(&s, file_id, &f, &n);      /* FAT entry -> bytes */
wl_sseq_open(&q, f, n);                 /* q.data, q.size: the sequence command bytes */
wl_ssar_open(&ar, f, n); wl_ssar_entry(&ar, k, &e);
wl_sbnk_open(&bk, f, n); wl_sbnk_inst(&bk, program, &inst);   /* WlSbnkInst is ~2 KiB: heap it */
wl_swar_open(&sw, f, n); wl_swar_wave(&sw, k, &wave);         /* encoding, rate, loop, data range */
wl_strm_check(f, n);
wl_sdat_close(&s);

WlSdatIndex ix;                         /* host builds */
wl_sdat_index_build(&s, &ix);           /* ix.text, ix.digest, ix.sum (counts, unresolved refs) */
wl_sdat_index_free(&ix);
```

## Layout (all little-endian)

SDAT header (0x40 bytes): `SDAT`, BOM `FEFF`, version, `u32` file size, `u16` header size, `u16`
block count (4), then `(u32 offset, u32 size)` of SYMB, INFO, FAT, FILE. The size recorded in the
header can be unpadded, so a block's own size field is its extent (it must still lie in the file).
SYMB is optional (offset and size 0 in stripped archives).

SYMB and INFO start with tag, size and eight `u32` list offsets (relative to the block) in the order
seq, seqarc, bank, wavearc, player, group, player2, strm. A list is a `u32` count then that many
`u32` offsets (0 = empty slot). SYMB entries point at NUL-terminated names; a SEQARC symbol entry
is a pair `(name offset, sub-name list offset)`.

INFO records:

| Kind | Record |
|---|---|
| seq | `u16 file id`, `u16`, `u16 bank`, `u8 volume`, `u8 channel priority`, `u8 player priority`, `u8 player`, 2 pad |
| seqarc, wavearc | `u16 file id`, `u16` |
| bank | `u16 file id`, `u16`, `u16 wave archive[4]` (`FFFF` = none) |
| player | `u16 max sequences`, `u16`, `u32 channel mask`, `u32 heap size` |
| group | `u32 count`, then 8-byte entries `u8 type` (0 seq, 1 seqarc, 2 bank, 3 wavearc), `u8`, `u16 load flags`, `u32 id` |
| player2 | `u8 count`, 16 channel bytes, pad |
| strm | `u16 file id`, `u16`, `u8 volume`, `u8 priority`, `u8 player`, pad |

FAT: `FAT `, size, `u32 count`, then 16-byte entries (`u32` offset from the SDAT start, `u32`
size, 8 reserved).

Sub-files share a header: tag, BOM `FEFF`, version, `u32` file size, `u16` header size (16), `u16`
block count, then a `DATA` block.

- SSEQ: `+0x18 u32` offset of the sequence command bytes (to the end of the file).
- SSAR: `+0x18 u32` offset of the shared command bytes, `+0x1C u32 count`, then 12-byte entries
  (`u32` offset of the sub-sequence, `u16 bank`, `u8 volume`, `u8 channel priority`, `u8 player
  priority`, `u8 player`, 2 pad).
- SBNK: `+0x38 u32 count`, then 4-byte records (`u8 type`, `u16 offset`, `u8`). Types: 1 PCM wave,
  2 PSG pulse (the wave field is the duty), 3 noise, 4 direct PCM, 5 null (all one 10-byte note
  record: `u16 wave`, `u16 wave archive slot`, `u8 key`, attack, decay, sustain, release, pan); 16
  drum set (`u8 low`, `u8 high`, then one 12-byte note per key: `u16 type` + the 10 bytes); 17 key
  split (8 region end keys, 0-terminated, then one 12-byte note per region).
- SWAR: `+0x38 u32 count`, then `u32` offsets to SWAVs (0 = unused slot). SWAV header (12 bytes):
  `u8 encoding` (0 PCM8, 1 PCM16, 2 IMA-ADPCM), `u8 loop`, `u16 rate`, `u16 timer`, `u16 loop start`
  and `u32 loop length` in 32-bit words (an ADPCM wave counts its 4-byte header in the loop start),
  then the data.
- STRM: only the signature and size are checked.

## Index text

`wl_sdat_index_build` writes `wlsdat-index 1`, a counts line, then in id order one line per wave
archive (size, SHA-1, wave counts per encoding), bank (instrument count, wave references and how
many are missing), sequence, the closure of every sequence (its bank and the wave archives the bank
names, with total bytes), sequence archive and sub-sequence (with its closure) and stream, and an
`end` line with the unresolved counts. A reference that does not resolve is printed as `UNRESOLVED`
and counted in `sum.unresolved`; it is not an error. The SHA-1 of the text is a stable fingerprint
of the archive's audio content.

## Limits

- No sequence interpretation (SSEQ commands are returned as bytes) and no wave decoding (SWAV data
  ranges are returned; PCM8/PCM16/IMA-ADPCM decoding belongs to the consumer).
- STRM streams are only signature-checked.
- Unknown SBNK instrument types give `WL_ERR_FORMAT` for that instrument only.
