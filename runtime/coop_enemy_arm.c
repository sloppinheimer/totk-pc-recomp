// Co-op: an attack armed by an AI node, on a copy (docs/coop/enemy-sync-spec.md B.35; research R20, the SensorHit
// component).
//
// An enemy's attack is not a held flag. The arm call main+0x1bf20cc(SensorHit component, request) is made again every
// frame while the attack is on, and the component's calc drops the armed bits each frame. Two things make the call:
// the animation (main+0x42ec98, inside the actor's calc, from the clip's AtCollision events), which also runs on a
// copy because the clip does; and nodes of the AI graph (ExecuteSensorHitSetupAttack and about twenty other callers),
// which a copy does not run. Seventeen actors are armed by nodes only (the plain, fire and ice Chuchus, a dungeon
// boss, the Keese swarms): their copies never hit; thirty-four more are armed by both, and the copy lacked what the
// graph adds.
//
// The request is 0x24 bytes with no pointer in it (+0 power-set id, +4 attack type, +8 power add, +0xc rate, +0x10
// the mask of sensor sets, +0x18 the layer-kind mask, +0x1c the layer, +0x20 a flag), so it is the same data in every
// game. The game that runs the enemy tells each node-route call; the copy's game makes the same call once a frame,
// before the copy's calc, for as long as the calls keep coming, and stops when they stop: the attack then ends by
// itself, as it does where the enemy runs.
//
// Two held values of the same component are stated with the enemy (EnState.bpad): the attack layer override (byte
// +0x23, 0x16 none), a plain byte the arm call reads; and "hit-receive sensors off" for the node's reason (bit 0 of
// the reasons word +0x26), put on and off at its edges with the game's own two calls.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);   // coop_spawn.c

#define FN_ARM 0x1bf20cc
#define ARM_SZ 0x24
#define ARM_FRAMES 4   // a request not told again in this many frames is no longer made (the owner's node stopped)
#define ARMQ 48

typedef struct { Enid id; uint8_t p[ARM_SZ]; } MArm;
static struct { _Atomic int cell; uint8_t p[ARM_SZ]; _Atomic uint32_t frame; } armq[ARMQ];   // cell + 1; 0 free, -1 being filled
static _Atomic int n_told, n_made;

static int arm_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_ARM"); on = !(e && *e == '0'); }
    return on;
}

// The arm call's entry (x0 the SensorHit component, x1 the request). In the game that runs the enemy a call that is
// not the animation's is told to the others. 0: the call goes on.
int en_arm_hook(Cpu *c, uint64_t off) {
    (void)off;
    uint64_t lr = c->x[30] - MOD_MAIN_BASE;
    if (lr >= 0x42ec98 && lr < 0x42f550) return 0;   // the animation's: a copy's own clip makes it
    if (!arm_on()) return 0;
    uint64_t ac = c->x[0], actor = ac > 0x10000 ? coop_rd_ptr(ac + 0x18) : 0;
    EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
    if (m && atomic_load_explicit(&m->state, memory_order_relaxed) == EM_COPY) {   // (a copy arming by a node of its own: the action layer's, B.38)
        static _Atomic int said;
        uint32_t type = 0;
        coop_rd(c->x[1] + 4, &type, 4);
        if (atomic_fetch_add(&said, 1) % 120 == 0) rt_log("coop: en: arm: the copy %llx's own node arms an attack (type %u, from main+%#llx; %d so far)", (unsigned long long)actor, type, (unsigned long long)lr, atomic_load(&said));
    }
    if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return 0;
    MArm a;
    memset(&a, 0, sizeof a);
    if (!coop_rd(c->x[1], a.p, ARM_SZ)) return 0;
    coop_lock_wait(&en_lock);
    int16_t ei = atomic_load(&m->en);
    if (ei >= 0 && en_tab[ei].used) {
        a.id = en_tab[ei].id;
        en_send_all(ENM_ARM, &a, sizeof a);
        if (atomic_fetch_add(&n_told, 1) % 600 == 0) {
            uint32_t type = 0;
            memcpy(&type, a.p + 4, 4);
            rt_log("coop: en: arm: %s's graph arms an attack (type %u, from main+%#llx): told to its copies (%d so far)", en_tab[ei].name, type, (unsigned long long)lr, atomic_load(&n_told));
        }
    }
    plat_mutex_unlock(&en_lock);
    return 0;
}

