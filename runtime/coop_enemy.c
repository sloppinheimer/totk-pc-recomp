// Co-op: the enemy sync (docs/coop/enemy-sync-spec.md): the table, identity, ownership, messages and the record.
//
// One game runs each enemy (thinks it) and states it; in every other game that has the actor it is a copy that shows
// what is stated and does not think (coop_enemy_copy.c). Ownership is per enemy, by an (epoch, incarnation) pair that
// only rises (en_apply_owner), stated as CLAIM on the one reliable stream everything else of an enemy travels on.
//
// Threads. The net tick (coop_enemy_net, twice a frame, also in a menu and a load) reads the reliable messages and
// changes the table by them, keeps the peers, sends what is owed and ends offers. Phase 0 (coop_enemy_phase0, once a
// frame at GameDataMgr::calc, outside the parallel actor update) looks at this game's actors: binds them to entries,
// claims, offers, takes over. Both hold en_lock. An actor's own update thread (coop_enemy_precalc and the switch's
// hooks) reads only the map cell of its actor, whose fields the hooks need are atomic.
//
// Settings: TOTK_COOP_ENEMY=1 turns it on (coop_world.c's older enemy sync then leaves the kinds listed in kind_of to
// this file and goes on for the others). TOTK_COOP_EN_NOSWITCH=1: ownership,
// claims and the record run, and no copy is switched off or moved (step 1 of the specification).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_msg.h"
#include "coop_net.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);   // coop_spawn.c

int coop_enemy_new(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_ENEMY"); on = e && *e == '1'; }
    return on;
}

int en_test_skip(char what) {
    static const char *s;
    static int read;
    if (!read) s = cfg_get("TOTK_COOP_EN_TEST_SKIP"), read = 1;
    return s && strchr(s, what) != NULL;
}

PlatMutex en_lock = PLAT_MUTEX_INIT;
En en_tab[EN_N];
EnCell en_map[EN_MAP_N];
_Atomic int en_active, en_world_stands;
int coop_enemy_world_stands(void) { return coop_enemy_new() && atomic_load(&en_world_stands); }

#define SEC 1000000000ull
uint64_t en_now(void) { return plat_now_ns(); }

// ---- Kinds -----------------------------------------------------------------------------------------------------------
// The Bokoblin shape (S 0): one actor with a character controller and the graph units the switch turns away.
static int kind_of(const char *nm) {
    // (the first four are one shape; the others were each tried by spawning one beside a Link with a second game
    // watching, and in a tester's session: made in the other game, followed, no leak. A swarm is not one actor.)
    static const char *const pre[] = { "Enemy_Bokoblin", "Enemy_Moriblin", "Enemy_Horablin", "Enemy_Lizalfos", "Enemy_Chuchu", "Enemy_Keese",
                                       "Enemy_Octarock", "Enemy_Wizzrobe", "Enemy_Zonau_Robot", "Enemy_Zonau_Golem", "Enemy_Assassin" };
    if (strstr(nm, "Swarm")) return 0;
    for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++)
        if (!strncmp(nm, pre[i], strlen(pre[i]))) return (int)i + 1;
    // Test (TOTK_COOP_EN_KINDS=<prefix>[,<prefix>...]): more kinds by the start of their actor names, for trying a family.
    static const char *more;
    static int read;
    if (!read) more = cfg_get("TOTK_COOP_EN_KINDS"), read = 1;
    for (const char *p = more; p && *p;) {
        const char *q = strchr(p, ',');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n && !strncmp(nm, p, n)) return 100;
        p = q ? q + 1 : NULL;
    }
    return 0;
}

int coop_enemy_kind(const char *nm) { return kind_of(nm); }

