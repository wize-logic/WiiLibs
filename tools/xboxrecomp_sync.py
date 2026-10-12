#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Follows xboxrecomp upstream: refresh the vendored copy, re-apply WiiLibs' patches, brief reviewers.

WiiLibs vendors part of xboxrecomp (https://github.com/sp00nznet/xboxrecomp, MIT) pristine in
third_party/xboxrecomp/ and keeps its own changes as patch files beside it. Two files there drive
this tool:

    UPSTREAM.json       state: the upstream commit the copy is at (full SHA, date, release), the
                        commit triaged up to (everything before it was read and decided), and the
                        git blob id of every vendored file
    upstream_map.json   policy: research areas, and for every upstream path what WiiLibs does with
                        it -- vendored | patched | port | concept | ignore -- with the WiiLibs files
                        and tests that depend on it

Commands (docs/xboxrecomp-sync.md is the procedure):

    status                      the copy's commit, how far upstream has moved, copy integrity,
                                whether the patches apply
    fetch [--url URL]           clone or update the cache clone (build/xboxrecomp_sync/upstream.git)
    brief [--from A] [--to B] [-o FILE] [--split-dir DIR]
                                the research brief for A..B (default: triaged..upstream head):
                                commits, release notes, files by area with what they map to,
                                licence flags (LGPL, xemu/Cxbx/QEMU mentions), the patches against
                                B, questions per area and the WiiLibs files affected; --split-dir
                                writes one brief per area for one research agent each
    merge [--to B] [--apply]    (alias: apply) refresh the pristine copy to B: every upstream file
                                mapped vendored or patched, copied as is; files upstream removed
                                are removed; then the patches are applied to a scratch copy and
                                each failure is reported. Without --apply: a dry run
    check-map                   the map covers every upstream file, no rule is stale, the copy
                                matches the map and UPSTREAM.json, patched files are the ones the
                                patches touch, and the WiiLibs files the map names exist
    test [--dry-run] [--wii]    integrity, patches, then WiiLibs' host and ppc-be builds and ctest
                                (and the Wii build with --wii)
    advance --to B [--release R] [--note N]
                                record that A..B was triaged; refuses while the copy is not at B's
                                vendored content or a patch does not apply
    patch-repo DIR              a git repository of the pristine copy with each patch as a commit,
                                for editing the patches (git commit --amend, rebase -i ...)
    export-patches DIR          write that repository's commits back as patches/NNNN-name.patch

Standard library only (Python 3.9+), plus git on PATH. Upstream is read from a git repository:
--repo, else the cache clone `fetch` makes. Nothing is ever written to --repo except the cache
clone's refs (fetch, and the refs/wl-sync/ pins that keep a synced commit from being collected).
Exit status: 0 ok, 1 a problem was found (or a command failed), 2 bad usage.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import OrderedDict, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xbox_recomp  # noqa: E402  (the patched-pipeline helpers are shared with the driver)

DEFAULT_WL_ROOT = HERE.parent
VENDOR_KINDS = ("vendored", "patched")
KINDS = ("vendored", "patched", "port", "concept", "ignore")
# Files of the vendor directory that are WiiLibs' own, not upstream's.
OWN_FILES = ("README.md", "UPSTREAM.json", "upstream_map.json")


class Problem(Exception):
    pass


# ------------------------------------------------------------------------------------- git

class Repo:
    def __init__(self, path: Path):
        self.path = Path(path)

    def git(self, *args, check=True, binary=False, input=None) -> str | bytes:
        r = subprocess.run(["git", "-C", str(self.path), *args], capture_output=True,
                           input=input)
        if check and r.returncode != 0:
            raise Problem("git %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
        return r.stdout if binary else r.stdout.decode("utf-8", "replace")

    def exists(self) -> bool:
        return self.path.exists() and subprocess.run(
            ["git", "-C", str(self.path), "rev-parse", "--git-dir"], capture_output=True).returncode == 0

    def rev(self, ref: str) -> str:
        return self.git("rev-parse", "--verify", "--quiet", ref + "^{commit}").strip()

    def tree(self, commit: str) -> dict[str, str]:
        """{path: blob id} of every file at commit."""
        out = {}
        for line in self.git("ls-tree", "-r", "-z", commit).split("\0"):
            if not line:
                continue
            meta, path = line.split("\t", 1)
            mode, typ, sha = meta.split()
            if typ == "blob":
                out[path] = sha
        return out

    def blob(self, sha: str) -> bytes:
        return self.git("cat-file", "blob", sha, binary=True)

    def show(self, commit: str, path: str) -> bytes | None:
        r = subprocess.run(["git", "-C", str(self.path), "show", f"{commit}:{path}"], capture_output=True)
        return r.stdout if r.returncode == 0 else None

    def commit_info(self, commit: str) -> dict:
        h, date, subj = self.git("log", "-1", "--format=%H%x1f%cs%x1f%s", commit).strip().split("\x1f", 2)
        rel = self.git("log", "-1", "--grep=^Release v", "--format=%s", commit).strip()
        m = re.match(r"Release (v\d+\.\d+\.\d+)", rel)
        return {"commit": h, "date": date, "subject": subj, "release": m.group(1) if m else ""}

    def commits(self, a: str, b: str) -> list[dict]:
        out = self.git("log", "--reverse", "--format=%H%x1f%an%x1f%as%x1f%s", f"{a}..{b}")
        res = []
        for line in out.splitlines():
            h, an, ad, s = line.split("\x1f", 3)
            pr = re.search(r"#(\d+)", s)
            res.append(dict(sha=h, author=an, date=ad, subject=s, pr=pr.group(1) if pr else None,
                            merge=s.startswith("Merge "), release=s.startswith("Release v")))
        return res

    def changed(self, a: str, b: str) -> list[dict]:
        """Files changed a..b, renames detected, with line counts."""
        files, stats = [], {}
        num = self.git("diff", "--numstat", "-M50%", "--no-ext-diff", a, b).splitlines()
        for line in num:
            add, dele, p = line.split("\t", 2)
            if " => " in p:
                if "{" in p:
                    p = re.sub(r"\{(.*?) => (.*?)\}", r"\2", p).replace("//", "/")
                else:
                    p = p.split(" => ")[1]
            stats[p] = (0 if add == "-" else int(add), 0 if dele == "-" else int(dele))
        for line in self.git("diff", "--name-status", "-M50%", "--no-ext-diff", a, b).splitlines():
            parts = line.split("\t")
            st = parts[0]
            if st[0] in "RC":
                files.append(dict(status=st[0], score=int(st[1:] or 100), old=parts[1], path=parts[2]))
            else:
                files.append(dict(status=st[0], score=None, old=None, path=parts[1]))
        for f in files:
            f["add"], f["del"] = stats.get(f["path"], (0, 0))
        return files

    def is_ancestor(self, a: str, b: str) -> bool:
        return subprocess.run(["git", "-C", str(self.path), "merge-base", "--is-ancestor", a, b],
                              capture_output=True).returncode == 0


def blob_id(data: bytes) -> str:
    return xbox_recomp.git_blob_sha(data)


# ----------------------------------------------------------------------------- map and state

def glob_re(pat: str):
    out, i = "", 0
    while i < len(pat):
        if pat.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif pat.startswith("**", i):
            out += ".*"
            i += 2
        elif pat[i] == "*":
            out += "[^/]*"
            i += 1
        elif pat[i] == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(pat[i])
            i += 1
    return re.compile("^" + out + "$")


class Rules:
    """First match wins, in file order: the specific rule goes before the general one."""

    def __init__(self, rules: list, key: str):
        self.rules = [(glob_re(r[key]), r) for r in rules]

    def match(self, path: str):
        for rx, r in self.rules:
            if rx.match(path):
                return r
        return None


def load_json(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f, object_pairs_hook=OrderedDict)


def save_json(path: Path, data) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    tmp.replace(path)


class Ctx:
    def __init__(self, args):
        self.wl = Path(args.wl_root).resolve()
        self.vendor = self.wl / "third_party" / "xboxrecomp"
        self.state_path = Path(args.state).resolve() if args.state else self.vendor / "UPSTREAM.json"
        self.map_path = Path(args.map).resolve() if args.map else self.vendor / "upstream_map.json"
        self.cache = self.wl / "build" / "xboxrecomp_sync" / "upstream.git"
        self.repo = Repo(Path(args.repo).resolve() if args.repo else self.cache)
        self.mp = load_json(self.map_path)
        self.state = load_json(self.state_path) if self.state_path.exists() else None
        self.rules = Rules(self.mp["rules"], "upstream")
        self.areas = Rules(self.mp["areas"], "path")
        self.patches_dir = self.vendor / "patches"

    def need_state(self):
        if not self.state:
            raise Problem(f"no state file {self.state_path}: vendor first (merge --to <commit> --apply)")
        return self.state

    def need_repo(self):
        if not self.repo.exists():
            raise Problem(f"no upstream repository at {self.repo.path}: run `fetch` (or pass --repo)")
        return self.repo

    def head(self) -> str:
        branch = self.mp["upstream"].get("branch", "main")
        for ref in (branch, f"refs/heads/{branch}", f"origin/{branch}", "HEAD"):
            try:
                h = self.repo.rev(ref)
                if h:
                    return h
            except Problem:
                continue
        raise Problem(f"cannot find branch {branch} in {self.repo.path}")

    def resolve(self, ref: str | None, default: str | None) -> str:
        ref = ref or default
        if not ref:
            raise Problem("no commit given")
        h = self.repo.rev(ref)
        if not h:
            raise Problem(f"unknown commit {ref} in {self.repo.path} (run `fetch`?)")
        return h

    def kind(self, path: str) -> str:
        r = self.rules.match(path)
        return r["kind"] if r else "UNMAPPED"

    def area(self, path: str) -> str:
        a = self.areas.match(path)
        return a["area"] if a else "other"

    def vendored_set(self, tree: dict[str, str]) -> dict[str, str]:
        return {p: b for p, b in tree.items() if self.kind(p) in VENDOR_KINDS}

    def patches(self) -> list[Path]:
        return xbox_recomp.patch_files(self.patches_dir)


def patch_touches(patch: Path) -> list[str]:
    """Paths a patch modifies or creates (a/ b/ prefixes stripped)."""
    out = []
    for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^diff --git a/(\S+) b/(\S+)$", line)
        if m:
            out.append(m.group(2))
    return out


def patch_subject(patch: Path) -> str:
    for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("Subject:"):
            return re.sub(r"^Subject:\s*(\[PATCH[^\]]*\]\s*)?", "", line).strip()
    return patch.stem


def try_patches(files: dict[str, bytes], patches: list[Path]) -> list[dict]:
    """Apply the patches, in order, to a scratch tree of files; one result per patch."""
    results = []
    with tempfile.TemporaryDirectory(prefix="wl-xr-") as td:
        root = Path(td) / "tree"
        for rel, data in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(root.parent))
        for p in patches:
            chk = subprocess.run(["git", "apply", "--check", "--whitespace=nowarn", str(p)],
                                 cwd=root, env=env, capture_output=True, text=True)
            if chk.returncode == 0:
                subprocess.run(["git", "apply", "--whitespace=nowarn", str(p)], cwd=root, env=env,
                               capture_output=True, text=True, check=True)
                results.append(dict(patch=p.name, ok=True, detail=""))
            else:
                results.append(dict(patch=p.name, ok=False, detail=chk.stderr.strip()))
    return results


# ------------------------------------------------------------------------------- licences

LIC_PATTERNS = [
    ("LGPL/GPL text", re.compile(r"\b(LGPL|GPL|Lesser General Public|GNU General Public)\b", re.I)),
    ("cites xemu", re.compile(r"\bxemu\b", re.I)),
    ("cites Cxbx", re.compile(r"\bcxbx", re.I)),
    ("cites QEMU", re.compile(r"\bqemu\b", re.I)),
    ("adapted/extracted from", re.compile(r"\b(adapted from|based on|extracted from|ported from|derived from)\b", re.I)),
    ("another copyright", re.compile(r"copyright\s*(\(c\)|©)(?!.*sp00nz)", re.I)),
]
LICENSE_DOC_RX = re.compile(r"^(LICENSE.*|NOTICE|LICENSES/.*|.*/LICENSE.*|CONTRIBUTORS\.md)$")
CODE_EXT = (".c", ".h", ".cpp", ".hpp", ".py", ".java", ".sh", ".cmake", ".inc", ".s", ".S")


def notice_lgpl_paths(repo: Repo, commit: str) -> set[str]:
    data = repo.show(commit, "NOTICE")
    if not data:
        return set()
    return set(re.findall(r"^\s{2,}((?:src|tools|templates)/[\w./-]+)\s", data.decode("utf-8", "replace"), re.M))


def licence_flags(ctx: Ctx, a: str, b: str, files: list[dict]) -> list[dict]:
    """BLOCK: LGPL/GPL in a code file's header or listed LGPL in upstream NOTICE (never copy into
    MIT WiiLibs; if such a file is mapped vendored or patched, the copy is refused). REVIEW: a code
    file cites xemu/Cxbx/QEMU, says it is adapted/derived from something, carries another
    copyright, or adds a dependency. INFO: licence and attribution files changed."""
    repo = ctx.repo
    flags = []
    lgpl = notice_lgpl_paths(repo, a) | notice_lgpl_paths(repo, b)
    for f in files:
        p = f["path"]
        kind = ctx.kind(f["old"] or p)
        if kind == "ignore":
            continue
        if LICENSE_DOC_RX.match(p):
            flags.append(dict(path=p, level="INFO", kind=kind,
                              why=f"licence/attribution file changed ({f['status']}): re-read it; mirror "
                                  "new LGPL files in upstream_map.json as concept with a licence note"))
        if f["status"] == "D":
            continue
        is_code = p.endswith(CODE_EXT) or p.endswith("CMakeLists.txt")
        if p in lgpl:
            flags.append(dict(path=p, level="BLOCK", kind=kind, why="listed LGPL in upstream NOTICE"))
            continue
        diff = repo.git("diff", "-U0", "-M50%", a, b, "--", p)
        added = [l[1:] for l in diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
        data = repo.show(b, p)
        head = []
        if data and b"\0" not in data[:4096]:
            head = data.decode("utf-8", "replace").splitlines()[:60]
        hits_h = [n for n, rx in LIC_PATTERNS if any(rx.search(l) for l in head)]
        hits_a = [n for n, rx in LIC_PATTERNS if any(rx.search(l) for l in added)]
        hits = sorted(set(hits_h) | set(hits_a))
        if hits:
            area = ctx.area(p)
            if "LGPL/GPL text" in hits_h and is_code:
                lvl = "BLOCK"
            elif area in ("docs", "meta") or not is_code:
                lvl = "INFO"
            else:
                lvl = "REVIEW"
            why = ", ".join(hits) + (" (in the file header)" if hits_h else " (in added lines)")
            if lvl == "BLOCK" and p not in lgpl:
                why += "; not in upstream's NOTICE list"
            if lvl == "BLOCK" and kind in VENDOR_KINDS:
                why += "; MAPPED AS VENDORED: change the map to concept, it must not be copied"
            flags.append(dict(path=p, level=lvl, kind=kind,
                              why=("new file: " if f["status"] == "A" else "") + why))
        for l in added:
            if re.search(r"FetchContent|find_package|pip install|apt(-get)? install|git clone|import\s+(numpy|pefile)", l):
                flags.append(dict(path=p, level="REVIEW", kind=kind, why="new dependency line: " + l.strip()[:80]))
                break
    return flags


def vendored_licence_blocks(ctx: Ctx, commit: str, vend: dict[str, str]) -> list[str]:
    """Vendored files at commit that carry LGPL/GPL text or that NOTICE lists LGPL."""
    lgpl = notice_lgpl_paths(ctx.repo, commit)
    bad = []
    for p, sha in vend.items():
        if p in lgpl:
            bad.append(f"{p}: listed LGPL in NOTICE")
            continue
        if not p.endswith(CODE_EXT):
            continue
        head = ctx.repo.blob(sha)[:8192].decode("utf-8", "replace").splitlines()[:60]
        if any(LIC_PATTERNS[0][1].search(l) for l in head):
            bad.append(f"{p}: LGPL/GPL text in its header")
    return bad


# -------------------------------------------------------------------------------- questions

# Who should research an area, and what they must answer. {wl} is replaced by the WiiLibs files
# the changed paths map to.
AREA_OWNER = {
    "lifter": "the toolchain (third_party/xboxrecomp patches, wl/xboxrt/recomp, tools/xbox_recomp.py)",
    "conformance": "the toolchain",
    "analysis": "the toolchain",
    "runtime-abi": "the toolchain and the runtime (wl/xboxrt)",
    "memory": "the runtime (wl/xboxrt)",
    "kernel": "the runtime (wl/xboxkrnl)",
    "nv2a": "graphics (wl/xbox)",
    "d3d": "graphics (wl/xbox)",
    "audio": "the runtime (wl/xboxrt audio devices) and wl/audio",
    "io": "the runtime (wl/xboxrt devices) and wl/platform input",
    "tools": "the toolchain",
    "docs": "anyone; route findings to the owning area",
    "tests": "the owner of what each test covers",
    "meta": "a licence reviewer",
    "build": "the toolchain",
    "other": "whoever adds the map rule",
}

QUESTIONS = {
    "lifter": [
        "For each changed emission (an f-string producing C), what C does it produce now? Does it assume a "
        "little-endian host: a host pointer cast other than XBOX_PTR, memcpy of host values into guest "
        "memory, a union lane view (.q/.ud[]/.d[]) mixed with another, a raw word atomic?",
        "Does it change a line a WiiLibs patch also changes (see the patch results)? If upstream fixed "
        "the same bug, drop the patch; otherwise rebase it (patch-repo / export-patches).",
        "Does it emit a helper or call a runtime function that is new? It must exist in "
        "templates/runtime/recomp_types.h and in wl/xboxrt/recomp/recomp_types.h (regenerate it); a new "
        "extern goes into wl/xboxrt/recomp/README.md's ABI list and to the runtime's owner.",
        "Do `ctest -R xbox_recomp_conformance` (ppc-be against the x86 CPU) and `ctest -R xboxrecomp_pytest` "
        "pass? Root-cause every new failure; list a case as expected only with the reason.",
        "Run tools/xbox_recomp_smoke.py on the reference title and compare functions, unresolved targets, "
        "unimplemented instructions and .text with the table in docs/xboxrecomp-sync.md.",
    ],
    "conformance": [
        "New or changed cases: do they pass in WiiLibs' ppc-be replay (ctest -R xbox_recomp_conformance)? "
        "A new case kind or harness field the replay generator (tests/xbox_recomp/conformance.py) must learn?",
        "Does a harness change alter what is compared (eax, x87 depth, XMM bytes) in a way the replay must copy?",
    ],
    "analysis": [
        "Does it change function boundaries, seeds or which sections are swept? Run the smoke check before and "
        "after: functions, unresolved call targets, chunk sizes.",
        "Is it a general detector fix, or a heuristic tied to one title (cachebeta/Burnout addresses)? Title "
        "knowledge must stay out of WiiLibs (a title config can carry it).",
    ],
    "runtime-abi": [
        "What changed in templates/runtime/recomp_types.h? Does wl/xboxrt/recomp/gen_recomp_types.py still "
        "apply (python3 wl/xboxrt/recomp/gen_recomp_types.py --check) and is the regenerated big-endian "
        "header right: memory macros, unions, atomics, TLS, the FP precision fast path, icall counters?",
        "Any new extern symbol or function the runtime must define? Update wl/xboxrt/recomp/README.md and tell "
        "the runtime's owner.",
    ],
    "memory": [
        "Does the guest memory model change (layout, mirrors, contiguous/physical windows, MMIO ranges, how "
        "MmGetPhysicalAddress answers)? Does wl/xboxrt's model (docs: the EA transform, lazy frames) need it?",
        "Is the change a general fact about the Xbox, or a workaround for one title's addresses?",
    ],
    "kernel": [
        "Which kernel exports, ordinals, argument sizes or object semantics changed (waits, APCs, files, threads)? "
        "Does wl/xboxkrnl need the same, and is the per-XDK ordinal table affected?",
        "Does it assume Win32 (handles, VEH, CreateFileMapping)? What is the portable counterpart?",
    ],
    "nv2a": [
        "Does PGRAPH method handling, push-buffer parsing (jumps, calls, semaphores, fences), register state "
        "or the combiner/vertex program behaviour change in a way wl/xbox must mirror? Add a case to tests/xbox.",
        "Read the licence flags first: xemu-derived code is LGPL; take only facts (method numbers, offsets) "
        "and cite a public source (envytools, nouveau).",
    ],
    "d3d": [
        "Which D3D8 states, formats or vertex paths changed, and do they reach wl/xbox (psh, vsh) or "
        "wl/codec/xbox_tex (formats, swizzle)?",
    ],
    "audio": [
        "Does the APU/AC97/DSOUND behaviour a title probes at start change (what an idle or absent device must "
        "answer)? src/apu is LGPL: facts only.",
    ],
    "io": [
        "Input, USB or video changes: a report layout, timing or probe answer the runtime's devices must give?",
    ],
    "tools": [
        "A new host tool worth vendoring (add a vendored rule), or a helper for a platform WiiLibs does not "
        "target? What does it need (capstone, Ghidra, wine, ffmpeg, numpy, pefile)?",
    ],
    "docs": [
        "Does the text record a gotcha, a measured result or a failure mode WiiLibs should know? Note it in "
        "docs/xboxrecomp-sync.md's facts or in the owning module's README. When a doc and recomp_types.h "
        "disagree, the header wins.",
    ],
    "tests": [
        "What fact does each new test pin? Should WiiLibs' runtime or header pin the same (a test in "
        "tests/xbox_recomp, wl/xboxrt or wl/xboxkrnl)?",
    ],
    "meta": [
        "Licence or attribution change: does NOTICE list a new LGPL file? Mark it concept in "
        "upstream_map.json with a licence note. Does third_party/README.md need a change?",
    ],
    "build": ["Does a build change add a dependency or an option WiiLibs' driver must pass?"],
    "other": ["Unclassified: add an area and a rule to upstream_map.json so the next brief files it."],
}


# ------------------------------------------------------------------------------------- brief

def build_brief(ctx: Ctx, a: str, b: str) -> tuple[str, dict]:
    repo = ctx.repo
    cs = repo.commits(a, b)
    files = repo.changed(a, b)
    flags = licence_flags(ctx, a, b, files)
    ia, ib = repo.commit_info(a), repo.commit_info(b)
    patches = ctx.patches()
    touched = {p.name: patch_touches(p) for p in patches}

    by_area = defaultdict(list)
    for f in files:
        f["area"] = ctx.area(f["path"]) if ctx.area(f["path"]) != "other" else ctx.area(f["old"] or f["path"])
        f["rule"] = ctx.rules.match(f["old"] or f["path"])
        f["patches"] = [n for n, ps in touched.items() if (f["old"] or f["path"]) in ps or f["path"] in ps]
        by_area[f["area"]].append(f)
    commit_areas = {}
    for c in cs:
        names = repo.git("diff-tree", "--no-commit-id", "--name-only", "-r", "-m", "--first-parent",
                         c["sha"]).split()
        commit_areas[c["sha"]] = sorted({ctx.area(n) for n in names})

    # the patches against B's vendored files
    tree_b = repo.tree(b)
    vend_b = ctx.vendored_set(tree_b)
    patch_results = try_patches({p: repo.blob(s) for p, s in vend_b.items()}, patches) if patches else []
    vend_changes = [f for f in files if ctx.kind(f["old"] or f["path"]) in VENDOR_KINDS
                    or ctx.kind(f["path"]) in VENDOR_KINDS]

    out = []
    w = out.append
    w(f"# xboxrecomp upstream brief: {a[:7]}..{b[:7]}\n")
    w(f"Generated by `tools/xboxrecomp_sync.py brief` on {datetime.date.today().isoformat()}. "
      f"Upstream: {ctx.mp['upstream']['url']} (branch {ctx.mp['upstream'].get('branch', 'main')}).")
    w(f"Range: `{a[:10]}` ({ia['release'] or 'unreleased'}, {ia['date']}) to `{b[:10]}` "
      f"({ib['release'] or 'unreleased'}, {ib['date']}); {len(cs)} commits "
      f"({sum(1 for c in cs if c['merge'])} merges), {len(files)} files changed "
      f"(+{sum(f['add'] for f in files)} / -{sum(f['del'] for f in files)}).")
    if ctx.state:
        w(f"WiiLibs' vendored copy is at `{ctx.state['vendored']['commit'][:10]}` "
          f"({ctx.state['vendored'].get('release', '')}); triaged up to `{ctx.state['triaged']['commit'][:10]}`.")
    if not repo.is_ancestor(a, b):
        w(f"\n> WARNING: {a[:10]} is not an ancestor of {b[:10]}: upstream history was rewritten or the "
          "range is reversed. The file diff is still exact (tree to tree); the commit list is not.")
    w("")
    w("How to use this brief: one reviewer (or research agent) per area section below. Each answers "
      "every question in its section with evidence (file:line in upstream at the new commit, a test "
      "run, a measurement) and lists the WiiLibs changes it proposes. docs/xboxrecomp-sync.md has the "
      "procedure.\n")

    notes = release_notes(repo, b, ia["release"])
    if notes:
        w("## Release notes in range (upstream README changelog)\n")
        for s_ in notes:
            w(f"### {s_['version']} \"{s_['name']}\" ({s_['date']})\n")
            body = [l for l in s_["body"] if l.strip()]
            for l in body[:40]:
                w(re.sub(r"\[(@?[\w-]+)\]\([^)]*\)", r"\1", l)[:400])
            if len(body) > 40:
                w(f"... {len(body) - 40} more lines in the README")
            w("")

    w("## Summary by area\n")
    w("| Area | Owner | Files | +lines | -lines | Kinds | Needs a decision |")
    w("|---|---|---:|---:|---:|---|---:|")
    order = []
    for r in ctx.mp["areas"]:
        if r["area"] not in order:
            order.append(r["area"])
    order.append("other")
    seen = [o for o in order if o in by_area]
    for o in seen:
        fs = by_area[o]
        kinds = defaultdict(int)
        for f in fs:
            kinds[(f["rule"] or {}).get("kind", "UNMAPPED")] += 1
        need = sum(1 for f in fs if f["status"] in "DR" or not f["rule"])
        w(f"| {o} | {AREA_OWNER.get(o, '')} | {len(fs)} | {sum(f['add'] for f in fs)} | "
          f"{sum(f['del'] for f in fs)} | {', '.join('%s x%d' % kv for kv in sorted(kinds.items()))} | {need} |")
    w("")

    w("## Licence flags\n")
    if flags:
        w("| Level | Path | Kind | Why |")
        w("|---|---|---|---|")
        for fl in sorted(flags, key=lambda x: (["BLOCK", "REVIEW", "INFO"].index(x["level"]), x["path"])):
            w(f"| {fl['level']} | `{fl['path']}` | {fl['kind']} | {fl['why']} |")
        w("\nBLOCK: never copy the file's text or structure into WiiLibs (MIT); take facts only, cite a public "
          "source. REVIEW: read the header and the diff before using the change. INFO: licence or attribution "
          "text to re-read.")
    else:
        w("None: no LGPL/GPL text, no xemu/Cxbx/QEMU mention, no third-party copyright and no dependency line "
          "was added or changed in this range, and no licence file changed.")
    w("")

    w("## Vendored copy and patches\n")
    if vend_changes:
        w(f"{len(vend_changes)} vendored file(s) change: `merge --to {b[:10]} --apply` brings them over "
          "(a pristine copy, so there is nothing to merge by hand).")
    else:
        w("No vendored file changes in this range.")
    if patch_results:
        w("")
        w("| Patch | Applies to upstream@%s | Touches changed files |" % b[:7])
        w("|---|---|---|")
        changed_paths = {f["path"] for f in files} | {f["old"] for f in files if f["old"]}
        for r in patch_results:
            hit = sorted(set(touched.get(r["patch"], [])) & changed_paths)
            w(f"| `{r['patch']}` | {'yes' if r['ok'] else '**NO**'} | {', '.join('`%s`' % h for h in hit) or '-'} |")
        for r in patch_results:
            if not r["ok"]:
                w(f"\n`{r['patch']}` fails:\n```\n{r['detail'][:1500]}\n```")
        w("\nA patch that touches a changed file needs a look even when it applies: upstream may have fixed "
          "the same thing (drop the patch) or changed what the patch relies on.")
    w("")

    w("## Mapping health\n")
    renames = [f for f in files if f["status"] == "R"]
    deletes = [f for f in files if f["status"] == "D"]
    unmapped = [f for f in files if f["status"] != "D" and not ctx.rules.match(f["path"])]
    w(f"- Renamed upstream: {len(renames)}; deleted: {len(deletes)}; with no mapping rule: {len(unmapped)}.")
    for f in renames:
        w(f"  - R{f['score']} `{f['old']}` -> `{f['path']}` ({ctx.kind(f['old'])} -> {ctx.kind(f['path'])})")
    for f in deletes:
        w(f"  - D `{f['path']}` ({ctx.kind(f['path'])}): "
          + ("merge removes the vendored copy; check nothing in WiiLibs imports it"
             if ctx.kind(f['path']) in VENDOR_KINDS else "check the WiiLibs counterpart still has a source of truth"))
    for f in unmapped:
        w(f"  - `{f['path']}` (area {f['area']}): decide vendored / port / concept / ignore and add a rule")
    w("")

    for ar in seen:
        fs = by_area[ar]
        w(f"## Area: {ar}\n")
        w(f"Owner: {AREA_OWNER.get(ar, '')}.\n")
        cl = [c for c in cs if ar in commit_areas.get(c["sha"], []) and not c["merge"] and not c["release"]]
        if cl:
            w(f"Commits touching this area ({len(cl)}):")
            for c in cl:
                w(f"- `{c['sha'][:7]}` {c['subject'][:110]}{' [#%s]' % c['pr'] if c['pr'] else ''} "
                  f"({c['author']}, {c['date']})")
            w("")
        w("| St | Upstream file | +/- | Kind | WiiLibs files affected | Tests | Patches |")
        w("|---|---|---:|---|---|---|---|")
        affected = []
        for f in sorted(fs, key=lambda x: x["path"]):
            r = f["rule"] or {}
            kind = r.get("kind", "UNMAPPED")
            wl = list(r.get("wl", []))
            if kind in VENDOR_KINDS:
                wl.insert(0, f"{ctx.mp.get('vendor_dir', 'third_party/xboxrecomp')}/{f['path']}")
            if r.get("status") == "planned":
                wl = [x + " (planned)" for x in wl]
            affected += wl
            w(f"| {f['status']}{f['score'] or ''} | `{f['path']}`{(' (was `%s`)' % f['old']) if f['old'] else ''} "
              f"| +{f['add']}/-{f['del']} | {kind}{(' [' + r['license'] + ']') if r.get('license') else ''} | "
              f"{', '.join('`%s`' % x for x in wl) or '-'} | {', '.join('`%s`' % t for t in r.get('wl_tests', [])) or '-'} "
              f"| {', '.join('`%s`' % p for p in f['patches']) or '-'} |")
        w("")
        uniq = list(OrderedDict.fromkeys(affected))
        if uniq:
            w("WiiLibs files to check: " + ", ".join(f"`{x}`" for x in uniq) + ".\n")
        w("Questions (answer each, with evidence):")
        for q in QUESTIONS.get(ar, QUESTIONS["other"]):
            w("- [ ] " + q)
        rule_notes = sorted({(f["rule"] or {}).get("notes") for f in fs if (f["rule"] or {}).get("notes")})
        for n in rule_notes:
            w("- Map note: " + n)
        w("")

    w("## Tests to run after the merge\n")
    w("- `python3 tools/xboxrecomp_sync.py test` (integrity, patches, host and ppc-be builds and ctest; "
      "`--wii` adds the Wii build)")
    tests = sorted({t for f in files for t in ((f["rule"] or {}).get("wl_tests", []))})
    if tests:
        w("- Directly affected: " + ", ".join(f"`ctest -R {t}`" for t in tests))
    w("- The reference title, if the lifter or analysis changed: `tools/xbox_recomp_smoke.py <xbe>`")
    w("")
    w("## Finishing\n")
    w(f"After the merge, the patch fixes and the WiiLibs changes the answers call for are committed: "
      f"`python3 tools/xboxrecomp_sync.py advance --to {b[:10]} --note \"...\"`, and commit UPSTREAM.json "
      "with that work.")
    data = dict(by_area=by_area, files=files, flags=flags, commits=cs, patch_results=patch_results)
    return "\n".join(out) + "\n", data


CHANGELOG_RX = re.compile(r'^###\s+(v\d+\.\d+\.\d+)\s+[—–-]+\s+\*?"?(.+?)"?\*?\s+\((.+?)\)\s*$')


def release_notes(repo: Repo, b: str, since: str) -> list[dict]:
    data = repo.show(b, "README.md")
    if not data:
        return []
    secs, cur = [], None
    for ln in data.decode("utf-8", "replace").splitlines():
        m = CHANGELOG_RX.match(ln)
        if m:
            cur = dict(version=m.group(1), name=m.group(2), date=m.group(3), body=[])
            secs.append(cur)
        elif ln.startswith("## ") and cur:
            cur = None
        elif cur is not None:
            cur["body"].append(ln)

    def vt(v):
        return tuple(int(x) for x in v[1:].split("."))
    base = vt(since) if since else (0, 0, 0)
    return [s for s in secs if vt(s["version"]) > base]


# ---------------------------------------------------------------------------------- commands

def integrity(ctx: Ctx) -> tuple[list[str], list[str], list[str]]:
    """(modified or missing vendored files, files in the vendor dir that are not accounted for,
    vendored files missing from the state)."""
    st = ctx.need_state()
    bad = []
    for rel, blob in st["files"].items():
        p = ctx.vendor / rel
        if not p.is_file():
            bad.append(f"missing: {rel}")
        elif blob_id(p.read_bytes()) != blob:
            bad.append(f"modified: {rel}")
    extra = []
    for p in sorted(ctx.vendor.rglob("*")):
        if p.is_dir() or "__pycache__" in p.parts:
            continue
        rel = p.relative_to(ctx.vendor).as_posix()
        if rel in st["files"] or rel in OWN_FILES or rel.startswith("patches/"):
            continue
        extra.append(rel)
    return bad, extra, []


def cmd_status(ctx: Ctx, args) -> int:
    st = ctx.need_state()
    v, t = st["vendored"], st["triaged"]
    print(f"upstream        : {ctx.mp['upstream']['url']} ({ctx.mp['upstream'].get('branch', 'main')})")
    print(f"vendored copy   : {v['commit']} {v.get('release', '')} {v.get('date', '')}")
    print(f"triaged up to   : {t['commit']} {t.get('release', '')} {t.get('date', '')}")
    print(f"vendored files  : {len(st['files'])}; patches: {len(ctx.patches())}")
    rc = 0
    if ctx.repo.exists():
        head = ctx.head()
        n = int(ctx.repo.git("rev-list", "--count", f"{t['commit']}..{head}").strip())
        rels = [c for c in ctx.repo.commits(t["commit"], head) if c["release"]]
        print(f"upstream head   : {head[:12]} ({ctx.repo.path}); behind by {n} commit(s), {len(rels)} release(s)")
        for c in rels:
            print(f"                  {c['sha'][:10]} {c['subject']}")
    else:
        print(f"upstream repo   : none at {ctx.repo.path} (run `fetch`)")
    bad, extra, _ = integrity(ctx)
    if bad or extra:
        rc = 1
        print("copy integrity  : PROBLEMS (the copy must stay pristine; changes go into patches/)")
        for b in bad + [f"not vendored: {e}" for e in extra]:
            print("                  " + b)
    else:
        print("copy integrity  : ok (every file matches UPSTREAM.json)")
    files = {rel: (ctx.vendor / rel).read_bytes() for rel in st["files"] if (ctx.vendor / rel).is_file()}
    for r in try_patches(files, ctx.patches()):
        print(f"patch           : {r['patch']}: {'applies' if r['ok'] else 'DOES NOT APPLY'}")
        if not r["ok"]:
            rc = 1
            print("                  " + r["detail"].replace("\n", "\n                  "))
    return rc


def cmd_fetch(ctx: Ctx, args) -> int:
    url = args.url or ctx.mp["upstream"]["url"]
    repo = ctx.repo
    if not repo.exists():
        if repo.path != ctx.cache:
            raise Problem(f"{repo.path} is not a git repository")
        repo.path.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(["git", "clone", "--bare", "--quiet", url, str(repo.path)])
        if r.returncode:
            raise Problem(f"git clone {url} failed")
        print(f"cloned {url} into {repo.path}")
    elif repo.path == ctx.cache:
        repo.git("fetch", "--quiet", "--prune", url, "+refs/heads/*:refs/heads/*")
        print(f"fetched {url} into {repo.path}")
    else:
        print(f"{repo.path} is not the cache clone: not fetching into it (update it yourself)")
    if ctx.state and repo.path == ctx.cache:
        for key in ("vendored", "triaged"):
            sha = ctx.state[key]["commit"]
            if repo.rev(sha):
                repo.git("update-ref", f"refs/wl-sync/{sha[:12]}", sha)
    print(f"upstream head: {ctx.head()}")
    return 0


def cmd_brief(ctx: Ctx, args) -> int:
    ctx.need_repo()
    st = ctx.state or {}
    a = ctx.resolve(args.frm, (st.get("triaged") or {}).get("commit"))
    b = ctx.resolve(args.to, ctx.head())
    text, data = build_brief(ctx, a, b)
    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"wrote {args.output} ({text.count(chr(10))} lines)")
    elif not args.split_dir:
        sys.stdout.write(text)
    if args.split_dir:
        d = Path(args.split_dir)
        d.mkdir(parents=True, exist_ok=True)
        head = text.split("## Summary by area")[0]
        lic = re.search(r"(## Licence flags\n.*?)(?=\n## )", text, re.S)
        pat = re.search(r"(## Vendored copy and patches\n.*?)(?=\n## )", text, re.S)
        for ar in data["by_area"]:
            sec = re.search(r"(## Area: %s\n.*?)(?=\n## |\Z)" % re.escape(ar), text, re.S)
            body = [head.rstrip() + "\n",
                    f"This file is the brief for one area, **{ar}**. Answer every question below and report: "
                    "findings with evidence (upstream file:line at the new commit, test output, measurements), "
                    "the WiiLibs changes you propose (file by file), and anything you could not settle.\n",
                    sec.group(1) if sec else "", lic.group(1) if lic else ""]
            if ar in ("lifter", "runtime-abi", "conformance") and pat:
                body.append(pat.group(1))
            (d / f"brief_{ar}.md").write_text("\n".join(body) + "\n", encoding="utf-8")
        print(f"wrote {len(data['by_area'])} per-area brief(s) to {d}")
    return 0


def cmd_merge(ctx: Ctx, args) -> int:
    repo = ctx.need_repo()
    b = ctx.resolve(args.to, ctx.head())
    info = repo.commit_info(b)
    tree = repo.tree(b)
    unmapped = sorted(p for p in tree if not ctx.rules.match(p))
    vend = ctx.vendored_set(tree)
    old = (ctx.state or {}).get("files", {})
    added = sorted(set(vend) - set(old))
    removed = sorted(set(old) - set(vend))
    changed = sorted(p for p in vend if p in old and vend[p] != old[p])
    print(f"refresh to {b[:12]} {info['release']} ({info['date']}): {len(vend)} vendored files; "
          f"{len(added)} new, {len(changed)} changed, {len(removed)} removed")
    for tag, lst in (("A", added), ("M", changed), ("D", removed)):
        for p in lst:
            print(f"  {tag} {p}")
    rc = 0
    if unmapped:
        print(f"{len(unmapped)} upstream file(s) have no mapping rule (add rules; check-map lists them):")
        for p in unmapped[:20]:
            print(f"  ? {p}")
    blocks = vendored_licence_blocks(ctx, b, vend)
    if blocks:
        print("REFUSED: vendored files with an LGPL/GPL licence (change their rule to concept):")
        for x in blocks:
            print("  " + x)
        return 1
    files = {p: repo.blob(s) for p, s in vend.items()}
    results = try_patches(files, ctx.patches())
    for r in results:
        print(f"  patch {r['patch']}: {'applies' if r['ok'] else 'DOES NOT APPLY'}")
        if not r["ok"]:
            rc = 1
            print("    " + r["detail"].replace("\n", "\n    "))
    if not args.apply:
        print("(dry run: nothing written; pass --apply)")
        return rc
    if ctx.state:
        bad, extra, _ = integrity(ctx)
        if bad and not args.force:
            raise Problem("the vendored copy was edited (" + "; ".join(bad[:5]) + "); move the change into a "
                          "patch, or pass --force to overwrite it")
    for p in removed:
        f = ctx.vendor / p
        if f.exists():
            f.unlink()
        d = f.parent
        while d != ctx.vendor and d.exists() and not any(d.iterdir()):
            d.rmdir()
            d = d.parent
    for p, data in files.items():
        f = ctx.vendor / p
        f.parent.mkdir(parents=True, exist_ok=True)
        if not f.exists() or f.read_bytes() != data:
            f.write_bytes(data)
    st = ctx.state or OrderedDict(schema=1)
    st["schema"] = 1
    st["upstream"] = OrderedDict(name=ctx.mp["upstream"]["name"], url=ctx.mp["upstream"]["url"],
                                 branch=ctx.mp["upstream"].get("branch", "main"))
    st["vendored"] = OrderedDict(commit=b, date=info["date"], release=info["release"], subject=info["subject"])
    if "triaged" not in st:
        st["triaged"] = OrderedDict(commit=b, date=info["date"], release=info["release"],
                                    note="initial vendoring")
    st.setdefault("history", [])
    st["files"] = OrderedDict(sorted(vend.items()))
    save_json(ctx.state_path, st)
    ctx.state = st
    if repo.path == ctx.cache:
        repo.git("update-ref", f"refs/wl-sync/{b[:12]}", b)
    print(f"vendored copy refreshed; {ctx.state_path.name} updated (triage cursor unchanged: run `advance` "
          "after review)")
    if rc:
        print("some patches no longer apply: fix them with `patch-repo` / `export-patches`")
    return rc


def cmd_check_map(ctx: Ctx, args) -> int:
    st = ctx.need_state()
    repo = ctx.need_repo()
    commit = st["vendored"]["commit"]
    tree = repo.tree(commit)
    problems = []
    unmapped = sorted(p for p in tree if not ctx.rules.match(p))
    for p in unmapped:
        problems.append(f"unmapped upstream file: {p}")
    for r in ctx.mp["rules"]:
        if r["kind"] not in KINDS:
            problems.append(f"rule {r['upstream']}: unknown kind {r['kind']}")
        rx = glob_re(r["upstream"])
        if not any(rx.match(p) for p in tree):
            problems.append(f"stale rule (matches no upstream file): {r['upstream']}")
    vend = ctx.vendored_set(tree)
    if set(vend) != set(st["files"]):
        for p in sorted(set(vend) - set(st["files"])):
            problems.append(f"mapped vendored but not in UPSTREAM.json: {p} (run merge --apply)")
        for p in sorted(set(st["files"]) - set(vend)):
            problems.append(f"in UPSTREAM.json but not mapped vendored: {p}")
    touched = set()
    for p in ctx.patches():
        for t in patch_touches(p):
            touched.add(t)
            if t in tree and ctx.kind(t) != "patched":
                problems.append(f"{p.name} changes {t}, which the map does not mark patched")
    for r in ctx.mp["rules"]:
        if r["kind"] == "patched" and not any(ch in r["upstream"] for ch in "*?") and r["upstream"] not in touched:
            problems.append(f"{r['upstream']} is marked patched but no patch changes it")
        for f in r.get("wl", []):
            if r.get("status") == "planned" or " " in f or "(" in f:
                continue
            if not (ctx.wl / f).exists():
                problems.append(f"rule {r['upstream']}: WiiLibs file {f} does not exist")
    bad, extra, _ = integrity(ctx)
    problems += [f"copy: {b}" for b in bad] + [f"copy: not vendored: {e}" for e in extra]
    print(f"upstream files at {commit[:10]}: {len(tree)}; mapped {len(tree) - len(unmapped)}; "
          f"vendored {len(vend)}; rules {len(ctx.mp['rules'])}; patches {len(ctx.patches())}")
    for p in problems:
        print("  " + p)
    print("ok" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


def cmd_test(ctx: Ctx, args) -> int:
    steps = [("status (copy integrity, patches)", [sys.executable, str(HERE / "xboxrecomp_sync.py"),
                                                   "--wl-root", str(ctx.wl), "status"])]
    bd = ctx.wl / "build" / "xboxrecomp_sync"
    for name, extra in (("host", []), ("ppc-be", [f"-DCMAKE_TOOLCHAIN_FILE={ctx.wl}/cmake/powerpc-linux-gnu.cmake"])):
        d = bd / name
        steps += [(f"{name}: configure", ["cmake", "-S", str(ctx.wl), "-B", str(d), "-G", "Ninja"] + extra),
                  (f"{name}: build", ["cmake", "--build", str(d)]),
                  (f"{name}: ctest", ["ctest", "--test-dir", str(d), "--output-on-failure", "-j8"])]
    if args.wii:
        dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
        d = bd / "wii"
        steps += [("wii: configure", ["cmake", "-S", str(ctx.wl), "-B", str(d), "-G", "Ninja",
                                      f"-DCMAKE_TOOLCHAIN_FILE={dkp}/cmake/Wii.cmake"]),
                  ("wii: build", ["cmake", "--build", str(d)])]
    for label, cmd in steps:
        print(f"# {label}\n  $ {' '.join(cmd)}", flush=True)
        if not args.dry_run:
            r = subprocess.run(cmd, cwd=ctx.wl)
            if r.returncode:
                print(f"FAILED: {label}")
                return 1
    return 0


def cmd_advance(ctx: Ctx, args) -> int:
    st = ctx.need_state()
    repo = ctx.need_repo()
    b = ctx.resolve(args.to, None)
    vend_b = ctx.vendored_set(repo.tree(b))
    if dict(vend_b) != dict(st["files"]) and not args.force:
        diff = sorted(set(vend_b.items()) ^ set(st["files"].items()))
        raise Problem(f"the vendored copy is not at {b[:10]}'s content ({len(diff)} difference(s), e.g. "
                      f"{diff[0][0]}): run `merge --to {b[:10]} --apply` first")
    bad, extra, _ = integrity(ctx)
    if bad and not args.force:
        raise Problem("the vendored copy was edited: " + "; ".join(bad[:5]))
    files = {rel: (ctx.vendor / rel).read_bytes() for rel in st["files"]}
    failed = [r["patch"] for r in try_patches(files, ctx.patches()) if not r["ok"]]
    if failed and not args.force:
        raise Problem("patches do not apply: " + ", ".join(failed))
    info = repo.commit_info(b)
    st["history"].append(st["triaged"])
    st["triaged"] = OrderedDict(commit=b, date=info["date"], release=args.release or info["release"],
                                note=args.note or "")
    save_json(ctx.state_path, st)
    if repo.path == ctx.cache:
        repo.git("update-ref", f"refs/wl-sync/{b[:12]}", b)
    print(f"triaged up to {b[:12]} {st['triaged']['release']}; commit {ctx.state_path.relative_to(ctx.wl)} "
          "with the work it records")
    return 0


def cmd_patch_repo(ctx: Ctx, args) -> int:
    st = ctx.need_state()
    d = Path(args.dir).resolve()
    if d.exists() and any(d.iterdir()):
        raise Problem(f"{d} exists and is not empty")
    d.mkdir(parents=True, exist_ok=True)
    for rel in st["files"]:
        f = d / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ctx.vendor / rel, f)
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(d.parent))
    r = Repo(d)

    def g(*a):
        p = subprocess.run(["git", "-C", str(d), *a], env=env, capture_output=True, text=True)
        if p.returncode:
            raise Problem(f"git {' '.join(a)}: {p.stderr.strip()}")
        return p.stdout
    g("init", "-q", "-b", "main")
    g("add", "-A")
    g("-c", "user.name=xboxrecomp", "-c", "user.email=upstream@xboxrecomp", "commit", "-q", "-m",
      f"xboxrecomp {st['vendored']['commit'][:12]} {st['vendored'].get('release', '')} (vendored subset)")
    g("tag", "upstream")
    for p in ctx.patches():
        g("am", "-q", "--keep-cr", str(p))
    print(f"{d}: tag `upstream` is the pristine copy; {len(ctx.patches())} patch commit(s) on top.")
    print(f"edit with git there, then: tools/xboxrecomp_sync.py export-patches {d}")
    del r
    return 0


