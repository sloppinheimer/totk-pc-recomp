// Co-op: the enemy sync's shared types (coop_enemy.c and its sibling files; docs/coop/enemy-sync-spec.md).
#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "coop_ctr.h"
#include "coop_game.h"
#include "cpu.h"
#include "platform/platform.h"

#define EN_N 256          // live entries
#define EN_MAP_N 4096     // actors of a listed kind this game has
#define EN_REST_N 1024    // resting records
#define EN_TOMB_N 2048    // tombstones
#define EN_SNAP_N 16      // states kept per copy
#define EN_SLOT_N 6       // animation slots stated
#define EN_CTRL_N 4       // blend controllers of a slot

typedef struct { uint64_t a; uint32_t b, zero; } Enid;   // placed: (placement hash, life number); made at run time: (the spawn sync's key, EN_RT)
#define EN_RT 0x80000001u
static inline int enid_eq(Enid x, Enid y) { return x.a == y.a && x.b == y.b; }

enum { EN_LIVE = 1, EN_DYING, EN_DEAD, EN_UNSYNCED };
// What this game does with an actor (a map cell's state): NEW held until phase 0 has looked at it; OWN the game's own
// (not synced, far, no session); HELD claimed, in the hold of its claim; OWNED this game runs it for all; COPY another
// game runs it and this actor shows it.
enum { EM_FREE, EM_NEW, EM_OWN, EM_HELD, EM_OWNED, EM_COPY };

typedef struct {
    int64_t ns;                       // the owner's frame stamp
    float pos[3], quat[4], vel[3];
    uint8_t teleports, flags, pad[2];
} EnSnap;
typedef struct {
    uint32_t cmd, play_no;            // the command's name hash, the number of the play that set it
    float rate;
    uint8_t cur, n, pad[2];
    struct { float clock, weight; } c[EN_CTRL_N];
} EnSlot;
#define EN_EQ_N 8         // equipment slots
typedef struct { EnSnap s; int32_t hp; uint32_t dstate; uint8_t nslot, rag, hide, vis, mem, verb, tgt, wx; uint32_t eq[EN_EQ_N]; uint32_t att[EN_EQ_N]; uint8_t eqs[EN_EQ_N]; uint32_t binfo, binfo2; uint8_t blink, gb, arm[2]; float look[4]; uint8_t lk[4]; EnSlot sl[EN_SLOT_N]; } EnState;   // look, lk: its model controller's looks (B.33): floats +0x4c +0x54 +0x58 +0x5c, bytes +0x9b +0x9c +0x9e, lk[3] 1 when stated;   // gb: 0x80 stated, bit 0 its attack is a swift one (B.31);   // eq: the name hash of what each equipment slot holds   // one stated moment of an enemy
// (hide: the model controller's "not drawn" byte, [comps+0x280]+0x9d; vis: actor+0x444 bit 23)

