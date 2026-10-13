# xboxrecomp (vendored)

The part of [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (MIT, Copyright (c) 2026 sp00nz and
contributors) that WiiLibs uses to recompile Xbox titles: the Python pipeline that turns an Xbox
executable (XBE) into C. Upstream commit `193e2995ffaec871bb3be43a43bd66aa5fccfeb1` (v0.13.1 "Cut
Short", 2026-10-07); `UPSTREAM.json` records it with the git blob id of every file here.

**The copy is pristine.** Every upstream file here is byte for byte what upstream has at that commit,
so it can be refreshed mechanically (`tools/xboxrecomp_sync.py merge --apply`). WiiLibs' changes are
the patch files in `patches/`, applied to a copy in the build tree (`tools/xbox_recomp.py prepare`),
never to these files. `tools/xboxrecomp_sync.py status` checks both.

## What is here, and why

| Path | What it is | Used by |
|---|---|---|
| `tools/xbe_parser/` | XBE header, sections, kernel imports | every stage |
| `tools/disasm/` | function detection (linear sweep, recursive descent, jump tables, xrefs) | `tools/xbox_recomp.py` |
| `tools/func_id/` | function classification (CRT signatures, vtables) | `tools/xbox_recomp.py` |
| `tools/abi_analysis/` | calling conventions, frames | `tools/xbox_recomp.py` |
| `tools/recomp/` (with `data/`) | the lifter: x86 to C, chunks, dispatch table | `tools/xbox_recomp.py`, the conformance net |
| `tools/conformance/` (not `msvc-wine/`) | the conformance cases (x86 snippets with inputs) and harness; `mkxbe.py` writes a synthetic XBE | `tests/xbox_recomp/` |
| `tools/kernel_audit/` | kernel import coverage, per-XDK ordinal remap | reference for the kernel (`wl/xboxkrnl`, planned) |
| `tools/xiso/` | XDVDFS disc image reader (`python3 -m tools.xiso`) | extracting a title's files |
| `tools/debug_symbols/` | function names from a debug build's assert strings | naming |
| `tools/seed_from_log/` | runtime-discovered entry points fed back as seeds | bring-up |
| `tools/ghidra_naming/merge_names.py` (+ its test) | the merge-time reserved-name list | imported by `tools/recomp/test_reserved_idents.py` |
| `tools/__init__.py` | makes `tools` a package | |
| `templates/runtime/recomp_types.h` | the runtime header the lifter writes beside its output | the template `wl/xboxrt/recomp/recomp_types.h` is generated from |
| `templates/new-game/src/main.c` | upstream's per-title `main` | read by two upstream tests (the register-globals contract) |
| `LICENSE`, `NOTICE` | upstream's licence and attribution | |

Each package's upstream tests (`test_*.py`) are vendored with it and run in WiiLibs' build
(`ctest -R xboxrecomp_pytest`) on the patched copy. Two need upstream's C runtime, which is not
vendored, and are deselected (`tests/xbox_recomp/run_pytest.py` says which and why).

Not vendored: upstream's C runtime (`src/`: Win32 kernel bridges, D3D8 on D3D11/OpenGL, audio,
input), the tests of that runtime (`tests/`), the other tools, CMake and Docker files, and the docs.
The docs are referred to at the commit above, for example
[`docs/pipeline/04-lifting.md`](https://github.com/sp00nznet/xboxrecomp/blob/193e2995ffaec871bb3be43a43bd66aa5fccfeb1/docs/pipeline/04-lifting.md)
and [`docs/technical/conformance-testing.md`](https://github.com/sp00nznet/xboxrecomp/blob/193e2995ffaec871bb3be43a43bd66aa5fccfeb1/docs/technical/conformance-testing.md).
Nothing under an LGPL licence is here: upstream's `NOTICE` lists `src/apu/*` and
`src/nv2a/nv2a_regs.h` (taken from xemu), and `src/nv2a/nv2a_core.c` and `nv2a_state.h` say LGPL in
their own headers. `upstream_map.json` marks them concept-only with that note, and the sync tool
refuses to vendor any file with LGPL text in its header.

## The patches

Each is a `git format-patch` file with a message saying what it fixes and why, written to be sent
upstream (`git am` applies them to an upstream checkout). They change the lifter, its tests and the
template header, and add conformance cases; upstream's own tests pass with them applied.

| Patch | Fixes |
|---|---|
| `0001-shift-count-mask.patch` | `shl`/`shr` mask their count to 5 bits as x86 does (C leaves counts of 32 and more undefined; PowerPC's `slw`/`srw` take 6 bits) |
| `0002-cvt-si32-helper.patch` | `cvt(t)ss2si`/`cvt(t)sd2si` through `recomp_cvt_si32`: the non-t forms round to nearest even instead of truncating, and NaN or out of range gives 0x80000000 on every host |
| `0003-memf-memd-set.patch` | float and double stores are `MEMF_SET`/`MEMD_SET` (a runtime can store the integer bits; the template keeps the assignment) |
| `0004-reserved-toolchain-names.patch` | newlib, glibc, libgcc and libogc names are reserved (mangled) like the MSVC CRT's |
| `0005-x87-m80.patch` | `fld m80`/`fstp m80` lifted through `recomp_ld80`/`recomp_st80` (they read 4 bytes as a float, and stored nothing and did not pop) |
| `0006-runtime-header-option.patch` | `--runtime-header template|none|PATH`: a runtime can supply its own `recomp_types.h` without editing generated output |
| `0007-parity-setcc.patch` | `setp`/`setnp`/`cmovp`/`cmovnp` lifted (float equality after `fucomip`/`ucomiss` read a stale register) |
| `0008-backedge-hook.patch` | `RECOMP_BACKEDGE()` before every backward jump (a nothing in the template): a runtime that serialises guest threads or delivers interrupts between guest operations gets a break-in point in loops that only poll memory |

To change one: `tools/xboxrecomp_sync.py patch-repo DIR` makes a git repository of the pristine copy
with each patch as a commit; edit there (amend, rebase), then `tools/xboxrecomp_sync.py
export-patches DIR` writes them back here.

## Following upstream

`UPSTREAM.json` is the state (the commit the copy is at, the commit triaged up to, and the blob ids);
`upstream_map.json` is the policy (every upstream path mapped to vendored, patched, port, concept or
ignore, with the WiiLibs files and tests that depend on it). `docs/xboxrecomp-sync.md` is the
procedure: fetch, brief (a research brief per area), merge, re-apply the patches, test, advance.
