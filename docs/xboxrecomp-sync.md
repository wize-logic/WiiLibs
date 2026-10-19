# Following xboxrecomp upstream

WiiLibs recompiles Xbox titles with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)'s pipeline,
vendored pristine in `third_party/xboxrecomp/` with WiiLibs' fixes as patch files beside it. Upstream
moves fast (six releases in 25 days around v0.13). This is the procedure for taking an upstream
update: refresh the copy, re-apply the patches, find out what the update means for WiiLibs (a
research brief per area, one reviewer or research agent each), make the WiiLibs changes, test, and
record that the range was triaged. `tools/xboxrecomp_sync.py` does the mechanical parts; it needs
only Python 3.9+ and git.

## The files

| File | What |
|---|---|
| `third_party/xboxrecomp/UPSTREAM.json` | **state**: the commit the vendored copy is at (`vendored`: full SHA, date, release), the commit triaged up to (`triaged`: everything before it has been read and decided), the history of earlier cursors, and the git blob id of every vendored file |
| `third_party/xboxrecomp/upstream_map.json` | **policy**: research areas (glob to area) and rules (glob to `vendored`, `patched`, `port`, `concept` or `ignore`, with the WiiLibs files (`wl`) and ctest names (`wl_tests`) that depend on the path; `status: planned` for WiiLibs files not written yet). First match wins |
| `third_party/xboxrecomp/patches/NNNN-name.patch` | WiiLibs' changes to upstream code, as `git format-patch` files with a message written to be sent upstream; applied in the build tree by `tools/xbox_recomp.py`, never to the vendored copy |
| `build/xboxrecomp_sync/upstream.git` | the cache clone `fetch` makes (gitignored); synced commits are pinned under `refs/wl-sync/` |

Upstream has no tags: releases are commits titled `Release vX.Y.Z -- "Name"`, so the state records
full SHAs and takes the release name from the newest such commit at or before them.

## The procedure

1. **See where things stand.**

       python3 tools/xboxrecomp_sync.py fetch          # clone or update the cache clone
       python3 tools/xboxrecomp_sync.py status         # behind by N commits / releases; copy and patches ok?

   `status` fails if the vendored copy was edited (it must stay pristine: move the change into a
   patch) or a patch no longer applies to it.

2. **Write the brief.**

       python3 tools/xboxrecomp_sync.py brief -o build/xboxrecomp_sync/brief.md \
           --split-dir build/xboxrecomp_sync/briefs

   Default range: `triaged` to upstream's head (`--from`/`--to` for another). The brief has the
   release notes in the range, a summary by area, **licence flags**, what happens to the vendored
   copy and whether each patch still applies to the new upstream, mapping health (renames,
   deletions, files no rule covers), then one section per area: its commits, its files with what
   they map to (the WiiLibs files affected, the tests that cover them, the patches that touch them)
   and the questions its reviewer must answer. `--split-dir` writes the same per area
   (`brief_<area>.md`), each self-contained, one per research agent.

   Licence flags: **BLOCK** for LGPL/GPL text in a code file's header or a file upstream's `NOTICE`
   lists as LGPL (xemu-derived: `src/apu/*`, `src/nv2a/nv2a_regs.h`, and `nv2a_core.c`/`nv2a_state.h`
   by their headers): never copy its text or structure into WiiLibs (MIT); take facts (register
   offsets, method numbers, ordinals) and cite a public source. **REVIEW** for a code file that cites
   xemu, Cxbx or QEMU, says it is adapted or derived from something, carries another copyright, or
   adds a dependency. **INFO** for licence and attribution files. The flags are a triage list, not
   legal advice: a licence stated only further down a file is missed.

