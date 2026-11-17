// Co-op: a copy runs the action its owner is in (docs/coop/enemy-sync.md B.38, B.41; research R22). Off unless
// TOTK_COOP_EN_ACT=1. Which actions: TOTK_COOP_EN_ACT_VERBS, a list of action verbs or `all` (default: 131 JumpAttack
// and 145 OrderAttack, a Chuchu's attack; 108 Appear, a Yiga's); for which enemies: TOTK_COOP_EN_ACT_FAMILY, a list of
// actor name prefixes (default: every enemy).
//
// Why. A copy has no AI, and most of what a player sees an enemy do is done by nodes of its AI graphs, not by its
// animation: effects, attacks armed by a node, lifting, commands on what it holds (research R19 counted about 9,700
// such node instances a copy does not do). Making each again by hand does not end.
//
// How the game is layered (R22). An enemy's deciding units (brain, utility, the scheduler with its ActionSeq) choose
// an action and ask for it with one call, main+0x50ad74(BSA, request, mode). The request (0x60 bytes: three vectors,
// a forced verb +0x28 or the action verb +0x4c, a variation string +0x38 with its valid byte +0x40, the slot +0x48,
// flags +0x50, the asking node +0x58) names a root of the slot's action graph by form key, verb and variation; the
// call stores it in the slot and ticks the slot's graph (its tail's call of main+0x50b404), which enters the root on
// a change and runs its nodes. That tail is the only place a slot's graph is ticked from.
//
// So the game that runs an enemy tells the request of its Main slot each frame it is one of the listed verbs, and the
// copy's game makes the same call on the copy before its calc, with a flag up that lets this one call tree through
// the copy switch: main+0x50ad74 itself, main+0x50b404 only from that tail, and every node tick under it (a tick let
// through by halves is the shape of the crash of B.37). Everything that decides stays turned away. The copy's place,
// clock and target are its owner's as before. When the owner's verb is no longer a listed one the copy is asked for
// Wait once, so the root it was in is left and what it held (a Yiga's `Sign`) is stopped by the node's own leave.
//
// What a copy's action does not do itself (B.41). (1) The acts the sync already carries from the owner, or that
// belong to one game only: a shot, a drop, a death, a made or deleted actor, a lift or a ride, game data, a message
// to another actor, a chemical switch, an attack turn asked of the game balance. The node classes that end in those
// are known from the audit of the AI graphs (coop_enemy_act_deny.inc); the tick of such a node is turned away and
// the node left as one that succeeded. (2) Its animation: the copy's slots play what its owner states (S 7.3), and a
// play asked by a node of the action is answered "played" without being made (TOTK_COOP_EN_ACT_PLAYS=1 lets them
// through). (3) Time: a request is made when this game shows its owner at the request's time, as a shot is.
// What its nodes do do: effects, an attack armed by a node, the model's looks, physics on its own body. While a copy
// runs an action the arm replay of B.35 is left out for it, and an effect its own node started is not started again
// by the replay of B.17 (coop_enemy_fx.c).
//
// Not built: the other slots; an interrupt request; the form key (the default one is assumed).
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);   // coop_spawn.c

#define FN_ACT 0x50ad74
#define ACT_REQ 0x58   // the request without the asking node's pointer
#define VERB_NONE 0x11c
#define VERB_WAIT 40
#define ACTQ 32

