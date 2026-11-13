// Co-op: entities (docs/coop/contraption-sync-v4-spec.md, cited as S; the design is contraption-sync-v4.md).
// TOTK_COOP_CONTRAPTION=1. TotK 1.4.3 main offsets.
//
// An entity is a Zonai device, an Autobuild part, or an actor that became an end of a glue; it has one id for life
// (Eid) and one owner, the game that simulates it. Entities connected by glue are an island. The owner reads its
// world and states each island whole; every other game holds a copy and reconciles its world to the statement
// (coop_ent_rec.c). Ownership moves by TAKE and GRANT (coop_ent_own.c). This file holds the table, the identity
// of new actors, the reading of the world into bonds, islands and digests, the sets the hooks read, the group views
// the kept code of coop_ctn.c works from, and the order of a frame's pass.
//
// Threads (S 1): coop_ent_phase0 from GameDataMgr::calc at actor phase 0 does everything that walks or changes the
// world; coop_ent_net beside the transport's tick keeps the heartbeat flags and the pause queue; coop_ent_drive at
// the co-op tick moves the copies. Hooks on any game thread read the sets and append to the event ring.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

void coop_lock_wait(PlatMutex *m);                 // coop_spawn.c: waits without holding the caller's core
void coop_life_veto_rebuild(Cpu *c, const uint64_t *parts, int nparts);
int coop_held_here(uint64_t a);                    // coop_ctn.c
void coop_obj_entity(uint64_t actor);              // coop_players.c: an actor that is an entity now (the tick's tables)

PlatMutex ent_lock = PLAT_MUTEX_INIT, grp_lock = PLAT_MUTEX_INIT;
Ent ents[ENT_N];
Bond bonds[BOND_N];
int bond_n;
Island isls[ISL_N];
Stated stated[STATED_N];
Recipe recipes[REC_N];
Grp grps[GRP_N];
Peer peers[ENT_PEERS];

int64_t ent_ms(void) { return (int64_t)plat_now_ms(); }
int ent_on(void) { return coop_ctn_core(); }

