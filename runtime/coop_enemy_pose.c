// A ragdoll's pose on a copy (docs/coop/enemy-sync.md B.7; research R12).
//
// A copy shows its owner's enemy by the root's place and the animation commands; a ragdoll is neither: its bones are
// driven by physics bodies in the game that runs the enemy. The copy's own ragdoll is not started (it would fight the
// static body and the written root, and two games' ragdolls part at the first bounce). While the owner's enemy is in
// any ragdoll state (its ragdoll component's state [[comps+0x3e8]+0x20]: 3 full, 2 partial: a flinch, 1 recovering), the owner's
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

void rt_dispatch(Cpu *c);
uint64_t en_state_key(Enid id);   // coop_enemy_copy.c: the latest-only key of an enemy's messages
// Test (TOTK_COOP_EN_TEST_POSE=1): three bones' places from the actor's, in the game that runs the ragdoll at each pose
// it sends and in the copy's game at the start of the copy's next calc after a pose was put, with the pose's number:
// the two agree when what was put is what the copy's skeleton holds for the frame's picture.
static int coop_enemy_bone_world(uint64_t a, int bone, float out[3]);
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

// ---- The skeleton and the small math the bones need (the same as coop_world.c's, which the stand-ins' poses use) -------
#define POSE_BONE 12
#define PUP_BONES 330
static int16_t q16(float v, float scale) {
    float x = v * scale;
    return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
}

static void mat_quat(const float r[9], float q[4]) {   // rows; q x y z w
    float trc = r[0] + r[4] + r[8];
    if (trc > 0) { float s = sqrtf(trc + 1) * 2; q[3] = s / 4, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s; }
    else if (r[0] > r[4] && r[0] > r[8]) { float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2; q[3] = (r[7] - r[5]) / s, q[0] = s / 4, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s; }
    else if (r[4] > r[8]) { float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2; q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = s / 4, q[2] = (r[5] + r[7]) / s; }
    else { float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2; q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = s / 4; }
}

static void quat_mat(const float q[4], float r[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}

static void qblend(const float a[4], const float b[4], float f, float out[4]) {
    float dot = 0, l = 0;
    for (int k = 0; k < 4; k++) dot += a[k] * b[k];
    float sg = dot < 0 ? -1.0f : 1.0f;
    for (int k = 0; k < 4; k++) out[k] = a[k] * (1 - f) + b[k] * sg * f, l += out[k] * out[k];
    l = sqrtf(l);
    for (int k = 0; k < 4; k++) out[k] = l > 0 ? out[k] / l : k == 3;
}

static int is_skeleton(uint64_t s) {
    uint64_t w[0xd];
    uint16_t n = 0;
    if (!coop_rd(s, w, sizeof w) || !coop_rd(s + 0x48, &n, 2)) return 0;
    return n >= 20 && n <= 600 && w[4] && w[4] == w[0xc] && (w[3] - w[4] == (uint64_t)n * 0x40 || w[0xb] - w[4] == (uint64_t)n * 0x40);
}

static uint64_t enemy_model(uint64_t a) { return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x8) + 0x28); }
// An actor's body skeleton from its model: model +0x18 +0 +0x58 +0xb0.
static uint64_t enemy_skeleton(uint64_t a) {
    static const uint32_t path[4] = {0x18, 0, 0x58, 0xb0};
    uint64_t sk = enemy_model(a);
    for (int i = 0; i < 4 && sk; i++) sk = coop_rd_ptr(sk + path[i]);
    return sk && is_skeleton(sk) ? sk : 0;
}

