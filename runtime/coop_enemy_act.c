// Co-op: a copy runs the action its owner is in (docs/coop/enemy-sync-spec.md B.38; research R22). A PROTOTYPE, off
// unless TOTK_COOP_EN_ACT=1, and only for the action verbs of TOTK_COOP_EN_ACT_VERBS (default: 131 JumpAttack and 145
// OrderAttack, a Chuchu's attack; 108 Appear, a Yiga's).
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
// Not in the prototype: the other slots; a net for the nodes a copy must never run (a death request, a delete, a
// drop, a shot, a message: none is in the listed verbs' roots but for a Chuchu's shockwave); an interrupt request;
// the form key (the default one is assumed). While a copy runs an action this file's caller leaves out the arm
// replay of B.35 and the held effects of B.23 for it, which the action's own nodes now do.
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

typedef struct { Enid id; uint8_t mode, stop, pad[2]; uint8_t req[ACT_REQ]; char var[32]; } MAct;
static struct {
    _Atomic int cell;   // cell + 1; 0 free, -1 being filled
    uint8_t req[ACT_REQ];
    char var[32];
    uint32_t verb, frame;
    uint8_t enter, stop, said;
    uint32_t sum_runs, sum_ticks, sum_calls, sum_frame;   // (the prototype's log: a second's worth)
} actq[ACTQ];
__thread uint64_t tl_act_actor;   // the copy whose action request this file is making (read by en_copy_hook)
__thread uint32_t tl_act_ticks, tl_act_runs, tl_act_other;   // under one call: node ticks, the slot's graph run, other runner entries turned away
__thread uint64_t tl_act_lr;      // the last of those others: its entry << 32 | where it was called from
static _Atomic int n_told, n_made;

int en_act_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_ACT"); on = e && *e == '1'; }
    return on;
}
static int verb_listed(uint32_t v) {
    static uint32_t list[16];
    static int n = -1;
    if (n < 0) {
        const char *e = cfg_get("TOTK_COOP_EN_ACT_VERBS");
        int k = 0;
        for (const char *p = e && *e ? e : "131,145,108"; p && *p && k < 16; p = strchr(p, ','), p = p ? p + 1 : NULL) list[k++] = (uint32_t)strtoul(p, NULL, 0);
        n = k;
    }
    for (int i = 0; i < n; i++) if (list[i] == v) return 1;
    return 0;
}
static uint32_t verb_of(const uint8_t *req) {
    uint32_t f, v;
    memcpy(&f, req + 0x28, 4), memcpy(&v, req + 0x4c, 4);
    return f != VERB_NONE ? f : v;
}

// The request's entry in the game that runs the enemy (x0 the BSA, x1 the request, w2 the mode), from en_copy_hook.
static __thread uint32_t tl_act_verb;
void en_act_tick_seen(uint64_t node);
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
    int listed = verb_listed(v) && !(flags & 1);
    if (!listed && !m->act_verb) return;
    memcpy(a.req, req, ACT_REQ);
    memset(a.req + 0x38, 0, 8);
    a.mode = (uint8_t)(c->x[2] & 1), a.stop = !listed;
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
        uint32_t v = verb_of(a->req);
        if (have < 0) actq[k].verb = 0, actq[k].enter = 0, actq[k].stop = 0, actq[k].said = 0;
        if (a->stop) actq[k].stop = 1;
        else {
            if (a->mode || v != actq[k].verb) actq[k].enter = 1;
            memcpy(actq[k].req, a->req, ACT_REQ), memcpy(actq[k].var, a->var, sizeof a->var);
            actq[k].verb = v, actq[k].stop = 0;
        }
        actq[k].frame = coop_sess_frame_no();
        if (have < 0) atomic_store(&actq[k].cell, cell);
    }
    return 1;
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
    if (frame - actq[k].sum_frame >= 60) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 60) rt_log("coop: en: act: the copy %llx in verb %u: %u calls, its slot's graph run %u times, %u node ticks in the last second", (unsigned long long)actor, actq[k].verb, actq[k].sum_calls, actq[k].sum_runs, actq[k].sum_ticks);
        actq[k].sum_runs = actq[k].sum_ticks = actq[k].sum_calls = 0, actq[k].sum_frame = frame;
    }
    if (stop) { atomic_store(&actq[k].cell, 0); return 0; }
    return 1;
}

// The prototype's log: each node class a copy's action ticks, once, by its vtable (the node table of the research
// names it), with the verb the copy was in.
static _Atomic uint64_t seen_vt[96];
void en_act_tick_seen(uint64_t node) {
    uint64_t vt = node > 0x10000 ? coop_rd_ptr(node) : 0;
    if (!vt) return;
    for (int i = 0; i < 96; i++) {
        uint64_t have = atomic_load_explicit(&seen_vt[i], memory_order_relaxed);
        if (have == vt) return;
        if (!have) {
            if (!atomic_compare_exchange_strong(&seen_vt[i], &have, vt)) { if (have == vt) return; continue; }
            uint16_t idx = 0;
            coop_rd(node + 0x1c, &idx, 2);
            rt_log("coop: en: act: a copy in verb %u ticks a node of class vtable main+%#llx (node index %u)", tl_act_verb, (unsigned long long)(vt - MOD_MAIN_BASE), idx);
            return;
        }
    }
}
