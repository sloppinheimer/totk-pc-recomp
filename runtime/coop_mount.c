// Co-op horses and animals: kinds of the enemy sync, and what they need besides it (docs/coop/facts/horses-animals.md).
//
// A horse or a land animal is an entry of the enemy sync's table like an enemy (coop_enemy.c kind_of takes the names
// coop_mount_kind gives), so its identity, owner, copy switch, pose, animation commands, life and death are that
// sync's. This file adds, with TOTK_COOP_MOUNTS=1 (off by default until the spec's runs pass):
// - the gait values (spec 7): the floats of the animation sequence's blackboard that choose the clip inside a command
//   (a turn, a slope, the gear), which the AI writes each frame and a copy's AI does not;
// - a horse's tack (spec 4.5): what its equipment slots 0xf, 0x10 and 0x11 hold (mane, reins, saddle);
// - riding (spec 4.6, 5, 6): who sits on a horse, where, and what an enemy rides; a ridden horse is run by the game
//   whose actor sits on it, a remote rider's stand-in is put on this game's copy of the horse, a copy rider is seated
//   on it by the game's own ride call;
// - wild herds (spec 4.3): one game makes a tag's herd.
// These travel in MOUNT (type 66, unreliable, the latest per entry), sent with the entry's STATE, so an enemy's state
// does not grow.
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_net.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);   // coop_spawn.c

#define SEC 1000000000ull
#define KIND_HORSE 200
#define KIND_ANIMAL 201
#define GAIT_N 12
#define FN_RIDE 0x502e60    // (rider component, mount link, type, seat): seat a rider (research R20, H3)
#define FN_GET_OFF 0xc448c8 // (rider component, flag, flags*): take a rider off
#define FN_TACK 0xc48ac0    // (equipment user, slot, &name, dye, 0, 1): what a horse's tack node calls (H2 section 4)
#define FN_HERD 0xd7ff68    // ExecuteWildHorseCreate's update: makes a tag's whole herd in one call (H2 section 3.2)
#define TACK_SLOT0 0xf      // mane 0xf, reins 0x10, saddle 0x11

uint64_t coop_player_standin(int net_id);   // coop_players.c

int coop_mounts_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_MOUNTS");
        on = e && *e == '1';
        if (on) rt_log("coop: mount: on (horses and land animals are kinds of the enemy sync)");
    }
    return on;
}

// ---- Kinds (spec 2; research H1: the actors with a brain and a life) -----------------------------------------------------
// Horses: every GameRomHorse but the tack (37 actors: Epona, the Lord of the Mountain, the giant, skeleton, golden and
// patterned horses, the ridable donkey). Animals by full name: a variant of a name without a brain (a rupee rabbit made
// by a cave boss, a sand seal that does not move) is not taken. Left out: fish, insects, birds, swarms, the scenery
// horses and event donkeys (Animal_Horse_*, Animal_Donkey_*: no brain, no life).
static const char *const animals[] = {
    "Animal_Bear_A", "Animal_Bear_B", "Animal_Boar_A", "Animal_Boar_B", "Animal_Bull_A", "Animal_Cassowary_A", "Animal_SkyCassowary_A",
    "Animal_Cow_A", "Animal_Deer_A", "Animal_Doe_A", "Animal_Elk_A", "Animal_Dog_A", "Animal_Dog_B", "Animal_Dog_C", "Animal_Fox_A",
    "Animal_Fox_B", "Animal_Goat_A", "Animal_WildGoat_A", "Animal_Sheep_A", "Animal_Kokko_A", "Animal_Kokko_B", "Animal_Kokko_B2",
    "Animal_Kokko_C", "Animal_Kokko_C2", "Animal_Kokko_D", "Animal_Kokko_E", "Animal_Kokko_E2", "Animal_Kokko_E3", "Animal_Kokko_E4",
    "Animal_Pao_A", "Animal_RupeeRabbit_A", "Animal_Squirrel_A", "Animal_Sunazarashi_A", "Animal_Sunazarashi_B", "Animal_Sunazarashi_C",
    "Animal_SunazarashiSP_A", "Animal_SunazarashiSP_C", "Animal_Wolf_A", "Animal_Wolf_B", "Animal_Wolf_C",
};
static int is_horse(const char *nm) {
    return !strncmp(nm, "GameRomHorse", 12) && strncmp(nm + 12, "Saddle", 6) && strncmp(nm + 12, "Reins", 5);
}
int coop_mount_kind(const char *nm) {
    if (!nm || !coop_mounts_on()) return 0;
    if (is_horse(nm)) return KIND_HORSE;
    if (strncmp(nm, "Animal_", 7)) return 0;
    for (size_t i = 0; i < sizeof animals / sizeof *animals; i++)
        if (!strcmp(nm, animals[i])) return KIND_ANIMAL;
    return 0;
}
static int kind_is_mount(int k) { return k == KIND_HORSE || k == KIND_ANIMAL; }

