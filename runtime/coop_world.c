// Co-op shared world: one game's enemies on the other games' screens.
//
// Every live actor passes the per-actor calc (main+0x41bb20) each frame; coop_actor_seen records it with the world
// time it was last seen and where it was first seen. An enemy (actor name "Enemy_...") is known in every game by
// its name and that first position: placed enemies are created at the same spot in every game on a save (checked
// to 0.1 m), and the map's own placement hash was not found in the actor (re-notes).
//
// Each enemy near more than one Link is run by one game, the one that has had it longest; the others show its place,
// pose, life and death and send it their hits (peer_tick and the enemy puppets, below). Enemies near no other Link
// are each game's own.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_mounts.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define SYNC_M 120.0f          // the enemies a game runs this close to another Link are sent
#define SNAP_M 1.5f            // an enemy another game runs (no poses) is put at its place past this
#define DEAD_KEEP_US 5000000   // an enemy that died here is reported dead this long to all

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

static uint32_t enemy_key(const char *name, const float home[3]) {
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int k = 0; k < 3; k++) {
        int32_t q = (int32_t)lroundf(home[k] * 2.0f);   // half metres: both games create it at the same spot
        for (int b = 0; b < 4; b++) h = (h ^ (uint8_t)(q >> (8 * b))) * 16777619u;
    }
    return h ? h : 1;
}
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

// Enemies made at run time (no place in the map: a night's Stalkoblins, a spawner's) are made in the other games
// too (spawn_*, below); each such actor's key is the one its maker gave it, kept here.
#define KEYED_N 128
// nh: the actor's name hash when it was given its key: one with another name at that address is another actor.
static uint32_t name_hash_of(uint64_t a);
static struct { _Atomic uint64_t actor; _Atomic uint32_t key; uint32_t nh; } keyed[KEYED_N];
static uint32_t keyed_of(uint64_t a) {
    for (int i = 0; i < KEYED_N; i++)
        if (atomic_load_explicit(&keyed[i].actor, memory_order_acquire) == a)
            return keyed[i].nh == name_hash_of(a) ? atomic_load_explicit(&keyed[i].key, memory_order_relaxed) : 0;
    return 0;
}
static void keyed_set(uint64_t a, uint32_t key) {
    int f = -1;
    for (int i = 0; i < KEYED_N; i++) {
        uint64_t x = atomic_load(&keyed[i].actor);
        if (x == a) { f = i; break; }
        if (f < 0 && !x) f = i;
    }
    if (f < 0) { rt_log("coop: spawn sync: no room to key %08x", key); return; }
    atomic_store_explicit(&keyed[f].actor, 0, memory_order_release);
    keyed[f].nh = name_hash_of(a), atomic_store_explicit(&keyed[f].key, key, memory_order_relaxed);
    atomic_store_explicit(&keyed[f].actor, a, memory_order_release);
}
// Entries of actors gone (or another actor at the address) let go of, once a second (spawn_tick).
static void keyed_prune(void) {
    for (int i = 0; i < KEYED_N; i++) {
        uint64_t a = atomic_load(&keyed[i].actor);
        int32_t st = 0;
        if (a && (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || keyed[i].nh != name_hash_of(a)))
            atomic_store(&keyed[i].actor, 0);
    }
}
// The key of the actor of a cell (its name in nm).
static uint32_t cell_key(int cell, uint64_t a, const char *nm) {
    uint32_t kk = keyed_of(a);
    if (kk) return kk;
    if (!home_is(cell, a)) {
        if (!coop_rd(a + COOP_ACTOR_POS, seen[cell].home, 12)) return 0;
        atomic_store_explicit(&seen[cell].home_of, a, memory_order_release);
        atomic_store_explicit(&seen[cell].home_ok, 1, memory_order_release);
    }
    return actor_key(a, nm, seen[cell].home);
}
// An enemy this game updates, with its key (0 if not an enemy).
static uint32_t enemy_of(int cell, uint64_t a, char *nm, size_t n) {
    coop_actor_name(a, nm, n);
    return strncmp(nm, "Enemy_", 6) ? 0 : cell_key(cell, a, nm);
}
// An actor the sync takes, enemy or (TOTK_COOP_MOUNTS=1) horse or animal (coop_world_kind, coop_mounts.c), with its
// key; 0 none. The enemy model below runs them all: whose each is, the hand-over, the copies' easing and life.
static uint32_t synced_of(int cell, uint64_t a, char *nm, size_t n, int *kind) {
    coop_actor_name(a, nm, n);
    int k = coop_world_kind(nm);
    if (kind) *kind = k;
    return k ? cell_key(cell, a, nm) : 0;
}

// Two enemies of one name made in the same half metre have one key: one game put both on the other's one enemy, which
// kept one per key (code review, 2026-11-06). Such a key is left out of the sync in both games: counted first
// over the live enemies (dup_count), then skipped.
static struct { uint32_t key; int n; } dup_keys[512];
static int dup_n;
static float dist3(const float a[3], const float b[3]);
// Every live enemy here, synced or not (refreshed with dup_count): coop_enemy_within.
static uint64_t all_enemies[512];
static int n_all_enemies;
static void dup_one(int cell, uint64_t a, void *arg) {
    (void)arg;
    char nm[64];
    int kind = 0;
    uint32_t key = synced_of(cell, a, nm, sizeof nm, &kind);
    if (!key) return;
    if (kind == COOP_KIND_ENEMY && n_all_enemies < 512) all_enemies[n_all_enemies++] = a;
    for (int i = 0; i < dup_n; i++) if (dup_keys[i].key == key) { dup_keys[i].n++; return; }
    if (dup_n < 512) dup_keys[dup_n].key = key, dup_keys[dup_n++].n = 1;
}
static void dup_count(void) { dup_n = 0, n_all_enemies = 0, each_live(dup_one, NULL); }
// Whether a live enemy of this game (synced or not) stands within m of p: a weapon there is in its hand (or was), not
// a thing lying in the world (coop_players.c, prop_fn). Night's Stalkoblins are made in each game on its own and not
// synced; one's sword was sent as a moved object and its copy hung in the air in the other game (a play session,
// 2026-11-07).
int coop_enemy_within(const float p[3], float m) {
    for (int i = 0; i < n_all_enemies; i++) {
        float q[3];
        int32_t st = 0;
        if (!coop_rd(all_enemies[i] + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || !coop_rd(all_enemies[i] + COOP_ACTOR_POS, q, 12)) continue;
        if (dist3(p, q) < m) return 1;
    }
    return 0;
}
static int dup_key(uint32_t key) {
    for (int i = 0; i < dup_n; i++) if (dup_keys[i].key == key) return dup_keys[i].n > 1;
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
static uint64_t audit_skeleton(uint64_t a);
static int pup_ready_key(uint32_t key);
static int pup_bound_to(uint32_t key, uint64_t actor);
// The sync audit (coop_players.c, audit_tick): the enemies this game updates within SYNC_M of any Link, as a JSON
// array of key, place, life and whether it is dead (life 0 or going away).
typedef struct { FILE *f; float links[COOP_MAX_PLAYERS + 1][3]; int nl, n; } AuditScan;
static void audit_one(int cell, uint64_t a, void *arg) {
    AuditScan *as = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    float p[3];
    int32_t st = 0;
    if (!key || !coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_STATE, &st, 4)) return;
    int near = 0;
    for (int i = 0; i < as->nl && !near; i++) {
        float dx = p[0] - as->links[i][0], dz = p[2] - as->links[i][2];
        near = dx * dx + dz * dz < SYNC_M * SYNC_M;
    }
    if (!near) return;
    int life = life_of(a);
    // Its body's shape, to compare the pose across games: its highest bone above its root bone and the farthest any
    // bone is from the root sideways (the skeleton's final matrices +0x20, 0x40 a bone, translation at 12..14).
    float top = 0, reach = 0;
    uint64_t sk = audit_skeleton(a), wm = sk ? coop_rd_ptr(sk + 0x20) : 0;
    uint16_t nb = 0;
    static float w[400][16];
    if (wm && coop_rd(sk + 0x48, &nb, 2) && nb > 0 && nb <= 400 && coop_rd(wm, w, (size_t)nb * 0x40))
        for (int b = 1; b < nb; b++) {
            float dy = w[b][13] - w[0][13], dx = w[b][12] - w[0][12], dz = w[b][14] - w[0][14], r = sqrtf(dx * dx + dz * dz);
            if (dy > top) top = dy;
            if (r > reach) reach = r;
        }
    fprintf(as->f, "%s{\"key\":%u,\"name\":\"%s\",\"life\":%d,\"dead\":%d,\"dup\":%d,\"p\":[%.3f,%.3f,%.3f],\"top\":%.3f,\"reach\":%.3f,\"pup\":%d}",
            as->n++ ? "," : "", key, nm, life, life == 0 || st > 6, dup_key(key), p[0], p[1], p[2], top, reach, pup_ready_key(key));
}
void coop_audit_enemies(FILE *f, uint64_t me) {
    static AuditScan as;
    as.f = f, as.n = 0;
    as.nl = coop_player_positions(as.links, COOP_MAX_PLAYERS);
    if (me && coop_rd(me + COOP_ACTOR_POS, as.links[as.nl], 12)) as.nl++;
    dup_count();
    fputc('[', f);
    each_live(audit_one, &as);
    fputc(']', f);
}

// ---- enemies shared between the games
// Each synced enemy is run by one game, its owner: the game that has had it longest (the time since this game first
// saw it run, sent as age_ms; within 2 s of each other, the lower player id). So a player who was fighting alone keeps
// the fight as it was when the others arrive (the tester's call, 2026-11-06: "whoever was there"). The other games freeze
// their own one and show the owner's place and body pose (enemy puppets, below); without poses (more enemies than
// EPOSE_MAX near), they put it at the owner's place when it drifts. A game keeps following an owner while the owner
// reports the enemy; when it no longer does (that player left the area, so its game let the enemy go), the game runs
// its own from where it stands, and so becomes the owner for the others.
// Every game sends about 10 times a second the enemies it runs within SYNC_M of another player's Link, and those it
// saw die: the last 5 s always, older ones of this session when another Link is within SYNC_M of where it stood, so
// an enemy killed while alone stays dead when the others come. A game's hit on an enemy it does not run goes to the
// owner (a HIT packet addressed to it), which takes the damage from its own.
typedef struct { uint32_t key; uint16_t life, flags; float pos[3], rot[9]; uint32_t age_ms; } NetEnemy;   // flags bit 0: dead
_Static_assert(sizeof(NetEnemy) == 60, "NetEnemy wire size");
#define MAX_SENT 28
#define MAX_DEAD_SENT 10
#define CLAIM_TIE_MS 2000
typedef struct { uint16_t count, reserved; NetEnemy e[MAX_SENT]; } WorldPacket;
typedef struct { uint32_t key; int32_t damage; uint32_t to; } HitPacket;   // to: the owner's player id
// An enemy's hit on a player's proxy here, for that player (to): the enemy's key and the damage (quarter hearts).
// Told from a HitPacket by its size.
// Then nslots LinkHitSlot: the 8-byte slots of the hit's record (damage component +0x1a0..+0x300) the hit changed,
// with how a pointer is moved into the other game (how).
enum { LH_RAW, LH_DC, LH_ACTOR, LH_ATTACKER };
typedef struct { uint8_t slot, how, pad[6]; uint64_t v; } LinkHitSlot;
#define LH_SLOTS (0x160 / 8)
// info: the hit as the game delivered it to the proxy (main+0x31dbc0's third argument, HIT_INFO bytes; coop_hit_delivered),
// info_len 0 when none was seen.
#define HIT_INFO 0xe8
typedef struct {
    uint32_t key; int32_t damage; uint32_t to, kind, flags, nslots, info_len;
    uint32_t atk_off;    // where the hit's attack (main+0x31dbc0's x1) sits in the proxy's damage component, 0 elsewhere
    uint8_t info[HIT_INFO];
    LinkHitSlot s[LH_SLOTS];
} LinkHitPacket;
_Static_assert(sizeof(LinkHitPacket) <= COOP_HIT_MAX, "LinkHitPacket fits a HIT packet");
#define LINKHIT_KIND 0x4c48   // "HL"

// The enemies this game runs, as last seen (for the hits on them, their deaths and their poses); d2 the square of the
// distance to the nearest other Link.
#define KNOWN_N 512
static struct { uint32_t key; uint64_t actor; int life; int64_t at_us; float home[3], d2; } mine_known[KNOWN_N];
// Enemies seen to die here this session, with where they first stood (an entry is dropped when this game runs that
// enemy alive again: a blood moon brought it back).
#define KILLED_N 512
static struct { uint32_t key; float home[3]; int64_t died_us; } killed[KILLED_N];
// Whether p is within m of a live enemy this game runs that another Link may see (mine_known): an arrow made there is
// that enemy's shot (coop_players.c, arrow_track_fn).
int coop_near_my_enemy(const float p[3], float m) {
    for (int i = 0; i < KNOWN_N; i++) {
        if (!mine_known[i].key || !mine_known[i].actor || mine_known[i].life <= 0) continue;
        float q[3];
        if (!coop_rd(mine_known[i].actor + COOP_ACTOR_POS, q, 12)) continue;
        float d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
        if (d2 < m * m) return 1;
    }
    return 0;
}
static void killed_add(uint32_t key, const float home[3]) {
    int f = 0;
    for (int i = 0; i < KILLED_N; i++) {
        if (killed[i].key == key) { f = i; break; }
        if (killed[i].died_us < killed[f].died_us) f = i;   // free (0) or the oldest
    }
    killed[f].key = key, memcpy(killed[f].home, home, 12), killed[f].died_us = frame_step_world_us();
}
// Per enemy this game does not run: whose it is (player id; 0 this game's), and the life last set or seen here.
// handed: the player this game handed it to (handover_to), until their game says it runs it (or HANDOVER_WAIT_US
// passes); moved_us: when its owner last changed here.
typedef struct { uint32_t key; int owner, last, handed; int64_t at_us, handed_us, moved_us; } Follow;
#define FOLLOW_N 512
static Follow follow[FOLLOW_N];
static Follow *follow_of(uint32_t key) {
    int free_i = -1;
    int64_t now = frame_step_world_us();
    for (int i = 0; i < FOLLOW_N; i++) {
        if (follow[i].key == key) { follow[i].at_us = now; return &follow[i]; }
        if (free_i < 0 && (!follow[i].key || now - follow[i].at_us > 30000000)) free_i = i;
    }
    if (free_i < 0) free_i = (int)(key % FOLLOW_N);
    follow[free_i].key = key, follow[free_i].owner = 0, follow[free_i].last = -1, follow[free_i].at_us = now;
    follow[free_i].handed = 0, follow[free_i].handed_us = 0, follow[free_i].moved_us = now;
    return &follow[free_i];
}
// Each other player's latest world packet.
static WorldPacket remote_pk[COOP_MAX_PLAYERS + 1];
static int remote_ok[COOP_MAX_PLAYERS + 1];

static int enemy_puppets_on(void) {
    static int on = -1;   // TOTK_COOP_ENEMY_PUPPET=0: no frozen copies, only put at the owner's place (control)
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_ENEMY_PUPPET"); on = !(e && *e == '0'); }
    return on;
}
static void pup_bind(uint32_t key, int owner, uint64_t actor);
static void pup_release(uint32_t key);
static int enemy_native(void);
static int pup_ready_key(uint32_t key);

typedef struct {
    Cpu *c; WorldPacket pk; float others[COOP_MAX_PLAYERS][3], me[3];
    int ids[COOP_MAX_PLAYERS], no, me_id, have_me, live_max, snaps, puppets, mounts_sent;
    uint64_t now_ns;
} PeerScan;
// Horses and animals take at most this many of a world packet's MAX_SENT entries (a ridden horse always goes), so
// the enemies keep their room.
#define MOUNTS_SENT_MAX 8
// The hand-over (TOTK_COOP_ENEMY_HANDOVER, on unless 0): an enemy this game runs goes to the game of another player
// whose Link is HANDOVER_GAIN_M nearer to it than this game's, within HANDOVER_M of it, and at most once in
// HANDOVER_HOLD_US. An enemy fights the one player of the game that runs it: a second Player actor there (the other
// player's co-op Link) is never noticed, nor an actor of our own made from the Player's parameters, or from a pigeon's
// with the Player's or a Sage's perception emitter and affiliation, put on the co-op Link each tick (a Bokoblin next to
// one stayed unaware; docs/coop/proxy-plan.md, 2026-11-07). So the enemy is run
// where the player it should fight is, as TotK Online does (its server gives each actor to the nearest player's game;
// read for behaviour only). The player who was there keeps the fight while their Link is about as near as the other.
#define HANDOVER_M 30.0f
#define HANDOVER_GAIN_M 5.0f
#define HANDOVER_HOLD_US 5000000
#define HANDOVER_WAIT_US 3000000
#define HANDOVER_CLAIM_M 3.0f   // (between two games' claims: smaller than HANDOVER_GAIN_M, so no hand-over undoes it)
static float dist3(const float a[3], const float b[3]) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
static int handover_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_ENEMY_HANDOVER"); on = !(e && *e == '0'); }
    return on;
}
// The player id of the other Link this enemy at p should go to, 0 when it stays here.
static int handover_to(const PeerScan *s, const float p[3], float *mine_m, float *theirs_m) {
    if (!s->have_me || !handover_on()) return 0;
    float dm = sqrtf((p[0] - s->me[0]) * (p[0] - s->me[0]) + (p[1] - s->me[1]) * (p[1] - s->me[1]) + (p[2] - s->me[2]) * (p[2] - s->me[2]));
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < s->no; i++) {
        const float *o = s->others[i];
        float d = sqrtf((p[0] - o[0]) * (p[0] - o[0]) + (p[1] - o[1]) * (p[1] - o[1]) + (p[2] - o[2]) * (p[2] - o[2]));
        if (d < bd) bd = d, best = s->ids[i];
    }
    *mine_m = dm, *theirs_m = bd;
    return best && bd < HANDOVER_M && bd + HANDOVER_GAIN_M < dm ? best : 0;
}
static int near_other(const PeerScan *s, const float p[3], float *d2_out) {
    float best = 1e30f;
    for (int i = 0; i < s->no; i++) {
        float dx = p[0] - s->others[i][0], dz = p[2] - s->others[i][2], d2 = dx * dx + dz * dz;
        if (d2 < best) best = d2;
    }
    if (d2_out) *d2_out = best;
    return best < SYNC_M * SYNC_M;
}
static void peer_one(int cell, uint64_t a, void *arg) {
    PeerScan *s = arg;
    char nm[64];
    int kind = 0;
    uint32_t key = synced_of(cell, a, nm, sizeof nm, &kind);
    const char *kw = coop_world_kind_word(kind);
    if (!key || dup_key(key)) return;
    // Listed for a second after its last update, it may be gone (and its memory another's): written to only while
    // alive (code review, 2026-11-06).
    int32_t st = 0;
    if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2) return;
    int life = life_of(a);
    if (life < 0) return;   // (no life yet, just made: sent as life 0 the others set theirs to 0; code review, 2026-11-06)
    uint32_t my_age = (uint32_t)((s->now_ns - seen[cell].first_ns) / 1000000ull);
    // The other games' word on it: dead there, or who runs it and for how long.
    int dead_from = 0, n_claims = 0, claim_id[COOP_MAX_PLAYERS];
    const NetEnemy *claim[COOP_MAX_PLAYERS];
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (!remote_ok[id]) continue;
        for (int i = 0; i < remote_pk[id].count; i++) {
            const NetEnemy *e = &remote_pk[id].e[i];
            if (e->key != key) continue;
            if (e->flags & 1) dead_from = id;
            else if (n_claims < COOP_MAX_PLAYERS) claim_id[n_claims] = id, claim[n_claims++] = e;
            break;
        }
    }
    Follow *f = follow_of(key);
    if (dead_from) {   // dead there: life 0 here, and the game's own death follows (it runs its own update again)
        if (life > 0 && st <= 6) {
            set_life(a, 0);
            rt_log("coop: %s %s %08x died in player %d's game", kw, nm, key, dead_from);
        }
        f->owner = 0, f->last = 0;
        pup_release(key);
        return;
    }
    if (st > 6) return;
    // Whose it is: kept with the owner it has while that one still reports it; else the longest-held claim.
    int keep = 0;
    for (int i = 0; i < n_claims && f->owner; i++) keep |= claim_id[i] == f->owner;
    int64_t wnow = frame_step_world_us();
    // A ridden horse is its rider's game's (coop_mounts.c), whoever had it, and not handed over while ridden nor in the
    // HANDOVER_HOLD_US after.
    int rider = kind == COOP_KIND_HORSE ? coop_mount_rider(key, s->me_id) : 0;
    if (rider) {
        int want = rider == s->me_id ? 0 : rider;
        if (f->owner != want || f->handed)
            rt_log("coop: %s %s %08x is ridden by player %d: run %s", kw, nm, key, rider, want ? "by their game" : "here");
        if (f->owner != want && want) f->last = -1;
        f->owner = want, f->handed = 0, f->moved_us = wnow;
        keep = 1;
    }
    if (keep && f->handed == f->owner) f->handed = 0;   // they run it now
    // Handed to another game a moment ago: theirs while that game takes it up.
    if (!keep && f->handed && f->owner == f->handed && wnow - f->handed_us < HANDOVER_WAIT_US) keep = 1;
    if (!keep && f->handed) {
        rt_log("coop: %s %s %08x: player %d did not take it up", kw, nm, key, f->handed);
        f->handed = 0;
        f->moved_us = wnow + 25000000;   // (their game may not have it: not offered again for 30 s)
    }
    if (!keep) {
        int best = s->me_id;
        uint32_t best_age = my_age;
        for (int i = 0; i < n_claims; i++) {
            uint32_t ag = claim[i]->age_ms;
            int older = ag > best_age + CLAIM_TIE_MS, tie = !older && best_age <= ag + CLAIM_TIE_MS;
            if (older || (tie && claim_id[i] < best)) best = claim_id[i], best_age = ag;
        }
        // Claimed by more than one game: the one whose Link is HANDOVER_CLAIM_M nearer to it than every other's (within
        // HANDOVER_M) keeps it, before the longest-held. A game whose Link was 30 m off let its enemies go and took
        // them back as they went in and out of the part of the world it updates; each time the nearer player's game
        // went from its own one, fighting, to the far game's, which had stood still: one stayed knocked down and could
        // not be locked on to for a while (a play session, 2026-11-07).
        float ep[3];
        if (n_claims && handover_on() && s->have_me && coop_rd(a + COOP_ACTOR_POS, ep, 12)) {
            int ids[COOP_MAX_PLAYERS + 1], nid = 0, near_id = 0;
            float d[COOP_MAX_PLAYERS + 1], dn = 1e30f, d2nd = 1e30f;
            ids[nid] = s->me_id, d[nid++] = dist3(ep, s->me);
            for (int i = 0; i < n_claims; i++)
                for (int k = 0; k < s->no; k++)
                    if (s->ids[k] == claim_id[i] && nid <= COOP_MAX_PLAYERS) ids[nid] = claim_id[i], d[nid++] = dist3(ep, s->others[k]);
            for (int i = 0; i < nid; i++) {
                if (d[i] < dn) d2nd = dn, dn = d[i], near_id = ids[i];
                else if (d[i] < d2nd) d2nd = d[i];
            }
            if (near_id && dn < HANDOVER_M && dn + HANDOVER_CLAIM_M < d2nd) best = near_id;
        }
        int was = f->owner;
        f->owner = best == s->me_id ? 0 : best;
        if (f->owner != was && f->moved_us < wnow) f->moved_us = wnow;
        if (f->owner != was)
            rt_log("coop: %s %s %08x is %s (held here %.1f s%s)", kw, nm, key, f->owner ? "run by another game" : "run here",
                   my_age / 1e3, f->owner ? "" : was ? "; its owner let it go" : "");
        if (f->owner) f->last = -1;
    }
    if (!rider && !f->owner && life > 0 && wnow - f->moved_us > HANDOVER_HOLD_US) {   // this game's: to a nearer player's?
        float p[3], dm = 0, dt = 0;
        int to = coop_rd(a + COOP_ACTOR_POS, p, 12) ? handover_to(s, p, &dm, &dt) : 0;
        if (to) {
            f->owner = f->handed = to, f->handed_us = f->moved_us = wnow, f->last = -1;
            rt_log("coop: %s %s %08x handed to player %d (their Link %.1f m from it, mine %.1f m)", kw, nm, key, to, dt, dm);
        }
    }
    if (!f->owner) {   // this game's: reported while another Link is near
        pup_release(key);
        // Known whether near another Link or not: one killed while this player was alone is then reported dead to the
        // others when they come.
        float p[3], d2 = 1e30f;
        if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
        if (near_other(s, p, &d2) && s->pk.count < s->live_max &&
            (kind == COOP_KIND_ENEMY || rider || s->mounts_sent < MOUNTS_SENT_MAX)) {
            if (kind != COOP_KIND_ENEMY) s->mounts_sent++;
            NetEnemy e = {.key = key, .age_ms = my_age};
            memcpy(e.pos, p, 12);
            coop_rd(a + COOP_ACTOR_ROT, e.rot, 36);
            e.life = (uint16_t)(life > 65535 ? 65535 : life);
            if (life == 0) e.flags |= 1;
            s->pk.e[s->pk.count++] = e;
        }
        // Alive again here after it died here: brought back (a blood moon), no longer reported dead.
        if (life > 0)
            for (int i = 0; i < KILLED_N; i++)
                if (killed[i].key == key && frame_step_world_us() - killed[i].died_us > 10000000) killed[i].key = 0;
        int64_t now = frame_step_world_us();
        int free_i = -1;
        for (int i = 0; i < KNOWN_N; i++) {
            if (mine_known[i].key == key) { free_i = i; break; }
            if (free_i < 0 && !mine_known[i].key) free_i = i;
        }
        if (free_i >= 0) {
            mine_known[free_i].key = key, mine_known[free_i].actor = a, mine_known[free_i].life = life;
            mine_known[free_i].at_us = now, mine_known[free_i].d2 = d2;
            if (home_is(cell, a)) memcpy(mine_known[free_i].home, seen[cell].home, 12);
        }
        return;
    }
    // Another game's.
    const NetEnemy *e = NULL;
    for (int i = 0; i < n_claims; i++) if (claim_id[i] == f->owner) e = claim[i];
    if (!e) return;
    // Test (TOTK_COOP_FAKEHIT=<n>): at 20 s and 30 s into the world, the first enemy another game runs loses n life here.
    static int fake = -1, fake_done;
    if (fake < 0) { const char *fk = getenv("TOTK_COOP_FAKEHIT"); fake = fk ? atoi(fk) : 0; }
    int64_t w = frame_step_world_us();
    if (fake > 0 && fake_done < 2 && w > (fake_done ? 30000000 : 20000000) && life > 0) {
        set_life(a, life - fake), life = life - fake < 0 ? 0 : life - fake, fake_done++;
        rt_log("coop: test: enemy %s %08x loses %d here", nm, key, fake);
    }
    // This game's hits: life lost since the owner's value was last set here goes to the owner.
    if (f->last >= 0 && life < f->last) {
        HitPacket hp = {key, f->last - life, (uint32_t)f->owner};
        coop_net_hit_send(&hp, sizeof hp);
        rt_log("coop: hit %s %s %08x for %d, sent to player %d", kw, nm, key, hp.damage, f->owner);
    }
    if (life != (int)e->life && (f->last < 0 || life >= f->last)) set_life(a, e->life), life = e->life;
    f->last = life;
    // Not made a frozen copy while this game is in a load (or a menu), nor in its first 2 s after it, nor before this
    // game has seen it run 3 s: player 2's game warped to where player 1 stood, froze three of player 1's enemies at
    // once (held here 1.3 s, its clock standing) and stayed on the loading screen for minutes (a play session,
    // 2026-11-07; that the freeze held the load up is a guess). Meanwhile it runs its own update, put at the owner's
    // place when it drifts.
    int coop_runs_for_ms(void);   // coop_players.c
    if (pup_bound_to(key, a) || (coop_runs_for_ms() >= 2000 && my_age >= 3000)) pup_bind(key, f->owner, a);
    if (enemy_puppets_on() && pup_ready_key(key)) { s->puppets++; return; }   // posed from the owner's (enemy puppets)
    if (enemy_native() && pup_bound_to(key, a)) { s->puppets++; return; }    // eased to the owner's place (native_follow)
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float dx = p[0] - e->pos[0], dy = p[1] - e->pos[1], dz = p[2] - e->pos[2];
    if (dx * dx + dy * dy + dz * dz > SNAP_M * SNAP_M) {
        float m[12];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) m[r * 4 + k] = e->rot[r * 3 + k];
            m[r * 4 + 3] = e->pos[r];
        }
        coop_guest_call(s->c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, a, (uint64_t)(uintptr_t)m, 0, 0);
        s->snaps++;
    }
}

