// SPDX-License-Identifier: MIT
// PowerPC load/store decoding and emulation for device faults (ppc_decode.h). Encodings from the
// PowerPC User Instruction Set Architecture (Book I), primary opcodes 32-55 and the X-form
// extended opcodes of primary 31.
#include "wl/xboxrt/mmio.h"
#include "wl/xboxrt/ppc_decode.h"

typedef struct {
    uint16_t xo;
    const char *name;
    uint8_t size;
    bool store, reverse, sign, update, refused;
} XForm;

// (the byte-reversed forms first: a recompiled title's device accesses are little-endian words, so
// the search for a push buffer kick's stwbrx ends at once)
static const XForm k_xforms[] = {
    {534, "lwbrx", 4, false, true, false, false, false},  {662, "stwbrx", 4, true, true, false, false, false},
    {790, "lhbrx", 2, false, true, false, false, false},  {918, "sthbrx", 2, true, true, false, false, false},
    {23, "lwzx", 4, false, false, false, false, false},   {55, "lwzux", 4, false, false, false, true, false},
    {87, "lbzx", 1, false, false, false, false, false},   {119, "lbzux", 1, false, false, false, true, false},
    {151, "stwx", 4, true, false, false, false, false},   {183, "stwux", 4, true, false, false, true, false},
    {215, "stbx", 1, true, false, false, false, false},   {247, "stbux", 1, true, false, false, true, false},
    {279, "lhzx", 2, false, false, false, false, false},  {311, "lhzux", 2, false, false, false, true, false},
    {343, "lhax", 2, false, false, true, false, false},   {375, "lhaux", 2, false, false, true, true, false},
    {407, "sthx", 2, true, false, false, false, false},   {439, "sthux", 2, true, false, false, true, false},
    {20, "lwarx", 4, false, false, false, false, true},   {150, "stwcx.", 4, true, false, false, false, true},
    {1014, "dcbz", 32, true, false, false, false, true},  {86, "dcbf", 0, false, false, false, false, true},
    {54, "dcbst", 0, false, false, false, false, true},   {470, "dcbi", 0, false, false, false, false, true},
    {278, "dcbt", 0, false, false, false, false, true},   {246, "dcbtst", 0, false, false, false, false, true},
    {533, "lswx", 0, false, false, false, false, true},   {597, "lswi", 0, false, false, false, false, true},
    {661, "stswx", 0, true, false, false, false, true},   {725, "stswi", 0, true, false, false, false, true},
    {310, "eciwx", 4, false, false, false, false, true},  {438, "ecowx", 4, true, false, false, false, true},
    {535, "lfsx", 4, false, false, false, false, true},   {599, "lfdx", 8, false, false, false, false, true},
    {663, "stfsx", 4, true, false, false, false, true},   {727, "stfdx", 8, true, false, false, false, true},
    {983, "stfiwx", 4, true, false, false, false, true},
};

static const char *const k_dnames[] = {
    "lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu", "lhz", "lhzu", "lha", "lhau",
    "sth", "sthu", "lmw", "stmw", "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu",
};

WlXbPpcDecode wl_xb_ppc_decode(uint32_t insn, WlXbPpcAccess *out) {
    *out = (WlXbPpcAccess){0};
    uint32_t op = insn >> 26;
    out->rt = (uint8_t)((insn >> 21) & 31u);
    out->ra = (uint8_t)((insn >> 16) & 31u);
    out->rb = (uint8_t)((insn >> 11) & 31u);
    if (op == 31) {
        uint32_t xo = (insn >> 1) & 0x3FFu;
        for (size_t i = 0; i < WL_ARRAY_COUNT(k_xforms); i++) {
            if (k_xforms[i].xo == xo) {
                const XForm *x = &k_xforms[i];
                out->name = x->name;
                out->size = x->size;
                out->store = x->store;
                out->reverse = x->reverse;
                out->sign = x->sign;
                out->update = x->update;
                out->indexed = true;
                return x->refused ? WL_XB_PPC_REFUSED : WL_XB_PPC_OK;
            }
        }
        return WL_XB_PPC_UNKNOWN;
    }
    if (op < 32 || op > 55) {
        return WL_XB_PPC_UNKNOWN;
    }
    out->name = k_dnames[op - 32];
    out->disp = (int16_t)(insn & 0xFFFFu);
    if (op >= 46) {
        return WL_XB_PPC_REFUSED; // lmw, stmw and the floating-point forms
    }
    static const uint8_t sizes[] = {4, 4, 1, 1, 4, 4, 1, 1, 2, 2, 2, 2, 2, 2};
    uint32_t k = op - 32;
    out->size = sizes[k];
    out->update = (k & 1u) != 0;
    out->store = (op >= 36 && op <= 39) || op == 44 || op == 45;
    out->sign = op == 42 || op == 43;
    return WL_XB_PPC_OK;
}

uint32_t wl_xb_ppc_ea(const WlXbPpcAccess *a, const uint32_t gpr[32]) {
    uint32_t base = a->ra != 0 ? gpr[a->ra] : 0u;
    int32_t disp = a->disp;
    uint32_t off = a->indexed ? gpr[a->rb] : (uint32_t)disp;
    return base + off;
}

static uint32_t swap(uint32_t v, unsigned size) {
    if (size == 4) {
        return __builtin_bswap32(v);
    }
    if (size == 2) {
        return (uint32_t)__builtin_bswap16((uint16_t)v);
    }
    return v;
}

bool wl_xb_ppc_emulate(const WlXbPpcAccess *a, uint32_t gpr[32], uint32_t ea, uint32_t va) {
    if (a->store) {
        uint32_t v = gpr[a->rt];
        v = a->size == 4 ? v : a->size == 2 ? (v & 0xFFFFu) : (v & 0xFFu);
        // a plain store writes the register big-endian: the device sees the bytes swapped
        if (!wl_xb_mmio_write(va, a->reverse ? v : swap(v, a->size), a->size)) {
            return false;
        }
    } else {
        uint64_t raw = 0;
        if (!wl_xb_mmio_read(va, a->size, &raw)) {
            return false;
        }
        uint32_t v = (uint32_t)raw;
        if (!a->reverse) {
            v = swap(v, a->size); // the value the x86 reads, as a big-endian load sees its bytes
        }
        if (a->sign) {
            v = (uint32_t)(int32_t)(int16_t)v;
        }
        gpr[a->rt] = v;
    }
    if (a->update && a->ra != 0) {
        gpr[a->ra] = ea;
    }
    return true;
}
