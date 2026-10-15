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

The interpreter does no decoding as it runs: compile resolves each instruction once into a step
(`WlXboxVshStep`: each operand's register file, index, swizzle and sign, the registers and masks
each unit writes, the output written) and finds, backwards from the end, the outputs each one's
results reach (`wl_xbox_vsh_live_needs`, which the compiled functions skip by too). A run skips the
steps that reach no output the draw reads. `wl_xbox_vsh_run_batch` runs many vertices, 16 at a
time (`WL_XBOX_VSH_BATCH`, the registers in the caller's `WlXboxVshWork`), an instruction at a time
for all of them: a constant operand is made once for the batch, a temporary or input read in place
(swizzled or negated ones gathered first), each result written with its mask for every vertex.
Each vertex gets the same operations in the same order as a run of the program for it alone, so the
bits are the same (`test_xbox_vsh_batch.c` holds the plain interpreter and checks the batch against
it on random programs). `wl_xbox_vsh_inputs_read` says which input registers the steps a draw runs
read: the GX backend fetches only those.

The analysis (`wl_xbox_vsh_analyze`, `_uniform`, `_constants`) walks a program with sets of input
registers in place of numbers: each output component's inputs, whether it is an affine function of
them (then GX's matrices can compute it), whether it reads a constant (one that does not is the
same whatever the draw's constants, so a static vertex buffer's can be kept), the constants read.

`tools/xbox_vsh_codegen.c`, a host tool, compiles a file of programs (the game's microcode, as C
text) to C: a function for each that does what the interpreter does, its operands fixed, for the
outputs a draw reads (with its loop over a batch of vertices, so a call's entry and exit are paid once
for them); one for its colours alone; and, for a program whose position reads constants
by a0 and is converted to the screen (a skinned model's), one for four positions at once. It writes
a table of `WlXboxVshNative`, which the program hands to `wl_xbox_vsh_compile`; a program found in
it by its words' hash (`wl_xbox_vsh_hash`) runs as its function. The table's name is the tool's
third argument (`xbox_vsh_natives` by default). `--accessor name` also writes a function that gives
the table and its count; `--if-exists` takes a missing file as no programs (an empty table).

```sh
xbox_vsh_codegen [--if-exists] [--accessor name] microcode.inc programs.c [table]
```

A recompiled title gets this without writing anything: the runtime appends each program it draws
with that is not compiled in to `<hard disk folder>/vsh_programs.inc` (wl/xboxrt), and
`wl_add_xbox_title` compiles that list at build time when the title config names it
(`"vertex_programs": "hdd/vsh_programs.inc"`). Play the title once on the host, build again (for
the Wii too), and its programs run as functions. The list and the C are made from the game's
code: they stay in the title's folders, never in WiiLibs.

## Tests

`tests/xbox/`: random combiner programs translated and simulated as TEV against the combiners
evaluated directly (none differs; a digest of every program, the same on ppc-be), recipes, fog,
bumped lookups; the cache against fresh translations, byte for byte; synthetic vertex programs
(`vsh_programs.py` assembles `vsh_programs.inc`) interpreted exactly, analysed, and compiled by
the tool at build time (under qemu on ppc-be) against the interpreter, bit for bit; the batch
interpreter against the plain one on random programs and live sets.

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
  physical 0 takes it to the push buffer, and GET is then PUT. The rest of a header's run to the 3D
  object goes to the state tracker at once (`wl_nv2a_kelvin_methods`: a run of ARRAY_ELEMENT16/32
  indices straight into the batch), each word what the puller would make of it.
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
FLOAT2H; bytes; the 11:11:10 normal), a disabled attribute its current value. A backend decides a
draw's fetch once (`wl_nv2a_fetch_plan`: the attributes a program reads, each with its address,
stride and decoder) and fetches a batch of vertices an attribute at a time (`wl_nv2a_fetch_vertices`;
bytes and the packed normal's fields through tables of the same values), to the bit what
`wl_nv2a_draw_fetch` gives. A long batch is handed over in parts of whole primitives (a strip's parts
even, so its winding holds).

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
  run. A program whose position is, for the draw's constants, an affine function of v0 (an F3
  array) leaves the position to GX's transform unit: the vertex sent as fetched, the matrix (the
  viewport and samples folded in, so the eye vector is the processor's, for the same projection)
  from the program run for eight probe vertices once a draw; the probes give a linear transform's
  columns exactly, so two programs over the same geometry get the same matrix and their passes the
  same depths. When every other output the draw reads is affine too (a colour passed from an input
  or constant, a texture coordinate from one input of two components or from the position, no fog,
  no sphere map, a projected stage's q 1), the program is not run at all: the coordinates come from
  the transform unit's generators. Not to the bit (`no_xf` in `WlNv2aGxConfig` keeps every vertex on
  the processor; the runtime's `noxf` file). The combiners through psh's cache (with a title's `WlXboxPshRecipe`), the fog psh's ramp, a
  program beyond GX texture 0 times colour. Blending, depth, alpha test and colour masks on GX
  (constant colours, MIN/MAX and src - dst approximated and counted); culling by the NV2A's rule on
  the screen winding (`wl_nv2a_cull`: SET_FRONT_FACE's CW is clockwise as the screen shows it, so
  Direct3D's D3DCULL_CCW keeps a quad wound clockwise on the screen; GX's sense measured once at
  start); a draw with the stencil test is drawn as if the test passed (counted as `stencil_ignored`),
  one that writes only the stencil is skipped (`stencil_skipped`). The
  EFB is the back buffer; another surface (a multisampled one among them, or one at the back
  buffer's depth buffer, whose memory Halo's screen blur borrows) takes it through
  `wl/platform/gx_target.h`, and a texture at its address then samples its image (the back buffer's
  own address, while a target of its size has the EFB, samples its parked colour: the picture
  being drawn); once a title
  samples a surface it showed (a displayed picture as a texture), each finished back buffer is
  copied out of the EFB into a target at its address too, for the next frame. The frame's first
  full clear is GX's copy clear, others are quads. A frame is shown on FLIP_INCREMENT_WRITE, on a
  switch to another screen-sized surface, or when the display flips to the surface in the EFB.
- **Textures** (`nv2a_texture.h`, `nv2a_texcache.h`): a stage's registers decoded (swizzled, linear,
  DXT, cube, P8; levels and faces) and converted into GX's formats (wl/codec's exact conversions,
  the rest decoded to RGBA8: A4R4G4B4 keeps its four bits of alpha so a glyph cache's text stays
  smooth; DXT3/5's alpha a separate I4; P8 as C8 with a TLUT; a cube map made into psh's sphere
  map), kept by (address, format, size, palette) within a budget (4 MiB by default) or in an arena
  of their own (a heap over it, best fit: a texture makes room by emptying the oldest window of
  the arena wide enough for it, not every image by age; one without room even so is kept smaller,
  from its next mip level or a halved sphere map, up to three levels down, and made whole again
  once textures not drawn for 60 frames give way; one with no smaller image is not tried again in
  that frame), and checked once a frame (every use once found
  rewritten): through a write watch where the caller has one (the runtime's MMU: a texture whose
  pages saw no store is as it was, its texels not read; one whose pages did is hashed, or made
  again if large), else by a hash of the guest's texels (XXH32's rounds: 32-bit multiplies). A
  capture begun inside a frame has the textures and vertex arrays checked before it read again at
  their next use.
