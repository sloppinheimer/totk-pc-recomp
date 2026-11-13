// Co-op: entities, the identity of actors and the events of the hooks (docs/coop/contraption-sync-v4-spec.md sections
// 4 and 10, cited as S).
//
// A Zonai device the game makes here gets its Eid where the game marks the actor created: the create hook decides
// what the request is (ours, a map actor, an Autobuild part, a capsule opening, another creator's) and remembers it
// by the request's pool record; the hook on the function that marks an actor created finds that record on the new
// actor and posts the decision to the ring. Any other actor becomes an entity when it is an end of a glue, and takes
// its Eid from the name the object mirror has for it, so two games that enter one thing mint one entity.
// The hooks post what they see (creates, deletes, deaths, glue, grabs, hits); phase 0 turns the events into state.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_game.h"
#include "coop_net.h"
#include "dispatch.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);
int coop_obj_ident(uint64_t a, int *owner, uint32_t *id);        // coop_players.c (the tick's tables)
uint64_t coop_obj_find(int owner, uint32_t id, const char *name);
int coop_is_copy(uint64_t a);
int coop_obj_made_here(uint64_t actor);                           // the mirror's own test of "made by this game"
void coop_obj_entity(uint64_t actor);
int coop_actor_placed(uint64_t a);                                // coop_world.c: an actor of the map's placement
uint32_t coop_actor_world_key(uint64_t a);
int ent_ring_take(Ev *out);
void ent_held_hint(uint64_t actor);
int coop_ent_strand_hook(Cpu *c, uint64_t off);

// ---- The create hooks (S 4.2) ----------------------------------------------------------------------------------------------------
static struct { _Atomic uint64_t r; _Atomic uint32_t info; } pend[PEND_N];   // pool record -> decision | batch << 8
static __thread int creating;
void coop_ent_creating(int on) { creating = on; }   // set around every create this code asks (coop_game.c, the maker)
static _Atomic uint32_t stand_in_capsules, pend_full;

