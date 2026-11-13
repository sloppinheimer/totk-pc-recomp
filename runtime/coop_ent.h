// Co-op: entities (docs/coop/contraption-sync-v4-spec.md, cited as S). Shared by coop_ent.c (the table, identity,
// reading the world, the passes), coop_ent_own.c (ownership and the messages), coop_ent_rec.c (reconcile, bonds, the
// glue across owners) and coop_ctn.c (what was kept of the v3 code: the drive, the devices, the copy maker, the gates).
// TotK 1.4.3 main offsets. Every timer is wall-clock milliseconds (ent_ms) unless a name ends in _us (world clock).
#pragma once
#include <stdatomic.h>
#include <stdint.h>

#include "coop_ctr.h"
#include "cpu.h"
#include "platform/platform.h"

#define ENT_N 768
#define ISL_N 768
#define BOND_N 1024
#define STATED_N 128
#define REQ_N 64
#define OP_N 16
#define SERVE_N 8
#define ASK_N 16
#define ANS_N 64
#define PEND_N 64
#define EV_N 1024
#define GRP_N 512
#define REC_N 64
#define ENT_PEERS 8              // per-peer arrays are indexed by relay id - 1
#define ENT_MEMBERS 21           // the game's group limit: a glue is refused at 22
#define ENT_BONDS 48             // bonds of one statement (S 6.2)
#define BP_SIZE 0x1818           // a CmbAct: header, 21 parts of 0x118 from +0xc, 21 bonds of 12 from +0x1704, the tail

// inc 0: a map actor, serial its 31-bit key (S 4.3). Hashed and compared field by field, never as bytes.
typedef struct { uint64_t inc; uint32_t serial; } Eid;
static inline int eid_eq(Eid a, Eid b) { return a.inc == b.inc && a.serial == b.serial; }
static inline int eid_lt(Eid a, Eid b) { return a.inc != b.inc ? a.inc < b.inc : a.serial < b.serial; }
static inline int eid_zero(Eid a) { return !a.inc && !a.serial; }
static inline uint64_t eid_hash(Eid a) {
    uint64_t h = (a.inc ^ ((uint64_t)a.serial << 32 | a.serial)) * 0x9e3779b97f4a7c15ull;
    return h ^ h >> 29;
}

enum { LIFE_LIVE, LIFE_DEAD, LIFE_GONE, LIFE_FREE };
enum { CLS_IDLE, CLS_HOLD, CLS_RIDE };
enum { ENT_GREEN = 1, ENT_MAP = 2, ENT_PREVIEW = 4, ENT_COPY = 8 };   // only GREEN is sent
enum { ST_AGREE, ST_FAR, ST_WAIT_MAKER, ST_MAKING, ST_DIFFER, ST_NOT_SHOWN };
enum { WHY_NONE, WHY_CREATE_FAILED, WHY_BOND_NOT_READY, WHY_BOND_POSE, WHY_FULL22, WHY_NO_RECIPE, WHY_MAP_MEMBER,
       WHY_OWN_MEMBER, WHY_MAKER_FAILED, WHY_TABLE_FULL };

typedef struct {
    uint8_t used;
    Eid id;
    uint64_t owner; uint32_t epoch;      // changed by ent_apply_owner alone (S 5.0)
    uint8_t life, death_kind, flags;     // death_kind 0xff: a plain delete
    char name[48]; float scale[3], radius;
    uint64_t actor; uint32_t gen;        // this game's actor (0: not here) and actor +0x10 when taken
    uint64_t main_body;                  // cached while gen matches
    float m[12];                         // its main body's matrix as last read (phase 0 step 3)
    int64_t moved_ms;
    uint8_t handle[0x40], link[0x40]; uint8_t making; int64_t make_ms;   // our own create of a copy (S 4.1)
    uint8_t mir_owner; uint32_t mir_id;  // the object mirror's name for it at entry (S 4.4), 0: none
    uint8_t mir_asked, mir_known;        // the mirror was asked / has answered
    uint64_t mir_actor; uint8_t mir_resolved, mir_copy;   // a pair received: the actor the mirror has for it (0 none)
    int64_t pass_ms, unload_ms, death_ms;   // an island of one: its last reconcile pass; an owned unload; a death replayed
    uint8_t batch;                       // the maker run it came from (PREVIEW, S 4.6)
    uint8_t end_hold, end_ride, death_due;   // world changes noted for phase 0
    int16_t isl, st;                     // owned: its Island; foreign in a statement of two or more: its Stated; else -1
    // a foreign island of one keeps here what a Stated keeps for a build:
    float root_m[12]; int64_t pose_ms;
    uint8_t state, why, passes; int64_t backoff_until, backoff; uint32_t digest, st_digest;
    uint8_t cls, offer; int64_t cls_ms;  // the owner's class and OFFER flag from POSE
    uint8_t named;                       // an ISLAND, GRANT or POSE named it since the last NEED(0, 0) to its owner
    uint8_t far_seen;                    // listed as a far end by a statement this pass
    int64_t stated_ms, dead_ms, entered_ms;
    uint8_t asked;                       // a request of this game is open on it (S 5.2)
    uint8_t bond_wanted;                 // out of the drive: a bond is being made (S 7.3 step 1)
} Ent;

