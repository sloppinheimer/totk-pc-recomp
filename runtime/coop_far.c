// Co-op: what a game holds for the others is let go of once it is far from every player.
//
// TotK Online (read for behaviour only, no code of it here) has its server dispose of a synced actor 1000 m from
// every player (its actorAutoDisposeDistance). The dispose reaches each client as a remove. A runtime-made copy (no map
// placement) gets the game's own delete request there. A map actor is only let go of, back under the game's own control.
// A player's puppet is never disposed for distance. Peer to peer there is no server, so each game does it for what it
// holds, against every Link here and every other player's last reported place:
// - enemies and horses made here at run time for another game (coop_world.c spawn_take): deleted (far_sweep, below);
// - the copies of another player's objects (coop_players.c arrow_copies): deleted, and not made while far;
// - map enemies, horses and animals this game follows for another (coop_world.c peer_one: the follow owner, the
//   frozen puppet): not followed any more, the game's own again; never deleted;
// - placed objects another player moves (coop_players.c force_to): not moved any more; never deleted;
// - the stand-ins (co-op Link, PlayerPuppet): never, as TotK Online's puppets; they stand where their player is.
// TOTK_COOP_FAR=0: none of it. TOTK_COOP_FAR_M=<m>: another distance (1000 by default).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c: the remote Links here

int coop_far_on(void) {
    const char *v = cfg_get("TOTK_COOP_FAR");
    return !(v && !strcmp(v, "0"));
}

float coop_far_m(void) {
    const char *v = cfg_get("TOTK_COOP_FAR_M");
    float m = v && *v ? (float)atof(v) : COOP_FAR_M;
    return m >= 100.0f ? m : COOP_FAR_M;
}

// The players' places: this Link, each remote Link here, each other player's last reported place. Read again when
// older than 100 ms (game thread only).
static float at[1 + 2 * COOP_MAX_PLAYERS][3];
static int nat;
static uint64_t at_ns;
static int me_known;

static void places(void) {
    uint64_t now = plat_now_ns();
    if (at_ns && now - at_ns < 100000000ull) return;
    at_ns = now, nat = 0;
    uint64_t me = coop_player_actor();
    me_known = me && coop_rd(me + COOP_ACTOR_POS, at[nat], 12);
    if (me_known) nat++;
    uint64_t links[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS];
    int n = coop_player_actors(links, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++)
        if (links[i] && coop_rd(links[i] + COOP_ACTOR_POS, at[nat], 12)) nat++;
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        static NetState st;
        uint64_t age;
        if (id != coop_net_id() && coop_net_remote(id, &st, &age)) memcpy(at[nat++], st.pos, 12);
    }
}

float coop_far_nearest(const float p[3]) {
    places();
    float best = INFINITY;
    for (int i = 0; i < nat; i++) {
        float dx = p[0] - at[i][0], dy = p[1] - at[i][1], dz = p[2] - at[i][2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < best) best = d;
    }
    return best;
}

int coop_far_from_players(const float p[3]) {
    if (!coop_far_on() || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
    places();
    // (this Link's place unknown, a load: nothing is far)
    return me_known && coop_far_nearest(p) > coop_far_m();
}

int coop_far_actor(uint64_t a, float *dist) {
    float p[3];
    if (!a || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return 0;
    int far = coop_far_from_players(p);
    if (far && dist) *dist = coop_far_nearest(p);
    return far;
}

// Test (TOTK_COOP_TEST_FAR=<s>[,dx,dy,dz]): at that world time this Link is put dx dy dz from where it stands
// (1100 0 0 by default) with ForceSetMatrix, and logged. With it in both games, what each made or follows for the
// other near where they stood is then far from every player.
void coop_far_test(Cpu *c, uint64_t me) {
    static int done;
    const char *v = cfg_get("TOTK_COOP_TEST_FAR");
    if (done || !v || !me || frame_step_world_us() < (int64_t)(atof(v) * 1e6)) return;
    done = 1;
    float d[3] = {1100.0f, 0, 0}, p[3], r[9];
    const char *o = strchr(v, ',');
    if (o) sscanf(o + 1, "%f,%f,%f", &d[0], &d[1], &d[2]);
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    _Alignas(16) float m[12];
    for (int row = 0; row < 3; row++) {
        for (int j = 0; j < 3; j++) m[row * 4 + j] = r[row * 3 + j];
        m[row * 4 + 3] = p[row] + d[row];
    }
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
    rt_log("coop: test: far: this Link put from %.1f %.1f %.1f to %.1f %.1f %.1f", p[0], p[1], p[2], m[3], m[7], m[11]);
}
