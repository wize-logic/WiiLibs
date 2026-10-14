# Notes for agents

WiiLibs is a set of C11 libraries for Wii homebrew and the host tools around it (README.md lists
the modules). It has two kinds of work:
- **The general libraries:** `wl/platform` over libogc (with a host backend), audio, graphics maths
  and GX texture codecs, readers for Nintendo formats.
- **Running Xbox titles on the Wii:** a recompiled title's machine (`wl/xboxrt`, `wl/xboxkrnl`,
  `wl/xboxapu`), the Xbox GPU on GX (`wl/xbox`, with `wl/gxsoft` to draw GX on the host), and
  xboxrecomp's static recompiler vendored with WiiLibs' patches (`third_party/xboxrecomp`,
  `tools/xbox_recomp.py`).

The first title is retail Halo. Its title project lives outside the repository (for example
`~/xbox-titles/halo`: CMake, its config, a play plugin and level runner, play scripts, checkpoint
snapshots) and is never committed anywhere. OpenCE-Wii (`../OpenCE-Wii`) also takes pieces from
WiiLibs.

Start with README.md, then the README of the module you touch.

## Building and testing

Every change is checked on three targets, and on a fourth for the Xbox work:

```sh
cmake -S . -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host
cmake -S . -B build/rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/rel && ctest --test-dir build/rel
cmake -S . -B build/ppc-be -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/powerpc-linux-gnu.cmake && cmake --build build/ppc-be && ctest --test-dir build/ppc-be
cmake -S . -B build/wii -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake && cmake --build build/wii
```

- **Release matters.** At -O3, GCC finds warnings and test assumptions that -O0 misses: a value a
  fault handler writes must be `volatile` in a test, and a loop bound read atomically needs a clamp.
- **ppc-be** is the Wii's byte order under qemu-ppc. Speed is counted there with
  `tools/qemu_icount.py`: instructions per item (per vertex, per frame, per call), exact and
  repeatable.
- **The Wii** is Dolphin, through `tools/dolphin_run.py`:
  - `ctest -L dolphin` runs the console suites;
  - `--mmu` is needed for `wl_xboxrt_console.dol` and for every recompiled title;
  - `--unthrottled` makes a long run finish sooner, and the program sees no difference;
  - `--dump-frames` and `--dump-audio` save what the console showed and played;
  - `--sd`/`--sd-out` sync an SD folder in and out.
- **A recompiled title:**
  - On the host it is built from its title project with `-DWIILIBS_DIR=<worktree>`.
  - It plays scripts (`WL_XB_PLAY=script:...`; on the Wii, `play.cfg` in the title folder;
    `wl/xboxrt/play.h` lists the steps).
  - `WL_XB_DETERMINISTIC=1` makes two host runs of a script the same game. Use it to compare
    builds: identical `do where` lines and screenshots.
  - `WL_XB_FRAME_MB=24 WL_XB_HEAP_MB=14` imitate the Wii's memory.

## Rules

- **Conventions:**
  - `wl_`/`WL_`/`Wl` names;
  - `WlResult` returns;
  - byte buffers read with `wl_rd_le*`/`wl_rd_be*`, never cast to structs;
  - external data validated.
  Match the surrounding code's comments and naming.
- **No game data,** keys, extracted assets, captures, checkpoint snapshots or game-derived code
  (generated C, vertex program lists, symbolized dumps of a game) in the repository or its history.
  Tests use synthetic fixtures.
- **`sdk-findings/`** is local and confidential: never read it, quote it, commit it or push it.
- **Commits:**
  - authored by the person's git identity alone;
  - no co-author, session or "generated with" lines, no emoji markers;
  - a one-line subject in the repository's style (`module: what it does now`), with a body when
    the reason is not obvious.
  - The working branch is `Xbox`.
