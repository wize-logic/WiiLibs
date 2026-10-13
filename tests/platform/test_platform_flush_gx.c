// SPDX-License-Identifier: MIT
// The transform unit's flush for a renderer's own vertex formats (wl/platform/wii/gx_flush_wii.c,
// with gx_vertex.c's sizes) compiled against the recording stand-in for libogc's GX
// (tests/platform/gx_stub): a vertex's bytes from the descriptor and the formats set, the strip of
// zeros sent before the first primitive after a raster state change and only then, none where a
// size is not known or the flush is off, and the descriptor GX misreads. OpenCE-Wii's Direct3D
// device draws through these, so the bytes must be the ones its d3d8_gx.c sent.
#include <ogc/gx.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/gx_direct.h"

static unsigned log_count(const char *prefix) {
    unsigned i, n = 0;

    for (i = 0; i < g_gx_stub_log_count; i++) {
        n += strncmp(g_gx_stub_log[i], prefix, strlen(prefix)) == 0;
    }
    return n;
}

static void setup(void *unused) {
    (void)unused;
    gx_stub_reset();
    wl_gx_direct_set_xf_flush(true);
    // (none due from an earlier test)
    (void)wl_gx_direct_flush_pending(7);
    gx_stub_reset();
}

TEST attribute_sizes_by_format(void) {
    ASSERT_EQ(12, wl_gx_attribute_bytes(WL_GX_VA_POS, WL_GX_COUNT_POS_XYZ, WL_GX_TYPE_F32));
    ASSERT_EQ(4, wl_gx_attribute_bytes(WL_GX_VA_POS, WL_GX_COUNT_POS_XY, WL_GX_TYPE_S16));
    ASSERT_EQ(3, wl_gx_attribute_bytes(WL_GX_VA_NRM, WL_GX_COUNT_NRM_XYZ, WL_GX_TYPE_S8));
    ASSERT_EQ(0, wl_gx_attribute_bytes(WL_GX_VA_NRM, 1, WL_GX_TYPE_S8)); // (NBT: not sized)
    ASSERT_EQ(4, wl_gx_attribute_bytes(WL_GX_VA_CLR0, 1, GX_RGBA8));
    ASSERT_EQ(2, wl_gx_attribute_bytes(WL_GX_VA_CLR1, 1, GX_RGB565));
    ASSERT_EQ(4, wl_gx_attribute_bytes(WL_GX_VA_TEX0 + 3, WL_GX_COUNT_TEX_S, WL_GX_TYPE_F32));
    ASSERT_EQ(4, wl_gx_attribute_bytes(WL_GX_VA_TEX7, 1, WL_GX_TYPE_U16));
    ASSERT_EQ(0, wl_gx_attribute_bytes(WL_GX_VA_TEX0, 1, 7));
    PASS();
}

TEST a_vertex_counts_indices_and_matrix_indices(void) {
    uint8_t bytes[WL_GX_VA_ATTRIBUTES] = {0};
    WlGxVtxDesc desc[] = {{WL_GX_VA_PNMTXIDX, WL_GX_IN_DIRECT}, {WL_GX_VA_POS, WL_GX_IN_INDEX16},
                          {WL_GX_VA_NRM, WL_GX_IN_INDEX8},      {WL_GX_VA_CLR0, WL_GX_IN_DIRECT},
                          {WL_GX_VA_TEX0, WL_GX_IN_NONE},       {WL_GX_VA_END, 0},
                          {WL_GX_VA_TEX7, WL_GX_IN_DIRECT}};

    // (the colour's size not known: no size)
    ASSERT_EQ(0, wl_gx_vertex_bytes(desc, 7, bytes));
    bytes[WL_GX_VA_CLR0] = 4;
    // a matrix index 1, an index16 2, an index8 1, the colour 4; nothing after the end
    ASSERT_EQ(8, wl_gx_vertex_bytes(desc, 7, bytes));
    ASSERT(wl_gx_vertex_shape_ok(desc, 7));
    {
        WlGxVtxDesc alone[] = {{WL_GX_VA_PNMTXIDX, WL_GX_IN_DIRECT},
                               {WL_GX_VA_POS, WL_GX_IN_INDEX16},
                               {WL_GX_VA_CLR0, WL_GX_IN_NONE},
                               {WL_GX_VA_END, 0}};
        WlGxVtxDesc no_matrix[] = {{WL_GX_VA_POS, WL_GX_IN_INDEX16}, {WL_GX_VA_END, 0}};

        ASSERT_FALSE(wl_gx_vertex_shape_ok(alone, 4));
        ASSERT(wl_gx_vertex_shape_ok(no_matrix, 2));
    }
    PASS();
}