static void epose_send_tick(void);
static void epose_take_tick(Cpu *c);
static uint64_t pup_me;
static void enemy_weapon_test(void);
static void enemy_hit_test(void);

// The Weapon_ actors this game updates that are no Link's (an enemy's weapon), refreshed with the enemy scan.
static uint64_t weaps[128];
static int nweaps;
static void weap_collect(int cell, uint64_t a, void *arg) {
    (void)cell, (void)arg;
    char nm[48];
    int coop_is_players_item(uint64_t a);   // coop_players.c
    if (nweaps < 128 && !strncmp(coop_actor_name(a, nm, sizeof nm), "Weapon_", 7) && !coop_is_players_item(a)) weaps[nweaps++] = a;
}
// The key of an enemy this game updates (0: not one, or not seen).
static uint32_t key_of_actor(uint64_t actor) {
    uint32_t h = (uint32_t)((actor >> 4) * 2654435761u) & (SEEN_N - 1);
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1))
        if (atomic_load_explicit(&seen[h].actor, memory_order_relaxed) == actor) {
            char nm[64];
            return synced_of((int)h, actor, nm, sizeof nm, NULL);
        }
    return 0;
}
// An enemy's hit on a player's proxy here goes to that player. The game's hit leaves a record in the proxy's damage
// component ([actor+0x228]+0x2f8, +0x1a0..; re-notes "The hit's mark on the device"): +0x1a4 nonzero the frame it
// lands, +0x1a8 the damage in quarter hearts (on this Link, 1 and 4 for a loss of 1 and 4: TOTK_COOP_TEST_LINKHIT,
// 2026-11-06), +0x1c0 the attacker's reference (+0x40 its actor). The proxy takes none of it (its life update does
// not run), and the enemy that hit it is frozen in its owner's game, so the owner's Link was never hurt (the same
// test: 14 hits on the proxy, its owner's life unchanged).
// main+0x31dbc0 (x0 a damage component's hit receiver, +0x190; x1 the attack; x2 the hit: +0 the damage, +0x14 its kind
// (the record's state is 1 << kind), +0x20 a handle on the attacker (+8 its reference, +0x10 that reference's serial),
// +0x38.. directions, +0xe0 flags) writes a hit into the receiver: the game's own delivery, found by a write watch on
// a Link's hit state (TOTK_COOP_TEST_HITWATCH, 2026-11-06), called from the hit functions such as main+0x1cd497c.
// When the receiver is a proxy's, the hit is kept here for link_hits_send to send to that proxy's player.
#define RCV_N COOP_MAX_PLAYERS
static _Atomic uint64_t proxy_rcv[RCV_N];   // the proxies' receivers, set by link_hits_send
static _Atomic int proxy_id[RCV_N];         // their players
static struct { _Atomic uint32_t seq; uint8_t info[HIT_INFO]; uint64_t atk, dc; } rcv_hit[RCV_N];
void coop_hit_delivered(Cpu *c) {
    uint64_t r = c->x[0], h = c->x[2];
    if (!r || !h) return;
    for (int i = 0; i < RCV_N; i++)
        if (atomic_load_explicit(&proxy_rcv[i], memory_order_relaxed) == r) {
            // the hit functions call it up to three times a hit; the one with damage and a kind (12 none) is the hit
            int32_t d[6];
            static int chain_on = -1;   // Test (TOTK_COOP_TEST_HITCHAIN=1): the guest call chain of the hit, 8 times
            static _Atomic int chain_said;
            if (chain_on < 0) chain_on = cfg_get("TOTK_COOP_TEST_HITCHAIN") != NULL;
            if (chain_on && coop_rd(h, d, sizeof d) && d[0] > 0 && d[5] != 12 && atomic_fetch_add(&chain_said, 1) < 8) {
                char chain[400];
                int m = snprintf(chain, sizeof chain, "coop: test: hitchain lr main+%llx, frames", (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
                uint64_t fp = c->x[29];
                for (int k = 0; k < 16 && fp && m < 380; k++) {
                    uint64_t next = coop_rd_ptr(fp), ret = coop_rd_ptr(fp + 8);
                    if (!ret) break;
                    m += snprintf(chain + m, sizeof chain - m, " %llx", (unsigned long long)(ret - MOD_MAIN_BASE));
                    if (next <= fp) break;
                    fp = next;
                }
                rt_log("%s", chain);
            }
            if (coop_rd(h, d, sizeof d) && d[0] > 0 && d[5] != 12 && coop_rd(h, rcv_hit[i].info, HIT_INFO))
                rcv_hit[i].atk = c->x[1], rcv_hit[i].dc = r - 0x190, atomic_fetch_add(&rcv_hit[i].seq, 1);
            return;
        }
}
static int contact_on(void);
static int enemy_native(void);
// When a contact went to each player (plat_now_ns): a hit on that player's proxy within CONTACT_COVER_NS of one is
// the contact's, so its hit replay is not sent (link_hits_send); a hit with no contact (one the contact list does not
// carry) still goes as the hit replay.
#define CONTACT_COVER_NS 400000000ull
static _Atomic uint64_t contact_sent_ns[COOP_MAX_PLAYERS + 1];
static void link_hits_send(void) {
    if (enemy_native()) {   // none: the copy in the hit player's game hits that player itself (review, 2026-11-07)
        for (int i = 0; i < RCV_N; i++) atomic_store(&proxy_rcv[i], 0);
        return;
    }
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(acts, ids, COOP_MAX_PLAYERS);
    static uint32_t rcv_seen[RCV_N];
    for (int i = 0; i < RCV_N; i++) {
        // (none with the native enemy model: an enemy's hits on a proxy here are its copy's in that player's game)
        uint64_t dc = i < n && !enemy_native() ? coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x2f8) : 0;
        atomic_store(&proxy_rcv[i], dc ? dc + 0x190 : 0);
        atomic_store(&proxy_id[i], i < n ? ids[i] : 0);
    }
    static uint64_t last_actor[COOP_MAX_PLAYERS];
    static uint32_t last_state[COOP_MAX_PLAYERS];
    static uint64_t idle[COOP_MAX_PLAYERS][LH_SLOTS];   // the record as it stood before the hit
    for (int i = 0; i < n; i++) {
        uint64_t dc = coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x2f8);
        uint64_t rs[LH_SLOTS];
        if (!dc || !coop_rd(dc + 0x1a0, rs, sizeof rs)) continue;
        uint32_t w[0x28 / 4];
        memcpy(w, rs, sizeof w);
        uint32_t st = w[1], was = last_actor[i] == acts[i] ? last_state[i] : st;
        int fresh = last_actor[i] != acts[i];
        last_actor[i] = acts[i], last_state[i] = st;
        if (!st) { memcpy(idle[i], rs, sizeof rs); rcv_seen[i] = atomic_load(&rcv_hit[i].seq); continue; }
        if (st == was || fresh) continue;
        uint64_t ref;
        memcpy(&ref, &w[0x20 / 4], 8);
        uint64_t att = ref ? coop_rd_ptr(ref + 0x40) : 0;
        int32_t dmg = (int32_t)w[2];
        char nm[64] = "";
        if (att) coop_actor_name(att, nm, sizeof nm);
        uint32_t key = strncmp(nm, "Enemy_", 6) ? 0 : key_of_actor(att);
        static int said;
        if (said++ < 40)
            rt_log("coop: %s %08x hit player %d's Link here for %d%s", nm[0] ? nm : "something", key, ids[i], dmg,
                   key && dmg > 0 ? ", sent to them" : "");
        if (!key || dmg <= 0 || dmg > 4000) continue;
        if (contact_on() && ids[i] > 0 && ids[i] <= COOP_MAX_PLAYERS && plat_now_ns() - atomic_load(&contact_sent_ns[ids[i]]) < CONTACT_COVER_NS)
            continue;   // its contact went to them: their game makes the hit itself
        // The record's slots the hit changed, pointers made relative to the proxy's damage component or actor, or
        // marked as the attacker's reference, so the player's game can write the same hit into its own Link.
        static LinkHitPacket lh;
        memset(&lh, 0, sizeof lh);
        lh.key = key, lh.damage = dmg, lh.to = (uint32_t)ids[i], lh.kind = LINKHIT_KIND;
        memcpy(&lh.flags, (const uint8_t *)rs + 0x30, 4);   // the record's flags (+0x1d0; past w, which holds +0x1a0..+0x1c8)
        if (i < RCV_N && atomic_load(&rcv_hit[i].seq) != rcv_seen[i]) {
            memcpy(lh.info, rcv_hit[i].info, HIT_INFO), lh.info_len = HIT_INFO;
            uint64_t ao = rcv_hit[i].atk - rcv_hit[i].dc;
            lh.atk_off = rcv_hit[i].atk > rcv_hit[i].dc && ao < 0x2000 ? (uint32_t)ao : 0;
            rcv_seen[i] = atomic_load(&rcv_hit[i].seq);
            static int said_info;
            if (said_info++ < 4) {
                char line[1200];
                int m = snprintf(line, sizeof line, "coop: the hit as delivered to player %d's proxy:", ids[i]);
                for (int k = 0; k < HIT_INFO / 8 && m < 1160; k++) {
                    uint64_t v;
                    memcpy(&v, lh.info + 8 * k, 8);
                    m += snprintf(line + m, sizeof line - m, " %02x:%llx", 8 * k, (unsigned long long)v);
                }
                rt_log("%s", line);
            }
        }
        for (int k = 0; k < LH_SLOTS; k++) {
            if (rs[k] == idle[i][k]) continue;
            LinkHitSlot *e = &lh.s[lh.nslots++];
            uint64_t v = rs[k];
            e->slot = (uint8_t)k;
            if (v == ref) e->how = LH_ATTACKER, e->v = 0;
            else if (v >= dc && v < dc + 0x2000) e->how = LH_DC, e->v = v - dc;
            else if (v >= acts[i] && v < acts[i] + 0x3000) e->how = LH_ACTOR, e->v = v - acts[i];
            else e->how = LH_RAW, e->v = v;
        }
        coop_net_hit_send(&lh, (int)(offsetof(LinkHitPacket, s) + lh.nslots * sizeof(LinkHitSlot)));
    }
}
static uint64_t pup_ready_actor(uint32_t key);
// An actor's reference object (+0x40 the actor), from a pointer in the actor's first 0x1000 bytes: what a hit record
// names its attacker by. 0 none found.
static uint64_t actor_ref(uint64_t a) {
    static uint64_t w[0x1000 / 8];
    if (!a || !coop_rd(a, w, sizeof w)) return 0;
    for (int i = 0; i < 0x1000 / 8; i++)
        if (w[i] > 0x100000000ull && w[i] < 0x8000000000ull && coop_rd_ptr(w[i] + 0x40) == a) return w[i];
    return 0;
}
// A hit written into this Link's record waits to be taken by its own update; if its life has not gone down by then,
// the damage is taken off it directly.
static struct { int frames, life, damage; uint32_t key; } lh_wait;
static _Atomic int lh_guarded;   // (defined with lh_dc below)
static void link_hit_check(uint64_t me) {
    if (!lh_wait.frames || --lh_wait.frames) return;
    if (atomic_exchange(&lh_guarded, 0)) {   // blocked: nothing to take
        rt_log("coop: the hit by enemy %08x was guarded by this Link (life %d)", lh_wait.key, life_of(me));
        return;
    }
    void coop_hit_drop(void);
    coop_hit_drop();   // a hit still waiting for this Link's update is not delivered after the damage was taken here
    int life = life_of(me);
    // (only an unchanged life: one gone up was a fairy or a meal, not a hit left untaken; 2 -> 20 by a fairy, then
    // 3 more taken here, 2026-11-06)
    if (life == lh_wait.life && life > 0) {
        int now = life - lh_wait.damage < 0 ? 0 : life - lh_wait.damage;
        set_life(me, now);
        rt_log("coop: the hit by enemy %08x was not taken from its record here: life %d -> %d directly", lh_wait.key, life, now);
    } else rt_log("coop: the hit by enemy %08x was taken by this Link (life %d -> %d)", lh_wait.key, lh_wait.life, life);
}
// A hit on this Link's proxy in another game, by an enemy that game runs: taken here, when this game shows that
// enemy as that game's (it follows it: its own one does not attack). The life goes down by the damage; no knock-back
// or flinch. TOTK_COOP_LINKHIT_RECORD=1 (a test) writes the hit's record into this Link's damage component instead,
// with this game's frozen copy of the enemy as the attacker: its update did not take it (life unchanged 12 frames
// later, twice; 2026-11-06), as a fan given another's record (re-notes): the hit goes somewhere else too. The damage
// is then taken off directly.
// The hit delivered here as the game delivers one (main+0x31dbc0 on this Link's receiver), from the one the owner's
// game delivered to this Link's proxy, the attacker handle cleared (TOTK_COOP_LINKHIT_ATTACKER=1: this game's frozen
// copy of the enemy). The attack (x1, which this game does not have) is read only by the guard check (main+0x1cceaac:
// the listener's vtable +0xe0, then the attack's +0x70 through main+0x1979130 and main+0x1cb6558 to a "guarded" byte);
// a blank one whose +0x70 and +0x30 point at itself, with the stop bits those two loops test (+0x4a, +0x183), reads as
// not guarded. (Flag +0xe0 bit 3 is that check's result, a guarded hit: set, the Link blocked every hit; 2026-11-06.)
// This Link's update then takes it as its own: damage, knock-back, flinch.
// It is delivered from inside that update, on its thread, as main+0x300a30 (x0 the damage component: its hit update)
// begins (coop_hit_update): delivered from this tick, while the update ran on another thread, the two changed the
// receiver's lists at once and the update crashed on a broken object at +0x1238 (twice, 2026-11-06). 1 when queued.
static _Alignas(16) uint8_t lh_info[HIT_INFO], lh_atk[0x400];
static _Atomic uint64_t lh_dc;   // the damage component a hit waits for, 0 none
static _Atomic int lh_guarded;   // the last hit delivered was guarded (flag +0xe0 bit 3 after the delivery)
static _Atomic uint64_t lh_x1;   // the attack it is delivered with
// Contact replay (TOTK_COOP_LINKHIT_CONTACT=1, a test): the hit is built in the hit player's own game from the
// contact, as the game builds one. A body's hits are built in its damage component's hit update, main+0x300a30 (the
// call chain of a hit on a proxy: TOTK_COOP_TEST_HITCHAIN, 2026-11-06), from the contacts waiting in the list at
// [dc+0x1910]+0x50 (count +0x18, capacity +8, 0x1c0-byte records from +0x10; main+0x1cd0c80 runs main+0x1cd263c on
// each): the attack's power (+0xa0), its kind (+0x90), the attacker's handles (+0x60, +0x78 with +0x94 bit 0), and
// the receiving body's own state, through main+0x1cd1b1c, which can cancel the hit (damage 0): a raised shield. So
// the owner's game sends the contacts waiting on a proxy as its hit update begins, and the player's game adds them
// to its own Link's list as that Link's hit update begins; its own update builds the hit, guard and all.
#define CONTACT_KIND 0x4c43   // "CL"
#define CONTACT_SZ 0x1c0
#define CONTACT_MAX 2
// brel: where the contact's body (+0x58) sat from the proxy's main body (enemy_body), plus 1; 0 not near it (a Link's
// is that body +0xb558, guarding or not, both Links, every run: TOTK_COOP_TEST_CONTACTS, 2026-11-06).
typedef struct { uint32_t key, to, kind, n, brel[CONTACT_MAX]; uint8_t e[CONTACT_MAX][CONTACT_SZ]; } ContactPacket;
_Static_assert(sizeof(ContactPacket) <= COOP_HIT_MAX, "ContactPacket fits a HIT packet");
// TOTK_COOP_LINKHIT_CONTACT=0: the hit replay only. Off with the native enemy model unless set: there the copy of an
// enemy fighting this game's Link hits it itself, in this game (sent as well, a hit counted twice).
static int contact_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_LINKHIT_CONTACT"); on = e ? *e != '0' : !enemy_native(); }
    return on;
}
static uint64_t contact_list(uint64_t dc) {
    uint64_t o = coop_rd_ptr(dc + 0x1910);
    return o ? coop_rd_ptr(o + 0x50) : 0;
}
static uint32_t key_of_actor(uint64_t a);
static uint64_t enemy_body(uint64_t a);
static uint64_t pup_ready_actor(uint32_t key);
static uint64_t actor_ref(uint64_t a);
static void contact_send(uint64_t dc, int to) {
    uint64_t l = contact_list(dc);
    int32_t cnt = 0, cap = 0;
    uint64_t base = 0;
    if (!l || !coop_rd(l + 0x18, &cnt, 4) || cnt <= 0 || !coop_rd(l + 8, &cap, 4) || !(base = coop_rd_ptr(l + 0x10))) return;
    static ContactPacket cp;   // (one proxy's hit update at a time: the update runs them in turn)
    static uint32_t last_id[RCV_N];
    memset(&cp, 0, offsetof(ContactPacket, e));
    cp.to = (uint32_t)to, cp.kind = CONTACT_KIND;
    for (int k = 0; k < cnt && cp.n < CONTACT_MAX; k++) {
        uint8_t *e = cp.e[cp.n];
        if (!coop_rd(base + (uint64_t)(k < cap ? k : 0) * CONTACT_SZ, e, CONTACT_SZ)) break;
        uint32_t id;
        memcpy(&id, e + 0x38, 4);
        // the attacker: either handle (+0x60 the weapon and +0x78 its holder, or +0x60 the enemy itself)
        uint64_t att = 0;
        char nm[64] = "";
        for (int hk = 0; hk < 2 && strncmp(nm, "Enemy_", 6); hk++) {
            uint64_t ref;
            memcpy(&ref, e + (hk ? 0x80 : 0x68), 8);
            att = ref ? coop_rd_ptr(ref + 0x40) : 0;
            nm[0] = 0;
            if (att) coop_actor_name(att, nm, sizeof nm);
        }
        static int said;
        if (said++ < 3) {
            char line[1400];
            int m = snprintf(line, sizeof line, "coop: test: contact on player %d's proxy by %s:", to, nm[0] ? nm : "?");
            for (int w = 0; w < CONTACT_SZ / 8 && m < 1360; w++) {
                uint64_t v;
                memcpy(&v, e + 8 * w, 8);
                if (v) m += snprintf(line + m, sizeof line - m, " %02x:%llx", 8 * w, (unsigned long long)v);
            }
            rt_log("%s", line);
        }
        if (strncmp(nm, "Enemy_", 6)) continue;   // an enemy's, this game's to run
        int slot = to > 0 && to <= RCV_N ? to - 1 : 0;
        if (id && id == last_id[slot]) continue;   // sent already (a contact waits there over several frames)
        last_id[slot] = id;
        cp.key = key_of_actor(att);
        uint64_t body;
        memcpy(&body, e + 0x58, 8);
        uint64_t mb0 = enemy_body(coop_rd_ptr(dc + 0x198));
        cp.brel[cp.n] = body && mb0 && body >= mb0 && body - mb0 < 0x20000 ? (uint32_t)(body - mb0) + 1 : 0;
        cp.n++;
    }
    if (cp.n) {
        coop_net_hit_send(&cp, (int)(offsetof(ContactPacket, e) + cp.n * CONTACT_SZ));
        if (to > 0 && to <= COOP_MAX_PLAYERS) atomic_store(&contact_sent_ns[to], plat_now_ns());
    }
}
static _Alignas(16) uint8_t ct_in[CONTACT_MAX][CONTACT_SZ];
static _Atomic int ct_n;                  // contacts waiting for this Link's hit update
static uint32_t ct_brel[CONTACT_MAX], ct_key;
static _Atomic uint64_t ct_dc;            // this Link's damage component they wait for
// This Link's guard. Its component +0x3b8's word +0x160 has bit 28 while ZL is held and bit 22 with the sword
// drawn (0x10000401 ZL with the sword on the back and no enemy, 0x400401 drawn, 0x10400401 both), +0x84 of its
// component +0x158 is 0x41 while ZL is held, and the actor's +0x328 has bit 1 only with both, the shield raised
// (0x32e00000 -> 0x32e00002): game B's own Link in the three states (TOTK_COOP_TEST_LINKDUMP, 2026-11-07).
static int shield_raised(uint64_t actor) {
    uint64_t tab = coop_rd_ptr(actor + 0x228), c1 = tab ? coop_rd_ptr(tab + 0x3b8) : 0;
    uint32_t f = 0, a = 0;
    if (!c1 || !coop_rd(c1 + 0x160, &f, 4) || !coop_rd(actor + 0x328, &a, 4)) return 0;
    return (f >> 28 & 1) && (f >> 22 & 1) && (a >> 1 & 1);
}
static const char *guard_state(uint64_t actor, char *buf, size_t n) {
    uint64_t tab = coop_rd_ptr(actor + 0x228), c1 = tab ? coop_rd_ptr(tab + 0x3b8) : 0, c2 = tab ? coop_rd_ptr(tab + 0x158) : 0;
    uint64_t eq = tab ? coop_rd_ptr(tab + 0x230) : 0;
    uint32_t f = 0, act = 0, ef = 0;
    if (c1) coop_rd(c1 + 0x160, &f, 4);
    if (c2) coop_rd(c2 + 0x84, &act, 4);
    if (eq) coop_rd(eq + 0x4600, &ef, 4);
    uint32_t a328 = 0;
    coop_rd(actor + 0x328, &a328, 4);
    snprintf(buf, n, "shield %s (%#x, actor +0x328 %#x, action %#x, equipment +0x4600 %#x)", shield_raised(actor) ? "raised" : "not raised",
             f, a328, act, ef);
    return buf;
}
static void contact_add(uint64_t dc) {
    uint64_t me_actor = coop_rd_ptr(dc + 0x198);
    int n = atomic_exchange(&ct_n, 0);
    if (!n) return;
    uint64_t l = contact_list(dc);
    int32_t cnt = 0, cap = 0;
    uint64_t base = 0;
    if (!l || !coop_rd(l + 0x18, &cnt, 4) || !coop_rd(l + 8, &cap, 4) || !(base = coop_rd_ptr(l + 0x10))) {
        rt_log("coop: test: contact: this Link has no contact list (%llx)", (unsigned long long)l);
        return;
    }
    for (int k = 0; k < n && cnt < cap; k++, cnt++) {
        uint8_t *e = ct_in[k];
        // the attacker's handles (+0x60, +0x78: +8 the reference, +0x10 its serial): this game's frozen copy of the
        // enemy for each one that was set (its weapon's as well, for now), none without a copy
        static int att_on = -1;   // TOTK_COOP_CONTACT_ATTACKER=0: both left empty
        if (att_on < 0) { const char *ev = cfg_get("TOTK_COOP_CONTACT_ATTACKER"); att_on = !(ev && *ev == '0'); }
        uint64_t cref = att_on ? actor_ref(pup_ready_actor(ct_key)) : 0;
        uint32_t cser = 0xffffffffu;
        if (cref) coop_rd(cref + 0x48, &cser, 4);
        for (int hk = 0; hk < 2; hk++) {
            uint64_t was, r = cref;
            uint32_t sr = cser;
            memcpy(&was, e + (hk ? 0x80 : 0x68), 8);
            if (!was) r = 0, sr = 0xffffffffu;
            memcpy(e + (hk ? 0x80 : 0x68), &r, 8), memcpy(e + (hk ? 0x88 : 0x70), &sr, 4);
        }
        // the body touched: the same place from this Link's main body, else none
        uint64_t mb = ct_brel[k] && enemy_body(me_actor) ? enemy_body(me_actor) + ct_brel[k] - 1 : 0;
        memcpy(e + 0x58, &mb, 8);
        // Test (TOTK_COOP_CONTACT_SHIELD=1): mark the contact as one that met the shield (+0x0b 1: the only byte apart
        // from places and directions between a guarding Link's blocked contact and its proxy's, same swing, 2026-11-06)
        static int sh = -1;
        if (sh < 0) { const char *ev = cfg_get("TOTK_COOP_CONTACT_SHIELD"); sh = ev && *ev == '1'; }
        if (sh) e[0x0b] = 1;
        // The block (TOTK_COOP_CONTACT_GUARDBIT=1, a test): the Player's hit filter cancels a hit when its equipment
        // component's +0x4600 has bit 17 (main+0x319120 from main+0x1cca59c), which the game sets when an attack
        // meets the raised shield; a replayed contact meets nothing here. So with this Link's shield up (guard_state)
        // and the attacker in front of it (within 60 degrees of its facing), the bit is set: 26 hits, life kept at 14
        // (with the shield up and without the bit: 14 -> 10 -> 6; 2026-11-06). Whether the block's reaction plays
        // is not checked.
        // (off by default: the "shield up" bit may be ZL held alone, the shield on the back in frames at a block, and
        // the hit is cancelled with no reaction; 2026-11-06)
        static int gb = -1;
        if (gb < 0) { const char *ev = cfg_get("TOTK_COOP_CONTACT_GUARDBIT"); gb = ev && *ev == '1'; }
        if (gb) {
            uint64_t tab = coop_rd_ptr(me_actor + 0x228), c1 = tab ? coop_rd_ptr(tab + 0x3b8) : 0, eq = tab ? coop_rd_ptr(tab + 0x230) : 0;
            uint64_t att = pup_ready_actor(ct_key);
            uint32_t f = 0, ef = 0;
            float lp[3], lr[9], ap[3];
            int front = 0;
            if (att && coop_rd(me_actor + COOP_ACTOR_POS, lp, 12) && coop_rd(me_actor + COOP_ACTOR_ROT, lr, 36) && coop_rd(att + COOP_ACTOR_POS, ap, 12)) {
                float dx = ap[0] - lp[0], dz = ap[2] - lp[2], d = sqrtf(dx * dx + dz * dz);
                front = d < 1e-3f || (lr[2] * dx + lr[8] * dz) / d > 0.5f;
            }
            (void)c1, (void)f;
            int up = shield_raised(me_actor);
            if (eq && front && up && coop_rd(eq + 0x4600, &ef, 4)) ef |= 1u << 17, memcpy((void *)(uintptr_t)(eq + 0x4600), &ef, 4);
            static int said;
            if (said++ < 20) rt_log("coop: contact guard at %.2f s: shield %s, attacker %s: %s", frame_step_world_us() / 1e6, up ? "raised" : "not raised",
                                    !att ? "not here" : front ? "in front" : "not in front", eq && front && up ? "blocked" : "not blocked");
        }
        memcpy((void *)(uintptr_t)(base + (uint64_t)cnt * CONTACT_SZ), e, CONTACT_SZ);
    }
    memcpy((void *)(uintptr_t)(l + 0x18), &cnt, 4);
    char gs[80];
    rt_log("coop: test: contact: %d added to this Link's list (now %d; %s; body %s, attacker %s)", n, cnt, guard_state(me_actor, gs, sizeof gs),
           ct_brel[0] ? "from its main body" : "none", actor_ref(pup_ready_actor(ct_key)) ? "this game's copy" : "none");
}
// Test (TOTK_COOP_TEST_CONTACTS=1): every damage component's hit update with contacts waiting: its actor (dc+0x198),
// and each contact's kind (+0x90), power (+0xa0), body (+0x58) and attacker handles' actors; 40 lines.
static void contacts_test(uint64_t dc) {
    static int on = -1;
    static _Atomic int said;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_CONTACTS") != NULL;
    if (!on || atomic_load(&said) > 40) return;
    uint64_t l = contact_list(dc), base;
    int32_t cnt = 0;
    if (!l || !coop_rd(l + 0x18, &cnt, 4) || cnt <= 0 || !(base = coop_rd_ptr(l + 0x10))) return;
    atomic_fetch_add(&said, 1);
    char an[64] = "", line[600];
    coop_actor_name(coop_rd_ptr(dc + 0x198), an, sizeof an);
    int m = snprintf(line, sizeof line, "coop: test: contacts on %s (%d; its main body %llx):", an, cnt,
                     (unsigned long long)enemy_body(coop_rd_ptr(dc + 0x198)));
    for (int k = 0; k < cnt && k < 3; k++) {
        uint8_t e[0xb0];
        if (!coop_rd(base + (uint64_t)k * CONTACT_SZ, e, sizeof e)) break;
        uint64_t body, r1, r2;
        memcpy(&body, e + 0x58, 8), memcpy(&r1, e + 0x68, 8), memcpy(&r2, e + 0x80, 8);
        char n1[48] = "", n2[48] = "";
        if (r1) coop_actor_name(coop_rd_ptr(r1 + 0x40), n1, sizeof n1);
        if (r2) coop_actor_name(coop_rd_ptr(r2 + 0x40), n2, sizeof n2);
        uint32_t pw;
        memcpy(&pw, e + 0xa0, 4);
        // whose body: the first actor its first 0x200 bytes point to (directly or one level down), and where
        char bn[48] = "?";
        int boff = -1, blv = 0;
        for (int w = 0; w < 0x200 / 8 && boff < 0; w++) {
            uint64_t q = coop_rd_ptr(body + 8 * w);
            for (int lv = 0; lv < 2 && q && boff < 0; lv++) {
                char t[48] = "";
                coop_actor_name(q, t, sizeof t);
                if (t[0] && (!strncmp(t, "Player", 6) || !strncmp(t, "Weapon_", 7) || !strncmp(t, "Armor_", 6)))
                    memcpy(bn, t, sizeof bn), boff = w * 8, blv = lv;
                q = coop_rd_ptr(q);
            }
        }
        m += snprintf(line + m, sizeof line - m, " [kind %u power %u body %llx (%s at +%#x%s) by %s / %s]", e[0x90], pw,
                      (unsigned long long)body, bn, boff, blv ? " ->+0" : "", n1, n2);
    }
    rt_log("%s", line);
    {   // and the first contact's every word, against the main body: a blocked one beside an unblocked one
        static uint8_t e[CONTACT_SZ];
        if (coop_rd(base, e, sizeof e)) {
            char w2[1600];
            int m2 = snprintf(w2, sizeof w2, "coop: test: contact words on %s:", an);
            for (int w = 0; w < CONTACT_SZ / 4 && m2 < 1560; w++) {
                uint32_t v;
                memcpy(&v, e + 4 * w, 4);
                if (v && v != 0xbf800000u) m2 += snprintf(w2 + m2, sizeof w2 - m2, " %x:%x", 4 * w, v);
            }
            rt_log("%s", w2);
        }
    }
}
// TotK Online (read for behaviour only) deletes, in each game, the contacts with an actor another game runs, so a hit
// is counted in one game only: the owner's, where the attacker's stand-in swings too. Here, on the copy of an enemy
// another game runs (native model), the contacts waiting for its hit update whose attacker (handle +0x80's actor) is
// a Link, this one or a co-op Link, and whose weapon (handle +0x68's) is a Weapon_ are taken out of the list before
// the hit is built: this Link's sword lands in the owner's game from its co-op Link there (and so is not also sent as
// the copy's lost life, peer_one), and a co-op Link's sword was its player's hit, already counted in the owner's game.
// The copy's reaction is its owner's action (coop_enemy_action). Arrows, bombs and the rest stay (their damage goes
// to the owner as lost life). TOTK_COOP_CONTACT_FILTER=0: none taken out.
static int ct_is_copy(uint64_t a);   // (below the copies' table)
static int ct_is_link(uint64_t a) {
    if (!a) return 0;
    if (a == pup_me) return 1;
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) if (co[i] == a) return 2;
    return 0;
}
// ---- rotating ground (TotK Online's ActorGroundControlSync, read for behaviour only): a GroundControlActor_Rotate*
// actor turns the platforms tied to it by a phase, [[actor+0x228]+0x2b0]+0x20 float +0x3c in 1.2.1. The host (the
// lowest player id among the games, world role 1) sends each one's phase ten times a second ("GR" packets: key and
// phase, up to GR_MAX), and the others write it every tick, so the platforms stand the same in every game.
// TOTK_COOP_GROUND=0: not. TOTK_COOP_TEST_GROUND=1: each one's phase logged every second.
#define GROUND_KIND 0x5247   // "GR"
#define GR_MAX 16
typedef struct { uint32_t kind, n; struct { uint32_t key; float phase; } g[GR_MAX]; } GroundPacket;
_Static_assert(sizeof(GroundPacket) <= COOP_HIT_MAX, "GroundPacket fits a HIT packet");
static uint64_t ground_phase_at(uint64_t a) {
    uint64_t c86 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2b0), o = c86 ? coop_rd_ptr(c86 + 0x20) : 0;
    return o ? o + 0x3c : 0;
}
static struct { uint32_t key; float phase; uint64_t ns; } ground_in[GR_MAX];
typedef struct { GroundPacket p; int test; } GroundScan;
static void ground_one(int cell, uint64_t a, void *arg) {
    GroundScan *gs = arg;
    char nm[64];
    coop_actor_name(a, nm, sizeof nm);
    if (strncmp(nm, "GroundControlActor_Rotate", 25)) return;
    uint64_t at = ground_phase_at(a);
    float ph = 0;
    if (!at || !coop_rd(at, &ph, 4)) return;
    float home[3];
    if (!home_is(cell, a)) {
        if (!coop_rd(a + COOP_ACTOR_POS, seen[cell].home, 12)) return;
        atomic_store_explicit(&seen[cell].home_of, a, memory_order_release);
        atomic_store_explicit(&seen[cell].home_ok, 1, memory_order_release);
    }
    memcpy(home, seen[cell].home, 12);
    uint32_t key = actor_key(a, nm, home);
    if (gs->test) rt_log("coop: test: ground %s %08x phase %.3f", nm, key, ph);
    if (coop_world_role() == 1) {
        if (gs->p.n < GR_MAX) gs->p.g[gs->p.n].key = key, gs->p.g[gs->p.n].phase = ph, gs->p.n++;
        return;
    }
    uint64_t now = plat_now_ns();
    for (int i = 0; i < GR_MAX; i++)
        if (ground_in[i].key == key && now - ground_in[i].ns < 1000000000ull) {
            memcpy((void *)(uintptr_t)at, &ground_in[i].phase, 4);
            static int said;
            if (!said++) rt_log("coop: ground %s %08x turns as the host's (phase %.3f here, %.3f there)", nm, key, ph, ground_in[i].phase);
        }
}
static void ground_tick(void) {
    static int on = -1, test = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_GROUND"); on = !(e && *e == '0'); test = cfg_get("TOTK_COOP_TEST_GROUND") != NULL; }
    if (!on || !coop_net_on()) return;
    static uint64_t last_send, last_test;
    uint64_t now = plat_now_ns();
    GroundScan gs = {.p = {.kind = GROUND_KIND}, .test = test && now - last_test > 1000000000ull};
    if (gs.test) last_test = now;
    each_live(ground_one, &gs);
    if (coop_world_role() == 1 && gs.p.n && now - last_send > 100000000ull) {
        last_send = now;
        coop_net_hit_send(&gs.p, (int)(offsetof(GroundPacket, g) + gs.p.n * sizeof gs.p.g[0]));
    }
}
static void ground_take(const GroundPacket *p, int n) {
    if (n != (int)(offsetof(GroundPacket, g) + p->n * sizeof p->g[0]) || p->n > GR_MAX) return;
    uint64_t now = plat_now_ns();
    for (uint32_t k = 0; k < p->n; k++) {
        int f = -1;
        for (int i = 0; i < GR_MAX && f < 0; i++) if (ground_in[i].key == p->g[k].key) f = i;
        for (int i = 0; i < GR_MAX && f < 0; i++) if (now - ground_in[i].ns > 2000000000ull) f = i;
        if (f >= 0) ground_in[f].key = p->g[k].key, ground_in[f].phase = p->g[k].phase, ground_in[f].ns = now;
    }
}