def _slug(subject: str) -> str:
    s = re.sub(r"^[a-z0-9_-]+:\s*", "", subject.lower())
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    return s[:48].rstrip("-") or "change"


def cmd_export_patches(ctx: Ctx, args) -> int:
    d = Path(args.dir).resolve()
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(d.parent))
    with tempfile.TemporaryDirectory() as td:
        p = subprocess.run(["git", "-C", str(d), "format-patch", "-q", "-N", "--no-signature",
                            "--zero-commit", "-o", td, "upstream..HEAD"], env=env, capture_output=True, text=True)
        if p.returncode:
            raise Problem(f"git format-patch: {p.stderr.strip()}")
        new = sorted(Path(td).glob("*.patch"))
        old = {patch_subject(x): x.name for x in ctx.patches()}
        old_dates = {}
        for x in ctx.patches():
            m = re.search(r"^Date: .*$", x.read_text(encoding="utf-8"), re.M)
            if m:
                old_dates[patch_subject(x)] = m.group(0)
        ctx.patches_dir.mkdir(parents=True, exist_ok=True)
        for x in ctx.patches():
            x.unlink()
        for i, x in enumerate(new, 1):
            subj = patch_subject(x)
            name = old.get(subj)
            stem = name.split("-", 1)[1] if name else _slug(subj) + ".patch"
            dest = ctx.patches_dir / f"{i:04d}-{stem}"
            text = x.read_text(encoding="utf-8")
            # an unchanged patch keeps its Date line, so a re-export does not churn the file
            if subj in old_dates:
                text = re.sub(r"^Date: .*$", lambda _m: old_dates[subj], text, count=1, flags=re.M)
            dest.write_text(text, encoding="utf-8")
            print(f"wrote {dest.relative_to(ctx.wl)}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[1:]))
    ap.add_argument("--wl-root", default=str(DEFAULT_WL_ROOT), help="WiiLibs working tree")
    ap.add_argument("--repo", help="upstream git repository (default: the cache clone "
                                   "build/xboxrecomp_sync/upstream.git)")
    ap.add_argument("--state", help="default third_party/xboxrecomp/UPSTREAM.json")
    ap.add_argument("--map", help="default third_party/xboxrecomp/upstream_map.json")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("fetch")
    p.add_argument("--url", help="default: the map's upstream url")
    p = sub.add_parser("brief")
    p.add_argument("--from", dest="frm")
    p.add_argument("--to")
    p.add_argument("-o", "--output")
    p.add_argument("--split-dir")
    for name in ("merge", "apply"):
        p = sub.add_parser(name)
        p.add_argument("--to")
        p.add_argument("--apply", action="store_true", help="write the files (else a dry run)")
        p.add_argument("--force", action="store_true", help="overwrite a vendored copy that was edited")
    sub.add_parser("check-map")
    p = sub.add_parser("test")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--wii", action="store_true", help="also configure and build for the Wii")
    p = sub.add_parser("advance")
    p.add_argument("--to", required=True)
    p.add_argument("--release")
    p.add_argument("--note")
    p.add_argument("--force", action="store_true")
    p = sub.add_parser("patch-repo")
    p.add_argument("dir")
    p = sub.add_parser("export-patches")
    p.add_argument("dir")
    args = ap.parse_args(argv)
    if args.cmd == "apply":
        args.apply = True
    try:
        ctx = Ctx(args)
        return {"status": cmd_status, "fetch": cmd_fetch, "brief": cmd_brief, "merge": cmd_merge,
                "apply": cmd_merge, "check-map": cmd_check_map, "test": cmd_test, "advance": cmd_advance,
                "patch-repo": cmd_patch_repo, "export-patches": cmd_export_patches}[args.cmd](ctx, args)
    except Problem as e:
        print(f"xboxrecomp_sync: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
