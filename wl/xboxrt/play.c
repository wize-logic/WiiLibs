// SPDX-License-Identifier: MIT
// The play harness (play.h). The script language, its steps' timing (a press of four reads and
// 100 ms, the bot's formulas, waits that give up and say where the script was) and the logging come
// from OpenCE-Wii's port/linux/src/test_input.c (CC0); the observation of the game is the title's,
// through the registrations, and the controller is the ports of input.h.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/mem_census.h"
#include "wl/platform/platform.h"
#include "wl/xboxrt/devices.h"
#include "wl/xboxrt/input.h"
#include "wl/xboxrt/irq.h"
#include "wl/xboxrt/mem.h"
#include "wl/xboxrt/mem_internal.h"
#include "wl/xboxrt/play.h"
#include "wl/xboxrt/thread.h"

#if defined(WL_XBRT_HAVE_GXSOFT)
#include "wl/gxsoft/gxsoft.h"
#include "wl/gxtex/gxtex.h"
#endif

#define TAG "play"
#define MAX_STEPS 1024
#define MAX_NESTING 8
#define TEXT_SIZE 160
#define OBSERVED_SIZE 160
#define DEFAULT_TIMEOUT 120.0
#define PRESS_REPORTS 4u
#define PRESS_MS 100u
#define PRESS_FRAMES 2
#define SELECT_MAX 16
#define VBLANK_HZ 59.94
#define MAX_REG 96 // a title's texts, numbers, predicates, actions and steps (Halo's plugin has 32)
#define PLAY_PRIORITY 31
#define TICK_JUMP 60 // a tick moving on more than this in a frame was a checkpoint loaded, not play
#define PROGRESS_SECONDS 60 // a line says the time played every game minute (host_run.py's progress)
#define STILL_FACTOR 4.0 // a bot step whose game time stands still ends after this many times its length

enum {
    K_WAIT,
    K_FRAMES,
    K_VBLANKS,
    K_PRESS,
    K_HOLD,
    K_DOWN,
    K_UP,
    K_RELEASE,
    K_STICK,
    K_TRIGGER,
    K_SCREEN,
    K_FOCUS,
    K_SELECT,
    K_PLAY,
    K_TICKS,
    K_CHECKPOINT,
    K_UNTIL,
    K_DO,
    K_MEMORY,
    K_PEEK,
    K_BOT,
    K_LOOK,
    K_TITLE_STEP,
    K_LOG,
    K_SCREENSHOT,
    K_CAPTURE,
    K_DUMP,
    K_CONNECT,
    K_BOT_PLAYER,
    K_QUIT,
    K_REPEAT, // only while parsing
    K_END,
};

typedef enum { DUR_SECONDS, DUR_FRAMES, DUR_TICKS } DurUnit;

typedef struct {
    int kind;
    int line;
    uint8_t player;
    char text[TEXT_SIZE];     // the line as written
    char arg[TEXT_SIZE];      // the text waited for, the predicate's argument, the log's text
    char name[32];            // a predicate's, action's or title step's name
    double number;            // seconds, a count, a timeout
    DurUnit unit;
    double sticks[4];
    uint16_t digital;
    uint8_t analog;
    uint8_t stick_dirs;       // lup ldown lleft lright rup rdown rleft rright
    // memory
    uint32_t address, mask, value;
    uint8_t size, op;
} Step;

typedef enum { REG_TEXT, REG_NUMBER, REG_PREDICATE, REG_ACTION, REG_STEP } RegKind;
typedef struct {
    RegKind kind;
    char name[24];
    void *fn;
    WlXbPlayStepType step;
    void *user;
} Reg;

static Reg g_reg[MAX_REG];
static int g_reg_count;
static WlXbPlayFrameFn g_frame_hook;
static void *g_frame_user;

enum { MODE_NONE, MODE_BOT, MODE_LOOK, MODE_SCRIPT };

static struct {
    int mode, seed;
    bool active;
    char base_dir[WL_FS_PATH_MAX], out_dir[WL_FS_PATH_MAX], script_path[WL_FS_PATH_MAX];
    WlXbThread *thread;
    volatile bool frame_point; // the title drives the frames
    uint32_t frames;
    uint32_t last_vblank;
    // the script
    Step *steps;
    int count, current;
    bool started, finished;
    double clock;              // the script's seconds (frames / the vertical blank's rate)
    double started_clock;
    uint32_t started_frame, started_vblank;
    int64_t started_tick, started_checkpoints, scratch;
    // the game's time played: the title's tick, but never back (a revert to a checkpoint puts the
    // tick back; this goes on), so a step's time and the bot's clock move on through deaths
    int64_t played, started_played, last_game_tick;
    int64_t progress_next;     // the time played at which the next progress line is logged
    uint32_t reverts;
    char map[64];
    bool pressed;
    uint32_t phase_reports;
    uint64_t phase_us;
    int64_t phase_frame;
    int presses;
    bool look;                 // select: look at the focus before the next press
    bool first;                // a predicate's first call of its step
    // latched buttons per port
    WlXboxPad latched[WL_XB_PLAY_PORTS];
    // the observed title
    char screen[OBSERVED_SIZE], focus[OBSERVED_SIZE];
    int64_t tick, checkpoints;
    bool observed;
    // controllers 2..4
    int bots[WL_XB_PLAY_PORTS];  // seed + 1, 0 none
    // the output this frame
    WlXboxPad out[WL_XB_PLAY_PORTS];
    // a title step
    WlXbPlayStep tstep;
    const Reg *treg;
    // a screenshot in progress (the host)
    char shot_path[WL_FS_PATH_MAX];
    int shot_frames;
    uint32_t shots;
} P;

static const struct {
    const char *name;
    uint16_t digital;
    int analog;
    uint8_t stick;
} k_buttons[] = {
    {"a", 0, WL_XBOX_PAD_A, 0},          {"b", 0, WL_XBOX_PAD_B, 0},
    {"x", 0, WL_XBOX_PAD_X, 0},          {"y", 0, WL_XBOX_PAD_Y, 0},
    {"black", 0, WL_XBOX_PAD_BLACK, 0},  {"white", 0, WL_XBOX_PAD_WHITE, 0},
    {"lt", 0, WL_XBOX_PAD_LT, 0},        {"rt", 0, WL_XBOX_PAD_RT, 0},
    {"up", WL_XBOX_PAD_UP, -1, 0},       {"down", WL_XBOX_PAD_DOWN, -1, 0},
    {"left", WL_XBOX_PAD_LEFT, -1, 0},   {"right", WL_XBOX_PAD_RIGHT, -1, 0},
    {"start", WL_XBOX_PAD_START, -1, 0}, {"back", WL_XBOX_PAD_BACK, -1, 0},
    {"lthumb", WL_XBOX_PAD_LTHUMB, -1, 0}, {"rthumb", WL_XBOX_PAD_RTHUMB, -1, 0},
    {"lup", 0, -1, 0x01},  {"ldown", 0, -1, 0x02}, {"lleft", 0, -1, 0x04}, {"lright", 0, -1, 0x08},
    {"rup", 0, -1, 0x10},  {"rdown", 0, -1, 0x20}, {"rleft", 0, -1, 0x40}, {"rright", 0, -1, 0x80},
};

// ---- logging -----------------------------------------------------------------------------------

