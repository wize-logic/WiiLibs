#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lists the fused multiply-adds in PowerPC objects, archives and ELF files.

A fused multiply-add rounds a product and a sum once, where a multiply then an add rounds twice,
so the same C expression gives different bits on a CPU that fuses it and on one that does not.
Code that must compute the same results as another machine bit for bit (lockstep networking,
replays, a port checked against its original) must contain none. Compilers fuse unasked: GCC
contracts by default, and Clang has let some through despite -ffp-contract=off (a function it does
not optimise). This reads the disassembly and lists each function that has one.

    fma_check.py build/obj/*.o                       # exit 1 if any object has one
    fma_check.py game.elf --exclude-function 'wl_*'  # skip functions by name (a library's)
    fma_check.py libgame.a --exclude '*/libm.a'      # skip objects or archives by path
    fma_check.py @objects.rsp --stamp check.stamp    # ninja's response file; the stamp is
                                                     # touched when the check passes
    fma_check.py -v main.o                           # also each instruction with its address

What is looked for: fmadd, fmsub, fnmadd, fnmsub (and the single-precision fmadds, fmsubs,
fnmadds, fnmsubs), the Gekko's paired singles (ps_madd, ps_msub, ps_nmadd, ps_nmsub, ps_madds0,
ps_madds1, ...), AltiVec's vmaddfp and vnmsubfp, and VSX's xsmadd*, xvmadd*, ... (every
form, with a record bit or not). Not found: ps_mul, ps_sum (no multiply and add in one), and
instructions that only look alike.

Objdump decodes the paired singles only as the Gekko, so it is run with `-M gekko` (without it ps_nmadd,
ps_nmsub and the ps_madds are shown as unrelated vector instructions, and missed); that mode does
not decode AltiVec or VSX, which a Wii has none of. For code with those give
--disassembler-options altivec,vsx (or the empty string for objdump's own default).

The disassembly is objdump's (GNU binutils): --objdump, else $OBJDUMP, else the first of
powerpc-eabi-objdump (devkitPPC: $DEVKITPRO/devkitPPC/bin too) and powerpc-linux-gnu-objdump on
PATH. A file the objdump cannot read as PowerPC is an error (exit 2), not a pass: a check that
reads nothing must not look clean.

Filters are shell patterns (fnmatch). --exclude and --include match an object by the path it was
given, its base name, and for an archive member both `lib.a(member.o)`, `member.o` and the archive's
path; --exclude-function and --include-function match a function's symbol (a stretch of code
before any symbol is named for its section). With --include only the matching objects (and
functions) are checked; --exclude removes from what remains.

Exit status: 0 none found, 1 some found, 2 the check could not be made.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional, Sequence

# objects handed to one objdump run (a command line has a limit)
BATCH = 200

# objdump's default does not decode the paired singles (see the module's text)
DEFAULT_DISASSEMBLER_OPTIONS = "gekko"

# fmadd fmsub fnmadd fnmsub (+ s, + .), ps_madd ps_msub ps_nmadd ps_nmsub ps_madds0 ps_madds1 (+ .),
# AltiVec vmaddfp vnmsubfp, VSX xsmaddadp xsmaddmdp xvnmsubasp ... (xs/xv, n, madd/msub, a/m, dp/sp)
FUSED_MNEMONIC = re.compile(
    r"(?:fn?m(?:add|sub)s?"
    r"|ps_n?m(?:add|sub)(?:s[01])?"
    r"|vmaddfp|vnmsubfp"
    r"|x[sv]n?m(?:add|sub)[am](?:dp|sp)"
    r")\.?")

FUNCTION = re.compile(r"^[0-9a-fA-F]+ <(.+)>:$")
SECTION = re.compile(r"^Disassembly of section (.+):$")
FILE_FORMAT = re.compile(r"^(?P<unit>.*?):\s+file format (?P<format>\S+)\s*$")
ARCHIVE = re.compile(r"^In archive (?P<archive>.*):$")
INSTRUCTION = re.compile(r"^\s*(?P<address>[0-9a-fA-F]+):\s+(?P<text>\S.*?)\s*$")