#define ACT_IN 16
typedef struct { Enid id; int64_t ns; uint8_t mode, stop, pad[6]; uint8_t req[ACT_REQ]; char var[32]; } MAct;
typedef struct { int64_t ns; uint64_t taken; uint8_t mode, stop; uint8_t req[ACT_REQ]; char var[32]; } ActIn;
static struct {
    _Atomic int cell;   // cell + 1; 0 free, -1 being filled
    ActIn in[ACT_IN];   // told, not yet due (written by the receiving thread at in_w, taken by the copy's at in_r)
    _Atomic uint32_t in_w, in_r;
    uint8_t req[ACT_REQ];
    char var[32];
    uint32_t verb, frame;
    uint8_t enter, stop, said, have;
    uint32_t sum_runs, sum_ticks, sum_calls, sum_frame;   // (the prototype's log: a second's worth)
} actq[ACTQ];
__thread uint64_t tl_act_actor;   // the copy whose action request this file is making (read by en_copy_hook)
__thread uint32_t tl_act_ticks, tl_act_runs, tl_act_other;   // under one call: node ticks, the slot's graph run, other runner entries turned away
__thread uint64_t tl_act_lr;      // the last of those others: its entry << 32 | where it was called from
__thread uint32_t tl_act_plays;   // plays asked by the action's nodes and not made (counted over the copy's life of the thread)
static _Atomic int n_told, n_made;

