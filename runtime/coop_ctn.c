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
void coop_deleted_here(uint64_t a);   // coop_players.c: not told to the others as destroyed
uint64_t coop_life_ability(void);
void coop_life_free_confirm(int on);
void coop_life_veto_rebuild(Cpu *c, const uint64_t *parts, int nparts);
uint32_t coop_life_refused(void);
extern uint8_t coop_ab_cai[0x1820];
extern _Atomic int64_t coop_ultrahand_hold_us;

static int ctn_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_CONTRAPTION") && *cfg_get("TOTK_COOP_CONTRAPTION") == '1';
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
    uint32_t fw, fk;        // at the runner: which parts are wheels / sticks, and their floats (StMsg)
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
    coop_deleted_here(a);
    if (!(coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, 0, 0, 0) & 1)) return 0;
    made_forget(a);
    return 1;
}
// Every part of e deleted; 1 once none is left to ask (a part whose delete was accepted is not asked again)
static int parts_delete(Cpu *c, Ent *e) {
    int left = 0;
    for (int r = 0; r < e->np && r < 21; r++) {
        if (e->del_ok >> r & 1) continue;
        if (!alive_as(e->parts[r], e->gen[r])) { made_forget(e->parts[r]), e->del_ok |= 1u << r; continue; }
        if (part_delete(c, e->parts[r])) e->del_ok |= 1u << r;
        else left++;
    }
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
static void bb_set_f32(uint64_t bb, uint32_t hash, float v) {
    int i = bb_index(bb, BB_FLOATS, hash);
    uint64_t arr = i >= 0 ? coop_rd_ptr(bb + BB_FLOATS + 8) : 0;
    if (arr && v == v) memcpy((void *)(uintptr_t)(arr + 4 * (uint64_t)i), &v, 4);
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
    uint32_t sw, on, flags, pad;
    uint32_t fw, fk;        // bit r: part r is a wheel / a steering stick, its floats in pf[r]
    float pf[21][4];        // wheel: model spin, model steering angle; stick: x, y, scaled x, scaled y
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
    rt_log("coop: contraption: %08x is run by %s now (change %u)", cid32(e), runs(e) ? "this game" : "another player's game", epoch);
}

// ---- The runner: records and versions (27.3 R1, R4, R5) ---------------------------------------------------------------
// Every 2 s, from actor phase 0: each group among this game's own objects and the builds it runs is recorded with the
// history writer and compared with the entry it belongs to.
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
    if (off || !me || now - last < 2000000) return;
    last = now;
    static uint64_t cand[256 + REG_N * 21];
    uint64_t groups[32];
    int n = 0, ng = 0;
    {
        uint64_t own[256];
        int no = coop_own_sent_objects(own, 256);
        for (int i = 0; i < no; i++) if (!coop_is_copy(own[i]) && !coop_contraption_made(own[i])) cand[n++] = own[i];
    }
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++)
        for (int r = 0; reg[i].used && runs(&reg[i]) && reg[i].local == L_LIVE && r < reg[i].np; r++) if (alive(reg[i].parts[r])) cand[n++] = reg[i].parts[r];
    plat_mutex_unlock(&reg_lock);
    int claimed[REG_N] = {0};
    for (int i = 0; i < n; i++) {
        uint64_t a = cand[i], g = part_group(a);
        if (!g) continue;
        int seen = 0;
        for (int k = 0; k < ng; k++) seen |= groups[k] == g;
        if (seen || ng >= 32) continue;
        groups[ng++] = g;
        // Its members in the order the history writer gives them (main+0x1913694): the group's members (group +0x60,
        // count +0x58) sorted by name ([actor+0x218], byte order) with swaps of unequal neighbours only, so parts of
        // one name keep the group's order (its comparator main+0x1914be0); one record part per member.
        uint64_t used[21] = {0};
        int nmem = 0;
        {
            int32_t gcount = 0;
            uint64_t arr = coop_rd_ptr(g + 0x60);
            if (!arr || !coop_rd(g + 0x58, &gcount, 4) || gcount < 2 || gcount > 21) continue;
            for (int j = 0; j < gcount && nmem < 21; j++) {
                uint64_t b = link_actor(coop_rd_ptr(arr + 8 * (uint64_t)j));
                if (b) used[nmem++] = b;
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
        // Whose it is: the entry most of its members belong to (R4, R5). A group holding a part of a build another game
        // runs is not this game's to record (R3 asks for it first).
        uint32_t ids[21] = {0}, best_serial = 0;
        uint64_t oinc[21] = {0}, best_inc = 0;
        int known[21] = {0};
        plat_mutex_lock(&reg_lock);
        int best = -1, best_n = 0, foreign = 0;
        for (int q = 0; q < REG_N; q++) {
            if (!reg[q].used || reg[q].local != L_LIVE) continue;
            int shared = 0;
            for (int r = 0; r < nmem; r++)
                for (int u = 0; u < reg[q].np; u++) shared += used[r] == reg[q].parts[u];
            if (shared && !runs(&reg[q])) foreign = 1;
            if (shared > best_n && !claimed[q]) best_n = shared, best = q;
        }
        if (best >= 0) best_inc = reg[best].inc, best_serial = reg[best].serial;
        for (int r = 0; r < nmem; r++) known[r] = origin_of(used[r], &ids[r], &oinc[r]);
        plat_mutex_unlock(&reg_lock);
        if (foreign) continue;
        static _Alignas(16) uint8_t link[0x40], rec[0x3000];
        static _Alignas(16) float align[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, info[12];
        memset(link, 0, sizeof link), memset(rec, 0, sizeof rec), memset(link + 0x10, 0xff, 4), link[0x14] = 1;
        coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, (uint64_t)(uintptr_t)link, a, 1, 0);
        uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + HISTORY_MGR_SLOT));
        if (!(call5(c, MOD_MAIN_BASE + HISTORY_WRITE, mgr, (uint64_t)(uintptr_t)link, (uint64_t)(uintptr_t)align, (uint64_t)(uintptr_t)rec,
                    (uint64_t)(uintptr_t)info) & 1) || rec[9] < 2) continue;
        // (the writer leaves the header to its caller: magic and version 2, as the Autobuild's loader checks them)
        memcpy(rec, "CmbAct", 6), rec[6] = 0, rec[7] = 0, rec[8] = 2;
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
        claimed[e - reg] = 1;
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
            e->v++, e->shape = shape, e->green = green, e->np = nmem, e->resend = 1;
            memcpy(e->rec, rec, BP_SIZE), memcpy(e->parts, used, sizeof e->parts), memcpy(e->ids, ids, sizeof e->ids), memcpy(e->oinc, oinc, sizeof e->oinc);
            e->del_ok = 0;
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
                if (grouped) e->gone_us = now;   // (still a build: the writer refused it this time, or it is changing)
                if (now - e->gone_us > 3000000) {
                    if (live_parts) {
                        send_ctl(COOP_CTR_RETIRE, e, e->epoch, me);
                        for (int r = 0; r < e->np; r++) if (!part_group(e->parts[r])) made_forget(e->parts[r]);
                    }
                    rt_log("coop: contraption: %08x, run here, is no build any more (%d of %d parts alive): %s", cid32(e), live_parts, e->np,
                           live_parts ? "retired" : "lapsed");
                    free(e->fs), memset(e, 0, sizeof *e);
                }
            }
        }
        plat_mutex_unlock(&reg_lock);
    }
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
        for (int i = 0; i < REG_N && !e; i++) if (reg[i].used && reg[i].want_make && reg[i].np) e = &reg[i];
        for (int i = 0; i < REG_N && !e; i++) if (reg[i].used && reg[i].want_make && reg[i].absent != 1) e = &reg[i];   // (not where the engine unloads it)
        plat_mutex_unlock(&reg_lock);
        if (!e) return;
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
                return;
            }
            plat_mutex_lock(&reg_lock);
            rt_log("coop: contraption: %08x changed shape (version %u): its %d parts here deleted, made again once gone", cid32(e), e->in_v, e->np);
            memcpy(e->old, e->parts, sizeof e->old), e->old_us = now;
            e->np = 0, e->local = L_NONE, e->original = 0, e->v = 0, e->del_ok = 0, memset(e->parts, 0, sizeof e->parts);
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
        memcpy(e->parts, mk.parts, sizeof e->parts), e->np = mk.n, e->del_ok = 0, e->absent = 0;
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
        if (now - mk.t > 3000000) {
            rt_log("coop: contraption: %08x: made, %d of %d parts in one group", cid32(e), grouped, mk.n);
            plat_mutex_lock(&reg_lock);
            if (e->local == L_MAKING) e->local = L_LIVE;
            e->changed_us = 0, e->remade_us = now;
            plat_mutex_unlock(&reg_lock);
            mk.st = 0, mk.n = 0, mk.e = NULL;
        }
    }
}
static void dev_flush(Cpu *c);
static void floats_follow(void);
void coop_ctn_phase0(Cpu *c) {
    if (!ctn_on()) return;
    record_pass(c);
    make_pass(c);
    dev_flush(c);
    floats_follow();
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
int coop_contraption_follower(uint64_t a) {
    if (!a || !coop_contraption_made(a)) return 0;
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
            yes = (reg[i].v && reg[i].ids[r] == id && reg[i].oinc[r] == oinc) || (reg[i].want_make && reg[i].in_ids[r] == id && reg[i].in_oinc[r] == oinc);
    }
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
    if (!found) return;
    m.want = !part_on(actor);   // (a toggle on the copy as it shows: the other state)
    coop_ctr_send(0, COOP_CTR_TOGGLE, COOP_CTR_RELIABLE, &m, sizeof m);
    rt_log("coop: contraption: a toggle asked on %08x part %u here: sent to the game that runs it, wanted %s (switch %llx)", id, m.part, m.want ? "on" : "off",
           (unsigned long long)comp);
}

