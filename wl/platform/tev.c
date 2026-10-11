// SPDX-License-Identifier: MIT
// TEV programs as data: builder, allocation helpers, validator, software evaluator and the fixed
// recipes. Portable C: no GX, no platform calls. Declarations are in tev.h.
#include "wl/platform/tev.h"

#include <string.h>

#include "wl/base/hash.h"

// ---- builder ------------------------------------------------------------------------------------

void wl_tev_program_init(WlTevProgram *p) {
    memset(p, 0, sizeof *p);
    static const uint8_t defaults[WL_TEV_SWAP_TABLES][4] = {
        {WL_TEV_SWAP_R, WL_TEV_SWAP_G, WL_TEV_SWAP_B, WL_TEV_SWAP_A},
        {WL_TEV_SWAP_R, WL_TEV_SWAP_R, WL_TEV_SWAP_R, WL_TEV_SWAP_A},
        {WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_G, WL_TEV_SWAP_A},
        {WL_TEV_SWAP_B, WL_TEV_SWAP_B, WL_TEV_SWAP_B, WL_TEV_SWAP_A},
    };
    memcpy(p->swap, defaults, sizeof p->swap);
    for (uint32_t i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        p->konst[i] = 0xFFFFFFFFu;
    }
}

static void fill_half(WlTevHalf *h, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t op, uint8_t bias,
                      uint8_t scale, bool clamp, uint8_t dest) {
    h->in[0] = a;
    h->in[1] = b;
    h->in[2] = c;
    h->in[3] = d;
    h->op = op;
    h->bias = bias;
    h->scale = scale;
    h->clamp = clamp ? 1u : 0u;
    h->dest = dest;
}

WlTevStage *wl_tev_add_stage(WlTevProgram *p) {
    if (p->stage_count >= WL_TEV_MAX_STAGES) {
        return NULL;
    }
    WlTevStage *s = &p->stages[p->stage_count++];
    memset(s, 0, sizeof *s);
    s->texcoord = s->texmap = s->channel = WL_TEV_NONE;
    s->kcsel = WL_TEV_KSEL_1;
    s->kasel = WL_TEV_KSEL_1;
    fill_half(&s->color, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_ADD, WL_TEV_BIAS_ZERO,
              WL_TEV_SCALE_1, true, WL_TEV_PREV);
    fill_half(&s->alpha, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_ADD, WL_TEV_BIAS_ZERO,
              WL_TEV_SCALE_1, true, WL_TEV_PREV);
    return s;
}

void wl_tev_stage_order(WlTevStage *s, uint8_t texcoord, uint8_t texmap, uint8_t channel) {
    s->texcoord = texcoord;
    s->texmap = texmap;
    s->channel = channel;
}

void wl_tev_stage_color(WlTevStage *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t op, uint8_t bias,
                        uint8_t scale, bool clamp, uint8_t dest) {
    fill_half(&s->color, a, b, c, d, op, bias, scale, clamp, dest);
}

void wl_tev_stage_alpha(WlTevStage *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t op, uint8_t bias,
                        uint8_t scale, bool clamp, uint8_t dest) {
    fill_half(&s->alpha, a, b, c, d, op, bias, scale, clamp, dest);
}

void wl_tev_set_swap(WlTevProgram *p, uint32_t table, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (table < WL_TEV_SWAP_TABLES) {
        p->swap[table][0] = r;
        p->swap[table][1] = g;
        p->swap[table][2] = b;
        p->swap[table][3] = a;
    }
}

// ---- konst slots, register pool, needs ----------------------------------------------------------

int wl_tev_konst_slot(WlTevProgram *p, uint32_t rgba) {
    uint32_t owned = (uint32_t)(p->konst_mask | p->konst_external);
    for (uint32_t i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        if ((p->konst_mask & (1u << i)) != 0 && p->konst[i] == rgba) {
            return (int)i;
        }
    }
    for (uint32_t i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        if ((owned & (1u << i)) == 0) {
            p->konst[i] = rgba;
            p->konst_mask = (uint8_t)(p->konst_mask | (1u << i));
            return (int)i;
        }
    }
    return -1;
}

void wl_tev_konst_external(WlTevProgram *p, uint32_t slot, uint32_t assumed_rgba) {
    if (slot < WL_TEV_KONST_SLOTS) {
        p->konst[slot] = assumed_rgba;
        p->konst_mask = (uint8_t)(p->konst_mask & ~(1u << slot));
        p->konst_external = (uint8_t)(p->konst_external | (1u << slot));
    }
}

void wl_tev_preload(WlTevProgram *p, uint8_t reg, uint32_t rgba) {
    if (reg < WL_TEV_REG_COUNT) { // (PREV is accepted, and then flagged by the validator)
        p->preload[reg] = rgba;
        p->preload_mask = (uint8_t)(p->preload_mask | (1u << reg));
    }
}

