# wl/gxtex: GX texture encoding and Nintendo DS graphics conversion

Target `wl_gxtex` (alias `WiiLibs::gxtex`) depends on `wl_codec` and `wl_base`. It is meant for offline tools,
but it is portable C with no platform calls, so it builds everywhere and its tests also run on the
big-endian build. Header: `wl/gxtex/gxtex.h`. Inputs are untrusted, so every offset and count is
checked. Output is deterministic.

## GX textures

`WlGxTex` holds one encoded level. `data` is in the exact block order GX reads (tiled and
big-endian), padded to whole tiles: RGBA formats repeat the image's last row and column, indexed and
I4 data pad with 0. C4 and C8 textures carry a big-endian RGB5A3 TLUT. The texel encoders and
decoders are the shared codec in `wl/codec/gx_tex.h`; this module wraps them in `WlGxTex`.

- **Encoders:**
  - `wl_gx_tex_from_rgba`: RGBA8, RGB5A3, RGB565 or CMPR.
  - `wl_gx_tex_from_indexed`: C4 or C8, with an RGB5A3 TLUT.
  - `wl_gx_tex_from_i4`: I4.
- **Decoder:** `wl_gx_decode` accepts raw untrusted bytes. `wl_gx_tex_decode` decodes a `WlGxTex`.
- **Tiles:**
  - I4, C4 and CMPR: 8x8.
  - C8: 8x4.
  - RGB565 and RGB5A3: 4x4.
  - Each of these tiles is 32 bytes.
  - RGBA8: 4x4 tiles of 64 bytes (16 AR pairs, then 16 GB pairs).
  - A 4-bit texel stores its left pixel in the high nibble.
- **CMPR:** each 8x8 tile holds four 4x4 sub-blocks. A sub-block is `u16 c0, u16 c1` (RGB565) plus
  4 bytes of 2-bit indices, MSB first. When c0 > c1 the sub-block has four colours, the two
  interpolants blended 5/8 : 3/8 and 3/8 : 5/8 as the hardware does. Otherwise it has c0, c1, their mean and transparent. The encoder picks the two most
  distant colours of a block, so blocks of at most two colours are exact up to RGB565 rounding.
- **RGB5A3:**
  - Pixels whose alpha rounds to opaque (alpha >= 237) use `1rrrrrgggggbbbbb`, which is exact for
    DS colours.
  - Other pixels use `0aaarrrrggggbbbb`. Every channel is rounded to nearest.
  - Pixels with alpha 0 keep a 4-4-4 colour.

## DS 2D resources

These files are little-endian. Each one starts with the magic as stored (`RLCN`, `RGCN`, `RECN`,
`RNAN`), the BOM bytes `FF FE`, a u32 size, a u16 header size of 16 and a u16 block count. Blocks
of `{tag, u32 size}` follow, and the size includes the 8-byte block header. The offsets below are
from the block start unless noted.

- **NCLR** (`TTLP`): `0x08` u16 depth (3 = 16 colours, 4 = 256). `0x10` u32 data size (when it is
  0 or too large, the data runs to the end of the block). `0x14` u32 data offset (from `0x08`). The
  entries are BGR555 (`r | g << 5 | b << 10`). The 16-colour banks are consecutive rows of 16.
- **NCGR** (`RAHC`): `0x08` u16 tiles high and `0x0A` u16 tiles wide (0xFFFF means unknown).
  `0x0C` depth (3 = 4 bpp, 4 = 8 bpp). `0x14` scan flag (1 = linear). `0x18` size. `0x1C` data
  offset (from `0x08`). A 4 bpp tile is 8x8 and 32 bytes, with the left pixel in the low nibble.
- **NCER** (`KBEC`): `0x08` u16 cells. `0x0A` attr (bit 0 means 16-byte cell entries with bounds).
  `0x0C` cell offset. `0x10` mapping (0..3: 1D with 32/64/128/256-byte tile units). `0x14`
  VRAM-transfer offset (0 = none).
  - A cell entry is: u16 objects, u16 attr, u32 object offset. In attr, bit 8 mirrors the whole
    cell horizontally and bit 9 vertically.
  - An object is three u16:
    - attr0: y (s8), bit 8 rotation, bit 9 hidden, bit 13 256 colours, bits 14-15 shape.
    - attr1: x (s9), bits 12-13 flips, bits 14-15 size.
    - attr2: tile (10 bits), bits 12-15 palette bank.
  - The VRAM-transfer table gives `{src, size}` for each cell. That NCGR window is loaded at tile 0,
    so an animated sprite can keep every frame's tiles in one NCGR.
