// SPDX-License-Identifier: MIT
// Limited claims (WlMemConfig.claim, wl_platform_mem_region_limit) on the host stub, in a process
// of their own: a region's claim is process-wide and made once.
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#define MIB (1024u * 1024u)

TEST claim_only_what_is_needed(void) {
    ASSERT_EQ(WL_ERR_ARG, wl_platform_mem_region_limit(WL_MEM1, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_mem_region_limit((WlMemRegion)7, MIB));

    WlMemConfig cfg;
    wl_mem_config_default(&cfg);
    ASSERT_EQ(WL_MEM_CLAIM_ALL, cfg.claim[WL_MEM1]);
    ASSERT_EQ(WL_MEM_CLAIM_ALL, cfg.claim[WL_MEM2]);
    // a claim below the budgets' need is refused before anything is claimed
    cfg.claim[WL_MEM1] = 4u * MIB;
    ASSERT_EQ(WL_ERR_NOMEM, wl_mem_init(&cfg));

    // MEM1: exactly the budgets and arenas (4 MiB + 2 x 1 MiB + 3 MiB); MEM2: 40 MiB
    cfg.claim[WL_MEM1] = WL_MEM_CLAIM_NEEDED;
    cfg.claim[WL_MEM2] = 40u * MIB;
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    void *base = NULL;
    size_t size = 0;
    ASSERT_EQ(WL_OK, wl_platform_mem_region(WL_MEM1, &base, &size));
    ASSERT_EQ((size_t)9u * MIB, size);
    ASSERT_EQ(WL_OK, wl_platform_mem_region(WL_MEM2, &base, &size));
    ASSERT_EQ((size_t)40u * MIB, size);
    void *p = WL_MEM_ALLOC(WL_TAG_ASSET, 1024);
    ASSERT(p != NULL);
    wl_mem_free(p);
    ASSERT_EQ(0u, wl_mem_shutdown());

    // the regions keep their claims: no new limit, and a re-init with other claims uses them
    ASSERT_EQ(WL_ERR_STATE, wl_platform_mem_region_limit(WL_MEM1, MIB));
    wl_mem_config_default(&cfg);
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    ASSERT_EQ(WL_OK, wl_platform_mem_region(WL_MEM1, &base, &size));
    ASSERT_EQ((size_t)9u * MIB, size);
    ASSERT_EQ(0u, wl_mem_shutdown());
    PASS();
}

SUITE(mem_limit) {
    RUN_TEST(claim_only_what_is_needed);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mem_limit);
    GREATEST_MAIN_END();
}