int en_act_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_ACT"); on = e && *e == '1'; }
    return on;
}
static int verb_listed(uint32_t v) {
    static uint32_t list[64];
    static int n = -1, all;
    if (n < 0) {
        const char *e = cfg_get("TOTK_COOP_EN_ACT_VERBS");
        int k = 0;
        all = e && !strcmp(e, "all");
        for (const char *p = e && *e ? e : "131,145,108"; !all && p && *p && k < 64; p = strchr(p, ','), p = p ? p + 1 : NULL) list[k++] = (uint32_t)strtoul(p, NULL, 0);
        n = k;
    }
    if (all) return v < VERB_NONE;
    for (int i = 0; i < n; i++) if (list[i] == v) return 1;
    return 0;
}
// Whether an enemy of this actor name is one whose copies run its action (a list of name prefixes; none: every enemy).
static int family_listed(const char *name) {
    static char buf[256];
    static const char *pre[16];
    static int n = -1;
    if (n < 0) {
        const char *e = cfg_get("TOTK_COOP_EN_ACT_FAMILY");
        int k = 0;
        if (e && *e) {
            strncpy(buf, e, sizeof buf - 1);
            for (char *q = buf; q && *q && k < 16;) { pre[k++] = q; q = strchr(q, ','); if (q) *q++ = 0; }
        }
        n = k;
    }
    if (!n) return 1;
    for (int i = 0; i < n; i++) if (!strncmp(name, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
// Whether a play asked by a node of a copy's action is made (default: no, the copy's slots are its owner's).
int en_act_plays(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_ACT_PLAYS"); on = e && *e == '1'; }
    return on;
}
// Whether this copy (cell + 1) has an action told for it (coop_enemy_fx.c: an effect may then be its own node's).
int en_act_has(int cell) {
    for (int i = 0; i < ACTQ; i++) if (atomic_load_explicit(&actq[i].cell, memory_order_relaxed) == cell) return 1;
    return 0;
}
static uint32_t verb_of(const uint8_t *req) {
    uint32_t f, v;
    memcpy(&f, req + 0x28, 4), memcpy(&v, req + 0x4c, 4);
    return f != VERB_NONE ? f : v;
}

// The request's entry in the game that runs the enemy (x0 the BSA, x1 the request, w2 the mode), from en_copy_hook.
static __thread uint32_t tl_act_verb;
int en_act_tick_seen(uint64_t node);
void en_act_owner(Cpu *c) {
    if (!en_act_on()) return;
    uint64_t actor = coop_rd_ptr(c->x[0] + 0x18);
    EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
    if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return;
    MAct a;
    uint8_t req[0x60];
    uint32_t slot = 0;
    uint64_t flags = 0;
    memset(&a, 0, sizeof a);
    if (!coop_rd(c->x[1], req, sizeof req)) return;
    memcpy(&slot, req + 0x48, 4), memcpy(&flags, req + 0x50, 8);
    if (slot) return;   // (the Main slot only)
    uint32_t v = verb_of(req);
    char an[48];
    int listed = verb_listed(v) && !(flags & 1) && family_listed(coop_actor_name(actor, an, sizeof an));
    if (!listed && !m->act_verb) return;
    memcpy(a.req, req, ACT_REQ);
    // What the request does not use holds leftovers of the asking game's stack, pieces of its addresses among them
    // (the net of B.40 named them in the first run): the rest of a vector whose first number says "none" (NaN), the
    // bytes after the valid byte, the bytes after the 16 flag bits (stored with a half-word store, main+0x2bbb0).
    for (int k = 0; k < 0x24; k += 0xc) { uint32_t x; memcpy(&x, a.req + k, 4); if ((x & 0x7fffffffu) > 0x7f800000u) memset(a.req + k + 4, 0, 8); }
    memset(a.req + 0x41, 0, 7), memset(a.req + 0x52, 0, 6);
    { void en_foreign_scan(const char *what, const uint8_t *p, int n, int skip); en_foreign_scan("an action request", a.req, ACT_REQ, 0x38); }   // coop_enemy_shot.c
    memset(a.req + 0x38, 0, 8);
    a.mode = (uint8_t)(c->x[2] & 1), a.stop = !listed, a.ns = (int64_t)coop_sess_frame_ns();
    if (listed && req[0x40] == 1) {
        uint64_t vp;
        memcpy(&vp, req + 0x38, 8);
        if (!vp || !coop_rd(vp, a.var, sizeof a.var - 1)) a.var[0] = 0;
        for (char *q = a.var; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
    }
    if (listed && v != m->act_verb) a.mode = 1;
    coop_lock_wait(&en_lock);
    int16_t ei = atomic_load(&m->en);
    if (ei >= 0 && en_tab[ei].used) {
        a.id = en_tab[ei].id;
        en_send_all(ENM_ACT, &a, sizeof a);
        if (v != m->act_verb || a.stop) {
            uint64_t sl = coop_rd_ptr(c->x[0] + 0x1f0), ct = sl ? coop_rd_ptr(sl + 0x70) : 0, ipo = ct ? coop_rd_ptr(ct + 0x20) : 0, ino = ipo ? coop_rd_ptr(ipo) : 0;
            int32_t rt = -1;
            uint32_t f7c = 0, qn = 0;
            if (ct) coop_rd(ct + 0x19c, &rt, 4), coop_rd(sl + 0x7c, &f7c, 4);
            if (ino) coop_rd(ino + 0x90, &qn, 4);
            if (atomic_fetch_add(&n_told, 1) < 60) rt_log("coop: en: act: %s %016llx (run here) %s verb %u%s%s, mode %u: told to its copies (its slot before the call: root %d, +0x7c %x, queue %u; asked from main+%#llx)", en_tab[ei].name, (unsigned long long)en_tab[ei].id.a, a.stop ? "leaves its action for" : "asks its Main slot for", v, a.var[0] ? " variation " : "", a.var, a.mode, rt, f7c, qn, (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
        }
    }
    plat_mutex_unlock(&en_lock);
    m->act_verb = listed ? v : 0;
}

// A message of this file's (from en_copy_msg). 1: it was one.
int en_act_msg(const CoopCtrMsg *m) {
    if (m->type != ENM_ACT) return 0;
    const MAct *a = (const MAct *)m->d;
    uint64_t me = coop_ctr_me();
    if (m->len != (int)sizeof *a || !memchr(a->var, 0, sizeof a->var)) return 1;
    for (int i = 0; i < EN_N; i++) {
        if (!en_tab[i].used || !enid_eq(en_tab[i].id, a->id) || en_tab[i].owner == me || en_tab[i].cell < 0) continue;
        int cell = en_tab[i].cell + 1, f = -1, have = -1;
        for (int k = 0; k < ACTQ; k++) {
            int cc = atomic_load(&actq[k].cell);
            if (cc == cell) have = k;
            else if (!cc && f < 0) f = k;
        }
        if (have < 0 && (a->stop || f < 0)) return 1;
        int k = have >= 0 ? have : f, z = 0;
        if (have < 0 && !atomic_compare_exchange_strong(&actq[k].cell, &z, -1)) return 1;
        if (have < 0) actq[k].verb = 0, actq[k].enter = 0, actq[k].stop = 0, actq[k].said = 0, actq[k].have = 0, atomic_store(&actq[k].in_w, 0), atomic_store(&actq[k].in_r, 0);
        uint32_t w = atomic_load(&actq[k].in_w);
        if (w - atomic_load(&actq[k].in_r) >= ACT_IN) w--;   // (full: the newest told takes the last place)
        ActIn *in = &actq[k].in[w % ACT_IN];
        in->ns = a->ns, in->taken = en_now(), in->mode = a->mode, in->stop = a->stop;
        memcpy(in->req, a->req, ACT_REQ), memcpy(in->var, a->var, sizeof a->var);
        atomic_store(&actq[k].in_w, w + 1);
        actq[k].frame = coop_sess_frame_no();
        if (have < 0) atomic_store(&actq[k].cell, cell);
    }
    return 1;
}

// The character controller's move state and velocity. An action's own waits read them (QueryPhysicsCctIsPreLand: true
// when the move state is the ground's, type 0; in the air only when falling with ground close below; IsOnGround,
// IsFalling and the rest of that family likewise), and a copy's controller never changes them by itself, its body
// being static and its place written. The controller is [[comps+0x50]+0x20]+0x10; K = [controller+8]; the state
// machine [K+0x40] keeps three state objects from +0x38 (each its type at +8) and the current one's index at +0x50;
// the velocity is three floats at [K+0x18]+0xf8 (the getter main+0x298ce1c). 1: read (the index with 0x80).
int en_act_cct_read(uint64_t actor, uint8_t *idx, float vel[3]) {
    uint64_t ph = coop_rd_ptr(en_comps(actor) + 0x50), cc = ph ? coop_rd_ptr(ph + 0x20) : 0, cct = cc ? coop_rd_ptr(cc + 0x10) : 0;
    uint64_t k = cct ? coop_rd_ptr(cct + 8) : 0, sm = k ? coop_rd_ptr(k + 0x40) : 0, vb = k ? coop_rd_ptr(k + 0x18) : 0;
    uint32_t i = 0;
    if (!sm || !vb || !coop_rd(sm + 0x50, &i, 4) || i > 2 || !coop_rd(vb + 0xf8, vel, 12)) return 0;
    *idx = (uint8_t)(0x80 | i);
    return 1;
}
// The copy's controller given its owner's move state and velocity, before its action is asked for.
static void cct_write(uint64_t actor, EnCell *m) {
    if (!(m->show_cct & 0x80)) return;
    uint64_t ph = coop_rd_ptr(en_comps(actor) + 0x50), cc = ph ? coop_rd_ptr(ph + 0x20) : 0, cct = cc ? coop_rd_ptr(cc + 0x10) : 0;
    uint64_t k = cct ? coop_rd_ptr(cct + 8) : 0, sm = k ? coop_rd_ptr(k + 0x40) : 0, vb = k ? coop_rd_ptr(k + 0x18) : 0;
    uint32_t own = 0, want = m->show_cct & 3;
    float ov[3] = {0};
    if (!sm || !vb || !coop_rd(sm + 0x50, &own, 4) || !coop_rd(vb + 0xf8, ov, 12) || want > 2) return;
    if (own != want) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 40) rt_log("coop: en: act: the copy %llx's controller is in move state %u (velocity %.2f %.2f %.2f), its owner's in %u (%.2f %.2f %.2f): given its owner's", (unsigned long long)actor, own, ov[0], ov[1], ov[2], want, m->show_cvel[0], m->show_cvel[1], m->show_cvel[2]);
        memcpy((void *)(uintptr_t)(sm + 0x50), &want, 4);
    }
    memcpy((void *)(uintptr_t)(vb + 0xf8), m->show_cvel, 12);
}

// The copy's pre-calc: its owner's action asked of its own Main slot. 1 while the copy runs an action (the caller
// then leaves the arm replay and the held effects to the action's own nodes).
int en_act_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    static __thread _Alignas(16) uint8_t req[0x60];
    static __thread _Alignas(16) char var[32];
    if (!en_act_on()) return 0;
    int cell = (int)(m - en_map) + 1, k = -1;
    for (int i = 0; i < ACTQ && k < 0; i++) if (atomic_load_explicit(&actq[i].cell, memory_order_relaxed) == cell) k = i;
    if (k < 0) return 0;
    uint64_t bsa = en_bsa(actor), slot = bsa ? coop_rd_ptr(bsa + 0x1f0) : 0, ctrl = slot ? coop_rd_ptr(slot + 0x70) : 0, ip = ctrl ? coop_rd_ptr(ctrl + 0x20) : 0;
    uint32_t frame = coop_sess_frame_no();
    {   // What was told and is due: this game shows the owner at the request's time (or it waited a second).
        int64_t en_shot_T_of(int id);   // coop_enemy_shot.c
        int16_t ei = atomic_load(&m->en);
        int id = ei >= 0 && en_tab[ei].used ? en_peer_id(en_tab[ei].owner) : 0;
        int64_t T = en_shot_T_of(id);
        uint64_t now = en_now();
        for (uint32_t r = atomic_load(&actq[k].in_r); r != atomic_load(&actq[k].in_w); atomic_store(&actq[k].in_r, ++r)) {
            const ActIn *in = &actq[k].in[r % ACT_IN];
            if (T && T < in->ns && now - in->taken < 1000000000ull) break;
            uint32_t v = verb_of(in->req);
            if (in->stop) actq[k].stop = 1;
            else {
                if (in->mode || v != actq[k].verb) actq[k].enter = 1;
                memcpy(actq[k].req, in->req, ACT_REQ), memcpy(actq[k].var, in->var, sizeof in->var);
                actq[k].verb = v, actq[k].stop = 0, actq[k].have = 1;
            }
        }
    }
    if (!actq[k].have && !actq[k].stop && frame - actq[k].frame <= 30) return 0;   // (told, none due yet)
    int stop = actq[k].stop || frame - actq[k].frame > 30;   // (told to leave, or not told for half a second: its owner's calls stopped)
    // Only once the copy has left its warm-up and its Main slot has its controller and its instance (R22 Q6).
    if (!atomic_load_explicit(&m->warm, memory_order_relaxed) || !ctrl || !ip || !coop_rd_ptr(ip)) {
        if (stop) atomic_store(&actq[k].cell, 0);
        return 0;
    }
    memset(req, 0, sizeof req);
    uint64_t mode;
    if (stop) {   // Wait, once, with the force: the root the copy was in is left
        uint32_t nan = 0x7fc00000u, two = 2, none = VERB_NONE, d = 0x12, w = VERB_WAIT;
        memcpy(req, &nan, 4), memcpy(req + 0xc, &nan, 4), memcpy(req + 0x18, &nan, 4), memcpy(req + 0x24, &two, 4), memcpy(req + 0x28, &none, 4);
        memcpy(req + 0x2c, &nan, 4), memcpy(req + 0x30, &nan, 4), memcpy(req + 0x34, &d, 4), memcpy(req + 0x4c, &w, 4);
        mode = 1;
    } else {
        memcpy(req, actq[k].req, ACT_REQ);
        if (actq[k].var[0]) {
            uint64_t vp = (uint64_t)(uintptr_t)var;
            memcpy(var, actq[k].var, sizeof var), memcpy(req + 0x38, &vp, 8), req[0x40] = 1;
        } else memset(req + 0x38, 0, 8), req[0x40] = 0;
        mode = actq[k].enter;
        actq[k].enter = 0;
    }
    int32_t root0 = -1, root1 = -1;
    uint32_t s7c0 = 0, s7c1 = 0, s780 = 0, s781 = 0, q0 = 0, q1 = 0;
    uint64_t inst = coop_rd_ptr(ip);
    coop_rd(ctrl + 0x19c, &root0, 4), coop_rd(slot + 0x7c, &s7c0, 4), coop_rd(slot + 0x78, &s780, 4), coop_rd(inst + 0x90, &q0, 4);
    tl_act_ticks = tl_act_runs = tl_act_other = 0, tl_act_lr = 0;
    cct_write(actor, m);
    tl_act_verb = stop ? VERB_WAIT : actq[k].verb;
    tl_act_actor = actor;
    uint64_t ret = coop_guest_call(c, MOD_MAIN_BASE + FN_ACT, bsa, (uint64_t)(uintptr_t)req, mode, 0);
    tl_act_actor = 0;
    coop_rd(ctrl + 0x19c, &root1, 4), coop_rd(slot + 0x7c, &s7c1, 4), coop_rd(slot + 0x78, &s781, 4), coop_rd(inst + 0x90, &q1, 4);
    if (mode || !actq[k].said || (uint32_t)ret) {
        if (atomic_fetch_add(&n_made, 1) < 80) rt_log("coop: en: act: the copy %llx is asked for %s verb %u, mode %u: the call answered %u, its Main slot's root %d -> %d; slot +0x7c %x -> %x, +0x78 %u -> %u, queue %u -> %u; under the call: the slot's graph run %u times, %u node ticks, %u other entries turned away (last main+%#llx from main+%#llx)", (unsigned long long)actor, stop ? "Wait, leaving" : "its owner's", stop ? VERB_WAIT : actq[k].verb, (unsigned)mode, (unsigned)ret, root0, root1, s7c0, s7c1, s780, s781, q0, q1, tl_act_runs, tl_act_ticks, tl_act_other, (unsigned long long)(tl_act_lr >> 32), (unsigned long long)(tl_act_lr & 0xffffffffu));
        actq[k].said = 1;
    }
    actq[k].sum_runs += tl_act_runs, actq[k].sum_ticks += tl_act_ticks, actq[k].sum_calls++;
    {   // (the prototype's log: the SensorHit component's armed bits right after the action ran, when any is set)
        uint64_t ac = coop_rd_ptr(en_comps(actor) + 0x428);
        uint8_t b[8] = {0};
        static _Atomic int said;
        if (ac && coop_rd(ac + 0x20, b, 8) && (b[0] || b[1]) && atomic_fetch_add(&said, 1) % 30 == 0) {
            float p[3] = {0}, lp[3] = {0};
            uint64_t link = coop_player_actor();
            coop_rd(actor + COOP_ACTOR_POS, p, 12);
            if (link) coop_rd(link + COOP_ACTOR_POS, lp, 12);
            rt_log("coop: en: act: the copy %llx after its action: armed now %02x, last frame %02x, flags %02x, layer %02x, +0x24 %02x %02x, tg-off %02x%02x; it is at %.2f %.2f %.2f, this Link at %.2f %.2f %.2f", (unsigned long long)actor, b[0], b[1], b[2], b[3], b[4], b[5], b[7], b[6], p[0], p[1], p[2], lp[0], lp[1], lp[2]);
        }
    }
    if (frame - actq[k].sum_frame >= 60) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 60) rt_log("coop: en: act: the copy %llx in verb %u: %u calls, its slot's graph run %u times, %u node ticks in the last second", (unsigned long long)actor, actq[k].verb, actq[k].sum_calls, actq[k].sum_runs, actq[k].sum_ticks);
        actq[k].sum_runs = actq[k].sum_ticks = actq[k].sum_calls = 0, actq[k].sum_frame = frame;
    }
    if (stop) { atomic_store(&actq[k].cell, 0); return 0; }
    return 1;
}

// A node of a copy's action that the copy does not run itself (the category, 0: it runs). The table is the audit's
// (research R19): every node class of the enemies' graphs that acts outward, read to where it ends, and listed here
// when that is an act the sync carries from the owner or one that belongs to one game only. The categories: 1 a shot
// or a Shooter's state, 2 a drop, a throw, a pick-up, 3 death and life, 4 an actor made, deleted or put to sleep, 5 a
// grab, a lift, a ride, a sweep-out, a knock-back, 6 game data and events, 7 a message or a noise for other actors,
// 8 a chemical switch, 9 the game balance's attack turns, 10 what the sync holds another way (weather, form,
// territory, a teleport, a jump of its own). The node's tick main+0x1920 takes the node and returns nothing; a leaf
// turned away with its result set to success leaves its parent a finished child, which is not the half-run tick of
// B.37 (a parent ticked with a child never made). All of these classes are leaves (Execute and OneShot nodes).
// TOTK_COOP_EN_ACT_ALLOW=<category>[,...] lets categories run (a trial); TOTK_COOP_EN_ACT_DENY=<vtable offset>[,...]
// adds classes (category 11).
static const struct { uint32_t vt; uint8_t cat; } deny_tab[] = {
#include "coop_enemy_act_deny.inc"
};
static int deny_cat(uint64_t vt) {
    static uint64_t extra[32];
    static int n = -1;
    static uint32_t allow;
    if (n < 0) {
        const char *e = cfg_get("TOTK_COOP_EN_ACT_DENY"), *al = cfg_get("TOTK_COOP_EN_ACT_ALLOW");
        int k = 0;
        for (const char *q = e; q && *q && k < 32; q = strchr(q, ','), q = q ? q + 1 : NULL) {
            uint64_t v = strtoull(q, NULL, 0);
            if (v) extra[k++] = MOD_MAIN_BASE + v;
        }
        for (const char *q = al; q && *q; q = strchr(q, ','), q = q ? q + 1 : NULL) { unsigned long v = strtoul(q, NULL, 0); if (v < 32) allow |= 1u << v; }
        n = k;
    }
    for (int i = 0; i < n; i++) if (extra[i] == vt) return 11;
    if (vt < MOD_MAIN_BASE) return 0;
    uint64_t off = vt - MOD_MAIN_BASE;
    int lo = 0, hi = (int)(sizeof deny_tab / sizeof deny_tab[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (deny_tab[mid].vt == off) return allow >> deny_tab[mid].cat & 1 ? 0 : deny_tab[mid].cat;
        if (deny_tab[mid].vt < off) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

// The prototype's log: each node class a copy's action ticks, once, by its vtable (the node table of the research
// names it), with the verb the copy was in. Returns whether the node's tick is turned away.
static _Atomic uint64_t seen_vt[512];
int en_act_tick_seen(uint64_t node) {
    uint64_t vt = node > 0x10000 ? coop_rd_ptr(node) : 0;
    if (!vt) return 0;
    int deny = deny_cat(vt), isnew = 0;
    if (deny) {
        uint32_t fl = 0;
        if (coop_rd(node + 0x18, &fl, 4)) { fl = (fl & 0x3fffffffu) | 0x80000000u; memcpy((void *)(uintptr_t)(node + 0x18), &fl, 4); }
    }
    for (int i = 0; i < 512; i++) {
        uint64_t have = atomic_load_explicit(&seen_vt[i], memory_order_relaxed);
        if (have == vt) break;
        if (!have) {
            if (!atomic_compare_exchange_strong(&seen_vt[i], &have, vt)) { if (have == vt) break; continue; }
            isnew = 1;
            break;
        }
    }
    if (isnew) {
        uint16_t idx = 0;
        coop_rd(node + 0x1c, &idx, 2);
        rt_log("coop: en: act: a copy in verb %u ticks a node of class vtable main+%#llx (node index %u)%s", tl_act_verb, (unsigned long long)(vt - MOD_MAIN_BASE), idx, deny ? ": turned away, left as succeeded" : "");
        if (deny) rt_log("coop: en: act: (denied category %d)", deny);
    }
    return deny != 0;
}
