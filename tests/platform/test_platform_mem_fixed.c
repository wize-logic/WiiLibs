// SPDX-License-Identifier: MIT
// Fixed MEM2 claims (wl_platform_mem_claim_fixed) on the host stub, in a process of their own:
// claims are process-wide and must come before MEM2 is claimed. The on-console runner checks
// the same on the Wii's arena (tests/console/console_runner.c).
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"

#define CLAIM_A ((void *)(uintptr_t)0x60000000u)
#define CLAIM_B ((void *)(uintptr_t)0x60400000u)
#define CLAIM_SIZE (4u * 1024u * 1024u)

TEST fixed_claims(void) {
    // bad arguments
    ASSERT_EQ(WL_ERR_ARG, wl_platform_mem_claim_fixed((WlMemRegion)7, CLAIM_A, CLAIM_SIZE));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_mem_claim_fixed(WL_MEM2, (char *)CLAIM_A + 4, CLAIM_SIZE));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_mem_claim_fixed(WL_MEM2, CLAIM_A, 100));
    ASSERT_EQ(WL_ERR_ARG, wl_platform_mem_claim_fixed(WL_MEM2, CLAIM_A, 0));

    // a claim is usable at exactly its address
    ASSERT_EQ(WL_OK, wl_platform_mem_claim_fixed(WL_MEM2, CLAIM_A, CLAIM_SIZE));
    memset(CLAIM_A, 0xA5, CLAIM_SIZE);
    ASSERT_EQ(0xA5, ((volatile uint8_t *)CLAIM_A)[CLAIM_SIZE - 1]);
    // claims ascend; one below or overlapping the last fails
    ASSERT_EQ(WL_ERR_RANGE, wl_platform_mem_claim_fixed(WL_MEM2, CLAIM_A, 32));
    ASSERT_EQ(WL_ERR_RANGE, wl_platform_mem_claim_fixed(WL_MEM2, (char *)CLAIM_A + CLAIM_SIZE - 32, 64));
    ASSERT_EQ(WL_OK, wl_platform_mem_claim_fixed(WL_MEM2, CLAIM_B, CLAIM_SIZE));

    // the region (and so wl_mem's MEM2 heaps) shrinks by what was claimed
    void *base = NULL;
    size_t mem2 = 0;
    ASSERT_EQ(WL_OK, wl_platform_mem_region(WL_MEM2, &base, &mem2));
    ASSERT_EQ((size_t)52u * 1024u * 1024u - 2u * CLAIM_SIZE, mem2);
    // no claims once MEM2 is claimed
    ASSERT_EQ(WL_ERR_STATE, wl_platform_mem_claim_fixed(WL_MEM2, (char *)CLAIM_B + CLAIM_SIZE, 32));
    WlMemConfig cfg;
    wl_mem_config_default(&cfg);
    ASSERT_EQ(WL_OK, wl_mem_init(&cfg));
    PASS();
}

TEST fixed_claims_mem1(void) {
    // MEM1 claims descend from the top; one above or overlapping the last fails
    void *high = (void *)(uintptr_t)0x61400000u;
    void *low = (void *)(uintptr_t)0x61000000u;
    ASSERT_EQ(WL_OK, wl_platform_mem_claim_fixed(WL_MEM1, high, CLAIM_SIZE));
    memset(high, 0x3C, CLAIM_SIZE);
    ASSERT_EQ(0x3C, ((volatile uint8_t *)high)[CLAIM_SIZE - 1]);
    ASSERT_EQ(WL_ERR_RANGE, wl_platform_mem_claim_fixed(WL_MEM1, (char *)high + 32, 32));
    ASSERT_EQ(WL_OK, wl_platform_mem_claim_fixed(WL_MEM1, low, CLAIM_SIZE));
    void *base = NULL;
    size_t mem1 = 0;
    ASSERT_EQ(WL_OK, wl_platform_mem_region(WL_MEM1, &base, &mem1));
    ASSERT_EQ((size_t)20u * 1024u * 1024u - 2u * CLAIM_SIZE, mem1);
    ASSERT_EQ(WL_ERR_STATE, wl_platform_mem_claim_fixed(WL_MEM1, (char *)low - CLAIM_SIZE, 32));
    PASS();
}

SUITE(mem_fixed) {
    RUN_TEST(fixed_claims_mem1);
    RUN_TEST(fixed_claims);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mem_fixed);
    GREATEST_MAIN_END();
}