// ---- An enemy's bones, packed and put -------------------------------------------------------------------------------------
// The skeleton's local records as epose_pack sends them: u16 bones, then 12 bytes a bone (the rotation's smallest three
// quaternion components and the translation in 1/4096 m). 0: no skeleton, or it does not fit.
#define POSE_ROOT 30   // u16 bones, the actor's place (3 floats) and turn (a quaternion)
static int coop_enemy_bones_pack(uint64_t a, uint8_t *out, int max) {
    uint64_t sk = enemy_skeleton(a), recs = sk ? coop_rd_ptr(sk + 0x18) : 0;
    uint16_t n = 0;
    static __thread float rec[PUP_BONES][24];
    // (the records themselves: a ragdoll's bodies are read back into the pose in the actor's model phase, main+0x2752c8
    // inside main+0x245030, and main+0x289620 turns the pose into these records; research R12b. Read after that phase.)
    if (!recs || !coop_rd(sk + 0x48, &n, 2) || n == 0 || n > PUP_BONES || POSE_ROOT + n * POSE_BONE > max || !coop_rd(recs, rec, (size_t)n * 0x60)) return 0;
    memcpy(out, &n, 2);
    {   // (bone 0's record is from the actor's matrix: the pose is whole only with the actor's place and turn of the instant)
        float rp[3] = {0}, rr[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, rq[4];
        coop_rd(a + COOP_ACTOR_POS, rp, 12), coop_rd(a + COOP_ACTOR_ROT, rr, 36);
        mat_quat(rr, rq);
        memcpy(out + 2, rp, 12), memcpy(out + 14, rq, 16);
    }
    for (int b = 0; b < n; b++) {
        const float *f = rec[b];
        float R[9] = {f[8], f[12], f[16], f[9], f[13], f[17], f[10], f[14], f[18]}, q[4];
        mat_quat(R, q);
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        int big = 0;
        for (int k = 0; k < 4; k++) { q[k] = len > 0 ? q[k] / len : (k == 3); if (fabsf(q[k]) > fabsf(q[big])) big = k; }
        float sign = q[big] < 0 ? -1.0f : 1.0f;
        int16_t v[6];
        for (int k = 0, j = 0; k < 4; k++) if (k != big) v[j++] = q16(q[k] * sign, 32767.0f);
        v[0] = (int16_t)((v[0] & ~3) | big);
        v[3] = q16(f[20], 4096), v[4] = q16(f[21], 4096), v[5] = q16(f[22], 4096);
        memcpy(out + POSE_ROOT + b * POSE_BONE, v, POSE_BONE);
    }
    return POSE_ROOT + n * POSE_BONE;
}
static void bone_unpack(const uint8_t *in, float q[4], float t[3]) {
    int16_t v[6];
    memcpy(v, in, POSE_BONE);
    int big = v[0] & 3;
    float s = 0;
    for (int k = 0, j = 0; k < 4; k++)
        if (k != big) {
            q[k] = (float)(j ? v[j] : (int16_t)(v[0] & ~3)) / 32767.0f;
            s += q[k] * q[k], j++;
        }
    q[big] = s < 1 ? sqrtf(1 - s) : 0;
    for (int k = 0; k < 3; k++) t[k] = (float)v[3 + k] / 4096.0f;
}
// Two packed poses blended (f from the first to the second) and written over the actor's skeleton, then the model's push
// (main+0x24cc30), as the stand-ins' poses are put. 0: the skeleton here has another number of bones.
static int coop_enemy_bones_put(Cpu *c, uint64_t a, const uint8_t *pa, const uint8_t *pb, float f) {
    uint64_t sk = enemy_skeleton(a), recs = sk ? coop_rd_ptr(sk + 0x18) : 0, model = enemy_model(a);
    uint16_t have = 0, na = 0, nb = 0;
    memcpy(&na, pa, 2), memcpy(&nb, pb, 2);
    // (a pose holds at most (1100 - 30) / 12 = 89 bones: a count above it, from another game, read past the buffers)
    if (!recs || !coop_rd(sk + 0x48, &have, 2) || have != na || na != nb || na > (1100 - POSE_ROOT) / POSE_BONE) return 0;
    for (int b = 0; b < na; b++) {
        float qa[4], qb[4], q[4], ta[3], tb[3], R[9];
        bone_unpack(pa + POSE_ROOT + b * POSE_BONE, qa, ta), bone_unpack(pb + POSE_ROOT + b * POSE_BONE, qb, tb);
        qblend(qa, qb, f, q), quat_mat(q, R);
        float *rec = (float *)(uintptr_t)(recs + (uint64_t)b * 0x60);
        for (int k = 0; k < 3; k++) rec[20 + k] = ta[k] + (tb[k] - ta[k]) * f;
        for (int cl = 0; cl < 3; cl++) for (int k = 0; k < 3; k++) rec[8 + cl * 4 + k] = R[k * 3 + cl];
    }
    {   // the actor and its model stand where the owner's stood at the pose's instant
        float pA[3], pB[3], qA[4], qB[4], pos[3], q[4], rot[9];
        _Alignas(16) float m[12];
        memcpy(pA, pa + 2, 12), memcpy(qA, pa + 14, 16), memcpy(pB, pb + 2, 12), memcpy(qB, pb + 14, 16);
        for (int k = 0; k < 3; k++) pos[k] = pA[k] + (pB[k] - pA[k]) * f;
        qblend(qA, qB, f, q), quat_mat(q, rot);
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) m[r * 4 + k] = rot[r * 3 + k];
            m[r * 4 + 3] = pos[r];
        }
        // NOT written (TOTK_COOP_EN_TEST_POSEROOT=1 writes them): with the actor and the model's matrix written from
        // the packet, the other player saw a ragdolled copy soar into the sky (a play test, 2026-11-16), while the
        // bones from bone 0 agreed to 0.2 cm: the model's matrix is not simply the actor's place and turn here. The
        // copy's root stays the stated one; what the model's matrix holds against the packet is logged.
        static int root = -1;
        if (root < 0) root = cfg_get("TOTK_COOP_EN_TEST_POSEROOT") != NULL;
        if (!root && model) {   // the model's matrix is this copy's own actor's, as it stands now (it was found stale: 5.5 m from the actor)
            float ap[3] = {0}, ar[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            _Alignas(16) float am[12];
            coop_rd(a + COOP_ACTOR_POS, ap, 12), coop_rd(a + COOP_ACTOR_ROT, ar, 36);
            for (int r = 0; r < 3; r++) {
                for (int k = 0; k < 3; k++) am[r * 4 + k] = ar[r * 3 + k];
                am[r * 4 + 3] = ap[r];
            }
            memcpy((void *)(uintptr_t)(model + 0x1f8), am, sizeof am);
        }
        float own[12] = {0};
        if (model) coop_rd(model + 0x1f8, own, sizeof own);
        static _Atomic int said;
        if (model && atomic_fetch_add(&said, 1) % 30 == 0 && atomic_load(&said) < 1200)
            rt_log("coop: en: pose: root: the copy's model matrix holds %.2f %.2f %.2f (rows %.2f %.2f %.2f | %.2f %.2f %.2f), its owner's actor was at %.2f %.2f %.2f (rows %.2f %.2f %.2f | %.2f %.2f %.2f)",
                   own[3], own[7], own[11], own[0], own[1], own[2], own[4], own[5], own[6], pos[0], pos[1], pos[2], m[0], m[1], m[2], m[4], m[5], m[6]);
        if (root) {
            memcpy((void *)(uintptr_t)(a + COOP_ACTOR_POS), pos, 12), memcpy((void *)(uintptr_t)(a + COOP_ACTOR_ROT), rot, 36);
            if (model) memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        }
    }
    // (the push builds the final matrices only for a model marked changed: bit 0 of model+0x240, which the actor's own
    // phase has just cleared; without it the copy kept its own animation's finals. Research R12b 3.A.)
    float f0[16] = {0}, f1[16] = {0};   // (bone 0's final matrix before and after: how far the put moved the whole model)
    uint64_t wmx = coop_rd_ptr(sk + 0x20);
    if (wmx) coop_rd(wmx, f0, sizeof f0);
    if (model) ST8(model + 0x240, LD8(model + 0x240) | 1), coop_guest_call(c, MOD_MAIN_BASE + 0x24cc30, model, 0, 0, 0);
    if (wmx) coop_rd(wmx, f1, sizeof f1);
    {
        static _Atomic int said;
        float d = sqrtf((f1[12] - f0[12]) * (f1[12] - f0[12]) + (f1[13] - f0[13]) * (f1[13] - f0[13]) + (f1[14] - f0[14]) * (f1[14] - f0[14]));
        if ((d > 2.0f || atomic_load(&said) % 20 == 0) && atomic_fetch_add(&said, 1) < 400) rt_log("coop: en: pose: put: bone 0 moved %.2f m by the put (from %.2f %.2f %.2f to %.2f %.2f %.2f)", d, f0[12], f0[13], f0[14], f1[12], f1[13], f1[14]);
        else atomic_fetch_add(&said, 0);
    }
    return 1;
}
// (test: where a bone is, from the actor's place, by the skeleton's world matrices; coop_enemy_pose.c lays the two games' side by side)
static int coop_enemy_bone_world(uint64_t a, int bone, float out[3]) {
    uint64_t sk = enemy_skeleton(a), wmx = sk ? coop_rd_ptr(sk + 0x20) : 0;
    uint16_t n = 0;
    float m[16], p[3];
    if (!wmx || !coop_rd(sk + 0x48, &n, 2) || bone >= n || !coop_rd(wmx + (uint64_t)bone * 0x40, m, sizeof m) || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return 0;
    (void)p;
    float m0[16];   // (from bone 0: the finals are from an origin of each game's own, so only differences compare)
    if (!coop_rd(wmx, m0, sizeof m0)) return 0;
    out[0] = m[12] - m0[12], out[1] = m[13] - m0[13], out[2] = m[14] - m0[14];
    return 1;
}

#define POSE_MAX 1100
_Static_assert(POSE_MAX == 1100, "coop_enemy_bones_put bounds the bone count by 1100 bytes");
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
    if (pose_off() || !rg || !coop_rd(rg + 0x20, &rs, 4) || !rs || ei < 0 || (coop_sess_frame_no() & 1)) return;
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
        if (!rag) continue;
        int id = en_peer_id(e->owner);
        static MPose in;
        uint64_t age = 0;
        int len = id ? coop_ctr_latest(id, ENM_POSE, en_state_key(e->id) ^ POSE_KEY, &in, sizeof in, &age) : 0;
        if (len < (int)offsetof(MPose, d) + 30 || !enid_eq(in.id, e->id) || in.len > POSE_MAX || len != (int)offsetof(MPose, d) + in.len || age > 300000000ull) continue;
        {   // the bones it says it holds are in it, and its root is a place and a turn (another game's packet: checked)
            uint16_t nbones = 0;
            float root[7];
            memcpy(&nbones, in.d, 2), memcpy(root, in.d + 2, sizeof root);
            int ok = in.len >= POSE_ROOT + nbones * POSE_BONE;
            for (int k = 0; k < 7; k++) ok &= isfinite(root[k]) && fabsf(root[k]) < 1e5f;
            if (!ok) continue;
        }
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
        if (!rg || !coop_rd(rg + 0x20, &rs, 4) || !rs) return 0;
        deep = 1, c->pc = addr;
        rt_dispatch(c);
        deep = 0;
        uint64_t r0 = c->x[0];
        en_pose_precalc(actor, m);
        c->x[0] = r0;
        return 1;
    }
    if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_COPY || !m->show_rag) return 0;
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