- **The pipeline** (`nv2a_pipeline.h`) is what the GX backend does on the processor: programs
  compiled and kept, vertices run (or the fixed function's composite matrix), the NV2A's fog modes,
  what psh is told of the textures, each stage's GX coordinates, the GX mappings of the raster state.
  Those mappings (compare functions, blending, culling) also take the values themselves
  (`wl_nv2a_gx_blend_of`, `wl_nv2a_cull_of`, Direct3D's cull modes `wl_nv2a_cull_d3d`, GX's cull mode
  of the measured winding `wl_nv2a_gx_cull`) in `nv2a_raster.c`, which needs nothing else of wl_xbox:
  an Xbox Direct3D device's render states are the NV2A's values, and OpenCE-Wii's takes them so.
- **Vertex arrays** (`nv2a_vcache.h`): a draw through the transform unit whose position, colours
  and texture coordinates come from static arrays (a level's geometry, drawn in several passes every
  frame) is sent as 16-bit indices alone: each array is converted once into big-endian arrays that
  GX reads by index (`wl_gx_direct_array`), kept by (address of element 0, stride, format, what it
  is converted as) and the range of elements, at most 65536. Like the textures' cache it is checked
  once a frame (the write watch, else a hash of the source), evicts the least recently drawn within
  its budget or arena (`WlNv2aGxConfig.vertices`; budget 0 is off) and gives memory back only once
  the GPU has finished the frame that may read it; an array found changed is converted again, one
  found changed twice (a skinned mesh) is not kept and its draws are sent as before. The runtime
  keeps 1 MB by default (`vcache`/`novcache`, `wl/xboxrt/README.md`).
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
`wl_nv2a_replay_stream` reads the capture from a source a piece at a time and may take the caller's
guest memory (its `phys_ptr`), into which the memory records go a 4 KB piece at a time where they
differ: the runtime's capture bench (wl/xboxrt's `bench.h`) replays over the title's paged memory so.
The host tool prints what was drawn (`-v` describes each clear, draw, frame and flip, `-vv` adds the 3D state of each draw) and, with `--png-gx`, each frame GX shows: the GX backend over
the host's software GX (wl/gxsoft), the display copy of its EFB.