// A message of this file's (from en_copy_msg). 1: it was one.
int en_arm_msg(const CoopCtrMsg *m) {
    if (m->type != ENM_ARM) return 0;
    const MArm *a = (const MArm *)m->d;
    uint64_t me = coop_ctr_me();
    uint32_t frame = coop_sess_frame_no();
    if (m->len != (int)sizeof *a) return 1;
    for (int i = 0; i < EN_N; i++) {
        if (!en_tab[i].used || !enid_eq(en_tab[i].id, a->id) || en_tab[i].owner == me || en_tab[i].cell < 0) continue;
        int cell = en_tab[i].cell + 1, f = -1;
        for (int k = 0; k < ARMQ; k++) {   // the same request again: it stays on
            if (atomic_load(&armq[k].cell) == cell && !memcmp(armq[k].p, a->p, ARM_SZ)) { atomic_store(&armq[k].frame, frame); return 1; }
            if (f < 0 && (!atomic_load(&armq[k].cell) || frame - atomic_load(&armq[k].frame) > 60)) f = k;
        }
        if (f < 0) return 1;
        atomic_store(&armq[f].cell, -1);
        memcpy(armq[f].p, a->p, ARM_SZ), atomic_store(&armq[f].frame, frame);
        atomic_store(&armq[f].cell, cell);
    }
    return 1;
}

// The copy's pre-calc: its owner's node-armed attacks made here, and the two held values of its SensorHit component.
void en_arm_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    static __thread _Alignas(16) uint8_t p[ARM_SZ + 12];
    if (!arm_on()) return;
    uint64_t ac = coop_rd_ptr(en_comps(actor) + 0x428);
    if (!ac) return;
    if (m->show_arm[1] & 0x80) {
        uint8_t layer = m->show_arm[0], own = 0;
        if (coop_rd(ac + 0x23, &own, 1) && own != layer) {
            memcpy((void *)(uintptr_t)(ac + 0x23), &layer, 1);
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: arm: the copy %llx's attack layer %#x -> %#x, as its owner's", (unsigned long long)actor, own, layer);
        }
        uint16_t mask = 0;
        int want = m->show_arm[1] & 1;
        if (coop_rd(ac + 0x26, &mask, 2) && want != (mask & 1)) {   // the node's reason bit, at its edge, with the calls the node makes
            uint16_t to = want ? (uint16_t)(mask | 1) : (uint16_t)(mask & ~1u);
            memcpy((void *)(uintptr_t)(ac + 0x26), &to, 2);
            if (want && !mask) coop_guest_call(c, MOD_MAIN_BASE + 0x4e0edc, ac, 0, 0, 0);
            if (!want && !to) coop_guest_call(c, MOD_MAIN_BASE + 0x4f2384, ac, 0, 0, 0);
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: arm: the copy %llx's hit-receive sensors are %s, as its owner's", (unsigned long long)actor, want ? "off" : "on again");
        }
    }
    int cell = (int)(m - en_map) + 1;
    uint32_t frame = coop_sess_frame_no();
    for (int k = 0; k < ARMQ; k++) {
        if (atomic_load_explicit(&armq[k].cell, memory_order_relaxed) != cell) continue;
        if (frame - atomic_load(&armq[k].frame) > ARM_FRAMES) { atomic_store(&armq[k].cell, 0); continue; }
        memcpy(p, armq[k].p, ARM_SZ);
        coop_guest_call(c, MOD_MAIN_BASE + FN_ARM, ac, (uint64_t)(uintptr_t)p, 0, 0);
        if (atomic_fetch_add(&n_made, 1) % 600 == 0) {
            uint32_t type = 0;
            memcpy(&type, p + 4, 4);
            rt_log("coop: en: arm: the copy %llx arms the attack its owner's graph arms (type %u; %d so far)", (unsigned long long)actor, type, atomic_load(&n_made));
        }
    }
}

// The owner's two held values, for STATE (0x80 in the second: stated).
void en_arm_read(uint64_t actor, uint8_t out[2]) {
    uint64_t ac = coop_rd_ptr(en_comps(actor) + 0x428);
    uint16_t mask = 0;
    out[0] = 0x16, out[1] = 0;
    if (ac && coop_rd(ac + 0x23, &out[0], 1) && coop_rd(ac + 0x26, &mask, 2)) out[1] = (uint8_t)(0x80 | (mask & 1));
}
