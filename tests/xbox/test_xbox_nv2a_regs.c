// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_regs.h against another public table of the same numbers, when one is checked out
// next to WiiLibs (xboxrecomp's src/nv2a/nv2a_regs.h, which follows xemu's names): a test-only
// cross-check that catches a typo in a number. Both headers are read as text at run time; the other
// table's lines are only parsed for a name and a number, nothing of it is compiled or kept. Without
// the other table the test passes, saying so.
//
// Every 3D method WL_NV2A_K_<NAME> is compared with NV097_<NAME> where that exists; a few registers
// with their block-relative names.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/xbox/nv2a_regs.h"

#ifndef WL_NV2A_REGS_MINE
#define WL_NV2A_REGS_MINE "wl/xbox/nv2a_regs.h"
#endif
#ifndef WL_NV2A_REGS_THEIRS
#define WL_NV2A_REGS_THEIRS ""
#endif

enum { MAX_NAMES = 2048 };

typedef struct Entry {
    char name[96];
    unsigned long value;
} Entry;

static Entry g_mine[MAX_NAMES], g_theirs[MAX_NAMES];
static unsigned g_mine_count, g_theirs_count;

// "<prefix>NAME = 0x..," (mine) or "define <prefix>NAME 0x..." (theirs)
static unsigned parse(const char *path, const char *prefix, bool define_form, Entry *out) {
    FILE *f = fopen(path, "r");
    char line[512];
    unsigned n = 0;
    size_t plen = strlen(prefix);

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL && n < MAX_NAMES) {
        char *p = line, *name, *end;
        unsigned long value;

        while (*p == ' ' || *p == '\t' || *p == '#') {
            p++;
        }
        if (define_form) {
            if (strncmp(p, "define", 6) != 0) {
                continue;
            }
            p += 6;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
        if (strncmp(p, prefix, plen) != 0) {
            continue;
        }
        name = p + plen;
        end = name;
        while ((*end >= 'A' && *end <= 'Z') || (*end >= '0' && *end <= '9') || *end == '_') {
            end++;
        }
        if (end == name || (size_t)(end - name) >= sizeof out[n].name) {
            continue;
        }
        p = end;
        while (*p == ' ' || *p == '\t' || *p == '=') {
            p++;
        }
        if (strncmp(p, "0x", 2) != 0 && strncmp(p, "0X", 2) != 0) {
            continue;
        }
        value = strtoul(p, &end, 16);
        if (*end != ',' && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != '/') {
            continue;
        }
        {
            size_t len = 0;
            while ((name[len] >= 'A' && name[len] <= 'Z') || (name[len] >= '0' && name[len] <= '9') || name[len] == '_') {
                len++;
            }
            memcpy(out[n].name, name, len);
            out[n].name[len] = 0;
        }
        out[n].value = value;
        n++;
    }
    fclose(f);
    return n;
}