// ---- contacts sent to the enemy's owner (TOTK_COOP_STANDIN=puppet). TotK Online records, in the game whose Link
// made it, a contact with an actor another game owns, takes it out there and puts it into the owner's game, where the
// game builds the hit from it (read for behaviour only). The PlayerPuppet's swing in the owner's game makes no hit
// of its own (its attack byte on, no contact came: pair test, 2026-11-07), so this Link's sword contact on a copy goes
// to the copy's owner ("CE" packets: the enemy's key, the body's place from the enemy's main body), whose game adds
// it to that enemy's waiting contacts with the handles made the stand-in's (attacker) and its weapon's.
#define ECONTACT_KIND 0x4543   // "CE"
int coop_standin_puppet(void);   // coop_players.c
static ContactPacket ce_out;
static int ce_out_n;
static uint64_t ce_out_dc;
static struct { _Atomic int n; uint64_t dc, by, weap; uint32_t brel[CONTACT_MAX]; _Alignas(16) uint8_t e[CONTACT_MAX][CONTACT_SZ]; } ce_in;
static int ce_owner_of(uint64_t a, uint32_t *key) {
    *key = key_of_actor(a);
    for (int k = 0; *key && k < FOLLOW_N; k++) if (follow[k].key == *key) return follow[k].owner;
    return 0;
}
static void ce_queue(uint64_t dc, uint64_t a, uint64_t e) {
    uint32_t key = 0;
    int owner = ce_owner_of(a, &key);
    if (!owner || !key) return;
    if (ce_out_dc != dc || ce_out.key != key) ce_out_n = 0;
    if (ce_out_n >= CONTACT_MAX) return;
    if (!ce_out_n) memset(&ce_out, 0, offsetof(ContactPacket, e)), ce_out.key = key, ce_out.to = (uint32_t)owner, ce_out.kind = ECONTACT_KIND;
    ce_out_dc = dc;
    if (!coop_rd(e, ce_out.e[ce_out_n], CONTACT_SZ)) return;
    uint64_t body, mb0 = enemy_body(a);
    memcpy(&body, ce_out.e[ce_out_n] + 0x58, 8);
    ce_out.brel[ce_out_n] = body && mb0 && body >= mb0 && body - mb0 < 0x20000 ? (uint32_t)(body - mb0) + 1 : 0;
    ce_out_n++, ce_out.n = (uint32_t)ce_out_n;
}
static void ce_flush(void) {
    if (!ce_out_n) return;
    coop_net_hit_send(&ce_out, (int)(offsetof(ContactPacket, e) + ce_out.n * CONTACT_SZ));
    static int said;
    if (said++ < 20) rt_log("coop: contact: %d of this Link's sword on the copy of %08x sent to player %u", ce_out_n, ce_out.key, ce_out.to);
    ce_out_n = 0;
}
uint64_t coop_eq_row_ref(uint64_t actor, int row);   // coop_players.c
// A CE packet from player `from` (the hit update of its enemy adds it: ce_add).
static void ce_take(const ContactPacket *cp, int from) {
    if (!cp->n || cp->n > CONTACT_MAX || atomic_load(&ce_in.n)) return;
    uint64_t a = 0, co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < KNOWN_N && !a; i++) if (mine_known[i].key == cp->key) a = mine_known[i].actor;
    uint64_t by = 0;
    for (int i = 0; i < nc; i++) if (ids[i] == from) by = co[i];
    uint64_t dc = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2f8) : 0;
    if (!dc || !by) { rt_log("coop: contact from player %d on %08x: %s", from, cp->key, !dc ? "no such enemy here" : "no stand-in of theirs"); return; }
    memcpy(ce_in.e, cp->e, cp->n * CONTACT_SZ), memcpy(ce_in.brel, cp->brel, sizeof ce_in.brel);
    ce_in.dc = dc, ce_in.by = actor_ref(by), ce_in.weap = coop_eq_row_ref(by, 0);
    atomic_store(&ce_in.n, (int)cp->n);
}
static void ce_add(uint64_t dc) {
    if (!atomic_load(&ce_in.n) || dc != ce_in.dc) return;
    int n = atomic_exchange(&ce_in.n, 0);
    uint64_t a = coop_rd_ptr(dc + 0x198), l = contact_list(dc), base;
    int32_t cnt = 0, cap = 0;
    if (!l || !coop_rd(l + 0x18, &cnt, 4) || !coop_rd(l + 8, &cap, 4) || !(base = coop_rd_ptr(l + 0x10)) || cnt < 0) return;
    int added = 0;
    for (int k = 0; k < n && cnt < cap; k++, cnt++, added++) {
        uint8_t *e = ce_in.e[k];
        uint64_t refs[2] = {ce_in.weap ? ce_in.weap : ce_in.by, ce_in.by};
        for (int hk = 0; hk < 2; hk++) {
            uint32_t ser = 0xffffffffu;
            if (refs[hk]) coop_rd(refs[hk] + 0x48, &ser, 4);
            memcpy(e + (hk ? 0x80 : 0x68), &refs[hk], 8), memcpy(e + (hk ? 0x88 : 0x70), &ser, 4);
        }
        uint64_t mb = enemy_body(a), body = ce_in.brel[k] && mb ? mb + ce_in.brel[k] - 1 : mb;
        memcpy(e + 0x58, &body, 8);
        memcpy((void *)(uintptr_t)(base + (uint64_t)cnt * CONTACT_SZ), e, CONTACT_SZ);
    }
    memcpy((void *)(uintptr_t)(l + 0x18), &cnt, 4);
    char nm[48];
    static int said;
    if (said++ < 20) rt_log("coop: contact: %d from a stand-in's sword added to %s (weapon %s)", added, coop_actor_name(a, nm, sizeof nm), ce_in.weap ? "its own" : "none");
}
static void contacts_filter(uint64_t dc) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_CONTACT_FILTER"); on = !(e && *e == '0'); }
    if (!on || !enemy_native() || !coop_net_on()) return;
    uint64_t a = coop_rd_ptr(dc + 0x198), l, base;
    int32_t cnt = 0, cap = 0;
    // (the list first: it is empty in most hit updates, and the copy check reads more; the count clamped to the
    // capacity as contact_send does; code review, 2026-11-07)
    if (!(l = contact_list(dc)) || !coop_rd(l + 0x18, &cnt, 4) || cnt <= 0 || !coop_rd(l + 8, &cap, 4) || cap <= 0) return;
    if (cnt > cap) cnt = cap;
    if (cnt > 64 || !(base = coop_rd_ptr(l + 0x10))) return;
    // A PlayerPuppet stand-in takes no hit here at all (TotK Online deletes every contact with an actor another game
    // owns): its owner's game has the attack on the real Link from its copy of the enemy. Hit here, it played its own
    // reaction, ended in a T pose and put its sword on its back (pair test, attack_enemy_3rd.txt, 2026-11-07).

    if (coop_standin_puppet() && ct_is_link(a) == 2) {
        int32_t zero = 0;
        memcpy((void *)(uintptr_t)(l + 0x18), &zero, 4);
        static _Atomic int said_s;
        if (atomic_fetch_add(&said_s, 1) < 10) rt_log("coop: contact on a stand-in: %d taken out (its owner's game counts them)", cnt);
        return;
    }
    if (!ct_is_copy(a)) return;
    int out = 0, keep = 0;
    for (int k = 0; k < cnt; k++) {
        uint64_t e = base + (uint64_t)k * CONTACT_SZ, r1 = coop_rd_ptr(e + 0x68), r2 = coop_rd_ptr(e + 0x80);
        uint64_t w = r1 ? coop_rd_ptr(r1 + 0x40) : 0, by = r2 ? coop_rd_ptr(r2 + 0x40) : 0;
        char wn[48] = "", an[48] = "";
        if (w) coop_actor_name(w, wn, sizeof wn);
        int who = ct_is_link(by), drop = who && !strncmp(wn, "Weapon_", 7);
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 40)
            rt_log("coop: contact on the copy of %s by %s with %s (kind %u): %s", coop_actor_name(a, an, sizeof an),
                   who == 1 ? "this Link" : who == 2 ? "a co-op Link" : "something else", wn[0] ? wn : "-", LD8(e + 0x90),
                   drop ? "taken out (its owner's game counts it)" : "kept");
        if (drop) {
            if (who == 1 && coop_standin_puppet()) ce_queue(dc, a, e);
            out++;
            continue;
        }
        if (keep != k) memmove((void *)(uintptr_t)(base + (uint64_t)keep * CONTACT_SZ), (void *)(uintptr_t)e, CONTACT_SZ);
        keep++;
    }
    if (out) memcpy((void *)(uintptr_t)(l + 0x18), &keep, 4);
    ce_flush();
}
void coop_hit_update(Cpu *c) {
    contacts_test(c->x[0]);
    ce_add(c->x[0]);
    contacts_filter(c->x[0]);
    if (contact_on()) {
        uint64_t x0 = c->x[0];
        if (x0 && x0 == atomic_load(&ct_dc)) contact_add(x0);
        for (int i = 0; i < RCV_N; i++)
            if (x0 && atomic_load_explicit(&proxy_rcv[i], memory_order_relaxed) == x0 + 0x190) contact_send(x0, atomic_load(&proxy_id[i]));
    }
    uint64_t dc = atomic_load_explicit(&lh_dc, memory_order_acquire);
    if (!dc || c->x[0] != dc || !atomic_compare_exchange_strong(&lh_dc, &dc, 0)) return;
    uint64_t keep[31];
    memcpy(keep, c->x, sizeof keep);
    rt_call(c, MOD_MAIN_BASE + 0x31dbc0, 3, dc + 0x190, atomic_load(&lh_x1), (uint64_t)(uintptr_t)lh_info);
    memcpy(c->x, keep, sizeof keep);
    atomic_store(&lh_guarded, lh_info[0xe0] >> 3 & 1);
}
void coop_hit_drop(void) {
    uint64_t dc = atomic_exchange(&lh_dc, 0);
    if (dc) rt_log("coop: the hit waiting for this Link's update was dropped (its update did not run)");
}
static int link_hit_deliver(Cpu *c, const LinkHitPacket *lh, uint64_t me, uint64_t att) {
    (void)c;
    uint64_t dc = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x2f8);
    if (!dc || lh->info_len != HIT_INFO || atomic_load(&lh_dc)) return 0;
    uint8_t *h = lh_info;
    memcpy(h, lh->info, HIT_INFO);
    // TOTK_COOP_LINKHIT_ATTACKER=1: the handle names this game's frozen copy of the enemy; else none (a test)
    static int att_on = -1;
    if (att_on < 0) { const char *e = cfg_get("TOTK_COOP_LINKHIT_ATTACKER"); att_on = e && *e == '1'; }
    uint64_t ref = att_on ? actor_ref(att) : 0;
    uint64_t r8 = ref;
    uint32_t serial = 0xffffffffu;
    if (ref) coop_rd(ref + 0x48, &serial, 4);
    memcpy(h + 0x28, &r8, 8);
    memcpy(h + 0x30, &serial, 4);
    h[0xe0] &= (uint8_t)~8;
    // A raised shield does not block these hits (the tester's worry, 2026-11-06). A blocked hit reaches main+0x31dbc0
    // with damage 0 and nothing else of it different (this Link's own hits with its shield up), so the block is made
    // where the hit is built, from the attack's contact with the shield, before the delivery; not by the guard check
    // the delivery runs (main+0x1cd380c from main+0x1cceaac: never guarded with this Link's own attack object), nor by
    // this Link's listener (vtable +0x48, main+0x1cca59c: damage 3 -> 3 with its shield up), nor by the proxy's own
    // update with ZL let through in the owner's game. docs/coop/proxy-plan.md: the contact replayed instead.
    uint8_t *atk = lh_atk;
    memset(atk, 0, sizeof lh_atk);
    uint64_t self = (uint64_t)(uintptr_t)atk;
    memcpy(atk + 0x70, &self, 8), memcpy(atk + 0x30, &self, 8);
    atk[0x4a] = 1, atk[0x183] = 1;
    // The "attack" is the hit body's own object in its damage component (dc+0x18d0 for a Link; +8 that body's life,
    // 14, 11, 8 as the hits landed; +0xb8 its actor): this Link's object at the same place, as the game passes it;
    // the blank one when that is not there.
    uint64_t own = lh->atk_off && lh->atk_off < 0x2000 ? dc + lh->atk_off : 0;
    if (own && coop_rd_ptr(own + 0xb8) != me) own = 0;
    atomic_store(&lh_x1, own ? own : self);
    int32_t d[6];
    memcpy(d, h, sizeof d);
    rt_log("coop: delivering the hit here: damage %d, kind %d, attacker %s; the attack %s (+%#x)", d[0], d[5],
           ref ? "this game's copy" : "none", own ? "this Link's own" : "blank", lh->atk_off);
    atomic_store_explicit(&lh_dc, dc, memory_order_release);
    return 1;
}
static void link_hit_take(Cpu *c, const LinkHitPacket *lh, int from, uint64_t me) {
    if (enemy_native()) return;   // (a sender with the old model: its copy here hits this Link itself)
    const Follow *f = NULL;
    for (int k = 0; k < FOLLOW_N; k++) if (follow[k].key == lh->key) f = &follow[k];
    if (!me || !f || f->owner != from) {
        rt_log("coop: player %d says enemy %08x hit this Link for %d; not taken (this game runs that enemy)", from, lh->key, lh->damage);
        return;
    }
    int life = life_of(me);
    if (life <= 0) return;
    static int deliver_on = -1;
    if (deliver_on < 0) { const char *e = cfg_get("TOTK_COOP_LINKHIT_DELIVER"); deliver_on = !(e && *e == '0'); }
    int dl = deliver_on && !lh_wait.frames ? link_hit_deliver(c, lh, me, pup_ready_actor(lh->key)) : 0;
    if (dl) atomic_store(&lh_guarded, 0);
    if (dl) {
        lh_wait.frames = 12, lh_wait.life = life, lh_wait.damage = lh->damage, lh_wait.key = lh->key;
        rt_log("coop: enemy %08x hit this Link in player %d's game for %d: delivered here as the game's own hit", lh->key, from, lh->damage);
        return;
    }
    static int rec_on = -1;
    if (rec_on < 0) { const char *e = cfg_get("TOTK_COOP_LINKHIT_RECORD"); rec_on = e && *e == '1'; }
    uint64_t att = pup_ready_actor(lh->key), ref = rec_on ? actor_ref(att) : 0;
    uint64_t dc = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x2f8);
    uint32_t cur = 1;
    if (dc) coop_rd(dc + 0x1a4, &cur, 4);
    if (ref && dc && !cur && lh->nslots && lh->nslots <= LH_SLOTS && !lh_wait.frames) {
        const LinkHitSlot *first = NULL;
        for (int pass = 0; pass < 2; pass++)   // slot 0 (with the state word +0x1a4) last: the update takes it then
            for (uint32_t k = 0; k < lh->nslots; k++) {
                const LinkHitSlot *e = &lh->s[k];
                if (e->slot >= LH_SLOTS || (pass == 0) == (e->slot == 0)) continue;
                uint64_t v = e->how == LH_ATTACKER ? ref : e->how == LH_DC ? dc + e->v : e->how == LH_ACTOR ? me + e->v : e->v;
                memcpy((void *)(uintptr_t)(dc + 0x1a0 + e->slot * 8), &v, 8);
                if (!first) first = e;
            }
        lh_wait.frames = 12, lh_wait.life = life, lh_wait.damage = lh->damage, lh_wait.key = lh->key;
        rt_log("coop: enemy %08x hit this Link in player %d's game for %d: its record (%u slots) written here, the attacker this game's copy",
               lh->key, from, lh->damage, lh->nslots);
        return;
    }
    int now = life - lh->damage < 0 ? 0 : life - lh->damage;
    set_life(me, now);
    rt_log("coop: enemy %08x hit this Link in player %d's game for %d (life %d -> %d%s)", lh->key, from, lh->damage, life, now,
           !rec_on ? "" : !ref ? "; no copy of it here to hit with" : cur ? "; a hit of its own was being taken" : "");
}
// Test (TOTK_COOP_TEST_HITWATCH=<from>-<to>[@<off>]): from that many seconds into the world to the other, a write
// watch (coop_watch_win.c) on this Link's damage component at +<off> (default 0x1a4, the hit record's state word), 8
// bytes: which game functions write it when an enemy's hit lands (logged when it is taken off).
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
static uint64_t pup_ready_actor(uint32_t key);
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
    {   // and the nearest Enemy_ this game runs or follows (TOTK_COOP_TEST_LINKDUMP_KEY=<hex key>: that one)
        const char *ks = cfg_get("TOTK_COOP_TEST_LINKDUMP_KEY");
        uint32_t want = ks ? (uint32_t)strtoul(ks, NULL, 16) : 0;
        uint64_t en = want ? pup_ready_actor(want) : 0;
        for (int i = 0; !en && want && i < KNOWN_N; i++) if (mine_known[i].key == want) en = mine_known[i].actor;
        if (en) actor_dump(en, "enemy", done);
        if (en) {   // where the enemy holds this Link: its actor or its reference, in its first 0x3000 bytes and its components
            uint64_t ref = actor_ref(me), tab = coop_rd_ptr(en + 0x228);
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
static void enemy_poke_test(void);   // (after pups)
// ---- enemies made at run time, made in the other games too (TotK Online's way: it registers every actor the game
// hands over from its create queue, 1.2.1 main+0xcd7d40, and the other games make one by name; read for behaviour).
// The game hands a new actor over at main+0x89841c (ActorCreateInfo::getActor returning to main+0x898420; found from
// the 1.2.1 site by its code). An Enemy_ with no map placement (its placement hash, [actor+0x370]+0x88 in 1.2.1, 0) is
// sent to the others (SPAWN_KIND: its key, name, place, rotation), and each makes one there with that key; when it goes
// away here without dying (unloaded), SPAWN_GONE deletes the others' (a death goes by the enemy sync).
#define SPAWN_KIND 0x5053   // "SP"
#define SPAWN_GONE 0x4753   // "SG"
typedef struct { uint32_t key, to, kind, pad; char name[48]; float pos[3], rot[9]; } SpawnPacket;
_Static_assert(sizeof(SpawnPacket) <= COOP_HIT_MAX, "SpawnPacket fits a HIT packet");
#define BORN_N 256   // (a load hands over hundreds; only enemies are kept)
static struct { _Atomic uint64_t actor; _Atomic uint64_t at_ns; } born[BORN_N];   // handed over here (spawn_tick)
static _Atomic unsigned born_n;
void coop_actor_created(uint64_t a) {   // coop_spawn.c, from the getActor hook (any thread)
    if (!a || !coop_net_on()) return;
    char nm[48];
    if (!coop_world_spawn_kind(coop_actor_name(a, nm, sizeof nm))) return;
    unsigned i = atomic_fetch_add(&born_n, 1) % BORN_N;
    atomic_store_explicit(&born[i].actor, 0, memory_order_release);
    atomic_store_explicit(&born[i].at_ns, plat_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&born[i].actor, a, memory_order_release);
}
static struct { uint32_t key; char name[48]; float pos[3]; int state; uint64_t actor, ref, ns; int from;
                _Alignas(16) uint8_t handle[0x40], link[0x40]; } made[32];   // the copies being made here (state 1, 2 made)
static struct { uint32_t key; uint64_t actor; } spawned_mine[64];   // enemies this game made at run time and sent
static int spawn_on(void) {   // TOTK_COOP_SPAWN_SYNC=0: not done
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_SPAWN_SYNC"); on = !(e && *e == '0'); }
    return on;
}
// Test (TOTK_COOP_TEST_ACTORDUMP=<dir>): 20 s into the world, each Enemy_ actor here (up to 6): its first 0x800 bytes,
// and 0x200 bytes behind each word of them that points into the heap, to <dir>/actor_<player>_<key>.bin (for finding
// the placement hash in 1.4.3: a word the same in both games for one enemy and different for another).
static void actor_dump_test(void) {
    static int done;
    const char *dir = done ? NULL : cfg_get("TOTK_COOP_TEST_ACTORDUMP");
    if (!dir || frame_step_world_us() < 20000000) return;
    done = 1;
    int n = 0;
    for (int i = 0; i < SEEN_N && n < 6; i++) {
        uint64_t a = atomic_load(&seen[i].actor);
        char nm[48];
        if (!a || strncmp(coop_actor_name(a, nm, sizeof nm), "Enemy_", 6)) continue;
        uint32_t key = enemy_of(i, a, nm, sizeof nm);
        static uint8_t buf[0x800 + 0x100 * 0x200];
        memset(buf, 0, sizeof buf);
        coop_rd(a, buf, 0x800);
        for (int k = 0; k < 0x100; k++) {
            uint64_t p;
            memcpy(&p, buf + 8 * k, 8);
            if (p > 0x100000000ull && p < 0x8000000000ull) coop_rd(p, buf + 0x800 + k * 0x200, 0x200);
        }
        char path[PLAT_PATH_MAX];
        snprintf(path, sizeof path, "%s/actor_%d_%08x.bin", dir, coop_net_id(), key);
        rt_log("coop: test: actordump %s %s: %s", nm, path, plat_file_write(path, buf, sizeof buf) ? "written" : "not written");
        n++;
    }
}
static void spawn_tick(Cpu *c) {
    actor_dump_test();
    if (!spawn_on()) return;
    uint64_t now = plat_now_ns();
    static uint64_t pruned;
    if (now - pruned > 1000000000ull) pruned = now, keyed_prune();
    for (int i = 0; i < BORN_N; i++) {   // new enemies 0.3 s on (its place settled); not ready by 3 s: let go of
        uint64_t a = atomic_load_explicit(&born[i].actor, memory_order_acquire);
        uint64_t at = atomic_load_explicit(&born[i].at_ns, memory_order_relaxed);
        if (!a || now - at < 300000000ull) continue;
        char nm[48];
        int32_t st = 0;
        coop_actor_name(a, nm, sizeof nm);
        int ready = coop_world_spawn_kind(nm) && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6;
        if (!ready && now - at < 3000000000ull) continue;
        uint64_t want = a;   // taken (unless another was handed over into the slot meanwhile)
        if (!atomic_compare_exchange_strong(&born[i].actor, &want, 0) || !ready) continue;
        uint64_t ph = 0, pp = coop_rd_ptr(a + PLACE_HASH_PTR);   // (place_hash, but all ones logged as it is)
        if (pp) coop_rd(pp + PLACE_HASH, &ph, 8);
        static int said;
        if (said++ < 40) rt_log("coop: spawn sync: %s %llx handed over, placement hash %llx", nm, (unsigned long long)a, (unsigned long long)ph);
        if (ph && ph != ~0ull) continue;   // placed in the map: every game has it
        int ours = 0;   // a copy this game made for another
        for (int k = 0; k < 32 && !ours; k++) if (made[k].state && made[k].actor == a) ours = 1;
        float p[3], r[9];
        if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36)) continue;
        for (int k = 0; k < 32 && !ours; k++)
            if (made[k].state == 1 && !strcmp(made[k].name, nm) && dist3(made[k].pos, p) < 2) ours = 1, made[k].actor = a;
        if (ours) continue;
        SpawnPacket sp = {.key = enemy_key(nm, p), .kind = SPAWN_KIND};
        snprintf(sp.name, sizeof sp.name, "%s", nm), memcpy(sp.pos, p, 12), memcpy(sp.rot, r, 36);
        keyed_set(a, sp.key);
        int f = -1;
        for (int k = 0; k < 64 && f < 0; k++) if (!spawned_mine[k].key) f = k;
        if (f >= 0) spawned_mine[f].key = sp.key, spawned_mine[f].actor = a;   // (full: its going away is not told)
        coop_net_hit_send(&sp, sizeof sp);
        rt_log("coop: spawn sync: %s %08x made here at %.1f %.1f %.1f: sent to the others", nm, sp.key, p[0], p[1], p[2]);
    }
    for (int k = 0; k < 32; k++) {   // the copies being made; made ones let go of 10 s on (handed over by then)
        if (made[k].state == 2 && now - made[k].ns > 10000000000ull) made[k].state = 0, made[k].actor = 0;
        if (made[k].state != 1) continue;
        int st = coop_handle_status(made[k].handle);
        if (st == 3) {
            uint64_t a = coop_claim(c, made[k].handle, made[k].link, &made[k].ref);
            if (a) made[k].actor = a, made[k].state = 2, made[k].ns = now, keyed_set(a, made[k].key);
            else made[k].state = 0;
            rt_log("coop: spawn sync: %s %08x for player %d %s", made[k].name, made[k].key, made[k].from, a ? "made here" : "not made");
        } else if (st > 3 || now - made[k].ns > 10000000000ull) {
            made[k].state = 0;
            rt_log("coop: spawn sync: %s %08x could not be made (%d)", made[k].name, made[k].key, st);
        }
    }
    // this game's spawned enemies gone (unloaded, not dead): the others' copies go too
    for (int k = 0; k < 64; k++) {
        if (!spawned_mine[k].key) continue;
        uint64_t a = spawned_mine[k].actor;
        int32_t st = 0;
        char nm[48];
        int alive = coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 && coop_world_spawn_kind(coop_actor_name(a, nm, sizeof nm));
        if (alive && keyed_of(a) == spawned_mine[k].key) continue;
        if (!alive || life_of(a) != 0) {
            SpawnPacket sp = {.key = spawned_mine[k].key, .kind = SPAWN_GONE};
            coop_net_hit_send(&sp, sizeof sp);
            rt_log("coop: spawn sync: %08x gone here: the others go too", sp.key);
        }
        spawned_mine[k].key = 0, spawned_mine[k].actor = 0;
    }
}
static void spawn_take(Cpu *c, const SpawnPacket *sp, int from) {
    if (!spawn_on()) return;
    for (int i = 0; i < KEYED_N; i++)
        if (keyed[i].key == sp->key && atomic_load(&keyed[i].actor)) {
            uint64_t a = atomic_load(&keyed[i].actor);
            if (sp->kind == SPAWN_GONE && keyed_of(a) == sp->key) {   // (still that enemy: alive, its name, that key)
                int32_t st = 0;
                if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6) return;
                coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, COOP_DELETE_REASON, 0, 0);
                atomic_store(&keyed[i].actor, 0);
                rt_log("coop: spawn sync: %08x gone in player %d game: deleted here", sp->key, from);
            }
            return;   // (have it)
        }
    if (sp->kind != SPAWN_KIND || memchr(sp->name, 0, sizeof sp->name) == NULL || !coop_world_spawn_kind(sp->name)) return;
    for (int k = 0; k < 32; k++) if (made[k].state && made[k].key == sp->key) return;
    for (int k = 0; k < 32; k++) {
        if (made[k].state) continue;
        memset(made[k].handle, 0, sizeof made[k].handle);
        snprintf(made[k].name, sizeof made[k].name, "%s", sp->name);   // (kept: the create may read it later)
        if (!coop_request_create(c, made[k].name, sp->pos, sp->rot, made[k].handle)) return;
        made[k].key = sp->key, made[k].state = 1, made[k].actor = 0, made[k].ns = plat_now_ns(), made[k].from = from;
        memcpy(made[k].pos, sp->pos, 12);
        rt_log("coop: spawn sync: %s %08x made in player %d game: requested here", made[k].name, sp->key, from);
        return;
    }
}
// For coop_mounts.c: each synced actor that is not an enemy, and who runs a key.
typedef struct { void (*fn)(uint64_t, const char *, uint32_t, int, void *); void *arg; } MountScan;
static void mount_one(int cell, uint64_t a, void *arg) {
    MountScan *m = arg;
    char nm[48];
    int kind = 0;
    int32_t st = 0;
    uint32_t key = synced_of(cell, a, nm, sizeof nm, &kind);
    if (key && kind != COOP_KIND_ENEMY && !dup_key(key) && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6)
        m->fn(a, nm, key, kind, m->arg);
}
void coop_world_each_mount(void (*fn)(uint64_t actor, const char *name, uint32_t key, int kind, void *arg), void *arg) {
    if (!coop_mounts_on()) return;
    MountScan m = {fn, arg};
    each_live(mount_one, &m);
}
int coop_world_owner_of(uint32_t key) {
    for (int i = 0; i < FOLLOW_N; i++) if (follow[i].key == key) return follow[i].owner;
    return -1;
}
static void peer_tick(Cpu *c, uint64_t me) {
    pup_me = me;
    coop_mounts_tick(c, me);
    enemy_poke_test();
    link_dump_test(me);
    guard_scan_test(me);
    hit_watch_test(me);
    link_hit_check(me);
    void coop_glue_tick(Cpu *c, uint64_t me);   // coop_glue.c: Ultrahand glue made here sent, the others' made again
    coop_glue_tick(c, me);
    int my_id = coop_net_id();
    // Hits from the others on the enemies this game runs: the damage taken from this game's one.
    HitPacket hp;
    int from = 0, n;
    static LinkHitPacket lhp;
    while ((n = coop_net_hit_take(&lhp, sizeof lhp, &from)) != 0) {
        int coop_gamedata_take(const void *data, int n, int from);   // coop_gamedata.c: shared world progress
        if (coop_gamedata_take(&lhp, n, from)) continue;
        {   // contact replay (a test)
            const ContactPacket *cp = (const ContactPacket *)&lhp;
            if (n >= (int)offsetof(GroundPacket, g) && ((const GroundPacket *)&lhp)->kind == GROUND_KIND) {
                ground_take((const GroundPacket *)&lhp, n);
                continue;
            }
            int coop_glue_take(const void *p, int n, int from);   // coop_glue.c: "GA" and "GL" packets (Ultrahand glue)
            if (coop_glue_take(&lhp, n, from)) continue;
            if (n >= (int)offsetof(ContactPacket, e) && cp->kind == ECONTACT_KIND && (int)cp->to == my_id && cp->n <= CONTACT_MAX &&
                n == (int)(offsetof(ContactPacket, e) + cp->n * CONTACT_SZ)) {
                ce_take(cp, from);
                continue;
            }
            if (n >= (int)offsetof(ContactPacket, e) && cp->kind == CONTACT_KIND) {
                if (contact_on() && (int)cp->to == my_id && cp->n && cp->n <= CONTACT_MAX &&
                    n == (int)(offsetof(ContactPacket, e) + cp->n * CONTACT_SZ) && !atomic_load(&ct_n)) {
                    uint64_t mdc = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x2f8);
                    memcpy(ct_in, cp->e, cp->n * CONTACT_SZ);
                    memcpy(ct_brel, cp->brel, sizeof ct_brel), ct_key = cp->key;
                    atomic_store(&ct_dc, mdc);
                    atomic_store(&ct_n, (int)cp->n);
                    rt_log("coop: test: contact by enemy %08x from player %d: %u waiting for this Link's hit update", cp->key, from, cp->n);
                }
                continue;
            }
        }
        {
            const SpawnPacket *sp = (const SpawnPacket *)&lhp;
            if (n == (int)sizeof(SpawnPacket) && (sp->kind == SPAWN_KIND || sp->kind == SPAWN_GONE)) { spawn_take(c, sp, from); continue; }
        }
        if (n == (int)sizeof(CoopTravelPacket) && ((const CoopTravelPacket *)&lhp)->kind == COOP_TRAVEL_KIND) {   // coop_notices.c
            coop_notice_travel_take((const CoopTravelPacket *)&lhp, n, from);
            continue;
        }
        if (n >= (int)offsetof(LinkHitPacket, s) && lhp.kind == LINKHIT_KIND && lhp.nslots <= LH_SLOTS &&
            n == (int)(offsetof(LinkHitPacket, s) + lhp.nslots * sizeof(LinkHitSlot))) {
            // (a hit its sender covered with a contact is not sent: link_hits_send)
            if ((int)lhp.to == my_id && lhp.damage > 0 && lhp.damage <= 4000) link_hit_take(c, &lhp, from, me);
            continue;
        }
        memcpy(&hp, &lhp, sizeof hp);
        if (n != (int)sizeof hp || (int)hp.to != my_id) continue;
        // (a damage that is not 1..65535 is not a hit: a negative one healed, a huge one overflowed; code review)
        if (hp.damage <= 0 || hp.damage > 65535) continue;
        for (int i = 0; i < KNOWN_N; i++)
            if (mine_known[i].key == hp.key && mine_known[i].actor) {
                // still that enemy: alive and an Enemy_ (its entry outlives it by up to 0.4 s)
                int32_t est = 0;
                char enm[64];
                if (!coop_rd(mine_known[i].actor + COOP_ACTOR_STATE, &est, 4) || est < 2 || est > 6 ||
                    !coop_world_kind(coop_actor_name(mine_known[i].actor, enm, sizeof enm)))
                    break;
                int life = life_of(mine_known[i].actor);
                if (life > 0) {
                    set_life(mine_known[i].actor, life - hp.damage);
                    rt_log("coop: player %d hit enemy %08x for %d (life %d -> %d)", from, hp.key, hp.damage, life,
                           life - hp.damage < 0 ? 0 : life - hp.damage);
                }
                break;
            }
    }
    epose_take_tick(c);
    spawn_tick(c);
    epose_send_tick();
    link_hits_send();
    enemy_weapon_test();
    enemy_hit_test();
    static int t;
    if (++t % 3) return;   // the rest about 10 times a second
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        uint64_t age = 0;
        int len = id == my_id ? 0 : coop_net_world(id, &remote_pk[id], sizeof remote_pk[id], &age);
        remote_ok[id] = len >= 4 && age < 1000000000ull && 4 + remote_pk[id].count * (int)sizeof(NetEnemy) <= len;
    }
    static PeerScan s;
    memset(&s.pk, 0, sizeof s.pk);
    s.c = c, s.me_id = my_id, s.snaps = 0, s.puppets = 0, s.mounts_sent = 0, s.now_ns = plat_now_ns();
    s.no = coop_player_positions_ids(s.others, s.ids, COOP_MAX_PLAYERS);
    s.have_me = me && coop_rd(me + COOP_ACTOR_POS, s.me, 12);
    // The dead to report: the last 5 s, and older ones of this session near another Link (up to MAX_DEAD_SENT).
    int64_t now = frame_step_world_us();
    uint32_t dead_keys[MAX_DEAD_SENT];
    int nd = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < KILLED_N && nd < MAX_DEAD_SENT; i++) {
            if (!killed[i].key) continue;
            int recent = now - killed[i].died_us < DEAD_KEEP_US;
            if (pass == 0 ? !recent : recent || !near_other(&s, killed[i].home, NULL)) continue;
            dead_keys[nd++] = killed[i].key;
        }
    s.live_max = MAX_SENT - nd;
    nweaps = 0;
    each_live(weap_collect, NULL);
    dup_count();
    each_live(peer_one, &s);
    // Enemies this game ran that are no longer seen: gone; those whose life was 0 when last seen died here.
    for (int i = 0; i < KNOWN_N; i++) {
        if (!mine_known[i].key || now - mine_known[i].at_us < 400000) continue;
        if (mine_known[i].life == 0) killed_add(mine_known[i].key, mine_known[i].home);
        mine_known[i].key = 0, mine_known[i].actor = 0;
    }
    for (int d = 0; d < nd && s.pk.count < MAX_SENT; d++) {
        NetEnemy e = {.key = dead_keys[d], .flags = 1};
        s.pk.e[s.pk.count++] = e;
    }
    if (s.no) coop_net_world_send(&s.pk, 4 + s.pk.count * (int)sizeof(NetEnemy));
    static int tl, snaps, puppets;
    snaps += s.snaps, puppets = s.puppets;
    if (++tl % 100 == 0) {
        int theirs = 0;
        for (int id = 1; id <= COOP_MAX_PLAYERS; id++) theirs += remote_ok[id] ? remote_pk[id].count : 0;
        rt_log("coop: enemies: %d sent, %d from the others; %d posed from their owner, %d put back in the last 10 s", s.pk.count,
               theirs, puppets, snaps);
        snaps = 0;
    }
}