```sh
xbox_nv2a_replay nv2a.capture -v --png-gx frames/ --programs programs.inc
xbox_vsh_codegen programs.inc programs.c     # the title's programs as C, for WlNv2aGxConfig.natives
xbox_nv2a_replay nv2a.capture --bench 10     # the processor's part alone, timed (ms a frame)
```

With `-v` and `--png-gx`, each draw also says what its GX primitives covered and wrote (triangles
culled and clipped, pixels failing the depth or alpha test, the bounds written, a state GX takes as
it is though probably not what was meant: a colour channel or texture coordinate a stage reads that
the layout does not send, an empty texture map) and whether psh's program was beyond GX or
approximated. To look closer: `--efb-after N DIR` writes the EFB after the replay's draw N,
`--gx-dump N` prints what GX is handed for its primitive N (projection, viewport, raster state,
texture maps, the TEV program, the first vertices) and `--gx-textures DIR` writes those maps,
`--psh-dump N` prints draw N's combiner words and psh's program.

`--textures-mb N` keeps the converted textures in an arena of N MiB as the runtime does,
`--vertex-cache-kb N` keeps the vertex arrays for GX to read by index in N KiB (none by default; the
summary gives a `vertex arrays:` line), and
`--watch` checks them through a write watch made from the capture's memory blocks (a page counts as
stored to when a block changes bytes in it), as the Wii's MMU checks them: with both, a replay
times the texture cache as the console runs it. The summary line `textures:` gives its counts.

`--programs` writes each vertex program the capture drew with, once, as `tools/xbox_vsh_codegen.c`'s
input: a title project compiles them ahead of time and hands the table to the GX backend, so its
programs run as functions rather than through the interpreter.

`--bench N` replays the capture N times through the GX backend alone (no software GX, no recorder;
the textures kept from one run to the next) and prints the best and the mean time a frame: the
vertex programs, the fetch, the translation, what the Wii's processor does for a frame; and the best
run's parts: the model's own (pusher, puller, state tracker), the backend's draws, its clears and
frames, the memory records put in. The same on the console: wl/xboxrt's capture bench
(`replay:<file> <runs>` in a title's `play.cfg`).

`--cpu-transform` keeps every vertex on the processor (the transform unit not used), for comparing:
`tools/png_compare.py A B --threshold N --diff D` counts each frame's pixels that differ, and beyond
a threshold, between two folders of frames.

`--stream` replays through `wl_nv2a_replay_stream`, the capture read in odd-sized pieces into a guest
memory given as the caller's; the output is the same (the test `xbox_nv2a_replay_stream`).

## The NV2A's tests

`tests/xbox/test_xbox_nv2a*.c` (host and ppc-be): the registers, the pusher (jumps, calls, a header
across kicks, a ring's wrap), Direct3D's handshake, RAMHT and DMA objects, semaphores, notifiers,
reports, the vblank and flips, a trap, a capture replayed (`test_xbox_nv2a.c`); every vertex format,
each kind of draw, long batches, the windows, the combiner words, the surface (`_kelvin`); texture
registers, conversions sampled against the texel decoders, levels, DXT and P8, sphere maps, the
cache (`_texture`); the vertex arrays' cache (`_vcache`: each kind converted to the bit, found again, rewritten once and twice, evicted); the GX backend over the host's recorder: vertices as GX gets them, the TEV
program, textures, the perspective path, culling, stencil, clears, render targets (`_gx`); the GX
backend drawn by the host's software GX from push buffer to pixel: the console suite whole, with
its Dolphin expectations, Direct3D's culling of screen-space quads, a program whose final product
needs every TEV register, and a capture replayed to the same pixels (`_pixels`); the numbers
against another table (`_regs`). The replay tool runs over the capture the pixel test writes. On
the console, `tests/xbox/suite_nv2a.c` (in `wl_console_tests.dol`) draws synthetic frames
through the GX backend on the Wii's GX and reads the EFB back.

## Not handled yet

Stencil (GX has none: a draw with the test is drawn as if it passed, a stencil-only pass is skipped); the fixed-function transform beyond the composite
matrix (no lighting, no texgen); point sprites, line widths; the NV2A's dependent texture modes
(bump-env, dot products) beyond what psh does; the 2D classes (M2MF, image blit: logged); surfaces
in guest memory (a render target's pixels are on the host: a title reading them back with the CPU
sees what was there); occlusion counts with a depth test (the GX backend counts area); the
pixel-centre offset is OpenCE-Wii's +0.5 and wants checking against a title's viewport constants.
