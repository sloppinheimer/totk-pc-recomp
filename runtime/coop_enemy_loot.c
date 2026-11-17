// What a dying enemy leaves, in every game once (docs/coop/enemy-sync-spec.md B.9; research R13, G8).
//
// A copy rolls and makes no drops (both the reserve and the release of its death sit under the death component's
// "no drops" bit, which a copy has), so loot is made in the owner's game alone, by main+0x1c78060: one create request
// (main+0x599670) per item with the dying enemy as creator. The object mirror (coop_players.c prop_fn) sent a new loose
// object only when it first saw it within 6 m of this Link and nearer it than any stand-in, and never a Weapon_ within
// 3 m of an enemy: loot of a kill far from the owner's Link, or by the other player's blow, and every weapon an enemy
// let fall, stayed in the owner's game alone. Here the owner's game notes each item as it is asked for, and each weapon
// an enemy it runs lets go of (the drop request main+0x1b738e4(equipment user, request, slot)), and the mirror takes
// the noted ones as this game's made objects wherever they are.
//
// The kill counters: a copy's own death record arms the counters' writer (main+0xb28394(record)) as the owner's does,
// so both games added one to DefeatedNum and the like, and the shared value went up by two. On a copy the record's
// "done" bit (byte +0x30 bit 3) is set at the writer's entry, which then does nothing.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

void rt_dispatch(Cpu *c);
uint64_t coop_enemy_holder(uint64_t actor);   // coop_enemy_copy.c
int coop_enemy_role(uint64_t actor);          // coop_enemy.c
const char *coop_actor_name(uint64_t a, char *out, size_t n);

#define LOOT_N 48
static struct { char name[48]; _Atomic uint32_t frame; float at[3]; } pend[LOOT_N];   // items asked for by a drop, by name, and where the dying enemy was
static struct { _Atomic uint64_t actor; _Atomic uint32_t frame; } let_go[LOOT_N];   // weapons an owned enemy let go of
static __thread int tl_in_drop;
static _Atomic uint32_t n_items, n_weapons, n_taken, n_counters;

// main+0x599670's entry (coop_spawn.c): inside a drop's item loop the name asked for is noted.
void coop_enemy_loot_create(Cpu *c) {
    if (!tl_in_drop) return;
    uint64_t np = coop_rd_ptr(c->x[1]);
    char nm[48] = "";
    if (!np || !coop_rd(np, nm, sizeof nm - 1) || !nm[0]) return;
    uint32_t frame = coop_sess_frame_no();
    for (int i = 0; i < LOOT_N; i++) {
        uint32_t f = atomic_load(&pend[i].frame);
        if (f && frame - f < 300) continue;
        {   // (where: the dying enemy of this game's nearest... any one dying now; a drop is made at its enemy's place)
            uint64_t me = coop_ctr_me();
            memset(pend[i].at, 0, 12);
            for (int k = 0; k < EN_N; k++)
                if (en_tab[k].used && en_tab[k].owner == me && en_tab[k].life == EN_DYING) { memcpy(pend[i].at, en_tab[k].pose, 12); break; }
        }
        memcpy(pend[i].name, nm, sizeof nm), atomic_store(&pend[i].frame, frame ? frame : 1);
        if (atomic_fetch_add(&n_items, 1) < 40) rt_log("coop: en: loot: a dying enemy's drop asks for %s: this game's, wherever it lands", nm);
        return;
    }
}