3. **Hand the brief to reviewers, one per area.** Each section names its owner (the toolchain, the
   runtime, graphics). For a research agent, give it its `brief_<area>.md` and this instruction:

   > You are researching one area of an xboxrecomp upstream update for WiiLibs. Read the brief
   > (`brief_<area>.md`) and the upstream changes it lists (`git -C build/xboxrecomp_sync/upstream.git
   > show <sha>`, or the files at the new commit). Answer every question in the brief with evidence:
   > upstream file:line at the new commit, a command you ran and its output, or a measurement. List
   > the WiiLibs changes you propose, file by file (the brief names the WiiLibs files affected), and
   > what you could not settle. Mark anything unverified as such. Do not edit the vendored copy
   > (third_party/xboxrecomp/); changes to upstream code are patches. Never copy text from a file
   > flagged BLOCK.

   What each area must check and report:

   | Area | Check | Report |
   |---|---|---|
   | lifter (`tools/recomp`) | every changed emission: does it assume a little-endian host (host pointer casts, memcpy of host values, union lane views, raw word atomics)? does a WiiLibs patch touch the same code, or did upstream fix the same bug? new helper or runtime symbol? | the generated-C change for each commit; patches to drop, rebase or add; new runtime ABI for `wl/xboxrt/recomp/README.md`; conformance and smoke results |
   | conformance (`tools/conformance`) | do new cases pass in the ppc-be replay? new case kinds or harness fields? | `ctest -R xbox_recomp_conformance` output; changes `tests/xbox_recomp/conformance.py` needs |
   | analysis (disasm, func_id, abi, xbe_parser) | function boundaries, seeds, sections swept; general fix or title heuristic? | smoke-check numbers before and after (functions, unresolved targets, chunks) |
   | runtime-abi (`templates/runtime`) | does `gen_recomp_types.py` still apply? is the regenerated big-endian header right? new extern symbols? | the header diff; ABI changes for the runtime's owner |
   | memory, kernel | guest memory model, kernel exports/ordinals/semantics; Win32 assumptions | what `wl/xboxrt` and `wl/xboxkrnl` should change (facts only from LGPL files) |
   | nv2a, d3d | PGRAPH methods, push-buffer parsing, combiners, vertex programs, formats | what `wl/xbox` and `wl/codec/xbox_tex` should mirror, with tests for `tests/xbox` |
   | audio, io | what an idle or absent device must answer; report layouts | runtime device changes |
   | docs, tests, tools, meta, build | new gotchas, pinned facts, tools worth vendoring, licence changes, dependencies | notes for the owning area; map rules to add |

4. **Merge the vendored copy.**

       python3 tools/xboxrecomp_sync.py merge            # dry run: files added/changed/removed, patches
       python3 tools/xboxrecomp_sync.py merge --apply    # (alias: apply) write it; UPSTREAM.json "vendored" moves

   The copy is pristine, so there is nothing to merge by hand: every upstream file mapped `vendored`
   or `patched` is copied as it is at the new commit and files upstream removed are removed. The
   merge refuses a vendored file with LGPL text, and reports each patch that no longer applies (the
   copy is still refreshed, so the patches can be fixed against it). New upstream files with no
   rule are listed: add rules (`check-map` must pass).

5. **Fix the patches** that no longer apply, or drop the ones upstream made unnecessary:

       python3 tools/xboxrecomp_sync.py patch-repo build/xboxrecomp_sync/patchwork
       # in that repository: git rebase -i upstream / git commit --amend / git am, as usual
       python3 tools/xboxrecomp_sync.py export-patches build/xboxrecomp_sync/patchwork

   A patch is a commit with a message saying what it fixes and why, its tests included (upstream's
   pytest suite must pass with it; add a test of the fix and, where the CPU can say, a conformance
   case in `tools/conformance/cases.py`). Mark files a patch touches `patched` in the map.

6. **Make the WiiLibs changes** the answers call for: regenerate the big-endian header (`python3
   wl/xboxrt/recomp/gen_recomp_types.py`, then review the diff; if an anchor fails, read upstream's
   change to that part of the template and update the edit), update `wl/xboxrt/recomp/README.md`'s ABI
   list, the runtime, the graphics, the map (`port` entries move from `planned` when written).

