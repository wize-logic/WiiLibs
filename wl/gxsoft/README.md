# wl/gxsoft: a software GX for the host

Target `wl_gxsoft` (alias `WiiLibs::gxsoft`), header `wl/gxsoft/gxsoft.h`; host and ppc-be only. It
depends on `wl_platform` (its host backend) and `wl_codec` (GX's texture formats).

On the host, wl/platform's GX layer is a recorder: it keeps what was asked (`host/gx_host.h`,
`host/gx_direct_host.h`, `host/tev_host.h`, `host/gx_target_host.h`) and draws nothing. Attached,
the software GX is that recorder's renderer (`host/gx_renderer_host.h`): every primitive the direct
layer is handed is drawn with the state recorded for it, into an EFB of GX's size, the way the
console's GX would draw it; frames are shown at `wl_gx_end_frame` (the display copy), render targets
are copied out of the EFB and put back, `wl_gx_peek_rgba` reads its pixels. So a renderer built on
the direct layer (wl/xbox's NV2A backend, `wl_xbox_gx`) can be looked at without a Wii or Dolphin:
`tools/xbox_nv2a_replay.c --png-gx` draws a title's captured frames with it, and tests check pixels.

```c
static void on_frame(void *user, const uint8_t *rgba, uint32_t w, uint32_t h) { /* a PNG, say */ }

WlGxSoftConfig cfg = {.frame = on_frame};
wl_gx_init();
wl_gxsoft_attach(&cfg);   // from here on, GX calls draw
...                       // the program's frames
wl_gxsoft_detach();
```

## What it models

It is faithful to GX, not to whatever the program meant: a mistake in the state reaches the
picture as it would on the console, and the draw is flagged (`WlGxSoftDraw.issues`: a TEV program
the validator refuses, a colour channel or texture coordinate a stage reads that the layout does not
send, an empty texture map, a TLUT not loaded).

- **Transform.** The position through the position matrix (the identity unless
  `wl_gx_direct_position_matrix` set one) and the projection as GX keeps it: six numbers of the
  matrix (`GX_LoadProjectionMtx`), orthographic (w 1) or perspective (w = -z); clipped where z'
  leaves [-w, 0] (GX's near and far planes) or w reaches 0; the viewport with depth 1 + z'/w.
  Texture coordinates through identity generators (one the layout does not send from the position,
  as the Wii backend makes it), or a 2x4 matrix's (`wl_gx_direct_texgen`) from the vertex's
  coordinate (s, t, 1, 1) or its position as sent; colours unlit from the vertex.
- **Rasterizing.** Triangles, strips, fans, quads; lines and points a pixel wide. Culling by the
  winding on the screen: clockwise (y down) is GX's front face, so `GX_CULL_FRONT` discards it, as
  Dolphin measures it. Pixels sampled at 7/12 of a pixel (Dolphin's centre), the top-left rule,
  attributes interpolated for the perspective and the depth across the screen.
- **Textures.** Each TEV stage's own lookup (its map at its coordinate, `wl_tev_eval_stages`),
  decoded from GX's formats by wl/codec (CMPR blended 5/8 : 3/8, C4/C8 through their TLUT) and kept
  until the texture cache is emptied and their bytes changed; the ramp (`wl_gx_ramp_bind`). Clamp,
  repeat and mirror (clamp for a size not a power of two, as the Wii backend forces), nearest and
  bilinear filtering with GX's 7-bit fractions, mipmaps by a level of detail from the coordinates'
  derivatives (four fractional bits), the sampler's bias.
- **Pixels.** The TEV program stage by stage (wl/platform's evaluator: konst, swap tables, colour
  channels, registers, bias, scale, clamps, compares), its output's eight low bits as the pixel
  engine takes them; the alpha compare; the depth test (24 bits) before or after texturing, its
  update only with the test, as GX; blending (GX's factors, each side's colour factor the other's,
  subtract); the colour and alpha updates.
- **The EFB.** 640 by 528 pixels, RGBA6_Z24 (six bits a channel) or RGB8_Z24 (alpha reads 255) as
  `wl_gx_set_efb_alpha` chose; copies into textures in GX's tile order (RGBA8, RGB565, RGB5A3, I4,
  I8, IA8, Z24X8), with their clear; a parked back buffer put back and clear quads within the
  scissor (the host recorder also keeps the state the console's quads leave); the display copy of
  the back buffer's part of the EFB.

Not modelled: indirect texturing, fog, logic ops, dithering, anti-aliasing, the display copy's
vertical filter, gamma and YUV, GX's texture cache (memory is read when a draw is made).

## Checked against

`tests/gxsoft/`: pixels of each part (fill rule, culling, clipping, projection, depth, blending,
the EFB's formats, sampling and filtering, levels, TEV stages reading one map at two coordinates,
copies, quads, the display copy), on the host and ppc-be. `tests/xbox/test_xbox_nv2a_pixels.c` runs
wl/xbox's console suite (`tests/xbox/suite_nv2a.c`, whose expectations are the EFB pixels Dolphin
produced) whole on it. A frame of Halo's title screen replayed from a capture matches Dolphin's dump
of the same frame to a mean of under one level a channel (0.3% of pixels more than 16 apart, at
edges, Dolphin's picture being scaled to 528 lines).
