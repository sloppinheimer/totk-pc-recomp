// Co-op: contraptions (docs/coop/contraption-sync-v3.md, section 27). TOTK_COOP_CONTRAPTION=1. TotK 1.4.3 main offsets.
//
// Every game keeps one entry per build, known by the id its first recorder minted (cid: that game's incarnation and a
// count). One game runs a build (simulates it); the others follow. The runner records the build with the game's
// history writer every 2 s and sends each new shape as a version; a follower makes the version with its own Autobuild
// build object and moves the copy as one rigid body toward the runner's root (the rigid field). Who runs a build
// changes by TAKE and GRANT when another player holds or steers it. All messages go over the contraption channel
// (coop_ctr.c).
//
// Threads: coop_ctn_phase0 (records, copy making) runs from GameDataMgr::calc at actor phase 0; coop_ctn_tick (messages,
// authority, drive, devices) from the co-op tick. The hooks (coop_g1, coop_ctn_driven, coop_ctn_no_charge,
// coop_contraption_made) run on any game thread and read atomic lists only; coop_contraption_follower takes the lock.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define HISTORY_WRITE 0x1913694
#define LINK_SET_FN 0x155ec
#define HISTORY_MGR_SLOT 0x395f6f8
#define BP_SIZE 0x1818          // a CmbAct: header, 21 parts of 0x118 from +0xc, 21 bonds of 12 from +0x1704, the tail
#define VER_CHUNK 840
#define VER_CHUNKS ((BP_SIZE + VER_CHUNK - 1) / VER_CHUNK)

int coop_own_sent_objects(uint64_t *out, int max), coop_is_copy(uint64_t a), coop_obj_ident(uint64_t a, int *owner, uint32_t *id);
uint64_t coop_obj_find(int owner, uint32_t id, const char *name);   // coop_players.c
static int lone_ask(uint64_t actor, uint32_t want, uint32_t bits);
void coop_deleted_here(uint64_t a);   // coop_players.c: not told to the others as destroyed
uint64_t coop_life_ability(void);
void coop_life_free_confirm(int on);
void coop_life_veto_rebuild(Cpu *c, const uint64_t *parts, int nparts);
uint32_t coop_life_refused(void);
extern uint8_t coop_ab_cai[0x1820];
extern _Atomic int64_t coop_ultrahand_hold_us;

static int ctn_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_get("TOTK_COOP_CONTRAPTION") && *cfg_get("TOTK_COOP_CONTRAPTION") == '1';
        // Devices' on and off are this file's then, lone ones too: the older flip path of coop_bonds.c stands down. And
        // glue and bonds: anything glued is a build of this file's, whoever owned the parts; the older glue sync made a
        // maker's bond again every second and refused its break (plan 37 note 11 and section 38). They are switched off
        // in this process's environment, which cfg_get reads first and cfg_save never writes, not with cfg_set, which
        // totk.ini would keep after this session (review 1 finding 84). memory.c calls this right after cfg_init, before
        // the older files read their switch once and keep it.
        static const char *const older[] = {"TOTK_COOP_DEVICE_SYNC", "TOTK_COOP_GLUE", "TOTK_COOP_BONDS"};
        for (int k = 0; on && k < 3; k++)
            if (!cfg_from_env(older[k])) plat_setenv(older[k], "0");
    }
    return on;
}

// ---- The registry ----------------------------------------------------------------------------------------------------
typedef struct FollowState {
    float v_written[21][3]; // the velocity written to each part's main body last frame
    int have_written;
    float gc;               // the vertical part of comp, for the log
    float comp[3];          // what one physics step takes off the written velocity (gravity, damping), added back, filtered
    float integ[3];         // the integral term (m/s)
    int rest;               // 1: resting (nothing written)
    int64_t still_since;    // since when the runner's root has been still (its clock)
    int64_t last_us;
} FollowState;

enum { L_NONE, L_MAKING, L_LIVE };
typedef struct {
    int used;
    uint64_t inc;           // cid: the minter's incarnation and its count
    uint32_t serial;
    uint64_t key64;         // the cid as one number (STATE's latest-only slot)
    uint64_t runner;        // the incarnation whose game runs it
    uint32_t epoch;         // rises at each change of runner
    int local;              // L_*: this game's parts
    uint32_t v, shape;      // the version the parts here are (0: none)
    uint32_t green, in_green; // bit r: record part r was made from zonaite by an Autobuild (SPR +0x150 bit 0 where it runs)
    uint8_t rec[BP_SIZE];   // its record, centre, and where each part came from: a game's object number (0: none)
    float centre[3];
    uint32_t ids[21];
    uint64_t oinc[21];
    uint64_t parts[21];     // this game's actor of each record part (0: gone here, see parts_sweep)
    uint32_t gen[21];       // each actor's id (actor +0x10) when it was taken: a freed and reused actor has another
    uint32_t del_ok;        // bit r: the delete of part r was accepted (not asked twice)
    uint32_t adopt;         // bit r: part r was not made by this entry: a mirror copy of another game's object, or this
                            // game's own object, taken into the build where it stood. Let go, never deleted.
    int64_t del_since;      // since when a delete of a part of it has been refused (0: none)
    int64_t stuck_until;    // not picked by the maker before this (a refused delete: the other builds go first)
    uint32_t unloaded;      // bit r: part r went here with no death asked of it (the engine unloaded it, plan 41)
    uint32_t vseen;         // the highest version number heard of it: a new runner numbers on from here (finding 39)
    int relinq_later;       // a RELINQUISH that came while it was being made: taken up once made (finding 38)
    int np;
    int absent;             // a follower whose parts the engine unloaded: 1 far, 2 near again and waiting for the version
    float absent_dist;      // how far this game's Link was when they went
    int64_t pulled_us;
    int original;           // the parts are this game's own objects (minted here, not made from a record)
    // a newer version coming in, and whether it waits to be made
    uint32_t in_v, in_shape, in_have, in_ids[21];
    uint64_t in_oinc[21];
    uint8_t in_rec[BP_SIZE];
    float in_centre[3];
    int want_make;
    uint64_t old[21];       // a remade copy's old parts, until they have left (one version per world)
    int64_t old_us, waited_us;
    // the runner: when last recorded, and whether the version is to be sent (again)
    int64_t seen_us, gone_us;
    int resend;
    // a follower: the runner's last three root samples (newest last, its clock), the clock offset, the drive
    float hm[3][12];
    int64_t hus[3], clock_off, clock_off_at, heard_us;
    FollowState *fs;
    uint32_t sw, on;        // at the runner: which record parts have a switch, which are on
    uint32_t fp;            // bit r: part r is a spring; pf[r] = its extension, upper limit, enables
    uint32_t burn;          // bit r: part r burns (where the build runs)
    uint32_t fw, fk, fb;    // at the runner: which parts are wheels / sticks / time bombs, and their floats (StMsg)
    float pf[21][4];
    uint64_t payer;         // the incarnation whose gauge pays (who switched it on)
    int64_t toggled_us, payer_us;
    int touched;            // this game's player holds or steers it (drive off, gates open, TAKE asked)
    int64_t asked_us, changed_us, remade_us;
} Ent;
#define REG_N 24
static Ent reg[REG_N];
static PlatMutex reg_lock = PLAT_MUTEX_INIT;

static uint64_t cid_key(uint64_t inc, uint32_t serial) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; i++) h = (h ^ (inc >> (8 * i) & 0xff)) * 1099511628211ull;
    for (int i = 0; i < 4; i++) h = (h ^ (serial >> (8 * i) & 0xff)) * 1099511628211ull;
    return h ? h : 1;
}
static Ent *ent_find(uint64_t inc, uint32_t serial) {
    for (int i = 0; i < REG_N; i++) if (reg[i].used && reg[i].inc == inc && reg[i].serial == serial) return &reg[i];
    return NULL;
}
static Ent *ent_new(uint64_t inc, uint32_t serial) {   // (a free slot; a full table gives up its oldest entry with no parts)
    Ent *e = NULL;
    for (int i = 0; i < REG_N && !e; i++) if (!reg[i].used) e = &reg[i];
    for (int i = 0; i < REG_N && !e; i++) if (reg[i].local == L_NONE && !reg[i].want_make) e = &reg[i];
    if (!e) return NULL;
    free(e->fs);
    memset(e, 0, sizeof *e);
    e->used = 1, e->inc = inc, e->serial = serial, e->key64 = cid_key(inc, serial);
    return e;
}
static int runs(const Ent *e) { return e->runner && e->runner == coop_ctr_me(); }
static uint32_t cid32(const Ent *e) { return (uint32_t)(e->key64 ^ e->key64 >> 32); }   // (for the log)
// Parts whose death was asked here lately (coop_ctn_death_seen): a part that goes with no death asked was unloaded by
// the engine, one that died was destroyed. The two are told apart where a build loses parts (record_pass, authority).
static struct { uint64_t actor; int64_t us; } died[32];
static int died_i;
static int died_lately(uint64_t a, int64_t now) {   // (reg_lock held)
    for (int i = 0; a && i < 32; i++) if (died[i].actor == a && now - died[i].us < 5000000) return 1;
    return 0;
}

// The parts made here from records: not this game's own objects (not sent as objects, not recorded as a new build)
#define MADE_N 160
static _Atomic uint64_t made_parts[MADE_N];
static void made_add(uint64_t a) {
    for (int i = 0; a && i < MADE_N; i++) {
        uint64_t have = atomic_load(&made_parts[i]);
        if (have == a) return;
        if (!have && atomic_compare_exchange_strong(&made_parts[i], &have, a)) return;
    }
}
static void made_forget(uint64_t a) {   // (a made part that became this game's own loose object)
    for (int i = 0; a && i < MADE_N; i++) {
        uint64_t have = a;
        atomic_compare_exchange_strong(&made_parts[i], &have, 0);
    }
}
int coop_contraption_made(uint64_t a) {
    for (int i = 0; a && i < MADE_N; i++) if (atomic_load(&made_parts[i]) == a) return 1;
    return 0;
}
static int alive(uint64_t a) {
    int32_t st = 0;
    return a && coop_rd(a + 0x24, &st, 4) && st >= 2 && st <= 6;
}
static uint32_t gen_of(uint64_t a) {   // the actor's id (actor +0x10, what an ActorLink's +0x10 is compared with)
    uint32_t g = 0;
    return a && coop_rd(a + 0x10, &g, 4) ? g : 0;
}
static int alive_as(uint64_t a, uint32_t gen) { return alive(a) && gen_of(a) == gen; }
// A part of a version being replaced or retired. 1: gone, going, or the request was accepted; 0: the game refused it
// now (a delete lock is held), to be asked again. The request main+0x63baa8 reads the actor's lock object
// ([actor +0x1a8]) first, and the game drops that object once the actor has passed state 7: asked on such an actor it
// read address 0x50 and the game died (plan 31, note 7). So it is asked only of an actor in state 2..6 with the object
// there. Reason 0 is "delete this actor"; reason 3 is the far delete of an unload (research sync-v3/m2/p4/death-delete).
static int part_delete(Cpu *c, uint64_t a) {
    if (!a) return 1;
    if (!alive(a) || !coop_rd_ptr(a + 0x1a8)) { made_forget(a); return 1; }
    // Already asked and accepted (actor +0x28 bit 17): the game answers 0 to a second ask, which is no refusal. It goes
    // once its locks let it (research notes sync-v3/m2/p7/delete-refused, Q1 and Q3). Taken as refused, one such part
    // held up every later build (plan 37, note 12).
    {
        uint32_t f28 = 0;
        if (coop_rd(a + 0x28, &f28, 4) && (f28 >> 17 & 1)) { made_forget(a); return 1; }
    }
    coop_deleted_here(a);
    if (!(coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, 0, 0, 0) & 1)) {
        static int64_t said;   // (a lock lane held: lane 2 is the one a delete of reason 0 minds)
        uint64_t lanes = 0;
        int64_t now = frame_step_world_us();
        coop_rd(coop_rd_ptr(a + 0x1a8) + 0x50, &lanes, 8);
        if (now - said > 2000000) said = now, rt_log("coop: contraption: a delete refused by the game: lock lanes %016llx (byte 2 held: a reference to the actor is kept somewhere)", (unsigned long long)lanes);
        return 0;
    }
    made_forget(a);
    return 1;
}
// Every part of e deleted; 1 once none is left to ask (a part whose delete was accepted is not asked again)
// A part whose delete the game still refuses after 2 s is given up: it stays in this game as it is, no longer a part
// of the entry, and the entry goes on. Waiting for it without end held up every later build of that game (plan 37,
// note 12: one fan's delete was refused for over a minute).
static int parts_delete(Cpu *c, Ent *e) {
    int left = 0;
    int64_t now = frame_step_world_us();
    for (int r = 0; r < e->np && r < 21; r++) {
        if (e->del_ok >> r & 1) continue;
        if (!alive_as(e->parts[r], e->gen[r])) { made_forget(e->parts[r]), e->del_ok |= 1u << r; continue; }
        // Only a part this game made from a record is deleted. Any other (the object mirror's copy, or this game's own
        // object taken into a build) is let go as it is, whatever the entry's bits say (review 1 finding 1).
        if ((e->adopt >> r & 1) || !coop_contraption_made(e->parts[r])) { e->del_ok |= 1u << r; continue; }
        if (part_delete(c, e->parts[r])) { e->del_ok |= 1u << r; continue; }
        if (!e->del_since) e->del_since = now;
        if (now - e->del_since > 2000000) {
            char nm[48];
            rt_log("coop: contraption: %08x: the game would not delete part %d (%s) for 2 s: left here as it is, the build goes on without it", cid32(e), r,
                   coop_actor_name(e->parts[r], nm, sizeof nm));
            e->del_ok |= 1u << r;
            continue;
        }
        left++;
    }
    if (!left) e->del_since = 0;
    return !left;
}

// ---- Game reads ------------------------------------------------------------------------------------------------------
static uint64_t link_actor(uint64_t link) {   // an ActorLink (coop_glue.c link_actor)
    uint64_t ref = coop_rd_ptr(link + 8);
    uint32_t id = 0, rid = 0;
    if (!ref || !coop_rd(link + 0x10, &id, 4) || id == ~0u || !coop_rd(ref + 0x48, &rid, 4) || rid != id) return 0;
    return coop_rd_ptr(ref + 0x40);
}
static uint64_t part_group(uint64_t a) {   // a part's contraption group (SPR +0x28)
    uint64_t spr = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) : 0;
    return spr ? coop_rd_ptr(spr + 0x28) : 0;
}
static uint64_t main_body_of(uint64_t a) {
    return a ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) + 0x150) : 0;
}
static uint64_t part_switch(uint64_t a) {   // component 166 when the device takes the switch (+0x60)
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c166 = t ? coop_rd_ptr(t + 0x530) : 0;
    uint8_t b60 = 0;
    return c166 && coop_rd(c166 + 0x60, &b60, 1) && b60 ? c166 : 0;
}
// An actor's blackboard ([[[actor +0x228] +0] +0x10]) and the index of a value in one of its sections: the bools at
// +0x198, the floats at +0x120; a section holds its count at +0, its values at +8, a hash table at +0x30 of
// {u32 hash, s32 index} slots and the table's size at +0x3c. A key is looked up from hash % size onward, wrapping; an
// empty slot (hash 0) ends the search. Keys are murmur3_32 of the name. Read from the game: research notes
// sync-v3/m2/p5/engine-access.
#define BB_BOOLS 0x198
#define BB_FLOATS 0x120
static uint64_t actor_bb(uint64_t a) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c0 = t ? coop_rd_ptr(t) : 0;
    return c0 ? coop_rd_ptr(c0 + 0x10) : 0;
}
static int bb_index(uint64_t bb, uint32_t sec, uint32_t hash) {
    uint64_t tab = bb ? coop_rd_ptr(bb + sec + 0x30) : 0;
    int32_t size = 0, count = 0;
    if (!tab || !coop_rd(bb + sec + 0x3c, &size, 4) || !coop_rd(bb + sec, &count, 4) || size <= 0 || size > 4096) return -1;
    uint32_t at = hash % (uint32_t)size;
    for (int i = 0; i < size; i++, at = (at + 1) % (uint32_t)size) {
        struct { uint32_t hash; int32_t index; } e;
        if (!coop_rd(tab + 8 * (uint64_t)at, &e, 8) || !e.hash) return -1;
        if (e.hash == hash) return e.index >= 0 && e.index < count ? e.index : -1;
    }
    return -1;
}
// The device's on state: the blackboard bool SpecialParts_IsOn (0xbcd1015f), which the on/off node writes at every
// change of its latch and which the game's own group code reads. The word this used to read ([comp 11] +0x348 bit
// 0x2000) is not the latch and its writer is unknown (research notes sync-v3/m2/p5/setoff-not-taking, section 5); it
// is the fallback for an actor with no such bool.
static int part_on(uint64_t a) {
    uint64_t bb = actor_bb(a);
    int i = bb_index(bb, BB_BOOLS, 0xbcd1015fu);
    uint8_t b = 0;
    if (i >= 0) return coop_rd(coop_rd_ptr(bb + BB_BOOLS + 8) + (uint64_t)i, &b, 1) && b;
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c11 = t ? coop_rd_ptr(t + 8 * 11) : 0;
    uint32_t v = 0;
    return c11 && coop_rd(c11 + 0x348, &v, 4) && (v & 0x2000);
}
// The floats a part shows itself with, read where the build runs and written here on its copy: a wheel's model spin
// and steering angle (FastWheel_ModelRotateSpeed, FastWheel_ModelSteeringAngle: the wheel's model node turns the wheel
// by them), a steering stick's tilt (ControlStick_StickX/Y, which the rider's pose reads, and ScaledStickX/Y, which
// the stick's own animation reads). Research notes sync-v3/m2/p5/wheels section 1, stick-head section 3.
#define BB_WHEEL_SPIN 0x4b7368f5u
#define BB_WHEEL_STEER 0xbc8c3de4u
#define BB_STICK_X 0x6f1afb02u
#define BB_STICK_Y 0xde18d120u
#define BB_STICK_SX 0x4b809ce9u
#define BB_STICK_SY 0x9479c43du
static int bb_f32(uint64_t bb, uint32_t hash, float *out) {
    int i = bb_index(bb, BB_FLOATS, hash);
    return i >= 0 && coop_rd(coop_rd_ptr(bb + BB_FLOATS + 8) + 4 * (uint64_t)i, out, 4);
}
// (with a Cpu: through the game's own float write main+0xc3e260(bb, index, &value), which also minds the lock byte and
// bumps the float's change counter; without: the bare store. Research notes sync-v3/m2/p7/stick-graph, answer 2.)
static void bb_set_f32(Cpu *c, uint64_t bb, uint32_t hash, float v) {
    static _Alignas(16) float arg;
    int i = bb_index(bb, BB_FLOATS, hash);
    uint64_t arr = i >= 0 ? coop_rd_ptr(bb + BB_FLOATS + 8) : 0;
    if (!arr || v != v) return;
    if (c) arg = v, coop_guest_call(c, MOD_MAIN_BASE + 0xc3e260, bb, (uint64_t)i, (uint64_t)(uintptr_t)&arg, 0);
    else memcpy((void *)(uintptr_t)(arr + 4 * (uint64_t)i), &v, 4);
}
static int part_green(uint64_t a) {   // made from zonaite by an Autobuild confirm (SPR +0x150 bit 0, as main+0x1b0f7d4 reads it)
    uint64_t spr = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) : 0;
    uint32_t f = 0;
    return spr && coop_rd(spr + 0x150, &f, 4) && (f & 1);
}
// Held with Ultrahand by this game's Link. The held bit (SPR +0x20 bit 16) is set only by main+0x1c0b83c, which
// stores who holds as a handle at [SPR +0x30] +0x10 (slot +8, id +0x10; the slot's +0x48 is the id while the handle is
// good, +0x40 the actor). The bit alone read true for one tick on a copy just made, with no hold by this game's
// player, and the other game lost its car to it (plan 31, note 6a): so the holder has to be this game's Link.
// Research notes sync-v3/m2/p5/stick-head, section 4.
static int part_held(uint64_t a) {
    uint64_t spr = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) : 0, me = coop_player_actor();
    uint64_t f = 0;
    if (!spr || !me || !coop_rd(spr + 0x20, &f, 8) || !(f & 0x10000)) return 0;
    uint64_t h = coop_rd_ptr(spr + 0x30), slot = h ? coop_rd_ptr(h + 0x10 + 8) : 0;
    uint32_t id = 0, sid = 0;
    uint64_t holder = slot && coop_rd(h + 0x10 + 0x10, &id, 4) && id != ~0u && coop_rd(slot + 0x48, &sid, 4) && sid == id ? coop_rd_ptr(slot + 0x40) : 0;
    if (holder != me) {
        static int64_t said;
        int64_t now = frame_step_world_us();
        char nm[48] = "";
        if (now - said > 2000000)
            said = now, rt_log("coop: contraption: a part reads held, and not by this Link (holder %s): no touch", holder ? coop_actor_name(holder, nm, sizeof nm) : "none");
        return 0;
    }
    return 1;
}
// The Link steering a stick: its rider sources (component 129, [actor+0x228]+0x408: count +0x20, array +0x28 of
// pointers), one with state +0x18 == 2, +0x74 <= 0 and a live handle (slot [src+8]: +0x48 its id, +0x40 the actor),
// as main+0x42cbdc reads them (research devices2/control/)
static uint64_t stick_rider(uint64_t a) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c129 = t ? coop_rd_ptr(t + 0x408) : 0;
    int32_t n = 0;
    if (!c129 || !coop_rd(c129 + 0x20, &n, 4) || n <= 0 || n > 8) return 0;
    uint64_t arr = coop_rd_ptr(c129 + 0x28);
    for (int i = 0; arr && i < n; i++) {
        uint64_t src = coop_rd_ptr(arr + 8 * (uint64_t)i), slot = src ? coop_rd_ptr(src + 8) : 0;
        int32_t st = 0, id = -1, sid = 0;
        float wait = 1;
        if (!slot || !coop_rd(src + 0x18, &st, 4) || st != 2 || !coop_rd(src + 0x74, &wait, 4) || wait > 0 || !coop_rd(src + 0x10, &id, 4) ||
            id == -1 || !coop_rd(slot + 0x48, &sid, 4) || sid != id)
            continue;
        uint64_t rider = coop_rd_ptr(slot + 0x40);
        if (alive(rider)) return rider;   // (the game's own readers also want the occupant in state 2 to 6)
    }
    return 0;
}
// 1: this game's player touches the contraption: holds a part with Ultrahand, or steers its stick
static int any_held(const uint64_t *parts, int n) {
    uint64_t me = coop_player_actor();
    for (int r = 0; r < n && r < 21; r++)
        if (part_held(parts[r]) || (me && stick_rider(parts[r]) == me)) return 1;
    return 0;
}
static uint32_t bp_shape(const uint8_t *r) {   // the structure: names and bonds, not where the parts are
    uint32_t h = 2166136261u;
    int parts = r[9] < 21 ? r[9] : 21, bonds = r[10] < 21 ? r[10] : 21;
    for (int i = 0; i < parts; i++)
        for (const uint8_t *p = r + 0xc + i * 0x118 + 0x98; *p && p < r + 0xc + i * 0x118 + 0xd8; p++) h = (h ^ *p) * 16777619u;
    for (int i = 0; i < bonds; i++)
        for (int b = 0; b < 12; b++) h = (h ^ r[0x1704 + i * 12 + b]) * 16777619u;
    return h ^ (uint32_t)(parts << 8 | bonds);
}
static uint64_t call5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {   // as coop_guest_call
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 5, a0, a1, a2, a3, a4);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}