// From en_copy_hook. 1: the function was run here.
int en_loot_hook(Cpu *c, uint64_t off) {
    switch (off) {
    case 0x1c78060: {   // a drop's items are made (only ever for an enemy that is no copy)
        static __thread int deep;
        if (deep) return 0;
        deep = 1, tl_in_drop = 1, c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        deep = 0, tl_in_drop = 0;
        return 1;
    }
    case 0x1b73be4:     // (the same arguments: a throw)
    case 0x1b738e4: {   // (x0 the equipment user, x1 the request, w2 the slot): an enemy this game runs lets go of what it holds
        uint64_t eu = c->x[0], ref = eu > 0x10000 && (uint32_t)c->x[2] < EN_EQ_N ? coop_rd_ptr(eu + 0x20 + (c->x[2] & 0xff) * 0x18 + 8) : 0, w = ref ? coop_rd_ptr(ref + 0x40) : 0;
        uint64_t holder = w ? coop_enemy_holder(w) : 0;
        if (!w || !holder || coop_enemy_role(holder) != 1) return 0;
        uint32_t frame = coop_sess_frame_no();
        for (int i = 0; i < LOOT_N; i++) {
            uint32_t f = atomic_load(&let_go[i].frame);
            if (atomic_load(&let_go[i].actor) && f && frame - f < 300) continue;
            atomic_store(&let_go[i].actor, w), atomic_store(&let_go[i].frame, frame ? frame : 1);
            char wn[48], hn[48];
            if (atomic_fetch_add(&n_weapons, 1) < 40) rt_log("coop: en: loot: %s %s its %s (slot %u): a loose object of this game's from here", coop_actor_name(holder, hn, sizeof hn), off == 0x1b73be4 ? "throws" : "lets go of", coop_actor_name(w, wn, sizeof wn), (uint32_t)c->x[2]);
            break;
        }
        {   // the copies let go at once, not after the three looks of the held-things rule (0.8 s)
            void en_equip_event(uint64_t holder, int slot);   // coop_enemy_copy.c
            en_equip_event(holder, (int)(c->x[2] & 0xff));
        }
        return 0;
    }
    case 0xb28394: {   // the kill counters' writer (x0 the record, its actor at +0x18): not for a copy
        uint64_t rec = c->x[0], actor = rec > 0x10000 ? coop_rd_ptr(rec + 0x18) : 0;
        uint8_t b = 0;
        if (!actor || coop_enemy_role(actor) != 2 || !coop_rd(rec + 0x30, &b, 1) || (b & 8)) return 0;
        b |= 8;
        memcpy((void *)(uintptr_t)(rec + 0x30), &b, 1);
        char an[48];
        if (atomic_fetch_add(&n_counters, 1) < 20) rt_log("coop: en: loot: the kill counters of the copy %s are left to its owner's game", coop_actor_name(actor, an, sizeof an));
        return 0;
    }
    }
    return 0;
}

// The object mirror asks of a loose object it looks at: 1 when it is a noted drop of this game's (taken once).
int coop_enemy_loot_take(uint64_t a, const char *nm, int first) {
    uint32_t frame = coop_sess_frame_no();
    if (!coop_enemy_new()) return 0;
    for (int i = 0; i < LOOT_N; i++) {
        if (atomic_load(&let_go[i].actor) != a) continue;
        uint32_t f = atomic_load(&let_go[i].frame);
        if (frame - f > 300) { atomic_store(&let_go[i].actor, 0); return 0; }
        if (coop_enemy_holder(a) && frame - f < 20) return 0;   // (still listed as held: asked again at the next look)
        atomic_store(&let_go[i].actor, 0), atomic_fetch_add(&n_taken, 1);
        return 1;
    }
    // (not only at the first look: a reserved item is made asleep at the death's start and woken at its end, and the
    // scan had seen it by then. One of the name within 15 m of where the enemy died, once.)
    (void)first;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return 0;
    for (int i = 0; i < LOOT_N; i++) {
        uint32_t f = atomic_load(&pend[i].frame);
        if (!f || frame - f > 300 || strncmp(pend[i].name, nm, sizeof pend[i].name)) continue;
        float dx = p[0] - pend[i].at[0], dy = p[1] - pend[i].at[1], dz = p[2] - pend[i].at[2];
        if ((pend[i].at[0] || pend[i].at[2]) && dx * dx + dy * dy + dz * dz > 15.f * 15.f) continue;
        atomic_store(&pend[i].frame, 0), atomic_fetch_add(&n_taken, 1);
        return 1;
    }
    return 0;
}