// a < b by (inc, serial). sa, sb: body keys. rel = inverse(Wa) * Wb: b's body in the frame of a's body (S 3.2).
// edge: this world's edge; ed: a glue by edge +0xed alone; ea: entity a is the edge's end A (local, not sent).
typedef struct { Eid a, b; uint8_t sa, sb; float rel[12]; uint64_t edge; uint8_t ed, ea; } Bond;

typedef struct { uint8_t used; uint8_t rec[BP_SIZE], n; Eid member[ENT_MEMBERS]; uint32_t digest; } Recipe;

typedef struct {            // an island this game owns (S 3.3)
    uint8_t used;
    Eid root; uint16_t n; int16_t m[ENT_MEMBERS];           // Ent indexes, ascending by Eid
    uint32_t digest, epoch; uint8_t cls, has_far;
    uint32_t stated_digest; uint8_t owed[ENT_PEERS];
    struct { uint32_t digest; uint8_t state, why; int64_t ms, diff_since, owed_ms; } peer[ENT_PEERS];
    int64_t moved_ms, class_ms, pose_ms; uint8_t class_sends;
    uint8_t frozen; int64_t offer_ms;
    int16_t rec;                                            // its Recipe, -1 none
    uint8_t due; int64_t read_ms;
} Island;

typedef struct {            // a statement of two or more members, or of one member with a bond (S 6.2)
    uint8_t used;
    uint64_t from; Eid root; uint32_t counter, digest; uint16_t n, nb, nf;
    uint8_t cls, offer; int64_t cls_ms;
    struct { Eid id; uint8_t life, death_kind, green; float rel_root[12], radius; } m[ENT_MEMBERS];
    Bond bond[ENT_BONDS];
    struct { Eid id; uint64_t owner; uint32_t epoch; } far[ENT_MEMBERS];
    float root_m[12]; int64_t pose_ms;
    uint8_t rec[BP_SIZE], rec_n; Eid rec_member[ENT_MEMBERS];
    int64_t ms; uint8_t state, why, passes, rung2_done; int64_t backoff_until, backoff, diff_ms, layout_ms;
    uint32_t my_digest;
    uint8_t bond_step; int64_t bond_ms, pass_ms;   // a bond being made over several frames (S 7.3 steps 1 to 5)
    Bond bond_now;                                 // the stated bond that make is for
} Stated;

enum { RQ_FREE, RQ_OPEN, RQ_GRANTED, RQ_REFUSED, RQ_EXPIRED };
enum { TAKE_GRAB = 1, TAKE_RIDE };
typedef struct { uint32_t id; uint8_t state, reason, forwarded; Eid member; uint64_t to; int64_t opened, sent, closed; } Req;

enum { OP_FREE, OP_WAIT_GRANT_X, OP_SENT, OP_BREAKING };
typedef struct {
    uint32_t op; uint8_t state; Eid x, t; uint8_t sx, st; float rel[12];
    uint64_t edge, to; int64_t opened, sent; uint8_t forwards, break_tries;
} Op;

