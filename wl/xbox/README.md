# wl/xbox: the Xbox's NV2A on GX

Target `wl_xbox` (alias `WiiLibs::xbox`) depends on `wl_platform` (its TEV programs,
`wl/platform/tev.h`) and `wl_gfx` (`wl/gfx/gfx_sqrt.h`). What a port of an Xbox (Direct3D 8) game
needs to draw with the game's own shaders on the Wii, which has no programmable stages: the pixel
shaders' register combiners become TEV programs, and the vertex programs run on the processor.
Game-agnostic: a game's own knowledge (programs it recognizes and makes by hand, its list of
vertex programs) stays with the game. Headers: `wl/xbox/psh.h`, `wl/xbox/psh_cache.h`,
`wl/xbox/vsh.h`, `wl/xbox/vsh_native.h`. Built with `-ffp-contract=off` (`DETERMINISTIC`), so a
vertex program gives the same bits interpreted or compiled, on every target.

(The Xbox's textures are `wl/codec/xbox_tex.h`.)

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
through TEV's lerp, bias and scale. A program beyond GX is refused (`reason`); one that only fits
approximated says so (`approximate`, `approximation`).

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
