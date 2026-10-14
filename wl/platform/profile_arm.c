// SPDX-License-Identifier: MIT
// Arming the sampling profiler from a control file (profile_arm.h): the settings' parser, the
// watcher's state machine and the loop that carries it out.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/platform/profile_arm.h"

#define SETTINGS_FILE_LIMIT (16u * 1024u)
#define KEY_SHOWN 32u

void wl_profile_arm_settings_default(WlProfileArmSettings *settings) {
    memset(settings, 0, sizeof *settings);
    settings->seconds = 20;
    settings->after = 10;
    settings->hz = 500;
    settings->depth = 12;
    settings->skip = 1;
    settings->repeat = 1;
    settings->log = 1;
}

void wl_profile_arm_settings_clamp(WlProfileArmSettings *settings) {
    if (settings->hz < 1) {
        settings->hz = 1;
    }
    if (settings->hz > 10000) {
        settings->hz = 10000;
    }
    if (settings->depth > WL_PROFILE_MAX_DEPTH) {
        settings->depth = WL_PROFILE_MAX_DEPTH;
    }
    if (settings->seconds < 1) {
        settings->seconds = 1;
    }
    if (settings->repeat < 1) {
        settings->repeat = 1;
    }
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

// Decimal digits at *p, then the end of the text, a blank, a line break or '#'. A value past
// UINT32_MAX stays at UINT32_MAX.
static bool parse_number(const char *p, const char *end, uint32_t *out) {
    uint64_t value = 0;
    const char *digits = p;

    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10u + (uint64_t)(*p - '0');
        if (value > UINT32_MAX) {
            value = UINT32_MAX;
        }
        p++;
    }
    if (p == digits || (p < end && !is_blank(*p) && *p != '#' && *p != '\r')) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

uint32_t wl_profile_arm_settings_parse(WlProfileArmSettings *settings, const char *text, size_t length,
                                       WlProfileArmIssueFn issue, void *user) {
    uint32_t reported = 0;
    uint32_t line_number = 0;
    const char *p = text;
    const char *const text_end = text + length;

    while (p < text_end) {
        const char *line_end = memchr(p, '\n', (size_t)(text_end - p));
        const char *next;
        const char *name;
        const char *value;
        char key[KEY_SHOWN];
        size_t key_length;
        uint32_t number = 0;

        if (line_end == NULL) {
            line_end = text_end;
        }
        next = line_end < text_end ? line_end + 1 : line_end;
        line_number++;
        while (p < line_end && is_blank(*p)) {
            p++;
        }
        name = p;
        if (p == line_end || *p == '#' || *p == '\r') {
            p = next;
            continue;
        }
        while (p < line_end && !is_blank(*p) && *p != '=' && *p != '\r') {
            p++;
        }
        key_length = (size_t)(p - name);
        memcpy(key, name, key_length < KEY_SHOWN - 1u ? key_length : KEY_SHOWN - 1u);
        key[key_length < KEY_SHOWN - 1u ? key_length : KEY_SHOWN - 1u] = '\0';
        while (p < line_end && (is_blank(*p) || *p == '=')) {
            p++;
        }
        value = p;
        // (a name alone, with no value: skipped without a report, as a note may be empty)
        if (key_length == 4 && memcmp(name, "note", 4) == 0) {
            size_t n = (size_t)(line_end - value);

            while (n > 0 && (value[n - 1] == '\r' || is_blank(value[n - 1]))) {
                n--;
            }
            if (n >= sizeof settings->note) {
                n = sizeof settings->note - 1u;
            }
            memcpy(settings->note, value, n);
            settings->note[n] = '\0';
        } else {
            uint32_t *field = NULL;

            if (key_length == 7 && memcmp(name, "seconds", 7) == 0) {
                field = &settings->seconds;
            } else if (key_length == 5 && memcmp(name, "after", 5) == 0) {
                field = &settings->after;
            } else if (key_length == 2 && memcmp(name, "hz", 2) == 0) {
                field = &settings->hz;
            } else if (key_length == 5 && memcmp(name, "depth", 5) == 0) {
                field = &settings->depth;
            } else if (key_length == 4 && memcmp(name, "skip", 4) == 0) {
                field = &settings->skip;
            } else if (key_length == 6 && memcmp(name, "repeat", 6) == 0) {
                field = &settings->repeat;
            } else if (key_length == 3 && memcmp(name, "log", 3) == 0) {
                field = &settings->log;
            }
            if (field == NULL) {
                reported++;
                if (issue != NULL) {
                    issue(user, WL_PROFILE_ARM_UNKNOWN_KEY, key, line_number);
                }
            } else if (value < line_end && parse_number(value, line_end, &number)) {
                *field = number;
            } else if (value < line_end) {
                reported++;
                if (issue != NULL) {
                    issue(user, WL_PROFILE_ARM_BAD_VALUE, key, line_number);
                }
            }
        }
        p = next;
    }
    wl_profile_arm_settings_clamp(settings);
    return reported;
}

WlResult wl_profile_arm_settings_load(WlProfileArmSettings *settings, const char *path, WlProfileArmIssueFn issue,
                                      void *user) {
    static char text[SETTINGS_FILE_LIMIT + 1u];
    FILE *file;
    size_t length;
    int failed;

    if (settings == NULL || path == NULL) {
        return WL_ERR_ARG;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return WL_ERR_NOT_FOUND;
    }
    length = fread(text, 1, sizeof text, file);
    failed = ferror(file);
    fclose(file);
    if (failed != 0) {
        return WL_ERR_IO;
    }
    if (length > SETTINGS_FILE_LIMIT) {
        return WL_ERR_LIMIT;
    }
    wl_profile_arm_settings_default(settings);
    wl_profile_arm_settings_parse(settings, text, length, issue, user);
    return WL_OK;
}

WlResult wl_profile_arm_dump_path(char *out, size_t size, const char *folder, uint32_t run) {
    int n;

    if (out == NULL || size == 0 || folder == NULL) {
        return WL_ERR_ARG;
    }
    n = snprintf(out, size, "%s/profile%u.txt", folder, (unsigned)(run + 1u));
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return WL_ERR_RANGE;
    }
    return WL_OK;
}

