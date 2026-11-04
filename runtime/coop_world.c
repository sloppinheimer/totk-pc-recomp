// Co-op shared world: the host's enemies on the guests' screens.
//
// Every live actor passes the per-actor calc (main+0x41bb20) each frame; coop_actor_seen records it with the world
// time it was last seen and where it was first seen. An enemy (actor name "Enemy_...") is known in every game by
// its name and that first position: placed enemies are created at the same spot in every game on a save (checked
// to 0.1 m), and the map's own placement hash was not found in the actor (re-notes).
//
// The host (coop_world_role 1) sends, about 10 times a second, every enemy within SYNC_M of any player: key,
// position, rotation, life, and those it saw die in the last 5 s marked dead. A guest (role 2) puts its own copy of
// each where the host has it when it drifts, keeps its life at the host's, and kills it when the host's died; when
// its copy loses life the host has not taken (the guest hit it), it sends the damage, and the host takes it from
// its enemy. Enemies the host does not report (too far from everyone, or only in this game) are left alone.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define SYNC_M 120.0f          // host: enemies this close to a player are sent
#define SNAP_M 1.5f            // guest: its copy is put at the host's position past this
#define DEAD_KEEP_US 5000000   // host: an enemy that died is reported dead this long

// ---- the actors this game updates
#define SEEN_N 16384
static struct {
    _Atomic uint64_t actor;
    _Atomic int64_t at_us;
    float home[3];        // where it was first seen
    _Atomic int home_ok;
} seen[SEEN_N];

// This PC's Link: when the game last updated it (real time). The game stops updating it while a menu is open (the
// world is paused), so its input then means menu moves, not Link's.
static _Atomic uint64_t me_actor, me_calc_ns;
int coop_local_paused(void) {
    uint64_t at = atomic_load_explicit(&me_calc_ns, memory_order_relaxed);
    return !at || plat_now_ns() - at > 150000000ull;
}

void coop_actor_seen(uint64_t actor) {
    if (!actor) return;
    if (actor == atomic_load_explicit(&me_actor, memory_order_relaxed))
        atomic_store_explicit(&me_calc_ns, plat_now_ns(), memory_order_relaxed);
    int64_t now = frame_step_world_us();
    uint32_t h = (uint32_t)((actor >> 4) * 2654435761u) & (SEEN_N - 1);
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1)) {
        uint64_t a = atomic_load_explicit(&seen[h].actor, memory_order_relaxed);
        if (a == actor) { atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed); return; }
        // An empty cell, or one whose actor has not been updated for 10 s (gone): take it.
        int64_t at = atomic_load_explicit(&seen[h].at_us, memory_order_relaxed);
        if ((!a || now - at > 10000000) && atomic_compare_exchange_strong(&seen[h].actor, &a, actor)) {
            atomic_store_explicit(&seen[h].home_ok, 0, memory_order_relaxed);
            atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed);
            return;
        }
    }
}

// Calls fn for each actor updated in the last second, with its cell (for the first position).
static void each_live(void (*fn)(int cell, uint64_t actor, void *arg), void *arg) {
    int64_t now = frame_step_world_us();
    for (int i = 0; i < SEEN_N; i++) {
        uint64_t a = atomic_load_explicit(&seen[i].actor, memory_order_relaxed);
        if (a && now - atomic_load_explicit(&seen[i].at_us, memory_order_relaxed) < 1000000) fn(i, a, arg);
    }
}

void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg) {
    int64_t now = frame_step_world_us();
    for (int i = 0; i < SEEN_N; i++) {
        uint64_t a = atomic_load_explicit(&seen[i].actor, memory_order_relaxed);
        if (a && now - atomic_load_explicit(&seen[i].at_us, memory_order_relaxed) < 1000000) fn(a, arg);
    }
}

// ---- enemies: key, life
// Life component [[actor+0x228] +0x2f8]: life u32 at [comp+0x1830] +8, max life at [comp+0x1838] +8 (AI query
// QueryActorLife; Link reads 14 of 152 with 3.5 of 38 hearts, quarter hearts) (2026-11-04).
static uint64_t life_word(uint64_t a, int k) {
    uint64_t lc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2f8);
    uint64_t p = lc ? coop_rd_ptr(lc + 0x1830 + 8 * (uint64_t)k) : 0;
    return p ? p + 8 : 0;
}
static int life_of(uint64_t a) {
    uint32_t v = 0;
    uint64_t p = life_word(a, 0);
    return p && coop_rd(p, &v, 4) ? (int)v : -1;
}
static void set_life(uint64_t a, int v) {
    uint64_t p = life_word(a, 0);
    if (p) ST32(p, (uint32_t)(v < 0 ? 0 : v));
}

static uint32_t enemy_key(const char *name, const float home[3]) {
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int k = 0; k < 3; k++) {
        int32_t q = (int32_t)lroundf(home[k] * 2.0f);   // half metres: both games create it at the same spot
        for (int b = 0; b < 4; b++) h = (h ^ (uint8_t)(q >> (8 * b))) * 16777619u;
    }
    return h ? h : 1;
}

