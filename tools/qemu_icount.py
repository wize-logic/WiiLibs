#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Counts the guest instructions a program runs under qemu-user: speed counted without a Wii.

Build the code for 32-bit big-endian PowerPC Linux as the Wii build compiles it (the same compiler
and flags, -mcpu=750, static: the ppc-be toolchain, cmake/powerpc-linux-gnu.cmake), then

    qemu_icount.py ./bench args...                         # instructions run in all
    qemu_icount.py --functions 20 ./bench args...          # and the 20 functions that ran the most
    qemu_icount.py --blocks 20 ./bench args...             # and the 20 hottest translation blocks
    qemu_icount.py --per 1000 --base "./bench 10" ./bench 1010   # (b - a) / 1000: a cost per item

An instruction count is not a cycle count (the 750's latencies, the caches and the bus are not
modelled), but it is exact and repeatable, so it shows what a change saves where Dolphin's timings
do not track the console. To take a fixed cost (start-up, set-up) out, run the same program at two
sizes and divide the difference (--base, --per).

How: qemu runs with `-d in_asm,exec,nochain` into a FIFO. `in_asm` gives each translation block's
address and size when it is translated (the bytes of its instructions as `OBJD-T` hex when qemu has
no disassembler, else one line an instruction); `exec` (with chaining off, so that every block
entered is logged) gives a `Trace` line each time a block runs. Instructions = the sum over blocks
of size times runs. A block translated again at the same address takes its newest size. Functions
come from `nm -n` of the program (static, so the program's own and the C library's are all there).
"""

from __future__ import annotations

import argparse
import bisect
import os
import re
import shlex
import subprocess
import sys
import tempfile
import threading
from collections import Counter
from dataclasses import dataclass, field
from typing import Iterable

TRACE = re.compile(rb"^Trace \d+: 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/")
ADDRESS = re.compile(rb"^0x([0-9a-f]+):(.*)$")


@dataclass
class Counts:
    """translation blocks: their sizes in instructions and how often each ran"""
    sizes: dict = field(default_factory=dict)
    runs: Counter = field(default_factory=Counter)

    def total(self) -> int:
        return sum(self.sizes.get(pc, 0) * runs for pc, runs in self.runs.items())

    def blocks(self) -> list:
        """(instructions, pc, size, runs), the most first"""
        rows = [(self.sizes.get(pc, 0) * runs, pc, self.sizes.get(pc, 0), runs) for pc, runs in self.runs.items()]
        return sorted(rows, reverse=True)


def parse(lines: Iterable[bytes]) -> Counts:
    """a qemu -d in_asm,exec,nochain log, a line at a time"""
    counts = Counts()
    block = None
    for line in lines:
        if line.startswith(b"Trace"):
            match = TRACE.match(line)
            if match:
                counts.runs[int(match.group(1), 16)] += 1
            block = None
        elif line.startswith(b"IN:"):
            block = None
        elif line.startswith(b"OBJD-T:"):
            # (no disassembler: the block's bytes, 4 an instruction)
            if block is not None:
                counts.sizes[block] += len(line[7:].strip()) // 8
        else:
            match = ADDRESS.match(line)
            if not match:
                continue
            if block is None:
                block = int(match.group(1), 16)
                counts.sizes[block] = 0
            # (a disassembled instruction on the address's line)
            if match.group(2).strip():
                counts.sizes[block] += 1
    return counts


def run(command: list, qemu: str) -> tuple:
    """(Counts, the program's standard output) of command under qemu"""
    directory = tempfile.mkdtemp(prefix="qemu_icount-")
    fifo = os.path.join(directory, "log")
    os.mkfifo(fifo)
    try:
        process = subprocess.Popen([qemu, "-d", "in_asm,exec,nochain", "-D", fifo, *command],
                                   stdout=subprocess.PIPE)
        output = []
        reader = threading.Thread(target=lambda: output.append(process.stdout.read()))
        reader.start()
        with open(fifo, "rb") as log:
            counts = parse(log)
        process.wait()
        reader.join()
        if process.returncode:
            raise SystemExit(f"qemu_icount: {command[0]} exited with {process.returncode}")
        return counts, output[0]
    finally:
        os.unlink(fifo)
        os.rmdir(directory)


class Symbols:
    """the program's functions by address (nm -n)"""

    def __init__(self, entries: list):
        self.entries = sorted(entries)
        self.starts = [address for address, _ in self.entries]

    @classmethod
    def from_program(cls, program: str, nm: str) -> "Symbols":
        entries = []
        listing = subprocess.run([nm, "-n", program], capture_output=True, text=True, check=True).stdout
        for line in listing.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "tTwW":
                entries.append((int(parts[0], 16), parts[2]))
        return cls(entries)

    def name(self, address: int) -> str:
        index = bisect.bisect_right(self.starts, address) - 1
        return self.entries[index][1] if index >= 0 else "?"

    def offset(self, address: int) -> str:
        index = bisect.bisect_right(self.starts, address) - 1
        return f"{self.entries[index][1]}+{address - self.starts[index]:#x}" if index >= 0 else f"{address:#x}"


def per_function(counts: Counts, symbols: Symbols) -> Counter:
    functions = Counter()
    for pc, runs in counts.runs.items():
        functions[symbols.name(pc)] += counts.sizes.get(pc, 0) * runs
    return functions


def difference(after: Counter, before: Counter) -> Counter:
    result = Counter()
    for key in set(after) | set(before):
        result[key] = after.get(key, 0) - before.get(key, 0)
    return result


def main(argv: list = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("--qemu", default="qemu-ppc", help="qemu-user program (default qemu-ppc)")
    parser.add_argument("--nm", default=None, help="nm for the program's symbols (default powerpc-linux-gnu-nm, else nm)")
    parser.add_argument("--functions", type=int, default=0, metavar="N", help="the N functions that ran the most")
    parser.add_argument("--blocks", type=int, default=0, metavar="N", help="the N translation blocks that ran the most")
    parser.add_argument("--base", default=None, metavar="COMMAND",
                        help="a smaller run of the same program, taken away (a quoted command line)")
    parser.add_argument("--per", type=float, default=1.0, metavar="ITEMS",
                        help="divide the counts by this (the items the difference from --base is)")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="program and its arguments")
    args = parser.parse_args(argv)
    if not args.command:
        parser.error("no program")
    nm = args.nm or next((tool for tool in ("powerpc-linux-gnu-nm", "nm")
                          if subprocess.run(["which", tool], capture_output=True).returncode == 0), "nm")

    counts, output = run(args.command, args.qemu)
    sys.stdout.buffer.write(output)
    base = run(shlex.split(args.base), args.qemu)[0] if args.base else None
    total = counts.total() - (base.total() if base else 0)
    print(f"instructions {total / args.per:.1f}" if args.per != 1.0 else f"instructions {total}")
    if args.functions:
        symbols = Symbols.from_program(args.command[0], nm)
        functions = per_function(counts, symbols)
        if base:
            functions = difference(functions, per_function(base, symbols))
        for name, value in functions.most_common(args.functions):
            print(f"  {value / args.per:14.1f} {100.0 * value / max(total, 1):5.1f}%  {name}")
    if args.blocks:
        symbols = Symbols.from_program(args.command[0], nm)
        rows = counts.blocks()
        if base:
            before = {pc: value for value, pc, _, _ in base.blocks()}
            rows = sorted(((value - before.get(pc, 0), pc, size, runs) for value, pc, size, runs in rows), reverse=True)
        for value, pc, size, runs in rows[:args.blocks]:
            print(f"  {value / args.per:14.1f}  {pc:08x} size {size:3d}  {symbols.offset(pc)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
