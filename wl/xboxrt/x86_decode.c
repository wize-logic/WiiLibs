// SPDX-License-Identifier: MIT
// Derived from xboxrecomp (https://github.com/sp00nznet/xboxrecomp), MIT, Copyright (c) 2026
// sp00nz and contributors: src/platform/mmio_decode.h (prefix scan, ModRM length, the mov/test/
// cmp/or/and/movzx cases). See x86_decode.h for what was added. Encodings: Intel 64 and IA-32
// Architectures Software Developer's Manual, volume 2.
#include <string.h>

#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/x86_decode.h"

typedef struct {
    const uint8_t *ip;
    WlXbX86Regs *r;
    uint32_t va;
    unsigned prefix_len;
    bool has_rex, rex_w, rex_r, rex_x, rex_b;
    bool p66, pf2, pf3;
    unsigned modrm_len; // ModRM + SIB + displacement
    unsigned reg;       // ModRM.reg (with REX.R)
    unsigned mod;
    const char *what;
} Insn;

static uint64_t mask_of(unsigned size) {
    return size >= 8 ? ~0ull : ((1ull << (size * 8u)) - 1u);
}

// ModRM (+ SIB + displacement) length; the memory operand's address is the fault's, so only the
// length matters.
static unsigned modrm_len(const uint8_t *m) {
    unsigned mod = (m[0] >> 6) & 3u, rm = m[0] & 7u, len = 1;
    if (mod == 3) {
        return 1;
    }
    if (rm == 4) {
        len++; // SIB
        if (mod == 0 && (m[1] & 7u) == 5u) {
            len += 4; // SIB base none: disp32
        }
    }
    if (mod == 0 && rm == 5) {
        len += 4; // rip-relative disp32
    } else if (mod == 1) {
        len += 1;
    } else if (mod == 2) {
        len += 4;
    }
    return len;
}

// Register operand reads and writes of `size` bytes, with the legacy high-byte registers.
static uint64_t reg_get(const Insn *in, unsigned reg, unsigned size) {
    if (size == 1 && !in->has_rex && reg >= 4 && reg < 8) {
        return (in->r->gpr[reg - 4u] >> 8) & 0xFFu; // ah ch dh bh
    }
    return in->r->gpr[reg] & mask_of(size);
}

static void reg_set(Insn *in, unsigned reg, unsigned size, uint64_t v) {
    uint64_t *g = &in->r->gpr[reg];
    if (size == 1 && !in->has_rex && reg >= 4 && reg < 8) {
        g = &in->r->gpr[reg - 4u];
        *g = (*g & ~0xFF00ull) | ((v & 0xFFu) << 8);
    } else if (size == 1) {
        *g = (*g & ~0xFFull) | (v & 0xFFu);
    } else if (size == 2) {
        *g = (*g & ~0xFFFFull) | (v & 0xFFFFu);
    } else if (size == 4) {
        *g = v & 0xFFFFFFFFull; // a 32-bit write clears the upper half
    } else {
        *g = v;
    }
}

static bool mem_read(Insn *in, unsigned size, uint64_t *v) {
    if (!wl_xb_mmio_read(in->va, size, v)) {
        in->what = "no device at the address";
        return false;
    }
    *v &= mask_of(size);
    return true;
}

static bool mem_write(Insn *in, unsigned size, uint64_t v) {
    if (!wl_xb_mmio_write(in->va, v & mask_of(size), size)) {
        in->what = "no device at the address";
        return false;
    }
    return true;
}

static bool parity_even(uint64_t v) {
    uint8_t b = (uint8_t)v;
    b ^= (uint8_t)(b >> 4);
    b ^= (uint8_t)(b >> 2);
    b ^= (uint8_t)(b >> 1);
    return (b & 1u) == 0;
}

