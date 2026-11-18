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
// So the game that runs an enemy tells the requests of its action slots (PLAN.md step 3; research R29), and the copy's
// game makes the same calls on the copy before its calc, with a flag up that lets this one call tree through the copy
// switch: main+0x50ad74 itself, main+0x50b404 only from that tail, and every node tick under it (a tick let through
// by halves is the shape of the crash of B.37). Everything that decides stays turned away. The copy's place, clock and
// target are its owner's as before.
//
// The five slots (Main, Sub, SP, SP2, SP3) are each their own: the call takes the slot from the request (+0x48; an
// index of 5 or more lands on Main), and each slot has its own object, controller, stored request and "took a request
// this frame" bit. Only Main runs the move-control adjustment and the interrupt teardown. So each slot is told and
// kept apart, per copy: a queue per (copy, slot), called Main first, then the others, as the owner's update runs its
// scheduler (Main) before its utility (Sub). A Bokoblin uses Main for every action and Sub for the lower body while it
// rides (Ride at a state's enter, None at its leave). When the owner's request is no longer a listed one, the copy's
// Main is asked for Wait once, so the root it was in is left and what it held (a Yiga's `Sign`) is stopped by the
// node's own leave; a slot 1 to 4 is asked for None, as the game's own fill asks it. For slots 1 to 4 None is a real
// request (the leave of a riding state): it is told as that slot's stop, once after a told request, since the
// scheduler's fill asks None of every empty slot 1 to 4 again and again and the copy's stop is that same request.
//
// Interrupt requests (flags bit 0) tear down the Main action sequence and set the status byte BSA+0x26a8 to 1, which
// a copy might never clear. So a copy runs an interrupt as a forced plain request of the same verb (bit 0 cleared, the
// force bit 8 kept): it enters the same root without the teardown. Only the light reactions are told this way; the
// heavy ones (Die, Struggle, Thrown, HugeHit, CriticalHit, WeakPointHit, Smash, Frozen, BrokenArmor) switch the body
// to rigid-body mode and push it, the launch of research R28, and stay a stop.
//
// The form key (BSA+0x2628, a pointer to text: `Sleep`, `HorseRide`, `RideSeat` for a Bokoblin; the default builds no
// prefix) decides which root a verb finds (`Sleep_InterruptSmallHit`). The owner's text travels with each request
// and is set on the copy before the requests of that frame, with the game's own setter.
//
// What a copy's action does not do itself (B.41). (1) The acts the sync already carries from the owner, or that
// belong to one game only: a shot, a drop, a death, a made or deleted actor, a lift or a ride, game data, a message
// to another actor, a chemical switch, an attack turn asked of the game balance. The node classes that end in those
// are known from the audit of the AI graphs (coop_enemy_act_deny.inc); the tick of such a node is turned away and
// the node left as one that succeeded. Whatever node makes it, a call into the engine functions those acts end in is
// refused too (en_act_refuse, at the end of this file). (2) Its animation: the copy's slots play what its owner states (S 7.3), and a
// play asked by a node of the action is answered "played" without being made (TOTK_COOP_EN_ACT_PLAYS=1 lets them
// through). (3) Time: a request is made when this game shows its owner at the request's time, as a shot is.
// What its nodes do do: effects, an attack armed by a node, the model's looks, physics on its own body. While a copy
// runs an action the arm replay of B.35 is left out for it, and an effect its own node started is not started again
// by the replay of B.17 (coop_enemy_fx.c).
//
#include <math.h>
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
#define FN_FORM_SET 0x88d524     // (scheduler, &key, force): the form key set, the brain verbs' containers picked again
#define FN_FORM_ATTRS 0xd141fc   // (BSA, &key): that form's action-verb and brain-verb attribute sets
#define FN_FORM_DEFAULT 0x88d794 // (BSA): the default attribute sets
#define FORM_DEFAULT_KEY 0x395db80   // a pointer to the cell that holds `cReservedDefaultFormChangeKey` (OneShotBSAResetForm's argument)
#define ACT_REQ 0x58   // the request without the asking node's pointer
#define VERB_NONE 0x11c
#define VERB_WAIT 40
#define ACT_SLOTS 5
#define ACTQ 64

