// SPDX-License-Identifier: MIT
// wl/xbox/nv2a: the NV2A device model. Registers answer as an idle GPU's; push buffers run on a
// DMA_PUT write (increasing and non-increasing methods, jumps old and new, call and return, a
// method's words across two kicks); Direct3D's start-up handshake (a jump at physical 0, GET
// waited for); objects bound through RAMHT; semaphores, notifiers and reports written into guest
// memory; interrupts latched for the ISR; a trapped method stopping the channel until the ISR
// clears it; a capture replayed to the same state.
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "wl/xbox/nv2a_capture.h"

// ---- a backend that counts -------------------------------------------------------------------

typedef struct Counts {
    unsigned clears, draws, frames, flips;
    uint32_t last_clear_flags, last_flip;
    uint32_t last_count, last_primitive;
} Counts;

static void count_clear(void *user, const WlNv2aKelvin *k, uint32_t flags) {
    Counts *c = user;
    (void)k;
    c->clears++;
    c->last_clear_flags = flags;
}

static uint32_t count_draw(void *user, const WlNv2aKelvin *k, const WlNv2aDraw *draw) {
    Counts *c = user;
    (void)k;
    c->draws++;
    c->last_count = draw->count;
    c->last_primitive = draw->primitive;
    return 100;
}

static void count_frame(void *user, const WlNv2aKelvin *k) {
    Counts *c = user;
    (void)k;
    c->frames++;
}

static void count_flip(void *user, const WlNv2aKelvin *k, uint32_t phys) {
    Counts *c = user;
    (void)k;
    c->flips++;
    c->last_flip = phys;
}

static WlNv2aBackend counting_backend(Counts *c) {
    WlNv2aBackend b;
    memset(c, 0, sizeof *c);
    memset(&b, 0, sizeof b);
    b.user = c;
    b.clear = count_clear;
    b.draw = count_draw;
    b.frame_done = count_frame;
    b.flip = count_flip;
    return b;
}

// ---- registers ----------------------------------------------------------------------------------

TEST registers_answer_as_an_idle_gpu(void) {
    TGuest g;
    ASSERT(t_create(&g, NULL) != NULL);
    // the write-combining flush: asked for with bit 16, done at once
    t_mmio(&g, WL_NV2A_PFB_WBC, 0x00010000u | 0x5u);
    ASSERT_EQ(0u, t_mmio_r(&g, WL_NV2A_PFB_WBC) & WL_NV2A_PFB_WBC_FLUSH);
    ASSERT_EQ(0x5u, t_mmio_r(&g, WL_NV2A_PFB_WBC));
    ASSERT_EQ(0u, t_mmio_r(&g, WL_NV2A_PGRAPH_STATUS));
    ASSERT(t_mmio_r(&g, WL_NV2A_PFIFO_CACHE1_STATUS) & WL_NV2A_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY);
    ASSERT(t_mmio_r(&g, WL_NV2A_PFIFO_RUNOUT_STATUS) & WL_NV2A_PFIFO_RUNOUT_LOW_MARK_EMPTY);
    ASSERT_EQ(0u, t_mmio_r(&g, WL_NV2A_PFIFO_CACHE1_DMA_PUSH) & WL_NV2A_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY);
    ASSERT_EQ((uint32_t)T_RAM, t_mmio_r(&g, WL_NV2A_PFB_CSTATUS));
    ASSERT_EQ(WL_NV2A_PCI_ID, t_mmio_r(&g, WL_NV2A_PBUS_PCI_NV_0));
    // a register without behaviour reads back what was written, bytes too
    t_mmio(&g, 0x001830, 0x12345678u);
    ASSERT_EQ(0x12345678u, t_mmio_r(&g, 0x001830));
    wl_nv2a_mmio_write(g.dev, 0x001831, 0xAB, 1);
    ASSERT_EQ(0x1234AB78u, t_mmio_r(&g, 0x001830));
    ASSERT_EQ(0x34u, wl_nv2a_mmio_read(g.dev, 0x001832, 1));
    // the timer moves
    {
        uint32_t t0 = t_mmio_r(&g, WL_NV2A_PTIMER_TIME_0), t1 = t_mmio_r(&g, WL_NV2A_PTIMER_TIME_0);
        ASSERT(t1 > t0);
    }
    // the CRTC's index and data registers
    wl_nv2a_mmio_write(g.dev, WL_NV2A_PRMCIO_CRX, 0x1F, 1);
    wl_nv2a_mmio_write(g.dev, WL_NV2A_PRMCIO_CR, 0x57, 1);
    ASSERT_EQ(0x57u, wl_nv2a_mmio_read(g.dev, WL_NV2A_PRMCIO_CR, 1));
    t_destroy(&g);
    PASS();
}