// a device's screen-space format: position F32 xyz, two colours RGBA8, four ST and one S coordinate
static void device_format(void) {
    unsigned n;

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    wl_gx_direct_vertex_format(7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    wl_gx_direct_vertex_format(7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    wl_gx_direct_vertex_format(7, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    for (n = 0; n < 5; n++) {
        GX_SetVtxDesc((u8)(GX_VA_TEX0 + n), GX_DIRECT);
        wl_gx_direct_vertex_format(7, GX_VA_TEX0 + n, n < 4 ? GX_TEX_ST : GX_TEX_S, GX_F32, 0);
    }
}

TEST the_flush_goes_before_the_next_primitive_only(void) {
    device_format();
    // (12 + 4 + 4 + 4 * 8 + 4)
    ASSERT_EQ(56, wl_gx_direct_vertex_bytes(7));
    ASSERT_EQ(WL_GX_FLUSH_NOT_DUE, wl_gx_direct_flush_pending(7));
    wl_gx_direct_flush_after();
    g_gx_stub_log_count = 0;
    g_gx_stub.fifo_bytes = 0;
    uint32_t before = wl_gx_direct_flushes();
    wl_gx_direct_begin_format(GX_TRIANGLES, 7, 3);
    // the strip of eight zero vertices (448 bytes, all words), then the primitive
    ASSERT_STR_EQ("GX_Begin 0x98 7 8", g_gx_stub_log[0]);
    ASSERT_EQ(448ul, g_gx_stub.fifo_bytes);
    ASSERT_EQ(112, log_count("GX_Color1u32 0"));
    ASSERT_EQ(0, log_count("GX_Position1x8"));
    ASSERT_STR_EQ("GX_End", g_gx_stub_log[113]);
    ASSERT_STR_EQ("GX_Begin 0x90 7 3", g_gx_stub_log[114]);
    ASSERT_EQ(before + 1, wl_gx_direct_flushes());
    // the next primitive: none
    g_gx_stub_log_count = 0;
    wl_gx_direct_begin_format(GX_TRIANGLES, 7, 3);
    ASSERT_EQ(1, g_gx_stub_log_count);
    ASSERT_EQ(before + 1, wl_gx_direct_flushes());
    PASS();
}

TEST an_indexed_vertex_with_a_matrix_index(void) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    wl_gx_direct_vertex_format(6, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    // (1 + 2 + 4 a vertex; a 3-byte colour, 6)
    ASSERT_EQ(7, wl_gx_direct_vertex_bytes(6));
    wl_gx_direct_vertex_format(6, GX_VA_CLR0, 0, 1 /* GX_RGB8 */, 0);
    ASSERT_EQ(6, wl_gx_direct_vertex_bytes(6));
    wl_gx_direct_flush_after();
    g_gx_stub.fifo_bytes = 0;
    ASSERT_EQ(WL_GX_FLUSH_SENT, wl_gx_direct_flush_pending(6));
    // (eight vertices are always whole words)
    ASSERT_EQ(48ul, g_gx_stub.fifo_bytes);
    ASSERT_EQ(12, log_count("GX_Color1u32"));
    ASSERT_EQ(0, log_count("GX_Position1x8"));
    // texture coordinates by index8 in place of the colour: 1 + 2 + 1 + 1
    GX_SetVtxDesc(GX_VA_CLR0, GX_NONE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GX_SetVtxDesc(GX_VA_TEX0 + 1, GX_INDEX8);
    wl_gx_direct_flush_after();
    g_gx_stub.fifo_bytes = 0;
    ASSERT_EQ(WL_GX_FLUSH_SENT, wl_gx_direct_flush_pending(6));
    ASSERT_EQ(40ul, g_gx_stub.fifo_bytes);
    PASS();
}

TEST a_size_not_known_or_the_flush_off_sends_none(void) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    // (format 5's position never set through wl_gx_direct_vertex_format)
    ASSERT_EQ(0, wl_gx_direct_vertex_bytes(5));
    wl_gx_direct_flush_after();
    ASSERT_EQ(WL_GX_FLUSH_SIZE_UNKNOWN, wl_gx_direct_flush_pending(5));
    ASSERT_EQ(0ul, g_gx_stub.fifo_bytes);
    // (and none is due after it)
    wl_gx_direct_vertex_format(5, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    ASSERT_EQ(WL_GX_FLUSH_NOT_DUE, wl_gx_direct_flush_pending(5));
    // off: none, and none due after
    wl_gx_direct_set_xf_flush(false);
    wl_gx_direct_flush_after();
    ASSERT_EQ(WL_GX_FLUSH_NOT_DUE, wl_gx_direct_flush_pending(5));
    wl_gx_direct_set_xf_flush(true);
    ASSERT_EQ(WL_GX_FLUSH_NOT_DUE, wl_gx_direct_flush_pending(5));
    ASSERT_EQ(0ul, g_gx_stub.fifo_bytes);
    PASS();
}

TEST the_descriptor_gx_misreads(void) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    ASSERT_FALSE(wl_gx_direct_vertex_shape_ok());
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    ASSERT(wl_gx_direct_vertex_shape_ok());
    PASS();
}

SUITE(flush_gx_suite) {
    SET_SETUP(setup, NULL);
    RUN_TEST(attribute_sizes_by_format);
    RUN_TEST(a_vertex_counts_indices_and_matrix_indices);
    RUN_TEST(the_flush_goes_before_the_next_primitive_only);
    RUN_TEST(an_indexed_vertex_with_a_matrix_index);
    RUN_TEST(a_size_not_known_or_the_flush_off_sends_none);
    RUN_TEST(the_descriptor_gx_misreads);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(flush_gx_suite);
    GREATEST_MAIN_END();
}
