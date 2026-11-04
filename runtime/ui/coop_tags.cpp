// The other players' names over their Links, drawn with the overlay (ui/menu.cpp menu_overlay_frame) whenever a
// co-op session has other players in view. Positions come from coop_players.c coop_name_tags.
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "cfg.h"
#include "coop_net.h"
#include "host.h"
typedef struct { float x, y, dist; char name[17]; } CoopTag;
int coop_name_tags(CoopTag *out, int max, float aspect);
}

// Something to draw: a session with others in it (or the self test).
extern "C" bool coop_tags_wanted(void) {
    const char *off = cfg_get("TOTK_COOP_TAGS");
    if (off && !strcmp(off, "off")) return false;
    if (cfg_get("TOTK_COOP_TEST_TAGSELF")) return true;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    return *coop_net_target() && coop_net_players(ids, names, COOP_MAX_PLAYERS) > 0;
}

extern "C" void coop_tags_draw(float w, float h, float scale, bool menu_open) {
    if (menu_open || !coop_tags_wanted()) return;
    CoopTag tags[COOP_MAX_PLAYERS + 1];
    int n = coop_name_tags(tags, COOP_MAX_PLAYERS + 1, h > 0 ? w / h : 16.0f / 9.0f);
    static int t;
    if (cfg_get("TOTK_COOP_TEST_TAGSELF") && ++t % 120 == 0)
        for (int i = 0; i < n; i++) rt_log("coop: tag %s at %.3f %.3f of %.0fx%.0f, %.1f m", tags[i].name, tags[i].x, tags[i].y, w, h, tags[i].dist);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < n; i++) {
        // nearer is larger and more opaque: 1.0 up close, 0.7 at 150 m
        float k = 1.0f - 0.3f * fminf(tags[i].dist / 150.0f, 1.0f);
        ImFont *f = ImGui::GetFont();
        float size = ImGui::GetFontSize() * k;
        ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0, tags[i].name);
        float pad = 5 * scale * k;
        ImVec2 c(tags[i].x * w, tags[i].y * h);
        ImVec2 a(c.x - sz.x / 2 - pad, c.y - sz.y - pad * 2), b(c.x + sz.x / 2 + pad, c.y);
        if (b.x < 0 || a.x > w || b.y < 0 || a.y > h) continue;
        int alpha = (int)(255 * k);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, alpha * 140 / 255), 4 * scale);
        dl->AddText(f, size, ImVec2(a.x + pad, a.y + pad), IM_COL32(255, 240, 200, alpha), tags[i].name);
    }
}