// An enemy this game updates, with its key (0 if not an enemy).
static uint32_t enemy_of(int cell, uint64_t a, char *nm, size_t n) {
    coop_actor_name(a, nm, n);
    if (strncmp(nm, "Enemy_", 6)) return 0;
    if (!atomic_load_explicit(&seen[cell].home_ok, memory_order_relaxed)) {
        if (!coop_rd(a + COOP_ACTOR_POS, seen[cell].home, 12)) return 0;
        atomic_store_explicit(&seen[cell].home_ok, 1, memory_order_relaxed);
    }
    return enemy_key(nm, seen[cell].home);
}

typedef struct { uint32_t key; uint16_t life, flags; float pos[3], rot[9]; } NetEnemy;   // flags bit 0: dead
_Static_assert(sizeof(NetEnemy) == 56, "NetEnemy wire size");
#define MAX_SENT 30
typedef struct { uint16_t count, reserved; NetEnemy e[MAX_SENT]; } WorldPacket;
typedef struct { uint32_t key; int32_t damage; } HitPacket;

// ---- host
static struct { uint32_t key; int64_t died_us; } dead[64];   // enemies seen to die, reported a while
static struct { uint32_t key; uint64_t actor; int life; int64_t at_us; } host_known[256];   // as last seen

typedef struct { WorldPacket pk; float players[COOP_MAX_PLAYERS + 1][3]; int np; } HostScan;
static void host_one(int cell, uint64_t a, void *arg) {
    HostScan *h = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key || h->pk.count >= MAX_SENT) return;
    NetEnemy e = {.key = key};
    if (!coop_rd(a + COOP_ACTOR_POS, e.pos, 12) || !coop_rd(a + COOP_ACTOR_ROT, e.rot, 36)) return;
    int near = 0;
    for (int i = 0; i < h->np && !near; i++) {
        float dx = e.pos[0] - h->players[i][0], dz = e.pos[2] - h->players[i][2];
        near = dx * dx + dz * dz < SYNC_M * SYNC_M;
    }
    if (!near) return;
    int life = life_of(a);
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    e.life = (uint16_t)(life < 0 ? 0 : life > 65535 ? 65535 : life);
    if (life == 0 || st > 6) e.flags |= 1;
    h->pk.e[h->pk.count++] = e;
    // Remembered for the hits, and for reporting its death once it is gone.
    int64_t now = frame_step_world_us();
    int free_i = -1;
    for (int i = 0; i < 256; i++) {
        if (host_known[i].key == key) { host_known[i].actor = a, host_known[i].life = life, host_known[i].at_us = now; return; }
        if (free_i < 0 && !host_known[i].key) free_i = i;
    }
    if (free_i >= 0) host_known[free_i].key = key, host_known[free_i].actor = a, host_known[free_i].life = life,
                     host_known[free_i].at_us = now;
}

static void host_tick(Cpu *c, uint64_t me) {
    (void)c;
    // Hits from the guests: take the damage from this game's enemy.
    HitPacket hp;
    int from = 0, n;
    while ((n = coop_net_hit_take(&hp, sizeof hp, &from)) != 0) {
        if (n != (int)sizeof hp) continue;
        for (int i = 0; i < 256; i++)
            if (host_known[i].key == hp.key && host_known[i].actor) {
                int life = life_of(host_known[i].actor);
                if (life > 0) {
                    set_life(host_known[i].actor, life - hp.damage);
                    rt_log("coop: player %d hit enemy %08x for %d (life %d -> %d)", from, hp.key, hp.damage, life,
                           life - hp.damage < 0 ? 0 : life - hp.damage);
                }
                break;
            }
    }
    static int t;
    if (++t % 3) return;   // the rest about 10 times a second
    static HostScan h;
    memset(&h.pk, 0, sizeof h.pk);
    h.np = coop_player_positions(h.players, COOP_MAX_PLAYERS);
    if (coop_rd(me + COOP_ACTOR_POS, h.players[h.np], 12)) h.np++;
    each_live(host_one, &h);
    // Known enemies no longer seen: gone. Those whose life was 0 when last seen died: reported dead a while.
    int64_t now = frame_step_world_us();
    for (int i = 0; i < 256; i++) {
        if (!host_known[i].key || now - host_known[i].at_us < 400000) continue;
        if (host_known[i].life == 0)
            for (int d = 0; d < 64; d++)
                if (dead[d].key == host_known[i].key || !dead[d].key || now - dead[d].died_us > DEAD_KEEP_US) {
                    dead[d].key = host_known[i].key, dead[d].died_us = now;
                    break;
                }
        host_known[i].key = 0, host_known[i].actor = 0;
    }
    for (int d = 0; d < 64 && h.pk.count < MAX_SENT; d++)
        if (dead[d].key && now - dead[d].died_us < DEAD_KEEP_US) {
            NetEnemy e = {.key = dead[d].key, .flags = 1};
            h.pk.e[h.pk.count++] = e;
        }
    coop_net_world_send(&h.pk, 4 + h.pk.count * (int)sizeof(NetEnemy));
}