// ---- Full tables (S 2): the action is not done, counted, one line per 10 s ----------------------------------------------
void ent_full(const char *table) {
    static struct { const char *t; uint32_t n; int64_t said; } f[12];
    int64_t now = ent_ms();
    for (int i = 0; i < 12; i++) {
        if (f[i].t && strcmp(f[i].t, table)) continue;
        f[i].t = table, f[i].n++;
        if (now - f[i].said > 10000) f[i].said = now, rt_log("coop: ent: the %s table is full (%u times): what needed a slot was not done", table, f[i].n);
        return;
    }
}
const char *ent_eid_str(Eid id, char *buf) {
    snprintf(buf, 24, "%04x.%u", (unsigned)(id.inc & 0xffff), id.serial);
    return buf;
}
uint32_t ent_fnv(uint32_t h, const void *p, int n) {
    const uint8_t *b = p;
    for (int i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// ---- The table and its two lookups (S 4.5) --------------------------------------------------------------------------------
#define HASH_N 2048
static int16_t by_id[HASH_N], by_actor[HASH_N];   // Ent index + 1, 0 empty, -1 a deleted slot
static uint32_t next_serial = 1;
uint32_t ent_mint_serial(void) { return next_serial++ & 0x7fffffffu ? next_serial - 1 : (next_serial = 2, 1); }

static void hash_put(int16_t *tab, uint64_t h, int i) {
    for (unsigned p = 0; p < HASH_N; p++) {
        unsigned k = (unsigned)((h + p) % HASH_N);
        if (tab[k] <= 0) { tab[k] = (int16_t)(i + 1); return; }
    }
}
static void hash_del(int16_t *tab, uint64_t h, int i) {
    for (unsigned p = 0; p < HASH_N; p++) {
        unsigned k = (unsigned)((h + p) % HASH_N);
        if (!tab[k]) return;
        if (tab[k] == i + 1) { tab[k] = -1; return; }
    }
}
static uint64_t actor_hash(uint64_t a) { return (a >> 4) * 0x9e3779b97f4a7c15ull >> 20; }
Ent *ent_by_id(Eid id) {
    uint64_t h = eid_hash(id);
    for (unsigned p = 0; p < HASH_N; p++) {
        int v = by_id[(h + p) % HASH_N];
        if (!v) return NULL;
        if (v > 0 && ents[v - 1].used && eid_eq(ents[v - 1].id, id)) return &ents[v - 1];
    }
    return NULL;
}
Ent *ent_by_actor(uint64_t actor) {
    uint64_t h = actor_hash(actor);
    for (unsigned p = 0; actor && p < HASH_N; p++) {
        int v = by_actor[(h + p) % HASH_N];
        if (!v) return NULL;
        if (v > 0 && ents[v - 1].used && ents[v - 1].actor == actor) return ctn_gen_of(actor) == ents[v - 1].gen ? &ents[v - 1] : NULL;
    }
    return NULL;
}
Ent *ent_new(Eid id) {
    Ent *e = ent_by_id(id);
    if (e) return e;
    for (int i = 0; i < ENT_N; i++) {
        if (ents[i].used) continue;
        e = &ents[i];
        memset(e, 0, sizeof *e);
        e->used = 1, e->id = id, e->isl = e->st = -1, e->scale[0] = e->scale[1] = e->scale[2] = 1, e->entered_ms = ent_ms();
        hash_put(by_id, eid_hash(id), i);
        return e;
    }
    ent_full("entity");
    return NULL;
}
void ent_unbind(Ent *e) {
    if (e->actor) hash_del(by_actor, actor_hash(e->actor), ent_i(e));
    e->actor = 0, e->gen = 0, e->main_body = 0;
}
void ent_bind(Ent *e, uint64_t actor) {
    if (e->actor == actor) return;
    ent_unbind(e);
    e->actor = actor, e->gen = ctn_gen_of(actor), e->main_body = 0;
    if (!e->name[0]) coop_actor_name(actor, e->name, sizeof e->name);
    if (!e->radius) e->radius = ctn_delete_radius(actor);
    if (e->radius < 5) e->radius = 5;
    hash_put(by_actor, actor_hash(actor), ent_i(e));
}
void ent_drop(Ent *e) {
    ent_unbind(e);
    hash_del(by_id, eid_hash(e->id), ent_i(e));
    e->used = 0;
}

// ---- The event ring (S 10): hooks append from any thread, phase 0 reads ------------------------------------------------------
static Ev ring[EV_N];
static _Atomic uint32_t ring_w;
static uint32_t ring_r;
static _Atomic uint32_t ring_lost;
void ent_post(const Ev *ev) {
    uint32_t w = atomic_fetch_add(&ring_w, 1);
    Ev *s = &ring[w % EV_N];
    if (atomic_exchange(&s->seq, 0)) atomic_fetch_add(&ring_lost, 1);   // (not yet read: the ring is full, the oldest is lost)
    memcpy((uint8_t *)s + sizeof s->seq, (const uint8_t *)ev + sizeof ev->seq, sizeof *ev - sizeof ev->seq);
    atomic_store(&s->seq, w + 1);
}
uint32_t ent_ring_lost(void) { return atomic_load(&ring_lost); }
int ent_ring_take(Ev *out) {
    Ev *s = &ring[ring_r % EV_N];
    uint32_t seq = atomic_load(&s->seq);
    if (!seq) {
        // (empty: nothing new, or a writer is still copying; when the writers are a whole ring ahead, what lay between
        // was overwritten and is skipped)
        if (atomic_load(&ring_w) - ring_r > EV_N) ring_r = atomic_load(&ring_w) - EV_N;
        return 0;
    }
    if (seq != ring_r + 1) { ring_r = seq - 1; }   // (a newer event took the slot: the reader jumps to it)
    memcpy(out, s, sizeof *out);
    atomic_store(&s->seq, 0);
    ring_r++;
    return 1;
}

// ---- Paused, loading, peers (S 1.2) -------------------------------------------------------------------------------------------
static _Atomic int64_t phase0_ms;
int ent_paused(void) { return ent_ms() - atomic_load(&phase0_ms) > 300; }
int ent_loading(void) {
    // The game's teardown flag: set when its delete-all-actors step starts, before the first delete, and cleared after
    // the last (research notes sync-v4/load-teardown; the byte coop_loadwatch.c reads). Or no Link yet.
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_ACTOR_MGR_SLOT));
    uint8_t b = 0;
    if (mgr && coop_rd(mgr + 0xe1, &b, 1) && (b & 2)) return 1;
    return !coop_player_actor();
}
int ent_peer_i(uint64_t inc) {
    int id = inc ? coop_ctr_route(inc) : 0;
    return id >= 1 && id <= ENT_PEERS ? id - 1 : -1;
}
int ent_peer_here(uint64_t inc) { return ent_peer_i(inc) >= 0; }
static uint32_t peer_flags(uint64_t inc) {
    int id = inc ? coop_ctr_route(inc) : 0;
    CoopCtrHb hb;
    uint64_t age = 0;
    return id && coop_ctr_hb(id, &hb, &age) && age < 2000000000ull ? hb.flags : 0;
}
int ent_peer_paused(uint64_t inc) { return (peer_flags(inc) & COOP_CTR_HB_PAUSED) != 0; }
int ent_peer_loading(uint64_t inc) { return (peer_flags(inc) & COOP_CTR_HB_LOADING) != 0; }