typedef struct { uint8_t used; uint64_t from; uint32_t op; Eid x, t; uint8_t sx, st; uint32_t epoch_x; float rel[12]; int64_t since; uint8_t made; } Serve;
typedef struct { uint8_t used; uint32_t id; Eid e; uint8_t kind; uint32_t value; int64_t opened, sent; } Ask;

enum { ANS_TAKE = 1, ANS_BOND, ANS_REQ };
typedef struct {            // the owner's memory of an answer, stored as fields and built again when sent
    uint8_t used; uint64_t from; uint32_t id; uint8_t kind, type, reason, send, n; int64_t ms, until, sent;
    uint64_t owner, asker; uint32_t epoch; Eid m[ENT_MEMBERS]; uint32_t sw, on; uint64_t payer;
} Ans;

typedef struct { uint32_t sent_counter, taken_counter; uint8_t index_miss; int64_t need_ms; uint32_t need_number; } Peer;

enum { DEC_NONE, DEC_OURS, DEC_MINT, DEC_MINT_PREVIEW, DEC_ASK_MIRROR };

enum { EV_NONE, EV_GLUE, EV_STRUCT, EV_DELETE, EV_DEATH, EV_CREATED, EV_GRAB, EV_TOGGLE, EV_SWITCH };
typedef struct {            // seq is written last, so the reader sees whole events only
    _Atomic uint32_t seq; uint8_t kind, flag, decision, batch; uint32_t gen, reason; uint64_t actor, a2, lr, edge; float m[2][12];
} Ev;

// ---- The group view: one island as the kept code of coop_ctn.c sees it, rebuilt by coop_ent.c each frame ---------------
struct FollowState;
typedef struct {
    uint8_t used, mine, agree, asked, teleport;
    Eid root; uint64_t key64; uint64_t owner;
    int n; uint64_t parts[ENT_MEMBERS]; uint32_t gen[ENT_MEMBERS]; Eid id[ENT_MEMBERS];
    uint32_t no_write;                    // bit r: member r is behind a body other than the main one (S 3.5): not written
    float rel_root[ENT_MEMBERS][12];
    int64_t stamp_ms;                     // when phase 0 last wrote this row
    // a follower: the owner's last three root samples (newest last, its clock), the clock offset, the drive
    float hm[3][12]; int64_t hus[3], clock_off, clock_off_at, heard_us; struct FollowState *fs;
    // devices: at the owner computed here, at a follower from POSE (used only when agree)
    uint32_t sw, on, fp, burn, fw, fk, fb; float pf[ENT_MEMBERS][4];
    uint64_t payer; int64_t toggled_us, payer_us;
} Grp;

// ---- coop_ent.c ----------------------------------------------------------------------------------------------------------
extern PlatMutex ent_lock;                 // the table; other takers than phase 0 wait with coop_lock_wait
extern Ent ents[ENT_N];
extern Bond bonds[BOND_N]; extern int bond_n;
extern Island isls[ISL_N];
extern Stated stated[STATED_N];
extern Recipe recipes[REC_N];
extern Grp grps[GRP_N]; extern PlatMutex grp_lock;
extern Peer peers[ENT_PEERS];
int64_t ent_ms(void);                      // wall clock, ms
int ent_on(void);                          // TOTK_COOP_CONTRAPTION=1
Ent *ent_by_id(Eid id);
Ent *ent_by_actor(uint64_t actor);         // checked against gen
Ent *ent_new(Eid id);                      // NULL: the table is full (counted, logged)
void ent_drop(Ent *e);
void ent_bind(Ent *e, uint64_t actor);     // takes the actor: gen, name, the actor hash
void ent_unbind(Ent *e);
static inline int ent_i(const Ent *e) { return (int)(e - ents); }
uint32_t ent_mint_serial(void);
void ent_full(const char *table);          // a full table: counted, one log line per 10 s
const char *ent_eid_str(Eid id, char *buf);   // "inc16.serial" (S 12.3); buf of 24
uint32_t ent_fnv(uint32_t h, const void *p, int n);
uint32_t ent_digest(const Eid *members, const uint8_t *life, int n, const Bond *b, int nb);   // S 3.4
int ent_island_members(const Ent *e, Ent **out, int max);          // the connected set e is in here, ascending by Eid
int ent_island_bonds(Ent **members, int n, Bond *out, int max);    // bonds with an end among them
uint32_t ent_digest_of(Ent **members, int n);                      // over this game's world
int ent_this_link_holds(const Ent *e);     // the island of e is the one this Link's Ultrahand holds (S 3.6)
int ent_this_link_rides(const Ent *e);
uint64_t ent_held_actor(void);             // the actor this Link's Ultrahand holds (0 none), as of this frame
void ent_mark_due(Ent *e);                 // its island is read this frame (S 3)
void ent_post(const Ev *ev);               // append to the ring (any thread)
void ent_delete_actor(Cpu *c, Ent *e);     // our own delete: the flag `ours`, the mirror row freed first (S 7.2)
int ent_peer_i(uint64_t inc);              // relay id - 1 of an incarnation, -1 none
int ent_loading(void); int ent_paused(void);
int ent_peer_paused(uint64_t inc); int ent_peer_loading(uint64_t inc); int ent_peer_here(uint64_t inc);
void coop_ent_phase0(Cpu *c);
void coop_ent_net(void);                   // beside coop_ctr_tick
void coop_ent_drive(Cpu *c);               // where coop_ctn_tick was called

