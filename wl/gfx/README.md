# wl/gfx: graphics maths, paired singles and GX texture codecs

Target `wl_gfx` (alias `WiiLibs::gfx`). Builds on every platform (host, `wii`, `ppc-be`); it depends
on `wl_codec`, libc and libm. Headers: `wl/gfx/gfx_math.h`, `wl/gfx/gfx_ps.h`, `wl/gfx/tex_codec.h`.

## gfx_math: vectors, matrices, cameras, culling

- Matrices use GX's layout, so they go to the GPU unchanged. `WlMat34` is a row-major 3x4 position
  matrix (libogc `Mtx`, with the translation in the fourth column). `WlMat44` is a row-major 4x4
  projection (`Mtx44`).
- The coordinate system is right-handed with +Y up, and the camera looks down -Z (as `guLookAt` does).
  Clip z maps near..far to -1..0 (as `guPerspective` and `guOrtho` do).
- `wl_mat34_mul(out, a, b)` applies `b` first. `out` may alias either input.
- `wl_mat34_from_trs` builds T * R(q) * S, the usual bone matrix.
- `WlCamera` describes a perspective or orthographic camera. It provides the view and projection
  matrices, orbit placement, projection of world points to viewport pixels and a world-space
  frustum (`WlFrustum`).
- Frustum tests are available for spheres, world AABBs and model-space AABBs under any affine
  matrix (`wl_frustum_aabb_xform`). For the model-space test the planes are carried into model
  space, so the box is never transformed. All of these tests are conservative.
- The code is plain C, so it runs in host tests. It uses floats, so do not use it for logic that
  must be bit-identical across machines.

## gfx_ps: paired-single skinning routines

`wl_mtx_concat` (3x4 times 3x4), `wl_skin_points` and `wl_skin_normals` are written in inline
assembly for the Gekko/Broadway paired-single unit. They use `psq_l`/`psq_st` with the default
float quantisation of GQR0 (the setting libogc leaves), plus `ps_muls0`, `ps_madds0`, `ps_madds1`,
`ps_madd`, `ps_sum0` and `frsqrte` with two Newton steps. No SDK header is involved.

- The assembly is compiled only when `__wii__` is defined. Every other build, including `ppc-be`,
  uses the plain C twins (`*_ref`). Those twins are also compiled on the Wii, so a test on the
  console can compare the two paths.
- `wl_ps_available()` reports which path the plain names run.
- Skin matrices (`WlSkinMtx`) store the 3x4 matrix as column pairs:
  `(m00 m10) (m01 m11) (m02 m12) (m03 m13) (m20 m21) (m22 m23)`. With this layout a vertex needs
  six paired loads.
- `wl_skinmtx_blend` computes an envelope matrix as the weighted sum of up to four bone matrices.
  It is plain C.
- Normals are renormalised. The squared length is clamped at 1e-12, so a zero normal stays zero
  instead of becoming NaN.

## tex_codec: GX texture encode / decode at run time

This module encodes and decodes every GX texture format: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8,
CMPR, C4 and C8, plus TLUTs in IA8, RGB565 and RGB5A3. It is meant for textures generated at run
time (ramps, placeholders) and for tests. It is not a fast path. The functions are thin wrappers
over the shared codec in `wl/codec/gx_tex.h` (see `wl/codec/README.md`).

- `WlTexFormat` is this module's own enum, not the GX hardware number. `wl_tex_gx_format` maps it to
  `GX_TF_*`.
- Tiles are 8x8 for I4, C4 and CMPR, 8x4 for I8, IA4 and C8, and 4x4 for IA8, RGB565, RGB5A3 and
  RGBA8. A tile is 32 bytes, except an RGBA8 tile, which is 64 bytes: 16 AR pairs, then 16 GB
  pairs. Within a tile, texels are row-major and big-endian. 4-bit texels put the left pixel in the
  high nibble.
- A CMPR 8x8 tile holds four DXT1-style 4x4 sub-blocks (two RGB565 endpoints, then 2-bit indices,
  MSB first). When c0 > c1 the block has four colours; otherwise it has three colours plus
  transparent. The decoder blends the endpoints 5/8 : 3/8, as the hardware does. The encoder
  (integer only) takes the two most distant 565 colours of each block as the endpoints.
- Image sizes are rounded up to whole tiles. The padding repeats the image's last row and column
  (indexed formats: index 0). `wl_tex_bytes` and
  `wl_tex_bytes_mips` give the sizes.
- The quantisers round to nearest. The decoders expand values by replicating the top bits, so exact
  values survive a round trip.
- `wl_tex_box_down` halves an image with a 2x2 box filter, for building mip chains.