TEST pramin_is_the_devices_own_memory(void) {
    TGuest g;
    unsigned i;
    ASSERT(t_create(&g, NULL) != NULL);
    t_ramin(&g, 0x1234 * 4, 0xCAFEF00Du);
    t_ramin(&g, 0xFFFFC, 0x01020304u);
    ASSERT_EQ(0xCAFEF00Du, t_mmio_r(&g, WL_NV2A_PRAMIN + 0x1234 * 4));
    ASSERT_EQ(0x01020304u, t_mmio_r(&g, WL_NV2A_PRAMIN + 0xFFFFC));
    ASSERT_EQ(0x02u, wl_nv2a_mmio_read(g.dev, WL_NV2A_PRAMIN + 0xFFFFE, 1));
    wl_nv2a_mmio_write(g.dev, WL_NV2A_PRAMIN + 0x41, 0xAB, 1);
    ASSERT_EQ(0x0000AB00u, t_mmio_r(&g, WL_NV2A_PRAMIN + 0x40));
    ASSERT_EQ(0u, t_mmio_r(&g, WL_NV2A_PRAMIN + 0x80000)); // never written
    // guest RAM untouched
    for (i = 0; i < T_RAM; i += 4) {
        if (t_r32(&g, i) != 0) {
            break;
        }
    }
    ASSERT_EQ((unsigned)T_RAM, i);
    t_destroy(&g);
    PASS();
}

// ---- the pusher ---------------------------------------------------------------------------------

TEST direct3d_handshake_jump_at_physical_zero(void) {
    TGuest g;
    ASSERT(t_create(&g, NULL) != NULL);
    // GET at 0 (reset), a jump at physical 0 to the push buffer, PUT at the push buffer's start
    t_mmio(&g, WL_NV2A_PFIFO_CACHE1_DMA_GET, 0);
    t_mmio(&g, WL_NV2A_PFIFO_CACHE1_DMA_PUT, 0);
    t_w32(&g, 0, T_PB | 1u);
    t_mmio(&g, WL_NV2A_USER + WL_NV2A_USER_DMA_PUT, T_PB);
    ASSERT_EQ((uint32_t)T_PB, t_get(&g));
    // then a method, which runs from there
    t_setup_3d(&g);
    ASSERT_EQ(g.put, t_get(&g));
    {
        WlNv2aStats s;
        wl_nv2a_stats(g.dev, &s);
        ASSERT_EQ(0u, s.pusher_errors);
        ASSERT(s.jumps >= 1);
    }
    t_destroy(&g);
    PASS();
}

TEST methods_increasing_and_non_increasing(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    // three increasing words from 0x300
    t_method(&g, WL_NV2A_K_SET_ALPHA_TEST_ENABLE, 3);
    t_word(&g, 1);
    t_word(&g, 1);
    t_word(&g, 0);
    // three non-increasing words to 0x340: the last stays
    t_word(&g, WL_NV2A_CMD_NON_INCREASING | (3u << 18) | WL_NV2A_K_SET_ALPHA_REF);
    t_word(&g, 10);
    t_word(&g, 20);
    t_word(&g, 30);
    t_kick(&g);
    ASSERT_EQ(1u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_TEST_ENABLE));
    ASSERT_EQ(1u, wl_nv2a_k(k, WL_NV2A_K_SET_BLEND_ENABLE));
    ASSERT_EQ(0u, wl_nv2a_k(k, WL_NV2A_K_SET_CULL_FACE_ENABLE));
    ASSERT_EQ(30u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    t_destroy(&g);
    PASS();
}

TEST a_method_header_goes_on_across_kicks(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_method(&g, WL_NV2A_K_SET_COMBINER_FACTOR0, 4);
    t_word(&g, 0x11111111u);
    t_word(&g, 0x22222222u);
    t_kick(&g); // PUT in the middle of the method's words
    ASSERT_EQ(0x22222222u, wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_FACTOR0 + 4));
    t_word(&g, 0x33333333u);
    t_word(&g, 0x44444444u);
    t_kick(&g);
    ASSERT_EQ(0x33333333u, wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_FACTOR0 + 8));
    ASSERT_EQ(0x44444444u, wl_nv2a_k(k, WL_NV2A_K_SET_COMBINER_FACTOR0 + 12));
    t_destroy(&g);
    PASS();
}

