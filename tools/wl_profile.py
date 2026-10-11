#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Symbolizes a WiiLibs sampling-profiler dump (wl/platform/profile.h) against the program's ELF.

The dump is the text wl_profile_dump_file / wl_profile_dump_log write: a `WLPROF 1 ...` header and a
`WLPS tick thread pc lr callers...` line for each sample. The lines may sit anywhere in a longer
text (a Dolphin log, say); a text with several profiles gives several reports.

    wl_profile.py program.elf dolphin.log                 # the default report
    wl_profile.py program.elf profile.txt --top 40 --callers physics --tree 2
    wl_profile.py program.elf profile.txt --thread 1 --collapsed folded.txt   # for flamegraph.pl

The default report: the sampling summary, each thread's share, the top functions by their own
samples ("self") and by the samples they were anywhere on the stack for ("inclusive", counted once
per sample even when recursive). Options add the callers and callees of a function (--callers),
the call tree from the outermost recorded frame down (--tree), the hottest instruction addresses
with their source lines (--hot, needs addr2line), a time line (--timeline) and folded stacks
(--collapsed).

How a sample becomes a stack: the interrupted pc is the leaf. The link register is the caller only
when it is outside the function pc is in (a function that has called something has its own return
address in the stack and a link register pointing back into itself). The return addresses of the
stack walk follow. Callers are looked up at the address before the return address (the call, not
the instruction after it, which can belong to the next function after a call that never returns).
The walk is `depth` frames deep: deeper callers are not known, so a tree's roots are the outermost
frames recorded, not necessarily the thread's first function.
"""

from __future__ import annotations

import argparse
import bisect
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Iterable, Iterator

HEADER_RE = re.compile(r"\bWLPROF (\d+)((?: [A-Za-z_]+=[^ ]*)*?)(?: note=(.*))?$")
SAMPLE_RE = re.compile(r"\bWLPS((?: [0-9a-fA-F]{1,8})+)\s*$")
DEFAULT_TIMEBASE = 60_750_000
UNKNOWN = "(unknown)"
WORD = 1 << 32


@dataclass
class Sample:
    tick: int  # unwrapped time base ticks since the first sample
    thread: int
    pc: int
    lr: int
    callers: list[int]


@dataclass
class Profile:
    header: dict[str, str] = field(default_factory=dict)
    note: str = ""
    samples: list[Sample] = field(default_factory=list)

    @property
    def timebase(self) -> int:
        return int(self.header.get("tbhz", 0)) or DEFAULT_TIMEBASE

    @property
    def hz(self) -> int:
        return int(self.header.get("hz", 0))


def parse_dump(lines: Iterable[str]) -> list[Profile]:
    """The profiles in `lines`: each header starts one (samples before any header make a headerless one).
    The 32-bit ticks are unwrapped (samples are in time order) and made relative to the first."""
    profiles: list[Profile] = []
    current: Profile | None = None
    previous_raw = 0
    unwrapped = 0
    first = None
    for line in lines:
        line = line.rstrip("\r\n")
        header = HEADER_RE.search(line)
        if header:
            fields = dict(item.split("=", 1) for item in header.group(2).split())
            current = Profile(header=fields, note=header.group(3) or "")
            profiles.append(current)
            first = None
            continue
        match = SAMPLE_RE.search(line)
        if not match:
            continue
        words = [int(word, 16) for word in match.group(1).split()]
        if len(words) < 4:
            continue
        if current is None:
            current = Profile()
            profiles.append(current)
            first = None
        raw = words[0]
        if first is None:
            first = raw
            unwrapped = 0
        else:
            unwrapped += (raw - previous_raw) % WORD
        previous_raw = raw
        current.samples.append(Sample(unwrapped, words[1], words[2], words[3], [w for w in words[4:] if w]))
    return profiles


class Symbols:
    """Functions by address. `table` is (start, size, name); a size of 0 runs to the next symbol."""

    def __init__(self, table: Iterable[tuple[int, int, str]]):
        # (several names for one address: the first, which for an ELF is the global one)
        seen: dict[int, tuple[int, str]] = {}
        for start, size, name in table:
            if start not in seen or (size and not seen[start][0]):
                seen[start] = (size, name)
        self.starts = sorted(seen)
        self.sizes = [seen[start][0] for start in self.starts]
        self.names = [seen[start][1] for start in self.starts]

    def lookup(self, address: int) -> str:
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0:
            return UNKNOWN
        start, size = self.starts[index], self.sizes[index]
        if size:
            return self.names[index] if address < start + size else UNKNOWN
        nxt = self.starts[index + 1] if index + 1 < len(self.starts) else start + 0x1000
        return self.names[index] if address < min(nxt, start + 0x10000) else UNKNOWN

    @classmethod
    def from_elf(cls, path: str) -> "Symbols":
        from elftools.elf.elffile import ELFFile
        from elftools.elf.sections import SymbolTableSection

        table: list[tuple[int, int, str]] = []
        with open(path, "rb") as stream:
            elf = ELFFile(stream)
            for section in elf.iter_sections():
                if not isinstance(section, SymbolTableSection):
                    continue
                for symbol in section.iter_symbols():
                    kind = symbol["st_info"]["type"]
                    # (assembly labels are NOTYPE; data and files are not code)
                    if kind not in ("STT_FUNC", "STT_NOTYPE") or not symbol.name or symbol["st_shndx"] == "SHN_UNDEF":
                        continue
                    if kind == "STT_NOTYPE" and (symbol["st_shndx"] == "SHN_ABS" or symbol.name.startswith(("$", "."))):
                        continue
                    value = symbol["st_value"]
                    if kind == "STT_NOTYPE" and not _in_code(elf, symbol["st_shndx"]):
                        continue
                    table.append((value, symbol["st_size"], symbol.name))
        return cls(table)


def _in_code(elf, index) -> bool:
    if not isinstance(index, int):
        return False
    return bool(elf.get_section(index)["sh_flags"] & 0x4)  # SHF_EXECINSTR


def stack_of(sample: Sample, symbols: Symbols) -> list[tuple[int, str]]:
    """The sample's stack, leaf first: (address, function) pairs."""
    leaf = symbols.lookup(sample.pc)
    stack = [(sample.pc, leaf)]
    callers = list(sample.callers)
    if sample.lr and symbols.lookup(sample.lr - 1) != leaf:
        # (a leaf function's caller; a frame's own return address would repeat in the stack)
        if not callers or callers[0] != sample.lr:
            callers.insert(0, sample.lr)
    for address in callers:
        stack.append((address, symbols.lookup(address - 1)))
    return stack