#define ACT_IN 16
typedef struct { Enid id; int64_t ns; uint8_t mode, stop, slot, intr, pad[4]; uint8_t req[ACT_REQ]; char var[32]; char form[32]; } MAct;
typedef struct { int64_t ns; uint64_t taken; uint8_t mode, stop; uint8_t req[ACT_REQ]; char var[32]; char form[32]; } ActIn;
static struct {
    _Atomic int cell;   // cell + 1; 0 free, -1 being filled
    uint8_t slot;       // the action slot this queue is for (0 Main ... 4 SP3)
    ActIn in[ACT_IN];   // told, not yet due (written by the receiving thread at in_w, taken by the copy's at in_r)
    _Atomic uint32_t in_w, in_r;
    uint8_t req[ACT_REQ];
    char var[32];
    char form[32]; int64_t form_ns;   // the owner's form key text with the last request taken, and that request's time
    uint32_t verb, frame;
    uint8_t enter, force, stop, said, have, form_have;
    uint32_t sum_runs, sum_ticks, sum_calls, sum_frame;   // (the prototype's log: a second's worth)
} actq[ACTQ];
__thread uint64_t tl_act_actor;   // the copy whose action request this file is making (read by en_copy_hook)
__thread uint32_t tl_act_ticks, tl_act_runs, tl_act_other;   // under one call: node ticks, the slot's graph run, other runner entries turned away
__thread uint64_t tl_act_lr;      // the last of those others: its entry << 32 | where it was called from
__thread uint32_t tl_act_plays;   // plays asked by the action's nodes and not made (counted over the copy's life of the thread)
static __thread uint64_t tl_act_node_vt;   // the class (vtable) of the last node the action ticked (named in a refusal's log)
extern __thread uint32_t tl_act_refused;
static _Atomic int n_told, n_made, n_refused_msg, n_form;

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
// The interrupt reactions a copy may run (research R29 2.3 and 2.5): their roots act on nothing step 2 leaves open
// (their knock-back and drops are denied, their plays answered "played"). 151 SmallHit, 152 MediumHit, 155 StealthHit,
// 160 Guard, 162 GuardBreak, 164 GuardJustRepelled, 166 Repelled, 167 Combust, 169 Paralyze, 173 Fall, 237 SightDown
// (the numbers of build/re/enemy/verbs/action_verbs_ids.tsv). Every other interrupt stays a stop.
static int light_interrupt(uint32_t v) {
    static const uint16_t light[] = {151, 152, 155, 160, 162, 164, 166, 167, 169, 173, 237};
    for (size_t i = 0; i < sizeof light / sizeof light[0]; i++) if (light[i] == v) return 1;
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
// The form keys a request may carry (research R29 3.2: every key OneShotBSAChangeForm sets by an immediate text, over
// the enemies' graphs; `Search` from the component mode change main+0x1c36fe8). Keys that come through a graph's link
// were not read and are not here [guess: the Bokoblin's HorseRide and RideSeat may be among those, and are listed].
// Empty: the default form.
static int form_known(const char *f) {
    static const char *const known[] = {
        "Sleep", "HorseRide", "RideSeat", "Down", "Battle", "Lynel_Battle", "HorseRideTarget", "specialAttackLife",
        "Mimic", "WallMimic", "ConfuseUnderWater", "Hide", "SightDown", "BasicAttack", "WeakPointHit", "WeakPointWithTar",
        "Chemical", "ChemicalSuperArmor", "SuperArmor", "SmallDamage", "Smash", "EscapeToCave", "IsFalling", "Eat",
        "Ride", "SecondHalf", "Search",
    };
    if (!*f) return 1;
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) if (!strcmp(f, known[i])) return 1;
    return 0;
}
// Whether a play asked by a node of a copy's action is made (default: no, the copy's slots are its owner's).
int en_act_plays(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_ACT_PLAYS"); on = e && *e == '1'; }
    return on;
}
// Whether this copy (cell + 1) has an action told for it, on any slot (coop_enemy_fx.c: an effect may then be its own
// node's).
int en_act_has(int cell) {
    for (int i = 0; i < ACTQ; i++) if (atomic_load_explicit(&actq[i].cell, memory_order_relaxed) == cell) return 1;
    return 0;
}
static uint32_t verb_of(const uint8_t *req) {
    uint32_t f, v;
    memcpy(&f, req + 0x28, 4), memcpy(&v, req + 0x4c, 4);
    return f != VERB_NONE ? f : v;
}
static uint32_t fnv_txt(uint32_t h, const char *s) {
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return (h ^ 0xff) * 16777619u;   // (an end mark: "ab" + "" is not "a" + "b")
}
// Text read from the game (at most n - 1 bytes) cut at its end or at the first byte that is not printable.
static void read_text(uint64_t p, char *out, size_t n) {
    out[0] = 0;
    if (!p || !coop_rd(p, out, n - 1)) { out[0] = 0; return; }
    out[n - 1] = 0;
    for (char *q = out; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
}
// An actor's form key as text, the default one as "".
static void form_of(uint64_t bsa, char out[32]) {
    read_text(coop_rd_ptr(bsa + 0x2628), out, 32);
    if (!strcmp(out, "cReservedDefaultFormChangeKey")) out[0] = 0;
}

// The request's entry in the game that runs the enemy (x0 the BSA, x1 the request, w2 the mode), from en_copy_hook.
// Told on every slot: whenever the request is a listed one, and once more when it stops being one (a stop). Mode 1
// (enter) whenever the verb, the variation or the form changed since the last told on that slot: with mode 0 the
// game refreshes the running root, which would keep a copy in the old one (R29 1.1 step 5). Told at the call's entry,
// so what is told is the asked request, before slot 0's move-control adjustment, and a second request of the frame on
// one slot (which the owner's slot refuses with 3) is told as well (R29 4.1: not measured whether that happens).
static __thread uint32_t tl_act_verb;
int en_act_tick_seen(uint64_t node);
void en_act_owner(Cpu *c) {
    if (!en_act_on()) return;
    uint64_t bsa = c->x[0], actor = coop_rd_ptr(bsa + 0x18);
    EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
    if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return;
    MAct a;
    uint8_t req[0x60];
    uint32_t slot = 0;
    uint64_t flags = 0;
    memset(&a, 0, sizeof a);
    if (!coop_rd(c->x[1], req, sizeof req)) return;
    memcpy(&slot, req + 0x48, 4), memcpy(&flags, req + 0x50, 8);
    if (slot >= ACT_SLOTS) slot = 0;   // (the game runs an index of 5 or more on Main)
    uint32_t v = verb_of(req);
    int intr = (int)(flags & 1);
    char an[48];
    // (None on slots 1 to 4 is never "listed": after a told request it goes as the slot's stop, the game's own None)
    int listed = family_listed(coop_actor_name(actor, an, sizeof an)) && verb_listed(v) && !(slot && !v);
    if (intr && !light_interrupt(v)) listed = 0;   // (a heavy interrupt: a stop, as before step 3)
    char form[32];
    form_of(bsa, form);
    if (listed && !form_known(form)) {   // (a form the copy's game would refuse: the copy could not enter the owner's root)
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 10) rt_log("coop: en: act: %s (run here) is in form '%s', which is not carried: its slot %u is told a stop", an, form, slot);
        listed = 0;
    }
    int told = m->act_told >> slot & 1;
    if (!listed && !told) return;
    memcpy(a.req, req, ACT_REQ);
    memcpy(a.req + 0x48, &slot, 4);
    // What the request does not use holds leftovers of the asking game's stack, pieces of its addresses among them
    // (the net of B.40 named them in the first run): the rest of a vector whose first number says "none" (NaN), the
    // bytes after the valid byte, the bytes after the 16 flag bits (stored with a half-word store, main+0x2bbb0).
    for (int k = 0; k < 0x24; k += 0xc) { uint32_t x; memcpy(&x, a.req + k, 4); if ((x & 0x7fffffffu) > 0x7f800000u) memset(a.req + k + 4, 0, 8); }
    memset(a.req + 0x41, 0, 7), memset(a.req + 0x52, 0, 6);
    { void en_foreign_scan(const char *what, const uint8_t *p, int n, int skip); en_foreign_scan("an action request", a.req, ACT_REQ, 0x38); }   // coop_enemy_shot.c
    memset(a.req + 0x38, 0, 8);
    a.mode = (uint8_t)(c->x[2] & 1), a.stop = !listed, a.slot = (uint8_t)slot, a.intr = (uint8_t)(listed && intr), a.ns = (int64_t)coop_sess_frame_ns();
    memcpy(a.form, form, sizeof a.form);
    if (listed && req[0x40] == 1) {
        uint64_t vp;
        memcpy(&vp, req + 0x38, 8);
        read_text(vp, a.var, sizeof a.var);
    }
    a.req[0x40] = a.var[0] ? 1 : 0;   // (the valid byte as the copy uses it: a variation sent or none)
    uint32_t key = fnv_txt(fnv_txt(2166136261u, a.var), a.form);
    int changed = listed && (!told || v != m->act_verb[slot] || key != m->act_key[slot]);
    if (changed) a.mode = 1;
    coop_lock_wait(&en_lock);
    int16_t ei = atomic_load(&m->en);
    if (ei >= 0 && en_tab[ei].used) {
        a.id = en_tab[ei].id;
        en_send_all(ENM_ACT, &a, sizeof a);
        if (changed || a.stop) {
            uint64_t sl = coop_rd_ptr(bsa + 0x1f0 + 8ull * slot), ct = sl ? coop_rd_ptr(sl + 0x70) : 0, ipo = ct ? coop_rd_ptr(ct + 0x20) : 0, ino = ipo ? coop_rd_ptr(ipo) : 0;
            int32_t rt = -1;
            uint32_t f7c = 0, qn = 0;
            if (ct) coop_rd(ct + 0x19c, &rt, 4), coop_rd(sl + 0x7c, &f7c, 4);
            if (ino) coop_rd(ino + 0x90, &qn, 4);
            if (atomic_fetch_add(&n_told, 1) < 80) rt_log("coop: en: act: %s %016llx (run here) %s slot %u%s verb %u%s%s%s%s, mode %u: told to its copies (its slot before the call: root %d, +0x7c %x, queue %u; asked from main+%#llx)", en_tab[ei].name, (unsigned long long)en_tab[ei].id.a, a.stop ? "leaves the action of" : "asks", slot, a.intr ? " (an interrupt, told as a forced request)" : "", v, a.var[0] ? " variation " : "", a.var, a.form[0] ? " form " : "", a.form, a.mode, rt, f7c, qn, (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
        }
    }
    plat_mutex_unlock(&en_lock);
    if (listed) m->act_told |= (uint8_t)(1u << slot), m->act_verb[slot] = v, m->act_key[slot] = key;
    else m->act_told &= (uint8_t)~(1u << slot);
}

// Text in a message: ended within its field and printable.
static int text_ok(const char *s, size_t n) {
    const char *z = memchr(s, 0, n);
    if (!z) return 0;
    for (; s < z; s++) if (*s < 0x20 || *s >= 0x7f) return 0;
    return 1;
}
// What another game tells is checked here, where it is received (docs/coop/README.md rule 7): the slot below 5 (5 and
// over would run on Main) and the request's own slot the same; the verb in range; flags only bits 0 (interrupt, with
// the mark) and 8 (force restart); the numbers finite (NaN is the request's "none") and within the world; texts ended
// and printable, the form one of the known keys or empty. 0: refused.
static int act_msg_ok(const MAct *a) {
    if (a->slot >= ACT_SLOTS || a->mode > 1 || a->stop > 1 || a->intr > 1) return 0;
    if (!text_ok(a->var, sizeof a->var) || !text_ok(a->form, sizeof a->form) || !form_known(a->form)) return 0;
    if (a->stop) return 1;   // (a stop's request is not used: the copy asks Wait or None of its own)
    uint32_t f28, v4c, slot;
    uint16_t fl;
    memcpy(&f28, a->req + 0x28, 4), memcpy(&v4c, a->req + 0x4c, 4), memcpy(&slot, a->req + 0x48, 4), memcpy(&fl, a->req + 0x50, 2);
    if (slot != a->slot || f28 > VERB_NONE || (f28 == VERB_NONE && v4c >= VERB_NONE)) return 0;
    if ((fl & ~0x101u) || (fl & 1) != a->intr || a->req[0x40] > 1 || (a->var[0] && a->req[0x40] != 1)) return 0;
    static const uint8_t fo[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x2c, 0x30};
    for (size_t i = 0; i < sizeof fo; i++) {
        float x;
        memcpy(&x, a->req + fo[i], 4);
        if (!isnan(x) && (!isfinite(x) || fabsf(x) > 1e5f)) return 0;
    }
    return 1;
}

// A message of this file's (from en_copy_msg). 1: it was one.
int en_act_msg(const CoopCtrMsg *m) {
    if (m->type != ENM_ACT) return 0;
    const MAct *a = (const MAct *)m->d;
    uint64_t me = coop_ctr_me();
    if (m->len != (int)sizeof *a || !act_msg_ok(a)) {
        if (atomic_fetch_add(&n_refused_msg, 1) < 10) rt_log("coop: en: act: a request from %016llx refused: it does not pass the checks (length %d)", (unsigned long long)m->from, m->len);
        return 1;
    }
    for (int i = 0; i < EN_N; i++) {
        // (the owner's word only: the game that runs the enemy)
        if (!en_tab[i].used || !enid_eq(en_tab[i].id, a->id) || en_tab[i].owner == me || en_tab[i].owner != m->from || en_tab[i].cell < 0) continue;
        int cell = en_tab[i].cell + 1, f = -1, fz = 0, have = -1;
        uint32_t fr = coop_sess_frame_no();
        for (int k = 0; k < ACTQ; k++) {
            int cc = atomic_load(&actq[k].cell);
            if (cc == cell && actq[k].slot == a->slot) have = k;
            // (free, or a queue whose copy has had no calc for 5 s: deleted or unloaded without a stop, as a Sub held in
            // Ride whose copy went is)
            else if (f < 0 && (!cc || (cc > 0 && cc <= EN_MAP_N && fr - atomic_load(&en_map[cc - 1].frame) > 300))) f = k, fz = cc;
        }
        if (have < 0 && (a->stop || f < 0)) return 1;
        int k = have >= 0 ? have : f, z = fz;
        if (have < 0 && !atomic_compare_exchange_strong(&actq[k].cell, &z, -1)) return 1;
        if (have < 0) {
            actq[k].slot = a->slot, actq[k].verb = 0, actq[k].enter = 0, actq[k].force = 0, actq[k].stop = 0, actq[k].said = 0, actq[k].have = 0, actq[k].form_have = 0;
            actq[k].sum_runs = actq[k].sum_ticks = actq[k].sum_calls = 0, actq[k].sum_frame = coop_sess_frame_no();
            atomic_store(&actq[k].in_w, 0), atomic_store(&actq[k].in_r, 0);
        }
        uint32_t w = atomic_load(&actq[k].in_w);
        if (w - atomic_load(&actq[k].in_r) >= ACT_IN) w--;   // (full: the newest told takes the last place)
        ActIn *in = &actq[k].in[w % ACT_IN];
        in->ns = a->ns, in->taken = en_now(), in->mode = a->mode, in->stop = a->stop;
        memcpy(in->req, a->req, ACT_REQ), memcpy(in->var, a->var, sizeof a->var), memcpy(in->form, a->form, sizeof a->form);
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

// ---- The form key on a copy ------------------------------------------------------------------------------------------
// The setter keeps the pointer it is given the address of, for as long as the form is set (main+0x88d524 stores
// *key at scheduler +0xba0). So each text is kept here once, in a table the mod never frees, with a cell holding its
// pointer; the setter is given that cell's address. The texts are the known keys (form_known), so the table never fills.
#define FORM_N 64
static char form_txt[FORM_N][32];
static uint64_t form_cell[FORM_N];
static _Atomic int form_n;
static atomic_flag form_lock = ATOMIC_FLAG_INIT;
static uint64_t form_keep(const char *t) {
    int n = atomic_load_explicit(&form_n, memory_order_acquire);
    for (int i = 0; i < n; i++) if (!strcmp(form_txt[i], t)) return (uint64_t)(uintptr_t)&form_cell[i];
    uint64_t r = 0;
    while (atomic_flag_test_and_set_explicit(&form_lock, memory_order_acquire)) {}
    n = atomic_load(&form_n);
    for (int i = 0; i < n && !r; i++) if (!strcmp(form_txt[i], t)) r = (uint64_t)(uintptr_t)&form_cell[i];
    if (!r && n < FORM_N) {
        strncpy(form_txt[n], t, sizeof form_txt[n] - 1);
        form_cell[n] = (uint64_t)(uintptr_t)form_txt[n];
        r = (uint64_t)(uintptr_t)&form_cell[n];
        atomic_store_explicit(&form_n, n + 1, memory_order_release);
    }
    atomic_flag_clear_explicit(&form_lock, memory_order_release);
    return r;
}
// The copy given its owner's form, as the game's own nodes give one (research R29 3.1; the calls read in Ghidra):
// OneShotBSAChangeForm sets the key (main+0x88d524 with force 0), then that form's attribute sets (main+0xd141fc),
// then clears BSA+0x24 bit 0x4000; OneShotBSAResetForm (main+0x2502b40), only while that bit is clear, sets the default
// key (force 1), the default attribute sets (main+0x88d794) and sets the bit. Called outside the action mark, before
// the copy's requests of the frame, so a root with the form's prefix is found. ChangeForm's own body (main+0xd1417c,
// no function in Ghidra) is taken from R29's reading.
static void form_set(Cpu *c, uint64_t actor, uint64_t bsa, const char *want) {
    char was[32];
    form_of(bsa, was);
    if (!strcmp(was, want)) return;
    uint32_t f24 = 0;
    coop_rd(bsa + 0x24, &f24, 4);
    if (!*want) {
        uint64_t def = coop_rd_ptr(MOD_MAIN_BASE + FORM_DEFAULT_KEY);
        char dt[32];
        read_text(def ? coop_rd_ptr(def) : 0, dt, sizeof dt);
        if (strcmp(dt, "cReservedDefaultFormChangeKey")) {
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 3) rt_log("coop: en: act: the default form key is not where it was read (main+%#x gives '%s'): the copy's form is left", FORM_DEFAULT_KEY, dt);
            return;
        }
        if (f24 & 0x4000) return;   // (the game's reset does nothing then either)
        coop_guest_call(c, MOD_MAIN_BASE + FN_FORM_SET, bsa + 0x1a88, def, 1, 0);
        coop_guest_call(c, MOD_MAIN_BASE + FN_FORM_DEFAULT, bsa, 0, 0, 0);
        __atomic_fetch_or((uint32_t *)(uintptr_t)(bsa + 0x24), 0x4000u, __ATOMIC_SEQ_CST);
    } else {
        uint64_t key = form_keep(want);
        if (!key) return;
        coop_guest_call(c, MOD_MAIN_BASE + FN_FORM_SET, bsa + 0x1a88, key, 0, 0);
        coop_guest_call(c, MOD_MAIN_BASE + FN_FORM_ATTRS, bsa, key, 0, 0);
        __atomic_fetch_and((uint32_t *)(uintptr_t)(bsa + 0x24), ~0x4000u, __ATOMIC_SEQ_CST);
    }
    char now_[32];
    form_of(bsa, now_);
    if (atomic_fetch_add(&n_form, 1) < 40) rt_log("coop: en: act: the copy %llx is given its owner's form '%s' (it was '%s'; now '%s')", (unsigned long long)actor, want, was, now_);
}

// A request of the game's own empty fill of slots 1 to 4 (the scheduler's fallback, main+0x46f020..0x46f0b0): verb 0
// None, +0x24 4, +0x28 0x11c, no vectors (NaN), +0x34 0x12, no variation, flags 0; or Main's leave: Wait (40) with
// +0x24 2.
static void stop_req(uint8_t *req, uint32_t slot) {
    uint32_t nan = 0x7fc00000u, kind = slot ? 4 : 2, none = VERB_NONE, d = 0x12, w = slot ? 0 : VERB_WAIT;
    memset(req, 0, 0x60);
    memcpy(req, &nan, 4), memcpy(req + 0xc, &nan, 4), memcpy(req + 0x18, &nan, 4), memcpy(req + 0x24, &kind, 4), memcpy(req + 0x28, &none, 4);
    memcpy(req + 0x2c, &nan, 4), memcpy(req + 0x30, &nan, 4), memcpy(req + 0x34, &d, 4), memcpy(req + 0x48, &slot, 4), memcpy(req + 0x4c, &w, 4);
}

// One slot's call on the copy. Returns the call's answer.
static uint64_t act_call(Cpu *c, uint64_t actor, uint64_t bsa, int k, int stop, uint32_t frame) {
    static __thread _Alignas(16) uint8_t req[0x60];
    static __thread _Alignas(16) char var[ACT_SLOTS][32];
    uint32_t s = actq[k].slot;
    uint64_t sl = coop_rd_ptr(bsa + 0x1f0 + 8ull * s), ctrl = sl ? coop_rd_ptr(sl + 0x70) : 0, ip = ctrl ? coop_rd_ptr(ctrl + 0x20) : 0;
    uint64_t mode;
    if (stop) {   // Main: Wait, once, with the force (the root the copy was in is left); slots 1 to 4: None, as the game's fill
        stop_req(req, s);
        mode = s ? 0 : 1;
    } else {
        memset(req, 0, sizeof req);
        memcpy(req, actq[k].req, ACT_REQ);
        if (actq[k].var[0]) {
            uint64_t vp = (uint64_t)(uintptr_t)var[s];
            memcpy(var[s], actq[k].var, sizeof var[s]), memcpy(req + 0x38, &vp, 8), req[0x40] = 1;
        } else memset(req + 0x38, 0, 8), req[0x40] = 0;
        // (an interrupt runs as a plain request of its verb: no teardown, no status 1; the force restart only on the call
        // of a request just taken, as the owner's node asks it once at its enter and refreshes without it)
        uint16_t fl;
        memcpy(&fl, req + 0x50, 2);
        fl = (uint16_t)((fl & 0x100u) & (actq[k].force ? 0x100u : 0u));
        memcpy(req + 0x50, &fl, 2);
        mode = actq[k].enter;
        actq[k].enter = 0, actq[k].force = 0;
    }
    int32_t root0 = -1, root1 = -1;
    uint32_t s7c0 = 0, s7c1 = 0, s780 = 0, s781 = 0, q0 = 0, q1 = 0;
    uint64_t inst = ip ? coop_rd_ptr(ip) : 0;
    if (ctrl) coop_rd(ctrl + 0x19c, &root0, 4);
    if (sl) coop_rd(sl + 0x7c, &s7c0, 4), coop_rd(sl + 0x78, &s780, 4);
    if (inst) coop_rd(inst + 0x90, &q0, 4);
    tl_act_ticks = tl_act_runs = tl_act_other = tl_act_refused = 0, tl_act_lr = 0, tl_act_node_vt = 0;
    uint32_t v = stop ? (s ? 0 : VERB_WAIT) : actq[k].verb;
    tl_act_verb = v;
    tl_act_actor = actor;
    uint64_t ret = coop_guest_call(c, MOD_MAIN_BASE + FN_ACT, bsa, (uint64_t)(uintptr_t)req, mode, 0);
    tl_act_actor = 0;
    if (ctrl) coop_rd(ctrl + 0x19c, &root1, 4);
    if (sl) coop_rd(sl + 0x7c, &s7c1, 4), coop_rd(sl + 0x78, &s781, 4);
    if (inst) coop_rd(inst + 0x90, &q1, 4);
    if (mode || stop || !actq[k].said || (uint32_t)ret) {
        if (atomic_fetch_add(&n_made, 1) < 120) rt_log("coop: en: act: the copy %llx's slot %u is asked for %s verb %u, mode %u: the call answered %u, the slot's root %d -> %d; slot +0x7c %x -> %x, +0x78 %u -> %u, queue %u -> %u; under the call: the slot's graph run %u times, %u node ticks, %u other entries turned away (last main+%#llx from main+%#llx), %u engine calls refused", (unsigned long long)actor, s, stop ? (s ? "None, leaving" : "Wait, leaving") : "its owner's", v, (unsigned)mode, (unsigned)ret, root0, root1, s7c0, s7c1, s780, s781, q0, q1, tl_act_runs, tl_act_ticks, tl_act_other, (unsigned long long)(tl_act_lr >> 32), (unsigned long long)(tl_act_lr & 0xffffffffu), tl_act_refused);
        actq[k].said = 1;
    }
    actq[k].sum_runs += tl_act_runs, actq[k].sum_ticks += tl_act_ticks, actq[k].sum_calls++;
    if (frame - actq[k].sum_frame >= 60) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 60) rt_log("coop: en: act: the copy %llx's slot %u in verb %u: %u calls, its graph run %u times, %u node ticks in the last second", (unsigned long long)actor, s, actq[k].verb, actq[k].sum_calls, actq[k].sum_runs, actq[k].sum_ticks);
        actq[k].sum_runs = actq[k].sum_ticks = actq[k].sum_calls = 0, actq[k].sum_frame = frame;
    }
    return ret;
}

// The copy's pre-calc: its owner's requests asked of its own slots. 1 while the copy's Main slot runs an action (the
// caller then leaves the arm replay and the held effects to the action's own nodes).
int en_act_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    if (!en_act_on()) return 0;
    int cell = (int)(m - en_map) + 1, ks[ACT_SLOTS], any = 0;
    for (int s = 0; s < ACT_SLOTS; s++) ks[s] = -1;
    for (int i = 0; i < ACTQ; i++)
        if (atomic_load_explicit(&actq[i].cell, memory_order_relaxed) == cell && actq[i].slot < ACT_SLOTS && ks[actq[i].slot] < 0) ks[actq[i].slot] = i, any = 1;
    if (!any) return 0;
    uint64_t bsa = en_bsa(actor);
    uint32_t frame = coop_sess_frame_no();
    int fk = -1;   // the queue whose last taken request carries the newest form
    {   // What was told and is due: this game shows the owner at the request's time (or it waited a second).
        int64_t en_shot_T_of(int id);   // coop_enemy_shot.c
        int16_t ei = atomic_load(&m->en);
        int id = ei >= 0 && en_tab[ei].used ? en_peer_id(en_tab[ei].owner) : 0;
        int64_t T = en_shot_T_of(id);
        uint64_t now = en_now();
        for (int s = 0; s < ACT_SLOTS; s++) {
            int k = ks[s];
            if (k < 0) continue;
            for (uint32_t r = atomic_load(&actq[k].in_r); r != atomic_load(&actq[k].in_w); atomic_store(&actq[k].in_r, ++r)) {
                const ActIn *in = &actq[k].in[r % ACT_IN];
                if (T && T < in->ns && now - in->taken < 1000000000ull) break;
                memcpy(actq[k].form, in->form, sizeof in->form), actq[k].form_ns = in->ns, actq[k].form_have = 1;
                if (in->stop) { actq[k].stop = 1; continue; }
                uint32_t v = verb_of(in->req);
                uint16_t fl;
                memcpy(&fl, in->req + 0x50, 2);
                if (in->mode || v != actq[k].verb) actq[k].enter = 1;
                if (fl & 0x100) actq[k].force = 1;
                memcpy(actq[k].req, in->req, ACT_REQ), memcpy(actq[k].var, in->var, sizeof in->var);
                actq[k].verb = v, actq[k].stop = 0, actq[k].have = 1;
            }
            if (actq[k].form_have && (fk < 0 || actq[k].form_ns > actq[fk].form_ns)) fk = k;
        }
    }
    // Only once the copy has left its warm-up and its Main slot has its controller and its instance (R22 Q6).
    uint64_t slot0 = bsa ? coop_rd_ptr(bsa + 0x1f0) : 0, ctrl0 = slot0 ? coop_rd_ptr(slot0 + 0x70) : 0, ip0 = ctrl0 ? coop_rd_ptr(ctrl0 + 0x20) : 0;
    int ready = atomic_load_explicit(&m->warm, memory_order_relaxed) && ip0 && coop_rd_ptr(ip0);
    if (ready && fk >= 0) form_set(c, actor, bsa, actq[fk].form);   // (the form first: it names the root the requests find)
    int run = 0, wrote = 0;
    for (int s = 0; s < ACT_SLOTS; s++) {   // Main first, then the others, as the owner's update asks them
        int k = ks[s];
        if (k < 0) continue;
        // Main is told every frame its action sequence runs, so half a second without a word means its owner's calls
        // stopped. Slots 1 to 4 are told once at a state's enter and leave: the copy keeps their request until told
        // otherwise, and asks it again each frame with mode 0, since a slot's graph ticks only under its own request
        // call (research R29 1.5).
        int stop = actq[k].stop || (!s && frame - actq[k].frame > 30);
        if (!actq[k].have && !stop) continue;   // (told, none due yet)
        if (!ready || !coop_rd_ptr(bsa + 0x1f0 + 8ull * (uint64_t)s)) {   // (not yet, or the copy has no such slot)
            if (stop || (s && ready)) atomic_store(&actq[k].cell, 0);
            continue;
        }
        if (!wrote) cct_write(actor, m), wrote = 1;
        act_call(c, actor, bsa, k, stop, frame);
        if (stop) atomic_store(&actq[k].cell, 0);
        else if (!s) run = 1;
    }
    if (run && coop_debug()) {   // (record: the SensorHit component's armed bits right after the action ran, when any is set)
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
    return run;
}

// A cell's queues let go of (cell + 1): its copy thinks again, or the cell starts for another actor (coop_enemy.c
// map_touch), whose calc must not get the old actor's held requests.
void en_act_forget(int cell) {
    for (int i = 0; i < ACTQ; i++) if (atomic_load_explicit(&actq[i].cell, memory_order_relaxed) == cell) atomic_store(&actq[i].cell, 0);
}

// A copy that thinks again (taken over, or its own): its queues are let go of, and each of its slots 1 to 4 that holds
// a started request other than None is asked None, as the game's own fill does. Its utility, starting from its
// default case, would never run the leave that asks it (R29 4.3: a Sub left in Ride keeps the lower body split). In
// the game that now runs it each such None is told to the copies elsewhere too, as that slot's stop (en_act_owner
// tells a None on slots 1 to 4 only after a told request, so the slot is marked told first), since their queues still
// hold the old owner's request. The other told marks are cleared, so the next request of each slot is told as an
// enter. From en_copy_precalc, on the actor's thread.
void en_act_leave(Cpu *c, uint64_t actor, EnCell *m) {
    if (!en_act_on()) return;
    int cell = (int)(m - en_map) + 1;
    en_act_forget(cell);
    m->act_told = 0;
    uint64_t bsa = en_bsa(actor);
    static __thread _Alignas(16) uint8_t req[0x60];
    for (uint32_t s = 1; bsa && s < ACT_SLOTS; s++) {
        uint64_t sl = coop_rd_ptr(bsa + 0x1f0 + 8ull * s);
        uint8_t held[0x50];
        uint32_t f7c = 0;
        if (!sl || !coop_rd(sl, held, sizeof held) || !coop_rd(sl + 0x7c, &f7c, 4) || !(f7c & 1)) continue;
        uint32_t v = verb_of(held);
        if (!v) continue;
        stop_req(req, s);
        m->act_told |= (uint8_t)(1u << s), m->act_verb[s] = ~0u;   // (so the owner's entry tells this None as the slot's stop)
        uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + FN_ACT, bsa, (uint64_t)(uintptr_t)req, 0, 0);
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: act: %llx thinks again: its slot %u, in verb %u, is asked None (the call answered %u)", (unsigned long long)actor, s, v, (unsigned)r);
    }
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
    tl_act_node_vt = vt;
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

// ---- Refusals at the engine functions an action ends in (PLAN.md step 2; docs/coop/facts/enemies.md 4.1) -------------
// What a copy's action would do to the shared world passes through a few engine functions: a create, an equipment
// create, a drop, a throw, a remove, a pick-up, a lifter's throw, a Shooter's prepare, shoot and sleep, a death
// request, a drop made without a death, a kill flag, a noise. The owner's game does each of these and the sync carries
// its result (a noise is carried by nothing: it is heard where it is made). So while the mod's call of a copy's action
// runs (tl_act_actor set), each is skipped, whichever node made the call, with the answer the game itself gives when
// it does nothing, chosen so that every caller an action reaches carries on (facts 4.1 says why for each). The deny
// list above is a second net: a refusal here logs the function, where it was called from and the last node class the
// action ticked, so a class missing from the list is found.
//
// Where the function names the actor it is about, the refusal is for the marked copy only (SYNC-006: were another
// actor's calc ever run inside the copy's action, its calls are its own): the equipment user's entries ([x0+0x18],
// the holder) and the pick-up (x1, the picker). The others are refused under the mark alone. A death request is among
// them on purpose: an action's own requests are about other actors too (the held weapon, a lifted actor, bullets),
// and each is an act of the owner's game.
//
// The mod's own replays on a copy (equipment create, draw and put away, the throw, the shots and sleeps) run in
// en_copy_precalc before the action is asked for, outside the mark, and are never refused here.
//
// A refusal sets the answer in x0 and the hook returns 1, which makes the entry return at once with the guest seeing
// x0 as the function's return; a void function's x0 is left as it is.
__thread uint32_t tl_act_refused;   // engine calls refused under one action call (the per-call log line)
static _Atomic uint64_t refuse_seen[512];   // (function << 32 | where it was called from), each logged once

static const char *refuse_name(uint64_t off) {
    switch (off) {
    case 0x599670: return "the actor create";
    case 0xaf100c: return "the equipment create";
    case 0xc4961c: return "the static equipment create";
    case 0x4f59ec: return "the equipment mode 0 (drop where it is)";
    case 0x1b738e4: return "the drop record";
    case 0x1b73be4: return "the throw record";
    case 0x1b70a04: return "the remove of a held thing";
    case 0x1d71dfc: return "the pick-up";
    case 0x1ba796c: return "the lifter's throw";
    case 0x1bf8df4: return "the Shooter's prepare";
    case 0x1bfbaa4: case 0x1bf8f74: case 0x1bfbb28: return "a Shooter's shoot";
    case 0x1bfa698: case 0x1bfa4e0: case 0x1bfa87c: return "a Shooter's sleep";
    case 0x1953184: return "the death request (by actor)";
    case 0x1b4b738: return "the death request";
    case 0x1b55b48: return "the drop now";
    case 0x1b55bb4: return "the drop reserve";
    case 0x195321c: return "placementSetDead";
    case 0x2ab6dcc: return "the game data Bool64 setter";
    case 0x1f40d34: return "a noise producer";
    case 0x4cac5c: return "the noise and terror producer";
    default: return NULL;
    }
}

// A call of one of those functions (off, main-relative), at its entry. 1: refused, the answer set; 0: it runs.
int en_act_refuse(Cpu *c, uint64_t off) {
    uint64_t me = tl_act_actor;
    if (!me) return 0;
    const char *what = refuse_name(off);
    if (!what) return 0;
    uint64_t about = 0;   // the actor the call names, where it names one
    switch (off) {
    case 0x4f59ec:   // (component, slot, mode): modes 1 and 2 draw and put away, the copy's own body; 0 is a drop
        if ((uint32_t)c->x[2] != 0) return 0;
        /* fall through */
    case 0xaf100c: case 0x1b738e4: case 0x1b73be4: case 0x1b70a04: about = coop_rd_ptr(c->x[0] + 0x18); break;   // the equipment user's holder
    case 0x1d71dfc: about = c->x[1]; break;   // the picker
    default: break;
    }
    int refuse = !about || about == me;
    uint64_t lr = c->x[30] - MOD_MAIN_BASE, key = off << 32 | (lr & 0xffffffffu), have;
    int isnew = 0;
    for (int i = 0; i < 512; i++) {
        have = atomic_load_explicit(&refuse_seen[i], memory_order_relaxed);
        if (have == key) break;
        if (!have) {
            if (!atomic_compare_exchange_strong(&refuse_seen[i], &have, key)) { if (have == key) break; continue; }
            isnew = 1;
            break;
        }
    }
    if (isnew) {
        char an[48] = "";
        if (refuse) rt_log("coop: en: act: the copy %llx in verb %u: %s main+%#llx refused, from main+%#llx (last node ticked: class vtable main+%#llx)", (unsigned long long)me, tl_act_verb, what, (unsigned long long)off, (unsigned long long)lr, (unsigned long long)(tl_act_node_vt ? tl_act_node_vt - MOD_MAIN_BASE : 0));
        else rt_log("coop: en: act: the copy %llx in verb %u: %s main+%#llx from main+%#llx is about another actor, %llx %s: let through", (unsigned long long)me, tl_act_verb, what, (unsigned long long)off, (unsigned long long)lr, (unsigned long long)about, about > 0x10000 ? coop_actor_name(about, an, sizeof an) : "-");
    }
    if (!refuse) return 0;
    tl_act_refused++;
    switch (off) {
    case 0x599670: {   // the game's "manager closed" answer: 0, error code 1 to the caller's out pointer ([sp+8] at the entry), the handle cell untouched
        uint64_t ep = coop_rd_ptr(c->sp + 8);
        uint32_t was;
        if (ep > 0x10000 && coop_rd(ep, &was, 4)) { uint32_t one = 1; memcpy((void *)(uintptr_t)ep, &one, 4); }
        c->x[0] = 0;
        break;
    }
    case 0xaf100c: case 0xc4961c: c->x[0] = 0; break;     // no equipment made: every node caller ignores it
    case 0x1d71dfc: case 0x1ba796c: c->x[0] = 1; break;   // picked, thrown: the node succeeds, as on the owner
    case 0x1bf8df4: c->x[0] = 0xffffffffull; break;       // no record (the 32-bit -1, zero-extended)
    case 0x1bfbaa4: case 0x1bf8f74: case 0x1bfbb28: c->x[0] = 0; break;   // no prepared record shot
    case 0x1bfa698: case 0x1bfa4e0: case 0x1bfa87c: c->x[0] = 0; break;   // nothing put to sleep
    case 0x1953184: case 0x1b4b738: c->x[0] = 1; break;   // "started": the copy's death comes from its owner's
    case 0x1b55bb4: case 0x2ab6dcc: c->x[0] = 0; break;
    default: break;   // void: the drop and throw records, the remove, mode 0, the drop now, placementSetDead, the noises
    }
    return 1;
}