// ---- Digests (S 3.4) ------------------------------------------------------------------------------------------------------------
uint32_t ent_digest(const Eid *members, const uint8_t *life, int n, const Bond *b, int nb) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h = ent_fnv(h, &members[i].inc, 8), h = ent_fnv(h, &members[i].serial, 4);
        h = ent_fnv(h, &life[i], 1);
    }
    for (int i = 0; i < nb; i++) {
        h = ent_fnv(h, &b[i].a.inc, 8), h = ent_fnv(h, &b[i].a.serial, 4), h = ent_fnv(h, &b[i].b.inc, 8), h = ent_fnv(h, &b[i].b.serial, 4);
        h = ent_fnv(h, &b[i].sa, 1), h = ent_fnv(h, &b[i].sb, 1);
    }
    return h;
}
static int bond_cmp(const void *x, const void *y) {
    const Bond *a = x, *b = y;
    if (!eid_eq(a->a, b->a)) return eid_lt(a->a, b->a) ? -1 : 1;
    if (!eid_eq(a->b, b->b)) return eid_lt(a->b, b->b) ? -1 : 1;
    if (a->sa != b->sa) return a->sa < b->sa ? -1 : 1;
    return a->sb < b->sb ? -1 : a->sb > b->sb;
}
static int ent_ptr_cmp(const void *x, const void *y) {
    const Ent *a = *(Ent *const *)x, *b = *(Ent *const *)y;
    return eid_eq(a->id, b->id) ? 0 : eid_lt(a->id, b->id) ? -1 : 1;
}

// ---- Islands (S 3.3): the connected sets under the bonds read from the world --------------------------------------------------
static int16_t uf[ENT_N];
static int uf_find(int i) { while (uf[i] != i) i = uf[i] = uf[uf[i]]; return i; }
int ent_island_members(const Ent *e, Ent **out, int max) {
    int r = uf_find(ent_i(e)), n = 0;
    for (int i = 0; i < ENT_N && n < max; i++) if (ents[i].used && uf_find(i) == r) out[n++] = &ents[i];
    qsort(out, (size_t)n, sizeof *out, ent_ptr_cmp);
    return n;
}
int ent_island_bonds(Ent **members, int n, Bond *out, int max) {
    int nb = 0;
    for (int i = 0; i < bond_n && nb < max; i++) {
        int in = 0;
        for (int k = 0; k < n && !in; k++) in = eid_eq(bonds[i].a, members[k]->id) || eid_eq(bonds[i].b, members[k]->id);
        if (in) out[nb++] = bonds[i];
    }
    return nb;
}
uint32_t ent_digest_of(Ent **members, int n) {
    Eid id[ENT_MEMBERS];
    uint8_t life[ENT_MEMBERS];
    Bond b[ENT_BONDS];
    if (n > ENT_MEMBERS) n = ENT_MEMBERS;
    for (int i = 0; i < n; i++) id[i] = members[i]->id, life[i] = members[i]->life;
    int nb = ent_island_bonds(members, n, b, ENT_BONDS);
    return ent_digest(id, life, n, b, nb);
}

// ---- This Link: what it holds and rides (S 3.6), found once a frame -----------------------------------------------------------
static uint64_t link_actor_now, held_actor, held_hint;   // held_hint: the last actor a grab accept named (the ring)
static int16_t held_root = -1, ride_root = -1;           // uf roots of the island held / ridden, -1 none
uint64_t ent_held_actor(void) { return held_actor; }
void ent_held_hint(uint64_t actor) { held_hint = actor; }
int ent_this_link_holds(const Ent *e) { return held_root >= 0 && uf_find(ent_i(e)) == held_root; }
int ent_this_link_rides(const Ent *e) { return ride_root >= 0 && uf_find(ent_i(e)) == ride_root; }
static void link_read(void) {
    link_actor_now = coop_player_actor();
    held_actor = 0, held_root = ride_root = -1;
    if (held_hint && coop_held_here(held_hint)) held_actor = held_hint; else held_hint = 0;
    // (the grab hook's hint is one way to it; four times a second every entity here is asked too, so that a hold is
    // found whatever the hook saw)
    static int64_t scan_ms;
    if (!held_actor && link_actor_now && ent_ms() - scan_ms > 250) {
        scan_ms = ent_ms();
        for (int i = 0; i < ENT_N && !held_actor; i++)
            if (ents[i].used && ents[i].actor && coop_held_here(ents[i].actor)) held_actor = held_hint = ents[i].actor;
    }
    Ent *h = held_actor ? ent_by_actor(held_actor) : NULL;
    if (h) held_root = (int16_t)uf_find(ent_i(h));
    for (int i = 0; link_actor_now && i < ENT_N && ride_root < 0; i++) {   // (sticks only: the name is tested, not every part)
        Ent *e = &ents[i];
        if (!e->used || !e->actor || !strstr(e->name, "Stick")) continue;
        if (ctn_stick_rider(e->actor) == link_actor_now) ride_root = (int16_t)uf_find(i);
    }
}

