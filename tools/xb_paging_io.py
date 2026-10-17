#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Where a title's card (SD) traffic comes from, from the page census's io and card lines
(wl/xboxrt/page_census.h; a host run with WL_XB_PAGE_CENSUS=<file>).

  xb_paging_io.py <census> [--window name:from:to ...] [--worst 5] [--lru from:to --sizes 24,33,40]

Per window: the card ops and MB a second, split into the pager's swap reads and writes, its re-reads
of map and XBE pages (file copies, with the pages read ahead in the same op), the compressed page
cache's hits (no op), and the kernel's own file reads and writes (the title's I/O). The worst
seconds (the pager's ops summed over --worst seconds) with the working set and the frames then. What
the pager read, by page class: "fresh" pages were filled by a file read and no device used them since
(a texture read back before its conversion), "map" pages a map read filled.

--lru (needs <census>.touch: WL_XB_PAGE_CENSUS_STACKS=1) replays the touches second by second through
an LRU pool of each size (MB of frames) and counts the touches that would read the card: a page with
contents, not in the pool, not filled by a file read in that second. The smallest pool with none is
the one that needs no SD reads in the window.
"""
import argparse
import collections
import struct
import sys

PAGE = 4096
FILL = 0xFFFFFFFF


def parse(path):
    secs, io, card = {}, collections.defaultdict(dict), {}
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            if w[0] == "second":
                s = int(w[1])
                secs[s] = {w[i]: int(w[i + 1]) for i in range(2, len(w) - 1, 2)}
            elif w[0] == "io":
                s, kind = int(w[1]), w[2]
                d, al = {}, {}
                for i in range(3, len(w) - 1, 2):
                    if w[i].startswith("@"):
                        pg, _, fr = w[i + 1].partition("/")
                        al[w[i][1:]] = (int(pg), int(fr or 0))
                    else:
                        d[w[i]] = int(w[i + 1])
                d["@"] = al
                io[s][kind] = d
            elif w[0] == "card":
                s = int(w[1])
                d = {}
                for i in range(2, len(w), 5):
                    k = w[i]
                    d[k] = tuple(int(x) for x in w[i + 1:i + 5])  # map ops, map KB, other ops, other KB
                card[s] = d
    return secs, io, card


def allocs(path):
    """The census's allocations by number: 'kind va size'."""
    out = {"0": "none", "rest": "(others)"}
    try:
        with open(path + ".pages") as f:
            for line in f:
                w = line.split()
                if w and w[0] == "alloc":
                    out[w[1]] = "%s %s %.1f MB" % (w[2], w[3], int(w[4], 16) / 1048576.0)
    except OSError:
        pass
    return out


def alloc_table(lo, hi, io, names):
    """What the pager read back and evicted, by allocation: pages and fresh pages."""
    tot = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0])  # reads, fresh, evicts, fresh, unused ahead, zhit
    cols = {"file_in": 0, "ahead": 0, "swap_in": 0, "evict": 2, "ahead_unused": 4, "zhit": 5}
    for s in range(lo, hi + 1):
        for kind, d in io.get(s, {}).items():
            col = cols.get(kind)
            if col is None:
                continue
            for a, (pg, fr) in d.get("@", {}).items():
                tot[a][col] += pg
                if col in (0, 2):
                    tot[a][col + 1] += fr
    print("| allocation | read back (pages) | of them fresh | evicted | of them fresh | ahead unused | zhit |")
    print("|---|---|---|---|---|---|---|")
    for a, v in sorted(tot.items(), key=lambda kv: -kv[1][0])[:8]:
        print("| %s (#%s) | %d | %d | %d | %d | %d | %d |" % (names.get(a, "?"), a, *v))
    print()


def xbe(d):
    return d.get("code", 0) + d.get("xdata", 0)