// ---- Messages (contraption-sync-v3.md 27.4) --------------------------------------------------------------------------
typedef struct {
    uint64_t key, inc;
    uint32_t serial, epoch, v, shape;
    uint64_t runner;
    float centre[3];
    uint16_t chunk, chunks, len, pad;
    uint32_t green, pad2;
    uint32_t ids[21];
    uint64_t oinc[21];
    uint8_t d[VER_CHUNK];
} VerMsg;
_Static_assert(sizeof(VerMsg) <= COOP_CTR_PAYLOAD, "a version piece fits one channel message");
typedef struct {
    uint64_t key, inc;      // (key first: the channel keeps the latest STATE per sender and key)
    uint32_t serial, epoch, v, shape;
    int64_t us;             // the runner's world clock
    uint64_t payer;
    float m[12];            // its root part's main body matrix
    uint32_t sw, on, flags, fb;
    uint32_t fp, burn;      // burn: bit r, part r burns. fp: bit r: part r is a spring; pf[r] = extension, upper limit, enables (1 spring, 2 motor)
    uint32_t fw, fk;        // bit r: part r is a wheel (fw) / a steering stick (fk) / a time bomb (fb), its floats in pf[r]
    float pf[21][4];        // wheel: model spin, model steering angle; stick: x, y, scaled x, scaled y; bomb: fuse left, armed
} StMsg;
_Static_assert(sizeof(StMsg) <= COOP_CTR_PAYLOAD, "a state fits one channel message");
typedef struct { uint64_t inc; uint32_t serial, epoch; uint64_t who; } CtlMsg;   // TAKE, GRANT, PULL, RETIRE
typedef struct { uint64_t inc; uint32_t serial, part, want, pad; } TogMsg;

static void send_version(Ent *e) {   // (reg_lock held) the runner's version to every game
    static VerMsg m;
    memset(&m, 0, sizeof m);
    m.key = e->key64, m.inc = e->inc, m.serial = e->serial, m.epoch = e->epoch, m.v = e->v, m.shape = e->shape, m.runner = e->runner, m.green = e->green;
    memcpy(m.centre, e->centre, sizeof m.centre), memcpy(m.ids, e->ids, sizeof m.ids), memcpy(m.oinc, e->oinc, sizeof m.oinc);
    m.chunks = VER_CHUNKS;
    int ok = 1;
    for (int k = 0; k < VER_CHUNKS && ok; k++) {
        int off = k * VER_CHUNK, len = BP_SIZE - off < VER_CHUNK ? BP_SIZE - off : VER_CHUNK;
        m.chunk = (uint16_t)k, m.len = (uint16_t)len;
        memcpy(m.d, e->rec + off, (size_t)len);
        ok = coop_ctr_send(0, COOP_CTR_VERSION, COOP_CTR_RELIABLE, &m, (int)(offsetof(VerMsg, d) + (size_t)len));
    }
    e->resend = !ok;   // (nobody here yet, or a full queue: again at the next record pass)
}
static void send_ctl(int type, const Ent *e, uint32_t epoch, uint64_t who) {
    CtlMsg m = {e->inc, e->serial, epoch, who};
    coop_ctr_send(0, type, COOP_CTR_RELIABLE, &m, sizeof m);
}
static void set_runner(Ent *e, uint64_t who, uint32_t epoch) {   // (reg_lock held)
    if (epoch <= e->epoch && e->runner) return;
    int was = runs(e);
    e->runner = who, e->epoch = epoch;
    memset(e->hus, 0, sizeof e->hus), e->clock_off_at = 0;   // (another sender's clock)
    if (e->fs) memset(e->fs, 0, sizeof *e->fs);
    if (runs(e) && !was) e->seen_us = frame_step_world_us(), e->gone_us = 0, e->payer = 0;
    // A new runner whose parts are an older version than one heard of (the old runner's last, not applied here yet):
    // its parts' record goes out under a number above all of them, or the games holding that one would keep it for good
    // (review 1 finding 39).
    if (runs(e) && !was && e->v && e->vseen > e->v) e->v = e->vseen + 1, e->resend = 1;
    if (runs(e) && !was) e->unloaded = 0;   // (parts that went here before are no longer its parts: not an unload now)
    rt_log("coop: contraption: %08x is run by %s now (change %u)", cid32(e), runs(e) ? "this game" : "another player's game", epoch);
}

// ---- The runner: records and versions (27.3 R1, R4, R5) ---------------------------------------------------------------
// Every 2 s, from actor phase 0: each group among this game's own objects and the builds it runs is recorded with the
// history writer and compared with the entry it belongs to.
static _Atomic int64_t record_soon_until;
void coop_ctn_structure_event(void) {   // a bond made or broken in this game (coop_life.c, any thread)
    atomic_store(&record_soon_until, frame_step_world_us() + 3000000);
}
static uint32_t mint_serial(void) {
    static uint32_t serial;
    return ++serial;
}
static int origin_of(uint64_t a, uint32_t *id, uint64_t *oinc) {   // (reg_lock held) where a registered part came from
    for (int i = 0; i < REG_N; i++)
        for (int r = 0; reg[i].used && r < reg[i].np; r++)
            if (reg[i].parts[r] == a) { *id = reg[i].ids[r], *oinc = reg[i].oinc[r]; return 1; }
    return 0;
}
static void record_pass(Cpu *c) {
    static int64_t last;
    int64_t now = frame_step_world_us();
    uint64_t me = coop_ctr_me();
    static int off = -1;   // (TOTK_COOP_TEST_NOBPSEND=1: nothing recorded, a test)
    if (off < 0) off = cfg_get("TOTK_COOP_TEST_NOBPSEND") != NULL;
    // (every 2 s, and every 0.1 s for 3 s after a bond was made or broken here: coop_ctn_structure_event)
    if (off || !me || now - last < (now < atomic_load(&record_soon_until) ? 100000 : 2000000)) return;
    last = now;
    static uint64_t cand[256 + REG_N * 21];
    uint64_t groups[32];
    int n = 0, ng = 0;
    {
        uint64_t own[256];
        int no = coop_own_sent_objects(own, 256);
        for (int i = 0; i < no; i++) if (!coop_is_copy(own[i]) && !coop_contraption_made(own[i])) cand[n++] = own[i];
        // (and the copies of the other games' objects that no build has: glued together here by this game's player,
        // they are a build this game mints and runs, plan 39.5)
        int coop_copies_all(uint64_t *out, int max);
        no = coop_copies_all(own, 256);
        for (int i = 0; i < no && n < 256; i++) if (!coop_ctn_part(own[i]) && !coop_contraption_made(own[i]) && part_group(own[i])) cand[n++] = own[i];
    }
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++)
        for (int r = 0; reg[i].used && runs(&reg[i]) && reg[i].local == L_LIVE && r < reg[i].np; r++) if (alive(reg[i].parts[r])) cand[n++] = reg[i].parts[r];
    plat_mutex_unlock(&reg_lock);
    // Pass 1: every group, its members, and where the registry has them from, all read before any entry changes. (Read
    // group by group, the second piece of a split found its parts already taken by the first piece's entry and lost
    // their origin numbers, review 1 finding 47.)
    static struct { uint64_t g, a, used[21], oinc[21]; uint32_t ids[21]; int known[21], nmem, skipped, best, foreign; uint64_t best_inc; uint32_t best_serial; } G[32];
    for (int i = 0; i < n; i++) {
        uint64_t a = cand[i], g = part_group(a);
        if (!g) continue;
        int seen = 0;
        for (int k = 0; k < ng; k++) seen |= groups[k] == g;
        if (seen || ng >= 32) continue;
        groups[ng] = g;
        // Its members in the order the history writer gives them (main+0x1913694): the group's members (group +0x60,
        // count +0x58) sorted by name ([actor+0x218], byte order) with swaps of unequal neighbours only, so parts of
        // one name keep the group's order (its comparator main+0x1914be0); one record part per member the writer
        // keeps. It skips a member with no SPR, with SPR byte +0x26 bit 1 set, or with SPR byte +0x2e8 bit 0 set
        // (research notes sync-v3/m2/p4/graph-groups, section 5; review 1 finding 48).
        uint64_t *used = G[ng].used;
        int nmem = 0, skipped = 0;
        memset(&G[ng], 0, sizeof G[ng]), G[ng].g = g, G[ng].a = a, G[ng].best = -1, ng++;
        {
            int32_t gcount = 0;
            uint64_t arr = coop_rd_ptr(g + 0x60);
            if (!arr || !coop_rd(g + 0x58, &gcount, 4) || gcount < 2 || gcount > 21) continue;
            for (int j = 0; j < gcount && nmem < 21; j++) {
                uint64_t b = link_actor(coop_rd_ptr(arr + 8 * (uint64_t)j));
                uint64_t spr = b ? coop_rd_ptr(coop_rd_ptr(b + 0x228) + 0x480) : 0;
                uint8_t f26 = 0, f2e8 = 0;
                if (!spr) continue;
                if ((coop_rd(spr + 0x26, &f26, 1) && (f26 & 2)) || (coop_rd(spr + 0x2e8, &f2e8, 1) && (f2e8 & 1))) { skipped++; continue; }
                used[nmem++] = b;
            }
            char key[21][0x40];
            for (int j = 0; j < nmem; j++) {
                uint64_t str = coop_rd_ptr(used[j] + 0x218);
                memset(key[j], 0, sizeof key[j]);
                if (str) coop_rd(str, key[j], sizeof key[j] - 1);
            }
            for (int j = 1; j < nmem; j++)   // (a stable sort)
                for (int q = j; q > 0 && strcmp(key[q - 1], key[q]) > 0; q--) {
                    char t[0x40];
                    uint64_t ta = used[q];
                    used[q] = used[q - 1], used[q - 1] = ta;
                    memcpy(t, key[q], sizeof t), memcpy(key[q], key[q - 1], sizeof t), memcpy(key[q - 1], t, sizeof t);
                }
        }
        G[ng - 1].nmem = nmem, G[ng - 1].skipped = skipped;
    }
    // Whose each is: an entry this game runs goes to the group holding most of its parts, so the larger piece of a split
    // keeps the build's identity (R4; a tie: the piece with its first part). A group holding a part of a build another
    // game runs is not this game's to record (R3 asks for it first).
    {
        static int shared[32][REG_N];
        plat_mutex_lock(&reg_lock);
        for (int k = 0; k < ng; k++) {
            for (int r = 0; r < G[k].nmem; r++) G[k].known[r] = origin_of(G[k].used[r], &G[k].ids[r], &G[k].oinc[r]);
            for (int q = 0; q < REG_N; q++) {
                shared[k][q] = 0;
                if (!reg[q].used || reg[q].local != L_LIVE) continue;
                for (int r = 0; r < G[k].nmem; r++)
                    for (int u = 0; u < reg[q].np; u++) shared[k][q] += G[k].used[r] == reg[q].parts[u];
                if (shared[k][q] && !runs(&reg[q])) G[k].foreign = 1;
            }
        }
        for (int q = 0; q < REG_N; q++) {
            if (!reg[q].used || reg[q].local != L_LIVE || !runs(&reg[q])) continue;
            int kb = -1;
            for (int k = 0; k < ng; k++) {
                if (G[k].foreign || !shared[k][q]) continue;
                int first = 0;
                for (int r = 0; r < G[k].nmem; r++) first |= reg[q].parts[0] && G[k].used[r] == reg[q].parts[0];
                if (kb < 0 || shared[k][q] > shared[kb][q] || (shared[k][q] == shared[kb][q] && first)) kb = k;
            }
            if (kb >= 0 && (G[kb].best < 0 || shared[kb][q] > shared[kb][G[kb].best])) G[kb].best = q;
        }
        for (int k = 0; k < ng; k++) if (G[k].best >= 0) G[k].best_inc = reg[G[k].best].inc, G[k].best_serial = reg[G[k].best].serial;
        plat_mutex_unlock(&reg_lock);
    }
    // Pass 2: each group recorded and compared with its entry
    for (int k = 0; k < ng; k++) {
        if (G[k].foreign || G[k].nmem < 2) continue;
        uint64_t a = G[k].a, *used = G[k].used, *oinc = G[k].oinc, best_inc = G[k].best_inc;
        uint32_t *ids = G[k].ids, best_serial = G[k].best_serial;
        int *known = G[k].known, nmem = G[k].nmem, best = G[k].best;
        static _Alignas(16) uint8_t link[0x40], rec[0x3000];
        static _Alignas(16) float align[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, info[12];
        memset(link, 0, sizeof link), memset(rec, 0, sizeof rec), memset(link + 0x10, 0xff, 4), link[0x14] = 1;
        // (a bond end whose part the writer skipped keeps the byte the buffer had: 0xff marks it, and that bond is
        // dropped below; a skipped part's bonds are still written, graph-groups section 6)
        memset(rec + 0x1704, 0xff, 21 * 12);
        coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, (uint64_t)(uintptr_t)link, a, 1, 0);
        uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + HISTORY_MGR_SLOT));
        if (!(call5(c, MOD_MAIN_BASE + HISTORY_WRITE, mgr, (uint64_t)(uintptr_t)link, (uint64_t)(uintptr_t)align, (uint64_t)(uintptr_t)rec,
                    (uint64_t)(uintptr_t)info) & 1) || rec[9] < 2) continue;
        // (the writer leaves the header to its caller: magic and version 2, as the Autobuild's loader checks them)
        memcpy(rec, "CmbAct", 6), rec[6] = 0, rec[7] = 0, rec[8] = 2;
        {
            int nb = rec[10] < 21 ? rec[10] : 21, kept = 0;
            for (int i = 0; i < nb; i++) {
                const uint8_t *b = rec + 0x1704 + i * 12;
                if (b[8] >= rec[9] || b[9] >= rec[9]) continue;
                if (kept != i) memmove(rec + 0x1704 + kept * 12, b, 12);
                kept++;
            }
            memset(rec + 0x1704 + kept * 12, 0, (size_t)(21 - kept) * 12);
            if (kept != rec[10] || G[k].skipped) {
                static int64_t said_skip;
                if (now - said_skip > 10000000) said_skip = now, rt_log("coop: contraption: a group of %d members: %d the game does not record, %d of its %u bonds to them left out", nmem + G[k].skipped, G[k].skipped, rec[10] - kept, rec[10]);
            }
            rec[10] = (uint8_t)kept;
        }
        int bad = nmem != rec[9];
        for (int r = 0; r < rec[9] && r < 21 && !bad; r++) {
            char want[0x40] = "", nm[0x40] = "";
            memcpy(want, rec + 0xc + r * 0x118 + 0x98, sizeof want - 1);
            coop_actor_name(used[r], nm, sizeof nm);
            if (want[0] && strcmp(nm, want)) { bad = 1; break; }
            if (known[r]) continue;
            int owner = 0;
            if (coop_contraption_made(used[r])) continue;   // (made here from a record whose entry is gone: no origin)
            if (!coop_obj_ident(used[r], &owner, &ids[r])) { bad = 2; break; }   // (the other games could not name it)
            oinc[r] = owner == coop_net_id() || !owner ? me : coop_ctr_incarnation(owner);
        }
        if (bad) {
            static int64_t said_miss;
            if (now - said_miss > 10000000)
                said_miss = now, rt_log("coop: contraption: a group of %d members, record of %u parts: %s: not sent", nmem, rec[9],
                                        bad == 2 ? "a part is no object the other games know" : "the count or a name does not line up");
            continue;
        }
        plat_mutex_lock(&reg_lock);
        Ent *e = best >= 0 ? ent_find(best_inc, best_serial) : NULL;
        if (best >= 0 && (!e || !runs(e) || e->local != L_LIVE)) { plat_mutex_unlock(&reg_lock); continue; }   // (it changed hands meanwhile)
        if (!e) {   // a new build: minted here, run here
            e = ent_new(me, mint_serial());
            if (!e) { plat_mutex_unlock(&reg_lock); continue; }
            e->runner = me, e->epoch = 1, e->local = L_LIVE, e->original = 1;
            for (int r = 0; r < nmem; r++) e->original &= !coop_contraption_made(used[r]);
        }
        // A part of it gone here with no death asked: the engine is unloading the build part by part (each part at its
        // own radius from this game's player). Recorded, each smaller group would be a new version, the other games'
        // copies would lose those parts and, at the end, the whole build. It is offered to them instead, unchanged; a
        // game that still has it runs it from then on (review 1 finding 36, plan 41.1).
        {
            uint32_t unl = e->unloaded;
            for (int r = 0; r < e->np && r < 21; r++)
                if (e->parts[r] && !alive_as(e->parts[r], e->gen[r]) && !died_lately(e->parts[r], now)) unl |= 1u << r;
            e->unloaded = unl;
            if (unl) {
                if (now - e->asked_us > 1000000) {
                    e->asked_us = now, send_ctl(COOP_CTR_RELINQUISH, e, e->epoch, me);
                    rt_log("coop: contraption: %08x, run here, is being unloaded here (parts %#x gone): offered to the games that still have it", cid32(e), unl);
                }
                e->seen_us = now, e->gone_us = 0;
                plat_mutex_unlock(&reg_lock);
                continue;
            }
        }
        uint32_t shape = bp_shape(rec), green = 0;
        for (int r = 0; r < nmem; r++) green |= (uint32_t)part_green(used[r]) << r;
        int same = e->v && shape == e->shape && e->np == nmem && green == e->green;
        for (int r = 0; same && r < nmem; r++) same = e->parts[r] == used[r];
        if (!same) {
            // parts that left it and are in no group now become this game's own loose objects (R4)
            for (int r = 0; r < e->np; r++) {
                int kept = 0;
                for (int u = 0; u < nmem; u++) kept |= used[u] == e->parts[r];
                if (!kept && !part_group(e->parts[r])) made_forget(e->parts[r]);
            }
            e->v = (e->v > e->vseen ? e->v : e->vseen) + 1, e->shape = shape, e->green = green, e->np = nmem, e->resend = 1;
            memcpy(e->rec, rec, BP_SIZE), memcpy(e->parts, used, sizeof e->parts), memcpy(e->ids, ids, sizeof e->ids), memcpy(e->oinc, oinc, sizeof e->oinc);
            e->del_ok = 0, e->unloaded = 0, e->adopt = 0;
            for (int r = 0; r < nmem; r++) if (!coop_contraption_made(used[r])) e->adopt |= 1u << r;   // (not made here: never deleted here)
            for (int r = 0; r < 21; r++) e->gen[r] = gen_of(e->parts[r]);
            rt_log("coop: contraption: %08x, run here: version %u (%u parts, %u bonds, shape %08x)", cid32(e), e->v, rec[9], rec[10], shape);
        }
        e->centre[0] = -info[3], e->centre[1] = -info[7], e->centre[2] = -info[11];   // (info is [I | -centre])
        e->seen_us = now, e->gone_us = 0;
        plat_mutex_unlock(&reg_lock);
    }
    // versions out: changed ones, and all of them again when a player joined
    {
        uint64_t inc;
        int id, joined, again = 0;
        while (coop_ctr_peer_event(&inc, &id, &joined)) again |= joined;
        plat_mutex_lock(&reg_lock);
        for (int i = 0; i < REG_N; i++) {
            Ent *e = &reg[i];
            if (!e->used || !runs(e) || e->local != L_LIVE || !e->v) continue;
            if (again) e->resend = 1;
            if (e->resend && e->seen_us == now) send_version(e);
            // A build this game runs that is no group any more: with parts still alive it broke apart, and the other
            // games' copies go (RETIRE); with none readable it was destroyed or unloaded, and the entry only lapses here.
            if (e->seen_us != now) {
                if (!e->gone_us) e->gone_us = now;
                int live_parts = 0, grouped = 0;
                for (int r = 0; r < e->np; r++) live_parts += alive(e->parts[r]), grouped += alive(e->parts[r]) && part_group(e->parts[r]);
                for (int r = 0; r < e->np && r < 21; r++)
                    if (e->parts[r] && !alive_as(e->parts[r], e->gen[r]) && !died_lately(e->parts[r], now)) e->unloaded |= 1u << r;
                if (grouped) e->gone_us = now;   // (still a build: the writer refused it this time, or it is changing)
                if (now - e->gone_us > 600000) {   // (0.6 s: a regrouping takes a frame or two)
                    // Parts of it went with no death asked: the engine unloaded them (each part deletes itself beyond its
                    // own display radius from this game's player, 60 to 130 m for most parts: research notes
                    // sync-v3/m2/p8/unload-travel). A game that still has the build is told it may run it now (plan
                    // 10.3). Else it broke apart here (parts alive, no group) or was destroyed (every part died, the
                    // deaths were sent): the other games' copies go (RETIRE), and none of them makes it again
                    // (review 1 finding 37).
                    int unl = e->unloaded != 0;
                    if (unl) send_ctl(COOP_CTR_RELINQUISH, e, e->epoch, me);
                    else send_ctl(COOP_CTR_RETIRE, e, e->epoch, me);
                    for (int r = 0; live_parts && r < e->np; r++) if (!part_group(e->parts[r])) made_forget(e->parts[r]);
                    rt_log("coop: contraption: %08x, run here, is no build any more (%d of %d parts alive): %s", cid32(e), live_parts, e->np,
                           unl ? "unloaded here, offered to the games that still have it" : live_parts ? "broken apart, retired" : "destroyed, retired");
                    free(e->fs), memset(e, 0, sizeof *e);
                }
            }
        }
        plat_mutex_unlock(&reg_lock);
    }
}

