// A ragdoll's pose on a copy (docs/coop/enemy-sync-spec.md B.7; research R12).
//
// A copy shows its owner's enemy by the root's place and the animation commands; a ragdoll is neither: its bones are
// driven by physics bodies in the game that runs the enemy. The copy's own ragdoll is not started (it would fight the
// static body and the written root, and two games' ragdolls part at the first bounce). While the owner's enemy is a
// full ragdoll or getting up (its ragdoll component's state [[comps+0x3e8]+0x20]: 3 full, 1 recovering), the owner's
// game sends the skeleton's local records as they stand after the enemy's model phase, every other frame, latest
// only; the copy's game writes the two newest, blended, over its own skeleton right after the actor's model phase
// (main+0x245030) and pushes the model, as the stand-ins' poses are put. A copy with no pose newer than 0.3 s shows
// its own animation again.
//
// The records are read after the owner's own model phase (the phase reads the ragdoll's bodies back into the pose,
// main+0x2752c8, and turns the pose into the records, main+0x289620), with the actor's place and turn of the same
// instant: bone 0's record is from the actor's matrix. The copy writes the records, sets the model's matrix from its
// own actor (it was found stale on a copy), marks the model changed (bit 0 of model+0x240: without it the push keeps
// the old final matrices) and pushes (research R12b). The actor's place and turn from the packet are NOT written
// (TOTK_COOP_EN_TEST_POSEROOT=1 writes them): the copy's root stays the stated one.
// Measured in two games (TOTK_COOP_EN_TEST_POSE=1: three bones' places from bone 0, per pose, in both): 2.3 cm
// median, 50 cm at worst (the copy's stated turn is a little behind the pose's); bone 0 moves 7 cm by the put.
// TOTK_COOP_EN_POSE=0 turns it off.
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

int coop_enemy_bones_pack(uint64_t a, uint8_t *out, int max);                                  // coop_world.c
int coop_enemy_bones_put(Cpu *c, uint64_t a, const uint8_t *pa, const uint8_t *pb, float f);   // coop_world.c
void rt_dispatch(Cpu *c);
uint64_t en_state_key(Enid id);   // coop_enemy_copy.c: the latest-only key of an enemy's messages
int coop_enemy_bone_world(uint64_t a, int bone, float out[3]);                                 // coop_world.c
// Test (TOTK_COOP_EN_TEST_POSE=1): three bones' places from the actor's, in the game that runs the ragdoll at each pose
// it sends and in the copy's game at the start of the copy's next calc after a pose was put, with the pose's number:
// the two agree when what was put is what the copy's skeleton holds for the frame's picture.
static int pose_test(void) {
    static int v = -1;
    if (v < 0) v = cfg_get("TOTK_COOP_EN_TEST_POSE") != NULL;
    return v;
}
static void pose_say(const char *who, uint64_t actor, uint32_t seq, EnCell *m) {
    static _Atomic int said;
    float b[3][3] = {{0}};
    if (!pose_test() || atomic_fetch_add(&said, 1) >= 400) return;
    coop_enemy_bone_world(actor, 5, b[0]), coop_enemy_bone_world(actor, 20, b[1]), coop_enemy_bone_world(actor, 40, b[2]);
    int16_t ei = atomic_load(&m->en);
    seq = (seq & 0xffff) | (uint32_t)(ei >= 0 ? (uint32_t)(en_tab[ei].id.a * 2654435761u) & 0x7fff0000u : 0);   // (the pose's number with its enemy's name in the high half)
    rt_log("coop: en: pose: test: %s pose %u: bone 5 at %.3f %.3f %.3f, bone 20 at %.3f %.3f %.3f, bone 40 at %.3f %.3f %.3f", who, seq, b[0][0], b[0][1], b[0][2], b[1][0], b[1][1], b[1][2], b[2][0], b[2][1], b[2][2]);
}
static _Atomic uint32_t put_seq[4096];   // per cell: the number of the pose last put on the copy (0 none)