// ---- enemy puppets: an enemy another game runs, frozen here and shown as the owner's
// The owner sends, for its enemies near another Link (the EPOSE_MAX nearest), an EPOSE packet each frame (every third
// beyond EPOSE_NEAR_M): the enemy's place and rotation and its body skeleton's local records (+0x18, 0x60 a bone),
// packed as a Link's (coop_proxy_pack_mine, without its final-matrix step). Here the enemy's own one runs no calc and
// none of its update phases once two poses have come (coop_enemy_puppet_calc / _phase, from coop_spawn.c's hooks); in
// place of its update phase main+0x245030 it is put at the owner's place and pose of EPOSE_DELAY_MS ago, blended
// between the two around it, as the proxy is (proxy_drive in coop_players.c): actor place, model world matrix, the
// records, the model push, and its physics body put there (main+0x25a450, which takes on a body whose actor does not
// run). Poses missing for PUP_STALE_NS: its own update runs again.
// Its weapons: an enemy's weapon is an actor of its own, which put itself on the hand a frame late and could be taken
// from the frozen enemy's hand (a play session, 2026-11-06), as an arrow's copy could. No pointer from a held
// weapon to its holder was found, so they go by place: the owner sends with each pose the Weapon_ actors within
// EPOSE_ITEM_M of the enemy that are no Link's (coop_is_players_item), by name and world place; here the Weapon_ of
// that name nearest that place is frozen as the enemy is and put there right after the enemy's model push, with its
// body kept 300 m under it so it cannot be taken. One the owner no longer sends (dropped and left behind, or the enemy
// let go here) runs its own update again from where it is, its body put back first.
#define EPOSE_MAX 12
#define EPOSE_NEAR_M 40.0f
#define PUP_N 32
#define PUP_HIST 6
#define PUP_BONES 330
#define PUP_STALE_NS 600000000ull
#define EPOSE_ITEMS 3
#define EPOSE_ITEM_M 2.5f
// then nitems EItem, then the pose (n | 0x8000, scaled count, bones)
// t_ns: when the owner took it (its plat_now_ns), so the poses are blended at the owner's own pace (stamped when they
// came, a few ms unevenly, the blend shook: 2 mm a frame on a Chuchu's copy that moved 0.1 mm, and none with one pose
// held; 2026-11-06).
// aware: the enemy's awareness words (AWARE_OFF in its component AWARE_COMP), has_aware 1 when read.
// An enemy's copy never notices the Link of the game that follows it (its AI is stopped), so that game showed the
// Sneakstrike prompt beside an enemy fighting its player, and its Link put its sword away. When the enemy notices a
// Link, its component at +0x228 table entry +0x158 changes: +0x24 0xc182 -> 0xc082, +0x58 and +0x60 0x12 -> 0xc,
// +0x64 and +0x70 0 -> 1, +0x84 0x28 -> 0x83 (the owner's Bokoblin just spawned and fighting, the copy keeping the
// first: TOTK_COOP_TEST_LINKDUMP, 2026-11-06). Those words, put on the copy each tick (TOTK_COOP_TEST_ENEMYPOKE),
// took the prompt away, the game said "We are under threat" and its Link drew its sword. So the owner sends them
// with each pose and the copy takes them (pup_aware_apply).
#define AWARE_COMP 0x158
#define AWARE_N 6
static const uint16_t AWARE_OFF[AWARE_N] = {0x24, 0x58, 0x60, 0x64, 0x70, 0x84};
typedef struct {
    uint32_t key; float pos[3], rot[9]; uint8_t nitems, has_aware, pad[2]; uint64_t t_ns; uint32_t aware[AWARE_N];
} EPoseHead;
typedef struct { uint32_t hash; float pos[3], rot[9]; } EItem;   // hash: its name's (name_hash_of)
// The native enemy model (enemy_native, TotK Online's way; read for behaviour only): pad[0] EPOSE_NATIVE, the packet
// is the header (the enemy's place and rotation) and, with EPOSE_ACTION, an EAction after it; no items, no bones.
#define EPOSE_NATIVE 2
#define EPOSE_ACTION 1
// An enemy's action request as the game made it (main+0x50ad74's x1, 0x58 bytes: three Vec3, ids, a pointer at +0x38
// to its name when +0x40 is 1, an action id at +0x4c) and its mode (w2); name: that name, copied.
#define EACT_REQ 0x58
typedef struct { uint8_t mode, has_name, pad[2]; char name[32]; uint8_t req[EACT_REQ]; } EAction;
_Static_assert(sizeof(EAction) == 0x7c && 88 + 0x7c <= COOP_EPOSE_MAX, "EAction fits an EPOSE packet");
static int enemy_native(void) {   // TOTK_COOP_ENEMY_MODEL=puppet: frozen copies posed bone by bone (the old model)
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_ENEMY_MODEL"); on = !(e && !strcmp(e, "puppet")); }
    return on;
}
_Static_assert(sizeof(EPoseHead) == 88 && sizeof(EItem) == 52 && 88 + EPOSE_ITEMS * 52 == 244, "EPOSE header size (COOP_EPOSE_MAX)");
typedef struct {
    uint64_t ns, src_ns;   // ns: here (src_ns, the owner's, moved onto this clock); src_ns: the owner's
    float pos[3], q[4];
    int n, ni;
    uint32_t ih[EPOSE_ITEMS];
    float ip[EPOSE_ITEMS][3], iq[EPOSE_ITEMS][4];
    float bq[PUP_BONES][4], bt[PUP_BONES][3], bs[PUP_BONES][3];
} PupRec;
static struct {
    _Atomic uint64_t actor;   // this game's one, while it follows its owner (pup_bind)
    _Atomic int ready;        // frozen and driven from the poses
    uint32_t key;
    int owner;
    unsigned count;
    uint64_t last_ns;
    _Atomic uint64_t weap[EPOSE_ITEMS];   // this game's weapons of it, frozen and put in its hand (0 none)
    uint32_t weap_hash[EPOSE_ITEMS];
    uint32_t aware[AWARE_N];   // the owner's awareness words, latest (have_aware)
    int have_aware;
    // native model: the owner's latest action (under pup_lock); esc_s: how long the copy's own state bit has been set
    // (enemy_action_hook); name: the action's name where the copy's request points
    EAction act;
    int has_act;
    float esc_s;
    uint64_t esc_ns;
    int esc_bit;   // the bit as the last request found it
    char act_name[32];
    PupRec r[PUP_HIST];
} pups[PUP_N];
// Test (TOTK_COOP_TEST_ENEMYPOKE=<comp>:<off>=<value>[;...]): every tick, those 32-bit words written into each
// frozen enemy copy's component (+0x228 table entry <comp>): an aware enemy's state put on a copy, to find what makes
// this game's Link see it as fighting it (no Sneakstrike prompt, the shield up).
static void enemy_poke_test(void) {
    static int n = -1;
    static uint32_t cm[16], of[16], va[16];
    if (n < 0) {
        n = 0;
        const char *e = cfg_get("TOTK_COOP_TEST_ENEMYPOKE");
        while (e && *e && n < 16) {
            unsigned a1, a2, a3;
            if (sscanf(e, "%x:%x=%x", &a1, &a2, &a3) != 3) break;
            cm[n] = a1, of[n] = a2, va[n] = a3, n++;
            e = strchr(e, ';');
            if (e) e++;
        }
        if (n) rt_log("coop: test: enemypoke %d words", n);
    }
    for (int p = 0; n && p < PUP_N; p++) {
        uint64_t a = atomic_load(&pups[p].ready) ? atomic_load(&pups[p].actor) : 0, tab = a ? coop_rd_ptr(a + 0x228) : 0;
        for (int k = 0; tab && k < n; k++) {
            uint64_t o = coop_rd_ptr(tab + cm[k]);
            if (o) memcpy((void *)(uintptr_t)(o + of[k]), &va[k], 4);
        }
    }
}

