// SPDX-License-Identifier: MIT
// Arming the profiler from a control file (wl/platform/profile_arm.h), on every host: the
// settings' parser, the watcher's step function against a fake program and a fake clock, the whole
// watcher loop over a fake sampler (files written, the hooks' notes in the header), and the thread
// that runs it. The real sampler (the decrementer interrupt) is the Wii's.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "greatest.h"
#include "wl/platform/profile_arm.h"

#define ARM_DIR "wl_profile_arm_test_dir"

// --- the parser -------------------------------------------------------------------------------
typedef struct {
    int count;
    int issue[8];
    char key[8][32];
    uint32_t line[8];
} Issues;

static void note_issue(void *user, WlProfileArmIssue issue, const char *key, uint32_t line) {
    Issues *issues = user;

    if (issues->count < 8) {
        issues->issue[issues->count] = (int)issue;
        snprintf(issues->key[issues->count], sizeof issues->key[0], "%s", key);
        issues->line[issues->count] = line;
    }
    issues->count++;
}

static uint32_t parse(WlProfileArmSettings *s, const char *text, Issues *issues) {
    memset(issues, 0, sizeof *issues);
    wl_profile_arm_settings_default(s);
    return wl_profile_arm_settings_parse(s, text, strlen(text), note_issue, issues);
}

TEST the_defaults_are_what_the_file_documents(void) {
    WlProfileArmSettings s;
    Issues issues;

    ASSERT_EQ(0u, parse(&s, "", &issues));
    ASSERT_EQ(20u, s.seconds);
    ASSERT_EQ(10u, s.after);
    ASSERT_EQ(500u, s.hz);
    ASSERT_EQ(12u, s.depth);
    ASSERT_EQ(1u, s.skip);
    ASSERT_EQ(1u, s.repeat);
    ASSERT_EQ(1u, s.log);
    ASSERT_STR_EQ("", s.note);
    PASS();
}

TEST every_key_is_read(void) {
    WlProfileArmSettings s;
    Issues issues;
    const char *text = "seconds 30\nafter 4\nhz 250\ndepth 8\nskip 0\nrepeat 3\nlog 0\nnote b30 on dolphin\n";

    ASSERT_EQ(0u, parse(&s, text, &issues));
    ASSERT_EQ(30u, s.seconds);
    ASSERT_EQ(4u, s.after);
    ASSERT_EQ(250u, s.hz);
    ASSERT_EQ(8u, s.depth);
    ASSERT_EQ(0u, s.skip);
    ASSERT_EQ(3u, s.repeat);
    ASSERT_EQ(0u, s.log);
    ASSERT_STR_EQ("b30 on dolphin", s.note);
    PASS();
}

TEST comments_blanks_indents_equals_and_crlf_are_accepted(void) {
    WlProfileArmSettings s;
    Issues issues;
    const char *text = "# a comment\r\n\r\n   \t\r\n  seconds = 5  # how long\r\nhz=100\r\n\t# indented comment\r\nnote  words  \r\n";

    ASSERT_EQ(0u, parse(&s, text, &issues));
    ASSERT_EQ(5u, s.seconds);
    ASSERT_EQ(100u, s.hz);
    ASSERT_STR_EQ("words", s.note);
    PASS();
}

TEST a_last_line_without_a_line_break_is_read(void) {
    WlProfileArmSettings s;
    Issues issues;

    ASSERT_EQ(0u, parse(&s, "repeat 4", &issues));
    ASSERT_EQ(4u, s.repeat);
    PASS();
}

