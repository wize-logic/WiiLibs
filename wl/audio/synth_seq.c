// SPDX-License-Identifier: MIT
// SSEQ synth sequencer - the track interpreter. Command numbers and parameter layouts
// follow the community SSEQ notes (wl/audio/README.md lists what each command group does here and
// which parts are interpretation).
//
// Track timing: every tick a track with a wait counts it down and, at zero, runs commands until one
// waits (a rest, or a note while "note wait" is on). Note lengths count down per tick as well and
// run first, so a note of N ticks followed by a rest of N ticks releases exactly when the next note
// starts.
#include <string.h>

#include "wl/audio/synth_internal.h"
#include "wl/base/endian.h"

void wl_syn_emit(WlSynth *s, uint32_t kind, uint32_t track, uint32_t voice, int32_t a, int32_t b,
                 int32_t c) {
    if (s->trace != NULL) {
        WlSynEvent e;
        e.kind = (uint8_t)kind;
        e.track = (uint8_t)track;
        e.voice = (uint8_t)voice;
        e.tick = s->tick;
        e.a = a;
        e.b = b;
        e.c = c;
        s->trace(s->trace_user, &e);
    }
}

void wl_syn_track_reset(WlSynTrack *t) {
    memset(t, 0, sizeof *t);
    t->notewait = true;
    t->vol = 127;
    t->expr = 127;
    t->bend_range = 2;
    t->prio = 64;
    t->porta_key = 60;
    t->mod_speed = 16;
    t->mod_range = 1;
    t->atk = t->dec = t->sus = t->rel = 255;
    t->tie_voice = -1;
}

// ---- operand reading --------------------------------------------------------------------------

typedef struct {
    WlSynth *s;
    WlSynTrack *t;
    bool err; // ran off the end of the sequence
} Rd;

static uint32_t u8(Rd *r) {
    if (r->t->pc >= r->s->seq.data_size) {
        r->err = true;
        return 0;
    }
    return r->s->seq.data[r->t->pc++];
}

static uint32_t u16(Rd *r) {
    const uint32_t lo = u8(r);
    return lo | (u8(r) << 8);
}

static uint32_t u24(Rd *r) {
    const uint32_t lo = u16(r);
    return lo | (u8(r) << 16);
}

static int32_t s16(Rd *r) {
    return (int16_t)(uint16_t)u16(r);
}

// Variable length number: 7 bits per byte, most significant group first, bit 7 = more.
static int32_t vlq(Rd *r) {
    int32_t v = 0;
    for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t b = u8(r);
        v = (v << 7) | (int32_t)(b & 127u);
        if (!(b & 128u)) {
            break;
        }
    }
    return v;
}

static int32_t var_get(const WlSynth *s, uint32_t i) {
    if (i < 16u) {
        return s->vars[i];
    }
    return i < WL_SYN_VARS ? s->gvars[i - 16u] : 0;
}

static void var_put(WlSynth *s, uint32_t i, int32_t v) {
    const int16_t w = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    if (i < 16u) {
        s->vars[i] = w;
    } else if (i < WL_SYN_VARS) {
        s->gvars[i - 16u] = w;
    }
}

// Inclusive random number in [lo, hi].
static int32_t rnd(WlSynth *s, int32_t lo, int32_t hi) {
    if (hi < lo) {
        const int32_t t = lo;
        lo = hi;
        hi = t;
    }
    s->rng = s->rng * 1664525u + 1013904223u;
    const uint32_t r = s->rng >> 16; // 0..65535
    return lo + (int32_t)((((uint32_t)(hi - lo) + 1u) * r) >> 16);
}

enum { MOD_NONE = 0, MOD_RAND, MOD_VAR };
enum { P_U8 = 0, P_S8, P_U16, P_S16, P_VLQ };