// ---- The gait values (spec 7) -------------------------------------------------------------------------------------------
// The animation sequence's blackboard (ASBB) is [G+0x40], G the animation player (coop_enemy_copy.c anim_g). Its
// methods (research G5 section 4): vt[+0x290](B, &name) -> id; vt[+0x170](B, &name) -> the float; vt[+0x70](value in
// s0, B, id) sets the float by id. The names per kind are the ASB tables' (research H4 B6.2).
static const char *const gait_horse[] = {"RequestGear", "StickX", "StickAngle", "DirectionAngle", "UserSpeed", "GroundNormal",
                                         "GroundNormalSide", "ForwardBent", "PullAnmRate", "Variation"};
static const char *const gait_animal[] = {"RequestGear", "StickX", "DirectionAngle", "GroundNormal", "Distance"};
static const char *const *gait_names(int kind, int *n) {
    if (kind == KIND_HORSE) { *n = (int)(sizeof gait_horse / sizeof *gait_horse); return gait_horse; }
    if (kind == KIND_ANIMAL) { *n = (int)(sizeof gait_animal / sizeof *gait_animal); return gait_animal; }
    *n = 0;
    return NULL;
}
static uint64_t asbb_of(uint64_t actor) {
    uint64_t g = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(en_comps(actor) + 0x40) + 0x18) + 0x18);
    return g ? coop_rd_ptr(g + 0x40) : 0;
}
// A guest call with a float in s0 and the float result read from s0 (the vector registers are put back).
static uint64_t call_fv(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, float s0_in, float *s0_out) {
    __uint128_t v0 = c->v[0];
    uint32_t bits;
    memcpy(&bits, &s0_in, 4);
    c->v[0] = bits;
    uint64_t r = coop_guest_call(c, fn, a0, a1, 0, 0);
    if (s0_out) { uint32_t o = (uint32_t)c->v[0]; memcpy(s0_out, &o, 4); }
    c->v[0] = v0;
    return r;
}
// The id of a name in an actor's ASBB; -1: it has none (or no ASBB).
static int32_t asbb_id(Cpu *c, uint64_t b, const char *name) {
    static __thread const char *p;
    uint64_t fn = b ? coop_rd_ptr(coop_rd_ptr(b) + 0x290) : 0;
    if (!fn) return -1;
    p = name;
    int32_t id = (int32_t)(uint32_t)coop_guest_call(c, fn, b, (uint64_t)(uintptr_t)&p, 0, 0);
    return id;
}

// ---- Per entry and per cell ---------------------------------------------------------------------------------------------
typedef struct {
    uint64_t key; Enid id; uint32_t epoch;
    uint8_t rider, rseat, rtype, ngait;   // rider: who sits on this horse (0 no one, 1 + net id a player's Link, 0xff an enemy)
    float roff[3];                        // the rider's place in this actor's frame
    float gait[GAIT_N];                   // the gait values, by the kind's list
    uint32_t tack[3];                     // what slots 0xf, 0x10, 0x11 hold (name hash), a horse's
    Enid ride; uint8_t ride_seat, ride_type, pad[2];   // what this actor rides (ride.a 0: nothing)
} MMount;
_Static_assert(sizeof(MMount) <= 160, "MOUNT is one small unreliable message");

// What the game that runs an entry last stated (a copy's), and what this game read of its own (an owned one's).
static MMount mt[EN_N];
static uint8_t mt_have[EN_N], mt_sent_ride[EN_N];
static uint64_t mt_ns[EN_N];
// What a copy's pre-calc applies (phase 0 writes it under the sequence; pre-calc on the actor's thread).
typedef struct {
    _Atomic uint32_t seq;
    MMount m;
    uint64_t ride_actor;                  // this game's actor of what it rides (0 nothing or not here)
    uint8_t on;
    // the ids of the gait values in this game's actor's ASBB (an owner's: phase 0; a copy's: its pre-calc)
    int32_t gid[GAIT_N]; uint64_t gid_actor; uint32_t gid_gen;
    // pre-calc's own (the actor's thread)
    uint64_t seated_on;                   // the actor the copy rider was seated on by this file
    uint32_t ride_try_frame, tack_frame;
    uint8_t ride_tries, tack_tries[3];
} MCell;
static MCell mc[EN_MAP_N];
// The ids of a kind's gait names in an actor's ASBB, looked up once per actor (by its address and generation).
static void gait_ids(Cpu *c, MCell *x, uint64_t actor, uint64_t b, int kind) {
    uint32_t gen = 0;
    coop_rd(actor + 0x10, &gen, 4);
    if (x->gid_actor == actor && x->gid_gen == gen) return;
    int n = 0;
    const char *const *names = gait_names(kind, &n);
    for (int k = 0; k < GAIT_N; k++) x->gid[k] = k < n ? asbb_id(c, b, names[k]) : -1;
    x->gid_actor = actor, x->gid_gen = gen;
    rt_log("coop: mount: %llx's gait values have ids %d %d %d %d %d (-1: not in its table)", (unsigned long long)actor, x->gid[0], x->gid[1], x->gid[2], x->gid[3], x->gid[4]);
}

