// Each co-op player's colour, the same in every game: it follows the player's id, which the host's relay gives out
// lowest free first (coop_relay.c, HELLO), so the first to join is 1 on every screen. Used by the name tags
// (ui/coop_tags.cpp), the notices (ui/coop_notices.cpp), the map and minimap markers (ui/coop_markers.cpp) and the
// player list of the Co-op tab (ui/coop_tab.cpp). Id 0 (the self tests) is white.
#pragma once
#include "imgui.h"

// Bright enough on the map's parchment, the minimap's dark and the tags' dark backing.
static inline void coop_colour_rgb(int id, unsigned char out[3]) {
    static const unsigned char c[8][3] = {{80, 200, 255}, {255, 120, 80}, {120, 230, 110}, {255, 210, 70},
                                          {220, 120, 255}, {255, 110, 170}, {90, 240, 220}, {250, 250, 250}};
    const unsigned char *p = id > 0 ? c[(id - 1) & 7] : c[7];
    out[0] = p[0], out[1] = p[1], out[2] = p[2];
}

static inline ImU32 coop_colour(int id, int alpha) {
    unsigned char p[3];
    coop_colour_rgb(id, p);
    return IM_COL32(p[0], p[1], p[2], alpha);
}

static inline ImVec4 coop_colour_v4(int id) {
    unsigned char p[3];
    coop_colour_rgb(id, p);
    return ImVec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f);
}