// ---- A follower: a version applied in place (plan 33.3) -------------------------------------------------------------------
// When every part of an incoming version is an actor this game already has in the registry (the same build with a bond
// more or less, a build split in two, two builds joined), nothing is deleted and made again: the glue edges the new
// record does not have are broken as the game breaks one, the bonds it has and this game lacks are made as an Autobuild
// confirm makes them, and the entry takes the actors. A part is known across versions and games by where it began
// (ids, oinc). Read from the game: research notes sync-v3/m2/p4/bond-make and bond-break.
//   edges of a body: main+0x191327c(list) then main+0x27c1c0(body, list); list +0 count, +8 cells, a cell's first word
//     the edge; an edge is live when main+0x2993218 says so and +0x10 bit 2 (destroy asked) is clear, glue when byte
//     +0xec or +0xed is set; its ends main+0x2993f98 / 0x2994078; a body's owner object at +0x110, its ActorLink +0x18
//   break: main+0x1cf54d4(actor +0x378, edge, 0, 0) posts the game's own break message to the owner of the edge's end A
//   make: a descriptor of the build object's pool (main+0x1d3ccf0 on object +0x3c88) with the two bodies' graph ids
//     (main+0xa2d39c, result through x8), their poses in one space and the pivot main+0x299daec gives, pushed on the
//     first part's SPR (main+0x1c07604); the part's own update makes the bond in a frame where SPR +0x27c bit 0 is set
//   a body by slot: main+0x44bba4([[[actor +0x228] +0x50] +0x20], slot). The record's bond names bodies 0 to 2 and
//     holds each part's three body poses relative to the build's root (CmbActPart.frame), so the relative pose of a
//     bond is inverse(frame A) * frame B. That the record's body number is this slot number for bodies 1 and 2 is the
//     reading's "likely", not shown; for body 0 (the main body, nearly every bond) both are the main body.
static uint64_t call_x8(Cpu *c, uint64_t fn, uint64_t x8, uint64_t a0) {   // (rt_call sets x0..x7 and leaves x8)
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    c->x[8] = x8;
    uint64_t r = rt_call(c, fn, 1, a0);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}
static void m34_mul(float out[12], const float a[12], const float b[12]) {
    float t[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        t[i * 4 + 3] = a[i * 4 + 3] + a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11];
    }
    memcpy(out, t, sizeof t);
}
static int m34_inv(float out[12], const float m[12]) {
    float c0 = m[5] * m[10] - m[9] * m[6], c1 = m[8] * m[6] - m[4] * m[10], c2 = m[4] * m[9] - m[8] * m[5];
    float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (det == 0.0f || det != det) return 0;
    float k = 1.0f / det, r[9];
    r[0] = c0 * k, r[1] = (m[2] * m[9] - m[1] * m[10]) * k, r[2] = (m[1] * m[6] - m[2] * m[5]) * k;
    r[3] = c1 * k, r[4] = (m[0] * m[10] - m[2] * m[8]) * k, r[5] = (m[2] * m[4] - m[0] * m[6]) * k;
    r[6] = c2 * k, r[7] = (m[1] * m[8] - m[0] * m[9]) * k, r[8] = (m[0] * m[5] - m[1] * m[4]) * k;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i * 4 + j] = r[i * 3 + j];
        out[i * 4 + 3] = -(r[i * 3] * m[3] + r[i * 3 + 1] * m[7] + r[i * 3 + 2] * m[11]);
    }
    return 1;
}
static uint64_t phys_sys(uint64_t a) { return a ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) : 0; }
static uint64_t body_slot(Cpu *c, uint64_t a, uint32_t slot) {
    uint64_t sys = phys_sys(a);
    return sys ? coop_guest_call(c, MOD_MAIN_BASE + 0x44bba4, sys, slot, 0, 0) : 0;
}
typedef struct { uint64_t edge, other; int part, slot, oslot, ed; } EdgeAt;   // ed: glue by +0xed alone (broken with withEd 1)
static int part_edges(Cpu *c, uint64_t actor, int part, EdgeAt *out, int max) {
    static _Alignas(16) uint8_t list[0x440];
    uint64_t L = (uint64_t)(uintptr_t)list, body0 = body_slot(c, actor, 0);
    int n = 0;
    for (uint32_t slot = 0; slot < 3; slot++) {
        uint64_t body = slot ? body_slot(c, actor, slot) : body0;
        if (!body || (slot && body == body0)) continue;
        coop_guest_call(c, MOD_MAIN_BASE + 0x191327c, L, 0, 0, 0);
        coop_guest_call(c, MOD_MAIN_BASE + 0x27c1c0, body, L, 0, 0);
        int32_t cnt = 0;
        memcpy(&cnt, list, 4);
        uint64_t cells = coop_rd_ptr(L + 8);
        for (int i = 0; cells && i < cnt && i < 0x40 && n < max; i++) {
            uint64_t cell = coop_rd_ptr(cells + 8 * (uint64_t)i), edge = cell ? coop_rd_ptr(cell) : 0;
            uint8_t g = 0, g2 = 0;
            uint32_t fl = 0;
            if (!edge || !(coop_guest_call(c, MOD_MAIN_BASE + 0x2993218, edge, 0, 0, 0) & 1)) continue;
            if (!coop_rd(edge + 0xec, &g, 1) || !coop_rd(edge + 0xed, &g2, 1) || (!g && !g2) || !coop_rd(edge + 0x10, &fl, 4) || (fl & 4)) continue;
            uint64_t ea = coop_guest_call(c, MOD_MAIN_BASE + 0x2993f98, edge, 0, 0, 0), eb = coop_guest_call(c, MOD_MAIN_BASE + 0x2994078, edge, 0, 0, 0);
            if (ea != body && eb != body) continue;
            uint64_t ob = ea == body ? eb : ea, owner = ob ? coop_rd_ptr(ob + 0x110) : 0, oa = owner ? link_actor(owner + 0x18) : 0;
            if (!oa) continue;
            int os = -1;
            for (uint32_t k = 0; k < 3 && os < 0; k++) if (body_slot(c, oa, k) == ob) os = (int)k;
            out[n++] = (EdgeAt){edge, oa, part, (int)slot, os, !g && g2};
        }
    }
    return n;
}
// One bond queued on a's SPR between a's body sa and b's body sb, b held at rel from a. 0: refused (why says where).
static int bond_queue(Cpu *c, uint64_t o, uint64_t a, uint32_t sa, uint64_t b, uint32_t sb, const float rel[12], int *why) {
    static _Alignas(16) float wa[12], wb[12], pivot[12];
    static _Alignas(16) int32_t idout;
    uint64_t ta = coop_rd_ptr(a + 0x228), spr = ta ? coop_rd_ptr(ta + 0x480) : 0, c288 = ta ? coop_rd_ptr(ta + 0x288) : 0, sys = phys_sys(a);
    uint64_t mgr = sys ? coop_rd_ptr(sys + 0xd8) : 0;
    *why = 1;
    if (!spr || !c288 || !sys || !coop_rd_ptr(sys + 0x150) || !mgr || !coop_rd_ptr(mgr + 0x18) || !coop_rd_ptr(coop_rd_ptr(b + 0x228) + 0x480)) return 0;
    uint64_t ba = body_slot(c, a, sa), bb = body_slot(c, b, sb), fa = 0, fb = 0;
    *why = 2;
    if (!ba || !bb || ba == bb) return 0;
    *why = 3;   // (a body torn down once keeps bit 24 of +0x68 and has no graph id: the game would drop the bond silently)
    if (!coop_rd(ba + 0x68, &fa, 8) || !coop_rd(bb + 0x68, &fb, 8) || ((fa | fb) >> 24 & 1)) return 0;
    int32_t ida, idb;
    idout = -1, call_x8(c, MOD_MAIN_BASE + 0xa2d39c, (uint64_t)(uintptr_t)&idout, ba), ida = idout;
    idout = -1, call_x8(c, MOD_MAIN_BASE + 0xa2d39c, (uint64_t)(uintptr_t)&idout, bb), idb = idout;
    *why = 4;
    if (ida == -1 || idb == -1) return 0;
    *why = 5;
    if (!coop_rd(ba + 0x98, wa, 48)) return 0;
    m34_mul(wb, wa, rel);
    static const float id12[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(pivot, id12, sizeof id12);
    call5(c, MOD_MAIN_BASE + 0x299daec, (uint64_t)(uintptr_t)pivot, ba, bb, (uint64_t)(uintptr_t)wa, (uint64_t)(uintptr_t)wb);
    uint64_t d = coop_guest_call(c, MOD_MAIN_BASE + 0x1d3ccf0, o + 0x3c88, 0, 0, 0);
    *why = 7;
    if (!d) return 0;
    ST32(d + 0x28, (uint32_t)ida), ST32(d + 0x2c, (uint32_t)idb);
    memcpy((void *)(uintptr_t)(d + 0x30), wa, 48), memcpy((void *)(uintptr_t)(d + 0x60), wb, 48), memcpy((void *)(uintptr_t)(d + 0x90), pivot, 48);
    coop_guest_call(c, MOD_MAIN_BASE + 0x1c07604, spr, d, 0, 0);
    __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x27c), 1u, __ATOMIC_SEQ_CST);
    *why = 0;
    return 1;
}
// The build object's 21 bond descriptors back on its free list, as the confirm does first; 0 while one is still queued
static int bond_pool_recycle(Cpu *c, uint64_t o) {
    uint32_t count = 0;
    coop_rd(o + 0x3c88, &count, 4);
    uint64_t arr = coop_rd_ptr(o + 0x3c90), freeh = coop_rd_ptr(o + 0x3c98);
    for (uint32_t i = 0; i < count && i < 21; i++) {
        uint64_t d = coop_rd_ptr(arr + 8ull * i), owner = 1;
        if (!d || !coop_rd(d + 0x20, &owner, 8) || owner) return 0;
    }
    for (uint32_t i = 0; i < count && i < 21; i++) {
        uint64_t d = coop_rd_ptr(arr + 8ull * i);
        coop_guest_call(c, MOD_MAIN_BASE + 0x1d43e84, d, 0, 0, 0);
        ST64(d, freeh), freeh = d;
    }
    ST64(o + 0x3c98, freeh), ST32(o + 0x3c88, 0);
    return 1;
}
// Parts an in-place change took out of their build and no entry has taken since: another entry's version (the other
// piece of a split) takes them within a second, or they are deleted (a part loose where the build runs comes back as
// that game's object).
#define ORPHAN_N 32
static struct { uint64_t actor, oinc; uint32_t gen, id; int adopt; int64_t us; } orphans[ORPHAN_N];
static void orphan_add(uint64_t a, uint32_t gen, uint32_t id, uint64_t oinc, int64_t now, int adopt) {   // (reg_lock held)
    for (int i = 0; a && i < ORPHAN_N; i++)
        if (!orphans[i].actor) { orphans[i].actor = a, orphans[i].gen = gen, orphans[i].id = id, orphans[i].oinc = oinc, orphans[i].us = now, orphans[i].adopt = adopt; return; }
}
static void orphans_pass(Cpu *c, int64_t now) {
    for (int i = 0; i < ORPHAN_N; i++) {
        plat_mutex_lock(&reg_lock);
        uint64_t a = orphans[i].actor && now - orphans[i].us > 1000000 ? orphans[i].actor : 0;
        uint32_t gen = orphans[i].gen;
        int adopted = orphans[i].adopt;
        if (a) orphans[i].actor = 0;
        plat_mutex_unlock(&reg_lock);
        if (a && (adopted || !coop_contraption_made(a))) continue;   // (an object of the mirror's or of this game's: it is a plain object again)
        if (a && alive_as(a, gen) && !part_delete(c, a)) {   // (refused now: asked again)
            plat_mutex_lock(&reg_lock);
            orphan_add(a, gen, 0, 0, now - 900000, 0);
            plat_mutex_unlock(&reg_lock);
        }
    }
}
// 1: e's incoming version applied in place; 0: not possible (made with the build object instead); -1: later
static int in_place(Cpu *c, Ent *e, uint64_t o, int64_t now) {
    static uint8_t r[BP_SIZE];
    static EdgeAt ed[21 * 12];
    uint32_t ids[21];
    uint64_t oinc[21], act[21] = {0}, breaks[64];
    int breaks_ed[64];
    uint32_t gens[21] = {0};
    int n, nb, ne = 0, nbreak = 0, have[21] = {0}, made = 0;
    uint32_t in_v, in_shape, in_green;   // (the version read here is the one committed: another may come meanwhile, finding 3)
    float in_centre[3];
    plat_mutex_lock(&reg_lock);
    memcpy(r, e->in_rec, BP_SIZE), memcpy(ids, e->in_ids, sizeof ids), memcpy(oinc, e->in_oinc, sizeof oinc);
    in_v = e->in_v, in_shape = e->in_shape, in_green = e->in_green, memcpy(in_centre, e->in_centre, sizeof in_centre);
    n = r[9], nb = r[10];
    int ok = n >= 2 && n <= 21 && nb >= 1 && nb <= 21 && !memcmp(r, "CmbAct", 6);
    for (int j = 0; ok && j < n; j++) {
        for (int q = 0; ids[j] && q < REG_N && !act[j]; q++)
            for (int u = 0; reg[q].used && reg[q].local == L_LIVE && u < reg[q].np && !act[j]; u++)
                if (reg[q].parts[u] && reg[q].ids[u] == ids[j] && reg[q].oinc[u] == oinc[j]) act[j] = reg[q].parts[u], gens[j] = reg[q].gen[u];
        for (int q = 0; ids[j] && q < ORPHAN_N && !act[j]; q++)
            if (orphans[q].actor && orphans[q].id == ids[j] && orphans[q].oinc == oinc[j]) act[j] = orphans[q].actor, gens[j] = orphans[q].gen;
        if (act[j] && !alive_as(act[j], gens[j])) act[j] = 0;
    }
    uint32_t in_adopt = 0;
    for (int j = 0; j < n; j++)   // (whether the registry's actor of it was itself taken from the objects)
        for (int q = 0; act[j] && q < REG_N; q++)
            for (int u = 0; reg[q].used && u < reg[q].np; u++) if (reg[q].parts[u] == act[j] && (reg[q].adopt >> u & 1)) in_adopt |= 1u << j;
    for (int q = 0; q < ORPHAN_N; q++)
        for (int j = 0; j < n; j++) if (orphans[q].actor && orphans[q].actor == act[j] && orphans[q].adopt) in_adopt |= 1u << j;
    plat_mutex_unlock(&reg_lock);
    // A part no entry has: the object it began as, here. The mirror copy of another game's object, or this game's own
    // object when it began here (another player glued it into a build). It is taken where it stands.
    for (int j = 0; ok && j < n; j++) {
        if (act[j] || !ids[j]) continue;
        char want[0x40] = "";
        memcpy(want, r + 0xc + j * 0x118 + 0x98, sizeof want - 1);
        int owner = oinc[j] == coop_ctr_me() ? coop_net_id() : coop_ctr_route(oinc[j]);
        uint64_t a = owner ? coop_obj_find(owner, ids[j], want) : 0;
        if (alive(a) && !coop_contraption_made(a)) act[j] = a, gens[j] = gen_of(a), in_adopt |= 1u << j;
    }
    for (int j = 0; ok && j < n; j++) {
        ok = act[j] != 0;
        for (int q = 0; ok && q < j; q++) ok = act[q] != act[j];
    }
    for (int j = 0; ok && j < n; j++) {
        char want[0x40] = "", nm[0x40] = "";
        memcpy(want, r + 0xc + j * 0x118 + 0x98, sizeof want - 1);
        coop_actor_name(act[j], nm, sizeof nm);
        ok = !want[0] || !strcmp(nm, want);
    }
    if (!ok) return 0;
    // the glue edges on the parts now: each is one of the record's bonds, or is broken
    for (int j = 0; j < n; j++) ne += part_edges(c, act[j], j, ed + ne, (int)(sizeof ed / sizeof ed[0]) - ne);
    for (int x = 0; x < ne; x++) {
        int k = -1, bond = -1;
        for (int j = 0; j < n; j++) if (act[j] == ed[x].other) k = j;
        for (int i = 0; k >= 0 && ed[x].oslot >= 0 && i < nb && bond < 0; i++) {
            const uint8_t *b = r + 0x1704 + i * 12;
            uint32_t ba, bb;
            memcpy(&ba, b, 4), memcpy(&bb, b + 4, 4);
            if ((b[8] == ed[x].part && (int)ba == ed[x].slot && b[9] == k && (int)bb == ed[x].oslot) ||
                (b[9] == ed[x].part && (int)bb == ed[x].slot && b[8] == k && (int)ba == ed[x].oslot)) bond = i;
        }
        if (bond >= 0) { have[bond] = 1; continue; }
        int seen = 0;
        for (int q = 0; q < nbreak; q++) seen |= breaks[q] == ed[x].edge;
        if (!seen && nbreak < 64) breaks_ed[nbreak] = ed[x].ed, breaks[nbreak++] = ed[x].edge;
    }
    int missing = 0;
    for (int i = 0; i < nb; i++) missing += !have[i];
    if (missing && !bond_pool_recycle(c, o)) return -1;   // (bonds of the last change still queued)
    for (int i = 0; i < nb; i++) {
        if (have[i]) continue;
        const uint8_t *b = r + 0x1704 + i * 12;
        uint32_t ba, bb;
        float fa[12], fb[12], inv[12], rel[12];
        int why = 0;
        memcpy(&ba, b, 4), memcpy(&bb, b + 4, 4);
        if (b[8] >= n || b[9] >= n || ba > 2 || bb > 2) return 0;
        memcpy(fa, r + 0xc + b[8] * 0x118 + ba * 0x30, 48), memcpy(fb, r + 0xc + b[9] * 0x118 + bb * 0x30, 48);
        if (!m34_inv(inv, fa)) return 0;
        m34_mul(rel, inv, fb);
        if (!bond_queue(c, o, act[b[8]], ba, act[b[9]], bb, rel, &why)) {
            rt_log("coop: contraption: %08x version %u: a bond could not be made in place (step %d, parts %u and %u): made again instead", cid32(e), e->in_v, why, b[8], b[9]);
            return 0;
        }
        made++;
    }
    for (int q = 0; q < nbreak; q++) {
        uint64_t ea = coop_guest_call(c, MOD_MAIN_BASE + 0x2993f98, breaks[q], 0, 0, 0), owner = ea ? coop_rd_ptr(ea + 0x110) : 0;
        uint64_t qa = owner ? link_actor(owner + 0x18) : 0;
        if (qa) coop_guest_call(c, MOD_MAIN_BASE + 0x1cf54d4, qa + 0x378, breaks[q], 0, (uint64_t)breaks_ed[q]);   // (withEd for a +0xed edge: bond-break, finding 49)
    }
    // the entry takes the actors; what it had and the version lacks waits for another entry (orphans)
    plat_mutex_lock(&reg_lock);
    for (int u = 0; u < e->np; u++) {
        int kept = 0;
        for (int j = 0; j < n; j++) kept |= e->parts[u] == act[j];
        if (!kept && e->parts[u]) orphan_add(e->parts[u], e->gen[u], e->ids[u], e->oinc[u], now, e->adopt >> u & 1);
    }
    for (int q = 0; q < REG_N; q++)
        for (int u = 0; reg[q].used && &reg[q] != e && u < reg[q].np; u++)
            for (int j = 0; j < n; j++) if (reg[q].parts[u] == act[j]) reg[q].parts[u] = 0;
    for (int q = 0; q < ORPHAN_N; q++)
        for (int j = 0; j < n; j++) if (orphans[q].actor == act[j]) orphans[q].actor = 0;
    memset(e->parts, 0, sizeof e->parts), memset(e->gen, 0, sizeof e->gen);
    memcpy(e->parts, act, sizeof(uint64_t) * (size_t)n), memcpy(e->gen, gens, sizeof(uint32_t) * (size_t)n);
    memcpy(e->rec, r, BP_SIZE), memcpy(e->ids, ids, sizeof e->ids), memcpy(e->oinc, oinc, sizeof e->oinc), memcpy(e->centre, in_centre, sizeof e->centre);
    e->np = n, e->v = in_v, e->shape = in_shape, e->green = in_green, e->local = L_LIVE, e->original = 0, e->del_ok = 0;
    if (e->in_v == in_v) e->want_make = 0;   // (a newer version, or a RETIRE, that came meanwhile stays to be done)
    e->absent = 0, e->old_us = 0, e->changed_us = 0, e->remade_us = now, e->adopt = in_adopt, e->unloaded = 0;
    plat_mutex_unlock(&reg_lock);
    rt_log("coop: contraption: %08x version %u applied in place: %d parts kept (%#x taken from the objects), %d bonds made, %d broken", cid32(e), e->v, n, in_adopt, made, nbreak);
    return 1;
}