// The last parameter of a command: as stored, or replaced by a random range (two s16) or a variable
// (one u8 index) when a prefix said so.
static int32_t param(Rd *r, int mod, int kind) {
    if (mod == MOD_RAND) {
        const int32_t lo = s16(r);
        const int32_t hi = s16(r);
        return rnd(r->s, lo, hi);
    }
    if (mod == MOD_VAR) {
        return var_get(r->s, u8(r));
    }
    switch (kind) {
    case P_U8:
        return (int32_t)u8(r);
    case P_S8:
        return (int8_t)u8(r);
    case P_U16:
        return (int32_t)u16(r);
    case P_S16:
        return s16(r);
    default:
        return vlq(r);
    }
}

static int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// ---- notes --------------------------------------------------------------------------------------

// `key` is final (transposed). The portamento start key (t->porta_key) is the previous note's.
static void note_on_inner(WlSynth *s, uint32_t ti, WlSynTrack *t, int32_t key, int32_t vel,
                          int32_t dur) {

    // Tie: a note that follows a held note on the same track continues it at the new key.
    if (t->tie && t->tie_voice >= 0) {
        WlSynVoice *v = &s->voice[t->tie_voice];
        if (v->state != WL_SYN_V_OFF && v->state != WL_SYN_V_RELEASE && v->order == t->tie_order) {
            const int32_t old_pitch = v->base_pitch + v->sweep_pitch;
            v->key = (uint8_t)key;
            v->vel = (uint8_t)vel;
            v->base_pitch = (key - (int32_t)v->base_key) * 64;
            v->len = dur;
            v->held = false;
            if (t->porta) {
                v->sweep_pitch = old_pitch - v->base_pitch;
                v->sweep_start_tick = s->tick;
                v->age_frames = 0;
                v->sweep_ticks = (uint32_t)dur;
                v->sweep_frames = 0;
                if (t->porta_time != 0) {
                    const int32_t a = v->sweep_pitch < 0 ? -v->sweep_pitch : v->sweep_pitch;
                    v->sweep_frames = ((uint32_t)t->porta_time * t->porta_time * (uint32_t)a) >> 11;
                }
            } else {
                v->sweep_pitch = t->sweep;
                v->sweep_start_tick = s->tick;
                v->age_frames = 0;
                v->sweep_ticks = (uint32_t)dur;
                v->sweep_frames = 0;
            }
            s->stats.ties++;
            if (dur == 0) {
                wl_syn_voice_release(s, (uint32_t)t->tie_voice);
            }
            return;
        }
    }

    WlSynNote nt;
    WlSynWave wave;
    memset(&wave, 0, sizeof wave);
    if (!wl_syn_bank_note(s->bank, t->prog, (uint32_t)key, &nt)) {
        s->stats.notes_silent++;
        return;
    }
    if (nt.type == WL_SYN_NOTE_PCM &&
        !wl_syn_wave_get(&s->bank->war[nt.slot & 3u], nt.wave, &wave)) {
        s->stats.notes_silent++;
        return;
    }
    const int32_t vi = wl_syn_voice_start(s, ti, &nt, &wave, (uint32_t)key, (uint32_t)vel, dur);
    if (vi < 0) {
        s->stats.notes_dropped++;
        return;
    }
    s->stats.notes++;
    t->tie_voice = t->tie ? (int8_t)vi : (int8_t)-1;
    t->tie_order = s->voice[vi].order;
    wl_syn_emit(s, WL_SYN_EV_NOTE_ON, ti, (uint32_t)vi, key, vel, dur);
}

static void note_on(WlSynth *s, uint32_t ti, WlSynTrack *t, int32_t key, int32_t vel, int32_t dur) {
    key = clamp_i(key + t->transpose, 0, 127);
    vel = clamp_i(vel, 0, 127);
    if (dur < 0) {
        dur = 0;
    }
    note_on_inner(s, ti, t, key, vel, dur);
    t->porta_key = (uint8_t)key;
}

// ---- commands -----------------------------------------------------------------------------------

