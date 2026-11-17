// Co-op shared world: what the games' syncs share about the actors of the world.
//
// - The actor registry: every live actor passes the per-actor calc (main+0x41bb20) each frame; coop_actor_seen records
//   it with the world time it was last seen and where it was first seen. An actor is known in every game by its
//   name and the map's placement hash, or, placed nowhere, by its name and that first place.
// - The spawn sync: an enemy made at run time (placed nowhere in the map) is announced to the other games, which make
//   one with the same key; that key is the enemy's identity in the enemy sync (coop_enemy.c, coop_world_spawn_key).
// - A player's blow on another game's enemy: the contact is sent to the game that runs the enemy ("CE"/"AE"
//   packets), which adds it to its enemy's hit update (coop_hit_update, contacts_filter); the enemy sync says whose
//   an enemy is.
// - Probes and tests of the research that found these.
// The HIT channel's reader is coop_hitchan.c.
// The enemies themselves are synced by coop_enemy.c and its files (docs/coop/enemy-sync-spec.md). The older enemy
// sync that lived here, and the horses and animals that rode on it, are gone (spec B.30).
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"


// ---- the actors this game updates
#define SEEN_N 16384
static struct {
    _Atomic uint64_t actor;
    _Atomic int64_t at_us;
    float home[3];        // where it was first seen
    _Atomic int home_ok;
    _Atomic uint64_t home_of;   // the actor home is of: a reader takes home only for its own actor (below)
    uint32_t name_hash;   // its name's, when first seen (an actor in a gone one's memory has another)
    uint64_t first_ns;    // when first seen (plat_now_ns: how long this game has run it, for whose an enemy is)
} seen[SEEN_N];
static uint32_t name_hash_of(uint64_t a) {
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    uint32_t h = 2166136261u;
    for (const char *p = nm; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}
uint32_t coop_world_name_hash(uint64_t a) { return name_hash_of(a); }   // (for coop_spawn_sync.c)
// Its first place, set where it is first seen (its first update): set at the first scan instead, up to 0.1 s later,
// an enemy that moved by then had another key in each game (code review, 2026-11-06).
static void set_home(int h, uint64_t actor) {
    float p[3];
    atomic_store_explicit(&seen[h].home_ok, 0, memory_order_relaxed);
    seen[h].name_hash = name_hash_of(actor);
    seen[h].first_ns = plat_now_ns();
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

uint32_t coop_world_enemy_key(const char *name, const float home[3]);
static uint32_t enemy_key(const char *name, const float home[3]) {
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int k = 0; k < 3; k++) {
        int32_t q = (int32_t)lroundf(home[k] * 2.0f);   // half metres: both games create it at the same spot
        for (int b = 0; b < 4; b++) h = (h ^ (uint8_t)(q >> (8 * b))) * 16777619u;
    }
    return h ? h : 1;
}
uint32_t coop_world_enemy_key(const char *name, const float home[3]) { return enemy_key(name, home); }   // (for coop_spawn_sync.c)
// An actor placed in the map has its placement's hash, [actor+0x370]+0x88 (the same in 1.2.1 and 1.4.3, and the
// same actor's in both games; TOTK_COOP_TEST_ACTORDUMP); 0 or all ones for one made at run time. TotK Online keys map
// actors by it (read for behaviour only): it holds when the actor has moved before a game first sees it, and two of
// one name placed half a metre apart get two keys. Folded with the name to 32 bits; 0: none. Enemies only
// (enemy_of): the objects of coop_actors_keyed and coop_actors_all keep the name and place, as the hash was checked
// on enemies alone, and an actor whose +0x370 is something else would be keyed apart in each game (code review).
#define PLACE_HASH_PTR 0x370
#define PLACE_HASH 0x88
static uint64_t place_hash(uint64_t a) {
    uint64_t ph = 0, pp = a ? coop_rd_ptr(a + PLACE_HASH_PTR) : 0;
    if (pp) coop_rd(pp + PLACE_HASH, &ph, 8);
    return ph == ~0ull ? 0 : ph;
}
int coop_actor_placed(uint64_t a) { return place_hash(a) != 0; }   // an actor of the map's placement (the same in every game)
static uint32_t place_key(uint64_t a, const char *name) {
    uint64_t ph = place_hash(a);
    if (!ph) return 0;
    uint32_t h = 2166136261u ^ 0x50u;   // (apart from enemy_key's)
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int b = 0; b < 8; b++) h = (h ^ (uint8_t)(ph >> (8 * b))) * 16777619u;
    return h ? h : 1;
}
// An actor's key: its placement's (place_key), else its name and home (enemy_key). TOTK_COOP_PLACE_KEY=0: the name
// and home always (the old way).
static uint32_t actor_key(uint64_t a, const char *name, const float home[3]) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_PLACE_KEY"); on = !(e && *e == '0'); }
    uint32_t k = on ? place_key(a, name) : 0;
    return k ? k : enemy_key(name, home);
}
uint32_t coop_world_actor_key(uint64_t a, const char *name, const float home[3]) { return actor_key(a, name, home); }   // (for the files split out of this one)
int coop_world_live_now(uint64_t a) {   // ran its calc in the last second (the seen table)
    uint32_t h = (uint32_t)((a >> 4) * 2654435761u) & (SEEN_N - 1);
    int64_t now = frame_step_world_us();
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1))
        if (atomic_load_explicit(&seen[h].actor, memory_order_relaxed) == a)
            return now - atomic_load_explicit(&seen[h].at_us, memory_order_relaxed) < 1000000;
    return 0;
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
// One actor's key as coop_actors_all gives it (0: no name); for the glue sync's objects (coop_glue.c).
uint32_t coop_actor_world_key(uint64_t a) {
    char nm[48] = {0};
    uint64_t np = 0;
    float home[3];
    if (!a || !coop_rd(a + COOP_ACTOR_NAME, &np, 8) || !np || !coop_rd(np, nm, 40) || !name_ok(nm)) return 0;
    if (!seen_home(a, home) && !coop_rd(a + COOP_ACTOR_POS, home, 12)) return 0;
    return enemy_key(nm, home);
}

