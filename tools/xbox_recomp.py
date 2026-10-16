#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Runs xboxrecomp's pipeline, with WiiLibs' patches, on an Xbox executable (XBE).

The pipeline is vendored pristine in third_party/xboxrecomp; WiiLibs' fixes to it are the patch
files in third_party/xboxrecomp/patches/. This tool copies the vendored tree into the output
directory, applies the patches there (never to the vendored copy, which stays re-syncable), and
runs the stages on the XBE:

    parse    tools.xbe_parser        sections, kernel imports           -> parse/analysis.json
    disasm   tools.disasm            every executable section, not only .text (--text-only is
                                     not passed: library sections hold code the title calls)
    func_id  tools.func_id           function classification            -> func_id/
    abi      tools.abi_analysis      calling conventions                -> abi/abi_functions.json
    recomp   tools.recomp --split    the C, into gen/                   -> gen/, recomp/summary.json

Each stage is skipped when its inputs (the patched pipeline, the XBE, the options, the files the
earlier stages wrote) hash the same as last time. The generated chunks are then wrapped into a
fixed set of translation units, tu/recomp_tu_00.c .. tu/recomp_tu_<N-1>.c plus
tu/recomp_tu_dispatch.c, tu/recomp_tu_stubs.c and tu/recomp_tu_hot.c, so a build system can name its
sources before the pipeline has run (wl_add_xbox_title in cmake/WiiLibs.cmake does). The chunk size
is chosen so each wrapper holds at most one chunk; an unused wrapper is empty. The functions the
title names as hot ("hot_functions", --hot-functions) are moved out of their chunks into
tu/recomp_tu_hot.c, which the build compiles at -O2 while the rest stays at -Os.

    xbox_recomp.py run default.xbe -o build/halo_recomp
    xbox_recomp.py run default.xbe -o out --config title.json --chunks 32
    xbox_recomp.py prepare -o build/pipeline     # only the patched pipeline copy
    xbox_recomp.py run ... --runtime-header template   # upstream's header instead of none

By default gen/ gets no recomp_types.h (the recomp stage's --runtime-header none): the build puts
WiiLibs' big-endian header, wl/xboxrt/recomp/recomp_types.h, on the include path.

The recomp stage keeps each function's x87 stack, control word and compare result in C locals,
written back to the g_fp_* globals only where control leaves the function (--x87-locals, patch
0013: the memory model's loads and stores were most of the time of x87 code on PowerPC; results are
identical), and its integer registers too (--reg-locals, patch 0014: every guest store made the
compiler reload them). A title's config can turn either off ("x87_locals": false,
"reg_locals": false). It writes a guest access's small constant displacement apart from its base
(--mem-disp, patch 0021: MEM32_D(esi, 0x8)), so the PowerPC header maps the base once for the
accesses through it ("mem_disp": false turns it off).

A title's own knowledge is data, passed with --config (JSON; relative paths are relative to it):

    {
      "name": "Halo",                        game name for the generated banners
      "seed_functions": ["seeds.json", "0x0001F000"],   files for disasm --seed-functions, or
                                             addresses (written to a seed file here)
      "extra_sections": ["XIPS"],            sections to treat as code
      "text_only": false,                    true restores disasm --text-only
      "manual_functions": {"0x00012345": "my_name"} or ["0x00012345"] or "manual.json",
                                             functions the title project writes by hand
      "coalesce_functions": ["bounds.json"], owner bounds for recomp --coalesce-functions
      "trace_functions": ["0x00012345"],     entry traces (recomp --trace-functions)
      "icall_sites": "icall_sites.json",     measured indirect-call targets (recomp --icall-sites)
      "patches": [{"va": "0x00032E64", "from": "6800006001", "to": "680000C000", "why": "..."}],
                                             byte changes to the XBE, each checked against the bytes
                                             it expects, made before lifting (a pushed constant
                                             lives in the C) and applied by the runtime to the
                                             image it loads (tu/recomp_tu_patches.c)
      "vertex_programs": "hdd/vsh_programs.inc",   the title's NV2A vertex programs as the runtime
                                             lists them, compiled to C at build time (not this
                                             tool's: wl_add_xbox_title's, tools/xbox_vsh_codegen.c)
      "split": 250,                          a fixed chunk size instead of the computed one
      "x87_locals": false,                   the x87 state in memory at every operation (the default
                                             true keeps it in locals inside a function)
      "reg_locals": false,                   the same for the integer registers (default true)
      "mem_disp": false,                     every access's address whole (the default true writes a
                                             small displacement apart: MEM32_D(esi, 0x8), patch 0021)
      "hot_functions": "hot.txt" or ["sub_00052520", "0x00088B80"],   the functions compiled at
                                             -O2 (tu/recomp_tu_hot.c); a file is read for its
                                             sub_XXXXXXXX names and 0x addresses (a profile report
                                             cut to the functions wanted will do)
      "disasm_args": [], "func_id_args": [], "abi_args": [], "recomp_args": []   passed through
    }