// ---- Names of tack (hash -> string, learned from this game's own reads; a copy's tack name must be known here) -------------
#define TN_N 64
static struct { uint32_t h; char s[40]; } tnames[TN_N];
static _Atomic int tn_n;
static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static const char *tname_of(uint32_t h) {
    int n = atomic_load(&tn_n);
    for (int i = 0; i < n && i < TN_N; i++) if (tnames[i].h == h) return tnames[i].s;
    return NULL;
}
static void tname_add(uint32_t h, const char *s) {
    if (tname_of(h)) return;
    int i = atomic_load(&tn_n);
    if (i >= TN_N) return;
    tnames[i].h = h, snprintf(tnames[i].s, sizeof tnames[i].s, "%s", s);
    atomic_store(&tn_n, i + 1);
}
// Every tack name the game has, so a copy can be given what its owner's horse wears though this game's horses wear
// other tack: the saddles, reins and manes of the census (research H1).
static void tnames_seed(void) {
    static int done;
    if (done) return;
    done = 1;
    static const char *const t[] = {
        "GameRomHorseReins_00", "GameRomHorseReins_00L", "GameRomHorseReins_01", "GameRomHorseReins_02", "GameRomHorseReins_03",
        "GameRomHorseReins_04", "GameRomHorseReins_05", "GameRomHorseReins_06", "GameRomHorseSaddle_00", "GameRomHorseSaddle_00L",
        "GameRomHorseSaddle_00S", "GameRomHorseSaddle_00S_AncientAssistant", "GameRomHorseSaddle_01", "GameRomHorseSaddle_01S",
        "GameRomHorseSaddle_02", "GameRomHorseSaddle_03", "GameRomHorseSaddle_04", "GameRomHorseSaddle_05", "GameRomHorseSaddle_06",
        "GameRomHorseSaddle_07", "GameRomHorseSaddle_07_ExternalCoupler", "GameRomHorseSaddle_07_WithWagon", "Horse_Link_Mane",
        "Horse_Link_Mane_00L", "Horse_Link_Mane_01", "Horse_Link_Mane_01L", "Horse_Link_Mane_02", "Horse_Link_Mane_03", "Horse_Link_Mane_04",
        "Horse_Link_Mane_05", "Horse_Link_Mane_06", "Horse_Link_Mane_07", "Horse_Link_Mane_08", "Horse_Link_Mane_09"};
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) tname_add(fnv(t[i]), t[i]);
}

// ---- Reads of the game --------------------------------------------------------------------------------------------------
static uint64_t rider_of(uint64_t actor) { uint64_t k = en_comps(actor); return k ? coop_rd_ptr(k + 0x410) : 0; }
// What a rider component sits on: its riding word +0x38 is 1 and its mount link (+0x20: +8 the reference object,
// whose +0x40 is the actor and +0x48 its id; +0x10 the id the link holds) names a live actor (research R20).
static uint64_t mount_of_rider(uint64_t rider, uint32_t *seat, uint32_t *type) {
    int32_t on = 0;
    uint32_t id = 0, rid = 0;
    if (!rider || !coop_rd(rider + 0x38, &on, 4) || on != 1) return 0;
    uint64_t ref = coop_rd_ptr(rider + 0x28), a = ref ? coop_rd_ptr(ref + 0x40) : 0;
    if (!a || !coop_rd(rider + 0x30, &id, 4) || id == ~0u || !coop_rd(ref + 0x48, &rid, 4) || rid != id) return 0;
    if (seat) coop_rd(rider + 0x3c, seat, 4);
    if (type) coop_rd(rider + 0x44, type, 4);
    return a;
}
static void to_frame(const float pose[7], const float p[3], float out[3]) {   // p in the frame of an actor at pose: R^T (p - pos)
    float r[9], d[3] = {p[0] - pose[0], p[1] - pose[1], p[2] - pose[2]};
    en_rot_of_quat(pose + 3, r);
    for (int i = 0; i < 3; i++) out[i] = r[0 * 3 + i] * d[0] + r[1 * 3 + i] * d[1] + r[2 * 3 + i] * d[2];
}
static void from_frame(const float pos[3], const float q[4], const float off[3], float out[3]) {
    float r[9];
    en_rot_of_quat(q, r);
    for (int i = 0; i < 3; i++) out[i] = pos[i] + r[i * 3 + 0] * off[0] + r[i * 3 + 1] * off[1] + r[i * 3 + 2] * off[2];
}

