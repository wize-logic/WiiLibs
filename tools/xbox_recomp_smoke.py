#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Smoke check for a real title: recompile an XBE and compile every chunk for the Wii.

Runs tools/xbox_recomp.py on the XBE into a build directory, then compiles each translation unit
with devkitPPC and the flags wl_add_xbox_title uses on the Wii (-mcpu=750 -meabi -mhard-float
-msdata=eabi -DWL_XB_EA_BASE_ZERO -ffp-contract=off -fno-strict-aliasing -w, -Os by default),
against WiiLibs' runtime header. Reports what the pipeline found (functions, unresolved call
targets, unimplemented instructions) and the size of the code (.text summed over the objects).
Nothing is linked or run: this says the title's code lifts and compiles, and how big it is.

The game and everything generated from it stay in the build directory (never in a repository).

    xbox_recomp_smoke.py /path/to/default.xbe
    xbox_recomp_smoke.py default.xbe --build build/halo_smoke --opt -O2 --jobs 12
    xbox_recomp_smoke.py default.xbe --config title.json --recomp-arg=--coalesce-functions=bounds.json

Writes <build>/smoke.json. Exit status: 0 everything compiled, 1 a stage or a compile failed.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WL_ROOT = HERE.parent
WII_FLAGS = ["-mcpu=750", "-meabi", "-mhard-float", "-msdata=eabi", "-DWL_XB_EA_BASE_ZERO",
             "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-std=gnu11"]


def devkitppc_tool(name: str) -> str:
    base = os.environ.get("DEVKITPPC") or os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitPPC")
    return os.path.join(base, "bin", f"powerpc-eabi-{name}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.splitlines()[1:]))
    ap.add_argument("xbe")
    ap.add_argument("--build", default=str(WL_ROOT / "build" / "xbox_recomp_smoke"))
    ap.add_argument("--config", help="the title's JSON config (tools/xbox_recomp.py)")
    ap.add_argument("--chunks", type=int, default=32)
    ap.add_argument("--opt", default="-Os")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--recomp-arg", action="append", default=[], help="one more argument for tools.recomp")
    ap.add_argument("--force", action="store_true", help="rerun every pipeline stage")
    args = ap.parse_args(argv)

    out = Path(args.build).resolve()
    cmd = [sys.executable, str(HERE / "xbox_recomp.py"), "run", args.xbe, "-o", str(out), "--chunks",
           str(args.chunks)] + (["--config", args.config] if args.config else []) + \
          [f"--recomp-arg={a}" for a in args.recomp_arg] + (["--force"] if args.force else [])
    t0 = time.time()
    r = subprocess.run(cmd)
    if r.returncode:
        return 1
    t_pipe = time.time() - t0
    report = json.loads((out / "xbox_recomp.json").read_text())

    gcc, size = devkitppc_tool("gcc"), devkitppc_tool("size")
    objdir = out / f"obj{args.opt}"
    objdir.mkdir(exist_ok=True)
    units = sorted((out / "tu").glob("recomp_tu_*.c"))
    inc = ["-I", str(WL_ROOT / "wl" / "xboxrt" / "recomp"), "-I", str(out / "gen")]

    def compile_one(src: Path):
        obj = objdir / (src.stem + ".o")
        t = time.time()
        p = subprocess.run([gcc, args.opt] + WII_FLAGS + inc + ["-c", str(src), "-o", str(obj)],
                           capture_output=True, text=True)
        return src.name, obj, p.returncode, p.stderr[-2000:], time.time() - t

    t1 = time.time()
    failed, objs, longest = [], [], 0.0
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for name, obj, rc, err, secs in ex.map(compile_one, units):
            longest = max(longest, secs)
            if rc:
                failed.append(name)
                print(f"COMPILE FAILED {name}:\n{err}", file=sys.stderr)
            else:
                objs.append(obj)
    t_cc = time.time() - t1
    totals = {"text": 0, "data": 0, "bss": 0}
    per = {}
    if objs:
        lines = subprocess.run([size] + [str(o) for o in objs], capture_output=True, text=True,
                               check=True).stdout.splitlines()[1:]
        for line in lines:
            p = line.split()
            text, data, bss = int(p[0]), int(p[1]), int(p[2])
            totals["text"] += text
            totals["data"] += data
            totals["bss"] += bss
            per[Path(p[-1]).name] = text
    report.update({"opt": args.opt, "objects": len(objs), "compile_failures": failed,
                   "text_bytes": totals["text"], "data_bytes": totals["data"], "bss_bytes": totals["bss"],
                   "largest_object_text": max(per.values()) if per else 0,
                   "pipeline_seconds": round(t_pipe, 1), "compile_seconds": round(t_cc, 1),
                   "longest_compile_seconds": round(longest, 1), "per_object_text": per})
    (out / f"smoke{args.opt}.json").write_text(json.dumps(report, indent=1) + "\n")
    print(f"functions detected {report['functions_detected']}, translated {report['functions_translated']} "
          f"({report['functions_failed']} failed)")
    print(f"unresolved call targets {report['unresolved_call_targets']}; unimplemented instructions "
          f"{report['unimplemented_instructions']} ({len(report['unimplemented_mnemonics'])} mnemonics)")
    print(f"{report['chunks']} chunks, {report['lines_of_c']} lines of C; {len(objs)}/{len(units)} units compiled "
          f"at {args.opt} in {t_cc:.0f}s (longest {longest:.0f}s)")
    print(f".text {totals['text']} bytes ({totals['text'] / 1048576:.2f} MiB), .data {totals['data']}, "
          f".bss {totals['bss']}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