// Ends the tie of a track: a held voice is released now.
static void untie(WlSynth *s, uint32_t ti) {
    WlSynTrack *t = &s->trk[ti];
    if (t->tie_voice >= 0) {
        WlSynVoice *v = &s->voice[t->tie_voice];
        if (v->held && v->order == t->tie_order) {
            v->held = false;
            wl_syn_emit(s, WL_SYN_EV_RELEASE, ti, (uint32_t)t->tie_voice, 0, 0, 0);
            wl_syn_voice_release(s, (uint32_t)t->tie_voice);
        }
    }
    t->tie_voice = -1;
}

static void end_track(WlSynth *s, uint32_t ti) {
    WlSynTrack *t = &s->trk[ti];
    if (t->active) {
        untie(s, ti);
        t->active = false;
        wl_syn_emit(s, WL_SYN_EV_TRACK_END, ti, 0, 0, 0, 0);
    }
}

static void jump_to(WlSynth *s, uint32_t ti, uint32_t off) {
    if (off >= s->seq.data_size) {
        end_track(s, ti);
        return;
    }
    s->trk[ti].pc = off;
}

// Variable commands B0..BD: var, value.
static void var_cmd(WlSynth *s, WlSynTrack *t, uint32_t op, uint32_t idx, int32_t val, bool skip) {
    if (skip || idx >= WL_SYN_VARS) {
        return;
    }
    const int32_t cur = var_get(s, idx);
    switch (op) {
    case 0xB0:
        var_put(s, idx, val);
        break;
    case 0xB1:
        var_put(s, idx, cur + val);
        break;
    case 0xB2:
        var_put(s, idx, cur - val);
        break;
    case 0xB3:
        var_put(s, idx, cur * val);
        break;
    case 0xB4:
        if (val != 0) {
            var_put(s, idx, cur / val);
        }
        break;
    case 0xB5: // shift: positive left, negative right
        if (val >= 0) {
            var_put(s, idx, val > 15 ? 0 : (int32_t)((uint32_t)cur << val));
        } else {
            var_put(s, idx, -val > 31 ? (cur < 0 ? -1 : 0) : cur >> (-val));
        }
        break;
    case 0xB6: // random in 0..val (or 0..-val negated)
        var_put(s, idx, val >= 0 ? rnd(s, 0, val) : -rnd(s, 0, -val));
        break;
    case 0xB8:
        t->cond = cur == val;
        break;
    case 0xB9:
        t->cond = cur >= val;
        break;
    case 0xBA:
        t->cond = cur > val;
        break;
    case 0xBB:
        t->cond = cur <= val;
        break;
    case 0xBC:
        t->cond = cur < val;
        break;
    case 0xBD:
        t->cond = cur != val;
        break;
    default:
        break;
    }
}