// ---- Phase 0: who sits on what (spec 4.6) -------------------------------------------------------------------------------
// This game's Link and every enemy this game runs, each with what it rides; read once a phase 0 under en_lock.
#define SEAT_N 32
static struct { uint64_t mount, rider_actor; uint8_t who; uint32_t seat, type; } seats[SEAT_N];   // who: 1 + net id, or 0xff
static int nseats;
static uint64_t my_mount;            // this Link's horse (0 none)
static uint64_t off_due_ns;          // since when this Link sits on a horse another game runs and states another rider on
static int off_ask;                  // phase 0 puts this Link off its horse (spec 4.6, two mounts at once)
static int16_t en_of_actor(uint64_t a) {
    EnCell *m = a ? en_cell_of(a) : NULL;
    return m ? atomic_load(&m->en) : (int16_t)-1;
}
void coop_mount_scan(void) {   // coop_enemy.c own_pass, under en_lock
    if (!coop_mounts_on()) return;
    uint64_t me = coop_ctr_me(), link = coop_player_actor(), now = en_now();
    nseats = 0, my_mount = 0;
    uint32_t seat = 0, type = 0;
    uint64_t lm = link ? mount_of_rider(rider_of(link), &seat, &type) : 0;
    if (lm) {
        my_mount = lm;
        seats[nseats].mount = lm, seats[nseats].rider_actor = link, seats[nseats].who = (uint8_t)(1 + coop_net_id()), seats[nseats].seat = seat, seats[nseats].type = type, nseats++;
    }
    for (int i = 0; i < EN_N && nseats < SEAT_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->owner != me || e->cell < 0 || e->life != EN_LIVE) continue;
        uint64_t mm = mount_of_rider(rider_of(e->actor), &seat, &type);
        if (mm) seats[nseats].mount = mm, seats[nseats].rider_actor = e->actor, seats[nseats].who = 0xff, seats[nseats].seat = seat, seats[nseats].type = type, nseats++;
    }
    // This Link sits on a horse another game runs whose state names another rider: two mounts crossed and the other
    // game's claim won (spec 4.6). Half a second for the claims to settle, then this Link is put off.
    int16_t hi = en_of_actor(lm);
    int lost = hi >= 0 && en_tab[hi].used && en_tab[hi].owner != me && mt_have[hi] && mt[hi].rider && mt[hi].rider != 1 + coop_net_id();
    if (!lost) off_due_ns = 0;
    else if (!off_due_ns) off_due_ns = now;
    else if (now - off_due_ns > SEC / 2) off_ask = 1, off_due_ns = now + 2 * SEC;   // (asked again 2.5 s later if still so)
}
// Who of this game sits on that actor: 1 + net id this Link, 0xff an enemy this game runs, 0 no one.
static uint8_t sitter_of(uint64_t actor, float roff[3], const float pose[7]) {
    for (int k = 0; k < nseats; k++) {
        if (seats[k].mount != actor) continue;
        float p[3];
        if (roff && pose && coop_rd(seats[k].rider_actor + COOP_ACTOR_POS, p, 12)) to_frame(pose, p, roff);
        return seats[k].who;
    }
    return 0;
}
// An entry this game runs that one of this game's actors sits on: not offered to a nearer player (spec 4.6).
int coop_mount_keep(const En *e) {
    if (!coop_mounts_on() || e->cell < 0) return 0;
    return sitter_of(e->actor, NULL, NULL) != 0;
}
// An entry another game runs whose last state names a rider: not taken because its owner is paused (spec 4.6: the
// rider is in a menu, still sitting on it).
int coop_mount_ridden(const En *e) {
    int i = (int)(e - en_tab);
    return coop_mounts_on() && mt_have[i] && mt[i].rider != 0;
}
// An entry another game runs that one of this game's actors sits on: this game claims it (spec 4.6), unless its owner
// states a rider of its own (the ride hook refused that seat; one taken anyway is put off by coop_mount_scan).
int coop_mount_claim(const En *e) {
    if (!coop_mounts_on() || e->cell < 0) return 0;
    int i = (int)(e - en_tab);
    if (mt_have[i] && mt[i].rider && mt[i].rider != 1 + coop_net_id() && mt[i].rider != 0xff) return 0;
    return sitter_of(e->actor, NULL, NULL) != 0;
}

