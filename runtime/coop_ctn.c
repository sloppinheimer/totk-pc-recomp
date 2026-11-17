// Co-op: contraptions, what was kept of the v3 code (docs/coop/contraption-sync-v4-spec.md sections 9, 11 and 12; the v3
// plan is docs/coop/contraption-sync-v3.md, an archive). TOTK_COOP_CONTRAPTION=0 turns it off. TotK 1.4.3 main offsets.
//
// The table of entities, who owns what and what is said between the games are in coop_ent*.c. This file holds the
// code that touches the game for them and that the v3 runs showed to work: the rigid-field drive of a copy, the
// devices of a copy set as where it is owned (switches, wheels, sticks, time bombs, springs, burning), the deaths
// replayed on copies, the copy maker (the game's own Autobuild part maker and confirm, from the owner's record), the
// reading of a part's glue edges, and the gate G1 that keeps game code from moving a copy's bodies.
// It works from the group views (Grp, coop_ent.h): one row per island here, rebuilt each frame by coop_ent.c.
//
// Threads: ctn_phase0_kept and the maker run at actor phase 0; ctn_drive on the co-op tick; coop_g1 and the other
// gates on any game thread, reading the published sets only.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_sess.h"
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

#include "coop_ent.h"

void coop_lock_wait(PlatMutex *m);    // coop_spawn.c
void coop_deleted_here(uint64_t a);   // coop_players.c: not told to the others as destroyed
uint64_t coop_life_ability(void);
void coop_life_free_confirm(int on);
extern uint8_t coop_ab_cai[0x1820];
void coop_ent_creating(int on);
void coop_ent_toggle_seen(uint64_t actor, int want);
void coop_ent_switch_seen(uint64_t actor, uint8_t bits);
void coop_ent_death_seen(uint64_t actor, uint32_t kind);
void coop_ent_structure_event(void);
static void springs_phase0(Cpu *c);
static int gauge_blocked(void);

static int ctn_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *set = cfg_get("TOTK_COOP_CONTRAPTION");   // on unless set to 0
        on = !(set && *set == '0');
    }
    return on;
}

typedef struct FollowState {
    float v_written[21][3]; // the velocity written to each part's main body last frame
    int have_written;
    float gc;               // the vertical part of comp, for the log
    float comp[3];          // what one physics step takes off the written velocity (gravity, damping), added back, filtered
    float integ[3];         // the integral term (m/s)
    int rest;               // 1: resting (nothing written)
    int64_t still_since;    // since when the runner's root has been still (its clock)
    int64_t off_since;      // since when the runner has rested with the copy out of the rest band (this game's clock), 0: not so
    int rest_leaves;        // how often the copy has left its rest while the runner went on resting
    int no_rest;            // 1: it left three times: held by the field until the runner moves
    int placed;             // how often it was put in place in this rest of the runner
    int64_t stuck_since;    // since when the copy has not moved though a velocity is written to it, 0: not so
    int64_t hold_until;     // just put in place: held still until then
    int64_t last_us;
} FollowState;