def second_row(s, io, card):
    """One second's traffic: ops and pages (or KB) by source."""
    g = io.get(s, {})
    z = {}
    si, so, fi, ah = g.get("swap_in", {}), g.get("swap_out", {}), g.get("file_in", {}), g.get("ahead", {})
    z["swap_rd_ops"], z["swap_rd_pg"] = si.get("ops", 0), si.get("pages", 0)
    z["swap_wr_ops"], z["swap_wr_pg"] = so.get("ops", 0), so.get("pages", 0)
    fx, ax = xbe(fi), xbe(ah)
    z["xbe_ops"], z["xbe_pg"] = fx, fx + ax
    z["map_ops"], z["map_pg"] = fi.get("map", 0), fi.get("map", 0) + ah.get("map", 0)
    z["oth_ops"] = fi.get("ops", 0) - fx - fi.get("map", 0)
    z["oth_pg"] = fi.get("pages", 0) + ah.get("pages", 0) - z["xbe_pg"] - z["map_pg"]
    z["zhit_pg"] = g.get("zhit", {}).get("pages", 0)
    z["pager_ops"] = z["swap_rd_ops"] + z["swap_wr_ops"] + fi.get("ops", 0)
    c = card.get(s, {})
    rd, ra, wr = c.get("read", (0, 0, 0, 0)), c.get("ahead", (0, 0, 0, 0)), c.get("write", (0, 0, 0, 0))
    rh = c.get("read_held", (0, 0, 0, 0))
    z["guest_rd_ops"] = rd[0] + rd[2] + ra[0] + ra[2] + rh[0] + rh[2]
    z["guest_rd_kb"] = rd[1] + rd[3] + ra[1] + ra[3] + rh[1] + rh[3]
    z["guest_held_ops"], z["guest_held_kb"] = rh[0] + rh[2], rh[1] + rh[3]
    z["guest_map_ops"], z["guest_map_kb"] = rd[0] + ra[0] + rh[0], rd[1] + ra[1] + rh[1]
    z["guest_wr_ops"], z["guest_wr_kb"] = wr[0] + wr[2], wr[1] + wr[3]
    return z


def mb(pages):
    return pages * PAGE / 1048576.0


def window_table(name, lo, hi, secs, io, card):
    rows = [second_row(s, io, card) for s in range(lo, hi + 1)]
    n = max(1, hi - lo + 1)
    t = collections.Counter()
    for r in rows:
        t.update(r)
    print("## %s: seconds %d-%d (%d s)" % (name, lo, hi, n))
    print("| source | ops | ops/s | MB | MB/s |")
    print("|---|---|---|---|---|")
    for label, ops, size in (("pager: swap reads", "swap_rd_ops", mb(t["swap_rd_pg"])),
                             ("pager: swap writes", "swap_wr_ops", mb(t["swap_wr_pg"])),
                             ("pager: map re-reads (+ahead)", "map_ops", mb(t["map_pg"])),
                             ("pager: XBE re-reads (+ahead)", "xbe_ops", mb(t["xbe_pg"])),
                             ("pager: other file re-reads", "oth_ops", mb(t["oth_pg"])),
                             ("compressed-cache hits (no op)", None, mb(t["zhit_pg"])),
                             ("guest file reads (title I/O)", "guest_rd_ops", t["guest_rd_kb"] / 1024.0),
                             ("  of them of maps", "guest_map_ops", t["guest_map_kb"] / 1024.0),
                             ("  of them small: guest lock kept", "guest_held_ops", t["guest_held_kb"] / 1024.0),
                             ("guest file writes", "guest_wr_ops", t["guest_wr_kb"] / 1024.0)):
        o = t[ops] if ops else 0
        print("| %s | %d | %.1f | %.1f | %.2f |" % (label, o, o / n, size, size / n))
    print("| pager ops in all | %d | %.1f | | |" % (t["pager_ops"], t["pager_ops"] / n))
    print()
    return rows


def kinds_table(lo, hi, io):
    tot = collections.defaultdict(collections.Counter)
    for s in range(lo, hi + 1):
        for k, d in io.get(s, {}).items():
            tot[k].update({c: v for c, v in d.items() if c != "@"})
    print("| kind | ops | pages | MB | map | fresh | by class (pages) |")
    print("|---|---|---|---|---|---|---|")
    for k in ("swap_in", "swap_out", "file_in", "ahead", "zhit", "evict", "ahead_unused", "forget"):
        d = tot.get(k)
        if not d:
            continue
        cls = ", ".join("%s %d" % (c, v) for c, v in d.most_common()
                        if c not in ("ops", "pages", "map", "fresh", "@") and v)
        print("| %s | %d | %d | %.1f | %d | %d | %s |" % (k, d["ops"], d["pages"], mb(d["pages"]), d["map"],
                                                          d["fresh"], cls))
    print()


