// What a copy's graphs would have asked of other systems (docs/coop/enemy-sync-spec.md B.17; research R16, R9).
//
// A copy's graphs do not run, so three things its AI nodes do reach nobody in the copy's game:
//
// 1. An effect or a sound a node starts by key (OneShotXLinkSearchAndEmit, calc main+0x25aafec: the node's actor is
//    [[[node+8]+8]+0x1f8], the key the string behind the cell at node+0x30, and two flag cells at node+0x40 and +0x48
//    say whether it goes to the effect user and to the sound user). The owner's game states actor, key and flags at the
//    node's entry; the copy's game makes the same emits on its own actor's users ([[comps+0x68]+0x70] the effect user,
//    +0x78 the sound user) by the calls the node itself ends in, main+0xa98cf8 and main+0xa98d00 (user, key, handle
//    out). Only what a NODE starts is stated: what an animation event starts happens on a copy by itself, and stated
//    too it would sound twice. Held effects (the Yiga's Sign and Vehicle_Magic, stopped by their node's leave) and the
//    emit with a matrix are not done.
// 2. A Wizzrobe's magic weather: its always-running graph asks the weather manager every frame while its weather is on
//    (main+0x21cdf44(manager, type, reserve only, actor); the manager keeps the claim for four updates after the last
//    call and reads nothing of the actor). The owner's game notes the type at that call, STATE carries it, and the
//    copy's game makes the same call for its own actor at each of the copy's pre-calcs while it is stated.
// 3. A weapon picked up from the ground (main+0x1d71dfc(weapon link, picker, 1)): the copy is given the weapon by the
//    held-things rule, and the one lying on the ground in its game stayed. The owner's game states which object the
//    weapon was, by the object mirror's name for it, and the copy's game deletes its own actor of that object. A
//    weapon the mirror does not know (one placed on the map that nobody has moved) is not told: it stays [open].
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);                                       // coop_spawn.c
const char *coop_actor_name(uint64_t a, char *out, size_t n);
int coop_obj_ident(uint64_t a, int *owner, uint32_t *id);                // coop_players.c
uint64_t coop_obj_find(int owner, uint32_t id, const char *name);       // coop_players.c

typedef struct { Enid id; uint8_t elink, slink, mode, pad[5]; char key[48]; } MFx;   // mode: 0 a one-shot, 1 a held one starts, 2 it stops
typedef struct { Enid id; int32_t obj_owner; uint32_t obj_id; char name[48]; } MPick;

#define FXQ 32
// A held effect on a copy (a Yiga footsoldier's Sign, Vehicle_Magic): the two handles its emits gave, kept until its
// owner's node leaves, when they are stopped by the node's own stop (main+0x6e34e8(the pair, -1: the resource's fade)).
static struct { int cell; char key[48]; _Alignas(16) uint8_t pair[16]; } held[8];   // (the copy's own thread only: its pre-calc)
static struct { _Atomic int cell; uint8_t elink, slink, mode; char key[48]; uint32_t frame; } fxq[FXQ];       // emits waiting for their copy's pre-calc (cell + 1; 0 free)
static struct { _Atomic uint32_t frame; int32_t owner; uint32_t id; char name[48]; } pickq[8];         // ground weapons to delete here
static _Atomic uint32_t n_fx_sent, n_fx_made, n_wx, n_pick_sent, n_pick_done;

static int fx_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = cfg_get("TOTK_COOP_EN_FX"); v = !(e && *e == '0'); }
    return v;
}

