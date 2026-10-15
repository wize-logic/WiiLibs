#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Golden runs of a recompiled title on the host: record them, then rerun and compare.

A run is the host program (`prog <title folder> <hard disk folder>`) playing a script in the
deterministic mode (WL_XB_DETERMINISTIC=1, which wl/xboxrt/README.md "Running a title" describes).
What it keeps: the harness's `test_input:` lines (everything the script logged, minus the wall-clock
times and the output folder's name), the audio's
running checksums (WL_XB_AUDIO_HASH) and the screenshots (PNG) the script took. Nothing here knows
a title: its folders, scripts and environment come from a JSON config that stays outside the
repository (the goldens are the title's data).

    golden_check.py record  CONFIG GOLDEN_DIR [--only NAME ...]
    golden_check.py compare CONFIG GOLDEN_DIR [--only NAME ...] [--out DIR] [--threshold N]
    golden_check.py repeat  CONFIG GOLDEN_DIR [--only NAME ...]     (two runs against each other)

Config (JSON):
    {
      "exe":     "path of the host program",
      "title":   "title folder (default.xbe, maps)",
      "hdd":     "hard disk folder, reused between runs",
      "restore": ["Z/savegame.bin", ...],     files of the hdd a run changes: saved before the first
                                              run, put back before each run (identical starts)
      "env":     {"WL_XB_FRAME_MB": "24", ...},   added to the environment (the deterministic mode,
                                                  the audio hash and the play setting are set here)
      "runs":    [{"name": "cryo", "script": "path of the play script"}, ...],
      "timeout": 1800                          seconds a run may take
    }

record writes GOLDEN_DIR/NAME/{lines.txt, audio.txt, *.png}. compare reruns into --out (default
GOLDEN_DIR/../compare) and diffs: the lines and the audio exactly, each PNG byte for byte (a PNG that
differs is measured with png_compare.py and called a difference all the same; --threshold N only
adds that tool's verdict to the report). Exit status 0 when every run matches, 1 otherwise.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
# the log's prefix before the harness's text: a timestamp and the tag, both varying
LINE_RE = re.compile(r"test_input: (.*)$")
# wall-clock seconds the harness prints after a script line ("line 9 (1.1 s): ...") vary with the host
WALL_RE = re.compile(r" \(\d+\.\d+ s\)")


def load_config(path: str) -> dict:
    cfg = json.loads(Path(path).read_text())
    for key in ("exe", "title", "hdd", "runs"):
        if key not in cfg:
            sys.exit(f"{path}: missing '{key}'")
    cfg.setdefault("restore", [])
    cfg.setdefault("env", {})
    cfg.setdefault("timeout", 1800)
    return cfg


class Restorer:
    """Saves the hdd files a run changes and puts them back."""

    def __init__(self, cfg: dict, keep: Path):
        self.hdd = Path(cfg["hdd"])
        self.files = cfg["restore"]
        self.keep = keep
        self.keep.mkdir(parents=True, exist_ok=True)
        for rel in self.files:
            src, dst = self.hdd / rel, self.keep / rel.replace("/", "__")
            if not dst.exists():
                if src.exists():
                    shutil.copy2(src, dst)
                else:
                    dst.write_bytes(b"")
                    dst.with_suffix(dst.suffix + ".absent").write_bytes(b"")

    def put_back(self) -> None:
        for rel in self.files:
            saved = self.keep / rel.replace("/", "__")
            dst = self.hdd / rel
            if saved.with_suffix(saved.suffix + ".absent").exists():
                dst.unlink(missing_ok=True)
            else:
                shutil.copy2(saved, dst)


def run_one(cfg: dict, run: dict, out: Path, restorer: Restorer) -> dict:
    """One run into OUT/NAME: lines.txt, audio.txt and the screenshots. Returns {name, status}."""
    d = out / run["name"]
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    restorer.put_back()
    env = dict(os.environ)
    env.update(cfg["env"])
    env["WL_XB_DETERMINISTIC"] = "1"
    env["WL_XB_PLAY"] = "script:" + run["script"]
    env["WL_XB_PLAY_OUT"] = str(d)
    env["WL_XB_AUDIO_HASH"] = str(d / "audio.txt")
    log = d / "run.log"
    with open(log, "wb") as lf:
        try:
            p = subprocess.run([cfg["exe"], cfg["title"], cfg["hdd"]], env=env, stdout=lf,
                               stderr=subprocess.STDOUT, timeout=cfg["timeout"])
            status = f"exit {p.returncode}"
        except subprocess.TimeoutExpired:
            status = "timeout"
    lines = []
    for raw in log.read_text(errors="replace").splitlines():
        m = LINE_RE.search(raw)
        if m:
            # the run's own folder and the wall-clock times are not part of what the game did
            lines.append(WALL_RE.sub("", m.group(1).replace(str(d.resolve()), "<OUT>").replace(str(d), "<OUT>")))
    (d / "lines.txt").write_text("\n".join(lines) + ("\n" if lines else ""))
    log.unlink()  # the log has host paths and times: only the lines are kept
    restorer.put_back()
    return {"name": run["name"], "status": status, "lines": len(lines)}


def diff_text(a: Path, b: Path, label: str, limit: int = 12) -> list[str]:
    ta = a.read_text().splitlines() if a.exists() else None
    tb = b.read_text().splitlines() if b.exists() else None
    if ta is None or tb is None:
        return [f"{label}: missing in {'golden' if ta is None else 'new run'}"]
    if ta == tb:
        return []
    diff = list(difflib.unified_diff(ta, tb, "golden", "new", lineterm="", n=0))
    first = next((i for i, (x, y) in enumerate(zip(ta, tb)) if x != y), min(len(ta), len(tb)))
    out = [f"{label}: differs (golden {len(ta)} lines, new {len(tb)}; first at line {first + 1})"]
    out += ["    " + x for x in diff[2:2 + limit]]
    if len(diff) - 2 > limit:
        out.append(f"    ... {len(diff) - 2 - limit} more diff lines")
    return out


def compare_dirs(gold: Path, new: Path, threshold: int, work: Path) -> list[str]:
    """Differences between two run folders (empty when identical)."""
    problems = diff_text(gold / "lines.txt", new / "lines.txt", "lines")
    problems += diff_text(gold / "audio.txt", new / "audio.txt", "audio hash")
    gp = {p.name for p in gold.glob("*.png")}
    np_ = {p.name for p in new.glob("*.png")}
    for n in sorted(gp - np_):
        problems.append(f"{n}: missing in new run")
    for n in sorted(np_ - gp):
        problems.append(f"{n}: not in golden")
    for n in sorted(gp & np_):
        if (gold / n).read_bytes() == (new / n).read_bytes():
            continue
        msg = f"{n}: bytes differ"
        pa, pb = work / "a", work / "b"
        for q in (pa, pb):
            shutil.rmtree(q, ignore_errors=True)
            q.mkdir(parents=True)
        shutil.copy2(gold / n, pa / n)
        shutil.copy2(new / n, pb / n)
        r = subprocess.run([sys.executable, "-I", str(HERE / "png_compare.py"), str(pa), str(pb),
                            "--threshold", str(threshold)], capture_output=True, text=True)
        detail = " ".join(r.stdout.split())
        msg += f" (png_compare, threshold {threshold}: {detail or 'no output'})"
        problems.append(msg)
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("record", "compare", "repeat"))
    ap.add_argument("config")
    ap.add_argument("golden")
    ap.add_argument("--only", nargs="*", help="run names to do (default: all)")
    ap.add_argument("--out", help="compare/repeat: where the new runs go")
    ap.add_argument("--threshold", type=int, default=2)
    args = ap.parse_args()

    cfg = load_config(args.config)
    gold = Path(args.golden)
    runs = [r for r in cfg["runs"] if not args.only or r["name"] in args.only]
    if not runs:
        sys.exit("no run selected")
    gold.parent.mkdir(parents=True, exist_ok=True)
    restorer = Restorer(cfg, gold.parent / ".restore")

    if args.mode == "record":
        gold.mkdir(parents=True, exist_ok=True)
        for run in runs:
            r = run_one(cfg, run, gold, restorer)
            print(f"{r['name']}: {r['status']}, {r['lines']} lines", flush=True)
        return 0

    out = Path(args.out) if args.out else gold.parent / "compare"
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    work = Path(tempfile.mkdtemp(prefix="golden_check_", dir=out))
    try:
        for run in runs:
            if args.mode == "repeat":
                run_one(cfg, run, out / "first", restorer)
                r = run_one(cfg, run, out / "second", restorer)
                base, new = out / "first" / run["name"], out / "second" / run["name"]
            else:
                r = run_one(cfg, run, out, restorer)
                base, new = gold / run["name"], out / run["name"]
            problems = compare_dirs(base, new, args.threshold, work)
            tag = "ok" if not problems else "DIFFERS"
            print(f"{r['name']}: {tag} ({r['status']}, {r['lines']} lines)", flush=True)
            for p in problems:
                print("  " + p)
            bad += bool(problems)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
