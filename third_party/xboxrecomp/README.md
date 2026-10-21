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
| `tools/kernel_audit/` | kernel import coverage, per-XDK ordinal remap | reference for the kernel (`wl/xboxkrnl`'s export table and argument sizes derive from it) |
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
| `0002-cvt-si32-helper.patch` | conformance cases for `cvt(t)ss2si`/`cvt(t)sd2si` at their edges (ties, signed zeroes, the int32 edges, infinities, NaN) and a test of `recomp_cvt_si32` against a Python reference; the helper itself is upstream's since v0.14.0 (c50c597) |
| `0003-memf-memd-set.patch` | float and double stores are `MEMF_SET`/`MEMD_SET` (a runtime can store the integer bits; the template keeps the assignment) |
| `0004-reserved-toolchain-names.patch` | newlib, glibc, libgcc and libogc names are reserved (mangled) like the MSVC CRT's |
| `0005-x87-m80.patch` | conformance cases for `fld m80`/`fstp m80` (a store's three parts, a round trip, loads at a rounding tie); the lifting is upstream's since v0.14.0 (b40805c) |
| `0006-runtime-header-option.patch` | `--runtime-header template|none|PATH`: a runtime can supply its own `recomp_types.h` without editing generated output |
| `0007-parity-setcc.patch` | conformance cases for the parity flag after `fcomi`/`fucomip`/`ucomiss` (float equality is `setnp; sete; and`); the lifting is upstream's since v0.14.0 (2db0a2f, bb2b5c7) |
| `0008-backedge-hook.patch` | `RECOMP_BACKEDGE()` before every backward jump (a nothing in the template): a runtime that serialises guest threads or delivers interrupts between guest operations gets a break-in point in loops that only poll memory |
| (0009) | dropped at v0.14.0: upstream lifts `in`/`out` to `xbox_PortIn`/`xbox_PortOut` (46aee91), which WiiLibs' header maps to wl/xboxrt's ports; the number is left free so the others keep theirs |
| `0010-interior-jump-targets.patch` | the code a jump, conditional jump, fall-through or switch arm reaches outside its function, where no function starts, is lifted into the function's own body (it was a call to a stub that popped four bytes a jump never pushed and returned, skipping the rest: Halo had 270 inside lifted code, its hs `and` evaluator among them, split by a false function start); switch tables read with bounds; nothing is split or merged |
| `0011-interior-call-entries.patch` | a called address inside lifted code that no function starts at gets a function of its own (its code from there, and what that reaches), in the dispatch table too |
| `0012-dispatch-unlifted-targets.patch` | a direct target that still has no function (undecodable, outside every section, from bytes nothing reaches) goes through `RECOMP_ITAIL`, which runs what the runtime has there or reports the miss through `recomp_icall_fail_log`; stubs (only for functions a run leaves out) dispatch the same way instead of popping |
| `0013-x87-locals.patch` | `--x87-locals`: inside a function the x87 stack, control word and compare result live in C locals, and memory (`g_fp_stack`, `g_fp_top`, ...) is written back only where control leaves the function or calls out, and read again where it next needs it; the depth is followed through the function's own C statements, and a join at different depths goes through memory (every x87 operation reloaded the index and loaded or stored a double in memory: most of the time of x87 code on PowerPC). Results are identical; the template gains `RECOMP_FP_PC_CW` |
| `0014-reg-locals.patch` | `--reg-locals`: the same for the integer registers (`g_eax` .. `g_esp`, which every guest store, a volatile access the compiler must assume aliases them, made it reload): locals in each function, written back (those that may have been assigned) before a call, a tail call or a return. The header's SIMD helpers no longer end a function's x87 locals |
| `0015-x87-status.patch` | conformance cases for the x87 status word through `fnstsw`/`sahf` after an unordered compare (`fprem`/`fprem1`'s C2 and quotient bits, the transcendentals' C2, `fxam`, `fcompp`); the lifting is upstream's since v0.14.0 (bb2b5c7, f69a435, 999a8c6) |
| `0016-x87-trig-depth.patch` | `fsin`/`fcos`/`fsincos`/`fptan` compute every argument with libm and clear C2 (upstream's range test makes the push of `fptan`/`fsincos` conditional, and a function whose stack depth is not static loses the x87 locals: three of Halo's grew by half) |
| `0017-integer-width-flags.patch` | one-operand `mul`/`imul`/`div`/`idiv` at the operand's width (AX, DX:AX), the divisor read once, a 32-bit path for `xor edx, edx; div` and `cdq; idiv`; `mul`/`imul` publish CF = OF (`seto`/`setno` fused); two- and three-operand `imul` multiply unsigned (no signed overflow in C); the signed conditions after a narrow `sub` at its width and after `add`/`neg` with OF; a shift by zero leaves the flags; `cwd` |
| `0018-mxcsr-fcmov.patch` | conformance cases for `fcmovcc` after `fcomi` (eight conditions; ordered, equal and unordered) and `stmxcsr`; the lifting is upstream's since v0.14.0 (c50c597, 01064f3) |
| `0019-discovery-gaps.patch` | `BINKDATA` is data (Bink's tables: their pointers are scanned, the blitters they name found, and its bytes make no garbage functions); upstream's gap-bounded callback probe (v0.14.0) replaced this patch's own |
| `0020-mmx-cases.patch` | conformance cases for every MMX operation in Bink's decoder, the whole 64-bit result compared |
| `0021-mem-disp.patch` | `--mem-disp`: a small displacement of a guest access is its own operand (`MEMxx_D(base, disp)`), so that a runtime whose guest-to-host map is not a plain addition maps the base once |
| `0022-simd-reg-locals.patch` | `--reg-locals` keeps the SSE and MMX registers in C locals too |
| `0023-stack-disp.patch` | `--stack-disp`: an access at `esp + disp` is `MEMxx_SP(esp, disp)` (about half of a function's guest accesses are to its own stack) |
| `0024-keep-preserved.patch` | `--keep-preserved`: ebx, esi and edi stay in the caller's locals across a call to a function proven to give them back |
| `0025-mem-disp-args.patch` | `--mem-disp` splits the address of float stores and SSE/MMX accesses too |
| `0026-simd-loop-latch.patch` | `--reg-locals` keeps the SIMD registers in their locals round a loop, written back after it |
| `0027-fpu-status-test.patch` | `fcom; fnstsw ax; test ah, imm; jcc` branches on the compare's outcome |
| `0028-rep-stosd-memset.patch` | `rep stosd` of a byte-uniform value forwards over memory is a `memset` |
| `0029-short-rep-string.patch` | short `rep movs`, `stos` and `cmps` inline (MSVC's memcpy is `rep movsd` and then `rep movsb` for 0..3 bytes) |
| `0030-fist-round-small.patch` | `fist` and `frndint` round a value under 2^30 in integers, not through floor, fmod and ldexp |
| `0031-preserve-ebp-data.patch` | `--keep-preserved` follows ebp used as a general register (code built with `/Oy`) |
| `0032-fist-wide-cold.patch` | `fist` and `frndint` keep their wide path out of line |
| `0033-bounded-loops.patch` | `--bounded-loops`: no `RECOMP_BACKEDGE()` in loops that end by themselves (one latch, no call, an exit test on a register the loop moves by a constant) |
| `0034-rodata-consts.patch` | `--rodata-consts`: a float or double read from a fixed address in read-only data is a literal |
| `0035-regs-without-x87.patch` | the integer and SIMD registers go into locals where the function's x87 stack cannot be kept in them |
| `0036-callee-regs.patch` | `--callee-regs`: around a call, only the registers the callee touches are written back and reloaded |
| `0037-x87-balanced.patch` | `--x87-balanced`: the caller's x87 positions stay in locals across a callee that leaves the stack balanced |
| `0038-stack-locals.patch` | `--stack-locals`: a function's own stack slots in C locals, not byte-reversed guest memory |
| `0039-stack-reach.patch` | `--stack-deltas` and `--stack-reach` for `--stack-locals`: a function's esp effect kept where it sets esp from its frame, and across a call only the slots below the callee's reach written back and reloaded |
| `0040-data-table-shared-tail.patch` | a data-table entry in a gap may share its neighbour's code: v0.14.0's discovery takes a table word in a gap only with a closed CFG inside the gap or a proven tail, which dropped Halo's MSVC CRT `fmod` classes (its entry for a zero dividend) and four more; this falls back to v0.13.1's linear probe (a ret or a jmp ahead) |
| `0041-stack-frames.patch` | `--stack-icall`, `--stack-align` and `--stack-alloca` for `--stack-locals`: esp followed past indirect calls the caller does not clean, `and esp, -2^k` frames, and the alloca probe (`esp -= eax`) |
| `0042-movsx-bp-sp.patch` | `movsx r32, bp` and `movsx r32, sp` sign-extend (upstream's facb626, in v0.15.0, taken as it is) and the conformance case `movsx_bp_32`: v0.14.0 zero-extended them, and Halo has 213 `movsx r32, bp`; dropped at the sync to v0.15.0, its case kept |

To change one: `tools/xboxrecomp_sync.py patch-repo DIR` makes a git repository of the pristine copy
with each patch as a commit; edit there (amend, rebase), then `tools/xboxrecomp_sync.py
export-patches DIR` writes them back here.

## Following upstream

`UPSTREAM.json` is the state (the commit the copy is at, the commit triaged up to, and the blob ids);
`upstream_map.json` is the policy (every upstream path mapped to vendored, patched, port, concept or
ignore, with the WiiLibs files and tests that depend on it). `docs/xboxrecomp-sync.md` is the
procedure: fetch, brief (a research brief per area), merge, re-apply the patches, test, advance.
