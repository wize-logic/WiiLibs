// SPDX-License-Identifier: MIT
// FSYS file-type dispatch names. XD's dispatch table `fsys_datatype` has 27 records (ids
// 0x00-0x1A, 0x1B0 bytes). Battle Revolution's handler table gives the named ids below; the other
// ids stay numeric until their handlers are identified.
#include <stdio.h>

#include "wl/gs/gs.h"

WlGsGame wl_gs_game_from_toc_version(uint32_t version) {
    return version == 0x104u ? WL_GS_GAME_COLOSSEUM
         : version == 0x201u ? WL_GS_GAME_XD
         : version == 0x251u ? WL_GS_GAME_PBR : WL_GS_GAME_UNKNOWN;
}

const char *wl_gs_type_name(WlGsGame game, uint32_t type, char buf[24]) {
    switch (game == WL_GS_GAME_PBR ? type : 0xFFFFu) {
    case 2: return "scene";
    case 3: return "object_part";
    case 4: return "material_pack";
    case 5: return "particle_effect";
    case 6: return "texture";
    case 9: return "stage_script"; /* TCOD script bytecode */
    case 18: return "lens_flare";
    default: break;
    }
    if (type >= WL_GS_TYPE_COUNT) {
        return "type_out_of_range";
    }
    (void)snprintf(buf, 24, "type_%02u", (unsigned)type);
    return buf;
}
