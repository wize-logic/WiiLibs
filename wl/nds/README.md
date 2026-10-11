# wl/nds — Nintendo DS ROM, NARC and LZ

Read-only, non-owning views over a Nintendo DS cartridge image held in memory, plus the Nintendo
LZ decoders. Portable C11 (`wl_base` only, target `wl_nds` / `WiiLibs::nds`); no allocation
except the decoders' output and the FNT walk's scratch. Every input is untrusted: all offsets and
sizes are bounds-checked and bad data returns `WL_ERR_FORMAT` / `WL_ERR_RANGE`, never crashes.

## API

| Header | What |
|---|---|
| `nds.h` | `wl_nds_open` (header + range checks), `wl_nds_file` (FAT entry bytes), `wl_nds_walk` (every named file with its path), `wl_nds_overlay` / `wl_nds_overlay_data` (ARM9/ARM7 overlay tables); NARC: `wl_narc_is_narc`, `wl_narc_open`, `wl_narc_member`, `wl_narc_walk`; `wl_nds_survey` (dev smoke test: walks everything, opens NARCs, trial-decodes compressed data, writes "path size" lines to an optional `FILE *`) |
| `fnt.h` | `wl_fnt_walk`: the Nitro file-name-table walker shared by the ROM and NARC `BTNF` |
| `lz.h` | `wl_lz_decode` (LZ10 `0x10` / LZ11 `0x11`), `wl_lz_looks_valid`, `wl_blz_decode` / `wl_blz_footer_ok` (backward LZ used by ARM9 and overlays) |

The caller keeps the ROM / NARC buffer alive while views are used. Decoded buffers are
`malloc`'d; free them.

## Format notes (little-endian)

- **Header** (≥ 0x200 bytes): game code at 0x0C; ARM9 offset/entry/RAM/size at 0x20..0x2C; ARM7
  at 0x30..0x3C; FNT offset/size 0x40/0x44; FAT offset/size 0x48/0x4C; ARM9 overlay table
  0x50/0x54; ARM7 overlay table 0x58/0x5C.
- **FAT**: 8-byte (start, end) pairs, absolute ROM offsets.
- **FNT**: a main table of 8-byte directory records (sub-table offset u32, first file id u16,
  parent / total-dir-count u16; the root's last field is the directory count), then sub-tables
  of entries: a length/type byte (bit 7 = directory, low 7 bits = name length, 0 = end), the
  name, and for directories a u16 id `0xF000 + n`. Walk depth is capped at 32, paths at 1024
  bytes, and every directory may be entered once (cycles are errors).
- **Overlay entry** (32 bytes): id, RAM address, RAM size, BSS size, static-init start, end, file
  id, and a final word with the compressed size in bits 0..23 and the "compressed" flag in bit 24.
- **NARC**: `"NARC"`, BOM 0xFFFE, version, file size, header size (16), section count; then
  `BTAF` (u16 count, (start, end) pairs relative to the GMIF data), `BTNF` (an FNT; a nameless
  archive has an empty root list) and `GMIF` (member bytes).
- **LZ10/LZ11**: type byte, 24-bit size (0 = a 32-bit size follows), flag bytes MSB first,
  a set bit is a back-reference. LZ10: 4-bit length + 3, 12-bit displacement + 1. LZ11 adds the
  extended lengths (`0x11+` and `0x111+`). Declared sizes are capped at 256 MiB and by `max_out`.
- **BLZ**: footer in the last 8 bytes: u32 (compressed length in bits 0..23 | header length in
  bits 24..31) and u32 size increase. Decoding runs backward from the end; bytes before the
  compressed region are copied verbatim.

References: GBATEK ("DS Cartridge Header", "DS Cartridge Nitro File System", "BIOS
Decompression Functions"); community NARC notes (e.g. Tinke).

## Limits

Read-only (no NDS/NARC writer, no LZ encoder). `wl_nds_survey` counts trial-decode failures of
LZ candidates as informational only, because arbitrary data can start with 0x10/0x11.