uint32_t wl_profile_arm_ring_samples(const WlProfileArmSettings *settings, uint32_t max_bytes, bool *clipped) {
    uint64_t base = (uint64_t)settings->seconds * settings->hz;
    uint64_t samples = base + base / 10u + 64u;
    uint64_t stride_bytes = (WL_PROFILE_HEADER_WORDS + (uint64_t)settings->depth) * 4u;
    uint64_t limit;

    if (max_bytes == 0) {
        max_bytes = WL_PROFILE_ARM_DEFAULT_RING_BYTES;
    }
    limit = max_bytes / stride_bytes;
    if (clipped != NULL) {
        *clipped = samples > limit;
    }
    return (uint32_t)(samples > limit ? limit : samples);
}

// --- the state machine --------------------------------------------------------------------------
void wl_profile_arm_state_init(WlProfileArmState *state) {
    memset(state, 0, sizeof *state);
    state->phase = WL_PROFILE_ARM_WAITING;
}

void wl_profile_arm_abandon(WlProfileArmState *state, const WlProfileArmSettings *settings) {
    state->phase = state->run + 1u < settings->repeat ? WL_PROFILE_ARM_NEXT : WL_PROFILE_ARM_DONE;
}

WlProfileArmAction wl_profile_arm_step(WlProfileArmState *state, const WlProfileArmSettings *settings,
                                       const WlProfileArmHooks *hooks, void *user, uint64_t now_us) {
    switch (state->phase) {
    case WL_PROFILE_ARM_WAITING:
        state->note[0] = '\0';
        if (hooks->ready(user, settings, state->run, state->note, sizeof state->note)) {
            state->note[sizeof state->note - 1u] = '\0';
            state->phase = WL_PROFILE_ARM_SAMPLING;
            state->started_us = now_us;
            state->stopped_early = false;
            return WL_PROFILE_ARM_START;
        }
        return WL_PROFILE_ARM_IDLE;
    case WL_PROFILE_ARM_SAMPLING:
        if (now_us - state->started_us >= (uint64_t)settings->seconds * 1000000u) {
            state->stopped_early = false;
        } else if (hooks->stop != NULL && hooks->stop(user, settings, state->run)) {
            state->stopped_early = true;
        } else {
            return WL_PROFILE_ARM_IDLE;
        }
        wl_profile_arm_abandon(state, settings);
        return WL_PROFILE_ARM_STOP;
    case WL_PROFILE_ARM_NEXT:
        if (hooks->advanced == NULL || hooks->advanced(user, settings, state->run)) {
            state->run++;
            state->phase = WL_PROFILE_ARM_WAITING;
        }
        return WL_PROFILE_ARM_IDLE;
    case WL_PROFILE_ARM_DONE:
        break;
    }
    return WL_PROFILE_ARM_FINISH;
}