void wl_tev_pool_init(WlTevRegPool *pool) {
    memset(pool, 0, sizeof *pool);
}

int wl_tev_pool_alloc(WlTevRegPool *pool, uint32_t avoid) {
    for (uint32_t i = 1; i <= WL_TEV_REG_COUNT; i++) {
        uint32_t reg = i % WL_TEV_REG_COUNT; // REG0, REG1, REG2, then PREV
        if (pool->holders[reg] == 0 && (avoid & (1u << reg)) == 0) {
            pool->holders[reg] = 1;
            return (int)reg;
        }
    }
    return -1;
}

void wl_tev_pool_retain(WlTevRegPool *pool, uint8_t reg) {
    if (reg < WL_TEV_REG_COUNT && pool->holders[reg] < 255u) {
        pool->holders[reg]++;
    }
}

void wl_tev_pool_release(WlTevRegPool *pool, uint8_t reg) {
    if (reg < WL_TEV_REG_COUNT && pool->holders[reg] > 0) {
        pool->holders[reg]--;
    }
}

bool wl_tev_pool_free(const WlTevRegPool *pool, uint8_t reg) {
    return reg < WL_TEV_REG_COUNT && pool->holders[reg] == 0;
}

void wl_tev_needs_clear(WlTevNeeds *n) {
    n->texcoord = n->texmap = n->channel = WL_TEV_NONE;
    n->tex_swaps = n->ras_swaps = WL_TEV_SWAPS_ANY;
    n->kcsel = n->kasel = WL_TEV_KSEL_NONE;
}

bool wl_tev_needs_texture(WlTevNeeds *n, uint8_t texcoord, uint8_t texmap, uint8_t swaps) {
    if (n->texmap != WL_TEV_NONE && (n->texmap != texmap || n->texcoord != texcoord)) {
        return false;
    }
    uint8_t merged = (uint8_t)(n->tex_swaps & swaps & WL_TEV_SWAPS_ANY);
    if (merged == 0) {
        return false;
    }
    n->texcoord = texcoord;
    n->texmap = texmap;
    n->tex_swaps = merged;
    return true;
}

bool wl_tev_needs_channel(WlTevNeeds *n, uint8_t channel, uint8_t swaps) {
    if (n->channel != WL_TEV_NONE && n->channel != channel) {
        return false;
    }
    uint8_t merged = (uint8_t)(n->ras_swaps & swaps & WL_TEV_SWAPS_ANY);
    if (merged == 0) {
        return false;
    }
    n->channel = channel;
    n->ras_swaps = merged;
    return true;
}

bool wl_tev_needs_konst_color(WlTevNeeds *n, uint8_t sel) {
    if (n->kcsel != WL_TEV_KSEL_NONE && n->kcsel != sel) {
        return false;
    }
    n->kcsel = sel;
    return true;
}

bool wl_tev_needs_konst_alpha(WlTevNeeds *n, uint8_t sel) {
    if (n->kasel != WL_TEV_KSEL_NONE && n->kasel != sel) {
        return false;
    }
    n->kasel = sel;
    return true;
}

bool wl_tev_needs_merge(WlTevNeeds *into, const WlTevNeeds *other) {
    WlTevNeeds m = *into;
    if (other->texmap != WL_TEV_NONE && !wl_tev_needs_texture(&m, other->texcoord, other->texmap, other->tex_swaps)) {
        return false;
    }
    if (other->texmap == WL_TEV_NONE) {
        m.tex_swaps = (uint8_t)(m.tex_swaps & other->tex_swaps);
        if (m.tex_swaps == 0) {
            return false;
        }
    }
    if (other->channel != WL_TEV_NONE && !wl_tev_needs_channel(&m, other->channel, other->ras_swaps)) {
        return false;
    }
    if (other->channel == WL_TEV_NONE) {
        m.ras_swaps = (uint8_t)(m.ras_swaps & other->ras_swaps);
        if (m.ras_swaps == 0) {
            return false;
        }
    }
    if (other->kcsel != WL_TEV_KSEL_NONE && !wl_tev_needs_konst_color(&m, other->kcsel)) {
        return false;
    }
    if (other->kasel != WL_TEV_KSEL_NONE && !wl_tev_needs_konst_alpha(&m, other->kasel)) {
        return false;
    }
    *into = m;
    return true;
}

static uint8_t lowest_table(uint8_t set) {
    for (uint8_t t = 0; t < WL_TEV_SWAP_TABLES; t++) {
        if ((set & (1u << t)) != 0) {
            return t;
        }
    }
    return 0;
}

void wl_tev_stage_set_needs(WlTevStage *s, const WlTevNeeds *n) {
    s->texcoord = n->texcoord;
    s->texmap = n->texmap;
    s->channel = n->channel;
    s->tex_swap = lowest_table(n->tex_swaps);
    s->ras_swap = lowest_table(n->ras_swaps);
    if (n->kcsel != WL_TEV_KSEL_NONE) {
        s->kcsel = n->kcsel;
    }
    if (n->kasel != WL_TEV_KSEL_NONE) {
        s->kasel = n->kasel;
    }
}

