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
// The enemies themselves are synced by coop_enemy.c and its files (docs/coop/enemy-sync.md). The older enemy
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
// An actor's key: its placement's (place_key), else its name and home (enemy_key).
static uint32_t actor_key(uint64_t a, const char *name, const float home[3]) {
    uint32_t k = place_key(a, name);
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

// The pointer path from an actor to its body skeleton (g3d SkeletonObj, coop_is_skeleton); coop_standin_pose.c follows it.
const uint32_t coop_skel_path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};   // actor to its body skeleton (g3d)

// ---- the co-op Links' life components, cached once a tick for coop_spawn.c (coop_is_coop_life).
static _Atomic uint64_t coop_life_comps[COOP_MAX_PLAYERS];
static void cache_links(uint64_t me) {
    (void)me;
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS];
    int n = coop_world_role() ? coop_player_actors(acts, ids, COOP_MAX_PLAYERS) : 0;
    for (int i = 0; i < n; i++) atomic_store(&coop_life_comps[i], coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x2f8));
    for (int i = n; i < COOP_MAX_PLAYERS; i++) atomic_store(&coop_life_comps[i], 0);
}
// A co-op Link's life component: its life update (main+0x4400b0) must not take life in this game (coop_spawn.c).
int coop_is_coop_life(uint64_t comp) {
    for (int i = 0; comp && i < COOP_MAX_PLAYERS; i++)
        if (atomic_load_explicit(&coop_life_comps[i], memory_order_relaxed) == comp) return 1;
    return 0;
}

// A g3d SkeletonObj (ModelObj::Initialize main+0x1f38c8 sets it up): +0x18 world matrices (3x4, 0x30 each) right
// after +0x20 local ones (0x40 each), both again at +0x58 / +0x60, the bone count u16 at +0x48. Found per Link.
int coop_is_skeleton(uint64_t s) {
    uint64_t w[0xd];
    uint16_t n = 0;
    if (!coop_rd(s, w, sizeof w) || !coop_rd(s + 0x48, &n, 2)) return 0;
    return n >= 20 && n <= 600 && w[4] && w[4] == w[0xc] && (w[3] - w[4] == (uint64_t)n * 0x40 || w[0xb] - w[4] == (uint64_t)n * 0x40);
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
// Test (TOTK_COOP_TEST_LINKDUMP=<s>[,<s2>[,<s3>[,<s4>]]]): at those world seconds, this Link's actor (0x3000 bytes) and the
// first 0x800 bytes of each object its +0x228 table points to (128 of them, in table order, zeros for none) to
// ./linkdump_<player id>_<n>.bin: two games' Links side by side, for a state one
// has and the other not (an enemy aware of it).
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
    enum { AS = 0x3000, CN = 128, CS = 0x800 };
    static uint8_t buf[AS + CN * CS];
    memset(buf, 0, sizeof buf);
    coop_rd(me, buf, AS);
    uint64_t tab = coop_rd_ptr(me + 0x228);
    for (int k = 0; tab && k < CN; k++) {
        uint64_t cp = coop_rd_ptr(tab + 8 * k);
        if (cp > 0x100000000ull && cp < 0x8000000000ull) coop_rd(cp, buf + AS + k * CS, CS);
    }
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "./linkdump_%d_%d.bin", coop_net_id(), done);
    rt_log("coop: test: linkdump %s at %.1f s: %s", path, frame_step_world_us() / 1e6, plat_file_write(path, buf, sizeof buf) ? "written" : "not written");
    done++;
}
// Each frame with another player in the session: this file's tests, the HIT channel (coop_hitchan.c), and about 10
// times a second the list of live enemies made again.
static void peer_tick(Cpu *c, uint64_t me) {
    coop_world_me = me;
    link_dump_test(me);
    coop_hitchan_tick(c, me);
    static int t;
    if (++t % 3 == 0) dup_count();
}

void coop_world_tick(Cpu *c, uint64_t me) {
    cache_links(me);
    coop_puppet_tick(me);
    static int ton = -1, tt;
    if (ton < 0) { const char *e = cfg_get("TOTK_COOP_TRACE"); ton = e && strstr(e, "target"); }
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
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_TRACE"); on = e && strstr(e, "enemy"); }
    if (on && ++t % 300 == 0) { probe_me = me; each_live(probe_one, NULL); }
    if (coop_world_role()) peer_tick(c, me);
    if (coop_world_role()) ground_tick(c, me);
}