typedef struct {
    _Atomic uint64_t actor;           // 0 free, 1 cleared
    _Atomic uint32_t gen, frame;      // frame: the frame number of its last calc
    _Atomic uint32_t run_frame;       // the frame a graph runner of it was last entered (let through or turned away): the game thinks it
    uint32_t born_frame;              // the frame its cell was made
    _Atomic uint8_t warm;             // its interrupt graph has started (or 30 frames passed): only then may the switch turn it off
    _Atomic uint8_t state;            // EM_*
    _Atomic int16_t en;               // its table row, -1 none
    uint8_t kind, hash_read, retry, rt;
    uint64_t hash;                    // its placement hash, or (rt) the key coop_world.c's spawn sync gave an enemy made at run time; 0 neither yet
    uint64_t new_ns;                  // since when it is NEW
    // What pre-calc has done to the actor (its update thread only).
    uint8_t sw_on, sw_saved, body_static, pinned;
    uint32_t sw_qcount; uint16_t sw_bits; int32_t sw_root; int16_t sw_pend;   // what the four writes overwrote
    float pin[7];                     // where a held actor stands
    uint32_t slot_try[EN_SLOT_N];     // the frame a slot's command was last asked for
    _Atomic uint32_t turned, leaks, passed;   // entries the switch turned away; node ticks that got through; entries let through (it thinks)
    _Atomic uint32_t n_play, n_hard, n_soft, n_off;   // plays asked, clocks written, rates bent, frames not at the pose asked
    // What pre-calc shows (phase 0 writes it under the sequence, pre-calc reads it whole).
    _Atomic uint32_t seq;
    float show_pos[3], show_quat[4];
    uint8_t show_on, show_teleport;
    EnSlot show_slot[EN_SLOT_N];
    uint8_t show_nslot, show_hold;
    int32_t show_hp; uint32_t show_dstate; uint8_t show_rag, show_hide, show_mem, show_verb, show_tgt;
    uint32_t eq_att[EN_EQ_N]; uint8_t eq_state[EN_EQ_N], eq_att_tries[EN_EQ_N]; uint32_t eq_state_frame;   // what is fused to each slot's thing at its owner's (name hash), and whether it is drawn (2) or put away (4)
    uint8_t throw_slot, throw_due; uint64_t throw_x3, throw_x4; uint8_t throw_req[0x30];   // a throw of its owner's enemy to make here (B.14)
    _Atomic uint8_t tgt_back;   // the player (1 + net id) its target slot is to name again: a change too soon after the last (B.22)
    uint8_t show_gb;   // its owner's game-balance bits, as EnState.gb (B.31)
    uint8_t show_arm[2];   // its owner's attack layer override and hit-receive-off bit (0x80 stated), as EnState.arm (B.35)
    float show_look[4]; uint8_t show_lk[4];   // its owner's model looks, as EnState.look and lk (B.33)
    uint32_t show_binfo; uint8_t show_blink, show_bhave;   // its owner's battle-info flags and whether it is in the battle manager's list (B.20)
    uint8_t wx_type, show_wx; uint32_t wx_frame;   // the magic weather its graph last asked for (type + 1) and when; what its owner states
    _Atomic uint8_t out_cls, out_frames;   // how another player took this enemy's blow (1 guarded, 3 at the right moment), to show its own sensor for a few frames (B.27)
    uint32_t out_id;      // the attack id of the copy's blow whose outcome on this game's Link was last told to its owner
    uint32_t fwd_id;      // the attack id of the contact last sent whole (one blow is sent once)
    uint32_t fwd_frame;   // the frame this game's player's contact on the copy was last sent whole to its owner (its lost life is then not sent as points too)
    uint8_t grace_verb, grace_tgt; uint32_t grace_frame, hold_frame, brain_log_frame;   // a taken-over enemy's brain is held at its owner's verb and target until this frame (B.6)
    // Hits and death (coop_enemy_hit.c).
    _Atomic int32_t hit_dmg;          // life this game's hits took from the copy since phase 0 last looked
    int32_t hit_hp; uint64_t hit_ns;  // the copy's life after this game's last hit, and when
    int32_t hp_written;               // the life pre-calc last gave the copy (0: none yet)
    _Atomic uint8_t eq_drop;          // equipment slots (a bit each) whose held actor the copy must let go: its owner's enemy holds nothing there
    _Atomic uint32_t eq_make[EN_EQ_N];   // per equipment slot, the name hash of what the copy must be given (its owner's enemy holds it, the copy nothing)
    uint32_t eq_made_frame;           // the frame of the copy's last equipment create
    uint8_t eq_tries[EN_EQ_N];        // creates made for a slot that stayed empty: after 2 no more (30 shields were made on one copy)
    uint8_t eq_str[2][0x60];          // the strings of that create (the game may read them after the call)
    _Atomic uint8_t dying, kill_req, put_away, flinch;   // asked of pre-calc: the copy's death, the owner's death, the actor of a dead placement put away, a flinch
    uint8_t put_done, dc_saved, dc_bits, req_tries;
} EnCell;

typedef struct {
    uint8_t used, life, kind, have_decl, offered, offer_all, stale;
    Enid id;
    uint64_t owner; uint32_t epoch;
    char name[48];
    uint64_t actor; uint32_t gen; int16_t cell;   // this game's actor for it (cell -1: none)
    int32_t hp; float pose[7];        // as last read (owned) or stated
    uint64_t made_ns, hold_until_ns, notrun_ns, offer_ns, no_offer_until_ns, offer_seen_ns, my_hit_ns, need_ns, state_ns, owner_ns;
    uint64_t offer_to;                // the game an offer is for (0: any that runs it)
    uint64_t hit_by_ns; int hit_by;   // when another game's player last struck this game's enemy, and that player's id
    uint32_t decl_no, changes, hit_serial; int32_t pend_dmg; uint8_t send_burst;
    // motion and animation (coop_enemy_copy.c)
    EnState st[EN_SNAP_N]; uint32_t snap_w; uint8_t teleports, tele_seen;
    EnState cur;                      // an owned enemy as last read
    uint64_t read_ns, sent_ns, log_ns;
    // the record
    uint8_t tgt_cur; uint64_t tgt_since_ns;   // which player its target slot names, and since when (B.22: no flicker)
    uint8_t attn[0x40], attn_have;   // the attention sender's client reason bytes, as this enemy's owner has them (B.16)
    uint32_t st_used, st_lost, st_held, eq_off, eq_att_off; float err_sum, err_max; uint32_t err_n;
} En;

extern PlatMutex en_lock;
extern En en_tab[EN_N];
extern EnCell en_map[EN_MAP_N];
extern _Atomic int en_world_stands;   // this game's enemies do not think though they should: it is in a menu
extern _Atomic int en_active;        // a session with every peer on this sync, and this game not loading

