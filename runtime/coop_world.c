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

#include "cfg.h"
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
    _Atomic uint64_t home_of;   // the actor home is of: a reader takes home only for its own actor (below)
    uint32_t name_hash;   // its name's, when first seen (an actor in a gone one's memory has another)
} seen[SEEN_N];
static uint32_t name_hash_of(uint64_t a) {
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    uint32_t h = 2166136261u;
    for (const char *p = nm; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}
// Its first place, set where it is first seen (its first update): set at the first scan instead, up to 0.1 s later,
// an enemy that moved by then had another key in each game (code review, 2026-11-06).
static void set_home(int h, uint64_t actor) {
    float p[3];
    atomic_store_explicit(&seen[h].home_ok, 0, memory_order_relaxed);
    seen[h].name_hash = name_hash_of(actor);
    if (!coop_rd(actor + COOP_ACTOR_POS, p, 12)) return;
    memcpy(seen[h].home, p, 12);
    atomic_store_explicit(&seen[h].home_of, actor, memory_order_release);
    atomic_store_explicit(&seen[h].home_ok, 1, memory_order_release);
}
// home is the cell's own actor's (set by set_home, or by a scan for this actor)
static int home_is(int h, uint64_t actor) {
    return atomic_load_explicit(&seen[h].home_ok, memory_order_acquire) && atomic_load_explicit(&seen[h].home_of, memory_order_acquire) == actor;
}

void coop_actor_seen(uint64_t actor) {
    if (!actor) return;
    int64_t now = frame_step_world_us();
    uint32_t h0 = (uint32_t)((actor >> 4) * 2654435761u) & (SEEN_N - 1), h = h0;
    // Its own cell first, over the whole run: taking the first free one before looking further moved an actor whose
    // cell was later in the run to an earlier one freed by a gone actor, and its key changed (the first position is
    // the cell's), so the other game no longer matched it (code review, 2026-11-06).
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1))
        if (atomic_load_explicit(&seen[h].actor, memory_order_relaxed) == actor) {
            // Back after more than a second unseen with another name: another actor in a gone one's memory, which took
            // the gone one's first place and key (code review, 2026-11-06). Its own now.
            if (now - atomic_load_explicit(&seen[h].at_us, memory_order_relaxed) > 1000000 && name_hash_of(actor) != seen[h].name_hash)
                set_home((int)h, actor);
            atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed);
            return;
        }
    h = h0;
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1)) {
        uint64_t a = atomic_load_explicit(&seen[h].actor, memory_order_relaxed);
        if (a == actor) { atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed); return; }
        // An empty cell, or one whose actor has not been updated for 10 s (gone): take it.
        int64_t at = atomic_load_explicit(&seen[h].at_us, memory_order_relaxed);
        if ((!a || now - at > 10000000) && atomic_compare_exchange_strong(&seen[h].actor, &a, actor)) {
            atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed);
            set_home((int)h, actor);
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
int coop_life_of(uint64_t a) { return life_of(a); }          // -1: no life component
void coop_set_life(uint64_t a, int v) { set_life(a, v); }
int coop_life_max_of(uint64_t a) {   // most life, -1 none
    uint32_t v = 0;
    uint64_t p = life_word(a, 1);
    return p && coop_rd(p, &v, 4) ? (int)v : -1;
}
void coop_set_life_max(uint64_t a, int v) {
    uint64_t p = life_word(a, 1);
    if (p && v > 0) ST32(p, (uint32_t)v);
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

// Calls fn for each actor updated in the last second, with its name, its key (the name and where this game first
// saw it, as the enemies': a placed object has the same key in every game) and whether it was first seen just now.
void coop_actors_keyed(void (*fn)(uint64_t actor, const char *name, uint32_t key, int first, void *arg), void *arg) {
    int64_t now = frame_step_world_us();
    for (int i = 0; i < SEEN_N; i++) {
        uint64_t a = atomic_load_explicit(&seen[i].actor, memory_order_relaxed);
        if (!a || now - atomic_load_explicit(&seen[i].at_us, memory_order_relaxed) >= 1000000) continue;
        char nm[48];
        coop_actor_name(a, nm, sizeof nm);
        int first = 0;
        static uint64_t first_of[SEEN_N];   // (the scan that first sees a cell's actor says so once)
        if (!home_is(i, a)) {
            if (!coop_rd(a + COOP_ACTOR_POS, seen[i].home, 12)) continue;
            atomic_store_explicit(&seen[i].home_of, a, memory_order_release);
            atomic_store_explicit(&seen[i].home_ok, 1, memory_order_release);
        }
        if (first_of[i] != a) first_of[i] = a, first = 1;
        fn(a, nm, enemy_key(nm, seen[i].home), first, arg);
    }
}

// Every actor of the game, at rest or not. Each actor has a reference object in one pool, 0x60 bytes apart: +0x40
// the actor, +0x48 its id, which is its entry number, so the Player's reference and id give the pool's start. About
// 800 actors in the In-isa save, among them the resting rocks, trees and a sword on the ground that the live list
// (actors that ran their calc) does not have (TOTK_COOP_TEST_REFPOOL, 2026-11-05). The scan ends after 4096 empty
// entries in a row. fn gets the actor's name and its key: the enemies' key from where this game first saw it run,
// else from where it is now (a resting placed object is where the map put it, in every game).
static int seen_home(uint64_t actor, float out[3]) {
    uint32_t h = (uint32_t)((actor >> 4) * 2654435761u) & (SEEN_N - 1);
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1))
        if (atomic_load_explicit(&seen[h].actor, memory_order_relaxed) == actor) {
            if (!home_is((int)h, actor)) return 0;
            memcpy(out, seen[h].home, 12);
            return 1;
        }
    return 0;
}
static int name_ok(const char *s) {
    int n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_')) return 0;
    return n >= 3;
}
void coop_actors_all(void (*fn)(uint64_t actor, const char *name, uint32_t key, void *arg), void *arg) {
    uint64_t ref = 0;
    if (!coop_resident_named("Player", &ref) || !ref) return;
    uint32_t id = 0;
    if (!coop_rd(ref + 0x48, &id, 4)) return;
    uint64_t base = ref - (uint64_t)(id & 0xffff) * 0x60;
    for (int i = 0, gap = 0; i < 65536 && gap < 4096; i++) {
        uint64_t r = base + (uint64_t)i * 0x60, a = 0, np = 0;
        char nm[48] = {0};
        if (!coop_rd(r + 0x40, &a, 8)) break;
        if (!a || (a & 7) || !coop_rd(a + COOP_ACTOR_NAME, &np, 8) || !np || !coop_rd(np, nm, 40) || !name_ok(nm)) { gap++; continue; }
        gap = 0;
        int32_t st = 0;
        float home[3];
        if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6) continue;
        if (!seen_home(a, home) && !coop_rd(a + COOP_ACTOR_POS, home, 12)) continue;
        fn(a, nm, enemy_key(nm, home), arg);
    }
}

// An enemy this game updates, with its key (0 if not an enemy).
static uint32_t enemy_of(int cell, uint64_t a, char *nm, size_t n) {
    coop_actor_name(a, nm, n);
    if (strncmp(nm, "Enemy_", 6)) return 0;
    if (!home_is(cell, a)) {
        if (!coop_rd(a + COOP_ACTOR_POS, seen[cell].home, 12)) return 0;
        atomic_store_explicit(&seen[cell].home_of, a, memory_order_release);
        atomic_store_explicit(&seen[cell].home_ok, 1, memory_order_release);
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

typedef struct { WorldPacket pk; float players[COOP_MAX_PLAYERS + 1][3]; int np, live_max; } HostScan;
// Two enemies of one name made in the same half metre have one key: the guest put both on one host enemy, and the
// host kept one per key (code review, 2026-11-06). Such a key is left out of the sync in both games: counted first
// over the live enemies (dup_count), then skipped.
static struct { uint32_t key; int n; } dup_keys[512];
static int dup_n;
static void dup_one(int cell, uint64_t a, void *arg) {
    (void)arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key) return;
    for (int i = 0; i < dup_n; i++) if (dup_keys[i].key == key) { dup_keys[i].n++; return; }
    if (dup_n < 512) dup_keys[dup_n].key = key, dup_keys[dup_n++].n = 1;
}
static void dup_count(void) { dup_n = 0, each_live(dup_one, NULL); }
static int dup_key(uint32_t key) {
    for (int i = 0; i < dup_n; i++) if (dup_keys[i].key == key) return dup_keys[i].n > 1;
    return 0;
}
static void host_one(int cell, uint64_t a, void *arg) {
    HostScan *h = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key || h->pk.count >= h->live_max || dup_key(key)) return;
    NetEnemy e = {.key = key};
    if (!coop_rd(a + COOP_ACTOR_POS, e.pos, 12) || !coop_rd(a + COOP_ACTOR_ROT, e.rot, 36)) return;
    int near = 0;
    for (int i = 0; i < h->np && !near; i++) {
        float dx = e.pos[0] - h->players[i][0], dz = e.pos[2] - h->players[i][2];
        near = dx * dx + dz * dz < SYNC_M * SYNC_M;
    }
    if (!near) return;
    int life = life_of(a);
    // (no life yet, just made: not sent, sent as life 0 the guest set its copy's life to 0; code review, 2026-11-06)
    if (life < 0) return;
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    e.life = (uint16_t)(life > 65535 ? 65535 : life);
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
        // (a damage that is not 1..65535 is not a hit: a negative one healed, a huge one overflowed; code review)
        if (hp.damage <= 0 || hp.damage > 65535) continue;
        for (int i = 0; i < 256; i++)
            if (host_known[i].key == hp.key && host_known[i].actor) {
                // still that enemy: alive and an Enemy_ (its entry outlives it by up to 0.4 s)
                int32_t est = 0;
                char enm[64];
                if (!coop_rd(host_known[i].actor + COOP_ACTOR_STATE, &est, 4) || est < 2 || est > 6 ||
                    strncmp(coop_actor_name(host_known[i].actor, enm, sizeof enm), "Enemy_", 6))
                    break;
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
    // Room kept for the dead (up to 10): the living filled all 30 places with 30 or more enemies near, and no death
    // was reported (code review, 2026-11-06).
    int64_t now0 = frame_step_world_us();
    int n_dead = 0;
    for (int d = 0; d < 64; d++) n_dead += dead[d].key && now0 - dead[d].died_us < DEAD_KEEP_US;
    h.live_max = MAX_SENT - (n_dead < 10 ? n_dead : 10);
    dup_count();
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
// The life this game last set or saw for each, and when. One not used for 30 s is free again: kept for good, the
// table filled in a long session and two live enemies took turns in one place, each clearing the other's life, so
// neither's hits were sent (code review, 2026-11-06).
static struct { uint32_t key; int life; int64_t at_us; } guest_last[256];
static int *guest_life_slot(uint32_t key) {
    int free_i = -1;
    int64_t now = frame_step_world_us();
    for (int i = 0; i < 256; i++) {
        if (guest_last[i].key == key) { guest_last[i].at_us = now; return &guest_last[i].life; }
        if (free_i < 0 && (!guest_last[i].key || now - guest_last[i].at_us > 30000000)) free_i = i;
    }
    if (free_i < 0) free_i = (int)(key & 255);
    guest_last[free_i].key = key, guest_last[free_i].life = -1, guest_last[free_i].at_us = now;
    return &guest_last[free_i].life;
}

typedef struct { Cpu *c; WorldPacket pk; int snaps; } GuestScan;
static void guest_one(int cell, uint64_t a, void *arg) {
    GuestScan *g = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key || dup_key(key)) return;
    const NetEnemy *e = NULL;
    for (int i = 0; i < g->pk.count; i++) if (g->pk.e[i].key == key) { e = &g->pk.e[i]; break; }
    if (!e) return;   // the host does not have it near anyone: this game's own
    // Listed for a second after its last update, it may be gone (and its memory another's): written to only while
    // alive (code review, 2026-11-06).
    int32_t ast = 0;
    if (!coop_rd(a + COOP_ACTOR_STATE, &ast, 4) || ast < 2 || ast > 6) return;
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
    dup_count();
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
    rt_log("coop: enemy %s %08x at %.1f %.1f %.1f, life %d; this Link at %.1f %.1f %.1f", nm, key, p[0], p[1], p[2], life_of(a), q[0], q[1], q[2]);
}

// ---- PROBE (TOTK_COOP_TRACE=target): where an enemy keeps the Link it is after. For enemies within 40 m: words in
// the actor's first 0x1000 bytes, and in its components' first 0x800 (the table at actor +0x228), that equal a Link
// actor (this PC's or a co-op one); logged with the offsets, every 2 s.
int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c
static uint64_t tp_links[COOP_MAX_PLAYERS + 1], tp_refs[COOP_MAX_PLAYERS + 1];
static uint32_t tp_ids[COOP_MAX_PLAYERS + 1];   // perception ids
// An actor's perception id: its perception component [[actor+0x228] +0x378], then [[comp+0x30] +0x10] (AI query
// QueryActorGetPerceptionID, main+0x1b0c67c); 0xffffffff none.
static uint32_t perception_id(uint64_t a) {
    uint64_t comp = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x378), d = comp ? coop_rd_ptr(comp + 0x30) : 0;
    uint32_t id = 0xffffffffu;
    if (d) coop_rd(d + 0x10, &id, 4);
    return id;
}
static int tp_n;
static const char *link_of(uint64_t v) {
    static char s[32];
    for (int i = 0; i < tp_n; i++) {
        if (v && v == tp_links[i]) { snprintf(s, sizeof s, i ? "co-op Link %d" : "this Link", i); return s; }
        if (v && v == tp_refs[i]) { snprintf(s, sizeof s, i ? "ref of co-op Link %d" : "ref of this Link", i); return s; }
        if (tp_ids[i] != 0xffffffffu && tp_ids[i] > 16 && ((uint32_t)v == tp_ids[i] || (uint32_t)(v >> 32) == tp_ids[i])) {
            const char *half = (uint32_t)v == tp_ids[i] ? "low" : "high";
            if (i) snprintf(s, sizeof s, "id of co-op Link %d (%s)", i, half);
            else snprintf(s, sizeof s, "id of this Link (%s)", half);   // (one format for both took i for the %s)
            return s;
        }
    }
    return NULL;
}
// A Link's reference object (an ActorLink's +8; [ref+0x40] is the actor): this PC's from the resident list.
static void target_probe_one(int cell, uint64_t a, void *arg) {
    (void)arg;
    char nm[64];
    if (!enemy_of(cell, a, nm, sizeof nm)) return;
    float p[3], q[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(tp_links[0] + COOP_ACTOR_POS, q, 12)) return;
    float dx = p[0] - q[0], dz = p[2] - q[2];
    if (dx * dx + dz * dz > 60.f * 60.f) return;
    char line[2048];
    int n = snprintf(line, sizeof line, "coop: target probe %s at %.1f m:", nm, sqrtf(dx * dx + dz * dz));
    for (int i = 1; i < tp_n; i++) {   // the co-op Links: how far, and their life (an enemy attacking one takes it)
        float r[3];
        if (!coop_rd(tp_links[i] + COOP_ACTOR_POS, r, 12)) continue;
        float ex = p[0] - r[0], ez = p[2] - r[2];
        n += snprintf(line + n, sizeof line - n, " co-op Link %d at %.1f m, life %d;", i, sqrtf(ex * ex + ez * ez), life_of(tp_links[i]));
    }
    n += snprintf(line + n, sizeof line - n, " this Link life %d;", life_of(tp_links[0]));
    static uint64_t w[0x1000 / 8], cw[0x800 / 8], tw[0x800 / 8];
    if (coop_rd(a, w, sizeof w))
        for (int j = 0; j < 0x1000 / 8 && n < 1900; j++)
            if (link_of(w[j])) n += snprintf(line + n, sizeof line - n, " actor+%#x=%s", j * 8, link_of(w[j]));
    // One level down from the actor itself: each object it points to, its first 0x400 bytes.
    static uint64_t ow[0x400 / 8];
    for (int j = 0; j < 0x1000 / 8 && n < 1900; j++) {
        uint64_t o = w[j];
        if (o < 0x100000000ull || o >= 0x8000000000ull || o == a || !coop_rd(o, ow, sizeof ow)) continue;
        for (int i = 0; i < 0x400 / 8 && n < 1900; i++)
            if (link_of(ow[i])) n += snprintf(line + n, sizeof line - n, " [actor+%#x]+%#x=%s", j * 8, i * 8, link_of(ow[i]));
    }
    uint64_t table = coop_rd_ptr(a + 0x228);
    if (table && coop_rd(table, tw, sizeof tw))
        for (int k = 0; k < 0x800 / 8 && n < 1900; k++) {
            uint64_t comp = tw[k];
            if (comp < 0x100000000ull || comp >= 0x8000000000ull || !coop_rd(comp, cw, sizeof cw)) continue;
            for (int j = 0; j < 0x800 / 8 && n < 1900; j++)
                if (link_of(cw[j])) n += snprintf(line + n, sizeof line - n, " comp[+%#x]+%#x=%s", k * 8, j * 8, link_of(cw[j]));
        }
    rt_log("%s", line);
    // The enemy's own perception data: the id slots its per-actor calc resets to -1 (+0xf8/+0xfc, +0x138/+0x13c)
    // and the words and flags reset with them; one of them may be the id of the player it is after.
    uint64_t pd = coop_rd_ptr(coop_rd_ptr(table + 0x378) + 0x30);
    if (pd) {
        uint32_t u[0x160 / 4] = {0};
        coop_rd(pd, u, sizeof u);
        const uint8_t *b = (const uint8_t *)u;
        rt_log("coop: target slots %s: id %#x; +f8 %#x +fc %#x +110 %#x +138 %#x +13c %#x +150 %#x; flags f5 %d 105 %d 135 %d 146 %d",
               nm, u[0x10 / 4], u[0xf8 / 4], u[0xfc / 4], u[0x110 / 4], u[0x138 / 4], u[0x13c / 4], u[0x150 / 4],
               b[0xf5], b[0x105], b[0x135], b[0x146]);
    }
    // The enemy's components around its perception one (table +0x360..+0x3a0): their u32s equal to this Link's id.
    for (int k = 0x360 / 8; table && k < 0x3a0 / 8; k++) {
        uint64_t comp = tw[k];
        if (comp < 0x100000000ull || comp >= 0x8000000000ull) continue;
        static uint32_t u[0x2000 / 4];
        if (!coop_rd(comp, u, sizeof u)) continue;
        char l2[1024];
        int m = snprintf(l2, sizeof l2, "coop: target probe %s comp[+%#x] %llx u32 == %#x at:", nm, k * 8,
                         (unsigned long long)comp, tp_ids[0]);
        for (int j = 0; j < 0x2000 / 4 && m < 950; j++)
            if (u[j] == tp_ids[0]) m += snprintf(l2 + m, sizeof l2 - m, " +%#x", j * 4);
        rt_log("%s", l2);
    }
}