- **Upstream xboxrecomp:**
  - Fixes are numbered patch files in `third_party/xboxrecomp/patches/` (no `.pyc`), written so
    they could be sent upstream.
  - Updates are taken with `tools/xboxrecomp_sync.py` (docs/xboxrecomp-sync.md); its
    `check-map` must stay clean.
- **Dolphin is the main session's.** Agents never launch it. They ask for a checkpoint run, naming
  the DOL and the SD staging and what the run should show. One window per run, closed when done.
- **No whole-filesystem searches** (they stall the machine); work in the repository, the title
  project and a scratch folder. `git checkout --`, `reset` and `revert` are blocked by a hook: undo
  by editing.

## Measuring

- **Correctness of what is drawn:**
  - A capture (`capture <frames>` in a script, or `WL_XB_CAPTURE` on the host) replays with
    `xbox_nv2a_replay --png-gx`.
  - A change must give byte-identical frames, or for GX's transform unit stay within
    `tools/png_compare.py`'s tolerance with no visible difference. Show before/after images.
- **Correctness of the game and sound:**
  - deterministic host runs compared line by line;
  - `WL_XB_AUDIO_HASH` for the audio's mixbins and output;
  - the conformance and e2e suites for the recompiler.
- **Speed on the Wii:**
  - The capture bench: `replay:play/<capture> <runs>` in `play.cfg`, about 2 minutes in Dolphin,
    gives the GPU path's ms per frame for one fixed frame.
  - A profile: `profile.txt` in the title folder, symbolized with `tools/wl_profile.py <elf> <log>`.
  - Compare shares, draws per second, or bench milliseconds. **Never compare the frame rate of a
    time window between two runs**: the scene drawn in that window changes with timing (one window
    had 324 draws a frame in one run and 482 in the next).
- **Long runs:** races show only in long runs. Play whole levels, several at once (each with its own
  SD copy and `--name`).
  - The runtime's stall report (no scheduling for 20 s) and freeze watch (no new frame for 30 s:
    each guest thread's return addresses, and a 10 s profile into the log) name what a hang was
    doing.
  - Diagnostics must cost nothing until they fire: a watcher thread that polls can move a race out
    of sight.

## Specialized agents

Two versions of one team. The lean one, three at a time, is the default. The full one, six at a
time, is used when the person asks for it; its roles are the lean ones split finer. Each agent is
started with Claude Code's `Agent` tool (a general-purpose agent whose prompt starts with its role
below), at its row's model and effort (the tool's `model` and `effort` parameters). It works in its
own git worktree, on a branch named for its task off `Xbox`.

### The lean version: three agents

| Role | Model | Effort | Owns |
| --- | --- | --- | --- |
| gpu | opus | high | The Xbox GPU on GX: `wl/xbox` (the NV2A model, the GX backend, combiners to TEV, vertex programs, textures and their caches, render targets), `wl/gxsoft`, the Xbox parts of `wl/codec`, `tools/xbox_nv2a_replay.c`, `tools/xbox_vsh_codegen.c`, `tools/png_compare.py` |
| runtime | opus | high | The recompiled title's machine and the recompiler: `wl/xboxrt` (memory and paging, threads and the scheduler, interrupts, devices, the play harness, diagnostics), `wl/xboxkrnl`, `wl/xboxapu`, `third_party/xboxrecomp` and its patches, `tools/xbox_recomp.py`, `tools/xboxrecomp_sync.py`, and a title project's play plugin and scripts |
| platform | sonnet | high | Everything general: `wl/platform` (libogc and host backends), `wl/audio`, `wl/base`, `wl/gfx`, `wl/gxtex`, the format readers, `tools/` apart from the Xbox ones, CMake, the console tests, the READMEs and this file, and what OpenCE-Wii takes from WiiLibs |

A shared file belongs, for one task, to the agent whose prompt names it.

#### Running them