int coop_enemy_new(void);
int en_test_skip(char what);                        // TOTK_COOP_EN_TEST_SKIP=<letters>: a part of the copy left out, for finding a fault
EnCell *en_cell_of(uint64_t actor);                 // any thread; NULL when the actor is not of a listed kind
int en_cell_state(uint64_t actor);                  // EM_* (EM_FREE: not ours)
uint64_t en_now(void);
int en_peer_id(uint64_t inc);
void en_send(int id, int type, const void *d, int len);        // reliable, in order, kept when the transport is full
void en_send_all(int type, const void *d, int len);
void en_log_own(const En *e, const char *what, const char *why);

// coop_enemy_copy.c
void en_copy_precalc(Cpu *c, uint64_t actor, EnCell *m);       // on the actor's update thread, before its calc
int en_copy_hook(Cpu *c, uint64_t off);                        // the switch: nonzero, the function is not run
void en_copy_phase0(Cpu *c);                                   // states read and sent, states taken, what copies show
void en_copy_burst(int id);                                    // the command names, to a peer that came
int en_copy_msg(const CoopCtrMsg *m, int from);                // 1: the message was this file's

// coop_mount.c (horses and animals: docs/coop/horse-animal-sync-spec.md)
void coop_mount_scan(void);                                    // under en_lock, at the top of the ownership pass
int coop_mount_keep(const En *e);                              // owned, sat on by this game's actor: not handed over
int coop_mount_ridden(const En *e);                            // another game's, stated ridden: not taken for a pause
int coop_mount_claim(const En *e);                             // another game's, sat on by this game's actor: claimed
void coop_mount_forget(int i, int cell);                       // an entry freed (cell) or changing owner (cell -1)
void coop_mount_phase0(Cpu *c);
void coop_mount_owner(Cpu *c, En *e, int due);                 // an owned entry's MOUNT, with its STATE
void coop_mount_take(En *e, int from);                         // a copy's latest MOUNT
int coop_mount_precalc_ride(Cpu *c, uint64_t actor, EnCell *m);   // 1: it sits on a horse here, its pose is not written
void coop_mount_precalc(Cpu *c, uint64_t actor, EnCell *m);    // the gait values and the tack

// coop_enemy_shot.c
int en_shot_hook(Cpu *c, uint64_t off, EnCell *m);              // a prepare or a shoot of an enemy this game runs: made here and captured
void en_shot_T(int id, int64_t t);                             // the time this game shows a peer at, this frame
void en_shot_phase0(void);
int en_shot_msg(const CoopCtrMsg *m, int from);
void en_shot_precalc(Cpu *c, uint64_t actor, EnCell *m);       // a copy makes the stated calls on its own records

// coop_enemy_hit.c
void en_hit_phase0(void);                                      // hits sent and applied, deaths seen
void en_hit_precalc(Cpu *c, uint64_t actor, EnCell *m, int off);   // life, death and the put-away on the actor's thread
void en_hit_leave(uint64_t actor, EnCell *m);                  // a copy thinks again: its death switches as they were
int en_tomb_add(uint64_t hash, uint32_t no, int flag);
uint32_t en_life_no(void);
void en_dead(En *e, const char *why);                          // the owner's: its actor is gone after its death
void en_entry_free(En *e);

// Reads of the game (any thread; 0 or the neutral value when a pointer is missing).
static inline uint64_t en_comps(uint64_t a) { return a ? coop_rd_ptr(a + 0x228) : 0; }
static inline uint64_t en_bsa(uint64_t a) { uint64_t k = en_comps(a); return k ? coop_rd_ptr(k + 0x158) : 0; }
uint64_t en_hash_of(uint64_t actor);
int en_gate_open(uint64_t actor);
int32_t en_hp_of(uint64_t actor);
int en_alive(uint64_t actor, uint32_t gen);
void en_pose_of(uint64_t actor, float pose[7]);
void en_quat_of_rot(const float r[9], float q[4]);
void en_rot_of_quat(const float q[4], float r[9]);

// Message types (60 to 89, through coop_msg.c). Not used, kept free: 66, 70, 73, 81, 85 (the design's
// HITACK, KILL, ORPHAN_ACK, PROJ_END and DEATH_SIGNAL, which the state-based sync never needed).
enum {
    ENM_STATE = 60, ENM_ARM = 61, ENM_DECL = 62, ENM_PLAY = 63, ENM_SHS = 64, ENM_HIT = 65,
    ENM_OUTCOME = 67, ENM_DEATH = 68, ENM_DEATH_END = 69, ENM_TOMBS = 71, ENM_CLAIM = 72,
    ENM_REST = 74, ENM_OFFER = 75, ENM_GONE = 76, ENM_NEED = 77, ENM_INDEX = 78, ENM_SYNC_END = 79,
    ENM_PROJ = 80, ENM_EQUIP = 82, ENM_MOON = 83, ENM_UNSYNCED = 84, ENM_POSE = 86, ENM_ATTN = 87, ENM_FX = 88, ENM_PICK = 89,
};