// The kept code below was written against the v3 registry; a group view stands where a registry entry stood.
#define REG_N GRP_N
#define reg grps
static int runs(const Grp *e) { return e->mine; }
static uint32_t cid32(const Grp *e) { return (uint32_t)(e->key64 ^ e->key64 >> 32); }   // (for the log)
static void queue_full(const char *what) {   // a full queue drops what comes: said, at most every 10 s (review 2 finding 22)
    static _Atomic int64_t said;
    int64_t now = frame_step_world_us();
    if (now - atomic_load(&said) > 10000000) atomic_store(&said, now), rt_log("coop: contraption: the queue of %s is full: dropped", what);
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
    if (!alive(a) || !coop_rd_ptr(a + 0x1a8)) return 1;
    // Already asked and accepted (actor +0x28 bit 17): the game answers 0 to a second ask, which is no refusal. It goes
    // once its locks let it (research notes sync-v3/m2/p7/delete-refused, Q1 and Q3). Taken as refused, one such part
    // held up every later build (plan 37, note 12).
    {
        uint32_t f28 = 0;
        if (coop_rd(a + 0x28, &f28, 4) && (f28 >> 17 & 1)) return 1;
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
    return 1;
}
// Parts whose delete the game kept refusing, left where they are: still copies (marked as made) while they are that
// actor, forgotten once gone, so that another actor at the address later is not taken for a copy (review 1 finding 33)
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
// This game's Link holds a with Ultrahand: SPR +0x20 bit 16, and the holder handle at [SPR +0x30] +0x10 this Link
int coop_held_here(uint64_t a) {
    uint64_t spr = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) : 0, me = coop_player_actor();
    uint64_t f = 0;
    if (!spr || !me || !coop_rd(spr + 0x20, &f, 8) || !(f & 0x10000)) return 0;
    uint64_t h = coop_rd_ptr(spr + 0x30), slot = h ? coop_rd_ptr(h + 0x10 + 8) : 0;
    uint32_t id = 0, sid = 0;
    return slot && coop_rd(h + 0x10 + 0x10, &id, 4) && id != ~0u && coop_rd(slot + 0x48, &sid, 4) && sid == id && coop_rd_ptr(slot + 0x40) == me;
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
static uint64_t call5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {   // as coop_guest_call
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 5, a0, a1, a2, a3, a4);
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
static float delete_radius(uint64_t a) {
    uint64_t P = a ? coop_rd_ptr(a + 0x370) : 0, info = P ? coop_rd_ptr(P + 0x18) : 0, R = info ? coop_rd_ptr(info + 0xc8) : 0;
    float disp = -1, off = -1;
    if (!R || !coop_rd(R + 0xac, &disp, 4) || !coop_rd(R + 0xa8, &off, 4) || disp != disp || off != off || disp < 0 || off < 0) return 0;
    return disp + off;
}
static uint64_t body_owner(uint64_t body) { return body ? link_actor(coop_rd_ptr(body + 0x110) + 0x18) : 0; }
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
static int follow_rigid(Cpu *c, Grp *e, int64_t now, float *err_out) {
    float hm[3][12];
    int64_t hus[3], off;
    uint64_t parts[21];
    uint32_t gens[21], skip;
    int np, cls, hv_ok;
    float hv[3];
    uint32_t key;
    FollowState *fs;
    coop_lock_wait(&grp_lock);
    memcpy(hm, e->hm, sizeof hm), memcpy(hus, e->hus, sizeof hus), off = e->clock_off, cls = e->cls;
    memcpy(hv, e->hv, sizeof hv), hv_ok = e->hv_ok;
    np = e->n < 21 ? e->n : 21, key = cid32(e);
    memcpy(parts, e->parts, sizeof parts), memcpy(gens, e->gen, sizeof gens), skip = e->no_write;
    if (!e->fs) e->fs = calloc(1, sizeof(FollowState));
    fs = e->fs;
    plat_mutex_unlock(&grp_lock);
    if (!fs || !hus[2] || !np) return 0;
    // The co-op tick comes twice a frame at one clock value, before and after the physics step. The first write is the
    // one a step runs on; a second one was replaced before any step used it and gave the step observer a write to
    // compare with that no step had seen.
    if (fs->last_us == now) return 0;
    // Just put in place: held still there for 0.4 s, and counted as resting, so that the list of copies that pass
    // through each other (rebuilt four times a second) has it as a resting one before it is let go: let go at once, a
    // copy whose place is against another copy fell through it and left its rest again.
    // (the timers of a drive that was not run for a while, as while this player's take was open, are not carried over)
    if (fs->last_us && now - fs->last_us > 500000) fs->off_since = fs->stuck_since = 0, fs->have_written = 0, fs->hold_until = 0;
    if (now < fs->hold_until && cls == CLS_IDLE) {   // (not once its owner has taken it up: the copy goes with the hand)
        _Alignas(16) static const float zero[4] = {0, 0, 0, 0};
        for (int j = 0; j < np; j++) {
            uint64_t b = (skip >> j & 1) || !alive_as(parts[j], gens[j]) ? 0 : main_body_of(parts[j]);
            if (!b) continue;
            sync_writing = 1;
            coop_guest_call(c, MOD_MAIN_BASE + 0x567750, b, (uint64_t)(uintptr_t)zero, 0, 0);   // SetLinearVelocity
            coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, b, (uint64_t)(uintptr_t)zero, 0, 0);   // SetAngularVelocity
            sync_writing = 0;
        }
        fs->last_us = now, fs->have_written = 0;
        return 0;
    }
    // the sample's age by the arrival offset's minimum (the jitter is taken out, the base delay is not: plan 3.2)
    double age = (now - (hus[2] + off)) / 1e6;
    double tau = clampf((float)age, 0, 0.25f);
    // the runner's velocity, acceleration and turn rate from its last samples
    float v_r[3] = {0}, a_r[3] = {0}, w_r[3] = {0};
    double d1 = hus[1] && hus[2] > hus[1] ? (hus[2] - hus[1]) / 1e6 : 0;
    if (d1 > 0.005 && d1 < 0.5) {
        for (int i = 0; i < 3; i++) v_r[i] = (hm[2][i * 4 + 3] - hm[1][i * 4 + 3]) / (float)d1;
        float r1[9], r2[9], r1t[9], dr[9];
        rot_of(hm[1], r1), rot_of(hm[2], r2);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r1t[i * 3 + j] = r1[j * 3 + i];
        mat_mul3(r2, r1t, dr), log_rot(dr, w_r);
        for (int i = 0; i < 3; i++) w_r[i] /= (float)d1;
    }
    // The linear velocity is the one the owner read from its root body, when the pose carries it. Taken from the
    // difference of two poses 33 ms apart it stepped by 3 m/s at a typical pose (13 m/s at one in ten), and an
    // acceleration taken from three poses sat at its clamp: no acceleration is used.
    float v_d[3] = {v_r[0], v_r[1], v_r[2]};   // (the difference of the last two poses, kept for the trace: the sent one is checked against it)
    if (hv_ok) memcpy(v_r, hv, sizeof v_r);
    // A velocity is good for the pose it came with. An owner whose object stops goes to one pose a second, and the
    // last pose before that may carry the speed of a moment: carried on for that second it put the target 0.5 m
    // past the object (a bench run). Past 0.1 s of age the velocities fade, to none at 0.3 s.
    if (age > 0.1) {
        float keep = age >= 0.3 ? 0 : (float)((0.3 - age) / 0.2);
        for (int i = 0; i < 3; i++) v_r[i] *= keep, w_r[i] *= keep;
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
    if (ep > 10.0f) memset(fs->integ, 0, sizeof fs->integ), memset(fs->comp, 0, sizeof fs->comp);   // (the caller teleports it: nothing built up is kept)
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
    // (0.1 m/s for a velocity the owner read from its body: a body that rests awake does not read exactly none)
    if (vn < (hv_ok ? 0.1f : 0.05f) && wn < 0.05f) { if (!fs->still_since) fs->still_since = hus[2]; }
    else fs->still_since = 0;
    // An object its owner holds or rides is never at rest: held still in the air it met the rule, nothing was written,
    // the copy fell out of the band and was caught again, over and over.
    int runner_rests = cls == CLS_IDLE && fs->still_since && hus[2] - fs->still_since >= 500000;
    const float deg = 180.0f / 3.14159265f;
    // (a wide band: a copy resting on the ground a few centimetres from where the runner's rests cannot be pushed
    // there through its contacts, and trying makes it chatter; plan RK2)
    // A copy that leaves its rest three times while the runner goes on resting cannot rest where the runner's object
    // does (what holds that one up is not under the copy): left alone it fell out of the band, was brought back and
    // let go again, four times a second. It is held by the field until the runner moves.
    int in_band = ep < 0.10f && er * deg < 4;
    if (!runner_rests) fs->rest_leaves = 0, fs->no_rest = 0;
    if (fs->rest && (!runner_rests || ep > 0.25f || er * deg > 8)) {
        fs->rest = 0;
        if (runner_rests && ++fs->rest_leaves >= 3 && !fs->no_rest) {
            fs->no_rest = 1;
            rt_log("coop: follow: copy %08x left its rest three times (%.3f m, %.1f deg): held by the drive until its owner's object moves", key, ep, er * deg);
        }
    } else if (!fs->rest && runner_rests && !fs->no_rest && in_band) fs->rest = 1;
    // The runner rests and the copy has stayed out of the entry band for 2 s, resting or not: the field cannot bring it
    // there (a copy tipped over lies on its contacts at another angle), or it rests inside the wider leave band (a copy
    // lay 5.8 degrees off for a whole bench run). It is put in place once; the caller marks it for phase 0. The third
    // time in one rest of the runner it is held by the field instead, as a copy that keeps leaving its rest is.
    // Once it has been put in place in this rest of the runner, a copy that comes to lie a few degrees off again is
    // left so, up to the leave band: its own resting place on the ground is not the runner's to the degree (a fan on
    // its edge lay 5 degrees off after every placement, and was put back every 2 s without end).
    // A copy is put in place three times at most in one rest of the runner; after that it is held by the field where
    // it can be (code review 3: nothing ended the placements of a copy that cannot stay, one every 2.4 s). And never
    // while this game's Link stands within 1.5 m of the place: the copy is solid to this Link (in its owner's game this
    // player's stand-in is not), and was put inside the Link again and again.
    int out_of_place = fs->placed ? ep > 0.25f || er * deg > 8 : !in_band, may_place = fs->placed < 3;
    {
        float lp[3];
        uint64_t link = coop_player_actor();
        if (link && coop_rd(link + COOP_ACTOR_POS, lp, 12)) {
            float dx = lp[0] - hm[2][3], dy = lp[1] - hm[2][7], dz = lp[2] - hm[2][11];
            if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) may_place = 0;
        }
    }
    if (!runner_rests) fs->placed = 0;
    if (runner_rests && out_of_place && fs->placed >= 3 && !fs->no_rest) {
        fs->no_rest = 1, fs->rest = 0;
        rt_log("coop: follow: copy %08x put in place three times in one rest of its owner's object: held by the drive from here", key);
    }
    if (!runner_rests || !out_of_place || !may_place) fs->off_since = 0;
    else if (!fs->off_since) fs->off_since = now;
    else if (now - fs->off_since > 2000000) {
        fs->off_since = 0, fs->placed++;
        rt_log("coop: follow: copy %08x out of place for 2 s at rest (%.3f m, %.1f deg): put in place", key, ep, er * deg);
        // (what the field had built up against the contact that held the copy is dropped: left in, it threw the
        // copy 0.4 m the moment the placement freed it)
        memset(fs->integ, 0, sizeof fs->integ), memset(fs->comp, 0, sizeof fs->comp), fs->have_written = 0;
        if (!fs->no_rest) fs->rest = 1;
        fs->hold_until = now + 400000;
        return 2;
    }
    if (fs->rest) { fs->have_written = 0, fs->stuck_since = 0; for (int i = 0; i < 3; i++) fs->integ[i] *= (float)(1 - dt); return 0; }
    // A copy that does not move when it is told to: over 0.25 m off, over 1 m/s written last frame, and its root's
    // velocity now under a third of that, for 0.5 s. Something holds it that did not hold the runner's object (a bench
    // run: 3 m/s written for 2 s, nothing moved, 0.49 m off). It is put in place at once, as by the 2 s rule.
    {
        float vb0[3] = {0}, vw = sqrtf(fs->v_written[0][0] * fs->v_written[0][0] + fs->v_written[0][1] * fs->v_written[0][1] + fs->v_written[0][2] * fs->v_written[0][2]);
        int coop_runs_for_ms(void);   // coop_players.c: -1 while this game sits in a menu (nothing moves then)
        // Only while the runner rests (code review 3): a copy carried past an edge is held up for a moment too, and
        // put at the runner's last pose and held still it fell behind its owner and hung in the air; and counted with
        // the 2 s rule's placements, under the same limits.
        int stuck = runner_rests && may_place && fs->have_written && coop_runs_for_ms() > 500 && ep > 0.25f && vw > 1.0f && coop_rd(rb + 0xf8, vb0, sizeof vb0) &&
                    sqrtf(vb0[0] * vb0[0] + vb0[1] * vb0[1] + vb0[2] * vb0[2]) < vw / 3;
        if (!stuck) fs->stuck_since = 0;
        else if (!fs->stuck_since) fs->stuck_since = now;
        else if (now - fs->stuck_since > 500000) {
            fs->stuck_since = 0, fs->off_since = 0, fs->placed++;
            rt_log("coop: follow: copy %08x does not move when written to (%.3f m off, %.1f m/s written): put in place", key, ep, vw);
            memset(fs->integ, 0, sizeof fs->integ), memset(fs->comp, 0, sizeof fs->comp), fs->have_written = 0;
            if (runner_rests && !fs->no_rest) fs->rest = 1;
            fs->hold_until = now + 400000;
            return 2;
        }
    }
    // the field: V, W with bounded corrections (gains fall with speed)
    float kp = vn <= 5 ? 5 : vn >= 15 ? 2.5f : 5 - 2.5f * (vn - 5) / 10, kr = wn <= 2 ? 5 : wn >= 6 ? 3 : 5 - 2 * (wn - 2) / 4;
    float cv[3], cw[3];
    for (int i = 0; i < 3; i++) {
        // (no build-up while the runner rests: a copy held by a contact a few centimetres off took a growing push,
        // 1.5 m/s after 2 s, that moved nothing)
        if (runner_rests) fs->integ[i] *= 0.9f;
        else fs->integ[i] = clampf(fs->integ[i] + 3.0f * e_p[i] * (float)dt, -1.0f, 1.0f);
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
        uint64_t b = (skip >> j & 1) || !alive_as(parts[j], gens[j]) ? 0 : main_body_of(parts[j]);
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
    // "vd" is the owner's velocity as the difference of its last two poses and "sent" whether "vr" is the one it sent.
    // TOTK_COOP_FOLLOW_TRACE=1: one line for every frame the drive writes, to read a jitter from. "sample" is the
    // owner's clock of the newest pose (a new value: a pose arrived this frame); "vb" is the root body's velocity
    // as read before this frame's write, so against the last line's V it shows what the physics step made of it.
    static int trace = -1;
    if (trace < 0) { const char *t = cfg_get("TOTK_COOP_FOLLOW_TRACE"); trace = t && *t == '1'; }
    if (trace) {
        float vb[3] = {0};
        coop_rd(rb + 0xf8, vb, sizeof vb);
        rt_log("coop: ftrace: %lld %08x cls %d rests %d dt %.4f sample %lld age %.4f | e %.3f %.3f %.3f (%.3f m) rot %.2f %.2f %.2f (%.2f deg) | "
               "vr %.2f %.2f %.2f ar %.1f %.1f %.1f wr %.2f %.2f %.2f | v* %.2f %.2f %.2f cv %.2f %.2f %.2f comp %.2f %.2f %.2f I %.2f %.2f %.2f | "
               "V %.2f %.2f %.2f W %.2f %.2f %.2f | vb %.2f %.2f %.2f | vd %.2f %.2f %.2f sent %d",
               (long long)now, key, cls, runner_rests, dt, (long long)hus[2], age, e_p[0], e_p[1], e_p[2], ep, e_r[0] * deg, e_r[1] * deg, e_r[2] * deg, er * deg,
               v_r[0], v_r[1], v_r[2], a_r[0], a_r[1], a_r[2], w_r[0], w_r[1], w_r[2], v_star[0], v_star[1], v_star[2], cv[0], cv[1], cv[2],
               fs->comp[0], fs->comp[1], fs->comp[2], fs->integ[0], fs->integ[1], fs->integ[2], V[0], V[1], V[2], W[0], W[1], W[2], vb[0], vb[1], vb[2], v_d[0], v_d[1], v_d[2], hv_ok);
    }
    static int64_t said;
    if (coop_debug() && now - said > 500000) {
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

// ---- Gates read from game threads: the sets coop_ent.c publishes each frame (specification section 11) ----------------
static _Atomic uint64_t mk_pub[21];   // the parts of the batch the maker is making now (no entity has them yet)
static int mk_part(uint64_t a) {
    for (int i = 0; a && i < 21; i++) if (atomic_load_explicit(&mk_pub[i], memory_order_relaxed) == a) return 1;
    return 0;
}
int coop_ctn_driven(uint64_t a) { return ent_set_has(ENT_SET_DRIVEN, a); }       // a copy this game moves by the drive
int coop_ctn_no_charge(uint64_t a) { return ent_set_has(ENT_SET_NOCHARGE, a); }   // its devices are paid by another player
int coop_ctn_runs_part(uint64_t a) { return ent_set_has(ENT_SET_OWNED, a); }      // an entity this game owns
int coop_ctn_parts_all(uint64_t *out, int max) { return ent_set_list(ENT_SET_ENT, out, max); }
int coop_ctn_part(uint64_t a) { return ent_set_has(ENT_SET_PART, a); }            // a member of an island of two or more
int coop_ent_is(uint64_t a) { return ent_set_has(ENT_SET_ENT, a) || mk_part(a); }
// G4: a live entity another game owns, held under a request or not, and the batch in the making
int coop_contraption_follower(uint64_t a) { return ent_set_has(ENT_SET_FOLLOWER, a) || mk_part(a); }
int coop_ctn_follows(uint64_t a) { return ent_set_has(ENT_SET_FOLLOWER, a); }
int coop_contraption_made(uint64_t a) { return ent_set_has(ENT_SET_COPY, a) || mk_part(a); }
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
    int k = -1;
    for (int i = 0; i < 9 && k < 0; i++) if (g1_fns[i] == off) k = i;
    if (k < 0 || !body) return 0;
    if (!ent_set_has(ENT_SET_DRIVEN_BODY, body)) return 0;
    uint64_t lr = c->x[30] - MOD_MAIN_BASE;
    if (lr == 0x221a9c0 || lr == 0x221aa14) return 0;   // (the spring's launches: returns after main+0x221a9bc / 0x221aa10)
    uint64_t actor = body_owner(body);
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
        unsigned k = (unsigned)(((actor >> 4) + (uint64_t)p) % NODE_N);   // (unsigned: an int went negative for an address with bit 35 set, finding 25)
        uint64_t have = atomic_load_explicit(&onoff_nodes[k].actor, memory_order_relaxed);
        if (have == actor) { atomic_store_explicit(&onoff_nodes[k].node, node, memory_order_relaxed); return; }
        if (!have || p == 3) { atomic_store(&onoff_nodes[k].node, node), atomic_store(&onoff_nodes[k].actor, actor); return; }
    }
}
static uint64_t onoff_node(uint64_t actor) {
    for (int p = 0; actor && alive(actor) && p < 4; p++) {   // (a dead actor's node may be freed: finding 27)
        unsigned k = (unsigned)(((actor >> 4) + (uint64_t)p) % NODE_N);
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
    for (int k = 0; actor && alive(actor) && k < 32; k++) {
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
#define EVQ 64
static struct { uint64_t actor; uint8_t bits; int64_t us; uint32_t gen; } ev_seen[EVQ], ev_put[EVQ];
static struct { uint64_t actor; int64_t us; } tog_seen[16];
static int ev_seen_n, ev_put_n, tog_i;
void coop_ctn_event_seen(uint64_t actor) {   // from the on/off calc of a followed part (a game thread), before the calc
    uint64_t en = event_entry(actor);
    uint8_t b = 0;
    if (!en || !coop_rd(en + 8, &b, 1) || !(b & 7)) return;
    __atomic_fetch_and((uint8_t *)(uintptr_t)(en + 8), (uint8_t)~7, __ATOMIC_SEQ_CST);
    plat_mutex_lock(&ev_lock);
    if (ev_seen_n >= EVQ) queue_full("switch events seen");
    if (ev_seen_n < EVQ) ev_seen[ev_seen_n].actor = actor, ev_seen[ev_seen_n].bits = b & 7, ev_seen[ev_seen_n].us = frame_step_world_us(), ev_seen_n++;
    plat_mutex_unlock(&ev_lock);
}
// A hit here that would switch on a device another game runs (a build's part or a lone device) while this game's gauge
// cannot pay is not sent: in the game a player whose battery is empty or recharging does not switch a device on, and
// sent, the device went on where it runs and the other player's battery paid for it (plan 43, note 17). Switching off
// is still sent.
static void refused_on(uint64_t actor) {
    static int64_t said;
    int64_t now = frame_step_world_us();
    char nm[48];
    if (now - said > 2000000) said = now, rt_log("coop: contraption: a hit here would switch on %s, run by another game, while this gauge cannot pay: not sent", coop_actor_name(actor, nm, sizeof nm));
}
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
        if ((bits & 5) && gauge_blocked()) { refused_on(actor); continue; }   // (plan 43 note 17: see refused_on)
        coop_ent_switch_seen(actor, bits);
    }
}
static void events_put(void) {   // actor phase 0, where the build runs: the events the other games sent, on the parts
    plat_mutex_lock(&ev_lock);
    for (int i = 0; i < ev_put_n; i++) {
        uint64_t en = alive_as(ev_put[i].actor, ev_put[i].gen) ? event_entry(ev_put[i].actor) : 0;
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
#define SETQ 32
static struct { uint64_t parts[21]; uint32_t gen[21]; int n; uint32_t on; char why; } setq[SETQ];   // gen: each part's id when queued (finding 23)
static int setq_n;
static void dev_set_later(const uint64_t *parts, int np, uint32_t on, char why) {   // (reg_lock held) on: bit r, part r wanted on
    if (!parts[0]) return;   // (its first part gone here: the build is about to be made again)
    for (int i = 0; i < setq_n; i++)
        if (setq[i].parts[0] == parts[0]) {
            memcpy(setq[i].parts, parts, sizeof setq[i].parts), setq[i].n = np, setq[i].on = on;
            for (int r = 0; r < 21; r++) setq[i].gen[r] = gen_of(parts[r]);
            return;
        }
    if (setq_n == SETQ) {
        static int said;
        if (!said++) rt_log("coop: contraption: the device set queue is full (%d): a set dropped", SETQ);
        return;
    }
    memcpy(setq[setq_n].parts, parts, sizeof setq[setq_n].parts), setq[setq_n].n = np, setq[setq_n].on = on, setq[setq_n].why = why;
    for (int r = 0; r < 21; r++) setq[setq_n].gen[r] = gen_of(parts[r]);
    setq_n++;
}
static void dev_flush(Cpu *c) {   // from actor phase 0
    static struct { uint64_t parts[21]; uint32_t gen[21]; int n; uint32_t on; char why; } q[SETQ];
    coop_lock_wait(&grp_lock);
    int n = setq_n;
    memcpy(q, setq, sizeof q), setq_n = 0;
    plat_mutex_unlock(&grp_lock);
    int total = 0;
    for (int i = 0; i < n; i++) {
        // (at most 40 set messages a frame: a 21-part build switching puts up to 21 on the game's actor message ring,
        // 100 by default, and a full ring drops messages with no trace; the rest wait for the next frame. Research
        // notes sync-v3/m2/p8/limits; plan 41.3 item 5)
        if (total && total + q[i].n > 40) {   // (with this entry's own count; the first entry of a frame always goes: finding 4)
            coop_lock_wait(&grp_lock);
            int newer = 0;
            for (int k = 0; k < setq_n; k++) newer |= setq[k].parts[0] == q[i].parts[0];
            if (!newer && setq_n < SETQ) memcpy(setq[setq_n].parts, q[i].parts, sizeof q[i].parts), memcpy(setq[setq_n].gen, q[i].gen, sizeof q[i].gen),
                                         setq[setq_n].n = q[i].n, setq[setq_n].on = q[i].on, setq[setq_n].why = q[i].why, setq_n++;
            else if (!newer) queue_full("device sets (held over a frame)");
            plat_mutex_unlock(&grp_lock);
            continue;
        }
        char was[24] = "";
        int sent = 0;
        for (int r = 0; r < q[i].n && r < 21; r++) {
            if (!alive_as(q[i].parts[r], q[i].gen[r])) continue;   // (gone, or another actor at its address, since it was queued: findings 28, 23)
            int sw = part_switch(q[i].parts[r]) != 0, on = sw && part_on(q[i].parts[r]), want = (int)(q[i].on >> r & 1);
            was[r] = sw ? (on ? '1' : '0') : '-', was[r + 1] = 0;
            if (sw && on != want) dev_msg(c, q[i].parts[r], want), sent++;
        }
        total += sent;
        if (q[i].why == 'a') rt_log("coop: contraption: devices asked %s here: they were %s, %d set", q[i].on ? "on" : "off", was, sent);
    }
}
// The devices of a build another game runs: each switched part on or off as there, compared twice a second
static void dev_follow(Cpu *c, Grp *e, int64_t now) {
    uint64_t parts[21];
    coop_lock_wait(&grp_lock);
    uint32_t sw = e->sw, on = e->on;
    int np = e->n, due = now - e->toggled_us > 500000;
    memcpy(parts, e->parts, sizeof parts);
    plat_mutex_unlock(&grp_lock);
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
        coop_lock_wait(&grp_lock);
        e->toggled_us = now + 500000;   // (once a second)
        plat_mutex_unlock(&grp_lock);
        coop_ent_toggle_seen(parts[0], 0);
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
    coop_lock_wait(&grp_lock);
    e->toggled_us = now;
    dev_set_later(parts, np, on & sw, 'f');
    plat_mutex_unlock(&grp_lock);
    rt_log("coop: contraption: %08x: its devices %s where it runs, %s here: set %s (%d parts, there on %#x of %#x)", cid32(e), there_on ? "on" : "off",
           here_on ? "on" : "off", there_on ? "on" : "off", set, on, sw);
}
// A hit by this game's player on a build another game runs sets, besides the device's toggle bit, the hit event in the
// build's group (group +0x880 bit 26, main+0x1c02a98), which the group's sender main+0x32eb90 would turn into on/off
// messages to every device here. The event is taken off follower groups at the sender's entry (coop_life.c): the
// runner decides, and its state comes back by STATE.
// The group of an island another game owns: the group's record is new for a frame after every bond change, so on a
// miss the group's members (group +0x60, count +0x58) are tested against the driven set.
int coop_ctn_sender_entry(uint64_t group) {   // 1: the sender sends nothing (coop_life.c)
    if (!group) return 0;
    int hit = ent_set_has(ENT_SET_FOLLOW_GROUP, group);
    if (!hit) {
        int32_t n = 0;
        uint64_t arr = coop_rd_ptr(group + 0x60);
        if (arr && coop_rd(group + 0x58, &n, 4) && n > 0 && n <= 22)
            for (int j = 0; j < n && !hit; j++) hit = coop_ctn_driven(link_actor(coop_rd_ptr(arr + 8 * (uint64_t)j)));
    }
    if (hit) __atomic_fetch_and((uint64_t *)(uintptr_t)(group + 0x880), ~(1ull << 26), __ATOMIC_SEQ_CST);
    return hit;
}
// A toggle asked on a part of a build another game runs (a hit here): not made here, sent to the runner as the state
// wanted. From the device's switch update main+0x4a3500 (coop_autobuild.c coop_phase0_hook).
void coop_ctn_toggle_asked(uint64_t comp, uint64_t actor) {
    (void)comp;
    if (!part_on(actor) && gauge_blocked()) {   // (switching on with a gauge that cannot pay: not asked, plan 43 note 17)
        refused_on(actor);
        return;
    }
    plat_mutex_lock(&ev_lock);   // (the same hit's switch event, if any, is not sent besides: events_out)
    tog_seen[tog_i % 16].actor = actor, tog_seen[tog_i % 16].us = frame_step_world_us(), tog_i++;
    plat_mutex_unlock(&ev_lock);
    coop_ent_toggle_seen(actor, !part_on(actor));   // (a toggle on the copy as it shows: the other state)
}

// The wheels and sticks of the builds followed here shown as where they run (at actor phase 0 and at the co-op tick:
// when in the frame the stick's own graph writes its floats is not read, so both)
static void floats_follow(Cpu *c) {
    // (what to write is taken under the registry lock, and written after it is let go: the game's float write runs game
    // code, and a hook in it that asks this file about a part would wait on the lock held here, findings 32, 76, 81)
    struct { uint64_t part; int kind; float f[4]; } w[256];   // (not static: the tick calls this too)
    int nw = 0;
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < REG_N; i++) {
        Grp *e = &reg[i];
        if (!e->used || runs(e) || e->asked || !(e->fw | e->fk | e->fb)) continue;
        for (int r = 0; r < e->n && r < 21 && nw < 256; r++) {
            if (!e->parts[r] || (e->fp >> r & 1)) continue;   // (a spring: springs_phase0)
            int kind = e->fb >> r & 1 ? 3 : e->fw >> r & 1 ? 1 : e->fk >> r & 1 ? 2 : 0;
            if (kind) w[nw].part = e->parts[r], w[nw].kind = kind, memcpy(w[nw].f, e->pf[r], sizeof w[nw].f), nw++;
        }
    }
    plat_mutex_unlock(&grp_lock);
    for (int i = 0; i < nw; i++) {
        const float *f = w[i].f;
        if (!alive(w[i].part)) continue;
        // A time bomb: armed as where it runs (the bit its own arming message sets, fuse node +0xe8 bit 0), and its
        // fuse (node +0xdc now, +0xe0 before) the runner's, never under 2 frames: at 0 the node here would ask the
        // bomb's death and blast by itself. The blast comes with the runner's. Research notes
        // sync-v3/m2/p5/rocket-timebomb, 2.2 and 2.4. (A fuse that is no number is left as it is: finding 56.)
        if (w[i].kind == 3) {
            if (!c) continue;   // (the fuse node is written at actor phase 0 only, not from the co-op tick)
            uint64_t node = fuse_node(w[i].part);
            float left = f[0] > 2.0f ? f[0] : 2.0f;
            if (!node || f[1] == 0 || f[0] != f[0]) continue;
            __atomic_fetch_or((uint8_t *)(uintptr_t)(node + 0xe8), (uint8_t)1, __ATOMIC_SEQ_CST);
            memcpy((void *)(uintptr_t)(node + 0xdc), &left, 4), memcpy((void *)(uintptr_t)(node + 0xe0), &left, 4);
            continue;
        }
        uint64_t bb = actor_bb(w[i].part);
        if (!bb) continue;
        if (w[i].kind == 1) bb_set_f32(c, bb, BB_WHEEL_SPIN, f[0]), bb_set_f32(c, bb, BB_WHEEL_STEER, f[1]);
        else bb_set_f32(c, bb, BB_STICK_X, f[0]), bb_set_f32(c, bb, BB_STICK_Y, f[1]), bb_set_f32(c, bb, BB_STICK_SX, f[2]), bb_set_f32(c, bb, BB_STICK_SY, f[3]);
    }
}

// ---- A part's death where the build runs, made the same on the copies (plan 36 item 4) ------------------------------------
// A copy part takes no death from this game's own rules (G4). The game that runs the build sends each death its part
// is asked (the kind: 8 exploded, 9 burned, 16 life gone, 20 an Autobuilt part broken off, ...), and the others ask
// the same death of their copy with the game's own request main+0x1953184(actor, kind, 0), at actor phase 0, with the
// refusal lifted for that call. So a time bomb's blast, a burned board and a broken-off part's vanishing play on the
// copy too. Death kinds: research notes sync-v3/m2/p5/rocket-timebomb (table at main+0x366f8b0).
static struct { uint64_t actor; uint32_t kind; int64_t us; uint32_t gen; } death_put[64];
static int death_put_n;
void coop_life_death_allow(int on);
void coop_ctn_death_seen(uint64_t actor, uint32_t kind) { coop_ent_death_seen(actor, kind); }
static void deaths_put(Cpu *c) {   // actor phase 0
    uint64_t actor[64];
    uint32_t kind[64], gen[64];
    coop_lock_wait(&grp_lock);
    int n = death_put_n;
    for (int i = 0; i < n; i++) actor[i] = death_put[i].actor, kind[i] = death_put[i].kind, gen[i] = death_put[i].gen;
    death_put_n = 0;
    plat_mutex_unlock(&grp_lock);
    for (int i = 0; i < n; i++) {
        if (!alive_as(actor[i], gen[i])) continue;   // (the same actor still, not another at its address: finding 28)
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
// ---- A rider's weight on a build this game runs (plan 41.2, P6; research notes sync-v3/p6/mass and controller) -------
// A Link standing on a build weighs on it through the contact solver: its body is Dynamic with mass 10 kg, and a
// standing contact is not mass-scaled (the scale 0.1 applies to pushing only), so the pair's effective mass is
// m = 10 m_part / (10 + m_part), at most 10 kg. Another player's stand-in here is a static body kept out of contact with
// builds (I16): it weighs nothing. So the game that runs the build adds that weight as a force at the stand-in's feet on
// the ridden part's main body, main+0x29ae7a8 (body, &F, &P), which applies F times the physics step once per call: once
// per frame here, so the weight is exact at one physics step per frame (not read: how many steps a frame has). Not done:
// a stand-in made a Dynamic body that touches the build (the mass reading found a null read in the game's foot-band test
// for a character body without a controller holder, which a stand-in may lack), and the push of a walking rider.
static void rider_weight(Cpu *c) {
    int coop_riders(uint64_t *ridden, float (*at)[3], int max);
    uint64_t ridden[8];
    float at[8][3];
    int n = coop_riders(ridden, at, 8);
    for (int i = 0; i < n; i++) {
        int here = 0;
        coop_lock_wait(&grp_lock);
        for (int q = 0; q < REG_N && !here; q++)
            for (int r = 0; reg[q].used && runs(&reg[q]) && !reg[q].asked && r < reg[q].n && !here; r++)
                here = reg[q].parts[r] == ridden[i];
        plat_mutex_unlock(&grp_lock);
        uint64_t body = here && alive(ridden[i]) ? main_body_of(ridden[i]) : 0;
        float m = 0;
        if (!body || !coop_rd(body + 0x130, &m, 4) || !(m > 0) || m > 1e6f) continue;
        // (a standing Link presses with its own weight, 10 kg, whatever the part weighs: the reduced mass is a contact
        // impulse's, not a resting load's, review 2 finding 9; and only from a point within 3 m of the part, finding 11)
        float me = 10.0f, pp[3];
        if (!coop_rd(ridden[i] + COOP_ACTOR_POS, pp, 12)) continue;
        float dx = at[i][0] - pp[0], dy = at[i][1] - pp[1], dz = at[i][2] - pp[2];
        if (dx * dx + dy * dy + dz * dz > 9.0f) continue;
        static _Alignas(16) float f[4], p[4];
        f[0] = 0, f[1] = -9.8f * me, f[2] = 0, f[3] = 0;
        memcpy(p, at[i], 12), p[3] = 0;
        coop_guest_call(c, MOD_MAIN_BASE + 0x29ae7a8, body, (uint64_t)(uintptr_t)f, (uint64_t)(uintptr_t)p, 0);
        static int64_t said;
        int64_t now = frame_step_world_us();
        if (now - said > 5000000) said = now, rt_log("coop: contraption: a co-op player's weight on a part run here: %.1f kg (part %.1f kg)", me, m);
    }
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
        coop_lock_wait(&grp_lock);
        Grp *e = &reg[i];
        int go = e->used && e->n && !e->asked;
        np = e->n, mine = go && runs(e), want = e->burn;
        memcpy(parts, e->parts, sizeof parts);
        plat_mutex_unlock(&grp_lock);
        if (!go) continue;
        for (int r = 0; r < np && r < 21; r++) {
            uint64_t comp = alive(parts[r]) ? chem_comp(parts[r]) : 0;
            if (comp && (coop_guest_call(c, MOD_MAIN_BASE + 0x1b306fc, comp, (uint64_t)-1, 0, 0) & 1)) have |= 1u << r;
        }
        if (mine) {
            coop_lock_wait(&grp_lock);
            if (reg[i].used && runs(&reg[i])) reg[i].burn = have;
            plat_mutex_unlock(&grp_lock);
            continue;
        }
        // (asked at most every 2 s per part, and said once per change: a request that does not hold was asked and logged
        // five times a second for as long as the part burned there, finding 64)
        static struct { uint64_t a; int64_t us; int want; } asked[64];
        for (int r = 0; r < np && r < 21; r++) {
            uint64_t comp = ((want ^ have) >> r & 1) && alive(parts[r]) ? chem_comp(parts[r]) : 0;
            if (!comp) continue;
            int k = -1, w = (int)(want >> r & 1), said = 0;
            for (int q = 0; q < 64 && k < 0; q++) if (asked[q].a == parts[r]) k = q;
            if (k >= 0 && asked[k].want == w && now - asked[k].us < 2000000) continue;
            if (k >= 0) said = asked[k].want == w;
            if (k < 0) k = (int)((parts[r] >> 4) % 64);
            asked[k].a = parts[r], asked[k].us = now, asked[k].want = w;
            if (w) coop_guest_call(c, MOD_MAIN_BASE + 0x1b34a44, comp, 2, (uint64_t)-1, 0);
            else coop_guest_call(c, MOD_MAIN_BASE + 0xb2b804, comp, 0, (uint64_t)-1, 0);
            if (!said) rt_log("coop: contraption: part %d of a build followed here %s where it runs: %s here", r, w ? "burns" : "does not burn", w ? "set alight" : "put out");
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
static void springs_phase0(Cpu *c) {
    static int64_t read_us;
    int64_t now = frame_step_world_us();
    uint64_t list[SPR_N];
    SprSt want[SPR_N];
    int n = 0, nw = 0;
    // this game's springs read (10 a second): the parts of builds it runs, and its own lone ones
    if (now - read_us > 100000) {
        read_us = now;
        coop_lock_wait(&grp_lock);
        for (int i = 0; i < REG_N; i++)
            for (int r = 0; reg[i].used && runs(&reg[i]) && r < reg[i].n && n < SPR_N; r++)
                if (alive_as(reg[i].parts[r], reg[i].gen[r]) && spring_actor(reg[i].parts[r])) list[n++] = reg[i].parts[r];
        plat_mutex_unlock(&grp_lock);
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
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < REG_N; i++) {
        Grp *e = &reg[i];
        if (!e->used || runs(e) || e->asked) continue;
        for (int r = 0; r < e->n && nw < SPR_N; r++)
            if ((e->fp >> r & 1) && e->parts[r]) want[nw].actor = e->parts[r], want[nw].ext = e->pf[r][0], want[nw].limit = e->pf[r][1], want[nw].flags = (uint32_t)e->pf[r][2], nw++;
    }
    plat_mutex_unlock(&grp_lock);
    for (int i = 0; i < nw; i++) if (alive(want[i].actor)) spring_apply(c, want[i].actor, &want[i]);
}
int coop_ctn_emitter_of_followed(uint64_t a) {
    uint64_t x = a, sh[64];
    for (int hop = 0; x && hop < 3; hop++) {
        if (coop_ctn_follows(x)) return 1;
        uint64_t t = coop_rd_ptr(x + 0x228), sb = t ? coop_rd_ptr(t + 0x440) : 0;
        x = sb ? handle_actor(sb + 0x70) : 0;
    }
    int n = ent_set_list(ENT_SET_SHOOTER, sh, 64);
    for (int i = 0; a && i < n; i++) if (shot_of(sh[i], a)) return 1;
    return 0;
}
int coop_ctn_core(void) { return ctn_on(); }
void coop_ctn_structure_event(void) { coop_ent_structure_event(); }

// ---- What coop_ent*.c asks of this file (coop_ent.h) -------------------------------------------------------------------
#ifndef LD32
#define LD32(a) (*(uint32_t *)(uintptr_t)(a))
#define LD64(a) (*(uint64_t *)(uintptr_t)(a))
#define ST8(a, v) (*(uint8_t *)(uintptr_t)(a) = (uint8_t)(v))
#define ST32(a, v) (*(uint32_t *)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(uint64_t *)(uintptr_t)(a) = (uint64_t)(v))
#endif
int ctn_alive(uint64_t a) { return alive(a); }
void ctn_follow_reset(struct FollowState *fs) { if (fs) memset(fs, 0, sizeof *fs); }
int ctn_follow_rests(const struct FollowState *fs) { return fs && (fs->rest || fs->hold_until > frame_step_world_us()); }   // (read at phase 0 while the drive may write it: a frame late at worst)
// For the session record (coop_ent_log.c): whether the drive lets this copy rest, holds it by the field for good, and
// how often it has put it in place in this rest of its owner's object.
void ctn_follow_peek(const struct FollowState *fs, int *rest, int *no_rest, int *placed) {
    *rest = fs ? fs->rest : 0, *no_rest = fs ? fs->no_rest : 0, *placed = fs ? fs->placed : 0;
}
// Whether a part's main body collides with anything: the upper half of the first word of its collision filter record
// ([body +0x140] +8) is not 0. Read from the session record (a play session, 2026-11-14): a battery that runs out goes
// from 0x03f7ffff there to 0 as it leaves its build, a second before its death, in the game that runs it and in a
// copy alike; what the bits are one by one is not read. A part whose record cannot be read counts as solid.
int ctn_part_solid(uint64_t a) {
    uint64_t body = main_body_of(a), h = body ? coop_rd_ptr(body + 0x140) : 0, w = 0;
    if (!h || !coop_rd(h + 8, &w, 8)) return 1;
    return (w >> 32) != 0;
}
uint32_t ctn_gen_of(uint64_t a) { return gen_of(a); }
int ctn_alive_as(uint64_t a, uint32_t gen) { return alive_as(a, gen); }
uint64_t ctn_main_body(uint64_t a) { return main_body_of(a); }
uint64_t ctn_spr(uint64_t a) { return a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) : 0; }
uint64_t ctn_part_group(uint64_t a) { return part_group(a); }
int ctn_part_green(uint64_t a) { return part_green(a); }
uint64_t ctn_stick_rider(uint64_t a) { return stick_rider(a); }
uint64_t ctn_body_actor(uint64_t body) { return body_owner(body); }
float ctn_delete_radius(uint64_t a) { return delete_radius(a); }
int ctn_part_delete(Cpu *c, uint64_t a) { return part_delete(c, a); }
void ctn_m34_mul(float out[12], const float a[12], const float b[12]) { m34_mul(out, a, b); }
int ctn_m34_inv(float out[12], const float m[12]) { return m34_inv(out, m); }
uint64_t ctn_call5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) { return call5(c, fn, a0, a1, a2, a3, a4); }
int ctn_group_count(uint64_t actor) {
    uint64_t g = part_group(actor);
    int32_t n = 0;
    return g && coop_rd(g + 0x58, &n, 4) && n > 0 && n < 64 ? n : 1;
}
// A part's rigid bodies and their keys: key 0 is the main body; the others are asked of main+0x44bba4 for keys 1 to 6
// (the keys a device's static data gives its bodies; research notes sync-v4/device-bodies).
uint64_t ctn_body_by_key(Cpu *c, uint64_t actor, uint8_t key) { return body_slot(c, actor, key); }
int ctn_bodies(Cpu *c, uint64_t actor, uint64_t *body, uint8_t *key, int max) {
    int n = 0;
    for (uint32_t k = 0; k < 7 && n < max; k++) {
        uint64_t b = body_slot(c, actor, k);
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= body[i] == b;
        if (b && !dup) body[n] = b, key[n] = (uint8_t)k, n++;
    }
    return n;
}
// The glue edges of a part (main+0x27c1c0 lists a body's edges): registered (edge +0x10 bit 3), not removed (bit 2),
// glue by +0xec or +0xed, with the other end's actor and both bodies' keys.
int ctn_part_edges(Cpu *c, uint64_t actor, CtnEdge *out, int max) {
    static _Alignas(16) uint8_t list[0x440];
    uint64_t L = (uint64_t)(uintptr_t)list, body[8];
    uint8_t key[8];
    int n = 0, nb = ctn_bodies(c, actor, body, key, 8);
    for (int s = 0; s < nb; s++) {
        coop_guest_call(c, MOD_MAIN_BASE + 0x191327c, L, 0, 0, 0);
        coop_guest_call(c, MOD_MAIN_BASE + 0x27c1c0, body[s], L, 0, 0);
        int32_t cnt = 0;
        memcpy(&cnt, list, 4);
        uint64_t cells = coop_rd_ptr(L + 8);
        for (int i = 0; cells && i < cnt && i < 0x40 && n < max; i++) {
            uint64_t cell = coop_rd_ptr(cells + 8 * (uint64_t)i), edge = cell ? coop_rd_ptr(cell) : 0;
            uint8_t g = 0, g2 = 0;
            uint32_t fl = 0;
            if (!edge || !(coop_guest_call(c, MOD_MAIN_BASE + 0x2993218, edge, 0, 0, 0) & 1)) continue;
            if (!coop_rd(edge + 0xec, &g, 1) || !coop_rd(edge + 0xed, &g2, 1) || (!g && !g2) || !coop_rd(edge + 0x10, &fl, 4) || (fl & 4) || !(fl & 8)) continue;
            uint64_t ea = coop_guest_call(c, MOD_MAIN_BASE + 0x2993f98, edge, 0, 0, 0), eb = coop_guest_call(c, MOD_MAIN_BASE + 0x2994078, edge, 0, 0, 0);
            if (ea != body[s] && eb != body[s]) continue;
            uint64_t ob = ea == body[s] ? eb : ea, oa = body_owner(ob);
            if (!oa || oa == actor) continue;
            int ok = -1;
            for (uint32_t k = 0; k < 7 && ok < 0; k++) if (body_slot(c, oa, k) == ob) ok = (int)k;
            if (ok < 0) continue;
            out[n++] = (CtnEdge){edge, oa, key[s], (uint8_t)ok, (uint8_t)(ea == body[s]), (uint8_t)(!g && g2)};
        }
    }
    return n;
}
// The pose a bond holds: end B's body in end A's body frame. main+0x7a4410(edge, outA, outB) gives each end's rest
// frame in its body's space, and the held pose is outA * inverse(outB) (research notes sync-v4/bond-frames, 4). The
// layout of the two outputs is taken as the bodies' own 3x4 matrices; a result that is no number, or more than half a
// metre from where the two bodies stand now, is not trusted and the bodies' present relation is given instead
// (counted: the first test session's log settles which one is right).
int ctn_edge_held(Cpu *c, uint64_t edge, float held[12]) {
    static _Alignas(16) float oa[16], ob[16];
    uint64_t ba = coop_guest_call(c, MOD_MAIN_BASE + 0x2993f98, edge, 0, 0, 0), bb = coop_guest_call(c, MOD_MAIN_BASE + 0x2994078, edge, 0, 0, 0);
    float wa[12], wb[12], inv[12], world[12];
    if (!ba || !bb || !coop_rd(ba + 0x98, wa, sizeof wa) || !coop_rd(bb + 0x98, wb, sizeof wb) || !m34_inv(inv, wa)) return 0;
    m34_mul(world, inv, wb);
    memset(oa, 0, sizeof oa), memset(ob, 0, sizeof ob);
    int have = coop_rd_ptr(edge + 0x38) != 0;   // (the function loads through edge +0x38 with no test of its own)
    if (have) coop_guest_call(c, MOD_MAIN_BASE + 0x7a4410, edge, (uint64_t)(uintptr_t)oa, (uint64_t)(uintptr_t)ob, 0);
    int good = have && m34_inv(inv, ob);
    if (good) m34_mul(held, oa, inv);
    for (int i = 0; good && i < 12; i++) good = held[i] == held[i];
    if (good) {
        float dx = held[3] - world[3], dy = held[7] - world[7], dz = held[11] - world[11];
        good = dx * dx + dy * dy + dz * dz < 0.25f;
    }
    static _Atomic uint32_t from_frames, from_world;
    static int64_t said;
    if (good) atomic_fetch_add(&from_frames, 1);
    else memcpy(held, world, sizeof world), atomic_fetch_add(&from_world, 1);
    int64_t now = frame_step_world_us();
    if (now - said > 30000000)
        said = now, rt_log("coop: ent: held poses of bonds read so far: %u from the bond's rest frames, %u from where the bodies stand", (unsigned)atomic_load(&from_frames), (unsigned)atomic_load(&from_world));
    return 1;
}
void ctn_place(Cpu *c, uint64_t actor, const float m[12]) {   // one teleport of an actor, let through G1 as the drive's writes are
    static _Alignas(16) float mm[12];
    if (!alive(actor)) return;
    memcpy(mm, m, sizeof mm);
    sync_writing = 1;
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)mm, 0, 0);
    sync_writing = 0;
}
void ctn_teleport(Cpu *c, Grp *g) {   // an island of another owner more than 10 m off: put at the owner's newest pose
    if (g->mine || g->asked || !g->hus[2]) return;
    for (int k = 0; k < g->n && k < 21; k++) {
        float m[12];
        const float *r = g->rel_root[k];
        if (r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < 0.5f) continue;   // (its place in the island was never read)
        m34_mul(m, g->hm[2], r);
        if (alive_as(g->parts[k], g->gen[k])) ctn_place(c, g->parts[k], m);
    }
}

// ---- The recipe: the game's own record of a group (specification 9.1) --------------------------------------------------------
// The history writer main+0x1913694 records the group of an actor as a CmbAct. Its parts are the group's members in the
// writer's order: those it keeps (an SPR, SPR byte +0x26 bit 1 clear, SPR byte +0x2e8 bit 0 clear), sorted stably by the
// bytes of the name at [actor +0x218]. The list is rebuilt here by that rule and taken only when its count and every
// name agree with the record; else there is no recipe. 0: none.
int ctn_recipe(Cpu *c, Ent **members, int n, uint8_t *rec_out, Eid *rec_member) {
    uint64_t a = members[0]->actor, g = a ? part_group(a) : 0, used[21];
    char key[21][0x40];
    int32_t gcount = 0;
    int nmem = 0;
    if (n < 2 || n > 21 || !g) return 0;
    uint64_t arr = coop_rd_ptr(g + 0x60);
    if (!arr || !coop_rd(g + 0x58, &gcount, 4) || gcount != n) return 0;   // (the group is not the island: a regroup is under way)
    for (int j = 0; j < gcount; j++) {
        uint64_t b = link_actor(coop_rd_ptr(arr + 8 * (uint64_t)j)), spr = b ? coop_rd_ptr(coop_rd_ptr(b + 0x228) + 0x480) : 0;
        uint8_t f26 = 0, f2e8 = 0;
        int mine = 0;
        for (int k = 0; k < n; k++) mine |= members[k]->actor == b;
        if (!b || !mine || !spr) return 0;
        if ((coop_rd(spr + 0x26, &f26, 1) && (f26 & 2)) || (coop_rd(spr + 0x2e8, &f2e8, 1) && (f2e8 & 1))) return 0;   // (a member the writer skips)
        used[nmem++] = b;
    }
    for (int j = 0; j < nmem; j++) {
        uint64_t str = coop_rd_ptr(used[j] + 0x218);
        memset(key[j], 0, sizeof key[j]);
        if (str) coop_rd(str, key[j], sizeof key[j] - 1);
    }
    for (int j = 1; j < nmem; j++)
        for (int q = j; q > 0 && strcmp(key[q - 1], key[q]) > 0; q--) {
            char t[0x40];
            uint64_t ta = used[q];
            used[q] = used[q - 1], used[q - 1] = ta;
            memcpy(t, key[q], sizeof t), memcpy(key[q], key[q - 1], sizeof t), memcpy(key[q - 1], t, sizeof t);
        }
    static _Alignas(16) uint8_t link[0x40], rec[0x3000];
    static _Alignas(16) float align[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, info[12];
    memset(link, 0, sizeof link), memset(rec, 0, sizeof rec), memset(link + 0x10, 0xff, 4), link[0x14] = 1;
    coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, (uint64_t)(uintptr_t)link, a, 1, 0);
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + HISTORY_MGR_SLOT));
    if (!(call5(c, MOD_MAIN_BASE + HISTORY_WRITE, mgr, (uint64_t)(uintptr_t)link, (uint64_t)(uintptr_t)align, (uint64_t)(uintptr_t)rec, (uint64_t)(uintptr_t)info) & 1))
        return 0;   // (the writer fails outright for a group with more than 21 qualifying edges)
    if (rec[9] != nmem) return 0;
    memcpy(rec, "CmbAct", 6), rec[6] = 0, rec[7] = 0, rec[8] = 2;   // (the header is its caller's: magic and version 2)
    for (int i = 0; i < rec[10] && i < 21; i++) {
        const uint8_t *b = rec + 0x1704 + i * 12;
        if (b[8] >= rec[9] || b[9] >= rec[9]) return 0;
    }
    for (int r = 0; r < nmem; r++) {
        char want[0x40] = "", nm[0x40] = "";
        memcpy(want, rec + 0xc + r * 0x118 + 0x98, sizeof want - 1);
        coop_actor_name(used[r], nm, sizeof nm);
        if (want[0] && strcmp(nm, want)) return 0;
        rec_member[r] = (Eid){0, 0};
        for (int k = 0; k < n; k++) if (members[k]->actor == used[r]) rec_member[r] = members[k]->id;
    }
    memcpy(rec_out, rec, BP_SIZE);
    return nmem;
}

// ---- The copy maker (specification 9.2): an island made whole with the game's own Autobuild ------------------------------
// The player's build object (ability +0x51f0) is filled from the record, the part maker main+0x1d3b470 makes the parts,
// each is taken and bound to its entity, and the confirm main+0x1d3c0d4 joins them, with no cost. The base matrix puts
// the root's record part on the owner's newest root pose: a part's world pose is base * its record frame. What the make
// overwrites of the player's own working design (the base matrix, the record, the three bytes at +0x3c70) is saved and
// put back; the part pool and the kinds list are left as the make leaves them, as in v3.
static struct {
    int st, st_i, n;
    int64_t t;
    uint64_t from;
    Eid root, member[21];
    uint32_t green;
    uint8_t rec[BP_SIZE];
    uint64_t parts[21];
    uint32_t gens[21];      // each made part's id when it was taken: a part gone since is not written
    uint8_t save_m[48], save_b[3], save_rec[BP_SIZE];
    int saved;
} mk;
int ctn_make_busy(void) { return mk.st != 0; }
static uint64_t mk_object(uint64_t *ab_out) {
    uint64_t ab = coop_life_ability(), o = ab ? coop_rd_ptr(ab + 0x51f0) : 0;
    if (ab_out) *ab_out = ab;
    return o;
}
const char *ctn_make_wait_why = "";   // why the last ctn_make_waits said to wait (for the log of an island that waits long)
static float ctn_make_lift;           // how far above its place the next make is to be (set by ctn_make_waits)
int ctn_make_waits(const float root_m[12], float reach) {
    uint64_t ab, o = mk_object(&ab), me = coop_player_actor();
    uint32_t selected = 0;
    float lp[3];
    ctn_make_wait_why = "";
    if (!o) { ctn_make_wait_why = "this player has no Autobuild build object"; return 1; }
    coop_rd(ab + 0x51e8, &selected, 4);
    if (selected & 0xff) { ctn_make_wait_why = "this player has an Autobuild design selected"; return 1; }   // (the build object is theirs)
    for (int i = 0; i < ENT_N; i++) if (ents[i].used && (ents[i].flags & ENT_PREVIEW)) { ctn_make_wait_why = "a part maker's run of this game is not confirmed yet"; return 1; }   // (their own Autobuild is under way)
    // Parts made inside the Link are pushed apart before they bond. The make does not wait for the Link to go (a build
    // that came to rest by this player stayed unseen for as long as they stood there, a bench run): it is made that
    // far above its place, and the drive brings it down.
    ctn_make_lift = 0;
    if (me && coop_rd(me + COOP_ACTOR_POS, lp, 12)) {
        float dx = lp[0] - root_m[3], dy = lp[1] - root_m[7], dz = lp[2] - root_m[11];
        if (sqrtf(dx * dx + dy * dy + dz * dz) < reach) ctn_make_lift = reach + 1.0f;
    }
    return 0;
}
static void mk_restore(uint64_t o) {
    if (!mk.saved || !o) return;
    memcpy((void *)(uintptr_t)(o + 0x120), mk.save_m, sizeof mk.save_m);
    memcpy((void *)(uintptr_t)(o + 0x3c70), mk.save_b, sizeof mk.save_b);
    uint64_t cai = coop_rd_ptr(o + 0x3c50);
    if (cai) memcpy((void *)(uintptr_t)cai, mk.save_rec, BP_SIZE);
    mk.saved = 0;
}
static Eid mk_done_root;
static int64_t mk_done_ms;
// The maker is making this island, or ended a run for it under 0.7 s ago: the reconcile leaves it alone then. (It took
// the parts just bound, with their bonds not confirmed yet, for an island with bonds missing and made one itself; the
// confirm then made the same bond again, the repeat was taken off, and the island was deleted and made anew.)
int ctn_make_is(Eid root) {
    if (mk.st && eid_eq(mk.root, root)) return 1;
    return eid_eq(mk_done_root, root) && ent_ms() - mk_done_ms < 700;
}
static void mk_end(int failed) {
    // (after a make that failed there is nothing to leave alone: its island takes its back-off in the reconcile)
    if (failed) mk_done_ms = 0;
    else mk_done_root = mk.root, mk_done_ms = ent_ms();
    for (int i = 0; i < 21; i++) atomic_store(&mk_pub[i], 0);
    if (failed && mk.st_i >= 0 && mk.st_i < STATED_N && stated[mk.st_i].used && eid_eq(stated[mk.st_i].root, mk.root)) {
        Stated *st = &stated[mk.st_i];
        st->state = ST_NOT_SHOWN, st->why = WHY_MAKER_FAILED, st->backoff = st->backoff ? st->backoff * 2 : 2000;
        if (st->backoff > 30000) st->backoff = 30000;
        st->backoff_until = ent_ms() + st->backoff;
    }
    mk.st = 0, mk.n = 0;
}
int ctn_make_start(Cpu *c, int st_i) {
    uint64_t o = mk_object(NULL);
    if (mk.st || !o || st_i < 0 || st_i >= STATED_N || !stated[st_i].used) return 0;
    Stated *st = &stated[st_i];
    const uint8_t *r = st->rec;
    int n = r[9], root_i = -1;
    char b[24];
    if (n < 2 || n > 21 || n != st->rec_n || memcmp(r, "CmbAct", 6)) return 0;
    for (int i = 0; i < n; i++) if (eid_eq(st->rec_member[i], st->root)) root_i = i;
    if (root_i < 0) return 0;
    float f[12], inv[12], base[12];
    memcpy(f, r + 0xc + root_i * 0x118, sizeof f);
    if (!m34_inv(inv, f)) return 0;
    float at[12];
    memcpy(at, st->root_m, sizeof at), at[7] += ctn_make_lift;
    m34_mul(base, at, inv);
    if (ctn_make_lift > 0) rt_log("coop: ent: this Link stands where %s is to be made: it is made %.1f m above", ent_eid_str(st->root, b), ctn_make_lift);
    ctn_make_lift = 0;
    memset(&mk, 0, sizeof mk);
    mk.st = 1, mk.st_i = st_i, mk.t = frame_step_world_us(), mk.from = st->from, mk.root = st->root;
    memcpy(mk.rec, r, BP_SIZE), memcpy(mk.member, st->rec_member, sizeof(Eid) * (size_t)n);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < st->n; k++) if (eid_eq(st->m[k].id, mk.member[i]) && st->m[k].green) mk.green |= 1u << i;
    if (!coop_rd_ptr(o + 0x3c50)) ST64(o + 0x3c50, o + 0xc50);
    uint64_t cai = coop_rd_ptr(o + 0x3c50);
    memcpy(mk.save_m, (void *)(uintptr_t)(o + 0x120), sizeof mk.save_m), memcpy(mk.save_b, (void *)(uintptr_t)(o + 0x3c70), sizeof mk.save_b);
    memcpy(mk.save_rec, (void *)(uintptr_t)cai, BP_SIZE), mk.saved = 1;
    memcpy((void *)(uintptr_t)cai, r, BP_SIZE);
    memcpy(coop_ab_cai, r, BP_SIZE);
    memcpy((void *)(uintptr_t)(o + 0x120), base, sizeof base);
    // the part pool emptied and refilled from the record, as the ability does before its part maker
    uint64_t pc = o + 0x4d10, pu = o + 0x4f48;
    uint32_t ctr = LD32(pc), used = LD32(pu);
    ST64(pc + 0x220, 0), ST32(pc, ++ctr);
    if ((int32_t)used >= 1 && LD64(o + 0x4f50)) {
        uint64_t head = LD64(o + 0x4f58), arr = LD64(o + 0x4f50);
        for (uint32_t i = 0; i < used && i < 64; i++) {
            uint64_t ent = LD64(arr + 8 * (uint64_t)i);
            if (!ent) continue;
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
    coop_ent_creating(1);   // (the creates the part maker asks are this code's: no identity is minted for them)
    coop_guest_call(c, MOD_MAIN_BASE + 0x1d3b470, o, 0, 0, 0);
    coop_ent_creating(0);
    rt_log("coop: ent: %lld the island %s of %04x (%d parts) is being made here at %.2f %.2f %.2f", (long long)ent_ms(), ent_eid_str(st->root, b), (unsigned)(st->from & 0xffff),
           n, st->root_m[3], st->root_m[7], st->root_m[11]);
    return 1;
}
void ctn_make_pass(Cpu *c) {
    if (!mk.st) return;
    int64_t now = frame_step_world_us();
    uint64_t ab, o = mk_object(&ab);
    uint32_t selected = 0;
    char b[24];
    if (!o) { mk_end(1); return; }
    if (ent_loading()) { mk.saved = 0; mk_end(0); return; }   // (the world is being torn down: the make is dropped as it is)
    uint64_t list = coop_rd_ptr(o + 0x1b8);
    int32_t ne = 0;
    coop_rd(o + 0x1b0, &ne, 4);
    // The abort: this game's player chose a design while the copy was being made. The game releases the build object,
    // cancelling the creates and deleting the made parts; the make is dropped and waits its turn again.
    coop_rd(ab + 0x51e8, &selected, 4);
    if (mk.st <= 2 && (selected & 0xff)) {
        rt_log("coop: ent: the copy of %s was being made when this player chose an Autobuild design: dropped, made again later", ent_eid_str(mk.root, b));
        for (int i = 0; i < mk.n; i++) { Ent *e = ent_by_id(mk.member[i]); if (e && e->actor == mk.parts[i]) ent_unbind(e); }
        mk.saved = 0;   // (the object holds the player's design now)
        mk_end(0);
        return;
    }
    if (mk.st == 1) {   // every part made: taken and bound to its entity, confirmed next frame
        int ready = ne == mk.rec[9];
        for (int i = 0; list && i < ne && i < 21; i++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)i);
            if (!ent || coop_handle_status((const uint8_t *)(uintptr_t)(ent + 0x60)) != 3) ready = 0;
        }
        if (!ready) {
            if (now - mk.t > 6000000) {
                rt_log("coop: ent: the parts of %s were not made in 6 s (%d of %u listed)", ent_eid_str(mk.root, b), ne, mk.rec[9]);
                mk_restore(o), mk_end(1);
            }
            return;
        }
        static _Alignas(16) uint8_t tmp[21][0x40];
        for (int i = 0; list && i < ne && i < 21; i++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)i), ref = 0;
            mk.parts[i] = coop_claim(c, (uint8_t *)(uintptr_t)(ent + 0x60), tmp[i], &ref);
            mk.gens[i] = gen_of(mk.parts[i]);
            atomic_store(&mk_pub[i], mk.parts[i]);
            Ent *e = ent_by_id(mk.member[i]);
            if (e && mk.parts[i] && e->owner == mk.from && !e->actor) ent_bind(e, mk.parts[i]), e->flags |= ENT_COPY;
            else if (mk.parts[i]) {   // (its entity went or changed hands while it was made: the part is no one's, deleted)
                part_delete(c, mk.parts[i]);
                atomic_store(&mk_pub[i], 0), mk.parts[i] = 0;
            }
        }
        mk.n = ne < 21 ? ne : 21, mk.st = 2;
        return;
    }
    if (mk.st == 2) {   // the confirm, as the ability makes it, with no cost
        uint64_t tab = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395f900)) + 8);
        uint64_t costs_at[21] = {0};
        uint32_t costs_were[21] = {0};
        int ncost = 0;
        static uint64_t keyname;
        for (int i = 0; i < mk.n; i++) if (mk.parts[i] && !alive_as(mk.parts[i], mk.gens[i])) mk.parts[i] = 0;   // (gone since it was taken)
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
        for (int j = 0; j < ncost; j++) ST32(costs_at[j] + 0x48, costs_were[j]);
        for (int i = 0; i < mk.n; i++) {
            uint64_t spr = mk.parts[i] ? coop_rd_ptr(coop_rd_ptr(mk.parts[i] + 0x228) + 0x480) : 0;
            if (!spr) continue;
            __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x27c), 1u, __ATOMIC_SEQ_CST);
            ST64(spr + 0x20, (LD64(spr + 0x20) & ~(1ull << 51)) | (1ull << 28));
            __atomic_fetch_and((uint32_t *)(uintptr_t)(mk.parts[i] + 0x5ac), ~4u, __ATOMIC_SEQ_CST);
            // green as where it is owned: the mark a confirm gives a part it made from zonaite (SPR +0x150 bit 0)
            if (mk.green >> i & 1) __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x150), 1u, __ATOMIC_SEQ_CST);
        }
        mk_restore(o);
        rt_log("coop: ent: the copy of %s confirmed (%llu)", ent_eid_str(mk.root, b), (unsigned long long)(r & 1));
        mk.st = 3, mk.t = now;
        coop_ent_structure_event();
        return;
    }
    if (mk.st == 3) {   // done once the parts are one group, or after a second
        int grouped = 0;
        for (int i = 0; i < mk.n; i++) grouped += mk.parts[i] && part_group(mk.parts[i]);
        if ((grouped == mk.n && now - mk.t > 200000) || now - mk.t > 1000000) {
            rt_log("coop: ent: the copy of %s made, %d of %d parts in one group", ent_eid_str(mk.root, b), grouped, mk.n);
            coop_ent_structure_event();
            mk_end(0);
        }
    }
}