// ---- Reading the world (S 3) -------------------------------------------------------------------------------------------------
static int bonds_dirty = 1;
static int64_t bonds_read_ms;
void ent_mark_due(Ent *e) { (void)e; bonds_dirty = 1; }

// Step 3: each entity's liveness and main-body matrix, one read each.
static void world_caches(void) {
    int64_t now = ent_ms();
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || !e->actor) continue;
        struct { uint32_t gen; uint8_t pad[0x10]; int32_t state; } hd;   // actor +0x10 .. +0x27
        if (!coop_rd(e->actor + 0x10, &hd, sizeof hd) || hd.gen != e->gen || hd.state < 2 || hd.state > 6) {
            // Gone here. An owned entity takes its life from its delete event (S 10.3), read from the ring before this
            // step; with none it is an unload. A copy's going is local (S 5.8).
            uint64_t was = e->actor;
            ent_unbind(e);
            if (e->owner == coop_ctr_me() && e->life == LIFE_LIVE && !e->death_due) {
                char b[24];
                rt_log("coop: ent: %s (%s) went here with no delete seen (actor %llx): an unload", ent_eid_str(e->id, b), e->name, (unsigned long long)was);
                e->life = e->flags & ENT_MAP ? LIFE_FREE : LIFE_GONE, e->dead_ms = now;
                if (e->flags & ENT_MAP) e->owner = 0;
                own_owe_ent(e, -1);
            }
            bonds_dirty = 1;
            continue;
        }
        if (!e->main_body) e->main_body = ctn_main_body(e->actor);
        float m[12];
        if (e->main_body && coop_rd(e->main_body + 0x98, m, sizeof m)) {
            float d = fabsf(m[3] - e->m[3]) + fabsf(m[7] - e->m[7]) + fabsf(m[11] - e->m[11]);
            if (d > 0.002f) e->moved_ms = now;
            memcpy(e->m, m, sizeof m);
        }
    }
}

// Step 5: the bonds of every entity here that has a group, from its real glue edges; then islands and digests.
static void bonds_read(Cpu *c) {
    static Bond nb[BOND_N];
    int n = 0, extra = 0;
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || !e->actor || !ctn_part_group(e->actor)) continue;
        CtnEdge ed[32];
        int ne = ctn_part_edges(c, e->actor, ed, 32);
        for (int k = 0; k < ne; k++) {
            Ent *o = ent_by_actor(ed[k].other);
            if (!o) {   // an end of a real glue edge of an entity that is none yet (S 4.4)
                void ent_enter_glued(Cpu *c, uint64_t actor);
                ent_enter_glued(c, ed[k].other);
                o = ent_by_actor(ed[k].other);
                if (!o) continue;
            }
            if (!eid_lt(e->id, o->id)) continue;   // (each edge is listed from both ends: taken from its lower entity)
            if (n >= BOND_N) { ent_full("bond"); break; }
            Bond *b = &nb[n];
            float held[12];
            memset(b, 0, sizeof *b);
            b->a = e->id, b->b = o->id, b->sa = ed[k].key, b->sb = ed[k].okey, b->edge = ed[k].edge, b->ed = ed[k].ed, b->ea = ed[k].end_a;
            // held = outA * inverse(outB): end B's body in end A's body frame. rel is b's body in a's body frame.
            if (!ctn_edge_held(c, ed[k].edge, held)) continue;
            if (ed[k].end_a) memcpy(b->rel, held, sizeof held);
            else ctn_m34_inv(b->rel, held);
            n++;
        }
    }
    qsort(nb, (size_t)n, sizeof *nb, bond_cmp);
    int w = 0;
    for (int i = 0; i < n; i++) {   // each (a, b, sa, sb) once; a surplus edge is logged (S 3.2)
        if (w && !bond_cmp(&nb[w - 1], &nb[i])) { extra++; continue; }
        nb[w++] = nb[i];
    }
    if (extra) {
        static int64_t said;
        if (ent_ms() - said > 5000) said = ent_ms(), rt_log("coop: ent: %d glue edges here repeat a bond another edge already holds", extra);
    }
    memcpy(bonds, nb, sizeof *nb * (size_t)w), bond_n = w;
    bonds_dirty = 0, bonds_read_ms = ent_ms();
}