// ---- Reads of the game -----------------------------------------------------------------------------------------------
int en_alive(uint64_t a, uint32_t gen) {
    int32_t st = 0;
    uint32_t g = 0;
    return a > 1 && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 && coop_rd(a + 0x10, &g, 4) && g == gen;
}
uint64_t en_hash_of(uint64_t a) {
    uint64_t p = coop_rd_ptr(a + 0x370), h = 0;
    if (!p || !coop_rd(p + 0x88, &h, 8) || h == ~0ull) return 0;
    return h;
}
// The far gate of the actor's calc (S 4.1): closed, the game does not run its graphs.
int en_gate_open(uint64_t a) {
    uint64_t comps = en_comps(a), bsa = en_bsa(a);
    if (!bsa) return 1;
    uint8_t b = 0;
    if (coop_rd(bsa + 0x26ad, &b, 1) && (b & 0x20)) return 1;
    uint64_t A = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(bsa + 0x25f0) + 0x18) + 0x150), X = A ? coop_rd_ptr(A + 0x38) : 0;
    if (!A || !X) return 1;
    if (!coop_rd(A + 0x41, &b, 1) || !(b & 1)) return 1;
    if (!coop_rd(X + 0x2d2, &b, 1) || !(b & 1)) return 1;
    float r = 0, d2 = 0;
    if (!coop_rd(X + 0x264, &r, 4) || r < 0) return 1;
    uint64_t lod = coop_rd_ptr(comps + 0xb0);
    if (!lod || !coop_rd(lod + 0x84, &d2, 4)) return 1;
    return d2 <= r * r;
}
int32_t en_hp_of(uint64_t a) {
    uint64_t l = coop_rd_ptr(en_comps(a) + 0x2f8), h = l ? coop_rd_ptr(l + 0x1830) : 0;
    int32_t hp = -1;
    if (h) coop_rd(h + 8, &hp, 4);
    return hp;
}
void en_quat_of_rot(const float r[9], float q[4]) {   // rows r[0..2], r[3..5], r[6..8]; q = x, y, z, w
    float t = r[0] + r[4] + r[8];
    if (t > 0) { float s = sqrtf(t + 1) * 2; q[3] = s / 4, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s; }
    else if (r[0] > r[4] && r[0] > r[8]) { float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2; q[3] = (r[7] - r[5]) / s, q[0] = s / 4, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s; }
    else if (r[4] > r[8]) { float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2; q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = s / 4, q[2] = (r[5] + r[7]) / s; }
    else { float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2; q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = s / 4; }
}
void en_rot_of_quat(const float q[4], float r[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
void en_pose_of(uint64_t a, float pose[7]) {
    float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memset(pose, 0, 28), pose[6] = 1;
    coop_rd(a + COOP_ACTOR_POS, pose, 12);
    if (coop_rd(a + COOP_ACTOR_ROT, r, 36)) en_quat_of_rot(r, pose + 3);
}

static uint64_t session_ns;   // when the session with a peer on this sync began (0: none)

// ---- The map: actor -> cell (S 1.1) -----------------------------------------------------------------------------------
static _Atomic uint64_t not_ours[8192];   // actors seen that are of no listed kind: (actor ^ generation << 44)
static uint32_t map_h(uint64_t a) { return (uint32_t)((a >> 4) * 2654435761u); }

EnCell *en_cell_of(uint64_t actor) {
    uint32_t h = map_h(actor) & (EN_MAP_N - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (EN_MAP_N - 1)) {
        uint64_t a = atomic_load_explicit(&en_map[h].actor, memory_order_relaxed);
        if (a == actor) return &en_map[h];
        if (!a) return NULL;
    }
    return NULL;
}
int en_cell_state(uint64_t actor) {
    EnCell *m = actor > 1 ? en_cell_of(actor) : NULL;
    return m ? atomic_load_explicit(&m->state, memory_order_relaxed) : EM_FREE;
}
// A blow of this game's player on a copy goes to the copy's owner whole (enemy-sync-spec.md B.10): coop_world.c asks
// where to (the owner's player id, 0 none) and under which 32-bit name; the owner's game finds its actor by that name.
static uint32_t en_key32(Enid id) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) h = (h ^ (uint8_t)(id.a >> (i * 8))) * 16777619u;
    for (int i = 0; i < 4; i++) h = (h ^ (uint8_t)(id.b >> (i * 8))) * 16777619u;
    return h ? h : 1;
}
int coop_enemy_contact_route(uint64_t actor, uint32_t *key, uint32_t attack_id) {
    // OFF unless TOTK_COOP_EN_FWD=1: with it, the enemy struck by the other player flew into the sky in the game that
    // runs it (a play test with pictures, 2026-11-16). What of the sent contact entry launches it is not found; until
    // then a blow goes as lost life (HIT), as before.
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_FWD"); on = e && *e == '1'; }
    if (!on) return 0;
    EnCell *m = actor > 1 ? en_cell_of(actor) : NULL;
    int16_t ei = m ? atomic_load(&m->en) : -1;
    if (!m || ei < 0 || atomic_load(&m->state) != EM_COPY || !en_tab[ei].used || en_tab[ei].life != EN_LIVE) return 0;
    *key = en_key32(en_tab[ei].id);
    uint32_t fr = coop_sess_frame_no();
    int again = attack_id && m->fwd_id == attack_id && fr - m->fwd_frame < 90;   // (the same blow, still in the list: sent once)
    m->fwd_frame = fr, m->fwd_id = attack_id;
    return again ? -1 : en_peer_id(en_tab[ei].owner);
}
uint64_t coop_enemy_contact_actor(uint32_t key, int from) {
    uint64_t me = coop_ctr_me(), a = 0;
    coop_lock_wait(&en_lock);
    for (int i = 0; i < EN_N && !a; i++)
        if (en_tab[i].used && en_tab[i].owner == me && en_tab[i].life == EN_LIVE && en_key32(en_tab[i].id) == key) {
            a = en_tab[i].actor;
            en_tab[i].hit_by = from, en_tab[i].hit_by_ns = en_now();   // (the struck hand-over's clue, as a HIT sets it)
        }
    plat_mutex_unlock(&en_lock);
    return a;
}
// For the other syncs (the element sync asks who decides for an enemy actor): 0 not an enemy this sync knows, 1 this
// game decides for it (it runs it for all, or it is unsynced and this game's own), 2 a copy of another game's enemy
// (or held for a moment before that is known).
int coop_enemy_role(uint64_t actor) {
    int st = en_cell_state(actor);
    return st == EM_FREE ? 0 : st == EM_COPY || st == EM_NEW ? 2 : 1;
}
// The sync's identity of an enemy actor (placed: its placement hash and life number; made at run time: the spawn
// sync's key and EN_RT), for another sync that states something of it. 0: the actor is not bound to an entry.
int coop_enemy_key(uint64_t actor, uint64_t *a, uint32_t *b) {
    EnCell *m = actor > 1 ? en_cell_of(actor) : NULL;
    if (!m || !m->hash) return 0;
    *a = m->hash, *b = m->rt ? EN_RT : en_life_no();
    return 1;
}
// The cell of an actor at its calc, made when the actor is of a listed kind and has none. Only the actor's own update
// thread inserts it, so one actor is never inserted twice at once.
static EnCell *map_touch(uint64_t actor) {
    uint32_t gen = 0;
    {   // Test (TOTK_COOP_EN_TEST_BLIND=<s>): for that long after the session began this game takes none of its own
        // actors into the sync, as a player does who comes to a camp after the other has fought there.
        static int blind = -1;
        if (blind < 0) { const char *e = cfg_get("TOTK_COOP_EN_TEST_BLIND"); blind = e ? atoi(e) : 0; }
        if (blind && (!session_ns || en_now() - session_ns < (uint64_t)blind * SEC)) return NULL;
    }
    if (!coop_rd(actor + 0x10, &gen, 4)) return NULL;
    EnCell *m = en_cell_of(actor);
    if (m) {
        if (atomic_load_explicit(&m->gen, memory_order_relaxed) == gen) return m;
        // another actor in a gone one's memory: the cell starts again, and phase 0 lets the old binding go
        atomic_store(&m->en, -1);
        atomic_store(&m->state, EM_FREE);
        atomic_store(&m->actor, 1);
    }
    uint64_t tag = actor ^ ((uint64_t)gen << 44);
    _Atomic uint64_t *no = &not_ours[(map_h(actor) >> 3) & 8191];
    if (atomic_load_explicit(no, memory_order_relaxed) == tag) return NULL;
    char nm[64] = "";
    int kind = kind_of(coop_actor_name(actor, nm, sizeof nm));
    if (!kind) { atomic_store_explicit(no, tag, memory_order_relaxed); return NULL; }
    uint32_t h = map_h(actor) & (EN_MAP_N - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (EN_MAP_N - 1)) {
        uint64_t a = atomic_load(&en_map[h].actor);
        if (a > 1 || !atomic_compare_exchange_strong(&en_map[h].actor, &a, actor)) continue;
        m = &en_map[h];
        atomic_store(&m->gen, gen), atomic_store(&m->en, -1);
        atomic_store(&m->run_frame, 0), atomic_store(&m->warm, 0);
        m->born_frame = coop_sess_frame_no();
        m->kind = (uint8_t)kind, m->hash_read = 0, m->hash = 0, m->rt = 0, m->retry = 0, m->new_ns = en_now();
        m->sw_on = m->sw_saved = m->body_static = 0, m->show_on = 0;
        atomic_store(&m->turned, 0), atomic_store(&m->leaks, 0);
        for (int k = 0; k < EN_EQ_N; k++) atomic_store(&m->eq_make[k], 0);
        m->eq_made_frame = 0, memset(m->eq_tries, 0, sizeof m->eq_tries);
        atomic_store(&m->eq_drop, 0), atomic_store(&m->hit_dmg, 0), atomic_store(&m->dying, 0), atomic_store(&m->kill_req, 0), atomic_store(&m->put_away, 0), atomic_store(&m->flinch, 0);
        m->put_done = m->dc_saved = m->req_tries = m->pinned = 0, m->hit_ns = 0, m->show_dstate = 0, m->hp_written = 0;
        for (int k = 0; k < EN_SLOT_N; k++) m->slot_try[k] = 0;
        // held from its first frame while a session is on (S 4.2); phase 0 says what it is
        atomic_store(&m->state, atomic_load(&en_active) ? EM_NEW : EM_OWN);
        return m;
    }
    return NULL;   // (the run is full: the actor is the game's own, and its calc asks again)
}

// ---- The table -------------------------------------------------------------------------------------------------------
typedef struct { Enid id; uint32_t epoch; int32_t hp; float pose[7]; } EnRest;
typedef struct { uint64_t hash; uint32_t life_no; uint8_t flag; } EnTomb;
static EnRest rest[EN_REST_N];
static EnTomb tomb[EN_TOMB_N];
static int n_rest, n_tomb;
static uint32_t life_no = 1;          // 1 + the blood moons this session has stated
static uint32_t n_full, n_dropped, n_changes;

static En *en_find(Enid id) {
    for (int i = 0; i < EN_N; i++) if (en_tab[i].used && enid_eq(en_tab[i].id, id)) return &en_tab[i];
    return NULL;
}
static En *en_by_hash(uint64_t hash) {
    for (int i = 0; i < EN_N; i++) if (en_tab[i].used && en_tab[i].id.a == hash) return &en_tab[i];
    return NULL;
}
static EnRest *rest_find(Enid id) {
    for (int i = 0; i < n_rest; i++) if (enid_eq(rest[i].id, id)) return &rest[i];
    return NULL;
}
static void rest_del(Enid id) {
    EnRest *r = rest_find(id);
    if (r) *r = rest[--n_rest];
}
static void rest_add(Enid id, uint32_t epoch, int32_t hp, const float pose[7]) {
    EnRest *r = rest_find(id);
    if (r && r->epoch >= epoch) return;
    if (!r) { if (n_rest >= EN_REST_N) { n_full++; return; } r = &rest[n_rest++]; }
    r->id = id, r->epoch = epoch, r->hp = hp, memcpy(r->pose, pose, 28);
}
static EnTomb *tomb_find(uint64_t hash, uint32_t no) {
    for (int i = 0; i < n_tomb; i++) if (tomb[i].hash == hash && tomb[i].life_no == no) return &tomb[i];
    return NULL;
}
int en_tomb_add(uint64_t hash, uint32_t no, int flag) {
    if (no < life_no || tomb_find(hash, no)) return 0;
    if (n_tomb >= EN_TOMB_N) { n_full++; return 0; }
    tomb[n_tomb].hash = hash, tomb[n_tomb].life_no = no, tomb[n_tomb].flag = (uint8_t)flag, n_tomb++;
    return 1;
}
int en_tombed(uint64_t hash) { return tomb_find(hash, life_no) != NULL; }

