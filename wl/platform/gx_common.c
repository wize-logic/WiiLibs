// SPDX-License-Identifier: MIT
// GX helpers that need no hardware: texture size arithmetic shared by the Wii backend, the host stub and
// the tools that lay out texture memory (declarations in platform.h).
#include "wl/platform/platform.h"
#include "wl/platform/gx_direct.h"

size_t wl_gx_tex_bytes(WlGxTexFormat fmt, uint32_t width, uint32_t height) {
    // tile width, tile height, bytes per tile
    static const uint8_t tile[WL_GX_TEX_FORMAT_COUNT][3] = {
        {8, 8, 32}, {8, 4, 32}, {8, 4, 32}, {4, 4, 32}, {4, 4, 32}, {4, 4, 32}, {4, 4, 64}, {8, 8, 32},
        {8, 8, 32}, {8, 4, 32}};
    if ((unsigned)fmt >= WL_GX_TEX_FORMAT_COUNT) {
        return 0;
    }
    size_t tx = (width + tile[fmt][0] - 1u) / tile[fmt][0];
    size_t ty = (height + tile[fmt][1] - 1u) / tile[fmt][1];
    return tx * ty * tile[fmt][2];
}

void wl_gx_ramp_fill(void *image) {
    uint8_t *texels = image;
    // I8 is stored in tiles of 8 by 4 texels, 32 bytes each, a row of 8 texels after another: the
    // 256 by 4 ramp is one row of 32 tiles, and a texel's value is its x coordinate
    for (uint32_t y = 0; y < WL_GX_RAMP_HEIGHT; y++) {
        for (uint32_t x = 0; x < WL_GX_RAMP_WIDTH; x++) {
            texels[(x / 8u) * 32u + y * 8u + (x % 8u)] = (uint8_t)x;
        }
    }
}

const void *wl_gx_ramp_image(void) {
    static _Alignas(32) uint8_t image[WL_GX_RAMP_BYTES];
    static bool made;
    if (!made) {
        wl_gx_ramp_fill(image);
        wl_gpu_buffer_commit(image, sizeof image);
        made = true;
    }
    return image;
}

size_t wl_gx_tex_bytes_mips(WlGxTexFormat fmt, uint32_t width, uint32_t height, uint32_t mips) {
    size_t total = 0;
    for (uint32_t i = 0; i < (mips == 0 ? 1u : mips); i++) {
        uint32_t w = width >> i;
        uint32_t h = height >> i;
        total += wl_gx_tex_bytes(fmt, w == 0 ? 1u : w, h == 0 ? 1u : h);
    }
    return total;
}

void wl_gx_resolve_filters(const WlGxSampler *s, uint32_t mips, WlGxFilterOverride override_mode, uint8_t *min_out,
                           uint8_t *mag_out) {
    uint8_t minf = s->min_filter;
    uint8_t magf = s->mag_filter;
    if (!s->pinned && override_mode != WL_GX_FILTER_OVERRIDE_NONE) {
        bool lin = override_mode == WL_GX_FILTER_OVERRIDE_BILINEAR;
        bool mip_linear = minf >= WL_GX_FILTER_NEAREST_MIP_LINEAR;
        magf = lin ? (uint8_t)WL_GX_FILTER_LINEAR : (uint8_t)WL_GX_FILTER_NEAREST;
        if (mips > 1u && minf >= WL_GX_FILTER_NEAREST_MIP_NEAREST) {
            minf = (uint8_t)(lin ? (mip_linear ? WL_GX_FILTER_LINEAR_MIP_LINEAR : WL_GX_FILTER_LINEAR_MIP_NEAREST)
                                 : (mip_linear ? WL_GX_FILTER_NEAREST_MIP_LINEAR : WL_GX_FILTER_NEAREST_MIP_NEAREST));
        } else {
            minf = magf;
        }
    }
    if (mips <= 1u && minf >= WL_GX_FILTER_NEAREST_MIP_NEAREST) {
        minf = (minf & 1u) != 0 ? (uint8_t)WL_GX_FILTER_LINEAR : (uint8_t)WL_GX_FILTER_NEAREST;
    }
    *min_out = minf;
    *mag_out = magf;
}

uint32_t wl_gx_check_draw(uint32_t chans, uint32_t texgens, uint32_t maps_loaded, const WlTevProgram *tev) {
    uint32_t found = 0, i, k;

    if (chans == 0 && texgens == 0) {
        found |= WL_GX_CHECK_NO_OUTPUT;
    }
    if (tev == NULL) {
        return found;
    }
    if (tev->stage_count == 0 || tev->stage_count > WL_TEV_MAX_STAGES) {
        return found | WL_GX_CHECK_STAGES;
    }
    for (i = 0; i < tev->stage_count; i++) {
        const WlTevStage *s = &tev->stages[i];
        bool ras = false, texel = false;

        for (k = 0; k < 4; k++) {
            ras = ras || s->color.in[k] == WL_TEV_CC_RASC || s->color.in[k] == WL_TEV_CC_RASA ||
                  s->alpha.in[k] == WL_TEV_CA_RASA;
            texel = texel || s->color.in[k] == WL_TEV_CC_TEXC || s->color.in[k] == WL_TEV_CC_TEXA ||
                    s->alpha.in[k] == WL_TEV_CA_TEXA;
        }
        if ((s->channel == WL_TEV_CH_COLOR0A0 && chans < 1u) || (s->channel == WL_TEV_CH_COLOR1A1 && chans < 2u)) {
            found |= WL_GX_CHECK_CHANNEL;
        }
        if (ras && s->channel == WL_TEV_NONE) {
            found |= WL_GX_CHECK_RAS_NULL;
        }
        if (s->texcoord != WL_TEV_NONE && s->texcoord >= texgens) {
            found |= WL_GX_CHECK_TEXCOORD;
        }
        if (texel && s->texmap == WL_TEV_NONE) {
            found |= WL_GX_CHECK_TEX_NULL;
        }
        if (s->texmap != WL_TEV_NONE && (s->texmap >= 8u || !(maps_loaded & (1u << s->texmap)))) {
            found |= WL_GX_CHECK_TEXMAP;
        }
    }
    return found;
}

const char *wl_gx_check_name(unsigned bit) {
    static const char *const names[WL_GX_CHECKS] = {
        "no colour channel and no texture coordinate", "a stage's colour channel not made",
        "the rasterised colour read with no channel", "a stage's texture coordinate not made",
        "the texel read with no texture map", "a stage's texture map empty", "no TEV stage or too many"};

    return bit < WL_GX_CHECKS ? names[bit] : "?";
}

void wl_gx_direct_layout_outputs(const WlGxDirectLayout *layout, uint32_t *chans, uint32_t *texgens) {
    uint32_t n, made = 0;

    for (n = 0; n < 8; n++) {
        if ((layout->texcoords | layout->texcoords_generated | layout->texcoords_projected) & (1u << n)) {
            made = n + 1u;
        }
    }
    *texgens = made;
    *chans = layout->colors != 0 || made != 0 ? layout->colors : 1u;
}
