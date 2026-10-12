// SPDX-License-Identifier: MIT
// Crash-safe replacement and recovery (wl/platform/file_replace.h): the plain rename, the FAT sequence
// (aside, rename, remove) and its undo, and the sweep after a cut at either point.
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/platform/file_replace.h"

static char g_root[256];

static void path_of(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", g_root, name);
}

static void write_file(const char *name, const char *text) {
    char path[512];
    path_of(path, sizeof path, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static bool read_file(const char *name, char *out, size_t size) {
    char path[512];
    path_of(path, sizeof path, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
    return true;
}

static bool exists(const char *name) {
    char path[512];
    struct stat st;
    path_of(path, sizeof path, name);
    return stat(path, &st) == 0;
}

// Removes a tree (no shell: the test also runs under qemu-user, where /bin/sh is another architecture's).
static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        if (d != NULL) {
            while ((e = readdir(d)) != NULL) {
                char child[600];
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                remove_tree(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void fresh(void) {
    remove_tree(g_root);
    if (mkdir(g_root, 0777) != 0) {
        perror(g_root);
        exit(2);
    }
}

TEST replace_puts_the_copy_in_the_files_place(void) {
    char a[512], b[512], text[64];
    fresh();
    write_file("save", "old");
    write_file("save.wtmp", "new");
    path_of(a, sizeof a, "save.wtmp");
    path_of(b, sizeof b, "save");
    ASSERT_EQ(0, wl_file_replace(a, b));
    ASSERT(read_file("save", text, sizeof text));
    ASSERT_STR_EQ("new", text);
    ASSERT(!exists("save.wtmp"));
    ASSERT(!exists("save.wold"));
    // a file that is not there yet
    write_file("fresh.wtmp", "first");
    path_of(a, sizeof a, "fresh.wtmp");
    path_of(b, sizeof b, "fresh");
    ASSERT_EQ(0, wl_file_replace(a, b));
    ASSERT(read_file("fresh", text, sizeof text));
    ASSERT_STR_EQ("first", text);
    PASS();
}

TEST the_fat_sequence_replaces_and_leaves_nothing_behind(void) {
    char a[512], b[512], text[64];
    fresh();
    write_file("save", "old");
    write_file("save.wold", "stale leftover");
    write_file("save.wtmp", "new");
    path_of(a, sizeof a, "save.wtmp");
    path_of(b, sizeof b, "save");
    ASSERT_EQ(0, wl_file_replace_aside(a, b));
    ASSERT(read_file("save", text, sizeof text));
    ASSERT_STR_EQ("new", text);
    ASSERT(!exists("save.wtmp"));
    ASSERT(!exists("save.wold"));
    PASS();
}

TEST a_failed_second_rename_puts_the_old_file_back(void) {
    char a[512], b[512], text[64];
    fresh();
    write_file("save", "old");
    path_of(a, sizeof a, "no_such_copy.wtmp");
    path_of(b, sizeof b, "save");
    ASSERT(wl_file_replace_aside(a, b) != 0);
    ASSERT(read_file("save", text, sizeof text));
    ASSERT_STR_EQ("old", text);
    ASSERT(!exists("save.wold"));
    // wl_file_replace refuses before touching anything
    ASSERT_EQ(ENOENT, wl_file_replace(a, b));
    ASSERT(read_file("save", text, sizeof text));
    ASSERT_STR_EQ("old", text);
    PASS();
}

TEST replace_refuses_a_directory_and_bad_arguments(void) {
    char a[512], b[512], text[64];
    char longp[WL_FILE_PATH_MAX + 64];
    fresh();
    write_file("copy.wtmp", "new");
    path_of(b, sizeof b, "folder");
    ASSERT_EQ(0, mkdir(b, 0777));
    path_of(a, sizeof a, "copy.wtmp");
    ASSERT_EQ(EISDIR, wl_file_replace(a, b));
    ASSERT(read_file("copy.wtmp", text, sizeof text));
    ASSERT_EQ(EINVAL, wl_file_replace(NULL, b));
    ASSERT_EQ(EINVAL, wl_file_replace(a, NULL));
    memset(longp, 'a', sizeof longp);
    longp[sizeof longp - 1] = '\0';
    ASSERT_EQ(ENAMETOOLONG, wl_file_replace(a, longp));
    PASS();
}

typedef struct {
    uint32_t restored;
    uint32_t dropped;
} Counts;

static void count(const char *path, int restored, void *user) {
    Counts *c = user;
    (void)path;
    if (restored) {
        c->restored++;
    } else {
        c->dropped++;
    }
}

TEST recover_after_a_cut_between_the_two_renames(void) {
    char text[64], sub[512];
    Counts c = {0, 0};
    fresh();
    // cut after the old file went aside: <file>.wold and the copy, no <file>
    write_file("save.wold", "old");
    write_file("save.wtmp", "new, maybe whole");
    // a nested folder, a second cut, and a leftover where the file is there
    path_of(sub, sizeof sub, "a");
    ASSERT_EQ(0, mkdir(sub, 0777));
    path_of(sub, sizeof sub, "a/b");
    ASSERT_EQ(0, mkdir(sub, 0777));
    write_file("a/b/other.wold", "other old");
    write_file("a/b/other", "other current");
    write_file("a/half.wtmp", "torn");
    write_file("a/keep", "kept");
    ASSERT_EQ(4u, wl_file_recover(g_root, 8, count, &c));
    ASSERT_EQ(1u, c.restored);
    ASSERT_EQ(3u, c.dropped);
    ASSERT(read_file("save", text, sizeof text));
    ASSERT_STR_EQ("old", text);
    ASSERT(!exists("save.wold"));
    ASSERT(!exists("save.wtmp"));
    ASSERT(read_file("a/b/other", text, sizeof text));
    ASSERT_STR_EQ("other current", text);
    ASSERT(!exists("a/b/other.wold"));
    ASSERT(!exists("a/half.wtmp"));
    ASSERT(exists("a/keep"));
    // a clean tree changes nothing
    ASSERT_EQ(0u, wl_file_recover(g_root, 8, NULL, NULL));
    PASS();
}

TEST recover_stops_at_the_depth_it_was_given(void) {
    char sub[512];
    fresh();
    path_of(sub, sizeof sub, "a");
    ASSERT_EQ(0, mkdir(sub, 0777));
    path_of(sub, sizeof sub, "a/b");
    ASSERT_EQ(0, mkdir(sub, 0777));
    write_file("a/b/deep.wtmp", "torn");
    ASSERT_EQ(0u, wl_file_recover(g_root, 1, NULL, NULL));
    ASSERT(exists("a/b/deep.wtmp"));
    ASSERT_EQ(1u, wl_file_recover(g_root, 2, NULL, NULL));
    ASSERT(!exists("a/b/deep.wtmp"));
    ASSERT_EQ(0u, wl_file_recover("/no/such/root/for/wl", 8, NULL, NULL));
    ASSERT_EQ(0u, wl_file_recover(NULL, 8, NULL, NULL));
    PASS();
}

SUITE(file_replace_suite) {
    RUN_TEST(replace_puts_the_copy_in_the_files_place);
    RUN_TEST(the_fat_sequence_replaces_and_leaves_nothing_behind);
    RUN_TEST(a_failed_second_rename_puts_the_old_file_back);
    RUN_TEST(replace_refuses_a_directory_and_bad_arguments);
    RUN_TEST(recover_after_a_cut_between_the_two_renames);
    RUN_TEST(recover_stops_at_the_depth_it_was_given);
    remove_tree(g_root);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    const char *base = getenv("TMPDIR");
    snprintf(g_root, sizeof g_root, "%s/wl_file_replace_%ld", base != NULL ? base : "/tmp", (long)getpid());
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(file_replace_suite);
    GREATEST_MAIN_END();
}