// ---- The group views: device state at the owner, requests put on the parts, the drive ------------------------------------------
uint64_t ctn_payer_hint;   // the player whose request the next ctn_toggle_put carries out (it pays)
// The devices of an island this game owns, as its pose message carries them (bit r: member r, ascending Eid).
void ctn_owner_state(Cpu *c, Grp *g) {
    int64_t now = frame_step_world_us();
    uint32_t sw = 0, on = 0, fp = 0, fw = 0, fk = 0, fb = 0;
    (void)c;
    for (int r = 0; r < g->n && r < 21; r++) {
        uint64_t a = g->parts[r], bb = actor_bb(a);
        SprSt sp;
        if (part_switch(a)) sw |= 1u << r, on |= (uint32_t)part_on(a) << r;
        memset(g->pf[r], 0, sizeof g->pf[r]);
        if (spr_find(a, &sp)) { fp |= 1u << r, g->pf[r][0] = sp.ext, g->pf[r][1] = sp.limit, g->pf[r][2] = (float)sp.flags; continue; }
        if (bb_f32(bb, BB_WHEEL_SPIN, &g->pf[r][0])) fw |= 1u << r, bb_f32(bb, BB_WHEEL_STEER, &g->pf[r][1]);
        else if (fuse_node(a)) {
            uint64_t node = fuse_node(a), cell = coop_rd_ptr(node + 0xd0) & ~3ull;
            uint8_t armed = 0;
            fb |= 1u << r, coop_rd(node + 0xdc, &g->pf[r][0], 4), g->pf[r][1] = cell && coop_rd(cell, &armed, 1) && armed;
        } else if (bb_f32(bb, BB_STICK_X, &g->pf[r][0]))
            fk |= 1u << r, bb_f32(bb, BB_STICK_Y, &g->pf[r][1]), bb_f32(bb, BB_STICK_SX, &g->pf[r][2]), bb_f32(bb, BB_STICK_SY, &g->pf[r][3]);
    }
    g->sw = sw, g->on = on, g->fp = fp, g->fw = fw, g->fk = fk, g->fb = fb;
    if (!(on & sw) && now - g->payer_us > 2000000) g->payer = 0;   // (off for 2 s: whoever switches it on next pays)
}
// A toggle another player asked of a device this game owns: every switched device of its island set to the state
// wanted (the game keeps a build's devices in one state), and that player pays.
void ctn_toggle_put(Cpu *c, uint64_t actor, int want) {
    (void)c;
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < GRP_N; i++) {
        Grp *g = &grps[i];
        int in = 0;
        if (g->used != 1 || !g->mine) continue;
        for (int r = 0; r < g->n; r++) in |= g->parts[r] == actor;
        if (!in) continue;
        if (want) g->payer = ctn_payer_hint ? ctn_payer_hint : coop_ctr_me(), g->payer_us = frame_step_world_us();
        dev_set_later(g->parts, g->n, want ? g->sw : 0, 'a');
        break;
    }
    ctn_payer_hint = 0;
    plat_mutex_unlock(&grp_lock);
}
void ctn_event_put(uint64_t actor, uint32_t gen, uint8_t bits) {   // a switch event another player's attack put on its copy
    if (bits & 5) {   // (set on: that player pays, as for a toggle)
        coop_lock_wait(&grp_lock);
        for (int i = 0; i < GRP_N; i++) {
            Grp *g = &grps[i];
            int in = 0;
            if (g->used != 1 || !g->mine) continue;
            for (int r = 0; r < g->n; r++) in |= g->parts[r] == actor;
            if (in) { g->payer = ctn_payer_hint ? ctn_payer_hint : coop_ctr_me(), g->payer_us = frame_step_world_us(); break; }
        }
        plat_mutex_unlock(&grp_lock);
    }
    ctn_payer_hint = 0;
    plat_mutex_lock(&ev_lock);
    if (ev_put_n < EVQ) ev_put[ev_put_n].actor = actor, ev_put[ev_put_n].bits = bits, ev_put[ev_put_n].gen = gen, ev_put_n++;
    else queue_full("switch events to put");
    plat_mutex_unlock(&ev_lock);
}
void ctn_death_put(uint64_t actor, uint32_t gen, uint32_t kind) {   // a death the owner's part had: asked of the copy at phase 0
    coop_lock_wait(&grp_lock);
    if (death_put_n < 64) death_put[death_put_n].actor = actor, death_put[death_put_n].kind = kind, death_put[death_put_n].gen = gen, death_put_n++;
    else queue_full("deaths to replay");
    plat_mutex_unlock(&grp_lock);
}
void ctn_phase0_kept(Cpu *c) {
    events_put();
    deaths_put(c);
    dev_flush(c);
    rider_weight(c);
    floats_follow(c);
    springs_phase0(c);
    burning_phase0(c);
    for (int i = 0; i < GRP_N; i++)
        if (grps[i].used == 1 && grps[i].teleport) grps[i].teleport = 0, ctn_teleport(c, &grps[i]);
}
// The co-op tick: the copies moved toward their owners' poses, their devices set as there.
void ctn_drive(Cpu *c) {
    int64_t now = frame_step_world_us(), wall = ent_ms();
    events_out();
    for (int i = 0; i < GRP_N; i++) {
        Grp *g = &grps[i];
        coop_lock_wait(&grp_lock);
        // (2.5 s without a pose: a resting owner sends one a second, and with a limit of 1 s the drive ran only in the
        // moments after each: a new copy made 0.4 m off its owner's resting object hung there until the 2 s rule)
        int here = g->used == 1 && !g->mine && g->n && wall - g->stamp_ms < 300 && g->hus[2] && now - g->heard_us < 2500000;
        int go = here && !g->asked, agree = g->agree;
        uint64_t key = g->key64, owner = g->owner;
        plat_mutex_unlock(&grp_lock);
        if (!here) continue;
        if (agree) dev_follow(c, g, now);   // (its devices as where it is owned, also while this player holds it)
        if (!go) continue;
        float err = 0;
        int placed = follow_rigid(c, g, now, &err) == 2;   // (2: out of place at rest for 2 s)
        if (err > 10.0f || placed) {   // (done at phase 0; the row may be another island's by now: marked only if it is still this one)
            coop_lock_wait(&grp_lock);
            if (g->used == 1 && !g->mine && g->key64 == key && g->owner == owner) g->teleport = 1;
            plat_mutex_unlock(&grp_lock);
        }
    }
    floats_follow(NULL);
}
