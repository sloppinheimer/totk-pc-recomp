// The co-op name tags drawn into the game's own frame, before its HUD (vk_tags.c): the HUD and the game's menus cover
// them, and the scene's depth hides them behind walls. ui/coop_tags.cpp builds them with Dear ImGui on the render
// thread; the replay draws them.
#pragma once
#include <stdint.h>

#define VK_TAGS_MAX 9   // COOP_MAX_PLAYERS and the self test

typedef struct { float pos[2], uv[2]; uint32_t col; } VkTagVert;   // ImDrawVert: pixels around the anchor, atlas uv, RGBA8
typedef struct { float world[3]; uint32_t first, count; } VkTagItem;   // the anchor in the world; its indices

// Render thread. vk_tags_live: 1 while the replay finds the HUD's start in its frames (TOTK_COOP_TAGS_INFRAME not 0);
// then the overlay leaves the tags to it. vk_tags_set: this frame's tags, in pixels of a window win_h high, with the
// generation of the font atlas their uv are from (vk_tags_font). vk_tags_font: the overlay font's atlas (RGBA8, copied)
// after each build; returns its generation.
int vk_tags_live(void);
void vk_tags_set(const VkTagVert *v, uint32_t nv, const uint16_t *idx, uint32_t ni, const VkTagItem *items, int n,
                 float win_h, uint32_t font_gen);
uint32_t vk_tags_font(const uint8_t *rgba, int w, int h);

// The replay (vk_exec.c vk_exec_pass_begin, vk_exec_draw.c prepare_draw, vk_backend.c present_image).
struct VkExec;
void vk_tags_pass_begin(struct VkExec *ex);
void vk_tags_draw(struct VkExec *ex);
void vk_tags_frame(void);
void vk_tags_release(void);   // before the device goes away