def thread_order(samples: Iterable[Sample]) -> list[int]:
    """The threads, most samples first (thread 1 is the busiest)."""
    return [thread for thread, _ in Counter(sample.thread for sample in samples).most_common()]


@dataclass
class Report:
    total: int
    seconds: float
    threads: list[tuple[int, int]]  # (thread id, samples), busiest first
    self_counts: Counter
    inclusive_counts: Counter
    callers_of: dict[str, Counter]  # function -> Counter of its direct callers
    callees_of: dict[str, Counter]  # function -> Counter of its direct callees ("(self)" for its own time)
    hot_addresses: Counter
    tree: dict
    folded: Counter
    timeline: list[Counter]


def aggregate(profile: Profile, symbols: Symbols, thread: int | None = None, bucket_seconds: float = 1.0) -> Report:
    samples = profile.samples
    if thread is not None:
        samples = [sample for sample in samples if sample.thread == thread]
    self_counts: Counter = Counter()
    inclusive: Counter = Counter()
    callers_of: dict[str, Counter] = defaultdict(Counter)
    callees_of: dict[str, Counter] = defaultdict(Counter)
    hot: Counter = Counter()
    folded: Counter = Counter()
    tree: dict = {"count": 0, "children": {}}
    timeline: list[Counter] = []
    per_bucket = max(1, int(bucket_seconds * profile.timebase))
    for sample in samples:
        names = [name for _, name in stack_of(sample, symbols)]
        self_counts[names[0]] += 1
        hot[sample.pc] += 1
        for name in set(names):
            inclusive[name] += 1
        # (a pair counts once per sample, so a recursive function is not counted twice)
        for pair in set(zip(names, names[1:])):
            callee, caller = pair
            callers_of[callee][caller] += 1
            callees_of[caller][callee] += 1
        callees_of[names[0]]["(self)"] += 1
        folded[";".join(reversed(names))] += 1
        node = tree
        node["count"] += 1
        for name in reversed(names):
            node = node["children"].setdefault(name, {"count": 0, "children": {}})
            node["count"] += 1
        bucket = sample.tick // per_bucket
        while len(timeline) <= bucket:
            timeline.append(Counter())
        timeline[bucket][names[0]] += 1
    ticks = (samples[-1].tick - samples[0].tick) if len(samples) > 1 else 0
    return Report(
        total=len(samples),
        seconds=ticks / profile.timebase,
        threads=[(t, n) for t, n in Counter(s.thread for s in profile.samples).most_common()],
        self_counts=self_counts,
        inclusive_counts=inclusive,
        callers_of=callers_of,
        callees_of=callees_of,
        hot_addresses=hot,
        tree=tree,
        folded=folded,
        timeline=timeline,
    )