// The key of the actor of a cell (its name in nm).
static uint32_t cell_key(int cell, uint64_t a, const char *nm) {
    uint32_t kk = coop_world_spawn_key(a);
    if (kk) return kk;
    if (!home_is(cell, a)) {
        if (!coop_rd(a + COOP_ACTOR_POS, seen[cell].home, 12)) return 0;
        atomic_store_explicit(&seen[cell].home_of, a, memory_order_release);
        atomic_store_explicit(&seen[cell].home_ok, 1, memory_order_release);
    }
    return actor_key(a, nm, seen[cell].home);
}
// An enemy this game updates, with its key (0 if not an enemy); for the probes below.
static uint32_t enemy_of(int cell, uint64_t a, char *nm, size_t n) {
    coop_actor_name(a, nm, n);
    return strncmp(nm, "Enemy_", 6) ? 0 : cell_key(cell, a, nm);
}
int coop_enemy_new(void);   // coop_enemy.c: the enemy sync is on

static float dist3(const float a[3], const float b[3]);
// Every live enemy here, refreshed each world tick (dup_count): coop_enemy_within, and the spawn sync's scan.
uint64_t coop_world_enemies[512];
int coop_world_n_enemies;
static void dup_one(int cell, uint64_t a, void *arg) {
    (void)arg, (void)cell;
    char nm[64];
    int coop_mount_kind(const char *nm);   // coop_mount.c: horses and animals, which the spawn sync takes too
    if ((!strncmp(coop_actor_name(a, nm, sizeof nm), "Enemy_", 6) || coop_mount_kind(nm)) && coop_world_n_enemies < 512) coop_world_enemies[coop_world_n_enemies++] = a;
}
static void dup_count(void) { coop_world_n_enemies = 0, each_live(dup_one, NULL); }
// Whether a live enemy of this game (synced or not) stands within m of p: a weapon there is in its hand (or was), not
// a thing lying in the world (coop_players.c, prop_fn). Night's Stalkoblins are made in each game on its own and not
// synced; one's sword was sent as a moved object and its copy hung in the air in the other game (a play session,
// 2026-11-07).
int coop_enemy_within(const float p[3], float m) {
    for (int i = 0; i < coop_world_n_enemies; i++) {
        float q[3];
        int32_t st = 0;
        if (!coop_rd(coop_world_enemies[i] + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || !coop_rd(coop_world_enemies[i] + COOP_ACTOR_POS, q, 12)) continue;
        if (dist3(p, q) < m) return 1;
    }
    return 0;
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

// Test (TOTK_COOP_TEST_WATCH5=<s>): from that many seconds into the world, for 10 s (TOTK_COOP_TEST_WATCHLEN=<s>
// for another length), the game functions that read slot 5 ([main+0x3ad9830] +0x250, this Link's perception data where enemies seem to take the player from).
const uint32_t coop_skel_path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};   // actor to its body skeleton (g3d)
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
                for (int i = 0; i < 8 && s; i++) s = coop_rd_ptr(s + coop_skel_path[i]);
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
        if (what && !strcmp(what, "coopeq")) {   // who reads the first stand-in's in-hand bytes (EquipmentUser +0x4620)
            uint64_t ca[1]; int ci[1];
            uint64_t q = coop_player_actors(ca, ci, 1) ? coop_rd_ptr(coop_rd_ptr(ca[0] + 0x228) + 0x230) : 0;
            if (q && coop_watch_arm(q + 0x4620, 8)) phase = 1, rt_log("coop: test: watching the stand-in's in-hand bytes %llx", (unsigned long long)(q + 0x4620));
            else if (!q) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "myeq")) {   // the same for this game's own Link
            uint64_t q = coop_rd_ptr(coop_rd_ptr(coop_player_actor() + 0x228) + 0x230);
            if (q && coop_watch_arm(q + 0x4620, 8)) phase = 1, rt_log("coop: test: watching this Link's in-hand bytes %llx", (unsigned long long)(q + 0x4620));
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
    } else if (phase == 1 && s >= atof(e) + (cfg_get("TOTK_COOP_TEST_WATCHLEN") ? atof(cfg_get("TOTK_COOP_TEST_WATCHLEN")) : 10)) {
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
int coop_is_skeleton(uint64_t s) {
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
        if (coop_is_skeleton(q)) {
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
        if (step < 8) node = coop_rd_ptr(node + coop_skel_path[step]);
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
static _Atomic uint64_t pose_src, pose_dst;
_Atomic uint64_t coop_pose_comp;
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
// (a pointer to it in the actor or its components) and keeps it in coop_pose_binding.
_Atomic uint64_t coop_bindings_seen[16], coop_pose_binding;
void coop_pose_saw(uint64_t binding) {
    for (int i = 0; binding && i < 16; i++) {
        uint64_t have = atomic_load_explicit(&coop_bindings_seen[i], memory_order_relaxed);
        if (have == binding) return;
        if (!have && atomic_compare_exchange_strong(&coop_bindings_seen[i], &have, binding)) return;
    }
}
// Before main+0x2e9c80 poses the co-op Link's model: this Link's animation pose (the binding's [+8] +0x38, 0x1c a
// bone - rotation and translation - count at +0x30) copied over the co-op Link's, so its model is posed with it.
void coop_pose_before_anim(uint64_t binding) {
    uint64_t mine = atomic_load(&coop_bindings_seen[0]);
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
    uint64_t comp = atomic_load_explicit(&coop_pose_comp, memory_order_relaxed);
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
        out[i] = s && coop_is_skeleton(s) ? s : 0;
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
// The sync audit's "enemies" (coop_players.c, audit_tick): an empty array. The enemy sync writes its own record to the
// log, which tools/coop/en_report.py compares across the games.
void coop_audit_enemies(FILE *f, uint64_t me) {
    (void)me;
    fputs("[]", f);
}

// ---- what the contact code and the spawn sync share
static float dist3(const float a[3], const float b[3]) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
uint64_t coop_world_me;   // this game's Link (set each world tick)
static void hit_watch_test(uint64_t me) {
    static int on = -1, phase;
    static double from, to;
    static unsigned off = 0x1a4, comp = 0x2f8;   // [@<off>[:<comp>]]: <comp> another entry of the +0x228 table
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_TEST_HITWATCH");
        on = e && sscanf(e, "%lf-%lf", &from, &to) == 2;
        const char *at = e ? strchr(e, '@') : NULL;
        if (at) off = (unsigned)strtoul(at + 1, NULL, 0);
        const char *co = at ? strchr(at, ':') : NULL;
        if (co) comp = (unsigned)strtoul(co + 1, NULL, 0);
    }
    if (!on || !me) return;
    double t = frame_step_world_us() / 1e6;
    {   // the receiver at the damage component's +0x190 (the attack contact calls its vtable +0x28): its vtable, once
        static int said;
        uint64_t dc = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x2f8), vt = dc ? coop_rd_ptr(dc + 0x190) : 0;
        if (vt && !said++) {
            char line[700];
            int m = snprintf(line, sizeof line, "coop: test: the damage component's receiver vtable main+%#llx:", (unsigned long long)(vt - MOD_MAIN_BASE));
            for (int k = 0; k < 24 && m < 660; k++) m += snprintf(line + m, sizeof line - m, " %d:%#llx", k, (unsigned long long)(coop_rd_ptr(vt + 8 * k) - MOD_MAIN_BASE));
            rt_log("%s", line);
            uint64_t dvt = coop_rd_ptr(dc);
            rt_log("coop: test: the damage component's own vtable main+%#llx", (unsigned long long)(dvt - MOD_MAIN_BASE));
        }
    }
    int coop_watch_arm(uint64_t addr, uint64_t len);
    void coop_watch_disarm(void);
    extern int coop_watch_writes;
    if (phase == 0 && t >= from) {
        uint64_t dc = coop_rd_ptr(coop_rd_ptr(me + 0x228) + comp);
        if (dc) coop_watch_writes = 1, phase = coop_watch_arm(dc + off, 8) ? 1 : 2;
    } else if (phase == 1 && t >= to) coop_watch_disarm(), phase = 2;
}
// Test (TOTK_COOP_TEST_GUARDSCAN=<t0>,<t1>): this Link's actor (0x3000 bytes) and the objects its +0x228 component
// table points to (+0..+0x400, 0x800 bytes each), at t0 and t1 world seconds: the 32-bit words that differ, logged
// for those that stayed the same over the 2 s before t0 (for finding a state such as the shield held up, ZL from t0 to t1).
static void guard_scan_test(uint64_t me) {
    static int on = -1, phase;
    static double t0, t1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_TEST_GUARDSCAN"); on = e && sscanf(e, "%lf,%lf", &t0, &t1) == 2; }
    if (!on || !me || phase > 2) return;
    enum { AW = 0x3000 / 4, CN = 0x400 / 8, CW = 0x800 / 4 };
    static uint32_t a[3][AW], cc[3][CN][CW];
    static uint64_t comps[CN];
    double t = frame_step_world_us() / 1e6, at[3] = {t0 - 2, t0 - 0.2, t1};
    if (t < at[phase]) return;
    uint64_t tab = coop_rd_ptr(me + 0x228);
    if (phase == 0 && tab) coop_rd(tab, comps, sizeof comps);
    coop_rd(me, a[phase], sizeof a[phase]);
    for (int k = 0; k < CN; k++) if (comps[k] > 0x100000000ull && comps[k] < 0x8000000000ull) coop_rd(comps[k], cc[phase][k], sizeof cc[phase][k]);
    if (++phase < 3) return;
    char line[1600];
    int m = snprintf(line, sizeof line, "coop: test: guardscan actor:");
    for (int i = 0; i < AW && m < 1500; i++)
        if (a[0][i] == a[1][i] && a[1][i] != a[2][i]) m += snprintf(line + m, sizeof line - m, " +%#x:%x>%x", i * 4, a[1][i], a[2][i]);
    rt_log("%s", line);
    for (int k = 0; k < CN; k++) {
        if (!comps[k]) continue;
        m = snprintf(line, sizeof line, "coop: test: guardscan comp +%#x:", k * 8);
        int any = 0;
        for (int i = 0; i < CW && m < 1500; i++)
            if (cc[0][k][i] == cc[1][k][i] && cc[1][k][i] != cc[2][k][i] && cc[2][k][i] - cc[1][k][i] + 0u < 0x100000u)
                m += snprintf(line + m, sizeof line - m, " +%#x:%x>%x", i * 4, cc[1][k][i], cc[2][k][i]), any = 1;
        if (any) rt_log("%s", line);
    }
}
// Test (TOTK_COOP_TEST_LINKDUMP=<s>[,<s2>[,<s3>[,<s4>]]]): at those world seconds, this Link's actor (0x3000 bytes) and the
// first 0x800 bytes of each object its +0x228 table points to (128 of them, in table order, zeros for none) to
// <TOTK_COOP_TEST_LINKDUMP_DIR or .>/linkdump_<player id>_<n>.bin: two games' Links side by side, for a state one
// has and the other not (an enemy aware of it).
static void actor_dump(uint64_t a, const char *what, int n) {
    enum { AS = 0x3000, CN = 128, CS = 0x800 };
    static uint8_t buf[AS + CN * CS];
    memset(buf, 0, sizeof buf);
    coop_rd(a, buf, AS);
    uint64_t tab = coop_rd_ptr(a + 0x228);
    for (int k = 0; tab && k < CN; k++) {
        uint64_t cp = coop_rd_ptr(tab + 8 * k);
        if (cp > 0x100000000ull && cp < 0x8000000000ull) coop_rd(cp, buf + AS + k * CS, CS);
    }
    const char *dir = cfg_get("TOTK_COOP_TEST_LINKDUMP_DIR");
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/%sdump_%d_%d.bin", dir ? dir : ".", what, coop_net_id(), n);
    rt_log("coop: test: %sdump %s at %.1f s: %s", what, path, frame_step_world_us() / 1e6, plat_file_write(path, buf, sizeof buf) ? "written" : "not written");
}
static void link_dump_test(uint64_t me) {
    static int on = -1, done;
    static double at[4];
    static int nat;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_TEST_LINKDUMP");
        nat = e ? sscanf(e, "%lf,%lf,%lf,%lf", &at[0], &at[1], &at[2], &at[3]) : 0;
        on = nat > 0;
    }
    if (!on || done >= nat || frame_step_world_us() / 1e6 < at[done]) return;
    {   // and the live enemy of that key (TOTK_COOP_TEST_LINKDUMP_KEY=<hex key>)
        const char *ks = cfg_get("TOTK_COOP_TEST_LINKDUMP_KEY");
        uint32_t want = ks ? (uint32_t)strtoul(ks, NULL, 16) : 0;
        uint64_t en = 0;
        for (int i = 0; !en && want && i < coop_world_n_enemies; i++) if (coop_actor_world_key(coop_world_enemies[i]) == want) en = coop_world_enemies[i];
        if (en) actor_dump(en, "enemy", done);
        if (en) {   // where the enemy holds this Link: its actor or its reference, in its first 0x3000 bytes and its components
            uint64_t ref = coop_actor_ref(me), tab = coop_rd_ptr(en + 0x228);
            char line[900];
            int m = snprintf(line, sizeof line, "coop: test: enemy %08x holds this Link (actor %llx, ref %llx) at:", want, (unsigned long long)me, (unsigned long long)ref);
            static uint64_t w[0x3000 / 8];
            for (int k = -1; k < 128 && m < 860; k++) {
                uint64_t base = k < 0 ? en : (tab ? coop_rd_ptr(tab + 8 * k) : 0);
                size_t len = k < 0 ? 0x3000 : 0x800;
                if (!base || !coop_rd(base, w, len)) continue;
                for (size_t i = 0; i < len / 8 && m < 860; i++)
                    if (w[i] == me || (ref && w[i] == ref))
                        m += snprintf(line + m, sizeof line - m, " %s+%#x%s(%s)", k < 0 ? "actor" : "comp", k < 0 ? (unsigned)(i * 8) : (unsigned)(k * 8),
                                      k < 0 ? "" : "", w[i] == me ? "actor" : "ref"),
                        m += k < 0 ? 0 : snprintf(line + m, sizeof line - m, "@%#x", (unsigned)(i * 8));
            }
            rt_log("%s", line);
        }
    }
    enum { AS = 0x3000, CN = 128, CS = 0x800 };
    static uint8_t buf[AS + CN * CS];
    memset(buf, 0, sizeof buf);
    coop_rd(me, buf, AS);
    uint64_t tab = coop_rd_ptr(me + 0x228);
    for (int k = 0; tab && k < CN; k++) {
        uint64_t cp = coop_rd_ptr(tab + 8 * k);
        if (cp > 0x100000000ull && cp < 0x8000000000ull) coop_rd(cp, buf + AS + k * CS, CS);
    }
    const char *dir = cfg_get("TOTK_COOP_TEST_LINKDUMP_DIR");
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/linkdump_%d_%d.bin", dir ? dir : ".", coop_net_id(), done);
    rt_log("coop: test: linkdump %s at %.1f s: %s", path, frame_step_world_us() / 1e6, plat_file_write(path, buf, sizeof buf) ? "written" : "not written");
    done++;
}
// Each frame with another player in the session: this file's tests, the HIT channel (coop_hitchan.c), and about 10
// times a second the list of live enemies made again.
static void peer_tick(Cpu *c, uint64_t me) {
    coop_world_me = me;
    link_dump_test(me);
    guard_scan_test(me);
    hit_watch_test(me);
    coop_hitchan_tick(c, me);
    static int t;
    if (++t % 3 == 0) dup_count();
}

void coop_world_tick(Cpu *c, uint64_t me) {
    refscan(me);
    as_play_test(c, me);
    target_swap(me);
    cache_links(me);
    watch_slot5();
    model_scan(me);
    coop_puppet_tick(me);
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
            for (int i = 0; i < 8 && s0 && s1; i++) s0 = coop_rd_ptr(s0 + coop_skel_path[i]), s1 = coop_rd_ptr(s1 + coop_skel_path[i]);
            uint16_t n0 = 0, n1 = 0;
            int ok = s0 && s1 && coop_is_skeleton(s0) && coop_is_skeleton(s1) && coop_rd(s0 + 0x48, &n0, 2) && coop_rd(s1 + 0x48, &n1, 2) && n0 == n1;
            atomic_store(&pose_src, ok ? s0 : 0), atomic_store(&pose_dst, ok ? s1 : 0), atomic_store(&pose_bones, ok ? n0 : 0);
            atomic_store(&coop_pose_comp, coop_rd_ptr(coop_rd_ptr(ol[0] + 0x228) + 0x8));
            coop_find_pose_binding(ol[0]);
            cache_skeletons(me, ol[0]);
            static int said;
            if (ok && !said++) rt_log("coop: test: skeleton copy %llx -> %llx, %u bones, after the animation of %llx",
                                      (unsigned long long)s0, (unsigned long long)s1, n0, (unsigned long long)atomic_load(&coop_pose_comp));
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
    if (coop_world_role()) peer_tick(c, me);
    void coop_anim_slots_test(uint64_t me);   // (below)
    coop_anim_slots_test(me);
    void coop_anim_copy_test(Cpu *c, uint64_t me);   // (below)
    coop_anim_copy_test(c, me);
    void coop_eqdiff_test(uint64_t me);   // (below)
    coop_eqdiff_test(me);
    void coop_binds_test(uint64_t me);   // (below)
    coop_binds_test(me);
    void coop_blur_test(uint64_t me);   // (below)
    coop_blur_test(me);
    void coop_eqcmp_test(uint64_t me);   // (below)
    coop_eqcmp_test(me);
    void coop_weaplog_test(uint64_t me);   // (below)
    coop_weaplog_test(me);
    if (coop_world_role()) ground_tick(c, me);
}

