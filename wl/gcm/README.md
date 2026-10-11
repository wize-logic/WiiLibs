# wl/gcm — GameCube disc images

Reader for unencrypted GameCube disc images (GCM / ISO): boot.bin header, apploader size, the
file system table (FST) and main.dol. Target `wl_gcm` / `WiiLibs::gcm`. Portable (`wl_base`):
the image is read through a callback (`wl_gcm_open_src`) or a borrowed buffer
(`wl_gcm_open_mem`); on host builds `wl_gcm_open(path)` is added and `wl_host` linked. Inputs are
untrusted: offsets, sizes and names are bounds-checked and directory nesting is validated, so a
hostile FST cannot loop or read outside the image. Wii discs are recognised and refused (use
`wl/wiidisc`).

## API (`gcm.h`)

- `wl_gcm_open_src` / `wl_gcm_open_mem` / `wl_gcm_open` (host), `wl_gcm_close`
- `wl_gcm_read` (exact-size read at an image offset), `wl_gcm_read_range` (malloc'd, capped)
- `wl_gcm_walk` (every FST entry in order, directories before children, `/`-joined paths),
  `wl_gcm_count`, `wl_gcm_find` (case-sensitive path lookup, `WL_ERR_NOT_FOUND`)
- `wl_gcm_dol_size`, `wl_gcm_read_dol` (main.dol, at most 16 MiB)
- `WlGcm` fields: game code, maker, disc number, version, title, DOL / FST offsets and sizes,
  apploader size.

## Format notes (big-endian)

- **boot.bin** (0x440 bytes): game code 0x00 (4), maker 0x04 (2), disc number 0x06, version 0x07,
  Wii magic `0x5D1C9EA3` at 0x18, GameCube magic `0xC2339F3D` at 0x1C, title at 0x20 (64);
  DOL offset 0x420, FST offset 0x424, FST size 0x428, FST max size 0x42C.
- **bi2.bin** at 0x440 (0x2000 bytes); **apploader** at 0x2440: date[16], entry, body size,
  trailer size, then the body.
- **FST**: 12-byte entries, then the name table. Entry: type byte (1 = directory), 24-bit name
  offset, then for files (data offset, size) and for directories (parent index, index after the
  last child). Entry 0 is the root; its third word is the entry count.
- **DOL**: 7 text + 11 data sections; file offsets at 0x00, load addresses at 0x48, sizes at
  0x90. The file size is the furthest section end.

## Limits

Read-only; FST capped at 8 MiB, depth 64, paths 1024 bytes, names 255 bytes.