def percent(count: int, total: int) -> str:
    return f"{100.0 * count / total:5.1f}%" if total else "    -"


def table(title: str, counts: Counter, total: int, top: int, other: bool = True) -> Iterator[str]:
    yield title
    shown = counts.most_common(top)
    for name, count in shown:
        yield f"  {count:7d} {percent(count, total)}  {name}"
    rest = sum(counts.values()) - sum(count for _, count in shown)
    if other and rest > 0 and len(counts) > top:
        yield f"  {rest:7d} {percent(rest, total)}  ({len(counts) - top} more)"


def render_tree(node: dict, total: int, min_percent: float, max_depth: int, depth: int = 0) -> Iterator[str]:
    children = sorted(node["children"].items(), key=lambda item: -item[1]["count"])
    for name, child in children:
        if 100.0 * child["count"] / total < min_percent or depth >= max_depth:
            continue
        yield f"{'  ' * depth}{percent(child['count'], total)} {child['count']:7d}  {name}"
        yield from render_tree(child, total, min_percent, max_depth, depth + 1)


def matching(report: Report, pattern: str) -> list[str]:
    names = set(report.inclusive_counts)
    exact = [name for name in names if name == pattern]
    return exact or sorted(name for name in names if pattern in name)


def addr2line_lines(addr2line: str, elf: str, addresses: list[int]) -> dict[int, str]:
    """Source lines for code addresses, through addr2line (-f -C -i is not used: the innermost line only)."""
    if not addresses:
        return {}
    result = subprocess.run([addr2line, "-e", elf, "-f", "-s", *[f"0x{a:08x}" for a in addresses]],
                            capture_output=True, text=True, check=False)
    lines = result.stdout.splitlines()
    return {address: f"{lines[2 * i]} {lines[2 * i + 1]}" for i, address in enumerate(addresses) if 2 * i + 1 < len(lines)}


def find_addr2line(explicit: str | None) -> str | None:
    for candidate in (explicit, "powerpc-eabi-addr2line", "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-addr2line",
                      "llvm-addr2line"):
        if candidate and shutil.which(candidate):
            return shutil.which(candidate)
    return None