// Weapons let go (their enemy no longer posed here) whose body is still to be put back, by epose_take_tick.
static uint64_t weap_free[64];
static int weap_free_n;
static PlatMutex pup_lock = PLAT_MUTEX_INIT;
static int pup_find(uint32_t key) {
    for (int i = 0; i < PUP_N; i++) if (pups[i].key == key) return i;
    return -1;
}
static void pup_bind(uint32_t key, int owner, uint64_t actor) {
    int i = pup_find(key);
    if (i < 0 || pups[i].owner != owner) return;
    atomic_store(&pups[i].actor, actor);
}
static void pup_release(uint32_t key) {
    int i = pup_find(key);
    if (i < 0) return;
    plat_mutex_lock(&pup_lock);
    if (atomic_load(&pups[i].ready)) rt_log("coop: enemy %08x: its own update runs again", key);
    atomic_store(&pups[i].ready, 0), atomic_store(&pups[i].actor, 0);
    for (int j = 0; j < EPOSE_ITEMS; j++) {
        uint64_t w = atomic_exchange(&pups[i].weap[j], 0);
        if (w && weap_free_n < 64) weap_free[weap_free_n++] = w;
    }
    pups[i].key = 0, pups[i].count = 0;
    plat_mutex_unlock(&pup_lock);
}
static uint64_t pup_ready_actor(uint32_t key) {   // this game's frozen copy of it, 0 none
    int i = pup_find(key);
    return i >= 0 && atomic_load(&pups[i].ready) ? atomic_load(&pups[i].actor) : 0;
}
static int pup_bound_to(uint32_t key, uint64_t actor) {   // already this actor's copy
    int i = pup_find(key);
    return i >= 0 && atomic_load(&pups[i].actor) == actor;
}
static int pup_ready_key(uint32_t key) {
    int i = pup_find(key);
    return i >= 0 && atomic_load(&pups[i].ready);
}
static void mat_quat(const float r[9], float q[4]) {   // rows; q x y z w
    float trc = r[0] + r[4] + r[8];
    if (trc > 0) { float s = sqrtf(trc + 1) * 2; q[3] = s / 4, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s; }
    else if (r[0] > r[4] && r[0] > r[8]) { float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2; q[3] = (r[7] - r[5]) / s, q[0] = s / 4, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s; }
    else if (r[4] > r[8]) { float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2; q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = s / 4, q[2] = (r[5] + r[7]) / s; }
    else { float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2; q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = s / 4; }
}
static void quat_mat(const float q[4], float r[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
static void qblend(const float a[4], const float b[4], float f, float out[4]) {
    float dot = 0, l = 0;
    for (int k = 0; k < 4; k++) dot += a[k] * b[k];
    float sg = dot < 0 ? -1.0f : 1.0f;
    for (int k = 0; k < 4; k++) out[k] = a[k] * (1 - f) + b[k] * sg * f, l += out[k] * out[k];
    l = sqrtf(l);
    for (int k = 0; k < 4; k++) out[k] = l > 0 ? out[k] / l : k == 3;
}
static uint64_t enemy_model(uint64_t a) { return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x8) + 0x28); }
// An actor's body skeleton from its model: model +0x18 +0 +0x58 +0xb0 (found by enemy_skel_probe on a Chuchu (25
// bones), a Bokoblin (76) and a Link (127, the one skel_path finds); a Link's path from the actor finds none on an
// enemy, 2026-11-06).
static uint64_t enemy_skeleton(uint64_t a) {
    static const uint32_t path[4] = {0x18, 0, 0x58, 0xb0};
    uint64_t sk = enemy_model(a);
    for (int i = 0; i < 4 && sk; i++) sk = coop_rd_ptr(sk + path[i]);
    return sk && is_skeleton(sk) ? sk : 0;
}
static uint64_t enemy_body(uint64_t a) {
    uint64_t phys = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    return ctl ? coop_rd_ptr(ctl + 0x150) : 0;
}
static uint64_t audit_skeleton(uint64_t a) { return enemy_skeleton(a); }
// Test (TOTK_COOP_TEST_ENEMYWEAPON=1): for each enemy this game runs, the Weapon_ actors within 3 m, and the offsets
// in each one's first 0x1000 bytes that hold the enemy's actor, its model or its body skeleton: how a held weapon
// knows its holder (the tester, 2026-11-06: an enemy puppet's weapon lags and can be taken from its hand).
typedef struct { uint64_t enemy; float at[3]; } WeapScan;
static void weap_one(uint64_t a, void *arg) {
    WeapScan *ws = arg;
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    if (strncmp(nm, "Weapon_", 7)) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float dx = p[0] - ws->at[0], dy = p[1] - ws->at[1], dz = p[2] - ws->at[2];
    if (dx * dx + dy * dy + dz * dz > 9) return;
    uint64_t model = enemy_model(ws->enemy), sk = enemy_skeleton(ws->enemy);
    static uint64_t w[0x1000 / 8];
    if (!coop_rd(a, w, sizeof w)) return;
    char line[600];
    int m = 0;
    for (int i = 0; i < 0x1000 / 8 && m < 560; i++) {
        uint64_t v = w[i];
        if (v == ws->enemy) m += snprintf(line + m, sizeof line - m, " +%#x=actor", i * 8);
        else if (v && v == model) m += snprintf(line + m, sizeof line - m, " +%#x=model", i * 8);
        else if (v && v == sk) m += snprintf(line + m, sizeof line - m, " +%#x=skeleton", i * 8);
        else if (v > ws->enemy && v < ws->enemy + 0x1000) m += snprintf(line + m, sizeof line - m, " +%#x=actor+%#llx", i * 8, (unsigned long long)(v - ws->enemy));
    }
    line[m] = 0;
    char en[48];
    rt_log("coop: test: enemy weapon: %s %llx, %.2f m from %s %llx:%s", nm, (unsigned long long)a, sqrtf(dx * dx + dy * dy + dz * dz),
           coop_actor_name(ws->enemy, en, sizeof en), (unsigned long long)ws->enemy, m ? line : " (no pointer to it)");
}
// Test (TOTK_COOP_TEST_ENEMYHIT=1): the hit record on each enemy this game runs (as pup_hits logs it on a frozen
// one), with its life then and 0.3 s later: what the record's numbers are in life.
static void enemy_hit_test(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ENEMYHIT") != NULL;
    if (!on) return;
    static struct { uint64_t a; uint32_t st; int life, wait, said; } eh[16];
    for (int i = 0; i < KNOWN_N; i++) {
        uint64_t a = mine_known[i].actor;
        if (!mine_known[i].key || !a) continue;
        int s = (int)(mine_known[i].key % 16);
        if (eh[s].a != a) memset(&eh[s], 0, sizeof eh[s]), eh[s].a = a;
        uint64_t dc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2f8);
        uint32_t w[0x40 / 4];
        if (!dc || !coop_rd(dc + 0x1a0, w, sizeof w)) continue;
        if (eh[s].wait && --eh[s].wait == 0)
            rt_log("coop: test: enemy %08x life %d -> %d 0.3 s after the hit", mine_known[i].key, eh[s].life, life_of(a));
        if (w[1] && w[1] != eh[s].st && eh[s].said++ < 20) {
            uint64_t ref;
            memcpy(&ref, &w[0x20 / 4], 8);
            uint64_t att = ref ? coop_rd_ptr(ref + 0x40) : 0;
            char nm[48] = "";
            if (att) coop_actor_name(att, nm, sizeof nm);
            eh[s].life = life_of(a), eh[s].wait = 9;
            rt_log("coop: test: enemy %08x (run here) was hit by %s: word %d, flags %#x, life %d", mine_known[i].key, nm, (int)w[2], w[0x30 / 4], eh[s].life);
        }
        eh[s].st = w[1];
    }
}
static void enemy_weapon_test(void) {
    static int on = -1, t, said;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ENEMYWEAPON") != NULL;
    if (!on || ++t % 300 || said++ > 6) return;
    for (int i = 0; i < KNOWN_N; i++) {
        if (!mine_known[i].key || !mine_known[i].actor) continue;
        WeapScan ws = {mine_known[i].actor};
        if (!coop_rd(ws.enemy + COOP_ACTOR_POS, ws.at, 12)) continue;
        coop_actors_live(weap_one, &ws);
    }
}
// The g3d skeletons reachable from an enemy whose skeleton enemy_skeleton does not find (pointers followed up to 6
// deep, 0x400 bytes each, from its model and from the actor), with the offsets on the way, logged once per enemy name
// (up to 4 names).
static void enemy_skel_walk(const char *who, uint64_t obj, int depth, const char *path, int *found) {
    if (depth > 6 || *found >= 8) return;
    uint64_t buf[0x400 / 8];
    if (!coop_rd(obj, buf, sizeof buf)) return;
    for (int i = 0; i < 0x400 / 8 && *found < 6; i++) {
        uint64_t q = buf[i];
        if (q < 0x100000000ull || q >= 0x8000000000ull || !first_visit(q)) continue;
        char p2[160];
        snprintf(p2, sizeof p2, "%s+%#x", path, i * 8);
        if (is_skeleton(q)) {
            uint16_t n = 0;
            coop_rd(q + 0x48, &n, 2);
            rt_log("coop: test: enemy %s skeleton at %s -> %llx: %u bones", who, p2, (unsigned long long)q, n);
            (*found)++;
            continue;
        }
        enemy_skel_walk(who, q, depth + 1, p2, found);
    }
}
static void enemy_skel_probe(uint64_t a) {
    static char done[4][48];
    static int nd;
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    for (int i = 0; i < nd; i++) if (!strcmp(done[i], nm)) return;
    if (nd >= 4) return;
    snprintf(done[nd++], sizeof done[0], "%s", nm);
    memset(visited, 0, sizeof visited);
    int found = 0;
    uint64_t model = enemy_model(a);
    if (model) enemy_skel_walk(nm, model, 3, "model", &found);   // from its model (actor +0x228 +0x8 +0x28), 4 deep
    memset(visited, 0, sizeof visited);
    enemy_skel_walk(nm, a, 0, "actor", &found);
    rt_log("coop: test: enemy %s: %d skeletons found (model %llx)", nm, found, (unsigned long long)model);
}
// Each enemy kind's bone parents (as find_parents finds a Link's): for each bone, the earlier bone whose final matrix
// times this bone's record gives this bone's final matrix most closely. Kept by name and bone count once 3/4 of the
// bones match within 2 mm; tried again every 30 calls until then.
#define EPAR_N 24
static struct { uint32_t name; int n, tries, ok; int16_t par[PUP_BONES]; } epar[EPAR_N];
static PlatMutex epar_lock = PLAT_MUTEX_INIT;
static const int16_t *enemy_parents(uint64_t a, int n, const float (*w)[16], const float (*rec)[24]) {
    uint32_t nh = name_hash_of(a);
    plat_mutex_lock(&epar_lock);
    int s = -1;
    for (int i = 0; i < EPAR_N && s < 0; i++) if (epar[i].name == nh && epar[i].n == n) s = i;
    for (int i = 0; i < EPAR_N && s < 0; i++) if (!epar[i].name) s = i, epar[i].name = nh, epar[i].n = n;
    const int16_t *out = NULL;
    if (s >= 0 && !epar[s].ok && epar[s].tries++ % 30 == 0) {
        int good = 0;
        for (int i = 1; i < n; i++) {
            int best = 0;
            float be = 1e9f;
            for (int p = 0; p < i; p++) {
                float e = fk_error(w, rec, i, p);
                if (e < be) be = e, best = p;
            }
            epar[s].par[i] = (int16_t)best, good += be < 2e-3f;
        }
        epar[s].par[0] = -1;
        epar[s].ok = good >= (n - 1) * 3 / 4;
        char nm[48];
        if (epar[s].ok || epar[s].tries % 300 == 1)
            rt_log("coop: enemy %s: bone parents %s: %d of %d bones match their parent within 2 mm", coop_actor_name(a, nm, sizeof nm),
                   epar[s].ok ? "found" : "not yet", good, n - 1);
    }
    if (s >= 0 && epar[s].ok) out = epar[s].par;
    plat_mutex_unlock(&epar_lock);
    return out;
}
// The owner: an enemy's place and its skeleton's records packed into out; its length, 0 when it has no skeleton found.
static int epose_pack_native(uint64_t a, uint32_t key, uint8_t *out, int max);
static int epose_pack(uint64_t a, uint32_t key, uint8_t *out, int max) {
    if (enemy_native()) return epose_pack_native(a, key, out, max);
    EPoseHead h = {.key = key, .t_ns = plat_now_ns()};
    {
        uint64_t aw = coop_rd_ptr(coop_rd_ptr(a + 0x228) + AWARE_COMP);
        int ok = aw != 0;
        for (int k = 0; ok && k < AWARE_N; k++) ok = coop_rd(aw + AWARE_OFF[k], &h.aware[k], 4);
        h.has_aware = (uint8_t)ok;
        // Test (TOTK_COOP_TEST_AWARELOG=1): each change of an enemy's awareness words where it runs, so a test can
        // tell when it noticed someone (0xc082 aware, 0xc182 not; pup_aware_apply).
        static int on = -1;
        if (on < 0) { const char *e = cfg_get("TOTK_COOP_TEST_AWARELOG"); on = e && *e == '1'; }
        if (on && ok) {
            static PlatMutex lk = PLAT_MUTEX_INIT;
            static struct { uint32_t key, w0, w5; } seen[32];
            static int said;
            plat_mutex_lock(&lk);
            int s = -1;
            for (int i = 0; i < 32 && s < 0; i++) if (seen[i].key == key || !seen[i].key) s = i;
            if (s >= 0 && (seen[s].key != key || seen[s].w0 != h.aware[0] || seen[s].w5 != h.aware[AWARE_N - 1]) && said++ < 200) {
                char nm[48];
                float p[3] = {0};
                coop_rd(a + COOP_ACTOR_POS, p, 12);
                rt_log("coop: test: my enemy %s %08x at %.1f %.1f %.1f: awareness %x %x %x %x %x %x", coop_actor_name(a, nm, sizeof nm), key,
                       p[0], p[1], p[2], h.aware[0], h.aware[1], h.aware[2], h.aware[3], h.aware[4], h.aware[5]);
                seen[s].key = key, seen[s].w0 = h.aware[0], seen[s].w5 = h.aware[AWARE_N - 1];
            }
            plat_mutex_unlock(&lk);
        }
    }
    uint64_t sk = enemy_skeleton(a), recs = sk ? coop_rd_ptr(sk + 0x18) : 0;
    uint16_t n = 0;
    static __thread float rec[PUP_BONES][24];
    static __thread float fin[PUP_BONES][16];
    uint64_t wmx = sk ? coop_rd_ptr(sk + 0x20) : 0;
    if (!recs || !wmx || !coop_rd(sk + 0x48, &n, 2) || n == 0 || n > PUP_BONES || (int)sizeof h + 4 + n * POSE_BONE > max ||
        !coop_rd(recs, rec, (size_t)n * 0x60) || !coop_rd(wmx, fin, (size_t)n * 0x40) || !coop_rd(a + COOP_ACTOR_POS, h.pos, 12) ||
        !coop_rd(a + COOP_ACTOR_ROT, h.rot, 36)) {
        static int said;
        char nm[64];
        if (said++ < 6) rt_log("coop: enemy %s %08x: no body skeleton found (skeleton %llx, %u bones)", coop_actor_name(a, nm, sizeof nm), key,
                               (unsigned long long)sk, n);
        if (!sk) enemy_skel_probe(a);
        return 0;
    }
    // Each record from the final matrices (R = Ap^-1 Ai S^-1, t = Ap^-1 (ti - tp)), as coop_proxy_pack_mine does for
    // a Link: the records alone miss what later steps put on the final matrices (feet on the ground), and may be in
    // the middle of being rebuilt when read (sent as they were, the copy jerked 4 to 9 mm a frame where the enemy's
    // own body moved 1.5 to 2.5 mm; 2026-11-06). Off unless TOTK_COOP_EPOSE_FINAL=1: it did not lower that, and a
    // Chuchu's bones matched no parents (8 of 24), so its copy's shape went 0.1 m off.
    static int finals = -1;
    if (finals < 0) { const char *e = cfg_get("TOTK_COOP_EPOSE_FINAL"); finals = e && *e == '1'; }
    const int16_t *par = finals ? enemy_parents(a, n, (const float (*)[16])fin, (const float (*)[24])rec) : NULL;
    for (int i = 1; par && i < n; i++) {
        float Ap[3][3], tp[3], Ai[3][3], ti[3], Ip[3][3];
        w_of(fin[par[i]], Ap, tp), w_of(fin[i], Ai, ti);
        if (!inv3(Ap, Ip)) continue;
        for (int r = 0; r < 3; r++)
            for (int cl = 0; cl < 3; cl++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += Ip[r][k] * Ai[k][cl];
                float sc = rec[i][4 + cl];
                rec[i][8 + cl * 4 + r] = fabsf(sc) > 1e-6f ? v / sc : v;
            }
        for (int r = 0; r < 3; r++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += Ip[r][k] * (ti[k] - tp[k]);
            rec[i][20 + r] = v;
        }
    }
    // its weapons: the Weapon_ actors near it that are no Link's
    EItem items[EPOSE_ITEMS];
    for (int i = 0; i < nweaps && h.nitems < EPOSE_ITEMS; i++) {
        EItem it;
        int32_t wst = 0;
        if (!coop_rd(weaps[i] + COOP_ACTOR_STATE, &wst, 4) || wst < 2 || wst > 6 || !coop_rd(weaps[i] + COOP_ACTOR_POS, it.pos, 12) ||
            !coop_rd(weaps[i] + COOP_ACTOR_ROT, it.rot, 36))
            continue;
        float dx = it.pos[0] - h.pos[0], dy = it.pos[1] - h.pos[1], dz = it.pos[2] - h.pos[2];
        if (dx * dx + dy * dy + dz * dz > EPOSE_ITEM_M * EPOSE_ITEM_M) continue;
        it.hash = name_hash_of(weaps[i]);
        {   // Test (TOTK_COOP_TEST_WEAPLAG=1): how far the weapon's actor place is from where its model is drawn
            // (model +0x1f8, rows | place), and how far the model moved since the last frame: every 120 samples.
            static int on = -1;
            if (on < 0) { const char *e = cfg_get("TOTK_COOP_TEST_WEAPLAG"); on = e && *e == '1'; }
            uint64_t wmod = on ? enemy_model(weaps[i]) : 0;
            float mm[12];
            if (wmod && coop_rd(wmod + 0x1f8, mm, sizeof mm)) {
                static PlatMutex lk = PLAT_MUTEX_INIT;
                static float last[3], sum_d, max_d, sum_v, max_v;
                static int n;
                static uint64_t last_w;
                plat_mutex_lock(&lk);
                float mp[3] = {mm[3], mm[7], mm[11]};
                float d = sqrtf((mp[0] - it.pos[0]) * (mp[0] - it.pos[0]) + (mp[1] - it.pos[1]) * (mp[1] - it.pos[1]) + (mp[2] - it.pos[2]) * (mp[2] - it.pos[2]));
                float v = last_w == weaps[i] ? sqrtf((mp[0] - last[0]) * (mp[0] - last[0]) + (mp[1] - last[1]) * (mp[1] - last[1]) + (mp[2] - last[2]) * (mp[2] - last[2])) : 0;
                memcpy(last, mp, 12), last_w = weaps[i];
                sum_d += d, sum_v += v, max_d = fmaxf(max_d, d), max_v = fmaxf(max_v, v);
                if (++n == 120) {
                    char wn[48];
                    rt_log("coop: test: weapon %s: actor place from its drawn place mean %.1f cm (max %.1f); drawn place moved %.1f cm a frame (max %.1f)",
                           coop_actor_name(weaps[i], wn, sizeof wn), sum_d / n * 100, max_d * 100, sum_v / n * 100, max_v * 100);
                    n = 0, sum_d = max_d = sum_v = max_v = 0;
                }
                plat_mutex_unlock(&lk);
            }
        }
        items[h.nitems++] = it;
    }
    int hl = (int)sizeof h + h.nitems * (int)sizeof(EItem);
    if (hl + 4 + n * POSE_BONE > max) return 0;
    memcpy(out, &h, sizeof h), memcpy(out + sizeof h, items, (size_t)h.nitems * sizeof(EItem));
    uint8_t *o = out + hl;
    int at = 4 + n * POSE_BONE, nsc = 0, lim = max - hl;
    for (int b = 0; b < n; b++) {
        const float *f = rec[b];
        float R[9] = {f[8], f[12], f[16], f[9], f[13], f[17], f[10], f[14], f[18]}, q[4];   // rows, from the record's columns
        mat_quat(R, q);
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        int big = 0;
        for (int k = 0; k < 4; k++) { q[k] = len > 0 ? q[k] / len : (k == 3); if (fabsf(q[k]) > fabsf(q[big])) big = k; }
        float sign = q[big] < 0 ? -1.0f : 1.0f;
        int16_t v[6];
        for (int k = 0, j = 0; k < 4; k++) if (k != big) v[j++] = q16(q[k] * sign, 32767.0f);
        v[0] = (int16_t)((v[0] & ~3) | big);
        v[3] = q16(f[20], 4096), v[4] = q16(f[21], 4096), v[5] = q16(f[22], 4096);
        memcpy(o + 4 + b * POSE_BONE, v, POSE_BONE);
        if (fabsf(f[4] - 1) > 1e-3f || fabsf(f[5] - 1) > 1e-3f || fabsf(f[6] - 1) > 1e-3f) {
            if (at + 14 > lim) return 0;
            uint16_t bi = (uint16_t)b;
            memcpy(o + at, &bi, 2), memcpy(o + at + 2, f + 4, 12), at += 14, nsc++;
        }
    }
    uint16_t hdr = (uint16_t)(n | 0x8000), ns = (uint16_t)nsc;
    memcpy(o, &hdr, 2), memcpy(o + 2, &ns, 2);
    return hl + at;
}
// The enemies whose pose goes out (epose_send_tick picks them), and their latest pose packed at their calc's start.
static struct {
    _Atomic uint64_t actor;
    uint32_t key;
    PlatMutex lock;
    uint8_t buf[COOP_EPOSE_MAX];
    int len, fresh;
} ewant[EPOSE_MAX] = {[0 ... EPOSE_MAX - 1] = {.lock = PLAT_MUTEX_INIT}};
static void jit_note(uint32_t key, int side, uint64_t sk);
static void epose_calc_start(uint64_t a) {
    static int tickpack = -1;
    if (tickpack < 0) tickpack = cfg_get("TOTK_COOP_EPOSE_TICKPACK") != NULL;
    for (int k = 0; k < EPOSE_MAX && !tickpack; k++) {
        if (atomic_load_explicit(&ewant[k].actor, memory_order_relaxed) != a) continue;
        static __thread uint8_t tmp[COOP_EPOSE_MAX];
        int len = epose_pack(a, ewant[k].key, tmp, sizeof tmp);
        jit_note(ewant[k].key, 0, enemy_skeleton(a));
        plat_mutex_lock(&ewant[k].lock);
        if (len && atomic_load(&ewant[k].actor) == a) memcpy(ewant[k].buf, tmp, len), ewant[k].len = len, ewant[k].fresh = 1;
        plat_mutex_unlock(&ewant[k].lock);
        return;
    }
}
// Jitter watch: an enemy's body top (its highest bone above the root, final matrices) frame to frame, in the game
// that runs it (side 0, at its calc's start) and on its frozen copy (side 1, at its drive's start: the last frame's
// result). The mean of the change's change (jerk) per frame, logged every 300 frames: smooth motion keeps it small.
static void jit_note(uint32_t key, int side, uint64_t sk) {
    static struct { uint32_t key; float a, b, pa[3], pb[3]; int have; double sum, psum, bsum; float max, pmax, bmax; int n;
                    float ba[PUP_BONES][3], bb[PUP_BONES][3]; int nb; } jt[2][8];
    static PlatMutex jl = PLAT_MUTEX_INIT;
    uint64_t wm = sk ? coop_rd_ptr(sk + 0x20) : 0;
    uint16_t nb = 0;
    static __thread float w[PUP_BONES][16];
    if (!wm || !coop_rd(sk + 0x48, &nb, 2) || !nb || nb > PUP_BONES || !coop_rd(wm, w, (size_t)nb * 0x40)) return;
    float top = -1e9f, p[3] = {0};
    for (int b = 1; b < nb; b++) if (w[b][13] - w[0][13] > top) top = w[b][13] - w[0][13];
    for (int k = 0; k < 3; k++) p[k] = w[0][12 + k];   // the root bone's place (the actor's, as the model has it)
    plat_mutex_lock(&jl);
    int i = (int)(key % 8);
    if (jt[side][i].key != key) memset(&jt[side][i], 0, sizeof jt[side][i]), jt[side][i].key = key;
    if (jt[side][i].have >= 2) {
        float j = fabsf(top - 2 * jt[side][i].b + jt[side][i].a);
        jt[side][i].sum += j, jt[side][i].n++;
        if (j > jt[side][i].max) jt[side][i].max = j;
        float pj = 0;
        for (int k = 0; k < 3; k++) { float d = p[k] - 2 * jt[side][i].pb[k] + jt[side][i].pa[k]; pj += d * d; }
        pj = sqrtf(pj), jt[side][i].psum += pj;
        if (pj > jt[side][i].pmax) jt[side][i].pmax = pj;
        if (jt[side][i].nb == nb) {   // every bone's place from the root, its change's change: the mean over the bones
            double s = 0;
            for (int b = 1; b < nb; b++) {
                float e = 0;
                for (int k = 0; k < 3; k++) {
                    float v = w[b][12 + k] - w[0][12 + k], d = v - 2 * jt[side][i].bb[b][k] + jt[side][i].ba[b][k];
                    e += d * d;
                }
                s += sqrtf(e);
            }
            float m = (float)(s / (nb - 1));
            jt[side][i].bsum += m;
            if (m > jt[side][i].bmax) jt[side][i].bmax = m;
        }
        if (jt[side][i].n == 300) {
            rt_log("coop: enemy %08x jitter %s: bones' jerk per frame mean %.1f mm (worst frame %.1f); body top %.1f mm (max %.1f)", key,
                   side ? "on its frozen copy" : "where it runs", jt[side][i].bsum / 300 * 1000, jt[side][i].bmax * 1000,
                   jt[side][i].sum / 300 * 1000, jt[side][i].max * 1000);
            jt[side][i].sum = 0, jt[side][i].psum = 0, jt[side][i].bsum = 0, jt[side][i].n = 0, jt[side][i].max = 0, jt[side][i].pmax = 0,
            jt[side][i].bmax = 0;
        }
    }
    jt[side][i].a = jt[side][i].b, jt[side][i].b = top, jt[side][i].have++;
    memcpy(jt[side][i].pa, jt[side][i].pb, 12), memcpy(jt[side][i].pb, p, 12);
    memcpy(jt[side][i].ba, jt[side][i].bb, sizeof jt[side][i].ba);
    for (int b = 0; b < nb; b++) for (int k = 0; k < 3; k++) jt[side][i].bb[b][k] = w[b][12 + k] - w[0][12 + k];
    jt[side][i].nb = nb;
    plat_mutex_unlock(&jl);
}
static void epose_send_tick(void) {
    if (!enemy_puppets_on()) return;
    static unsigned frame;
    frame++;
    int idx[KNOWN_N], n = 0;
    for (int i = 0; i < KNOWN_N; i++)
        if (mine_known[i].key && mine_known[i].actor && mine_known[i].life > 0 && mine_known[i].d2 < SYNC_M * SYNC_M) idx[n++] = i;
    for (int i = 1; i < n; i++)   // nearest another Link first
        for (int j = i; j > 0 && mine_known[idx[j]].d2 < mine_known[idx[j - 1]].d2; j--) { int t = idx[j]; idx[j] = idx[j - 1], idx[j - 1] = t; }
    // The ones to send are packed at the start of their own calc (coop_enemy_puppet_calc, epose_calc_start), from the
    // last frame's records as its update left them: packed here, in this tick, they were read while the enemy's update
    // ran on a worker thread, some bones of this frame and some of the last, and its copy jiggled (a play session,
    // 2026-11-06).
    for (int k = 0; k < EPOSE_MAX; k++) {
        uint64_t a = k < n ? mine_known[idx[k]].actor : 0;
        if (atomic_load(&ewant[k].actor) != a) {
            plat_mutex_lock(&ewant[k].lock);
            atomic_store(&ewant[k].actor, a), ewant[k].key = a ? mine_known[idx[k]].key : 0, ewant[k].len = 0, ewant[k].fresh = 0;
            plat_mutex_unlock(&ewant[k].lock);
        }
    }
    static uint8_t buf[COOP_EPOSE_MAX];
    int sent = 0;
    for (int k = 0; k < n && k < EPOSE_MAX; k++) {
        int i = idx[k];
        if (mine_known[i].d2 > EPOSE_NEAR_M * EPOSE_NEAR_M && (frame + (unsigned)k) % 3) continue;
        int len = 0;
        static int tickpack = -1;   // TOTK_COOP_EPOSE_TICKPACK=1: packed here in the tick, as before (control)
        if (tickpack < 0) tickpack = cfg_get("TOTK_COOP_EPOSE_TICKPACK") != NULL;
        if (tickpack) {
            len = epose_pack(mine_known[i].actor, mine_known[i].key, buf, sizeof buf);
            jit_note(mine_known[i].key, 0, enemy_skeleton(mine_known[i].actor));
        } else {
            plat_mutex_lock(&ewant[k].lock);
            if (ewant[k].fresh && ewant[k].len) memcpy(buf, ewant[k].buf, ewant[k].len), len = ewant[k].len, ewant[k].fresh = 0;
            plat_mutex_unlock(&ewant[k].lock);
        }
        if (len) coop_net_epose_send(buf, len), sent++;
    }
    static int said;
    if (sent && !said++) rt_log("coop: enemy poses go out (%d enemies this frame)", sent);
}
static int pup_decode(const uint8_t *in, int len, PupRec *r) {
    EPoseHead h;
    if (len < (int)sizeof h) return 0;
    memcpy(&h, in, sizeof h);
    if (h.pad[0] & EPOSE_NATIVE) {   // the native model's: its place and rotation only (the action: epose_take_tick)
        memcpy(r->pos, h.pos, 12), mat_quat(h.rot, r->q), r->src_ns = h.t_ns, r->n = 0, r->ni = 0;
        return 1;
    }
    if (len < (int)sizeof h + 4) return 0;
    in += sizeof h, len -= (int)sizeof h;
    if (h.nitems > EPOSE_ITEMS || len < h.nitems * (int)sizeof(EItem) + 4) return 0;
    r->ni = h.nitems;
    for (int k = 0; k < h.nitems; k++) {
        EItem it;
        memcpy(&it, in + k * sizeof it, sizeof it);
        r->ih[k] = it.hash, memcpy(r->ip[k], it.pos, 12), mat_quat(it.rot, r->iq[k]);
    }
    in += h.nitems * sizeof(EItem), len -= h.nitems * (int)sizeof(EItem);
    uint16_t n = 0, nsc = 0;
    memcpy(&n, in, 2), memcpy(&nsc, in + 2, 2);
    n &= 0x7fff;
    if (n == 0 || n > PUP_BONES || 4 + n * POSE_BONE > len) return 0;
    memcpy(r->pos, h.pos, 12), mat_quat(h.rot, r->q), r->src_ns = h.t_ns;
    for (int b = 0; b < n; b++) {
        int16_t v[6];
        memcpy(v, in + 4 + b * POSE_BONE, POSE_BONE);
        int big = v[0] & 3;
        float sum = 0;
        for (int k = 0, j = 0; k < 4; k++) if (k != big) { r->bq[b][k] = v[j++] / 32767.0f; sum += r->bq[b][k] * r->bq[b][k]; }
        r->bq[b][big] = sum < 1 ? sqrtf(1 - sum) : 0;
        r->bt[b][0] = v[3] / 4096.0f, r->bt[b][1] = v[4] / 4096.0f, r->bt[b][2] = v[5] / 4096.0f;
        r->bs[b][0] = r->bs[b][1] = r->bs[b][2] = 1;
    }
    for (int k = 0; k < nsc; k++) {
        int at = 4 + n * POSE_BONE + k * 14;
        if (at + 14 > len) break;
        uint16_t bi = 0;
        memcpy(&bi, in + at, 2);
        if (bi < n) memcpy(r->bs[bi], in + at + 2, 12);
    }
    r->n = n;
    return 1;
}
// Poses that came: into their enemy's history (a slot taken for a new one when this game follows that owner for it).
// A weapon let go: its body (kept 300 m under it) put back where it is, then its own update runs again.
static void weap_let_go(Cpu *c, uint64_t w) {
    int32_t st = 0;
    _Alignas(16) float m[12], p[3], r[9];
    if (!coop_rd(w + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || !coop_rd(w + COOP_ACTOR_POS, p, 12) || !coop_rd(w + COOP_ACTOR_ROT, r, 36)) return;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) m[i * 4 + k] = r[i * 3 + k];
        m[i * 4 + 3] = p[i];
    }
    uint64_t body = enemy_body(w);
    if (body) coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, body, (uint64_t)(uintptr_t)m, 0, 0);
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, w, (uint64_t)(uintptr_t)m, 0, 0);
}
static int weap_bound(uint64_t w) {
    for (int i = 0; i < PUP_N; i++)
        for (int j = 0; j < EPOSE_ITEMS; j++) if (atomic_load_explicit(&pups[i].weap[j], memory_order_relaxed) == w) return 1;
    return 0;
}
// A ready enemy puppet's weapons: each one the owner sends bound to this game's nearest of that name; one no longer
// sent let go.
static void pup_weapons(Cpu *c, int k) {
    const PupRec *r = pups[k].count ? &pups[k].r[(pups[k].count - 1) % PUP_HIST] : NULL;
    for (int j = 0; j < EPOSE_ITEMS; j++) {   // let go: no longer sent, or gone
        uint64_t w = atomic_load(&pups[k].weap[j]);
        if (!w) continue;
        int sent = 0;
        for (int q = 0; r && q < r->ni; q++) sent |= r->ih[q] == pups[k].weap_hash[j];
        int32_t st = 0;
        int alive = coop_rd(w + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6;
        if (sent && alive) continue;
        atomic_store(&pups[k].weap[j], 0);
        if (alive) weap_let_go(c, w);
        rt_log("coop: enemy %08x: its weapon %llx is its own again (%s)", pups[k].key, (unsigned long long)w, alive ? "no longer sent" : "gone");
    }
    for (int q = 0; r && q < r->ni; q++) {
        int have = 0, free_j = -1;
        for (int j = 0; j < EPOSE_ITEMS; j++) {
            if (atomic_load(&pups[k].weap[j]) && pups[k].weap_hash[j] == r->ih[q]) have = 1;
            if (free_j < 0 && !atomic_load(&pups[k].weap[j])) free_j = j;
        }
        if (have || free_j < 0) continue;
        uint64_t best = 0;
        float bd = 3.0f * 3.0f;
        for (int i = 0; i < nweaps; i++) {
            float p[3];
            if (name_hash_of(weaps[i]) != r->ih[q] || weap_bound(weaps[i]) || !coop_rd(weaps[i] + COOP_ACTOR_POS, p, 12)) continue;
            float dx = p[0] - r->ip[q][0], dy = p[1] - r->ip[q][1], dz = p[2] - r->ip[q][2], d = dx * dx + dy * dy + dz * dz;
            if (d < bd) bd = d, best = weaps[i];
        }
        if (!best) continue;
        pups[k].weap_hash[free_j] = r->ih[q];
        atomic_store(&pups[k].weap[free_j], best);
        char nm[48];
        rt_log("coop: enemy %08x: its weapon %s %llx (%.2f m from the owner's) is put in its hand from the owner's", pups[k].key,
               coop_actor_name(best, nm, sizeof nm), (unsigned long long)best, sqrtf(bd));
    }
}
// A hit on a frozen enemy here (it runs no update, so it takes none): the record the game leaves in its damage
// component (as on a proxy, link_hits_send). This Link's arrows and bombs (a hit by it without the melee flag
// 0x100000: a sword's had 0x180001, an arrow's 0x80000; 2026-11-06) go to its owner as a hit of the record's damage
// (+0x1a8: on a Link the quarter hearts lost; 90 for a normal arrow, 45 for the Master Sword here). A sword's is not
// sent: the proxy's swing in the owner's game lands it there already.
static uint64_t pup_me;   // this Link (peer_tick)
static void pup_hits(int k) {
    static uint32_t last[PUP_N];
    static uint64_t last_a[PUP_N];
    uint64_t a = atomic_load(&pups[k].actor);
    uint64_t dc = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2f8) : 0;
    uint32_t w[0x40 / 4];
    if (!dc || !coop_rd(dc + 0x1a0, w, sizeof w)) return;
    uint32_t st = w[1], was = last_a[k] == a ? last[k] : st;
    last[k] = st, last_a[k] = a;
    if (!st || st == was) return;
    uint64_t ref;
    memcpy(&ref, &w[0x20 / 4], 8);
    uint64_t att = ref ? coop_rd_ptr(ref + 0x40) : 0;
    char nm[64] = "";
    if (att) coop_actor_name(att, nm, sizeof nm);
    int dmg = (int)w[2], melee = (w[0x30 / 4] & 0x100000) != 0, mine = att && att == pup_me;
    int send = mine && !melee && dmg > 0 && dmg <= 65535;
    static int said;
    if (said++ < 40)
        rt_log("coop: enemy %08x (frozen here) was hit by %s for %d (flags %#x)%s", pups[k].key, mine ? "this Link" : nm[0] ? nm : "?", dmg,
               w[0x30 / 4], send ? ", sent to its owner" : melee && mine ? " (a sword: its owner's game has it from the proxy)" : "");
    if (send) {
        HitPacket hp = {pups[k].key, dmg, (uint32_t)pups[k].owner};
        coop_net_hit_send(&hp, sizeof hp);
    }
}
// The owner's awareness words onto each frozen copy (EPoseHead's aware). TOTK_COOP_ENEMY_AWARE=0: not.
static void pup_aware_apply(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_ENEMY_AWARE"); on = !(e && *e == '0'); }
    for (int k = 0; on && k < PUP_N; k++) {
        uint64_t a = atomic_load(&pups[k].ready) && pups[k].have_aware ? atomic_load(&pups[k].actor) : 0;
        uint64_t aw = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + AWARE_COMP) : 0;
        if (!aw) continue;
        for (int j = 0; j < AWARE_N; j++) memcpy((void *)(uintptr_t)(aw + AWARE_OFF[j]), &pups[k].aware[j], 4);
        static uint32_t last[AWARE_N];
        static uint64_t last_ns;
        uint64_t now = plat_now_ns();
        if (memcmp(last, pups[k].aware, sizeof last) || now - last_ns > 2000000000ull) {   // a change, or every 2 s
            memcpy(last, pups[k].aware, sizeof last), last_ns = now;
            static int said;
            if (said++ < 60)
                rt_log("coop: enemy %08x awareness from its owner at %.1f s: %x %x %x %x %x %x", pups[k].key, frame_step_world_us() / 1e6,
                       pups[k].aware[0], pups[k].aware[1], pups[k].aware[2], pups[k].aware[3], pups[k].aware[4], pups[k].aware[5]);
        }
    }
}
// ---- the native enemy model (TotK Online's way, read for behaviour only; docs/coop/proxy-plan.md). An enemy another
// game runs is not frozen here: its own update and AI run, it is eased to the owner's place each tick (p += (owner's -
// p) * min(10 * dt, 1), the rotation set), its life is the owner's (peer_one), and whenever its AI makes an action
// request (main+0x50ad74, 1.2.1 main+0xa29340: tools/coop/xver_match.py and the code), the owner's last one is put in
// its place. So the copy plays the owner's actions with its own animation, and holds its own weapon.
#define ENEMY_UPDATE_TRANS 0x2af6004   // engine::actor::ActorBase::updateTrans(actor, const Vector3 &, int), 1.2.1 0xe8daf0
#define ENEMY_UPDATE_ROT 0x2af6094     // engine::actor::ActorBase::updateRotation(actor, const Matrix33 &, int), 1.2.1 0xe655d8
static void native_follow(Cpu *c, int k) {
    uint64_t a = atomic_load(&pups[k].actor);
    int32_t st = 0;
    float p[3];
    if (!a || !coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    plat_mutex_lock(&pup_lock);
    const PupRec *r = &pups[k].r[(pups[k].count - 1) % PUP_HIST];
    float t[3], q[4];
    memcpy(t, r->pos, 12), memcpy(q, r->q, 16);
    plat_mutex_unlock(&pup_lock);
    static uint64_t last[PUP_N];
    uint64_t now = plat_now_ns();
    float dt = last[k] ? (float)((now - last[k]) / 1e9) : 1.0f / 30;
    last[k] = now;
    if (dt > 1) dt = 1;
    // (all the way when it is over SNAP_M off: knocked back in its owner's game, the copy, whose sword hits are taken
    // out here (contacts_filter), was over 2 m behind for a few seconds, the sync audit, 2026-11-07)
    float gap2 = (t[0] - p[0]) * (t[0] - p[0]) + (t[1] - p[1]) * (t[1] - p[1]) + (t[2] - p[2]) * (t[2] - p[2]);
    float f = gap2 > SNAP_M * SNAP_M ? 1 : fminf(10 * dt, 1);
    _Alignas(16) float np[4] = {p[0] + (t[0] - p[0]) * f, p[1] + (t[1] - p[1]) * f, p[2] + (t[2] - p[2]) * f, 0};
    _Alignas(16) float m[9];
    quat_mat(q, m);
    coop_guest_call(c, MOD_MAIN_BASE + ENEMY_UPDATE_ROT, a, (uint64_t)(uintptr_t)m, 0, 0);
    coop_guest_call(c, MOD_MAIN_BASE + ENEMY_UPDATE_TRANS, a, (uint64_t)(uintptr_t)np, 0, 0);
    static int said;
    if (!said++) rt_log("coop: native enemies: %08x eased to its owner's place (%.2f m off)", pups[k].key,
                        sqrtf((t[0] - p[0]) * (t[0] - p[0]) + (t[1] - p[1]) * (t[1] - p[1]) + (t[2] - p[2]) * (t[2] - p[2])));
}
// The owner's side: each enemy's last action request, by actor (the hook runs on the enemies' threads).
static struct { uint64_t actor; uint32_t nh; EAction a; uint64_t ns; } act_mine[64];   // nh: its name's hash
static PlatMutex act_lock = PLAT_MUTEX_INIT;
static int epose_pack_native(uint64_t a, uint32_t key, uint8_t *out, int max) {
    EPoseHead h = {.key = key, .t_ns = plat_now_ns()};
    if (!coop_rd(a + COOP_ACTOR_POS, h.pos, 12) || !coop_rd(a + COOP_ACTOR_ROT, h.rot, 36)) return 0;
    h.pad[0] = EPOSE_NATIVE;
    int len = (int)sizeof h;
    plat_mutex_lock(&act_lock);
    uint32_t nh = name_hash_of(a);
    for (int i = 0; i < 64; i++)   // (the same actor, not another at its address: its name, and an action of the last 2 s)
        if (act_mine[i].actor == a && act_mine[i].nh == nh && h.t_ns - act_mine[i].ns < 2000000000ull &&
            len + (int)sizeof(EAction) <= max) {
            memcpy(out + sizeof h, &act_mine[i].a, sizeof(EAction)), len += (int)sizeof(EAction), h.pad[0] |= EPOSE_ACTION;
            break;
        }
    plat_mutex_unlock(&act_lock);
    memcpy(out, &h, sizeof h);
    return len;
}
// main+0x50ad74 (x0 an object whose +0x18 is the actor, x1 the 0x58-byte request, w2 its mode): on this game's enemy
// the request is kept to be sent; on another game's (a copy, native model) the owner's last one replaces it, unless
// the copy's own state bit (component 8 -> +0x18 -> +0x18, byte +0x124 bit 5: TotK Online's escape, meaning unknown)
// has been set for over 0.25 s. A dead enemy (life 0) plays its own.
#define ENEMY_ACTION 0x50ad74
void coop_enemy_action(Cpu *c) {
    if (!enemy_native() || !coop_net_on()) return;
    uint64_t obj = c->x[0], req = c->x[1], a = obj ? coop_rd_ptr(obj + 0x18) : 0;
    if (!a || !req) return;
    char nm[48];
    if (!coop_world_action_kind(coop_actor_name(a, nm, sizeof nm)) || life_of(a) == 0) return;
    static _Atomic int said_seen;
    if (atomic_fetch_add(&said_seen, 1) < 3) rt_log("coop: native enemies: %s makes an action request (mode %u)", nm, (unsigned)c->x[2]);
    // a copy: the owner's
    for (int k = 0; k < PUP_N; k++) {
        if (atomic_load_explicit(&pups[k].actor, memory_order_relaxed) != a) continue;
        plat_mutex_lock(&pup_lock);
        int have = pups[k].has_act && pups[k].count && plat_now_ns() - pups[k].last_ns < PUP_STALE_NS;
        // the escape: the copy's own state while its bit has been set 0.25 s
        uint64_t s = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 8 * 8) + 0x18) + 0x18);
        uint8_t b = 0;
        uint64_t now = plat_now_ns();
        int bit = s && coop_rd(s + 0x124, &b, 1) && (b & 0x20);
        if (bit && pups[k].esc_bit && pups[k].esc_ns) pups[k].esc_s += (float)((now - pups[k].esc_ns) / 1e9);
        else if (!bit) pups[k].esc_s = 0;
        pups[k].esc_bit = bit, pups[k].esc_ns = now;
        if (have && pups[k].esc_s <= 0.25f) {
            memcpy((void *)(uintptr_t)req, pups[k].act.req, EACT_REQ);
            if (pups[k].act.has_name) {
                memcpy(pups[k].act_name, pups[k].act.name, sizeof pups[k].act_name);
                pups[k].act_name[31] = 0;
                uint64_t np = (uint64_t)(uintptr_t)pups[k].act_name;
                memcpy((void *)(uintptr_t)(req + 0x38), &np, 8);
            }
            c->x[2] = pups[k].act.mode;
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 6)
                rt_log("coop: native enemies: %s %08x plays its owner's action %s (id %u, mode %u)", nm, pups[k].key,
                       pups[k].act.has_name ? pups[k].act.name : "-", *(uint32_t *)(pups[k].act.req + 0x4c), pups[k].act.mode);
        }
        plat_mutex_unlock(&pup_lock);
        return;
    }
    // this game's: kept (it goes out with the enemy's next pose packet, epose_pack_native)
    EAction e = {.mode = (uint8_t)c->x[2]};
    coop_rd(req, e.req, EACT_REQ);
    uint64_t np = 0;
    uint8_t has = 0;
    memcpy(&np, e.req + 0x38, 8), has = e.req[0x40];
    if (has == 1 && np) {
        for (int i = 0; i < 31; i++) {
            char ch = 0;
            if (!coop_rd(np + (uint64_t)i, &ch, 1) || !ch) break;
            e.name[i] = ch;
        }
        e.has_name = 1;
    }
    memset(e.req + 0x38, 0, 8);   // (a pointer of this game's: the copy's points to its own copy of the name)
    plat_mutex_lock(&act_lock);
    int f = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < 64; i++) {
        if (act_mine[i].actor == a) { f = i; break; }
        if (act_mine[i].ns < oldest) oldest = act_mine[i].ns, f = i;
    }
    act_mine[f].actor = a, act_mine[f].nh = name_hash_of(a), act_mine[f].a = e, act_mine[f].ns = plat_now_ns();
    plat_mutex_unlock(&act_lock);
}
static void epose_take_tick(Cpu *c) {
    pup_aware_apply();
    for (int i = 0; i < weap_free_n; i++) if (!weap_bound(weap_free[i])) weap_let_go(c, weap_free[i]);
    weap_free_n = 0;
    static uint8_t in[COOP_EPOSE_MAX];
    int from = 0, len;
    uint64_t age = 0, now = plat_now_ns();
    while ((len = coop_net_epose_take(in, sizeof in, &from, &age)) != 0) {
        if (len < (int)sizeof(EPoseHead)) continue;
        uint32_t key;
        memcpy(&key, in, 4);
        int i = pup_find(key);
        if (i < 0) {
            Follow *f = NULL;
            for (int k = 0; k < FOLLOW_N; k++) if (follow[k].key == key) f = &follow[k];
            if (!f || f->owner != from) continue;   // not one this game follows that player for
            uint64_t oldest = UINT64_MAX;
            for (int k = 0; k < PUP_N; k++) {
                uint64_t l = pups[k].key ? pups[k].last_ns : 0;
                if (l < oldest) oldest = l, i = k;
            }
            if (pups[i].key && now - pups[i].last_ns < PUP_STALE_NS) continue;   // all in use
            if (pups[i].key) pup_release(pups[i].key);
            plat_mutex_lock(&pup_lock);
            pups[i].key = key, pups[i].owner = from, pups[i].count = 0, atomic_store(&pups[i].actor, 0);
            pups[i].has_act = 0, pups[i].esc_s = 0, pups[i].esc_ns = 0, pups[i].esc_bit = 0;
            plat_mutex_unlock(&pup_lock);
        }
        if (pups[i].owner != from) continue;
        plat_mutex_lock(&pup_lock);
        PupRec *r = &pups[i].r[pups[i].count % PUP_HIST];
        if (pup_decode(in, len, r)) {
            EPoseHead hh;
            memcpy(&hh, in, sizeof hh);
            pups[i].have_aware = hh.has_aware;
            if (hh.has_aware) memcpy(pups[i].aware, hh.aware, sizeof pups[i].aware);
            if ((hh.pad[0] & EPOSE_ACTION) && len >= (int)(sizeof hh + sizeof(EAction)))
                memcpy(&pups[i].act, in + sizeof hh, sizeof(EAction)), pups[i].has_act = 1;
            else if (hh.pad[0] & EPOSE_NATIVE) pups[i].has_act = 0;   // (none from the owner: the copy plays its own)
            // The owner's clock onto this one: the smallest (arrival - owner's time) seen, the packet that came
            // quickest, rising slowly so a drift between the clocks is followed.
            static int64_t off[COOP_MAX_PLAYERS + 1];
            static int off_ok[COOP_MAX_PLAYERS + 1];
            int64_t d = (int64_t)(now - age) - (int64_t)r->src_ns;
            if (from >= 1 && from <= COOP_MAX_PLAYERS) {
                if (!off_ok[from] || d < off[from]) off[from] = d, off_ok[from] = 1;
                else off[from] += (d - off[from]) / 1024;
                r->ns = (uint64_t)((int64_t)r->src_ns + off[from]);
            } else r->ns = now - age;
            pups[i].count++, pups[i].last_ns = now;
        }
        plat_mutex_unlock(&pup_lock);
    }
    // Ready: bound to this game's one, with two poses, the last one fresh.
    for (int k = 0; k < PUP_N; k++) {
        if (!pups[k].key) continue;
        int ok = atomic_load(&pups[k].actor) && pups[k].count >= 2 && now - pups[k].last_ns < PUP_STALE_NS;
        if (enemy_native()) {   // never frozen: its own update runs, eased to the owner's place (native_follow)
            if (ok) native_follow(c, k);
            ok = 0;
        }
        int was = atomic_load(&pups[k].ready);
        if (ok != was) {
            char nm[64] = "";
            uint64_t a = atomic_load(&pups[k].actor);
            if (a) coop_actor_name(a, nm, sizeof nm);
            rt_log("coop: enemy %s %08x %s", nm, pups[k].key,
                   ok ? "is posed from its owner's (its own update stops here)" : "no longer gets poses (its own update runs again)");
            atomic_store(&pups[k].ready, ok);
        }
        if (ok) pup_weapons(c, k), pup_hits(k);
        else
            for (int j = 0; j < EPOSE_ITEMS; j++) {
                uint64_t w = atomic_exchange(&pups[k].weap[j], 0);
                if (w) weap_let_go(c, w);
            }
        if (!ok && pups[k].count && now - pups[k].last_ns > 5 * PUP_STALE_NS) pup_release(pups[k].key);
    }
}
static int pup_of_actor(uint64_t a) {
    if (!a) return -1;
    for (int i = 0; i < PUP_N; i++)
        if (atomic_load_explicit(&pups[i].ready, memory_order_relaxed) && atomic_load_explicit(&pups[i].actor, memory_order_relaxed) == a) return i;
    return -1;
}
static int epose_delay_ms(void) {
    static int d = -1;
    if (d < 0) { const char *e = cfg_get("TOTK_COOP_ENEMY_DELAY_MS"); d = e ? atoi(e) : 100; }
    return d;
}
// The writer hook (main+0x283c750, x1 the skeleton): 1 when it is a frozen enemy's (the call is skipped).
static _Atomic uint64_t pup_skel[PUP_N];
int coop_enemy_puppet_skel(uint64_t skel, uint64_t lr) {
    if (!skel) return 0;
    for (int i = 0; i < PUP_N; i++)
        if (atomic_load_explicit(&pup_skel[i], memory_order_relaxed) == skel && atomic_load_explicit(&pups[i].ready, memory_order_relaxed)) {
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) == 0)
                rt_log("coop: an enemy puppet's own pose build skipped (main+0x283c750, from main+%#llx)", (unsigned long long)(lr - MOD_MAIN_BASE));
            return 1;
        }
    return 0;
}
static _Atomic uint64_t pup_model[PUP_N];
// Test: the other writers of a body skeleton's matrices (re-notes, write watch: main+0x289620, 0x2925e0, 0x43e450;
// and the animation binding main+0x2e9c80) called with an argument naming a frozen enemy's skeleton or model (or
// pointing into them): logged once each; TOTK_COOP_TEST_PUPSKIP=1 skips them.
int coop_enemy_puppet_writer(uint64_t addr, const uint64_t *x, uint64_t lr) {
    static int skip = -1;
    if (skip < 0) skip = cfg_get("TOTK_COOP_TEST_PUPSKIP") != NULL;
    for (int i = 0; i < PUP_N; i++) {
        if (!atomic_load_explicit(&pups[i].ready, memory_order_relaxed)) continue;
        uint64_t sk = atomic_load_explicit(&pup_skel[i], memory_order_relaxed), md = atomic_load_explicit(&pup_model[i], memory_order_relaxed);
        for (int k = 0; k < 4; k++) {
            uint64_t v = x[k];
            int hit = v && (v == sk || v == md);
            if (!hit && addr == MOD_MAIN_BASE + 0x2e9c80 && v) {   // a binding: [+8] +8 its model (coop_spawn.c's log)
                uint64_t a = coop_rd_ptr(v + 8), b = a ? coop_rd_ptr(a + 8) : 0;
                hit = b && (b == md || b == sk);
            }
            if (!hit) continue;
            static _Atomic uint32_t said[4];
            int w = addr == MOD_MAIN_BASE + 0x289620 ? 0 : addr == MOD_MAIN_BASE + 0x2925e0 ? 1 : addr == MOD_MAIN_BASE + 0x43e450 ? 2 : 3;
            if (atomic_fetch_add(&said[w], 1) == 0)
                rt_log("coop: test: main+%#llx is called on an enemy puppet (x%d its %s; from main+%#llx)%s", (unsigned long long)(addr - MOD_MAIN_BASE), k,
                       v == sk ? "skeleton" : v == md ? "model" : "model's binding", (unsigned long long)(lr - MOD_MAIN_BASE), skip ? ", skipped" : "");
            return skip;
        }
    }
    return 0;
}
static int pup_weapon_of(uint64_t a) {
    if (!a) return 0;
    for (int i = 0; i < PUP_N; i++)
        if (atomic_load_explicit(&pups[i].ready, memory_order_relaxed))
            for (int j = 0; j < EPOSE_ITEMS; j++) if (atomic_load_explicit(&pups[i].weap[j], memory_order_relaxed) == a) return 1;
    return 0;
}
// The calc hook: 1 when the actor is a frozen enemy puppet or one of its weapons (its calc is skipped).
int coop_enemy_puppet_calc(uint64_t actor) {
    if (pup_of_actor(actor) >= 0 || pup_weapon_of(actor)) return 1;
    epose_calc_start(actor);
    return 0;
}
// An update phase of an enemy puppet (1: taken; main+0x245030 is replaced by the drive, the others skipped).
int coop_enemy_puppet_phase(Cpu *c, uint64_t addr) {
    uint64_t a = c->x[0];
    int i = pup_of_actor(a);
    if (i < 0) return pup_weapon_of(a);   // a puppet's weapon: no phase (put in the hand in its enemy's drive)
    if (addr != MOD_MAIN_BASE + 0x245030) return 1;
    static __thread PupRec out;
    uint64_t t = plat_now_ns() - (uint64_t)epose_delay_ms() * 1000000ull;
    uint64_t sk = enemy_skeleton(a), recs = sk ? coop_rd_ptr(sk + 0x18) : 0;
    uint16_t have = 0;
    if (sk) coop_rd(sk + 0x48, &have, 2);
    jit_note(pups[i].key, 1, sk);
    atomic_store(&pup_skel[i], sk), atomic_store(&pup_model[i], enemy_model(a));
    int ok = 0;
    plat_mutex_lock(&pup_lock);
    unsigned cnt = pups[i].count, n = cnt < PUP_HIST ? cnt : PUP_HIST;
    if (pups[i].key && n) {
        const PupRec *A = NULL, *B = NULL, *newest = &pups[i].r[(cnt - 1) % PUP_HIST];
        if (t >= newest->ns || n == 1) A = B = newest;
        else
            for (unsigned k = 1; k < n && !A; k++) {
                const PupRec *pb = &pups[i].r[(cnt - k) % PUP_HIST], *pa = &pups[i].r[(cnt - k - 1) % PUP_HIST];
                if (pa->ns <= t) A = pa, B = pb;
            }
        if (!A) A = B = &pups[i].r[(cnt - n) % PUP_HIST];
        static int hold = -1;   // TOTK_COOP_TEST_EPOSE_HOLD=1: the first pose kept, unblended (is the jitter ours?)
        if (hold < 0) hold = cfg_get("TOTK_COOP_TEST_EPOSE_HOLD") != NULL;
        if (hold) {   // (a copy: the history's slots are written over by the next poses)
            static PupRec held[PUP_N];
            static uint32_t held_key[PUP_N];
            if (held_key[i] != pups[i].key) held[i] = *newest, held_key[i] = pups[i].key;
            A = B = &held[i];
        }
        float f = A != B && B->ns > A->ns && t > A->ns ? (float)((double)(t - A->ns) / (double)(B->ns - A->ns)) : 0;
        if (f > 1) f = 1;
        for (int k = 0; k < 3; k++) out.pos[k] = A->pos[k] + (B->pos[k] - A->pos[k]) * f;
        qblend(A->q, B->q, f, out.q);
        out.n = 0;
        if (A->n == B->n && A->n == have) {
            for (int b = 0; b < A->n; b++) {
                qblend(A->bq[b], B->bq[b], f, out.bq[b]);
                for (int k = 0; k < 3; k++) out.bt[b][k] = A->bt[b][k] + (B->bt[b][k] - A->bt[b][k]) * f, out.bs[b][k] = f < 0.5f ? A->bs[b][k] : B->bs[b][k];
            }
            out.n = A->n;
        }
        out.ni = 0;   // its weapons, as both poses have them
        for (int q = 0; q < B->ni; q++)
            for (int p = 0; p < A->ni; p++)
                if (A->ih[p] == B->ih[q]) {
                    out.ih[out.ni] = B->ih[q];
                    for (int k = 0; k < 3; k++) out.ip[out.ni][k] = A->ip[p][k] + (B->ip[q][k] - A->ip[p][k]) * f;
                    qblend(A->iq[p], B->iq[q], f, out.iq[out.ni]);
                    out.ni++;
                    break;
                }
        ok = 1;
    }
    plat_mutex_unlock(&pup_lock);
    if (!ok) return 1;
    float rot[9];
    quat_mat(out.q, rot);
    _Alignas(16) float m[12];
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) m[r * 4 + k] = rot[r * 3 + k];
        m[r * 4 + 3] = out.pos[r];
    }
    memcpy((void *)(uintptr_t)(a + COOP_ACTOR_POS), out.pos, 12);
    memcpy((void *)(uintptr_t)(a + COOP_ACTOR_ROT), rot, 36);
    uint64_t model = enemy_model(a);
    if (model) {
        memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        ST8(model + 0x240, LD8(model + 0x240) | 1);
    }
    if (recs && out.n)   // a local record (0x60): scale +0x10, the rotation's columns at +0x20 / +0x30 / +0x40, translation +0x50
        for (int b = 0; b < out.n; b++) {
            float *rec = (float *)(uintptr_t)(recs + (uint64_t)b * 0x60), R[9];
            quat_mat(out.bq[b], R);
            for (int k = 0; k < 3; k++) rec[4 + k] = out.bs[b][k], rec[20 + k] = out.bt[b][k];
            for (int cl = 0; cl < 3; cl++) for (int k = 0; k < 3; k++) rec[8 + cl * 4 + k] = R[k * 3 + cl];
        }
    if (model) coop_guest_call(c, MOD_MAIN_BASE + 0x24cc30, model, 0, 0, 0);
    uint64_t body = enemy_body(a);
    if (body) coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, body, (uint64_t)(uintptr_t)m, 0, 0);
    for (int j = 0; j < EPOSE_ITEMS; j++) {   // its weapons, after the push: in its hand as on the owner's
        uint64_t w = atomic_load(&pups[i].weap[j]);
        int q = -1;
        for (int k = 0; w && k < out.ni; k++) if (out.ih[k] == pups[i].weap_hash[j]) q = k;
        if (q < 0) continue;
        float wr[9];
        _Alignas(16) float wm[12];
        quat_mat(out.iq[q], wr);
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) wm[r * 4 + k] = wr[r * 3 + k];
            wm[r * 4 + 3] = out.ip[q][r];
        }
        memcpy((void *)(uintptr_t)(w + COOP_ACTOR_POS), out.ip[q], 12);
        memcpy((void *)(uintptr_t)(w + COOP_ACTOR_ROT), wr, 36);
        uint64_t wmod = enemy_model(w);
        {   // Test (TOTK_COOP_TEST_WEAPLAG=1): whether the weapon's drawn place (model +0x1f8) is still the one put
            // there a frame before when it is placed again (something else writing it: its own update), every 120.
            static int on = -1;
            if (on < 0) { const char *e = cfg_get("TOTK_COOP_TEST_WEAPLAG"); on = e && *e == '1'; }
            static float put[3];
            static uint64_t put_w;
            static float sum, mx, sum_step;
            static int n;
            float now[12];
            if (on && wmod && coop_rd(wmod + 0x1f8, now, sizeof now)) {
                if (put_w == w) {
                    float d = sqrtf((now[3] - put[0]) * (now[3] - put[0]) + (now[7] - put[1]) * (now[7] - put[1]) + (now[11] - put[2]) * (now[11] - put[2]));
                    float st = sqrtf((wm[3] - put[0]) * (wm[3] - put[0]) + (wm[7] - put[1]) * (wm[7] - put[1]) + (wm[11] - put[2]) * (wm[11] - put[2]));
                    sum += d, mx = fmaxf(mx, d), sum_step += st;
                    if (++n == 120) {
                        rt_log("coop: test: copy weapon: drawn place moved from where it was put %.1f cm on average (max %.1f) before it was put again; "
                               "put %.1f cm a frame from the last", sum / n * 100, mx * 100, sum_step / n * 100);
                        n = 0, sum = mx = sum_step = 0;
                    }
                }
                put[0] = wm[3], put[1] = wm[7], put[2] = wm[11], put_w = w;
            }
        }
        if (wmod) {
            memcpy((void *)(uintptr_t)(wmod + 0x1f8), wm, sizeof wm);
            ST8(wmod + 0x240, LD8(wmod + 0x240) | 1);
            coop_guest_call(c, MOD_MAIN_BASE + 0x24cc30, wmod, 0, 0, 0);
        }
        uint64_t wb = enemy_body(w);
        wm[7] -= 300.0f;   // its body out of reach: taken from the frozen enemy's hand, it was the player's
        if (wb) coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, wb, (uint64_t)(uintptr_t)wm, 0, 0);
    }
    static _Atomic int said, said_nb;
    if (atomic_fetch_add(&said, 1) == 0)
        rt_log("coop: an enemy puppet is driven (skeleton %llx, %u bones, model %llx, body %llx)", (unsigned long long)sk, have,
               (unsigned long long)model, (unsigned long long)body);
    if (!out.n && atomic_fetch_add(&said_nb, 1) < 4) rt_log("coop: an enemy puppet's pose has another bone count than its skeleton (%u here)", have);
    return 1;
}