// A row is claimed by a compare-exchange to a reserved value, so two threads never take one free row; its decision is
// stored before its record. Rows older than 10 s are free again (a request that failed never reaches the second hook).
static int64_t pend_ms[PEND_N];
static void pend_set(uint64_t r, uint32_t info) {
    int64_t now = ent_ms();
    for (int i = 0; i < PEND_N; i++) {
        uint64_t have = atomic_load_explicit(&pend[i].r, memory_order_relaxed);
        if (have != r) continue;
        if (info) atomic_store(&pend[i].info, info), pend_ms[i] = now;
        else atomic_store(&pend[i].r, 0);
        return;
    }
    if (!info) return;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < PEND_N; i++) {
            uint64_t have = atomic_load(&pend[i].r);
            if (have == 1 || (have && (!pass || now - pend_ms[i] < 10000))) continue;   // (pass 1 takes a row left over for 10 s)
            if (!atomic_compare_exchange_strong(&pend[i].r, &have, 1)) continue;
            atomic_store(&pend[i].info, info), pend_ms[i] = now;
            atomic_store(&pend[i].r, r);
            return;
        }
    atomic_fetch_add(&pend_full, 1);
}
// main+0x599670, the asynchronous create request: (mgr, &name, arg, handle x3, lane, placement record x5, ...). It is
// run inside the hook; afterwards [x3] is the pool record R the request made.
int coop_ent_create_hook(Cpu *c, uint64_t addr) {
    static __thread int inner;
    if (inner) { inner = 0; return 0; }
    if (!ent_on()) return 0;
    char nm[64] = "";
    uint64_t np = coop_rd_ptr(c->x[1]), x3 = c->x[3], x5 = c->x[5], lr = c->x[30] - MOD_MAIN_BASE;
    int ours = creating;
    if (np) coop_rd(np, nm, sizeof nm - 1);
    uint32_t dec = DEC_NONE;
    static _Atomic uint32_t batch = 1;
    static _Atomic int64_t batch_ms;
    if (ours || x5) dec = DEC_NONE;   // (ours: bound by its handle, S 4.1; a placement record: a map actor)
    else if (lr == 0x1d3bb98) {       // the Autobuild part maker: one batch per maker run
        int64_t now = ent_ms();
        if (now - atomic_exchange(&batch_ms, now) > 500) atomic_fetch_add(&batch, 1);
        dec = DEC_MINT_PREVIEW | (atomic_load(&batch) & 0xff) << 8;
    } else if (strncmp(nm, "SpObj_", 6) || strstr(nm, "Capsule")) dec = DEC_NONE;
    else if (lr == 0x1b7f8c8) {       // a capsule opening into its device: its Player is [(x3 - 0x3e88) +0x18]
        if (coop_rd_ptr(x3 - 0x3e88 + 0x18) == coop_player_actor()) dec = DEC_MINT;
        else atomic_fetch_add(&stand_in_capsules, 1);
    } else dec = DEC_ASK_MIRROR;
    inner = 1;
    c->pc = addr;
    rt_dispatch(c);
    uint64_t r = x3 && (c->x[0] & 1) ? coop_rd_ptr(x3) : 0;
    if (r) pend_set(r, dec);
    return 1;
}
// main+0x54cb00(actor): the game marks an actor created here (the pool record R at actor +0x1b8 gets status 3 and the
// actor at R +8). Run inside the hook, then the record is looked up. The read before the wrap is a plain load: the
// function runs for every actor in the making, and x0 is the actor it was called for.
int coop_ent_created_hook(Cpu *c, uint64_t addr) {
    static __thread int inner;
    if (inner) { inner = 0; return 0; }
    uint64_t actor = c->x[0];
    if (!actor || !*(uint64_t *)(uintptr_t)(actor + 0x1b8) || !ent_on()) return 0;
    inner = 1;
    c->pc = addr;
    rt_dispatch(c);
    uint64_t r = *(uint64_t *)(uintptr_t)(actor + 0x1b8);
    if (!r || (*(uint32_t *)(uintptr_t)r & 0xffff) != 3 || *(uint64_t *)(uintptr_t)(r + 8) != actor) return 1;
    for (int i = 0; i < PEND_N; i++) {
        if (atomic_load_explicit(&pend[i].r, memory_order_relaxed) != r) continue;
        uint32_t info = atomic_load(&pend[i].info);
        atomic_store(&pend[i].r, 0);
        Ev ev = {0};
        ev.kind = EV_CREATED, ev.actor = actor, ev.gen = ctn_gen_of(actor), ev.decision = (uint8_t)(info & 0xff), ev.batch = (uint8_t)(info >> 8);
        if (ev.decision) ent_post(&ev);
        break;
    }
    return 1;
}
// main+0xa1bc30, the synchronous create (placement record in x3): none of its callers read makes a device; a device
// name through it is logged.
int coop_ent_create_sync_hook(Cpu *c) {
    char nm[64] = "";
    uint64_t np = coop_rd_ptr(c->x[1]);
    static _Atomic int said;
    if (!ent_on() || c->x[3] || !np || !coop_rd(np, nm, sizeof nm - 1) || strncmp(nm, "SpObj_", 6)) return 0;
    if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: ent: a device (%s) made by the synchronous create from main+%#llx: not given an identity there", nm, (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
    return 0;
}