uint32_t en_life_no(void) { return life_no; }
static void cell_unbind(En *e) {
    if (e->cell >= 0) {
        EnCell *m = &en_map[e->cell];
        if (atomic_load(&m->en) == (int16_t)(e - en_tab)) atomic_store(&m->en, -1), atomic_store(&m->state, EM_OWN), m->retry = 1;
    }
    e->actor = 0, e->gen = 0, e->cell = -1;
}
static void en_free(En *e) {
    cell_unbind(e);
    memset(e, 0, sizeof *e);
    e->cell = -1;
}
void en_entry_free(En *e) { en_free(e); }
static En *en_alloc(Enid id) {
    for (int i = 0; i < EN_N; i++)
        if (!en_tab[i].used) {
            En *e = &en_tab[i];
            memset(e, 0, sizeof *e);
            e->used = 1, e->id = id, e->life = EN_LIVE, e->cell = -1, e->made_ns = en_now();
            return e;
        }
    static uint64_t said;
    n_full++;
    if (en_now() - said > 10 * SEC) said = en_now(), rt_log("coop: en: the table is full (%d entries): %016llx is not taken", EN_N, (unsigned long long)id.a);
    return NULL;
}
// A blood moon (enemy-sync-spec.md 10.5, B.12): the life number rises to `no`; every tombstone, resting record and
// entry of a placement's earlier life is dropped (the moon resets the placements' dead flags and their enemies are
// made again, bound anew under the new number). Entries of enemies made at run time stay. Under en_lock.
// [open: a tombstone whose save flag the moon does not reset (reset masks 208 and 80: cave and sky ones) is dropped
// too; the mask is not read yet.]
static uint64_t moon_ns;
static void moon_apply(uint32_t no, const char *why) {
    if (no <= life_no) return;
    int tombs = n_tomb, rests = 0, ents = 0;
    life_no = no, moon_ns = en_now(), n_tomb = 0;
    for (int i = 0; i < n_rest;) if (rest[i].id.b != EN_RT && rest[i].id.b < no) rest[i] = rest[--n_rest], rests++; else i++;
    for (int i = 0; i < EN_N; i++) if (en_tab[i].used && en_tab[i].id.b != EN_RT && en_tab[i].id.b < no) en_free(&en_tab[i]), ents++;
    rt_log("coop: en: moon: %s: life number %u now; %d tombstones, %d resting records and %d entries of the life before dropped", why, no, tombs, rests, ents);
}

void en_log_own(const En *e, const char *what, const char *why) {
    rt_log("coop: en: own: %s %016llx/%u epoch %u %s (%s)", e->name[0] ? e->name : "?", (unsigned long long)e->id.a, e->id.b, e->epoch, what, why);
}

// ---- Peers and sending ------------------------------------------------------------------------------------------------
#define OUT_N 256
typedef struct { uint8_t type; uint16_t len; uint8_t d[COOP_CTR_PAYLOAD]; } OutMsg;
static struct {
    uint64_t inc;
    uint8_t ready, cap_bad, sync_end, burst_owed, was_loading, index_bad, state;
    uint64_t ready_ns, paused_ns, need_all_ns, said_ns;
    OutMsg *out; uint32_t out_r, out_w;
} peer[COOP_MAX_PLAYERS + 1];
static uint64_t load_end_ns;
static uint32_t n_out_full;

int en_peer_id(uint64_t inc) {
    for (int id = 1; inc && id <= COOP_MAX_PLAYERS; id++) if (peer[id].inc == inc) return id;
    return 0;
}
void en_send(int id, int type, const void *d, int len) {
    if (id < 1 || id > COOP_MAX_PLAYERS || !peer[id].ready || len < 0 || len > (int)sizeof peer[id].out->d) return;
    if (!peer[id].out) peer[id].out = calloc(OUT_N, sizeof *peer[id].out);
    if (!peer[id].out || peer[id].out_w - peer[id].out_r >= OUT_N) { n_out_full++; return; }
    OutMsg *o = &peer[id].out[peer[id].out_w++ % OUT_N];
    o->type = (uint8_t)type, o->len = (uint16_t)len;
    memcpy(o->d, d, (size_t)len);
}
void en_send_all(int type, const void *d, int len) {
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) if (peer[id].ready) en_send(id, type, d, len);
}
static void out_flush(void) {
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++)
        while (peer[id].out && peer[id].out_r != peer[id].out_w) {
            OutMsg *o = &peer[id].out[peer[id].out_r % OUT_N];
            if (!coop_ctr_send(id, o->type, COOP_CTR_RELIABLE, o->d, o->len)) break;   // (kept, in order, for the next pass)
            peer[id].out_r++;
        }
}

// ---- Messages ---------------------------------------------------------------------------------------------------------
typedef struct { Enid id; uint32_t epoch; uint8_t life, pad[3]; } MClaimRow;
typedef struct { uint8_t n, first, pad[6]; MClaimRow r[40]; } MClaim;
typedef struct { Enid id; uint32_t epoch, decl_no; int64_t ns; int32_t hp; uint8_t life, kind, pad[2]; char name[48]; float pose[7]; uint32_t pad2; } MDecl;
typedef struct { Enid id; uint32_t epoch; int32_t hp; float pose[7]; uint8_t dying, all, pad[2]; uint64_t to; } MRestRow;
typedef struct { uint8_t n, pad[7]; MRestRow r[16]; } MRows;
typedef struct { uint64_t hash; uint32_t life_no; uint8_t flag, pad[3]; } MTombRow;
typedef struct { uint8_t n, pad[7]; MTombRow r[64]; } MTombs;
typedef struct { Enid id; } MNeed;
typedef struct { uint32_t life_no, count; } MSyncEnd;
typedef struct { uint64_t key; uint32_t count, hash; } MIndex;
typedef struct { uint64_t hash; uint8_t why, pad[7]; } MUnsynced;
_Static_assert(sizeof(MClaim) <= 1100 && sizeof(MRows) <= 1100 && sizeof(MTombs) <= 1100 && sizeof(MDecl) <= 1100, "enemy messages fit one transport payload");

static void claim_send(int id, const En *e) {   // id 0: everyone
    MClaim m;
    memset(&m, 0, sizeof m);
    m.n = 1, m.r[0].id = e->id, m.r[0].epoch = e->epoch, m.r[0].life = e->life;
    int len = (int)(offsetof(MClaim, r) + sizeof m.r[0]);
    if (id) en_send(id, ENM_CLAIM, &m, len); else en_send_all(ENM_CLAIM, &m, len);
}
static void decl_send(int id, En *e) {
    MDecl m;
    memset(&m, 0, sizeof m);
    m.id = e->id, m.epoch = e->epoch, m.decl_no = ++e->decl_no, m.ns = (int64_t)coop_sess_frame_ns(), m.hp = e->hp, m.life = e->life, m.kind = e->kind;
    memcpy(m.name, e->name, sizeof m.name), memcpy(m.pose, e->pose, 28);
    if (id) en_send(id, ENM_DECL, &m, sizeof m); else en_send_all(ENM_DECL, &m, sizeof m);
}
static void row_send(int id, int type, const En *e, int all) {
    MRows m;
    memset(&m, 0, sizeof m);
    m.n = 1, m.r[0].id = e->id, m.r[0].epoch = e->epoch, m.r[0].hp = e->hp, memcpy(m.r[0].pose, e->pose, 28);
    m.r[0].dying = e->life == EN_DYING, m.r[0].all = (uint8_t)all, m.r[0].to = type == ENM_OFFER ? e->offer_to : 0;
    int len = (int)(offsetof(MRows, r) + sizeof m.r[0]);
    if (id) en_send(id, type, &m, len); else en_send_all(type, &m, len);
}
static void unsynced_set(uint64_t hash, int why, int tell) {
    En *e = en_by_hash(hash);
    if (e && e->life == EN_UNSYNCED) return;
    if (!e) e = en_alloc((Enid){hash, life_no, 0});
    if (!e) return;
    cell_unbind(e);
    e->life = EN_UNSYNCED, e->owner = 0;
    rt_log("coop: en: unsynced: %016llx %s: %s", (unsigned long long)hash, e->name, why == 1 ? "the games' actors there have different names" : why == 2 ? "a graph node ran on its copy" : "a peer said so");
    if (tell) { MUnsynced m = {hash, (uint8_t)why, {0}}; en_send_all(ENM_UNSYNCED, &m, sizeof m); }
}
void en_unsynced(uint64_t hash, int why) { unsynced_set(hash, why, 1); }