7. **Test.**

       python3 tools/xboxrecomp_sync.py check-map        # the map covers every upstream file; no stale rule
       python3 tools/xboxrecomp_sync.py test --wii       # integrity, patches, host and ppc-be ctest, Wii build

   The ctests that matter most: `xboxrecomp_pytest` (upstream's suite on the patched copy),
   `xbox_recomp_conformance` (every conformance case lifted and run big-endian under qemu-ppc against
   a recording of the x86 CPU), `xbox_recomp_e2e` (whole programs, same), `test_xbox_recomp_header`
   (host and ppc-be), `xbox_recomp_header_current`, `xbox_recomp_synthetic_title`. If the lifter or
   analysis changed, also run the smoke check on the reference title and compare with the table below:

       python3 tools/xbox_recomp_smoke.py /path/to/default.xbe

8. **Record the triage**, in the same commit as the work it records:

       python3 tools/xboxrecomp_sync.py advance --to <sha> --note "what was decided"

   `advance` refuses while the copy is not at that commit's vendored content or a patch does not
   apply.

## Pending: upstream v0.14.0 (eb0aa1d)

Looked at on 2026-10-19 (`fetch`, `brief`; not synced). The copy is at v0.13.1 (193e299, 2026-10-07)
and `check-map` is clean there (441 upstream files, all mapped; 158 vendored, 36 patches). Upstream's
head eb0aa1d (v0.14.0 "Other Machines", 2026-10-16) is 98 commits later (122 with merges; 167 files,
+18,953/-1,461), and the brief maps every new file (no area needs a decision). What it would bring:

| Area | Upstream commits | For WiiLibs |
|---|---|---|
| lifter (vendored, patched) | `cebfc64` neg in unsigned arithmetic; `e108d0e` xadd / lock xadd flags; `01064f3` FCMOVcc; `c50c597` MXCSR modelled, CVTSS2SI/CVTSD2SI round by its RC; `46aee91`/`6931018` in/out to runtime port I/O, the field pin at 0x80C0; `bb2b5c7` sahf, fcomi, comiss flags; `b40805c` x87 tbyte loads and stores; `2d38bcd` ficom; `999a8c6` x87 trig, remainder, scale edges; `8862b6c` SSE conversions and estimates; `f69a435` FPREM's C2; `bbbaadb` COMISD/UCOMISD double lanes; `2db0a2f` parity SETcc/CMOVcc; `8af3ef7` `tools/recomp/fpdiff.py`, an x87/SSE differential tester | most of it is fixed already by WiiLibs' patches: 0005 (m80), 0007 (parity), 0009 (port I/O), 0015 (sahf, C2), 0017 (neg and widths), 0018 (MXCSR, FCMOVcc) are to be dropped or cut to what upstream lacks, each against its conformance cases; the xadd flags and SSE rounding are new to WiiLibs |
| analysis (patched) | `0239dd3` ... `4b449d7` (18 commits): static callback recovery (callbacks hidden by aliases, passed as immediates, past inline switch tables, their dependencies and bounds); `34f5e1c` ... `801e288` (15 disasm fixes): tail closure, shared bodies, int3 padding; `2649f5f` blocks that alias bodies branch to | 0019 (discovery gaps) to rebase or drop; the Halo smoke check (functions, unresolved targets, `.text`) to compare with "Reference numbers" |
| runtime-abi (patched) | `templates/runtime/recomp_types.h` for MXCSR and port I/O | patches 0002, 0003, 0005, 0008, 0009, 0013, 0015, 0021, 0023, 0025, 0030, 0032 touch it; `wl/xboxrt/recomp/` gains the new runtime symbols (`RECOMP_PORT_IN`/`OUT` exist already through 0009) |
| nv2a, d3d (concept, port) | `1d050ac`, `19ef9f0`, `53f49ad` the pushbuffer executor (a DMA-style walk, fixed-function transform and lighting, back ends); `eed7f4e` oPos.w == 0 drawn with w = 1; `0ca0947` vertex-program triangles that never divide by w; `27c95f4` constant-arrival tracing | facts for `wl/xbox` (the w = 0 vertices, screen-space programs) |
| audio (concept; LGPL files) | `cde57b3` addresses above 64 MB; `148431d` pitch resampling; `a16163f` device-paced output; `e0d2525` FETINTSTS latched once a trap; `7fe4d49` one idle-voice trap at a time; `5e40efb` the sample-limit interrupt (ISTS bit 3), a real XGSCNT; diagnostics (`a87919d`, `2d9c96c`, `ed1432d`, `b7c311a`) | `src/apu/*` is LGPL (BLOCK): facts only, from a public source, for `wl/xboxapu` (the sample-limit interrupt: Black's DirectSound waited for it) |
| kernel, memory, io (concept) | `e7462cf` guest thread objects, TLS from TlsDataSize; `f775a5a` events in multiple-object waits; `ae11965` FATX overwrite and rename; `8d0442b` directory searches released at NtClose; `13d9637` a page allocator for the contiguous arena, MmAllocateContiguousMemoryEx's range; POSIX and arm64 hosts, SDL2, a keyboard/mouse pad | facts for `wl/xboxkrnl` (the multiple-object waits, the contiguous range) |
| conformance, tests, build | `607f62c` C fixtures under MSVC; `1f7ab62`, `c0af02d` exact comparisons; `60a947c` archives linked as a group | the conformance suite's new cases come with the vendored files |

Patches against eb0aa1d, in order (one that fails is left out): 0001, 0004, 0006, 0008, 0010, 0027 and 0028 apply; the other 29
do not (upstream's own fixes in the same lines, or the context of a patch left out before them). The sync is
`merge --to eb0aa1d --apply`, then step 5 above, patch by patch from 0002.

## Reference numbers

The smoke check on the reference title, Halo: Combat Evolved retail (01.10.12.2276), so a later
sync can see what changed. Code size is the summed `.text` of the generated objects compiled for the
Wii (devkitPPC 16.1, the flags above), not linked.

| Upstream | Configuration | Functions (detected / translated) | Unresolved call targets | Unimplemented instructions | `.text` -Os | `.text` -O2 |
|---|---|---|---|---|---|---|
| v0.13.1 + patches 0001-0014 (`--x87-locals --reg-locals`), 22 hot functions at -O2 | all executable sections | 7,474 / 7,493 | 0 | 90 (29 mnemonics) | 10,799,240 (10.30 MiB) | |
| v0.13.1 + patches 0001-0013 (`--x87-locals`) | all executable sections (the driver's default) | 7,474 / 7,493 | 0 | 90 (29 mnemonics) | 12,367,796 (11.79 MiB) | |
| v0.13.1 + patches 0001-0012, the header as of the x87 change (inline EA, inline MEMF_SET) | all executable sections | 7,474 / 7,493 | 0 | 90 (29 mnemonics) | 13,524,568 (12.90 MiB) | |
| v0.13.1 + patches 0001-0012 | all executable sections (the driver's default) | 7,474 / 7,493 | 0 (60 jumps in bytes nothing reaches go by address) | 90 (29 mnemonics) | 12,192,978 (11.63 MiB) | |
| v0.13.1 + patches 0001-0007 | all executable sections (the driver's default) | 7,474 / 7,493 | 294 | 106 (29 mnemonics) | 11,319,192 (10.79 MiB) | 15,741,715 (15.01 MiB) |
| v0.13.1 + patches 0001-0007 | `.text` only (`"text_only": true`) | 6,948 / 6,961 | 599 | 71 (21 mnemonics) | 10,963,718 (10.46 MiB) | |
| v0.13.1 unpatched, a byte-swapping probe header (research, r2) | `.text` only | 6,948 / 6,961 | 599 | 71 | 10,775,590 (10.28 MiB) | 17,119,463 (16.33 MiB) |

The newest row is the current state (the Wii executable was 15.04 MB before patch 0013 and 13.89 MB
after, per the commit that added it). The smoke check needs the game's XBE, so a change to the lifter
or the runtime header is measured again before the table is updated; the older rows come from the
commits that added them.

The unimplemented instructions are privileged or legacy ones (`in`, `out`, `cli`/`sti`,
`pushfd`/`popfd`, `hlt`, `iretd`, `wbinvd`, `cpuid`, `aam`, `bound`, ...). Some are data decoded as
code (the six `lock or eax, [eax]` in Halo's `.text` are not even valid: a lock needs a memory
destination), but an Xbox title runs in ring 0 and its XDK libraries may execute `cli`/`sti`,
`wbinvd` or port I/O for real: which sites are reached is known only at run time (`recomp_unimpl`
reports each one reached).

## Facts worth knowing about upstream

- When a doc and `templates/runtime/recomp_types.h` disagree, the header wins (several docs still
  describe plain-global registers or a hardcoded code range).
- `tail_jump_alias` entries (a second entry into another function's body) are lifted as complete
  copies: for Halo 2.00 MB of x86 is lifted from 1.63 MB of unique code. `--coalesce-functions` does
  not help: it treats those entries as having independent evidence and refuses them (1 of 562
  candidate owners passed its checks in a probe). Removing the duplication needs multi-entry
  functions in the lifter.
- Upstream lifts a function from its range and makes every direct jump out of it a tail call to the
  function at the target; where none starts (the middle of another body, the rest of a function past
  a false start from an immediate), the target got an "unresolved" stub that popped four bytes and
  returned. WiiLibs' patch 0010 lifts the code such a jump reaches into the jumping function's body
  instead (Halo: 314 functions gain 33,732 instructions, 2,204 at most), 0011 gives a called interior
  address a function, and 0012 sends what is left through `RECOMP_ITAIL`, which reports a miss. A
  loop that runs through two function starts is still a chain of host tail calls (none in Halo).
- Upstream's Halo work used the debug build `cachebeta.xbe`, not retail; addresses in upstream's
  code and docs are that build's.