// ---- Phase 0: read and send, take and show (coop_enemy_copy.c en_copy_phase0) ---------------------------------------------
static uint64_t key_of_id(Enid id) {
    uint64_t en_state_key(Enid id);   // coop_enemy_copy.c
    return en_state_key(id);
}
static void tack_read(uint64_t actor, uint32_t t[3]) {
    uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
    for (int s = 0; s < 3; s++) {
        t[s] = 0;
        uint64_t ln = eu + 0x20 + (uint64_t)(TACK_SLOT0 + s) * 0x18, ref = eu ? coop_rd_ptr(ln + 8) : 0, held = ref ? coop_rd_ptr(ref + 0x40) : 0;
        uint32_t id = 0, gen = 0;
        if (!held || !coop_rd(ln + 0x10, &id, 4) || !coop_rd(held + 0x10, &gen, 4) || id != gen) continue;
        char nm[40] = "";
        coop_actor_name(held, nm, sizeof nm);
        if (!nm[0]) continue;
        t[s] = fnv(nm), tname_add(t[s], nm);
    }
}
// An entry this game runs: what MOUNT says of it, sent when its STATE is (due), and at once when a rider or what it
// rides changes. Called for every owned entry; an enemy that rides nothing and never did sends none.
void coop_mount_owner(Cpu *c, En *e, int due) {
    if (!coop_mounts_on()) return;
    tnames_seed();
    int i = (int)(e - en_tab), mount = kind_is_mount(e->kind);
    MMount m;
    memset(&m, 0, sizeof m);
    uint32_t seat = 0, type = 0;
    uint64_t rides = mount_of_rider(rider_of(e->actor), &seat, &type);
    if (!mount && !rides && !mt_sent_ride[i]) return;   // (an enemy that rides nothing and has said so: nothing to send)
    m.key = key_of_id(e->id), m.id = e->id, m.epoch = e->epoch;
    int16_t ri = en_of_actor(rides);
    if (rides && ri >= 0 && en_tab[ri].used) m.ride = en_tab[ri].id, m.ride_seat = (uint8_t)seat, m.ride_type = (uint8_t)type;
    float pose[7];
    en_pose_of(e->actor, pose);
    if (mount) {
        m.rider = sitter_of(e->actor, m.roff, pose);
        int n = 0;
        const char *const *names = gait_names(e->kind, &n);
        uint64_t b = asbb_of(e->actor), fn = b ? coop_rd_ptr(coop_rd_ptr(b) + 0x170) : 0;
        MCell *x = &mc[e->cell];
        if (fn && due) {
            gait_ids(c, x, e->actor, b, e->kind);
            for (int k = 0; k < n && k < GAIT_N; k++) {
                if (x->gid[k] < 0) continue;
                static __thread const char *p;
                p = names[k];
                call_fv(c, fn, b, (uint64_t)(uintptr_t)&p, 0, &m.gait[k]);
            }
        } else if (mt_have[i]) memcpy(m.gait, mt[i].gait, sizeof m.gait);
        m.ngait = (uint8_t)n;
        if (e->kind == KIND_HORSE) tack_read(e->actor, m.tack);
    }
    int changed = !mt_have[i] || m.rider != mt[i].rider || !enid_eq(m.ride, mt[i].ride) || memcmp(m.tack, mt[i].tack, sizeof m.tack);
    if (changed && (m.rider != mt[i].rider || !enid_eq(m.ride, mt[i].ride)))
        rt_log("coop: mount: %s %016llx (run here): rider %u -> %u at %.2f %.2f %.2f in its frame; rides %016llx seat %u type %u", e->name, (unsigned long long)e->id.a,
               mt_have[i] ? mt[i].rider : 0, m.rider, m.roff[0], m.roff[1], m.roff[2], (unsigned long long)m.ride.a, m.ride_seat, m.ride_type);
    mt[i] = m, mt_have[i] = 1;
    if (!due && !changed) return;
    coop_ctr_send(0, ENM_MOUNT, 0, &m, sizeof m);
    mt_sent_ride[i] = m.ride.a != 0;
}
// An entry another game runs: its latest MOUNT, and what pre-calc shows from it.
void coop_mount_take(En *e, int from) {
    if (!coop_mounts_on() || e->cell < 0) return;
    int i = (int)(e - en_tab);
    MMount m;
    uint64_t age = 0;
    int len = coop_ctr_latest(from, ENM_MOUNT, key_of_id(e->id), &m, sizeof m, &age);
    if (len != (int)sizeof m || !enid_eq(m.id, e->id) || m.epoch != e->epoch || age > 2 * SEC || m.ngait > GAIT_N) return;
    if (!mt_have[i] || m.rider != mt[i].rider || !enid_eq(m.ride, mt[i].ride))
        rt_log("coop: mount: %s %016llx (copy): its owner states rider %u at %.2f %.2f %.2f; rides %016llx seat %u type %u", e->name, (unsigned long long)e->id.a,
               m.rider, m.roff[0], m.roff[1], m.roff[2], (unsigned long long)m.ride.a, m.ride_seat, m.ride_type);
    mt[i] = m, mt_have[i] = 1, mt_ns[i] = en_now();
    MCell *x = &mc[e->cell];
    uint64_t ra = 0;
    if (m.ride.a)
        for (int k = 0; k < EN_N; k++)
            if (en_tab[k].used && enid_eq(en_tab[k].id, m.ride) && en_tab[k].cell >= 0) { ra = en_tab[k].actor; break; }
    uint32_t s0 = atomic_load_explicit(&x->seq, memory_order_relaxed);
    atomic_store_explicit(&x->seq, s0 + 1, memory_order_release);
    x->m = m, x->ride_actor = ra, x->on = 1;
    atomic_store_explicit(&x->seq, s0 + 2, memory_order_release);
}
// An entry freed or changing owner: nothing of an earlier owner is kept.
void coop_mount_forget(int i, int cell) {
    if (i >= 0 && i < EN_N) mt_have[i] = 0, mt_sent_ride[i] = 0;
    if (cell >= 0 && cell < EN_MAP_N) mc[cell].on = 0;
}