// Everything this game knows, to one peer: at its join, after its load, and when it asks (S 10.6, 11.3).
static void burst(int id) {
    MClaim cm;
    memset(&cm, 0, sizeof cm);
    cm.first = 1;
    uint32_t count = 0;
    uint64_t me = coop_ctr_me();
    for (int i = 0; i <= EN_N; i++) {
        En *e = i < EN_N ? &en_tab[i] : NULL;
        if (e && (!e->used || e->owner != me || e->life == EN_UNSYNCED)) continue;
        if (e) cm.r[cm.n].id = e->id, cm.r[cm.n].epoch = e->epoch, cm.r[cm.n].life = e->life, cm.n++, count++;
        if (cm.n == 40 || (!e && (cm.n || cm.first))) {
            en_send(id, ENM_CLAIM, &cm, (int)(offsetof(MClaim, r) + cm.n * sizeof cm.r[0]));
            cm.n = 0, cm.first = 0;
        }
    }
    for (int i = 0; i < EN_N; i++) if (en_tab[i].used && en_tab[i].owner == me && en_tab[i].life != EN_UNSYNCED) decl_send(id, &en_tab[i]);
    for (int i = 0; i < EN_N; i++)
        if (en_tab[i].used && en_tab[i].life == EN_UNSYNCED) { MUnsynced m = {en_tab[i].id.a, 3, {0}}; en_send(id, ENM_UNSYNCED, &m, sizeof m); }
    MTombs tm;
    memset(&tm, 0, sizeof tm);
    for (int i = 0; i <= n_tomb; i++) {
        if (i < n_tomb) tm.r[tm.n].hash = tomb[i].hash, tm.r[tm.n].life_no = tomb[i].life_no, tm.r[tm.n].flag = tomb[i].flag, tm.n++;
        if (tm.n == 64 || (i == n_tomb && tm.n)) en_send(id, ENM_TOMBS, &tm, (int)(offsetof(MTombs, r) + tm.n * sizeof tm.r[0])), tm.n = 0;
    }
    MRows rm;
    memset(&rm, 0, sizeof rm);
    for (int i = 0; i <= n_rest; i++) {
        if (i < n_rest) rm.r[rm.n].id = rest[i].id, rm.r[rm.n].epoch = rest[i].epoch, rm.r[rm.n].hp = rest[i].hp, memcpy(rm.r[rm.n].pose, rest[i].pose, 28), rm.n++;
        if (rm.n == 16 || (i == n_rest && rm.n)) en_send(id, ENM_REST, &rm, (int)(offsetof(MRows, r) + rm.n * sizeof rm.r[0])), rm.n = 0;
    }
    en_copy_burst(id);
    MSyncEnd se = {life_no, count};
    en_send(id, ENM_SYNC_END, &se, sizeof se);
    rt_log("coop: en: everything stated to player %d: %u enemies of this game, %d tombstones, %d resting", id, count, n_tomb, n_rest);
}

// ---- Ownership (S 4) --------------------------------------------------------------------------------------------------
// The one place an owner or an epoch changes. 1: the pair was taken.
static int en_apply_owner(Enid id, uint64_t owner, uint32_t epoch, const char *why) {
    uint64_t me = coop_ctr_me();
    En *e = en_find(id), *eh = e ? NULL : en_by_hash(id.a);
    if (eh) {   // another life of that placement: the greater life number is the enemy
        if (id.b < eh->id.b) { n_dropped++; return 0; }
        en_free(eh);
    }
    EnRest *r = rest_find(id);
    uint32_t ce = e ? e->epoch : 0;
    uint64_t co = e ? e->owner : 0;
    if (r && r->epoch > ce) ce = r->epoch, co = 0;
    if (!(epoch > ce || (epoch == ce && owner > co))) { if (owner != co || epoch != ce) n_dropped++; return 0; }
    if (!e && !(e = en_alloc(id))) return 0;
    if (r) { if (!e->have_decl) e->hp = r->hp, memcpy(e->pose, r->pose, 28); rest_del(id); }
    uint64_t was = e->owner;
    e->owner = owner, e->epoch = epoch, e->offered = 0, e->offer_to = 0, e->offer_seen_ns = 0, e->notrun_ns = 0, e->owner_ns = en_now();
    e->snap_w = 0, e->tele_seen = 0;   // (the ring of states is the old owner's)
    if (e->life == EN_UNSYNCED) e->life = EN_LIVE;
    if (was && was != owner) e->changes++, n_changes++;
    // (nothing of the earlier owner is shown, and the life last written to the actor as a copy is forgotten: kept, a
    // game that became a copy again took the difference to it for its own player's hit, sent it, and the enemy lost
    // that life at every change of owner)
    if (e->cell >= 0) en_map[e->cell].show_on = 0, en_map[e->cell].hp_written = 0, en_map[e->cell].show_rag = 0, atomic_store(&en_map[e->cell].state, owner == me ? EM_OWNED : EM_COPY);
    if (owner != me) e->need_ns = en_now();
    en_log_own(e, owner == me ? (was ? "taken over by this game" : "claimed by this game") : was == me ? "another game's now: this game's is a copy" : "another game's", why);
    return 1;
}

static int claims_allowed(uint64_t now) {
    if (now - load_end_ns < 2 * SEC) return 0;
    // Two games that came together would claim the same enemies in the same frame, and the smaller incarnation's
    // would be taken back a moment later: it waits half a second longer, so the other's claims are here first.
    uint64_t me = coop_ctr_me(), extra = 0;
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) if (peer[id].ready && peer[id].inc > me) extra = SEC / 2;
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (!peer[id].ready) continue;
        uint64_t since = peer[id].ready_ns > load_end_ns ? peer[id].ready_ns : load_end_ns;
        if (now - since < 2 * SEC + extra) return 0;
        if (!peer[id].sync_end && now - since < 5 * SEC) return 0;   // (a peer that does not answer in 5 s is not waited for)
    }
    return 1;
}

static void bind(En *e, EnCell *m, uint64_t actor, int state) {
    if (e->cell >= 0 && &en_map[e->cell] != m) cell_unbind(e);
    e->actor = actor, e->gen = atomic_load(&m->gen), e->cell = (int16_t)(m - en_map);
    atomic_store(&m->en, (int16_t)(e - en_tab));
    m->show_on = 0, m->hp_written = 0, m->show_rag = 0;   // (nothing of an earlier owner is shown)
    atomic_store(&m->state, (uint8_t)state);
    m->retry = 0;
}
static int runs(const En *e, uint32_t frame) {   // S 4.1, without its waiting time
    if (e->cell < 0 || !en_alive(e->actor, e->gen)) return 0;
    // (its calc ran, its far gate is open, and a graph runner of it was entered in the last second and a half: the
    // gate as read here was open for an enemy 70 m off that no runner came for, and the game that "ran" it took it)
    return frame - atomic_load(&en_map[e->cell].frame) < 30 && en_gate_open(e->actor) && frame - atomic_load(&en_map[e->cell].run_frame) < 90;
}

