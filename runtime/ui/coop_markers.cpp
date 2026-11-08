// The other players' markers on the game's map screen and HUD minimap, drawn with the overlay (ui/menu.cpp
// menu_overlay_frame) while a co-op session has others in it. Where they go: coop_markers.c coop_markers.
// TOTK_COOP_MARKERS=0 (or off) turns them off.
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "coop_colour.h"

extern "C" {
#include "cfg.h"
#include "coop_net.h"
#include "host.h"
typedef struct { float x, y; int id, edge; char name[17]; } CoopMarker;
typedef struct { int mode; float x0, y0, x1, y1, alpha; } CoopMarkView;
int coop_markers(CoopMarker *out, int max, float w, float h, CoopMarkView *v);
int coop_markers_on(void);
}

// Something to draw: markers on and a session with others in it (or the self test).
extern "C" bool coop_markers_wanted(void) {
    if (!coop_markers_on()) return false;
    if (cfg_get("TOTK_COOP_TEST_MARKSELF")) return true;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    return *coop_net_target() && coop_net_players(ids, names, COOP_MAX_PLAYERS) > 0;
}

extern "C" void coop_markers_draw(float w, float h, float scale, bool menu_open) {
    if (menu_open || !coop_markers_wanted()) return;
    CoopMarker mk[COOP_MAX_PLAYERS + 1];
    CoopMarkView v;
    int n = coop_markers(mk, COOP_MAX_PLAYERS + 1, w, h, &v);
    if (!n || !v.mode) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(v.x0, v.y0), ImVec2(v.x1, v.y1), false);
    bool mini = v.mode == 2;
    float k = h / 1080.0f, r = (mini ? 7.0f : 11.0f) * k;
    int alpha = (int)(255 * fminf(fmaxf(v.alpha, 0.0f), 1.0f));
    ImFont *f = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * (mini ? 0.75f : 1.0f);
    for (int i = 0; i < n; i++) {
        ImVec2 c(mk[i].x, mk[i].y);
        // a filled disc with a dark ring; held at the edge (the player is off this view): a ring only
        dl->AddCircleFilled(c, r + 2 * k, IM_COL32(0, 0, 0, alpha * 200 / 255), 20);
        if (mk[i].edge) dl->AddCircle(c, r - 1 * k, coop_colour(mk[i].id, alpha), 20, 3 * k);
        else dl->AddCircleFilled(c, r, coop_colour(mk[i].id, alpha), 20);
        if (mini) continue;   // the minimap shows the dot alone; its colour says who it is
        ImVec2 sz = f->CalcTextSizeA(fs, FLT_MAX, 0, mk[i].name);
        float pad = 3 * scale;
        ImVec2 a(c.x + r + 4 * k, c.y - sz.y / 2 - pad);
        if (a.x + sz.x + 2 * pad > v.x1) a.x = c.x - r - 4 * k - sz.x - 2 * pad;   // no room on the right: the left
        dl->AddRectFilled(a, ImVec2(a.x + sz.x + 2 * pad, a.y + sz.y + 2 * pad), IM_COL32(0, 0, 0, alpha * 150 / 255), 3 * scale);
        dl->AddText(f, fs, ImVec2(a.x + pad, a.y + pad), coop_colour(mk[i].id, alpha), mk[i].name);
    }
    dl->PopClipRect();
}

// TOTK_COOP_TEST_MARKSHOTS=<lo>-<hi>:<step> (world-clock seconds, as TOTK_SHOTS=w...): the swapchain image, overlay
// and all, saved as mark-<s>.png in TOTK_MENU_TEST_DIR (TOTK_SHOTS saves the game's picture before the overlay).
extern "C" const char *coop_markers_shot(void) {
    static int parsed;
    static double lo, hi, step, next;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("TOTK_COOP_TEST_MARKSHOTS");
        if (!e || sscanf(e, "%lf-%lf:%lf", &lo, &hi, &step) != 3 || step <= 0) hi = -1;
        next = lo;
    }
    int64_t us = frame_step_world_us();
    if (hi < 0 || us < 0 || us < next * 1e6 || next > hi) return NULL;
    static char path[512];
    const char *dir = cfg_get("TOTK_MENU_TEST_DIR");
    snprintf(path, sizeof path, "%s/mark-%03.0f.png", dir ? dir : ".", next);
    rt_log("coop: markers: overlay shot %s", path);
    while (next * 1e6 <= us) next += step;
    return path;
}
