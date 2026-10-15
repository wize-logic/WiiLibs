#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rewrites a file of vertex programs from tools/xbox_vsh_codegen.c for the host: its paired-single
code (#if WL_XBOX_VSH_PS, the Wii's) compiled in, each asm statement replaced by C that does what
the Gekko does, so the host checks the code the Wii runs against the interpreter
(tests/xbox/suite_vsh_ps.c).

    vsh_ps_emulate.py programs.c emulated.c

A pair is two floats. A scalar input's ps1 is a NaN, so code that reads it shows; the fused
multiply-adds are fmaf (one rounding); a product of two floats is exact in a double, so a float
multiply rounds once as ps_mul does. psq_l reads floats (GQR 0); with W=1 its ps1 is 1.0.
"""
import re
import sys


def operands(text):
    """[name] "constraint"(value), ... as (name or None, constraint, value)"""
    parts, depth, current = [], 0, ""
    for ch in text:
        if ch == "," and depth == 0:
            parts.append(current.strip())
            current = ""
            continue
        depth += (ch == "(") - (ch == ")")
        current += ch
    if current.strip():
        parts.append(current.strip())
    result = []
    for part in parts:
        m = re.match(r'(?:\[(\w+)\]\s*)?"([^"]*)"\((.*)\)$', part, re.S)
        if not m:
            raise SystemExit("vsh_ps_emulate: operand %r" % part)
        result.append(m.groups())
    return result


def register(text):
    return "e_" + re.match(r"%\[(\w+)\]", text.strip()).group(1)


def pair(d, a, b):
    return "{ EmuPair t = { %s, %s }; %s = t; }" % (a, b, d)


def instruction(op, args):
    a = [x.strip() for x in args.split(",")]
    if op == "psq_l":
        offset, base = re.match(r"(-?\d+)\((%\[\w+\])\)", a[1]).groups()
        address = "(%s + %s)" % (register(base), offset)
        return ("{ EmuPair t; memcpy(&t.a, %s, 4); if (%s) t.b = 1.0f; else memcpy(&t.b, %s + 4, 4); "
                "%s = t; }" % (address, a[2], address, register(a[0])))
    d = register(a[0])
    r = [register(x) for x in a[1:]]
    if op == "ps_merge00":
        return pair(d, r[0] + ".a", r[1] + ".a")
    if op == "ps_merge11":
        return pair(d, r[0] + ".b", r[1] + ".b")
    if op == "ps_mul":
        return pair(d, "%s.a * %s.a" % (r[0], r[1]), "%s.b * %s.b" % (r[0], r[1]))
    if op == "ps_muls0":
        return pair(d, "%s.a * %s.a" % (r[0], r[1]), "%s.b * %s.a" % (r[0], r[1]))
    if op == "ps_add":
        return pair(d, "%s.a + %s.a" % (r[0], r[1]), "%s.b + %s.b" % (r[0], r[1]))
    # (frD, frA, frC, frB: A * C + B; ps_sum0: A.ps0 + B.ps1, C.ps1)
    if op == "ps_madd":
        return pair(d, "fmaf(%s.a, %s.a, %s.a)" % tuple(r), "fmaf(%s.b, %s.b, %s.b)" % tuple(r))
    if op == "ps_madds0":
        return pair(d, "fmaf(%s.a, %s.a, %s.a)" % tuple(r), "fmaf(%s.b, %s.a, %s.b)" % tuple(r))
    if op == "ps_sum0":
        return pair(d, "%s.a + %s.b" % (r[0], r[2]), "%s.b" % r[1])
    if op == "fmadds":
        return pair(d, "fmaf(%s.a, %s.a, %s.a)" % tuple(r), "fmaf(%s.a, %s.a, %s.a)" % tuple(r))
    raise SystemExit("vsh_ps_emulate: no emulation of " + op)


def statement(match):
    lines = match.group(1).split("\n")
    template = [re.match(r'\s*"(.*)\\n\\t"', x).group(1) for x in lines if re.match(r'\s*"', x)]
    sections = [x.strip()[1:].strip() for x in lines if x.strip().startswith(":")]
    code = ["{"]
    outputs = []
    for name, constraint, value in operands(sections[0]):
        code.append("EmuPair e_%s = { 0.0f, 0.0f };" % name)
        outputs.append((name, value))
    for name, constraint, value in operands(sections[1] if len(sections) > 1 else ""):
        if constraint == "f":
            code.append("EmuPair e_%s = { (float)(%s), NAN };" % (name, value))
        elif constraint == "b":
            code.append("const char *e_%s = (const char *)(%s);" % (name, value))
    for line in template:
        op, args = line.split(None, 1)
        code.append(instruction(op, args))
    for name, value in outputs:
        code.append("%s = e_%s.a;" % (value, name))
    code.append("}")
    return "\n\t\t".join(code)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: vsh_ps_emulate.py programs.c emulated.c")
    text = open(sys.argv[1]).read()
    text = text.replace("#if WL_XBOX_VSH_PS", "#if 1")
    text = text.replace('#include "wl/xbox/vsh_native.h"',
                        '#include "wl/xbox/vsh_native.h"\n\ntypedef struct { float a, b; } EmuPair;', 1)
    text, count = re.subn(r"__asm__\(\n(.*?)\);", statement, text, flags=re.S)
    if "__asm__" in text:
        raise SystemExit("vsh_ps_emulate: an asm statement left")
    with open(sys.argv[2], "w") as out:
        out.write(text)


if __name__ == "__main__":
    main()