@dataclass
class Finding:
    """The fused multiply-adds of one function of one object"""

    unit: str
    function: str
    instructions: list = field(default_factory=list)  # (address, text) of each

    @property
    def mnemonics(self) -> list:
        return sorted({text.split()[0].rstrip(".") for _, text in self.instructions})


class CheckError(Exception):
    """The check could not be made (an unreadable file, a wrong objdump)"""


def split_response(argument: str) -> list:
    """The paths in @FILE: a response file as ninja writes (quoted as a shell's)"""
    return shlex.split(Path(argument[1:]).read_text(encoding="utf-8"))


def find_objdump(explicit: Optional[str] = None, environ: Optional[dict] = None) -> str:
    """--objdump, else $OBJDUMP, else a PowerPC objdump (devkitPPC's, then Linux's) on the path"""
    environ = os.environ if environ is None else environ
    if explicit:
        return explicit
    if environ.get("OBJDUMP"):
        return environ["OBJDUMP"]
    names = ["powerpc-eabi-objdump", "powerpc-linux-gnu-objdump", "powerpc-unknown-linux-gnu-objdump"]
    devkitpro = environ.get("DEVKITPRO")
    if devkitpro:
        candidate = Path(devkitpro) / "devkitPPC" / "bin" / "powerpc-eabi-objdump"
        if candidate.is_file():
            return str(candidate)
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise CheckError("no PowerPC objdump found (powerpc-eabi-objdump or powerpc-linux-gnu-objdump): "
                     "give --objdump or set OBJDUMP")


def is_fused(text: str) -> bool:
    """Whether an instruction (its mnemonic and operands) is a fused multiply-add"""
    words = text.split()
    return bool(words) and FUSED_MNEMONIC.fullmatch(words[0]) is not None


def parse_listing(listing: str) -> list:
    """The findings in objdump -d's output: one Finding per function of each object.

    Raises CheckError for an object that is not PowerPC."""
    findings = {}
    order = []
    archive = None
    unit = function = None
    for line in listing.splitlines():
        match = ARCHIVE.match(line)
        if match:
            archive = match.group("archive")
            continue
        match = FILE_FORMAT.match(line)
        if match:
            if "powerpc" not in match.group("format"):
                raise CheckError(f"{match.group('unit')}: file format {match.group('format')} is not PowerPC "
                                 "(wrong objdump?)")
            name = match.group("unit")
            unit = f"{archive}({name})" if archive else name
            function = None
            continue
        match = SECTION.match(line)
        if match:
            function = f"<{match.group(1)}>"
            continue
        match = FUNCTION.match(line)
        if match:
            function = match.group(1)
            continue
        match = INSTRUCTION.match(line)
        if match and unit is not None and is_fused(match.group("text")):
            key = (unit, function or "<no symbol>")
            if key not in findings:
                findings[key] = Finding(*key)
                order.append(key)
            findings[key].instructions.append((match.group("address"), match.group("text")))
    return [findings[key] for key in order]


def unit_names(unit: str) -> list:
    """What a path pattern may match for an object: `lib.a(member.o)` is also `member.o`, `lib.a`"""
    names = [unit]
    inside = re.fullmatch(r"(.*)\((.*)\)", unit)
    if inside:
        names += [inside.group(2), inside.group(1), os.path.basename(inside.group(1))]
    else:
        names.append(os.path.basename(unit))
    return names


def matches(patterns: Iterable[str], names: Iterable[str]) -> bool:
    names = list(names)
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns for name in names)


def wanted(unit: str, function: Optional[str], include: Sequence[str], exclude: Sequence[str],
           include_function: Sequence[str], exclude_function: Sequence[str]) -> bool:
    """Whether the filters keep an object's (and a function's) findings"""
    names = unit_names(unit)
    if include and not matches(include, names):
        return False
    if exclude and matches(exclude, names):
        return False
    if function is not None:
        if include_function and not matches(include_function, [function]):
            return False
        if exclude_function and matches(exclude_function, [function]):
            return False
    return True