TEST jumps_calls_and_returns(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    uint32_t sub = 0x30000, far = 0x38000, back;
    WlNv2aStats s;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    // a subroutine that sets the alpha ref, and returns
    t_w32(&g, sub, (1u << 18) | WL_NV2A_K_SET_ALPHA_REF);
    t_w32(&g, sub + 4, 77);
    t_w32(&g, sub + 8, WL_NV2A_CMD_RETURN);
    // call it, then an old-style jump to `far`, which sets the depth func and jumps back with a
    // new-style jump
    t_word(&g, sub | WL_NV2A_CMD_CALL);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_FUNC, WL_NV2A_FUNC_GREATER);
    t_word(&g, WL_NV2A_CMD_OLD_JUMP | far);
    back = g.put;
    t_w32(&g, far, (1u << 18) | WL_NV2A_K_SET_DEPTH_FUNC);
    t_w32(&g, far + 4, WL_NV2A_FUNC_LEQUAL);
    t_w32(&g, far + 8, back | WL_NV2A_CMD_JUMP);
    t_m1(&g, WL_NV2A_K_SET_DEPTH_MASK, 1);
    t_kick(&g);
    ASSERT_EQ(g.put, t_get(&g));
    ASSERT_EQ(77u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ((uint32_t)WL_NV2A_FUNC_GREATER, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_FUNC));
    ASSERT_EQ((uint32_t)WL_NV2A_FUNC_LEQUAL, wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_FUNC));
    ASSERT_EQ(1u, wl_nv2a_k(k, WL_NV2A_K_SET_DEPTH_MASK));
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.calls);
    ASSERT_EQ(1u, s.returns);
    ASSERT_EQ(0u, s.pusher_errors);
    // a return outside a call, a call inside a call: errors, skipped
    t_word(&g, WL_NV2A_CMD_RETURN);
    t_kick(&g);
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.pusher_errors);
    t_destroy(&g);
    PASS();
}

TEST ring_wrap_through_a_jump(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    // a jump back to the ring's start, then methods there; PUT below GET
    t_word(&g, T_PB | WL_NV2A_CMD_JUMP);
    g.put = T_PB;
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 99);
    t_kick(&g);
    ASSERT_EQ(99u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    t_destroy(&g);
    PASS();
}

TEST a_jump_to_itself_is_stopped_and_the_channel_goes_on(void) {
    TGuest g;
    WlNv2aStats s;
    const WlNv2aKelvin *k;
    uint32_t at;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    at = g.put;
    t_word(&g, at | WL_NV2A_CMD_JUMP);
    t_kick(&g);
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.pusher_errors);
    ASSERT_EQ(g.put, t_get(&g));
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 42);
    t_kick(&g);
    ASSERT_EQ(42u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    t_destroy(&g);
    PASS();
}

// ---- objects, semaphores, notifiers, reports ------------------------------------------------------

TEST objects_bound_through_ramht_and_context_dmas(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    ASSERT_EQ(1u, k->dma_a.valid);
    ASSERT_EQ((uint32_t)WL_NV2A_CLASS_DMA_IN_MEMORY, k->dma_a.class_id);
    ASSERT_EQ(0u, k->dma_a.base);
    ASSERT_EQ(0x8000u, k->dma_notifies.base);
    ASSERT_EQ(0xFFFu, k->dma_notifies.limit);
    // a DMA object with an odd base (adjust): base = page | adjust
    t_dma(&g, 0x11200, 0x00123456u, 0xFF);
    t_ramht(&g, 9, 0xD0000009u, 0x11200);
    t_m1(&g, WL_NV2A_K_SET_CONTEXT_DMA_REPORT, 0xD0000009u);
    t_kick(&g);
    ASSERT_EQ(0x00123456u, k->dma_report.base);
    ASSERT_EQ(0xFFu, k->dma_report.limit);
    t_destroy(&g);
    PASS();
}

TEST semaphores_written_into_guest_memory(void) {
    TGuest g;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    // the 3D class's back-end release
    t_m1(&g, WL_NV2A_K_SET_SEMAPHORE_OFFSET, 0x20000);
    t_m1(&g, WL_NV2A_K_BACK_END_WRITE_SEMAPHORE_RELEASE, 0xABCD0001u);
    // the puller's own: DMA, offset, release (any subchannel)
    t_m1(&g, WL_NV2A_PULLER_DMA_SEMAPHORE, 0xD0000001u);
    t_m1(&g, WL_NV2A_PULLER_SEMAPHORE_OFFSET, 0x10);
    t_m1(&g, WL_NV2A_PULLER_SEMAPHORE_RELEASE, 0x55AA55AAu);
    t_m1(&g, WL_NV2A_PULLER_SEMAPHORE_ACQUIRE, 0x55AA55AAu);
    t_m1(&g, WL_NV2A_PULLER_REF_CNT, 42);
    t_kick(&g);
    ASSERT_EQ(0xABCD0001u, t_r32(&g, 0x20000));
    ASSERT_EQ(0x55AA55AAu, t_r32(&g, 0x8010));
    ASSERT_EQ(42u, t_mmio_r(&g, WL_NV2A_USER + WL_NV2A_USER_REF));
    {
        WlNv2aStats s;
        wl_nv2a_stats(g.dev, &s);
        ASSERT_EQ(2u, s.semaphores);
        ASSERT_EQ(0u, s.acquires_unmet);
    }
    t_destroy(&g);
    PASS();
}