TEST unknown_names_and_bad_values_are_reported_with_their_lines(void) {
    WlProfileArmSettings s;
    Issues issues;
    const char *text = "seconds 12\nsecnds 3\nhz fast\ndepth 4x\nflavour\nskip 2\nthis_is_a_very_long_name_that_is_cut_at_thirty_one 1\n";

    ASSERT_EQ(5u, parse(&s, text, &issues));
    ASSERT_EQ(5, issues.count);
    ASSERT_EQ(WL_PROFILE_ARM_UNKNOWN_KEY, issues.issue[0]);
    ASSERT_STR_EQ("secnds", issues.key[0]);
    ASSERT_EQ(2u, issues.line[0]);
    ASSERT_EQ(WL_PROFILE_ARM_BAD_VALUE, issues.issue[1]);
    ASSERT_STR_EQ("hz", issues.key[1]);
    ASSERT_EQ(3u, issues.line[1]);
    ASSERT_EQ(WL_PROFILE_ARM_BAD_VALUE, issues.issue[2]);
    ASSERT_STR_EQ("depth", issues.key[2]);
    ASSERT_EQ(WL_PROFILE_ARM_UNKNOWN_KEY, issues.issue[3]); // (a name alone, "flavour")
    ASSERT_STR_EQ("flavour", issues.key[3]);
    ASSERT_EQ(5u, issues.line[3]);
    ASSERT_EQ(WL_PROFILE_ARM_UNKNOWN_KEY, issues.issue[4]);
    ASSERT_EQ(7u, issues.line[4]);
    ASSERT_EQ(31u, strlen(issues.key[4]));
    // (what was not understood leaves the setting as it was)
    ASSERT_EQ(12u, s.seconds);
    ASSERT_EQ(500u, s.hz);
    ASSERT_EQ(12u, s.depth);
    ASSERT_EQ(2u, s.skip);
    PASS();
}

TEST a_missing_callback_still_counts(void) {
    WlProfileArmSettings s;

    wl_profile_arm_settings_default(&s);
    ASSERT_EQ(1u, wl_profile_arm_settings_parse(&s, "bogus 1\n", 8, NULL, NULL));
    PASS();
}

TEST values_are_kept_inside_what_the_profiler_can_do(void) {
    WlProfileArmSettings s;
    Issues issues;

    parse(&s, "hz 0\ndepth 99\nseconds 0\nrepeat 0\n", &issues);
    ASSERT_EQ(1u, s.hz);
    ASSERT_EQ(WL_PROFILE_MAX_DEPTH, s.depth);
    ASSERT_EQ(1u, s.seconds);
    ASSERT_EQ(1u, s.repeat);
    parse(&s, "hz 1000000\nafter 99999999999\n", &issues);
    ASSERT_EQ(10000u, s.hz);
    ASSERT_EQ(UINT32_MAX, s.after);
    PASS();
}

TEST a_long_note_is_cut_to_fit(void) {
    WlProfileArmSettings s;
    Issues issues;
    char text[200];

    memset(text, 'x', sizeof text);
    memcpy(text, "note ", 5);
    text[sizeof text - 1] = '\0';
    parse(&s, text, &issues);
    ASSERT_EQ(WL_PROFILE_ARM_NOTE_SIZE - 1u, strlen(s.note));
    PASS();
}

TEST a_later_line_overrides_an_earlier_one(void) {
    WlProfileArmSettings s;
    Issues issues;

    parse(&s, "hz 100\nhz 200\n", &issues);
    ASSERT_EQ(200u, s.hz);
    PASS();
}

TEST the_file_loads_over_the_defaults_and_a_missing_one_is_not_found(void) {
    WlProfileArmSettings s;
    WlProfileArmConfig config;
    FILE *file;
    Issues issues;

    mkdir(ARM_DIR, 0777);
    remove(ARM_DIR "/profile.txt");
    memset(&issues, 0, sizeof issues);
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_profile_arm_settings_load(&s, ARM_DIR "/profile.txt", note_issue, &issues));
    ASSERT_EQ(WL_ERR_NOT_FOUND, wl_profile_arm_config_load(&config, ARM_DIR, note_issue, &issues));
    file = fopen(ARM_DIR "/profile.txt", "w");
    ASSERT(file != NULL);
    fputs("seconds 7\nnote tour\nwarp 9\n", file);
    fclose(file);
    ASSERT_EQ(WL_OK, wl_profile_arm_config_load(&config, ARM_DIR, note_issue, &issues));
    ASSERT_EQ(7u, config.settings.seconds);
    ASSERT_EQ(500u, config.settings.hz);
    ASSERT_STR_EQ("tour", config.settings.note);
    ASSERT_STR_EQ(ARM_DIR, config.folder);
    ASSERT_EQ(WL_THREAD_PRIO_NET, config.priority);
    ASSERT_EQ(1, issues.count);
    ASSERT_STR_EQ("warp", issues.key[0]);
    remove(ARM_DIR "/profile.txt");
    PASS();
}