// ---- validation ---------------------------------------------------------------------------------

static void issue(WlTevReport *r, bool error, WlTevCode code, uint32_t stage, uint32_t alpha, uint32_t detail) {
    if (error) {
        r->errors++;
    } else {
        r->warnings++;
    }
    if (r->count < WL_TEV_MAX_ISSUES) {
        WlTevIssue *i = &r->issues[r->count++];
        i->code = code;
        i->stage = (uint8_t)stage;
        i->alpha = (uint8_t)alpha;
        i->detail = (uint8_t)detail;
    }
}

static bool kcsel_valid(uint8_t s) {
    return s < 8u || (s >= 0x0Cu && s <= 0x1Fu);
}

static bool kasel_valid(uint8_t s) {
    return s < 8u || (s >= 0x10u && s <= 0x1Fu);
}

static bool op_valid(uint8_t op, bool alpha) {
    if (op == WL_TEV_ADD || op == WL_TEV_SUB) {
        return true;
    }
    if (alpha) {
        return op == WL_TEV_COMP_A8_GT || op == WL_TEV_COMP_A8_EQ;
    }
    return op >= WL_TEV_COMP_R8_GT && op <= WL_TEV_COMP_RGB8_EQ;
}

static void check_half(WlTevReport *r, const WlTevStage *s, uint32_t idx, bool alpha) {
    const WlTevHalf *h = alpha ? &s->alpha : &s->color;
    const uint32_t limit = alpha ? WL_TEV_CA_COUNT : WL_TEV_CC_COUNT;
    for (uint32_t i = 0; i < 4; i++) {
        if (h->in[i] >= limit) {
            issue(r, true, WL_TEV_E_INPUT, idx, alpha, i);
            continue;
        }
        bool tex = alpha ? h->in[i] == WL_TEV_CA_TEXA : (h->in[i] == WL_TEV_CC_TEXC || h->in[i] == WL_TEV_CC_TEXA);
        bool ras = alpha ? h->in[i] == WL_TEV_CA_RASA : (h->in[i] == WL_TEV_CC_RASC || h->in[i] == WL_TEV_CC_RASA);
        if (tex && s->texmap == WL_TEV_NONE) {
            issue(r, true, WL_TEV_E_TEXTURE_UNBOUND, idx, alpha, i);
        }
        if (ras && s->channel == WL_TEV_NONE) {
            issue(r, true, WL_TEV_E_RAS_UNBOUND, idx, alpha, i);
        }
    }
    if (!op_valid(h->op, alpha)) {
        issue(r, true, WL_TEV_E_OP, idx, alpha, h->op);
    } else if (h->op >= WL_TEV_COMP_R8_GT) {
        if (h->bias != WL_TEV_BIAS_ZERO || h->scale != WL_TEV_SCALE_1) {
            issue(r, true, WL_TEV_E_BIAS_SCALE, idx, alpha, h->op);
        }
    }
    if (h->bias >= WL_TEV_BIAS_COUNT || h->scale >= WL_TEV_SCALE_COUNT || h->clamp > 1u) {
        issue(r, true, WL_TEV_E_BIAS_SCALE, idx, alpha, h->scale);
    }
    if (h->dest >= WL_TEV_REG_COUNT) {
        issue(r, true, WL_TEV_E_DEST, idx, alpha, h->dest);
    }
}

// The register component an input reads: *reg and *comp (0 colour, 1 alpha), or false.
static bool input_register(uint8_t code, bool alpha, uint32_t *reg, uint32_t *comp) {
    if (alpha) {
        if (code > WL_TEV_CA_A2) {
            return false;
        }
        *reg = code;
        *comp = 1;
        return true;
    }
    if (code > WL_TEV_CC_A2) {
        return false;
    }
    *reg = (uint32_t)code >> 1;
    *comp = code & 1u;
    return true;
}

// A half that does nothing to PREV: 0 + PREV into PREV (what wl_tev_add_stage leaves).
static bool half_passes_prev(const WlTevHalf *h, bool alpha) {
    const uint8_t zero = alpha ? WL_TEV_CA_ZERO : WL_TEV_CC_ZERO;
    const uint8_t prev = alpha ? WL_TEV_CA_APREV : WL_TEV_CC_CPREV;
    return h->in[0] == zero && h->in[1] == zero && h->in[2] == zero && h->in[3] == prev && h->op == WL_TEV_ADD &&
           h->bias == WL_TEV_BIAS_ZERO && h->scale == WL_TEV_SCALE_1 && h->dest == WL_TEV_PREV;
}