TEST notify_raises_the_pgraph_interrupt(void) {
    TGuest g;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, WL_NV2A_PMC_INTR_EN_HARDWARE);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, WL_NV2A_PGRAPH_INTR_NOTIFY);
    memset(g.ram + 0x8000, 0xFF, 16);
    // a notify that only writes, then one that wakes
    t_m1(&g, WL_NV2A_K_NOTIFY, 0);
    t_kick(&g);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    ASSERT_EQ(0u, t_r32(&g, 0x800C)); // status: done
    t_m1(&g, WL_NV2A_K_NOTIFY, 1);
    t_m1(&g, WL_NV2A_K_NO_OPERATION, 0);
    t_kick(&g);
    ASSERT(wl_nv2a_irq_pending(g.dev));
    ASSERT(t_mmio_r(&g, WL_NV2A_PMC_INTR_0) & WL_NV2A_PMC_INTR_PGRAPH);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_NOTIFY);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    t_destroy(&g);
    PASS();
}

TEST reports_count_the_pixels_drawn(void) {
    TGuest g;
    Counts c;
    WlNv2aBackend backend = counting_backend(&c);
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    t_m1(&g, WL_NV2A_K_CLEAR_REPORT_VALUE, WL_NV2A_REPORT_ZPASS_PIXEL_COUNT);
    t_m1(&g, WL_NV2A_K_SET_ZPASS_PIXEL_COUNT_ENABLE, 1);
    // a position array (three floats) and two triangles drawn: 100 pixels each, as the backend says
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET, 0x40000);
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT, (12u << 8) | (3u << 4) | WL_NV2A_VTX_F);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (2u << 24) | 0);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (2u << 24) | 3);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_m1(&g, WL_NV2A_K_GET_REPORT, (WL_NV2A_REPORT_ZPASS_PIXEL_COUNT << 24) | 0x21000);
    t_kick(&g);
    ASSERT_EQ(2u, c.draws);
    ASSERT_EQ(200u, t_r32(&g, 0x21008));
    ASSERT_EQ(0u, t_r32(&g, 0x2100C));
    t_destroy(&g);
    PASS();
}

// ---- vblank, flips, frames -------------------------------------------------------------------------

TEST vblank_interrupt_and_flip(void) {
    TGuest g;
    Counts c;
    WlNv2aBackend backend = counting_backend(&c);
    ASSERT(t_create(&g, &backend) != NULL);
    wl_nv2a_vblank(g.dev);
    // not enabled: no interrupt, the status bit set
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    ASSERT_EQ(1u, t_mmio_r(&g, WL_NV2A_PCRTC_INTR_0));
    t_mmio(&g, WL_NV2A_PCRTC_INTR_0, 1);
    t_mmio(&g, WL_NV2A_PCRTC_INTR_EN_0, 1);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 1);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    wl_nv2a_vblank(g.dev);
    ASSERT(wl_nv2a_irq_pending(g.dev));
    ASSERT_EQ((uint32_t)WL_NV2A_PMC_INTR_PCRTC, t_mmio_r(&g, WL_NV2A_PMC_INTR_0));
    // the ISR: the flip, then the status cleared
    t_mmio(&g, WL_NV2A_PCRTC_START, 0x00240000u);
    t_mmio(&g, WL_NV2A_PCRTC_INTR_0, 1);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    ASSERT_EQ(1u, c.flips);
    ASSERT_EQ(0x00240000u, c.last_flip);
    ASSERT_EQ(0x00240000u, t_mmio_r(&g, WL_NV2A_PCRTC_START));
    // FLIP_INCREMENT_WRITE is a frame done
    t_setup_3d(&g);
    t_m1(&g, WL_NV2A_K_SET_FLIP_MODULO, 2);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_m1(&g, WL_NV2A_K_FLIP_STALL, 0);
    t_kick(&g);
    ASSERT_EQ(1u, c.frames);
    ASSERT_EQ(1u, wl_nv2a_kelvin(g.dev)->flip_write);
    t_destroy(&g);
    PASS();
}

TEST a_trapped_method_stops_the_channel_until_acknowledged(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 1);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_COLOR_CLEAR_VALUE, 0xB0B0B0B0u); // pbkit's parameter B
    t_m1(&g, WL_NV2A_K_NO_OPERATION, 0x1234);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 55);
    t_kick(&g);
    // stopped after the trap: the ISR sees it, the method after it has not run
    ASSERT(wl_nv2a_irq_pending(g.dev));
    ASSERT(t_get(&g) != g.put);
    ASSERT(t_mmio_r(&g, WL_NV2A_PGRAPH_INTR) & WL_NV2A_PGRAPH_INTR_ERROR);
    ASSERT_EQ(0x1234u, t_mmio_r(&g, 0x400708));
    ASSERT_EQ((uint32_t)WL_NV2A_K_NO_OPERATION, t_mmio_r(&g, 0x400704) & 0x1FFCu);
    ASSERT_EQ(0xB0B0B0B0u, t_mmio_r(&g, 0x40186C));
    ASSERT_EQ((uint32_t)WL_NV2A_CLASS_KELVIN, t_mmio_r(&g, 0x40014C) & 0xFFu);
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 55u);
    // the ISR clears it: the rest runs
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_ERROR);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    ASSERT_EQ(g.put, t_get(&g));
    ASSERT_EQ(55u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    t_destroy(&g);
    PASS();
}

