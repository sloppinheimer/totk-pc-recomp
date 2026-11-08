// The other players' names over their Links, whenever a co-op session has other players in view, each in the
// player's colour (ui/coop_colour.h). Positions come from coop_players.c coop_name_tags.
// While the replay finds the start of the game's HUD in its frames (vk_tags.c, vk_tags_live), the tags are laid out
// here with Dear ImGui around their anchor and handed to it: it draws them into the frame before the HUD, so the HUD and
// the game's menus cover them and walls hide them. Otherwise they are drawn with the overlay (ui/menu.cpp
// menu_overlay_frame) over everything, where the overlay's own guess of the camera (TOTK_COOP_TAG_FOV) places them.
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "coop_colour.h"

extern "C" {
#include "cfg.h"
#include "coop_net.h"
#include "host.h"
#include "vk_tags.h"
typedef struct { float x, y, dist, world[3]; int id; char name[17]; } CoopTag;
int coop_name_tags(CoopTag *out, int max, float aspect);
int coop_markers_map_open(void);   // coop_markers.c: the game's full map shows
uint32_t menu_font_gen(void);      // ui/menu.cpp: vk_tags_font's generation of the overlay font
}

static_assert(sizeof(ImDrawVert) == sizeof(VkTagVert) && sizeof(ImDrawIdx) == 2, "vk_tags.c reads ImGui's vertices");

// Something to draw: a session with others in it (or the self test).
extern "C" bool coop_tags_wanted(void) {
    const char *off = cfg_get("TOTK_COOP_TAGS");
    if (off && !strcmp(off, "off")) return false;
    if (cfg_get("TOTK_COOP_TEST_TAGSELF")) return true;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    return *coop_net_target() && coop_net_players(ids, names, COOP_MAX_PLAYERS) > 0;
}

// One tag: a dark box with the name in the player's colour, its bottom centre at c. Nearer is larger and more opaque:
// 1.0 up close, 0.7 at 150 m.
static void tag(ImDrawList *dl, ImVec2 c, const CoopTag *t, float scale) {
    float k = 1.0f - 0.3f * fminf(t->dist / 150.0f, 1.0f);
    ImFont *f = ImGui::GetFont();
    float size = ImGui::GetFontSize() * k;
    ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0, t->name);
    float pad = 5 * scale * k;
    ImVec2 a(c.x - sz.x / 2 - pad, c.y - sz.y - pad * 2), b(c.x + sz.x / 2 + pad, c.y);
    int alpha = (int)(255 * k);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, alpha * 140 / 255), 4 * scale);
    dl->AddText(f, size, ImVec2(a.x + pad, a.y + pad), coop_colour(t->id, alpha), t->name);
}

// The tags for the replay: each laid out around (0, 0), in pixels of this window, with its indices.
static void hand_over(const CoopTag *tags, int n, float h, float scale) {
    static ImDrawList *dl;
    if (!dl) dl = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    dl->_ResetForNewFrame();
    dl->PushTextureID(ImGui::GetIO().Fonts->TexID);
    dl->PushClipRect(ImVec2(-1e5f, -1e5f), ImVec2(1e5f, 1e5f));
    VkTagItem it[VK_TAGS_MAX];
    int m = 0;
    for (int i = 0; i < n && m < VK_TAGS_MAX; i++) {
        uint32_t first = (uint32_t)dl->IdxBuffer.Size;
        tag(dl, ImVec2(0, 0), &tags[i], scale);
        it[m].world[0] = tags[i].world[0], it[m].world[1] = tags[i].world[1], it[m].world[2] = tags[i].world[2];
        it[m].first = first, it[m++].count = (uint32_t)dl->IdxBuffer.Size - first;
    }
    vk_tags_set((const VkTagVert *)dl->VtxBuffer.Data, (uint32_t)dl->VtxBuffer.Size, dl->IdxBuffer.Data,
                (uint32_t)dl->IdxBuffer.Size, it, m, h, menu_font_gen());
}

extern "C" void coop_tags_draw(float w, float h, float scale, bool menu_open) {
    // the full map covers the world: its markers name the players instead
    if (menu_open || !coop_tags_wanted() || coop_markers_map_open()) {
        vk_tags_set(nullptr, 0, nullptr, 0, nullptr, 0, h, 0);
        return;
    }
    CoopTag tags[COOP_MAX_PLAYERS + 1];
    int n = coop_name_tags(tags, COOP_MAX_PLAYERS + 1, h > 0 ? w / h : 16.0f / 9.0f);
    static int t;
    if (cfg_get("TOTK_COOP_TEST_TAGSELF") && ++t % 120 == 0)
        for (int i = 0; i < n; i++)
            rt_log("coop: tag %s at %.3f %.3f of %.0fx%.0f, %.1f m (%s)", tags[i].name, tags[i].x, tags[i].y, w, h,
                   tags[i].dist, vk_tags_live() ? "in the frame" : "overlay");
    if (vk_tags_live()) {
        hand_over(tags, n, h, scale);
        return;
    }
    vk_tags_set(nullptr, 0, nullptr, 0, nullptr, 0, h, 0);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < n; i++) {
        ImVec2 c(tags[i].x * w, tags[i].y * h);
        if (c.x < -w * 0.1f || c.x > w * 1.1f || c.y < 0 || c.y > h * 1.1f) continue;
        tag(dl, c, &tags[i], scale);
    }
}
