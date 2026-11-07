// Co-op notices (coop_notices.c): a line on screen when a player joins, leaves or fast travels ("the tester travelled to
// Lookout Landing Skyview Tower"), drawn with the overlay by ui/coop_notices.cpp. TOTK_COOP_NOTICES=0 turns them off.
#pragma once
#include <stdint.h>

#include "cpu.h"

// A fast travel, sent in a HIT packet (coop_net_hit_send) and dispatched by coop_world.c peer_tick on its kind.
#define COOP_TRAVEL_KIND 0x5254   // "TR"
typedef struct {
    uint32_t kind, seq;   // seq: the sender's count of its travels; each is sent three times (UDP), told once
    float pos[3];         // where the traveller stands after the load
    char label[40];       // the place's message label (City_BaseCamp, Dungeon000, CheckPoint012), "" none
    uint8_t near, pad[3]; // 1: no travel point there, label is the nearest named place ("is now near")
} CoopTravelPacket;

int coop_notices_on(void);                                              // TOTK_COOP_NOTICES is not 0
void coop_notices_tick(Cpu *c, uint64_t me);                            // game thread, each co-op tick
void coop_notice_travel_take(const CoopTravelPacket *p, int len, int from);   // game thread (peer_tick)

// The overlay (render thread): the notices to show now, newest last, with their age; and the joins and leaves, found
// by comparing the player list with the last frame's (coop_notices_wanted does it each overlay frame).
typedef struct { char text[160]; float age_s; uint32_t id; } CoopNotice;   // id: 1, 2, ... in the order told
int coop_notices_wanted(void);                                         // 1: something to draw now
int coop_notices_get(CoopNotice *out, int max);                         // the newest max live ones, oldest first
#define COOP_NOTICE_SECONDS 6.0f
// The place names of language `lang` (a ui_text catalog language), for the overlay font's glyphs: visited only when the
// game's language is that one (ui/menu.cpp fonts_up).
void coop_notices_glyphs(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx);