// two NOTIFYs that wake the title, in one kick: two interrupts, the channel stopped after each until
// the ISR acknowledges it, and only while PGRAPH's FIFO access is on
TEST each_awakening_notify_stops_the_channel_until_acknowledged(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    WlNv2aStats s;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, WL_NV2A_PMC_INTR_EN_HARDWARE);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, WL_NV2A_PGRAPH_INTR_NOTIFY);
    t_m1(&g, WL_NV2A_K_NOTIFY, 1);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 7);
    t_m1(&g, WL_NV2A_K_NOTIFY, 1);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 9);
    t_kick(&g);
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.notifies);
    ASSERT(wl_nv2a_irq_pending(g.dev));
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 7u);
    // the ISR with FIFO access off: acknowledged, still stopped until access is on again
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 0);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_NOTIFY);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 7u);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 1);
    // the second NOTIFY: its own interrupt, the method after it not run yet
    ASSERT_EQ(7u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT(wl_nv2a_irq_pending(g.dev));
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(2u, s.notifies);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_NOTIFY);
    ASSERT_EQ(9u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    // (with PGRAPH's NOTIFY interrupt off a NOTIFY does not stop the channel)
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, 0);
    t_m1(&g, WL_NV2A_K_NOTIFY, 1);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 11);
    t_kick(&g);
    ASSERT_EQ(11u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    t_destroy(&g);
    PASS();
}

// an acquire whose value is not there stops the channel before it; tried again at the next vertical
// blank and doorbell, it goes on once the value is there
TEST an_unmet_acquire_waits_for_its_value(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    WlNv2aStats s;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_w32(&g, 0x8020, 1);
    t_m1(&g, WL_NV2A_PULLER_DMA_SEMAPHORE, 0xD0000001u);
    t_m1(&g, WL_NV2A_PULLER_SEMAPHORE_OFFSET, 0x20);
    t_m1(&g, WL_NV2A_PULLER_SEMAPHORE_ACQUIRE, 5);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 3);
    t_kick(&g);
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 3u);
    ASSERT(t_get(&g) != g.put);
    wl_nv2a_vblank(g.dev);
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 3u);
    // the title writes the value: the next vertical blank finds it
    t_w32(&g, 0x8020, 5);
    wl_nv2a_vblank(g.dev);
    ASSERT_EQ(3u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.acquires_unmet);
    // again, met at the next doorbell (a kick with more after it)
    t_m1(&g, WL_NV2A_PULLER_SEMAPHORE_ACQUIRE, 6);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 4);
    t_kick(&g);
    ASSERT_EQ(3u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    t_w32(&g, 0x8020, 6);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_FUNC, WL_NV2A_FUNC_GREATER);
    t_kick(&g);
    ASSERT_EQ(4u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ((uint32_t)WL_NV2A_FUNC_GREATER, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_FUNC));
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(2u, s.acquires_unmet);
    t_destroy(&g);
    PASS();
}

// TRAPPED_ADDR: the method (12:2), the subchannel (18:16), the channel (24:20)
TEST a_trap_on_another_subchannel_names_it(void) {
    TGuest g;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 1);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, 0xFFFFFFFFu);
    // the 3D object bound to subchannel 3 too, then a NO_OPERATION with a parameter there
    t_word(&g, (1u << 18) | (3u << 13) | 0u);
    t_word(&g, 0x97000001u);
    t_word(&g, (1u << 18) | (3u << 13) | WL_NV2A_K_NO_OPERATION);
    t_word(&g, 0x55);
    t_kick(&g);
    ASSERT(wl_nv2a_irq_pending(g.dev));
    ASSERT_EQ((uint32_t)(3u << 16 | WL_NV2A_K_NO_OPERATION), t_mmio_r(&g, 0x400704));
    t_destroy(&g);
    PASS();
}

// an invalid command stops the channel: DMA_PUSHER raised, GET at PUT, nothing after it run
TEST a_pusher_error_stops_the_channel(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    WlNv2aStats s;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_word(&g, WL_NV2A_CMD_RETURN); // (a return outside a call)
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 21);
    t_kick(&g);
    ASSERT(t_mmio_r(&g, WL_NV2A_PFIFO_INTR_0) & WL_NV2A_PFIFO_INTR_DMA_PUSHER);
    ASSERT_EQ(g.put, t_get(&g));
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 21u);
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.pusher_errors);
    // (the channel goes on with what is written after)
    t_mmio(&g, WL_NV2A_PFIFO_INTR_0, WL_NV2A_PFIFO_INTR_DMA_PUSHER);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 22);
    t_kick(&g);
    ASSERT_EQ(22u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    t_destroy(&g);
    PASS();
}