// An actor this game has and nobody has stated: what it is (S 4.2).
static void cell_resolve(EnCell *m, uint64_t now, uint32_t frame) {
    uint64_t actor = atomic_load(&m->actor), me = coop_ctr_me();
    int st = atomic_load(&m->state);
    if (!m->hash) {   // (looked at again while it has none: a run-time enemy's key comes when the spawn sync has announced it)
        uint32_t coop_world_spawn_key(uint64_t a);   // coop_world.c
        m->hash = en_hash_of(actor), m->rt = 0;
        if (!m->hash) m->hash = coop_world_spawn_key(actor), m->rt = m->hash != 0;
    }
    if (!m->hash) {   // made at run time and not announced (yet): the game's own; a new one is held half a second for its key
        if (st != EM_NEW || now - m->new_ns > SEC / 2) atomic_store(&m->state, EM_OWN);
        return;
    }
    uint32_t idb = m->rt ? EN_RT : life_no;
    En *e = en_by_hash(m->hash);
    if (e) {
        char nm[64] = "";
        coop_actor_name(actor, nm, sizeof nm);
        if (e->life == EN_UNSYNCED || e->id.b != idb) { atomic_store(&m->state, EM_OWN); return; }
        if (e->cell >= 0 && &en_map[e->cell] != m && en_alive(e->actor, e->gen)) { atomic_store(&m->state, EM_OWN); return; }   // (a second actor of one placement)
        if (e->have_decl && strcmp(e->name, nm)) { unsynced_set(m->hash, 1, 1); atomic_store(&m->state, EM_OWN); return; }
        if (!e->name[0]) snprintf(e->name, sizeof e->name, "%s", nm);
        bind(e, m, actor, e->owner == me ? EM_OWNED : EM_COPY);
        en_log_own(e, e->owner == me ? "this game's actor bound to its own entry" : "this game's actor is its copy", "the actor appeared");
        return;
    }
    if (!m->rt && en_tombed(m->hash)) {   // it died this session in another game: the placement's actor here is put away (S 9.4)
        atomic_store(&m->state, EM_OWN), atomic_store(&m->put_away, 1);
        return;
    }
    int allowed = claims_allowed(now);
    uint32_t rf = atomic_load(&m->run_frame);
    if (st == EM_NEW && !rf && now - m->new_ns < SEC / 2) return;   // (a new actor: its runners have not come yet)
    if (frame - atomic_load(&m->frame) >= 30 || !en_gate_open(actor) || !rf || frame - rf >= 90) {   // not run here: the game's own, looked at again
        atomic_store(&m->state, EM_OWN);
        return;
    }
    if (!allowed) {   // a join or a load is being answered: a new actor stays held, at most 5 s; one that thinks goes on
        if (st == EM_NEW && now - m->new_ns > 5 * SEC) atomic_store(&m->state, EM_OWN);
        return;
    }
    Enid id = {m->hash, idb, 0};
    EnRest *r = rest_find(id);
    uint32_t epoch = 1 + (r ? r->epoch : 0);
    if (!en_apply_owner(id, me, epoch, "nobody had it")) { atomic_store(&m->state, EM_OWN); return; }
    e = en_find(id);
    coop_actor_name(actor, e->name, sizeof e->name);
    e->kind = m->kind, e->have_decl = 1;
    e->hp = en_hp_of(actor), en_pose_of(actor, e->pose);
    double hold = st == EM_NEW ? coop_sess_hold_s() : 0;   // (an actor that already thinks is not stopped for the hold)
    e->hold_until_ns = now + (uint64_t)(hold * 1e9);
    bind(e, m, actor, hold > 0 ? EM_HELD : EM_OWNED);
    claim_send(0, e), decl_send(0, e);
}

static void take(En *e, const char *why, uint64_t now) {
    if (!en_apply_owner(e->id, coop_ctr_me(), e->epoch + 1, why)) return;
    e->have_decl = 1, e->hold_until_ns = 0;
    e->hp = en_hp_of(e->actor), en_pose_of(e->actor, e->pose);
    claim_send(0, e), decl_send(0, e);
    (void)now;
}
static void gone(En *e, const char *why) {
    row_send(0, ENM_GONE, e, 0);
    en_log_own(e, "let go: it rests", why);
    rest_add(e->id, e->epoch, e->hp, e->pose);
    en_free(e);
}

// The hand-over (TOTK_COOP_EN_HANDOVER=0 turns it off). An enemy fights the player of the game that runs it: no other
// player is noticed there. So an enemy this game runs is offered to the game of another player whose Link is within
// HAND_M of it and HAND_GAIN_M nearer to it than this game's: not in its first HAND_HOLD s with this game, not in the
// middle of an interrupt action (S 4.6), and not again for 10 s when that game did not take it. A player who strikes
// it gets it sooner: for 5 s after another player's hit (its flinch is an interrupt action and has to end first) the enemy is offered to that player's game when its Link is
// nearer than this game's at all, or this game's is over HAND_FAR_M away, after 3 s with this game; so an enemy turns
// on the player who attacks it while its owner's player stands off.
#define HAND_FAR_M 15.f
#define HAND_M 35.f
#define HAND_GAIN_M 6.f
#define HAND_HOLD 6
static float hand_me[3], hand_them[COOP_MAX_PLAYERS][3];
static int hand_ids[COOP_MAX_PLAYERS], hand_n, hand_have_me;
static void handover(En *e, uint64_t now) {
    static int on = -1;
    if (on < 0) { const char *v = cfg_get("TOTK_COOP_EN_HANDOVER"); on = !(v && *v == '0'); }
    int struck = e->hit_by_ns && now - e->hit_by_ns < 5 * SEC ? e->hit_by : 0;
    if (!on || !hand_have_me || !hand_n || now - e->owner_ns < (struck ? 3 : HAND_HOLD) * SEC || (!struck && now < e->no_offer_until_ns)) return;
    float dm = 0, best = 1e30f;
    int to = 0;
    for (int k = 0; k < 3; k++) dm += (e->pose[k] - hand_me[k]) * (e->pose[k] - hand_me[k]);
    dm = sqrtf(dm);
    for (int i = 0; i < hand_n; i++) {
        float d = 0;
        for (int k = 0; k < 3; k++) d += (e->pose[k] - hand_them[i][k]) * (e->pose[k] - hand_them[i][k]);
        d = sqrtf(d);
        if (struck ? hand_ids[i] == struck : d < best) best = d, to = hand_ids[i];
    }
    if (!to || !peer[to].ready || best >= (struck ? 80.f : HAND_M) || coop_sess_peer(to) != COOP_SESS_RUNS) return;   // (a blow from afar counts: an arrow)
    if (struck ? !(best < dm || dm > HAND_FAR_M) : best + HAND_GAIN_M >= dm) return;
    uint8_t status = 0;
    uint64_t bsa = en_bsa(e->actor);
    if (bsa && coop_rd(bsa + 0x26a8, &status, 1) && (status == 1 || status == 2)) return;
    // Nor while it is not drawn: a Wizzrobe handed over in its unseen flight stayed in it for good in the game that
    // took it (its graphs there started from nothing in the middle of the warp; a play session, 2026-11-16).
    if (e->cur.hide) return;
    e->offered = 1, e->offer_all = 0, e->offer_to = peer[to].inc, e->offer_ns = now, e->hit_by_ns = 0;
    row_send(to, ENM_OFFER, e, 0);
    rt_log("coop: en: own: %s %016llx/%u epoch %u offered to player %d (its Link is %.1f m from it, this game's %.1f m)", e->name, (unsigned long long)e->id.a, e->id.b, e->epoch, to, best, dm);
}

