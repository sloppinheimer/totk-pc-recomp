// The co-op parts of the in-game menu (ui/coop_menu.cpp, ui/coop_tab.cpp, ui/coop_notices.cpp), as ui/menu.cpp calls
// them: menu.cpp includes this file in its extern "C" block. Kept out of menu.cpp so a merge of upstream's menu.cpp
// meets a few lines of co-op code.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void tab_coop(float scale);   // ui/coop_tab.cpp (sets the page modal itself)
// The co-op overlay drawn over the game with the menu closed or open: the name tags, the map markers and the notices.
// coop_overlay_wanted runs each frame (it also finds the joins and leaves).
bool coop_overlay_wanted(void);
void coop_overlay_draw(float w, float h, float scale, bool menu_open);
const char *coop_overlay_shot(uint64_t frame);   // a test's screenshot file for this frame, or NULL
// The place names the co-op notices show, for the font of lang (coop_notices.c).
void coop_notices_glyphs(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx);
// The menu's font atlas (an ImFontAtlas *), built: the name tags the replay draws into the frame take a copy (vk_tags.c).
void coop_menu_font(void *atlas);
uint32_t menu_font_gen(void);   // that copy's generation
