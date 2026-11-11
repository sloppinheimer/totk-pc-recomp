// The co-op notices (coop_notices.c: a player joined, left or fast travelled), drawn with the overlay
// (ui/menu.cpp menu_overlay_frame) while the menu is closed: one line each at the bottom left of the screen, where the
// game's HUD has nothing (play test: top centre was too much in the way, 2026-11-07), the newest lowest, the older ones
// above it, for COOP_NOTICE_SECONDS, fading in and out. Drawn as ui/coop_tags.cpp draws the name tags.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "coop_colour.h"

extern "C" {
#include "coop_notices.h"
}

extern "C" void coop_notices_draw(float w, float h, float scale, bool menu_open) {
    if (menu_open) return;
    CoopNotice ns[4];
    int n = coop_notices_get(ns, 4);
    if (!n) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *f = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 0.9f, pad = 6 * scale, x = w * 0.025f, y = h * 0.93f;   // y: the lowest line's bottom
    for (int i = n - 1; i >= 0; i--) {   // newest (last) at the bottom, going up
        float a = ns[i].age_s, k = fminf(fminf(a / 0.25f, (COOP_NOTICE_SECONDS - a) / 1.0f), 1.0f);
        if (k <= 0) continue;
        ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0, ns[i].text);
        ImVec2 p0(x, y - sz.y - pad * 2), p1(x + sz.x + pad * 4, y);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, (int)(110 * k)), 5 * scale);
        // the text in three pieces: before the name, the name in the player's colour, after it
        const char *t = ns[i].text, *n0 = t + ns[i].name_at, *n1 = n0 + ns[i].name_len, *end = t + strlen(t);
        ImU32 plain = IM_COL32(255, 240, 200, (int)(230 * k));
        ImVec2 at(p0.x + pad * 2, p0.y + pad);
        if (n0 > t) dl->AddText(f, size, at, plain, t, n0), at.x += f->CalcTextSizeA(size, FLT_MAX, 0, t, n0).x;
        if (n1 > n0) dl->AddText(f, size, at, coop_colour(ns[i].who, (int)(230 * k)), n0, n1), at.x += f->CalcTextSizeA(size, FLT_MAX, 0, n0, n1).x;
        if (end > n1) dl->AddText(f, size, at, plain, n1, end);
        y = p0.y - pad;
    }
}

// Test (TOTK_COOP_TEST_NOTICESHOT=1): each notice's frame, overlay and all, 0.6 s after it is told (faded in), to
// TOTK_MENU_TEST_DIR (else TOTK_SHOTS_DIR, else the game's folder) as notice-<id>.png; the file is logged.
extern "C" const char *cfg_get(const char *name);
extern "C" void rt_log(const char *fmt, ...);
extern "C" const char *coop_notices_shot(void) {
    static int on = -1;
    static uint32_t done;
    static char path[512];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_NOTICESHOT") != NULL;
    if (!on) return NULL;
    CoopNotice ns[4];
    int n = coop_notices_get(ns, 4);
    for (int i = 0; i < n; i++)
        if (ns[i].id > done && ns[i].age_s >= 0.6f) {
            done = ns[i].id;
            const char *dir = cfg_get("TOTK_MENU_TEST_DIR") ? cfg_get("TOTK_MENU_TEST_DIR") : cfg_get("TOTK_SHOTS_DIR");
            snprintf(path, sizeof path, "%s/notice-%u.png", dir ? dir : ".", (unsigned)done);
            rt_log("coop: test: notice %u (%s) saved as %s", (unsigned)done, ns[i].text, path);
            return path;
        }
    return NULL;
}