// From en_copy_hook, at the function's entry. Always 0: the function runs.
int en_fx_hook(Cpu *c, uint64_t off) {
    if (!fx_on()) return 0;
    switch (off) {
    case 0x25aafec: {   // a node's emit by key
        uint64_t node = c->x[0], inst = node > 0x10000 ? coop_rd_ptr(node + 8) : 0, own = inst ? coop_rd_ptr(inst + 8) : 0, actor = own ? coop_rd_ptr(own + 0x1f8) : 0;
        EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
        if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return 0;
        uint64_t kc = coop_rd_ptr(node + 0x30) & ~3ull, kp = kc ? coop_rd_ptr(kc) : 0, ec = coop_rd_ptr(node + 0x40), sc = coop_rd_ptr(node + 0x48);
        MFx f;
        memset(&f, 0, sizeof f);
        if (!kp || !coop_rd(kp, f.key, sizeof f.key - 1) || !f.key[0]) return 0;
        for (char *q = f.key; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
        if (ec) coop_rd(ec, &f.elink, 1);
        if (sc) coop_rd(sc, &f.slink, 1);
        coop_lock_wait(&en_lock);
        int16_t ei = atomic_load(&m->en);
        if (ei >= 0 && en_tab[ei].used && f.key[0]) {
            f.id = en_tab[ei].id;
            en_send_all(ENM_FX, &f, sizeof f);
            if (atomic_fetch_add(&n_fx_sent, 1) < 40) rt_log("coop: en: fx: %s's graph starts '%s' (effect %u, sound %u): told to its copies", en_tab[ei].name, f.key, f.elink, f.slink);
        }
        plat_mutex_unlock(&en_lock);
        return 0;
    }
    case 0x24901b0: case 0x24902e0: {   // a held emit's node enters (key cell +0x40, flags +0x50 and +0x58), and leaves
        uint64_t node = c->x[0], inst = node > 0x10000 ? coop_rd_ptr(node + 8) : 0, own = inst ? coop_rd_ptr(inst + 8) : 0, actor = own ? coop_rd_ptr(own + 0x1f8) : 0;
        EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
        if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return 0;
        uint64_t kc = coop_rd_ptr(node + 0x40) & ~3ull, kp = kc ? coop_rd_ptr(kc) : 0, ec = coop_rd_ptr(node + 0x50), sc = coop_rd_ptr(node + 0x58);
        MFx f;
        memset(&f, 0, sizeof f);
        if (!kp || !coop_rd(kp, f.key, sizeof f.key - 1) || !f.key[0]) return 0;
        for (char *q = f.key; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
        if (ec) coop_rd(ec, &f.elink, 1);
        if (sc) coop_rd(sc, &f.slink, 1);
        f.mode = off == 0x24901b0 ? 1 : 2;
        coop_lock_wait(&en_lock);
        int16_t ei = atomic_load(&m->en);
        if (ei >= 0 && en_tab[ei].used && f.key[0]) {
            f.id = en_tab[ei].id;
            en_send_all(ENM_FX, &f, sizeof f);
            if (atomic_fetch_add(&n_fx_sent, 1) < 40) rt_log("coop: en: fx: %s's graph %s the held '%s': told to its copies", en_tab[ei].name, f.mode == 1 ? "starts" : "stops", f.key);
        }
        plat_mutex_unlock(&en_lock);
        return 0;
    }
    case 0xa80f28: {   // the auto placement asks for an enemy near this game's player (x3: a pointer to its name's pointer)
        // Each game places its own ambient enemies around its own player; two players together got them twice, each
        // game's as the other's copies too (research R5 Q1). Not asked for here while another game's enemy of the same
        // name already stands within 60 m of this game's Link.
        uint64_t np = c->x[3] > 0x10000 ? coop_rd_ptr(c->x[3]) : 0, link = coop_player_actor(), me = coop_ctr_me();
        char nm[48] = "";
        float lp[3];
        if (!np || !coop_rd(np, nm, sizeof nm - 1) || !link || !coop_rd(link + COOP_ACTOR_POS, lp, 12)) return 0;
        int near = 0;
        coop_lock_wait(&en_lock);
        for (int i = 0; i < EN_N && !near; i++) {
            En *e = &en_tab[i];
            if (!e->used || e->owner == me || !e->owner || e->life != EN_LIVE || strcmp(e->name, nm)) continue;
            float dx = e->pose[0] - lp[0], dz = e->pose[2] - lp[2];
            near = dx * dx + dz * dz < 60.f * 60.f;
        }
        plat_mutex_unlock(&en_lock);
        if (!near) return 0;
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: en: spawner: this game's auto placement of %s is not asked for: another game's stands within 60 m", nm);
        c->x[0] = 0;
        return 1;
    }
    case 0x21cdf44: {   // (x0 the weather manager, w1 the type, w2 reserve only, x3 the asking actor)
        uint64_t actor = c->x[3];
        EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
        if (m && atomic_load_explicit(&m->state, memory_order_relaxed) == EM_OWNED && !(c->x[2] & 1)) m->wx_type = (uint8_t)(1 + (c->x[1] & 0x7f)), m->wx_frame = coop_sess_frame_no();
        return 0;
    }
    case 0x1d71dfc: {   // (x0 the weapon's link, x1 the picker)
        uint64_t picker = c->x[1], link = c->x[0] > 0x10000 ? coop_rd_ptr(c->x[0]) : 0, ref = link ? coop_rd_ptr(link + 8) : 0, w = ref ? coop_rd_ptr(ref + 0x40) : 0;
        EnCell *m = picker > 0x10000 ? en_cell_of(picker) : NULL;
        MPick p;
        memset(&p, 0, sizeof p);
        if (!m || !w || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED || !coop_obj_ident(w, &p.obj_owner, &p.obj_id)) return 0;
        coop_actor_name(w, p.name, sizeof p.name);
        coop_lock_wait(&en_lock);
        int16_t ei = atomic_load(&m->en);
        if (ei >= 0 && en_tab[ei].used) {
            p.id = en_tab[ei].id;
            en_send_all(ENM_PICK, &p, sizeof p);
            if (atomic_fetch_add(&n_pick_sent, 1) < 20) rt_log("coop: en: pick: %s picks up %s (object %d/%u): told, so the other games' own lying one goes", en_tab[ei].name, p.name, p.obj_owner, p.obj_id);
        }
        plat_mutex_unlock(&en_lock);
        return 0;
    }
    }
    return 0;
}

// A message of this file's (from en_copy_msg). 1: it was one.
int en_fx_msg(const CoopCtrMsg *m) {
    uint64_t me = coop_ctr_me();
    if (m->type == ENM_FX) {
        const MFx *f = (const MFx *)m->d;
        if (m->len != (int)sizeof *f || !memchr(f->key, 0, sizeof f->key)) return 1;
        for (int i = 0; i < EN_N; i++) {
            if (!en_tab[i].used || !enid_eq(en_tab[i].id, f->id) || en_tab[i].owner == me || en_tab[i].cell < 0) continue;
            for (int k = 0; k < FXQ; k++) {
                int z = 0;
                if (!atomic_compare_exchange_strong(&fxq[k].cell, &z, -1)) continue;
                fxq[k].elink = f->elink, fxq[k].slink = f->slink, fxq[k].mode = f->mode, fxq[k].frame = coop_sess_frame_no();
                memcpy(fxq[k].key, f->key, sizeof f->key);
                atomic_store(&fxq[k].cell, en_tab[i].cell + 1);
                break;
            }
        }
        return 1;
    }
    if (m->type == ENM_PICK) {
        const MPick *p = (const MPick *)m->d;
        if (m->len != (int)sizeof *p || !memchr(p->name, 0, sizeof p->name)) return 1;
        for (int k = 0; k < 8; k++) {
            if (atomic_load(&pickq[k].frame)) continue;
            pickq[k].owner = p->obj_owner, pickq[k].id = p->obj_id, memcpy(pickq[k].name, p->name, sizeof p->name);
            atomic_store(&pickq[k].frame, coop_sess_frame_no() | 1);
            break;
        }
        return 1;
    }
    return 0;
}

// The copy's pre-calc: the emits stated for it, and its owner's weather request.
void en_fx_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    static __thread _Alignas(16) char key[48];
    static __thread _Alignas(16) uint8_t handle[16];
    int cell = (int)(m - en_map) + 1;
    uint32_t frame = coop_sess_frame_no();
    if (!fx_on()) return;
    for (int k = 0; k < FXQ; k++) {
        if (atomic_load_explicit(&fxq[k].cell, memory_order_relaxed) != cell) continue;
        uint64_t xl = coop_rd_ptr(en_comps(actor) + 0x68), eu = xl ? coop_rd_ptr(xl + 0x70) : 0, su = xl ? coop_rd_ptr(xl + 0x78) : 0;
        memcpy(key, fxq[k].key, sizeof key);
        if (fxq[k].mode && frame - fxq[k].frame < 600) {   // a held one: its handles are kept, or it is stopped
            int h = -1, fr = -1;
            for (int j = 0; j < 8; j++) { if (held[j].cell == cell && !strcmp(held[j].key, key)) h = j; else if (!held[j].cell && fr < 0) fr = j; }
            if (fxq[k].mode == 1 && h < 0 && fr >= 0) {
                memset(held[fr].pair, 0, 16), held[fr].pair[0] = held[fr].pair[8] = 0xff, held[fr].pair[2] = held[fr].pair[3] = held[fr].pair[10] = held[fr].pair[11] = 0xff;
                if (fxq[k].elink && eu) coop_guest_call(c, MOD_MAIN_BASE + 0xa98cf8, eu, (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)held[fr].pair, 0);
                if (fxq[k].slink && su) coop_guest_call(c, MOD_MAIN_BASE + 0xa98d00, su, (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)(held[fr].pair + 8), 0);
                held[fr].cell = cell, memcpy(held[fr].key, key, sizeof key);
                rt_log("coop: en: fx: the copy %llx starts the held '%s', as its owner's graph did", (unsigned long long)actor, key);
            } else if (fxq[k].mode == 2 && h >= 0) {
                coop_guest_call(c, MOD_MAIN_BASE + 0x6e34e8, (uint64_t)(uintptr_t)held[h].pair, (uint64_t)(uint32_t)-1, 0, 0);
                held[h].cell = 0;
                rt_log("coop: en: fx: the copy %llx stops the held '%s', as its owner's graph did", (unsigned long long)actor, key);
            }
        } else if (frame - fxq[k].frame < 60) {
            if (fxq[k].elink && eu) { memset(handle, 0, sizeof handle), handle[0] = 0xff, handle[2] = handle[3] = 0xff; coop_guest_call(c, MOD_MAIN_BASE + 0xa98cf8, eu, (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)handle, 0); }
            if (fxq[k].slink && su) { memset(handle, 0, sizeof handle), handle[0] = 0xff, handle[2] = handle[3] = 0xff; coop_guest_call(c, MOD_MAIN_BASE + 0xa98d00, su, (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)handle, 0); }
            if (atomic_fetch_add(&n_fx_made, 1) < 40) rt_log("coop: en: fx: the copy %llx starts '%s' (effect %u, sound %u), as its owner's graph did", (unsigned long long)actor, key, fxq[k].elink && eu, fxq[k].slink && su);
        }
        atomic_store(&fxq[k].cell, 0);
    }
    if (m->show_wx) {
        uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395dc20));
        if (mgr) coop_guest_call(c, MOD_MAIN_BASE + 0x21cdf44, mgr, (uint64_t)(m->show_wx - 1), 0, actor);
        if (mgr && !(atomic_fetch_add(&n_wx, 1) % 600)) rt_log("coop: en: fx: the copy %llx asks the weather its owner's Wizzrobe asks (type %u)", (unsigned long long)actor, m->show_wx - 1);
    }
}

// Phase 0: a weapon another game's enemy picked up is taken off this game's ground.
void en_fx_phase0(Cpu *c) {
    uint32_t frame = coop_sess_frame_no();
    for (int k = 0; k < 8; k++) {
        uint32_t f = atomic_load(&pickq[k].frame);
        if (!f) continue;
        uint64_t a = coop_obj_find(pickq[k].owner, pickq[k].id, pickq[k].name);
        int32_t st = 0;
        if (a && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6) {
            uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, 0, 0, 0);
            atomic_fetch_add(&n_pick_done, 1);
            rt_log("coop: en: pick: this game's %s (object %d/%u) is deleted: another game's enemy picked it up (the answer %llu)", pickq[k].name, pickq[k].owner, pickq[k].id, (unsigned long long)(r & 1));
            atomic_store(&pickq[k].frame, 0);
        } else if (frame - f > 90) atomic_store(&pickq[k].frame, 0);   // (not here, or gone already)
    }
}