// The arithmetic flags of `result` (sign, zero, parity) plus the carry and overflow given.
static void set_flags(Insn *in, uint64_t result, unsigned size, bool cf, bool of, bool af, bool keep_cf) {
    uint64_t m = mask_of(size);
    uint64_t f = in->r->rflags;
    uint64_t keep = keep_cf ? (f & WL_XB_X86_CF) : 0;
    f &= ~(uint64_t)(WL_XB_X86_CF | WL_XB_X86_PF | WL_XB_X86_AF | WL_XB_X86_ZF | WL_XB_X86_SF | WL_XB_X86_OF);
    result &= m;
    if (result == 0) {
        f |= WL_XB_X86_ZF;
    }
    if ((result >> (size * 8u - 1u)) & 1u) {
        f |= WL_XB_X86_SF;
    }
    if (parity_even(result)) {
        f |= WL_XB_X86_PF;
    }
    if (keep_cf) {
        f |= keep;
    } else if (cf) {
        f |= WL_XB_X86_CF;
    }
    if (of) {
        f |= WL_XB_X86_OF;
    }
    if (af) {
        f |= WL_XB_X86_AF;
    }
    in->r->rflags = f;
}

enum { OP_ADD = 0, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };

// d = d op s, with flags; returns the result (the caller decides whether to store it).
static uint64_t alu(Insn *in, unsigned op, uint64_t d, uint64_t s, unsigned size) {
    uint64_t m = mask_of(size);
    uint64_t sign = 1ull << (size * 8u - 1u);
    bool carry_in = (in->r->rflags & WL_XB_X86_CF) != 0;
    uint64_t r;
    d &= m;
    s &= m;
    switch (op) {
    case OP_ADD:
    case OP_ADC: {
        uint64_t c = (op == OP_ADC && carry_in) ? 1u : 0u;
        r = (d + s + c) & m;
        bool cf = size == 8 ? (r < d || (c && r == d)) : ((d + s + c) > m);
        bool of = ((~(d ^ s) & (d ^ r)) & sign) != 0;
        set_flags(in, r, size, cf, of, ((d ^ s ^ r) & 0x10u) != 0, false);
        return r;
    }
    case OP_SUB:
    case OP_SBB:
    case OP_CMP: {
        uint64_t c = (op == OP_SBB && carry_in) ? 1u : 0u;
        r = (d - s - c) & m;
        bool cf = d < s || (c && d == s);
        bool of = (((d ^ s) & (d ^ r)) & sign) != 0;
        set_flags(in, r, size, cf, of, ((d ^ s ^ r) & 0x10u) != 0, false);
        return r;
    }
    case OP_AND:
        r = d & s;
        break;
    case OP_OR:
        r = d | s;
        break;
    default:
        r = d ^ s;
        break;
    }
    set_flags(in, r, size, false, false, false, false);
    return r;
}

static uint64_t imm(const uint8_t *p, unsigned bytes, unsigned size) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    if (bytes < size) { // sign-extend to the operand size
        uint64_t sign = 1ull << (bytes * 8u - 1u);
        if (v & sign) {
            v |= ~((sign << 1) - 1u);
        }
    }
    return v & mask_of(size);
}