// ---- A follower: a version made with the Autobuild build object (plan 7) ------------------------------------------------
// One at a time, over several frames, from actor phase 0: the part maker main+0x1d3b470, the parts put in the world
// just before the confirm main+0x1d3c0d4 (every cost record at 0), the confirm's mark and the green at the confirm.
static struct {
    Ent *e;
    int st;
    int64_t t;
    uint64_t parts[21];
    int n;
    uint32_t v, shape, green, ids[21];   // the version being made, taken from the entry when the making starts
    uint64_t oinc[21];
    float centre[3];
    uint8_t rec[BP_SIZE];
} mk;
// The game that ran e no longer has it: this game runs it, if two or more of its parts are here (reg_lock held)
static void take_over(Ent *e, uint32_t epoch) {
    int here = 0;
    for (int r = 0; r < e->np; r++) here += alive_as(e->parts[r], e->gen[r]);
    if (runs(e) || e->local != L_LIVE || here < 2) return;
    uint32_t ep = (epoch > e->epoch ? epoch : e->epoch) + 1;
    rt_log("coop: contraption: %08x: the game that ran it no longer has it (unloaded there): run here now", cid32(e));
    set_runner(e, coop_ctr_me(), ep), send_ctl(COOP_CTR_GRANT, e, ep, coop_ctr_me()), e->resend = 1;
}
static void make_pass(Cpu *c) {
    int64_t now = frame_step_world_us();
    static int own_obj = -1;   // (TOTK_COOP_COPY_OBJ=menu: the Autobuild menu's thumbnail object instead, a test)
    if (own_obj < 0) { const char *e = cfg_get("TOTK_COOP_COPY_OBJ"); own_obj = !(e && !strcmp(e, "menu")); }
    uint64_t ab = coop_life_ability(), o = ab ? coop_rd_ptr(ab + (own_obj ? 0x51f0 : 0x51f8)) : 0;
    if (!o) return;
    uint64_t list = coop_rd_ptr(o + 0x1b8);
    int32_t ne = 0;
    coop_rd(o + 0x1b0, &ne, 4);
    if (mk.st == 0) {   // the next version waiting: remakes first, so a build that split is gone before its pieces come
        uint32_t selected = 0;
        coop_rd(ab + 0x51e8, &selected, 4);
        if (selected & 0xff) return;   // (this game's player has an Autobuild design selected: the build object is theirs)
        Ent *e = NULL;
        plat_mutex_lock(&reg_lock);
        // (no build waits behind another: one whose old part the game will not delete yet steps aside for 0.3 s)
        for (int i = 0; i < REG_N && !e; i++) if (reg[i].used && reg[i].want_make && reg[i].np && reg[i].stuck_until <= now) e = &reg[i];
        for (int i = 0; i < REG_N && !e; i++) if (reg[i].used && reg[i].want_make && reg[i].absent != 1 && reg[i].stuck_until <= now) e = &reg[i];   // (not where the engine unloads it)
        plat_mutex_unlock(&reg_lock);
        if (!e) return;
        if (e->in_v != 0xffffffffu && in_place(c, e, o, now)) return;   // (applied where the parts stand, or to be tried again)
        {   // Not while this game's Link stands where the copy goes: parts made inside it are pushed apart before they
            // bond. The old copy, if any, stays and follows until the Link has moved off (plan 7, step 11).
            const uint8_t *rr = e->in_rec;
            float reach = 0, lp[3] = {0};
            for (int i = 0; i < rr[9] && i < 21; i++) {
                float f[12];
                memcpy(f, rr + 0xc + i * 0x118, sizeof f);
                float d = sqrtf(f[3] * f[3] + f[7] * f[7] + f[11] * f[11]);
                if (d > reach) reach = d;
            }
            uint64_t me = coop_player_actor();
            if (me && coop_rd(me + COOP_ACTOR_POS, lp, 12)) {
                float dx = lp[0] - e->in_centre[0], dy = lp[1] - e->in_centre[1], dz = lp[2] - e->in_centre[2];
                if (sqrtf(dx * dx + dy * dy + dz * dz) < reach + 1.5f) {
                    if (!e->waited_us || now - e->waited_us > 5000000)
                        rt_log("coop: contraption: %08x: waiting for this Link to move off where it goes", cid32(e));
                    if (!e->waited_us) e->waited_us = now;
                    return;
                }
            }
            e->waited_us = 0;
        }
        if (e->np) {   // the parts of the version held: deleted; the new one made once they have left
            if (!parts_delete(c, e)) {   // (a delete lock held on a part: asked again next frame, nothing made meanwhile)
                static int64_t said;
                if (now - said > 5000000) said = now, rt_log("coop: contraption: %08x: the game refused the delete of a part here (asked %#x of %d): asked again", cid32(e), e->del_ok, e->np);
                e->stuck_until = now + 300000;
                return;
            }
            plat_mutex_lock(&reg_lock);
            rt_log("coop: contraption: %08x changed shape (version %u): its %d parts here deleted, made again once gone", cid32(e), e->in_v, e->np);
            memcpy(e->old, e->parts, sizeof e->old), e->old_us = now;
            e->np = 0, e->local = L_NONE, e->original = 0, e->v = 0, e->del_ok = 0, e->adopt = 0, e->unloaded = 0, memset(e->parts, 0, sizeof e->parts);
            plat_mutex_unlock(&reg_lock);
            return;
        }
        if (e->old_us) {   // (one version per world: the old parts gone, or 3 s, before the new are made)
            int left = 0;
            for (int i = 0; i < 21; i++) left += alive(e->old[i]);
            if (left && now - e->old_us < 3000000) return;
            e->old_us = 0, memset(e->old, 0, sizeof e->old);
        }
        plat_mutex_lock(&reg_lock);
        memcpy(mk.rec, e->in_rec, BP_SIZE), memcpy(mk.centre, e->in_centre, sizeof mk.centre);
        memcpy(mk.ids, e->in_ids, sizeof mk.ids), memcpy(mk.oinc, e->in_oinc, sizeof mk.oinc);
        mk.v = e->in_v, mk.shape = e->in_shape, mk.green = e->in_green, e->want_make = 0;
        plat_mutex_unlock(&reg_lock);
        const uint8_t *r = mk.rec;
        int n = r[9];
        if (n < 2 || n > 21 || memcmp(r, "CmbAct", 6)) {
            rt_log("coop: contraption: %08x: the record is not a CmbAct (%u parts)", cid32(e), n);
            return;
        }
        if (!coop_rd_ptr(o + 0x3c50)) ST64(o + 0x3c50, o + 0xc50);
        uint64_t cai = coop_rd_ptr(o + 0x3c50);
        memcpy((void *)(uintptr_t)cai, r, BP_SIZE);
        memcpy(coop_ab_cai, r, BP_SIZE);
        float m[12] = {1, 0, 0, mk.centre[0], 0, 1, 0, mk.centre[1], 0, 0, 1, mk.centre[2]};
        memcpy((void *)(uintptr_t)(o + 0x120), m, sizeof m);
        // the part pool emptied and refilled from the record, as the ability does before its part maker
        uint64_t pc = o + 0x4d10, pu = o + 0x4f48;
        uint32_t ctr = LD32(pc), used = LD32(pu);
        ST64(pc + 0x220, 0), ST32(pc, ++ctr);
        if ((int32_t)used >= 1) {
            uint64_t head = LD64(o + 0x4f58);
            for (uint32_t i = 0; i < used; i++) {
                uint64_t ent = LD64(LD64(o + 0x4f50) + 8 * (uint64_t)i);
                ST64(ent, head), ST64(o + 0x4f58, ent), head = ent;
            }
        }
        ST32(pu, 0);
        if (ctr == 0x7fffffff) ST32(pc, 0);
        for (int i = 0; i < n; i++) {
            uint64_t en = coop_guest_call(c, MOD_MAIN_BASE + 0x1d3cfc8, o + 0x1b0, 0, 0, 0);
            if (!en) continue;
            uint64_t rr = cai + 0xc + (uint64_t)i * 0x118;
            ST8(rr + 0xd7, 0), ST64(en, rr + 0x98), ST8(rr + 0x117, 0), ST64(en + 8, rr + 0xd8);
            ST32(en + 0x10, LD32(rr + 0x90)), ST32(en + 0x14, LD32(rr + 0x94));
        }
        if (!own_obj) ST8(o + 0x3c80, 0);   // (real parts, not thumbnails: put back after the confirm)
        coop_guest_call(c, MOD_MAIN_BASE + 0x1d3b470, o, 0, 0, 0);
        plat_mutex_lock(&reg_lock);
        e->local = L_MAKING;
        plat_mutex_unlock(&reg_lock);
        mk.e = e, mk.st = 1, mk.t = now, mk.n = 0;
        rt_log("coop: contraption: %08x version %u (%d parts): being made here at %.2f %.2f %.2f", cid32(e), mk.v, n, mk.centre[0], mk.centre[1], mk.centre[2]);
        return;
    }
    Ent *e = mk.e;
    if (mk.st == 1) {   // every part made: put in the world now, confirmed next frame
        int ready = 1;
        for (int i = 0; list && i < ne && i < 21; i++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)i);
            if (!ent || coop_handle_status((const uint8_t *)(uintptr_t)(ent + 0x60)) != 3) ready = 0;
        }
        if (!ready) {
            if (now - mk.t > 6000000) {
                rt_log("coop: contraption: %08x: its parts were not made in 6 s", cid32(e));
                plat_mutex_lock(&reg_lock);
                e->local = L_NONE, mk.st = 0;
                plat_mutex_unlock(&reg_lock);
                if (!own_obj) ST8(o + 0x3c80, 1);
            }
            return;
        }
        static _Alignas(16) uint8_t tmp[21][0x40];
        for (int i = 0; list && i < ne && i < 21; i++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)i), ref = 0;
            mk.parts[i] = coop_claim(c, (uint8_t *)(uintptr_t)(ent + 0x60), tmp[i], &ref);
            made_add(mk.parts[i]);
        }
        mk.n = ne < 21 ? ne : 21, mk.st = 2;
        plat_mutex_lock(&reg_lock);   // (the entry's parts from now on: held where the runner's is while being made)
        memcpy(e->parts, mk.parts, sizeof e->parts), e->np = mk.n, e->del_ok = 0, e->absent = 0, e->adopt = 0, e->unloaded = 0;
        for (int i = 0; i < 21; i++) e->gen[i] = gen_of(e->parts[i]);
        memcpy(e->rec, mk.rec, BP_SIZE), memcpy(e->centre, mk.centre, sizeof e->centre);
        memcpy(e->ids, mk.ids, sizeof e->ids), memcpy(e->oinc, mk.oinc, sizeof e->oinc);
        e->v = mk.v, e->shape = mk.shape, e->green = mk.green, e->original = 0;
        plat_mutex_unlock(&reg_lock);
        return;
    }
    if (mk.st == 2) {   // the confirm, as the ability makes it, with no cost
        uint64_t tab = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395f900)) + 8);
        uint64_t costs_at[21] = {0};
        uint32_t costs_were[21] = {0};
        int ncost = 0;
        static uint64_t keyname;
        for (int i = 0; i < mk.n; i++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)i);
            if (!ent || !mk.parts[i]) continue;
            uint8_t *l = (uint8_t *)(uintptr_t)(ent + 0x48);
            memset(l + 0x10, 0xff, 4), l[0x14] = 1;
            coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, ent + 0x48, mk.parts[i], 1, 0);
            ST32(ent + 0x1c, 3);
            keyname = coop_rd_ptr(ent);
            uint64_t rec = tab ? coop_guest_call(c, MOD_MAIN_BASE + 0xae6e04, tab, (uint64_t)(uintptr_t)&keyname, 0, 0) : 0;
            int seen = 0;
            for (int j = 0; j < ncost; j++) seen |= costs_at[j] == rec;
            if (rec && !seen) costs_at[ncost] = rec, costs_were[ncost] = LD32(rec + 0x48), ST32(rec + 0x48, 0), ncost++;
        }
        ST8(o + 0x3c70, 0);
        memcpy((void *)(uintptr_t)coop_rd_ptr(o + 0x3c50), mk.rec, BP_SIZE);
        coop_life_free_confirm(1);   // (its Zonaite charge skipped too: zeroed costs alone still go through the item routine)
        uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + 0x1d3c0d4, o, 0, 0, 0);
        coop_life_free_confirm(0);
        if (!own_obj) ST8(o + 0x3c80, 1);   // (the menu's thumbnail object again)
        for (int j = 0; j < ncost; j++) ST32(costs_at[j] + 0x48, costs_were[j]);
        for (int i = 0; i < mk.n; i++) {
            uint64_t spr = mk.parts[i] ? coop_rd_ptr(coop_rd_ptr(mk.parts[i] + 0x228) + 0x480) : 0;
            if (!spr) continue;
            __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x27c), 1u, __ATOMIC_SEQ_CST);
            ST64(spr + 0x20, (LD64(spr + 0x20) & ~(1ull << 51)) | (1ull << 28));
            __atomic_fetch_and((uint32_t *)(uintptr_t)(mk.parts[i] + 0x5ac), ~4u, __ATOMIC_SEQ_CST);
            // Green as where the build runs: the game gives the mark (SPR +0x150 bit 0, set only by main+0x1c07114) to a
            // part its confirm made from zonaite, and to none it took from the world or a player glued by hand (plan 31,
            // note 1). At the confirm, as the game's own confirm gives it. A game rule's death of a copy part is refused
            // by G4 (coop_life.c) whether the part is green or not.
            if (mk.green >> i & 1) __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x150), 1u, __ATOMIC_SEQ_CST);
        }
        rt_log("coop: contraption: %08x: confirmed (%llu)", cid32(e), (unsigned long long)(r & 1));
        mk.st = 3, mk.t = now;
        return;
    }
    if (mk.st == 3) {   // done after 3 s
        int grouped = 0;
        for (int i = 0; i < mk.n; i++) {
            uint64_t spr = mk.parts[i] ? coop_rd_ptr(coop_rd_ptr(mk.parts[i] + 0x228) + 0x480) : 0;
            if (spr && coop_rd_ptr(spr + 0x28)) grouped++;
        }
        if ((grouped == mk.n && now - mk.t > 200000) || now - mk.t > 1000000) {
            rt_log("coop: contraption: %08x: made, %d of %d parts in one group", cid32(e), grouped, mk.n);
            plat_mutex_lock(&reg_lock);
            if (e->local == L_MAKING) e->local = L_LIVE;
            e->changed_us = 0, e->remade_us = now;
            if (e->relinq_later) e->relinq_later = 0, take_over(e, e->epoch);
            plat_mutex_unlock(&reg_lock);
            mk.st = 0, mk.n = 0, mk.e = NULL;
        }
    }
}
static void dev_flush(Cpu *c);
static void events_put(void);
static void deaths_put(Cpu *c);
static int gauge_blocked(void);
static void floats_follow(Cpu *c);
static void burning_phase0(Cpu *c);
static void springs_phase0(Cpu *c);
void coop_ctn_phase0(Cpu *c) {
    if (!ctn_on()) return;
    record_pass(c);
    make_pass(c);
    events_put();
    deaths_put(c);
    dev_flush(c);
    floats_follow(c);
    springs_phase0(c);
    burning_phase0(c);
}

// ---- The rigid-field follower drive (plan section 3) ------------------------------------------------------------------
// A follower's copy is moved as one rigid body: each frame one linear and one angular velocity (V, W) are taken from
// the runner's root, predicted to the runner's present, plus a bounded correction toward it; every part's main body
// gets v_j = V + W x (x_j - x_root) and w_j = W, from where the parts are in this game, so the bonds between them see
// no relative motion. Gravity: what a step takes off the written vertical velocity is measured and added back. Rest: a
// copy whose runner rests and which sits where it should is not written, so its bodies may sleep.
static __thread int sync_writing;   // the drive's own body writes run with this set (G1 lets them through)
static void mat_mul3(const float a[9], const float b[9], float o[9]) {
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++) o[r * 3 + q] = a[r * 3] * b[q] + a[r * 3 + 1] * b[3 + q] + a[r * 3 + 2] * b[6 + q];
}
static void rot_of(const float m[12], float r[9]) {   // the 3x3 of a 3x4 row-major body matrix
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i * 3 + j] = m[i * 4 + j];
}
// The rotation vector (axis * angle) of R, valid up to 180 degrees.
static void log_rot(const float r[9], float w[3]) {
    float tr = r[0] + r[4] + r[8], c = (tr - 1) * 0.5f;
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    float ang = acosf(c);
    float v[3] = {r[7] - r[5], r[2] - r[6], r[3] - r[1]}, sn = sinf(ang);
    if (ang < 1e-4f) { w[0] = v[0] * 0.5f, w[1] = v[1] * 0.5f, w[2] = v[2] * 0.5f; return; }
    if (sn > 1e-3f) { float k = ang / (2 * sn); w[0] = v[0] * k, w[1] = v[1] * k, w[2] = v[2] * k; return; }
    // near 180 degrees: the axis from the diagonal
    float ax[3] = {sqrtf(fmaxf(0, (r[0] + 1) * 0.5f)), sqrtf(fmaxf(0, (r[4] + 1) * 0.5f)), sqrtf(fmaxf(0, (r[8] + 1) * 0.5f))};
    if (r[1] < 0) ax[1] = -ax[1];
    if (r[2] < 0) ax[2] = -ax[2];
    w[0] = ax[0] * ang, w[1] = ax[1] * ang, w[2] = ax[2] * ang;
}
static void exp_rot(const float w[3], float r[9]) {   // Rodrigues
    float a = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (a < 1e-6f) { float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(r, id, sizeof id); return; }
    float k[3] = {w[0] / a, w[1] / a, w[2] / a}, c = cosf(a), sn = sinf(a), t = 1 - c;
    r[0] = c + k[0] * k[0] * t, r[1] = k[0] * k[1] * t - k[2] * sn, r[2] = k[0] * k[2] * t + k[1] * sn;
    r[3] = k[1] * k[0] * t + k[2] * sn, r[4] = c + k[1] * k[1] * t, r[5] = k[1] * k[2] * t - k[0] * sn;
    r[6] = k[2] * k[0] * t - k[1] * sn, r[7] = k[2] * k[1] * t + k[0] * sn, r[8] = c + k[2] * k[2] * t;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void clamp_vec(float v[3], float max) {
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > max && n > 0) for (int i = 0; i < 3; i++) v[i] *= max / n;
}
static int follow_rigid(Cpu *c, Ent *e, int64_t now, float *err_out) {
    float hm[3][12];
    int64_t hus[3], off;
    uint64_t parts[21];
    int np;
    uint32_t key;
    FollowState *fs;
    plat_mutex_lock(&reg_lock);
    memcpy(hm, e->hm, sizeof hm), memcpy(hus, e->hus, sizeof hus), off = e->clock_off;
    np = e->np < 21 ? e->np : 21, key = cid32(e);
    memcpy(parts, e->parts, sizeof parts);
    if (!e->fs) e->fs = calloc(1, sizeof(FollowState));
    fs = e->fs;
    plat_mutex_unlock(&reg_lock);
    if (!fs || !hus[2] || !np) return 0;
    // the sample's age by the arrival offset's minimum (the jitter is taken out, the base delay is not: plan 3.2)
    double age = (now - (hus[2] + off)) / 1e6;
    double tau = clampf((float)age, 0, 0.25f);
    // the runner's velocity, acceleration and turn rate from its last samples
    float v_r[3] = {0}, a_r[3] = {0}, w_r[3] = {0};
    double d1 = hus[1] && hus[2] > hus[1] ? (hus[2] - hus[1]) / 1e6 : 0, d0 = hus[0] && hus[1] > hus[0] ? (hus[1] - hus[0]) / 1e6 : 0;
    if (d1 > 0.005 && d1 < 0.5) {
        for (int i = 0; i < 3; i++) v_r[i] = (hm[2][i * 4 + 3] - hm[1][i * 4 + 3]) / (float)d1;
        float r1[9], r2[9], r1t[9], dr[9];
        rot_of(hm[1], r1), rot_of(hm[2], r2);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r1t[i * 3 + j] = r1[j * 3 + i];
        mat_mul3(r2, r1t, dr), log_rot(dr, w_r);
        for (int i = 0; i < 3; i++) w_r[i] /= (float)d1;
        if (d0 > 0.005 && d0 < 0.5)
            for (int i = 0; i < 3; i++) {
                float v0 = (hm[1][i * 4 + 3] - hm[0][i * 4 + 3]) / (float)d0;
                a_r[i] = (v_r[i] - v0) / (float)(0.5 * (d0 + d1));
            }
        clamp_vec(a_r, 2 * 29.0f);   // (2 g at this game's gravity, X1)
    }
    // the prediction: where the runner's root is now
    float p_star[3], v_star[3], r_star[9], r_now[9];
    for (int i = 0; i < 3; i++) p_star[i] = hm[2][i * 4 + 3] + v_r[i] * (float)tau + 0.5f * a_r[i] * (float)(tau * tau), v_star[i] = v_r[i] + a_r[i] * (float)tau;
    {
        float wt[3] = {w_r[0] * (float)tau, w_r[1] * (float)tau, w_r[2] * (float)tau}, dr[9];
        exp_rot(wt, dr), rot_of(hm[2], r_now), mat_mul3(dr, r_now, r_star);
    }
    // where the copy is: its root body
    uint64_t rb = main_body_of(parts[0]);
    float root[12];
    if (!rb || !coop_rd(rb + 0x98, root, sizeof root)) return 0;
    float e_p[3] = {p_star[0] - root[3], p_star[1] - root[7], p_star[2] - root[11]}, rf[9], rft[9], re[9], e_r[3];
    rot_of(root, rf);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rft[i * 3 + j] = rf[j * 3 + i];
    mat_mul3(r_star, rft, re), log_rot(re, e_r);
    float ep = sqrtf(e_p[0] * e_p[0] + e_p[1] * e_p[1] + e_p[2] * e_p[2]), er = sqrtf(e_r[0] * e_r[0] + e_r[1] * e_r[1] + e_r[2] * e_r[2]);
    *err_out = ep;
    double dt = fs->last_us && now > fs->last_us ? (now - fs->last_us) / 1e6 : 1 / 60.0;
    if (dt > 0.1) dt = 0.1;
    fs->last_us = now;
    // the step observer: what the last physics step took off the velocity written (gravity, the bodies' damping,
    // contacts), the parts' mean, on each axis; added back so the copy moves at the velocity it is given
    if (fs->have_written) {
        float sum[3] = {0};
        int n = 0;
        for (int j = 0; j < np; j++) {
            uint64_t b = main_body_of(parts[j]);
            float vb[3];
            if (b && coop_rd(b + 0xf8, vb, sizeof vb)) for (int i = 0; i < 3; i++) sum[i] += vb[i] - fs->v_written[j][i], n += i == 0;
        }
        for (int i = 0; n && i < 3; i++) {
            float d = clampf(sum[i] / n, -1.5f, i == 1 ? 0.0f : 1.5f);   // (m/s a frame; gravity never pushes up)
            fs->comp[i] += (-d - fs->comp[i]) * 0.2f;                     // (about 5 frames)
        }
        fs->gc = fs->comp[1];
    }
    // sideways the step's loss is the bodies' air damping only while the build moves; on the ground at rest it is
    // friction, and adding that back pushed the copy to and fro
    float vn_now = sqrtf(v_r[0] * v_r[0] + v_r[2] * v_r[2]);
    if (vn_now < 0.5f) fs->comp[0] *= 0.8f, fs->comp[2] *= 0.8f;
    // rest: the runner still for 0.5 s and the copy where it should be: nothing written, bodies may sleep
    float vn = sqrtf(v_r[0] * v_r[0] + v_r[1] * v_r[1] + v_r[2] * v_r[2]), wn = sqrtf(w_r[0] * w_r[0] + w_r[1] * w_r[1] + w_r[2] * w_r[2]);
    if (vn < 0.05f && wn < 0.05f) { if (!fs->still_since) fs->still_since = hus[2]; }
    else fs->still_since = 0;
    int runner_rests = fs->still_since && hus[2] - fs->still_since >= 500000;
    const float deg = 180.0f / 3.14159265f;
    // (a wide band: a copy resting on the ground a few centimetres from where the runner's rests cannot be pushed
    // there through its contacts, and trying makes it chatter; plan RK2)
    if (fs->rest && (!runner_rests || ep > 0.25f || er * deg > 8)) fs->rest = 0;
    else if (!fs->rest && runner_rests && ep < 0.10f && er * deg < 4) fs->rest = 1;
    if (fs->rest) { fs->have_written = 0; for (int i = 0; i < 3; i++) fs->integ[i] *= (float)(1 - dt); return 0; }
    // the field: V, W with bounded corrections (gains fall with speed)
    float kp = vn <= 5 ? 5 : vn >= 15 ? 2.5f : 5 - 2.5f * (vn - 5) / 10, kr = wn <= 2 ? 5 : wn >= 6 ? 3 : 5 - 2 * (wn - 2) / 4;
    float cv[3], cw[3];
    for (int i = 0; i < 3; i++) {
        fs->integ[i] = clampf(fs->integ[i] + 3.0f * e_p[i] * (float)dt, -1.0f, 1.0f);
        cv[i] = kp * e_p[i] + fs->integ[i], cw[i] = kr * e_r[i];
    }
    clamp_vec(cv, 3.0f), clamp_vec(cw, 2.5f);
    _Alignas(16) float V[4] = {v_star[0] + cv[0] + fs->comp[0], v_star[1] + cv[1] + fs->comp[1], v_star[2] + cv[2] + fs->comp[2], 0}, W[4] = {w_r[0] + cw[0], w_r[1] + cw[1], w_r[2] + cw[2], 0};
    // The setters skip a value equal to the body's stored one (unless body flag 44/45), so a copy held still by the same
    // velocity each frame fell asleep and stayed where it dozed off, 27 cm from its place. A dither of 2e-4 m/s
    // (rad/s), alternating each frame, makes every write a change and keeps the bodies awake.
    static int parity;
    float dz = (parity ^= 1) ? 2e-4f : -2e-4f;
    W[0] += dz, W[1] += dz, W[2] += dz;
    for (int j = 0; j < np; j++) {
        uint64_t b = main_body_of(parts[j]);
        float m[12];
        if (!b || !coop_rd(b + 0x98, m, sizeof m)) continue;
        float d[3] = {m[3] - root[3], m[7] - root[7], m[11] - root[11]};
        _Alignas(16) float vj[4] = {V[0] + W[1] * d[2] - W[2] * d[1] + dz, V[1] + W[2] * d[0] - W[0] * d[2] + dz, V[2] + W[0] * d[1] - W[1] * d[0] + dz, 0};
        sync_writing = 1;
        coop_guest_call(c, MOD_MAIN_BASE + 0x567750, b, (uint64_t)(uintptr_t)vj, 0, 0);   // SetLinearVelocity
        coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, b, (uint64_t)(uintptr_t)W, 0, 0);    // SetAngularVelocity
        sync_writing = 0;
        fs->v_written[j][0] = vj[0], fs->v_written[j][1] = vj[1], fs->v_written[j][2] = vj[2];
    }
    fs->have_written = 1;
    static int64_t said;
    if (now - said > 500000) {
        said = now;
        float vb[3] = {0}, f68 = 0;
        uint64_t flags = 0;
        coop_rd(rb + 0xf8, vb, sizeof vb), coop_rd(rb + 0x68, &flags, 8);
        uint64_t in_world = coop_rd_ptr(rb + 0x70), req = coop_rd_ptr(rb + 0x60);
        (void)f68;
        rt_log("coop: follow: %.2f s copy %08x: error %.3f m (%.3f %.3f %.3f) %.1f deg, age %.3f s, v* %.2f %.2f %.2f, V %.2f %.2f %.2f, gc %.3f, "
               "I %.2f %.2f %.2f; root y %.3f, its velocity %.2f %.2f %.2f, flags %#llx, +0x70 %llx, request %llx, target y %.3f",
               now / 1e6, key, ep, e_p[0], e_p[1], e_p[2], er * deg, age, v_star[0], v_star[1], v_star[2], V[0], V[1], V[2], fs->gc,
               fs->integ[0], fs->integ[1], fs->integ[2], root[7], vb[0], vb[1], vb[2], (unsigned long long)flags, (unsigned long long)in_world, (unsigned long long)req, p_star[1]);
    }
    return 1;
}