def run_objdump(objdump: str, files: Sequence[str], options: str = DEFAULT_DISASSEMBLER_OPTIONS) -> str:
    command = [objdump, "-d", "--no-show-raw-insn", *(["-M", options] if options else []), *files]
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    except OSError as error:
        raise CheckError(f"cannot run {objdump}: {error}") from error
    if result.returncode != 0:
        detail = (result.stderr.strip() or result.stdout.strip()).splitlines()
        raise CheckError(f"{objdump} failed ({result.returncode}) on {len(files)} file(s): "
                         f"{detail[0] if detail else 'no message'}")
    return result.stdout


def scan(objdump: str, files: Sequence[str], include: Sequence[str] = (), exclude: Sequence[str] = (),
         include_function: Sequence[str] = (), exclude_function: Sequence[str] = (),
         options: str = DEFAULT_DISASSEMBLER_OPTIONS) -> list:
    """The findings in these files, after the filters (objdump reads them in batches).

    A file whose own path the filters leave out is not read, unless it is an archive (only its
    members' names tell)."""
    kept = []
    for path in files:
        if not Path(path).exists():
            raise CheckError(f"{path}: no such file")
        if not Path(path).is_file():
            raise CheckError(f"{path}: not a file")
        if Path(path).suffix != ".a" and not wanted(path, None, include, exclude, (), ()):
            continue
        kept.append(path)
    found = []
    for start in range(0, len(kept), BATCH):
        for finding in parse_listing(run_objdump(objdump, kept[start:start + BATCH], options)):
            if wanted(finding.unit, finding.function, include, exclude, include_function, exclude_function):
                found.append(finding)
    return found


def report(findings: Sequence[Finding], instructions: bool = False, out=None) -> None:
    out = out or sys.stdout
    for finding in findings:
        count = len(finding.instructions)
        print(f"{finding.unit}: {finding.function}: {count} fused multiply-add{'s' if count != 1 else ''} "
              f"({', '.join(finding.mnemonics)})", file=out)
        if instructions:
            for address, text in finding.instructions:
                print(f"    {address}: {text}", file=out)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=None)
    parser.add_argument("files", nargs="+", help="objects, archives or ELF files, or @FILE: a response file of them "
                        "(quoted as a shell's, as ninja writes)")
    parser.add_argument("--objdump", help="GNU objdump for PowerPC (default: $OBJDUMP, else devkitPPC's or Linux's)")
    parser.add_argument("--include", action="append", default=[], metavar="PATTERN",
                        help="check only objects matching this pattern (repeatable)")
    parser.add_argument("--exclude", action="append", default=[], metavar="PATTERN",
                        help="skip objects matching this pattern (repeatable)")
    parser.add_argument("--include-function", action="append", default=[], metavar="PATTERN",
                        help="check only functions matching this pattern (repeatable)")
    parser.add_argument("--exclude-function", action="append", default=[], metavar="PATTERN",
                        help="skip functions matching this pattern (repeatable)")
    parser.add_argument("--disassembler-options", default=DEFAULT_DISASSEMBLER_OPTIONS, metavar="OPTIONS",
                        help="objdump's -M OPTIONS (default %(default)s; '' for none)")
    parser.add_argument("--stamp", type=Path, help="file to touch when none is found (for a build system)")
    parser.add_argument("-v", "--instructions", action="store_true", help="list each instruction with its address")
    parser.add_argument("-q", "--quiet", action="store_true", help="print nothing but the findings")
    args = parser.parse_args(argv)
    files = []
    try:
        for argument in args.files:
            files += split_response(argument) if argument.startswith("@") else [argument]
        if not files:
            raise CheckError("no files to check")
        objdump = find_objdump(args.objdump)
        findings = scan(objdump, files, args.include, args.exclude, args.include_function, args.exclude_function,
                        args.disassembler_options)
    except (CheckError, OSError) as error:
        print(f"fma_check.py: {error}", file=sys.stderr)
        return 2
    report(findings, args.instructions)
    if findings:
        total = sum(len(f.instructions) for f in findings)
        if not args.quiet:
            print(f"fma_check.py: {total} fused multiply-add(s) in {len(findings)} function(s)", file=sys.stderr)
        return 1
    if args.stamp:
        args.stamp.touch()
    if not args.quiet:
        print(f"fma_check.py: no fused multiply-adds in {len(files)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