static void islands_build(Cpu *c) {
    for (int i = 0; i < ENT_N; i++) uf[i] = (int16_t)i;
    for (int i = 0; i < bond_n; i++) {
        Ent *a = ent_by_id(bonds[i].a), *b = ent_by_id(bonds[i].b);
        if (a && b) uf[uf_find(ent_i(a))] = (int16_t)uf_find(ent_i(b));
    }
    // Owned islands: the owned members of each connected set. A row inherits the per-peer state of the island its
    // root was in before, so that state follows a build through merges and splits.
    static Island old[ISL_N];
    static int16_t was[ENT_N];
    uint64_t me = coop_ctr_me();
    memcpy(old, isls, sizeof old);
    for (int i = 0; i < ENT_N; i++) was[i] = ents[i].used ? ents[i].isl : -1, ents[i].isl = -1;
    memset(isls, 0, sizeof isls);
    int ni = 0;
    static uint8_t seen[ENT_N];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < ENT_N && me; i++) {
        Ent *e = &ents[i];
        if (!e->used || seen[i] || e->owner != me || e->life != LIFE_LIVE || (e->flags & ENT_PREVIEW) || !e->mir_known) continue;
        Ent *set[64], *own[ENT_MEMBERS];
        int ns = ent_island_members(e, set, 64), no = 0, far = 0;
        uint32_t ep = 0;
        for (int k = 0; k < ns; k++) {
            if (set[k]->owner == me && set[k]->life == LIFE_LIVE && !(set[k]->flags & ENT_PREVIEW) && set[k]->mir_known) {
                seen[ent_i(set[k])] = 1;
                if (no < ENT_MEMBERS) own[no++] = set[k];
                if (set[k]->epoch > ep) ep = set[k]->epoch;
            } else far = 1;
        }
        if (ni >= ISL_N) { ent_full("island"); break; }
        Island *is = &isls[ni];
        int from = was[ent_i(own[0])];
        if (from >= 0 && old[from].used) *is = old[from];
        else { memset(is, 0, sizeof *is); is->rec = -1; for (int p = 0; p < ENT_PEERS; p++) is->owed[p] = 1; }
        is->used = 1, is->root = own[0]->id, is->n = (uint16_t)no, is->has_far = (uint8_t)far;
        for (int k = 0; k < no; k++) {
            is->m[k] = (int16_t)ent_i(own[k]), own[k]->isl = (int16_t)ni;
            if (own[k]->epoch != ep) own[k]->epoch = ep;   // one epoch per island: a merge takes the greater (S 3.3)
            if (own[k]->moved_ms > is->moved_ms) is->moved_ms = own[k]->moved_ms;
        }
        is->epoch = ep;
        is->digest = ent_digest_of(own, no);
        uint8_t cls = ent_this_link_holds(own[0]) ? CLS_HOLD : ent_this_link_rides(own[0]) ? CLS_RIDE : CLS_IDLE;
        if (cls != is->cls) is->cls = cls, is->class_ms = ent_ms(), is->class_sends = 3;
        if (is->digest != is->stated_digest) for (int p = 0; p < ENT_PEERS; p++) is->owed[p] = 1;
        ni++;
    }
    // A recipe no island points at any more is free.
    for (int r = 0; r < REC_N; r++) {
        int ref = 0;
        for (int k = 0; k < ni && !ref; k++) ref = isls[k].rec == r;
        if (!ref) recipes[r].used = 0;
    }
    (void)c;
}

