/* SPDX-License-Identifier: MIT */
/* Synthetic NV2A vertex programs (tests/xbox/vsh_programs.py writes this file: do not edit).
   Each a header word (its instruction count in the high half) and four words an instruction. */
/* a transform: the position by four constant rows, a colour, coordinates */
	0x00092078,
	/* dp4 oPos.x, v0, c[0] */
	0x00000000, 0x00EC001B, 0x0836186C, 0x00008800,
	/* dp4 oPos.y, v0, c[1] */
	0x00000000, 0x00EC201B, 0x0836186C, 0x00004800,
	/* dp4 oPos.z, v0, c[2] */
	0x00000000, 0x00EC401B, 0x0836186C, 0x00002800,
	/* dp4 oPos.w, v0, c[3] */
	0x00000000, 0x00EC601B, 0x0836186C, 0x00001800,
	/* mov oD0, v3 */
	0x00000000, 0x0020061B, 0x0836006C, 0x0000F818,
	/* mul oD1, v3, c[4] */
	0x00000000, 0x004C861B, 0x0836186C, 0x0000F820,
	/* mov oT0, v9 */
	0x00000000, 0x0020121B, 0x0836006C, 0x0000F848,
	/* mov r1, c[6] */
	0x00000000, 0x002CC01B, 0x0C36006C, 0x0F100000,
	/* mad oT1.xy, v9.yx, c[5], r1 */
	0x00000000, 0x008CB240, 0x0836186C, 0x5000C851,
/* to the screen: the clip position captured, then divided by w and moved to the viewport */
	0x00092078,
	/* dp4 r12.x, v0, c[0] */
	0x00000000, 0x00EC001B, 0x0836186C, 0x08C00000,
	/* dp4 r12.y, v0, c[1] */
	0x00000000, 0x00EC201B, 0x0836186C, 0x04C00000,
	/* dp4 r12.z, v0, c[2] */
	0x00000000, 0x00EC401B, 0x0836186C, 0x02C00000,
	/* dp4 r12.w, v0, c[3] */
	0x00000000, 0x00EC601B, 0x0836186C, 0x01C00000,
	/* ircc r1.x, r12.w */
	0x00000000, 0x0600001B, 0x003603FF, 0x10180000,
	/* mul r12.xyz, r12, r1.x */
	0x00000000, 0x0040001B, 0xC400286C, 0x0EC00000,
	/* mov r2, c[-37] */
	0x00000000, 0x0027601B, 0x0C36006C, 0x0F200000,
	/* mad oPos.xyz, r12, c[-38], r2 */
	0x00000000, 0x0087401B, 0xC436186C, 0x9000E800,
	/* mov oFog.x, r12.z */
	0x00000000, 0x002000AA, 0xC436006C, 0x00008829,
/* skinned: a node's rows by v1's index, its weight from v1.y, to the screen */
	0x000F2078,
	/* arl a0.x, v1.x */
	0x00000000, 0x01A00200, 0x0836006C, 0x00000000,
	/* dp4 r0.x, v0, c[a0+10] */
	0x00000000, 0x00ED401B, 0x0836186C, 0x08000002,
	/* dp4 r0.y, v0, c[a0+11] */
	0x00000000, 0x00ED601B, 0x0836186C, 0x04000002,
	/* dp4 r0.z, v0, c[a0+12] */
	0x00000000, 0x00ED801B, 0x0836186C, 0x02000002,
	/* mov r0.w, c[7].w */
	0x00000000, 0x002CE0FF, 0x0C36006C, 0x01000000,
	/* mov r2, c[-37] */
	0x00000000, 0x0027601B, 0x0C36006C, 0x0F200000,
	/* mul r0.xyz, r0, v1.y */
	0x00000000, 0x0040021B, 0x04AA106C, 0x0E000000,
	/* dp4 r12.x, r0, c[0] */
	0x00000000, 0x00EC001B, 0x0436186C, 0x08C00000,
	/* dp4 r12.y, r0, c[1] */
	0x00000000, 0x00EC201B, 0x0436186C, 0x04C00000,
	/* dp4 r12.z, r0, c[2] */
	0x00000000, 0x00EC401B, 0x0436186C, 0x02C00000,
	/* dp4 r12.w, r0, c[3] */
	0x00000000, 0x00EC601B, 0x0436186C, 0x01C00000,
	/* ircc r1.x, r12.w */
	0x00000000, 0x0600001B, 0x003603FF, 0x10180000,
	/* mul r12.xyz, r12, r1.x */
	0x00000000, 0x0040001B, 0xC400286C, 0x0EC00000,
	/* mad oPos.xyz, r12, c[-38], r2 */
	0x00000000, 0x0087401B, 0xC436186C, 0x9000E800,
	/* dp3 oD0.xyz, v2, c[a0+13] */
	0x00000000, 0x00ADA41B, 0x0836186C, 0x0000E81B,