// The XMM registers hold little-endian lanes whatever the host's byte order.
static void put_le(uint8_t *dst, uint64_t v, unsigned size) {
    for (unsigned i = 0; i < size; i++) {
        dst[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint64_t get_le(const uint8_t *src, unsigned size) {
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)src[i] << (8u * i);
    }
    return v;
}

static uint32_t sse(Insn *in, const uint8_t *op) {
    uint8_t (*x)[16] = in->r->xmm;
    unsigned reg = in->reg;
    unsigned len = in->prefix_len + 2u + in->modrm_len;
    uint64_t v = 0;
    switch (op[1]) {
    case 0x10:
    case 0x11: {
        unsigned size = in->pf3 ? 4u : in->pf2 ? 8u : 16u; // movss, movsd, movups
        bool load = op[1] == 0x10;
        if (size == 16) {
            for (unsigned k = 0; k < 4; k++) {
                uint64_t w = 0;
                if (load) {
                    if (!wl_xb_mmio_read(in->va + 4u * k, 4, &w)) {
                        in->what = "no device at the address";
                        return 0;
                    }
                    put_le(&x[reg][4u * k], w, 4);
                } else {
                    if (!wl_xb_mmio_write(in->va + 4u * k, get_le(&x[reg][4u * k], 4), 4)) {
                        in->what = "no device at the address";
                        return 0;
                    }
                }
            }
            return len;
        }
        if (load) {
            if (!mem_read(in, size, &v)) {
                return 0;
            }
            memset(x[reg], 0, 16);
            put_le(x[reg], v, size);
        } else {
            if (!mem_write(in, size, get_le(x[reg], size))) {
                return 0;
            }
        }
        return len;
    }
    case 0x28:
    case 0x29: // movaps / movapd: 16 bytes as four words
        in->pf3 = false;
        in->pf2 = false;
        {
            uint8_t fake[2] = {0x0F, (uint8_t)(op[1] == 0x28 ? 0x10 : 0x11)};
            return sse(in, fake);
        }
    case 0x6E: // movd/movq xmm, r/m
    case 0x7E:
        if (op[1] == 0x7E && in->pf3) { // movq xmm, m64
            if (!mem_read(in, 8, &v)) {
                return 0;
            }
            memset(x[reg], 0, 16);
            put_le(x[reg], v, 8);
            return len;
        }
        if (!in->p66) {
            break; // the MMX forms are not used on devices
        }
        {
            unsigned size = in->rex_w ? 8u : 4u;
            if (op[1] == 0x6E) {
                if (!mem_read(in, size, &v)) {
                    return 0;
                }
                memset(x[reg], 0, 16);
                put_le(x[reg], v, size);
            } else {
                if (!mem_write(in, size, get_le(x[reg], size))) {
                    return 0;
                }
            }
            return len;
        }
    case 0xD6: // movq m64, xmm
        if (!in->p66) {
            break;
        }
        return mem_write(in, 8, get_le(x[reg], 8)) ? len : 0;
    case 0x5A: // cvtss2sd xmm, m32 / cvtsd2ss xmm, m64
        if (in->pf3) {
            if (!mem_read(in, 4, &v)) {
                return 0;
            }
            uint32_t bits = (uint32_t)v;
            float f;
            memcpy(&f, &bits, 4);
            double d = (double)f;
            uint64_t db;
            memcpy(&db, &d, 8);
            put_le(x[reg], db, 8);
            return len;
        }
        if (in->pf2) {
            if (!mem_read(in, 8, &v)) {
                return 0;
            }
            double d;
            memcpy(&d, &v, 8);
            float f = (float)d;
            uint32_t fb;
            memcpy(&fb, &f, 4);
            put_le(x[reg], fb, 4);
            return len;
        }
        break;
    default:
        break;
    }
    in->what = "SSE form not emulated";
    return 0;
}

uint32_t wl_xb_x86_emulate(const uint8_t *ip, WlXbX86Regs *regs, uint32_t va, const char **what) {
    Insn in;
    memset(&in, 0, sizeof in);
    in.ip = ip;
    in.r = regs;
    in.va = va;
    in.what = "instruction not emulated";
    unsigned p = 0;
    for (;;) {
        uint8_t b = ip[p];
        if (b == 0x66) {
            in.p66 = true;
        } else if (b == 0xF2) {
            in.pf2 = true;
        } else if (b == 0xF3) {
            in.pf3 = true;
        } else if (b == 0xF0 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {
            // lock and segment overrides change nothing here
        } else {
            break;
        }
        p++;
    }
    if (ip[p] >= 0x40 && ip[p] <= 0x4F) {
        uint8_t rex = ip[p++];
        in.has_rex = true;
        in.rex_w = (rex & 8u) != 0;
        in.rex_r = (rex & 4u) != 0;
        in.rex_x = (rex & 2u) != 0;
        in.rex_b = (rex & 1u) != 0;
    }
    in.prefix_len = p;
    const uint8_t *op = ip + p;
    unsigned size = in.rex_w ? 8u : in.p66 ? 2u : 4u;
    const uint8_t *m = op[0] == 0x0F ? op + 2 : op + 1;
    in.modrm_len = modrm_len(m);
    in.mod = (m[0] >> 6) & 3u;
    in.reg = ((m[0] >> 3) & 7u) | (in.rex_r ? 8u : 0u);
    unsigned len = (unsigned)(m - ip) + in.modrm_len; // through the ModRM operand
    uint64_t v = 0, s = 0;
    uint32_t result = 0;

    if (in.mod == 3) {
        in.what = "register operand: not a memory access";
        goto out;
    }
    switch (op[0]) {
    case 0x88:
    case 0x89: // mov r/m, r
        size = op[0] == 0x88 ? 1u : size;
        result = mem_write(&in, size, reg_get(&in, in.reg, size)) ? len : 0;
        break;
    case 0x8A:
    case 0x8B: // mov r, r/m
        size = op[0] == 0x8A ? 1u : size;
        if (mem_read(&in, size, &v)) {
            reg_set(&in, in.reg, size, v);
            result = len;
        }
        break;
    case 0xC6:
    case 0xC7: { // mov r/m, imm
        if (((m[0] >> 3) & 7u) != 0) {
            break;
        }
        size = op[0] == 0xC6 ? 1u : size;
        unsigned ib = size == 1 ? 1u : size == 2 ? 2u : 4u;
        result = mem_write(&in, size, imm(ip + len, ib, size)) ? len + ib : 0;
        break;
    }
    case 0x63: // movsxd r, r/m32
        if (mem_read(&in, 4, &v)) {
            reg_set(&in, in.reg, in.rex_w ? 8u : 4u, in.rex_w ? (uint64_t)(int64_t)(int32_t)(uint32_t)v : v);
            result = len;
        }
        break;
    case 0x84:
    case 0x85: // test r/m, r
        size = op[0] == 0x84 ? 1u : size;
        if (mem_read(&in, size, &v)) {
            alu(&in, OP_AND, v, reg_get(&in, in.reg, size), size);
            result = len;
        }
        break;
    case 0x86:
    case 0x87: // xchg r/m, r
        size = op[0] == 0x86 ? 1u : size;
        if (mem_read(&in, size, &v) && mem_write(&in, size, reg_get(&in, in.reg, size))) {
            reg_set(&in, in.reg, size, v);
            result = len;
        }
        break;
    case 0x80:
    case 0x81:
    case 0x83: { // group 1: op r/m, imm
        unsigned g = (m[0] >> 3) & 7u;
        size = op[0] == 0x80 ? 1u : size;
        unsigned ib = (op[0] == 0x81) ? (size == 2 ? 2u : 4u) : 1u;
        s = imm(ip + len, ib, size);
        if (mem_read(&in, size, &v)) {
            uint64_t r = alu(&in, g, v, s, size);
            if (g == OP_CMP || mem_write(&in, size, r)) {
                result = len + ib;
            }
        }
        break;
    }
    case 0xF6:
    case 0xF7: { // test r/m, imm (/0); not and neg (/2, /3)
        unsigned g = (m[0] >> 3) & 7u;
        size = op[0] == 0xF6 ? 1u : size;
        if (g == 0) {
            unsigned ib = size == 1 ? 1u : size == 2 ? 2u : 4u;
            if (mem_read(&in, size, &v)) {
                alu(&in, OP_AND, v, imm(ip + len, ib, size), size);
                result = len + ib;
            }
        } else if (g == 2 || g == 3) {
            if (mem_read(&in, size, &v)) {
                uint64_t r = g == 2 ? ~v : alu(&in, OP_SUB, 0, v, size);
                if (g == 3) {
                    // neg: CF set unless the operand was 0
                    in.r->rflags = (in.r->rflags & ~(uint64_t)WL_XB_X86_CF) | ((v & mask_of(size)) ? WL_XB_X86_CF : 0u);
                }
                result = mem_write(&in, size, r) ? len : 0;
            }
        }
        break;
    }
    case 0xFE:
    case 0xFF: { // inc / dec r/m
        unsigned g = (m[0] >> 3) & 7u;
        size = op[0] == 0xFE ? 1u : size;
        if (g > 1) {
            break; // call/jmp/push through a device register: no
        }
        if (mem_read(&in, size, &v)) {
            uint64_t mk = mask_of(size), sign = 1ull << (size * 8u - 1u);
            uint64_t r = (g == 0 ? v + 1u : v - 1u) & mk;
            bool of = g == 0 ? (r == sign) : ((v & mk) == sign);
            set_flags(&in, r, size, false, of, ((v ^ r) & 0x10u) != 0, true);
            result = mem_write(&in, size, r) ? len : 0;
        }
        break;
    }
    case 0x0F:
        switch (op[1]) {
        case 0xB6:
        case 0xB7:
        case 0xBE:
        case 0xBF: { // movzx / movsx r, r/m8|16
            unsigned s8 = (op[1] & 1u) ? 2u : 1u;
            if (mem_read(&in, s8, &v)) {
                if (op[1] >= 0xBE) {
                    v = s8 == 1 ? (uint64_t)(int64_t)(int8_t)v : (uint64_t)(int64_t)(int16_t)v;
                }
                reg_set(&in, in.reg, size, v & mask_of(size));
                result = len;
            }
            break;
        }
        case 0xB0:
        case 0xB1: { // cmpxchg r/m, r
            size = op[1] == 0xB0 ? 1u : size;
            if (mem_read(&in, size, &v)) {
                uint64_t acc = reg_get(&in, 0, size);
                alu(&in, OP_CMP, acc, v, size);
                if ((acc & mask_of(size)) == v) {
                    result = mem_write(&in, size, reg_get(&in, in.reg, size)) ? len : 0;
                } else {
                    reg_set(&in, 0, size, v);
                    result = len;
                }
            }
            break;
        }
        case 0xC0:
        case 0xC1: { // xadd r/m, r
            size = op[1] == 0xC0 ? 1u : size;
            if (mem_read(&in, size, &v)) {
                uint64_t sum = alu(&in, OP_ADD, v, reg_get(&in, in.reg, size), size);
                if (mem_write(&in, size, sum)) {
                    reg_set(&in, in.reg, size, v);
                    result = len;
                }
            }
            break;
        }
        default:
            result = sse(&in, op);
            break;
        }
        break;
    default:
        if (op[0] < 0x40 && (op[0] & 7u) < 4u) { // the ALU families: op r/m, r and op r, r/m
            unsigned g = op[0] >> 3;
            bool byte = (op[0] & 1u) == 0;
            bool to_reg = (op[0] & 2u) != 0;
            size = byte ? 1u : size;
            if (mem_read(&in, size, &v)) {
                uint64_t rv = reg_get(&in, in.reg, size);
                if (to_reg) {
                    uint64_t r = alu(&in, g, rv, v, size);
                    if (g != OP_CMP) {
                        reg_set(&in, in.reg, size, r);
                    }
                    result = len;
                } else {
                    uint64_t r = alu(&in, g, v, rv, size);
                    result = (g == OP_CMP || mem_write(&in, size, r)) ? len : 0;
                }
            }
        }
        break;
    }
out:
    if (what != NULL) {
        *what = result != 0 ? NULL : in.what;
    }
    return result;
}