TEST an_overlong_folder_is_refused(void) {
    WlProfileArmConfig config;
    char folder[200];

    memset(folder, 'a', sizeof folder);
    folder[sizeof folder - 1] = '\0';
    ASSERT_EQ(WL_ERR_RANGE, wl_profile_arm_config_load(&config, folder, NULL, NULL));
    PASS();
}

// --- names and sizes ----------------------------------------------------------------------------
TEST dumps_are_named_by_run_from_one(void) {
    char path[32];

    ASSERT_EQ(WL_OK, wl_profile_arm_dump_path(path, sizeof path, "sd:/apps/halo", 0));
    ASSERT_STR_EQ("sd:/apps/halo/profile1.txt", path);
    ASSERT_EQ(WL_OK, wl_profile_arm_dump_path(path, sizeof path, "sd:/apps/halo", 11));
    ASSERT_STR_EQ("sd:/apps/halo/profile12.txt", path);
    ASSERT_EQ(WL_ERR_RANGE, wl_profile_arm_dump_path(path, 20, "sd:/apps/halo", 0));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_arm_dump_path(NULL, 20, "sd:/apps/halo", 0));
    PASS();
}

TEST the_ring_holds_the_run_and_a_tenth_over_within_its_limit(void) {
    WlProfileArmSettings s;
    bool clipped = true;

    wl_profile_arm_settings_default(&s);
    // 20 s at 500 Hz: 10000 samples, 11000 and 64 with the tenth and the margin
    ASSERT_EQ(11064u, wl_profile_arm_ring_samples(&s, 0, &clipped));
    ASSERT(!clipped);
    // 16 words (64 bytes) a sample: a megabyte holds 16384, a quarter of it 4096
    ASSERT_EQ(11064u, wl_profile_arm_ring_samples(&s, 1024u * 1024u, &clipped));
    ASSERT(!clipped);
    ASSERT_EQ(4096u, wl_profile_arm_ring_samples(&s, 256u * 1024u, &clipped));
    ASSERT(clipped);
    s.seconds = 100;
    s.hz = 1000;
    ASSERT_EQ(65536u, wl_profile_arm_ring_samples(&s, 0, &clipped)); // (the default limit is 4 MiB)
    ASSERT(clipped);
    s.depth = 0;
    ASSERT_EQ(110064u, wl_profile_arm_ring_samples(&s, 0, &clipped));
    ASSERT(!clipped);
    PASS();
}

// --- the step function against a fake program ---------------------------------------------------
// A program whose "position" the test sets; ready when it is at or past `ready_at`; the stretch
// ends (advanced) when `stretch` is above the one profiled.
typedef struct {
    uint32_t position;
    uint32_t ready_at;
    uint32_t stop_at; // the stop hook says yes at or past this (0: never)
    uint32_t stretch; // bumped by the test when the program moves on
    uint32_t stretch_profiled;
    uint32_t ready_calls, stop_calls, advanced_calls;
    uint32_t last_run;
} Program;

static bool hook_ready(void *user, const WlProfileArmSettings *s, uint32_t run, char *note, size_t size) {
    Program *p = user;

    (void)s;
    p->ready_calls++;
    p->last_run = run;
    if (p->position < p->ready_at) {
        return false;
    }
    p->stretch_profiled = p->stretch;
    snprintf(note, size, "stretch %u at %u", (unsigned)p->stretch, (unsigned)p->position);
    return true;
}

static bool hook_stop(void *user, const WlProfileArmSettings *s, uint32_t run) {
    Program *p = user;

    (void)s;
    (void)run;
    p->stop_calls++;
    return p->stop_at != 0 && p->position >= p->stop_at;
}

static bool hook_advanced(void *user, const WlProfileArmSettings *s, uint32_t run) {
    Program *p = user;

    (void)s;
    (void)run;
    p->advanced_calls++;
    return p->stretch != p->stretch_profiled;
}