static void exec_one(WlSynth *s, uint32_t ti) {
    WlSynTrack *t = &s->trk[ti];
    Rd r = {s, t, false};
    int mod = MOD_NONE;
    bool skip = false;
    uint32_t op = u8(&r);
    while (!r.err && (op == 0xA0u || op == 0xA1u || op == 0xA2u)) { // prefixes
        if (s->op_hist != NULL) {
            s->op_hist[op]++;
        }
        if (op == 0xA0u) {
            mod = MOD_RAND;
        } else if (op == 0xA1u) {
            mod = MOD_VAR;
        } else {
            skip = !t->cond;
        }
        op = u8(&r);
    }
    if (r.err) {
        end_track(s, ti);
        return;
    }
    if (s->op_hist != NULL) {
        s->op_hist[op]++;
    }

    if (op < 0x80u) { // note: velocity, length
        const int32_t vel = (int32_t)u8(&r);
        const int32_t dur = param(&r, mod, P_VLQ);
        if (r.err) {
            end_track(s, ti);
            return;
        }
        if (!skip) {
            note_on(s, ti, t, (int32_t)op, vel, dur);
            if (t->notewait) {
                t->wait = dur < 0 ? 0 : dur;
            }
        }
        return;
    }

    switch (op) {
    case 0x80: { // rest
        const int32_t d = param(&r, mod, P_VLQ);
        if (!skip && !r.err) {
            t->wait = d < 0 ? 0 : d;
        }
        break;
    }
    case 0x81: { // program change
        const int32_t p = param(&r, mod, P_VLQ);
        if (!skip && !r.err) {
            t->prog = (uint16_t)clamp_i(p, 0, 16383);
            wl_syn_emit(s, WL_SYN_EV_PROGRAM, ti, 0, p, 0, 0);
        }
        break;
    }
    case 0x93: { // open track: track number, offset
        const uint32_t n = u8(&r);
        const uint32_t off = u24(&r);
        if (!skip && !r.err && n < WL_SYN_TRACKS && off < s->seq.data_size) {
            WlSynTrack *nt = &s->trk[n];
            wl_syn_track_reset(nt);
            nt->active = true;
            nt->pc = off;
        }
        break;
    }
    case 0x94: { // jump
        const uint32_t off = u24(&r);
        if (!skip && !r.err) {
            jump_to(s, ti, off);
            return;
        }
        break;
    }
    case 0x95: { // call
        const uint32_t off = u24(&r);
        if (!skip && !r.err) {
            if (t->call_depth < WL_SYN_STACK) {
                t->call_pc[t->call_depth++] = t->pc;
                jump_to(s, ti, off);
                return;
            }
        }
        break;
    }
    case 0xFD: // return
        if (!skip && t->call_depth > 0) {
            t->pc = t->call_pc[--t->call_depth];
        }
        break;
    case 0xD4: { // loop start: pass count (0 = forever)
        const int32_t n = param(&r, mod, P_U8);
        if (!skip && !r.err && t->loop_depth < WL_SYN_STACK) {
            t->loop_pc[t->loop_depth] = t->pc;
            t->loop_cnt[t->loop_depth] = (uint8_t)n;
            t->loop_depth++;
        }
        break;
    }
    case 0xFC: // loop end
        if (!skip && t->loop_depth > 0) {
            const uint32_t d = t->loop_depth - 1u;
            if (t->loop_cnt[d] == 0) {
                t->pc = t->loop_pc[d];
            } else if (--t->loop_cnt[d] > 0) {
                t->pc = t->loop_pc[d];
            } else {
                t->loop_depth--;
            }
        }
        break;
    case 0xFE: // allocate tracks (bitmask); this player has all sixteen
        (void)u16(&r);
        break;
    case 0xFF: // end of track
        if (!skip) {
            end_track(s, ti);
            return;
        }
        break;
    case 0xC0: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->pan = (int8_t)clamp_i(v - 64, -64, 63);
        }
        break;
    }
    case 0xC1: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->vol = (uint8_t)clamp_i(v, 0, 127);
        }
        break;
    }
    case 0xC2: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            s->master = (uint8_t)clamp_i(v, 0, 127);
        }
        break;
    }
    case 0xC3: {
        const int32_t v = param(&r, mod, P_S8);
        if (!skip) {
            t->transpose = (int8_t)clamp_i(v, -128, 127);
        }
        break;
    }
    case 0xC4: {
        const int32_t v = param(&r, mod, P_S8);
        if (!skip) {
            t->bend = (int8_t)clamp_i(v, -128, 127);
        }
        break;
    }
    case 0xC5: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->bend_range = (uint8_t)clamp_i(v, 0, 255);
        }
        break;
    }
    case 0xC6: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->prio = (uint8_t)clamp_i(v, 0, 255);
        }
        break;
    }
    case 0xC7: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->notewait = v != 0;
        }
        break;
    }
    case 0xC8: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->tie = v != 0;
            if (!t->tie) {
                untie(s, ti);
            }
        }
        break;
    }
    case 0xC9: { // portamento control: the key to glide from; turns portamento on
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->porta_key = (uint8_t)clamp_i(v, 0, 127);
            t->porta = true;
        }
        break;
    }
    case 0xCA: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->mod_depth = (uint8_t)clamp_i(v, 0, 255);
        }
        break;
    }
    case 0xCB: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->mod_speed = (uint8_t)clamp_i(v, 0, 255);
        }
        break;
    }
    case 0xCC: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->mod_type = (uint8_t)clamp_i(v, 0, 2);
        }
        break;
    }
    case 0xCD: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->mod_range = (uint8_t)clamp_i(v, 0, 255);
        }
        break;
    }
    case 0xCE: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->porta = v != 0;
        }
        break;
    }
    case 0xCF: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->porta_time = (uint8_t)clamp_i(v, 0, 255);
        }
        break;
    }
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            const uint8_t b = (uint8_t)clamp_i(v, 0, 127);
            if (op == 0xD0) {
                t->atk = b;
            } else if (op == 0xD1) {
                t->dec = b;
            } else if (op == 0xD2) {
                t->sus = b;
            } else {
                t->rel = b;
            }
        }
        break;
    }
    case 0xD5: {
        const int32_t v = param(&r, mod, P_U8);
        if (!skip) {
            t->expr = (uint8_t)clamp_i(v, 0, 127);
        }
        break;
    }
    case 0xD6: // print variable: a debugging aid of the original tools
        (void)param(&r, mod, P_U8);
        break;
    case 0xE0: {
        const int32_t v = param(&r, mod, P_U16);
        if (!skip) {
            t->mod_delay = (uint16_t)clamp_i(v, 0, 65535);
        }
        break;
    }
    case 0xE1: {
        const int32_t v = param(&r, mod, P_U16);
        if (!skip) {
            s->tempo = (uint16_t)clamp_i(v, 1, 1023);
        }
        break;
    }
    case 0xE3: {
        const int32_t v = param(&r, mod, P_S16);
        if (!skip) {
            t->sweep = (int16_t)clamp_i(v, -32768, 32767);
        }
        break;
    }
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD: {
        const uint32_t idx = u8(&r);
        const int32_t val = param(&r, mod, P_S16);
        if (!r.err) {
            var_cmd(s, t, op, idx, val, skip);
        }
        break;
    }
    default:
        s->stats.unknown_cmds++;
        s->stats.last_unknown = (uint8_t)op;
        end_track(s, ti);
        return;
    }
    if (r.err) {
        end_track(s, ti);
    }
}

