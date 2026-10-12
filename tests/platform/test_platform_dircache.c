// SPDX-License-Identifier: MIT
// The directory listing cache (wl/platform/dircache.h): spellings from a listing read once, a folder that is
// not there, a folder too large to keep, the table's limit and the default lister over a real folder.
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "greatest.h"
#include "wl/platform/dircache.h"

// A lister over a table, counting the folders it was asked for.
typedef struct {
    uint32_t asked;
    uint32_t big_entries; // the folder "big" has this many entries
} Fake;

static bool fake_lister(const char *directory, WlDirVisit visit, void *ctx, void *user) {
    Fake *fake = user;
    fake->asked++;
    if (strcmp(directory, "gone") == 0) {
        return false;
    }
    if (strcmp(directory, "big") == 0) {
        for (uint32_t i = 0; i < fake->big_entries; i++) {
            char name[32];
            snprintf(name, sizeof name, "Entry%u", i);
            if (!visit(name, ctx)) {
                break;
            }
        }
        return true;
    }
    return visit("Maps", ctx) && visit("maps_old", ctx) && visit("MAPS", ctx) && visit("Bitmaps.map", ctx);
}

TEST an_entry_is_spelled_as_the_folder_has_it(void) {
    WlDirCache cache;
    Fake fake = {0, 0};
    char out[64];
    wl_dircache_init(&cache, fake_lister, &fake);
    ASSERT(wl_dircache_spelling(&cache, "root", "bitmaps.MAP", out, sizeof out));
    ASSERT_STR_EQ("Bitmaps.map", out);
    // written exactly as an entry is: that entry, though another differs only in the case and comes earlier
    ASSERT(wl_dircache_spelling(&cache, "root", "MAPS", out, sizeof out));
    ASSERT_STR_EQ("MAPS", out);
    // the first of the entries that differ in the case only
    ASSERT(wl_dircache_spelling(&cache, "root", "maps", out, sizeof out));
    ASSERT_STR_EQ("Maps", out);
    // not there: as written
    ASSERT(wl_dircache_spelling(&cache, "root", "new_file", out, sizeof out));
    ASSERT_STR_EQ("new_file", out);
    ASSERT_EQ(1u, fake.asked);
    ASSERT_EQ(1u, cache.reads);
    // truncated to the buffer
    ASSERT(wl_dircache_spelling(&cache, "root", "bitmaps.map", out, 5));
    ASSERT_STR_EQ("Bitm", out);
    wl_dircache_clear(&cache);
    PASS();
}

TEST a_missing_folder_stays_missing_and_is_read_once(void) {
    WlDirCache cache;
    Fake fake = {0, 0};
    char out[64];
    wl_dircache_init(&cache, fake_lister, &fake);
    ASSERT(wl_dircache_spelling(&cache, "gone", "Anything", out, sizeof out));
    ASSERT_STR_EQ("Anything", out);
    ASSERT(wl_dircache_spelling(&cache, "gone", "other", out, sizeof out));
    ASSERT_STR_EQ("other", out);
    ASSERT_EQ(1u, fake.asked);
    wl_dircache_clear(&cache);
    PASS();
}

TEST a_folder_over_the_limit_is_not_kept_and_not_read_again(void) {
    WlDirCache cache;
    Fake fake = {0, WL_DIRCACHE_MAX_ENTRIES + 1u};
    char out[64] = "untouched";
    wl_dircache_init(&cache, fake_lister, &fake);
    ASSERT(!wl_dircache_spelling(&cache, "big", "entry3", out, sizeof out));
    ASSERT_STR_EQ("untouched", out);
    ASSERT(!wl_dircache_spelling(&cache, "big", "entry4", out, sizeof out));
    ASSERT_EQ(1u, fake.asked); // (the verdict is kept)
    // exactly the limit is kept
    wl_dircache_forget(&cache, "big");
    fake.big_entries = WL_DIRCACHE_MAX_ENTRIES;
    ASSERT(wl_dircache_spelling(&cache, "big", "entry511", out, sizeof out));
    ASSERT_STR_EQ("Entry511", out);
    ASSERT_EQ(2u, fake.asked);
    wl_dircache_clear(&cache);
    PASS();
}