void coop_world_tick(Cpu *c, uint64_t me) {
    {   // The enemy tables start again with a session, and when this Link becomes another actor (a save loaded): kept,
        // enemies made again at the same place within 5 s were still reported dead and died at once, and old lives
        // made hits that were not (code review, 2026-11-06).
        static int was_on;
        static uint64_t was_me;
        int on = coop_net_on();
        if ((on && !was_on) || (me && was_me && me != was_me)) {
            memset(killed, 0, sizeof killed), memset(mine_known, 0, sizeof mine_known), memset(follow, 0, sizeof follow);
            for (int i = 0; i < PUP_N; i++) if (pups[i].key) pup_release(pups[i].key);
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
    if (coop_world_role()) peer_tick(c, me);
    void coop_anim_slots_test(uint64_t me);   // (below)
    coop_anim_slots_test(me);
    void coop_anim_copy_test(Cpu *c, uint64_t me);   // (below)
    coop_anim_copy_test(c, me);
    void coop_eqdiff_test(uint64_t me);   // (below)
    coop_eqdiff_test(me);
    void coop_binds_test(uint64_t me);   // (below)
    coop_binds_test(me);
    if (coop_world_role()) ground_tick();
}

// An enemy another game runs (contacts_filter): a copy following its owner, or one this game has seen run elsewhere
// before it follows (pup_bind waits for its first 3 s here).
static int ct_is_copy(uint64_t a) {
    for (int k = 0; a && k < PUP_N; k++) if (atomic_load_explicit(&pups[k].actor, memory_order_relaxed) == a) return 1;
    uint32_t key = a ? key_of_actor(a) : 0;
    for (int k = 0; key && k < FOLLOW_N; k++) if (follow[k].key == key) return follow[k].owner != 0;
    return 0;
}

// Test (TOTK_COOP_TEST_ANIMSLOTS=1): the animation player TotK Online replays onto its stand-in (read for behaviour
// only; 1.2.1 layout, checked here on 1.4.3): [[[actor+0x228]+8]+0x28]+0x40, its slot count at +0x38 and slots of
// 0xb0 bytes from [+0x40]: animation index u16 +2, weight f32 +0x10, frame f32 +0x28. Logged every 0.5 s for this
// Link and each co-op stand-in.
static uint16_t anim_bind_off(uint64_t g, uint64_t b);
static uint64_t anim_graph_of(uint64_t a);
static void anim_slots_log(const char *who, uint64_t a) {
    uint64_t mc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 8), model = mc ? coop_rd_ptr(mc + 0x28) : 0;
    uint64_t ap = model ? coop_rd_ptr(model + 0x40) : 0, arr = ap ? coop_rd_ptr(ap + 0x40) : 0;
    int32_t n = -1;
    if (ap) coop_rd(ap + 0x38, &n, 4);
    char line[700];
    int m = snprintf(line, sizeof line, "coop: test: anim slots of %s: model %llx player %llx (vtable main+%#llx) count %d:", who,
                     (unsigned long long)model, (unsigned long long)ap, (unsigned long long)(coop_rd_ptr(ap) - MOD_MAIN_BASE), n);
    {   // TotK Online's player for its play call: [G+0x28], G = [[[actor+0x228]+0x40]+0x18]+0x18 (a guess at its chain)
        uint64_t c8 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0, g = x ? coop_rd_ptr(x + 0x18) : 0;
        uint64_t p = g ? coop_rd_ptr(g + 0x28) : 0, pv = p ? coop_rd_ptr(p) : 0;
        rt_log("coop: test: anim chain of %s: comp8 %llx (vt main+%#llx) +0x18 %llx (vt main+%#llx) G %llx (vt main+%#llx) [G+0x28] %llx (vt main+%#llx, +0x30 main+%#llx)%s",
               who, (unsigned long long)c8, (unsigned long long)(coop_rd_ptr(c8) - MOD_MAIN_BASE), (unsigned long long)x,
               (unsigned long long)(coop_rd_ptr(x) - MOD_MAIN_BASE), (unsigned long long)g, (unsigned long long)(coop_rd_ptr(g) - MOD_MAIN_BASE),
               (unsigned long long)p, (unsigned long long)(pv - MOD_MAIN_BASE), (unsigned long long)(coop_rd_ptr(pv + 0x30) - MOD_MAIN_BASE),
               p == ap ? " (= the model's player)" : "");
    }
    {   // the partial binds (TotK Online, 1.2.1): G+0xf8 a count and an array of per-slot entries, each's byte +0x1c
        // the slot's partial index; the bind record [[[G+0x20] + index*0x48]+0x58]+0x18]+8. Raw words to check them.
        uint64_t c8 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0, g = x ? coop_rd_ptr(x + 0x18) : 0;
        uint64_t w[8] = {0};
        if (g) coop_rd(g + 0xf0, w, sizeof w);
        char l2[600];
        int m2 = snprintf(l2, sizeof l2, "coop: test: anim binds of %s: G+0xf0..: %llx %llx %llx %llx %llx %llx;", who, (unsigned long long)w[0],
                          (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5]);
        uint64_t ev[6] = {0};
        if (w[1]) coop_rd(w[1], ev, sizeof ev);
        m2 += snprintf(l2 + m2, sizeof l2 - m2, " [G+0xf8]: %llx %llx %llx %llx %llx %llx;", (unsigned long long)ev[0], (unsigned long long)ev[1],
                       (unsigned long long)ev[2], (unsigned long long)ev[3], (unsigned long long)ev[4], (unsigned long long)ev[5]);
        for (int z = 0; z < 6; z++) {   // each word that points somewhere: its first 8 pointers' byte +0x1c
            uint64_t arr2 = ev[z];
            if (arr2 < 0x100000000ull || arr2 > 0x8000000000ull) continue;
            m2 += snprintf(l2 + m2, sizeof l2 - m2, " w%d:", z);
            for (int i = 0; i < 7 && m2 < 560; i++) {
                uint64_t e = coop_rd_ptr(arr2 + 8ull * i);
                uint8_t b = 0xff;
                if (e) coop_rd(e + 0x1c, &b, 1);
                m2 += snprintf(l2 + m2, sizeof l2 - m2, " %u", b);
            }
        }
        uint64_t g20 = g ? coop_rd_ptr(g + 0x20) : 0;
        int32_t g18 = 0;
        if (g) coop_rd(g + 0x18, &g18, 4);
        m2 += snprintf(l2 + m2, sizeof l2 - m2, "; G+0x18 %d, G+0x20 %llx", g18, (unsigned long long)g20);
        rt_log("%s", l2);
        // each slot's bind (+0x08) and change count (+0xa8), against the table's: [[[G+0x20]+i*0x48]+0x58]+0x18 is
        // TotK Online's record P, whose +8 the play compares with the slot's +0x08
        char l3[700];
        int m3 = snprintf(l3, sizeof l3, "coop: test: anim bind table of %s:", who);
        for (int i = 0; g20 && i < 10 && m3 < 600; i++) {
            uint64_t e = coop_rd_ptr(g20 + 0x48ull * i + 0x58), p = e ? coop_rd_ptr(e + 0x18) : 0, b = p ? coop_rd_ptr(p + 8) : 0;
            m3 += snprintf(l3 + m3, sizeof l3 - m3, " [%d] P %llx B %llx", i, (unsigned long long)p, (unsigned long long)b);
        }
        m3 += snprintf(l3 + m3, sizeof l3 - m3, "; slots:");
        for (int i = 0; arr && i < 7 && m3 < 680; i++) {
            uint64_t b = coop_rd_ptr(arr + 0xb0ull * i + 8);
            uint32_t cc = 0;
            coop_rd(arr + 0xb0ull * i + 0xa8, &cc, 4);
            m3 += snprintf(l3 + m3, sizeof l3 - m3, " %llx/%u", (unsigned long long)b, cc);
        }
        rt_log("%s", l3);
        // where each slot's bind is held: words equal to it in G's first 0x400 bytes, and one level down (0x200 of
        // each object G points to), as "G+off" or "G+off -> +off2"
        static int found_said;
        for (int i = 0; arr && g && i < 3 && found_said < 12; i++) {
            uint64_t b = coop_rd_ptr(arr + 0xb0ull * i + 8);
            if (!b) continue;
            static uint64_t gw[0x400 / 8], ow[0x200 / 8];
            if (!coop_rd(g, gw, sizeof gw)) break;
            char l4[400];
            int m4 = snprintf(l4, sizeof l4, "coop: test: anim bind %llx of slot %d is at:", (unsigned long long)b, i);
            for (int j = 0; j < 0x400 / 8 && m4 < 360; j++) {
                if (gw[j] == b) m4 += snprintf(l4 + m4, sizeof l4 - m4, " G+%#x", j * 8);
                if (gw[j] < 0x100000000ull || gw[j] > 0x8000000000ull || !coop_rd(gw[j], ow, sizeof ow)) continue;
                for (int q = 0; q < 0x200 / 8 && m4 < 360; q++)
                    if (ow[q] == b) m4 += snprintf(l4 + m4, sizeof l4 - m4, " G+%#x->+%#x", j * 8, q * 8);
                    else if (ow[q] > 0x100000000ull && ow[q] < 0x8000000000ull && (q & 1) == 0) {
                        uint64_t z = 0;   // two levels: an array of records whose +8 is the bind
                        for (int r = 0; r < 16; r++)
                            if (coop_rd(ow[q] + 0x48ull * r + 8, &z, 8) && z == b) { m4 += snprintf(l4 + m4, sizeof l4 - m4, " G+%#x->+%#x->[%d*0x48]+8", j * 8, q * 8, r); break; }
                    }
            }
            rt_log("%s", l4), found_said++;
        }
    }
    for (int i = 0; arr && i < n && i < 8 && m < 640; i++) {
        uint8_t s[0x30];
        if (!coop_rd(arr + 0xb0ull * i, s, sizeof s)) break;
        uint16_t idx;
        float w, f;
        memcpy(&idx, s + 2, 2), memcpy(&w, s + 0x10, 4), memcpy(&f, s + 0x28, 4);
        uint64_t b;
        memcpy(&b, s + 8, 8);
        m += snprintf(line + m, sizeof line - m, " [%d] %u w%.2f f%.1f m%#x", i, idx, w, f, anim_bind_off(anim_graph_of(a), b));
    }
    rt_log("%s", line);
}
void coop_anim_slots_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ANIMSLOTS") != NULL;
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (!on || now - last < 500000000ull) return;
    last = now;
    anim_slots_log("this Link", me);
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) anim_slots_log("a stand-in", co[i]);
}