int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c: the readers of a range, reported on disarm
void coop_watch_disarm(void);

// Test (TOTK_COOP_TEST_WATCH5=<s>): from that many seconds into the world, for 10 s, the game functions that read
// slot 5 ([main+0x3ad9830] +0x250, this Link's perception data where enemies seem to take the player from).
static const uint32_t skel_path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};   // actor to its body skeleton (g3d)
static void watch_slot5(void) {
    const char *e = cfg_get("TOTK_COOP_TEST_WATCH5");
    static int phase;
    if (!e || phase == 2) return;
    double s = frame_step_world_us() / 1e6;
    if (!phase && s >= atof(e)) {
        uint64_t obj = coop_rd_ptr(MOD_MAIN_BASE + 0x3ad9830);
        const char *what = cfg_get("TOTK_COOP_TEST_WATCHWHAT");   // =data: this Link's perception data (0x300) instead
        if (what && !strcmp(what, "slot3")) {   // the perception manager's player emitter field
            uint64_t m = coop_rd_ptr(MOD_MAIN_BASE + 0x3a6ea80);
            if (m && coop_watch_arm(m + 0x58, 8)) phase = 1, rt_log("coop: test: watching the perception manager's player field %llx", (unsigned long long)(m + 0x58));
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "coopmodel")) {   // who reads the first co-op Link's model world matrix
            extern int coop_watch_writes;
            uint64_t ca[1]; int ci[1];
            uint64_t m = coop_player_actors(ca, ci, 1) ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ca[0] + 0x228) + 0x8) + 0x28) : 0;
            coop_watch_writes = 0;
            if (m && coop_watch_arm(m + 0x1f8, 0x30)) phase = 1, rt_log("coop: test: watching the co-op Link's model %llx world matrix", (unsigned long long)m);
            else if (!m) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "coopskel")) {   // who reads the first co-op Link's body skeleton world matrices
            extern int coop_watch_writes;
            uint64_t ca[1]; int ci[1];
            uint64_t s = 0;
            if (coop_player_actors(ca, ci, 1)) {
                s = ca[0];
                for (int i = 0; i < 8 && s; i++) s = coop_rd_ptr(s + skel_path[i]);
            }
            uint64_t wm = s ? coop_rd_ptr(s + 0x18) : 0;
            coop_watch_writes = cfg_get("TOTK_COOP_TEST_WATCHWRITES") != NULL;   // =1: only who writes them
            if (wm && coop_watch_arm(wm, 127 * 0x30)) phase = 1, rt_log("coop: test: watching the co-op Link's skeleton %llx world matrices %llx", (unsigned long long)s, (unsigned long long)wm);
            else if (!wm) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "cooplife")) {   // who writes the first co-op Link's life (writes only)
            extern int coop_watch_writes;
            uint64_t ca[1]; int ci[1];
            uint64_t w = coop_player_actors(ca, ci, 1) ? life_word(ca[0], 0) : 0;
            coop_watch_writes = 1;
            if (w && coop_watch_arm(w, 4)) phase = 1, rt_log("coop: test: watching writes to the co-op Link's life %llx (%d)", (unsigned long long)w, life_of(ca[0]));
            else if (!w) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "coopdata")) {   // the first co-op Link's perception data
            uint64_t ca[1]; int ci[1];
            uint64_t d = coop_player_actors(ca, ci, 1) ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ca[0] + 0x228) + 0x378) + 0x30) : 0;
            if (d && coop_watch_arm(d, 0x300)) phase = 1, rt_log("coop: test: watching the co-op Link's perception data %llx", (unsigned long long)d);
            else if (!d) return;   // no co-op Link yet: try again next tick
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "data")) {
            uint64_t me = coop_player_actor(), d = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x378) + 0x30);
            if (d && coop_watch_arm(d, 0x300)) phase = 1, rt_log("coop: test: watching this Link's perception data %llx", (unsigned long long)d);
            else phase = 2;
            return;
        }
        if (obj && coop_watch_arm(obj + 0x250, 8)) phase = 1, rt_log("coop: test: watching slot 5 at %llx", (unsigned long long)(obj + 0x250));
        else phase = 2;
    } else if (phase == 1 && s >= atof(e) + 10) {
        coop_watch_disarm();
        phase = 2;
    }
}

// ---- enemies choose between players. The game's perception has one player emitter (the manager's +0x58, this
// Link's data); coop_spawn.c's hook on the receiver check main+0x45b930 asks here, per enemy, for a co-op Link
// nearer than this one, and puts its data there for that check. Cached once a tick (the hook runs per receiver).
static _Atomic uint64_t near_me;
static struct { _Atomic uint64_t actor, data; } near_links[COOP_MAX_PLAYERS];
static _Atomic int near_n;
static _Atomic uint64_t coop_life_comps[COOP_MAX_PLAYERS];   // the co-op Links' life components, for coop_spawn.c
static void cache_links(uint64_t me) {
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS];
    int n = coop_world_role() ? coop_player_actors(acts, ids, COOP_MAX_PLAYERS) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x378) + 0x30);
        atomic_store(&near_links[i].actor, acts[i]), atomic_store(&near_links[i].data, d);
        atomic_store(&coop_life_comps[i], coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x2f8));
    }
    for (int i = n; i < COOP_MAX_PLAYERS; i++) atomic_store(&coop_life_comps[i], 0);
    atomic_store(&near_n, n);
    atomic_store(&near_me, me);
}
// A co-op Link's life component: its life update (main+0x4400b0) must not take life in this game (coop_spawn.c).
int coop_is_coop_life(uint64_t comp) {
    for (int i = 0; comp && i < COOP_MAX_PLAYERS; i++)
        if (atomic_load_explicit(&coop_life_comps[i], memory_order_relaxed) == comp) return 1;
    return 0;
}
uint64_t coop_nearest_link_data(uint64_t enemy) {
    int n = atomic_load_explicit(&near_n, memory_order_relaxed);
    uint64_t me = atomic_load_explicit(&near_me, memory_order_relaxed);
    if (!n || !me || enemy == me) return 0;
    char nm[8];
    coop_actor_name(enemy, nm, sizeof nm);
    if (strncmp(nm, "Enemy_", 6)) return 0;
    float e[3], p[3];
    if (!coop_rd(enemy + COOP_ACTOR_POS, e, 12) || !coop_rd(me + COOP_ACTOR_POS, p, 12)) return 0;
    float best = (e[0] - p[0]) * (e[0] - p[0]) + (e[1] - p[1]) * (e[1] - p[1]) + (e[2] - p[2]) * (e[2] - p[2]);
    best = sqrtf(best) - 1.0f;   // a co-op Link must be over 1 m nearer: no flicker between two players at one distance
    uint64_t pick = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = atomic_load_explicit(&near_links[i].actor, memory_order_relaxed);
        uint64_t d = atomic_load_explicit(&near_links[i].data, memory_order_relaxed);
        if (!a || !d || !coop_rd(a + COOP_ACTOR_POS, p, 12)) continue;
        float dist = sqrtf((e[0] - p[0]) * (e[0] - p[0]) + (e[1] - p[1]) * (e[1] - p[1]) + (e[2] - p[2]) * (e[2] - p[2]));
        if (dist < best) best = dist, pick = d;
    }
    return pick;
}