// ---- The delete and death hooks (S 10.3, 10.4) ------------------------------------------------------------------------------------
// main+0x63baa8(actor, reason), for an entity's actor only: run inside the hook and posted when it answered 1 (an
// accepted request; later ones answer 0), with the reason, the return address and the load flag as read then.
int coop_ent_delete_hook(Cpu *c, uint64_t addr) {
    static __thread int inner;
    if (inner) { inner = 0; return 0; }
    uint64_t actor = c->x[0];
    if (!ent_set_has(ENT_SET_ENT, actor)) return 0;
    Ev ev = {0};
    ev.kind = EV_DELETE, ev.actor = actor, ev.gen = ctn_gen_of(actor), ev.reason = (uint32_t)c->x[1], ev.lr = c->x[30] - MOD_MAIN_BASE;
    ev.flag = (uint8_t)((coop_ent_ours_delete() ? 1 : 0) | (ent_loading() ? 2 : 0));
    inner = 1;
    c->pc = addr;
    rt_dispatch(c);
    if (c->x[0] & 1) ent_post(&ev);
    return 1;
}
void coop_ent_death_seen(uint64_t actor, uint32_t kind) {   // an accepted death request (coop_life.c, after G4)
    if (!ent_set_has(ENT_SET_OWNED, actor)) return;
    Ev ev = {0};
    ev.kind = EV_DEATH, ev.actor = actor, ev.gen = ctn_gen_of(actor), ev.reason = kind;
    ent_post(&ev);
}
void coop_ent_glue_made(uint64_t ba, uint64_t bb, uint64_t edge) {   // the bond maker's wrap, a non-zero edge (coop_life.c)
    Ev ev = {0};
    ev.kind = EV_GLUE, ev.actor = ba, ev.a2 = bb, ev.edge = edge;
    ent_post(&ev);
}
void coop_ent_structure_event(void) {   // a bond made, an edge removed (coop_life.c)
    static _Atomic int64_t last;
    int64_t now = ent_ms();
    if (now - atomic_exchange(&last, now) < 5) return;   // (many edges of one change: one event)
    Ev ev = {0};
    ev.kind = EV_STRUCT;
    ent_post(&ev);
}
void coop_ent_grab_seen(uint64_t spr, uint64_t holder) {   // main+0x1c0b83c: an Ultrahand took hold
    Ev ev = {0};
    ev.kind = EV_GRAB, ev.actor = spr, ev.a2 = holder;
    ent_post(&ev);
}
void coop_ent_toggle_seen(uint64_t actor, int want) {   // a hit here on a device another game owns: the state wanted
    Ev ev = {0};
    ev.kind = EV_TOGGLE, ev.actor = actor, ev.gen = ctn_gen_of(actor), ev.flag = (uint8_t)want;
    ent_post(&ev);
}
void coop_ent_switch_seen(uint64_t actor, uint8_t bits) {   // a switch event put on such a device (a jump attack)
    Ev ev = {0};
    ev.kind = EV_SWITCH, ev.actor = actor, ev.gen = ctn_gen_of(actor), ev.flag = bits;
    ent_post(&ev);
}
// The seat claim main+0x502e60(rider component, stick handle, mode, seat): refused for this Link on a stick of an
// island whose owner holds or rides it (S 5.6). Every refusal of the game's own returns before any write.
int coop_ent_seat_hook(Cpu *c) {
    uint64_t rider = c->x[0], h = c->x[1];
    if (!rider || !h || coop_rd_ptr(rider + 0x18) != coop_player_actor()) return 0;
    uint64_t slot = coop_rd_ptr(h + 8), stick = 0;
    uint32_t id = 0, sid = 0;
    if (slot && coop_rd(h + 0x10, &id, 4) && id != ~0u && coop_rd(slot + 0x48, &sid, 4) && sid == id) stick = coop_rd_ptr(slot + 0x40);
    if (!ent_set_has(ENT_SET_REFUSE_STICK, stick)) return 0;
    static _Atomic int64_t said;
    int64_t now = ent_ms();
    if (now - atomic_exchange(&said, now) > 2000) rt_log("coop: ent: this Link may not take a stick its owner holds or rides: the seat is refused");
    c->x[0] = 0;
    return 1;
}
// The new hooks of this file and of coop_ent_rec.c, from coop_spawn.c coop_hook.
int coop_ent_hook(Cpu *c, uint64_t addr) {
    uint64_t off = addr - MOD_MAIN_BASE;
    if (off == 0x54cb00) return coop_ent_created_hook(c, addr);
    if (off == 0x599670) return coop_ent_create_hook(c, addr);
    if (off == 0xa1bc30) return coop_ent_create_sync_hook(c);
    if (off == 0x502e60) return coop_ent_seat_hook(c);
    if (off == 0x190f034 || off == 0x191642c || off == 0x190d924) return ent_on() && coop_ent_strand_hook(c, off);
    return 0;
}

