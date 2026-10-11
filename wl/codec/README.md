# wl/codec: GX texture codec, Xbox texture conversion and DSP-ADPCM decoder

Target `wl_codec` (alias `WiiLibs::codec`) depends on `wl_base` only. Integer only, no allocation,
no platform calls; it builds everywhere and produces the same bytes on little- and big-endian CPUs
(its tests also run on the `ppc-be` build). Headers: `wl/codec/gx_tex.h`, `wl/codec/xbox_tex.h`,
`wl/codec/dsp_adpcm.h`.

It is the one implementation behind `wl/gfx`'s `tex_codec` (run-time textures), `wl/gxtex`'s
`WlGxTex` encoders and decoder, `wl/audio`'s DSP-ADPCM players and `wl/brstm`'s
`wl_brstm_dsp_decode`.

## GX textures (`gx_tex.h`)

- **Formats:** `WlGxcFormat` lists I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, then the
  colour-indexed C4 and C8. I4..RGBA8 have the GX_TF_* values; CMPR, C4 and C8 do not (callers
  map their own numbering). TLUT formats (`WlGxcTlutFormat`) are IA8, RGB565, RGB5A3 in GX_TL_*
  order.
- **Sizes:** `wl_gxc_tex_bytes` rounds up to whole tiles (8x8 for I4/C4/CMPR, 8x4 for I8/IA4/C8,
  4x4 for the rest; 32-byte tiles, 64 for RGBA8). 0 for a bad format or a dimension of 0 or above
  `WL_GXC_MAX_DIM` (16384).
- **Encoders:** `wl_gxc_encode` (RGBA8 pixels in) and `wl_gxc_encode_ci` (index bytes in; I4 data
  in 0..15 packs the same way as C4). They return the bytes written, 0 for bad arguments, a
  too-small `cap` or a format the call does not take. Tile padding repeats the image's last row
  and column, so a block on the edge sees no phantom colour; the indexed encoder pads with index 0.
- **Decoders:** `wl_gxc_decode` / `wl_gxc_decode_ci` take the source length and return the bytes
  read (0 when `src_len` is shorter than the image needs). They expand like the hardware: 565 and
  4-bit values replicate their top bits; I4/I8 put the intensity in all four channels, IA formats in
  RGB.
- **Colours:** `wl_gxc_rgb565_*` and `wl_gxc_rgb5a3_*` quantise with round to nearest, so exact
  values survive a round trip. RGB5A3 uses the opaque `1rrrrrgggggbbbbb` form when the alpha rounds
  to 7/7 (alpha >= 237), else `0aaarrrrggggbbbb`.
- **CMPR:** an 8x8 tile holds four 4x4 sub-blocks (`u16 c0, u16 c1` RGB565, then 2-bit indices,
  MSB first). When c0 > c1 the palette is c0, c1, `(5 c0 + 3 c1) >> 3` and `(3 c0 + 5 c1) >> 3`
  (the hardware's weights, not S3TC's 2/3 : 1/3); otherwise c0, c1, their mean and transparent
  black. `wl_gxc_cmpr_encode_block` is deterministic and integer only: the endpoints are the two
  most distant distinct 565 colours of the block, pixels with alpha < 128 select the transparent
  mode, so blocks of at most two colours are exact up to 565 rounding.
- **TLUTs:** `wl_gxc_tlut_encode` / `wl_gxc_tlut_decode` convert RGBA entries to big-endian 16-bit
  entries and back.

## Xbox textures to GX (`xbox_tex.h`)

What a port of an Xbox (Direct3D 8) game needs to turn the game's textures into GX's: the Xbox's
layouts, S3TC/DXT blocks and the GX format that shows each Xbox format as it is. All of it is
`wl_xbox_*`, on `WlXboxFormat` (A8R8G8B8, X8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, L8, A8, AL8, A8L8,
P8, DXT1, DXT3, DXT5). Xbox texels are little-endian; the output is GX's big-endian tiles.

- **Swizzle:** `wl_xbox_unswizzle` / `wl_xbox_swizzle` convert a power-of-two level between Morton
  order and linear rows for any texel size of 1..16 bytes (`wl_xbox_swizzle_masks` gives the index
  bits: x and y alternate, x first, and the longer side's bits continue above).
- **Formats:** `wl_xbox_to_gx` converts one level, swizzled or linear (with a row pitch), straight
  to the tiles of `wl_xbox_gx_format`: A8R8G8B8/X8R8G8B8 to RGBA8, R5G6B5 to RGB565, A1R5G5B5 and
  A4R4G4B4 to RGB5A3 (re-packed bit for bit, with no 8-bit round trip), L8/A8/AL8 to I8 (the byte is
  kept), A8L8 to IA8, P8 to C8. `wl_xbox_palette_to_tlut` turns the P8 palette (D3DCOLOR entries)
  into a TLUT of any `WlGxcTlutFormat`.
- **DXT to CMPR:** `wl_xbox_dxt_to_cmpr` is a lossless re-ordering: endpoints big-endian, each
  index byte's four 2-bit codes reversed (S3TC puts texel 0 in the low bits, CMPR in the high
  bits), four blocks to an 8x8 tile. DXT1 blocks keep their meaning. A DXT3 or DXT5 block is always
  four-colour mode, which CMPR is only when c0 > c1, so where c0 < c1 the endpoints are swapped and
  the indices XORed with 0x55 (0<->1, 2<->3), and where c0 == c1 every index becomes 0.
- **DXT alpha:** `wl_xbox_dxt_alpha_i4` makes DXT3/DXT5 alpha an I4 texture (top nibble; exact for
  DXT3), the partner of the CMPR colour when the two are bound as separate textures (what the
  OpenCE-Wii port does). `wl_xbox_dxt_alpha_block` is one block's 16 alphas.
- **DXT to a colour format:** `wl_xbox_dxt_to_gx` decodes the blocks (S3TC: 2/3 : 1/3, unlike
  CMPR's 5/8 : 3/8) straight to RGBA8, RGB5A3 or RGB565 (one 4x4 block is one GX tile, so there is
  no intermediate image). `wl_xbox_dxt_decode_block` is the decoder.
- **Padding:** unlike the `gx_tex.h` encoders, which repeat the edge, these converters move data
  that already is in blocks, so the tile padding is zero bytes (the DXT blocks past the image, the
  I4 texels past it). The padding is never sampled.
- **Reference:** `tests/codec/xbox_tex_cases.inc` holds 154 synthetic textures (every format,
  swizzled and linear with pitches, odd sizes, DXT blocks of every endpoint order) and the CRC-32 of
  what the OpenCE-Wii port's `tools/wii_gx_textures.py` makes of them. `tools/xbox_tex_reference.py
  --emit` regenerates the table; with `WL_XBOX_DUMP_DIR=DIR ./test_xbox_tex` and
  `xbox_tex_reference.py --compare DIR` the outputs are compared byte for byte.

## DSP-ADPCM (`dsp_adpcm.h`)

The GameCube/Wii DSP format: 8-byte frames of 14 samples. Header byte: predictor pair index (high
nibble; taken & 7) and scale exponent (low nibble), then 14 signed 4-bit samples, high nibble
first: `s = clamp16((nibble * (2048 << scale) + 1024 + c0 * h1 + c1 * h2) >> 11)` with `h1`, `h2`
the previous two outputs and `(c0, c1)` the pair (8 pairs per channel, units of 1/2048), in 64-bit
arithmetic. `WlDspState` holds the coefficients and history. `wl_dsp_decode` decodes whole frames;
`wl_dsp_decode_frame_from` starts mid-frame with a caller-chosen header byte (loop restarts).