// ---- Gates and lists read from game threads ----------------------------------------------------------------------------
#define DRIVEN_N 96
static _Atomic uint64_t driven[DRIVEN_N];        // the parts of builds another game runs (not touched here)
static _Atomic uint64_t driven_body[DRIVEN_N];   // their main bodies (G1)
static _Atomic uint64_t nocharge[DRIVEN_N];      // the parts whose devices another player pays for
int coop_ctn_driven(uint64_t a) {
    for (int i = 0; a && i < DRIVEN_N; i++) {
        uint64_t d = atomic_load_explicit(&driven[i], memory_order_relaxed);
        if (!d) return 0;
        if (d == a) return 1;
    }
    return 0;
}
int coop_ctn_no_charge(uint64_t a) {
    for (int i = 0; a && i < DRIVEN_N; i++)
        if (atomic_load_explicit(&nocharge[i], memory_order_relaxed) == a) return 1;
    return 0;
}
// A part of a build this game does not run, or one still being made (G3 and G4: it takes no game rule's death)
// Every part of every entry here, for reads from other threads and files (the object mirror: coop_players.c)
static _Atomic uint64_t part_list[REG_N * 21];
int coop_ctn_part(uint64_t a) {
    for (int i = 0; a && i < REG_N * 21; i++) {
        uint64_t p = atomic_load_explicit(&part_list[i], memory_order_relaxed);
        if (!p) return 0;
        if (p == a) return 1;
    }
    return 0;
}
int coop_contraption_follower(uint64_t a) {
    if (!a || (!coop_contraption_made(a) && !coop_ctn_part(a))) return 0;
    int yes = 0;
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N && !yes; i++) {
        if (!reg[i].used || (runs(&reg[i]) && reg[i].local == L_LIVE)) continue;
        for (int r = 0; r < reg[i].np && !yes; r++) yes = reg[i].parts[r] == a;
    }
    for (int i = 0; i < mk.n && !yes; i++) yes = mk.parts[i] == a && mk.st;
    plat_mutex_unlock(&reg_lock);
    return yes;
}
// G1 (contraption-sync-v3.md section 4): a core body of a part another game runs is written only by the drive here.
// Every other write to it is dropped: the velocity setters main+0x567750, 0x25a1f0, 0x40e490, the impulses
// main+0x44be18, 0x29ae114, 0x29ae7a8, the force writes main+0x29ae7fc, 0x29a4afc and SetTransform main+0x25a450, so
// devices, hits and pushes move a contraption only in the game that runs it. Passed: the drive's own writes
// (sync_writing), the spring's two launches (from main+0x221a9bc and 0x221aa10: the Link on a follower's spring is
// launched natively, plan G1), and a part this game's player holds with Ultrahand (SPR +0x20 bit 16; until P3's touch
// words, so a grab on another player's contraption moves it here while the hand-over is under way).
// TOTK_COOP_G1=0: off. Counted per function for the clean line and X5.
static _Atomic uint32_t g1_dropped[10];
static const uint64_t g1_fns[10] = {0x567750, 0x25a1f0, 0x40e490, 0x44be18, 0x29ae114, 0x29ae7a8, 0x29ae7fc, 0x29a4afc, 0x25a450, 0};
int coop_g1(Cpu *c, uint64_t off) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_G1"); on = ctn_on() && !(e && *e == '0'); }
    if (!on || sync_writing) return 0;
    uint64_t body = c->x[0];
    int k = -1, hit = 0;
    for (int i = 0; i < 9 && k < 0; i++) if (g1_fns[i] == off) k = i;
    if (k < 0 || !body) return 0;
    for (int i = 0; i < DRIVEN_N && !hit; i++) {
        uint64_t b = atomic_load_explicit(&driven_body[i], memory_order_relaxed);
        if (!b) break;
        if (b == body) hit = i + 1;
    }
    if (!hit) return 0;
    uint64_t lr = c->x[30] - MOD_MAIN_BASE;
    if (lr == 0x221a9c0 || lr == 0x221aa14) return 0;   // (the spring's launches: returns after main+0x221a9bc / 0x221aa10)
    uint64_t actor = atomic_load_explicit(&driven[hit - 1], memory_order_relaxed);
    uint64_t spr = actor ? coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x480) : 0, f20 = 0;
    if (spr && coop_rd(spr + 0x20, &f20, 8) && (f20 >> 16 & 1)) return 0;   // (held here)
    atomic_fetch_add(&g1_dropped[k], 1);
    {   // each (function, caller) the first time, with the caller's caller: which game code writes to followers
        static struct { uint64_t off, lr; } seen[48];
        static PlatMutex sl = PLAT_MUTEX_INIT;
        int fresh = 0;
        plat_mutex_lock(&sl);
        for (int i = 0; i < 48; i++) {
            if (seen[i].off == off && seen[i].lr == lr) break;
            if (!seen[i].off) { seen[i].off = off, seen[i].lr = lr, fresh = 1; break; }
        }
        plat_mutex_unlock(&sl);
        if (fresh) {
            uint64_t fp = c->x[29], lr1 = fp ? coop_rd_ptr(fp + 8) : 0, fp2 = fp ? coop_rd_ptr(fp) : 0, lr2 = fp2 ? coop_rd_ptr(fp2 + 8) : 0;
            char nm[48];
            rt_log("coop: g1: main+%#llx on a follower's body (%s) from main+%#llx <- %#llx <- %#llx: dropped", (unsigned long long)off,
                   coop_actor_name(actor, nm, sizeof nm), (unsigned long long)lr, (unsigned long long)(lr1 ? lr1 - MOD_MAIN_BASE : 0),
                   (unsigned long long)(lr2 ? lr2 - MOD_MAIN_BASE : 0));
        }
    }
    c->x[0] = 0;
    return 1;
}
void coop_g1_counts(uint32_t out[9]) { for (int i = 0; i < 9; i++) out[i] = atomic_load(&g1_dropped[i]); }
// Where a part came from, by the numbers of the game whose object it was, both ways: claims, glue and hit messages that
// name that game's object find the part made here (coop_players.c coop_obj_find / coop_obj_ident / coop_is_copy)
int coop_contraption_ident(uint64_t a, int *owner, uint32_t *id) {
    if (!a || !ctn_on()) return 0;
    int got = 0;
    uint64_t me = coop_ctr_me();
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N && !got; i++)
        for (int r = 0; reg[i].used && !reg[i].original && r < reg[i].np && !got; r++)
            if (reg[i].parts[r] == a && reg[i].ids[r] && reg[i].oinc[r] != me && coop_ctr_route(reg[i].oinc[r]))
                *owner = coop_ctr_route(reg[i].oinc[r]), *id = reg[i].ids[r], got = 1;
    plat_mutex_unlock(&reg_lock);
    return got;
}
uint64_t coop_contraption_find(int owner, uint32_t id) {
    if (!id || !ctn_on()) return 0;
    uint64_t a = 0, oinc = coop_ctr_incarnation(owner);
    plat_mutex_lock(&reg_lock);
    for (int i = 0; oinc && i < REG_N && !a; i++)
        for (int r = 0; reg[i].used && !reg[i].original && r < reg[i].np && !a; r++)
            if (reg[i].ids[r] == id && reg[i].oinc[r] == oinc) a = reg[i].parts[r];
    plat_mutex_unlock(&reg_lock);
    return alive(a) ? a : 0;
}
// 1: player from's object id is a part of a build this game makes with its Autobuild (its loose copy is not wanted)
int coop_contraption_owns(int from, uint32_t id) {
    if (!ctn_on() || !id) return 0;
    int yes = 0;
    uint64_t oinc = coop_ctr_incarnation(from);
    plat_mutex_lock(&reg_lock);
    for (int i = 0; oinc && i < REG_N && !yes; i++) {
        if (!reg[i].used || reg[i].original) continue;
        for (int r = 0; r < 21 && !yes; r++)
            yes = reg[i].v && reg[i].ids[r] == id && reg[i].oinc[r] == oinc && !(reg[i].adopt >> r & 1);
    }
    for (int r = 0; oinc && mk.st && r < 21 && !yes; r++) yes = mk.ids[r] == id && mk.oinc[r] == oinc;   // (being made now)
    plat_mutex_unlock(&reg_lock);
    return yes;
}