// PTIMER's alarm: raised when the time passes ALARM_0, by its own clock (a vertical blank's time on)
// or by the configuration's
static uint64_t g_time_ns;

static uint64_t test_time_ns(void *user) {
    (void)user;
    return g_time_ns;
}

TEST the_ptimer_alarm_rises_when_the_time_passes_it(void) {
    TGuest g;
    WlNv2aConfig cfg;
    WlNv2a *dev;
    uint32_t now;
    ASSERT(t_create(&g, NULL) != NULL);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 1);
    t_mmio(&g, WL_NV2A_PTIMER_INTR_EN_0, WL_NV2A_PTIMER_INTR_ALARM);
    now = t_mmio_r(&g, WL_NV2A_PTIMER_TIME_0);
    t_mmio(&g, WL_NV2A_PTIMER_ALARM_0, now + 20000000u);
    wl_nv2a_vblank(g.dev);
    ASSERT_EQ(0u, t_mmio_r(&g, WL_NV2A_PTIMER_INTR_0));
    wl_nv2a_vblank(g.dev);
    ASSERT_EQ((uint32_t)WL_NV2A_PTIMER_INTR_ALARM, t_mmio_r(&g, WL_NV2A_PTIMER_INTR_0));
    ASSERT(t_mmio_r(&g, WL_NV2A_PMC_INTR_0) & WL_NV2A_PMC_INTR_PTIMER);
    t_mmio(&g, WL_NV2A_PTIMER_INTR_0, WL_NV2A_PTIMER_INTR_ALARM);
    ASSERT_EQ(0u, t_mmio_r(&g, WL_NV2A_PTIMER_INTR_0));
    t_destroy(&g);
    // the configuration's clock
    memset(&cfg, 0, sizeof cfg);
    g.ram = calloc(1, T_RAM);
    cfg.phys_ptr = t_phys;
    cfg.user = &g;
    cfg.ram_bytes = T_RAM;
    cfg.time_ns = test_time_ns;
    g_time_ns = 1000;
    dev = wl_nv2a_create(&cfg);
    ASSERT(dev != NULL);
    wl_nv2a_mmio_write(dev, WL_NV2A_PTIMER_ALARM_0, 5000, 4);
    ASSERT_EQ(1000u & ~0x1Fu, wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_TIME_0, 4));
    ASSERT_EQ(0u, wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_INTR_0, 4));
    g_time_ns = 6000;
    ASSERT_EQ(6000u & ~0x1Fu, wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_TIME_0, 4));
    ASSERT_EQ((uint32_t)WL_NV2A_PTIMER_INTR_ALARM, wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_INTR_0, 4));
    // a jump of more than 2^31 ns past the alarm (a long pause) raises it too
    wl_nv2a_mmio_write(dev, WL_NV2A_PTIMER_INTR_0, WL_NV2A_PTIMER_INTR_ALARM, 4);
    wl_nv2a_mmio_write(dev, WL_NV2A_PTIMER_ALARM_0, 7000, 4);
    g_time_ns = 6000u + 3000000000u;
    (void)wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_TIME_0, 4);
    ASSERT_EQ((uint32_t)WL_NV2A_PTIMER_INTR_ALARM, wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_INTR_0, 4));
    // and one past a whole wrap of the low word
    wl_nv2a_mmio_write(dev, WL_NV2A_PTIMER_INTR_0, WL_NV2A_PTIMER_INTR_ALARM, 4);
    wl_nv2a_mmio_write(dev, WL_NV2A_PTIMER_ALARM_0, (uint32_t)g_time_ns + 10u, 4);
    g_time_ns += ((uint64_t)1 << 32) + 5u;
    (void)wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_TIME_0, 4);
    ASSERT_EQ((uint32_t)WL_NV2A_PTIMER_INTR_ALARM, wl_nv2a_mmio_read(dev, WL_NV2A_PTIMER_INTR_0, 4));
    wl_nv2a_destroy(dev);
    free(g.ram);
    PASS();
}