- **Three at once, one per role.** When one finishes, the main session:
  1. reviews its report;
  2. merges its branch once host, Release and ppc-be ctest pass and the Wii build compiles;
  3. runs a Dolphin checkpoint first if the change alters what is drawn, played or timed;
  4. pushes `Xbox`;
  5. gives the role its next task at once.
- **The prompt** is short: the role's paragraph, the task, what "done" looks like, how to confirm
  it, and the paths it needs (captures, logs, ELF, staging).
- **The report** is at most 25 lines: what changed and why, how it was checked (numbers), the
  commits, and the checkpoint run it needs (DOL, ELF, staging, what to look for).
- **Helpers:** an agent may use helper agents only on `sonnet`, and only when they really help.
- **Re-use:** an agent that has finished keeps its context. For follow-up work in its own area,
  send it the next task (SendMessage) rather than starting a fresh one.

#### gpu

Draws what the Xbox drew, as fast as the Wii's GX allows.
- A material that GX cannot do exactly (cube maps, per-pixel dot products, the stencil) gets a
  plausible stand-in, never raw intermediate values on screen.
- Speed:
  - Count the processor's work per vertex and per draw (`qemu_icount`, `xbox_nv2a_replay --bench`).
  - Ask for the Wii capture bench and a profile.
  - Prefer moving work to GX's own units (the transform unit, indexed arrays, matrix indices)
    over making the processor's loops faster.
- Keeps the replay tests and the tolerance compares green. A render-only shortcut never changes
  game state.

#### runtime

Makes a recompiled title run on the Wii as it runs on the Xbox, from boot to the end of every level,
and makes its own code fast.
- **The guest's rules hold:**
  - nothing runs inside a raised IRQL but what the Xbox would run there;
  - one guest thread at a time behind the guest lock;
  - device access by faults;
  - the guest's memory little-endian.
- **Recompiler changes** keep the conformance and e2e suites and deterministic title runs
  identical. The DOL's size is watched: MEM1 is tight.
- **A hang or crash** is found by its evidence (the stall report, the freeze watch, the crash
  report, a profile) before it is fixed. A unit test reproduces it where possible.

#### platform

Keeps the libraries general, tested on every target, and true to their documents.
- A Wii feature gets its host stub and tests on host, Release, ppc-be and the Wii compile.
- What a consumer (a title, OpenCE-Wii) worked out that any Wii program could use moves here.
- The READMEs and this file are checked against the code; stale ones are fixed.

### The full version: six agents

| Role | Model | Effort | Owns |
| --- | --- | --- | --- |
| nv2a-renderer | opus | high | What is drawn: the combiners and their stand-ins (`wl/xbox/psh*.c`), textures and texture shaders (`nv2a_texture.c`, `nv2a_texcache.c`), render targets, blending, fades, the stencil, `wl/gxsoft` |
| gx-speed | opus | high | How fast it is drawn: the pusher and 3D state (`nv2a.c`, `nv2a_kelvin.c`), the vertex path, GX's transform unit, the vertex cache (`nv2a_gx.c`'s vertex functions, `nv2a_vcache.c`, `vsh.c`, `tools/xbox_vsh_codegen.c`), the capture bench |
| recompiler | opus | high | The lifter and its patches (`third_party/xboxrecomp`), the driver (`tools/xbox_recomp.py`, `wl_add_xbox_title`), the runtime header (`wl/xboxrt/recomp/`), the conformance and e2e suites, upstream sync |
| runtime-kernel | opus | high | `wl/xboxrt` (memory, paging and the write watch, threads and the scheduler, interrupts, devices, diagnostics, the boot), `wl/xboxkrnl`, `wl/xboxapu` |
| playtester | opus | high | Playing titles from start to end: `wl/xboxrt/play.c`, a title project's play plugin, level runner, checkpoint snapshots and scripts; finding what breaks and handing it on with evidence |
| platform-steward | sonnet | high | The general libraries, `tools/` apart from the Xbox ones, CMake, the console tests, the four builds' health, the READMEs and this file, OpenCE-Wii's share |