static void track_run(WlSynth *s, uint32_t ti) {
    WlSynTrack *t = &s->trk[ti];
    uint32_t budget = WL_SYN_RUNAWAY;
    while (t->active && t->wait == 0) {
        if (budget-- == 0) {
            s->stats.runaway++;
            t->wait = 1;
            break;
        }
        exec_one(s, ti);
    }
}

void wl_syn_tick(WlSynth *s) {
    s->tick++;
    s->stats.ticks++;
    wl_syn_emit(s, WL_SYN_EV_TICK, 0, 0, s->tempo, 0, 0);
    // Note lengths first.
    for (uint32_t i = 0; i < WL_SYN_VOICES; i++) {
        WlSynVoice *v = &s->voice[i];
        if (v->state != WL_SYN_V_OFF && v->state != WL_SYN_V_RELEASE && v->len > 0) {
            if (--v->len == 0) {
                const WlSynTrack *t = &s->trk[v->track];
                if (v->tied && t->tie && t->tie_voice == (int8_t)i && t->tie_order == v->order) {
                    v->held = true; // a tied note rings on until the next note takes it over or the
                                    // tie ends
                } else {
                    wl_syn_emit(s, WL_SYN_EV_RELEASE, v->track, i, 0, 0, 0);
                    wl_syn_voice_release(s, i);
                }
            }
        }
    }
    for (uint32_t ti = 0; ti < WL_SYN_TRACKS; ti++) {
        WlSynTrack *t = &s->trk[ti];
        if (!t->active) {
            continue;
        }
        if (t->wait > 0) {
            t->wait--;
        }
        if (t->wait == 0) {
            track_run(s, ti);
        }
    }
}