#define SECOND 1000000ull

TEST the_state_machine_waits_samples_for_the_time_and_finishes(void) {
    WlProfileArmSettings s;
    WlProfileArmState st;
    Program p = {.ready_at = 5};
    WlProfileArmHooks hooks = {hook_ready, hook_stop, hook_advanced};

    wl_profile_arm_settings_default(&s);
    s.seconds = 3;
    wl_profile_arm_state_init(&st);
    ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, 0));
    p.position = 4;
    ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, 1 * SECOND));
    ASSERT_EQ(0u, p.stop_calls);
    p.position = 5;
    ASSERT_EQ(WL_PROFILE_ARM_START, wl_profile_arm_step(&st, &s, &hooks, &p, 2 * SECOND));
    ASSERT_STR_EQ("stretch 0 at 5", st.note);
    ASSERT_EQ(WL_PROFILE_ARM_SAMPLING, st.phase);
    ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, 3 * SECOND));
    ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, 5 * SECOND - 1));
    ASSERT_EQ(2u, p.stop_calls);
    ASSERT_EQ(WL_PROFILE_ARM_STOP, wl_profile_arm_step(&st, &s, &hooks, &p, 5 * SECOND));
    ASSERT(!st.stopped_early);
    ASSERT_EQ(0u, st.run);
    ASSERT_EQ(WL_PROFILE_ARM_DONE, st.phase); // (repeat 1)
    ASSERT_EQ(WL_PROFILE_ARM_FINISH, wl_profile_arm_step(&st, &s, &hooks, &p, 6 * SECOND));
    ASSERT_EQ(WL_PROFILE_ARM_FINISH, wl_profile_arm_step(&st, &s, &hooks, &p, 7 * SECOND));
    PASS();
}

TEST the_stop_hook_ends_a_profile_early(void) {
    WlProfileArmSettings s;
    WlProfileArmState st;
    Program p = {.stop_at = 9};
    WlProfileArmHooks hooks = {hook_ready, hook_stop, NULL};

    wl_profile_arm_settings_default(&s);
    wl_profile_arm_state_init(&st);
    ASSERT_EQ(WL_PROFILE_ARM_START, wl_profile_arm_step(&st, &s, &hooks, &p, 0));
    p.position = 8;
    ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, SECOND));
    p.position = 9;
    ASSERT_EQ(WL_PROFILE_ARM_STOP, wl_profile_arm_step(&st, &s, &hooks, &p, 2 * SECOND));
    ASSERT(st.stopped_early);
    PASS();
}

TEST a_tour_waits_for_each_stretch_to_end_before_asking_ready_again(void) {
    WlProfileArmSettings s;
    WlProfileArmState st;
    Program p = {0};
    WlProfileArmHooks hooks = {hook_ready, NULL, hook_advanced};
    uint64_t now = 0;

    wl_profile_arm_settings_default(&s);
    s.seconds = 1;
    s.repeat = 3;
    wl_profile_arm_state_init(&st);
    for (uint32_t run = 0; run < 3; run++) {
        p.stretch = run;
        p.position = run;
        ASSERT_EQ(WL_PROFILE_ARM_START, wl_profile_arm_step(&st, &s, &hooks, &p, now));
        ASSERT_EQ(run, p.last_run);
        ASSERT_EQ(run, st.run);
        now += SECOND;
        ASSERT_EQ(WL_PROFILE_ARM_STOP, wl_profile_arm_step(&st, &s, &hooks, &p, now));
        if (run < 2) {
            ASSERT_EQ(WL_PROFILE_ARM_NEXT, st.phase);
            // the program is still in the stretch just profiled: not ready is not even asked
            uint32_t asked = p.ready_calls;
            ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, now));
            ASSERT_EQ(WL_PROFILE_ARM_NEXT, st.phase);
            ASSERT_EQ(asked, p.ready_calls);
            p.stretch = run + 1;
            ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, now));
            ASSERT_EQ(WL_PROFILE_ARM_WAITING, st.phase);
        }
    }
    ASSERT_EQ(WL_PROFILE_ARM_DONE, st.phase);
    ASSERT_EQ(WL_PROFILE_ARM_FINISH, wl_profile_arm_step(&st, &s, &hooks, &p, now));
    PASS();
}