// ---- The published sets (S 11): rebuilt each frame, read lock-free from the hooks ---------------------------------------------
#define SET_SLOTS 2048
typedef struct { _Atomic uint64_t s[2][SET_SLOTS]; _Atomic int cur; uint64_t w[SET_SLOTS]; } EntSet;
static EntSet sets[ENT_SET_COUNT];
static void set_add(int which, uint64_t key) {
    if (!key) return;
    uint64_t *w = sets[which].w, h = actor_hash(key);
    for (unsigned p = 0; p < SET_SLOTS; p++) {
        unsigned k = (unsigned)((h + p) % SET_SLOTS);
        if (w[k] == key) return;
        if (!w[k]) { w[k] = key; return; }
    }
}
static void set_commit(int which) {
    EntSet *s = &sets[which];
    int n = !atomic_load(&s->cur);
    for (int i = 0; i < SET_SLOTS; i++) atomic_store_explicit(&s->s[n][i], s->w[i], memory_order_relaxed);
    atomic_store(&s->cur, n);
    memset(s->w, 0, sizeof s->w);
}
int ent_set_has(int which, uint64_t key) {
    if (!key) return 0;
    EntSet *s = &sets[which];
    int n = atomic_load_explicit(&s->cur, memory_order_relaxed);
    uint64_t h = actor_hash(key);
    for (unsigned p = 0; p < SET_SLOTS; p++) {
        uint64_t v = atomic_load_explicit(&s->s[n][(h + p) % SET_SLOTS], memory_order_relaxed);
        if (v == key) return 1;
        if (!v) return 0;
    }
    return 0;
}
int ent_set_list(int which, uint64_t *out, int max) {
    EntSet *s = &sets[which];
    int n = atomic_load(&s->cur), k = 0;
    for (int i = 0; i < SET_SLOTS && k < max; i++) {
        uint64_t v = atomic_load_explicit(&s->s[n][i], memory_order_relaxed);
        if (v) out[k++] = v;
    }
    return k;
}