// ---- Devices (plan 8.1, 8.2) -------------------------------------------------------------------------------------------
// A device is switched by the message its on/off node takes as "set on" / "set off" (0x6b510000 / 0x6b510001: the
// handler main+0x245c114 sets bit 0 / bit 1 of the node's event byte, and the node's calc turns its on latch on only
// when it is off, off only when it is on, so the same message again changes nothing). It is queued as the build's own
// sender main+0x32eb90 queues it: main+0x2b76c34(queue, {target key, id, 0}, 0), the queue an actor's +0x378 and the
// key the device actor's +0x380. A DeviceSwitch request pair does not switch a device (the node never reads it), and
// the toggle bit a hit sets flips it instead of setting it. Read from the game: research notes sync-v3/onoff/.
// The on/off node of each device, noted when its calc runs (x0 = node; coop_autobuild.c): the game keeps no path
// from an actor to its nodes (research notes sync-v3/m2/p5/engine-access, Q1). A node is used only while it still
// names the actor.
#define NODE_N 256
static struct { _Atomic uint64_t actor, node; } onoff_nodes[NODE_N];
void coop_ctn_note_onoff(uint64_t node, uint64_t actor) {
    if (!node || !actor) return;
    for (int p = 0; p < 4; p++) {
        int k = (int)((actor >> 4) + (uint64_t)p) % NODE_N;
        uint64_t have = atomic_load_explicit(&onoff_nodes[k].actor, memory_order_relaxed);
        if (have == actor) { atomic_store_explicit(&onoff_nodes[k].node, node, memory_order_relaxed); return; }
        if (!have || p == 3) { atomic_store(&onoff_nodes[k].node, node), atomic_store(&onoff_nodes[k].actor, actor); return; }
    }
}
static uint64_t onoff_node(uint64_t actor) {
    for (int p = 0; actor && p < 4; p++) {
        int k = (int)((actor >> 4) + (uint64_t)p) % NODE_N;
        if (atomic_load(&onoff_nodes[k].actor) != actor) continue;
        uint64_t node = atomic_load(&onoff_nodes[k].node);
        return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(node + 8) + 8) + 0x1f8) == actor ? node : 0;
    }
    return 0;
}
// A time bomb's fuse node (ExecuteSpObjTimerBomb, calc main+0x2474478), noted the same way (coop_life.c)
static struct { _Atomic uint64_t actor, node; } fuse_nodes[32];
void coop_ctn_note_fuse(uint64_t node, uint64_t actor) {
    if (!node || !actor) return;
    for (int k = 0; k < 32; k++) if (atomic_load_explicit(&fuse_nodes[k].actor, memory_order_relaxed) == actor) { atomic_store(&fuse_nodes[k].node, node); return; }
    for (int k = 0; k < 32; k++) {
        uint64_t have = atomic_load(&fuse_nodes[k].actor), n = atomic_load(&fuse_nodes[k].node);
        if (have && coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(n + 8) + 8) + 0x1f8) == have && alive(have)) continue;   // (still that bomb's)
        atomic_store(&fuse_nodes[k].node, node), atomic_store(&fuse_nodes[k].actor, actor);
        return;
    }
}
static uint64_t fuse_node(uint64_t actor) {
    for (int k = 0; actor && k < 32; k++) {
        if (atomic_load(&fuse_nodes[k].actor) != actor) continue;
        uint64_t node = atomic_load(&fuse_nodes[k].node);
        return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(node + 8) + 8) + 0x1f8) == actor ? node : 0;
    }
    return 0;
}
// A device's switch event byte: byte +8 of the blackboard entry SpecialPartsParam (0xc9efe622; the entries are the
// section at +0x918, their pointers at +0x920). Bit 0 and bit 2 ask on, bit 1 off; the on/off node takes the byte at
// the start of its calc and clears it. It is what a jump or fall attack writes on the device it hits (main+0x1b640f4,
// from the attacker's side: set on, never a flip), with no toggle request at all, which is why such an attack by a
// player on another game's build did nothing (plan 31, note 2). Research notes sync-v3/m2/p6/jump-attack.
// On a part of a build another game runs the byte is taken off before the node here reads it and sent to the game
// that runs the build, which puts it on its own part at phase 0: the node there does with it what it does with its own
// player's. A hit that also asked a toggle (a plain swing) is sent as the toggle only.
#define BB_ENTRIES 0x918
#define BB_SWITCH_EVENT 0xc9efe622u
static uint64_t event_entry(uint64_t actor) {
    uint64_t bb = actor_bb(actor);
    int i = bb_index(bb, BB_ENTRIES, BB_SWITCH_EVENT);
    return i >= 0 ? coop_rd_ptr(coop_rd_ptr(bb + BB_ENTRIES + 8) + 8 * (uint64_t)i) : 0;
}
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static struct { uint64_t actor; uint8_t bits; int64_t us; } ev_seen[16], ev_put[16];
static struct { uint64_t actor; int64_t us; } tog_seen[16];
static int ev_seen_n, ev_put_n, tog_i;
void coop_ctn_event_seen(uint64_t actor) {   // from the on/off calc of a followed part (a game thread), before the calc
    uint64_t en = event_entry(actor);
    uint8_t b = 0;
    if (!en || !coop_rd(en + 8, &b, 1) || !(b & 7)) return;
    __atomic_fetch_and((uint8_t *)(uintptr_t)(en + 8), (uint8_t)~7, __ATOMIC_SEQ_CST);
    plat_mutex_lock(&ev_lock);
    if (ev_seen_n < 16) ev_seen[ev_seen_n].actor = actor, ev_seen[ev_seen_n].bits = b & 7, ev_seen[ev_seen_n].us = frame_step_world_us(), ev_seen_n++;
    plat_mutex_unlock(&ev_lock);
}
typedef struct { uint64_t inc; uint32_t serial, part, bits, v; } EvMsg;   // (v: the version part is a number of)
static void events_out(void) {   // the co-op tick: events seen 50 ms ago or more, sent unless the same hit asked a toggle
    int64_t now = frame_step_world_us();
    for (;;) {
        uint64_t actor = 0;
        uint8_t bits = 0;
        int toggled = 0;
        plat_mutex_lock(&ev_lock);
        for (int i = 0; i < ev_seen_n && !actor; i++) {
            if (now - ev_seen[i].us < 50000) continue;
            actor = ev_seen[i].actor, bits = ev_seen[i].bits;
            for (int k = 0; k < 16; k++) toggled |= tog_seen[k].actor == actor && llabs(tog_seen[k].us - ev_seen[i].us) < 300000;
            ev_seen[i] = ev_seen[--ev_seen_n];
        }
        plat_mutex_unlock(&ev_lock);
        if (!actor) return;
        if (toggled) continue;
        EvMsg m = {0};
        int found = 0;
        uint32_t id = 0;
        plat_mutex_lock(&reg_lock);
        for (int i = 0; i < REG_N && !found; i++)
            for (int r = 0; reg[i].used && !runs(&reg[i]) && r < reg[i].np && !found; r++)
                if (reg[i].parts[r] == actor) m.inc = reg[i].inc, m.serial = reg[i].serial, m.part = (uint32_t)r, m.v = reg[i].v, id = cid32(&reg[i]), found = 1;
        plat_mutex_unlock(&reg_lock);
        if (!found) { lone_ask(actor, 0, bits); continue; }
        m.bits = bits;
        coop_ctr_send(0, COOP_CTR_EVENT, COOP_CTR_RELIABLE, &m, sizeof m);
        rt_log("coop: contraption: a switch event %#x put on %08x part %u here (a jump attack, or another set-on): sent to the game that runs it", bits, id, m.part);
    }
}
static void events_put(void) {   // actor phase 0, where the build runs: the events the other games sent, on the parts
    plat_mutex_lock(&ev_lock);
    for (int i = 0; i < ev_put_n; i++) {
        uint64_t en = alive(ev_put[i].actor) ? event_entry(ev_put[i].actor) : 0;
        if (en) __atomic_fetch_or((uint8_t *)(uintptr_t)(en + 8), ev_put[i].bits, __ATOMIC_SEQ_CST);
    }
    ev_put_n = 0;
    plat_mutex_unlock(&ev_lock);
}
static void dev_msg(Cpu *c, uint64_t actor, int on) {
    static _Alignas(16) uint8_t msg[16];
    uint32_t key = 0, id = on ? 0x6b510000u : 0x6b510001u;
    if (!actor || !coop_rd(actor + 0x380, &key, 4)) return;
    // A device in separate-message mode (node +0x98 bit 1: the spring and the time bomb) ignores the two messages.
    // Its latch is set as the handler of an accepted message sets it: bit 0 (on) or bit 1 (off) of the node's event
    // byte +0x40, which the next calc turns into the latch, off tested first (research notes
    // sync-v3/m2/p5/spring-stake, note 4; sync-v3/onoff/a_node.md). For the spring the tick the latch changes is the
    // launch. A time bomb is not set this way: on does not arm its fuse (rocket-timebomb, 2.4).
    {
        uint64_t node = onoff_node(actor), sw = part_switch(actor);
        uint8_t f98 = 0;
        int32_t kind = 0;
        if (node && coop_rd(node + 0x98, &f98, 1) && (f98 & 2)) {
            if (sw && coop_rd(sw + 0x64, &kind, 4) && kind == 0x18) return;
            __atomic_fetch_or((uint8_t *)(uintptr_t)(node + 0x40), (uint8_t)(on ? 1 : 2), __ATOMIC_SEQ_CST);
            return;
        }
    }
    memset(msg, 0, sizeof msg), memcpy(msg, &key, 4), memcpy(msg + 4, &id, 4);
    coop_guest_call(c, MOD_MAIN_BASE + 0x2b76c34, actor + 0x378, (uint64_t)(uintptr_t)msg, 0, 0);
}
// The wanted states of a build's devices are queued and all sent in one go at actor phase 0 (coop_ctn_phase0), outside
// the actors' updates. The game keeps a build's devices in one state: while some are on and some are off, its group
// sender main+0x32eb90 sends "on" to all of them (group flag bits 11 and 21, research notes sync-v3/onoff/c_group.md).
// Sent from the co-op tick, the messages of one build could land on either side of the devices' node updates, one
// fan went off a frame before the others, and the game switched them all back on.
#define SETQ 8
static struct { uint64_t parts[21]; int n; uint32_t on; char why; } setq[SETQ];
static int setq_n;
static void dev_set_later(const uint64_t *parts, int np, uint32_t on, char why) {   // (reg_lock held) on: bit r, part r wanted on
    if (!parts[0]) return;   // (its first part gone here: the build is about to be made again)
    for (int i = 0; i < setq_n; i++)
        if (setq[i].parts[0] == parts[0]) { memcpy(setq[i].parts, parts, sizeof setq[i].parts), setq[i].n = np, setq[i].on = on; return; }
    if (setq_n == SETQ) return;
    memcpy(setq[setq_n].parts, parts, sizeof setq[setq_n].parts), setq[setq_n].n = np, setq[setq_n].on = on, setq[setq_n].why = why, setq_n++;
}
static void dev_flush(Cpu *c) {   // from actor phase 0
    static struct { uint64_t parts[21]; int n; uint32_t on; char why; } q[SETQ];
    plat_mutex_lock(&reg_lock);
    int n = setq_n;
    memcpy(q, setq, sizeof q), setq_n = 0;
    plat_mutex_unlock(&reg_lock);
    for (int i = 0; i < n; i++) {
        char was[24] = "";
        int sent = 0;
        for (int r = 0; r < q[i].n && r < 21; r++) {
            int sw = part_switch(q[i].parts[r]) != 0, on = sw && part_on(q[i].parts[r]), want = (int)(q[i].on >> r & 1);
            was[r] = sw ? (on ? '1' : '0') : '-', was[r + 1] = 0;
            if (sw && on != want) dev_msg(c, q[i].parts[r], want), sent++;
        }
        if (q[i].why == 'a') rt_log("coop: contraption: devices asked %s here: they were %s, %d set", q[i].on ? "on" : "off", was, sent);
    }
}
// The devices of a build another game runs: each switched part on or off as there, compared twice a second
static void dev_follow(Cpu *c, Ent *e, int64_t now) {
    uint64_t parts[21];
    plat_mutex_lock(&reg_lock);
    uint32_t sw = e->sw, on = e->on;
    int np = e->np, due = now - e->toggled_us > 500000;
    memcpy(parts, e->parts, sizeof parts);
    plat_mutex_unlock(&reg_lock);
    {   // (every 3 s: what the follow sees, each switched part's on bool here against the runner's)
        static int64_t said;
        if (now - said > 1000000) {
            said = now;
            char here[24] = "";
            for (int r = 0; r < np && r < 21; r++) here[r] = part_switch(parts[r]) ? (part_on(parts[r]) ? '1' : '0') : '-', here[r + 1] = 0;
            rt_log("coop: contraption: devices of %08x: there sw %#x on %#x, here %s", cid32(e), sw, on, here);
        }
    }
    if (!sw || !due) return;
    // This game's player pays for the build and another game runs it: when this game's gauge can no longer pay, the
    // build is asked off where it runs, as the player's own would go off (plan 36 item 3).
    // The same when its parts here cannot run on this game's battery for another reason: the update writes "the gauge
    // can pay and the device is in range of this game's Link" into DeviceSwitch +0x8d bit 0, and the on/off node turns a
    // device off while it is clear (research notes sync-v3/m2/p5/energy, section 0.2: 20 m from the Link, the substitute
    // energy window). All of them so: asked off. Some: those are not set on here, where the node would turn them off
    // again every half second (review 1 finding 61).
    uint32_t unpowered = 0;
    if (e->payer == coop_ctr_me())
        for (int r = 0; r < np && r < 21; r++) {
            uint64_t d = (on & sw) >> r & 1 ? part_switch(parts[r]) : 0;
            uint8_t b8d = 1;
            if (d && coop_rd(d + 0x8d, &b8d, 1) && !(b8d & 1)) unpowered |= 1u << r;
        }
    if ((on & sw) && e->payer == coop_ctr_me() && (gauge_blocked() || unpowered == (on & sw))) {
        TogMsg t = {e->inc, e->serial, 0, 0, 0};
        plat_mutex_lock(&reg_lock);
        e->toggled_us = now + 500000;   // (once a second)
        plat_mutex_unlock(&reg_lock);
        coop_ctr_send(0, COOP_CTR_TOGGLE, COOP_CTR_RELIABLE, &t, sizeof t);
        rt_log("coop: contraption: %08x: this player pays and the gauge cannot: asked off where it runs", cid32(e));
        return;
    }
    on &= ~unpowered;
    int set = 0, there_on = (on & sw) != 0, here_on = 0;
    for (int r = 0; r < np; r++) here_on |= (sw >> r & 1) && part_switch(parts[r]) && part_on(parts[r]);
    (void)c;
    for (int r = 0; r < np; r++) set += (sw >> r & 1) && part_switch(parts[r]) && part_on(parts[r]) != (int)(on >> r & 1);
    static _Atomic uint64_t pending;   // (the build last set: its devices reading as the runner's is logged once)
    if (!set) {
        uint64_t k = e->key64;
        if (atomic_compare_exchange_strong(&pending, &k, 0))
            rt_log("coop: contraption: %08x: its devices are as where it runs: %s", cid32(e), there_on ? "on" : "off");
        return;
    }
    atomic_store(&pending, e->key64);
    plat_mutex_lock(&reg_lock);
    e->toggled_us = now;
    dev_set_later(parts, np, on & sw, 'f');
    plat_mutex_unlock(&reg_lock);
    rt_log("coop: contraption: %08x: its devices %s where it runs, %s here: set %s (%d parts, there on %#x of %#x)", cid32(e), there_on ? "on" : "off",
           here_on ? "on" : "off", there_on ? "on" : "off", set, on, sw);
}
// A hit by this game's player on a build another game runs sets, besides the device's toggle bit, the hit event in the
// build's group (group +0x880 bit 26, main+0x1c02a98), which the group's sender main+0x32eb90 would turn into on/off
// messages to every device here. The event is taken off follower groups at the sender's entry (coop_life.c): the
// runner decides, and its state comes back by STATE.
static _Atomic uint64_t follow_group[REG_N];
int coop_ctn_sender_entry(uint64_t group) {   // 1: the group of a build followed here (the sender sends nothing: coop_life.c)
    for (int i = 0; group && i < REG_N; i++)
        if (atomic_load_explicit(&follow_group[i], memory_order_relaxed) == group) {
            __atomic_fetch_and((uint64_t *)(uintptr_t)(group + 0x880), ~(1ull << 26), __ATOMIC_SEQ_CST);
            return 1;
        }
    return 0;
}
// A toggle asked on a part of a build another game runs (a hit here): not made here, sent to the runner as the state
// wanted. From the device's switch update main+0x4a3500 (coop_autobuild.c coop_phase0_hook).
void coop_ctn_toggle_asked(uint64_t comp, uint64_t actor) {
    TogMsg m = {0};
    int found = 0;
    uint32_t id = 0;
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N && !found; i++)
        for (int r = 0; reg[i].used && !runs(&reg[i]) && r < reg[i].np && !found; r++)
            if (reg[i].parts[r] == actor) m.inc = reg[i].inc, m.serial = reg[i].serial, m.part = (uint32_t)r, id = cid32(&reg[i]), found = 1;
    plat_mutex_unlock(&reg_lock);
    if (!found) {   // (not a build's part: a lone device of another player's)
        if (lone_ask(actor, !part_on(actor), 0)) {
            plat_mutex_lock(&ev_lock);
            tog_seen[tog_i % 16].actor = actor, tog_seen[tog_i % 16].us = frame_step_world_us(), tog_i++;
            plat_mutex_unlock(&ev_lock);
        }
        return;
    }
    plat_mutex_lock(&ev_lock);   // (the same hit's switch event, if any, is not sent besides: events_out)
    tog_seen[tog_i % 16].actor = actor, tog_seen[tog_i % 16].us = frame_step_world_us(), tog_i++;
    plat_mutex_unlock(&ev_lock);
    m.want = !part_on(actor);   // (a toggle on the copy as it shows: the other state)
    coop_ctr_send(0, COOP_CTR_TOGGLE, COOP_CTR_RELIABLE, &m, sizeof m);
    rt_log("coop: contraption: a toggle asked on %08x part %u here: sent to the game that runs it, wanted %s (switch %llx)", id, m.part, m.want ? "on" : "off",
           (unsigned long long)comp);
}

// The wheels and sticks of the builds followed here shown as where they run (at actor phase 0 and at the co-op tick:
// when in the frame the stick's own graph writes its floats is not read, so both)
static void floats_follow(Cpu *c) {
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || runs(e) || e->local != L_LIVE || e->touched || !(e->fw | e->fk | e->fb)) continue;
        (void)c;
        for (int r = 0; r < e->np && r < 21; r++) {
            // A time bomb: armed as where it runs (the bit its own arming message sets, fuse node +0xe8 bit 0), and its
            // fuse (node +0xdc now, +0xe0 before) the runner's, never under 2 frames: at 0 the node here would ask the
            // bomb's death and blast by itself. The blast comes with the runner's. Research notes
            // sync-v3/m2/p5/rocket-timebomb, 2.2 and 2.4.
            if (e->fb >> r & 1) {
                uint64_t node = fuse_node(e->parts[r]);
                float left = e->pf[r][0] > 2.0f ? e->pf[r][0] : 2.0f;
                if (!node || e->pf[r][1] == 0 || left != left) continue;
                __atomic_fetch_or((uint8_t *)(uintptr_t)(node + 0xe8), (uint8_t)1, __ATOMIC_SEQ_CST);
                memcpy((void *)(uintptr_t)(node + 0xdc), &left, 4), memcpy((void *)(uintptr_t)(node + 0xe0), &left, 4);
                continue;
            }
            uint64_t bb = (e->fw | e->fk) >> r & 1 ? actor_bb(e->parts[r]) : 0;
            if (!bb) continue;
            if (e->fp >> r & 1) continue;   // (a spring: springs_phase0)
            if (e->fw >> r & 1) bb_set_f32(c, bb, BB_WHEEL_SPIN, e->pf[r][0]), bb_set_f32(c, bb, BB_WHEEL_STEER, e->pf[r][1]);
            else bb_set_f32(c, bb, BB_STICK_X, e->pf[r][0]), bb_set_f32(c, bb, BB_STICK_Y, e->pf[r][1]), bb_set_f32(c, bb, BB_STICK_SX, e->pf[r][2]), bb_set_f32(c, bb, BB_STICK_SY, e->pf[r][3]);
        }
    }
    plat_mutex_unlock(&reg_lock);
}

// ---- A part's death where the build runs, made the same on the copies (plan 36 item 4) ------------------------------------
// A copy part takes no death from this game's own rules (G4). The game that runs the build sends each death its part
// is asked (the kind: 8 exploded, 9 burned, 16 life gone, 20 an Autobuilt part broken off, ...), and the others ask
// the same death of their copy with the game's own request main+0x1953184(actor, kind, 0), at actor phase 0, with the
// refusal lifted for that call. So a time bomb's blast, a burned board and a broken-off part's vanishing play on the
// copy too. Death kinds: research notes sync-v3/m2/p5/rocket-timebomb (table at main+0x366f8b0).
#define CTR_DEATH COOP_CTR_HIT   // (a message number of the channel the rebuilt core had left unused)
typedef struct { uint64_t inc; uint32_t serial, part, kind, v; } DeathMsg;   // (v: the version part is a number of)
static struct { uint64_t actor; uint32_t kind; int64_t us; } death_said[16], death_put[16];
static int death_said_i, death_put_n;
void coop_life_death_allow(int on);
void coop_ctn_death_seen(uint64_t actor, uint32_t kind) {   // from the death hooks (any thread), a request not refused
    if (!ctn_on() || !coop_ctn_part(actor)) return;
    DeathMsg m = {0};
    int found = 0;
    int64_t now = frame_step_world_us();
    plat_mutex_lock(&reg_lock);
    died[died_i % 32].actor = actor, died[died_i++ % 32].us = now;   // (died, not unloaded: record_pass, authority)
    // (asked every frame by some rules: once per part and kind in 3 s; another kind is another death, finding 63)
    for (int i = 0; i < 16 && !found; i++) found = death_said[i].actor == actor && death_said[i].kind == kind && now - death_said[i].us < 3000000;
    if (!found) {
        for (int i = 0; i < REG_N && !m.inc; i++)
            for (int r = 0; reg[i].used && runs(&reg[i]) && reg[i].local == L_LIVE && r < reg[i].np && !m.inc; r++)
                if (reg[i].parts[r] == actor) m.inc = reg[i].inc, m.serial = reg[i].serial, m.part = (uint32_t)r, m.v = reg[i].v;
        if (m.inc) death_said[death_said_i % 16].actor = actor, death_said[death_said_i % 16].kind = kind, death_said[death_said_i++ % 16].us = now;
    }
    plat_mutex_unlock(&reg_lock);
    if (found || !m.inc) return;
    m.kind = kind;
    coop_ctr_send(0, CTR_DEATH, COOP_CTR_RELIABLE, &m, sizeof m);
    rt_log("coop: contraption: part %u of a build run here is asked death kind %u: told to the other games", m.part, kind);
}
static void deaths_put(Cpu *c) {   // actor phase 0
    uint64_t actor[16];
    uint32_t kind[16];
    plat_mutex_lock(&reg_lock);
    int n = death_put_n;
    for (int i = 0; i < n; i++) actor[i] = death_put[i].actor, kind[i] = death_put[i].kind;
    death_put_n = 0;
    plat_mutex_unlock(&reg_lock);
    for (int i = 0; i < n; i++) {
        if (!alive(actor[i])) continue;
        coop_life_death_allow(1);
        coop_guest_call(c, MOD_MAIN_BASE + 0x1953184, actor[i], kind[i], 0, 0);
        coop_life_death_allow(0);
    }
}
// The gauge manager of this game (x0 of its charge functions, noted by coop_autobuild.c), and whether it can pay:
// blocked = byte +0x50 bit 0, or byte +0xe8 bit 0 clear (research notes sync-v3/m2/p5/energy, section 0.1).
static _Atomic uint64_t gauge_mgr;
void coop_ctn_gauge_seen(uint64_t mgr) { if (mgr) atomic_store_explicit(&gauge_mgr, mgr, memory_order_relaxed); }
static int gauge_blocked(void) {
    uint64_t mgr = atomic_load_explicit(&gauge_mgr, memory_order_relaxed);
    uint8_t b50 = 0, be8 = 1;
    return mgr && coop_rd(mgr + 0x50, &b50, 1) && coop_rd(mgr + 0xe8, &be8, 1) && ((b50 & 1) || !(be8 & 1));
}

// ---- Burning parts (plan 36 item 2; research notes sync-v3/m2/p8/wood-status) --------------------------------------------
// A wooden part burns by its own chemistry in each game, from what touches it there. So that a copy burns when its
// part does where the build runs, and stops when it stops, the runner sends which parts burn (the game's own
// "is any element burning" main+0x1b306fc(chemical component, -1), the component at [actor +0x228] +0x180) and a copy
// that differs is given the request the game's own nodes give: ignite main+0x1b34a44(comp, 2, -1) (fire cause
// "forced"), extinguish main+0xb2b804(comp, 0, -1). The burned-out end comes as the part's death (kind 9), sent with
// the deaths above. Not shown by reading: that a request alone brings a copy to the same state (the step that takes
// the game's decision into the element was not found).
static uint64_t chem_comp(uint64_t a) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0;
    return t ? coop_rd_ptr(t + 0x180) : 0;
}
static void burning_phase0(Cpu *c) {
    static int64_t last;
    int64_t now = frame_step_world_us();
    if (now - last < 200000) return;
    last = now;
    for (int i = 0; i < REG_N; i++) {
        uint64_t parts[21];
        uint32_t want = 0, have = 0;
        int np, mine;
        plat_mutex_lock(&reg_lock);
        Ent *e = &reg[i];
        int go = e->used && e->local == L_LIVE && e->np && !e->touched;
        np = e->np, mine = go && runs(e), want = e->burn;
        memcpy(parts, e->parts, sizeof parts);
        plat_mutex_unlock(&reg_lock);
        if (!go) continue;
        for (int r = 0; r < np && r < 21; r++) {
            uint64_t comp = alive(parts[r]) ? chem_comp(parts[r]) : 0;
            if (comp && (coop_guest_call(c, MOD_MAIN_BASE + 0x1b306fc, comp, (uint64_t)-1, 0, 0) & 1)) have |= 1u << r;
        }
        if (mine) {
            plat_mutex_lock(&reg_lock);
            if (reg[i].used && runs(&reg[i])) reg[i].burn = have;
            plat_mutex_unlock(&reg_lock);
            continue;
        }
        for (int r = 0; r < np && r < 21; r++) {
            uint64_t comp = ((want ^ have) >> r & 1) && alive(parts[r]) ? chem_comp(parts[r]) : 0;
            if (!comp) continue;
            if (want >> r & 1) coop_guest_call(c, MOD_MAIN_BASE + 0x1b34a44, comp, 2, (uint64_t)-1, 0);
            else coop_guest_call(c, MOD_MAIN_BASE + 0xb2b804, comp, 0, (uint64_t)-1, 0);
            rt_log("coop: contraption: part %d of a build followed here %s where it runs: %s here", r, want >> r & 1 ? "burns" : "does not burn", want >> r & 1 ? "set alight" : "put out");
        }
    }
}

// ---- A spring's head (plan 37 note 14; research notes sync-v3/m2/p7/spring-bodies) ---------------------------------------
// A spring is two bodies, the base (main) and the head, on a slider joint "PistonSlider" with a motor that pulls the head
// in and a spring that pushes it out. The object mirror and the follow drive move the base only, and after a launch the
// joint holds no extension of its own: the head is where each game's physics left it. So the game that runs a spring
// sends its extension, the joint's upper limit and its two enables (spring, motor), and a copy is given them: the
// joint by the game's own setters, the head put where that extension is from the base (W_head = W_base * frame B *
// Tx(ext) * inverse(frame A), the joint's own two pivot frames).
//   joint from the actor: main+0x29629d4(phys, "PistonSlider" at main+0x2c478e7) -> ref; ref's vtable +0x10 (ref, key
//   main+0xba6c2c()) -> J; data D = J+0x38's vtable slot 0 (it, key main+0xba6fc8()); head [J+0x100], base [J+0x108];
//   D +0x58 bit 0 spring on, bit 1 motor on, +0x60 upper limit, +0x64 / +0x94 the frames of head and base.
//   setters: main+0x2997c04(J, s0 = upper limit), 0x2997c54(J, spring on), 0x2997d94(J, motor on).
typedef struct { uint64_t actor; float ext, limit; uint32_t flags; } SprSt;   // flags: 1 spring on, 2 motor on
#define SPR_N 32
static SprSt spr_have[SPR_N];      // this game's own springs and those of builds it runs, read 10 times a second
static int spr_have_n;
static PlatMutex spr_lock = PLAT_MUTEX_INIT;
typedef struct { uint64_t J, D, head, base; } SprJoint;
static int spring_actor(uint64_t a) {
    char nm[48];
    return a && !strncmp(coop_actor_name(a, nm, sizeof nm), "SpObj_SpringPiston", 18);
}
static int spring_joint(Cpu *c, uint64_t actor, SprJoint *j) {
    uint64_t phys = phys_sys(actor);
    memset(j, 0, sizeof *j);
    uint64_t ref = phys ? coop_guest_call(c, MOD_MAIN_BASE + 0x29629d4, phys, MOD_MAIN_BASE + 0x2c478e7, 0, 0) : 0;
    uint64_t fn = ref ? coop_rd_ptr(coop_rd_ptr(ref) + 0x10) : 0;
    if (!fn) return 0;
    j->J = coop_guest_call(c, fn, ref, coop_guest_call(c, MOD_MAIN_BASE + 0xba6c2c, 0, 0, 0, 0), 0, 0);
    uint64_t dw = j->J ? coop_rd_ptr(j->J + 0x38) : 0, dfn = dw ? coop_rd_ptr(coop_rd_ptr(dw)) : 0;
    j->D = dfn ? coop_guest_call(c, dfn, dw, coop_guest_call(c, MOD_MAIN_BASE + 0xba6fc8, 0, 0, 0, 0), 0, 0) : 0;
    if (!j->D) return 0;
    j->head = coop_rd_ptr(j->J + 0x100), j->base = coop_rd_ptr(j->J + 0x108);
    return j->head && j->base;
}
static int spring_read(const SprJoint *j, SprSt *s) {   // (the extension as main+0x2997ff4 computes it)
    float fa[12], fb[12], ma[12], mb[12];
    uint8_t f58 = 0;
    if (!coop_rd(j->D + 0x64, fa, 48) || !coop_rd(j->D + 0x94, fb, 48) || !coop_rd(j->head + 0x98, ma, 48) || !coop_rd(j->base + 0x98, mb, 48) ||
        !coop_rd(j->D + 0x58, &f58, 1) || !coop_rd(j->D + 0x60, &s->limit, 4)) return 0;
    s->ext = 0;
    for (int i = 0; i < 3; i++) {
        float axis = fb[0] * mb[i * 4] + fb[4] * mb[i * 4 + 1] + fb[8] * mb[i * 4 + 2];
        float pa = ma[i * 4 + 3] + fa[3] * ma[i * 4] + fa[7] * ma[i * 4 + 1] + fa[11] * ma[i * 4 + 2];
        float pb = mb[i * 4 + 3] + fb[3] * mb[i * 4] + fb[7] * mb[i * 4 + 1] + fb[11] * mb[i * 4 + 2];
        s->ext += axis * (pa - pb);
    }
    s->flags = f58 & 3;
    return s->ext == s->ext && s->limit == s->limit;
}
static uint64_t call_f(Cpu *c, uint64_t fn, uint64_t a0, float s0) {   // a guest call with a float in s0
    __uint128_t v0 = c->v[0];
    uint32_t bits;
    memcpy(&bits, &s0, 4);
    c->v[0] = bits;
    uint64_t r = coop_guest_call(c, fn, a0, 0, 0, 0);
    c->v[0] = v0;
    return r;
}
static void spring_apply(Cpu *c, uint64_t actor, const SprSt *want) {
    SprJoint j;
    SprSt cur;
    if (!spring_joint(c, actor, &j) || !spring_read(&j, &cur)) return;
    if (fabsf(cur.limit - want->limit) > 0.001f) call_f(c, MOD_MAIN_BASE + 0x2997c04, j.J, want->limit);
    if ((cur.flags ^ want->flags) & 1) coop_guest_call(c, MOD_MAIN_BASE + 0x2997c54, j.J, want->flags & 1, 0, 0);
    if ((cur.flags ^ want->flags) & 2) coop_guest_call(c, MOD_MAIN_BASE + 0x2997d94, j.J, want->flags >> 1 & 1, 0, 0);
    if (fabsf(cur.ext - want->ext) > 0.05f) {
        static _Alignas(16) float m[12];
        float fa[12], fb[12], fai[12], base[12], t[12], tx[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (!coop_rd(j.D + 0x64, fa, 48) || !coop_rd(j.D + 0x94, fb, 48) || !coop_rd(j.base + 0x98, base, 48) || !m34_inv(fai, fa)) return;
        tx[3] = want->ext;
        m34_mul(t, base, fb), m34_mul(t, t, tx), m34_mul(m, t, fai);
        coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, j.head, (uint64_t)(uintptr_t)m, 0, 0);
    }
}
static int spr_find(uint64_t actor, SprSt *out) {   // what this game last read of its own spring
    int got = 0;
    plat_mutex_lock(&spr_lock);
    for (int i = 0; i < spr_have_n && !got; i++) if (spr_have[i].actor == actor) *out = spr_have[i], got = 1;
    plat_mutex_unlock(&spr_lock);
    return got;
}
static void springs_phase0(Cpu *c);