// The animation play TotK Online calls on its stand-in (read for behaviour only): the object Q = [G+0x28], G =
// [[[actor+0x228]+0x40]+0x18]+0x18 (component 8's animation graph), its vtable +0x30 (1.4.3 main+0x1b569fc):
// (Q, slot, animation index, partial bind or 0, 0, 0xffff, s0 weight, s1 frame); it writes the slot of the player
// [[Q+8]+0x40] (the model's, anim_player_of).
static uint64_t anim_player_of(uint64_t a) {
    uint64_t mc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 8), model = mc ? coop_rd_ptr(mc + 0x28) : 0;
    return model ? coop_rd_ptr(model + 0x40) : 0;
}
static uint64_t anim_graph_of(uint64_t a) {
    uint64_t c8 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0;
    return x ? coop_rd_ptr(x + 0x18) : 0;
}
static uint64_t anim_caller_of(uint64_t a) {
    uint64_t g = anim_graph_of(a);
    return g ? coop_rd_ptr(g + 0x28) : 0;
}
// A slot's body mask (its bind, slot +0x08; the play compares its 4th argument's +8 with it and the bind's +0x18 with
// slot +0xa8) is held at +0xc0 of a layer object the graph points to: [G+0x1a8]+0xc0 for a Link's whole-body layers,
// [G+0x2c8]+0xc0 for the upper-body one (idle, walking and the bow; TOTK_COOP_TEST_ANIMSLOTS, 2026-11-07). The same
// offset names the same mask in every game (both load Link's animation set), so it is what is sent. 0: not found.
static uint16_t anim_bind_off(uint64_t g, uint64_t b) {
    static __thread struct { uint64_t g, b; uint16_t off; } cache[8];
    if (!g || !b) return 0;
    for (int i = 0; i < 8; i++) if (cache[i].g == g && cache[i].b == b) return cache[i].off;
    uint16_t off = 0;
    static __thread uint64_t gw[0x800 / 8];
    if (coop_rd(g, gw, sizeof gw))
        for (int j = 1; j < 0x800 / 8 && !off; j++)
            if (gw[j] > 0x100000000ull && gw[j] < 0x8000000000ull && coop_rd_ptr(gw[j] + 0xc0) == b) off = (uint16_t)(j * 8);
    static __thread int next;
    cache[next].g = g, cache[next].b = b, cache[next].off = off, next = (next + 1) & 7;
    return off;
}
static uint64_t anim_bind_at(uint64_t g, uint16_t off) {
    uint64_t layer = g && off && off < 0x800 ? coop_rd_ptr(g + off) : 0;
    return layer ? coop_rd_ptr(layer + 0xc0) : 0;
}
// bind: the mask (0 none): passed as a record whose +8 is it, as the play reads it.
static void anim_play_bind(Cpu *c, uint64_t q, int slot, int idx, float weight, float frame, uint64_t bind) {
    static __thread uint64_t rec[2];
    rec[0] = 0, rec[1] = bind;
    uint64_t vt = coop_rd_ptr(q), fn = vt ? coop_rd_ptr(vt + 0x30) : 0, ap = q;
    if (!fn) return;
    __uint128_t v0 = c->v[0], v1 = c->v[1];
    uint32_t wb, fb;
    memcpy(&wb, &weight, 4), memcpy(&fb, &frame, 4);
    c->v[0] = wb, c->v[1] = fb;
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    rt_call(c, fn, 6, ap, (uint64_t)slot, (uint64_t)idx, bind ? (uint64_t)(uintptr_t)rec : 0ull, 0ull, 0xffffull);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    c->v[0] = v0, c->v[1] = v1;
}
static void anim_play(Cpu *c, uint64_t q, int slot, int idx, float weight, float frame) {
    uint64_t vt = coop_rd_ptr(q), fn = vt ? coop_rd_ptr(vt + 0x30) : 0, ap = q;
    if (!fn) return;
    __uint128_t v0 = c->v[0], v1 = c->v[1];
    uint32_t wb, fb;
    memcpy(&wb, &weight, 4), memcpy(&fb, &frame, 4);
    c->v[0] = wb, c->v[1] = fb;
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    rt_call(c, fn, 6, ap, (uint64_t)slot, (uint64_t)idx, 0ull, 0ull, 0xffffull);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    c->v[0] = v0, c->v[1] = v1;
}
// An animation player's first slots as {index, weight, frame}; the count read (at most max).
#define ANIM_SLOTS COOP_ANIM_SLOTS
typedef struct { uint16_t idx, pad; float w, f; uint64_t bind; } AnimSlot;   // bind: slot +0x08
static int anim_read(uint64_t ap, AnimSlot *out, int max) {
    uint64_t arr = ap ? coop_rd_ptr(ap + 0x40) : 0;
    int32_t n = 0;
    if (!arr || !coop_rd(ap + 0x38, &n, 4) || n <= 0) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint8_t s[0x30];
        if (!coop_rd(arr + 0xb0ull * i, s, sizeof s)) return i;
        memcpy(&out[i].idx, s + 2, 2), memcpy(&out[i].w, s + 0x10, 4), memcpy(&out[i].f, s + 0x28, 4), memcpy(&out[i].bind, s + 8, 8);
    }
    return n;
}
// Test (TOTK_COOP_TEST_ANIMCOPY=1): this game's own Link's slots played on each stand-in every tick (no network).
void coop_anim_copy_test(Cpu *c, uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ANIMCOPY") != NULL;
    if (!on) return;
    AnimSlot s[ANIM_SLOTS];
    int n = anim_read(anim_player_of(me), s, ANIM_SLOTS);
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int k = 0; k < nc; k++) {
        uint64_t ap = anim_caller_of(co[k]);
        for (int i = 0; ap && i < n; i++) if (s[i].idx != 0xffff) anim_play(c, ap, i, s[i].idx, s[i].w, s[i].f);
        static int said;
        if (ap && !said++) rt_log("coop: test: animcopy: %d slots of this Link played on a stand-in (play main+%#llx)", n,
                                  (unsigned long long)(coop_rd_ptr(coop_rd_ptr(ap) + 0x30) - MOD_MAIN_BASE));
    }
}

