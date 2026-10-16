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

The fused batches' (tools/xbox_vsh_pairs.c's pr_fuse): the GQRs are variables (mtspr, mfspr);
a quantized load reads u8 or s16 (the s16 in the host's order, as sth wrote it), a quantized
u8 store cuts the value down and clamps it (a NaN 0) and writes its bytes where the Gekko's
would land in the word (a little-endian host's word holds them reversed); lwbrx and lhbrx read
little-endian; an lwz whose displacement names %[ptr] reads a pointer (the host's may be 8
bytes).
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


# ---- whole programs (tools/xbox_vsh_pairs.c): an asm volatile statement with its own loop ----
#
# Floating-point registers are EmuReg (ps0 a double, ps1, and fctiwz's word); a lane the Gekko
# leaves undefined, or that the code must not read (ps1 after a double operation), is a NaN. The
# single-precision operations (lfs, fadds, fmadds, ...) write both lanes, as in paired-single
# mode. frsqrte gives Broadway's table estimate and stfs / psq_st store a double as Broadway does,
# its mantissa cut, not rounded (emu_frsqrte, emu_single: Dolphin's ApproximateReciprocalSquareRoot
# and ConvertToSingle), so code that stores a double result without frsp shows on the host as
# it would on the Wii.

def emu_expression(text, immediates):
    return re.sub(r"%\[(\w+)\]", lambda m: "(long)(%s)" % immediates[m.group(1)], text)


def emu_gpr(text):
    text = text.strip()
    if text == "0":
        return "r0"
    return "e_" + re.match(r"%\[(\w+)\]$", text).group(1)


def emu_memory(text, immediates):
    m = re.match(r"(.*)\((%\[\w+\]|0)\)$", text.strip())
    offset = emu_expression(m.group(1), immediates) if m.group(1).strip() else "0"
    return "((char *)(%s) + (%s))" % (emu_gpr(m.group(2)), offset)


def emu_label(text):
    return "L_" + re.sub(r"\W", "_", text.strip().replace("%=", ""))


def emu_f(text):
    return "f[%d]" % int(text)


def emu_line(line, immediates):
    line = line.strip()
    if line.endswith(":"):
        return emu_label(line[:-1]) + ":;"
    op, _, rest = line.partition(" ")
    a = [x.strip() for x in rest.split(",")] if rest else []
    F = emu_f

    def fa(r, lane):
        return "(float)%s.%s" % (F(r), lane)

    if op == "psq_l":
        return "f_psq_lq(&%s, %s, %s, %s);" % (F(a[0]), emu_memory(a[1], immediates), a[2], a[3])
    if op == "psq_st":
        return "f_psq_stq(&%s, %s, %s, %s);" % (F(a[0]), emu_memory(a[1], immediates), a[2], a[3])
    if op == "lfs":
        return "{ float t; memcpy(&t, %s, 4); f_set(&%s, t); }" % (emu_memory(a[1], immediates), F(a[0]))
    if op == "stfs":
        return "{ uint32_t t = emu_single(%s.a, 0); memcpy(%s, &t, 4); }" % (F(a[0]), emu_memory(a[1], immediates))
    if op == "stfiwx":
        return "memcpy((char *)%s, &%s.i, 4);" % (emu_gpr(a[2]), F(a[0]))
    if op == "lwz" and "%[ptr]" in a[1]:
        return "{ void *t; memcpy(&t, %s, sizeof t); %s = (intptr_t)t; }" % (emu_memory(a[1], immediates),
                                                                           emu_gpr(a[0]))
    if op == "lwz":
        return "{ int32_t t; memcpy(&t, %s, 4); %s = t; }" % (emu_memory(a[1], immediates), emu_gpr(a[0]))
    if op in ("lwbrx", "lhbrx", "lfsx"):
        base = "0" if a[1] == "0" else "(char *)" + emu_gpr(a[1])
        address = "((char *)%s + %s)" % (base, emu_gpr(a[2]))
        if op == "lwbrx":
            return "%s = (int32_t)emu_le32(%s);" % (emu_gpr(a[0]), address)
        if op == "lhbrx":
            return "%s = (intptr_t)emu_le16(%s);" % (emu_gpr(a[0]), address)
        return "{ float t; memcpy(&t, %s, 4); f_set(&%s, t); }" % (address, F(a[0]))
    if op == "sth":
        return "{ uint16_t t = (uint16_t)%s; memcpy(%s, &t, 2); }" % (emu_gpr(a[0]), emu_memory(a[1], immediates))
    if op == "lfd":
        return "{ double t; memcpy(&t, %s, 8); f_double(&%s, t); }" % (emu_memory(a[1], immediates), F(a[0]))
    if op == "mullw":
        return "%s = (int32_t)((uint32_t)%s * (uint32_t)%s);" % (emu_gpr(a[0]), emu_gpr(a[1]), emu_gpr(a[2]))
    if op == "rlwinm":
        return "%s = cr_insert(0u, (uint32_t)%s, %s, %s, %s);" % (emu_gpr(a[0]), emu_gpr(a[1]), a[2], a[3], a[4])
    if op == "ori":
        return "%s = (intptr_t)(int32_t)((uint32_t)%s | %s);" % (emu_gpr(a[0]), emu_gpr(a[1]), a[2])
    if op == "oris":
        return "%s = (intptr_t)(int32_t)((uint32_t)%s | ((uint32_t)%s << 16));" % (
            emu_gpr(a[0]), emu_gpr(a[1]), a[2])
    if op == "mr":
        return "%s = %s;" % (emu_gpr(a[0]), emu_gpr(a[1]))
    if op == "cmpwi":
        return "cr_compare((double)(int32_t)%s, (double)%s);" % (emu_gpr(a[0]), a[1])
    if op == "mfspr":
        return "%s = (intptr_t)emu_gqr[%d];" % (emu_gpr(a[0]), int(a[1]) - 912)
    if op == "mtspr":
        return "emu_gqr[%d] = (uint32_t)%s;" % (int(a[0]) - 912, emu_gpr(a[1]))
    if op == "isync":
        return ";"
    if op == "stw":
        return "{ int32_t t = (int32_t)%s; memcpy(%s, &t, 4); }" % (emu_gpr(a[0]),
                                                                   emu_memory(a[1], immediates))
    if op in ("ps_mr", "ps_neg"):
        sign = "-" if op == "ps_neg" else ""
        return "{ EmuReg t = %s; t.a = %st.a; t.b = %st.b; %s = t; }" % (F(a[1]), sign, sign, F(a[0]))
    m = re.match(r"ps_merge([01])([01])$", op)
    if m:
        return "f_pair(&%s, %s, %s);" % (F(a[0]), fa(a[1], "ab"[int(m.group(1))]),
                                         fa(a[2], "ab"[int(m.group(2))]))
    if op in ("ps_mul", "ps_add", "ps_sub"):
        sym = {"ps_mul": "*", "ps_add": "+", "ps_sub": "-"}[op]
        return "f_pair(&%s, %s %s %s, %s %s %s);" % (F(a[0]), fa(a[1], "a"), sym, fa(a[2], "a"),
                                                     fa(a[1], "b"), sym, fa(a[2], "b"))
    m = re.match(r"ps_muls([01])$", op)
    if m:
        lane = "ab"[int(m.group(1))]
        return "f_pair(&%s, %s * %s, %s * %s);" % (F(a[0]), fa(a[1], "a"), fa(a[2], lane),
                                                   fa(a[1], "b"), fa(a[2], lane))
    # (frD, frA, frC, frB: A * C + B)
    fused = {"ps_madd": ("", ""), "ps_msub": ("", "-"), "ps_nmadd": ("-", ""), "ps_nmsub": ("-", "-")}
    if op in fused:
        n, nb = fused[op]
        return "f_pair(&%s, %sfmaf(%s, %s, %s%s), %sfmaf(%s, %s, %s%s));" % (
            F(a[0]), n, fa(a[1], "a"), fa(a[2], "a"), nb, fa(a[3], "a"),
            n, fa(a[1], "b"), fa(a[2], "b"), nb, fa(a[3], "b"))
    m = re.match(r"ps_madds([01])$", op)
    if m:
        lane = "ab"[int(m.group(1))]
        return "f_pair(&%s, fmaf(%s, %s, %s), fmaf(%s, %s, %s));" % (
            F(a[0]), fa(a[1], "a"), fa(a[2], lane), fa(a[3], "a"), fa(a[1], "b"), fa(a[2], lane),
            fa(a[3], "b"))
    if op == "ps_sum0":
        return "f_pair(&%s, %s + %s, %s);" % (F(a[0]), fa(a[1], "a"), fa(a[3], "b"), fa(a[2], "b"))
    if op == "ps_sum1":
        return "f_pair(&%s, %s, %s + %s);" % (F(a[0]), fa(a[2], "a"), fa(a[1], "a"), fa(a[3], "b"))
    if op == "ps_sel":
        return "f_pair(&%s, %s.a >= 0 ? %s : %s, %s.b >= 0 ? %s : %s);" % (
            F(a[0]), F(a[1]), fa(a[2], "a"), fa(a[3], "a"), F(a[1]), fa(a[2], "b"), fa(a[3], "b"))
    if op == "ps_div":
        return "f_pair(&%s, %s / %s, %s / %s);" % (F(a[0]), fa(a[1], "a"), fa(a[2], "a"), fa(a[1], "b"),
                                                   fa(a[2], "b"))
    if op in ("fadd", "fsub"):
        return "f_double(&%s, %s.a %s %s.a);" % (F(a[0]), F(a[1]), "+" if op == "fadd" else "-", F(a[2]))
    if op in ("fadds", "fsubs", "fdivs", "fmuls"):
        sym = {"fadds": "+", "fsubs": "-", "fdivs": "/", "fmuls": "*"}[op]
        return "f_set(&%s, %s %s %s);" % (F(a[0]), fa(a[1], "a"), sym, fa(a[2], "a"))
    if op in ("fmadds", "fnmadds"):
        return "f_set(&%s, %sfmaf(%s, %s, %s));" % (F(a[0]), "-" if op == "fnmadds" else "", fa(a[1], "a"),
                                                     fa(a[2], "a"), fa(a[3], "a"))
    if op == "fnmsubs":
        return "f_set(&%s, -fmaf(%s, %s, -%s));" % (F(a[0]), fa(a[1], "a"), fa(a[2], "a"), fa(a[3], "a"))
    if op == "fabs":
        return "f_double(&%s, fabs(%s.a));" % (F(a[0]), F(a[1]))
    if op == "fmr":
        return "f_double(&%s, %s.a);" % (F(a[0]), F(a[1]))
    if op == "fsel":
        return "f_double(&%s, %s.a >= 0 ? %s.a : %s.a);" % (F(a[0]), F(a[1]), F(a[2]), F(a[3]))
    if op == "frsqrte":
        return "f_double(&%s, emu_frsqrte(%s.a));" % (F(a[0]), F(a[1]))
    if op == "fmul":
        return "f_double(&%s, %s.a * %s.a);" % (F(a[0]), F(a[1]), F(a[2]))
    if op == "fnmsub":
        return "f_double(&%s, -fma(%s.a, %s.a, -%s.a));" % (F(a[0]), F(a[1]), F(a[2]), F(a[3]))
    if op == "frsp":
        return "f_double(&%s, (float)%s.a);" % (F(a[0]), F(a[1]))
    if op == "fctiwz":
        return "f_word(&%s, %s.a);" % (F(a[0]), F(a[1]))
    if op == "fcmpu":
        return "cr_compare(%s.a, %s.a);" % (F(a[1]), F(a[2]))
    if op == "li":
        return "%s = %s;" % (emu_gpr(a[0]), a[1])
    if op == "lis":
        return "%s = (intptr_t)(int32_t)((uint32_t)%s << 16);" % (emu_gpr(a[0]), a[1])
    if op == "addi":
        return "%s = %s + (%s);" % (emu_gpr(a[0]), emu_gpr(a[1]), emu_expression(a[2], immediates))
    if op == "add":
        return "%s = %s + %s;" % (emu_gpr(a[0]), emu_gpr(a[1]), emu_gpr(a[2]))
    if op == "andc":
        return "%s = %s & ~%s;" % (emu_gpr(a[0]), emu_gpr(a[1]), emu_gpr(a[2]))
    if op == "slwi":
        return "%s = (intptr_t)(int32_t)((uint32_t)%s << %s);" % (emu_gpr(a[0]), emu_gpr(a[1]), a[2])
    if op == "srawi":
        return "%s = (int32_t)%s >> %s;" % (emu_gpr(a[0]), emu_gpr(a[1]), a[2])
    if op == "andi.":
        return "%s = %s & %s; cr_compare((double)%s, 0.0);" % (emu_gpr(a[0]), emu_gpr(a[1]), a[2],
                                                               emu_gpr(a[0]))
    if op == "cmpw":
        return "cr_compare((double)(int32_t)%s, (double)(int32_t)%s);" % (emu_gpr(a[0]), emu_gpr(a[1]))
    if op == "cmplwi":
        return "cr_compare((double)(uint32_t)%s, (double)(uint32_t)%s);" % (emu_gpr(a[0]), a[1])
    if op == "neg":
        return "%s = (intptr_t)(int32_t)(0u - (uint32_t)%s);" % (emu_gpr(a[0]), emu_gpr(a[1]))
    if op == "srwi":
        return "%s = (intptr_t)((uint32_t)%s >> %s);" % (emu_gpr(a[0]), emu_gpr(a[1]), a[2])
    if op == "rlwimi":
        return "%s = cr_insert((uint32_t)%s, (uint32_t)%s, %s, %s, %s);" % (
            emu_gpr(a[0]), emu_gpr(a[0]), emu_gpr(a[1]), a[2], a[3], a[4])
    if op == "mtcrf":
        return "cr_fields(%s, (uint32_t)%s);" % (a[0], emu_gpr(a[1]))
    if op in ("bf", "bt"):
        return "if (%scr_bit(%s)) goto %s;" % ("!" if op == "bf" else "", a[0], emu_label(a[1]))
    if op == "mtctr":
        return "ctr = (uint32_t)%s;" % emu_gpr(a[0])
    if op == "bdnz":
        return "if (--ctr) goto %s;" % emu_label(a[0])
    branches = {"b": "1", "beq": "cr_eq", "bne": "!cr_eq", "bgt": "cr_gt", "bng": "!cr_gt",
                "blt": "cr_lt", "bnl": "!cr_lt", "ble": "(cr_lt || cr_eq)", "bun": "cr_un",
                "bnu": "!cr_un"}
    if op in branches:
        return "if (%s) goto %s;" % (branches[op], emu_label(a[0]))
    raise SystemExit("vsh_ps_emulate: no emulation of " + op)


def whole(match):
    lines = match.group(1).split("\n")
    template = [re.match(r'\s*"(.*)\\n\\t"', x).group(1) for x in lines if re.match(r'\s*"', x)]
    sections, current = [], None
    for x in lines:
        if x.strip().startswith(":"):
            current = [x.strip()[1:].strip()]
            sections.append(current)
        elif current is not None and x.strip():
            current.append(x.strip())
    outputs = operands(" ".join(sections[0]))
    inputs = operands(" ".join(sections[1]))
    immediates = {}
    code = ["{", "EmuReg f[32]; intptr_t r0 = 0; uint32_t ctr = 0; int k;",
            "for (k = 0; k < 32; k++) { f[k].a = f[k].b = NAN; f[k].i = 0; }"]
    for name, constraint, value in outputs:
        code.append("intptr_t e_%s = (intptr_t)(%s);" % (name, value if "+" in constraint else "0"))
    for name, constraint, value in inputs:
        if constraint == "i":
            immediates[name] = value
        else:
            code.append("intptr_t e_%s = (intptr_t)(%s);" % (name, value))
    for line in template:
        code.append(emu_line(line, immediates))
    for name, constraint, value in outputs:
        code.append("%s = (__typeof__(%s))e_%s;" % (value, value, name))
    code.append("(void)r0;")
    code.append("}")
    return "\n\t\t".join(code)


EMU_HEADER = r"""
#include <stdio.h>
#include <stdlib.h>
typedef struct { double a, b; int32_t i; } EmuReg;
static int cr_lt, cr_gt, cr_eq, cr_un;
static void cr_compare(double x, double y) {
    cr_un = isnan(x) || isnan(y);
    cr_lt = !cr_un && x < y;
    cr_gt = !cr_un && x > y;
    cr_eq = !cr_un && x == y;
}
static uint32_t cr_rest; /* cr1 to cr7: bit 4 + k at 1 << (27 - k) */
static void cr_fields(unsigned mask, uint32_t value) {
    for (int field = 1; field < 8; field++) {
        if (mask & (0x80u >> field)) {
            uint32_t bits = 0xf0000000u >> (4 * field);
            cr_rest = (cr_rest & ~bits) | (value & bits);
        }
    }
}
static int cr_bit(int bit) {
    if (bit < 4)
        return bit == 0 ? cr_lt : bit == 1 ? cr_gt : bit == 2 ? cr_eq : cr_un;
    return (int)((cr_rest >> (31 - bit)) & 1);
}
static uint32_t cr_insert(uint32_t a, uint32_t s, int shift, int first, int last) {
    uint32_t rotated = shift ? (s << shift) | (s >> (32 - shift)) : s, mask = 0;
    for (int bit = first; bit <= last; bit++)
        mask |= 0x80000000u >> bit;
    return (a & ~mask) | (rotated & mask);
}
static void f_set(EmuReg *r, float x) { r->a = r->b = x; }
static void f_pair(EmuReg *r, float x, float y) { r->a = x; r->b = y; }
static void f_double(EmuReg *r, double x) { r->a = x; r->b = NAN; }
static void f_word(EmuReg *r, double x) {
    r->a = r->b = NAN;
    r->i = isnan(x) ? INT32_MIN : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : (int32_t)x;
}
static void f_psq_l(EmuReg *r, const char *p, int one) {
    float x, y = 1.0f;
    memcpy(&x, p, 4);
    if (!one)
        memcpy(&y, p + 4, 4);
    f_pair(r, x, y);
}
/* a double's single as Broadway's stfs (flush 0: psq_st's) writes it: the mantissa cut, not
rounded (Dolphin's ConvertToSingle, ConvertToSingleFTZ) */
static uint32_t emu_single(double value, int flush) {
    uint64_t x;
    uint32_t e;
    memcpy(&x, &value, 8);
    e = (uint32_t)(x >> 52) & 0x7ffu;
    if (e > 896 || (x & ~(1ull << 63)) == 0 || (!flush && e < 874))
        return (uint32_t)(((x >> 32) & 0xc0000000u) | ((x >> 29) & 0x3fffffffu));
    if (flush)
        return (uint32_t)(x >> 32) & 0x80000000u;
    {
        uint32_t t = (uint32_t)(0x80000000u | ((x & 0xfffffffffffffull) >> 21));
        return (t >> (905 - e)) | ((uint32_t)(x >> 32) & 0x80000000u);
    }
}
/* frsqrte: Broadway's estimate (Dolphin's ApproximateReciprocalSquareRoot) */
static double emu_frsqrte(double value) {
    static const int32_t base[32] = {
        0x1a7e800, 0x17cb800, 0x1552800, 0x130c000, 0x10f2000, 0x0eff000, 0x0d2e000, 0x0b7c000,
        0x09e5000, 0x0867000, 0x06ff000, 0x05ab800, 0x046a000, 0x0339800, 0x0218800, 0x0105800,
        0x3ffa000, 0x3c29000, 0x38aa000, 0x3572000, 0x3279000, 0x2fb7000, 0x2d26000, 0x2ac0000,
        0x2881000, 0x2665000, 0x2468000, 0x2287000, 0x20c1000, 0x1f12000, 0x1d79000, 0x1bf4000};
    static const int32_t dec[32] = {
        0x568, 0x4f3, 0x48d, 0x435, 0x3e7, 0x3a2, 0x365, 0x32e, 0x2fc, 0x2d0, 0x2a8, 0x283, 0x261, 0x243, 0x226, 0x20b,
        0x7a4, 0x700, 0x670, 0x5f2, 0x584, 0x524, 0x4cc, 0x47e, 0x43a, 0x3fa, 0x3c2, 0x38e, 0x35e, 0x332, 0x30a, 0x2e6};
    int64_t integral, mantissa, exponent, low;
    int i;
    memcpy(&integral, &value, 8);
    mantissa = integral & ((1ll << 52) - 1);
    exponent = integral & (0x7ffll << 52);
    if (mantissa == 0 && exponent == 0)
        return integral < 0 ? -INFINITY : INFINITY;
    if (exponent == (0x7ffll << 52))
        return mantissa != 0 ? value + 0.0 : integral < 0 ? NAN : 0.0;
    if (integral < 0)
        return NAN;
    if (exponent == 0) {
        do {
            exponent -= 1ll << 52;
            mantissa <<= 1;
        } while (!(mantissa & (1ll << 52)));
        mantissa &= (1ll << 52) - 1;
        exponent += 1ll << 52;
    }
    low = exponent & (1ll << 52);
    exponent = ((0x3ffll << 52) - ((exponent - (0x3fell << 52)) / 2)) & (0x7ffll << 52);
    i = (int)((low | mantissa) >> 37);
    integral = exponent | (int64_t)(base[i / 2048] - dec[i / 2048] * (i % 2048)) << 26;
    memcpy(&value, &integral, 8);
    return value;
}
static void f_psq_st(const EmuReg *r, char *p, int one) {
    uint32_t x = emu_single(r->a, 1), y = emu_single(r->b, 1);
    memcpy(p, &x, 4);
    if (!one)
        memcpy(p + 4, &y, 4);
}
static uint32_t emu_gqr[8];
static uint32_t emu_le32(const char *p) {
    const unsigned char *b = (const unsigned char *)p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint32_t emu_le16(const char *p) {
    const unsigned char *b = (const unsigned char *)p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8;
}
static float emu_dequantize(const char *p, unsigned type) {
    switch (type) {
    case 4: return (float)*(const unsigned char *)p;
    case 6: return (float)*(const signed char *)p;
    case 5: { uint16_t v; memcpy(&v, p, 2); return (float)v; }
    case 7: { int16_t v; memcpy(&v, p, 2); return (float)v; }
    default: { float v; memcpy(&v, p, 4); return v; }
    }
}
static unsigned emu_size(unsigned type) {
    return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
}
static void f_psq_lq(EmuReg *r, const char *p, int one, int gqr) {
    unsigned type = (emu_gqr[gqr] >> 16) & 7;
    if (gqr == 0) {
        f_psq_l(r, p, one);
        return;
    }
    f_pair(r, emu_dequantize(p, type), one ? 1.0f : emu_dequantize(p + emu_size(type), type));
}
/* a u8 store's byte at p, where the Gekko's would be in its word */
static void emu_store_byte(char *p, unsigned char b) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    p = (char *)((uintptr_t)p ^ 3u);
#endif
    *(unsigned char *)p = b;
}
/* a NaN quantized to 255, as Dolphin stores it (the fused colours select it away first) */
static unsigned char emu_u8(double x) {
    return x != x ? 255 : !(x > 0.0) ? 0 : x >= 255.0 ? 255 : (unsigned char)(int)x;
}
static void f_psq_stq(const EmuReg *r, char *p, int one, int gqr) {
    unsigned type = emu_gqr[gqr] & 7;
    if (gqr == 0) {
        f_psq_st(r, p, one);
        return;
    }
    if (type != 4) {
        fprintf(stderr, "vsh_ps_emulate: a quantized store of type %u\n", type);
        abort();
    }
    emu_store_byte(p, emu_u8((float)r->a));
    if (!one)
        emu_store_byte(p + 1, emu_u8((float)r->b));
}
"""


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: vsh_ps_emulate.py programs.c emulated.c")
    text = open(sys.argv[1]).read()
    text = text.replace("#if WL_XBOX_VSH_PS", "#if 1")
    text = re.sub(r"WL_XBOX_VSH_FUSED_FORMS\((\w+), (\d+)\)", r"\1, \2", text)
    text = text.replace('#include "wl/xbox/vsh_native.h"',
                        '#include "wl/xbox/vsh_native.h"\n\ntypedef struct { float a, b; } EmuPair;\n' +
                        EMU_HEADER, 1)
    text = re.sub(r"__asm__ volatile\(\n(.*?)\);", whole, text, flags=re.S)
    text, count = re.subn(r"__asm__\(\n(.*?)\);", statement, text, flags=re.S)
    if "__asm__" in text:
        raise SystemExit("vsh_ps_emulate: an asm statement left")
    with open(sys.argv[2], "w") as out:
        out.write(text)


if __name__ == "__main__":
    main()