// Test (TOTK_COOP_TEST_MODELSCAN=<s>): once, that many seconds into the world, where each Link's position is kept
// besides the actor: the actor (0x4000), its components (the table at +0x228, 0x1000 each) and what they point to
// (0x800 each), as 3 floats in a row or with a stride of 16 (the last column of a 3x4 row matrix). For the frozen
// Link spike: the copies the model draws from.
static int near3(const float *a, const float *p) {
    return fabsf(a[0] - p[0]) < 0.02f && fabsf(a[1] - p[1]) < 0.02f && fabsf(a[2] - p[2]) < 0.02f && fabsf(p[0]) + fabsf(p[2]) > 1;
}
static uint64_t cand[256];   // the co-op Link's position copies (absolute), for TOTK_COOP_TEST_FOLLOW
static uint8_t cand_col[256];   // 1: a matrix column (stride 16), 0: 3 floats in a row
static int ncand, scanning_coop;
static void add_cand(uint64_t a, int col) {
    for (int i = 0; i < ncand; i++) if (cand[i] == a) return;
    if (ncand < 256) cand[ncand] = a, cand_col[ncand] = (uint8_t)col, ncand++;
}
static void scan_block(const char *who, const char *path, uint64_t base, size_t len, const float p[3], int *n) {
    static uint8_t buf[0x4000];
    if (len > sizeof buf || !coop_rd(base, buf, len)) return;
    for (size_t o = 0; o + 12 <= len && *n < 200; o += 4) {
        float a[3], b[3];
        memcpy(a, buf + o, 12);
        if (o + 36 <= len) memcpy(&b[0], buf + o, 4), memcpy(&b[1], buf + o + 16, 4), memcpy(&b[2], buf + o + 32, 4);
        if (near3(a, p)) { if (scanning_coop) add_cand(base + o, 0); else rt_log("coop: modelscan %s %s+%#zx: position (3 floats)", who, path, o); ++*n; }
        else if (o + 36 <= len && near3(b, p)) { if (scanning_coop) add_cand(base + o, 1); else rt_log("coop: modelscan %s %s+%#zx: position (matrix column)", who, path, o); ++*n; }
    }
}
// Runs of 3x4 row matrices (stride 0x30, or 0x40) whose translation is within 2.5 m of p, at least 30 long: bone
// world matrices. Logged with the block path, the start and the count.
static uint64_t bone_runs[2][8];
static int bone_counts[2][8], bone_stride[2][8], nbone[2];
static uint64_t visited[1 << 16];
static int first_visit(uint64_t a) {
    uint32_t h = (uint32_t)((a >> 4) * 2654435761u) & 0xffff;
    for (int i = 0; i < 0x10000; i++, h = (h + 1) & 0xffff) {
        if (visited[h] == a) return 0;
        if (!visited[h]) { visited[h] = a; return 1; }
    }
    return 0;
}
static int scan_bones(int k, const char *who, const char *path, uint64_t base, size_t len, const float p[3]) {
    static uint8_t buf[0x8000];
    if (!first_visit(base)) return 0;
    if (len > sizeof buf || !coop_rd(base, buf, len)) return 1;
    for (int stride = 0x30; stride <= 0x40; stride += 0x10)
        for (size_t s0 = 0; s0 < (size_t)stride; s0 += 4) {
            int run = 0;
            size_t start = 0;
            for (size_t o = s0; o + stride <= len; o += stride) {
                float m[12];
                memcpy(m, buf + o, 48);
                // translation as a row matrix (m[3], m[7], m[11]) or a column one (m[9..11]); near Link (world) or
                // near the origin (model space, not all zero)
                float r0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
                int ok = 0;
                for (int lay = 0; lay < 2 && !ok; lay++) {
                    float tx = lay ? m[9] : m[3], ty = lay ? m[10] : m[7], tz = lay ? m[11] : m[11];
                    float dx = tx - p[0], dy = ty - p[1], dz = tz - p[2];
                    float t2 = tx * tx + ty * ty + tz * tz;
                    ok = (dx * dx + dy * dy + dz * dz < 2.5f * 2.5f || (t2 < 2.5f * 2.5f && t2 > 1e-6f)) && r0 > 0.8f && r0 < 1.2f;
                }
                if (ok) { if (!run) start = o; run++; }
                if (!ok || o + 2 * stride > len) {
                    if (run >= 30) {
                        rt_log("coop: bonescan %s %s+%#zx: %d matrices, stride %#x", who, path, start, run, stride);
                        if (nbone[k] < 8) bone_runs[k][nbone[k]] = base + start, bone_counts[k][nbone[k]] = run, bone_stride[k][nbone[k]] = stride, nbone[k]++;
                    }
                    run = 0;
                }
            }
        }
    return 1;
}
// A g3d SkeletonObj (ModelObj::Initialize main+0x1f38c8 sets it up): +0x18 world matrices (3x4, 0x30 each) right
// after +0x20 local ones (0x40 each), both again at +0x58 / +0x60, the bone count u16 at +0x48. Found per Link.
static uint64_t skel_of[2][4];
static int nskel[2];
static int is_skeleton(uint64_t s) {
    uint64_t w[0xd];
    uint16_t n = 0;
    if (!coop_rd(s, w, sizeof w) || !coop_rd(s + 0x48, &n, 2)) return 0;
    return n >= 20 && n <= 600 && w[4] && w[4] == w[0xc] && (w[3] - w[4] == (uint64_t)n * 0x40 || w[0xb] - w[4] == (uint64_t)n * 0x40);
}
static void find_skeletons(int k, uint64_t obj, int depth, const char *path) {
    static uint64_t w[0x400 / 8];
    if (depth > 7) return;
    uint64_t buf[0x400 / 8];
    if (!coop_rd(obj, buf, sizeof buf)) return;
    (void)w;
    for (int i = 0; i < 0x400 / 8; i++) {
        uint64_t q = buf[i];
        if (q < 0x100000000ull || q >= 0x8000000000ull || !first_visit(q)) continue;
        char p2[160];
        snprintf(p2, sizeof p2, "%s+%#x", path, i * 8);
        if (is_skeleton(q)) {
            uint16_t n = 0;
            coop_rd(q + 0x48, &n, 2);
            uint64_t wm = coop_rd_ptr(q + 0x18);
            float m[12] = {0};
            coop_rd(wm, m, 48);
            rt_log("coop: skeleton %s %s -> %llx: %u bones, world matrices %llx, first at %.2f %.2f %.2f",
                   k ? "co-op Link" : "this Link", p2, (unsigned long long)q, n, (unsigned long long)wm, m[3], m[7], m[11]);
            if (nskel[k] < 4) skel_of[k][nskel[k]++] = q;
            continue;
        }
        find_skeletons(k, q, depth + 1, p2);
    }
}
// The objects on the way from a Link to its body skeleton (the path in TOTK_COOP_TEST_SKELCOPY): each one's first
// 0x1000 bytes searched for the Link's position (3 floats, or a 3x4 row matrix's last column), logged by step.
static void path_scan(const char *who, uint64_t a) {
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    uint64_t node = a;
    for (int step = 0; step <= 8 && node; step++) {
        static uint8_t buf[0x1000];
        if (coop_rd(node, buf, sizeof buf))
            for (size_t o = 0; o + 36 <= sizeof buf; o += 4) {
                float c3[3], col[3];
                memcpy(c3, buf + o, 12);
                memcpy(&col[0], buf + o, 4), memcpy(&col[1], buf + o + 16, 4), memcpy(&col[2], buf + o + 32, 4);
                if (near3(c3, p)) rt_log("coop: pathscan %s step %d (%llx) +%#zx: position (3 floats)", who, step, (unsigned long long)node, o);
                else if (near3(col, p)) rt_log("coop: pathscan %s step %d (%llx) +%#zx: position (matrix column)", who, step, (unsigned long long)node, o);
            }
        if (step < 8) node = coop_rd_ptr(node + skel_path[step]);
    }
}
static void model_scan(uint64_t me) {
    const char *e = cfg_get("TOTK_COOP_TEST_MODELSCAN");
    static int done;
    if (!e || done || frame_step_world_us() / 1e6 < atof(e)) return;
    done = 1;
    uint64_t links[2] = {me, 0};
    int ids[1];
    coop_player_actors(&links[1], ids, 1);
    for (int k = 0; k < 2 && links[k]; k++) {
        uint64_t a = links[k];
        float p[3];
        if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) continue;
        const char *who = k ? "co-op Link" : "this Link";
        scanning_coop = k;
        memset(visited, 0, sizeof visited);
        find_skeletons(k, a, 0, "actor");
        path_scan(k ? "co-op Link" : "this Link", a);
        rt_log("coop: skeleton %s: %d found", k ? "co-op Link" : "this Link", nskel[k]);
        memset(visited, 0, sizeof visited);
        int n = 0;
        rt_log("coop: modelscan %s %llx at %.2f %.2f %.2f", who, (unsigned long long)a, p[0], p[1], p[2]);
        scan_block(who, "actor", a, 0x4000, p, &n);
        uint64_t table = coop_rd_ptr(a + 0x228);
        for (int j = 0; table && j < 0x100; j++) {
            uint64_t comp = coop_rd_ptr(table + j * 8);
            if (comp < 0x100000000ull || comp >= 0x8000000000ull) continue;
            char path[64];
            snprintf(path, sizeof path, "comp[+%#x]", j * 8);
            scan_block(who, path, comp, 0x1000, p, &n);
            static uint64_t w[0x1000 / 8];
            if (!coop_rd(comp, w, sizeof w)) continue;
            for (int i = 0; i < 0x1000 / 8; i++) {
                uint64_t q = w[i];
                if (q < 0x100000000ull || q >= 0x8000000000ull || q == a) continue;
                snprintf(path, sizeof path, "[comp[+%#x]+%#x]", j * 8, i * 8);
                scan_block(who, path, q, 0x800, p, &n);
                if (!scan_bones(k, who, path, q, 0x8000, p)) continue;
                // and one level further: bone arrays hang off model objects
                static uint64_t w2[0x400 / 8];
                if (!coop_rd(q, w2, sizeof w2)) continue;
                for (int i2 = 0; i2 < 0x400 / 8; i2++) {
                    uint64_t q2 = w2[i2];
                    if (q2 < 0x100000000ull || q2 >= 0x8000000000ull || q2 == a || q2 == q) continue;
                    char p2[96];
                    snprintf(p2, sizeof p2, "[[comp[+%#x]+%#x]+%#x]", j * 8, i * 8, i2 * 8);
                    scan_bones(k, who, p2, q2, 0x8000, p);
                }
            }
        }
        rt_log("coop: modelscan %s: %d places", who, n);
        if (k) rt_log("coop: modelscan co-op Link: %d unique copies", ncand);
    }
}