// Every entry against this game's actors and the peers' states (S 4.4 to 4.6).
static void own_pass(uint64_t now, uint32_t frame) {
    uint64_t me = coop_ctr_me(), link = coop_player_actor();
    hand_have_me = link && coop_rd(link + COOP_ACTOR_POS, hand_me, 12);
    hand_n = coop_player_positions_ids(hand_them, hand_ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->life == EN_UNSYNCED) continue;
        if (e->cell >= 0 && (!en_alive(e->actor, e->gen) || atomic_load(&en_map[e->cell].en) != i)) cell_unbind(e);
        int r = runs(e, frame);
        if (e->owner == me) {
            if (e->cell >= 0 && atomic_load(&en_map[e->cell].state) == EM_HELD && now >= e->hold_until_ns) atomic_store(&en_map[e->cell].state, EM_OWNED);
            if (e->cell >= 0) e->hp = en_hp_of(e->actor), en_pose_of(e->actor, e->pose);
            if (e->life == EN_DYING && e->cell < 0) { en_dead(e, "its actor is gone"); continue; }
            if (e->life != EN_LIVE) continue;
            if (r) {
                e->notrun_ns = 0;
                if (e->offered && !e->offer_all && !e->offer_to) e->offered = 0, en_log_own(e, "offer withdrawn", "this game runs it again");
                if (e->offered && e->offer_to && now - e->offer_ns >= 2 * SEC)   // the nearer player's game did not take it: not asked again for 10 s
                    e->offered = 0, e->offer_to = 0, e->no_offer_until_ns = now + 10 * SEC, en_log_own(e, "offer withdrawn", "the nearer player's game did not take it");
                if (!e->offered) handover(e, now);
                continue;
            }
            if (e->offered && e->offer_to) e->offered = 0, e->offer_to = 0;   // (no longer run here: the open offer below takes over)
            if (!e->notrun_ns) e->notrun_ns = now;
            if (!e->offered && (e->cell < 0 || now - e->notrun_ns >= SEC / 2) && now >= e->no_offer_until_ns) {
                e->offered = 1, e->offer_all = 0, e->offer_ns = now;
                row_send(0, ENM_OFFER, e, 0);
                en_log_own(e, "offered", e->cell < 0 ? "this game's actor is gone" : "this game no longer runs it");
            }
            continue;   // (the offer's end is the net tick's: it also comes in a load)
        }
        if (e->life != EN_LIVE || !r) continue;
        int id = en_peer_id(e->owner), ps = id ? coop_sess_peer(id) : COOP_SESS_GONE;
        if (e->offer_seen_ns && now - e->offer_seen_ns < 3 * SEC) take(e, "its owner offered it and this game runs it", now), e->no_offer_until_ns = now + 10 * SEC;
        else if (!id || ps == COOP_SESS_GONE) take(e, "its owner left", now);
        else if (ps == COOP_SESS_LOADING) take(e, "its owner is in a load", now);
        else if (ps == COOP_SESS_PAUSED && peer[id].paused_ns) {
            uint64_t p = now - peer[id].paused_ns;
            if (p >= 5 * SEC) take(e, "its owner has been paused 5 s", now);
            else if (p >= SEC && e->my_hit_ns && now - e->my_hit_ns < 5 * SEC) take(e, "its owner is paused and this player fights it", now);
        }
    }
}

// ---- Messages taken (the net tick) --------------------------------------------------------------------------------------
static void peer_left(uint64_t inc) {
    uint64_t now = en_now();
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->owner != inc || e->life == EN_UNSYNCED) continue;
        if (e->cell >= 0) continue;   // (this game has its actor: phase 0 takes it over if it runs it)
        en_log_own(e, "rests", "its owner left and this game has no actor for it");
        rest_add(e->id, e->epoch, e->hp, e->pose);
        en_free(e);
    }
    (void)now;
}

static void take_msg(const CoopCtrMsg *m) {
    int from = en_peer_id(m->from);
    uint64_t now = en_now(), me = coop_ctr_me();
    if (!from) { n_dropped++; return; }
    switch (m->type) {
    case ENM_CLAIM: {
        const MClaim *c = (const MClaim *)m->d;
        if (m->len < (int)offsetof(MClaim, r) || c->n > 40 || m->len != (int)(offsetof(MClaim, r) + c->n * sizeof c->r[0])) break;
        if (c->first)   // the first of a full statement: what its sender does not name again it no longer has
            for (int i = 0; i < EN_N; i++) if (en_tab[i].used && en_tab[i].owner == m->from) en_tab[i].stale = 1;
        for (int k = 0; k < c->n; k++) {
            if (c->r[k].id.b < life_no) { n_dropped++; continue; }
            en_apply_owner(c->r[k].id, m->from, c->r[k].epoch, "its claim");
            En *e = en_find(c->r[k].id);
            if (e && e->owner == m->from && e->epoch == c->r[k].epoch) e->stale = 0;
        }
        break;
    }
    case ENM_DECL: {
        const MDecl *d = (const MDecl *)m->d;
        if (m->len != (int)sizeof *d) break;
        En *e = en_find(d->id);
        if (!e || e->owner != m->from || e->epoch != d->epoch) { n_dropped++; break; }
        memcpy(e->name, d->name, sizeof e->name), e->name[sizeof e->name - 1] = 0;
        e->hp = d->hp, memcpy(e->pose, d->pose, 28), e->kind = d->kind, e->have_decl = 1, e->need_ns = 0;
        if (d->life > e->life && d->life <= EN_DEAD) e->life = d->life;   // (life only moves forward)
        if (e->cell >= 0) {
            char nm[64] = "";
            coop_actor_name(e->actor, nm, sizeof nm);
            if (strcmp(nm, e->name)) unsynced_set(e->id.a, 1, 1);
        }
        break;
    }
    case ENM_OFFER: case ENM_GONE: case ENM_REST: {
        const MRows *r = (const MRows *)m->d;
        if (m->len < (int)offsetof(MRows, r) || r->n > 16 || m->len != (int)(offsetof(MRows, r) + r->n * sizeof r->r[0])) break;
        for (int k = 0; k < r->n; k++) {
            const MRestRow *w = &r->r[k];
            En *e = en_find(w->id);
            if (m->type == ENM_REST) { if (!e && w->id.b >= life_no) rest_add(w->id, w->epoch, w->hp, w->pose); continue; }
            if (!e || e->owner != m->from || e->epoch != w->epoch) { n_dropped++; continue; }
            e->hp = w->hp, memcpy(e->pose, w->pose, 28);
            if (m->type == ENM_OFFER) { if (!w->to || w->to == me) e->offer_seen_ns = now; }
            else {
                en_log_own(e, "rests", "its owner let it go");
                rest_add(e->id, e->epoch, e->hp, e->pose);
                en_free(e);
            }
        }
        break;
    }
    case ENM_TOMBS: {
        const MTombs *t = (const MTombs *)m->d;
        if (m->len < (int)offsetof(MTombs, r) || t->n > 64 || m->len != (int)(offsetof(MTombs, r) + t->n * sizeof t->r[0])) break;
        for (int k = 0; k < t->n; k++) en_tomb_add(t->r[k].hash, t->r[k].life_no, t->r[k].flag);
        break;
    }
    case ENM_NEED: {
        const MNeed *n = (const MNeed *)m->d;
        if (m->len != (int)sizeof *n) break;
        if (!n->id.a) { peer[from].burst_owed = 1; break; }
        En *e = en_find(n->id);
        EnRest *r = rest_find(n->id);
        if (e && e->owner == me) claim_send(from, e), decl_send(from, e);
        else if (r) {
            MRows rm;
            memset(&rm, 0, sizeof rm);
            rm.n = 1, rm.r[0].id = r->id, rm.r[0].epoch = r->epoch, rm.r[0].hp = r->hp, memcpy(rm.r[0].pose, r->pose, 28);
            en_send(from, ENM_REST, &rm, (int)(offsetof(MRows, r) + sizeof rm.r[0]));
        }
        break;
    }
    case ENM_MOON: {   // another game's blood moon began
        const MSyncEnd *s = (const MSyncEnd *)m->d;
        if (m->len == (int)sizeof *s) moon_apply(s->life_no, "another game's blood moon");
        break;
    }
    case ENM_SYNC_END: {
        const MSyncEnd *s = (const MSyncEnd *)m->d;
        if (m->len != (int)sizeof *s) break;
        if (s->life_no > life_no) life_no = s->life_no;
        int stale = 0;
        for (int i = 0; i < EN_N; i++)
            if (en_tab[i].used && en_tab[i].owner == m->from && en_tab[i].stale) {
                en_log_own(&en_tab[i], "rests", "its owner's full statement did not name it");
                rest_add(en_tab[i].id, en_tab[i].epoch, en_tab[i].hp, en_tab[i].pose);
                en_free(&en_tab[i]), stale++;
            }
        peer[from].sync_end = 1, peer[from].index_bad = 0;
        rt_log("coop: en: player %d stated everything: %u enemies it runs (%d of this game's entries for it were stale)", from, s->count, stale);
        break;
    }
    case ENM_UNSYNCED: {
        const MUnsynced *u = (const MUnsynced *)m->d;
        if (m->len == (int)sizeof *u) unsynced_set(u->hash, 3, 0);
        break;
    }
    default: {
        void en_hit_msg(const CoopCtrMsg *m, int from);   // coop_enemy_hit.c: events, hits, deaths
        if (!en_copy_msg(m, from) && !en_shot_msg(m, from)) en_hit_msg(m, from);
    }
    }
}