// The wheels and sticks of the builds followed here shown as where they run (at actor phase 0 and at the co-op tick:
// when in the frame the stick's own graph writes its floats is not read, so both)
static void floats_follow(void) {
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || runs(e) || e->local != L_LIVE || e->touched || !(e->fw | e->fk)) continue;
        for (int r = 0; r < e->np && r < 21; r++) {
            uint64_t bb = (e->fw | e->fk) >> r & 1 ? actor_bb(e->parts[r]) : 0;
            if (!bb) continue;
            if (e->fw >> r & 1) bb_set_f32(bb, BB_WHEEL_SPIN, e->pf[r][0]), bb_set_f32(bb, BB_WHEEL_STEER, e->pf[r][1]);
            else bb_set_f32(bb, BB_STICK_X, e->pf[r][0]), bb_set_f32(bb, BB_STICK_Y, e->pf[r][1]), bb_set_f32(bb, BB_STICK_SX, e->pf[r][2]), bb_set_f32(bb, BB_STICK_SY, e->pf[r][3]);
        }
    }
    plat_mutex_unlock(&reg_lock);
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
        if (e && !runs(e) && m.from == e->runner && !(mk.st && mk.e == e)) e->want_make = 0, e->in_v = 0xffffffffu, e->gone_us = now;   // (deleted at phase 0: retire_pass)
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
        if (s.flags & 2) e->sw = s.sw, e->on = s.on, e->payer = s.payer, e->fw = s.fw, e->fk = s.fk, memcpy(e->pf, s.pf, sizeof e->pf);
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
            s.key = e->key64, s.inc = e->inc, s.serial = e->serial, s.epoch = e->epoch, s.v = e->v, s.shape = e->shape, s.us = now, s.flags = 2;
            for (int r = 0; r < e->np; r++)
                if (part_switch(e->parts[r])) s.sw |= 1u << r, s.on |= (uint32_t)part_on(e->parts[r]) << r;
            for (int r = 0; r < e->np; r++) {
                uint64_t bb = actor_bb(e->parts[r]);
                if (bb_f32(bb, BB_WHEEL_SPIN, &s.pf[r][0])) s.fw |= 1u << r, bb_f32(bb, BB_WHEEL_STEER, &s.pf[r][1]);
                else if (bb_f32(bb, BB_STICK_X, &s.pf[r][0]))
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
            else made_forget(e->parts[r]), e->parts[r] = 0;
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
        } else if (differs && !mine && now - e->changed_us > 3000000 && now - e->remade_us > 10000000 && !e->want_make && !(mk.st && mk.e == e)) {
            // changed by nothing this game's player did: made again from the version held
            rt_log("coop: contraption: %08x is not whole here (%d members, %d parts): made again", cid32(e), gn, e->np);
            memcpy(e->in_rec, e->rec, BP_SIZE), memcpy(e->in_centre, e->centre, sizeof e->centre);
            memcpy(e->in_ids, e->ids, sizeof e->in_ids), memcpy(e->in_oinc, e->oinc, sizeof e->in_oinc);
            e->in_v = e->v, e->in_shape = e->shape, e->in_green = e->green, e->want_make = 1, e->remade_us = now;
        }
    }
    plat_mutex_unlock(&reg_lock);
}
static void retire_pass(Cpu *c) {   // builds their runner retired: the parts here deleted, the entry freed
    plat_mutex_lock(&reg_lock);
    for (int i = 0; i < REG_N; i++) {
        Ent *e = &reg[i];
        if (!e->used || e->in_v != 0xffffffffu || (mk.st && mk.e == e)) continue;
        plat_mutex_unlock(&reg_lock);   // (the request runs game code, which asks this file about parts)
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
    for (int i = 0; i < DRIVEN_N; i++) atomic_store_explicit(&nocharge[i], i < nb ? b[i] : 0, memory_order_relaxed);
    for (int i = 0; i < REG_N; i++) {   // (the groups of the builds followed here: coop_ctn_sender_entry)
        uint64_t g = reg[i].used && !runs(&reg[i]) && reg[i].local == L_LIVE && reg[i].np ? part_group(reg[i].parts[0]) : 0;
        atomic_store_explicit(&follow_group[i], g, memory_order_relaxed);
    }
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
    floats_follow();
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