// ---- The mirror asked (S 4.4): phase 0 asks, the co-op tick answers a tick later ----------------------------------------------------
#define ENTER_N 32
static struct {
    uint64_t actor; uint32_t gen; uint8_t kind, answered, has_pair, is_copy, made_here; int owner; uint32_t id; int64_t ms;
} enter[ENTER_N];   // kind 1: an end of a glue; 2: a device another creator made (ASK_MIRROR)
static void enter_ask(uint64_t actor, uint8_t kind) {
    int f = -1;
    for (int i = 0; i < ENTER_N; i++) {
        if (enter[i].actor == actor) return;
        if (f < 0 && !enter[i].actor) f = i;
    }
    if (f < 0) { ent_full("mirror question"); return; }
    memset(&enter[f], 0, sizeof enter[f]);
    enter[f].actor = actor, enter[f].gen = ctn_gen_of(actor), enter[f].kind = kind, enter[f].ms = ent_ms();
}
void ent_enter_glued(Cpu *c, uint64_t actor) {
    (void)c;
    char nm[48];
    if (!actor || !ctn_alive(actor) || !ctn_spr(actor) || ent_by_actor(actor)) return;
    coop_actor_name(actor, nm, sizeof nm);
    if (!strncmp(nm, "Player", 6)) return;
    enter_ask(actor, 1);
}
// On the co-op tick, where the mirror's tables live.
void coop_ent_mirror_tick(void) {
    if (!ent_on()) return;
    coop_lock_wait(&ent_lock);
    for (int i = 0; i < ENTER_N; i++) {
        if (!enter[i].actor || enter[i].answered) continue;
        if (enter[i].kind == 3) { coop_obj_entity(enter[i].actor), enter[i].answered = 1; continue; }   // (an actor minted at its create: the mirror forgets it)
        if (enter[i].kind == 2) {   // (the mirror takes some ticks to call an object made here: asked for 2 s)
            enter[i].made_here = (uint8_t)(coop_obj_made_here(enter[i].actor) != 0);
            if (!enter[i].made_here && ent_ms() - enter[i].ms < 2000) continue;
        }
        else {
            enter[i].has_pair = (uint8_t)(coop_obj_ident(enter[i].actor, &enter[i].owner, &enter[i].id) != 0);
            enter[i].is_copy = (uint8_t)(coop_is_copy(enter[i].actor) != 0);
        }
        if (enter[i].kind == 1 || enter[i].made_here) coop_obj_entity(enter[i].actor);
        enter[i].answered = 1;
    }
    for (int i = 0; i < ENT_N; i++) {   // a pair received: the actor the mirror has for it
        Ent *e = &ents[i];
        if (!e->used || !e->mir_id || e->mir_resolved || e->actor) continue;
        uint64_t a = coop_obj_find(e->mir_owner, e->mir_id, e->name);
        if (!a && ent_ms() - e->entered_ms < 1000) continue;   // (asked again until 1 s has passed: a keyed scan runs once a second)
        e->mir_actor = a, e->mir_copy = (uint8_t)(a && coop_is_copy(a)), e->mir_resolved = 1;
        if (a) coop_obj_entity(a);
    }
    plat_mutex_unlock(&ent_lock);
}
static void entered(uint64_t actor, uint32_t gen, int has_pair, int owner, uint32_t id, int is_copy) {
    uint64_t me = coop_ctr_me();
    Eid eid;
    uint8_t flags = 0;
    if (!me || !ctn_alive_as(actor, gen) || ent_by_actor(actor)) return;
    int map = coop_actor_placed(actor);
    if (map) {   // S 4.3: the key the mirror holds for it, else its world key; computed once, stored
        uint32_t key = has_pair && (id & COOP_OBJ_KEYED) ? id & ~COOP_OBJ_KEYED : coop_actor_world_key(actor) & ~COOP_OBJ_KEYED;
        eid = (Eid){0, key}, flags = ENT_MAP, id = COOP_OBJ_KEYED | key, has_pair = 1, owner = 0;
    } else if (has_pair && !(id & COOP_OBJ_KEYED)) {
        uint64_t oinc = coop_ctr_incarnation(owner);
        if (!oinc) oinc = me;
        eid = (Eid){oinc, 0x80000000u | id};
        if (is_copy) flags = ENT_COPY;
    } else eid = (Eid){me, ent_mint_serial()}, has_pair = 0;
    Ent *e = ent_new(eid);
    if (!e) return;
    ent_bind(e, actor);
    e->flags |= flags, e->mir_known = 1;
    if (has_pair) e->mir_owner = (uint8_t)owner, e->mir_id = id, e->mir_resolved = 1, e->mir_actor = actor;
    if (!e->owner || (e->owner != me && !e->epoch)) e->owner = me, e->epoch = 1;   // (a row another game stated first keeps its owner)
    if (e->life == LIFE_FREE) e->life = LIFE_LIVE;
    ent_mark_due(e);
    char b[24];
    rt_log("coop: ent: %lld %s (%s) is an entity now: an end of a glue (%s)", (long long)ent_ms(), ent_eid_str(e->id, b), e->name,
           map ? "a map object" : has_pair ? (is_copy ? "the mirror's copy of another player's object" : "an object of this game the mirror sent") : "known to no one else");
}