// The group views (S 12.1) and the sets, from the table. A view is the members of one connected set that have one
// owner and an actor here; the kept code (coop_ctn.c) drives and reads devices from them.
static Grp *grp_for(uint64_t key64, uint64_t owner) {
    Grp *free = NULL;
    for (int i = 0; i < GRP_N; i++) {
        if (grps[i].used == 2 && grps[i].key64 == key64 && grps[i].owner == owner) return &grps[i];   // (kept from last frame)
        if (!grps[i].used && !free) free = &grps[i];
    }
    return free;
}
static void publish(Cpu *c) {
    uint64_t me = coop_ctr_me();
    int64_t now = ent_ms();
    static uint64_t part_list[ENT_N];
    int np = 0;
    static uint8_t seen[ENT_N];
    memset(seen, 0, sizeof seen);
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < GRP_N; i++) if (grps[i].used) grps[i].used = 2;   // 2: last frame's, to be matched or freed
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || !e->actor || e->life != LIFE_LIVE) continue;
        set_add(ENT_SET_ENT, e->actor);
        if (e->flags & ENT_COPY) set_add(ENT_SET_COPY, e->actor);
        if (e->owner == me) set_add(ENT_SET_OWNED, e->actor);
        if (seen[i] || (e->flags & ENT_PREVIEW)) continue;
        Ent *set[64], *mem[ENT_MEMBERS];
        int ns = ent_island_members(e, set, 64), nm = 0;
        for (int k = 0; k < ns; k++)
            if (set[k]->owner == e->owner && set[k]->actor && set[k]->life == LIFE_LIVE && nm < ENT_MEMBERS) mem[nm++] = set[k], seen[ent_i(set[k])] = 1;
        int mine = e->owner == me, asked = 0, wanted = 0, riding = 0;
        for (int k = 0; k < nm; k++) asked |= mem[k]->asked, wanted |= mem[k]->bond_wanted;
        asked |= !mine && (rec_op_open_for(e) || ent_this_link_holds(e) || ent_this_link_rides(e));
        riding = ent_this_link_rides(e);
        (void)riding;
        uint64_t key = eid_hash(mem[0]->id);
        Grp *g = grp_for(key, e->owner);
        if (!g) { ent_full("group view"); continue; }
        if (g->used != 2) {   // a new row: no samples yet, no drive state
            struct FollowState *fs = g->fs;
            memset(g, 0, sizeof *g);
            g->fs = fs, g->key64 = key, g->owner = e->owner;
        }
        g->used = 1, g->mine = (uint8_t)mine, g->asked = (uint8_t)(asked || wanted), g->root = mem[0]->id, g->n = nm, g->stamp_ms = now;
        g->no_write = 0, g->agree = 0;
        float inv[12];
        int have_inv = ctn_m34_inv(inv, mem[0]->m);
        Stated *st = !mine && mem[0]->st >= 0 ? &stated[mem[0]->st] : NULL;
        if (!mine) g->agree = (uint8_t)(st ? st->state == ST_AGREE : mem[0]->state == ST_AGREE);
        for (int k = 0; k < nm; k++) {
            g->parts[k] = mem[k]->actor, g->gen[k] = mem[k]->gen, g->id[k] = mem[k]->id;
            if (have_inv) ctn_m34_mul(g->rel_root[k], inv, mem[k]->m);
            if (nm >= 2) part_list[np++] = mem[k]->actor, set_add(ENT_SET_PART, mem[k]->actor);
            if (!mine) {
                set_add(ENT_SET_FOLLOWER, mem[k]->actor);
                uint64_t body[8];
                uint8_t key8[8];
                int nbod = ctn_bodies(c, mem[k]->actor, body, key8, 8);
                for (int b = 0; b < nbod; b++) set_add(ENT_SET_FOLLOWER_BODY, body[b]);
                if (!g->asked) {
                    set_add(ENT_SET_DRIVEN, mem[k]->actor), set_add(ENT_SET_DRIVEN_BODY, mem[k]->main_body);
                    if (strstr(mem[k]->name, "Cannon") || strstr(mem[k]->name, "Beam") || strstr(mem[k]->name, "Emitter") || strstr(mem[k]->name, "Shooter"))
                        set_add(ENT_SET_SHOOTER, mem[k]->actor);
                }
            }
        }
        if (!mine) {
            uint8_t cls = st ? st->cls : mem[0]->cls;
            int64_t cls_ms = st ? st->cls_ms : mem[0]->cls_ms;
            // A class does not age while its owner's heartbeat says PAUSED (S 5.7); else it is IDLE after 2 s.
            if (cls != CLS_IDLE && now - cls_ms > 2000 && !ent_peer_paused(e->owner)) cls = CLS_IDLE;
            if (cls != CLS_IDLE)
                for (int k = 0; k < nm; k++) if (strstr(mem[k]->name, "Stick")) set_add(ENT_SET_REFUSE_STICK, mem[k]->actor);
            uint64_t grp = ctn_part_group(mem[0]->actor);
            if (grp && nm >= 2) set_add(ENT_SET_FOLLOW_GROUP, grp);
        }
        // members behind a body other than the main one (S 3.5): a bond on the way to the root at a key other than 0
        if (nm >= 2) {
            static Bond ib[ENT_BONDS];
            int nib = ent_island_bonds(mem, nm, ib, ENT_BONDS);
            uint32_t loose = 0;
            for (int pass = 0; pass < nm; pass++)
                for (int q = 0; q < nib; q++) {
                    int ia = -1, ibx = -1;
                    for (int k = 0; k < nm; k++) { if (eid_eq(mem[k]->id, ib[q].a)) ia = k; if (eid_eq(mem[k]->id, ib[q].b)) ibx = k; }
                    if (ia < 0 || ibx < 0) continue;
                    // (a and b are joined rigidly only when both ends are main bodies; else the side away from the root
                    // moves on a joint)
                    if (ib[q].sa == 0 && ib[q].sb == 0) { if ((loose >> ia & 1) != (loose >> ibx & 1)) loose |= 1u << ia | 1u << ibx; continue; }
                    int far = ia == 0 ? ibx : ibx == 0 ? ia : (loose >> ia & 1) ? ibx : ia;
                    if (far) loose |= 1u << far;
                }
            g->no_write = loose & ~1u;
        }
        if (mine) ctn_owner_state(c, g);
        if (g->payer && g->payer != me) for (int k = 0; k < nm; k++) set_add(ENT_SET_NOCHARGE, mem[k]->actor);
        else if (!mine && !g->payer) for (int k = 0; k < nm; k++) set_add(ENT_SET_NOCHARGE, mem[k]->actor);
    }
    for (int i = 0; i < GRP_N; i++) if (grps[i].used == 2) { free(grps[i].fs); memset(&grps[i], 0, sizeof grps[i]); }
    plat_mutex_unlock(&grp_lock);
    for (int w = 0; w < ENT_SET_COUNT; w++) set_commit(w);
    static int64_t veto_ms;
    if (now - veto_ms > 250) veto_ms = now, coop_life_veto_rebuild(c, part_list, np);
}

// An entity as the riders name it between the games (coop_players.c: the stand-in of a player who stands on it is
// placed on it): a hash of its Eid, never 0. Read without the lock, on the co-op tick: a look-up that races a change
// of the table misses for one tick.
int coop_ent_ride_name(uint64_t actor, uint32_t *id) {
    Ent *e = ent_on() ? ent_by_actor(actor) : NULL;
    if (!e) return 0;
    *id = (uint32_t)eid_hash(e->id) | 1;
    return 1;
}

