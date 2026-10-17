#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tables from a recompiled title's page census (wl/xboxrt/page_census.h).

    xb_page_census.py <census file> [--from S] [--to S] [--allocs N]

Reads <census file> (a line a second) and <census file>.pages (the allocations and every page's
record) and prints: what the pages touched are (by class and by allocation kind, in MB), the
working set each second (its spread over the window asked for, and a line every 10 s), the
allocations that hold the most touched pages, and the four questions of the census: x86 code
touched after the load, pages the GPU converted (a texture or a vertex array that also lives
converted in a GX arena), pages the APU read, allocated pages never touched.
"""
import argparse
import collections
import statistics
import sys

PAGE = 4096
USE = {0x01: "write", 0x02: "gpu", 0x04: "tex", 0x08: "vtx", 0x10: "apu", 0x20: "map", 0x40: "file"}


def mb(pages):
    return "%.1f" % (pages * PAGE / 1048576.0)


def use_names(u):
    return ",".join(n for b, n in USE.items() if u & b) or "-"


def page_class(kind, u):
    if kind == "xbe_code":
        return "code"
    for bit, name in ((0x04, "tex"), (0x08, "vtx"), (0x10, "apu"), (0x02, "gpu")):
        if u & bit:
            return name
    return {"xbe_data": "xdata", "stack": "stack", "pool": "kernel", "system": "kernel",
            "contig": "cmap" if u & 0x20 else "contig", "virtual": "vmap" if u & 0x20 else "virt"}.get(kind, "other")


def read(path):
    seconds = []
    with open(path) as f:
        for line in f:
            w = line.split()
            if w and w[0] == "second":
                seconds.append({w[i]: int(w[i + 1]) for i in range(0, len(w) - 1, 2)})
    allocs, pages = {}, []
    with open(path + ".pages") as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            if w[0] == "alloc":
                allocs[int(w[1])] = {"kind": w[2], "va": int(w[3], 16), "bytes": int(w[4], 16),
                                     "caller": int(w[5], 16), "second": int(w[6])}
            elif w[0] == "page":
                pages.append({"p": int(w[1], 16), "alloc": int(w[2]), "use": int(w[3], 16), "first": int(w[4]),
                              "last": int(w[5]), "seconds": int(w[6]), "resident": int(w[7])})
    return seconds, allocs, pages


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("census")
    ap.add_argument("--from", dest="lo", type=int, default=1, help="the window's first second")
    ap.add_argument("--to", dest="hi", type=int, default=0, help="its last (0: the end)")
    ap.add_argument("--allocs", type=int, default=20, help="allocations listed")
    a = ap.parse_args()
    seconds, allocs, pages = read(a.census)
    hi = a.hi or (seconds[-1]["second"] if seconds else 0)
    for pg in pages:
        al = allocs.get(pg["alloc"])
        pg["kind"] = al["kind"] if al else "none"
        pg["class"] = page_class(pg["kind"], pg["use"])
    touched = [pg for pg in pages if pg["first"]]
    print("census: %d seconds, %d pages touched (%s MB), %d allocations" % (len(seconds), len(touched),
                                                                         mb(len(touched)), len(allocs)))

    print("\npages touched, by class (MB): ever / after second 5 / in [%d, %d] / allocated, never touched" % (a.lo, hi))
    by = collections.defaultdict(lambda: [0, 0, 0, 0])
    for pg in pages:
        row = by[pg["class"]]
        if pg["first"]:
            row[0] += 1
            row[1] += pg["last"] > 5
            row[2] += pg["last"] >= a.lo and pg["first"] <= hi
        elif pg["alloc"]:
            row[3] += 1
    for c, row in sorted(by.items(), key=lambda kv: -kv[1][0]):
        print("  %-7s %7s %7s %7s %7s" % (c, mb(row[0]), mb(row[1]), mb(row[2]), mb(row[3])))

    window = [s for s in seconds if a.lo <= s["second"] <= hi]
    if window:
        ws = [s["ws"] for s in window]
        print("\nworking set per second in [%d, %d] (MB): min %s median %s max %s; resident median %s" % (
            a.lo, hi, mb(min(ws)), mb(statistics.median(ws)), mb(max(ws)),
            mb(statistics.median(s["resident"] for s in window))))
        for w in ("ws5", "ws10", "ws30"):
            if w in window[0]:
                v = [s[w] for s in window]
                print("  touched in the last %s s (MB): median %s max %s" % (w[2:], mb(statistics.median(v)), mb(max(v))))
        cols = ["code", "tex", "vtx", "apu", "gpu", "xdata", "stack", "kernel", "cmap", "contig", "vmap", "virt", "other"]
        print("  median by class (MB): " + " ".join("%s %s" % (c, mb(statistics.median(s.get(c, 0) for s in window)))
                                                    for c in cols))
        print("  second   ws  new  resident ws10 ws30 (MB)  " + " ".join(cols))
        for s in window:
            if s["second"] % 10 == 0 or s is window[0]:
                print("  %5d %6s %4s %6s %5s %5s      %s" % (s["second"], mb(s["ws"]), mb(s["new"]), mb(s["resident"]),
                                                     mb(s.get("ws10", 0)), mb(s.get("ws30", 0)),
                                                     " ".join("%4s" % mb(s.get(c, 0)) for c in cols)))

    print("\nallocations by pages touched (MB touched / allocated, in the window, uses):")
    per = collections.defaultdict(lambda: {"t": 0, "n": 0, "w": 0, "use": 0})
    for pg in pages:
        e = per[pg["alloc"]]
        e["n"] += 1
        e["t"] += pg["first"] > 0
        e["w"] += pg["first"] > 0 and pg["last"] >= a.lo and pg["first"] <= hi
        e["use"] |= pg["use"]
    for i, e in sorted(per.items(), key=lambda kv: -kv[1]["t"])[:a.allocs]:
        al = allocs.get(i, {"kind": "none", "va": 0, "bytes": 0, "caller": 0, "second": 0})
        print("  #%-5d %-8s va %08x %6s KB from %08x (s%d): %s / %s MB, window %s, %s" % (
            i, al["kind"], al["va"], al["bytes"] >> 10, al["caller"], al["second"], mb(e["t"]), mb(e["n"]),
            mb(e["w"]), use_names(e["use"])))

    print("\nthe questions:")
    code = [pg for pg in pages if pg["kind"] == "xbe_code"]
    later = [pg for pg in code if pg["last"] > pg["first"] or pg["seconds"] > 1]
    print("  (a) x86 code: %s MB in the XBE's code sections; %s MB touched in more than its load second "
          "(read as data: jump tables, the runtime's reads)" % (mb(len(code)), mb(len(later))))
    for name, bit in (("textures", 0x04), ("vertex arrays", 0x08)):
        sel = [pg for pg in pages if pg["use"] & bit]
        recent = [pg for pg in sel if pg["last"] >= a.lo and pg["first"] <= hi]
        print("  (b) %s converted by the GPU: %s MB (%s MB from a map file), %s MB touched in the window" % (
            name, mb(len(sel)), mb(sum(1 for pg in sel if pg["use"] & 0x20)), mb(len(recent))))
    apu = [pg for pg in pages if pg["use"] & 0x10]
    print("  (c) read by the APU: %s MB (%s MB from a map file), %s MB touched in the window" % (
        mb(len(apu)), mb(sum(1 for pg in apu if pg["use"] & 0x20)),
        mb(sum(1 for pg in apu if pg["last"] >= a.lo and pg["first"] <= hi))))
    never = [pg for pg in pages if pg["alloc"] and not pg["first"]]
    once = [pg for pg in touched if pg["seconds"] == 1]
    print("  (d) allocated, never touched: %s MB; touched in one second only: %s MB" % (mb(len(never)), mb(len(once))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