// --- the watcher --------------------------------------------------------------------------------
typedef struct Sampling {
    WlProfileRing ring;
    uint32_t *storage;
    WlProfileDumpInfo info;
    char note[WL_PROFILE_ARM_NOTE_SIZE + 66u];
    uint32_t main_thread;
} Sampling;

static uint64_t ops_time(const WlProfileArmOps *ops) {
    return ops != NULL && ops->time_us != NULL ? ops->time_us(ops->user) : wl_platform_time_us();
}

static void ops_sleep(const WlProfileArmOps *ops, uint32_t ms) {
    if (ops != NULL && ops->sleep_ms != NULL) {
        ops->sleep_ms(ops->user, ms);
    } else {
        wl_platform_thread_sleep_ms(ms);
    }
}

static WlResult ops_start(const WlProfileArmOps *ops, WlProfileRing *ring, uint32_t hz) {
    return ops != NULL && ops->start != NULL ? ops->start(ops->user, ring, hz) : wl_profile_start(ring, hz);
}

static void ops_stop(const WlProfileArmOps *ops) {
    if (ops != NULL && ops->stop != NULL) {
        ops->stop(ops->user);
    } else {
        wl_profile_stop();
    }
}

// the ring, and the sampler started on it; false when there was no memory or the sampler refused
static bool begin_sampling(const WlProfileArmConfig *config, const WlProfileArmState *state, Sampling *sampling) {
    const WlProfileArmSettings *s = &config->settings;
    bool clipped = false;
    uint32_t samples = wl_profile_arm_ring_samples(s, config->max_ring_bytes, &clipped);
    size_t bytes = (((size_t)samples * (WL_PROFILE_HEADER_WORDS + s->depth) * 4u) + 31u) & ~(size_t)31u;
    WlResult result;

    if (clipped) {
        WL_LOG_W("profile", "%u s at %u Hz do not fit %u KB: the ring holds %u samples", (unsigned)s->seconds,
                 (unsigned)s->hz,
                 (unsigned)((config->max_ring_bytes != 0 ? config->max_ring_bytes : WL_PROFILE_ARM_DEFAULT_RING_BYTES) /
                            1024u),
                 (unsigned)samples);
    }
    sampling->storage = bytes != 0 ? aligned_alloc(32, bytes) : NULL;
    if (sampling->storage == NULL) {
        WL_LOG_E("profile", "no memory for the ring (%u KB)", (unsigned)(bytes / 1024u));
        return false;
    }
    if (wl_profile_ring_init(&sampling->ring, sampling->storage, (uint32_t)(bytes / 4u), s->depth,
                             WL_PROFILE_STOP_WHEN_FULL) != WL_OK) {
        free(sampling->storage);
        sampling->storage = NULL;
        return false;
    }
    snprintf(sampling->note, sizeof sampling->note, "%s%s%s", s->note, s->note[0] != '\0' && state->note[0] != '\0' ? " " : "",
             state->note);
    sampling->info.hz = s->hz;
    sampling->info.timebase_hz = 0;
    sampling->info.main_thread = 0;
    sampling->info.note = sampling->note;
    WL_LOG_I("profile", "sampling %u Hz for %u s, %u callers, %u KB: %s", (unsigned)s->hz, (unsigned)s->seconds,
             (unsigned)s->depth, (unsigned)(bytes / 1024u), sampling->note);
    wl_platform_log_flush();
    result = ops_start(config->ops, &sampling->ring, s->hz);
    if (result != WL_OK) {
        WL_LOG_E("profile", "wl_profile_start: %s", wl_result_name(result));
        free(sampling->storage);
        sampling->storage = NULL;
        return false;
    }
    return true;
}

static void end_sampling(const WlProfileArmConfig *config, const WlProfileArmState *state, Sampling *sampling,
                         uint64_t started_us, uint64_t now_us) {
    char path[sizeof config->folder + 32u];
    WlResult result;

    ops_stop(config->ops);
    WL_LOG_I("profile", "%u samples in %u ms (%u dropped)%s", (unsigned)sampling->ring.count,
             (unsigned)((now_us - started_us) / 1000u), (unsigned)sampling->ring.dropped,
             state->stopped_early ? ", stopped early" : "");
    result = wl_profile_arm_dump_path(path, sizeof path, config->folder, state->run);
    if (result == WL_OK) {
        result = wl_profile_dump_file(&sampling->ring, &sampling->info, path);
    }
    WL_LOG_I("profile", "%s: %s", path, wl_result_name(result));
    if (config->settings.log != 0) {
        wl_profile_dump_log(&sampling->ring, &sampling->info);
    }
    wl_platform_log_flush();
    free(sampling->storage);
    sampling->storage = NULL;
}