TEST the_table_holds_a_fixed_number_of_folders(void) {
    WlDirCache cache;
    Fake fake = {0, 0};
    char out[64], directory[32];
    wl_dircache_init(&cache, fake_lister, &fake);
    for (uint32_t i = 0; i < WL_DIRCACHE_MAX_DIRECTORIES; i++) {
        snprintf(directory, sizeof directory, "dir%u", i);
        ASSERT(wl_dircache_spelling(&cache, directory, "maps", out, sizeof out));
    }
    ASSERT(!wl_dircache_spelling(&cache, "one_too_many", "maps", out, sizeof out));
    // a folder already kept still answers
    ASSERT(wl_dircache_spelling(&cache, "dir0", "maps", out, sizeof out));
    // forgetting one makes room
    wl_dircache_forget(&cache, "dir7");
    ASSERT(wl_dircache_spelling(&cache, "one_too_many", "maps", out, sizeof out));
    wl_dircache_clear(&cache);
    ASSERT_EQ(0u, cache.count);
    PASS();
}

TEST the_default_lister_reads_a_real_folder(void) {
    char root[256], path[512], out[64];
    WlDirCache cache = WL_DIRCACHE_INIT;
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/wl_dircache_%ld", base != NULL ? base : "/tmp", (long)getpid());
    ASSERT_EQ(0, mkdir(root, 0777));
    snprintf(path, sizeof path, "%s/Halo.MAP", root);
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    fclose(f);
    ASSERT(wl_dircache_spelling(&cache, root, "halo.map", out, sizeof out));
    ASSERT_STR_EQ("Halo.MAP", out);
    // made since: taken as written
    snprintf(path, sizeof path, "%s/later.txt", root);
    f = fopen(path, "wb");
    ASSERT(f != NULL);
    fclose(f);
    ASSERT(wl_dircache_spelling(&cache, root, "LATER.txt", out, sizeof out));
    ASSERT_STR_EQ("LATER.txt", out);
    ASSERT_EQ(1u, cache.reads);
    wl_dircache_forget(&cache, root);
    ASSERT(wl_dircache_spelling(&cache, root, "LATER.TXT", out, sizeof out));
    ASSERT_STR_EQ("later.txt", out);
    ASSERT_EQ(2u, cache.reads);
    // a folder that is not there
    snprintf(path, sizeof path, "%s/nope", root);
    ASSERT(wl_dircache_spelling(&cache, path, "x", out, sizeof out));
    ASSERT_STR_EQ("x", out);
    wl_dircache_clear(&cache);
    snprintf(path, sizeof path, "%s/Halo.MAP", root);
    unlink(path);
    snprintf(path, sizeof path, "%s/later.txt", root);
    unlink(path);
    rmdir(root);
    PASS();
}

TEST bad_arguments_are_refused(void) {
    WlDirCache cache;
    Fake fake = {0, 0};
    char out[8];
    wl_dircache_init(&cache, fake_lister, &fake);
    ASSERT(!wl_dircache_spelling(NULL, "root", "x", out, sizeof out));
    ASSERT(!wl_dircache_spelling(&cache, NULL, "x", out, sizeof out));
    ASSERT(!wl_dircache_spelling(&cache, "root", NULL, out, sizeof out));
    ASSERT(!wl_dircache_spelling(&cache, "root", "x", NULL, sizeof out));
    ASSERT(!wl_dircache_spelling(&cache, "root", "x", out, 0));
    ASSERT_EQ(0u, fake.asked);
    PASS();
}

SUITE(dircache_suite) {
    RUN_TEST(an_entry_is_spelled_as_the_folder_has_it);
    RUN_TEST(a_missing_folder_stays_missing_and_is_read_once);
    RUN_TEST(a_folder_over_the_limit_is_not_kept_and_not_read_again);
    RUN_TEST(the_table_holds_a_fixed_number_of_folders);
    RUN_TEST(the_default_lister_reads_a_real_folder);
    RUN_TEST(bad_arguments_are_refused);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(dircache_suite);
    GREATEST_MAIN_END();
}
