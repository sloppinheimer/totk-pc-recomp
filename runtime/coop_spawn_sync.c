// The sync of enemies made at run time. Moved out of coop_world.c as it was (docs/coop/enemy-sync-spec.md B.28); its
// packets come in through that file's reader of the HIT channel. The key it gives an enemy is that enemy's identity
// in the enemy sync (coop_world_spawn_key).
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

static float dist3(const float a[3], const float b[3]) {
    return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Enemies made at run time (no place in the map: a night's Stalkoblins, a spawner's) are made in the other games
// too; each such actor's key is the one its maker gave it, kept here.
#define KEYED_N 128
// nh: the actor's name hash when it was given its key: one with another name at that address is another actor.
static struct { _Atomic uint64_t actor; _Atomic uint32_t key; uint32_t nh; } keyed[KEYED_N];
static uint32_t keyed_of(uint64_t a) {
    for (int i = 0; i < KEYED_N; i++)
        if (atomic_load_explicit(&keyed[i].actor, memory_order_acquire) == a)
            return keyed[i].nh == coop_world_name_hash(a) ? atomic_load_explicit(&keyed[i].key, memory_order_relaxed) : 0;
    return 0;
}
uint32_t coop_world_spawn_key(uint64_t a) { return keyed_of(a); }   // 0: not an actor the spawn sync announced or made
static void keyed_set(uint64_t a, uint32_t key) {
    int f = -1;
    for (int i = 0; i < KEYED_N; i++) {
        uint64_t x = atomic_load(&keyed[i].actor);
        if (x == a) { f = i; break; }
        if (f < 0 && !x) f = i;
    }
    if (f < 0) { rt_log("coop: spawn sync: no room to key %08x", key); return; }
    atomic_store_explicit(&keyed[f].actor, 0, memory_order_release);
    keyed[f].nh = coop_world_name_hash(a), atomic_store_explicit(&keyed[f].key, key, memory_order_relaxed);
    atomic_store_explicit(&keyed[f].actor, a, memory_order_release);
}
// Entries of actors gone (or another actor at the address) let go of, once a second (spawn_tick).
static void keyed_prune(void) {
    for (int i = 0; i < KEYED_N; i++) {
        uint64_t a = atomic_load(&keyed[i].actor);
        int32_t st = 0;
        if (a && (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || keyed[i].nh != coop_world_name_hash(a)))
            atomic_store(&keyed[i].actor, 0);
    }
}

// ---- enemies made at run time, made in the other games too (TotK Online's way: it registers every actor the game
// hands over from its create queue, 1.2.1 main+0xcd7d40, and the other games make one by name; read for behaviour).
// The game hands a new actor over at main+0x89841c (ActorCreateInfo::getActor returning to main+0x898420; found from
// the 1.2.1 site by its code). An Enemy_ with no map placement (its placement hash, [actor+0x370]+0x88 in 1.2.1, 0) is
// sent to the others (SPAWN_KIND: its key, name, place, rotation), and each makes one there with that key; when it goes
// away here without dying (unloaded), SPAWN_GONE deletes the others' (a death goes by the enemy sync).
int coop_enemy_spawn_synced(const char *nm);   // coop_enemy.c
static int spawn_kind(const char *nm) { return coop_enemy_spawn_synced(nm); }   // what the spawn sync announces and makes: the enemies the enemy sync takes, and their parts
#define BORN_N 256   // (a load hands over hundreds; only enemies are kept)
static struct { _Atomic uint64_t actor; _Atomic uint64_t at_ns; } born[BORN_N];   // handed over here (spawn_tick)
static _Atomic unsigned born_n;
void coop_actor_created(uint64_t a) {   // coop_spawn.c, from the getActor hook (any thread)
    if (!a || !coop_net_on()) return;
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    {   // Test (TOTK_COOP_TEST_BORN=1): the first 60 actors handed over, by name and whether the spawn sync keeps it
        static int on = -1;
        static _Atomic int said;
        if (on < 0) on = cfg_get("TOTK_COOP_TEST_BORN") != NULL;
        static char seen[200][48];
        static _Atomic int nseen;
        int dup = 0;
        for (int i = 0; on && i < atomic_load(&nseen) && i < 200; i++) dup |= !strcmp(seen[i], nm);
        if (on && !dup && atomic_fetch_add(&said, 1) < 200) {
            int i = atomic_fetch_add(&nseen, 1);
            if (i < 200) snprintf(seen[i], sizeof seen[i], "%s", nm);
            rt_log("coop: test: born %llx '%s' kind %d at %.1f s", (unsigned long long)a, nm, spawn_kind(nm), frame_step_world_us() / 1e6);
        }
    }
    if (!spawn_kind(nm)) return;   // (an enemy too, whichever enemy sync is on: its key here is its identity there)
    unsigned i = atomic_fetch_add(&born_n, 1) % BORN_N;
    atomic_store_explicit(&born[i].actor, 0, memory_order_release);
    atomic_store_explicit(&born[i].at_ns, plat_now_ns(), memory_order_relaxed);
    atomic_store_explicit(&born[i].actor, a, memory_order_release);
}
static struct { uint32_t key; char name[48]; float pos[3]; int state; uint64_t actor, ref, ns; int from;
                _Alignas(16) uint8_t handle[0x40], link[0x40]; } made[32];   // the copies being made here (state 1, 2 made)
static struct { uint32_t key; uint64_t actor; } spawned_mine[64];   // enemies this game made at run time and sent
// The copies made here for another game, after made[] let go of them: deleted once far from every player (coop_far.c),
// as TotK Online's server disposes of an actor 1000 m from every player and its client deletes a runtime-made one.
static struct { uint32_t key; uint64_t actor; int from; char name[48]; } made_held[64];
// Whether this game made that actor for another game's announcement (coop_enemy.c: its maker claims it, not this game).
int coop_spawn_made_for_other(uint64_t actor) {
    for (int k = 0; actor && k < 64; k++) if (made_held[k].actor == actor) return 1;
    return 0;
}
static void made_held_add(uint32_t key, uint64_t a, int from, const char *name) {
    int f = -1;
    for (int k = 0; k < 64; k++) {
        if (made_held[k].actor == a) { f = k; break; }
        if (f < 0 && !made_held[k].actor) f = k;
    }
    if (f < 0) return;   // (full: not swept)
    made_held[f].key = key, made_held[f].actor = a, made_held[f].from = from;
    snprintf(made_held[f].name, sizeof made_held[f].name, "%s", name);
}
// Once a second: each copy still that actor (alive, its name, its key) and far from every player is deleted with the
// game's own delete request; one gone or another actor now is forgotten.
static void far_sweep(Cpu *c) {
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (now - last < 1000000000ull) return;
    last = now;
    for (int k = 0; k < 64; k++) {
        uint64_t a = made_held[k].actor;
        if (!a) continue;
        int32_t st = 0;
        char nm[48];
        if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || strcmp(coop_actor_name(a, nm, sizeof nm), made_held[k].name) ||
            keyed_of(a) != made_held[k].key) {
            made_held[k].actor = 0;
            continue;
        }
        float d = 0;
        if (!coop_far_actor(a, &d)) continue;
        coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, COOP_DELETE_REASON, 0, 0);
        for (int i = 0; i < KEYED_N; i++)
            if (atomic_load(&keyed[i].actor) == a) atomic_store(&keyed[i].actor, 0);
        rt_log("coop: far: %s %08x made here for player %d is %.0f m from every player: deleted", made_held[k].name,
               made_held[k].key, made_held[k].from, d);
        made_held[k].actor = 0;
    }
}
static int spawn_on(void) {   // TOTK_COOP_SPAWN_SYNC=0: not done
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_SPAWN_SYNC"); on = !(e && *e == '0'); }
    return on;
}
// Test (TOTK_COOP_TEST_FAR_COPY=<s>[,name]): at that world time, a SPAWN as if the other player had sent one, of a
// Bokoblin (or that actor) 4 m in front of this Link, goes through spawn_take: a copy made here for that player, as
// the spawn sync makes them (the test spawn TOTK_COOP_TEST_SPAWN does not pass the create queues, so it is no spawn
// sync). With TOTK_COOP_TEST_FAR in both games, far_sweep then deletes it (tools/coop/far_3rd.txt).
static void far_copy_test(Cpu *c) {
    static int done;
    const char *v = cfg_get("TOTK_COOP_TEST_FAR_COPY");
    uint64_t me = coop_player_actor();
    if (done || !v || !me || frame_step_world_us() < (int64_t)(atof(v) * 1e6)) return;
    int other = 0, ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    if (coop_net_players(ids, names, COOP_MAX_PLAYERS) > 0) other = ids[0];
    if (!other) return;   // (waits for the other player)
    done = 1;
    SpawnPacket sp = {.kind = SPAWN_KIND};
    const char *nm = strchr(v, ',') ? strchr(v, ',') + 1 : "Enemy_Bokoblin_Junior";
    snprintf(sp.name, sizeof sp.name, "%s", nm);
    float p[3], r[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    for (int i = 0; i < 3; i++) sp.pos[i] = p[i] + 4 * r[i * 3 + 2];   // 4 m along the Link's forward (rotation column z)
    memcpy(sp.rot, r, 36);
    sp.key = coop_world_enemy_key(sp.name, sp.pos);
    rt_log("coop: test: far copy: %s %08x as if from player %d, at %.1f %.1f %.1f", sp.name, sp.key, other, sp.pos[0], sp.pos[1], sp.pos[2]);
    spawn_take(c, &sp, other);
}
// An enemy of this game sent to the others when it is placed nowhere in the map (every game has those) and is no
// copy of another's (made here for a player, or handed over to this game). From the create queues (born) and, since
// those never handed over an enemy in play (TOTK_COOP_TEST_BORN: only effects and prefab sets; a Keese in one game only,
// A play session, 2026-11-07), from the scan of the live enemies too (spawn_scan).
static int spawn_send(uint64_t a, const char *nm) {
    if (coop_actor_placed(a)) return 0;   // placed in the map: every game has it
    if (keyed_of(a)) return 0;         // known by a key already (sent, a copy, or taken over)
    for (int k = 0; k < 64; k++) if (spawned_mine[k].key && spawned_mine[k].actor == a) return 0;
    int ours = 0;   // a copy this game made for another
    for (int k = 0; k < 32 && !ours; k++) if (made[k].state && made[k].actor == a) ours = 1;
    float p[3], r[9];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36)) return 0;
    for (int k = 0; k < 32 && !ours; k++)
        if (made[k].state == 1 && !strcmp(made[k].name, nm) && dist3(made[k].pos, p) < 2) ours = 1, made[k].actor = a;
    if (ours) return 0;
    SpawnPacket sp = {.key = coop_world_enemy_key(nm, p), .kind = SPAWN_KIND};
    snprintf(sp.name, sizeof sp.name, "%s", nm), memcpy(sp.pos, p, 12), memcpy(sp.rot, r, 36);
    keyed_set(a, sp.key);
    int f = -1;
    for (int k = 0; k < 64 && f < 0; k++) if (!spawned_mine[k].key) f = k;
    if (f >= 0) spawned_mine[f].key = sp.key, spawned_mine[f].actor = a;   // (full: its going away is not told)
    coop_net_hit_send(&sp, sizeof sp);
    rt_log("coop: spawn sync: %s %08x made here at %.1f %.1f %.1f: sent to the others", nm, sp.key, p[0], p[1], p[2]);
    return 1;
}
// Twice a second: each live enemy here (dup_count's list) seen in SCAN_SETTLE scans running goes to spawn_send, which
// sends only one that is placed nowhere in the map and has no key yet. An enemy both games made at one place has one
// key in both: the game that gets the other's packet keeps its own under that key and makes no second (spawn_take).
// Needs another player here.
#define SCAN_SETTLE 4
static void spawn_scan(void) {
    static uint64_t last;
    static uint64_t seen[64];
    static int seen_n[64];
    static int nseen;
    uint64_t now = plat_now_ns();
    if (now - last < 500000000ull) return;
    last = now;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    if (coop_net_players(ids, names, COOP_MAX_PLAYERS) <= 0) { nseen = 0; return; }
    uint64_t next[64];
    int next_n[64], nnext = 0;
    for (int i = 0; i < coop_world_n_enemies; i++) {
        uint64_t a = coop_world_enemies[i];
        int32_t st = 0;
        char nm[48];
        if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || keyed_of(a)) continue;
        if (!spawn_kind(coop_actor_name(a, nm, sizeof nm))) continue;
        int was = 0;
        for (int k = 0; k < nseen; k++) if (seen[k] == a) was = seen_n[k];
        if (was + 1 >= SCAN_SETTLE) { spawn_send(a, nm); continue; }
        if (nnext < 64) next[nnext] = a, next_n[nnext++] = was + 1;
    }
    memcpy(seen, next, (size_t)nnext * sizeof next[0]), memcpy(seen_n, next_n, (size_t)nnext * sizeof next_n[0]), nseen = nnext;
}
void spawn_tick(Cpu *c) {
    if (!spawn_on()) return;
    far_copy_test(c);
    far_sweep(c);
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
        int ready = spawn_kind(nm) && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6;
        if (!ready && now - at < 3000000000ull) continue;
        uint64_t want = a;   // taken (unless another was handed over into the slot meanwhile)
        if (!atomic_compare_exchange_strong(&born[i].actor, &want, 0) || !ready) continue;
        static int said;
        if (said++ < 40) rt_log("coop: spawn sync: %s %llx handed over", nm, (unsigned long long)a);
        spawn_send(a, nm);
    }
    spawn_scan();
    for (int k = 0; k < 32; k++) {   // the copies being made; made ones let go of 10 s on (handed over by then)
        if (made[k].state == 2 && now - made[k].ns > 10000000000ull) made[k].state = 0, made[k].actor = 0;
        if (made[k].state != 1) continue;
        int st = coop_handle_status(made[k].handle);
        if (st == 3) {
            uint64_t a = coop_claim(c, made[k].handle, made[k].link, &made[k].ref);
            if (a) made[k].actor = a, made[k].state = 2, made[k].ns = now, keyed_set(a, made[k].key), made_held_add(made[k].key, a, made[k].from, made[k].name);
            else made[k].state = 0;
            rt_log("coop: spawn sync: %s %08x for player %d %s", made[k].name, made[k].key, made[k].from, a ? "made here" : "not made");
        } else if (st > 3 || now - made[k].ns > 10000000000ull) {
            coop_handle_release(c, made[k].handle);   // (failed or given up: its record freed, coop_game.h)
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
        int alive = coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 && spawn_kind(coop_actor_name(a, nm, sizeof nm));
        if (alive && keyed_of(a) == spawned_mine[k].key) continue;
        if (!alive || coop_life_of(a) != 0) {
            SpawnPacket sp = {.key = spawned_mine[k].key, .kind = SPAWN_GONE};
            coop_net_hit_send(&sp, sizeof sp);
            rt_log("coop: spawn sync: %08x gone here: the others go too", sp.key);
        }
        spawned_mine[k].key = 0, spawned_mine[k].actor = 0;
    }
}
void spawn_take(Cpu *c, const SpawnPacket *sp, int from) {
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
    if (sp->kind != SPAWN_KIND || memchr(sp->name, 0, sizeof sp->name) == NULL || !spawn_kind(sp->name)) return;
    for (int k = 0; k < 32; k++) if (made[k].state && made[k].key == sp->key) return;
    // This game's own spawner made one of that name at that place (both games run the same spawners): that one
    // takes the key, no second is made.
    for (int i = 0; i < coop_world_n_enemies; i++) {
        uint64_t a = coop_world_enemies[i];
        int32_t st = 0;
        float q[3];
        char nm[48];
        if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || keyed_of(a) || !coop_rd(a + COOP_ACTOR_POS, q, 12) ||
            dist3(q, sp->pos) > 3 || strcmp(coop_actor_name(a, nm, sizeof nm), sp->name))
            continue;
        keyed_set(a, sp->key);
        rt_log("coop: spawn sync: %s %08x made in player %d game: this game's own one there takes its key", sp->name, sp->key, from);
        return;
    }
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
