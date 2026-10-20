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

## Synced: upstream v0.14.0 (eb0aa1d)

Taken on 2026-10-19, from v0.13.1 (193e299): 98 commits (122 with merges), 17 vendored files added and
33 changed, none removed; `check-map` clean (516 upstream files, 175 vendored, 39 patches). Upstream's
head had moved on to v0.15.0 (6376eb8) by then; that range is untriaged. What it brought:

| Area | Upstream commits | What WiiLibs did |
|---|---|---|
| lifter (vendored, patched) | `cebfc64` neg in unsigned arithmetic; `e108d0e` xadd / lock xadd flags; `01064f3` FCMOVcc; `c50c597` MXCSR modelled (`g_mxcsr`), CVTSS2SI/CVTSD2SI round by its RC through `recomp_cvt_si32(value, mxcsr, truncate)`; `46aee91`/`6931018` in/out to `xbox_PortIn`/`xbox_PortOut`; `bb2b5c7` sahf, fcomi, comiss flags; `b40805c` x87 tbyte loads and stores (`recomp_f80_load`/`store`); `2d38bcd` ficom; `999a8c6` x87 trig, remainder, scale edges (`recomp_fp_trig_in_range`, `recomp_fprem`, `recomp_fscale`); `8862b6c` SSE conversions and estimates (`recomp_rcpss`/`rsqrtss`, `recomp_sse_d2i`); `f69a435` FPREM's C2; `bbbaadb` COMISD/UCOMISD; `2db0a2f` parity SETcc/CMOVcc; `8af3ef7` `fpdiff.py` | each patch checked against upstream's code (below); `g_mxcsr` joins wl/xboxrt's register file (0x1F80 at reset); the header maps `xbox_PortIn`/`Out` to `wl_xb_port_in`/`out` and rounds `MMX_CVT_F2I` by `g_mxcsr` |
| analysis (patched) | 18 commits of static callback recovery (`probes_as_callback_body`: a closed CFG inside the gap, no size cap), 15 disasm fixes (tail closure, shared bodies, int3 padding), `2649f5f` | 0019's gap-bounded probe gave way to upstream's; BINKDATA as data is kept |
| runtime-abi (patched) | `templates/runtime/recomp_types.h`: the helpers above, `g_mxcsr`, `xbox_PortIn`/`Out` | `gen_recomp_types.py`: the MEMF_SET defaults end at the SSE section now, the port anchor is upstream's declarations |
| nv2a, d3d, audio, kernel, io (concept) | the pushbuffer executor, oPos.w == 0, the APU's sample-limit interrupt and pacing (LGPL `src/apu/*`: facts only), guest thread objects, multiple-object waits, the contiguous arena's allocator, POSIX hosts | nothing vendored; facts for `wl/xbox`, `wl/xboxapu`, `wl/xboxkrnl` left to their owners |

The patches, each rebased with git and compared with upstream's version of the same fix:
- dropped: 0009 (in/out): upstream's `xbox_PortIn`/`xbox_PortOut` do it. The number is left free so
  that the others, and every reference to them, keep theirs.
- cut to their conformance cases, the fix being upstream's: 0002 (also a test of `recomp_cvt_si32`'s
  edges), 0005 (upstream's m80 load differs from 0005's only for exponent 0 and unnormals, values far
  below a double's range), 0007, 0015, 0018. Upstream's own cases test parity after cmp/test only and
  C2 after fprem only; these run the floating-point compares, sahf and the quotient bits on the CPU.
- cut: 0019 keeps BINKDATA as data.
- kept, merged by hand: 0003 (beside upstream's tbyte store), 0016 (upstream's range test pushes
  conditionally and writes `g_fp_cc` inside `recomp_fp_trig_in_range`, out of the x87 locals pass's
  sight), 0017 (mul/imul join upstream's flag-snapshot setters), 0029 (beside upstream's FCMOV
  helpers). The helper lists of 0013, 0024, 0036 and 0038 name upstream's `recomp_f80_load`/`store`
  and `recomp_fscale`; `recomp_fprem` and `recomp_fp_trig_in_range` write `g_fp_cc` and stay calls.
- the other 25 applied as they were, and 0039 (stack reach, taken on Xbox beside the sync) applies to
  v0.14.0's lifter unchanged.
- new: 0040 (data-table shared tail). Upstream's `_pass_data_ptr_targets` now takes a table word in a
  gap only with a closed CFG inside the gap (`probes_as_callback_body`) or a proven tail; an entry that
  falls through into the next function, branches into its body or tail-jumps to a target found later
  was dropped. 0040 falls back to v0.13.1's linear probe (a ret or a jmp ahead): Halo's table entries
  1,472 -> 1,509, its functions 6,801 -> 6,843.

On Halo (retail, the title's own configuration) against perf/guest-code6 (v0.13.1): 7,687 lifted functions
became 6,801 and the lifted C 1.37 M lines 1.11 M. Of the 938 functions gone, 845 were `tail_jump_alias` copies
and 60 `gap_prologue` starts, none with a caller; 822 lie inside another function's body now (upstream's
alias and shared-body recovery). The Wii program's `sub_` code fell from 1,763,499 to 1,402,998 instructions
(-20.4%) and the DOL from 8,960,832 to 7,505,984 bytes; the hot TU is unchanged (61,880 / 61,881). The
backward jumps fell from 7,453 sites to 5,735 (dead copies: the jumps run differ by one in the first 292
million). The deterministic host runs of a10 and b30 (FRAME 33, HEAP 12, `bounded_loops` off) against Xbox
fc4b371 parted first at b30's tick 1328 (an awaited volume reached 10 ticks sooner) and at a10's audio
frame 144000. The cause was five functions reached only through data tables that the new discovery
dropped (0x2219C, 0x2223E, 0x180380, 0x1805D0, 0x180830): the MSVC CRT's fmod dispatches on its operands'
classes through a table at 0x25F638, and its entry for a zero dividend, 0x2219C, was not translated, so
the indirect jump resolved to nothing, fmod(0, y) returned y and leaked an x87 slot, and an animation
decoder's blend fraction came out 1.0 instead of 0.0 (guest RAM differed from tick 0). With 0040, a10
is identical (every line, screenshot and audio hash) and b30's lines and screenshots are; b30's audio
hash parts at frame 69000, and guest RAM dumps differ there only in timestamps. That is the virtual
clock: the lifted code runs a few backward jumps fewer (285,654,539 for 285,654,540 by the 65,536th
kernel call, 11 fewer of 347 million by the 131,072nd: loops shaped by the new function boundaries),
and the deterministic mode counts each. With backward jumps costing nothing, the two builds play b30
identically, its audio included.

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