bool wl_tev_validate(const WlTevProgram *p, WlTevReport *out) {
    WlTevReport local;
    WlTevReport *report = out != NULL ? out : &local;
    memset(report, 0, sizeof *report);
    if (p->stage_count == 0 || p->stage_count > WL_TEV_MAX_STAGES) {
        issue(report, true, WL_TEV_E_NO_STAGES, 0, 0, p->stage_count);
        return false;
    }
    for (uint32_t t = 0; t < WL_TEV_SWAP_TABLES; t++) {
        for (uint32_t i = 0; i < 4; i++) {
            if (p->swap[t][i] > WL_TEV_SWAP_A) {
                issue(report, true, WL_TEV_E_SWAP, 0, 0, t);
            }
        }
    }
    if ((p->preload_mask & (1u << WL_TEV_PREV)) != 0) {
        issue(report, true, WL_TEV_E_PRELOAD, 0, 0, WL_TEV_PREV);
    }
    for (uint32_t si = 0; si < p->stage_count; si++) {
        const WlTevStage *s = &p->stages[si];
        if ((s->texcoord != WL_TEV_NONE && s->texcoord >= WL_TEV_TEXCOORDS) ||
            (s->texmap != WL_TEV_NONE && s->texmap >= WL_TEV_TEXMAPS) ||
            ((s->texcoord == WL_TEV_NONE) != (s->texmap == WL_TEV_NONE))) {
            issue(report, true, WL_TEV_E_ORDER, si, 0, s->texmap);
        }
        if (s->channel == 7u || s->channel == 8u) {
            issue(report, true, WL_TEV_E_CHANNEL, si, 0, s->channel);
        } else if (s->channel != WL_TEV_NONE && s->channel != WL_TEV_CH_COLOR0A0 && s->channel != WL_TEV_CH_COLOR1A1 &&
                   s->channel != WL_TEV_CH_ZERO) {
            issue(report, true, WL_TEV_E_ORDER, si, 0, s->channel);
        }
        if (s->ras_swap >= WL_TEV_SWAP_TABLES || s->tex_swap >= WL_TEV_SWAP_TABLES) {
            issue(report, true, WL_TEV_E_SWAP, si, 0, s->ras_swap >= WL_TEV_SWAP_TABLES ? s->ras_swap : s->tex_swap);
        }
        if (!kcsel_valid(s->kcsel)) {
            issue(report, true, WL_TEV_E_KSEL, si, 0, s->kcsel);
        }
        if (!kasel_valid(s->kasel)) {
            issue(report, true, WL_TEV_E_KSEL, si, 1, s->kasel);
        }
        check_half(report, s, si, false);
        check_half(report, s, si, true);
    }
    if (report->errors != 0) {
        return false;
    }
    if (out == NULL) {
        return true; // a yes/no: the warnings are not worked out
    }

    // Dataflow warnings. written[half][reg]: some stage has written it; pending[half][reg]: the
    // last write (stage + 1) not yet read.
    bool written[2][WL_TEV_REG_COUNT] = {{false}};
    bool warned_read[2][WL_TEV_REG_COUNT] = {{false}};
    uint8_t pending[2][WL_TEV_REG_COUNT] = {{0}};
    bool warned_konst[WL_TEV_KONST_SLOTS] = {false};
    const uint32_t known_konst = (uint32_t)(p->konst_mask | p->konst_external);
    for (uint32_t si = 0; si < p->stage_count; si++) {
        const WlTevStage *s = &p->stages[si];
        for (uint32_t half = 0; half < 2; half++) {
            const WlTevHalf *h = half ? &s->alpha : &s->color;
            if (half_passes_prev(h, half != 0)) {
                continue;
            }
            for (uint32_t i = 0; i < 4; i++) {
                uint32_t reg, comp;
                if (input_register(h->in[i], half != 0, &reg, &comp)) {
                    if (!written[comp][reg] && (p->preload_mask & (1u << reg)) == 0 && !warned_read[comp][reg]) {
                        warned_read[comp][reg] = true;
                        issue(report, false, WL_TEV_W_READ_UNWRITTEN, si, half, reg);
                    }
                    pending[comp][reg] = 0;
                } else if (half ? h->in[i] == WL_TEV_CA_KONST : h->in[i] == WL_TEV_CC_KONST) {
                    uint8_t sel = half ? s->kasel : s->kcsel;
                    uint32_t slot = sel & 3u;
                    if (sel >= (half ? 0x10u : 0x0Cu) && (known_konst & (1u << slot)) == 0 && !warned_konst[slot]) {
                        warned_konst[slot] = true;
                        issue(report, false, WL_TEV_W_KONST_UNSET, si, half, slot);
                    }
                }
            }
        }
        for (uint32_t half = 0; half < 2; half++) {
            if (half_passes_prev(half ? &s->alpha : &s->color, half != 0)) {
                continue;
            }
            uint32_t d = half ? s->alpha.dest : s->color.dest;
            if (pending[half][d] != 0) {
                issue(report, false, WL_TEV_W_DEAD_WRITE, (uint32_t)pending[half][d] - 1u, half, d);
            }
            pending[half][d] = (uint8_t)(si + 1u);
            written[half][d] = true;
        }
    }
    for (uint32_t half = 0; half < 2; half++) {
        pending[half][WL_TEV_PREV] = 0; // the pipeline reads the result
        for (uint32_t reg = 0; reg < WL_TEV_REG_COUNT; reg++) {
            if (pending[half][reg] != 0) {
                issue(report, false, WL_TEV_W_DEAD_WRITE, (uint32_t)pending[half][reg] - 1u, half, reg);
            }
        }
        if (!written[half][WL_TEV_PREV]) {
            issue(report, false, WL_TEV_W_NO_RESULT, 0, half, WL_TEV_PREV);
        }
    }
    return true;
}