// ---- coop_ent_own.c: ownership and the messages (S 5, 6) -------------------------------------------------------------------
int ent_apply_owner(Ent *e, uint64_t owner, uint32_t epoch);       // 1: taken
void own_receive(Cpu *c, const CoopCtrMsg *m);                     // one reliable message, at phase 0
void own_slots(void);                                              // POSE, APPLIED, INDEX (phase 0 step 1)
void own_timers(Cpu *c);                                           // step 6
void own_causes(Cpu *c);                                           // step 7: TAKE for grabs and rides
void own_send(Cpu *c);                                             // step 8: statements owed, POSE, APPLIED, INDEX, NEED
void own_grant(uint64_t asker, uint32_t id, uint64_t new_owner, Ent **mem, int n, uint32_t epoch, uint8_t kind);
void own_net(void);                                                // the net tick: pause queue, TAKE answers, repeats
int own_pq_take(CoopCtrMsg *m);                                    // phase 0 reads the pause queue first
void own_peer_events(void);
void own_grab(Cpu *c, Ent *e);                                     // this Link's Ultrahand took hold of e
void own_claim(Ent **members, int n, const char *why);             // S 5.1, 5.7: owner this game, stated at once
void own_end_hold(Cpu *c, Ent *e);                                 // S 5.5: the refusal message; read back and resent
void own_end_ride(Cpu *c);                                         // S 5.6
void own_owe(Island *is, int peer);                                // a statement is owed (peer -1: all)
void own_owe_ent(Ent *e, int peer);
void own_death(Ent *e, uint8_t kind);                              // DEATH to all
void own_req(Ent *e, uint8_t kind, uint32_t value);                // REQ to the owner (toggle, switch event)
void own_unload(Cpu *c, Ent *e);                                   // S 5.8: an accepted unload of an owned entity
int own_send_to(uint64_t inc, int type, int flags, const void *d, int len);   // per peer; 0: owed
uint32_t own_req_open_for(const Ent *e);                           // the id of an open request on e's island, 0 none
extern Req reqs[REQ_N];

// ---- coop_ent_rec.c: reconcile, bonds, the glue across owners (S 7, 8) -----------------------------------------------------
void rec_pass(Cpu *c);                                             // step 9
void rec_glue_made(Cpu *c, const Ev *ev);                          // S 8.1
void rec_edge_found(Cpu *c, const Bond *b);                        // an owned-foreign edge with no operation
void rec_bond_req(Cpu *c, const CoopCtrMsg *m);                    // S 8.3
void rec_bond_ans(Cpu *c, const CoopCtrMsg *m, int ack);           // S 8.4
void rec_ops(Cpu *c);                                              // step 7: operations, requests in service
int rec_op_open_for(const Ent *e);                                 // an operation of this game touches e's island
int rec_break(Cpu *c, const Bond *b);                              // the break message; read back by the caller
int rec_make(Cpu *c, Ent *a, uint8_t sa, Ent *b, uint8_t sb, const float rel[12], uint8_t *why);   // S 7.3 steps 3 to 5
int rec_part_count(Cpu *c, Ent *e);                                // max(game group count, our union) of its island
void rec_owned_now(Cpu *c, Ent **members, int n);                  // strain fields zeroed (S 7.6)
extern Op ops[OP_N];

