#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes tests/xbox/vsh_programs.inc: synthetic NV2A vertex programs for the wl/xbox/vsh tests.

The programs are assembled here from a small text form (one instruction a line, as the comments in
the output show it) into the microcode words wl/xbox/vsh.c decodes: a header word (the instruction
count in its high half) and four words an instruction. Between them they use every MAC and ILU
operation, relative constants, the screen-space conversion's captured clip position, and every
output. tools/xbox_vsh_codegen.c compiles the file at build time and test_xbox_vsh_native.c checks
the compiled functions against the interpreter; test_xbox_vsh.c includes it too.

usage: python3 tests/xbox/vsh_programs.py > tests/xbox/vsh_programs.inc
"""

MAC = {"nop": 0, "mov": 1, "mul": 2, "add": 3, "mad": 4, "dp3": 5, "dph": 6, "dp4": 7, "dst": 8, "min": 9,
       "max": 10, "slt": 11, "sge": 12, "arl": 13}
ILU = {"nop": 0, "mov": 1, "rcp": 2, "rcc": 3, "rsq": 4, "exp": 5, "log": 6, "lit": 7}
OUTPUTS = {"oPos": 0, "oD0": 3, "oD1": 4, "oFog": 5, "oPts": 6, "oB0": 7, "oB1": 8, "oT0": 9, "oT1": 10,
           "oT2": 11, "oT3": 12}
SWIZZLE = "xyzw"


def mask(text):
    if not text:
        return 0xF
    return sum(8 >> SWIZZLE.index(c) for c in text)


def operand(text):
    """(mux, index, negate, swizzle, input or constant, relative) of r<n>, v<n>, c[<n>] or c[a0+<n>]
    (n: c[-96] to c[95]), each with an optional -, and .swizzle (its last letter repeated)"""
    negate = text.startswith("-")
    relative = False
    text = text.lstrip("-")
    name, _, swizzle = text.partition(".")
    swizzle = (swizzle or "xyzw")
    swizzle = (swizzle + swizzle[-1] * 4)[:4]
    swizzle = [SWIZZLE.index(c) for c in swizzle]
    if name.startswith("r"):
        return 1, int(name[1:]), negate, swizzle, None, False
    if name.startswith("v"):
        return 2, 0, negate, swizzle, int(name[1:]), False
    inner = name[2:-1]
    if inner.startswith("a0+"):
        relative = True
        inner = inner[3:]
    return 3, 0, negate, swizzle, ("c", int(inner) + 96), relative


def assemble(line, last):
    """one instruction: a MAC operation ('dp4 dest, a, b') and/or an ILU one, named with an i ('ircc
    dest, c'), joined by ' : '. A destination is r<n> (r12 the position), a0 (ARL's) or an output
    o<name>, with an optional .mask; '+' joins an output and a register. Both units read their
    operands before either writes; with both, the ILU writes r1."""
    mac = ilu = 0
    mac_mask = ilu_mask = output_mask = 0
    temporary = 0
    output_address = output_is_register = output_from_ilu = 0
    none = (0, 0, False, [0, 1, 2, 3], None, False)
    operands = [none, none, none]
    for part in line.split(" : "):
        op, _, rest = part.partition(" ")
        args = [a.strip() for a in rest.split(",")]
        from_ilu = op.startswith("i") and op[1:] in ILU
        sources = [operand(s) for s in args[1:]]
        if from_ilu:
            ilu = ILU[op[1:]]
            operands[2] = sources[0]
        else:
            mac = MAC[op]
            for slot, value in zip([0, 2] if op == "add" else [0, 1, 2], sources):
                operands[slot] = value
        for dest in args[0].split("+"):
            name, _, components = dest.partition(".")
            if name.startswith("o"):
                output_address, output_is_register, output_from_ilu = OUTPUTS[name], 1, int(from_ilu)
                output_mask = mask(components)
            elif name == "a0":
                pass
            elif from_ilu:
                if not mac:
                    temporary = int(name[1:])
                ilu_mask = mask(components)
            else:
                temporary = int(name[1:])
                mac_mask = mask(components)
    input_register = constant = None
    relative = False
    for mux, _, _, _, extra, rel in operands:
        if mux == 2:
            assert input_register in (None, extra), f"{line}: one input register an instruction"
            input_register = extra
        if mux == 3:
            assert constant in (None, extra[1]), f"{line}: one constant an instruction"
            constant = extra[1]
            relative = relative or rel
    input_register = input_register or 0
    constant = constant or 0
    (amux, aindex, aneg, aswz, _, _), (bmux, bindex, bneg, bswz, _, _), (cmux, cindex, cneg, cswz, _, _) = operands
    words = [0, 0, 0, 0]
    words[1] = (ilu << 25 | mac << 21 | constant << 13 | input_register << 9 | int(aneg) << 8 |
                aswz[0] << 6 | aswz[1] << 4 | aswz[2] << 2 | aswz[3])
    words[2] = (aindex << 28 | amux << 26 | int(bneg) << 25 | bswz[0] << 23 | bswz[1] << 21 | bswz[2] << 19 |
                bswz[3] << 17 | bindex << 13 | bmux << 11 | int(cneg) << 10 | cswz[0] << 8 | cswz[1] << 6 |
                cswz[2] << 4 | cswz[3] << 2 | (cindex >> 2) & 3)
    words[3] = ((cindex & 3) << 30 | cmux << 28 | mac_mask << 24 | temporary << 20 | ilu_mask << 16 |
                output_mask << 12 | output_is_register << 11 | output_address << 3 | output_from_ilu << 2 |
                int(relative) << 1 | int(last))
    return words


PROGRAMS = [
    ("a transform: the position by four constant rows, a colour, coordinates", [
        "dp4 oPos.x, v0, c[0]",
        "dp4 oPos.y, v0, c[1]",
        "dp4 oPos.z, v0, c[2]",
        "dp4 oPos.w, v0, c[3]",
        "mov oD0, v3",
        "mul oD1, v3, c[4]",
        "mov oT0, v9",
        "mov r1, c[6]",
        "mad oT1.xy, v9.yx, c[5], r1",
    ]),
    ("to the screen: the clip position captured, then divided by w and moved to the viewport", [
        "dp4 r12.x, v0, c[0]",
        "dp4 r12.y, v0, c[1]",
        "dp4 r12.z, v0, c[2]",
        "dp4 r12.w, v0, c[3]",
        "ircc r1.x, r12.w",
        "mul r12.xyz, r12, r1.x",
        "mov r2, c[-37]",
        "mad oPos.xyz, r12, c[-38], r2",
        "mov oFog.x, r12.z",
    ]),
    ("skinned: a node's rows by v1's index, its weight from v1.y, to the screen", [
        "arl a0.x, v1.x",
        "dp4 r0.x, v0, c[a0+10]",
        "dp4 r0.y, v0, c[a0+11]",
        "dp4 r0.z, v0, c[a0+12]",
        "mov r0.w, c[7].w",
        "mov r2, c[-37]",
        "mul r0.xyz, r0, v1.y",
        "dp4 r12.x, r0, c[0]",
        "dp4 r12.y, r0, c[1]",
        "dp4 r12.z, r0, c[2]",
        "dp4 r12.w, r0, c[3]",
        "ircc r1.x, r12.w",
        "mul r12.xyz, r12, r1.x",
        "mad oPos.xyz, r12, c[-38], r2",
        "dp3 oD0.xyz, v2, c[a0+13]",
    ]),
    ("lighting: a normal's dot products, the specular power, a distance, the comparisons", [
        "dp4 oPos, v0, c[0]",
        "dp3 r0.x, v2, c[8]",
        "max r0.x, r0.x, c[7].x",
        "dph r0.y, v0, c[9]",
        "irsq r1.x, r0.y",
        "dst r2, r0.yyyy, r1.xxxx",
        "slt r3, v3, c[10]",
        "sge r4, v3, c[10]",
        "min r5, v3, -c[11]",
        "mov r6.xy, r0.xy : ilit oD1, r6.xyzw",
        "irsq oT2, -v2.x",
        "add oD0, r3, r4",
        "mul oT0, r2, r5",
        "mad oB0, r0.xxxx, c[12], v3",
        "mov oB1, -v3.wzyx",
        "mov oPts.x, c[13].x",
    ]),
    ("the ILU's others, and the MAC and ILU in one instruction", [
        "dp4 oPos, v0, c[0]",
        "iexp r0, v4.x",
        "ilog r1, v4.y",
        "ircp r2.x, v4.z",
        "mul r3, v4, c[14] : imov r1.zw, v4.wwww",
        "add oT3, r0, r1",
        "mad oT1, r3, r2.x, c[15]",
        "mul r4.xy, v4, v4 : ircc oT0.x+r1, v4.w",
        "mov oD0.xw, r4.xyxy",
        "mov oFog.x, r1.x",
    ]),
    ("two nodes blended by v6's weights (their indices v5 by c[7].w): a prefix a vertex with the nodes and "
     "weights of the vertex before takes from it (tools/xbox_vsh_codegen.c), and an output from v4 within it", [
        "mul r2.xy, v5, c[7].w",
        "arl a0.x, r2.x : imov oD0.w, v4.zzzz",
        "mul r3, v6.xxxx, c[a0+60]",
        "mul r4, v6.xxxx, c[a0+61]",
        "mul r5, v6.xxxx, c[a0+62]",
        "arl a0.x, r2.y",
        "add r6.x, v6.w, -v6.x",
        "mad r7, r6.xxxx, c[a0+60], r3",
        "mad r8, r6.xxxx, c[a0+61], r4",
        "mad r9, r6.xxxx, c[a0+62], r5",
        "dph r10.x, v0, r7",
        "dph r10.y, v0, r8",
        "dph r10.z, v0, r9",
        "dp3 r11.x, v1, r7",
        "dp3 r11.y, v1, r8",
        "dp3 r11.z, v1, r9",
        "dp4 oPos.x, r10, c[0]",
        "dp4 oPos.y, r10, c[1]",
        "dp4 oPos.z, r10, c[2]",
        "dp4 oPos.w, r10, c[3]",
        "dp3 oD0.xyz, r11, c[8]",
        "mul oT0, r9, c[a0+63]",
    ]),
]


def main():
    print("/* SPDX-License-Identifier: MIT */")
    print("/* Synthetic NV2A vertex programs (tests/xbox/vsh_programs.py writes this file: do not edit).")
    print("   Each a header word (its instruction count in the high half) and four words an instruction. */")
    for title, lines in PROGRAMS:
        print(f"/* {title} */")
        print(f"\t0x{len(lines) << 16 | 0x2078:08X},")
        for index, line in enumerate(lines):
            words = assemble(line, index == len(lines) - 1)
            print(f"\t/* {line} */")
            print("\t" + ", ".join(f"0x{w:08X}" for w in words) + ",")


if __name__ == "__main__":
    main()