TEST an_abandoned_run_moves_on_like_a_stopped_one(void) {
    WlProfileArmSettings s;
    WlProfileArmState st;
    Program p = {0};
    WlProfileArmHooks hooks = {hook_ready, NULL, NULL};

    wl_profile_arm_settings_default(&s);
    s.repeat = 2;
    wl_profile_arm_state_init(&st);
    ASSERT_EQ(WL_PROFILE_ARM_START, wl_profile_arm_step(&st, &s, &hooks, &p, 0));
    wl_profile_arm_abandon(&st, &s);
    ASSERT_EQ(WL_PROFILE_ARM_NEXT, st.phase);
    // (no advanced hook: the next run waits for ready at once)
    ASSERT_EQ(WL_PROFILE_ARM_IDLE, wl_profile_arm_step(&st, &s, &hooks, &p, 0));
    ASSERT_EQ(1u, st.run);
    ASSERT_EQ(WL_PROFILE_ARM_START, wl_profile_arm_step(&st, &s, &hooks, &p, 0));
    wl_profile_arm_abandon(&st, &s);
    ASSERT_EQ(WL_PROFILE_ARM_DONE, st.phase);
    PASS();
}

// --- the watcher over a fake clock and a fake sampler -------------------------------------------
// Time passes only in sleep; each poll moves the program one position on, and its stretch changes
// every three seconds of fake time.
typedef struct {
    uint64_t now;
    Program *program;
    uint32_t last_hz;
    uint32_t starts, stops;
    WlResult start_result;
} Fake;

static uint64_t fake_time(void *user) {
    return ((Fake *)user)->now;
}

static void fake_sleep(void *user, uint32_t ms) {
    Fake *f = user;

    f->now += (uint64_t)ms * 1000u;
    if (f->program != NULL) {
        f->program->position++;
        f->program->stretch = (uint32_t)(f->now / (3u * SECOND));
    }
}

static bool no_stack(void *user, uint32_t address, uint32_t *value) {
    (void)user;
    (void)address;
    (void)value;
    return false;
}

static WlResult fake_start(void *user, WlProfileRing *ring, uint32_t hz) {
    Fake *f = user;

    f->starts++;
    f->last_hz = hz;
    if (f->start_result != WL_OK) {
        return f->start_result;
    }
    // the "interrupt" took three samples before the profile ended
    wl_profile_ring_record_with(ring, 100, 1, 0x80001000u, 0x80002000u, 0, no_stack, NULL);
    wl_profile_ring_record_with(ring, 200, 1, 0x80001004u, 0x80002000u, 0, no_stack, NULL);
    wl_profile_ring_record_with(ring, 300, 2, 0x80001008u, 0x80002000u, 0, no_stack, NULL);
    return WL_OK;
}

static void fake_stop(void *user) {
    ((Fake *)user)->stops++;
}

// line `index` of the file into `line`, with its first characters checked against `prefix`
static bool file_line(const char *path, int index, const char *prefix, char *line, size_t size) {
    FILE *file = fopen(path, "r");
    int i = 0;
    bool ok = false;

    if (file == NULL) {
        return false;
    }
    while (fgets(line, (int)size, file) != NULL) {
        if (i++ == index) {
            ok = strncmp(line, prefix, strlen(prefix)) == 0;
            break;
        }
    }
    fclose(file);
    return ok;
}

static void watcher_config(WlProfileArmConfig *config, WlProfileArmOps *ops, Fake *fake, Program *p) {
    memset(config, 0, sizeof *config);
    wl_profile_arm_settings_default(&config->settings);
    config->settings.seconds = 2;
    config->settings.hz = 250;
    config->settings.depth = 2;
    config->settings.repeat = 2;
    config->settings.log = 0;
    snprintf(config->settings.note, sizeof config->settings.note, "tour");
    snprintf(config->folder, sizeof config->folder, "%s", ARM_DIR);
    config->hooks = (WlProfileArmHooks){hook_ready, hook_stop, hook_advanced};
    config->hooks_user = p;
    ops->time_us = fake_time;
    ops->sleep_ms = fake_sleep;
    ops->start = fake_start;
    ops->stop = fake_stop;
    ops->user = fake;
    config->ops = ops;
    mkdir(ARM_DIR, 0777);
    remove(ARM_DIR "/profile1.txt");
    remove(ARM_DIR "/profile2.txt");
}

