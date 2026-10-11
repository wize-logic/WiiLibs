# wl/ctr — Nintendo 3DS containers

Readers for decrypted ("NoCrypto") 3DS images: NCSD (CCI), NCCH, ExeFS, RomFS (inside IVFC) and
GARC v4 archives. Target `wl_ctr` / `WiiLibs::ctr`. Portable (`wl_base`, plus `wl_nds` for the
LZ11 check); on host builds `wl_ctr_src_open` (a file source) is added and `wl_host` linked.
Images can be several GiB, so everything reads through a byte source with 64-bit offsets. All
inputs are untrusted.

## API (`ctr.h`)

| Area | Functions |
|---|---|
| Byte source | `WlCtrSrc` (`read`, `close`, `ctx`, `size` — fill it yourself for any medium), `wl_ctr_src_mem` (borrowed buffer), `wl_ctr_src_open` (host only), `wl_ctr_src_read`, `wl_ctr_src_close` |
| NCSD | `wl_ncsd_open`: eight partition (offset, size) pairs; partition 0 required |
| NCCH | `wl_ncch_open`: ExeFS / RomFS ranges; `WL_ERR_ENCRYPTED` when the NoCrypto flag is clear |
| ExeFS | `wl_exefs_open` (up to 10 files), `wl_exefs_read` (malloc'd copy) |
| RomFS | `wl_romfs_open` / `_close`, `wl_romfs_walk` (depth-first, UTF-8 paths), `wl_romfs_find_file` (hash-table lookup), `wl_romfs_read`, `wl_romfs_read_all` |
| GARC | `wl_garc_is_garc`, `wl_garc_open`, `wl_garc_mask` (sub-files present per entry), `wl_garc_sub`, `wl_garc_is_lz11` (decode with `wl_lz_decode` from `wl/nds/lz.h`) |
| Survey | `wl_ctr_survey`: NCSD → NCCH 0 → ExeFS count and RomFS walk; optionally opens every GARC and trial-decodes LZ11 sub-files (dev smoke test, counts only) |

## Format notes (little-endian)

- **NCSD**: magic `"NCSD"` at 0x100; partition table of eight (offset, size) u32 pairs at
  0x120, in 0x200-byte media units.
- **NCCH**: magic `"NCCH"` at 0x100; flags at 0x188 (flags[7] bit 0x4 = NoCrypto); ExeFS
  (offset, size) at 0x1A0, RomFS at 0x1B0, media units, relative to the partition.
- **ExeFS**: 0x200-byte header of ten (name[8], offset u32, size u32) records; data follows the
  header. `.code` may be backward-LZ compressed (not handled here).
- **RomFS**: `"IVFC"`, version 0x10000, master-hash size at +8, level-3 size (u64) at +0x44, block
  size log2 at +0x4C. Level 3 follows the master hash at 0x60, aligned up to the block size, and
  starts with a 0x28-byte header (size 0x28, then offset/length of dir hash, dir meta, file hash,
  file meta, and the file-data offset). Directory records: parent, sibling, first child dir, first
  child file, next-in-bucket, name length, UTF-16 name; file records: parent, sibling, data
  offset (u64), data size (u64), next-in-bucket, name length, name. The lookup hash is
  `h = parent ^ 123456789; for each UTF-16 unit: h = ror(h, 5) ^ unit`, bucket `h % count`.
  Hashes are not verified.
- **GARC v4**: header `"CRAG"` (size 0x1C, BOM 0xFEFF, version 0x0400, block count, data offset,
  file size, largest entry); `OTAF` (FATO: size, u16 count, u16 padding, one u32 per entry =
  offset into FATB's entry area); `BTAF` (FATB: size, sub-file count, then per entry a u32 bit
  mask and one (start, end, length) triple per set bit, start relative to the data); `BMIF`
  (FIMB: size, data size, data; sub-files padded to 4 bytes).

References: 3dbrew.org (NCSD, NCCH, ExeFS, RomFS, IVFC); community GARC notes.

## Limits

No decryption (no keys); IVFC hashes are not checked; only GARC version 4. Tables are capped
(64 MiB each, path 2048 bytes, depth 64).