static uint32_t fnv(uint32_t h, const void *p, size_t n) {
    for (const uint8_t *b = p; n--; b++) h = (h ^ *b) * 16777619u;
    return h;
}
static uint32_t index_of(uint64_t owner, uint32_t *count) {
    uint32_t x = 0, n = 0;
    for (int i = 0; i < EN_N; i++) {
        const En *e = &en_tab[i];
        if (!e->used || e->owner != owner || e->life != EN_LIVE) continue;
        uint32_t h = fnv(2166136261u, &e->id.a, 8);
        h = fnv(h, &e->id.b, 4), h = fnv(h, &e->epoch, 4);
        x ^= h, n++;
    }
    *count = n;
    return x;
}

static void deactivate(int keep_table) {
    for (int i = 0; i < EN_MAP_N; i++)
        if (atomic_load(&en_map[i].actor) > 1) atomic_store(&en_map[i].en, -1), atomic_store(&en_map[i].state, EM_OWN), en_map[i].retry = 1;
    for (int i = 0; i < EN_N; i++) {
        if (!en_tab[i].used) continue;
        if (keep_table) en_tab[i].actor = 0, en_tab[i].gen = 0, en_tab[i].cell = -1;
        else memset(&en_tab[i], 0, sizeof en_tab[i]), en_tab[i].cell = -1;
    }
    if (!keep_table) n_rest = n_tomb = 0, life_no = 1;
}

// The watch on the net tick: where it is (0: not in coop_enemy_net) and when it was last entered and left. An actor's
// calc says so once when the tick has not come round for 3 s while the game runs.
static _Atomic int tick_stage;
static _Atomic uint64_t tick_in_ns, tick_out_ns;
#define STAGE(n) atomic_store_explicit(&tick_stage, n, memory_order_relaxed)

void coop_enemy_net(void) {
    if (!coop_enemy_new()) return;
    atomic_store(&tick_in_ns, en_now()), STAGE(1);
    static int opened;
    if (!opened) opened = 1, coop_msg_open(COOP_MSG_ENEMY), rt_log("coop: en: the enemy sync is on (TOTK_COOP_ENEMY=1) for the kinds it lists; coop_world.c's older one keeps the others");
    coop_lock_wait(&en_lock);
    STAGE(2);
    uint64_t now = en_now(), me = coop_ctr_me();
    int my_id = coop_net_id(), any = 0, bad = 0;
    {   // (join and leave events: read from the peers' state below; the queue is only emptied)
        uint64_t inc;
        int id, joined;
        while (coop_msg_peer_event(COOP_MSG_ENEMY, &inc, &id, &joined)) { }
    }
    STAGE(3);
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        uint64_t inc = id != my_id && me && coop_net_live(id) ? coop_ctr_incarnation(id) : 0;
        if (peer[id].inc && peer[id].inc != inc) {   // it left (or came back as another incarnation)
            uint64_t was = peer[id].inc;
            OutMsg *out = peer[id].out;
            if (peer[id].ready) rt_log("coop: en: player %d left", id);
            memset(&peer[id], 0, sizeof peer[id]);
            peer[id].out = out;
            peer_left(was);
        }
        if (!inc) continue;
        peer[id].inc = inc;
        CoopCtrHb hb;
        uint64_t age = 0;
        if (!coop_ctr_hb(id, &hb, &age)) continue;   // (no heartbeat yet: what it runs is not known)
        if (!(hb.flags & COOP_CTR_HB_ENEMY)) {
            if (!peer[id].cap_bad) {
                void coop_notice_enemy_sync(int id);   // coop_notices.c: said on screen too
                rt_log("coop: en: player %d does not run this enemy sync (TOTK_COOP_ENEMY=1 is not set there): no enemy is synced while it is here", id);
                coop_notice_enemy_sync(id);
            }
            peer[id].cap_bad = 1, peer[id].ready = 0, bad = 1;
            continue;
        }
        if (!peer[id].ready) peer[id].ready = 1, peer[id].ready_ns = now, peer[id].burst_owed = 1, peer[id].sync_end = 0, rt_log("coop: en: player %d runs this enemy sync", id);
        int ps = coop_sess_peer(id);
        if (ps != peer[id].state) {
            static const char *const word[] = { "runs", "is paused", "is in a load", "is silent", "is gone" };
            rt_log("coop: en: player %d %s", id, word[ps]);
            peer[id].state = (uint8_t)ps;
        }
        if (ps == COOP_SESS_PAUSED) { if (!peer[id].paused_ns) peer[id].paused_ns = now; } else peer[id].paused_ns = 0;
        if (ps == COOP_SESS_LOADING) peer[id].was_loading = 1;
        else if (ps == COOP_SESS_RUNS && peer[id].was_loading) peer[id].was_loading = 0, peer[id].burst_owed = 1;   // it ran again after a load
        any = 1;
    }
    STAGE(4);
    int session = any && !bad, loading = coop_sess_loading();
    static int was_session, was_loading;
    if (!session) {
        if (was_session) deactivate(0), rt_log("coop: en: no session: every enemy is this game's own, the table is empty");
        was_session = 0, session_ns = 0;
        atomic_store(&en_active, 0);
        CoopCtrMsg m;
        while (coop_msg_recv(COOP_MSG_ENEMY, &m)) { }
        plat_mutex_unlock(&en_lock);
        STAGE(0), atomic_store(&tick_out_ns, en_now());
        return;
    }
    was_session = 1;
    if (!session_ns) session_ns = now;
    if (loading && !was_loading) deactivate(1), rt_log("coop: en: this game is in a load: its copies are let go and its enemies offered");
    if (!loading && was_loading) {
        load_end_ns = now;
        for (int id = 1; id <= COOP_MAX_PLAYERS; id++) peer[id].sync_end = 0;
        rt_log("coop: en: this game's load ended: it claims nothing until every peer has stated what it has");
    }
    was_loading = loading;
    atomic_store(&en_active, !loading);

    static CoopCtrMsg m;
    STAGE(5);
    while (coop_msg_recv(COOP_MSG_ENEMY, &m)) take_msg(&m);
    STAGE(6);

    for (int id = 1; id <= COOP_MAX_PLAYERS; id++)
        if (peer[id].ready && peer[id].burst_owed && !loading) peer[id].burst_owed = 0, burst(id);

    // Offers end here: a load offers everything at once, and an offer nobody took in 2 s lets the enemy rest (S 4.5).
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->owner != me || e->life != EN_LIVE) continue;
        if (loading && (!e->offered || e->offer_to)) e->offer_to = 0, e->offered = 1, e->offer_all = 1, e->offer_ns = now, row_send(0, ENM_OFFER, e, 1), en_log_own(e, "offered", "this game is in a load");
        if (e->offered && !e->offer_to && now - e->offer_ns >= 2 * SEC) gone(e, "nobody claimed it in 2 s");
    }
    // An entry of another game with no declaration: asked for (S 11.4).
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->owner == me || e->have_decl || !e->need_ns || now - e->need_ns < SEC / 2) continue;
        MNeed n = {e->id};
        en_send(en_peer_id(e->owner), ENM_NEED, &n, sizeof n);
        e->need_ns = now + 3 * SEC - SEC / 2;
    }
    {   // INDEX (S 11.3): what this game runs, every 2 s; a peer whose table differs twice in a row asks for everything
        static uint64_t sent_ns;
        if (now - sent_ns >= 2 * SEC && !loading) {
            sent_ns = now;
            MIndex ix = {0, 0, 0};
            ix.hash = index_of(me, &ix.count);
            for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
                if (!peer[id].ready) continue;
                if (peer[id].out_r == peer[id].out_w) coop_ctr_send(id, ENM_INDEX, 0, &ix, sizeof ix);   // (not while something is owed to it)
                MIndex theirs;
                uint64_t age = 0;
                if (!peer[id].sync_end || coop_ctr_latest(id, ENM_INDEX, 0, &theirs, sizeof theirs, &age) != (int)sizeof theirs || age > 3 * SEC) continue;
                uint32_t n = 0, h = index_of(peer[id].inc, &n);
                if (n == theirs.count && h == theirs.hash) { peer[id].index_bad = 0; continue; }
                if (++peer[id].index_bad >= 2 && now - peer[id].need_all_ns > 10 * SEC) {
                    MNeed nd = {{0, 0, 0}};
                    peer[id].need_all_ns = now, peer[id].index_bad = 0;
                    en_send(id, ENM_NEED, &nd, sizeof nd);
                    rt_log("coop: en: this game's table of player %d's enemies differs from its own (%u here, %u there): asked for everything", id, n, theirs.count);
                }
            }
        }
    }
    STAGE(7);
    out_flush();
    STAGE(8);
    {   // The record, once a second: the table as counts and one number two games with equal tables agree on.
        static uint64_t said_ns;
        if (now - said_ns >= SEC) {
            said_ns = now;
            int live = 0, mine = 0, bound = 0, uns = 0;
            uint32_t h = 0;
            for (int i = 0; i < EN_N; i++) {
                const En *e = &en_tab[i];
                if (!e->used) continue;
                if (e->life == EN_UNSYNCED) { uns++; continue; }
                live++, mine += e->owner == me, bound += e->cell >= 0;
                uint32_t x = fnv(2166136261u, &e->id.a, 8);
                x = fnv(x, &e->id.b, 4), x = fnv(x, &e->owner, 8), x = fnv(x, &e->epoch, 4), x = fnv(x, &e->life, 1);
                h ^= x;
            }
            rt_log("coop: en: table: %d entries (%d this game's, %d with an actor here), %d unsynced, %d resting, %d tombstones; table number %08x; "
                   "%u changes of owner, %u messages dropped, %u not sent (queue full), %u table full",
                   live, mine, bound, uns, n_rest, n_tomb, h, n_changes, n_dropped, n_out_full, n_full);
        }
    }
    plat_mutex_unlock(&en_lock);
    STAGE(0), atomic_store(&tick_out_ns, en_now());
}