Python needs capstone (pip install capstone). Exit status: 0 done, 1 a stage failed, 2 bad usage.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

WL_ROOT = Path(__file__).resolve().parent.parent
VENDOR = WL_ROOT / "third_party" / "xboxrecomp"
PATCHES = VENDOR / "patches"
STATE = VENDOR / "UPSTREAM.json"
# Bumped when this driver changes what a stage is given, so cached stages rerun.
DRIVER_VERSION = 1
DEFAULT_CHUNKS = 32


class Failure(Exception):
    """A stage or the pipeline preparation failed; the message says which and why."""


# ------------------------------------------------------------------------------------- hashing

def git_blob_sha(data: bytes) -> str:
    """The git blob id of data (what `git hash-object` prints), without needing git."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def vendored_files(vendor: Path = VENDOR) -> dict[str, str]:
    """{upstream path: blob id} for every vendored upstream file, from UPSTREAM.json."""
    with open(vendor / "UPSTREAM.json", encoding="utf-8") as f:
        state = json.load(f)
    return dict(state["files"])


def patch_files(patches: Path = PATCHES) -> list[Path]:
    return sorted(patches.glob("[0-9][0-9][0-9][0-9]-*.patch")) if patches.is_dir() else []


def pipeline_hash(vendor: Path = VENDOR, patches: Path = PATCHES) -> str:
    """Identity of the patched pipeline: the vendored files as they are on disk, and the patches."""
    h = hashlib.sha256(b"wl-xbox-recomp %d\n" % DRIVER_VERSION)
    for rel in sorted(vendored_files(vendor)):
        h.update(rel.encode() + b"\0" + git_blob_sha((vendor / rel).read_bytes()).encode() + b"\n")
    for p in patch_files(patches):
        h.update(p.name.encode() + b"\0" + p.read_bytes())
    return h.hexdigest()


# --------------------------------------------------------------------------- patched pipeline

def check_pristine(vendor: Path = VENDOR) -> list[str]:
    """Vendored files whose content differs from the blob recorded in UPSTREAM.json."""
    changed = []
    for rel, blob in vendored_files(vendor).items():
        path = vendor / rel
        if not path.is_file() or git_blob_sha(path.read_bytes()) != blob:
            changed.append(rel)
    return changed


def apply_patches(tree: Path, patches: list[Path]) -> None:
    """git apply each patch to tree, in order. tree must not be inside a git work tree's view:
    GIT_CEILING_DIRECTORIES keeps git from finding an enclosing repository (a build directory
    inside the WiiLibs checkout), so `git apply` treats paths as relative to tree."""
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(tree.parent))
    for p in patches:
        r = subprocess.run(["git", "apply", "--whitespace=nowarn", str(p)], cwd=tree, env=env,
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise Failure(f"patch {p.name} does not apply to the vendored xboxrecomp:\n"
                          f"{r.stderr.strip()}")


def prepare_pipeline(dest: Path, vendor: Path = VENDOR, patches_dir: Path = PATCHES,
                     quiet: bool = False) -> Path:
    """A patched copy of the vendored pipeline at dest (reused when its hash matches)."""
    want = pipeline_hash(vendor, patches_dir)
    stamp = dest / ".wl_pipeline.json"
    try:
        if json.loads(stamp.read_text(encoding="utf-8")).get("hash") == want:
            return dest
    except (OSError, ValueError):
        pass
    if not quiet:
        changed = check_pristine(vendor)
        if changed:
            print("xbox_recomp: warning: the vendored copy differs from UPSTREAM.json in "
                  f"{len(changed)} file(s) (e.g. {changed[0]}); keep it pristine and put changes in "
                  "third_party/xboxrecomp/patches/", file=sys.stderr)
    if dest.exists():
        shutil.rmtree(dest)
    tmp = dest.with_name(dest.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    for rel in vendored_files(vendor):
        target = tmp / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(vendor / rel, target)
    patches = patch_files(patches_dir)
    apply_patches(tmp, patches)
    (tmp / ".wl_pipeline.json").write_text(json.dumps(
        {"hash": want, "patches": [p.name for p in patches]}, indent=1) + "\n", encoding="utf-8")
    tmp.rename(dest)
    if not quiet:
        print(f"xbox_recomp: patched pipeline ready ({len(patches)} patches) in {dest}",
              file=sys.stderr)
    return dest


# ---------------------------------------------------------------------------------- the stages

def write_if_changed(path: Path, text: str) -> bool:
    try:
        if path.read_text(encoding="utf-8") == text:
            return False
    except OSError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def _addr_list(items) -> list[str]:
    return [("0x%08X" % (int(a, 0) if isinstance(a, str) else int(a))) for a in items]


class Run:
    def __init__(self, args, config: dict, config_dir: Path):
        self.args = args
        self.config = config
        self.config_dir = config_dir
        self.out = Path(args.output).resolve()
        self.xbe = Path(args.xbe).resolve()
        self.python = args.python or sys.executable
        self.pipeline = None
        self.pipe_hash = None
        self.stats = {}

    def cfg_path(self, p: str) -> Path:
        q = Path(p)
        return q if q.is_absolute() else (self.config_dir / q).resolve()

    def log(self, msg: str) -> None:
        if not self.args.quiet:
            print("xbox_recomp: " + msg, file=sys.stderr)

    def stage(self, name: str, cmd: list[str], inputs: list[Path], outputs: list[Path],
              extra_key=None) -> bool:
        """Run one stage unless its key matches the stamp; returns True when it ran."""
        key_src = {"stage": name, "pipeline": self.pipe_hash, "cmd": cmd[1:], "extra": extra_key,
                   "inputs": {str(p): file_sha256(p) for p in inputs if p.is_file()}}
        key = hashlib.sha256(json.dumps(key_src, sort_keys=True).encode()).hexdigest()
        stamp = self.out / "stages" / f"{name}.key"
        if (not self.args.force and stamp.is_file() and stamp.read_text().strip() == key
                and all(o.exists() for o in outputs)):
            self.log(f"{name}: up to date")
            return False
        self.log(f"{name}: running")
        t0 = time.time()
        logf = self.out / "logs" / f"{name}.log"
        logf.parent.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONPATH=str(self.pipeline))
        with open(logf, "w", encoding="utf-8") as lf:
            lf.write("$ " + " ".join(cmd) + "\n")
            lf.flush()
            r = subprocess.run(cmd, cwd=self.pipeline, env=env, stdout=lf, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            tail = logf.read_text(encoding="utf-8", errors="replace").splitlines()[-25:]
            raise Failure(f"stage {name} failed (exit {r.returncode}); log {logf}:\n" + "\n".join(tail))
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(key + "\n")
        self.log(f"{name}: done in {time.time() - t0:.1f}s")
        return True

    def execute(self) -> dict:
        self.out.mkdir(parents=True, exist_ok=True)
        try:
            import capstone  # noqa: F401  (the disassembler needs it; fail early and clearly)
        except ImportError:
            if self.python == sys.executable:
                raise Failure("Python module 'capstone' is missing (pip install capstone)")
        self.pipeline = prepare_pipeline(self.out / "pipeline", quiet=self.args.quiet)
        self.pipe_hash = pipeline_hash()
        self.apply_patches()
        py, xbe, out, c = self.python, str(self.xbe), self.out, self.config

        analysis = out / "parse" / "analysis.json"
        analysis.parent.mkdir(parents=True, exist_ok=True)
        self.stage("parse", [py, "-m", "tools.xbe_parser", xbe, "--json", str(analysis)],
                   [self.xbe], [analysis])

        disasm = out / "disasm"
        dcmd = [py, "-m", "tools.disasm", xbe, "--analysis-json", str(analysis), "-o", str(disasm),
                "--force"]
        if c.get("text_only"):
            dcmd.append("--text-only")
        if c.get("extra_sections"):
            dcmd += ["--extra-sections", ",".join(c["extra_sections"])]
        seed_inputs = []
        inline = [s for s in c.get("seed_functions", []) if isinstance(s, int) or
                  (isinstance(s, str) and s.lower().startswith("0x"))]
        if inline:
            seeds = out / "config" / "seed_functions.json"
            write_if_changed(seeds, json.dumps([{"start": a} for a in _addr_list(inline)], indent=1) + "\n")
            seed_inputs.append(seeds)
        seed_inputs += [self.cfg_path(s) for s in c.get("seed_functions", []) if s not in inline]
        for s in seed_inputs:
            dcmd += ["--seed-functions", str(s)]
        dcmd += list(c.get("disasm_args", []))
        functions = disasm / "functions.json"
        self.stage("disasm", dcmd, [self.xbe, analysis] + seed_inputs, [functions])

        func_id = out / "func_id"
        self.stage("func_id", [py, "-m", "tools.func_id", xbe, "--functions", str(functions),
                               "--strings", str(disasm / "strings.json"), "--xrefs",
                               str(disasm / "xrefs.json"), "-o", str(func_id)]
                   + list(c.get("func_id_args", [])),
                   [self.xbe, functions, disasm / "strings.json", disasm / "xrefs.json"],
                   [func_id / "identified_functions.json"])

        abi = out / "abi" / "abi_functions.json"
        abi.parent.mkdir(parents=True, exist_ok=True)
        self.stage("abi", [py, "-m", "tools.abi_analysis", xbe, "--functions", str(functions),
                           "--identified", str(func_id / "identified_functions.json"),
                           "--output", str(abi)] + list(c.get("abi_args", [])),
                   [self.xbe, functions, func_id / "identified_functions.json"], [abi])

        with open(functions, encoding="utf-8") as f:
            n_functions = len(json.load(f))
        chunks = self.args.chunks
        split = int(c.get("split") or 0) or max(1, math.ceil((n_functions + 64) / max(1, chunks - 1)))
        gen = out / "gen"
        rcmd = [py, "-m", "tools.recomp", xbe, "--all", "--split", str(split),
                "--game-name", c.get("name", "Xbox title"), "-o", str(out / "recomp"),
                "--gen-dir", str(gen), "--disasm-dir", str(disasm), "--func-id-dir", str(func_id),
                "--abi-dir", str(abi.parent), "--runtime-header", self.args.runtime_header]
        if c.get("x87_locals", True):
            rcmd.append("--x87-locals")
        if c.get("reg_locals", True):
            rcmd.append("--reg-locals")
        if c.get("mem_disp", True):
            rcmd.append("--mem-disp")
        rin = [self.xbe, functions, disasm / "labels.json", func_id / "identified_functions.json", abi]
        if c.get("icall_sites"):
            rcmd += ["--icall-sites", str(self.cfg_path(c["icall_sites"]))]
            rin.append(self.cfg_path(c["icall_sites"]))
        manual = c.get("manual_functions")
        if manual:
            if isinstance(manual, str):
                mpath = self.cfg_path(manual)
            else:
                mpath = out / "config" / "manual_functions.json"
                body = ({("0x%08X" % int(k, 0)): v for k, v in manual.items()}
                        if isinstance(manual, dict) else _addr_list(manual))
                write_if_changed(mpath, json.dumps(body, indent=1) + "\n")
            rcmd += ["--manual-functions", str(mpath)]
            rin.append(mpath)
        if c.get("trace_functions"):
            tpath = out / "config" / "trace_functions.json"
            write_if_changed(tpath, json.dumps(_addr_list(c["trace_functions"])) + "\n")
            rcmd += ["--trace-functions", str(tpath)]
            rin.append(tpath)
        for cj in c.get("coalesce_functions", []):
            rcmd += ["--coalesce-functions", str(self.cfg_path(cj))]
            rin.append(self.cfg_path(cj))
        rcmd += list(c.get("recomp_args", [])) + list(self.args.recomp_arg or [])
        summary = out / "recomp" / "summary.json"
        self.stage("recomp", rcmd, rin, [summary, gen / "recomp_dispatch.c"],
                   extra_key={"runtime_header_file": (file_sha256(Path(self.args.runtime_header))
                              if self.args.runtime_header not in ("none", "template") else None)})

        with open(summary, encoding="utf-8") as f:
            stats = json.load(f)
        self.write_wrappers(gen, chunks, stats)
        self.write_patch_table()
        report = self.report(stats, n_functions, split)
        write_if_changed(out / "xbox_recomp.json", json.dumps(report, indent=1) + "\n")
        return report

    def apply_patches(self) -> None:
        """The title config's "patches": byte changes to the XBE, each checked against the bytes it
        expects, made to a copy (out/xbe/patched.xbe) that every stage then reads. A constant the
        code pushes (a cache's size, say) is lifted into the C, so it can only change here, before
        lifting; the runtime applies the same patches to the image it loads (tu/recomp_tu_patches.c),
        so data and code agree."""
        patches = self.config.get("patches", [])
        self.patches = []
        if not patches:
            return
        data = bytearray(self.xbe.read_bytes())
        u32 = lambda o: int.from_bytes(data[o:o + 4], "little")
        base, count, table = u32(0x104), u32(0x11C), u32(0x120) - u32(0x104)
        secs = []
        for i in range(count):
            o = table + i * 56
            secs.append((u32(o + 4), u32(o + 8), u32(o + 12), u32(o + 16)))  # va, vsize, raw, rawsize
        for i, p in enumerate(patches):
            try:
                va = int(str(p["va"]), 16) if isinstance(p["va"], str) else int(p["va"])
                old, new = bytes.fromhex(p["from"]), bytes.fromhex(p["to"])
            except (KeyError, ValueError) as e:
                raise Failure(f"patch {i}: needs \"va\", \"from\" and \"to\" (hex bytes): {e}")
            if len(old) != len(new) or not old:
                raise Failure(f"patch {i} at 0x{va:08X}: \"from\" and \"to\" must be the same, non-zero length")
            off = None
            for sva, _vsize, raw, rawsize in secs:
                if sva <= va and va + len(old) <= sva + rawsize:
                    off = raw + (va - sva)
            if off is None:
                raise Failure(f"patch {i} at 0x{va:08X}: not in any section's file bytes")
            if bytes(data[off:off + len(old)]) != old:
                raise Failure(f"patch {i} at 0x{va:08X}: expected {old.hex()}, the XBE has "
                              f"{bytes(data[off:off + len(old)]).hex()} (another build?)")
            data[off:off + len(new)] = new
            self.patches.append((va, old, new, str(p.get("why", ""))))
        dst = self.out / "xbe" / "patched.xbe"
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.is_file() or dst.read_bytes() != bytes(data):
            dst.write_bytes(bytes(data))
        self.log(f"{len(self.patches)} patches applied: {dst}")
        self.xbe = dst

    def write_patch_table(self) -> None:
        """tu/recomp_tu_patches.c: the patches as a table the runtime applies to the loaded image."""
        lines = ["/* Generated by tools/xbox_recomp.py: the title config's XBE patches (build tree",
                 " * only). wl/xboxrt applies them to the image after loading it. */",
                 "#include <stdint.h>",
                 "typedef struct { uint32_t va, bytes; const uint8_t *from, *to; } WlXbTitlePatch;"]
        rows = []
        for i, (va, old, new, why) in enumerate(getattr(self, "patches", [])):
            lines.append(f"static const uint8_t p{i}_from[] = {{{', '.join(f'0x{b:02X}' for b in old)}}};")
            lines.append(f"static const uint8_t p{i}_to[] = {{{', '.join(f'0x{b:02X}' for b in new)}}};")
            rows.append(f"    {{0x{va:08X}u, {len(old)}u, p{i}_from, p{i}_to}}, /* {why.replace('*/', '')} */")
        lines.append("extern const WlXbTitlePatch wl_xb_title_patches[];")
        lines.append("extern const uint32_t wl_xb_title_patch_count;")
        lines.append("const WlXbTitlePatch wl_xb_title_patches[] = {")
        lines += rows or ["    {0u, 0u, 0, 0},"]
        lines.append("};")
        lines.append(f"const uint32_t wl_xb_title_patch_count = {len(rows)}u;")
        write_if_changed(self.out / "tu" / "recomp_tu_patches.c", "\n".join(lines) + "\n")

    def hot_functions(self) -> set:
        """The function names the config ("hot_functions") and --hot-functions name hot."""
        texts = []
        items = self.config.get("hot_functions") or []
        if isinstance(items, str):
            items = [items]
        for item in items:
            if re.fullmatch(r"(sub_[0-9A-Fa-f]{8}|0[xX][0-9A-Fa-f]+|\d+)", str(item)):
                texts.append(str(item))
            else:
                texts.append(self.cfg_path(item).read_text(encoding="utf-8"))
        if self.args.hot_functions:
            texts.append(Path(self.args.hot_functions).read_text(encoding="utf-8"))
        names = set()
        for t in texts:
            for tok in re.findall(r"\b(sub_[0-9A-Fa-f]{8}|0[xX][0-9A-Fa-f]+)\b", t):
                names.add("sub_%08X" % int(tok, 16) if tok[:2].lower() == "0x" else
                          "sub_" + tok[4:].upper())
        return names

    def write_wrappers(self, gen: Path, n: int, stats: dict) -> None:
        """tu/recomp_tu_XX.c: a fixed set of translation units over the generated chunks."""
        chunks = sorted(gen.glob("recomp_[0-9][0-9][0-9][0-9].c"))
        count = stats.get("num_chunks", len(chunks))
        chunks = [p for p in chunks if int(p.stem.split("_")[1]) < count]
        if len(chunks) > n:
            self.log(f"warning: {len(chunks)} chunks for {n} translation units; some units hold "
                     "several (raise --chunks)")
        tu = self.out / "tu"

        # Each wrapper carries the hashes of what it includes. A build system knows only the
        # wrappers as the pipeline's outputs (the chunks' names are not known before it runs), and
        # ninja decides what is out of date before a rule's outputs are re-stat'ed; a wrapper
        # that stayed the same while its chunk changed left the object stale until the next build.
        def digest(path: Path) -> str:
            return hashlib.sha1(path.read_bytes()).hexdigest()[:16] if path.is_file() else "none"

        def include(path: Path) -> str:
            return f'#include "../gen/{path.name}" /* {digest(path)} */\n'

        head = ("/* Generated by tools/xbox_recomp.py: a fixed translation unit over the pipeline's\n"
                " * output (derived from the title's code; build tree only, never commit it).\n"
                f" * recomp_funcs.h {digest(gen / 'recomp_funcs.h')} */\n")

        # The hot functions leave their chunks for recomp_tu_hot.c: a chunk that loses one is
        # written into its wrapper without it, instead of included.
        hot = self.hot_functions()
        moved, cold = [], {}
        if hot:
            for p in chunks:
                lines = p.read_text(encoding="utf-8").split("\n")
                keep, i, took = [], 0, False
                while i < len(lines):
                    m = re.fullmatch(r"void (\w+)\(void\)", lines[i])
                    if m and m.group(1) in hot and i + 1 < len(lines) and lines[i + 1] == "{":
                        start = len(keep)
                        while start > 0 and keep[start - 1] != "/**":
                            start -= 1  # its header comment goes with it
                        start = start - 1 if start > 0 else len(keep)
                        j = lines.index("}", i)
                        moved.append("\n".join(keep[start:] + lines[i:j + 1]))
                        del keep[start:]
                        i, took = j + 1, True
                        continue
                    keep.append(lines[i])
                    i += 1
                if took:
                    cold[p] = "\n".join(keep)

        def unit(p: Path) -> str:
            if p in cold:
                return f"/* ../gen/{p.name} without its hot functions */\n{cold[p]}\n"
            return include(p)

        for i in range(n):
            mine = [p for j, p in enumerate(chunks) if j % n == i]
            body = "".join(unit(p) for p in mine) or \
                f"typedef int recomp_tu_{i:02d}_empty;\n"
            write_if_changed(tu / f"recomp_tu_{i:02d}.c", head + body)
        hot_body = ("#define RECOMP_GENERATED_CODE\n#include \"recomp_funcs.h\"\n#include <math.h>\n\n"
                    + "\n\n".join(moved) + "\n") if moved else "typedef int recomp_tu_hot_empty;\n"
        write_if_changed(tu / "recomp_tu_hot.c", head + hot_body)
        if hot:
            self.log(f"{len(moved)} of {len(hot)} hot functions in tu/recomp_tu_hot.c")
        self.stats["hot_functions"] = len(moved)
        write_if_changed(tu / "recomp_tu_dispatch.c", head + include(gen / "recomp_dispatch.c"))
        stubs = gen / "recomp_stubs_unresolved.c"
        write_if_changed(tu / "recomp_tu_stubs.c", head + (
            include(stubs)
            if stats.get("unresolved_stubs") and stubs.is_file() else "typedef int recomp_tu_stubs_empty;\n"))
        for old in tu.glob("recomp_tu_[0-9][0-9].c"):
            if int(old.stem.split("_")[2]) >= n:
                old.unlink()

    def report(self, stats: dict, n_functions: int, split: int) -> dict:
        unimpl = stats.get("unimplemented") or {}
        rep = {
            "xbe": str(self.xbe), "pipeline": self.pipe_hash[:16],
            "functions_detected": n_functions,
            "functions_translated": stats.get("translated"),
            "functions_failed": stats.get("failed"),
            "unresolved_call_targets": stats.get("unresolved_stubs", 0),
            "unimplemented_instructions": sum(len(v) for v in unimpl.values()),
            "unimplemented_mnemonics": {k: len(v) for k, v in
                                        sorted(unimpl.items(), key=lambda kv: -len(kv[1]))},
            "chunks": stats.get("num_chunks"), "split": split,
            "lines_of_c": stats.get("total_lines"),
            "hot_functions": self.stats.get("hot_functions", 0),
        }
        if "x87_locals" in stats:
            # functions using the x87: how many keep its state in locals, and why the rest do not
            rep["x87_locals"] = stats["x87_locals"]
        self.log("%(functions_translated)s/%(functions_detected)s functions translated "
                 "(%(functions_failed)s failed), %(unresolved_call_targets)s unresolved call targets, "
                 "%(unimplemented_instructions)s unimplemented instructions, %(chunks)s chunks" % rep)
        return rep


def load_config(path):
    if not path:
        return {}, Path.cwd()
    p = Path(path).resolve()
    with open(p, encoding="utf-8") as f:
        cfg = json.load(f)
    if not isinstance(cfg, dict):
        raise Failure(f"{path}: the title config must be a JSON object")
    known = {"name", "seed_functions", "extra_sections", "text_only", "manual_functions",
             "coalesce_functions", "trace_functions", "split", "disasm_args", "func_id_args",
             "abi_args", "recomp_args", "icall_sites", "patches", "comment", "vertex_programs", "x87_locals",
             "reg_locals", "mem_disp", "hot_functions"}
    unknown = sorted(set(cfg) - known)
    if unknown:
        raise Failure(f"{path}: unknown keys {unknown} (known: {sorted(known)})")
    return cfg, p.parent


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[1:]))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run", help="run the pipeline on an XBE")
    p.add_argument("xbe")
    p.add_argument("-o", "--output", required=True, help="output directory (a build directory)")
    p.add_argument("--config", help="the title's JSON config")
    p.add_argument("--chunks", type=int, default=DEFAULT_CHUNKS,
                   help=f"translation units to spread the code over (default {DEFAULT_CHUNKS})")
    p.add_argument("--runtime-header", default="none",
                   help="recomp's --runtime-header: none (default: the build supplies "
                        "WiiLibs' header), template, or a path")
    p.add_argument("--recomp-arg", action="append", help="one more argument for tools.recomp")
    p.add_argument("--hot-functions", help="a file naming more functions to compile at -O2 "
                   "(sub_XXXXXXXX names or 0x addresses, as in \"hot_functions\")")
    p.add_argument("--python", help="Python to run the pipeline with (default: this one)")
    p.add_argument("--force", action="store_true", help="rerun every stage")
    p.add_argument("--stamp", help="file to touch when the run succeeds (for build systems)")
    p.add_argument("-q", "--quiet", action="store_true")
    p = sub.add_parser("prepare", help="only make the patched pipeline copy")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "prepare":
            print(prepare_pipeline(Path(args.output).resolve(), quiet=args.quiet))
            return 0
        if args.chunks < 1:
            ap.error("--chunks must be at least 1")
        if not Path(args.xbe).is_file():
            raise Failure(f"no such XBE: {args.xbe}")
        cfg, cdir = load_config(args.config)
        Run(args, cfg, cdir).execute()
        if args.stamp:
            Path(args.stamp).parent.mkdir(parents=True, exist_ok=True)
            Path(args.stamp).write_text(time.strftime("%Y-%m-%d %H:%M:%S\n"))
        return 0
    except Failure as e:
        print(f"xbox_recomp: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
