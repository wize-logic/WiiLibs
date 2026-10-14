# wl/xbox: the Xbox's NV2A on GX

Target `wl_xbox` (alias `WiiLibs::xbox`) depends on `wl_platform` (its TEV programs,
`wl/platform/tev.h`), `wl_gfx` (`wl/gfx/gfx_sqrt.h`) and `wl_codec` (the Xbox's textures). What a
port or a recompilation of an Xbox (Direct3D 8) game needs to draw on the Wii, which has no
programmable stages: the pixel shaders' register combiners become TEV programs, the vertex programs
run on the processor, and, for a title whose own Direct3D runs unchanged, the NV2A itself is a
device that executes the title's push buffers ("The NV2A as a device", below). Game-agnostic: a
game's own knowledge (programs it recognizes and makes by hand, its list of vertex programs) stays
with the game. Headers: `wl/xbox/psh.h`, `wl/xbox/psh_cache.h`, `wl/xbox/vsh.h`,
`wl/xbox/vsh_native.h`, and the NV2A's `nv2a.h`, `nv2a_regs.h`, `nv2a_kelvin.h`, `nv2a_capture.h`,
`nv2a_texture.h`, `nv2a_texcache.h`, `nv2a_pipeline.h`, `nv2a_record.h`. Built with
`-ffp-contract=off` (`DETERMINISTIC`), so a vertex program gives the same bits interpreted or
compiled, on every target.

Target `wl_xbox_gx` (alias `WiiLibs::xbox_gx`, header `nv2a_gx.h`) is the NV2A backend on GX. It calls
only wl/platform (its direct GX layer, `wl/platform/gx_direct.h`, render targets and TEV programs), so
it builds everywhere: over libogc on the Wii, over the host's recorder elsewhere.

(The Xbox's textures as such are `wl/codec/xbox_tex.h`.)

## Pixel shaders (`psh.h`)

`wl_xbox_psh_translate` takes the combiners as the words of the Xbox's pixel shader render states,
in their order (`WL_XBOX_PSH_ALPHA_INPUTS0` .. `WL_XBOX_PSH_COMBINER_COUNT`, 57 words), and what
the device knows of the textures (`WlXboxPshTextures`: which stages sample one, which hold colour
or alpha only, cube maps sampled as sphere maps, bumped lookups, the fog and its colour, D0's alpha
from a texture), and makes a `WlXboxPshProgram`: the TEV program (`WlTevProgram`), the texture
coordinates and colour channels its stages read, each stage's part in a bumped lookup through GX's
indirect unit, and what the device must put in the vertices. `psh.c`'s comment says how: each
product a stage, registers renamed as they are written, inputs a stage cannot reach made first,
the colour and alpha halves sharing stages, dead halves taken out; signed dot products exact
through TEV's lerp, bias and scale; a half bias negate or expand negate (0.5 - x, 1 - 2x) made in
its product's stage, with no register of its own, and the alpha's reads of a colour's blue made
again where they are read (Halo's title-screen logo, all four TEV registers busy, is exact so). A
program beyond GX is refused (`reason`); one that only fits approximated says so (`approximate`,
`approximation`); of the attempts the translation makes, the first exact one is kept.

The fixed places the device and the translation agree on: the fog ramp is map 7 at texture
coordinate 4 (`WL_XBOX_PSH_FOG_*`); a bumped lookup's offset reads the bump map as map 5, its terms
at coordinates 1 and 2 (`WL_XBOX_PSH_BUMP_*`); D0's alpha from a texture is read at coordinate 5.

A caller can make programs by hand for combiners it recognizes (`WlXboxPshRecipe`, tried before the
translation): where the exact program needs more than the general translation can see, such as a
dot product of a register, which no TEV swap reaches and the translation takes as 1. Its
`vertices` values from `WL_XBOX_PSH_VERTICES_RECIPE` up are the caller's own.
`wl_xbox_psh_apply` writes a program to GX.

## The programs kept (`psh_cache.h`)

A translation is long, and a game may set its programs' constants for every object.
`WlXboxPshCache` (the caller's, static: it is large) keeps each program by its structure: the
combiners, the textures and each constant's signature (which channels are 0, 255 or equal, which
constants are equal). A structure whose program changes with its constants only in the konst and
preloaded register values that hold them (found by translating it again with other constants of
the same signatures) is generic: other constants are written into its program in place, and its first change and one in 32
after are translated anew and compared. `wl_xbox_psh_cache_counts` says what it did.
`extra` is a word of the caller's kept in the key (its texture modes, say).

## Vertex programs (`vsh.h`, `vsh_native.h`)

`wl_xbox_vsh_compile` decodes a program's instructions (four words each, as the Xbox's Direct3D
takes a vertex shader's function after its header word); `wl_xbox_vsh_run` runs it for a vertex
(the MAC and ILU units, r12 the position read back, a0's relative constants clamped to the 192),
leaving the screen-space position, the clip-space position before the conversion
(`clip_captured`), the colours, texture coordinates, fog and point size. `wl_xbox_vsh_run_live`
computes only the outputs a draw reads (`WL_XBOX_VSH_LIVE_*`), the same bits.

The analysis (`wl_xbox_vsh_analyze`, `_uniform`, `_constants`) walks a program with sets of input
registers in place of numbers: each output component's inputs, whether it is an affine function of
them (then GX's matrices can compute it), whether it reads a constant (one that does not is the
same whatever the draw's constants, so a static vertex buffer's can be kept), the constants read.

`tools/xbox_vsh_codegen.c`, a host tool, compiles a file of programs (the game's microcode, as C
text) to C: a function for each that does what the interpreter does, its operands fixed, for the
outputs a draw reads; one for its colours alone; and, for a program whose position reads constants
by a0 and is converted to the screen (a skinned model's), one for four positions at once. It writes
a table of `WlXboxVshNative`, which the program hands to `wl_xbox_vsh_compile`; a program found in
it by its words' hash (`wl_xbox_vsh_hash`) runs as its function. The table's name is the tool's
third argument (`xbox_vsh_natives` by default).

```sh
xbox_vsh_codegen microcode.inc programs.c [table]
```

## Tests

`tests/xbox/`: random combiner programs translated and simulated as TEV against the combiners
evaluated directly (none differs; a digest of every program, the same on ppc-be), recipes, fog,
bumped lookups; the cache against fresh translations, byte for byte; synthetic vertex programs
(`vsh_programs.py` assembles `vsh_programs.inc`) interpreted exactly, analysed, and compiled by
the tool at build time (under qemu on ppc-be) against the interpreter, bit for bit.

The OpenCE-Wii port (Halo) checks the translation further with the game's own programs.

## The NV2A as a device (`nv2a.h`)

A recompiled title keeps its own Direct3D (Halo's is D3D8 1.0.3925, statically linked), which writes
NV2A push buffers into contiguous guest RAM and kicks them by writing a channel's DMA_PUT through
MMIO. `WlNv2a` is the device behind that MMIO window:

```c
WlNv2a *wl_nv2a_create(const WlNv2aConfig *cfg);   // {phys_ptr, user, backend, [ram_bytes, time_ns]}
uint32_t wl_nv2a_mmio_read(WlNv2a *, uint32_t offset, unsigned size);   // offset from 0xFD000000
void     wl_nv2a_mmio_write(WlNv2a *, uint32_t offset, uint32_t value, unsigned size);
void     wl_nv2a_vblank(WlNv2a *);                   // the host's retrace
bool     wl_nv2a_irq_pending(const WlNv2a *);        // then the runtime runs the title's ISR
```

`phys_ptr(user, phys, bytes)` gives the host address of guest-physical bytes (little-endian,
contiguous for the length asked; `EA(0x80000000 | phys)` in the runtime), and every read the model
and its backends make of guest memory goes through it (`wl_nv2a_phys`). Pure C11, one thread at a
time (the guest lock); backend callbacks run inside the MMIO write that kicked the push buffer.

- **Registers** answer as an idle GPU's: the FIFO's caches empty (CACHE1_STATUS, RUNOUT_STATUS),
  PGRAPH not busy (STATUS 0), the write-combining flush done at once (PFB 0x100410's bit 16 reads
  clear), PTIMER counting (the configuration's clock, or a microsecond a read), the RAM's size in
  PFB_CSTATUS, the interrupt status registers (PMC, PFIFO, PGRAPH, PCRTC, PTIMER) showing what was
  raised and cleared by writing ones, PMC_INTR_0 their enabled sum. A write of PCRTC_START is the
  flip. Any other register reads back what was written. Instance memory (PRAMIN, 0x700000) is the
  device's own megabyte, made in 64 KiB pages as it is written (xemu keeps it apart from RAM too).
- **The pusher.** A write of DMA_PUT (0x800040 + chid * 0x10000) runs the channel's push buffer from
  GET to PUT before it returns, then GET = PUT: method headers increasing and non-increasing, old
  and new jumps, one level of call and return (envytools' "PFIFO DMA pusher"); a header's words may
  span kicks. Direct3D's start-up handshake works as on the hardware: GET starts at 0, a jump at
  physical 0 takes it to the push buffer, and GET is then PUT.
- **The puller** binds objects to subchannels by handle (RAMHT, searched whole), handles its own
  methods (the reference counter, NV1A semaphores: an acquire whose value is not there is counted
  and passed, as a synchronous GPU has nothing to wait for), looks up the handles of methods
  0x180-0x1FC and hands the 3D class's methods to the state tracker (other classes are logged once).
- **Semaphores, notifiers, reports** are written into guest memory: the 3D class's back-end
  semaphore release (0x1D6C/0x1D70, Direct3D's fences), NOTIFY's notifier (and PGRAPH's NOTIFY
  interrupt for a NOTIFY of 1), GET_REPORT's pixel count (what the backend says it drew while
  counting: the GX backend estimates the area).
- **Traps.** NO_OPERATION with a parameter raises PGRAPH's ERROR interrupt with the method and data
  in TRAPPED_ADDR/DATA, as nxdk's pbkit uses it for callbacks; with the interrupt enabled the channel
  stops after it, as the GPU stalls, and goes on once the ISR clears it.

The numbers (`nv2a_regs.h`) are WiiLibs' names for facts each cited to a public source (envytools'
docs and rnndb, nxdk's pbkit, nouveau); `tests/xbox/test_xbox_nv2a_regs.c` compares them by name
with another table when one is checked out beside WiiLibs (test-only: read as text, nothing kept).

## The 3D state and draws (`nv2a_kelvin.h`)

`WlNv2aKelvin` is every method's last word (`raw[method / 4]`, read with `wl_nv2a_k`), the vertex
program's 136 slots and 192 constants loaded through their windows (the slot moving on after the
fourth word), SET_VIEWPORT_SCALE/OFFSET also written into c[58]/c[59] (Direct3D's c[-38]/c[-37],
which its programs' epilogue reads), the context DMAs resolved, and the sixteen attributes' current
values. Helpers decode it: the surface (`wl_nv2a_surface`: addresses, clip, formats, samples, depth
range), the combiners as psh's 57 words (`wl_nv2a_psh_words`), the program from
SET_TRANSFORM_PROGRAM_START to its end bit.

A begin/end becomes draws (`WlNv2aDraw`): DRAW_ARRAYS' ranges and ARRAY_ELEMENT16/32's indices into
the vertex arrays (offset bit 31 selecting DMA B, stride bits 31:8), INLINE_ARRAY's words (the enabled
attributes in order, each at a multiple of its component's size), or immediate vertices
(SET_VERTEX3F/4F/4S, SET_VERTEX_DATA2F/2S/4UB/4S/4F: a write of attribute 0's last component completes
a vertex). `wl_nv2a_draw_fetch` gives a vertex as the program's sixteen input registers, every
array format decoded (D3DCOLOR's B, G, R, A to r, g, b, a; normalized and plain shorts; floats and
FLOAT2H; bytes; the 11:11:10 normal), a disabled attribute its current value. A long batch is handed
over in parts of whole primitives (a strip's parts even, so its winding holds).

## Backends

- **GX** (`nv2a_gx.h`, target `wl_xbox_gx`): OpenCE-Wii's Direct3D device on GX (d3d8_gx.c, CC0),
  reorganized around the NV2A's state. Vertices through the title's program on the processor, sent
  in screen space through an orthographic projection with the depth range, or, for a program that
  divides by w, as (x w, y w, -w) through a perspective projection whose depth a + b / w is fitted
  from the draw and kept by its transform: the clip-space positions the program gives four
  canonical inputs, bit for bit, which every draw of the same position and constants shares, so
  each uses the transform's fit, the same numbers, and a pass over the same geometry with an EQUAL
  or LEQUAL test passes as on the NV2A whatever was drawn between (`wl_nv2a_gx_last_projection`
  says which; the far plane moved so nothing is clipped there); indexed draws reuse the vertices
  run. The combiners through psh's cache (with a title's `WlXboxPshRecipe`), the fog psh's ramp, a
  program beyond GX texture 0 times colour. Blending, depth, alpha test and colour masks on GX
  (constant colours, MIN/MAX and src - dst approximated and counted); culling by the NV2A's rule on
  the screen winding (`wl_nv2a_cull`: SET_FRONT_FACE's CW is clockwise as the screen shows it, so
  Direct3D's D3DCULL_CCW keeps a quad wound clockwise on the screen; GX's sense measured once at
  start); stencil draws skipped and counted. The
  EFB is the back buffer; another surface (a multisampled one among them) takes it through
  `wl/platform/gx_target.h`, and a texture at its address then samples its image; once a title
  samples a surface it showed (a displayed picture as a texture), each finished back buffer is
  copied out of the EFB into a target at its address too, for the next frame. The frame's first
  full clear is GX's copy clear, others are quads. A frame is shown on FLIP_INCREMENT_WRITE, on a
  switch to another screen-sized surface, or when the display flips to the surface in the EFB.
- **Textures** (`nv2a_texture.h`, `nv2a_texcache.h`): a stage's registers decoded (swizzled, linear,
  DXT, cube, P8; levels and faces) and converted into GX's formats (wl/codec's exact conversions,
  the rest decoded to RGBA8: A4R4G4B4 keeps its four bits of alpha so a glyph cache's text stays
  smooth; DXT3/5's alpha a separate I4; P8 as C8 with a TLUT; a cube map made into psh's sphere
  map), kept by (address, format, size, palette) within a budget (4 MiB by default) or in an arena
  of their own (a heap over it: a texture makes room by evicting the least recently drawn; one
  without room is not tried again in that frame), and checked once a frame (every use once found
  rewritten): through a write watch where the caller has one (the runtime's MMU: a texture whose
  pages saw no store is as it was, its texels not read; one whose pages did is hashed, or made
  again if large), else by a hash of the guest's texels (XXH32's rounds: 32-bit multiplies).
- **The pipeline** (`nv2a_pipeline.h`) is what the GX backend does on the processor: programs
  compiled and kept, vertices run (or the fixed function's composite matrix), the NV2A's fog modes,
  what psh is told of the textures, each stage's GX coordinates, the GX mappings of the raster state.
  Those mappings (compare functions, blending, culling) also take the values themselves
  (`wl_nv2a_gx_blend_of`, `wl_nv2a_cull_of`, Direct3D's cull modes `wl_nv2a_cull_d3d`, GX's cull mode
  of the measured winding `wl_nv2a_gx_cull`) in `nv2a_raster.c`, which needs nothing else of wl_xbox:
  an Xbox Direct3D device's render states are the NV2A's values, and OpenCE-Wii's takes them so.
- **The recorder** (`nv2a_record.h`) digests and describes every clear, draw, frame and flip.

On the host, the GX backend's calls reach wl/platform's host backend, which records them; with
wl/gxsoft's software GX attached (`wl_gxsoft_attach`, `wl/gxsoft/README.md`) they are also drawn,
as the console's GX draws them, so a host build or a replayed capture shows the frames a Wii would
show (checked against the console suite's Dolphin pixels and against Dolphin's frame dumps of a
title). `wl_nv2a_gx_last_program` says what psh made of the last draw's combiners, for tools.

## Captures (`nv2a_capture.h`) and `tools/xbox_nv2a_replay.c`

`wl_nv2a_capture_frames(dev, &sink, n)` captures from the next flip to n flips after it: a snapshot of
the model's state, then every MMIO write and vertical blank it receives and every block of guest
memory it or its backend reads (unchanged blocks not repeated). The runtime gives a sink that writes
a file, for example:

```c
static WlResult file_write(void *user, const void *data, uint32_t bytes) {
    return fwrite(data, 1, bytes, (FILE *)user) == bytes ? WL_OK : WL_ERR_IO;
}
WlNv2aCaptureSink sink = {fopen("sd:/nv2a.capture", "wb"), file_write};
wl_nv2a_capture_frames(dev, &sink, 2);     // close the file once wl_nv2a_capture_active is false
```

`wl_nv2a_replay` runs a capture through a new model and any backend over a guest memory of its own.
The host tool prints what was drawn and, with `--png-gx`, each frame GX shows: the GX backend over
the host's software GX (wl/gxsoft), the display copy of its EFB.

```sh
xbox_nv2a_replay nv2a.capture -v --png-gx frames/ --programs programs.inc
xbox_vsh_codegen programs.inc programs.c     # the title's programs as C, for WlNv2aGxConfig.natives
```

With `-v` and `--png-gx`, each draw also says what its GX primitives covered and wrote (triangles
culled and clipped, pixels failing the depth or alpha test, the bounds written, a state GX takes as
it is though probably not what was meant: a colour channel or texture coordinate a stage reads that
the layout does not send, an empty texture map) and whether psh's program was beyond GX or
approximated. To look closer: `--efb-after N DIR` writes the EFB after the replay's draw N,
`--gx-dump N` prints what GX is handed for its primitive N (projection, viewport, raster state,
texture maps, the TEV program, the first vertices) and `--gx-textures DIR` writes those maps,
`--psh-dump N` prints draw N's combiner words and psh's program.

`--textures-mb N` keeps the converted textures in an arena of N MiB as the runtime does, and
`--watch` checks them through a write watch made from the capture's memory blocks (a page counts as
stored to when a block changes bytes in it), as the Wii's MMU checks them: with both, a replay
times the texture cache as the console runs it. The summary line `textures:` gives its counts.

`--programs` writes each vertex program the capture drew with, once, as `tools/xbox_vsh_codegen.c`'s
input: a title project compiles them ahead of time and hands the table to the GX backend, so its
programs run as functions rather than through the interpreter.

## The NV2A's tests

`tests/xbox/test_xbox_nv2a*.c` (host and ppc-be): the registers, the pusher (jumps, calls, a header
across kicks, a ring's wrap), Direct3D's handshake, RAMHT and DMA objects, semaphores, notifiers,
reports, the vblank and flips, a trap, a capture replayed (`test_xbox_nv2a.c`); every vertex format,
each kind of draw, long batches, the windows, the combiner words, the surface (`_kelvin`); texture
registers, conversions sampled against the texel decoders, levels, DXT and P8, sphere maps, the
cache (`_texture`); the GX backend over the host's recorder: vertices as GX gets them, the TEV
program, textures, the perspective path, culling, stencil, clears, render targets (`_gx`); the GX
backend drawn by the host's software GX from push buffer to pixel: the console suite whole, with
its Dolphin expectations, Direct3D's culling of screen-space quads, a program whose final product
needs every TEV register, and a capture replayed to the same pixels (`_pixels`); the numbers
against another table (`_regs`). The replay tool runs over the capture the pixel test writes. On
the console, `tests/xbox/suite_nv2a.c` (in `wl_console_tests.dol`) draws synthetic frames
through the GX backend on the Wii's GX and reads the EFB back.

## Not handled yet

Stencil (GX has none: those draws are skipped); the fixed-function transform beyond the composite
matrix (no lighting, no texgen); point sprites, line widths; the NV2A's dependent texture modes
(bump-env, dot products) beyond what psh does; the 2D classes (M2MF, image blit: logged); surfaces
in guest memory (a render target's pixels are on the host: a title reading them back with the CPU
sees what was there); occlusion counts with a depth test (the GX backend counts area); the
pixel-centre offset is OpenCE-Wii's +0.5 and wants checking against a title's viewport constants.