const char *wl_tev_code_name(WlTevCode code) {
    switch (code) {
    case WL_TEV_OK: return "ok";
    case WL_TEV_E_NO_STAGES: return "no stages or more than 16";
    case WL_TEV_E_INPUT: return "input code out of range";
    case WL_TEV_E_OP: return "op does not exist";
    case WL_TEV_E_BIAS_SCALE: return "bias or scale invalid for the op";
    case WL_TEV_E_DEST: return "destination register out of range";
    case WL_TEV_E_ORDER: return "texcoord, texmap or channel invalid";
    case WL_TEV_E_CHANNEL: return "colour channel not modelled (alpha bump)";
    case WL_TEV_E_SWAP: return "swap table invalid";
    case WL_TEV_E_KSEL: return "konst selection invalid";
    case WL_TEV_E_TEXTURE_UNBOUND: return "reads a texel without a texture map";
    case WL_TEV_E_RAS_UNBOUND: return "reads the rasterised colour without a channel";
    case WL_TEV_E_PRELOAD: return "PREV cannot be preloaded";
    case WL_TEV_W_KONST_UNSET: return "reads a konst register that is not set";
    case WL_TEV_W_READ_UNWRITTEN: return "reads a register nothing has written";
    case WL_TEV_W_DEAD_WRITE: return "register written but never read";
    case WL_TEV_W_NO_RESULT: return "nothing writes PREV";
    }
    return "unknown";
}

// ---- what a program uses ------------------------------------------------------------------------

uint32_t wl_tev_channels_used(const WlTevProgram *p) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        uint8_t c = p->stages[i].channel;
        if (c == WL_TEV_CH_COLOR1A1) {
            n = 2;
        } else if ((c == WL_TEV_CH_COLOR0A0 || c == WL_TEV_CH_ZERO) && n < 1) {
            n = 1;
        }
    }
    return n;
}

uint32_t wl_tev_texcoords_used(const WlTevProgram *p) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        uint8_t c = p->stages[i].texcoord;
        if (c != WL_TEV_NONE && c < WL_TEV_TEXCOORDS && c + 1u > n) {
            n = c + 1u;
        }
    }
    return n;
}

uint32_t wl_tev_channels_mask(const WlTevProgram *p) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < p->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        uint8_t c = p->stages[i].channel;
        if (c == WL_TEV_CH_COLOR0A0 || c == WL_TEV_CH_COLOR1A1) {
            mask |= 1u << (c - WL_TEV_CH_COLOR0A0);
        }
    }
    return mask;
}

uint32_t wl_tev_texcoords_mask(const WlTevProgram *p) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < p->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        uint8_t c = p->stages[i].texcoord;
        if (c != WL_TEV_NONE && c < WL_TEV_TEXCOORDS) {
            mask |= 1u << c;
        }
    }
    return mask;
}

uint32_t wl_tev_texmaps_used(const WlTevProgram *p) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < p->stage_count && i < WL_TEV_MAX_STAGES; i++) {
        uint8_t m = p->stages[i].texmap;
        if (m != WL_TEV_NONE && m < WL_TEV_TEXMAPS) {
            mask |= 1u << m;
        }
    }
    return mask;
}

