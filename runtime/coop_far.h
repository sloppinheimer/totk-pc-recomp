// Co-op (coop_far.c): what a game holds for the other players is let go of beyond COOP_FAR_M of every player, as
// TotK Online's server disposes of synced actors. TOTK_COOP_FAR=0 turns it off, TOTK_COOP_FAR_M sets the distance.
#pragma once
#include <stdint.h>

#include "cpu.h"

#define COOP_FAR_M 1000.0f
int coop_far_on(void);
float coop_far_m(void);
// 1 when p is more than coop_far_m() from this Link, every remote Link here and every other player's reported place
// (0 while this Link's place is unknown). Game thread.
int coop_far_from_players(const float p[3]);
float coop_far_nearest(const float p[3]);               // metres to the nearest of those places (INFINITY none)
int coop_far_actor(uint64_t actor, float *dist);        // coop_far_from_players at the actor's place; dist: how far
void coop_far_test(Cpu *c, uint64_t me);                // TOTK_COOP_TEST_FAR (coop_players.c coop_players_tick)