/* lighting: a normal's dot products, the specular power, a distance, the comparisons */
	0x00102078,
	/* dp4 oPos, v0, c[0] */
	0x00000000, 0x00EC001B, 0x0836186C, 0x0000F800,
	/* dp3 r0.x, v2, c[8] */
	0x00000000, 0x00AD041B, 0x0836186C, 0x08000000,
	/* max r0.x, r0.x, c[7].x */
	0x00000000, 0x014CE000, 0x0400186C, 0x08000000,
	/* dph r0.y, v0, c[9] */
	0x00000000, 0x00CD201B, 0x0836186C, 0x04000000,
	/* irsq r1.x, r0.y */
	0x00000000, 0x0800001B, 0x00360154, 0x10180000,
	/* dst r2, r0.yyyy, r1.xxxx */
	0x00000000, 0x01000055, 0x0400286C, 0x0F200000,
	/* slt r3, v3, c[10] */
	0x00000000, 0x016D461B, 0x0836186C, 0x0F300000,
	/* sge r4, v3, c[10] */
	0x00000000, 0x018D461B, 0x0836186C, 0x0F400000,
	/* min r5, v3, -c[11] */
	0x00000000, 0x012D661B, 0x0A36186C, 0x0F500000,
	/* mov r6.xy, r0.xy : ilit oD1, r6.xyzw */
	0x00000000, 0x0E200015, 0x0436006D, 0x9C60F824,
	/* irsq oT2, -v2.x */
	0x00000000, 0x0800041B, 0x00360400, 0x2000F85C,
	/* add oD0, r3, r4 */
	0x00000000, 0x0060001B, 0x3436006D, 0x1000F818,
	/* mul oT0, r2, r5 */
	0x00000000, 0x0040001B, 0x2436A86C, 0x0000F848,
	/* mad oB0, r0.xxxx, c[12], v3 */
	0x00000000, 0x008D8600, 0x0436186C, 0x2000F838,
	/* mov oB1, -v3.wzyx */
	0x00000000, 0x002007E4, 0x0836006C, 0x0000F840,
	/* mov oPts.x, c[13].x */
	0x00000000, 0x002DA000, 0x0C36006C, 0x00008831,
/* the ILU's others, and the MAC and ILU in one instruction */
	0x000A2078,
	/* dp4 oPos, v0, c[0] */
	0x00000000, 0x00EC001B, 0x0836186C, 0x0000F800,
	/* iexp r0, v4.x */
	0x00000000, 0x0A00081B, 0x00360000, 0x200F0000,
	/* ilog r1, v4.y */
	0x00000000, 0x0C00081B, 0x00360154, 0x201F0000,
	/* ircp r2.x, v4.z */
	0x00000000, 0x0400081B, 0x003602A8, 0x20280000,
	/* mul r3, v4, c[14] : imov r1.zw, v4.wwww */
	0x00000000, 0x024DC81B, 0x08361BFC, 0x2F330000,
	/* add oT3, r0, r1 */
	0x00000000, 0x0060001B, 0x0436006C, 0x5000F860,
	/* mad oT1, r3, r2.x, c[15] */
	0x00000000, 0x008DE01B, 0x3400486C, 0x3000F850,
	/* mul r4.xy, v4, v4 : ircc oT0.x+r1, v4.w */
	0x00000000, 0x0640081B, 0x083613FC, 0x2C4F884C,
	/* mov oD0.xw, r4.xyxy */
	0x00000000, 0x00200011, 0x4436006C, 0x00009818,
	/* mov oFog.x, r1.x */
	0x00000000, 0x00200000, 0x1436006C, 0x00008829,