// ---- Deleting an actor ourselves (S 7.2) ------------------------------------------------------------------------------------------
static __thread int ours_delete;
int coop_ent_ours_delete(void) { return ours_delete; }
void ent_delete_actor(Cpu *c, Ent *e) {
    if (!e->actor) return;
    coop_obj_entity(e->actor);   // (the mirror tells nobody: its rows for the actor go first)
    ours_delete = 1;
    ctn_part_delete(c, e->actor);
    ours_delete = 0;
    ent_unbind(e);
    bonds_dirty = 1;
}

// ---- The frame's pass (S 1.1) -------------------------------------------------------------------------------------------------------
void ent_ring_events(Cpu *c);       // below: identity and the hooks' events
void ent_creates(Cpu *c);
void ent_status_line(void);

void coop_ent_phase0(Cpu *c) {
    if (!ent_on()) return;
    coop_lock_wait(&ent_lock);
    atomic_store(&phase0_ms, ent_ms());
    own_peer_events();
    // 1. the pause queue, then the transport, in arrival order; then the latest-only slots
    {
        static CoopCtrMsg m;
        while (own_pq_take(&m)) own_receive(c, &m);
        while (coop_ctr_recv(&m)) own_receive(c, &m);
        own_slots();
    }
    ent_ring_events(c);                                   // 2
    world_caches();                                       // 3
    ent_creates(c), ctn_make_pass(c);                     // 4
    if (bonds_dirty || ent_ms() - bonds_read_ms > 250) bonds_read(c);   // 5
    islands_build(c);
    link_read();
    islands_build(c);                                     // (the class needs this Link's island: built again with it)
    own_timers(c);                                        // 6
    own_causes(c), rec_ops(c);                            // 7
    own_send(c);                                          // 8
    if (!ent_loading()) rec_pass(c);                      // 9
    ctn_phase0_kept(c);
    publish(c);                                           // 10
    ent_status_line();
    plat_mutex_unlock(&ent_lock);
}
void coop_ent_drive(Cpu *c) {
    if (!ent_on() || ent_loading() || ent_paused()) return;
    ctn_drive(c);
}
void coop_ent_net(void) {
    if (!ent_on()) return;
    CoopCtrHb hb = {0};
    if (ent_paused()) hb.flags |= COOP_CTR_HB_PAUSED;
    if (ent_loading()) hb.flags |= COOP_CTR_HB_LOADING;
    coop_ctr_hb_set(&hb);
    own_net();
}

// ---- The status line (S 12.3) -----------------------------------------------------------------------------------------------------
void ent_status_line(void) {
    static int64_t said;
    static char last[200];
    int64_t now = ent_ms();
    uint64_t me = coop_ctr_me();
    int owned = 0, oi = 0, copies = 0, far = 0, wait = 0, notshown = 0, differ = 0;
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || e->life != LIFE_LIVE) continue;
        if (e->owner == me) owned++;
        else {
            copies += e->actor != 0;
            if (e->st < 0) far += e->state == ST_FAR, wait += e->state == ST_WAIT_MAKER, notshown += e->state == ST_NOT_SHOWN, differ += e->state == ST_DIFFER;
        }
    }
    for (int i = 0; i < ISL_N; i++) oi += isls[i].used;
    for (int i = 0; i < STATED_N; i++)
        if (stated[i].used) far += stated[i].state == ST_FAR, wait += stated[i].state == ST_WAIT_MAKER, notshown += stated[i].state == ST_NOT_SHOWN, differ += stated[i].state == ST_DIFFER;
    char line[200], unc[96] = "";
    int ul = 0;
    for (int i = 0; i < ISL_N && ul < 70; i++)
        for (int p = 0; isls[i].used && p < ENT_PEERS; p++)
            if (isls[i].peer[p].diff_since && now - isls[i].peer[p].diff_since > 1000) {
                char b[24];
                ul += snprintf(unc + ul, sizeof unc - (size_t)ul, " %d:%s:%llds", p + 1, ent_eid_str(isls[i].root, b), (long long)((now - isls[i].peer[p].diff_since) / 1000));
            }
    snprintf(line, sizeof line, "owned %d (%d islands) | copies %d | far %d | wait %d | differ %d | not shown %d | bonds %d | unconverged%s | ring lost %u",
             owned, oi, copies, far, wait, differ, notshown, bond_n, unc[0] ? unc : " none", (unsigned)atomic_load(&ring_lost));
    if (now - said < 10000 && !strcmp(line, last)) return;
    if (now - said < 1000) return;
    said = now, memcpy(last, line, sizeof last);
    rt_log("coop: ent: %lld %s", (long long)now, line);
}