// D3D's way with a software method: its ISR has the GPU's interrupts off until its DPC runs, so a
// trap can come while they are off; the DPC turns PGRAPH_FIFO off, acknowledges the trap, reads and
// runs the method, then turns PGRAPH_FIFO on. The channel stays stopped at the trap through all of it,
// so the method after it (a second trap here) cannot replace TRAPPED_DATA before it is read.
TEST a_trap_waits_for_pgraph_fifo(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 0); // the ISR turned them off
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_NO_OPERATION, 0x304);
    t_m1(&g, WL_NV2A_K_NO_OPERATION, 0x320);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 66);
    t_kick(&g);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev)); // not delivered, but the channel waits all the same
    ASSERT(t_get(&g) != g.put);
    ASSERT_EQ(0x304u, t_mmio_r(&g, 0x400708));
    // the DPC: interrupts on, PGRAPH_FIFO off, acknowledge, read, PGRAPH_FIFO on
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 1);
    ASSERT(wl_nv2a_irq_pending(g.dev));
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 0);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_ERROR);
    ASSERT_EQ(0x304u, t_mmio_r(&g, 0x400708)); // still the first: nothing ran on
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 66u);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 1);
    // on to the second trap, which waits in turn
    ASSERT_EQ(0x320u, t_mmio_r(&g, 0x400708));
    ASSERT(t_mmio_r(&g, WL_NV2A_PGRAPH_INTR) & WL_NV2A_PGRAPH_INTR_ERROR);
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 66u);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 0);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_ERROR);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 1);
    ASSERT_EQ(g.put, t_get(&g));
    ASSERT_EQ(66u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    t_destroy(&g);
    PASS();
}

// D3D's PGRAPH_FIFO write runs the channel while its ISR still has the GPU's interrupts off: an
// awakening NOTIFY stops it there all the same (PGRAPH_INTR_EN is what counts, as at a trap), so two
// of them are still two interrupts
TEST an_awakening_notify_stops_the_channel_with_the_interrupts_off(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    WlNv2aStats s;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, 0);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR_EN, WL_NV2A_PGRAPH_INTR_NOTIFY);
    t_m1(&g, WL_NV2A_K_NOTIFY, 1);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 7);
    t_m1(&g, WL_NV2A_K_NOTIFY, 1);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 9);
    t_kick(&g);
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(1u, s.notifies);
    ASSERT_FALSE(wl_nv2a_irq_pending(g.dev));
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 7u);
    t_mmio(&g, WL_NV2A_PMC_INTR_EN_0, WL_NV2A_PMC_INTR_EN_HARDWARE);
    ASSERT(wl_nv2a_irq_pending(g.dev));
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 0);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_NOTIFY);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 1);
    ASSERT_EQ(7u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    wl_nv2a_stats(g.dev, &s);
    ASSERT_EQ(2u, s.notifies);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_NOTIFY);
    ASSERT_EQ(9u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    t_destroy(&g);
    PASS();
}

// with PGRAPH's FIFO access off, a doorbell runs nothing; the methods run once it is on again
TEST nothing_runs_while_pgraph_fifo_access_is_off(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 0);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 21);
    t_kick(&g);
    ASSERT(wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF) != 21u);
    ASSERT(t_get(&g) != g.put);
    wl_nv2a_vblank(g.dev);
    ASSERT(t_get(&g) != g.put);
    t_mmio(&g, WL_NV2A_PGRAPH_FIFO, 1);
    ASSERT_EQ(21u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    t_destroy(&g);
    PASS();
}

// a method run outside a kick (wl_nv2a_method) leaves no stall for the channel's next kick: an
// acquire not met and a trap stop no pusher there
TEST a_method_outside_a_kick_leaves_no_stall(void) {
    TGuest g;
    const WlNv2aKelvin *k;
    ASSERT(t_create(&g, NULL) != NULL);
    t_setup_3d(&g);
    k = wl_nv2a_kelvin(g.dev);
    wl_nv2a_method(g.dev, 0, WL_NV2A_PULLER_SEMAPHORE_ACQUIRE, 0x12345678u);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 31);
    t_kick(&g);
    ASSERT_EQ(31u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    wl_nv2a_method(g.dev, 0, WL_NV2A_K_NO_OPERATION, 0x304);
    t_mmio(&g, WL_NV2A_PGRAPH_INTR, WL_NV2A_PGRAPH_INTR_ERROR);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 32);
    t_kick(&g);
    ASSERT_EQ(32u, wl_nv2a_k(k, WL_NV2A_K_SET_ALPHA_REF));
    ASSERT_EQ(g.put, t_get(&g));
    t_destroy(&g);
    PASS();
}

// ---- capture and replay ---------------------------------------------------------------------------

typedef struct Sink {
    uint8_t *data;
    size_t size, capacity;
} Sink;

static WlResult sink_write(void *user, const void *data, uint32_t bytes) {
    Sink *s = user;
    if (s->size + bytes > s->capacity) {
        size_t capacity = (s->size + bytes) * 2;
        uint8_t *grown = realloc(s->data, capacity);
        if (grown == NULL) {
            return WL_ERR_NOMEM;
        }
        s->data = grown;
        s->capacity = capacity;
    }
    memcpy(s->data + s->size, data, bytes);
    s->size += bytes;
    return WL_OK;
}

static uint32_t g_replay_alpha_ref, g_replay_flips;

static void replay_flip(void *user, WlNv2a *dev, uint32_t index) {
    (void)user;
    g_replay_alpha_ref = wl_nv2a_k(wl_nv2a_kelvin(dev), WL_NV2A_K_SET_ALPHA_REF);
    g_replay_flips = index + 1;
}