static const Entry *find(const Entry *list, unsigned count, const char *name) {
    unsigned i;
    for (i = 0; i < count; i++) {
        if (strcmp(list[i].name, name) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

TEST methods_match_the_other_table(void) {
    unsigned i, compared = 0, mismatched = 0;

    g_theirs_count = parse(WL_NV2A_REGS_THEIRS, "NV097_", true, g_theirs);
    if (g_theirs_count == 0) {
        printf("no other table at '%s': nothing to compare\n", WL_NV2A_REGS_THEIRS);
        PASS();
    }
    g_mine_count = parse(WL_NV2A_REGS_MINE, "WL_NV2A_K_", false, g_mine);
    ASSERT(g_mine_count > 150);
    for (i = 0; i < g_mine_count; i++) {
        const Entry *other = find(g_theirs, g_theirs_count, g_mine[i].name);

        if (other == NULL) {
            continue;
        }
        compared++;
        if (other->value != g_mine[i].value) {
            printf("WL_NV2A_K_%s = 0x%04lx, NV097_%s = 0x%04lx\n", g_mine[i].name, g_mine[i].value, other->name,
                   other->value);
            mismatched++;
        }
    }
    printf("%u methods compared with %s\n", compared, WL_NV2A_REGS_THEIRS);
    ASSERT(compared > 150);
    ASSERT_EQ(0u, mismatched);
    PASS();
}

TEST registers_match_the_other_table(void) {
    static const struct {
        const char *name;
        unsigned long block;
        unsigned long mine;
    } pairs[] = {
        {"NV_PMC_INTR_0", 0x000000, WL_NV2A_PMC_INTR_0},
        {"NV_PMC_INTR_EN_0", 0x000000, WL_NV2A_PMC_INTR_EN_0},
        {"NV_PFIFO_INTR_0", 0x002000, WL_NV2A_PFIFO_INTR_0},
        {"NV_PFIFO_INTR_EN_0", 0x002000, WL_NV2A_PFIFO_INTR_EN_0},
        {"NV_PFIFO_RAMHT", 0x002000, WL_NV2A_PFIFO_RAMHT},
        {"NV_PFIFO_RAMFC", 0x002000, WL_NV2A_PFIFO_RAMFC},
        {"NV_PFIFO_CACHE1_PUSH1", 0x002000, WL_NV2A_PFIFO_CACHE1_PUSH1},
        {"NV_PFIFO_CACHE1_DMA_PUT", 0x002000, WL_NV2A_PFIFO_CACHE1_DMA_PUT},
        {"NV_PFIFO_CACHE1_DMA_GET", 0x002000, WL_NV2A_PFIFO_CACHE1_DMA_GET},
        {"NV_PFIFO_CACHE1_DMA_SUBROUTINE", 0x002000, WL_NV2A_PFIFO_CACHE1_DMA_SUBROUTINE},
        {"NV_PGRAPH_INTR", 0x400000, WL_NV2A_PGRAPH_INTR},
        {"NV_PGRAPH_INTR_EN", 0x400000, WL_NV2A_PGRAPH_INTR_EN},
        {"NV_PCRTC_INTR_0", 0x600000, WL_NV2A_PCRTC_INTR_0},
        {"NV_PCRTC_INTR_EN_0", 0x600000, WL_NV2A_PCRTC_INTR_EN_0},
        {"NV_PCRTC_START", 0x600000, WL_NV2A_PCRTC_START},
        {"NV_PFB_WBC", 0x100000, WL_NV2A_PFB_WBC},
        {"NV_PTIMER_TIME_0", 0x009000, WL_NV2A_PTIMER_TIME_0},
        {"NV_PTIMER_TIME_1", 0x009000, WL_NV2A_PTIMER_TIME_1},
        {"NV_USER_DMA_PUT", 0, WL_NV2A_USER_DMA_PUT},
        {"NV_USER_DMA_GET", 0, WL_NV2A_USER_DMA_GET},
    };
    unsigned i, compared = 0;

    g_theirs_count = parse(WL_NV2A_REGS_THEIRS, "NV_", true, g_theirs);
    if (g_theirs_count == 0) {
        PASS();
    }
    for (i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        const Entry *other = find(g_theirs, g_theirs_count, pairs[i].name + 3);

        if (other == NULL) {
            continue;
        }
        compared++;
        if (pairs[i].block + other->value != pairs[i].mine) {
            printf("%s: 0x%06lx here, 0x%06lx there\n", pairs[i].name, pairs[i].mine, pairs[i].block + other->value);
        }
        ASSERT_EQ(pairs[i].mine, pairs[i].block + other->value);
    }
    printf("%u registers compared\n", compared);
    ASSERT(compared >= 10);
    PASS();
}

// the texture formats: WL_NV2A_TEX_<SZ_x | LU_x | LC_x> against NV097_SET_TEXTURE_FORMAT_COLOR_<SZ_x |
// LU_IMAGE_x | LC_IMAGE_x>
TEST texture_formats_match_the_other_table(void) {
    static Entry mine[256], theirs[256];
    unsigned m, t, i, compared = 0;

    t = parse(WL_NV2A_REGS_THEIRS, "NV097_SET_TEXTURE_FORMAT_COLOR_", true, theirs);
    if (t == 0) {
        PASS();
    }
    m = parse(WL_NV2A_REGS_MINE, "WL_NV2A_TEX_", false, mine);
    for (i = 0; i < m; i++) {
        char name[128];
        const Entry *other;

        int n;
        if (strncmp(mine[i].name, "LU_", 3) == 0 || strncmp(mine[i].name, "LC_", 3) == 0) {
            n = snprintf(name, sizeof name, "%.2s_IMAGE_%s", mine[i].name, mine[i].name + 3);
        } else {
            n = snprintf(name, sizeof name, "%s", mine[i].name);
        }
        ASSERT(n > 0 && (size_t)n < sizeof name);
        other = find(theirs, t, name);
        if (other == NULL) {
            continue;
        }
        compared++;
        if (other->value != mine[i].value) {
            printf("WL_NV2A_TEX_%s = 0x%02lx, %s = 0x%02lx\n", mine[i].name, mine[i].value, name, other->value);
        }
        ASSERT_EQ(other->value, mine[i].value);
    }
    printf("%u texture formats compared\n", compared);
    ASSERT(compared >= 20);
    PASS();
}

SUITE(regs_suite) {
    RUN_TEST(methods_match_the_other_table);
    RUN_TEST(registers_match_the_other_table);
    RUN_TEST(texture_formats_match_the_other_table);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(regs_suite);
    GREATEST_MAIN_END();
}