#define POSE_MAX 1100
#define POSE_SLOTS 8
#define POSE_KEY 0x706f7365ull
typedef struct { uint64_t key; Enid id; uint32_t seq; uint16_t len, pad; uint8_t d[POSE_MAX]; } MPose;
_Static_assert(sizeof(MPose) <= COOP_CTR_PAYLOAD, "a pose fits one transport payload");
typedef struct {
    int cell;                       // -1 free
    uint32_t frame;                 // last use
    MPose out;                      // the owner's side: the pose to send
    int out_new;
    uint8_t in[2][POSE_MAX];        // the copy's side: the two newest poses and when each came
    uint64_t in_ns[2];
    uint32_t in_seq;
    int in_n;
} PoseSlot;
static PoseSlot slots[POSE_SLOTS];
static PlatMutex pose_lk = PLAT_MUTEX_INIT;
static int pose_init;
static _Atomic uint32_t n_sent, n_taken, n_put, n_miss;

static PoseSlot *slot_of(int cell, int make) {   // (under pose_lk)
    uint32_t frame = coop_sess_frame_no();
    PoseSlot *fr = NULL;
    if (!pose_init) {
        for (int i = 0; i < POSE_SLOTS; i++) slots[i].cell = -1;
        pose_init = 1;
    }
    for (int i = 0; i < POSE_SLOTS; i++) {
        if (slots[i].cell == cell) { slots[i].frame = frame; return &slots[i]; }
        if (slots[i].cell >= 0 && frame - slots[i].frame > 90) slots[i].cell = -1;
        if (slots[i].cell < 0 && !fr) fr = &slots[i];
    }
    if (!make || !fr) return NULL;
    memset(fr, 0, sizeof *fr);
    fr->cell = cell, fr->frame = frame;
    return fr;
}
static int pose_off(void) {
    static int v = -1;
    if (v < 0) { const char *e = cfg_get("TOTK_COOP_EN_POSE"); v = e && *e == '0'; }
    return v;
}

// The owner's enemy, at the start of its calc: its bones kept for phase 0 to send while it is a ragdoll.
void en_pose_precalc(uint64_t actor, EnCell *m) {
    uint64_t rg = coop_rd_ptr(en_comps(actor) + 0x3e8);
    int32_t rs = 0;
    int16_t ei = atomic_load(&m->en);
    if (pose_off() || !rg || !coop_rd(rg + 0x20, &rs, 4) || (rs != 3 && rs != 1) || ei < 0 || (coop_sess_frame_no() & 1)) return;
    // (called after the actor's model phase: the bodies have just been read back into the records)
    plat_mutex_lock(&pose_lk);
    PoseSlot *s = slot_of((int)(m - en_map), 1);
    if (s) {
        int len = coop_enemy_bones_pack(actor, s->out.d, sizeof s->out.d);
        if (len) s->out.key = en_state_key(en_tab[ei].id) ^ POSE_KEY, s->out.id = en_tab[ei].id, s->out.seq++, s->out.len = (uint16_t)len, s->out_new = 1;
        if (len) pose_say("sent", actor, s->out.seq, m);
    }
    plat_mutex_unlock(&pose_lk);
}

