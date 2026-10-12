#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wraps the e2e title (an i386 ELF linked with title.ld) in an XBE that xboxrecomp's pipeline and
wl/xboxrt load: the headers on the first page (certificate, section headers and names), each
allocated ELF section as an XBE section (.bss folded into .data's virtual size), the kernel thunk
table filled with 0x80000000 | ordinal, the entry point and thunk address XOR-encoded with the
retail keys. Also writes a header with the addresses the test reads (`results`).

Usage: mkxbe_title.py title.elf out.xbe out_syms.h
"""
import struct
import sys

BASE = 0x00010000
HDR = 0x1000
ENTRY_KEY = 0xA8FC57AB
THUNK_KEY = 0x5B6D40B6
# the thunk slots, in title.c's order
ORDINALS = [184, 166, 173, 126, 255, 258, 189, 225, 233, 187, 190, 236, 8, 53, 49, 156]

SHF_ALLOC, SHF_WRITE, SHF_EXEC = 0x2, 0x1, 0x4
SHT_NOBITS, SHT_SYMTAB = 8, 2


def elf_sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("not a 32-bit ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(shnum):
        f = struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
        secs.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3], offset=f[4], size=f[5], link=f[6],
                         entsize=f[9]))
    strtab = secs[shstrndx]
    for s in secs:
        end = data.index(b"\0", strtab["offset"] + s["name_off"])
        s["name"] = data[strtab["offset"] + s["name_off"]:end].decode()
    return secs


def elf_symbols(data, secs):
    syms = {}
    for s in secs:
        if s["type"] != SHT_SYMTAB:
            continue
        strs = secs[s["link"]]
        for i in range(s["size"] // 16):
            name_off, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, s["offset"] + 16 * i)
            end = data.index(b"\0", strs["offset"] + name_off)
            name = data[strs["offset"] + name_off:end].decode()
            if name:
                syms[name] = value
    return syms


def main():
    elf_path, xbe_path, syms_path = sys.argv[1:4]
    data = open(elf_path, "rb").read()
    secs = [s for s in elf_sections(data) if s["flags"] & SHF_ALLOC and s["size"] > 0]
    syms = elf_symbols(data, elf_sections(data))
    # fold .bss into the section before it (.data)
    xsecs = []
    for s in sorted(secs, key=lambda s: s["addr"]):
        raw = b"" if s["type"] == SHT_NOBITS else data[s["offset"]:s["offset"] + s["size"]]
        if s["type"] == SHT_NOBITS and xsecs:
            prev = xsecs[-1]
            prev["vsize"] = s["addr"] + s["size"] - prev["va"]
            continue
        xsecs.append(dict(name=s["name"], va=s["addr"], vsize=s["size"], raw=bytearray(raw), flags=s["flags"]))
    thunk_va = syms["kthunks"]
    for x in xsecs:
        if x["va"] <= thunk_va < x["va"] + len(x["raw"]) + 0x1000:
            need = thunk_va - x["va"] + 4 * (len(ORDINALS) + 1)
            if len(x["raw"]) < need:
                x["raw"] += bytes(need - len(x["raw"]))
            for i, o in enumerate(ORDINALS):
                struct.pack_into("<I", x["raw"], thunk_va - x["va"] + 4 * i, 0x80000000 | o)
            struct.pack_into("<I", x["raw"], thunk_va - x["va"] + 4 * len(ORDINALS), 0)
    image_end = max(x["va"] + max(x["vsize"], len(x["raw"])) for x in xsecs)
    image_size = (image_end - BASE + 0xFFF) & ~0xFFF

    hdr = bytearray(HDR)
    cert_va, sec_va, names_va = BASE + 0x200, BASE + 0x400, BASE + 0x800
    hdr[0:4] = b"XBEH"
    struct.pack_into("<I", hdr, 0x104, BASE)
    struct.pack_into("<I", hdr, 0x108, HDR)
    struct.pack_into("<I", hdr, 0x10C, image_size)
    struct.pack_into("<I", hdr, 0x110, 0x178)
    struct.pack_into("<I", hdr, 0x118, cert_va)
    struct.pack_into("<I", hdr, 0x11C, len(xsecs))
    struct.pack_into("<I", hdr, 0x120, sec_va)
    struct.pack_into("<I", hdr, 0x128, syms["title_entry"] ^ ENTRY_KEY)
    struct.pack_into("<I", hdr, 0x130, 0x10000)  # stack commit
    struct.pack_into("<I", hdr, 0x134, 0x100000)  # heap reserve
    struct.pack_into("<I", hdr, 0x138, 0x1000)
    struct.pack_into("<I", hdr, 0x13C, BASE)
    struct.pack_into("<I", hdr, 0x140, image_size)
    struct.pack_into("<I", hdr, 0x158, thunk_va ^ THUNK_KEY)
    c = cert_va - BASE
    struct.pack_into("<I", hdr, c + 0x00, 0x1D0)
    struct.pack_into("<I", hdr, c + 0x08, 0x57490001)
    title = "WLE2E".encode("utf-16-le")
    hdr[c + 0x0C:c + 0x0C + len(title)] = title
    struct.pack_into("<I", hdr, c + 0xA0, 1)  # game region: North America
    body = bytearray()
    name_at = names_va
    for i, x in enumerate(xsecs):
        raw_off = HDR + len(body)
        body += x["raw"]
        body += bytes((-len(body)) % 0x1000)
        flags = 0x2  # preload
        if x["flags"] & SHF_WRITE:
            flags |= 0x1
        if x["flags"] & SHF_EXEC:
            flags |= 0x4
        name = x["name"].encode() + b"\0"
        hdr[name_at - BASE:name_at - BASE + len(name)] = name
        struct.pack_into("<IIIIII", hdr, sec_va - BASE + 56 * i, flags, x["va"], max(x["vsize"], len(x["raw"])), raw_off,
                         len(x["raw"]), name_at)
        name_at += len(name)
    open(xbe_path, "wb").write(bytes(hdr) + bytes(body))
    with open(syms_path, "w") as f:
        f.write("// generated by mkxbe_title.py\n#pragma once\n")
        f.write("#define E2E_RESULTS_VA 0x%08Xu\n" % syms["results"])
        f.write("#define E2E_ENTRY_VA 0x%08Xu\n" % syms["title_entry"])
        f.write("#define E2E_THUNKS_VA 0x%08Xu\n" % thunk_va)


if __name__ == "__main__":
    main()
