# WiiLibs

Reusable C11 libraries for Wii homebrew and the host tools around it: a platform layer over
libogc, software audio, graphics maths and GX texture codecs, and readers for Nintendo file
formats (DS, 3DS, GameCube, Wii). Game-agnostic.

No game data, keys or extracted assets are included; every test uses synthetic fixtures.

## Modules

Each module is `wl/<name>/` with its own README, a CMake target `wl_<name>` (alias
`WiiLibs::<name>`) and tests in `tests/<name>/`. Headers are included as `"wl/<name>/<file>.h"`.

| Module | What it does | Platforms |
|---|---|---|
| `base` | Result codes, asserts, logging, bump arena, endian readers, fixed point, FNV-1a/CRC-32, PRNG, UTF-8 | all |
| `codec` | The GX texture codec (every texel format, TLUTs, CMPR with the hardware's 5/8 : 3/8 blend), Xbox texture conversion (Morton swizzle, DXT to CMPR/RGBA8, Xbox formats to GX) and the DSP-ADPCM decoder, shared by `gfx`, `gxtex`, `audio` and `brstm` | all |
| `host` | POSIX helpers for tools: files, JSON reader, SHA-1, threads, processes | host |
| `platform` | Platform layer (GX with render targets, video, input, audio out, files (and a crash-safe replace with its sweep, a directory listing cache), sockets, threads, tagged memory, a malloc that spills from MEM1 into MEM2, ring buffers, backtraces, a sampling profiler that a control file arms, hardware counters (the performance monitor and GX's) with a verdict on what bounds a frame, a bring-up text console and stop screen, a frame overlay with its drawing helpers, a HOME menu with the Wii's notices and a wrist strap reminder) with a libogc backend and a dependency-free host backend | all |
| `gxsoft` | A software GX for host builds and tests: what a program asks of `platform`'s GX layer (the direct layer, TEV programs stage by stage, textures decoded with `codec`, blending, depth, culling, clipping, render targets' EFB copies, the display copy) drawn on the processor as the console's GX draws it, checked against Dolphin | host, ppc-be |
| `audio` | ADPCM decode (DSP-ADPCM via `codec`), 32-voice mixer, streamed music, DS SSEQ/SBNK/SWAR synthesiser, music track engine (`wl_audio_core` is the portable part) | all |
| `gfx` | Matrix/vector maths, Broadway paired-single fast paths (C fallback elsewhere), a correctly rounded `sqrtf` from `frsqrte` (opt-in `--wrap=sqrtf`), GX texture decode/encode (wrappers over `codec`) | all |
| `gxtex` | Offline GX texture encoding, BRRES TEX0 reading, NDS 2D graphics (NCGR/NCLR/NCER/NANR) to GX | all |
| `nds` | NDS ROM header, FNT/FAT, overlays, NARC, LZ10/LZ11/BLZ | all |
| `ctr` | 3DS NCSD, NCCH, ExeFS, RomFS, GARC (decrypted images) | all (file source: host) |
| `gcm` | GameCube disc images: header, FST, main.dol | all (path open: host) |
| `wiidisc` | Wii disc extraction through `dolphin-tool` (holds no keys) | host |
| `gs` | Genius Sonority FSYS archives, TOC, LZSS | all |
| `brstm` | NW4R audio: BRSTM, BRSAR, RWSD, DSP-ADPCM (via `codec`) | all (path open: host) |
| `sdat` | NDS SDAT sound archives (SSEQ/SBNK/SWAR/STRM) | all (index: host) |
| `xbox` | The Xbox's NV2A on GX: pixel shaders' register combiners translated to TEV programs (with a cache that rewrites constants in place, and a hook for a game's hand-made programs), vertex programs decoded, analysed (inputs, affinity, constants read) and run on the processor, and compiled ahead of time to C by `tools/xbox_vsh_codegen.c`; the NV2A as a device for a title whose own Direct3D runs unchanged (MMIO registers, push buffers executed, semaphores, interrupts, the 3D state, vertex fetch of every format, textures converted and kept, captures and their replay), with a backend on GX (`wl_xbox_gx`) and a recorder | all (the tools: host) |
| `xboxapu` | The Xbox's audio hardware as device models a recompiled title's own DirectSound drives: the MCPX APU (registers, front end methods, the voice processor playing PCM and Xbox ADPCM buffer and stream voices with pitch, envelopes and 32 mixbins at 48 kHz, notifiers, interrupts, a stereo mix in place of the GP/EP DSP programs) and the AC'97 controller | all |
| `xboxrt` | The runtime a recompiled Xbox title runs on: guest memory at the Xbox's addresses (the Wii's MMU with a hashed page table filled on first touch, demand paging to a swap file when the frames run out; a reservation elsewhere, paged the same way under a frame limit), devices behind faults with PowerPC and x86-64 instruction decoders, the generated code's register file, guest threads under one guest lock, interrupts and the vertical blank, the kernel thunk dispatcher, the XBE loader, crash reports, the audio devices (`xboxapu`'s models, the platform's output at 48 kHz, a WAV on the host), the USB host controllers with the Xbox's gamepads (the Wii's controllers mapped onto them), a play harness that drives them from a script, and the boot code (`wl_xboxrt_boot`, `wl_xboxrt_main`) | all (the MMU backend: Wii; faults: host, ppc-be) |
| `xboxkrnl` | The Xbox kernel's exports for recompiled titles over `xboxrt` and `platform`: dispatcher objects and waits, critical sections, threads, IRQL/DPCs/timers/interrupts, memory, files through a mount table (the title's folder, the hard disk's partitions, sparse cache drives), settings, AV and HAL queries, Rtl, SHA-1/HMAC/RC4; every import of Halo's XBE covered | all |

`wl/xboxrt/recomp/` holds the runtime header recompiled Xbox code includes (`recomp_types.h`,
generated from xboxrecomp's template; its README lists the runtime ABI): see "Recompiling an Xbox
title" below.

## Building

```sh
# host (x86-64): libraries and tests
cmake -S . -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host

# big-endian test build (32-bit PowerPC Linux under qemu-ppc, same byte order as the Wii)
cmake -S . -B build/ppc-be -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/powerpc-linux-gnu.cmake
cmake --build build/ppc-be && ctest --test-dir build/ppc-be

# Wii (devkitPPC + libogc)
cmake -S . -B build/wii -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake
cmake --build build/wii
```

On the Wii, the tests are one program, `tests/console/wl_console_tests.dol` (and
`wl_xboxrt_console.dol` beside it for wl/xboxrt's guest memory on the MMU, which Dolphin runs with
`--mmu`: `wl/xboxrt/README.md`). It runs the
platform-independent suites (threads, files, the SSEQ synth through real audio output) and a
GX/video smoke test on the real backends. `ctest -L dolphin` runs it in Dolphin through
`tools/dolphin_run.py`. From Linux (WSL) that is the Windows Dolphin: `WL_DOLPHIN`, or
`Documents\Emulators\Dolphin-x64` in a Windows profile. The runner gives Dolphin a user folder of
its own and syncs a folder in as the SD card. It also captures the screen and can dump frames as
PNGs. With `--backtrace ELF` it runs the program under gdb through Dolphin's GDB stub: gdb runs
in WSL and reaches the stub through a PowerShell relay over interop's pipes (OpenMMO's
`mmo/windows/dev.sh` pattern). At the end it prints the CPU's backtrace and every libogc
thread's, from the context each was switched out with. The same DOL runs from the Homebrew
Channel on hardware.

```sh
cmake -S . -B build/wii -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake
cmake --build build/wii && ctest --test-dir build/wii -L dolphin --output-on-failure
```

## Tools

| Tool | What it does |
|---|---|
| `tools/wii_package.py` | Zips a `.dol` for the Homebrew Channel: `apps/<app>/boot.dol`, a `meta.xml` (name, coder, version, descriptions; escaped), a 128x48 `icon.png` (given, or drawn from the name) and any `--extra` files; reproducible (`SOURCE_DATE_EPOCH`). From CMake: `wl_add_hbc_package(<dol target> NAME ... [ICON_TEXT|ICON] [EXTRA ...])` (`cmake/WiiLibs.cmake`) adds `<target>_hbc`. Tests: `tests/tools/` (host) |
| `tools/dolphin_run.py` | Runs a `.dol` in Dolphin: see above; `--sd-out DIR` copies the files the program wrote to the card (new or changed since `--sd`: a log, a save) out of the runner's copy of it. Tests: `tests/tools/test_dolphin_run.py` |
| `tools/link_stubs.py` | Links a program that is not complete yet: on undefined symbols it writes a C file defining each as a function that stops the program naming it (`--stop-function`, default `wl_platform_missing_symbol` of `wl/platform/stop.h`, which shows the name on the Wii's screen and waits for HOME), compiles it, links again and lists the names in `--report`; `--data NAME[:BYTES]` defines a data symbol as zero bytes instead. Tests: `tests/tools/test_link_stubs.py` (host) |
| `tools/wl_profile.py` | Symbolizes the sampling profiler's dumps (`wl/platform/profile.h`) against the program's ELF: the top functions by their own samples and by inclusive, callers and callees of a function, a call tree, the hottest instructions, a time line, folded stacks for flame graphs. Tests: `tests/tools/test_wl_profile.py` |
| `tools/qemu_icount.py` | Counts the instructions a program built for big-endian PowerPC Linux (the `ppc-be` toolchain, `-mcpu=750`) runs under qemu-ppc: in all, by function, by translation block, and the difference from a smaller run of the same program (a cost per item: per vertex, per output frame). Speed counted without a Wii: exact and repeatable, though not cycles. Tests: `tests/tools/test_qemu_icount.py` |
| `tools/fma_check.py` | Lists the fused multiply-adds (`fmadd`, `fmsub`, `fnmadd`, `fnmsub` and their single forms, the Gekko's paired singles `ps_madd`..`ps_madds1`, AltiVec's and VSX's) in PowerPC objects, archives and ELF files, by object and function, with the instructions (`-v`); exit 1 if there are any, 2 if the check could not be made (a file objdump cannot read as PowerPC is never a pass). For a program whose results must match another machine's bit for bit: compilers fuse unasked, and Clang has let some through `-ffp-contract=off`. Shell-pattern filters on objects and archive members (`--include`, `--exclude`) and on functions (`--include-function`, `--exclude-function`), `@file` response files (ninja's), `--stamp` for build systems. Runs objdump with `-M gekko`, without which the paired singles are misread. From CMake: `wl_add_fma_check(<target> [LINKED] [OBJDUMP ...] [EXCLUDE ...] [EXCLUDE_FUNCTION ...])` (`cmake/WiiLibs.cmake`) fails the target's build if its objects (or, with `LINKED`, its ELF) have one; pairs with `wl_target_defaults(<target> DETERMINISTIC)`. Tests: `tests/tools/` (host; real objects where a PowerPC Linux compiler is installed) |
| `tools/xbox_tex_reference.py` | Reference implementation of the Xbox texture conversions |
| `tools/xbox_nv2a_replay.c` | Replays a capture of the NV2A model (`wl/xbox/nv2a_capture.h`: a title's push buffers and the guest memory they read) on the host: each clear, draw, frame and flip described, the model's counts, and with `--png-gx` each frame the GX backend would show, drawn by the host's software GX (`wl/gxsoft`; `wl/xbox/README.md`). Built with the host build (`xbox_nv2a_replay`). Tests: `tests/xbox/` |
| `tools/xbox_vsh_codegen.c` | Compiles a file of Xbox (NV2A) vertex programs to C functions that give, bit for bit, what `wl/xbox/vsh.h`'s interpreter gives, and a table of them for `wl_xbox_vsh_compile` (`wl/xbox/README.md`). Built with the host build (`xbox_vsh_codegen`). Tests: `tests/xbox/test_xbox_vsh_native.c` |
| `tools/xbox_recomp.py` | Runs xboxrecomp's pipeline (vendored in `third_party/xboxrecomp`, WiiLibs' patches applied in the build tree) on an XBE: parse, disassemble every executable section, identify, calling conventions, lift to C in chunks, each stage cached by its inputs' hashes; wraps the chunks in a fixed set of translation units for a build system. A title's knowledge comes in a JSON config (`--config`: hand-written functions, seeds, extra options). From CMake: `wl_add_xbox_title` (below). Tests: `tests/tools/test_xbox_recomp.py`, `tests/xbox_recomp/` (host) |
| `tools/xbox_recomp_smoke.py` | Smoke check on a real title: runs the driver on an XBE into a build directory and compiles every chunk for the Wii with devkitPPC, reporting functions, unresolved call targets, unimplemented instructions and the summed `.text`. Nothing it generates leaves the build directory |
| `tools/xboxrecomp_sync.py` | Follows xboxrecomp upstream: `status`, `fetch`, `brief` (a research brief per area: commits, release notes, files mapped to the WiiLibs files they affect, licence flags for LGPL/xemu/Cxbx, questions for each reviewer), `merge`/`apply` (refresh the pristine copy, re-apply the patches, report failures), `check-map`, `test`, `advance` (record the triage), `patch-repo`/`export-patches` (edit the patches in git). Procedure: [docs/xboxrecomp-sync.md](docs/xboxrecomp-sync.md). Tests: `tests/tools/test_xboxrecomp_sync.py` (a synthetic upstream) |

To use it from another CMake project: `add_subdirectory(path/to/WiiLibs)` and link
`WiiLibs::<module>`. Tests and `-Werror` are on only when WiiLibs is the top-level project
(`WL_BUILD_TESTS`, `WL_WERROR`).

## Recompiling an Xbox title

WiiLibs carries [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)'s static recompiler (MIT): its
Python pipeline lifts an Xbox executable's x86 code to C, function by function, and the C is compiled
for the Wii (or the host, or `ppc-be` for tests). In a CMake project that adds WiiLibs:

```cmake
wl_add_xbox_title(mygame XBE ${GAME_DIR}/default.xbe
    TITLE_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/title.json   # optional: hand-written functions, seeds
    OPT -Os                                               # default; -O2 is about 40% bigger
    SOURCES src/overrides.c)                              # the title project's own code
```

At build time `tools/xbox_recomp.py` runs the pipeline on the XBE into the build tree (the generated
code is derived from the game: it stays there and is never committed), the chunks are compiled with
the flags they need against `wl/xboxrt/recomp/recomp_types.h`, and the executable links WiiLibs' Xbox
runtime (`WiiLibs::xboxrt`, being written; `RUNTIME` names another). On the Wii it gets its `.dol`.
The header makes the generated code right on a big-endian CPU: guest memory keeps the x86's
little-endian bytes and every access byte-reverses (`lwbrx`/`stwbrx`); guest addresses go through
`WL_XB_EA` (user space as is, the 0x8xxxxxxx/0xFxxxxxxx windows at 0x1xxxxxxx). Its README lists the
symbols a runtime defines.

What makes it trustworthy: `ctest -R xbox_recomp_conformance` lifts every one of xboxrecomp's
conformance cases (x86 snippets with inputs; 207 cases, 6,322 vectors) and runs them big-endian
under qemu-ppc against a recording of this machine's x86 CPU; `xbox_recomp_e2e` does the same with
whole programs; `xboxrecomp_pytest` runs upstream's own test suite on the patched pipeline.
WiiLibs' fixes to upstream (shift counts, float-to-int conversions, float stores, reserved names,
80-bit x87 operands, parity conditions, a runtime-header option) are patch files in
`third_party/xboxrecomp/patches/`, written to be sent upstream. Upstream updates are taken with
`tools/xboxrecomp_sync.py` ([docs/xboxrecomp-sync.md](docs/xboxrecomp-sync.md)). Retail Halo
(7,493 functions) lifts and compiles for the Wii to 10.8 MiB of code at `-Os`.

## Conventions

- Public names use `wl_` / `WL_` / `Wl` prefixes. Functions that can fail return `WlResult`
  (0 = ok, negative = `WL_ERR_*`) and write outputs through pointers.
- Big-endian safe: byte buffers are read with `wl_rd_le*`/`wl_rd_be*`, never cast to structs.
- External data is validated and returns errors; `WL_ASSERT` is for programmer errors only.

## Open items

- `wl_tex_bytes` (`wl/gfx/tex_codec.c`) keeps its own tile table beside `wl_gxc_tex_bytes`.
  It is not delegated yet because `wl_gxc_tex_bytes` returns 0 above `WL_GXC_MAX_DIM` and
  `wl_tex_bytes` does not.
- The Wii backends of `wl/platform` and `wl/audio` pass the console tests in Dolphin: GX
  read-back, video pacing, LWP threads with real priorities, libfat SD on IOS 58, and AESND
  output. Input is only initialised, not driven. Nothing has run on hardware yet.
- No tests against real game files (the Xbox smoke check, `tools/xbox_recomp_smoke.py`, is run by
  hand on a title the developer owns).
- Recompiled Xbox code: the runtime (`wl/xboxrt`), the kernel and NV2A on GX are being written; a
  title links only against a stand-in so far. Each `tail_jump_alias` entry is lifted as a full copy
  of the body it enters (Halo: 2.0 MB of x86 lifted from 1.6 MB of unique code).

## Licence

MIT: see [LICENSE](LICENSE). Every source file says so in its `SPDX-License-Identifier` line. Third-party code keeps its own licence: [third_party/README.md](third_party/README.md).