TEST the_watcher_writes_a_dump_per_run_for_the_time_asked(void) {
    Program p = {.ready_at = 3};
    Fake fake = {.program = &p};
    WlProfileArmOps ops;
    WlProfileArmConfig config;
    char line[256];

    watcher_config(&config, &ops, &fake, &p);
    ASSERT_EQ(WL_OK, wl_profile_arm_run(&config));
    ASSERT_EQ(2u, fake.starts);
    ASSERT_EQ(2u, fake.stops);
    ASSERT_EQ(250u, fake.last_hz);
    // run 0: ready after three polls (0.3 s), two seconds of samples; run 1: the stretch changed at 3.0 s
    ASSERT(file_line(ARM_DIR "/profile1.txt", 0, "WLPROF 1 hz=250 ", line, sizeof line));
    ASSERT(strstr(line, " depth=2 samples=3 total=3 dropped=0 ") != NULL);
    ASSERT(strstr(line, " note=tour stretch 0 at 3\n") != NULL);
    ASSERT(file_line(ARM_DIR "/profile1.txt", 1, "WLPS 64 1 80001000 80002000", line, sizeof line));
    ASSERT(file_line(ARM_DIR "/profile1.txt", 3, "WLPS 12c 2 80001008 80002000", line, sizeof line));
    ASSERT(file_line(ARM_DIR "/profile2.txt", 0, "WLPROF 1 hz=250 ", line, sizeof line));
    ASSERT(strstr(line, " note=tour stretch 1 at 31\n") != NULL);
    // two seconds of fake time from the start of run 1 (3.1 s), and no more
    ASSERT_EQ(5100000ull, fake.now);
    remove(ARM_DIR "/profile1.txt");
    remove(ARM_DIR "/profile2.txt");
    PASS();
}

TEST the_stop_hook_cuts_a_run_short_and_the_dump_still_comes(void) {
    Program p = {.ready_at = 1, .stop_at = 8};
    Fake fake = {.program = &p};
    WlProfileArmOps ops;
    WlProfileArmConfig config;
    char line[256];

    watcher_config(&config, &ops, &fake, &p);
    config.settings.repeat = 1;
    config.settings.seconds = 100;
    ASSERT_EQ(WL_OK, wl_profile_arm_run(&config));
    ASSERT_EQ(1u, fake.starts);
    ASSERT_EQ(1u, fake.stops);
    ASSERT_EQ(800000ull, fake.now); // (stopped at position 8, not at 100 s)
    ASSERT(file_line(ARM_DIR "/profile1.txt", 0, "WLPROF 1 hz=250 ", line, sizeof line));
    remove(ARM_DIR "/profile1.txt");
    PASS();
}

TEST a_sampler_that_will_not_start_loses_the_run_not_the_watcher(void) {
    Program p = {.ready_at = 0};
    Fake fake = {.program = &p, .start_result = WL_ERR_UNSUPPORTED};
    WlProfileArmOps ops;
    WlProfileArmConfig config;
    FILE *file;

    watcher_config(&config, &ops, &fake, &p);
    config.hooks.advanced = NULL;
    ASSERT_EQ(WL_OK, wl_profile_arm_run(&config));
    ASSERT_EQ(2u, fake.starts); // (both runs tried)
    ASSERT_EQ(0u, fake.stops);
    file = fopen(ARM_DIR "/profile1.txt", "r");
    ASSERT(file == NULL);
    PASS();
}