// ---- Lone devices (plan 31, notes 4 and 5) -------------------------------------------------------------------------------
// A device in no build (a single fan, spring, light) is its owner's object, and the other games show its copy through
// the object mirror, which moves it. Its on state goes the way a build's does: the owner's game sends the state it has
// (five times a second and at once on a change), the others set their copy to it with the same set messages, and a
// hit or a jump attack here on a copy is sent to the owner as the state wanted or the switch event. Nothing is flipped.
// The older path (coop_bonds.c, TOTK_COOP_DEVICE_SYNC) flipped the copy with the hit bit and tested "on" by a word
// that is not the latch, so two games drifted apart by one flip; it is switched off when this file is on.
#define CTR_LONE COOP_CTR_PUSH        // (two message numbers of the channel the rebuilt core had left unused)
#define CTR_LONE_ASK COOP_CTR_IMPULSE
typedef struct { uint64_t key; uint32_t id, on; uint32_t spring, flags; float ext, limit; } LoneMsg;   // (key first: the channel keeps the latest per sender and key)
typedef struct { uint64_t owner; uint32_t id, want, bits, pad; char name[48]; } LoneAsk;
#define LONE_N 48
static struct { uint64_t actor, owner; int owner_id; uint32_t id, on; int64_t heard_us, set_us; int spring; SprSt spr; uint32_t gate; } lone[LONE_N];
static _Atomic uint64_t lone_actor[LONE_N];
static PlatMutex lone_lock = PLAT_MUTEX_INIT;
int coop_ctn_follows(uint64_t a) {
    if (coop_ctn_driven(a)) return 1;
    for (int i = 0; a && i < LONE_N; i++) if (atomic_load_explicit(&lone_actor[i], memory_order_relaxed) == a) return 1;
    return 0;
}
// 1: actor a is a device this game only follows, or a shot of one (a beam, a cannon ball): its hits on an enemy's copy
// here are not counted, since the game that runs the device has the same emitter hitting the same enemy and the
// enemy's life is its owner's. A shot is tied to its device both ways the game keeps: the shot's Shootable component
// ([shot +0x228] +0x440) names its creator by the handle at +0x70, and the device's Shooter ([device +0x228] +0x448)
// lists its shots: rows at +0x20 (0x38 bytes each, count +0x2c), a row's entries at +0x10 (0xf8 bytes each, count the
// byte +0x2c), an entry's shot the handle at +0x20. Research notes sync-v3/m2/p6/emitter-harm, section 0.3.
static uint64_t handle_actor(uint64_t h) {
    int32_t serial = -1, have = 0;
    uint64_t ref = h ? coop_rd_ptr(h + 8) : 0;
    if (!ref || !coop_rd(h + 0x10, &serial, 4) || serial == -1 || !coop_rd(ref + 0x48, &have, 4) || have != serial) return 0;
    return coop_rd_ptr(ref + 0x40);
}
static int shot_of(uint64_t device, uint64_t a) {
    uint64_t t = coop_rd_ptr(device + 0x228), sh = t ? coop_rd_ptr(t + 0x448) : 0, rows = sh ? coop_rd_ptr(sh + 0x20) : 0;
    uint32_t nrows = 0;
    if (!rows || !coop_rd(sh + 0x2c, &nrows, 4) || nrows > 16) return 0;
    for (uint32_t r = 0; r < nrows; r++) {
        uint64_t row = rows + 0x38ull * r, en = coop_rd_ptr(row + 0x10);
        uint8_t ne = 0;
        if (!en || !coop_rd(row + 0x2c, &ne, 1) || ne > 16) continue;
        for (uint32_t k = 0; k < ne; k++) if (handle_actor(en + 0xf8ull * k + 0x20) == a) return 1;
    }
    return 0;
}
int coop_ctn_emitter_of_followed(uint64_t a) {
    uint64_t x = a;
    for (int hop = 0; x && hop < 3; hop++) {
        if (coop_ctn_follows(x)) return 1;
        uint64_t t = coop_rd_ptr(x + 0x228), sb = t ? coop_rd_ptr(t + 0x440) : 0;
        x = sb ? handle_actor(sb + 0x70) : 0;
    }
    for (int i = 0; a && i < DRIVEN_N; i++) {
        uint64_t d = atomic_load_explicit(&driven[i], memory_order_relaxed);
        if (!d) break;
        if (shot_of(d, a)) return 1;
    }
    for (int i = 0; a && i < LONE_N; i++) {
        uint64_t d = atomic_load_explicit(&lone_actor[i], memory_order_relaxed);
        if (d && shot_of(d, a)) return 1;
    }
    return 0;
}
static int lone_ask(uint64_t actor, uint32_t want, uint32_t bits) {   // a hit here on a lone copy: to its owner. 1: it was one
    LoneAsk m = {0};
    plat_mutex_lock(&lone_lock);
    for (int i = 0; i < LONE_N && !m.id; i++) if (lone[i].actor == actor) m.owner = lone[i].owner, m.id = lone[i].id;
    plat_mutex_unlock(&lone_lock);
    if (!m.id) return 0;
    m.want = want, m.bits = bits;
    coop_actor_name(actor, m.name, sizeof m.name);
    coop_ctr_send(0, CTR_LONE_ASK, COOP_CTR_RELIABLE, &m, sizeof m);
    rt_log("coop: contraption: a %s on another player's %s here: sent to its owner (%s)", bits ? "switch event" : "toggle", m.name, bits ? "event" : want ? "wanted on" : "wanted off");
    return 1;
}
static void lone_store(uint64_t a, uint64_t from, int from_id, const LoneMsg *p, int64_t now) {   // a lone copy's state, from its owner
    plat_mutex_lock(&lone_lock);
    int k = -1;
    for (int i = 0; i < LONE_N && k < 0; i++) if (lone[i].actor == a) k = i;
    for (int i = 0; i < LONE_N && k < 0; i++) if (!lone[i].actor || now - lone[i].heard_us > 3000000) k = i;
    if (k >= 0) {
        if (lone[k].actor != a) lone[k].set_us = 0;
        lone[k].actor = a, lone[k].owner = from, lone[k].owner_id = from_id, lone[k].id = p->id, lone[k].on = p->on, lone[k].heard_us = now;
        lone[k].spring = (int)p->spring, lone[k].spr.ext = p->ext, lone[k].spr.limit = p->limit, lone[k].spr.flags = p->flags;
    }
    plat_mutex_unlock(&lone_lock);
}
static void lone_tick(void) {
    int64_t now = frame_step_world_us();
    uint64_t me = coop_ctr_me();
    static int64_t sent_us;
    static struct { uint32_t id, on; } last[64];
    CoopCtrMsg m;
    // this game's own lone switch devices: their state out
    {
        uint64_t own[256];
        int no = coop_own_sent_objects(own, 256), k = 0, all = now - sent_us > 200000;
        if (all) sent_us = now;
        for (int i = 0; i < no && k < 64; i++) {
            int owner = 0;
            uint32_t id = 0;
            if (coop_is_copy(own[i]) || coop_contraption_made(own[i]) || coop_ctn_part(own[i]) || !part_switch(own[i]) || part_group(own[i]) || !coop_obj_ident(own[i], &owner, &id) || !id) continue;
            LoneMsg s = {cid_key(me, id) ^ 0x4c4f4e45ull, id, (uint32_t)part_on(own[i]), 0, 0, 0, 0};
            SprSt sp;
            if (spr_find(own[i], &sp)) s.spring = 1, s.flags = sp.flags, s.ext = sp.ext, s.limit = sp.limit;
            int changed = 1;
            for (int q = 0; q < 64; q++) if (last[q].id == id) changed = last[q].on != s.on;
            if (all || changed) coop_ctr_send(0, CTR_LONE, changed ? COOP_CTR_RELIABLE : 0, &s, sizeof s);
            last[k].id = id, last[k].on = s.on, k++;
        }
        for (; all && k < 64; k++) last[k].id = 0;
    }
    // the other games' lone devices: their copies here found and set. A change comes reliable; the refresh five times a
    // second comes unreliable, kept by the channel as the latest per sender and key, and is read from there for each
    // copy here of another game's switch device: a copy made after the last change is set too, and an entry stays while
    // its owner keeps sending (review 1 finding 14; the reliable queue alone saw only changes, and an entry lapsed 3 s
    // after the last one).
    while (coop_ctr_recv_type(CTR_LONE, &m)) {
        const LoneMsg *p = (const LoneMsg *)m.d;
        if (m.len != (int)sizeof(LoneMsg) || !p->id) continue;
        uint64_t a = coop_obj_find(m.from_id, p->id, "");
        if (a && coop_is_copy(a) && !coop_contraption_made(a) && !coop_ctn_part(a) && part_switch(a)) lone_store(a, m.from, m.from_id, p, now);
    }
    static int64_t polled_us;
    if (now - polled_us > 200000) {
        polled_us = now;
        int coop_copies_all(uint64_t *out, int max);
        uint64_t cp[256];
        int nc = coop_copies_all(cp, 256);
        for (int i = 0; i < nc; i++) {
            int owner = 0;
            uint32_t id = 0;
            uint64_t age = 0;
            LoneMsg p;
            if (coop_contraption_made(cp[i]) || coop_ctn_part(cp[i]) || !part_switch(cp[i]) || !coop_obj_ident(cp[i], &owner, &id) || !id || owner == coop_net_id()) continue;
            uint64_t oinc = coop_ctr_incarnation(owner);
            if (!oinc || coop_ctr_latest(owner, CTR_LONE, cid_key(oinc, id) ^ 0x4c4f4e45ull, &p, sizeof p, &age) != (int)sizeof p) continue;
            if (p.id != id || age > 1000000000ull) continue;   // (the owner stopped sending it a second ago or more)
            lone_store(cp[i], oinc, owner, &p, now);
        }
    }
    plat_mutex_lock(&lone_lock);
    for (int i = 0; i < LONE_N; i++) {
        uint64_t a = lone[i].actor;
        if (a && (now - lone[i].heard_us > 3000000 || !alive(a) || coop_ctn_part(a))) a = lone[i].actor = 0;   // (a build's part now: the build's state sets it, finding 67)
        atomic_store_explicit(&lone_actor[i], a, memory_order_relaxed);
        if (a) {   // Whether the game runs this copy's update: its calc mask (actor +0x3fc: bits 0 and 1 skip the whole
            // update, 0xc4 its components) and the blocked bit (+0x5ac bit 6), said when they change. A device out of
            // step while its player looks away (plan 37, note 14) is expected to show here. Research notes
            // sync-v3/m2/p7/offscreen.
            uint32_t m3fc = 0, f5ac = 0;
            coop_rd(a + 0x3fc, &m3fc, 4), coop_rd(a + 0x5ac, &f5ac, 4);
            uint32_t gate = (m3fc & 0xc7) | (f5ac & 0x40) << 2;
            if (gate != lone[i].gate) {
                lone[i].gate = gate;
                rt_log("coop: contraption: player %d's lone device %u here: its update is %s (calc mask %#x, blocked %u)", lone[i].owner_id, lone[i].id,
                       gate & 0x103 ? "not run by the game now" : gate ? "run in part" : "run", m3fc, f5ac >> 6 & 1);
            }
        }
        if (!a || now - lone[i].set_us < 500000 || part_on(a) == (int)lone[i].on) continue;
        uint64_t parts[21] = {a};
        lone[i].set_us = now;
        plat_mutex_lock(&reg_lock);
        dev_set_later(parts, 1, lone[i].on ? 1 : 0, 'f');
        plat_mutex_unlock(&reg_lock);
        rt_log("coop: contraption: player %d's lone device %u is %s there, %s here: set", lone[i].owner_id, lone[i].id, lone[i].on ? "on" : "off", lone[i].on ? "off" : "on");
    }
    plat_mutex_unlock(&lone_lock);
    // asks from the other games about a device of this game's
    while (coop_ctr_recv_type(CTR_LONE_ASK, &m)) {
        LoneAsk q;
        if (m.len != (int)sizeof(LoneAsk)) continue;
        memcpy(&q, m.d, sizeof q), q.name[sizeof q.name - 1] = 0;
        if (q.owner != me) continue;
        uint64_t a = coop_obj_find(coop_net_id(), q.id, q.name);
        if (!a || !part_switch(a)) continue;
        if (q.bits) {
            plat_mutex_lock(&ev_lock);
            if (ev_put_n < 16) ev_put[ev_put_n].actor = a, ev_put[ev_put_n].bits = (uint8_t)(q.bits & 7), ev_put_n++;
            plat_mutex_unlock(&ev_lock);
        } else {
            uint64_t parts[21] = {a};
            plat_mutex_lock(&reg_lock);
            dev_set_later(parts, 1, q.want ? 1 : 0, 'a');
            plat_mutex_unlock(&reg_lock);
        }
        rt_log("coop: contraption: player %d asked this game's lone %s %s", m.from_id, q.name, q.bits ? "a switch event" : q.want ? "on" : "off");
    }
}

static void springs_phase0(Cpu *c) {
    static int64_t read_us;
    int64_t now = frame_step_world_us();
    uint64_t list[SPR_N];
    SprSt want[SPR_N];
    int n = 0, nw = 0;
    // this game's springs read (10 a second): the parts of builds it runs, and its own lone ones
    if (now - read_us > 100000) {
        read_us = now;
        plat_mutex_lock(&reg_lock);
        for (int i = 0; i < REG_N; i++)
            for (int r = 0; reg[i].used && runs(&reg[i]) && reg[i].local == L_LIVE && r < reg[i].np && n < SPR_N; r++)
                if (spring_actor(reg[i].parts[r])) list[n++] = reg[i].parts[r];
        plat_mutex_unlock(&reg_lock);
        uint64_t own[256];
        int no = coop_own_sent_objects(own, 256);
        for (int i = 0; i < no && n < SPR_N; i++)
            if (!coop_is_copy(own[i]) && !coop_ctn_part(own[i]) && spring_actor(own[i])) list[n++] = own[i];
        SprSt got[SPR_N];
        int ng = 0;
        for (int i = 0; i < n; i++) {
            SprJoint j;
            if (spring_joint(c, list[i], &j) && spring_read(&j, &got[ng])) got[ng++].actor = list[i];
        }
        plat_mutex_lock(&spr_lock);
        memcpy(spr_have, got, sizeof(SprSt) * (size_t)ng), spr_have_n = ng;
        plat_mutex_unlock(&spr_lock);
    }
    // the springs followed here given the state where they run
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || runs(e) || e->local != L_LIVE || e->touched) continue;
        for (int r = 0; r < e->np && nw < SPR_N; r++)
            if ((e->fp >> r & 1) && e->parts[r]) want[nw].actor = e->parts[r], want[nw].ext = e->pf[r][0], want[nw].limit = e->pf[r][1], want[nw].flags = (uint32_t)e->pf[r][2], nw++;
    }
    plat_mutex_unlock(&reg_lock);
    plat_mutex_lock(&lone_lock);
    for (int i = 0; i < LONE_N && nw < SPR_N; i++)
        if (lone[i].actor && lone[i].spring) want[nw] = lone[i].spr, want[nw].actor = lone[i].actor, nw++;
    plat_mutex_unlock(&lone_lock);
    for (int i = 0; i < nw; i++) if (alive(want[i].actor)) spring_apply(c, want[i].actor, &want[i]);
}