// The copied pose (TOTK_COOP_TEST_SKELCOPY): this Link's body skeleton onto the first co-op Link's, its world
// matrices (both buffers, +0x18 and +0x58) and local ones (+0x20).
static _Atomic uint64_t pose_src, pose_dst, pose_comp;
static _Atomic int pose_bones;
void coop_pose_copy_now(void) {
    uint64_t s0 = atomic_load(&pose_src), s1 = atomic_load(&pose_dst);
    int n = atomic_load(&pose_bones);
    if (!s0 || !s1 || n <= 0 || n > 600) return;
    static __thread uint8_t tmp[600 * 0x40];
    static const int fields[3][2] = {{0x18, 0x30}, {0x58, 0x30}, {0x20, 0x40}};
    for (int f = 0; f < 3; f++) {
        uint64_t from = coop_rd_ptr(s0 + fields[f][0]), to = coop_rd_ptr(s1 + fields[f][0]);
        size_t len = (size_t)n * fields[f][1];
        if (from && to && len <= sizeof tmp && coop_rd(from, tmp, len)) memcpy((void *)(uintptr_t)to, tmp, len);
    }
}
// main+0x2e9c80 (x0: an animation binding; [[x0+8]+8] the model component it poses, the one at [actor+0x228] +0x8)
// has just written a model's pose: when it is the co-op Link's, the copied pose goes over it.
// The bindings main+0x2e9c80 has posed (coop_pose_saw, from its hook); the tick finds the one the co-op Link holds
// (a pointer to it in the actor or its components) and keeps it in pose_binding.
static _Atomic uint64_t bindings_seen[16], pose_binding;
void coop_pose_saw(uint64_t binding) {
    for (int i = 0; binding && i < 16; i++) {
        uint64_t have = atomic_load_explicit(&bindings_seen[i], memory_order_relaxed);
        if (have == binding) return;
        if (!have && atomic_compare_exchange_strong(&bindings_seen[i], &have, binding)) return;
    }
}
// Before main+0x2e9c80 poses the co-op Link's model: this Link's animation pose (the binding's [+8] +0x38, 0x1c a
// bone - rotation and translation - count at +0x30) copied over the co-op Link's, so its model is posed with it.
void coop_pose_before_anim(uint64_t binding) {
    uint64_t mine = atomic_load(&bindings_seen[0]);
    if (!mine || binding == mine) return;
    uint64_t s = coop_rd_ptr(mine + 8), d = coop_rd_ptr(binding + 8);
    uint32_t ns = 0, nd = 0;
    if (!s || !d || !coop_rd(s + 0x30, &ns, 4) || !coop_rd(d + 0x30, &nd, 4)) return;
    uint32_t n = ns < nd ? ns : nd;
    uint64_t ps = coop_rd_ptr(s + 0x38), pd = coop_rd_ptr(d + 0x38);
    static __thread uint8_t tmp[1024 * 0x1c];
    if (!ps || !pd || !n || n > 1024 || !coop_rd(ps, tmp, (size_t)n * 0x1c)) return;
    memcpy((void *)(uintptr_t)pd, tmp, (size_t)n * 0x1c);
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) == 0) rt_log("coop: test: animation pose copied before the co-op Link's: %u bones (%u, %u)", n, ns, nd);
}
// The co-op Link's objects a bone writer may be called with (x0..x3): its body skeleton, both world matrix
// buffers, its model component and model object, the actor. 0: none.
int coop_pose_arg_match(const uint64_t *args, int n) {
    uint64_t s1 = atomic_load_explicit(&pose_dst, memory_order_relaxed);
    if (!s1) return 0;
    uint64_t comp = atomic_load_explicit(&pose_comp, memory_order_relaxed);
    uint64_t set[6] = {s1, coop_rd_ptr(s1 + 0x18), coop_rd_ptr(s1 + 0x58), comp, comp ? coop_rd_ptr(comp + 0x28) : 0, 0};
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 5; k++)
            if (args[i] && args[i] == set[k]) return 1 + i * 8 + k;
    return 0;
}
// A Link's model parts and their g3d skeletons: the model object (component +0x8, +0x28) lists its units at +0x38
// (count u32 +0x30, main+0x24cc30); a unit's [+0] +0x110 is its g3d ModelObj, whose +0x38 is the SkeletonObj
// (ModelObj::Initialize main+0x1f38c8). Body (127 bones), head / hair and the rest, in the same order on every Link.
#define POSE_UNITS 16
int coop_link_skeletons(uint64_t actor, uint64_t out[POSE_UNITS]) {
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x8) + 0x28);
    uint32_t n = 0;
    if (!model || !coop_rd(model + 0x30, &n, 4)) return 0;
    if (n > POSE_UNITS) n = POSE_UNITS;
    uint64_t arr = coop_rd_ptr(model + 0x38);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t unit = arr ? coop_rd_ptr(arr + i * 8) : 0, u0 = unit ? coop_rd_ptr(unit) : 0;
        uint64_t g = u0 ? coop_rd_ptr(u0 + 0x110) : 0, s = g ? coop_rd_ptr(g + 0x38) : 0;
        out[i] = s && is_skeleton(s) ? s : 0;
    }
    return (int)n;
}
static _Atomic uint64_t my_skels[POSE_UNITS], co_skels[POSE_UNITS];
static _Atomic int n_skels;
static void cache_skeletons(uint64_t me, uint64_t other) {
    uint64_t a[POSE_UNITS] = {0}, b[POSE_UNITS] = {0};
    int na = coop_link_skeletons(me, a), nb = coop_link_skeletons(other, b), n = na < nb ? na : nb;
    for (int i = 0; i < POSE_UNITS; i++) atomic_store(&my_skels[i], i < n ? a[i] : 0), atomic_store(&co_skels[i], i < n ? b[i] : 0);
    atomic_store(&n_skels, n);
    static int said;
    if (n && !said++) {
        char line[512];
        int m = snprintf(line, sizeof line, "coop: test: %d model parts:", n);
        for (int i = 0; i < n && m < 480; i++) {
            uint16_t c0 = 0, c1 = 0;
            if (a[i]) coop_rd(a[i] + 0x48, &c0, 2);
            if (b[i]) coop_rd(b[i] + 0x48, &c1, 2);
            m += snprintf(line + m, sizeof line - m, " %u/%u", c0, c1);
        }
        rt_log("%s bones (this Link / co-op Link)", line);
    }
}
// x0..x3 of a bone writer: the co-op Link's part skeleton i, copied from this Link's part i (its world matrices,
// both buffers; TOTK_COOP_TEST_SKELCOPY=local: the local ones too). Returns 1 when it was one.
int coop_pose_after_writer(const uint64_t *args, int nargs) {
    int n = atomic_load_explicit(&n_skels, memory_order_relaxed);
    for (int a = 0; a < nargs; a++)
        for (int i = 0; args[a] && i < n; i++) {
            if (args[a] != atomic_load_explicit(&co_skels[i], memory_order_relaxed)) continue;
            uint64_t s0 = atomic_load(&my_skels[i]), s1 = args[a];
            uint16_t c0 = 0, c1 = 0;
            if (!s0 || !coop_rd(s0 + 0x48, &c0, 2) || !coop_rd(s1 + 0x48, &c1, 2) || c0 != c1) return 1;
            static __thread uint8_t tmp[600 * 0x40];
            const char *mode = cfg_get("TOTK_COOP_TEST_SKELCOPY");
            int nf = mode && !strcmp(mode, "local") ? 3 : 2;
            static const int fields[3][2] = {{0x18, 0x30}, {0x58, 0x30}, {0x20, 0x40}};
            for (int f = 0; f < nf; f++) {
                uint64_t from = coop_rd_ptr(s0 + fields[f][0]), to = coop_rd_ptr(s1 + fields[f][0]);
                size_t len = (size_t)c0 * fields[f][1];
                if (from && to && len <= sizeof tmp && coop_rd(from, tmp, len)) memcpy((void *)(uintptr_t)to, tmp, len);
            }
            return 1;
        }
    return 0;
}
// ---- puppet mode (TOTK_COOP_PUPPET=1): each player's body pose goes to the others as a POSE packet and is put on
// their copy of that player's Link right after main+0x283c750 (the world-matrix writer); that copy gets no input
// (coop_players.c), so it does nothing in this game (2026-11-05).
// Packet: u16 bone count, u16 0, then per bone 12 bytes: the rotation as the smallest three of its quaternion
// (3 x s16, the dropped component's index in the low 2 bits of the first) and the position in model space
// (3 x s16, 1/4096 m).
void coop_puppet_dump_input(uint64_t pose, uint64_t skel) {
    static _Atomic int done;
    uint64_t me = coop_player_actor(), s = me;
    for (int i = 0; i < 8 && s; i++) s = coop_rd_ptr(s + skel_path[i]);
    if (!s || skel != s || atomic_exchange(&done, 1)) return;
    uint16_t n = 0;
    coop_rd(pose + 0xa, &n, 2);
    uint64_t arr = coop_rd_ptr(pose);
    rt_log("coop: puppet input pose %llx: %u bones at %llx", (unsigned long long)pose, n, (unsigned long long)arr);
    for (int b = 0; b < 4; b++) {
        float f[20];
        if (!coop_rd(arr + b * 0x50, f, sizeof f)) break;
        rt_log("coop: puppet input %d: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", b,
               f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19]);
    }
}
// Proxy mode, the default (TOTK_COOP_PROXY=0 turns it off: the co-op Links then follow their owners' buttons, or
// with TOTK_COOP_PUPPET=1 are puppets): a display-only co-op Link. Its own update does not run at all; coop_players.c
// drives its model transform and body pose every frame (docs/coop/proxy-plan.md). It uses puppet mode's pose packets
// and gives no input, so it counts as puppet mode too.
int coop_proxy_mode(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_PROXY"); on = !(e && *e == '0'); }
    return on;
}
int coop_puppet_mode(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET"); on = (e && *e == '1') || coop_proxy_mode(); }
    return on;
}
static int16_t q16(float v, float scale) {
    float x = v * scale;
    return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
}
// The body's input pose, as main+0x283c750 (x0 the pose, x1 the SkeletonObj it writes) takes it: u16 bone count at
// pose +0xa, [pose] the bones, 0x50 each: +0x00 scale (3 floats), +0x10 translation, +0x20 and +0x30 the first two
// rows of the rotation (the third is their cross product), +0x40 left alone (2026-11-05).
// Packet: u16 bones, u16 scaled count, then per bone 12 bytes: the rotation's smallest three quaternion components
// (3 x s16, the dropped one's index in the low 2 bits of the first) and the translation (3 x s16, 1/4096 m); then
// per bone whose scale is not 1: u16 index + 3 floats.
#define POSE_BONE 12
#define POSE_IN_STRIDE 0x50
static int pose_pack_input(uint64_t pose, uint8_t *out, int max) {
    uint16_t n = 0;
    uint64_t arr = coop_rd_ptr(pose);
    if (!arr || !coop_rd(pose + 0xa, &n, 2) || n == 0 || n > 600 || 4 + n * POSE_BONE > max) return 0;
    static __thread float f[600 * 20];
    if (!coop_rd(arr, f, (size_t)n * POSE_IN_STRIDE)) return 0;
    int at = 4 + n * POSE_BONE, nsc = 0;
    for (int b = 0; b < n; b++) {
        const float *e = f + b * 20;
        const float *sc = e, *tr = e + 4, *a = e + 8, *c = e + 12;   // rows a, c; third = a x c
        float r00 = a[0], r01 = a[1], r02 = a[2], r10 = c[0], r11 = c[1], r12 = c[2];
        float r20 = r01 * r12 - r02 * r11, r21 = r02 * r10 - r00 * r12, r22 = r00 * r11 - r01 * r10;
        float q[4];   // x y z w
        float trc = r00 + r11 + r22;
        if (trc > 0) { float s = sqrtf(trc + 1) * 2; q[3] = s / 4, q[0] = (r21 - r12) / s, q[1] = (r02 - r20) / s, q[2] = (r10 - r01) / s; }
        else if (r00 > r11 && r00 > r22) { float s = sqrtf(1 + r00 - r11 - r22) * 2; q[3] = (r21 - r12) / s, q[0] = s / 4, q[1] = (r01 + r10) / s, q[2] = (r02 + r20) / s; }
        else if (r11 > r22) { float s = sqrtf(1 + r11 - r00 - r22) * 2; q[3] = (r02 - r20) / s, q[0] = (r01 + r10) / s, q[1] = s / 4, q[2] = (r12 + r21) / s; }
        else { float s = sqrtf(1 + r22 - r00 - r11) * 2; q[3] = (r10 - r01) / s, q[0] = (r02 + r20) / s, q[1] = (r12 + r21) / s, q[2] = s / 4; }
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        int big = 0;
        for (int k = 0; k < 4; k++) { q[k] = len > 0 ? q[k] / len : (k == 3); if (fabsf(q[k]) > fabsf(q[big])) big = k; }
        float sign = q[big] < 0 ? -1.0f : 1.0f;
        int16_t v[6];
        for (int k = 0, j = 0; k < 4; k++) if (k != big) v[j++] = q16(q[k] * sign, 32767.0f);
        v[0] = (int16_t)((v[0] & ~3) | big);
        v[3] = q16(tr[0], 4096), v[4] = q16(tr[1], 4096), v[5] = q16(tr[2], 4096);
        memcpy(out + 4 + b * POSE_BONE, v, POSE_BONE);
        if (fabsf(sc[0] - 1) > 1e-3f || fabsf(sc[1] - 1) > 1e-3f || fabsf(sc[2] - 1) > 1e-3f) {
            if (at + 14 > max) return 0;
            uint16_t bi = (uint16_t)b;
            memcpy(out + at, &bi, 2), memcpy(out + at + 2, sc, 12), at += 14, nsc++;
        }
    }
    memcpy(out, &n, 2);
    uint16_t ns = (uint16_t)nsc;
    memcpy(out + 2, &ns, 2);
    return at;
}
static int pose_unpack_input(const uint8_t *in, int len, uint64_t pose) {
    uint16_t n = 0, have = 0, nsc = 0;
    uint64_t arr = coop_rd_ptr(pose);
    if (len < 4 || !arr) return 0;
    memcpy(&n, in, 2), memcpy(&nsc, in + 2, 2);
    if (!coop_rd(pose + 0xa, &have, 2) || have != n || n > 600 || 4 + n * POSE_BONE > len) return 0;
    for (int b = 0; b < n; b++) {
        int16_t v[6];
        memcpy(v, in + 4 + b * POSE_BONE, POSE_BONE);
        int big = v[0] & 3;
        float q[4], sum = 0;
        for (int k = 0, j = 0; k < 4; k++) if (k != big) { q[k] = v[j++] / 32767.0f; sum += q[k] * q[k]; }
        q[big] = sum < 1 ? sqrtf(1 - sum) : 0;
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float e[16] = {1, 1, 1, 0, v[3] / 4096.0f, v[4] / 4096.0f, v[5] / 4096.0f, 0,
                       1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0,
                       2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0};
        memcpy((void *)(uintptr_t)(arr + b * POSE_IN_STRIDE), e, sizeof e);
    }
    for (int k = 0; k < nsc; k++) {
        int at = 4 + n * POSE_BONE + k * 14;
        if (at + 14 > len) break;
        uint16_t bi = 0;
        memcpy(&bi, in + at, 2);
        if (bi < n) memcpy((void *)(uintptr_t)(arr + bi * POSE_IN_STRIDE), in + at + 2, 12);
    }
    return 1;
}
// This Link's latest packed pose (from the hook, any thread) and the tick that sends it.
static uint8_t pose_latest[COOP_POSE_MAX];
static _Atomic int pose_latest_len;
static PlatMutex pose_lock = PLAT_MUTEX_INIT;
// The co-op Links' body skeletons and their players' net ids, cached once a tick for the writer hook.
static _Atomic uint64_t puppet_skel[COOP_MAX_PLAYERS], puppet_pose[COOP_MAX_PLAYERS];
static _Atomic int puppet_id[COOP_MAX_PLAYERS], puppet_n;
// Proxy mode: the input pose object main+0x283c750 was last called with for a co-op Link's body skeleton (kept from
// before its update stopped; the proxy driver calls the writer with it). 0: not seen yet.
uint64_t coop_puppet_pose_obj(uint64_t skel) {
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++)
        if (skel == atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) return atomic_load(&puppet_pose[i]);
    return 0;
}
static _Atomic uint64_t my_body_skel;
static void puppet_tick(uint64_t me) {
    if (!coop_puppet_mode()) return;
    uint64_t s0 = me;
    for (int i = 0; i < 8 && s0; i++) s0 = coop_rd_ptr(s0 + skel_path[i]);
    atomic_store(&my_body_skel, s0 && is_skeleton(s0) ? s0 : 0);
    static uint8_t pkt[COOP_POSE_MAX];
    plat_mutex_lock(&pose_lock);
    int len = atomic_load(&pose_latest_len);
    if (len) memcpy(pkt, pose_latest, len);
    plat_mutex_unlock(&pose_lock);
    if (len) coop_net_pose_send(pkt, len);
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(acts, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) {
        uint64_t s = acts[i];
        for (int k = 0; k < 8 && s; k++) s = coop_rd_ptr(s + skel_path[k]);
        uint64_t sk = s && is_skeleton(s) ? s : 0;
        if (sk != atomic_load(&puppet_skel[i])) atomic_store(&puppet_pose[i], 0);
        atomic_store(&puppet_skel[i], sk), atomic_store(&puppet_id[i], ids[i]);
    }
    atomic_store(&puppet_n, n);
    static int said;
    if (len && n && !said++) rt_log("coop: puppet mode: poses go out (%d bytes) and onto %d co-op Link(s)", len, n);
}
// From the hook on main+0x283c750, before it runs (x0 the input pose, x1 the skeleton): this Link's pose is packed
// for the network; a co-op Link's gets its player's latest. Returns 1 when x1 was a co-op Link's.
int coop_puppet_is_co_skeleton(uint64_t skel) {
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++)
        if (skel == atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) return 1;
    return 0;
}
// TOTK_COOP_POSE_LOG=<s>: for 3 s from that world time, bones 0..3's translation in each pose sent and put on.
static void pose_log(const char *what, uint64_t pose) {
    static double at = -2;
    if (at == -2) { const char *e = cfg_get("TOTK_COOP_POSE_LOG"); at = e ? atof(e) : -1; }
    double s = frame_step_world_us() / 1e6;
    if (at < 0 || s < at || s > at + 3) return;
    uint64_t arr = coop_rd_ptr(pose);
    float f[4][20];
    if (!arr || !coop_rd(arr, f, sizeof f)) return;
    rt_log("coop: pose %s %.3f s: b0 %.3f %.3f %.3f  b1 %.3f %.3f %.3f  b2 %.3f %.3f %.3f  b3 %.3f %.3f %.3f", what, s,
           f[0][4], f[0][5], f[0][6], f[1][4], f[1][5], f[1][6], f[2][4], f[2][5], f[2][6], f[3][4], f[3][5], f[3][6]);
}
// Proxy mode: the last POSE_HIST poses received from each player, decoded, with their arrival times, so the proxy
// shows the pose from the same moment as its position (coop_players.c owner_smoothed: TOTK_COOP_PUPPET_DELAY_MS
// behind, default 60), blended between the two poses around it. With the newest pose put on as it came, the pose ran
// about 40 ms ahead of the position and moved in 30 Hz steps (tools/coop/top_compare.py on tour.txt, 2026-11-05).
#define POSE_HIST 8
#define POSE_HIST_BONES 160
typedef struct { uint64_t ns; int n, records; float q[POSE_HIST_BONES][4], t[POSE_HIST_BONES][3], sc[POSE_HIST_BONES][3]; } PoseRec;
static struct { PoseRec r[POSE_HIST]; unsigned count; uint64_t last_ns; } pose_hist[COOP_MAX_PLAYERS + 1];   // by net id
static PlatMutex hist_lock = PLAT_MUTEX_INIT;
static int pose_decode(const uint8_t *in, int len, PoseRec *r) {
    uint16_t n = 0, nsc = 0;
    if (len < 4) return 0;
    memcpy(&n, in, 2), memcpy(&nsc, in + 2, 2);
    int records = n >> 15;   // top bit: the skeleton's final local records (proxy_pack_mine), not the input
    n &= 0x7fff;
    // (set only for a good packet: a bad one changed the entry the blend still used; code review, 2026-11-06)
    if (n == 0 || n > POSE_HIST_BONES || 4 + n * POSE_BONE > len) return 0;
    r->records = records;
    for (int b = 0; b < n; b++) {
        int16_t v[6];
        memcpy(v, in + 4 + b * POSE_BONE, POSE_BONE);
        int big = v[0] & 3;
        float sum = 0;
        for (int k = 0, j = 0; k < 4; k++) if (k != big) { r->q[b][k] = v[j++] / 32767.0f; sum += r->q[b][k] * r->q[b][k]; }
        r->q[b][big] = sum < 1 ? sqrtf(1 - sum) : 0;
        r->t[b][0] = v[3] / 4096.0f, r->t[b][1] = v[4] / 4096.0f, r->t[b][2] = v[5] / 4096.0f;
        r->sc[b][0] = r->sc[b][1] = r->sc[b][2] = 1;
    }
    for (int k = 0; k < nsc; k++) {
        int at = 4 + n * POSE_BONE + k * 14;
        if (at + 14 > len) break;
        uint16_t bi = 0;
        memcpy(&bi, in + at, 2);
        if (bi < n) memcpy(r->sc[bi], in + at + 2, 12);
    }
    r->n = n;
    return 1;
}
static int pose_delay_ms(void) {
    static int d = -1;
    if (d < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_DELAY_MS"); d = e ? atoi(e) : 60; }
    return d;
}
// Writes player id's pose at (now - delay) into the input pose object (pose; skel 0), or straight into a body
// skeleton's local records (skel; pose 0; the packets carry records then); 0 when there is none (bone count
// differs, none yet).
static int pose_put_delayed(int id, uint64_t pose, uint64_t skel) {
    uint16_t have = 0;
    uint64_t arr = pose ? coop_rd_ptr(pose) : skel ? coop_rd_ptr(skel + 0x18) : 0;
    if (!arr || !coop_rd(pose ? pose + 0xa : skel + 0x48, &have, 2) || id < 1 || id > COOP_MAX_PLAYERS) return 0;
    static __thread PoseRec out;
    uint64_t t = plat_now_ns() - (uint64_t)pose_delay_ms() * 1000000ull;
    int ok = 0;
    plat_mutex_lock(&hist_lock);
    unsigned cnt = pose_hist[id].count, n = cnt < POSE_HIST ? cnt : POSE_HIST;
    const PoseRec *a = NULL, *b = NULL;
    if (n) {
        const PoseRec *newest = &pose_hist[id].r[(cnt - 1) % POSE_HIST];
        if (t >= newest->ns || n == 1) a = b = newest;
        else
            for (unsigned i = 1; i < n && !a; i++) {
                const PoseRec *pb = &pose_hist[id].r[(cnt - i) % POSE_HIST], *pa = &pose_hist[id].r[(cnt - i - 1) % POSE_HIST];
                if (pa->ns <= t) a = pa, b = pb;
            }
        if (!a) a = b = &pose_hist[id].r[(cnt - n) % POSE_HIST];   // older than all kept: the oldest
    }
    if (a && a->n == have && b->n == have && a->records == !pose && b->records == !pose) {
        float f = b->ns > a->ns && t > a->ns ? (float)((double)(t - a->ns) / (double)(b->ns - a->ns)) : 1;
        if (f > 1) f = 1;
        if (a == b) f = 0;
        for (int i = 0; i < have; i++) {
            float dot = 0, l = 0;
            for (int k = 0; k < 4; k++) dot += a->q[i][k] * b->q[i][k];
            float sg = dot < 0 ? -1.0f : 1.0f;
            for (int k = 0; k < 4; k++) out.q[i][k] = a->q[i][k] * (1 - f) + b->q[i][k] * sg * f, l += out.q[i][k] * out.q[i][k];
            l = sqrtf(l);
            for (int k = 0; k < 4; k++) out.q[i][k] = l > 0 ? out.q[i][k] / l : k == 3;
            for (int k = 0; k < 3; k++) out.t[i][k] = a->t[i][k] + (b->t[i][k] - a->t[i][k]) * f, out.sc[i][k] = f < 0.5f ? a->sc[i][k] : b->sc[i][k];
        }
        out.n = have, ok = 1;
    }
    plat_mutex_unlock(&hist_lock);
    if (!ok) return 0;
    for (int i = 0; i < out.n; i++) {
        float x = out.q[i][0], y = out.q[i][1], z = out.q[i][2], w = out.q[i][3];
        if (!pose) {   // a local record (0x60): scale +0x10, the rotation's columns at +0x20 / +0x30 / +0x40, translation +0x50
            float *rec = (float *)(uintptr_t)(arr + (uint64_t)i * 0x60);
            float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
                          2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
            for (int k = 0; k < 3; k++) rec[4 + k] = out.sc[i][k], rec[20 + k] = out.t[i][k];
            for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) rec[8 + c * 4 + k] = R[k * 3 + c];
            continue;
        }
        float e[16] = {out.sc[i][0], out.sc[i][1], out.sc[i][2], 0, out.t[i][0], out.t[i][1], out.t[i][2], 0,
                       1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0,
                       2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0};
        memcpy((void *)(uintptr_t)(arr + i * POSE_IN_STRIDE), e, sizeof e);
    }
    return 1;
}
// Proxy mode: a newly arrived pose from player id goes into its history.
static void pose_hist_take(int id, const uint8_t *in, int len, uint64_t age) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return;
    uint64_t at = plat_now_ns() - age;
    plat_mutex_lock(&hist_lock);
    uint64_t last = pose_hist[id].last_ns;
    if (at > last + 1000000ull || at + 1000000ull < last) {   // not the packet already kept (arrival times differ)
        PoseRec *r = &pose_hist[id].r[pose_hist[id].count % POSE_HIST];
        if (pose_decode(in, len, r)) r->ns = at, pose_hist[id].count++, pose_hist[id].last_ns = at;
    }
    plat_mutex_unlock(&hist_lock);
}
// Proxy mode: this Link's body skeleton's local records (+0x18, 0x60 a bone) as they stand at the start of its
// update phase main+0x257310 (the last frame's, after its IK and helper-bone steps), packed for sending as a POSE
// packet with the bone count's top bit set. The writer's input (what puppet mode sends) misses those steps, and a
// proxy does not run them: 30 bones differed between the two even standing still (TOTK_COOP_TEST_IKDIFF, 2026-11-05).
static int proxy_records_on(void) {
    static int on = -1;   // TOTK_COOP_PROXY_POSE=input: send and apply the writer's input as puppet mode does (control)
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_PROXY_POSE"); on = !(e && !strcmp(e, "input")); }
    return on;
}
// The final bone matrices (skeleton +0x20, 0x40 a bone: rows 0..2 the rotation of a row-vector matrix, row 3 the
// translation) also carry steps that write them after the local records are built: feet put on stairs, a hand put
// on a wall (the owner's body top 1.55 m on the stairs and 1.90 m at the shrine wall against its proxy's 1.60 m,
// tools/coop/top_compare.py, 2026-11-05). So the records sent are made from the final matrices: each bone's
// rotation and translation relative to its parent's final matrix. The parents are found once on this Link: for each
// bone, the earlier bone whose final matrix times this bone's record gives this bone's final matrix most closely.
// Column-form here: A[r][c] = m[c * 4 + r], t = m[12..14]; a record's rotation R[r][c] = rec[8 + c * 4 + r].
static int bone_parent[POSE_HIST_BONES], parents_n;   // parents_n: bones whose parent is known (0: not yet)
static void w_of(const float *m, float A[3][3], float t[3]) {
    for (int r = 0; r < 3; r++) { t[r] = m[12 + r]; for (int c = 0; c < 3; c++) A[r][c] = m[c * 4 + r]; }
}
static void r_of(const float *rec, float R[3][3]) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r][c] = rec[8 + c * 4 + r];
}
static int inv3(const float a[3][3], float o[3][3]) {
    float d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
              a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(d) < 1e-12f) return 0;
    float id = 1 / d;
    o[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id, o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id, o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    o[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * id, o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id, o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
    o[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id, o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id, o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    return 1;
}
// How far bone i's final matrix is from parent p's final matrix times bone i's record (rotation and scale, translation).
static float fk_error(const float (*w)[16], const float (*rec)[24], int i, int p) {
    float Ap[3][3], tp[3], Ai[3][3], ti[3], R[3][3];
    w_of(w[p], Ap, tp), w_of(w[i], Ai, ti), r_of(rec[i], R);
    float e = 0;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += Ap[r][k] * R[k][c];
            v *= rec[i][4 + c];
            e = fmaxf(e, fabsf(v - Ai[r][c]));
        }
        float tv = tp[r];
        for (int k = 0; k < 3; k++) tv += Ap[r][k] * rec[i][20 + k];
        e = fmaxf(e, fabsf(tv - ti[r]));
    }
    return e;
}
static void find_parents(int n, const float (*w)[16], const float (*rec)[24]) {
    int good = 0;
    float worst = 0;
    for (int i = 1; i < n; i++) {
        int best = 0;
        float be = 1e9f;
        for (int p = 0; p < i; p++) {
            float e = fk_error(w, rec, i, p);
            if (e < be) be = e, best = p;
        }
        bone_parent[i] = best;
        if (be < 2e-3f) good++;
        else if (be > worst) worst = be;
    }
    bone_parent[0] = -1;
    static int tries;
    // Kept once nearly all bones match (the rest are the ones later steps moved this frame); else tried again.
    if (good >= (n - 1) * 3 / 4 || ++tries > 600) parents_n = n;
    if (parents_n || tries % 120 == 1)
        rt_log("coop: proxy: bone parents %s: %d of %d bones match their parent within 2 mm (worst other %.3f)",
               parents_n ? "found" : "not yet", good, n - 1, worst);
}
void coop_proxy_pack_mine(uint64_t me) {
    int coop_proxy_mode(void);
    if (!coop_proxy_mode() || !proxy_records_on()) return;
    uint64_t sk = me;
    for (int i = 0; i < 8 && sk; i++) sk = coop_rd_ptr(sk + skel_path[i]);
    uint16_t n = 0;
    uint64_t recs = sk ? coop_rd_ptr(sk + 0x18) : 0;
    static float rec[POSE_HIST_BONES][24], w[POSE_HIST_BONES][16];
    static uint8_t out[COOP_POSE_MAX];
    uint64_t wm = sk ? coop_rd_ptr(sk + 0x20) : 0;
    if (!recs || !wm || !coop_rd(sk + 0x48, &n, 2) || n == 0 || n > POSE_HIST_BONES || 4 + n * POSE_BONE > COOP_POSE_MAX ||
        !coop_rd(recs, rec, (size_t)n * 0x60) || !coop_rd(wm, w, (size_t)n * 0x40))
        return;
    static int finals = -1;   // TOTK_COOP_PROXY_FINAL=0: send the records as they are (control)
    if (finals < 0) { const char *e = cfg_get("TOTK_COOP_PROXY_FINAL"); finals = !(e && *e == '0'); }
    if (finals && parents_n != n) find_parents(n, (const float (*)[16])w, (const float (*)[24])rec);
    if (finals && parents_n == n)   // each record from the final matrices: R = Ap^-1 Ai S^-1, t = Ap^-1 (ti - tp)
        for (int i = 1; i < n; i++) {
            float Ap[3][3], tp[3], Ai[3][3], ti[3], Ip[3][3];
            w_of(w[bone_parent[i]], Ap, tp), w_of(w[i], Ai, ti);
            if (!inv3(Ap, Ip)) continue;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    float v = 0;
                    for (int k = 0; k < 3; k++) v += Ip[r][k] * Ai[k][c];
                    float sc = rec[i][4 + c];
                    rec[i][8 + c * 4 + r] = fabsf(sc) > 1e-6f ? v / sc : v;
                }
            }
            for (int r = 0; r < 3; r++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += Ip[r][k] * (ti[k] - tp[k]);
                rec[i][20 + r] = v;
            }
        }
    int at = 4 + n * POSE_BONE, nsc = 0;
    for (int b = 0; b < n; b++) {
        const float *f = rec[b];
        // R (rows) from the record's columns
        float r00 = f[8], r01 = f[12], r02 = f[16], r10 = f[9], r11 = f[13], r12 = f[17], r20 = f[10], r21 = f[14], r22 = f[18];
        float q[4], trc = r00 + r11 + r22;
        if (trc > 0) { float s = sqrtf(trc + 1) * 2; q[3] = s / 4, q[0] = (r21 - r12) / s, q[1] = (r02 - r20) / s, q[2] = (r10 - r01) / s; }
        else if (r00 > r11 && r00 > r22) { float s = sqrtf(1 + r00 - r11 - r22) * 2; q[3] = (r21 - r12) / s, q[0] = s / 4, q[1] = (r01 + r10) / s, q[2] = (r02 + r20) / s; }
        else if (r11 > r22) { float s = sqrtf(1 + r11 - r00 - r22) * 2; q[3] = (r02 - r20) / s, q[0] = (r01 + r10) / s, q[1] = s / 4, q[2] = (r12 + r21) / s; }
        else { float s = sqrtf(1 + r22 - r00 - r11) * 2; q[3] = (r10 - r01) / s, q[0] = (r02 + r20) / s, q[1] = (r12 + r21) / s, q[2] = s / 4; }
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        int big = 0;
        for (int k = 0; k < 4; k++) { q[k] = len > 0 ? q[k] / len : (k == 3); if (fabsf(q[k]) > fabsf(q[big])) big = k; }
        float sign = q[big] < 0 ? -1.0f : 1.0f;
        int16_t v[6];
        for (int k = 0, j = 0; k < 4; k++) if (k != big) v[j++] = q16(q[k] * sign, 32767.0f);
        v[0] = (int16_t)((v[0] & ~3) | big);
        v[3] = q16(f[20], 4096), v[4] = q16(f[21], 4096), v[5] = q16(f[22], 4096);
        memcpy(out + 4 + b * POSE_BONE, v, POSE_BONE);
        if (fabsf(f[4] - 1) > 1e-3f || fabsf(f[5] - 1) > 1e-3f || fabsf(f[6] - 1) > 1e-3f) {
            if (at + 14 > COOP_POSE_MAX) return;
            uint16_t bi = (uint16_t)b;
            memcpy(out + at, &bi, 2), memcpy(out + at + 2, f + 4, 12), at += 14, nsc++;
        }
    }
    uint16_t hdr = (uint16_t)(n | 0x8000), ns = (uint16_t)nsc;
    memcpy(out, &hdr, 2), memcpy(out + 2, &ns, 2);
    plat_mutex_lock(&pose_lock);
    memcpy(pose_latest, out, at), atomic_store(&pose_latest_len, at);
    plat_mutex_unlock(&pose_lock);
}
// Proxy mode, from the proxy driver (coop_players.c): the owner's records at the delayed moment straight into the
// proxy's body skeleton. 0: not done (no records yet: the caller runs the writer with the input pose instead).
int coop_proxy_put_records(uint64_t skel) {
    if (!proxy_records_on()) return 0;
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++) {
        if (skel != atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) continue;
        static __thread uint8_t in[COOP_POSE_MAX];
        uint64_t age = 0;
        int id = atomic_load(&puppet_id[i]), len = coop_net_pose(id, in, sizeof in, &age);
        if (!len || age >= 2000000000ull) return 0;
        pose_hist_take(id, in, len, age);
        int ok = pose_put_delayed(id, 0, skel);
        static _Atomic int said;
        if (ok && atomic_fetch_add(&said, 1) == 0) rt_log("coop: proxy: the owner's skeleton records go onto the proxy");
        return ok;
    }
    return 0;
}
// Test (TOTK_COOP_TEST_IKDIFF=1): this Link's body skeleton local records (+0x18, 0x60 a bone: scale +0x10, rotation
// rows +0x20 / +0x30 / +0x40, transposed against the input pose's, translation +0x50) against the last pose packed for sending (the writer's input):
// whether later steps (IK) change the records after main+0x283c750 built them. Every 60 calls: how many bones differ.
void coop_test_ikdiff(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_IKDIFF") != NULL;
    if (!on) return;
    uint64_t sk = me;
    for (int i = 0; i < 8 && sk; i++) sk = coop_rd_ptr(sk + skel_path[i]);
    uint64_t recs = sk ? coop_rd_ptr(sk + 0x18) : 0;
    static PoseRec r;
    static uint8_t pkt[COOP_POSE_MAX];
    plat_mutex_lock(&pose_lock);
    int len = atomic_load(&pose_latest_len);
    if (len) memcpy(pkt, pose_latest, len);
    plat_mutex_unlock(&pose_lock);
    if (!recs || !len || !pose_decode(pkt, len, &r)) return;
    static float rec[POSE_HIST_BONES][24];
    if (!coop_rd(recs, rec, (size_t)r.n * 0x60)) return;
    static int dumped;
    if (!dumped++)
        for (int b = 1; b <= 3; b++) {
            float *f = rec[b];
            rt_log("coop: test: ikdiff record %d: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                   b, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]);
            rt_log("coop: test: ikdiff input %d: q %.3f %.3f %.3f %.3f t %.3f %.3f %.3f", b, r.q[b][0], r.q[b][1], r.q[b][2], r.q[b][3], r.t[b][0], r.t[b][1], r.t[b][2]);
        }
    static int calls, nb_rot, nb_tr, worst_b;
    static float worst;
    static unsigned char seen[POSE_HIST_BONES];
    for (int b = 0; b < r.n; b++) {
        float x = r.q[b][0], y = r.q[b][1], z = r.q[b][2], w = r.q[b][3];
        float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
                      2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
        float dr = 0, dt = 0;
        for (int row = 0; row < 3; row++) for (int k = 0; k < 3; k++) { float d = fabsf(rec[b][8 + row * 4 + k] - m[k * 3 + row]); if (d > dr) dr = d; }   // records hold it transposed
        for (int k = 0; k < 3; k++) { float d = fabsf(rec[b][20 + k] - r.t[b][k]); if (d > dt) dt = d; }
        if (dr > 0.02f) nb_rot++, seen[b] = 1;
        if (dt > 0.002f) nb_tr++, seen[b] = 1;
        if (dr > worst) worst = dr, worst_b = b;
    }
    if (++calls % 60) return;
    char list[400];
    int m = 0;
    for (int b = 0; b < r.n && m < 380; b++) if (seen[b]) m += snprintf(list + m, sizeof list - m, " %d", b);
    list[m] = 0;
    rt_log("coop: test: ikdiff over 60 frames: %d bone-frames rotated off, %d moved off, worst rotation %.3f (bone %d); bones:%s",
           nb_rot, nb_tr, worst, worst_b, list);
    nb_rot = nb_tr = 0, worst = 0;
    memset(seen, 0, sizeof seen);
}
int coop_proxy_mode(void);
int coop_puppet_before_writer(uint64_t pose, uint64_t skel) {
    if (skel && skel == atomic_load_explicit(&my_body_skel, memory_order_relaxed)) {
        static __thread uint8_t tmp[COOP_POSE_MAX];
        if (coop_proxy_mode() && proxy_records_on()) return 0;   // coop_proxy_pack_mine sends the records instead
        int len = pose_pack_input(pose, tmp, sizeof tmp);
        pose_log("out", pose);
        if (len) {
            plat_mutex_lock(&pose_lock);
            memcpy(pose_latest, tmp, len), atomic_store(&pose_latest_len, len);
            plat_mutex_unlock(&pose_lock);
        }
        return 0;
    }
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++) {
        if (skel != atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) continue;
        uint64_t was = atomic_exchange(&puppet_pose[i], pose);
        static _Atomic int said_obj;
        if (was != pose && atomic_fetch_add(&said_obj, 1) < 4) rt_log("coop: puppet: input pose object %llx for skeleton %llx", (unsigned long long)pose, (unsigned long long)skel);
        static __thread uint8_t in[COOP_POSE_MAX];
        uint64_t age = 0;
        int len = coop_net_pose(atomic_load(&puppet_id[i]), in, sizeof in, &age);
        int ok = 0;
        if (coop_proxy_mode()) {   // the pose from the same moment as the proxy's position
            if (len && age < 2000000000ull) pose_hist_take(atomic_load(&puppet_id[i]), in, len, age);
            ok = len && age < 2000000000ull && pose_put_delayed(atomic_load(&puppet_id[i]), pose, 0);
        } else ok = len && age < 2000000000ull ? pose_unpack_input(in, len, pose) : 0;
        if (ok) pose_log("on", pose);
        static _Atomic int said_u, said_f;
        uint16_t have = 0, n2 = 0;
        coop_rd(pose + 0xa, &have, 2);
        if (len >= 2) memcpy(&n2, in, 2);
        if (ok && atomic_fetch_add(&said_u, 1) == 0) rt_log("coop: puppet: pose in (%d bytes, %u bones, %.0f ms old)", len, n2, age / 1e6);
        if (!ok && atomic_fetch_add(&said_f, 1) < 3) rt_log("coop: puppet: pose not put on (%d bytes, %u bones in it, %u here, %.0f ms old)", len, n2, have, age / 1e6);
        return 1;
    }
    return 0;
}
int coop_pose_wanted(void) { return atomic_load_explicit(&pose_comp, memory_order_relaxed) != 0; }
int coop_pose_is_target(uint64_t binding) {
    return binding && binding == atomic_load_explicit(&pose_binding, memory_order_relaxed);
}
static void find_pose_binding(uint64_t actor) {
    if (atomic_load(&pose_binding)) return;
    static uint64_t w[0x4000 / 8], cw[0x1000 / 8];
    if (!coop_rd(actor, w, sizeof w)) return;
    uint64_t table = coop_rd_ptr(actor + 0x228);
    for (int i = 0; i < 16; i++) {
        uint64_t b = atomic_load(&bindings_seen[i]);
        if (!b) continue;
        for (int j = 0; j < 0x4000 / 8; j++)
            if (w[j] == b) { atomic_store(&pose_binding, b); rt_log("coop: test: the co-op Link's animation binding %llx at actor+%#x", (unsigned long long)b, j * 8); return; }
        for (int k = 0; table && k < 0x100; k++) {
            uint64_t comp = coop_rd_ptr(table + k * 8);
            if (comp < 0x100000000ull || comp >= 0x8000000000ull || !coop_rd(comp, cw, sizeof cw)) continue;
            for (int j = 0; j < 0x1000 / 8; j++)
                if (cw[j] == b) {
                    atomic_store(&pose_binding, b);
                    rt_log("coop: test: the co-op Link's animation binding %llx at comp[+%#x]+%#x", (unsigned long long)b, k * 8, j * 8);
                    return;
                }
        }
    }
    // Not referenced there: by elimination, for this test (this Link's binding is posed from the start, before the
    // co-op Link exists, so it is the first one seen; the next one is the co-op Link's).
    uint64_t second = atomic_load(&bindings_seen[1]);
    if (second) {
        atomic_store(&pose_binding, second);
        rt_log("coop: test: the co-op Link's animation binding %llx, by elimination (this Link's %llx)", (unsigned long long)second,
               (unsigned long long)atomic_load(&bindings_seen[0]));
    }
}
// Test (TOTK_COOP_TEST_REFSCAN=<s1>,<s2>[,...]): at each of those world times, every 8-byte word in guest memory
// 0x100000000..0x240000000 equal to this Link's actor or its reference object, logged (up to 600) with which it is:
// before and after an enemy notices this Link, the new holders are where it keeps its target (stage 5).
static void refscan(uint64_t me) {
    const char *e = cfg_get("TOTK_COOP_TEST_REFSCAN");
    static int done_n;
    if (!e) return;
    double times[8];
    int nt = 0;
    for (const char *q = e; *q && nt < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) times[nt++] = atof(q);
    double now = frame_step_world_us() / 1e6;
    if (done_n >= nt || now < times[done_n]) return;
    done_n++;
    uint64_t ref = 0;
    coop_resident_named("Player", &ref);
    static uint64_t buf[0x100000 / 8];
    int found = 0;
    uint64_t t0 = plat_now_ns();
    for (uint64_t a = 0x100000000ull; a < 0x240000000ull; a += sizeof buf) {
        if (!plat_mem_read(a, buf, sizeof buf)) {   // in 64 KB pieces where the whole MB does not read
            for (uint64_t b = a; b < a + sizeof buf; b += 0x10000) {
                if (!plat_mem_read(b, buf, 0x10000)) continue;
                for (int i = 0; i < 0x10000 / 8; i++)
                    if ((buf[i] == me || (ref && buf[i] == ref)) && found++ < 600)
                        rt_log("coop: test: refscan %d %llx = %s", done_n, (unsigned long long)(b + i * 8), buf[i] == me ? "actor" : "ref");
            }
            continue;
        }
        for (size_t i = 0; i < sizeof buf / 8; i++)
            if ((buf[i] == me || (ref && buf[i] == ref)) && found++ < 600)
                rt_log("coop: test: refscan %d %llx = %s", done_n, (unsigned long long)(a + i * 8), buf[i] == me ? "actor" : "ref");
    }
    rt_log("coop: test: refscan %d at %.1f s: %d words (actor %llx, ref %llx), %.1f s", done_n, now, found, (unsigned long long)me,
           (unsigned long long)ref, (plat_now_ns() - t0) / 1e9);
}
// Test (TOTK_COOP_TEST_TARGETSWAP=<s0>,<s1>): the words in guest memory holding this Link's reference object at s1
// but not at s0 (an enemy spawned in between: where it keeps its target, refscan) are rewritten with the first
// co-op Link's reference whenever they hold this Link's, every tick from s1 (TOTK_COOP_TEST_TARGETSWAP_OFF=1: only
// watched). Every 60 ticks: the nearest enemy's distance to both Links, this Link's life, writes so far.
static uint64_t swap_base[4096], swap_words[16];
static int swap_nbase, swap_nwords, swap_state;
static void ref_holders(uint64_t ref, uint64_t *out, int *n, int max) {
    static uint64_t buf[0x10000 / 8];
    *n = 0;
    for (uint64_t a = 0x100000000ull; a < 0x240000000ull; a += sizeof buf) {
        if (!plat_mem_read(a, buf, sizeof buf)) continue;
        for (size_t i = 0; i < sizeof buf / 8; i++) if (buf[i] == ref && *n < max) out[(*n)++] = a + i * 8;
    }
}
struct near_enemy { uint64_t me, best; float d; };
static void near_fn(int cell, uint64_t a, void *arg) {
    struct near_enemy *ne = arg;
    char nm[48];
    if (!enemy_of(cell, a, nm, sizeof nm)) return;
    float p[3], q[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(ne->me + COOP_ACTOR_POS, q, 12)) return;
    float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    if (!ne->best || d < ne->d) ne->best = a, ne->d = d;
}
static void target_swap(uint64_t me) {
    const char *e = cfg_get("TOTK_COOP_TEST_TARGETSWAP");
    if (!e || !strchr(e, ',')) return;
    double s0 = atof(e), s1 = atof(strchr(e, ',') + 1), now = frame_step_world_us() / 1e6;
    uint64_t ref = 0;
    coop_resident_named("Player", &ref);
    if (!ref) return;
    if (swap_state == 0 && now >= s0) { ref_holders(ref, swap_base, &swap_nbase, 4096); swap_state = 1; rt_log("coop: test: targetswap base: %d words", swap_nbase); }
    if (swap_state == 1 && now >= s1) {
        static uint64_t now_w[4096];
        int n = 0;
        ref_holders(ref, now_w, &n, 4096);
        for (int i = 0; i < n && swap_nwords < 16; i++) {
            int old = 0;
            for (int k = 0; k < swap_nbase && !old; k++) old = swap_base[k] == now_w[i];
            if (!old) swap_words[swap_nwords++] = now_w[i], rt_log("coop: test: targetswap word %llx", (unsigned long long)now_w[i]);
        }
        swap_state = 2;
        rt_log("coop: test: targetswap: %d new words of %d", swap_nwords, n);
    }
    if (swap_state != 2) return;
    uint64_t co[1];
    int ids[1];
    uint64_t co_ref = 0;
    if (coop_player_actors(co, ids, 1) != 1) return;
    for (int k = 0; k < 16 && !co_ref; k++) {   // the co-op Link's reference: a word in its first 0x400 bytes whose +0x40 is it
        uint64_t w[0x400 / 8];
        if (!coop_rd(co[0], w, sizeof w)) break;
        for (int i = 0; i < 0x400 / 8 && !co_ref; i++) if (w[i] > 0x100000000ull && coop_rd_ptr(w[i] + 0x40) == co[0]) co_ref = w[i];
    }
    static int off = -1, writes, t;
    if (off < 0) off = cfg_get("TOTK_COOP_TEST_TARGETSWAP_OFF") != NULL;
    for (int i = 0; i < swap_nwords && !off && co_ref; i++)
        if (coop_rd_ptr(swap_words[i]) == ref) ST64(swap_words[i], co_ref), writes++;
    if (++t % 60) return;
    struct near_enemy ne = {me, 0, 0};
    each_live(near_fn, &ne);
    float pe[3] = {0}, pc[3] = {0};
    if (ne.best) coop_rd(ne.best + COOP_ACTOR_POS, pe, 12);
    coop_rd(co[0] + COOP_ACTOR_POS, pc, 12);
    float dc = sqrtf((pe[0] - pc[0]) * (pe[0] - pc[0]) + (pe[1] - pc[1]) * (pe[1] - pc[1]) + (pe[2] - pc[2]) * (pe[2] - pc[2]));
    char vals[200];
    int m = 0;
    for (int i = 0; i < swap_nwords && m < 180; i++) {
        uint64_t v = coop_rd_ptr(swap_words[i]);
        m += snprintf(vals + m, sizeof vals - m, " %s", v == ref ? "mine" : v == co_ref ? "co-op" : "other");
    }
    vals[m] = 0;
    rt_log("coop: test: targetswap %.1f s: enemy %.1f m from this Link, %.1f m from the co-op Link; life %d; %d writes (co-op ref %llx); words:%s",
           now, ne.best ? ne.d : -1.0f, ne.best ? dc : -1.0f, life_of(me), writes, (unsigned long long)co_ref, vals);
}
// TOTK_COOP_TEST_ASPLAY (see coop_spawn.c coop_as_template): the AS manager's place on a Player, learned from this
// Link's (a word in one of its components, or in an object a component points to, equal to the manager), then the
// call for the co-op Link's.
static void as_play_test(Cpu *c, uint64_t me) {
    const char *e = cfg_get("TOTK_COOP_TEST_ASPLAY");
    static int done, comp = -1, off = -1, off2 = -1, calls;
    double at = e && strchr(e, '@') ? atof(strchr(e, '@') + 1) : 0, now = frame_step_world_us() / 1e6;
    if (!e || !strchr(e, '@') || now < at || now > at + 1.5) return;   // every tick for 1.5 s (its AI may undo one)
    uint64_t coop_as_template(uint64_t *mgr, uint8_t **name_obj, uint8_t **req, char **text);   // coop_spawn.c
    uint64_t mgr;
    uint8_t *nobj, *req;
    char *text;
    if (!coop_as_template(&mgr, &nobj, &req, &text)) return;
    (void)done;
    uint64_t table = coop_rd_ptr(me + 0x228);
    static uint64_t w[0x1000 / 8], w2[0x400 / 8];
    for (int j = 0; table && j < 0x100 && comp == -1; j++) {
        uint64_t cp = coop_rd_ptr(table + j * 8);
        if (cp < 0x100000000ull || cp >= 0x8000000000ull || !coop_rd(cp, w, sizeof w)) continue;
        for (int i = 0; i < 0x1000 / 8 && comp < 0; i++) {
            if (w[i] == mgr) comp = j * 8, off = i * 8;
            else if (w[i] > 0x100000000ull && w[i] < 0x8000000000ull && coop_rd(w[i], w2, sizeof w2))
                for (int k = 0; k < 0x400 / 8; k++) if (w2[k] == mgr) { comp = j * 8, off = i * 8, off2 = k * 8; break; }
        }
    }
    uint64_t co[1];
    int ids[1];
    if (comp < 0 || coop_player_actors(co, ids, 1) != 1) { rt_log("coop: test: asplay: manager not found on this Link (%d)", comp); return; }
    uint64_t x = coop_rd_ptr(coop_rd_ptr(co[0] + 0x228) + (uint64_t)comp);
    x = x ? coop_rd_ptr(x + (uint64_t)off) : 0;
    if (off2 >= 0) x = x ? coop_rd_ptr(x + (uint64_t)off2) : 0;
    char nm[40] = "";
    int slot = 0;
    sscanf(e, "%39[^,],%d", nm, &slot);
    snprintf(text, 48, "%s", nm);
    uint64_t tp = (uint64_t)(uintptr_t)text;
    memcpy(nobj + 8, &tp, 8);   // the string object's text pointer (+8 as name_like found it, or +0)
    if (coop_rd_ptr((uint64_t)(uintptr_t)nobj) < 0x7000000000ull) memcpy(nobj, &tp, 8);
    int32_t sl = slot;
    memcpy(req + 0x28, &sl, 4);
    uint64_t r = x ? coop_guest_call(c, MOD_MAIN_BASE + 0x4dcc1c, x, (uint64_t)(uintptr_t)nobj, (uint64_t)(uintptr_t)req, 0) : 0;
    if (calls++ % 15) return;
    rt_log("coop: test: asplay %s slot %d on the co-op Link's manager %llx (this Link's %llx at comp +%#x +%#x +%#x): returned %llu",
           nm, slot, (unsigned long long)x, (unsigned long long)mgr, comp, off, off2, (unsigned long long)r);
}
void coop_world_tick(Cpu *c, uint64_t me) {
    {   // The enemy tables start again with a session, and when this Link becomes another actor (a save loaded): kept,
        // enemies made again at the same place within 5 s were still reported dead and died at once, and old lives
        // made hits that were not (code review, 2026-11-06).
        static int was_on;
        static uint64_t was_me;
        int on = coop_net_on();
        if ((on && !was_on) || (me && was_me && me != was_me)) {
            memset(dead, 0, sizeof dead), memset(host_known, 0, sizeof host_known), memset(guest_last, 0, sizeof guest_last);
            rt_log("coop: enemy tables cleared (%s)", on && !was_on ? "a session began" : "this Link is another actor");
        }
        was_on = on;
        if (me) was_me = me;
    }
    refscan(me);
    as_play_test(c, me);
    target_swap(me);
    cache_links(me);
    watch_slot5();
    model_scan(me);
    puppet_tick(me);
    // Test (TOTK_COOP_TEST_MOVE=1|2|3): every tick, the first co-op Link's owner position written into the two 3x4
    // matrices on the way to its skeleton: [actor+0x48] +0xa34 (1), [[actor+0x48]+0x2a8] +0x26c (2), or both (3)
    // (translation column: +0xc, +0x1c, +0x2c). With TOTK_COOP_TEST_FREEZE: what moves the frozen model.
    const char *mv = cfg_get("TOTK_COOP_TEST_MOVE");
    float owner[1][3];
    uint64_t ma[1];
    int mi[1];
    if (mv && atoi(mv) == 6 && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        // =6: the model (component +0x8, +0x28): its world matrix +0x1f8 (3x4 rows) takes the owner's position, and
        // its dirty bit (+0x240 bit 0) is set so main+0x24cc30 pushes it to the model units (needs that phase to run:
        // TOTK_COOP_TEST_FREEZE_PHASES=0).
        uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ma[0] + 0x228) + 0x8) + 0x28);
        if (model) {
            ST32(model + 0x1f8 + 0xc, *(uint32_t *)&owner[0][0]);
            ST32(model + 0x1f8 + 0x1c, *(uint32_t *)&owner[0][1]);
            ST32(model + 0x1f8 + 0x2c, *(uint32_t *)&owner[0][2]);
            ST8(model + 0x240, LD8(model + 0x240) | 1);
        }
        static int said6;
        if (!said6++) rt_log("coop: test: move 6: model %llx follows the owner", (unsigned long long)model);
    } else if (mv && atoi(mv) == 5 && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        // =5: the actor's own position (+0x2b4) set to the owner's every tick
        memcpy((void *)(uintptr_t)(ma[0] + COOP_ACTOR_POS), owner[0], 12);
        static int said5;
        if (!said5++) rt_log("coop: test: move 5: the co-op Link's actor position follows its owner");
    } else if (mv && atoi(mv) == 4 && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        // =4: the 4x4 row-vector draw matrices at [[comp table +0x8] +0x28] +0x5b0 (127, 0x40 each; translation in
        // the last row, from a world origin), shifted by the owner's movement since the first tick (snapshot kept).
        static uint8_t snap4[127 * 0x40];
        static float base[3];
        static uint64_t arr4;
        uint64_t arr = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ma[0] + 0x228) + 0x8) + 0x28);
        if (arr && arr4 != arr + 0x5b0) {
            arr4 = arr + 0x5b0;
            coop_rd(arr4, snap4, sizeof snap4);
            coop_rd(ma[0] + COOP_ACTOR_POS, base, 12);
            rt_log("coop: test: move 4: draw matrices %llx taken at %.2f %.2f %.2f", (unsigned long long)arr4, base[0], base[1], base[2]);
        }
        if (arr4) {
            static uint8_t out[127 * 0x40];
            memcpy(out, snap4, sizeof out);
            for (int i = 0; i < 127; i++) {
                float *m = (float *)(out + i * 0x40);
                for (int k = 0; k < 3; k++) m[12 + k] += owner[0][k] - base[k];
            }
            memcpy((void *)(uintptr_t)arr4, out, sizeof out);
        }
    } else if (mv && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        int which = atoi(mv);
        uint64_t n1 = coop_rd_ptr(ma[0] + 0x48), n2 = n1 ? coop_rd_ptr(n1 + 0x2a8) : 0;
        uint64_t mats[2] = {(which & 1) && n1 ? n1 + 0xa34 : 0, (which & 2) && n2 ? n2 + 0x26c : 0};
        for (int i = 0; i < 2; i++)
            if (mats[i]) ST32(mats[i] + 0xc, *(uint32_t *)&owner[0][0]), ST32(mats[i] + 0x1c, *(uint32_t *)&owner[0][1]), ST32(mats[i] + 0x2c, *(uint32_t *)&owner[0][2]);
        static int said;
        if (!said++) rt_log("coop: test: moving the co-op Link's matrices %llx %llx", (unsigned long long)mats[0], (unsigned long long)mats[1]);
    }
    // Test (TOTK_COOP_TEST_SKELCOPY=1): every tick, this Link's body skeleton (the g3d SkeletonObj at actor +0x48,
    // +0x2a8, +0x20, +0x20, +0, +0xf8, +0x58, +0xb0: 127 bones) copied onto the first co-op Link's: its world matrices
    // (both buffers, +0x18 and +0x58) and local ones (+0x20). With TOTK_COOP_TEST_FREEZE: a pose shown without the
    // co-op Link's update running.
    // The pose copy itself runs right after the co-op Link's animation step (coop_pose_after_anim, from coop_spawn.c's
    // hook on main+0x2e9c80); here the pointers it needs are cached once a tick.
    if (cfg_get("TOTK_COOP_TEST_SKELCOPY")) {
        uint64_t ol[1];
        int oi[1];
        if (coop_player_actors(ol, oi, 1) == 1) {
            uint64_t s0 = me, s1 = ol[0];
            for (int i = 0; i < 8 && s0 && s1; i++) s0 = coop_rd_ptr(s0 + skel_path[i]), s1 = coop_rd_ptr(s1 + skel_path[i]);
            uint16_t n0 = 0, n1 = 0;
            int ok = s0 && s1 && is_skeleton(s0) && is_skeleton(s1) && coop_rd(s0 + 0x48, &n0, 2) && coop_rd(s1 + 0x48, &n1, 2) && n0 == n1;
            atomic_store(&pose_src, ok ? s0 : 0), atomic_store(&pose_dst, ok ? s1 : 0), atomic_store(&pose_bones, ok ? n0 : 0);
            atomic_store(&pose_comp, coop_rd_ptr(coop_rd_ptr(ol[0] + 0x228) + 0x8));
            find_pose_binding(ol[0]);
            cache_skeletons(me, ol[0]);
            static int said;
            if (ok && !said++) rt_log("coop: test: skeleton copy %llx -> %llx, %u bones, after the animation of %llx",
                                      (unsigned long long)s0, (unsigned long long)s1, n0, (unsigned long long)atomic_load(&pose_comp));
            // TOTK_COOP_TEST_SKELCOPY=tick: the old way, copied here (for comparison)
            const char *mode = cfg_get("TOTK_COOP_TEST_SKELCOPY");
            if (ok && !strcmp(mode, "tick")) coop_pose_copy_now();
        }
    }
    // Test (TOTK_COOP_TEST_POSECOPY=<offset>,<count>,<stride>): every tick, the matrices at [[comp table +0x8] +0x28]
    // + offset (the bone scan's candidate) copied from this Link to the first co-op Link, raw.
    const char *pc = cfg_get("TOTK_COOP_TEST_POSECOPY");
    uint64_t other[1];
    int oid[1];
    if (pc && coop_player_actors(other, oid, 1) == 1) {
        unsigned off = (unsigned)strtoul(pc, NULL, 0), cnt = 127, str = 0x40;
        const char *q = strchr(pc, ',');
        if (q) cnt = (unsigned)strtoul(q + 1, NULL, 0), q = strchr(q + 1, ',');
        if (q) str = (unsigned)strtoul(q + 1, NULL, 0);
        uint64_t src = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x8) + 0x28);
        uint64_t dst = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(other[0] + 0x228) + 0x8) + 0x28);
        // TOTK_COOP_TEST_POSEPATH=<o1>,<o2>...: more pointer steps from there before the offset
        for (const char *pp = cfg_get("TOTK_COOP_TEST_POSEPATH"); pp && *pp && src && dst;) {
            unsigned o = (unsigned)strtoul(pp, NULL, 0);
            src = coop_rd_ptr(src + o), dst = coop_rd_ptr(dst + o);
            pp = strchr(pp, ',');
            if (pp) pp++;
        }
        static int said;
        if (src && dst) {
            static uint8_t tmp[0x8000];
            size_t len = (size_t)cnt * str;
            if (len <= sizeof tmp && coop_rd(src + off, tmp, len)) {
                if (!said++) {
                    float *m = (float *)tmp;
                    rt_log("coop: test: pose copy %#x x%u from %llx to %llx; first matrix %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
                           off, cnt, (unsigned long long)src, (unsigned long long)dst, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                           m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                }
                memcpy((void *)(uintptr_t)(dst + off), tmp, len);
            }
        }
    }
    // Test (TOTK_COOP_TEST_FOLLOW=lo-hi): after the scan, the co-op Link's position copies lo..hi (indices) take its
    // owner's position every tick (with TOTK_COOP_TEST_FREEZE: which copy the frozen model is drawn at).
    const char *fl = cfg_get("TOTK_COOP_TEST_FOLLOW");
    float want[1][3];
    if (fl && ncand && coop_player_positions(want, 1) == 1) {
        int lo = atoi(fl), hi = strchr(fl, '-') ? atoi(strchr(fl, '-') + 1) : ncand - 1;
        static int said;
        if (!said++) rt_log("coop: test: copies %d..%d of %d follow the owner", lo, hi, ncand);
        for (int i = lo; i <= hi && i < ncand; i++) {
            if (cand_col[i]) ST32(cand[i], *(uint32_t *)&want[0][0]), ST32(cand[i] + 16, *(uint32_t *)&want[0][1]), ST32(cand[i] + 32, *(uint32_t *)&want[0][2]);
            else memcpy((void *)(uintptr_t)cand[i], want[0], 12);
        }
    }
    static int ton = -1, tt;
    if (ton < 0) { const char *e = getenv("TOTK_COOP_TRACE"); ton = e && strstr(e, "target"); }
    if (ton && ++tt % 60 == 0) {
        int ids[COOP_MAX_PLAYERS];
        tp_links[0] = me;
        coop_resident_named("Player", &tp_refs[0]);
        tp_n = 1 + coop_player_actors(tp_links + 1, ids, COOP_MAX_PLAYERS);
        for (int i = 1; i < tp_n; i++) tp_refs[i] = 0;
        for (int i = 0; i < tp_n; i++) tp_ids[i] = perception_id(tp_links[i]);
        static int said;
        if (!said++ || tt % 600 == 0) rt_log("coop: perception id of this Link %#x%s", tp_ids[0], tp_n > 1 ? ", of co-op Links:" : "");
        for (int i = 1; i < tp_n && tt % 600 == 0; i++) rt_log("coop:   co-op Link %d: %#x", i, tp_ids[i]);
        // Test (TOTK_COOP_TEST_RESIDENT): the co-op Links' perception flags (data +0x14, +0x1c and the second record's
        // +0x27c, +0x284) made this Link's, to see whether enemies then take them as targets.
        if (cfg_get("TOTK_COOP_TEST_RESIDENT")) {
            uint64_t d0 = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(tp_links[0] + 0x228) + 0x378) + 0x30);
            for (int i = 1; i < tp_n && d0; i++) {
                uint64_t d1 = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(tp_links[i] + 0x228) + 0x378) + 0x30);
                static const int offs[4] = {0x14, 0x1c, 0x27c, 0x284};
                for (int k = 0; d1 && k < 4; k++) {
                    uint32_t v = 0, old = 0;
                    coop_rd(d0 + offs[k], &v, 4), coop_rd(d1 + offs[k], &old, 4);
                    if (v != old) ST32(d1 + offs[k], v), rt_log("coop: test: co-op Link %d perception +%#x %#x -> %#x", i, offs[k], old, v);
                }
            }
        }
        // The registry main+0x72b4e8 takes "the player" from: [main+0x3a75a40], count +0x38, sorted pointers +0x40 to
        // {u32 key, +8 object}; key 0x5d5d398d, then object +0x150. What that object and its +0x150 are. Once, at 20 s.
        static int reg_said;
        if (!reg_said && tt > 600) {
            reg_said = 1;
            uint64_t r = coop_rd_ptr(MOD_MAIN_BASE + 0x3a75a40), arr = r ? coop_rd_ptr(r + 0x40) : 0;
            uint32_t n = 0;
            if (r) coop_rd(r + 0x38, &n, 4);
            rt_log("coop: registry %llx: %u entries, +0x20 %llx -> +0x150 %llx", (unsigned long long)r, n,
                   (unsigned long long)coop_rd_ptr(r + 0x20), (unsigned long long)coop_rd_ptr(coop_rd_ptr(r + 0x20) + 0x150));
            for (uint32_t i = 0; arr && i < n && i < 4096; i++) {
                uint64_t e = coop_rd_ptr(arr + i * 8);
                uint32_t key = 0;
                if (!e || !coop_rd(e, &key, 4) || key != 0x5d5d398du) continue;
                uint64_t o = coop_rd_ptr(e + 8), p = o ? coop_rd_ptr(o + 0x150) : 0;
                char on[64] = "", pn[64] = "";
                coop_actor_name(o, on, sizeof on), coop_actor_name(p, pn, sizeof pn);
                rt_log("coop: registry player entry %u: %llx object %llx (%s) +0x150 %llx (%s); this Link %llx, its ref %llx",
                       i, (unsigned long long)e, (unsigned long long)o, on, (unsigned long long)p, pn,
                       (unsigned long long)tp_links[0], (unsigned long long)tp_refs[0]);
                const char *w = link_of(o) ? link_of(o) : link_of(p);
                rt_log("coop: registry player entry is %s", w ? w : "neither Link nor its ref");
                // Words of both objects (0x1000) and of what they point to (0x400) equal to a Link, its ref, or its
                // perception component or data.
                uint64_t pc = coop_rd_ptr(coop_rd_ptr(tp_links[0] + 0x228) + 0x378), pdat = pc ? coop_rd_ptr(pc + 0x30) : 0;
                uint64_t roots[2] = {o, p};
                for (int k = 0; k < 2; k++)
                    for (int j = 0; roots[k] && j < 0x1000 / 8; j++) {
                        uint64_t v = coop_rd_ptr(roots[k] + j * 8);
                        const char *s = link_of(v) ? link_of(v) : v && v == pc ? "this Link's perception component" : v && v == pdat ? "this Link's perception data" : NULL;
                        if (s) rt_log("coop:   %s +%#x = %s", k ? "+0x150 object" : "object", j * 8, s);
                        if (v < 0x100000000ull || v >= 0x8000000000ull) continue;
                        for (int i2 = 0; i2 < 0x400 / 8; i2++) {
                            uint64_t w2 = coop_rd_ptr(v + i2 * 8);
                            const char *s2 = link_of(w2) ? link_of(w2) : w2 && w2 == pc ? "this Link's perception component" : w2 && w2 == pdat ? "this Link's perception data" : NULL;
                            if (s2) rt_log("coop:   [%s +%#x]+%#x = %s", k ? "+0x150 object" : "object", j * 8, i2 * 8, s2);
                        }
                    }
            }
        }
        // Where the game keeps either Link's perception component or data: words of main's data (.data to .bss) and
        // of the objects they point to (first 0x800 bytes) equal to one of them. Once, after 20 s.
        static int scanned;
        if (tp_n > 1 && !scanned && tt > 600) {
            scanned = 1;
            uint64_t want[4];
            for (int i = 0; i < 2; i++) {
                want[i * 2] = coop_rd_ptr(coop_rd_ptr(tp_links[i] + 0x228) + 0x378);
                want[i * 2 + 1] = want[i * 2] ? coop_rd_ptr(want[i * 2] + 0x30) : 0;
            }
            static const char *const what[4] = {"this Link's component", "this Link's data", "co-op Link's component", "co-op Link's data"};
            int found = 0;
            static uint64_t obj[0x800 / 8];
            for (uint64_t g = MOD_MAIN_BASE + 0x357dee8; g < MOD_MAIN_BASE + 0x3adefd0 && found < 40; g += 8) {
                uint64_t v = coop_rd_ptr(g);
                for (int k = 0; k < 4; k++)
                    if (v && v == want[k]) rt_log("coop: perception scan: main+%#llx = %s", (unsigned long long)(g - MOD_MAIN_BASE), what[k]), found++;
                if (v < 0x100000000ull || v >= 0x8000000000ull || !coop_rd(v, obj, sizeof obj)) continue;
                for (int j = 0; j < 0x800 / 8; j++)
                    for (int k = 0; k < 4; k++)
                        if (obj[j] && obj[j] == want[k]) {
                            rt_log("coop: perception scan: [main+%#llx]+%#x = %s", (unsigned long long)(g - MOD_MAIN_BASE), j * 8, what[k]);
                            found++;
                        }
            }
            rt_log("coop: perception scan: %d places", found);
            for (uint64_t g = MOD_MAIN_BASE + 0x357dee8; g < MOD_MAIN_BASE + 0x3adefd0; g += 8) {   // who points at the objects
                uint64_t v = coop_rd_ptr(g);
                if (v == MOD_MAIN_BASE + 0x3ad9830 || v == MOD_MAIN_BASE + 0x3a6ea80 || v == MOD_MAIN_BASE + 0x3a4e6a8 ||
                    v == MOD_MAIN_BASE + 0x3a4e6e0 || v == MOD_MAIN_BASE + 0x3a83a30)
                    rt_log("coop: perception scan: main+%#llx holds &main+%#llx", (unsigned long long)(g - MOD_MAIN_BASE),
                           (unsigned long long)(v - MOD_MAIN_BASE));
            }
        }
        // Test (TOTK_COOP_TEST_SLOT=1..5): one of those five places made to hold the co-op Link's data from 20 s on.
        const char *slot = cfg_get("TOTK_COOP_TEST_SLOT");
        if (slot && tp_n > 1 && tt > 600) {
            static const uint32_t g_off[5][2] = {{0x3a4e6a8, 0x98}, {0x3a4e6e0, 0x370}, {0x3a6ea80, 0x58}, {0x3a83a30, 0x3b0}, {0x3ad9830, 0x250}};
            int k = atoi(slot) - 1;
            uint64_t obj = k >= 0 && k < 5 ? coop_rd_ptr(MOD_MAIN_BASE + g_off[k][0]) : 0;
            uint64_t d1 = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(tp_links[1] + 0x228) + 0x378) + 0x30);
            if (obj && d1 && coop_rd_ptr(obj + g_off[k][1]) != d1) {
                rt_log("coop: test: [main+%#x]+%#x = the co-op Link's perception data", g_off[k][0], g_off[k][1]);
                ST64(obj + g_off[k][1], d1);
            }
        }
        // Both Links' perception components ([actor+0x228] +0x378) and the object at their +0x30, side by side.
        static int dumped;
        if (tp_n > 1 && !dumped && tt > 600) {
            dumped = 1;
            for (int lvl = 0; lvl < 2; lvl++) {
                uint64_t c0 = coop_rd_ptr(coop_rd_ptr(tp_links[0] + 0x228) + 0x378), c1 = coop_rd_ptr(coop_rd_ptr(tp_links[1] + 0x228) + 0x378);
                if (lvl) c0 = coop_rd_ptr(c0 + 0x30), c1 = coop_rd_ptr(c1 + 0x30);
                uint64_t a0[0x300 / 8] = {0}, a1[0x300 / 8] = {0};
                coop_rd(c0, a0, sizeof a0), coop_rd(c1, a1, sizeof a1);
                rt_log("coop: perception %s: this Link's %llx, co-op Link's %llx", lvl ? "data (+0x30)" : "component", (unsigned long long)c0, (unsigned long long)c1);
                for (int j = 0; j < 0x300 / 8; j++) {
                    int ptr0 = a0[j] >= 0x100000000ull && a0[j] < 0x8000000000ull, ptr1 = a1[j] >= 0x100000000ull && a1[j] < 0x8000000000ull;
                    if (a0[j] != a1[j] && !(ptr0 && ptr1))
                        rt_log("coop:   +%#05x  %016llx  %016llx", j * 8, (unsigned long long)a0[j], (unsigned long long)a1[j]);
                }
            }
        }
        each_live(target_probe_one, NULL);
    }
    static int on = -1, t;
    if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "enemy"); }
    if (on && ++t % 300 == 0) { probe_me = me; each_live(probe_one, NULL); }
    int role = coop_world_role();
    if (role == 1) host_tick(c, me);
    else if (role == 2) guest_tick(c);
}