TEST a_ring_too_small_for_the_run_drops_samples_and_says_so(void) {
    Program p = {.ready_at = 0};
    Fake fake = {.program = &p};
    WlProfileArmOps ops;
    WlProfileArmConfig config;
    char line[256];

    watcher_config(&config, &ops, &fake, &p);
    config.settings.repeat = 1;
    config.settings.depth = 0;
    config.max_ring_bytes = 32; // 4 words (16 bytes) a sample: two fit
    ASSERT_EQ(WL_OK, wl_profile_arm_run(&config));
    ASSERT(file_line(ARM_DIR "/profile1.txt", 0, "WLPROF 1 hz=250 ", line, sizeof line));
    ASSERT(strstr(line, " depth=0 samples=2 total=3 dropped=1 ") != NULL);
    remove(ARM_DIR "/profile1.txt");
    PASS();
}

TEST the_hooks_are_required(void) {
    WlProfileArmConfig config;
    WlProfileArm arm;

    memset(&config, 0, sizeof config);
    ASSERT_EQ(WL_ERR_ARG, wl_profile_arm_run(&config));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_arm_run(NULL));
    ASSERT_EQ(WL_ERR_ARG, wl_profile_arm_launch(&arm, &config));
    ASSERT_EQ(WL_ERR_STATE, wl_profile_arm_join(&arm));
    PASS();
}

// The same on a thread of its own (the real clock is not used: the fake's sleep returns at once).
static Program g_thread_program;
static Fake g_thread_fake;
static WlProfileArmOps g_thread_ops;

TEST the_watcher_runs_on_a_thread_and_is_joined(void) {
    WlProfileArmConfig config;
    WlProfileArm arm;
    char line[256];

    g_thread_program = (Program){.ready_at = 2};
    g_thread_fake = (Fake){.program = &g_thread_program};
    watcher_config(&config, &g_thread_ops, &g_thread_fake, &g_thread_program);
    config.settings.repeat = 1;
    ASSERT_EQ(WL_OK, wl_profile_arm_launch(&arm, &config));
    ASSERT_EQ(WL_OK, wl_profile_arm_join(&arm));
    ASSERT_EQ(1u, g_thread_fake.stops);
    ASSERT(file_line(ARM_DIR "/profile1.txt", 0, "WLPROF 1 hz=250 ", line, sizeof line));
    remove(ARM_DIR "/profile1.txt");
    PASS();
}

SUITE(profile_arm_suite) {
    RUN_TEST(the_defaults_are_what_the_file_documents);
    RUN_TEST(every_key_is_read);
    RUN_TEST(comments_blanks_indents_equals_and_crlf_are_accepted);
    RUN_TEST(a_last_line_without_a_line_break_is_read);
    RUN_TEST(unknown_names_and_bad_values_are_reported_with_their_lines);
    RUN_TEST(a_missing_callback_still_counts);
    RUN_TEST(values_are_kept_inside_what_the_profiler_can_do);
    RUN_TEST(a_long_note_is_cut_to_fit);
    RUN_TEST(a_later_line_overrides_an_earlier_one);
    RUN_TEST(the_file_loads_over_the_defaults_and_a_missing_one_is_not_found);
    RUN_TEST(an_overlong_folder_is_refused);
    RUN_TEST(dumps_are_named_by_run_from_one);
    RUN_TEST(the_ring_holds_the_run_and_a_tenth_over_within_its_limit);
    RUN_TEST(the_state_machine_waits_samples_for_the_time_and_finishes);
    RUN_TEST(the_stop_hook_ends_a_profile_early);
    RUN_TEST(a_tour_waits_for_each_stretch_to_end_before_asking_ready_again);
    RUN_TEST(an_abandoned_run_moves_on_like_a_stopped_one);
    RUN_TEST(the_watcher_writes_a_dump_per_run_for_the_time_asked);
    RUN_TEST(the_stop_hook_cuts_a_run_short_and_the_dump_still_comes);
    RUN_TEST(a_sampler_that_will_not_start_loses_the_run_not_the_watcher);
    RUN_TEST(a_ring_too_small_for_the_run_drops_samples_and_says_so);
    RUN_TEST(the_hooks_are_required);
    RUN_TEST(the_watcher_runs_on_a_thread_and_is_joined);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(profile_arm_suite);
    GREATEST_MAIN_END();
}
