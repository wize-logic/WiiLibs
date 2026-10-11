# wl/gs — Genius Sonority FSYS archives

Readers for the archive formats of Genius Sonority's GameCube/Wii engine (Pokémon Colosseum,
XD and Battle Revolution): `GSfsys.toc` (archive index), FSYS archives and their LZSS entries.
Target `wl_gs` / `WiiLibs::gs`. Portable (`wl_base`). Data comes from memory or a read callback;
`wl/gs/gs_file.h` (header-only, host) adapts a `wl_host` `WlFile`. Inputs are untrusted: offsets,
counts and sizes are bounds-checked and decompression is capped (256 MiB per entry).

## API (`gs.h`)

- Source: `wl_gs_src_mem`, `wl_gs_src_cb` (callback over `[base, base + size)` of any medium),
  `wl_gs_src_read`; host: `wl_gs_src_file` in `gs_file.h`.
- LZSS: `wl_gs_lzss_decode` (raw stream, exact output size), `wl_gs_lzss_unpack` (16-byte header
  + stream, malloc'd).
- TOC: `wl_gs_toc_parse`, `wl_gs_toc_free`.
- FSYS: `wl_gs_fsys_open`, `wl_gs_fsys_close`, `wl_gs_fsys_read` (unpacked bytes, malloc'd),
  `wl_gs_fsys_verify`.
- Types: `wl_gs_game_from_toc_version`, `wl_gs_type_name` (names only where a game's handler
  table is known; otherwise `type_NN`).

## Format notes (big-endian)

- **GSfsys.toc**: `"GLLA"`; version +4, entry count +8, index offset +0x10, name-area offset
  +0x14. Entries are 8 bytes (id, name offset) for version 0x104 (Colosseum) and 16 bytes (id,
  name offset, size, flags) for 0x201 (XD) and 0x251 (Battle Revolution). Names are
  NUL-terminated at absolute offsets.
- **FSYS**: 0x60-byte header, `"FSYS"`; entry count +0x0C, file size +0x1C, offset of a u32
  table of record offsets +0x40. Record: id +0x00, data offset +0x04, unpacked size +0x08, flags
  +0x0C (bit 31 = compressed), packed size +0x14, file type +0x20, name offset +0x24 (0 = none).
- **LZSS** (Okumura variant): header `"LZSS"`, unpacked size +4, packed size including the
  header +8. Stream: 4 KiB zero-filled ring starting at 4078, flag byte read LSB first (1 =
  literal), a match is 2 bytes: 12-bit ring position + 4-bit length + 3. The decoded size must
  equal the entry's unpacked size.
- **File types**: ids 0x00–0x1A (XD's dispatch table has 27 records). Meanings differ per game;
  Battle Revolution: 2 scene, 3 object part, 4 material pack, 5 particle effect, 6 texture,
  9 stage script, 18 lens flare.

## Limits

Read-only (no FSYS writer or LZSS encoder); names are capped at 48 bytes (FSYS) and 64 (TOC).
