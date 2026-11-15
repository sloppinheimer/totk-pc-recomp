// The co-op parts of the in-game menu's overlay (ui/coop_menu.h): what ui/menu.cpp calls each frame, gathered here so
// upstream's menu.cpp carries a few lines of co-op code.
#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"

extern "C" {
#include "cfg.h"
#include "ui/coop_menu.h"
#include "vk_tags.h"
bool coop_tags_wanted(void);   // ui/coop_tags.cpp: the other players' names over their Links
void coop_tags_draw(float w, float h, float scale, bool menu_open);
bool coop_markers_wanted(void);   // ui/coop_markers.cpp: the other players on the map and the minimap
void coop_markers_draw(float w, float h, float scale, bool menu_open);
const char *coop_markers_shot(uint64_t frame);
int coop_notices_wanted(void);   // coop_notices.c: joins, leaves and fast travels to show (checked each frame)
void coop_notices_draw(float w, float h, float scale, bool menu_open);   // ui/coop_notices.cpp
const char *coop_notices_shot(void);   // ui/coop_notices.cpp: TOTK_COOP_TEST_NOTICESHOT's file for this frame, or NULL
}

extern "C" bool coop_overlay_wanted(void) {
    bool notices = coop_notices_wanted();   // first: each frame, it also finds the joins and leaves
    return coop_tags_wanted() || coop_markers_wanted() || notices;
}

extern "C" void coop_overlay_draw(float w, float h, float scale, bool menu_open) {
    coop_markers_draw(w, h, scale, menu_open);
    coop_tags_draw(w, h, scale, menu_open);
    coop_notices_draw(w, h, scale, menu_open);
}

extern "C" const char *coop_overlay_shot(uint64_t frame) {
    const char *shot = coop_markers_shot(frame);   // TOTK_COOP_TEST_MARKSHOTS (tests)
    if (!shot) shot = coop_notices_shot();   // (after the markers': it overwrote their shot, merge 53e62d1)
    // TOTK_COOP_TEST_TAGSHOT=<frame>: that frame's swapchain image, overlay and all, to TOTK_MENU_TEST_DIR (tests)
    static long long tag_shot = -2;
    if (tag_shot == -2) tag_shot = cfg_get("TOTK_COOP_TEST_TAGSHOT") ? atoll(cfg_get("TOTK_COOP_TEST_TAGSHOT")) : -1;
    if (!shot && tag_shot > 0 && frame >= (uint64_t)tag_shot) {
        static char tpath[512];
        const char *dir = cfg_get("TOTK_MENU_TEST_DIR");
        snprintf(tpath, sizeof tpath, "%s/tag-%llu.png", dir ? dir : ".", (unsigned long long)frame);
        shot = tpath;
        tag_shot = -1;
    }
    return shot;
}

static uint32_t g_font_gen;   // vk_tags_font's generation of the menu's atlas
extern "C" uint32_t menu_font_gen(void) { return g_font_gen; }
extern "C" void coop_menu_font(void *atlas) {
    unsigned char *px = nullptr;
    int fw = 0, fh = 0;
    ((ImFontAtlas *)atlas)->GetTexDataAsRGBA32(&px, &fw, &fh);
    g_font_gen = vk_tags_font(px, fw, fh);
}