// This Link put off its horse when two mounts crossed (spec 4.6); from phase 0, which has a Cpu.
void coop_mount_phase0(Cpu *c) {
    if (!coop_mounts_on() || !off_ask) return;
    off_ask = 0;
    uint64_t link = coop_player_actor(), r = link ? rider_of(link) : 0;
    if (!r) return;
    coop_guest_call(c, MOD_MAIN_BASE + FN_GET_OFF, r, 1, 1, 0);
    rt_log("coop: mount: this Link is put off its horse: another player's game runs it with that player on it");
}

// ---- Pre-calc of a copy -------------------------------------------------------------------------------------------------
static int show_read(MCell *x, MMount *m, uint64_t *ra) {
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s0 = atomic_load_explicit(&x->seq, memory_order_acquire);
        if (s0 & 1) continue;
        int on = x->on;
        *m = x->m, *ra = x->ride_actor;
        if (atomic_load_explicit(&x->seq, memory_order_acquire) == s0) return on;
    }
    return 0;
}
static __thread int tl_ride_mine;   // inside this file's own ride call (the ride hook lets it through)
// Before the pose is put: a copy rider is seated on, or taken off, this game's copy of what its owner's rides (spec 6).
// 1: it sits on it, and its pose is not written (the seat places it from the horse).
int coop_mount_precalc_ride(Cpu *c, uint64_t actor, EnCell *cell) {
    if (!coop_mounts_on()) return 0;
    MCell *x = &mc[cell - en_map];
    MMount m;
    uint64_t want = 0;
    uint64_t r = rider_of(actor);
    uint32_t seat = 0, type = 0;
    uint64_t on = r ? mount_of_rider(r, &seat, &type) : 0;
    // (nothing stated yet, as after a change of owner: what it sits on is left as it is until its owner says)
    if (!show_read(x, &m, &want)) return x->seated_on && on == x->seated_on;
    uint32_t fr = coop_sess_frame_no();
    if (on && on != want) {   // its owner's rides nothing, or another horse: off first (the ride refuses a busy rider)
        if (fr - x->ride_try_frame >= 30) {
            x->ride_try_frame = fr;
            static __thread uint64_t zero;
            zero = 0;
            coop_guest_call(c, MOD_MAIN_BASE + FN_GET_OFF, r, 0, (uint64_t)(uintptr_t)&zero, 0);
            rt_log("coop: mount: the copy %llx gets off %llx: its owner's rides %s", (unsigned long long)actor, (unsigned long long)on, want ? "another" : "nothing");
        }
        x->seated_on = 0;
        return 0;
    }
    if (!on && want && r && x->ride_tries < 5 && fr - x->ride_try_frame >= 30) {
        x->ride_try_frame = fr;
        uint32_t id = 0;
        uint64_t brain_ref = 0;
        coop_rd(want + 0x10, &id, 4);
        // The mount's link reference: the object actor links point at ([ref+0x40] the actor, [ref+0x48] its id), found by
        // its content among the horse's first words, as coop_enemy_copy.c brain_ref_of finds it.
        for (int o = 0; o < 0x400 && !brain_ref; o += 8) {
            uint64_t p = coop_rd_ptr(want + (uint64_t)o);
            uint32_t rid = 0;
            if (p > 0x10000 && p != want && coop_rd_ptr(p + 0x40) == want && coop_rd(p + 0x48, &rid, 4) && rid == id) brain_ref = p;
        }
        if (brain_ref) {
            static __thread _Alignas(16) uint64_t link[4];
            memset(link, 0, sizeof link);
            link[0] = coop_rd_ptr(r + 0x20), link[1] = brain_ref, memcpy(&link[2], &id, 4);
            tl_ride_mine = 1;
            uint64_t ok = coop_guest_call(c, MOD_MAIN_BASE + FN_RIDE, r, (uint64_t)(uintptr_t)link, m.ride_type ? m.ride_type : 1, m.ride_seat);
            tl_ride_mine = 0;
            x->ride_tries++;
            on = mount_of_rider(r, NULL, NULL);
            rt_log("coop: mount: the copy %llx is seated on %llx (type %u, seat %u) as its owner's rides: the ride answered %llu, it %s", (unsigned long long)actor,
                   (unsigned long long)want, m.ride_type, m.ride_seat, (unsigned long long)(ok & 1), on == want ? "sits" : "does not sit");
        }
    }
    if (!want) x->ride_tries = 0;
    x->seated_on = on == want ? on : 0;
    return x->seated_on != 0;
}
// After the animation is asked: the gait values and the tack (spec 7.1, 4.5).
void coop_mount_precalc(Cpu *c, uint64_t actor, EnCell *cell) {
    if (!coop_mounts_on()) return;
    MCell *x = &mc[cell - en_map];
    MMount m;
    uint64_t ra;
    if (!show_read(x, &m, &ra)) return;
    int16_t ei = atomic_load(&cell->en);
    int kind = ei >= 0 ? en_tab[ei].kind : 0;
    if (!kind_is_mount(kind)) return;
    uint64_t b = asbb_of(actor);
    int n = 0;
    gait_names(kind, &n);
    if (b) gait_ids(c, x, actor, b, kind);
    uint64_t set = b ? coop_rd_ptr(coop_rd_ptr(b) + 0x70) : 0;
    for (int k = 0; set && k < n && k < GAIT_N && k < m.ngait; k++)
        if (x->gid[k] >= 0) call_fv(c, set, b, (uint64_t)(uint32_t)x->gid[k], m.gait[k], NULL);
    if (kind != KIND_HORSE) return;
    // The tack: a slot its owner's horse fills and the copy's does not is filled by the game's tack call, once in a
    // second, three tries a slot. (A copy made by name wears nothing: research H2 section 4.)
    uint32_t have[3];
    tack_read(actor, have);
    uint32_t fr = coop_sess_frame_no();
    uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
    for (int s = 0; eu && s < 3 && fr - x->tack_frame >= 60; s++) {
        const char *nm = m.tack[s] ? tname_of(m.tack[s]) : NULL;
        if (!nm || have[s] == m.tack[s] || x->tack_tries[s] >= 3) continue;
        x->tack_frame = fr, x->tack_tries[s]++;
        static __thread _Alignas(16) uint8_t str[0x60];
        memset(str, 0, sizeof str);
        snprintf((char *)str + 0x10, 0x40, "%s", nm);
        uint64_t tp = (uint64_t)(uintptr_t)(str + 0x10);
        int32_t cap = 0x40;
        memcpy(str, &tp, 8), memcpy(str + 8, &cap, 4);
        uint64_t sp0 = c->sp;
        c->sp = (sp0 - 0x100) & ~0xfull;
        uint64_t r = rt_call(c, MOD_MAIN_BASE + FN_TACK, 6, eu, (uint64_t)(TACK_SLOT0 + s), (uint64_t)(uintptr_t)str, (uint64_t)(uint32_t)-1, (uint64_t)0, (uint64_t)1);
        c->sp = sp0;
        rt_log("coop: mount: the copy %llx is given %s (slot %#x) as its owner's horse wears: the call answered %llu (try %u)", (unsigned long long)actor, nm, TACK_SLOT0 + s, (unsigned long long)(r & 1), x->tack_tries[s]);
        break;
    }
}

