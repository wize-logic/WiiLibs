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
| `platform` | Platform layer (GX, video, input, audio out, files, sockets, threads, tagged memory, a malloc that spills from MEM1 into MEM2, ring buffers, backtraces, a bring-up text console and stop screen) with a libogc backend and a dependency-free host backend | all |
| `audio` | ADPCM decode (DSP-ADPCM via `codec`), 32-voice mixer, streamed music, DS SSEQ/SBNK/SWAR synthesiser, music track engine (`wl_audio_core` is the portable part) | all |
| `gfx` | Matrix/vector maths, Broadway paired-single fast paths (C fallback elsewhere), GX texture decode/encode (wrappers over `codec`) | all |
| `gxtex` | Offline GX texture encoding, BRRES TEX0 reading, NDS 2D graphics (NCGR/NCLR/NCER/NANR) to GX | all |
| `nds` | NDS ROM header, FNT/FAT, overlays, NARC, LZ10/LZ11/BLZ | all |
| `ctr` | 3DS NCSD, NCCH, ExeFS, RomFS, GARC (decrypted images) | all (file source: host) |
| `gcm` | GameCube disc images: header, FST, main.dol | all (path open: host) |
| `wiidisc` | Wii disc extraction through `dolphin-tool` (holds no keys) | host |
| `gs` | Genius Sonority FSYS archives, TOC, LZSS | all |
| `brstm` | NW4R audio: BRSTM, BRSAR, RWSD, DSP-ADPCM (via `codec`) | all (path open: host) |
| `sdat` | NDS SDAT sound archives (SSEQ/SBNK/SWAR/STRM) | all (index: host) |

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

On the Wii, the tests are one program, `tests/console/wl_console_tests.dol`. It runs the
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
| `tools/dolphin_run.py` | Runs a `.dol` in Dolphin: see above |
| `tools/link_stubs.py` | Links a program that is not complete yet: on undefined symbols it writes a C file defining each as a function that stops the program naming it (`--stop-function`, default `wl_platform_missing_symbol` of `wl/platform/stop.h`, which shows the name on the Wii's screen and waits for HOME), compiles it, links again and lists the names in `--report`; `--data NAME[:BYTES]` defines a data symbol as zero bytes instead. Tests: `tests/tools/test_link_stubs.py` (host) |
| `tools/wl_profile.py` | Symbolizes the sampling profiler's dumps (`wl/platform/profile.h`) against the program's ELF: the top functions by their own samples and by inclusive, callers and callees of a function, a call tree, the hottest instructions, a time line, folded stacks for flame graphs. Tests: `tests/tools/test_wl_profile.py` |
| `tools/qemu_icount.py` | Counts the instructions a program built for big-endian PowerPC Linux (the `ppc-be` toolchain, `-mcpu=750`) runs under qemu-ppc: in all, by function, by translation block, and the difference from a smaller run of the same program (a cost per item: per vertex, per output frame). Speed counted without a Wii: exact and repeatable, though not cycles. Tests: `tests/tools/test_qemu_icount.py` |
| `tools/xbox_tex_reference.py` | Reference implementation of the Xbox texture conversions |

To use it from another CMake project: `add_subdirectory(path/to/WiiLibs)` and link
`WiiLibs::<module>`. Tests and `-Werror` are on only when WiiLibs is the top-level project
(`WL_BUILD_TESTS`, `WL_WERROR`).

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
- No tests against real game files.

## Licence

MIT: see [LICENSE](LICENSE). Every source file says so in its `SPDX-License-Identifier` line. Third-party code keeps its own licence: [third_party/README.md](third_party/README.md).