/* two nodes blended by v6's weights (their indices v5 by c[7].w): a prefix a vertex with the nodes and weights of the vertex before takes from it (tools/xbox_vsh_codegen.c), and an output from v4 within it */
	0x00162078,
	/* mul r2.xy, v5, c[7].w */
	0x00000000, 0x004CEA1B, 0x09FE186C, 0x0C200000,
	/* arl a0.x, r2.x : imov oD0.w, v4.zzzz */
	0x00000000, 0x03A00800, 0x243602A8, 0x2000181C,
	/* mul r3, v6.xxxx, c[a0+60] */
	0x00000000, 0x00538C00, 0x0836186C, 0x0F300002,
	/* mul r4, v6.xxxx, c[a0+61] */
	0x00000000, 0x0053AC00, 0x0836186C, 0x0F400002,
	/* mul r5, v6.xxxx, c[a0+62] */
	0x00000000, 0x0053CC00, 0x0836186C, 0x0F500002,
	/* arl a0.x, r2.y */
	0x00000000, 0x01A00055, 0x2436006C, 0x00000000,
	/* add r6.x, v6.w, -v6.x */
	0x00000000, 0x00600CFF, 0x08360400, 0x28600000,
	/* mad r7, r6.xxxx, c[a0+60], r3 */
	0x00000000, 0x00938000, 0x6436186C, 0xDF700002,
	/* mad r8, r6.xxxx, c[a0+61], r4 */
	0x00000000, 0x0093A000, 0x6436186D, 0x1F800002,
	/* mad r9, r6.xxxx, c[a0+62], r5 */
	0x00000000, 0x0093C000, 0x6436186D, 0x5F900002,
	/* dph r10.x, v0, r7 */
	0x00000000, 0x00C0001B, 0x0836E86C, 0x08A00000,
	/* dph r10.y, v0, r8 */
	0x00000000, 0x00C0001B, 0x0837086C, 0x04A00000,
	/* dph r10.z, v0, r9 */
	0x00000000, 0x00C0001B, 0x0837286C, 0x02A00000,
	/* dp3 r11.x, v1, r7 */
	0x00000000, 0x00A0021B, 0x0836E86C, 0x08B00000,
	/* dp3 r11.y, v1, r8 */
	0x00000000, 0x00A0021B, 0x0837086C, 0x04B00000,
	/* dp3 r11.z, v1, r9 */
	0x00000000, 0x00A0021B, 0x0837286C, 0x02B00000,
	/* dp4 oPos.x, r10, c[0] */
	0x00000000, 0x00EC001B, 0xA436186C, 0x00008800,
	/* dp4 oPos.y, r10, c[1] */
	0x00000000, 0x00EC201B, 0xA436186C, 0x00004800,
	/* dp4 oPos.z, r10, c[2] */
	0x00000000, 0x00EC401B, 0xA436186C, 0x00002800,
	/* dp4 oPos.w, r10, c[3] */
	0x00000000, 0x00EC601B, 0xA436186C, 0x00001800,
	/* dp3 oD0.xyz, r11, c[8] */
	0x00000000, 0x00AD001B, 0xB436186C, 0x0000E818,
	/* mul oT0, r9, c[a0+63] */
	0x00000000, 0x0053E01B, 0x9436186C, 0x0000F84B,
/* the exponent and logarithm, all their lanes and one at a time */
	0x000A2078,
	/* dp4 oPos, v0, c[0] */
	0x00000000, 0x00EC001B, 0x0836186C, 0x0000F800,
	/* ilog r5, v4.x */
	0x00000000, 0x0C00081B, 0x00360000, 0x205F0000,
	/* iexp r6, v4.y */
	0x00000000, 0x0A00081B, 0x00360154, 0x206F0000,
	/* ilog r7.z, -v4.z */
	0x00000000, 0x0C00081B, 0x003606A8, 0x20720000,
	/* iexp r8.x, v4.w */
	0x00000000, 0x0A00081B, 0x003603FC, 0x20880000,
	/* iexp r8.z, -v4.x */
	0x00000000, 0x0A00081B, 0x00360400, 0x20820000,
	/* mov oT0, r5 */
	0x00000000, 0x0020001B, 0x5436006C, 0x0000F848,
	/* mov oT1, r6 */
	0x00000000, 0x0020001B, 0x6436006C, 0x0000F850,
	/* mov oT2, r8 */
	0x00000000, 0x0020001B, 0x8436006C, 0x0000F858,
	/* mov oFog.x, r7.z */
	0x00000000, 0x002000AA, 0x7436006C, 0x00008829,