// Whether p is within m of a live enemy this game runs (coop_world.c coop_near_my_enemy, for the object mirror's
// arrows: one made there is that enemy's shot). From the co-op tick.
int coop_enemy_near_owned(const float p[3], float m) {
    if (!atomic_load(&en_active)) return 0;
    int near = 0;
    uint64_t me = coop_ctr_me();
    coop_lock_wait(&en_lock);
    for (int i = 0; i < EN_N && !near; i++) {
        const En *e = &en_tab[i];
        if (!e->used || e->owner != me || e->life != EN_LIVE || e->cell < 0) continue;
        float dx = p[0] - e->pose[0], dy = p[1] - e->pose[1], dz = p[2] - e->pose[2];
        near = dx * dx + dy * dy + dz * dz < m * m;
    }
    plat_mutex_unlock(&en_lock);
    return near;
}

// ---- Phase 0 ----------------------------------------------------------------------------------------------------------
void coop_enemy_phase0(Cpu *c) {
    if (!coop_enemy_new()) return;
    coop_lock_wait(&en_lock);
    if (!atomic_load(&en_active)) { plat_mutex_unlock(&en_lock); return; }
    uint64_t now = en_now();
    uint32_t frame = coop_sess_frame_no();
    {   // The record: this game's Link's life at each change (what an owner's and a copy's attacks take from it).
        static int32_t was = -1;
        static int said;
        uint64_t link = coop_player_actor();
        int coop_life_of(uint64_t a);   // coop_players.c
        int32_t hp = link ? coop_life_of(link) : -1;
        if (hp != was && hp >= 0 && was >= 0 && said++ < 300) rt_log("coop: en: this Link's life %d -> %d", was, hp);
        if (hp >= 0) was = hp;
    }
    for (int i = 0; i < EN_MAP_N; i++) {
        EnCell *m = &en_map[i];
        uint64_t a = atomic_load(&m->actor);
        if (a <= 1) continue;
        if (!en_alive(a, atomic_load(&m->gen))) {
            int32_t st = 0;
            // (an actor not ready yet, state 0 or 1, keeps its cell; one going away or gone gives it up)
            if (coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st < 2 && frame - atomic_load(&m->frame) < 600) continue;
            int16_t ei = atomic_load(&m->en);
            if (ei >= 0 && en_tab[ei].used && en_tab[ei].cell == i) en_tab[ei].actor = 0, en_tab[ei].gen = 0, en_tab[ei].cell = -1;
            atomic_store(&m->en, -1), atomic_store(&m->state, EM_FREE), atomic_store(&m->actor, 1);
            continue;
        }
        int st = atomic_load(&m->state);
        if (st == EM_NEW || st == EM_OWN) cell_resolve(m, now, frame);
    }
    own_pass(now, frame);
    en_hit_phase0();
    en_copy_phase0(c);
    en_shot_phase0();
    { void en_pose_phase0(void); en_pose_phase0(); }   // coop_enemy_pose.c
    { void en_loot_phase0(Cpu *c); en_loot_phase0(c); }   // coop_enemy_loot.c
    { void en_fx_phase0(Cpu *c); en_fx_phase0(c); }       // coop_enemy_fx.c
    out_flush();
    plat_mutex_unlock(&en_lock);
}

// ---- The actor's own thread --------------------------------------------------------------------------------------------
// The entry of an actor's calc (main+0x41bb20, x0 the actor), before the older handlers of that address.
int coop_enemy_calc(Cpu *c) {
    if (!coop_enemy_new()) return 0;
    uint64_t actor = c->x[0];
    {
        static _Atomic uint64_t said_ns;
        uint64_t now = en_now(), in = atomic_load_explicit(&tick_in_ns, memory_order_relaxed), out = atomic_load_explicit(&tick_out_ns, memory_order_relaxed), said = atomic_load_explicit(&said_ns, memory_order_relaxed);
        if (in && now > in + 3 * SEC && now > out + 3 * SEC && now > said + 10 * SEC && atomic_compare_exchange_strong(&said_ns, &said, now))
            rt_log("coop: en: the net tick has not come round for %.1f s: it %s (stage %d)", (double)(now - in) / 1e9, out >= in ? "is not called" : "stands inside the enemy sync", atomic_load(&tick_stage));
    }
    EnCell *m = actor ? map_touch(actor) : NULL;
    if (!m) return 0;
    atomic_store_explicit(&m->frame, coop_sess_frame_no(), memory_order_relaxed);
    en_copy_precalc(c, actor, m);
    return 0;
}

// The blood moon's start in this game (main+0x21cd650, from en_copy_hook): the life number goes up by one and every
// game is told, so each drops what the moon brings back. Not twice for one moon: a game told of the moon less than
// five minutes ago does not raise the number again when its own moon begins.
void en_moon_local(void) {
    coop_lock_wait(&en_lock);
    if (atomic_load(&en_active) && (!moon_ns || en_now() - moon_ns > 300 * SEC)) {
        MSyncEnd se = {life_no + 1, 0};
        moon_apply(se.life_no, "this game's blood moon");
        en_send_all(ENM_MOON, &se, sizeof se);
    }
    plat_mutex_unlock(&en_lock);
}