WlResult wl_profile_arm_run(const WlProfileArmConfig *config) {
    WlProfileArmState state;
    Sampling sampling;
    uint32_t poll_ms;

    if (config == NULL || config->hooks.ready == NULL) {
        return WL_ERR_ARG;
    }
    poll_ms = config->poll_ms != 0 ? config->poll_ms : WL_PROFILE_ARM_DEFAULT_POLL_MS;
    memset(&sampling, 0, sizeof sampling);
    wl_profile_arm_state_init(&state);
    for (;;) {
        uint64_t now = ops_time(config->ops);
        WlProfileArmAction action =
            wl_profile_arm_step(&state, &config->settings, &config->hooks, config->hooks_user, now);

        if (action == WL_PROFILE_ARM_START) {
            if (begin_sampling(config, &state, &sampling)) {
                // (the time runs from the sampler's start, not from the step that asked for it)
                state.started_us = ops_time(config->ops);
            } else {
                wl_profile_arm_abandon(&state, &config->settings);
            }
        } else if (action == WL_PROFILE_ARM_STOP) {
            if (sampling.storage != NULL) {
                end_sampling(config, &state, &sampling, state.started_us, now);
            }
        } else if (action == WL_PROFILE_ARM_FINISH) {
            break;
        } else {
            ops_sleep(config->ops, poll_ms);
        }
    }
    WL_LOG_I("profile", "done");
    wl_platform_log_flush();
    return WL_OK;
}

WlResult wl_profile_arm_config_load(WlProfileArmConfig *config, const char *folder, WlProfileArmIssueFn issue,
                                    void *user) {
    char path[sizeof config->folder + 16u];
    WlResult result;
    int n;

    if (config == NULL || folder == NULL) {
        return WL_ERR_ARG;
    }
    memset(config, 0, sizeof *config);
    n = snprintf(config->folder, sizeof config->folder, "%s", folder);
    if (n < 0 || (size_t)n >= sizeof config->folder) {
        config->folder[0] = '\0';
        return WL_ERR_RANGE;
    }
    snprintf(path, sizeof path, "%s/" WL_PROFILE_ARM_FILE, config->folder);
    result = wl_profile_arm_settings_load(&config->settings, path, issue, user);
    if (result != WL_OK) {
        return result;
    }
    config->priority = WL_THREAD_PRIO_NET;
    return WL_OK;
}

static void *watcher_thread(void *argument) {
    WlProfileArm *arm = argument;

    wl_profile_arm_run(&arm->config);
    return NULL;
}

WlResult wl_profile_arm_launch(WlProfileArm *arm, const WlProfileArmConfig *config) {
    uint32_t stack_size;
    WlResult result;

    if (arm != NULL) {
        memset(arm, 0, sizeof *arm); // (a launch that fails leaves nothing to join)
    }
    if (arm == NULL || config == NULL || config->hooks.ready == NULL) {
        return WL_ERR_ARG;
    }
    arm->config = *config;
    stack_size = config->stack_size != 0 ? config->stack_size : WL_PROFILE_ARM_DEFAULT_STACK_SIZE;
    stack_size = (stack_size + 31u) & ~31u;
    arm->stack = aligned_alloc(32, stack_size);
    if (arm->stack == NULL) {
        WL_LOG_E("profile", "no memory for the watcher's stack");
        return WL_ERR_NOMEM;
    }
    result = wl_platform_thread_create(&arm->thread, watcher_thread, arm, arm->stack, stack_size, config->priority);
    if (result != WL_OK) {
        WL_LOG_E("profile", "no thread for the watcher: %s", wl_result_name(result));
        free(arm->stack);
        arm->stack = NULL;
        return result;
    }
    arm->launched = true;
    WL_LOG_I("profile", "%s/" WL_PROFILE_ARM_FILE ": %u s at %u Hz after %u of the program, after %u skipped, %u time(s)",
             config->folder, (unsigned)config->settings.seconds, (unsigned)config->settings.hz,
             (unsigned)config->settings.after, (unsigned)config->settings.skip, (unsigned)config->settings.repeat);
    return WL_OK;
}

WlResult wl_profile_arm_join(WlProfileArm *arm) {
    WlResult result;

    if (arm == NULL || !arm->launched) {
        return WL_ERR_STATE;
    }
    result = wl_platform_thread_join(arm->thread, NULL);
    arm->launched = false;
    free(arm->stack);
    arm->stack = NULL;
    return result;
}