// ---- guest
static struct { uint32_t key; int life; } guest_last[256];   // the life this game last set or saw for each
static int *guest_life_slot(uint32_t key) {
    int free_i = -1;
    for (int i = 0; i < 256; i++) {
        if (guest_last[i].key == key) return &guest_last[i].life;
        if (free_i < 0 && !guest_last[i].key) free_i = i;
    }
    if (free_i < 0) free_i = (int)(key & 255);
    guest_last[free_i].key = key, guest_last[free_i].life = -1;
    return &guest_last[free_i].life;
}

typedef struct { Cpu *c; WorldPacket pk; int snaps; } GuestScan;
static void guest_one(int cell, uint64_t a, void *arg) {
    GuestScan *g = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key) return;
    const NetEnemy *e = NULL;
    for (int i = 0; i < g->pk.count; i++) if (g->pk.e[i].key == key) { e = &g->pk.e[i]; break; }
    if (!e) return;   // the host does not have it near anyone: this game's own
    int life = life_of(a), *last = guest_life_slot(key);
    if (life < 0) return;
    if (e->flags & 1) {   // dead on the host
        // Life 0 and the game's own death follows (as on the host); a delete request instead lets the map make a
        // fresh one at the same spot at once (pair test 2026-11-04).
        if (life > 0) {
            set_life(a, 0);
            rt_log("coop: enemy %s %08x died on the host", nm, key);
        }
        *last = 0;
        return;
    }
    // Test (TOTK_COOP_FAKEHIT=<n>): at 20 s and 30 s into the world, the first synced enemy loses n life here.
    static int fake = -1, fake_done;
    if (fake < 0) { const char *f = getenv("TOTK_COOP_FAKEHIT"); fake = f ? atoi(f) : 0; }
    int64_t w = frame_step_world_us();
    if (fake > 0 && fake_done < 2 && w > (fake_done ? 30000000 : 20000000) && life > 0) {
        set_life(a, life - fake), life = life - fake < 0 ? 0 : life - fake, fake_done++;
        rt_log("coop: test: enemy %s %08x loses %d here", nm, key, fake);
    }
    // This game's hits: life lost since the host's value was last set here goes to the host.
    if (*last >= 0 && life < *last) {
        HitPacket hp = {key, *last - life};
        coop_net_hit_send(&hp, sizeof hp);
        rt_log("coop: hit enemy %s %08x for %d, sent to the host", nm, key, hp.damage);
    }
    if (life != (int)e->life && (*last < 0 || life >= *last)) set_life(a, e->life), life = e->life;
    *last = life;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float dx = p[0] - e->pos[0], dy = p[1] - e->pos[1], dz = p[2] - e->pos[2];
    if (dx * dx + dy * dy + dz * dz > SNAP_M * SNAP_M) {
        float m[12];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) m[r * 4 + k] = e->rot[r * 3 + k];
            m[r * 4 + 3] = e->pos[r];
        }
        coop_guest_call(g->c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, a, (uint64_t)(uintptr_t)m, 0, 0);
        g->snaps++;
    }
}

static void guest_tick(Cpu *c) {
    static GuestScan g;
    int from = 0;
    uint64_t age = 0;
    int n = coop_net_world(&g.pk, sizeof g.pk, &from, &age);
    if (n < 4 || age > 1000000000ull || 4 + g.pk.count * (int)sizeof(NetEnemy) > n) return;
    g.c = c, g.snaps = 0;
    each_live(guest_one, &g);
    static int t, snaps;
    snaps += g.snaps;
    if (++t % 300 == 0) {
        rt_log("coop: the host's world: %d enemies near players; %d put back here in the last 10 s", g.pk.count, snaps);
        snaps = 0;
    }
}

// ---- PROBE (TOTK_COOP_TRACE=enemy): enemies within 150 m of this PC's Link, with their key and life.
static uint64_t probe_me;
static void probe_one(int cell, uint64_t a, void *arg) {
    (void)arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    float p[3], q[3];
    if (!key || !coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(probe_me + COOP_ACTOR_POS, q, 12)) return;
    float dx = p[0] - q[0], dz = p[2] - q[2];
    if (dx * dx + dz * dz > 150.f * 150.f) return;
    rt_log("coop: enemy %s %08x at %.1f %.1f %.1f, life %d", nm, key, p[0], p[1], p[2], life_of(a));
}

void coop_world_tick(Cpu *c, uint64_t me) {
    atomic_store_explicit(&me_actor, me, memory_order_relaxed);
    static int on = -1, t;
    if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "enemy"); }
    if (on && ++t % 300 == 0) { probe_me = me; each_live(probe_one, NULL); }
    int role = coop_world_role();
    if (role == 1) host_tick(c, me);
    else if (role == 2) guest_tick(c);
}