void wl_xb_play_log(const char *fmt, ...) {
    char line[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    WL_LOG_I(TAG, "test_input: %s", line);
}

uint32_t wl_xb_play_frames(void) {
    return P.frames;
}

bool wl_xb_play_active(void) {
    return P.active;
}

// ---- registrations -----------------------------------------------------------------------------

static const Reg *reg_find(RegKind kind, const char *name) {
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg[i].kind == kind && strcmp(g_reg[i].name, name) == 0) {
            return &g_reg[i];
        }
    }
    return NULL;
}

static WlResult reg_add(RegKind kind, const char *name, void *fn, void *user, const WlXbPlayStepType *step) {
    if (name == NULL || (fn == NULL && step == NULL) || strlen(name) >= sizeof g_reg[0].name) {
        return WL_ERR_ARG;
    }
    Reg *r = (Reg *)reg_find(kind, name);
    if (r == NULL) {
        if (g_reg_count == MAX_REG) {
            // said aloud: a title's step or action that is quietly missing reads later as "not an action"
            wl_xb_play_log("cannot register \"%s\": all %d places are taken (MAX_REG)", name, MAX_REG);
            return WL_ERR_LIMIT;
        }
        r = &g_reg[g_reg_count++];
    }
    memset(r, 0, sizeof *r);
    r->kind = kind;
    snprintf(r->name, sizeof r->name, "%s", name);
    r->fn = fn;
    r->user = user;
    if (step != NULL) {
        r->step = *step;
    }
    return WL_OK;
}

WlResult wl_xb_play_register_text(const char *name, WlXbPlayTextFn fn, void *user) {
    return reg_add(REG_TEXT, name, (void *)fn, user, NULL);
}

WlResult wl_xb_play_register_number(const char *name, WlXbPlayNumberFn fn, void *user) {
    return reg_add(REG_NUMBER, name, (void *)fn, user, NULL);
}

WlResult wl_xb_play_register_predicate(const char *name, WlXbPlayPredicateFn fn, void *user) {
    return reg_add(REG_PREDICATE, name, (void *)fn, user, NULL);
}

WlResult wl_xb_play_register_action(const char *name, WlXbPlayActionFn fn, void *user) {
    return reg_add(REG_ACTION, name, (void *)fn, user, NULL);
}

WlResult wl_xb_play_register_step(const char *name, const WlXbPlayStepType *type, void *user) {
    if (type == NULL || type->frame == NULL) {
        return WL_ERR_ARG;
    }
    return reg_add(REG_STEP, name, NULL, user, type);
}

void wl_xb_play_set_frame_hook(WlXbPlayFrameFn fn, void *user) {
    g_frame_user = user;
    g_frame_hook = fn;
}

bool wl_xb_play_number(const char *name, int64_t *out) {
    const Reg *r = reg_find(REG_NUMBER, name);
    return r != NULL && ((WlXbPlayNumberFn)r->fn)(r->user, out);
}

