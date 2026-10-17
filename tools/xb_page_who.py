#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Who touched a recompiled title's guest pages: the page census's host stacks, symbolized.

    xb_page_who.py <census file> --elf <program> [--from S] [--to S] [--alloc VA] [--top N]

Reads <census file>.stacks and .touch (a run with WL_XB_PAGE_CENSUS_STACKS=1, wl/xboxrt/page_census.c)
and .pages (the allocations), and prints the touches (page-seconds: a page's first touch in a census
second) and the distinct pages, in MB, by toucher: the guest's code (the innermost translated
function, sub_<address>), our texture cache (hashing, converting, restoring), the vertex cache, the
APU, the kernel's file reads (with the guest function that asked), and anything else by its innermost
host function. --alloc keeps the pages of the allocation at that guest address (0x82AFF000: Halo's
texture cache); --from/--to the census seconds.
"""
import argparse
import bisect
import collections
import struct
import subprocess
import sys

PAGE = 4096
# the fault's own frames: everything up to and including the handler is dropped
HANDLER = ("on_fault",)


def symbols(elf):
    out = subprocess.run(["nm", "-C", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--from", dest="lo", type=int, default=1)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
    ap.add_argument("--alloc", type=lambda s: int(s, 0), default=None, help="an allocation's guest address")
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()

    addrs, names = symbols(a.elf)
    name_at = {n: ad for ad, n in zip(addrs, names)}
    stacks, bias = {}, 0
    with open(a.census + ".stacks") as f:
        for line in f:
            w = line.split()
            if w[0] == "anchor":
                bias = int(w[1], 16) - name_at["wl_xb_census_start"]
            elif w[0] == "stack":
                stacks[int(w[1])] = [int(x, 16) for x in w[2:]]

    def sym(pc):
        i = bisect.bisect_right(addrs, pc - bias) - 1
        return names[i] if i >= 0 and pc - bias - addrs[i] < 0x100000 else None

    keep = None
    if a.alloc is not None:
        alloc_ids, keep = set(), set()
        with open(a.census + ".pages") as f:
            for line in f:
                w = line.split()
                if w[0] == "alloc" and int(w[3], 16) == a.alloc:
                    alloc_ids.add(w[1])
                elif w[0] == "page" and w[2] in alloc_ids:
                    keep.add(int(w[1], 16))
        if not keep:
            sys.exit("no pages in an allocation at 0x%08x" % a.alloc)

    cache = {}

    def who(sid):
        if sid in cache:
            return cache[sid]
        frames = [sym(pc) for pc in stacks.get(sid, [])]
        for i, n in enumerate(frames):
            if n in HANDLER:
                frames = frames[i + 1:]
                break
        inner = next((n for n in frames if n is not None), "?")
        guest = next((n for n in frames if n is not None and n.startswith("sub_")), None)
        named = [n for n in frames if n is not None]
        if any(n.startswith("wl_nv2a_texcache") or n == "hash_texels" for n in named):
            # (the path inside the cache: the innermost frames down to its entry point)
            k = next(i for i, n in enumerate(named) if n.startswith("wl_nv2a_texcache"))
            r = ("our texture cache", " < ".join(named[: k + 2]))
        elif any("vcache" in n for n in named):
            r = ("our vertex cache", inner)
        elif any("apu" in n.lower() for n in named):
            r = ("the APU", inner)
        elif any(n.startswith(("xk_", "Nt", "wl_xb_copy")) or "nt_file" in n for n in named):
            k = next(n for n in named if n.startswith(("xk_", "Nt", "wl_xb_copy")) or "nt_file" in n)
            r = ("the kernel (" + k + ")", guest or inner)
        elif guest is not None and inner.startswith("sub_"):
            r = ("guest code", guest)
        elif guest is not None:
            r = ("guest code via " + inner, guest)
        else:
            r = ("other", inner)
        cache[sid] = r
        return r

    touches = collections.Counter()
    pages = collections.defaultdict(set)
    detail = collections.Counter()
    per_cls_pages = collections.defaultdict(set)
    data = open(a.census + ".touch", "rb").read()
    n = len(data) // 12
    for sec, page, sid in struct.iter_unpack("<3I" if sys.byteorder == "little" else ">3I", data[: n * 12]):
        if sec < a.lo or sec > a.hi or (keep is not None and page not in keep):
            continue
        cls, d = who(sid)
        touches[cls] += 1
        per_cls_pages[cls].add(page)
        detail[(cls, d)] += 1
        pages[(cls, d)].add(page)
    total = sum(touches.values())
    allp = set().union(*per_cls_pages.values()) if per_cls_pages else set()
    print("touches (page-seconds) %d = %.1f MB; distinct pages %d = %.1f MB" %
          (total, total * PAGE / 1048576.0, len(allp), len(allp) * PAGE / 1048576.0))
    print("%-48s %9s %7s %8s %7s" % ("toucher", "touches", "MB", "pages", "MB"))
    for cls, t in touches.most_common():
        p = len(per_cls_pages[cls])
        print("%-48s %9d %7.1f %8d %7.1f" % (cls[:48], t, t * PAGE / 1048576.0, p, p * PAGE / 1048576.0))
    print()
    print("%-130s %9s %8s" % ("toucher: function", "touches", "pages"))
    for (cls, d), t in detail.most_common(a.top):
        print("%-130s %9d %8d" % ((cls + ": " + d)[:130], t, len(pages[(cls, d)])))


if __name__ == "__main__":
    main()