// ---- coop_ctn.c: the kept code ---------------------------------------------------------------------------------------------
int ctn_alive(uint64_t a);
uint32_t ctn_gen_of(uint64_t a);
int ctn_alive_as(uint64_t a, uint32_t gen);
uint64_t ctn_main_body(uint64_t a);
uint64_t ctn_spr(uint64_t a);
uint64_t ctn_part_group(uint64_t a);
int ctn_part_green(uint64_t a);
uint64_t ctn_stick_rider(uint64_t a);
int ctn_bodies(Cpu *c, uint64_t actor, uint64_t *body, uint8_t *key, int max);          // S 3.2: the part's body list
uint64_t ctn_body_by_key(Cpu *c, uint64_t actor, uint8_t key);
typedef struct { uint64_t edge, other; uint8_t key, okey, end_a, ed; } CtnEdge;          // end_a: `actor` is the edge's end A
int ctn_part_edges(Cpu *c, uint64_t actor, CtnEdge *out, int max);
int ctn_edge_held(Cpu *c, uint64_t edge, float held[12]);                                 // outA * inverse(outB)
uint64_t ctn_body_actor(uint64_t body);                                                   // the actor a rigid body is of
int ctn_group_count(uint64_t actor);                                                      // the game's count of its group (1: none)
float ctn_delete_radius(uint64_t a);
int ctn_part_delete(Cpu *c, uint64_t a);
void ctn_m34_mul(float out[12], const float a[12], const float b[12]);
int ctn_m34_inv(float out[12], const float m[12]);
uint64_t ctn_call5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
// the recipe (S 9.1) and the maker (S 9.2)
int ctn_recipe(Cpu *c, Ent **members, int n, uint8_t *rec, Eid *rec_member);            // record parts, 0: none
int ctn_make_start(Cpu *c, int st_i);                                                    // 0: the maker is busy or waits
void ctn_make_pass(Cpu *c);
int ctn_make_busy(void);
int ctn_make_waits(const float root_m[12], float reach);                                 // a design selected, a preview open, the Link in the way
// devices and the drive, from the group views
void ctn_phase0_kept(Cpu *c);              // events_put, deaths_put, dev_flush, rider_weight, floats, springs, burning
void ctn_drive(Cpu *c);                    // follow_rigid, dev_follow, floats_follow, events_out (the drive tick)
void ctn_owner_state(Cpu *c, Grp *g);      // at the owner: sw, on, fw, fk, fb, fp, burn, pf, payer of an owned island
void ctn_death_put(uint64_t actor, uint32_t gen, uint32_t kind);                         // replayed at phase 0
void ctn_event_put(uint64_t actor, uint32_t gen, uint8_t bits);
void ctn_toggle_put(Cpu *c, uint64_t actor, int want);
void ctn_teleport(Cpu *c, Grp *g);         // one SetTransform of a foreign island to its newest pose (S 12.1)
void ctn_place(Cpu *c, uint64_t actor, const float m[12]);   // one SetTransform of a main body, exempt from G1

// ---- The published sets (S 11): rebuilt by phase 0 each frame, read lock-free from any thread ---------------------------------
enum { ENT_SET_ENT, ENT_SET_OWNED, ENT_SET_FOLLOWER, ENT_SET_DRIVEN, ENT_SET_DRIVEN_BODY, ENT_SET_FOLLOWER_BODY, ENT_SET_COPY,
       ENT_SET_NOCHARGE, ENT_SET_PART, ENT_SET_REFUSE_STICK, ENT_SET_SHOOTER, ENT_SET_FOLLOW_GROUP, ENT_SET_COUNT };
int ent_set_has(int which, uint64_t key);
int ent_set_list(int which, uint64_t *out, int max);
void ent_enter_glued(Cpu *c, uint64_t actor);   // S 4.4: an end of a glue that is no entity yet
int coop_ent_ours_delete(void);                 // the thread is inside a delete this code asked