bool wl_xb_play_text(const char *name, char *out, size_t cap) {
    const Reg *r = reg_find(REG_TEXT, name);
    if (r == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    return ((WlXbPlayTextFn)r->fn)(r->user, out, cap);
}

// ---- the bot -----------------------------------------------------------------------------------

void wl_xb_play_bot(WlXboxPad *pad, uint64_t t_ms, int seed, bool look) {
    double t = (double)t_ms / 1000.0 + seed * 1.7;
    if (look) {
        pad->thumb[WL_XBOX_PAD_RX] = (int16_t)(sin(t * 0.5) * 14000.0);
        pad->thumb[WL_XBOX_PAD_RY] = (int16_t)(sin(t * 0.3) * 32000.0);
        return;
    }
    pad->thumb[WL_XBOX_PAD_LY] = (int16_t)(sin(t * 0.9) * 32000.0);
    pad->thumb[WL_XBOX_PAD_LX] = (int16_t)(cos(t * 0.6 + seed) * 20000.0);
    pad->thumb[WL_XBOX_PAD_RX] = (int16_t)(sin(t * 0.4) * 14000.0);
    if (fmod(t, 3.0) < 0.3) {
        pad->analog[WL_XBOX_PAD_RT] = 255;
    }
    if (fmod(t, 5.0) < 0.1) {
        pad->analog[WL_XBOX_PAD_A] = 255;
    }
    if (fmod(t, 7.0) < 0.2) {
        pad->analog[WL_XBOX_PAD_LT] = 255;
    }
}

// ---- parsing -----------------------------------------------------------------------------------

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// whether `text` holds `part`, in any case
static bool contains(const char *text, const char *part) {
    size_t n = strlen(part), len = strlen(text);
    for (size_t at = 0; at + n <= len; at++) {
        size_t i = 0;
        while (i < n && lower(text[at + i]) == lower(part[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static bool parse_buttons(const char *text, Step *s) {
    while (*text) {
        const char *end = strchr(text, '+');
        size_t len = end ? (size_t)(end - text) : strlen(text);
        size_t i;
        for (i = 0; i < WL_ARRAY_COUNT(k_buttons); i++) {
            if (strlen(k_buttons[i].name) == len && strncmp(k_buttons[i].name, text, len) == 0) {
                break;
            }
        }
        if (i == WL_ARRAY_COUNT(k_buttons)) {
            return false;
        }
        s->digital |= k_buttons[i].digital;
        if (k_buttons[i].analog >= 0) {
            s->analog |= (uint8_t)(1u << k_buttons[i].analog);
        }
        s->stick_dirs |= k_buttons[i].stick;
        text += len;
        if (*text == '+') {
            text++;
        }
    }
    return true;
}

static bool parse_number(const char *w, double *out) {
    char *end;
    double v = strtod(w, &end);
    if (end == w || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_duration(const char *w, Step *s) {
    char buf[32];
    size_t n = strlen(w);
    if (n == 0 || n >= sizeof buf) {
        return false;
    }
    memcpy(buf, w, n + 1);
    s->unit = DUR_SECONDS;
    if (buf[n - 1] == 'f') {
        s->unit = DUR_FRAMES;
        buf[n - 1] = '\0';
    } else if (buf[n - 1] == 't') {
        s->unit = DUR_TICKS;
        buf[n - 1] = '\0';
    } else if (buf[n - 1] == 's') {
        buf[n - 1] = '\0';
    }
    return parse_number(buf, &s->number);
}

static bool parse_u32(const char *w, uint32_t *out) {
    char *end;
    unsigned long v = strtoul(w, &end, 0);
    if (end == w || *end != '\0') {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static int controller_number(const char *w, int lowest) {
    if (w[0] < '0' + lowest || w[0] > '0' + (int)WL_XB_PLAY_PORTS || w[1] != '\0') {
        return 0;
    }
    return w[0] - '0';
}

static int g_parse_player;

// one line into a step: 0 for a blank line or a comment, -1 for a fault
static int parse_line(char *line, int number, Step *s) {
    char *words[10] = {NULL};
    int count = 0;
    int player = g_parse_player;
    char *c = strchr(line, '#');
    if (c != NULL) {
        *c = '\0';
    }
    for (c = line + strlen(line); c > line && (unsigned char)c[-1] <= ' '; c--) {
        c[-1] = '\0';
    }
    for (c = line; *c && (unsigned char)*c <= ' '; c++) {
    }
    if (*c == '\0') {
        return 0;
    }
    memset(s, 0, sizeof *s);
    s->line = number;
    if (c[0] >= '1' && c[0] <= '0' + (int)WL_XB_PLAY_PORTS && c[1] == ':') {
        player = c[0] - '1';
        for (c += 2; *c && (unsigned char)*c <= ' '; c++) {
        }
        if (*c == '\0') {
            return -1;
        }
    }
    s->player = (uint8_t)player;
    snprintf(s->text, sizeof s->text, "%s", c);
    if (strncmp(c, "log", 3) == 0 && (unsigned char)c[3] <= ' ') {
        s->kind = K_LOG;
        for (c += 3; *c && (unsigned char)*c <= ' '; c++) {
        }
        snprintf(s->arg, sizeof s->arg, "%s", c);
        return 1;
    }
    while (*c && count < (int)WL_ARRAY_COUNT(words)) {
        words[count++] = c;
        while (*c && (unsigned char)*c > ' ') {
            c++;
        }
        while (*c && (unsigned char)*c <= ' ') {
            *c++ = '\0';
        }
    }
    const char *w0 = words[0];
    s->number = DEFAULT_TIMEOUT;
    if (strcmp(w0, "wait") == 0 && count == 2) {
        s->kind = K_WAIT;
        return parse_duration(words[1], s) ? 1 : -1;
    }
    if ((strcmp(w0, "frames") == 0 || strcmp(w0, "vblanks") == 0) && count == 2) {
        s->kind = w0[0] == 'f' ? K_FRAMES : K_VBLANKS;
        return parse_number(words[1], &s->number) ? 1 : -1;
    }
    if ((strcmp(w0, "screen") == 0 || strcmp(w0, "focus") == 0) && (count == 2 || count == 3)) {
        s->kind = w0[0] == 's' ? K_SCREEN : K_FOCUS;
        snprintf(s->arg, sizeof s->arg, "%s", words[1]);
        return count == 3 && !parse_number(words[2], &s->number) ? -1 : 1;
    }
    if ((strcmp(w0, "play") == 0 || strcmp(w0, "checkpoint") == 0) && count <= 2) {
        s->kind = w0[0] == 'p' ? K_PLAY : K_CHECKPOINT;
        return count == 2 && !parse_number(words[1], &s->number) ? -1 : 1;
    }
    if (strcmp(w0, "ticks") == 0 && (count == 2 || count == 3)) {
        s->kind = K_TICKS;
        double n;
        if (!parse_number(words[1], &n)) {
            return -1;
        }
        s->sticks[0] = n;
        return count == 3 && !parse_number(words[2], &s->number) ? -1 : 1;
    }
    if (strcmp(w0, "press") == 0 && (count == 2 || count == 3) && parse_buttons(words[1], s)) {
        s->kind = K_PRESS;
        s->number = 1.0;
        return count == 3 && !parse_number(words[2], &s->number) ? -1 : 1;
    }
    if (strcmp(w0, "select") == 0 && count >= 2 && count <= 4) {
        s->kind = K_SELECT;
        snprintf(s->arg, sizeof s->arg, "%s", words[1]);
        s->number = SELECT_MAX;
        if (!parse_buttons(count >= 3 ? words[2] : "down", s) || s->analog != 0) {
            return -1;
        }
        return count == 4 && !parse_number(words[3], &s->number) ? -1 : 1;
    }
    if (strcmp(w0, "hold") == 0 && count == 3 && parse_buttons(words[1], s)) {
        s->kind = K_HOLD;
        return parse_duration(words[2], s) ? 1 : -1;
    }
    if ((strcmp(w0, "down") == 0 || strcmp(w0, "up") == 0) && count == 2 && parse_buttons(words[1], s)) {
        s->kind = w0[0] == 'd' ? K_DOWN : K_UP;
        return 1;
    }
    if (strcmp(w0, "release") == 0 && count == 1) {
        s->kind = K_RELEASE;
        return 1;
    }
    if (strcmp(w0, "stick") == 0 && count == 6) {
        s->kind = K_STICK;
        for (int i = 0; i < 4; i++) {
            if (!parse_number(words[1 + i], &s->sticks[i])) {
                return -1;
            }
        }
        return parse_duration(words[5], s) ? 1 : -1;
    }
    if (strcmp(w0, "trigger") == 0 && count == 4) {
        s->kind = K_TRIGGER;
        if (!parse_number(words[1], &s->sticks[0]) || !parse_number(words[2], &s->sticks[1])) {
            return -1;
        }
        return parse_duration(words[3], s) ? 1 : -1;
    }
    if (strcmp(w0, "until") == 0 && count >= 2) {
        s->kind = K_UNTIL;
        snprintf(s->name, sizeof s->name, "%s", words[1]);
        int last = count;
        if (count >= 3 && parse_number(words[count - 1], &s->number)) {
            last = count - 1;
        }
        for (int i = 2; i < last; i++) {
            size_t used = strlen(s->arg);
            snprintf(s->arg + used, sizeof s->arg - used, "%s%s", i > 2 ? " " : "", words[i]);
        }
        return 1;
    }
    if ((strcmp(w0, "do") == 0 && count >= 2) || (strcmp(w0, "load") == 0 && count >= 2)) {
        s->kind = K_DO;
        int first = 1;
        if (w0[0] == 'd') {
            snprintf(s->name, sizeof s->name, "%s", words[1]);
            first = 2;
        } else {
            snprintf(s->name, sizeof s->name, "load");
        }
        for (int i = first; i < count; i++) {
            size_t used = strlen(s->arg);
            snprintf(s->arg + used, sizeof s->arg - used, "%s%s", i > first ? " " : "", words[i]);
        }
        return 1;
    }
    if ((strcmp(w0, "memory") == 0 && count >= 4) || (strcmp(w0, "peek") == 0 && count == 2)) {
        s->kind = w0[0] == 'm' ? K_MEMORY : K_PEEK;
        char addr[32];
        snprintf(addr, sizeof addr, "%s", words[1]);
        s->size = 4;
        char *colon = strchr(addr, ':');
        if (colon != NULL) {
            *colon = '\0';
            s->size = (uint8_t)atoi(colon + 1);
            if (s->size != 1 && s->size != 2 && s->size != 4) {
                return -1;
            }
        }
        if (!parse_u32(addr, &s->address)) {
            return -1;
        }
        if (s->kind == K_PEEK) {
            return 1;
        }
        int i = 2;
        s->mask = 0xFFFFFFFFu;
        if (words[i][0] == '&') {
            if (!parse_u32(words[i] + 1, &s->mask)) {
                return -1;
            }
            i++;
        }
        static const char *ops[] = {"==", "!=", "<", "<=", ">", ">="};
        if (i + 1 >= count) {
            return -1;
        }
        s->op = 0xFF;
        for (uint8_t k = 0; k < 6; k++) {
            if (strcmp(words[i], ops[k]) == 0) {
                s->op = k;
            }
        }
        if (s->op == 0xFF || !parse_u32(words[i + 1], &s->value)) {
            return -1;
        }
        if (i + 2 < count && !parse_number(words[i + 2], &s->number)) {
            return -1;
        }
        return i + 3 >= count ? 1 : -1;
    }
    if (strcmp(w0, "player") == 0 && count == 2 && controller_number(words[1], 1)) {
        g_parse_player = controller_number(words[1], 1) - 1;
        return 0;
    }
    if ((strcmp(w0, "connect") == 0 && (count == 2 || (count == 3 && strcmp(words[2], "real") == 0))) ||
        (strcmp(w0, "disconnect") == 0 && count == 2)) {
        int n = controller_number(words[1], 1);
        if (n == 0) {
            return -1;
        }
        s->kind = K_CONNECT;
        s->sticks[0] = n - 1;
        s->sticks[1] = w0[0] == 'd' ? WL_XB_PORT_OFF : count == 3 ? WL_XB_PORT_AUTO : WL_XB_PORT_ON;
        return 1;
    }
    if (strcmp(w0, "bot") == 0 && count == 3 && strcmp(words[2], "checkpoint") != 0) {
        int n = controller_number(words[1], 1);
        char *end;
        long seed = strtol(words[2], &end, 10);
        if (n == 0 || (strcmp(words[2], "off") != 0 && (*end != '\0' || seed < 0 || seed > 1000000))) {
            return -1;
        }
        s->kind = K_BOT_PLAYER;
        s->sticks[0] = n - 1;
        s->sticks[1] = strcmp(words[2], "off") == 0 ? 0 : (double)seed + 1;
        return 1;
    }
    if ((strcmp(w0, "bot") == 0 && (count == 2 || (count == 3 && strcmp(words[2], "checkpoint") == 0))) ||
        (strcmp(w0, "look") == 0 && count == 2)) {
        s->kind = w0[0] == 'b' ? K_BOT : K_LOOK;
        if (count == 3) {
            snprintf(s->arg, sizeof s->arg, "checkpoint");
        }
        return parse_duration(words[1], s) ? 1 : -1;
    }
    if (strcmp(w0, "screenshot") == 0 && count <= 2) {
        s->kind = K_SCREENSHOT;
        if (count == 2) {
            snprintf(s->arg, sizeof s->arg, "%s", words[1]);
        }
        return 1;
    }
    if (strcmp(w0, "dump") == 0 && count == 2) {
        s->kind = K_DUMP;
        snprintf(s->arg, sizeof s->arg, "%s", words[1]);
        return 1;
    }
    if (strcmp(w0, "capture") == 0 && (count == 2 || count == 3)) {
        s->kind = K_CAPTURE;
        if (!parse_number(words[1], &s->number)) {
            return -1;
        }
        if (count == 3) {
            snprintf(s->arg, sizeof s->arg, "%s", words[2]);
        }
        return 1;
    }
    if (strcmp(w0, "repeat") == 0 && count == 2 && atoi(words[1]) >= 1) {
        s->kind = K_REPEAT;
        s->number = atoi(words[1]);
        return 1;
    }
    if (strcmp(w0, "end") == 0 && count == 1) {
        s->kind = K_END;
        return 1;
    }
    if (strcmp(w0, "quit") == 0 && count == 1) {
        s->kind = K_QUIT;
        return 1;
    }
    // a title's long step: <name> [<args>] [<timeout>] (its registration is checked when it runs,
    // so a script parses before the title has registered); a built-in verb whose arguments are
    // wrong is a fault, not a title's step
    static const char *const verbs[] = {"wait", "frames", "vblanks", "screen", "focus", "play", "checkpoint", "ticks",
                                        "press", "select", "hold", "down", "up", "release", "stick", "trigger",
                                        "until", "do", "load", "memory", "peek", "player", "connect", "disconnect",
                                        "bot", "look", "screenshot", "capture", "dump", "repeat", "end", "quit", "log"};
    for (size_t i = 0; i < WL_ARRAY_COUNT(verbs); i++) {
        if (strcmp(w0, verbs[i]) == 0) {
            return -1;
        }
    }
    if (strlen(w0) < sizeof s->name) {
        s->kind = K_TITLE_STEP;
        snprintf(s->name, sizeof s->name, "%s", w0);
        s->number = -1.0; // the step type's default
        int last = count;
        if (count >= 2 && parse_number(words[count - 1], &s->number)) {
            last = count - 1;
        }
        for (int i = 1; i < last; i++) {
            size_t used = strlen(s->arg);
            snprintf(s->arg + used, sizeof s->arg - used, "%s%s", i > 1 ? " " : "", words[i]);
        }
        return 1;
    }
    return -1;
}

// Parses `text` into `steps` (MAX_STEPS of them): the count, or -1 with `*bad_line`.
static int parse_script(char *text, Step *steps, int *bad_line) {
    int count = 0, depth = 0, number = 0;
    int repeat_start[MAX_NESTING], repeat_count[MAX_NESTING];
    g_parse_player = 0;
    char *line = text;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        number++;
        Step st;
        int r = parse_line(line, number, &st);
        line = next;
        if (r == 0) {
            continue;
        }
        if (r < 0 || (st.kind == K_REPEAT && depth == MAX_NESTING) || (st.kind == K_END && depth == 0) ||
            count == MAX_STEPS) {
            *bad_line = number;
            return -1;
        }
        if (st.kind == K_REPEAT) {
            repeat_start[depth] = count;
            repeat_count[depth] = (int)st.number;
            depth++;
        } else if (st.kind == K_END) {
            depth--;
            int start = repeat_start[depth], len = count - start;
            if (count + (repeat_count[depth] - 1) * len > MAX_STEPS) {
                *bad_line = number;
                return -1;
            }
            for (int k = 1; k < repeat_count[depth]; k++) {
                memcpy(&steps[count], &steps[start], (size_t)len * sizeof *steps);
                count += len;
            }
        } else {
            steps[count++] = st;
        }
    }
    if (depth != 0) {
        *bad_line = number;
        return -1;
    }
    return count;
}

int wl_xb_play_parse(const char *text, int *bad_line) {
    size_t n = strlen(text);
    char *copy = malloc(n + 1);
    Step *steps = malloc(MAX_STEPS * sizeof *steps);
    int r = -1;
    *bad_line = 0;
    if (copy != NULL && steps != NULL) {
        memcpy(copy, text, n + 1);
        r = parse_script(copy, steps, bad_line);
    }
    free(copy);
    free(steps);
    return r;
}

// ---- running -----------------------------------------------------------------------------------

static bool number_of(const char *name, int64_t *out) {
    return wl_xb_play_number(name, out);
}

static int64_t tick_now(void) {
    int64_t t = -1;
    return number_of("tick", &t) ? t : -1;
}

static double tick_rate(void) {
    int64_t r = 0;
    return number_of("tick_rate", &r) && r > 0 ? (double)r : 30.0;
}

static void observe(void) {
    char screen[OBSERVED_SIZE], focus[OBSERVED_SIZE];
    if (!wl_xb_play_text("screen", screen, sizeof screen)) {
        screen[0] = '\0';
    }
    if (!wl_xb_play_text("focus", focus, sizeof focus)) {
        focus[0] = '\0';
    }
    int64_t tick = tick_now();
    int64_t checkpoints = 0;
    if (!number_of("checkpoints", &checkpoints)) {
        checkpoints = 0;
    }
    if (!P.observed || strcmp(screen, P.screen) != 0 || strcmp(focus, P.focus) != 0) {
        wl_xb_play_log("screen \"%s\", focus \"%s\" (tick %lld)", screen, focus, (long long)tick);
        snprintf(P.screen, sizeof P.screen, "%s", screen);
        snprintf(P.focus, sizeof P.focus, "%s", focus);
    }
    if (P.observed && (P.tick < 0) != (tick < 0)) {
        wl_xb_play_log("%s (tick %lld)", tick < 0 ? "out of a game" : "in a game", (long long)tick);
    }
    if (P.observed && checkpoints > P.checkpoints) {
        wl_xb_play_log("checkpoint %lld saved (tick %lld)", (long long)checkpoints, (long long)tick);
    }
    // the time played (P.played): the game's tick when a game or a map begins, or a checkpoint
    // ahead is loaded; it goes on, not back, when the game reverts to a checkpoint behind (a death)
    char map[sizeof P.map];
    if (!wl_xb_play_text("map", map, sizeof map)) {
        map[0] = '\0';
    }
    if (tick >= 0) {
        int64_t every = (int64_t)(PROGRESS_SECONDS * tick_rate());
        if (P.last_game_tick < 0 || strcmp(map, P.map) != 0 || tick - P.last_game_tick > TICK_JUMP) {
            P.played = tick;
            P.progress_next = (tick / every + 1) * every;
        } else if (tick > P.last_game_tick) {
            P.played += tick - P.last_game_tick;
        } else if (tick < P.last_game_tick) {
            P.reverts++;
            wl_xb_play_log("the game went back from tick %lld to %lld (a revert, %u so far): the time played goes on "
                           "from %lld", (long long)P.last_game_tick, (long long)tick, (unsigned)P.reverts,
                           (long long)P.played);
        }
        // a cutscene or a long walk logs nothing else for minutes: a line a game minute shows the
        // game moving on (a run's watcher counts these ticks), and a game that stands still shows none
        if (P.played >= P.progress_next) {
            wl_xb_play_log("played to tick %lld (the game's tick %lld)", (long long)P.played, (long long)tick);
            P.progress_next = (P.played / every + 1) * every;
        }
    }
    P.last_game_tick = tick;
    snprintf(P.map, sizeof P.map, "%s", map);
    P.tick = tick;
    P.checkpoints = checkpoints;
    P.observed = true;
}

static void stop(const char *why) {
    const Step *s = &P.steps[P.current];
    wl_xb_play_log("script stopped at line %d (%s): %s; screen \"%s\", focus \"%s\", tick %lld", s->line, s->text, why,
                   P.screen, P.focus, (long long)P.tick);
    P.finished = true;
}

static void next_step(void) {
    P.current++;
    P.started = false;
    if (P.current >= P.count) {
        wl_xb_play_log("script finished");
        P.finished = true;
    }
}

static void buttons_held(WlXboxPad *pad, const Step *s) {
    pad->buttons |= s->digital;
    for (unsigned i = 0; i < WL_XBOX_PAD_ANALOG_COUNT; i++) {
        if (s->analog & (1u << i)) {
            pad->analog[i] = 255;
        }
    }
    if (s->stick_dirs & 0x01) {
        pad->thumb[WL_XBOX_PAD_LY] = 32767;
    }
    if (s->stick_dirs & 0x02) {
        pad->thumb[WL_XBOX_PAD_LY] = -32767;
    }
    if (s->stick_dirs & 0x04) {
        pad->thumb[WL_XBOX_PAD_LX] = -32767;
    }
    if (s->stick_dirs & 0x08) {
        pad->thumb[WL_XBOX_PAD_LX] = 32767;
    }
    if (s->stick_dirs & 0x10) {
        pad->thumb[WL_XBOX_PAD_RY] = 32767;
    }
    if (s->stick_dirs & 0x20) {
        pad->thumb[WL_XBOX_PAD_RY] = -32767;
    }
    if (s->stick_dirs & 0x40) {
        pad->thumb[WL_XBOX_PAD_RX] = -32767;
    }
    if (s->stick_dirs & 0x80) {
        pad->thumb[WL_XBOX_PAD_RX] = 32767;
    }
}

static void buttons_released(WlXboxPad *pad, const Step *s) {
    pad->buttons &= (uint16_t)~s->digital;
    for (unsigned i = 0; i < WL_XBOX_PAD_ANALOG_COUNT; i++) {
        if (s->analog & (1u << i)) {
            pad->analog[i] = 0;
        }
    }
    if (s->stick_dirs & 0x0F) {
        pad->thumb[WL_XBOX_PAD_LX] = pad->thumb[WL_XBOX_PAD_LY] = 0;
    }
    if (s->stick_dirs & 0xF0) {
        pad->thumb[WL_XBOX_PAD_RX] = pad->thumb[WL_XBOX_PAD_RY] = 0;
    }
}

// a press and the release after it, each phase long enough for a slow title to see; true once a
// release has lasted long enough (a press is done)
static bool press_phase(WlXboxPad *pad, const Step *s) {
    uint32_t reports = wl_xb_input_reports(s->player);
    uint64_t now = wl_xb_time_us();
    int64_t frame = -1;
    bool has_frame = number_of("frame", &frame);
    bool long_enough = reports - P.phase_reports >= PRESS_REPORTS && now - P.phase_us >= PRESS_MS * 1000u &&
                       (!has_frame || frame - P.phase_frame >= PRESS_FRAMES);
    bool done = false;
    if (long_enough) {
        // the press ends, or the release after it: the next press begins at the next frame, so a
        // step that looks before each press (select) sees the title after the release
        done = !P.pressed;
        P.pressed = !P.pressed;
        P.phase_reports = reports;
        P.phase_us = now;
        P.phase_frame = frame;
    }
    if (P.pressed && !done) {
        buttons_held(pad, s);
    }
    return done;
}

static int16_t stick_value(double v) {
    if (v > 1.0) {
        v = 1.0;
    }
    if (v < -1.0) {
        v = -1.0;
    }
    return (int16_t)(v * 32767.0);
}

// the step's elapsed time in its unit: seconds (the script's clock; in a game the game's time
// played when the step began in one), frames, ticks (played)
static double elapsed(const Step *s) {
    bool in_game = P.started_tick >= 0 && P.tick >= 0 && P.played >= P.started_played;
    switch (s->unit) {
    case DUR_FRAMES:
        return (double)(P.frames - P.started_frame);
    case DUR_TICKS:
        return in_game ? (double)(P.played - P.started_played) : 0.0;
    default:
        if (in_game) {
            return (double)(P.played - P.started_played) / tick_rate();
        }
        return P.clock - P.started_clock;
    }
}

// the bot's clock in milliseconds: the game's time played (the same input at the same tick, and
// after a revert other input than the play that ended in it), else the script's clock
static uint64_t bot_ms(void) {
    return P.tick >= 0 ? (uint64_t)((double)P.played * 1000.0 / tick_rate()) + 1u : (uint64_t)(P.clock * 1000.0);
}

static bool memory_holds(const Step *s, uint32_t *value) {
    if (!wl_xb_range_ok(s->address, s->size)) {
        return false;
    }
    uint32_t v = s->size == 1 ? wl_xb_rd8(s->address) : s->size == 2 ? wl_xb_rd16(s->address) : wl_xb_rd32(s->address);
    v &= s->mask;
    *value = v;
    switch (s->op) {
    case 0:
        return v == s->value;
    case 1:
        return v != s->value;
    case 2:
        return v < s->value;
    case 3:
        return v <= s->value;
    case 4:
        return v > s->value;
    default:
        return v >= s->value;
    }
}

static void shot_path(char *out, size_t cap, const char *name, const char *ext) {
    char n[64];
    if (name[0] != '\0') {
        snprintf(n, sizeof n, "%s", name);
    } else {
        snprintf(n, sizeof n, "shot_%04u", (unsigned)P.shots);
    }
    P.shots++;
    if (snprintf(out, cap, "%s/%s.%s", P.out_dir, n, ext) >= (int)cap) {
        snprintf(out, cap, "%s.%s", n, ext); // (a folder too long for the name: the current one)
    }
}

#if defined(WL_XBRT_HAVE_GXSOFT)
// the host: the software GX draws two frames (the first may begin mid-frame), the second is kept
static void shot_frame(void *user, const uint8_t *rgba, uint32_t w, uint32_t h) {
    (void)user;
    if (P.shot_frames <= 0 || --P.shot_frames > 0) {
        return;
    }
    // a screen shows no alpha: the EFB's destination alpha (which blending reads) is not the picture
    size_t bytes = (size_t)w * h * 4u;
    uint8_t *opaque = malloc(bytes);
    if (opaque == NULL) {
        wl_xb_play_log("screenshot: no memory for %ux%u", (unsigned)w, (unsigned)h);
        return;
    }
    memcpy(opaque, rgba, bytes);
    for (size_t i = 3; i < bytes; i += 4) {
        opaque[i] = 0xFF;
    }
    uint8_t *png = NULL;
    size_t len = 0;
    WlResult encoded = wl_png_encode(opaque, w, h, &png, &len);
    free(opaque);
    if (encoded == WL_OK) {
        WlPlatformFile *f = NULL;
        if (wl_platform_fs_open(P.shot_path, WL_FILE_WRITE_NEW, &f) == WL_OK) {
            wl_platform_fs_write(f, png, (uint32_t)len);
            wl_platform_fs_close(f);
            wl_xb_play_log("screenshot %s (%ux%u, frame %u)", P.shot_path, (unsigned)w, (unsigned)h, (unsigned)P.frames);
        } else {
            wl_xb_play_log("screenshot: cannot write %s", P.shot_path);
        }
        free(png);
    }
}
#endif

// Guest RAM, physical page 0 up, into <name>.bin in the output folder (a host run and a Wii run
// compared byte for byte). Each page is copied out before it is written, so a page that faults in
// does so here and not inside the card's driver; a page with no frame and nothing in the swap file
// (never touched, or all zero) is written as zeros without being brought in.
static void dump_ram(const char *name) {
    char path[WL_FS_PATH_MAX];
    shot_path(path, sizeof path, name, "bin");
    WlPlatformFile *f = NULL;
    if (wl_platform_fs_open(path, WL_FILE_WRITE_NEW, &f) != WL_OK) {
        wl_xb_play_log("dump: cannot write %s", path);
        return;
    }
    static uint8_t page[WL_XB_PAGE];
    uint32_t held = 0;
    for (uint32_t p = 0; p < g_xb_ram_pages; p++) {
        if (g_xb_pg_frames != 0 && g_xb_pg_frame[p] == 0 && g_xb_pg_slot[p] == 0) {
            memset(page, 0, sizeof page);
        } else {
            memcpy(page, wl_xb_phys_ptr(p * WL_XB_PAGE), sizeof page);
            held++;
        }
        wl_platform_fs_write(f, page, sizeof page);
    }
    wl_platform_fs_close(f);
    wl_xb_play_log("dump %s: %u pages, %u held data", path, (unsigned)g_xb_ram_pages, (unsigned)held);
}

static void screenshot(const char *name) {
#if defined(WL_XBRT_HAVE_GXSOFT)
    if (wl_gxsoft_attached()) {
        wl_xb_play_log("screenshot: one is being taken");
        return;
    }
    shot_path(P.shot_path, sizeof P.shot_path, name, "png");
    static WlGxSoftConfig cfg;
    cfg.frame = shot_frame;
    P.shot_frames = 2;
    // (the software GX is the host's alone: kept out of the host heap's count, mem_census.h)
    wl_platform_heap_exempt(true);
    WlResult attached = wl_gxsoft_attach(&cfg);
    wl_platform_heap_exempt(false);
    if (attached != WL_OK) {
        wl_xb_play_log("screenshot: the software GX did not start");
        P.shot_frames = 0;
    }
#else
    char path[WL_FS_PATH_MAX];
    shot_path(path, sizeof path, name, "cap");
    wl_xb_gpu_capture(path, 1, 0);
    wl_xb_play_log("screenshot %s (a one-frame capture: xbox_nv2a_replay %s --png-gx DIR)", path, path);
#endif
}

// the host's software GX detached once its frames are kept
static void screenshot_poll(void) {
#if defined(WL_XBRT_HAVE_GXSOFT)
    if (P.shot_frames <= 0 && wl_gxsoft_attached()) {
        wl_platform_heap_exempt(true);
        wl_gxsoft_detach();
        wl_platform_heap_exempt(false);
    }
#endif
}

static void title_step_end(WlXbStepEnd how) {
    if (P.treg != NULL && P.treg->step.end != NULL) {
        P.treg->step.end(P.treg->user, &P.tstep, how);
    }
    P.treg = NULL;
}

// One frame of the script: writes the step's presses into P.out.
static void script_frame(void) {
    while (!P.finished) {
        const Step *s = &P.steps[P.current];
        WlXboxPad *pad = &P.out[s->player];
        if (!P.started) {
            P.started = true;
            P.started_clock = P.clock;
            P.started_frame = P.frames;
            P.started_vblank = wl_xb_vblank_count();
            P.started_tick = P.tick;
            P.started_played = P.played;
            P.started_checkpoints = P.checkpoints;
            P.scratch = 0;
            P.pressed = true;
            P.phase_reports = wl_xb_input_reports(s->player);
            P.phase_us = wl_xb_time_us();
            P.phase_frame = -1;
            number_of("frame", &P.phase_frame);
            P.presses = 0;
            P.look = true;
            P.first = true;
            if (s->player != 0) {
                wl_xb_play_log("line %d (%.1f s): controller %d: %s", s->line, P.clock, s->player + 1, s->text);
            } else {
                wl_xb_play_log("line %d (%.1f s): %s", s->line, P.clock, s->text);
            }
            if (s->kind == K_TITLE_STEP) {
                const Reg *r = reg_find(REG_STEP, s->name);
                if (r == NULL) {
                    stop("not a step");
                    return;
                }
                memset(&P.tstep, 0, sizeof P.tstep);
                P.tstep.arg = s->arg;
                P.tstep.timeout_s = s->number >= 0 ? s->number
                                    : r->step.default_timeout_s > 0 ? r->step.default_timeout_s
                                                                    : 3600.0;
                P.tstep.port = s->player;
                P.tstep.ticks = -1;
                P.treg = r;
                if (r->step.begin != NULL && r->step.begin(r->user, &P.tstep) != WL_OK) {
                    P.treg = NULL;
                    stop(P.tstep.why[0] ? P.tstep.why : "it did not begin");
                    return;
                }
            }
        }
        double t = elapsed(s);
        switch (s->kind) {
        case K_WAIT:
            if (t < s->number) {
                return;
            }
            break;
        case K_FRAMES:
            if ((double)(P.frames - P.started_frame) < s->number) {
                return;
            }
            break;
        case K_VBLANKS:
            if ((double)(wl_xb_vblank_count() - P.started_vblank) < s->number) {
                return;
            }
            break;
        case K_SCREEN:
        case K_FOCUS:
        case K_PLAY:
        case K_CHECKPOINT:
        case K_UNTIL:
        case K_MEMORY: {
            bool reached = false;
            bool first = P.first;
            P.first = false;
            if (s->kind == K_SCREEN) {
                reached = P.screen[0] != '\0' && contains(P.screen, s->arg);
            } else if (s->kind == K_FOCUS) {
                reached = P.focus[0] != '\0' && contains(P.focus, s->arg);
            } else if (s->kind == K_PLAY) {
                const Reg *r = reg_find(REG_PREDICATE, "play");
                reached = r != NULL ? ((WlXbPlayPredicateFn)r->fn)(r->user, "", first, &P.scratch)
                                    : P.tick >= 0 && P.screen[0] == '\0';
            } else if (s->kind == K_CHECKPOINT) {
                reached = P.checkpoints > P.started_checkpoints;
            } else if (s->kind == K_UNTIL) {
                const Reg *r = reg_find(REG_PREDICATE, s->name);
                if (r == NULL) {
                    stop("not a predicate");
                    return;
                }
                reached = ((WlXbPlayPredicateFn)r->fn)(r->user, s->arg, first, &P.scratch);
            } else {
                uint32_t v = 0;
                reached = memory_holds(s, &v);
                if (reached) {
                    wl_xb_play_log("memory 0x%08x = 0x%x", (unsigned)s->address, (unsigned)v);
                }
            }
            if (!reached) {
                if (P.clock - P.started_clock >= s->number) {
                    stop("timed out");
                }
                return;
            }
            break;
        }
        case K_TICKS:
            if (P.started_tick < 0) {
                P.started_tick = P.tick; // from the first tick seen, when it began outside a game
            }
            if (P.tick < 0 || (double)(P.tick - P.started_tick) < s->sticks[0]) {
                if (P.clock - P.started_clock >= s->number && s->number != DEFAULT_TIMEOUT) {
                    stop("timed out");
                }
                return;
            }
            break;
        case K_PRESS:
            if (press_phase(pad, s)) {
                P.presses++;
            }
            if (P.presses < (int)s->number) {
                return;
            }
            break;
        case K_SELECT:
            // the focus is looked at before each press (the last one released)
            if (P.look) {
                P.look = false;
                if (P.focus[0] != '\0' && contains(P.focus, s->arg)) {
                    break;
                }
                if (P.presses >= (int)s->number) {
                    stop("not found");
                    return;
                }
            }
            if (press_phase(pad, s)) {
                P.presses++;
                P.look = true;
            }
            return;
        case K_HOLD:
        case K_STICK:
        case K_TRIGGER:
        case K_BOT:
        case K_LOOK:
            if (s->kind == K_BOT && s->arg[0] != '\0' && P.checkpoints > P.started_checkpoints) {
                break;
            }
            if (t >= s->number) {
                if (s->kind == K_BOT && s->arg[0] != '\0') {
                    stop("timed out");
                    return;
                }
                break;
            }
            if ((s->kind == K_LOOK || (s->kind == K_BOT && s->arg[0] == '\0')) && s->unit == DUR_SECONDS &&
                P.clock - P.started_clock >= s->number * STILL_FACTOR + 60.0) {
                // the game's time stood still (a pause, a cutscene that waits): the script's clock ends it
                wl_xb_play_log("%s: %.0f s of the game's time in %.0f s: ended by the script's clock", s->text, t,
                               P.clock - P.started_clock);
                break;
            }
            if (s->kind == K_HOLD) {
                buttons_held(pad, s);
            } else if (s->kind == K_STICK) {
                for (int i = 0; i < 4; i++) {
                    pad->thumb[i] = stick_value(s->sticks[i]);
                }
            } else if (s->kind == K_TRIGGER) {
                pad->analog[WL_XBOX_PAD_LT] = (uint8_t)(fmin(fmax(s->sticks[0], 0.0), 1.0) * 255.0);
                pad->analog[WL_XBOX_PAD_RT] = (uint8_t)(fmin(fmax(s->sticks[1], 0.0), 1.0) * 255.0);
            } else {
                wl_xb_play_bot(pad, bot_ms(), 1, s->kind == K_LOOK);
            }
            return;
        case K_DOWN:
            buttons_held(&P.latched[s->player], s);
            break;
        case K_UP:
            buttons_released(&P.latched[s->player], s);
            break;
        case K_RELEASE:
            memset(&P.latched[s->player], 0, sizeof P.latched[0]);
            break;
        case K_DO: {
            const Reg *r = reg_find(REG_ACTION, s->name);
            if (r == NULL) {
                stop("not an action");
                return;
            }
            WlResult res = ((WlXbPlayActionFn)r->fn)(r->user, s->arg);
            if (res != WL_OK) {
                char why[64];
                snprintf(why, sizeof why, "the action failed (%s)", wl_result_name(res));
                stop(why);
                return;
            }
            break;
        }
        case K_PEEK: {
            uint32_t v = 0;
            Step any = *s;
            any.mask = 0xFFFFFFFFu;
            any.op = 5;
            any.value = 0;
            if (memory_holds(&any, &v)) {
                wl_xb_play_log("peek 0x%08x = 0x%x (%u)", (unsigned)s->address, (unsigned)v, (unsigned)v);
            } else {
                wl_xb_play_log("peek 0x%08x: not mapped", (unsigned)s->address);
            }
            break;
        }
        case K_TITLE_STEP: {
            if (P.treg == NULL) {
                stop("the step is gone");
                return;
            }
            P.tstep.frames = P.frames - P.started_frame;
            P.tstep.ticks = P.started_tick >= 0 && P.tick >= 0 ? P.played - P.started_played : -1;
            P.tstep.seconds = t;
            WlXboxPad tp;
            memset(&tp, 0, sizeof tp);
            P.tstep.pad = &tp;
            if (t >= P.tstep.timeout_s) {
                title_step_end(WL_XB_STEP_END_TIMEOUT);
                stop("timed out");
                return;
            }
            WlXbStepStatus st = P.treg->step.frame(P.treg->user, &P.tstep);
            wl_xbox_pad_merge(pad, &tp);
            if (st == WL_XB_STEP_RUNNING) {
                return;
            }
            if (st == WL_XB_STEP_FAILED) {
                title_step_end(WL_XB_STEP_END_FAILED);
                stop(P.tstep.why[0] ? P.tstep.why : "the step failed");
                return;
            }
            title_step_end(WL_XB_STEP_END_DONE);
            wl_xb_play_log("%s done (tick %lld)", s->name, (long long)P.tick);
            break;
        }
        case K_LOG: {
            char values[96] = "";
            int64_t x = 0, y = 0, z = 0;
            if (number_of("player_x_mm", &x) && number_of("player_y_mm", &y) && number_of("player_z_mm", &z)) {
                snprintf(values, sizeof values, " (at %.2f %.2f %.2f, tick %lld)", (double)x / 1000.0, (double)y / 1000.0,
                         (double)z / 1000.0, (long long)P.tick);
            } else if (P.tick >= 0) {
                snprintf(values, sizeof values, " (tick %lld)", (long long)P.tick);
            }
            wl_xb_play_log("%s%s", s->arg, values);
            break;
        }
        case K_SCREENSHOT:
            // the picture is of the frames after the step began: the script waits for it (on the
            // Wii a one-frame capture, written whole before the next step)
            if (P.first) {
                P.first = false;
                screenshot(s->arg);
            }
            if (P.shot_frames > 0 || wl_xb_gpu_capture_busy()) {
                if (P.clock - P.started_clock < 10.0) {
                    return;
                }
                wl_xb_play_log("screenshot: not done after 10 s; the script goes on");
            }
            break;
        case K_CAPTURE: {
            // the capture's frames are written whole before the next step (a quit right after
            // would cut it short): a wait of 5 s and 1 s a frame at most
            if (P.first) {
                P.first = false;
                char path[WL_FS_PATH_MAX];
                shot_path(path, sizeof path, s->arg, "cap");
                wl_xb_gpu_capture(path, (uint32_t)s->number, 0);
                wl_xb_play_log("capture of %u frames to %s", (unsigned)s->number, path);
            }
            if (wl_xb_gpu_capture_busy()) {
                double limit = 5.0 + (double)s->number;
                if (P.clock - P.started_clock < limit) {
                    return;
                }
                wl_xb_play_log("capture: not done after %.0f s; the script goes on", limit);
            }
            break;
        }
        case K_DUMP:
            dump_ram(s->arg);
            break;
        case K_CONNECT:
            wl_xb_input_set_mode((uint32_t)s->sticks[0], (WlXbPortMode)(int)s->sticks[1]);
            break;
        case K_BOT_PLAYER:
            P.bots[(int)s->sticks[0]] = (int)s->sticks[1];
            break;
        case K_QUIT:
            // a screenshot just before is written first (the card goes once the title has left)
            if (wl_xb_gpu_capture_busy() && P.clock - P.started_clock < 10.0) {
                return;
            }
            wl_xb_play_log("script finished: quit");
            P.finished = true;
            wl_platform_log_flush();
            wl_xb_process_exit(0);
            return;
        default:
            break;
        }
        next_step();
    }
}

static WlResult load_script(void) {
    WlPlatformFile *f = NULL;
    WlResult r = wl_platform_fs_open(P.script_path, WL_FILE_READ, &f);
    if (r != WL_OK) {
        wl_xb_play_log("no script at %s (%s)", P.script_path, wl_result_name(r));
        return r;
    }
    uint64_t size = wl_platform_fs_size(f);
    char *text = size < (1u << 20) ? malloc((size_t)size + 1u) : NULL;
    P.steps = malloc(MAX_STEPS * sizeof *P.steps);
    if (text == NULL || P.steps == NULL || wl_platform_fs_read_at(f, 0, text, (uint32_t)size) != WL_OK) {
        wl_platform_fs_close(f);
        free(text);
        return WL_ERR_NOMEM;
    }
    wl_platform_fs_close(f);
    text[size] = '\0';
    int bad = 0;
    P.count = parse_script(text, P.steps, &bad);
    free(text);
    if (P.count < 0) {
        wl_xb_play_log("%s:%d: not a step", P.script_path, bad);
        return WL_ERR_FORMAT;
    }
    // (parsed into room for the most a script may have, 1024 steps of about 430 bytes: kept at its
    // own count, for the Wii's heap)
    Step *fit = realloc(P.steps, (size_t)(P.count > 0 ? P.count : 1) * sizeof *P.steps);
    if (fit != NULL) {
        P.steps = fit;
    }
    wl_xb_play_log("script %s, %d steps", P.script_path, P.count);
    if (P.count == 0) {
        P.finished = true;
    }
    return WL_OK;
}

// One frame of the harness (lock held).
static void play_frame(void) {
    P.frames++;
    uint32_t vb = wl_xb_vblank_count();
    if (P.last_vblank != 0 && vb > P.last_vblank) {
        uint32_t d = vb - P.last_vblank;
        P.clock += (double)(d < 15u ? d : 15u) / VBLANK_HZ; // a long stall (loading) counts a quarter second
    }
    P.last_vblank = vb;
    screenshot_poll();
    observe();
    int64_t menu = 0;
    if (number_of("menu", &menu)) {
        wl_xb_input_set_menu(menu != 0);
    }
    memset(P.out, 0, sizeof P.out);
    if (P.mode == MODE_SCRIPT) {
        int64_t busy = 0;
        bool inhibited = number_of("menu_busy", &busy) && busy != 0 && P.screen[0] != '\0';
        // a menu that takes no input yet gets no presses (a person would press again)
        if (!P.finished && inhibited && P.started) {
            int k = P.steps[P.current].kind;
            if (k == K_PRESS || k == K_SELECT || k == K_HOLD || k == K_STICK) {
                if (P.clock - P.started_clock >= 60.0) {
                    stop("the menus took no input for a minute");
                }
            } else {
                script_frame();
            }
        } else {
            script_frame();
        }
        for (uint32_t p = 0; p < WL_XB_PLAY_PORTS; p++) {
            wl_xbox_pad_merge(&P.out[p], &P.latched[p]);
            if (P.bots[p] != 0) {
                wl_xb_play_bot(&P.out[p], bot_ms(), P.bots[p] - 1, false);
            }
        }
    } else if (P.mode == MODE_BOT || P.mode == MODE_LOOK) {
        wl_xb_play_bot(&P.out[0], bot_ms(), P.seed, P.mode == MODE_LOOK);
    }
    if (g_frame_hook != NULL) {
        WlXbPlayFrame fr = {P.frames, vb, P.out};
        g_frame_hook(g_frame_user, &fr);
    }
    for (uint32_t p = 0; p < WL_XB_PLAY_PORTS; p++) {
        wl_xb_input_set_script(p, &P.out[p]);
    }
}

void wl_xb_play_frame_point(void) {
    if (!P.active) {
        return;
    }
    if (!P.frame_point) {
        P.frame_point = true;
        wl_xb_play_log("frames from the title's frame point from now on");
    }
    play_frame();
}

static void play_vblank(void *user) {
    (void)user;
    if (P.thread != NULL && !P.frame_point) {
        wl_xb_wake(P.thread);
    }
}

static void play_thread(WlXbThread *self, void *user) {
    (void)user;
    for (;;) {
        wl_xb_wait(self, WL_WAIT_FOREVER);
        if (!P.frame_point) {
            play_frame();
        }
    }
}

WlResult wl_xb_play_start(const char *setting, const char *base_dir, const char *out_dir) {
    if (setting == NULL || setting[0] == '\0') {
        return WL_OK;
    }
    if (P.active) {
        return WL_ERR_STATE;
    }
    snprintf(P.base_dir, sizeof P.base_dir, "%s", base_dir != NULL ? base_dir : "");
    if (strncmp(setting, "bot", 3) == 0) {
        P.mode = MODE_BOT;
        P.seed = setting[3] == ':' ? atoi(setting + 4) : 0;
    } else if (strncmp(setting, "look", 4) == 0) {
        P.mode = MODE_LOOK;
        P.seed = setting[4] == ':' ? atoi(setting + 5) : 0;
    } else if (strncmp(setting, "script:", 7) == 0 && setting[7] != '\0') {
        P.mode = MODE_SCRIPT;
        const char *path = setting + 7;
        if (strchr(path, ':') != NULL || path[0] == '/') {
            snprintf(P.script_path, sizeof P.script_path, "%s", path);
        } else {
            if (snprintf(P.script_path, sizeof P.script_path, "%s/%s", P.base_dir, path) >= (int)sizeof P.script_path) {
                wl_xb_play_log("the script's path is too long: %s/%s", P.base_dir, path);
                return WL_ERR_RANGE;
            }
        }
    } else {
        wl_xb_play_log("\"%s\" is not bot:, look: or script:", setting);
        return WL_ERR_ARG;
    }
    if (out_dir != NULL && out_dir[0] != '\0') {
        snprintf(P.out_dir, sizeof P.out_dir, "%s", out_dir);
    } else if (P.mode == MODE_SCRIPT) {
        snprintf(P.out_dir, sizeof P.out_dir, "%s", P.script_path);
        char *slash = strrchr(P.out_dir, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
    } else {
        snprintf(P.out_dir, sizeof P.out_dir, "%s", P.base_dir);
    }
    P.tick = -1;
    P.last_game_tick = -1;
    if (P.mode == MODE_SCRIPT) {
        WlResult r = load_script();
        if (r != WL_OK) {
            return r;
        }
    }
    WlResult r = wl_xb_thread_create_host("play", play_thread, NULL, 64u << 10, PLAY_PRIORITY, &P.thread);
    if (r != WL_OK) {
        return r;
    }
    r = wl_xb_service_add_vblank_hook(play_vblank, NULL);
    if (r != WL_OK) {
        return r;
    }
    P.active = true;
    wl_xb_play_log("%s (output %s)", setting, P.out_dir);
    return WL_OK;
}