// ---- The co-op tick: messages, authority, touch, drive ------------------------------------------------------------------
static void receive(void) {
    CoopCtrMsg m;
    int64_t now = frame_step_world_us();
    while (coop_ctr_recv_type(COOP_CTR_VERSION, &m)) {
        const VerMsg *p = (const VerMsg *)m.d;
        if (m.len < (int)offsetof(VerMsg, d) || p->chunk >= VER_CHUNKS || p->len > VER_CHUNK || m.len != (int)(offsetof(VerMsg, d) + p->len) ||
            (size_t)p->chunk * VER_CHUNK + p->len > BP_SIZE)
            continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        if (!e && (e = ent_new(p->inc, p->serial))) rt_log("coop: contraption: %08x: a build of another game's, first heard of", cid32(e));
        if (e && p->v > e->vseen && p->v != 0xffffffffu) e->vseen = p->v;
        if (e && p->epoch >= e->epoch) {
            if (p->epoch > e->epoch || !e->runner) set_runner(e, p->runner, p->epoch);
            // (a version this game holds or passed, or one it sent itself, is not taken; a late piece of an older one neither)
            int making = mk.st && mk.e == e && p->v <= mk.v;   // (the version being made right now, sent again)
            if (!runs(e) && !making && p->v > e->v && p->v >= e->in_v) {
                if (p->v != e->in_v) e->in_v = p->v, e->in_shape = p->shape, e->in_have = 0, e->want_make = 0;
                e->in_green = p->green;
                memcpy(e->in_rec + p->chunk * VER_CHUNK, p->d, p->len);
                memcpy(e->in_ids, p->ids, sizeof e->in_ids), memcpy(e->in_oinc, p->oinc, sizeof e->in_oinc), memcpy(e->in_centre, p->centre, sizeof e->in_centre);
                e->in_have |= 1u << p->chunk;
                if (e->in_have == (1u << VER_CHUNKS) - 1 && !e->want_make) {
                    e->want_make = 1;
                    rt_log("coop: contraption: %08x version %u (shape %08x): its record came whole", cid32(e), p->v, p->shape);
                }
            }
        }
        plat_mutex_unlock(&reg_lock);
    }
    for (int type = COOP_CTR_TAKE; type <= COOP_CTR_GRANT; type++)
        while (coop_ctr_recv_type(type, &m)) {
            const CtlMsg *p = (const CtlMsg *)m.d;
            if (m.len != (int)sizeof(CtlMsg)) continue;
            plat_mutex_lock(&reg_lock);
            Ent *e = ent_find(p->inc, p->serial);
            if (e && type == COOP_CTR_GRANT) set_runner(e, p->who, p->epoch);
            else if (e && runs(e) && e->local == L_LIVE) {   // asked: handed over unless this game's player holds it
                if (!any_held(e->parts, e->np)) {
                    uint32_t ep = e->epoch + 1;
                    set_runner(e, m.from, ep);
                    send_ctl(COOP_CTR_GRANT, e, ep, m.from);
                    rt_log("coop: contraption: player %d asked to run %08x: handed over", m.from_id, cid32(e));
                } else {
                    static int64_t said;
                    if (now - said > 1000000) said = now, rt_log("coop: contraption: player %d asked to run %08x: held here, kept", m.from_id, cid32(e));
                }
            }
            plat_mutex_unlock(&reg_lock);
        }
    while (coop_ctr_recv_type(COOP_CTR_RETIRE, &m)) {
        const CtlMsg *p = (const CtlMsg *)m.d;
        if (m.len != (int)sizeof(CtlMsg)) continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        // (deleted at phase 0 by retire_pass, which waits for a making of it to end: a RETIRE that comes meanwhile is
        // kept, not dropped, review 1 finding 15)
        if (e && !runs(e) && m.from == e->runner) e->want_make = 0, e->in_v = 0xffffffffu, e->gone_us = now, e->relinq_later = 0;
        plat_mutex_unlock(&reg_lock);
    }
    while (coop_ctr_recv_type(COOP_CTR_EVENT, &m)) {   // a switch event another game's player put on a build run here
        const EvMsg *p = (const EvMsg *)m.d;
        if (m.len != (int)sizeof(EvMsg)) continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        uint64_t actor = e && runs(e) && e->local == L_LIVE && p->v == e->v && p->part < (uint32_t)e->np ? e->parts[p->part] : 0;
        if (actor && (p->bits & 5)) e->payer = m.from, e->payer_us = now;   // (switched on from there: that player pays)
        plat_mutex_unlock(&reg_lock);
        if (!actor) continue;
        plat_mutex_lock(&ev_lock);
        if (ev_put_n < 16) ev_put[ev_put_n].actor = actor, ev_put[ev_put_n].bits = (uint8_t)(p->bits & 7), ev_put_n++;
        plat_mutex_unlock(&ev_lock);
        rt_log("coop: contraption: player %d put switch event %#x on part %u of a build run here", m.from_id, p->bits, p->part);
    }
    while (coop_ctr_recv_type(CTR_DEATH, &m)) {   // a part's death where its build runs: asked of the copy at phase 0
        const DeathMsg *p = (const DeathMsg *)m.d;
        if (m.len != (int)sizeof(DeathMsg)) continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        uint64_t actor = e && !runs(e) && m.from == e->runner && e->local == L_LIVE && p->v == e->v && p->part < (uint32_t)e->np ? e->parts[p->part] : 0;
        if (actor && death_put_n < 16) death_put[death_put_n].actor = actor, death_put[death_put_n].kind = p->kind, death_put_n++;
        plat_mutex_unlock(&reg_lock);
        if (actor) rt_log("coop: contraption: part %u of %08x died where it runs (kind %u): the same asked of the copy here", p->part, cid32(e), p->kind);
    }
    while (coop_ctr_recv_type(COOP_CTR_RELINQUISH, &m)) {   // the game that ran a build no longer has it: run here if it is here
        const CtlMsg *p = (const CtlMsg *)m.d;
        if (m.len != (int)sizeof(CtlMsg)) continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        if (e && !runs(e) && m.from == e->runner) {
            if (mk.st && mk.e == e) e->relinq_later = 1;   // (being made here: taken up once made, finding 38)
            else take_over(e, p->epoch);
        }
        plat_mutex_unlock(&reg_lock);
    }
    while (coop_ctr_recv_type(COOP_CTR_PULL, &m)) {   // a game back in reach of a build it had unloaded: the version again
        const CtlMsg *p = (const CtlMsg *)m.d;
        if (m.len != (int)sizeof(CtlMsg)) continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        if (e && runs(e) && e->local == L_LIVE) e->resend = 1;
        plat_mutex_unlock(&reg_lock);
    }
    while (coop_ctr_recv_type(COOP_CTR_TOGGLE, &m)) {
        const TogMsg *p = (const TogMsg *)m.d;
        if (m.len != (int)sizeof(TogMsg)) continue;
        plat_mutex_lock(&reg_lock);
        Ent *e = ent_find(p->inc, p->serial);
        if (e && runs(e) && e->local == L_LIVE) {
            dev_set_later(e->parts, e->np, p->want ? 0x1fffffu : 0, 'a');
            e->payer = p->want ? m.from : 0, e->payer_us = now;   // (switched on from there: that player pays)
            rt_log("coop: contraption: player %d asked %08x (part %u), run here, %s", m.from_id, cid32(e), p->part, p->want ? "on" : "off");
        }
        plat_mutex_unlock(&reg_lock);
    }
    // the runner's root and devices: the latest STATE of each build this game follows
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        int route = e->used && !runs(e) ? coop_ctr_route(e->runner) : 0;
        StMsg s;
        uint64_t age = 0;
        if (!route || coop_ctr_latest(route, COOP_CTR_STATE, e->key64, &s, sizeof s, &age) != (int)sizeof s) continue;
        if (s.inc != e->inc || s.serial != e->serial || s.epoch != e->epoch || s.us <= e->hus[2]) continue;
        e->heard_us = now;
        if (e->v && s.v != e->v) continue;   // (another version's part numbers are other parts: devices and root alike, finding 17)
        if (s.flags & 2) e->sw = s.sw, e->on = s.on, e->payer = s.payer, e->fw = s.fw, e->fk = s.fk, e->fb = s.fb, e->fp = s.fp, e->burn = s.burn, memcpy(e->pf, s.pf, sizeof e->pf);
        if (!(s.flags & 1)) continue;
        memmove(e->hm[0], e->hm[1], 2 * sizeof e->hm[0]), memmove(e->hus, e->hus + 1, 2 * sizeof e->hus[0]);
        memcpy(e->hm[2], s.m, sizeof s.m), e->hus[2] = s.us;
        int64_t off = now - (int64_t)(age / 1000) - s.us;   // (min of arrival - sample time over 10 s: the jitter out)
        if (!e->clock_off_at || off < e->clock_off || now - e->clock_off_at > 10000000) e->clock_off = off, e->clock_off_at = now;
    }
    plat_mutex_unlock(&reg_lock);
}
static void send_states(void) {   // the builds this game runs: root and devices, 30 a second
    static int64_t last, said;
    int64_t now = frame_step_world_us();
    uint64_t me = coop_ctr_me();
    if (now - last < 33000) return;
    last = now;
    for (int i = 0; i < REG_N; i++) {
        StMsg s = {0};
        uint64_t root = 0;
        plat_mutex_lock(&reg_lock);
        Ent *e = &reg[i];
        int go = e->used && runs(e) && e->local == L_LIVE && e->v && e->np;
        if (go) {
            s.burn = e->burn;
            s.key = e->key64, s.inc = e->inc, s.serial = e->serial, s.epoch = e->epoch, s.v = e->v, s.shape = e->shape, s.us = now, s.flags = 2;
            for (int r = 0; r < e->np; r++)
                if (part_switch(e->parts[r])) s.sw |= 1u << r, s.on |= (uint32_t)part_on(e->parts[r]) << r;
            for (int r = 0; r < e->np; r++) {
                uint64_t bb = actor_bb(e->parts[r]);
                SprSt sp;
                if (spr_find(e->parts[r], &sp)) { s.fp |= 1u << r, s.pf[r][0] = sp.ext, s.pf[r][1] = sp.limit, s.pf[r][2] = (float)sp.flags; continue; }
                if (bb_f32(bb, BB_WHEEL_SPIN, &s.pf[r][0])) s.fw |= 1u << r, bb_f32(bb, BB_WHEEL_STEER, &s.pf[r][1]);
                else if (fuse_node(e->parts[r])) {
                    uint64_t node = fuse_node(e->parts[r]), cell = coop_rd_ptr(node + 0xd0) & ~3ull;
                    uint8_t armed = 0;
                    s.fb |= 1u << r, coop_rd(node + 0xdc, &s.pf[r][0], 4), s.pf[r][1] = cell && coop_rd(cell, &armed, 1) && armed;
                } else if (bb_f32(bb, BB_STICK_X, &s.pf[r][0]))
                    s.fk |= 1u << r, bb_f32(bb, BB_STICK_Y, &s.pf[r][1]), bb_f32(bb, BB_STICK_SX, &s.pf[r][2]), bb_f32(bb, BB_STICK_SY, &s.pf[r][3]);
            }
            if (!(s.on & s.sw) && now - e->payer_us > 2000000) e->payer = 0;
            s.payer = e->payer ? e->payer : me;
            root = e->parts[0];
        }
        plat_mutex_unlock(&reg_lock);
        if (!go) continue;
        uint64_t body = main_body_of(root);
        if (body && coop_rd(body + 0x98, s.m, sizeof s.m)) s.flags |= 1;
        int sent = coop_ctr_send(0, COOP_CTR_STATE, 0, &s, sizeof s);
        if (now - said > 1000000) said = now, rt_log("coop: contraption: state of %08x sent (%d): sw %#x on %#x, root %s", (uint32_t)(s.key ^ s.key >> 32), sent, s.sw, s.on, s.flags & 1 ? "read" : "not read");
    }
}
static void authority(void) {   // touch (plan 5.3), asks, a runner that left, a follower's shape changed here (27.3 R3)
    uint64_t me = coop_ctr_me();
    int64_t now = frame_step_world_us();
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used) continue;
        // Parts that are gone here (state past 6, or the actor freed and its memory another actor's now) leave the entry
        // at once, so nothing below reads or writes through them.
        int live = 0;
        for (int r = 0; e->local == L_LIVE && r < e->np; r++) {
            if (!e->parts[r]) continue;
            if (alive_as(e->parts[r], e->gen[r])) live++;
            else {
                if (!died_lately(e->parts[r], now)) e->unloaded |= 1u << r;
                made_forget(e->parts[r]), e->parts[r] = 0;
            }
        }
        // How far this game's Link is from the build (the runner's last root, else the version's centre)
        float dist = -1;
        {
            float lp[3], at[3] = {e->centre[0], e->centre[1], e->centre[2]};
            uint64_t link = coop_player_actor();
            if (e->hus[2]) at[0] = e->hm[2][3], at[1] = e->hm[2][7], at[2] = e->hm[2][11];
            if (link && coop_rd(link + COOP_ACTOR_POS, lp, 12)) dist = sqrtf((lp[0] - at[0]) * (lp[0] - at[0]) + (lp[1] - at[1]) * (lp[1] - at[1]) + (lp[2] - at[2]) * (lp[2] - at[2]));
        }
        // A followed build with no part left here and this game's Link far from it was unloaded by the engine: it is not
        // made again there (10.3). It is made again, from the runner's version asked anew, once the Link is back within
        // half the distance it went at.
        if (!runs(e) && e->local == L_LIVE && e->np && !live && !(mk.st && mk.e == e) && (dist < 0 || dist > 30)) {
            rt_log("coop: contraption: %08x: its %d parts here are gone with this Link %.0f m away: unloaded, not made again until back", cid32(e), e->np, dist);
            e->absent = 1, e->absent_dist = dist < 0 ? 120 : dist, e->local = L_NONE, e->np = 0, e->v = 0, e->del_ok = 0, e->changed_us = 0;
            memset(e->parts, 0, sizeof e->parts);
        }
        if (!runs(e) && e->absent == 1 && dist >= 0 && dist < e->absent_dist * 0.5f) {
            rt_log("coop: contraption: %08x: this Link is back within %.0f m: its version asked again", cid32(e), dist);
            e->absent = 2, e->pulled_us = 0;
        }
        if (!runs(e) && e->absent == 2 && e->local == L_NONE && !e->want_make && now - e->pulled_us > 3000000 && coop_ctr_route(e->runner))
            e->pulled_us = now, send_ctl(COOP_CTR_PULL, e, e->epoch, me);
        int follows = !runs(e) && e->local == L_LIVE && e->np;
        // held or steered here: this game moves it from that frame on, before the grant; the drive and the gates step aside
        int t = follows && any_held(e->parts, e->np);
        if (t != e->touched) rt_log("coop: contraption: %08x %s here", cid32(e), t ? "touched: run by this game's touch until the grant" : "let go");
        e->touched = t;
        if (!follows) continue;
        // Silence never deletes or moves a build (I7). The game that runs it leaving the session makes the
        // lowest-numbered game still here run it (plan 5.9).
        if (!coop_ctr_route(e->runner)) {
            int lowest = coop_net_id();
            for (int id = 1; id < lowest; id++) if (coop_net_live(id)) { lowest = id; break; }
            if (lowest == coop_net_id()) {
                uint32_t ep = e->epoch + 1;
                rt_log("coop: contraption: %08x: the game running it left: run here now", cid32(e));
                set_runner(e, me, ep), send_ctl(COOP_CTR_GRANT, e, ep, me), e->resend = 1;
            }
            continue;
        }
        // its parts here no longer one group of the record's size
        uint64_t g0 = part_group(e->parts[0]);
        int32_t gn = 0;
        int differs = !g0 || !coop_rd(g0 + 0x58, &gn, 4) || gn != e->np;
        for (int r = 1; r < e->np && !differs; r++) differs = alive(e->parts[r]) && part_group(e->parts[r]) != g0;
        if (!differs) e->changed_us = 0;
        else if (!e->changed_us) e->changed_us = now;
        int mine = e->touched || now - atomic_load(&coop_ultrahand_hold_us) < 2000000;
        int ask = e->touched || (differs && mine && now - e->changed_us > 1000000);
        if (ask && now - e->asked_us > 300000) {
            e->asked_us = now;
            send_ctl(COOP_CTR_TAKE, e, e->epoch, me);
            static int64_t said;
            if (now - said > 1000000) said = now, rt_log("coop: contraption: %08x %s here: asked to run it", cid32(e), e->touched ? "held" : "changed shape");
        } else if (differs && !mine && live == e->np && now - e->changed_us > 3000000 && now - e->remade_us > 10000000 && !e->want_make && !(mk.st && mk.e == e)) {
            // Changed by nothing this game's player did: made again from the version held. Only with every part still
            // here: a part gone was unloaded or destroyed, and the game that runs it sends the next version or RETIRE;
            // made again, a destroyed build would come back as a copy no game runs (review 1 findings 37 and 44).
            rt_log("coop: contraption: %08x is not whole here (%d members, %d parts): made again", cid32(e), gn, e->np);
            memcpy(e->in_rec, e->rec, BP_SIZE), memcpy(e->in_centre, e->centre, sizeof e->centre);
            memcpy(e->in_ids, e->ids, sizeof e->in_ids), memcpy(e->in_oinc, e->oinc, sizeof e->in_oinc);
            e->in_v = e->v, e->in_shape = e->shape, e->in_green = e->green, e->want_make = 1, e->remade_us = now;
        }
    }
    plat_mutex_unlock(&reg_lock);
}
static void retire_pass(Cpu *c) {   // builds their runner retired: the parts here deleted, the entry freed
    orphans_pass(c, frame_step_world_us());
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || e->in_v != 0xffffffffu || (mk.st && mk.e == e)) continue;
        plat_mutex_unlock(&reg_lock);   // (the request runs game code, which asks this file about parts)
        // Parts taken in from the objects are let go, not deleted: the glue between them is broken first, as the game
        // breaks one, or they would stay joined here when the build is gone where it ran.
        if (e->adopt) {
            static EdgeAt red[21 * 12];
            int ne = 0;
            for (int r = 0; r < e->np && r < 21; r++)
                if ((e->adopt >> r & 1) && alive_as(e->parts[r], e->gen[r])) ne += part_edges(c, e->parts[r], r, red + ne, (int)(sizeof red / sizeof red[0]) - ne);
            for (int x = 0; x < ne; x++) {
                int seen = 0;
                for (int q = 0; q < x; q++) seen |= red[q].edge == red[x].edge;
                if (!seen) coop_guest_call(c, MOD_MAIN_BASE + 0x1cf54d4, e->parts[red[x].part] + 0x378, red[x].edge, 0, (uint64_t)red[x].ed);
            }
            e->adopt &= 0;   // (once: the parts are plain objects from here on; parts_delete below finds none to delete)
            for (int r = 0; r < e->np && r < 21; r++) if (alive_as(e->parts[r], e->gen[r]) && !coop_contraption_made(e->parts[r])) e->del_ok |= 1u << r;
        }
        int all = parts_delete(c, e);
        plat_mutex_lock(&reg_lock);
        if (!all || !e->used || e->in_v != 0xffffffffu) continue;   // (a delete refused now: asked again next tick)
        rt_log("coop: contraption: %08x retired by the game that ran it: its %d parts here deleted", cid32(e), e->np);
        free(e->fs), memset(e, 0, sizeof *e);
    }
    plat_mutex_unlock(&reg_lock);
}
static void lists_rebuild(Cpu *c) {
    uint64_t l[DRIVEN_N], b[DRIVEN_N], me = coop_ctr_me();
    static uint64_t all[REG_N * 21];
    int n = 0, nb = 0, na = 0;
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || e->local == L_NONE) continue;
        for (int r = 0; r < e->np; r++) if (e->parts[r]) all[na++] = e->parts[r];
        if (!runs(e) && e->local == L_LIVE && !e->touched)
            for (int r = 0; r < e->np && n < DRIVEN_N; r++) if (e->parts[r]) l[n++] = e->parts[r];
        uint64_t payer = e->payer ? e->payer : e->runner;
        if (e->local == L_LIVE && payer != me)
            for (int r = 0; r < e->np && nb < DRIVEN_N; r++) if (e->parts[r]) b[nb++] = e->parts[r];
    }
    plat_mutex_unlock(&reg_lock);
    for (int i = 0; i < DRIVEN_N; i++) atomic_store_explicit(&driven[i], i < n ? l[i] : 0, memory_order_relaxed);
    for (int i = 0; i < DRIVEN_N; i++) atomic_store_explicit(&driven_body[i], i < n ? main_body_of(l[i]) : 0, memory_order_relaxed);
    for (int i = 0; i < LONE_N && nb < DRIVEN_N; i++) {   // (another player's lone device: its owner pays)
        uint64_t la = atomic_load_explicit(&lone_actor[i], memory_order_relaxed);
        if (la) b[nb++] = la;
    }
    for (int i = 0; i < DRIVEN_N; i++) atomic_store_explicit(&nocharge[i], i < nb ? b[i] : 0, memory_order_relaxed);
    for (int i = 0; i < REG_N; i++) {   // (the groups of the builds followed here: coop_ctn_sender_entry)
        uint64_t g = reg[i].used && !runs(&reg[i]) && reg[i].local == L_LIVE && reg[i].np ? part_group(reg[i].parts[0]) : 0;
        atomic_store_explicit(&follow_group[i], g, memory_order_relaxed);
    }
    for (int i = 0; i < REG_N * 21; i++) atomic_store_explicit(&part_list[i], i < na ? all[i] : 0, memory_order_relaxed);
    coop_life_veto_rebuild(c, all, na);   // every contraption part here, for the stand-in collision veto (coop_life.c, I16)
}
// Every game logs this line every 10 s: the counts a clean run keeps at 0 (plan 25.2 point 4).
static void clean_line(int64_t now) {
    static int64_t last;
    if (now - last < 10000000) return;
    last = now;
    int copies = 0, not_whole = 0, dead = 0, unread = 0;
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || e->local != L_LIVE || e->original || !e->np) continue;
        copies++;
        uint64_t g0 = 0;
        int whole = 1;
        for (int r = 0; r < e->np; r++) {
            uint64_t a = e->parts[r], t = 0, spr = 0, g = 0;
            int32_t st = 0;
            if (!a || !coop_rd(a + 0x24, &st, 4) || !coop_rd(a + 0x228, &t, 8) || !coop_rd(t + 0x480, &spr, 8) || !coop_rd(spr + 0x28, &g, 8)) { unread++, whole = 0; continue; }
            if (st < 2 || st > 6) dead++;
            if (!g || (r && g != g0)) whole = 0;
            if (!r) g0 = g;
        }
        uint32_t gn = 0;
        if (whole && g0 && coop_rd(g0 + 0x58, &gn, 4) && (int)gn != e->np) whole = 0;
        not_whole += !whole;
    }
    plat_mutex_unlock(&reg_lock);
    uint32_t od = 0, idr = 0, g1[9];
    coop_net_ctr_drops(&od, &idr);
    coop_g1_counts(g1);
    rt_log("coop: clean: %.0f s: %d copies; refused deaths %u, copies not whole %d, copy parts gone %d, unreadable %d, channel drops %u/%u; "
           "writes kept off followers (G1): velocity %u/%u/%u, impulse %u/%u/%u, force %u/%u, transform %u",
           now / 1e6, copies, coop_life_refused(), not_whole, dead, unread, od, idr, g1[0], g1[1], g1[2], g1[3], g1[4], g1[5], g1[6], g1[7], g1[8]);
}
// TOTK_COOP_TEST_CPTOGGLE=<s>[,<s2>]: at each time the toggle request a hit makes (DeviceSwitch +0x8d bit 3) is set on
// part 1 of the first build this game follows, as a hit by this game's player would.
static void cptoggle_test(void) {
    static double at[2] = {-1, 0};
    static int done;
    if (at[0] < 0) { const char *e = cfg_get("TOTK_COOP_TEST_CPTOGGLE"); at[0] = 0; if (e) sscanf(e, "%lf,%lf", &at[0], &at[1]); }
    double t = frame_step_world_us() / 1e6;
    if (done >= 2 || at[done] <= 0 || t < at[done]) return;
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || runs(e) || e->local != L_LIVE || e->np < 2) continue;
        uint64_t d = part_switch(e->parts[1]);
        if (d) __atomic_fetch_or((uint8_t *)(uintptr_t)(d + 0x8d), 8, __ATOMIC_SEQ_CST);
        rt_log("coop: test: cptoggle: %.2f s the toggle request set on copy part 1 of %08x", t, cid32(e));
        done++;
        break;
    }
    plat_mutex_unlock(&reg_lock);
}
void coop_ctn_tick(Cpu *c) {
    if (!ctn_on() || !coop_ctr_me()) return;
    int64_t now = frame_step_world_us();
    clean_line(now);
    receive();
    retire_pass(c);
    authority();
    lone_tick();   // (before the lists: a lone copy first heard of is in the no-charge list the same tick)
    lists_rebuild(c);
    cptoggle_test();
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        plat_mutex_lock(&reg_lock);
        // (a copy still being made is held where the runner's is, so one made in the air does not fall meanwhile)
        int drive = e->used && !runs(e) && e->np && (e->local == L_LIVE || (e->local == L_MAKING && mk.st >= 2)) && !e->touched && e->hus[2] && now - e->heard_us < 1000000;
        int devs = e->used && !runs(e) && e->local == L_LIVE && e->np;
        plat_mutex_unlock(&reg_lock);
        float err = 0;
        if (drive) follow_rigid(c, e, now, &err);
        if (devs) dev_follow(c, e, now);
    }
    floats_follow(NULL);
    events_out();
    send_states();
}
int coop_ctn_first(int run_here, uint64_t parts[21]) {   // for the tests: the first live build this game runs (or follows)
    int n = 0;
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N && !n; i++)
        if (reg[i].used && reg[i].local == L_LIVE && reg[i].np >= 2 && !!runs(&reg[i]) == !!run_here) memcpy(parts, reg[i].parts, sizeof reg[i].parts), n = reg[i].np;
    plat_mutex_unlock(&reg_lock);
    return n;
}

int coop_ctn_core(void) { return ctn_on(); }
