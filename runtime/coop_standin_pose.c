// The players' body poses and the stand-in that shows them: moved out of coop_world.c as it was
// (docs/coop/enemy-sync.md B.28: the world file is taken apart so the older enemy sync in it can go). Nothing
// here is the enemy sync's.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// ---- body poses: each player's body pose goes to the others as a POSE packet and is put on
// their copy of that player's Link right after main+0x283c750 (the world-matrix writer); that copy gets no input
// (coop_players.c), so it does nothing in this game (2026-11-05).
// Packet: u16 bone count, u16 0, then per bone 12 bytes: the rotation as the smallest three of its quaternion
// (3 x s16, the dropped component's index in the low 2 bits of the first) and the position in model space
// (3 x s16, 1/4096 m).
int16_t coop_pose_q16(float v, float scale) {
    float x = v * scale;
    return (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
}
// The body's input pose, as main+0x283c750 (x0 the pose, x1 the SkeletonObj it writes) takes it: u16 bone count at
// pose +0xa, [pose] the bones, 0x50 each: +0x00 scale (3 floats), +0x10 translation, +0x20 and +0x30 the first two
// rows of the rotation (the third is their cross product), +0x40 left alone (2026-11-05).
// Packet: u16 bones, u16 scaled count, then per bone 12 bytes: the rotation's smallest three quaternion components
// (3 x s16, the dropped one's index in the low 2 bits of the first) and the translation (3 x s16, 1/4096 m); then
// per bone whose scale is not 1: u16 index + 3 floats.
#define POSE_IN_STRIDE 0x50
// This Link's latest packed pose (from the hook, any thread) and the tick that sends it.
static uint8_t pose_latest[COOP_POSE_MAX];
static _Atomic int pose_latest_len;
static PlatMutex pose_lock = PLAT_MUTEX_INIT;
// The co-op Links' body skeletons and their players' net ids, cached once a tick for the writer hook.
static _Atomic uint64_t puppet_skel[COOP_MAX_PLAYERS], puppet_pose[COOP_MAX_PLAYERS];
static _Atomic int puppet_id[COOP_MAX_PLAYERS], puppet_n;
// Proxy mode: the input pose object main+0x283c750 was last called with for a co-op Link's body skeleton (kept from
// before its update stopped; the proxy driver calls the writer with it). 0: not seen yet.
uint64_t coop_puppet_pose_obj(uint64_t skel) {
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++)
        if (skel == atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) return atomic_load(&puppet_pose[i]);
    return 0;
}
static _Atomic uint64_t my_body_skel;
void coop_puppet_tick(uint64_t me) {
    uint64_t s0 = me;
    for (int i = 0; i < 8 && s0; i++) s0 = coop_rd_ptr(s0 + coop_skel_path[i]);
    atomic_store(&my_body_skel, s0 && coop_is_skeleton(s0) ? s0 : 0);
    static uint8_t pkt[COOP_POSE_MAX];
    plat_mutex_lock(&pose_lock);
    int len = atomic_load(&pose_latest_len);
    if (len) memcpy(pkt, pose_latest, len);
    plat_mutex_unlock(&pose_lock);
    if (len) coop_net_pose_send(pkt, len);
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(acts, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) {
        uint64_t s = acts[i];
        for (int k = 0; k < 8 && s; k++) s = coop_rd_ptr(s + coop_skel_path[k]);
        uint64_t sk = s && coop_is_skeleton(s) ? s : 0;
        if (sk != atomic_load(&puppet_skel[i])) atomic_store(&puppet_pose[i], 0);
        atomic_store(&puppet_skel[i], sk), atomic_store(&puppet_id[i], ids[i]);
    }
    atomic_store(&puppet_n, n);
    static int said;
    if (len && n && !said++) rt_log("coop: puppet mode: poses go out (%d bytes) and onto %d co-op Link(s)", len, n);
}
// From the hook on main+0x283c750, before it runs (x0 the input pose, x1 the skeleton): this Link's pose is packed
// for the network; a co-op Link's gets its player's latest. Returns 1 when x1 was a co-op Link's.
int coop_puppet_is_co_skeleton(uint64_t skel) {
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++)
        if (skel == atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) return 1;
    return 0;
}
// TOTK_COOP_POSE_LOG=<s>: for 3 s from that world time, bones 0..3's translation in each pose sent and put on.
static void pose_log(const char *what, uint64_t pose) {
    static double at = -2;
    if (at == -2) { const char *e = cfg_get("TOTK_COOP_POSE_LOG"); at = e ? atof(e) : -1; }
    double s = frame_step_world_us() / 1e6;
    if (at < 0 || s < at || s > at + 3) return;
    uint64_t arr = coop_rd_ptr(pose);
    float f[4][20];
    if (!arr || !coop_rd(arr, f, sizeof f)) return;
    rt_log("coop: pose %s %.3f s: b0 %.3f %.3f %.3f  b1 %.3f %.3f %.3f  b2 %.3f %.3f %.3f  b3 %.3f %.3f %.3f", what, s,
           f[0][4], f[0][5], f[0][6], f[1][4], f[1][5], f[1][6], f[2][4], f[2][5], f[2][6], f[3][4], f[3][5], f[3][6]);
}
// Proxy mode: the last POSE_HIST poses received from each player, decoded, with their arrival times, so the proxy
// shows the pose from the same moment as its position (coop_players.c owner_smoothed: TOTK_COOP_PUPPET_DELAY_MS
// behind, default 60), blended between the two poses around it. With the newest pose put on as it came, the pose ran
// about 40 ms ahead of the position and moved in 30 Hz steps (tools/coop/top_compare.py on tour.txt, 2026-11-05).
#define POSE_HIST 8
#define POSE_HIST_BONES 160
typedef struct { uint64_t ns; int n, records; float q[POSE_HIST_BONES][4], t[POSE_HIST_BONES][3], sc[POSE_HIST_BONES][3]; } PoseRec;
static struct { PoseRec r[POSE_HIST]; unsigned count; uint64_t last_ns; } pose_hist[COOP_MAX_PLAYERS + 1];   // by net id
static PlatMutex hist_lock = PLAT_MUTEX_INIT;
static int pose_decode(const uint8_t *in, int len, PoseRec *r) {
    uint16_t n = 0, nsc = 0;
    if (len < 4) return 0;
    memcpy(&n, in, 2), memcpy(&nsc, in + 2, 2);
    int records = n >> 15;   // top bit: the skeleton's final local records (proxy_pack_mine), not the input
    n &= 0x7fff;
    // (set only for a good packet: a bad one changed the entry the blend still used; code review, 2026-11-06)
    if (n == 0 || n > POSE_HIST_BONES || 4 + n * POSE_BONE > len) return 0;
    r->records = records;
    for (int b = 0; b < n; b++) {
        int16_t v[6];
        memcpy(v, in + 4 + b * POSE_BONE, POSE_BONE);
        int big = v[0] & 3;
        float sum = 0;
        for (int k = 0, j = 0; k < 4; k++) if (k != big) { r->q[b][k] = v[j++] / 32767.0f; sum += r->q[b][k] * r->q[b][k]; }
        r->q[b][big] = sum < 1 ? sqrtf(1 - sum) : 0;
        r->t[b][0] = v[3] / 4096.0f, r->t[b][1] = v[4] / 4096.0f, r->t[b][2] = v[5] / 4096.0f;
        r->sc[b][0] = r->sc[b][1] = r->sc[b][2] = 1;
    }
    for (int k = 0; k < nsc; k++) {
        int at = 4 + n * POSE_BONE + k * 14;
        if (at + 14 > len) break;
        uint16_t bi = 0;
        memcpy(&bi, in + at, 2);
        if (bi < n) memcpy(r->sc[bi], in + at + 2, 12);
    }
    r->n = n;
    return 1;
}
static int pose_delay_ms(void) {
    static int d = -1;
    if (d < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_DELAY_MS"); d = e ? atoi(e) : 60; }
    return d;
}
// Writes player id's pose at (now - delay) into the input pose object (pose; skel 0), or straight into a body
// skeleton's local records (skel; pose 0; the packets carry records then); 0 when there is none (bone count
// differs, none yet).
static int pose_put_delayed(int id, uint64_t pose, uint64_t skel) {
    uint16_t have = 0;
    uint64_t arr = pose ? coop_rd_ptr(pose) : skel ? coop_rd_ptr(skel + 0x18) : 0;
    if (!arr || !coop_rd(pose ? pose + 0xa : skel + 0x48, &have, 2) || id < 1 || id > COOP_MAX_PLAYERS) return 0;
    static __thread PoseRec out;
    uint64_t t = plat_now_ns() - (uint64_t)pose_delay_ms() * 1000000ull;
    int ok = 0;
    plat_mutex_lock(&hist_lock);
    unsigned cnt = pose_hist[id].count, n = cnt < POSE_HIST ? cnt : POSE_HIST;
    const PoseRec *a = NULL, *b = NULL;
    if (n) {
        const PoseRec *newest = &pose_hist[id].r[(cnt - 1) % POSE_HIST];
        if (t >= newest->ns || n == 1) a = b = newest;
        else
            for (unsigned i = 1; i < n && !a; i++) {
                const PoseRec *pb = &pose_hist[id].r[(cnt - i) % POSE_HIST], *pa = &pose_hist[id].r[(cnt - i - 1) % POSE_HIST];
                if (pa->ns <= t) a = pa, b = pb;
            }
        if (!a) a = b = &pose_hist[id].r[(cnt - n) % POSE_HIST];   // older than all kept: the oldest
    }
    if (a && a->n == have && b->n == have && a->records == !pose && b->records == !pose) {
        float f = b->ns > a->ns && t > a->ns ? (float)((double)(t - a->ns) / (double)(b->ns - a->ns)) : 1;
        if (f > 1) f = 1;
        if (a == b) f = 0;
        for (int i = 0; i < have; i++) {
            float dot = 0, l = 0;
            for (int k = 0; k < 4; k++) dot += a->q[i][k] * b->q[i][k];
            float sg = dot < 0 ? -1.0f : 1.0f;
            for (int k = 0; k < 4; k++) out.q[i][k] = a->q[i][k] * (1 - f) + b->q[i][k] * sg * f, l += out.q[i][k] * out.q[i][k];
            l = sqrtf(l);
            for (int k = 0; k < 4; k++) out.q[i][k] = l > 0 ? out.q[i][k] / l : k == 3;
            for (int k = 0; k < 3; k++) out.t[i][k] = a->t[i][k] + (b->t[i][k] - a->t[i][k]) * f, out.sc[i][k] = f < 0.5f ? a->sc[i][k] : b->sc[i][k];
        }
        out.n = have, ok = 1;
    }
    plat_mutex_unlock(&hist_lock);
    if (!ok) return 0;
    for (int i = 0; i < out.n; i++) {
        float x = out.q[i][0], y = out.q[i][1], z = out.q[i][2], w = out.q[i][3];
        if (!pose) {   // a local record (0x60): scale +0x10, the rotation's columns at +0x20 / +0x30 / +0x40, translation +0x50
            float *rec = (float *)(uintptr_t)(arr + (uint64_t)i * 0x60);
            float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
                          2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
            for (int k = 0; k < 3; k++) rec[4 + k] = out.sc[i][k], rec[20 + k] = out.t[i][k];
            for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) rec[8 + c * 4 + k] = R[k * 3 + c];
            continue;
        }
        float e[16] = {out.sc[i][0], out.sc[i][1], out.sc[i][2], 0, out.t[i][0], out.t[i][1], out.t[i][2], 0,
                       1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0,
                       2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0};
        memcpy((void *)(uintptr_t)(arr + i * POSE_IN_STRIDE), e, sizeof e);
    }
    return 1;
}
// Proxy mode: a newly arrived pose from player id goes into its history.
static void pose_hist_take(int id, const uint8_t *in, int len, uint64_t age) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return;
    uint64_t at = plat_now_ns() - age;
    plat_mutex_lock(&hist_lock);
    uint64_t last = pose_hist[id].last_ns;
    if (at > last + 1000000ull || at + 1000000ull < last) {   // not the packet already kept (arrival times differ)
        PoseRec *r = &pose_hist[id].r[pose_hist[id].count % POSE_HIST];
        if (pose_decode(in, len, r)) r->ns = at, pose_hist[id].count++, pose_hist[id].last_ns = at;
    }
    plat_mutex_unlock(&hist_lock);
}
// Proxy mode: this Link's body skeleton's local records (+0x18, 0x60 a bone) as they stand at the start of its
// update phase main+0x257310 (the last frame's, after its IK and helper-bone steps), packed for sending as a POSE
// packet with the bone count's top bit set. The writer's input (what puppet mode sends) misses those steps, and a
// proxy does not run them: 30 bones differed between the two even standing still (TOTK_COOP_TEST_IKDIFF, 2026-11-05).
// The final bone matrices (skeleton +0x20, 0x40 a bone: rows 0..2 the rotation of a row-vector matrix, row 3 the
// translation) also carry steps that write them after the local records are built: feet put on stairs, a hand put
// on a wall (the owner's body top 1.55 m on the stairs and 1.90 m at the shrine wall against its proxy's 1.60 m,
// tools/coop/top_compare.py, 2026-11-05). So the records sent are made from the final matrices: each bone's
// rotation and translation relative to its parent's final matrix. The parents are found once on this Link: for each
// bone, the earlier bone whose final matrix times this bone's record gives this bone's final matrix most closely.
// Column-form here: A[r][c] = m[c * 4 + r], t = m[12..14]; a record's rotation R[r][c] = rec[8 + c * 4 + r].
static int bone_parent[POSE_HIST_BONES], parents_n;   // parents_n: bones whose parent is known (0: not yet)
void coop_pose_w_of(const float *m, float A[3][3], float t[3]) {
    for (int r = 0; r < 3; r++) { t[r] = m[12 + r]; for (int c = 0; c < 3; c++) A[r][c] = m[c * 4 + r]; }
}
static void r_of(const float *rec, float R[3][3]) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r][c] = rec[8 + c * 4 + r];
}
int coop_pose_inv3(const float a[3][3], float o[3][3]) {
    float d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
              a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(d) < 1e-12f) return 0;
    float id = 1 / d;
    o[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id, o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id, o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    o[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * id, o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id, o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
    o[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id, o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id, o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    return 1;
}
// How far bone i's final matrix is from parent p's final matrix times bone i's record (rotation and scale, translation).
float coop_pose_fk_error(const float (*w)[16], const float (*rec)[24], int i, int p) {
    float Ap[3][3], tp[3], Ai[3][3], ti[3], R[3][3];
    coop_pose_w_of(w[p], Ap, tp), coop_pose_w_of(w[i], Ai, ti), r_of(rec[i], R);
    float e = 0;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += Ap[r][k] * R[k][c];
            v *= rec[i][4 + c];
            e = fmaxf(e, fabsf(v - Ai[r][c]));
        }
        float tv = tp[r];
        for (int k = 0; k < 3; k++) tv += Ap[r][k] * rec[i][20 + k];
        e = fmaxf(e, fabsf(tv - ti[r]));
    }
    return e;
}
static void find_parents(int n, const float (*w)[16], const float (*rec)[24]) {
    int good = 0;
    float worst = 0;
    for (int i = 1; i < n; i++) {
        int best = 0;
        float be = 1e9f;
        for (int p = 0; p < i; p++) {
            float e = coop_pose_fk_error(w, rec, i, p);
            if (e < be) be = e, best = p;
        }
        bone_parent[i] = best;
        if (be < 2e-3f) good++;
        else if (be > worst) worst = be;
    }
    bone_parent[0] = -1;
    static int tries;
    // Kept once nearly all bones match (the rest are the ones later steps moved this frame); else tried again.
    if (good >= (n - 1) * 3 / 4 || ++tries > 600) parents_n = n;
    if (parents_n || tries % 120 == 1)
        rt_log("coop: proxy: bone parents %s: %d of %d bones match their parent within 2 mm (worst other %.3f)",
               parents_n ? "found" : "not yet", good, n - 1, worst);
}
void coop_proxy_pack_mine(uint64_t me) {
    uint64_t sk = me;
    for (int i = 0; i < 8 && sk; i++) sk = coop_rd_ptr(sk + coop_skel_path[i]);
    uint16_t n = 0;
    uint64_t recs = sk ? coop_rd_ptr(sk + 0x18) : 0;
    static float rec[POSE_HIST_BONES][24], w[POSE_HIST_BONES][16];
    static uint8_t out[COOP_POSE_MAX];
    uint64_t wm = sk ? coop_rd_ptr(sk + 0x20) : 0;
    if (!recs || !wm || !coop_rd(sk + 0x48, &n, 2) || n == 0 || n > POSE_HIST_BONES || 4 + n * POSE_BONE > COOP_POSE_MAX ||
        !coop_rd(recs, rec, (size_t)n * 0x60) || !coop_rd(wm, w, (size_t)n * 0x40))
        return;
    if (parents_n != n) find_parents(n, (const float (*)[16])w, (const float (*)[24])rec);
    if (parents_n == n)   // each record from the final matrices: R = Ap^-1 Ai S^-1, t = Ap^-1 (ti - tp)
        for (int i = 1; i < n; i++) {
            float Ap[3][3], tp[3], Ai[3][3], ti[3], Ip[3][3];
            coop_pose_w_of(w[bone_parent[i]], Ap, tp), coop_pose_w_of(w[i], Ai, ti);
            if (!coop_pose_inv3(Ap, Ip)) continue;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    float v = 0;
                    for (int k = 0; k < 3; k++) v += Ip[r][k] * Ai[k][c];
                    float sc = rec[i][4 + c];
                    rec[i][8 + c * 4 + r] = fabsf(sc) > 1e-6f ? v / sc : v;
                }
            }
            for (int r = 0; r < 3; r++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += Ip[r][k] * (ti[k] - tp[k]);
                rec[i][20 + r] = v;
            }
        }
    int at = 4 + n * POSE_BONE, nsc = 0;
    for (int b = 0; b < n; b++) {
        const float *f = rec[b];
        // R (rows) from the record's columns
        float r00 = f[8], r01 = f[12], r02 = f[16], r10 = f[9], r11 = f[13], r12 = f[17], r20 = f[10], r21 = f[14], r22 = f[18];
        float q[4], trc = r00 + r11 + r22;
        if (trc > 0) { float s = sqrtf(trc + 1) * 2; q[3] = s / 4, q[0] = (r21 - r12) / s, q[1] = (r02 - r20) / s, q[2] = (r10 - r01) / s; }
        else if (r00 > r11 && r00 > r22) { float s = sqrtf(1 + r00 - r11 - r22) * 2; q[3] = (r21 - r12) / s, q[0] = s / 4, q[1] = (r01 + r10) / s, q[2] = (r02 + r20) / s; }
        else if (r11 > r22) { float s = sqrtf(1 + r11 - r00 - r22) * 2; q[3] = (r02 - r20) / s, q[0] = (r01 + r10) / s, q[1] = s / 4, q[2] = (r12 + r21) / s; }
        else { float s = sqrtf(1 + r22 - r00 - r11) * 2; q[3] = (r10 - r01) / s, q[0] = (r02 + r20) / s, q[1] = (r12 + r21) / s, q[2] = s / 4; }
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        int big = 0;
        for (int k = 0; k < 4; k++) { q[k] = len > 0 ? q[k] / len : (k == 3); if (fabsf(q[k]) > fabsf(q[big])) big = k; }
        float sign = q[big] < 0 ? -1.0f : 1.0f;
        int16_t v[6];
        for (int k = 0, j = 0; k < 4; k++) if (k != big) v[j++] = coop_pose_q16(q[k] * sign, 32767.0f);
        v[0] = (int16_t)((v[0] & ~3) | big);
        v[3] = coop_pose_q16(f[20], 4096), v[4] = coop_pose_q16(f[21], 4096), v[5] = coop_pose_q16(f[22], 4096);
        memcpy(out + 4 + b * POSE_BONE, v, POSE_BONE);
        if (fabsf(f[4] - 1) > 1e-3f || fabsf(f[5] - 1) > 1e-3f || fabsf(f[6] - 1) > 1e-3f) {
            if (at + 14 > COOP_POSE_MAX) return;
            uint16_t bi = (uint16_t)b;
            memcpy(out + at, &bi, 2), memcpy(out + at + 2, f + 4, 12), at += 14, nsc++;
        }
    }
    uint16_t hdr = (uint16_t)(n | 0x8000), ns = (uint16_t)nsc;
    memcpy(out, &hdr, 2), memcpy(out + 2, &ns, 2);
    plat_mutex_lock(&pose_lock);
    memcpy(pose_latest, out, at), atomic_store(&pose_latest_len, at);
    plat_mutex_unlock(&pose_lock);
}
// A PlayerPuppet stand-in, after its model phase (coop_players.c): its owner's final skeleton records (coop_proxy_pack_mine:
// from the final matrices, so after the owner's arm and hand corrections) at the delayed moment straight into the
// stand-in's body skeleton; the caller then pushes its model, which makes the world matrices from them and puts its
// items on their bones. The clips the stand-in plays (coop_anim_apply) leave every hold a little off and a two-handed
// weapon's left hand off its grip: those corrections are the Player's own, which the stand-in has not (the tester's
// session, 2026-11-08). 0: not done (no records from that player, or another bone count).
int coop_standin_put_pose(uint64_t skel, int id) {
    if (!skel || id < 1 || id > COOP_MAX_PLAYERS) return 0;
    static __thread uint8_t in[COOP_POSE_MAX];
    uint64_t age = 0;
    int len = coop_net_pose(id, in, sizeof in, &age);
    if (!len || age >= 2000000000ull) return 0;
    pose_hist_take(id, in, len, age);
    int ok = pose_put_delayed(id, 0, skel);
    static _Atomic int said, said_no;
    if (ok && atomic_fetch_add(&said, 1) == 0) rt_log("coop: stand-in: its owner's skeleton records go onto it");
    if (!ok && atomic_fetch_add(&said_no, 1) < 3) rt_log("coop: stand-in: its owner's records not put on (%d bytes)", len);
    return ok;
}
int coop_puppet_before_writer(uint64_t pose, uint64_t skel) {
    if (skel && skel == atomic_load_explicit(&my_body_skel, memory_order_relaxed)) return 0;   // (coop_proxy_pack_mine sends this Link's records)
    int n = atomic_load_explicit(&puppet_n, memory_order_relaxed);
    for (int i = 0; skel && i < n; i++) {
        if (skel != atomic_load_explicit(&puppet_skel[i], memory_order_relaxed)) continue;
        uint64_t was = atomic_exchange(&puppet_pose[i], pose);
        static _Atomic int said_obj;
        if (was != pose && atomic_fetch_add(&said_obj, 1) < 4) rt_log("coop: puppet: input pose object %llx for skeleton %llx", (unsigned long long)pose, (unsigned long long)skel);
        static __thread uint8_t in[COOP_POSE_MAX];
        uint64_t age = 0;
        int len = coop_net_pose(atomic_load(&puppet_id[i]), in, sizeof in, &age);
        int ok = 0;
        {   // the pose from the same moment as the stand-in's position
            if (len && age < 2000000000ull) pose_hist_take(atomic_load(&puppet_id[i]), in, len, age);
            ok = len && age < 2000000000ull && pose_put_delayed(atomic_load(&puppet_id[i]), pose, 0);
        }
        if (ok) pose_log("on", pose);
        static _Atomic int said_u, said_f;
        uint16_t have = 0, n2 = 0;
        coop_rd(pose + 0xa, &have, 2);
        if (len >= 2) memcpy(&n2, in, 2);
        if (ok && atomic_fetch_add(&said_u, 1) == 0) rt_log("coop: puppet: pose in (%d bytes, %u bones, %.0f ms old)", len, n2, age / 1e6);
        if (!ok && atomic_fetch_add(&said_f, 1) < 3) rt_log("coop: puppet: pose not put on (%d bytes, %u bones in it, %u here, %.0f ms old)", len, n2, have, age / 1e6);
        return 1;
    }
    return 0;
}
