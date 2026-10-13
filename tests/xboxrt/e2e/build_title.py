#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the e2e title and lifts it: title.c compiled for i386 (gcc -m32, freestanding) and linked
with title.ld, wrapped in an XBE (mkxbe_title.py), run through xboxrecomp's pipeline (xbe_parser,
disasm, recomp) into gen/, and the generated code's header replaced with WiiLibs' (the big-endian
correct one), since the pipeline writes its own little-endian copy beside the code.

Usage: build_title.py --cc CC --src DIR --out DIR --xboxrecomp DIR --header DIR
"""
import argparse
import os
import shutil
import subprocess
import sys


def run(cmd, cwd=None, log=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if log is not None:
        with open(log, "w") as f:
            f.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stdout)
        raise SystemExit("failed: %s" % " ".join(cmd))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--ld", default="ld")
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--xboxrecomp", required=True)
    ap.add_argument("--header", required=True)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    obj, elf, xbe = os.path.join(out, "title.o"), os.path.join(out, "title.elf"), os.path.join(out, "title.xbe")
    run([a.cc, "-m32", "-O1", "-fno-pic", "-fno-pie", "-ffreestanding", "-fno-asynchronous-unwind-tables",
         "-fno-stack-protector", "-mno-sse", "-fcf-protection=none", "-c", os.path.join(a.src, "title.c"), "-o", obj])
    run([a.ld, "-m", "elf_i386", "-T", os.path.join(a.src, "title.ld"), "-nostdlib", "-o", elf, obj])
    run([sys.executable, os.path.join(a.src, "mkxbe_title.py"), elf, xbe, os.path.join(out, "title_syms.h")])
    for d in ("parse", "disasm", "recomp", "gen"):
        shutil.rmtree(os.path.join(out, d), ignore_errors=True)
        os.makedirs(os.path.join(out, d))
    py = [sys.executable, "-m"]
    run(py + ["tools.xbe_parser", xbe, "--json", os.path.join(out, "parse", "analysis.json")], cwd=a.xboxrecomp,
        log=os.path.join(out, "parse.log"))
    run(py + ["tools.disasm", xbe, "--analysis-json", os.path.join(out, "parse", "analysis.json"), "--text-only", "-o",
              os.path.join(out, "disasm")], cwd=a.xboxrecomp, log=os.path.join(out, "disasm.log"))
    run(py + ["tools.recomp", xbe, "--all", "--split", "250", "--game-name", "E2E", "-o", os.path.join(out, "recomp"),
              "--gen-dir", os.path.join(out, "gen"), "--disasm-dir", os.path.join(out, "disasm")], cwd=a.xboxrecomp,
        log=os.path.join(out, "recomp.log"))
    shutil.copyfile(os.path.join(a.header, "recomp_types.h"), os.path.join(out, "gen", "recomp_types.h"))
    for f in ("recomp_0000.c", "recomp_dispatch.c"):
        if not os.path.exists(os.path.join(out, "gen", f)):
            raise SystemExit("the pipeline did not write gen/%s" % f)


if __name__ == "__main__":
    main()