def report_profile(profile: Profile, index: int, symbols: Symbols, args: argparse.Namespace, out) -> None:
    thread = None
    if args.thread:
        order = thread_order(profile.samples)
        if args.thread.lower().startswith("0x") or len(args.thread) > 3:
            thread = int(args.thread, 16)
        elif args.thread.isdigit() and 1 <= int(args.thread) <= len(order):
            thread = order[int(args.thread) - 1]
        else:
            sys.exit(f"wl_profile.py: no thread {args.thread} (the profile has {len(order)})")
    report = aggregate(profile, symbols, thread, args.timeline or 1.0)

    def emit(lines: Iterable[str]) -> None:
        for line in lines:
            print(line, file=out)

    note = f" ({profile.note})" if profile.note else ""
    emit([f"== profile {index + 1}{note}"])
    header = profile.header
    emit([f"samples {report.total}"
          + (f" of {len(profile.samples)} (thread filter)" if thread is not None else "")
          + f", {report.seconds:.2f} s at a nominal {profile.hz} Hz"
          + (f", {report.total / report.seconds:.0f} a second" if report.seconds > 0 else "")
          + f", depth {header.get('depth', '?')}, dropped {header.get('dropped', '?')}"])
    emit(["threads (busiest first)"])
    for number, (tid, count) in enumerate(report.threads, 1):
        mark = "  <- started the profile" if header.get("thread") and int(header["thread"], 16) == tid else ""
        emit([f"  {number}: {tid:08x} {count:7d} {percent(count, len(profile.samples))}{mark}"])
    if report.total == 0:
        return
    emit(table("top functions by their own samples (self)", report.self_counts, report.total, args.top))
    emit(table("top functions by samples anywhere on the stack (inclusive)", report.inclusive_counts, report.total,
               args.top, other=False))
    for pattern in args.callers or []:
        names = matching(report, pattern)
        if not names:
            emit([f"no function matches {pattern!r}"])
        for name in names[:3]:
            total = report.inclusive_counts[name]
            emit([f"{name}: {total} samples ({percent(total, report.total).strip()}), "
                  f"{report.self_counts[name]} of them its own"])
            emit(table("  called from", report.callers_of.get(name, Counter()), total, args.top))
            emit(table("  spent in", report.callees_of.get(name, Counter()), total, args.top))
    if args.tree is not None:
        emit([f"call tree (outermost recorded frame first; branches under {args.tree}% left out)"])
        emit(render_tree(report.tree, report.total, args.tree, args.tree_depth))
    if args.hot:
        hottest = report.hot_addresses.most_common(args.hot)
        lines = {}
        tool = find_addr2line(args.addr2line)
        if tool:
            lines = addr2line_lines(tool, args.elf, [address for address, _ in hottest])
        emit([f"hottest instructions"])
        for address, count in hottest:
            emit([f"  {count:7d} {percent(count, report.total)}  {address:08x}  {symbols.lookup(address)}"
                  + (f"  {lines[address]}" if address in lines else "")])
        if not tool:
            emit(["  (no addr2line found for the source lines: --addr2line)"])
    if args.timeline:
        emit([f"time line, {args.timeline:g} s a step: samples and the top three functions"])
        for step, counter in enumerate(report.timeline):
            total = sum(counter.values())
            top3 = ", ".join(f"{name} {percent(count, total).strip()}" for name, count in counter.most_common(3))
            emit([f"  {step * args.timeline:7.1f} s {total:6d}  {top3}"])
    if args.collapsed:
        path = args.collapsed if index == 0 else f"{args.collapsed}.{index + 1}"
        with open(path, "w", encoding="utf-8") as stream:
            for stack, count in sorted(report.folded.items()):
                stream.write(f"{stack} {count}\n")
        emit([f"folded stacks written to {path}"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("elf", help="the program's ELF (symbols are read with pyelftools)")
    parser.add_argument("dump", nargs="+", help="dump files or logs holding WLPROF/WLPS lines (- for standard input)")
    parser.add_argument("--top", type=int, default=30, help="rows in each table (30)")
    parser.add_argument("--thread", help="only this thread: its number in the busiest-first list, or its id in hex")
    parser.add_argument("--callers", action="append", metavar="FUNC",
                        help="who calls FUNC and where its time goes (a name or part of one; repeatable)")
    parser.add_argument("--tree", type=float, nargs="?", const=1.0, metavar="MIN_PERCENT",
                        help="the call tree, leaving out branches under MIN_PERCENT of the samples (1)")
    parser.add_argument("--tree-depth", type=int, default=16, help="levels of the tree (16)")
    parser.add_argument("--hot", type=int, nargs="?", const=20, metavar="N", help="the N hottest instructions (20)")
    parser.add_argument("--addr2line", help="the addr2line for --hot (powerpc-eabi-addr2line by default)")
    parser.add_argument("--timeline", type=float, nargs="?", const=1.0, metavar="SECONDS",
                        help="the busiest functions in each step of SECONDS (1)")
    parser.add_argument("--collapsed", metavar="FILE", help="write folded stacks for flamegraph.pl")
    args = parser.parse_args(argv)

    lines: list[str] = []
    for path in args.dump:
        if path == "-":
            lines.extend(sys.stdin.read().splitlines())
        else:
            with open(path, encoding="utf-8", errors="replace") as stream:
                lines.extend(stream.read().splitlines())
    profiles = parse_dump(lines)
    if not profiles:
        print("wl_profile.py: no WLPROF or WLPS lines in the input", file=sys.stderr)
        return 1
    symbols = Symbols.from_elf(args.elf)
    for index, profile in enumerate(profiles):
        report_profile(profile, index, symbols, args, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