def worst(lo, hi, span, secs, io, card, top):
    ops = {s: second_row(s, io, card)["pager_ops"] for s in range(lo, hi + 1)}
    sums = []
    for s in range(lo, hi - span + 2):
        sums.append((sum(ops[s + k] for k in range(span)), s))
    sums.sort(reverse=True)
    picked = []
    for v, s in sums:
        if v == 0 or len(picked) >= top:
            break
        if all(abs(s - p) >= span for _, p in picked):
            picked.append((v, s))
    print("| worst %d s | pager ops | guest rd ops | ws (1 s, max) | ws5 at end | resident | what the pager read (pages) |"
          % span)
    print("|---|---|---|---|---|---|---|")
    for v, s in picked:
        e = s + span - 1
        g = sum(second_row(k, io, card)["guest_rd_ops"] for k in range(s, e + 1))
        wsmax = max(secs.get(k, {}).get("ws", 0) for k in range(s, e + 1))
        last = secs.get(e, {})
        what = collections.Counter()
        for k in range(s, e + 1):
            for kind in ("swap_in", "file_in", "ahead"):
                d = io.get(k, {}).get(kind, {})
                for c, n in d.items():
                    if c not in ("ops", "pages", "map", "fresh", "@"):
                        what[kind + ":" + c] += n
        print("| %d-%d | %d | %d | %.1f MB | %.1f MB | %.1f MB | %s |" % (
            s, e, v, g, mb(wsmax), mb(last.get("ws5", 0)), mb(last.get("resident", 0)),
            ", ".join("%s %d" % kv for kv in what.most_common(5))))
    print()


def lru(path, lo, hi, sizes_mb):
    data = open(path + ".touch", "rb").read()
    n = len(data) // 12
    fmt = "<3I" if sys.byteorder == "little" else ">3I"
    by_sec = collections.defaultdict(set)
    fills = collections.defaultdict(set)
    for sec, page, sid in struct.iter_unpack(fmt, data[:n * 12]):
        if sid == FILL:
            fills[sec].add(page)
        else:
            by_sec[sec].add(page)
    last_sec = max(by_sec) if by_sec else 0
    hi = min(hi, last_sec)
    print("| frames (MB) | touches that read the card, s %d-%d | MB | per s |" % (lo, hi))
    print("|---|---|---|---|")
    out = {}
    for size in sizes_mb:
        cap = size * 1048576 // PAGE
        pool = collections.OrderedDict()
        has = set()
        miss = 0
        for s in range(1, hi + 1):
            filled = fills.get(s, set())
            for p in sorted(by_sec.get(s, ())):
                if p in pool:
                    pool.move_to_end(p)
                    continue
                if p in has and p not in filled and s >= lo:
                    miss += 1
                pool[p] = True
                has.add(p)
                if len(pool) > cap:
                    pool.popitem(last=False)
            for p in filled:  # (a fill seen without a touch: the page has contents)
                has.add(p)
        out[size] = miss
        print("| %d | %d | %.1f | %.2f |" % (size, miss, mb(miss), miss / max(1, hi - lo + 1)))
    print()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census")
    ap.add_argument("--window", action="append", default=[], help="name:from:to (census seconds)")
    ap.add_argument("--worst", type=int, default=5)
    ap.add_argument("--top", type=int, default=3)
    ap.add_argument("--lru", default=None, help="from:to")
    ap.add_argument("--sizes", default="24,28,33,36,40,44,48,56,64")
    a = ap.parse_args()
    secs, io, card = parse(a.census)
    if not secs:
        sys.exit("no census lines in " + a.census)
    names = allocs(a.census)
    windows = [w.split(":") for w in a.window] or [["all", "1", str(max(secs))]]
    for name, lo, hi in windows:
        lo, hi = int(lo), min(int(hi), max(secs))
        window_table(name, lo, hi, secs, io, card)
        kinds_table(lo, hi, io)
        alloc_table(lo, hi, io, names)
        worst(lo, hi, a.worst, secs, io, card, a.top)
    if a.lru:
        lo, hi = (int(x) for x in a.lru.split(":"))
        lru(a.census, lo, hi, [int(x) for x in a.sizes.split(",")])


if __name__ == "__main__":
    main()