#### Running them

- **Keep six running.** When one finishes, the main session reviews, merges (after the checks
  above, and a Dolphin checkpoint if it changes what is drawn, played or timed), pushes, and starts
  that role's next task.
- **No overlaps:** no two running agents edit the same files. `nv2a_gx.c` is split by function:
  vertex functions are gx-speed's, material, target and blend functions nv2a-renderer's.
- **Doubled roles:** a role may run twice on unrelated tasks. Two nv2a-renderer agents on two
  unrelated faults, such as a material and a fade, work well.
- **The person's reports come first.** When the person reports something on screen (a colour, a
  flash, a hang), the main session finds the moment in the logs (time, tick, what the level runner
  was doing) and gives it to the fitting role with that evidence.

#### nv2a-renderer

Draws what the Xbox drew.
- Finds a fault in a capture (`capture N` across the moment; frames that jump in brightness show a
  flash; `--psh-dump`, `--gx-dump`, `-vv` describe each draw) and fixes it in the translation.
- Every other capture's output stays byte-identical unless it has the same fault.
- Shows before/after images in its report.

#### gx-speed

Makes the GPU path cheaper without changing a pixel, or within the transform unit's tolerance.
- Per-vertex and per-draw work is counted with `qemu_icount` and `--bench`, and on the Wii with the
  capture bench and a profile.
- The largest share goes first.

#### recompiler

Makes the lifted code right and fast.
- Changes go in as numbered patches.
- Proof: the conformance cases against recorded x86 results, e2e programs, upstream's test suite,
  deterministic title runs identical, and captured real guest states replayed old against new.
- Reports the instructions saved in the hot functions and the DOL's size.

#### runtime-kernel

Runs the title as the Xbox does.
- Scheduling, IRQL, DPCs, paging and device models follow the Xbox's rules.
- A fault found by a long Wii run is reproduced on the host if it can be (an amplifier knob may be
  used and left out of the commits), fixed, given a unit test, and confirmed by long runs in
  Dolphin.

#### playtester

Plays every level of a title from start to end, on the host (deterministic, with the Wii's memory)
and through the main session's Dolphin runs.
- **Owns the level runner:** its goals, aids and robustness at the Wii's low frame rate.
- **Keeps snapshots:** checkpoint snapshots for every level, so any moment can be resumed in a
  minute.
- **Reports a table:** per level, whether it reached the end, its ticks, crashes, hangs, aids used
  and what looked wrong. Hands each fault to its role with the evidence: log lines, a guest stack,
  a capture, a snapshot.

#### platform-steward

Keeps WiiLibs general and healthy.
- Runs the four builds and the tests from a clean checkout.
- Keeps the READMEs and this file true to the code.
- Moves into the libraries what consumers worked out.
- Prepares what OpenCE-Wii needs to take a change.

### Choosing the work

Without being told, pick the most valuable work a host check, a replay, a deterministic run or a
Dolphin checkpoint can confirm. The aim is a recompiled title a person can sit down and play on a
Wii. In order:
1. **Playable from start to end.** Every level, cutscenes included, with no crash, hang, freeze or
   stuck script over a long session, at the Wii's memory, found by playing it (the level runner, and
   the person's own reports).
2. **Drawn right.** What a Dolphin screenshot, a capture or the person shows wrong: materials, fades
   and flashes, text, the HUD.
3. **The frame rate.** The Xbox's 30 frames a second is the target. Cut the largest share of the
   profile first, and count the cut as the processor's work as well as the bench's milliseconds.
4. **Heard right.** Music, dialogue and effects (`--dump-audio`, `WL_XB_AUDIO_HASH`).
5. **Health.** The four builds and their tests, upstream xboxrecomp, the documents, the general
   libraries and their consumers.

Work that needs a real Wii (timings, the SD card's speed, the controllers, `frsqrte`'s accuracy) is
not done by agents. It is listed for the person, with what to look for.