// Phase 0: this game's ragdolls' poses go out; the newest pose of each copy whose owner states a ragdoll is taken.
void en_pose_phase0(void) {
    uint64_t me = coop_ctr_me(), now = plat_now_ns();
    if (pose_off()) return;
    plat_mutex_lock(&pose_lk);
    for (int i = 0; pose_init && i < POSE_SLOTS; i++)
        if (slots[i].cell >= 0 && slots[i].out_new) {
            slots[i].out_new = 0;
            coop_ctr_send(0, ENM_POSE, 0, &slots[i].out, (int)offsetof(MPose, d) + slots[i].out.len);
            atomic_fetch_add(&n_sent, 1);
        }
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->owner == me || !e->owner || e->cell < 0 || e->life == EN_UNSYNCED) continue;
        uint8_t rag = en_map[e->cell].show_rag;
        if (rag != 3 && rag != 1) continue;
        int id = en_peer_id(e->owner);
        static MPose in;
        uint64_t age = 0;
        int len = id ? coop_ctr_latest(id, ENM_POSE, en_state_key(e->id) ^ POSE_KEY, &in, sizeof in, &age) : 0;
        if (len < (int)offsetof(MPose, d) + 30 || !enid_eq(in.id, e->id) || in.len > POSE_MAX || len != (int)offsetof(MPose, d) + in.len || age > 300000000ull) continue;
        PoseSlot *s = slot_of(e->cell, 1);
        if (!s || (s->in_n && s->in_seq == in.seq)) continue;
        if (s->in_n) memcpy(s->in[0], s->in[1], POSE_MAX), s->in_ns[0] = s->in_ns[1];
        memcpy(s->in[1], in.d, in.len), s->in_ns[1] = now, s->in_seq = in.seq;
        if (!s->in_n) memcpy(s->in[0], s->in[1], POSE_MAX), s->in_ns[0] = now;
        s->in_n++;
        atomic_fetch_add(&n_taken, 1);
    }
    plat_mutex_unlock(&pose_lk);
    static uint64_t said_ns;
    if (now - said_ns > 5000000000ull && (atomic_load(&n_sent) || atomic_load(&n_taken))) {
        said_ns = now;
        rt_log("coop: en: pose: %u ragdoll poses sent, %u taken, %u put on copies, %u not put (another number of bones)", atomic_load(&n_sent), atomic_load(&n_taken), atomic_load(&n_put), atomic_load(&n_miss));
    }
}

// The actor's model phase (main+0x245030, x0 the actor): for a copy with a fresh pose the phase runs, then the pose
// goes over what it made. 1: the phase was run here.
int en_pose_phase(Cpu *c, uint64_t addr) {
    static __thread int deep;
    static __thread uint8_t pa[POSE_MAX], pb[POSE_MAX];
    if (deep || addr != MOD_MAIN_BASE + 0x245030 || !coop_enemy_new() || pose_off()) return 0;
    uint64_t actor = c->x[0], now = plat_now_ns();
    EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
    if (m && atomic_load_explicit(&m->state, memory_order_relaxed) == EM_OWNED) {   // this game's own: the phase, then its bones are kept to send
        uint64_t rg = coop_rd_ptr(en_comps(actor) + 0x3e8);
        int32_t rs = 0;
        if (!rg || !coop_rd(rg + 0x20, &rs, 4) || (rs != 3 && rs != 1)) return 0;
        deep = 1, c->pc = addr;
        rt_dispatch(c);
        deep = 0;
        uint64_t r0 = c->x[0];
        en_pose_precalc(actor, m);
        c->x[0] = r0;
        return 1;
    }
    if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_COPY || (m->show_rag != 3 && m->show_rag != 1)) return 0;
    float f = 1;
    int have = 0;
    uint32_t seq = 0;
    plat_mutex_lock(&pose_lk);
    PoseSlot *s = pose_init ? slot_of((int)(m - en_map), 0) : NULL;
    if (s && s->in_n && now - s->in_ns[1] < 300000000ull) {
        uint64_t span = s->in_ns[1] - s->in_ns[0];
        memcpy(pa, s->in[0], POSE_MAX), memcpy(pb, s->in[1], POSE_MAX);
        f = span ? (float)((double)(now - s->in_ns[1]) / (double)span) : 1;   // (shown one interval late, so it is always between two)
        if (f > 1) f = 1;
        have = 1, seq = s->in_seq;
    }
    plat_mutex_unlock(&pose_lk);
    if (!have) return 0;
    deep = 1, c->pc = addr;
    rt_dispatch(c);
    deep = 0;
    uint64_t r0 = c->x[0];
    if (coop_enemy_bones_put(c, actor, pa, pb, f)) atomic_fetch_add(&n_put, 1), atomic_store(&put_seq[(m - en_map) & 4095], f >= 1 ? seq : 0);
    else atomic_fetch_add(&n_miss, 1);
    c->x[0] = r0;
    return 1;
}

// (test) The copy's calc start: what its skeleton holds after the last frame's put.
void en_pose_copy_precalc(uint64_t actor, EnCell *m) {
    uint32_t seq = pose_test() ? atomic_exchange(&put_seq[(m - en_map) & 4095], 0) : 0;
    if (seq) pose_say("shown", actor, seq, m);
}