- **NANR** (`KNBA`): `0x08` u16 sequences, u16 frames, then the sequence, frame and result offsets.
  - A sequence is 16 bytes: u16 frames, u16 loop start, u16 element, u16 type, u32 play mode,
    u32 frame-table offset. The play modes are 1 forward, 2 forward loop, 3 reverse and 4 reverse
    loop.
  - A frame is 8 bytes: u32 result offset, u16 duration in 60 Hz ticks. The first u16 of the
    result is the cell.

`wl_ds_render_cell` draws a cell into an RGBA canvas. Object 0 is on top and index 0 is
transparent. Rotation and scaling objects are refused.

**Sprite sheets.** `wl_ds_sprite_build` turns a cell set into a palettised sheet:

- Every cell becomes a frame the size of the union bounding box, rounded up to 8.
- The colours are palettised exactly to 15 plus transparent. A sprite with more colours keeps its
  15 most frequent RGB555 colours and maps the rest to the nearest (`lossy`).
- NANR sequences become animations. Reverse modes are reversed, the loop flag comes from the play
  mode, and `ticks = duration / tick_div` rounded half up, clamped to 1..255.
- Per-frame durations go to a hold table. A sequence whose cells are out of order, or whose holds
  conflict with another sequence, gets its own copies of the frames.

`wl_ds_cells_tex` stacks the cells top to bottom into one C4 texture, which suits two-frame icons.

## DS textures (TEX0)

A TEX0 block comes from a `BTX0` file, from the block list of a `BMD0` file, or is passed raw.

- **Header:**
  - `0x0C` u16 texture bytes >> 3, `0x0E` texture dictionary, `0x14` u32 texel data.
  - `0x1C` u16 compressed bytes >> 3, `0x24` compressed texels, `0x28` palette-index words.
  - `0x30` u16 palette bytes >> 3, `0x34` palette dictionary, `0x38` palette data.
- **Dictionaries:** `u8 rev, u8 count, u16 size, u16 8, u16 data offset`. At the data offset come
  `u16 unit, u16 name offset`, then the entries, then 16-byte names.
- **Texture entry param:**
  - Bits 0-15: offset >> 3.
  - Bits 20-22: width (8 << n). Bits 23-25: height.
  - Bits 26-28: format. Bit 29: colour 0 is transparent.
- **Palette entry:** `u16 offset >> 3`. A palette runs to the start of the next one or to the end of
  the block.
- **Formats:** 1 A3I5, 2 4-colour, 3 16-colour, 4 256-colour, 5 4x4 compressed, 6 A5I3, 7 direct.
  Texels are row-major, low bits first.
- **4x4 compressed:** each 4x4 block has a u32 of 2-bit texels and a u16 index word. Bits 0-13 of
  the word give the palette offset / 2, and bits 14-15 give the mode:
  - 0: c0, c1, c2 and transparent.
  - 1: c0, c1, their mean and transparent.
  - 2: four palette colours.
  - 3: c0, c1, (5 c0 + 3 c1) / 8 and (3 c0 + 5 c1) / 8.

`wl_tex0_decode` is exact. `wl_tex0_pair_palette` picks the palette for a texture, trying these in
order:

1. The palette named `<texture>_pl`.
2. The palette with the texture's own name.
3. The palette with the same index, when the counts match.
4. Palette 0.

**Conversion policy** (`wl_tex0_to_gx`):

- 4- and 16-colour textures become C4 and 256-colour textures become C8, each with an RGB5A3
  TLUT. This is exact.
- A3I5 and A5I3 become RGB5A3 when alpha is only 0 or 255, and RGBA8 otherwise. This is exact.
- Direct colour becomes RGB5A3.
- 4x4 compressed becomes exact RGB5A3, or CMPR with `WlTex0Policy.cmpr`.
- Transparent texels take the average colour of the opaque texels they border. A paletted
  texture's transparent entry takes that average too. This stops the GX bilinear filter from
  pulling the artist's key colour (magenta, lime) into cutout edges. The exact decode is unchanged.

## PNG

`wl_png_encode` writes an RGBA8 PNG with stored (uncompressed) deflate. It is meant for previews.