TEST capture_replays_to_the_same_draws(void) {
    TGuest g;
    Counts c, rc;
    WlNv2aBackend backend = counting_backend(&c), replay_backend = counting_backend(&rc);
    WlNv2aCaptureSink sink;
    WlNv2aReplayHooks hooks;
    WlNv2aReplayInfo info;
    Sink s;
    memset(&s, 0, sizeof s);
    ASSERT(t_create(&g, &backend) != NULL);
    t_setup_3d(&g);
    sink.user = &s;
    sink.write = sink_write;
    // armed now, begun at the next flip, ended two flips later
    ASSERT_EQ(WL_OK, wl_nv2a_capture_frames(g.dev, &sink, 2));
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 1); // (before the capture: in its snapshot)
    t_kick(&g);
    t_mmio(&g, WL_NV2A_PCRTC_START, 0x100000);
    ASSERT_EQ(1u, (unsigned)wl_nv2a_capture_active(g.dev));
    // a frame: vertices in guest memory, a draw, a clear, a flip
    {
        float tri[9] = {0, 0, 0, 10, 0, 0, 0, 10, 0};
        unsigned i;
        for (i = 0; i < 9; i++) {
            t_w32(&g, 0x50000 + 4 * i, t_f(tri[i]));
        }
    }
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET, 0x50000);
    t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT, (12u << 8) | (3u << 4) | WL_NV2A_VTX_F);
    t_m1(&g, WL_NV2A_K_CLEAR_SURFACE, WL_NV2A_CLEAR_COLOR | WL_NV2A_CLEAR_Z);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (2u << 24) | 0);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 77);
    t_kick(&g);
    wl_nv2a_vblank(g.dev);
    t_mmio(&g, WL_NV2A_PCRTC_START, 0x200000);
    t_m1(&g, WL_NV2A_K_SET_ALPHA_REF, 88);
    t_kick(&g);
    t_mmio(&g, WL_NV2A_PCRTC_START, 0x100000);
    ASSERT_EQ(0u, (unsigned)wl_nv2a_capture_active(g.dev));
    ASSERT(s.size > 16);

    memset(&hooks, 0, sizeof hooks);
    hooks.flip = replay_flip;
    ASSERT_EQ(WL_OK, wl_nv2a_replay(s.data, s.size, &replay_backend, &hooks, &info));
    ASSERT_EQ(1u, rc.draws);
    ASSERT_EQ(3u, rc.last_count);
    ASSERT_EQ(1u, rc.clears);
    ASSERT_EQ(2u, rc.flips);
    ASSERT_EQ(2u, g_replay_flips);
    ASSERT_EQ(88u, g_replay_alpha_ref);
    ASSERT_EQ(1u, info.vblanks);
    ASSERT(info.blocks >= 2); // push buffers and the vertices
    // a damaged file
    s.data[0] = 'X';
    ASSERT_EQ(WL_ERR_FORMAT, wl_nv2a_replay(s.data, s.size, NULL, NULL, NULL));
    free(s.data);
    t_destroy(&g);
    PASS();
}

SUITE(nv2a_suite) {
    RUN_TEST(registers_answer_as_an_idle_gpu);
    RUN_TEST(pramin_is_the_devices_own_memory);
    RUN_TEST(direct3d_handshake_jump_at_physical_zero);
    RUN_TEST(methods_increasing_and_non_increasing);
    RUN_TEST(a_method_header_goes_on_across_kicks);
    RUN_TEST(jumps_calls_and_returns);
    RUN_TEST(ring_wrap_through_a_jump);
    RUN_TEST(a_jump_to_itself_is_stopped_and_the_channel_goes_on);
    RUN_TEST(objects_bound_through_ramht_and_context_dmas);
    RUN_TEST(semaphores_written_into_guest_memory);
    RUN_TEST(notify_raises_the_pgraph_interrupt);
    RUN_TEST(reports_count_the_pixels_drawn);
    RUN_TEST(vblank_interrupt_and_flip);
    RUN_TEST(a_trapped_method_stops_the_channel_until_acknowledged);
    RUN_TEST(each_awakening_notify_stops_the_channel_until_acknowledged);
    RUN_TEST(an_unmet_acquire_waits_for_its_value);
    RUN_TEST(a_trap_on_another_subchannel_names_it);
    RUN_TEST(a_pusher_error_stops_the_channel);
    RUN_TEST(the_ptimer_alarm_rises_when_the_time_passes_it);
    RUN_TEST(a_trap_waits_for_pgraph_fifo);
    RUN_TEST(an_awakening_notify_stops_the_channel_with_the_interrupts_off);
    RUN_TEST(nothing_runs_while_pgraph_fifo_access_is_off);
    RUN_TEST(a_method_outside_a_kick_leaves_no_stall);
    RUN_TEST(capture_replays_to_the_same_draws);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(nv2a_suite);
    GREATEST_MAIN_END();
}