// ---- Where a remote rider's stand-in sits (coop_players.c ride_place) ------------------------------------------------------
// On this game's copy of the horse its owner's game states that player on, at the stated place in its frame, from the
// pose the copy is shown at this frame (spec 5.2). 0: that player rides no horse here.
int coop_mount_place(int net_id, float out[3]) {
    if (!coop_mounts_on() || net_id < 0) return 0;
    for (int i = 0; i < EN_N; i++) {
        if (!mt_have[i] || mt[i].rider != 1 + net_id || !en_tab[i].used || en_tab[i].cell < 0 || en_tab[i].owner == coop_ctr_me()) continue;
        EnCell *m = &en_map[en_tab[i].cell];
        float pos[3], q[4];
        int on = 0;
        for (int tries = 0; tries < 4; tries++) {
            uint32_t s0 = atomic_load_explicit(&m->seq, memory_order_acquire);
            if (s0 & 1) continue;
            on = m->show_on, memcpy(pos, m->show_pos, 12), memcpy(q, m->show_quat, 16);
            if (atomic_load_explicit(&m->seq, memory_order_acquire) == s0) break;
            on = 0;
        }
        if (!on) return 0;
        from_frame(pos, q, mt[i].roff, out);
        return 1;
    }
    return 0;
}

// ---- Hooks ------------------------------------------------------------------------------------------------------------
// The ride main+0x502e60 (x0 the rider component, x1 the mount link): refused, before any write, for a horse another game
// runs and states someone else on (spec 4.6). 1: refused.
int coop_mount_ride_hook(Cpu *c) {
    if (!coop_mounts_on() || tl_ride_mine) return 0;
    uint64_t h = c->x[1], ref = h > 0x10000 ? coop_rd_ptr(h + 8) : 0, a = ref ? coop_rd_ptr(ref + 0x40) : 0;
    EnCell *m = a ? en_cell_of(a) : NULL;
    int16_t i = m ? atomic_load(&m->en) : (int16_t)-1;
    if (i < 0 || !en_tab[i].used || en_tab[i].owner == coop_ctr_me() || !mt_have[i] || !mt[i].rider) return 0;
    uint64_t who = coop_rd_ptr(c->x[0] + 0x18);
    if (mt[i].rider == 1 + coop_net_id() && who == coop_player_actor()) return 0;   // (this Link again: its own seat as stated)
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: mount: a seat on %s %016llx is refused here: its owner's game states rider %u on it", en_tab[i].name, (unsigned long long)en_tab[i].id.a, mt[i].rider);
    c->x[0] = 0;
    return 1;
}
// A wild herd's create (main+0xd7ff68, x0 the node; its actor, the tag, at [[[x0+8]+8]+0x1f8]): made in one game only
// (spec 4.3). Not here while another player's Link is nearer the tag than this game's (within 400 m), or another game's
// horse made at run time stands within 200 m of it; the node is told it made its herd (it returns 1), so it does not
// ask again in this load.
int coop_mount_herd_hook(Cpu *c) {
    if (!coop_mounts_on()) return 0;
    uint64_t tag = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(c->x[0] + 8) + 8) + 0x1f8), link = coop_player_actor();
    float tp[3], lp[3], pl[COOP_MAX_PLAYERS][3];
    if (!tag || !link || !coop_rd(tag + COOP_ACTOR_POS, tp, 12) || !coop_rd(link + COOP_ACTOR_POS, lp, 12)) return 0;
    float dm = sqrtf((tp[0] - lp[0]) * (tp[0] - lp[0]) + (tp[1] - lp[1]) * (tp[1] - lp[1]) + (tp[2] - lp[2]) * (tp[2] - lp[2]));
    int ids[COOP_MAX_PLAYERS], n = coop_player_positions_ids(pl, ids, COOP_MAX_PLAYERS);
    const char *why = NULL;
    for (int k = 0; k < n && !why; k++) {
        float d = sqrtf((tp[0] - pl[k][0]) * (tp[0] - pl[k][0]) + (tp[1] - pl[k][1]) * (tp[1] - pl[k][1]) + (tp[2] - pl[k][2]) * (tp[2] - pl[k][2]));
        if (d < 400.f && d < dm) why = "another player's Link is nearer it";
    }
    uint64_t me = coop_ctr_me();
    coop_lock_wait(&en_lock);
    for (int i = 0; i < EN_N && !why; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->owner == me || e->life != EN_LIVE || e->kind != KIND_HORSE || e->id.b != EN_RT) continue;
        float d = sqrtf((tp[0] - e->pose[0]) * (tp[0] - e->pose[0]) + (tp[1] - e->pose[1]) * (tp[1] - e->pose[1]) + (tp[2] - e->pose[2]) * (tp[2] - e->pose[2]));
        if (d < 200.f) why = "another game's wild horse stands within 200 m of it";
    }
    plat_mutex_unlock(&en_lock);
    if (!why) { rt_log("coop: mount: herd: a wild horse tag %.0f %.0f %.0f makes its herd here", tp[0], tp[1], tp[2]); return 0; }
    rt_log("coop: mount: herd: a wild horse tag %.0f %.0f %.0f makes no herd here: %s", tp[0], tp[1], tp[2], why);
    c->x[0] = 1;
    return 1;
}