// ---- Step 2: the ring (S 10) ------------------------------------------------------------------------------------------------------------
static struct { uint64_t actor; uint32_t kind; int64_t ms; } died[64];
static int died_i;
static int died_kind(uint64_t actor, uint32_t *kind) {
    for (int i = 0; i < 64; i++) if (died[i].actor == actor && ent_ms() - died[i].ms < 10000) { *kind = died[i].kind; return 1; }
    return 0;
}
static Ent *ent_by_actor_raw(uint64_t actor) {   // by pointer alone: the actor of a delete event may be past its states already
    for (int i = 0; actor && i < ENT_N; i++) if (ents[i].used && ents[i].actor == actor) return &ents[i];
    return NULL;
}
void ent_ring_events(Cpu *c) {
    Ev ev;
    uint64_t me = coop_ctr_me();
    int64_t now = ent_ms();
    char b[24];
    static uint32_t lost_seen;
    uint32_t ent_ring_lost(void);
    if (ent_ring_lost() != lost_seen) {   // an overflow: every owned entity is read again by step 3 (S 10)
        lost_seen = ent_ring_lost();
        rt_log("coop: ent: the event ring overflowed (%u events lost so far): the world is read again", lost_seen);
        ent_mark_due(&ents[0]);
    }
    // the mirror's answers
    for (int i = 0; i < ENTER_N; i++) {
        if (!enter[i].actor) continue;
        if (!enter[i].answered) { if (now - enter[i].ms > 5000) enter[i].actor = 0; continue; }
        if (enter[i].kind == 1) entered(enter[i].actor, enter[i].gen, enter[i].has_pair, enter[i].owner, enter[i].id, enter[i].is_copy);
        else if (enter[i].kind == 3) { }
        else if (enter[i].made_here && me && ctn_alive_as(enter[i].actor, enter[i].gen) && !ent_by_actor(enter[i].actor)) {
            Ent *e = ent_new((Eid){me, ent_mint_serial()});
            if (e) ent_bind(e, enter[i].actor), e->owner = me, e->epoch = 1, e->mir_known = 1, enter[i].kind = 3, enter[i].answered = 0;
            if (e) continue;   // (kept one more tick: the mirror frees its row for it)
        }
        enter[i].actor = 0;
    }
    while (ent_ring_take(&ev)) {
        switch (ev.kind) {
        case EV_CREATED: {
            if (!me || !ctn_alive_as(ev.actor, ev.gen) || ent_by_actor(ev.actor)) break;
            if (ev.decision == DEC_ASK_MIRROR) { enter_ask(ev.actor, 2); break; }
            Ent *e = ent_new((Eid){me, ent_mint_serial()});
            if (!e) break;
            ent_bind(e, ev.actor);
            e->owner = me, e->epoch = 1, e->mir_known = 1;
            if (ev.decision == DEC_MINT_PREVIEW) e->flags |= ENT_PREVIEW, e->batch = ev.batch;
            enter_ask(ev.actor, 3);   // (the mirror may have taken it as an object in the frame before: it forgets it)
            if (ctn_part_green(ev.actor)) e->flags |= ENT_GREEN;
            rt_log("coop: ent: %lld %s (%s) made here%s", (long long)now, ent_eid_str(e->id, b), e->name, ev.decision == DEC_MINT_PREVIEW ? " by an Autobuild (a preview until it is confirmed)" : "");
            break;
        }
        case EV_GLUE: rec_glue_made(c, &ev); break;
        case EV_STRUCT: ent_mark_due(&ents[0]); break;
        case EV_DEATH: {
            died[died_i % 64].actor = ev.actor, died[died_i % 64].kind = ev.reason, died[died_i++ % 64].ms = now;
            Ent *e = ent_by_actor_raw(ev.actor);
            static struct { Eid e; uint32_t kind; int64_t ms; } said[16];
            static int said_i;
            int again = 0;
            if (!e || e->owner != me || e->life != LIFE_LIVE || (e->flags & ENT_PREVIEW)) break;
            for (int i = 0; i < 16; i++) again |= eid_eq(said[i].e, e->id) && said[i].kind == ev.reason && now - said[i].ms < 3000;
            if (again) break;   // (some rules ask every frame: once per entity and kind in 3 s)
            said[said_i % 16].e = e->id, said[said_i % 16].kind = ev.reason, said[said_i++ % 16].ms = now;
            own_death(e, (uint8_t)ev.reason);   // the other games replay it on their copies
            break;
        }
        case EV_DELETE: {
            Ent *e = ent_by_actor_raw(ev.actor);
            uint32_t kind = 0xff;
            if (!e || e->gen != ev.gen) break;
            if (ev.flag & 1) break;                                   // a delete this code asked
            if (e->flags & ENT_PREVIEW) { ent_drop(e); break; }       // a cancelled hologram: nothing is sent (S 4.6)
            if (e->owner != me || e->life != LIFE_LIVE) break;        // a copy's going is local (S 5.8)
            int dead = died_kind(ev.actor, &kind);
            uint64_t lr = ev.lr;
            // Reason 0 is the game's "delete this actor": every death procedure (straight, through the deferring wrapper
            // or its later runner), the lapse and green-kill fallbacks, a pick-up. The unloads have their own reasons
            // (3, 4, 5, 6, 7, 10). So reason 0 is a destroy, but for a refused creation (from main+0x54db94).
            (void)dead;
            int destroy = ev.reason == 0 || ev.reason == 2;
            if (ev.reason == 2) kind = 0xff;
            if (ev.reason == 0 && lr == 0x54db94) break;              // a refused creation
            if (ev.flag & 2) destroy = 0;                             // the teardown of a load: an unload
            if (destroy) {
                rt_log("coop: ent: %lld %s (%s) destroyed here (reason %u from main+%#llx, death kind %u)", (long long)now, ent_eid_str(e->id, b), e->name, ev.reason, (unsigned long long)lr, kind);
                e->life = LIFE_DEAD, e->death_kind = (uint8_t)kind, e->dead_ms = now;
                own_owe_ent(e, -1);
                ent_mark_due(e);
            } else {
                rt_log("coop: ent: %lld %s (%s) unloads here (reason %u from main+%#llx%s): offered", (long long)now, ent_eid_str(e->id, b), e->name, ev.reason,
                       (unsigned long long)lr, ev.flag & 2 ? ", loading" : "");
                e->unload_ms = now;
                own_unload(c, e);
            }
            break;
        }
        case EV_GRAB:
            // (the SPR's actor, found among the entities; whether this Link is the holder is read at once after, by
            // coop_held_here. A thing that is no entity is the mirror's.)
            for (int i = 0; i < ENT_N; i++) if (ents[i].used && ents[i].actor && ctn_spr(ents[i].actor) == ev.actor) { ent_held_hint(ents[i].actor); break; }
            break;
        case EV_TOGGLE: case EV_SWITCH: {
            Ent *e = ent_by_actor(ev.actor);
            if (e && e->owner != me && e->owner) own_req(e, ev.kind == EV_TOGGLE ? 1 : 2, ev.flag);
            break;
        }
        default: break;
        }
    }
    // S 4.6: a preview is cleared for a whole maker run together
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || !(e->flags & ENT_PREVIEW)) continue;
        int ready = now - e->entered_ms > 1000;
        for (int k = 0; k < ENT_N && !ready; k++) {
            Ent *o = &ents[k];
            if (!o->used || !(o->flags & ENT_PREVIEW) || o->batch != e->batch) continue;
            uint64_t spr = o->actor ? ctn_spr(o->actor) : 0, f20 = 0;
            int32_t queued = 0;
            if (!spr || !coop_rd(spr + 0x20, &f20, 8) || (f20 >> 51 & 1) || !coop_rd(spr + 0x2b0, &queued, 4) || queued) { ready = -1; break; }
        }
        if (ready < 0) continue;
        uint8_t batch = e->batch;
        int cleared = 0;
        for (int k = 0; k < ENT_N; k++) if (ents[k].used && (ents[k].flags & ENT_PREVIEW) && ents[k].batch == batch) {
            uint64_t spr = ents[k].actor ? ctn_spr(ents[k].actor) : 0, f20 = 0;
            if (spr && coop_rd(spr + 0x20, &f20, 8) && (f20 >> 51 & 1)) continue;   // (still a hologram after 1 s: left a preview)
            ents[k].flags &= (uint8_t)~ENT_PREVIEW, cleared = 1;
            if (ents[k].actor && ctn_part_green(ents[k].actor)) ents[k].flags |= ENT_GREEN;
        }
        if (cleared) ent_mark_due(e);
    }
}

// ---- Step 4: creates of copies that completed (S 4.1) ------------------------------------------------------------------------------------
void ent_creates(Cpu *c) {
    int64_t now = ent_ms();
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || !e->making) continue;
        if (ent_loading()) { coop_handle_release(c, e->handle), e->making = 0; continue; }
        int st = coop_handle_status(e->handle);
        if (st == 3) {
            uint64_t ref = 0, a = coop_claim(c, e->handle, e->link, &ref);
            e->making = 0;
            if (a) ent_bind(e, a), e->flags |= ENT_COPY, ent_mark_due(e);
            else e->death_ms = now;   // (the claim gave no actor: not asked again for 5 s)
        } else if (st == 4 || now - e->make_ms > 10000) {
            char b[24];
            rt_log("coop: ent: %lld the copy of %s (%s) was not made (%s): tried again after a wait", (long long)now, ent_eid_str(e->id, b), e->name, st == 4 ? "the create failed" : "no answer in 10 s");
            coop_handle_release(c, e->handle), e->making = 0, e->death_ms = now, e->why = WHY_CREATE_FAILED;
        }
    }
}