bool wl_tev_equal(const WlTevProgram *a, const WlTevProgram *b) {
    if (a->stage_count != b->stage_count || a->stage_count > WL_TEV_MAX_STAGES ||
        a->konst_mask != b->konst_mask || a->konst_external != b->konst_external ||
        a->preload_mask != b->preload_mask || memcmp(a->swap, b->swap, sizeof a->swap) != 0 ||
        memcmp(a->stages, b->stages, a->stage_count * sizeof a->stages[0]) != 0) {
        return false;
    }
    for (uint32_t i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        if ((a->konst_mask & (1u << i)) != 0 && a->konst[i] != b->konst[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < WL_TEV_REG_COUNT; i++) {
        if ((a->preload_mask & (1u << i)) != 0 && a->preload[i] != b->preload[i]) {
            return false;
        }
    }
    return true;
}

static uint64_t hash_u32(uint64_t h, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return wl_fnv1a64_update(h, b, 4);
}

uint64_t wl_tev_hash(const WlTevProgram *p) {
    uint64_t h = WL_FNV1A64_INIT;
    uint32_t n = p->stage_count <= WL_TEV_MAX_STAGES ? p->stage_count : WL_TEV_MAX_STAGES;
    uint8_t head[3] = {(uint8_t)n, p->konst_mask, (uint8_t)(p->konst_external | (p->preload_mask << 4))};
    h = wl_fnv1a64_update(h, head, sizeof head);
    h = wl_fnv1a64_update(h, p->stages, n * sizeof p->stages[0]);
    h = wl_fnv1a64_update(h, p->swap, sizeof p->swap);
    for (uint32_t i = 0; i < WL_TEV_KONST_SLOTS; i++) {
        if ((p->konst_mask & (1u << i)) != 0) {
            h = hash_u32(h, p->konst[i]);
        }
    }
    for (uint32_t i = 0; i < WL_TEV_REG_COUNT; i++) {
        if ((p->preload_mask & (1u << i)) != 0) {
            h = hash_u32(h, p->preload[i]);
        }
    }
    return h;
}

// ---- evaluation ---------------------------------------------------------------------------------

// A konst selection's fractions n/8 of 255, rounded.
static const uint8_t k_fraction[8] = {255, 223, 191, 159, 128, 96, 64, 32};

static uint32_t konst_comp(const WlTevProgram *p, uint32_t slot, uint32_t comp) { // comp 0..3 = r g b a
    return (p->konst[slot] >> (24u - 8u * comp)) & 0xFFu;
}

// The colour half's KONST: a byte per channel.
static void konst_color(const WlTevProgram *p, uint8_t sel, uint32_t out[3]) {
    if (sel < 8u) {
        out[0] = out[1] = out[2] = k_fraction[sel];
    } else if (sel < 0x10u) {
        for (uint32_t c = 0; c < 3; c++) {
            out[c] = konst_comp(p, sel & 3u, c);
        }
    } else {
        uint32_t v = konst_comp(p, sel & 3u, ((uint32_t)sel - 0x10u) >> 2);
        out[0] = out[1] = out[2] = v;
    }
}

static uint32_t konst_alpha(const WlTevProgram *p, uint8_t sel) {
    if (sel < 8u) {
        return k_fraction[sel];
    }
    return konst_comp(p, sel & 3u, ((uint32_t)sel - 0x10u) >> 2);
}

static int32_t clamp_value(int32_t v, bool clamp) {
    int32_t lo = clamp ? 0 : -1024;
    int32_t hi = clamp ? 255 : 1023;
    return v < lo ? lo : v > hi ? hi : v;
}

// One channel of the blend: (d +- ((256 - c') * a + c' * b) / 256 + bias) * scale, rounded at the
// 8 fractional bits the hardware keeps. a, b and c are 0..255; d is -1024..1023. DIVIDE_2 halves the
// whole sum (bias too), as Dolphin does.
static int32_t blend(int32_t a, int32_t b, int32_t c, int32_t d, const WlTevHalf *h) {
    int32_t c2 = c + (c >> 7);
    int32_t lerp = a * (256 - c2) + b * c2;
    int32_t v = d * 256 + (h->op == WL_TEV_SUB ? -lerp : lerp);
    if (h->bias == WL_TEV_BIAS_ADDHALF) {
        v += 128 * 256;
    } else if (h->bias == WL_TEV_BIAS_SUBHALF) {
        v -= 128 * 256;
    }
    switch (h->scale) {
    case WL_TEV_SCALE_2: v *= 2; break;
    case WL_TEV_SCALE_4: v *= 4; break;
    case WL_TEV_DIVIDE_2: v >>= 1; break;
    default: break;
    }
    return clamp_value((v + 128) >> 8, h->clamp != 0);
}

// The compare ops. a, b, c, d are the three channels (r, g, b) of each input.
static void compare(const int32_t a[3], const int32_t b[3], const int32_t c[3], const int32_t d[3], const WlTevHalf *h,
                    int32_t out[3]) {
    const bool eq = (h->op & 1u) != 0;
    bool cond[3];
    uint32_t va, vb;
    switch (h->op & ~1u) {
    case WL_TEV_COMP_R8_GT: va = (uint32_t)a[0]; vb = (uint32_t)b[0]; break;
    case WL_TEV_COMP_GR16_GT: va = (uint32_t)(a[1] << 8 | a[0]); vb = (uint32_t)(b[1] << 8 | b[0]); break;
    case WL_TEV_COMP_BGR24_GT: va = (uint32_t)(a[2] << 16 | a[1] << 8 | a[0]); vb = (uint32_t)(b[2] << 16 | b[1] << 8 | b[0]); break;
    default: va = vb = 0; break;
    }
    if ((h->op & ~1u) == WL_TEV_COMP_RGB8_GT) {
        for (uint32_t i = 0; i < 3; i++) {
            cond[i] = eq ? a[i] == b[i] : a[i] > b[i];
        }
    } else {
        cond[0] = cond[1] = cond[2] = eq ? va == vb : va > vb;
    }
    for (uint32_t i = 0; i < 3; i++) {
        out[i] = clamp_value(d[i] + (cond[i] ? c[i] : 0), h->clamp != 0);
    }
}

static void swapped(const uint8_t texel[4], const uint8_t table[4], int32_t out[4]) {
    for (uint32_t i = 0; i < 4; i++) {
        out[i] = texel[table[i] & 3u];
    }
}

void wl_tev_eval(const WlTevProgram *p, const WlTevPixel *px, WlTevRegValue regs[WL_TEV_REG_COUNT]) {
    int32_t reg[WL_TEV_REG_COUNT][4];
    for (uint32_t r = 0; r < WL_TEV_REG_COUNT; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            reg[r][c] = (p->preload_mask & (1u << r)) != 0 ? (int32_t)((p->preload[r] >> (24u - 8u * c)) & 0xFFu) : 0;
        }
    }
    if (wl_tev_validate(p, NULL)) {
        for (uint32_t si = 0; si < p->stage_count; si++) {
            const WlTevStage *s = &p->stages[si];
            int32_t tex[4] = {0, 0, 0, 0};
            int32_t ras[4] = {0, 0, 0, 0};
            if (s->texmap != WL_TEV_NONE) {
                swapped(px->tex[s->texmap], p->swap[s->tex_swap], tex);
            }
            if (s->channel == WL_TEV_CH_COLOR0A0 || s->channel == WL_TEV_CH_COLOR1A1) {
                swapped(px->ras[s->channel - WL_TEV_CH_COLOR0A0], p->swap[s->ras_swap], ras);
            }
            uint32_t kc[3];
            konst_color(p, s->kcsel, kc);

            // colour inputs, three channels each
            int32_t in[4][3];
            for (uint32_t i = 0; i < 4; i++) {
                uint8_t code = s->color.in[i];
                for (uint32_t c = 0; c < 3; c++) {
                    int32_t v;
                    if (code <= WL_TEV_CC_A2) {
                        const int32_t *rv = reg[code >> 1];
                        v = (code & 1u) != 0 ? rv[3] : rv[c];
                    } else {
                        switch (code) {
                        case WL_TEV_CC_TEXC: v = tex[c]; break;
                        case WL_TEV_CC_TEXA: v = tex[3]; break;
                        case WL_TEV_CC_RASC: v = ras[c]; break;
                        case WL_TEV_CC_RASA: v = ras[3]; break;
                        case WL_TEV_CC_ONE: v = 255; break;
                        case WL_TEV_CC_HALF: v = 128; break;
                        case WL_TEV_CC_KONST: v = (int32_t)kc[c]; break;
                        default: v = 0; break;
                        }
                    }
                    in[i][c] = i < 3 ? (v & 255) : v; // a, b, c are 8 bits; d is the register's signed value
                }
            }
            int32_t cout[3];
            if (s->color.op >= WL_TEV_COMP_R8_GT) {
                compare(in[0], in[1], in[2], in[3], &s->color, cout);
            } else {
                for (uint32_t c = 0; c < 3; c++) {
                    cout[c] = blend(in[0][c], in[1][c], in[2][c], in[3][c], &s->color);
                }
            }

            // alpha inputs
            int32_t ain[4];
            for (uint32_t i = 0; i < 4; i++) {
                uint8_t code = s->alpha.in[i];
                int32_t v;
                switch (code) {
                case WL_TEV_CA_APREV:
                case WL_TEV_CA_A0:
                case WL_TEV_CA_A1:
                case WL_TEV_CA_A2: v = reg[code][3]; break;
                case WL_TEV_CA_TEXA: v = tex[3]; break;
                case WL_TEV_CA_RASA: v = ras[3]; break;
                case WL_TEV_CA_KONST: v = (int32_t)konst_alpha(p, s->kasel); break;
                default: v = 0; break;
                }
                ain[i] = i < 3 ? (v & 255) : v;
            }
            int32_t aout;
            if (s->alpha.op >= WL_TEV_COMP_R8_GT) {
                const int32_t ac[3][3] = {{ain[0], 0, 0}, {ain[1], 0, 0}, {ain[2], 0, 0}};
                const int32_t ad[3] = {ain[3], 0, 0};
                int32_t o[3];
                WlTevHalf h = s->alpha;
                h.op = (uint8_t)(h.op == WL_TEV_COMP_A8_EQ ? WL_TEV_COMP_R8_EQ : WL_TEV_COMP_R8_GT);
                compare(ac[0], ac[1], ac[2], ad, &h, o);
                aout = o[0];
            } else {
                aout = blend(ain[0], ain[1], ain[2], ain[3], &s->alpha);
            }

            // write after every input was read
            for (uint32_t c = 0; c < 3; c++) {
                reg[s->color.dest][c] = cout[c];
            }
            reg[s->alpha.dest][3] = aout;
        }
    } else {
        memset(reg, 0, sizeof reg);
    }
    for (uint32_t r = 0; r < WL_TEV_REG_COUNT; r++) {
        regs[r].r = (int16_t)reg[r][0];
        regs[r].g = (int16_t)reg[r][1];
        regs[r].b = (int16_t)reg[r][2];
        regs[r].a = (int16_t)reg[r][3];
    }
}

uint32_t wl_tev_eval_rgba(const WlTevProgram *p, const WlTevPixel *px) {
    WlTevRegValue regs[WL_TEV_REG_COUNT];
    wl_tev_eval(p, px, regs);
    const WlTevRegValue *v = &regs[WL_TEV_PREV];
    return (uint32_t)clamp_value(v->r, true) << 24 | (uint32_t)clamp_value(v->g, true) << 16 |
           (uint32_t)clamp_value(v->b, true) << 8 | (uint32_t)clamp_value(v->a, true);
}

// ---- the fixed recipes --------------------------------------------------------------------------

void wl_tev_recipe(WlTevProgram *p, WlGxRecipe recipe) {
    wl_tev_program_init(p);
    wl_tev_konst_external(p, 0, 0xFFFFFFFFu);
    WlTevStage *s = wl_tev_add_stage(p);
    // (the first stage of a recipe is always one a program can have)
    switch (recipe) {
    case WL_GX_RECIPE_TEX_REPLACE:
        wl_tev_stage_order(s, 0, 0, WL_TEV_NONE);
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        break;
    case WL_GX_RECIPE_TEX_MODULATE:
    case WL_GX_RECIPE_TEX_LIT:
    case WL_GX_RECIPE_TEX_TINT:
    case WL_GX_RECIPE_UI:
        wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_ZERO, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        if (recipe == WL_GX_RECIPE_TEX_TINT) { // previous * konst, colour and alpha
            s = wl_tev_add_stage(p);
            s->kcsel = wl_tev_kcsel(0, WL_TEV_K_RGB);
            s->kasel = wl_tev_kasel(0, WL_TEV_K_A);
            wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_KONST, WL_TEV_CC_ZERO, WL_TEV_ADD,
                               WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
            wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_ADD,
                               WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        } else if (recipe == WL_GX_RECIPE_UI) { // alpha times konst alpha
            s = wl_tev_add_stage(p);
            s->kasel = wl_tev_kasel(0, WL_TEV_K_A);
            wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_ADD,
                               WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
            wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_CA_KONST, WL_TEV_CA_ZERO, WL_TEV_ADD,
                               WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        }
        break;
    case WL_GX_RECIPE_ADDITIVE: // rgb = tex * ras * ras.a, alpha 1
        wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
        s->kasel = WL_TEV_KSEL_1;
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_CC_RASC, WL_TEV_CC_ZERO, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        s = wl_tev_add_stage(p);
        wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR0A0);
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_CC_RASA, WL_TEV_CC_ZERO, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_APREV, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        break;
    case WL_GX_RECIPE_TEXT: // rgb from the vertex colour, alpha = glyph coverage * vertex alpha
        wl_tev_stage_order(s, 0, 0, WL_TEV_CH_COLOR0A0);
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_ZERO, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        break;
    case WL_GX_RECIPE_KONST:
        s->kcsel = wl_tev_kcsel(0, WL_TEV_K_RGB);
        s->kasel = wl_tev_kasel(0, WL_TEV_K_A);
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_KONST, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        break;
    case WL_GX_RECIPE_VERTEX_COLOR:
    default:
        wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_CH_COLOR0A0);
        wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_RASC, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_RASA, WL_TEV_ADD,
                           WL_TEV_BIAS_ZERO, WL_TEV_SCALE_1, true, WL_TEV_PREV);
        break;
    }
}

// ---- applying -----------------------------------------------------------------------------------

WlResult wl_tev_apply_ex(const WlTevProgram *p, uint32_t flags) {
    if (!wl_tev_validate(p, NULL)) {
        return WL_ERR_ARG;
    }
    wl_tev_apply_unchecked_ex(p, flags);
    return WL_OK;
}

WlResult wl_tev_apply(const WlTevProgram *p) {
    return wl_tev_apply_ex(p, 0);
}

void wl_tev_apply_unchecked(const WlTevProgram *p) {
    wl_tev_apply_unchecked_ex(p, 0);
}