// ---- the stand-in's animation (TOTK_COOP_STANDIN=puppet; TotK Online's ActorAnimationSync, read for behaviour only).
// The owner sends its Link's first seven animation slots with its state (NetState.anim): clip, weight, frame and the
// frames a second it moved since the last send. Each other game plays them on its PlayerPuppet every tick, the frame
// carried on by that rate for the state's age, so the stand-in moves with its owner's own clips and nothing of its
// own chooses another (it has no Player component). Each slot's body mask goes as where it is in the graph
// (anim_bind_off) and is played with the stand-in's own mask from there (TotK Online sends its partial-bind index
// and flags; read for behaviour only).
void coop_anim_pack(uint64_t me, CoopAnim *out) {
    AnimSlot s[COOP_ANIM_SLOTS];
    int n = anim_read(anim_player_of(me), s, COOP_ANIM_SLOTS);
    uint64_t g = anim_graph_of(me);
    static float last_f[COOP_ANIM_SLOTS];
    static uint16_t last_i[COOP_ANIM_SLOTS];
    static uint64_t last_ns;
    uint64_t now = plat_now_ns();
    float dt = last_ns ? (float)((now - last_ns) / 1e9) : 0;
    last_ns = now;
    for (int i = 0; i < COOP_ANIM_SLOTS; i++) {
        CoopAnim a = {.idx = 0xffff};
        if (i < n && s[i].idx != 0xffff && s[i].w == s[i].w && s[i].f == s[i].f) {
            float w = s[i].w < 0 ? 0 : s[i].w > 1 ? 1 : s[i].w;
            a.idx = s[i].idx, a.weight = (uint16_t)lroundf(w * 65535), a.frame = s[i].f, a.bind = anim_bind_off(g, s[i].bind);
            a.rate = last_i[i] == s[i].idx && dt > 0 && dt < 0.5f && s[i].f >= last_f[i] ? (s[i].f - last_f[i]) / dt : 0;
            if (a.rate > 600) a.rate = 0;
        }
        last_i[i] = a.idx, last_f[i] = a.frame;
        out[i] = a;
    }
}
// The owner's slots (in, age_ns old) played on a stand-in; a slot the owner left empty is played with weight 0 on
// the clip the stand-in still has there.
void coop_anim_apply(Cpu *c, uint64_t standin, const CoopAnim *in, uint64_t age_ns) {
    uint64_t q = anim_caller_of(standin);
    if (!q) return;
    AnimSlot have[COOP_ANIM_SLOTS];
    int n = anim_read(anim_player_of(standin), have, COOP_ANIM_SLOTS);
    uint64_t g = anim_graph_of(standin);
    float age = age_ns < 1000000000ull ? (float)(age_ns / 1e9) : 1;
    int played = 0, masked = 0;
    for (int i = 0; i < COOP_ANIM_SLOTS; i++) {
        if (in[i].idx != 0xffff) {
            // The masks (TOTK_COOP_STANDIN_MASKS=0: none, every clip on the whole body). Played with the stand-in's
            // own mask flags they put it in a T pose in a sword fight; with its owner's flags on them
            // (coop_anim_masks_apply) it stood and held its sword (attack_enemy_3rd.txt frames, 2026-11-07).
            static int masks = -1;
            if (masks < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_MASKS"); masks = !(e && *e == '0'); }
            uint64_t b = masks ? anim_bind_at(g, in[i].bind) : 0;
            masked += b != 0;
            anim_play_bind(c, q, i, in[i].idx, in[i].weight / 65535.0f, in[i].frame + in[i].rate * age, b), played++;
        } else if (i < n && have[i].idx != 0xffff && have[i].w > 0) anim_play(c, q, i, have[i].idx, 0, have[i].f);
    }
    static int said;
    if (played && !said++) rt_log("coop: stand-in: its owner's animation (%d slots, %d with a body mask) plays on it", played, masked);
}

// Test (TOTK_COOP_TEST_EQDIFF=1): this Link's EquipmentUser ([actor+0x228]+0x230, its first 0x4800 bytes) each tick;
// every byte that goes between 0 and 1 is logged with the world time (at most 200 lines): to find the "weapon is
// attacking" byte TotK Online copies onto its stand-in (1.2.1 eq+0x660; read for behaviour only) by a swing's times.
void coop_eqdiff_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_EQDIFF") != NULL;
    if (!on) return;
    enum { N = 0x4800 };
    static uint8_t last[N], now[N];
    static int have, said;
    static uint16_t flips[N];
    uint64_t eq = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x230);
    if (!eq || !coop_rd(eq, now, N)) return;
    if (have)
        for (int i = 0; i < N && said < 200; i++)
            if (now[i] != last[i] && now[i] <= 1 && last[i] <= 1 && flips[i]++ < 12)
                rt_log("coop: test: eqdiff +%#x %u -> %u at %.2f s", i, last[i], now[i], frame_step_world_us() / 1e6), said++;
    memcpy(last, now, N), have = 1;
}

// Test (TOTK_COOP_TEST_BINDS=1): the masks at the graph offsets a Link's slots use (anim_bind_off), for this Link and
// each stand-in side by side, every 2 s: the layer object's mask (+0xc0) and that mask's first 0x40 bytes, to see
// whether one offset holds the same mask in both (the stand-in has no Player component).
void coop_binds_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_BINDS") != NULL;
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (!on || now - last < 2000000000ull) return;
    last = now;
    static const uint16_t offs[] = {0x1a8, 0x1f0, 0x238, 0x280, 0x2c8, 0x310};
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int w = 0; w <= nc && w < 3; w++) {
        uint64_t a = w ? co[w - 1] : me, g = anim_graph_of(a);
        for (unsigned k = 0; g && k < sizeof offs / sizeof offs[0]; k++) {
            uint64_t layer = coop_rd_ptr(g + offs[k]), b = layer ? coop_rd_ptr(layer + 0xc0) : 0;
            uint32_t v[16] = {0};
            if (b) coop_rd(b, v, sizeof v);
            char ln[400];
            int m = snprintf(ln, sizeof ln, "coop: test: binds %s G+%#x: layer %llx (vt main+%#llx) mask %llx:", w ? "stand-in" : "this Link", offs[k],
                             (unsigned long long)layer, (unsigned long long)(layer ? coop_rd_ptr(layer) - MOD_MAIN_BASE : 0), (unsigned long long)b);
            for (int i = 0; i < 16; i++) m += snprintf(ln + m, sizeof ln - m, " %x", v[i]);
            rt_log("%s", ln);
        }
    }
}

// The masks' state (CoopMask). The stand-in's graph has the same layer objects at the same offsets as a Link's, each
// with its mask at +0xc0, but the flags the graph sets in them differ (a Link's whole-body mask 83 83 80 83 80 83 80,
// the stand-in's 83 83 80 80 83, TOTK_COOP_TEST_BINDS, 2026-11-07): the stand-in's own graph does not set them as the
// owner's does. So the owner sends the flags of each mask its slots use (offset, u16 +0x1c, byte flags [+0x10] count
// +8, word flags [+0x28] count +0x20) and the stand-in's masks take them before its slots are played; a change bumps
// the mask's change count (+0x18), which the play compares with the slot's (+0xa8) to bind it again.
void coop_anim_masks_pack(uint64_t me, const CoopAnim *anim, CoopMask *out) {
    memset(out, 0, sizeof(CoopMask) * COOP_ANIM_MASKS);
    uint64_t g = anim_graph_of(me);
    int n = 0;
    for (int i = 0; g && i < COOP_ANIM_SLOTS && n < COOP_ANIM_MASKS; i++) {
        uint16_t off = anim[i].bind;
        int dup = !off;
        for (int k = 0; k < n && !dup; k++) dup = out[k].off == off;
        uint64_t b = dup ? 0 : anim_bind_at(g, off);
        if (!b) continue;
        CoopMask *m = &out[n];
        uint32_t nb = 0, nw = 0;
        coop_rd(b + 8, &nb, 4), coop_rd(b + 0x20, &nw, 4), coop_rd(b + 0x1c, &m->c1c, 2);
        if (nb > 16) nb = 16;
        if (nw > 16) nw = 16;
        uint64_t pb = coop_rd_ptr(b + 0x10), pw = coop_rd_ptr(b + 0x28);
        if ((nb && (!pb || !coop_rd(pb, m->b, nb))) || (nw && (!pw || !coop_rd(pw, m->w, 4 * nw)))) continue;
        m->off = off, m->nb = (uint8_t)nb, m->nw = (uint8_t)nw, n++;
    }
}
void coop_anim_masks_apply(uint64_t standin, const CoopMask *in) {
    uint64_t g = anim_graph_of(standin);
    int changed = 0;
    for (int k = 0; g && k < COOP_ANIM_MASKS; k++) {
        if (!in[k].off) continue;
        uint64_t b = anim_bind_at(g, in[k].off);
        uint32_t nb = 0, nw = 0;
        if (!b || !coop_rd(b + 8, &nb, 4) || !coop_rd(b + 0x20, &nw, 4) || nb != in[k].nb || nw != in[k].nw) continue;   // (another shape)
        uint64_t pb = coop_rd_ptr(b + 0x10), pw = coop_rd_ptr(b + 0x28);
        uint8_t hb[16];
        uint32_t hw[16];
        uint16_t c1c = 0;
        coop_rd(b + 0x1c, &c1c, 2);
        if ((nb && (!pb || !coop_rd(pb, hb, nb))) || (nw && (!pw || !coop_rd(pw, hw, 4 * nw)))) continue;
        if ((!nb || !memcmp(hb, in[k].b, nb)) && (!nw || !memcmp(hw, in[k].w, 4 * nw)) && c1c == in[k].c1c) continue;
        if (nb) memcpy((void *)(uintptr_t)pb, in[k].b, nb);
        if (nw) memcpy((void *)(uintptr_t)pw, in[k].w, 4 * nw);
        ST16(b + 0x1c, in[k].c1c);
        ST32(b + 0x18, LD32(b + 0x18) + 1);
        changed++;
    }
    static int said;
    if (changed && said++ < 5) rt_log("coop: stand-in: %d of its body masks set as its owner's", changed);
}
