// Co-op: entities, reconcile and bonds (docs/coop/entity-sync.md sections 7 and 8, cited as S).
//
// A game that does not own an island compares its world with the owner's statement and makes them equal: it creates
// what is missing, replays deaths, breaks glue edges the statement lacks and makes stated bonds that have no edge.
// Nothing is assumed: each pass reads the world again, and a pass that acted and still differs climbs a ladder that
// ends in "not shown here". A follower's failure never changes the owner's world.
// A glue between two owners (S 8) is an operation: the gluing game keeps its edge, asks the other end's owner to
// make the bond, and that owner answers and leaves the merged island with one owner.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_ent_msg.h"
#include "coop_game.h"
#include "host.h"
#include "layout.h"

Op ops[OP_N];
static Serve serves[SERVE_N];
static uint32_t op_next = 1;
static struct { uint64_t from; uint32_t op; uint8_t ack, reason; uint64_t who; uint32_t epoch; int64_t ms; } bond_mem[16];
static struct { uint64_t edge; int64_t ms; int tries; char why[40]; } breaking[16];   // breaks being read back

// ---- Small things ---------------------------------------------------------------------------------------------------------------
static float m34_dist(const float a[12], const float b[12]) {
    float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static float m34_angle_deg(const float a[12], const float b[12]) {   // the angle between two rotations
    float tr = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) tr += a[i * 4 + j] * b[i * 4 + j];
    float cs = (tr - 1) / 2;
    cs = cs < -1 ? -1 : cs > 1 ? 1 : cs;
    return acosf(cs) * 57.29578f;
}
static int body_mat(Cpu *c, Ent *e, uint8_t key, uint64_t *body, float m[12]) {
    uint64_t b = key ? ctn_body_by_key(c, e->actor, key) : e->main_body ? e->main_body : ctn_main_body(e->actor);
    if (body) *body = b;
    return b && coop_rd(b + 0x98, m, 48);
}
static const Bond *my_bond(Eid a, Eid b, uint8_t sa, uint8_t sb) {
    for (int i = 0; i < bond_n; i++) if (eid_eq(bonds[i].a, a) && eid_eq(bonds[i].b, b) && bonds[i].sa == sa && bonds[i].sb == sb) return &bonds[i];
    return NULL;
}
static int stated_lists(uint64_t owner, const Bond *b) {   // a statement from that owner lists the bond
    for (int i = 0; i < STATED_N; i++) {
        if (!stated[i].used || stated[i].from != owner) continue;
        for (int k = 0; k < stated[i].nb; k++)
            if (eid_eq(stated[i].bond[k].a, b->a) && eid_eq(stated[i].bond[k].b, b->b) && stated[i].bond[k].sa == b->sa && stated[i].bond[k].sb == b->sb) return 1;
    }
    return 0;
}
int rec_part_count(Cpu *c, Ent *e) {
    Ent *set[64];
    int ours = ent_island_members(e, set, 64), game = e->actor ? ctn_group_count(e->actor) : 1;
    (void)c;
    return ours > game ? ours : game;
}
int rec_op_open_for(const Ent *e) {
    for (int i = 0; i < OP_N; i++) {
        if (ops[i].state == OP_FREE) continue;
        for (int side = 0; side < 2; side++) {
            Ent *x = ent_by_id(side ? ops[i].t : ops[i].x), *set[64];
            int n = x ? ent_island_members(x, set, 64) : 0;
            for (int k = 0; k < n; k++) if (set[k] == e) return 1;
        }
    }
    for (int i = 0; i < SERVE_N; i++) {
        if (!serves[i].used) continue;
        Ent *x = ent_by_id(serves[i].x), *t = ent_by_id(serves[i].t);
        if (x == e || t == e) return 1;
    }
    return 0;
}

// ---- Breaking and making one bond (S 7.3) -----------------------------------------------------------------------------------------
// The break is the game's own message main+0x1cf54d4(end A's owner actor +0x378, edge, 0, withEd), read back by the
// caller on a later pass (the message is queued, and a full actor ring drops it silently).
int rec_break(Cpu *c, const Bond *b) {
    Ent *a = ent_by_id(b->ea ? b->a : b->b);
    if (!a || !a->actor || !b->edge) return 0;
    for (int i = 0; i < 16; i++) if (breaking[i].edge == b->edge && ent_ms() - breaking[i].ms < 170) return 1;   // (asked, not yet read back)
    {   // (the bond list is up to 250 ms old and the game reuses edges: the edge must still be one of the part's)
        CtnEdge ed[32];
        int ne = ctn_part_edges(c, a->actor, ed, 32), there = 0;
        for (int k = 0; k < ne && !there; k++) there = ed[k].edge == b->edge;
        if (!there) { ent_mark_due(a); return 0; }
    }
    coop_guest_call(c, MOD_MAIN_BASE + 0x1cf54d4, a->actor + 0x378, b->edge, 0, 1);
    int f = 0;
    for (int i = 0; i < 16; i++) { if (breaking[i].edge == b->edge) { f = i; break; } if (ent_ms() - breaking[i].ms > 2000) f = i; }
    if (breaking[f].edge != b->edge) breaking[f].edge = b->edge, breaking[f].tries = 0;
    breaking[f].ms = ent_ms(), breaking[f].tries++;
    ent_mark_due(a);
    return 1;
}
static __thread int bond_cap_on;
static __thread uint64_t bond_cap_edge;
int coop_ent_bond_capture(void) { return bond_cap_on; }             // the bond maker's hook asks (coop_life.c)
void coop_ent_bond_captured(uint64_t edge) { bond_cap_edge = edge; }
// Steps 3 to 5 of the make: the preconditions, the direct bond call, the glue look. 1 made; 0 with *why set.
int rec_make(Cpu *c, Ent *a, uint8_t sa, Ent *b, uint8_t sb, const float rel[12], uint8_t *why) {
    uint64_t ba = 0, bb = 0;
    _Alignas(16) float wa[12], wb[12];
    float now_rel[12], inv[12];
    *why = WHY_BOND_NOT_READY;
    if (!a->actor || !b->actor || !body_mat(c, a, sa, &ba, wa) || !body_mat(c, b, sb, &bb, wb)) return 0;
    for (int side = 0; side < 2; side++) {
        // The readiness test of the direct bond (research notes sync-v4/direct-bond, section 4): the component chain,
        // the physics set and its bond manager, the component at +0x288, and a graph node for the body (body +0x68
        // bit 24 clear: an edge on a body with no node joins nothing and is never listed). The game's own node
        // look-up main+0xa2d39c is not called: it writes its answer through x8.
        uint64_t actor = side ? b->actor : a->actor, body = side ? bb : ba;
        uint64_t comps = coop_rd_ptr(actor + 0x228), c50 = comps ? coop_rd_ptr(comps + 0x50) : 0, phys = c50 ? coop_rd_ptr(c50 + 0x20) : 0;
        uint64_t d8 = phys ? coop_rd_ptr(phys + 0xd8) : 0, f68 = 0;
        if (!comps || !phys || !d8 || !coop_rd_ptr(d8 + 0x18) || !coop_rd_ptr(comps + 0x288) || !coop_rd_ptr(phys + 0x150)) return 0;
        if (!coop_rd(body + 0x68, &f68, 8) || (f68 >> 24 & 1)) return 0;   // (bit 24: the body has no graph node)
    }
    // At 22 parts the game breaks every glue edge of the part: the count is the greater of the game's group counts and
    // this table's, both sides together.
    if (rec_part_count(c, a) + rec_part_count(c, b) > ENT_MEMBERS) { *why = WHY_FULL22; return 0; }
    if (!ctn_m34_inv(inv, wa)) return 0;
    ctn_m34_mul(now_rel, inv, wb);
    *why = WHY_BOND_POSE;
    if (m34_dist(now_rel, rel) > 0.05f || m34_angle_deg(now_rel, rel) > 5) return 0;
    {   // (the new edge joins the lists a physics step later: the call is not made twice for one pair in a second, and
        // not at all when the part has the edge)
        static struct { Eid a, b; uint8_t sa, sb; int64_t ms; } called[8];
        static int called_i;
        CtnEdge ed0[32];
        int n0 = ctn_part_edges(c, a->actor, ed0, 32);
        for (int k = 0; k < n0; k++) if (ed0[k].other == b->actor && ed0[k].key == sa && ed0[k].okey == sb) { *why = WHY_NONE; return 1; }
        for (int k = 0; k < 8; k++)
            if (eid_eq(called[k].a, a->id) && eid_eq(called[k].b, b->id) && called[k].sa == sa && called[k].sb == sb && ent_ms() - called[k].ms < 1000) {
                *why = WHY_BOND_NOT_READY;
                return 0;
            }
        called[called_i % 8].a = a->id, called[called_i % 8].b = b->id, called[called_i % 8].sa = sa, called[called_i % 8].sb = sb, called[called_i++ % 8].ms = ent_ms();
    }
    // main+0x1c05a98(SPR of a, a's body key, b's body, matA, matB): the bond holds inverse(matA) * matB, so the two
    // world matrices as read now hold the bodies as they stand (research notes sync-v4/bond-frames).
    // The edge is taken where the game makes it: the bond maker's hook (coop_life.c) hands back what the maker returned
    // while this thread has the capture on. The edge joins the lists a physics step later, so it is not looked for.
    bond_cap_on = 1, bond_cap_edge = 0;
    ctn_call5(c, MOD_MAIN_BASE + 0x1c05a98, ctn_spr(a->actor), sa, bb, (uint64_t)(uintptr_t)wa, (uint64_t)(uintptr_t)wb);
    bond_cap_on = 0;
    int found = bond_cap_edge != 0;
    // its glue look (edge +0xef: the strand is bound a physics step later and reads it then)
    if (found) __atomic_store_n((uint8_t *)(uintptr_t)(bond_cap_edge + 0xef), (uint8_t)1, __ATOMIC_SEQ_CST);
    uint64_t spr = ctn_spr(a->actor);
    if (spr) __atomic_fetch_or((uint64_t *)(uintptr_t)(spr + 0x20), 1ull << 18, __ATOMIC_SEQ_CST);
    ent_mark_due(a);
    *why = WHY_NONE;
    {   // (per attempt, for the first test session: made, and the two bodies' relative speed now)
        float va[3] = {0}, vb[3] = {0};
        char x[24], y[24];
        coop_rd(ba + 0xf8, va, sizeof va), coop_rd(bb + 0xf8, vb, sizeof vb);
        rt_log("coop: ent: %lld bond made here between %s and %s (keys %u %u): edge %s, relative speed %.2f m/s", (long long)ent_ms(), ent_eid_str(a->id, x),
               ent_eid_str(b->id, y), sa, sb, found ? "made" : "not made (the maker gave none)", sqrtf((va[0] - vb[0]) * (va[0] - vb[0]) + (va[1] - vb[1]) * (va[1] - vb[1]) + (va[2] - vb[2]) * (va[2] - vb[2])));
    }
    return found ? 1 : 2;   // 2: called, the edge not listed yet
}
// Step 2: the smaller fragment placed as one rigid set so that the bond's two bodies stand at rel. 1 placed (or in
// place already); 0 when neither side may be placed.
static int place_for_bond(Cpu *c, Ent *a, uint8_t sa, Ent *b, uint8_t sb, const float rel[12]) {
    Ent *fa[64], *fb[64];
    int na = ent_island_members(a, fa, 64), nb = ent_island_members(b, fb, 64);
    uint64_t me = coop_ctr_me();
    float wa[12], wb[12], inv[12], target[12], delta[12], now_rel[12];
    if (!body_mat(c, a, sa, NULL, wa) || !body_mat(c, b, sb, NULL, wb) || !ctn_m34_inv(inv, wa)) return 0;
    ctn_m34_mul(now_rel, inv, wb);
    if (m34_dist(now_rel, rel) <= 0.05f && m34_angle_deg(now_rel, rel) <= 5) return 1;
    for (int pass = 0; pass < 2; pass++) {
        int move_b = (nb <= na) != pass;   // the smaller first, then the other
        Ent **f = move_b ? fb : fa;
        int n = move_b ? nb : na, may = 1;
        for (int k = 0; k < n && may; k++)
            may = f[k]->owner != me && f[k]->actor && !ent_this_link_holds(f[k]) && !ent_this_link_rides(f[k]) && !own_req_open_for(f[k]);
        if (!may) continue;
        if (move_b) ctn_m34_mul(target, wa, rel), ctn_m34_inv(inv, wb);          // target of b's body = Wa * rel
        else { float ir[12]; ctn_m34_inv(ir, rel), ctn_m34_mul(target, wb, ir), ctn_m34_inv(inv, wa); }   // of a's body = Wb * inverse(rel)
        ctn_m34_mul(delta, target, inv);
        for (int k = 0; k < n; k++) {
            float nm[12];
            ctn_m34_mul(nm, delta, f[k]->m);
            ctn_place(c, f[k]->actor, nm);
            f[k]->bond_wanted = 1;
        }
        return 1;
    }
    return 0;
}

// ---- One island of another owner against its statement (S 7.1 to 7.4) ---------------------------------------------------------------
static void backoff(Stated *st, uint8_t why) {
    int64_t now = ent_ms();
    st->state = ST_NOT_SHOWN, st->why = why;
    st->backoff = st->backoff ? (st->backoff * 2 > 30000 ? 30000 : st->backoff * 2) : 2000;
    st->backoff_until = now + st->backoff, st->passes = 0;
    char b[24];
    rt_log("coop: ent: %lld the island %s of %04x is not shown here (reason %u): tried again in %lld s", (long long)now, ent_eid_str(st->root, b),
           (unsigned)(st->from & 0xffff), why, (long long)(st->backoff / 1000));
}
static void rec_one(Cpu *c, Stated *st, int st_i) {
    int64_t now = ent_ms();
    uint64_t me = coop_ctr_me();
    Ent *m[ENT_MEMBERS];
    int n = 0, live = 0, missing = 0, making = 0, acted = 0;
    float min_r = 1e9f, max_rel = 0;
    for (int k = 0; k < st->n; k++) {
        Ent *e = ent_by_id(st->m[k].id);
        if (!e || e->owner != st->from) continue;   // (not the sender's by this table: not reconciled)
        m[n++] = e;
        if (st->m[k].life == LIFE_LIVE && e->life == LIFE_LIVE) {
            float d = sqrtf(st->m[k].rel_root[3] * st->m[k].rel_root[3] + st->m[k].rel_root[7] * st->m[k].rel_root[7] + st->m[k].rel_root[11] * st->m[k].rel_root[11]);
            live++, missing += !e->actor, making += e->making;
            if (st->m[k].radius < min_r) min_r = st->m[k].radius;
            if (d > max_rel) max_rel = d;
        }
    }
    if (!n) return;
    for (int k = 0; k < n; k++) if (rec_op_open_for(m[k])) return;   // (an operation of this game is open on it: S 8.2)
    // this game's digest of it: the members that are here, and the bonds of this world with an end among them
    {
        Eid id[ENT_MEMBERS];
        uint8_t life[ENT_MEMBERS];
        Bond b[ENT_BONDS];
        int nb = ent_island_bonds(m, n, b, ENT_BONDS);
        for (int k = 0; k < n; k++) id[k] = m[k]->id, life[k] = m[k]->life == LIFE_LIVE && !m[k]->actor ? 0xee : m[k]->life;
        st->my_digest = ent_digest(id, life, n, b, nb);
    }
    // deaths and goings, whatever the range
    for (int k = 0; k < n; k++) {
        Ent *e = m[k];
        if (!e->actor || e->life == LIFE_LIVE) continue;
        if (e->life == LIFE_DEAD && e->death_kind != 0xff && !e->death_ms) {
            ctn_death_put(e->actor, e->gen, e->death_kind), e->death_ms = now, acted = 1;   // replayed with its kind, G4 lifted
        } else if (e->life == LIFE_DEAD && (e->death_kind == 0xff || now - e->death_ms > 1000)) {
            ent_delete_actor(c, e), e->death_due = 0, acted = 1;   // (the replay did not take it, or a plain delete)
        } else if (e->life == LIFE_GONE) {
            if (e->flags & ENT_COPY) ent_delete_actor(c, e), acted = 1;
            else { Ent *one[1] = {e}; e->life = LIFE_LIVE; own_claim(one, 1, "its owner states it gone, and it is this game's actor"); }
        }
    }
    // in range: this Link within (the smallest radius, less 10 m, less the island's reach) of the newest root pose
    float lp[3] = {0};
    uint64_t link = coop_player_actor();
    int in_range = 0;
    if (link && coop_rd(link + COOP_ACTOR_POS, lp, 12)) {
        float dx = lp[0] - st->root_m[3], dy = lp[1] - st->root_m[7], dz = lp[2] - st->root_m[11];
        float lim = min_r - 10 - max_rel;
        if (lim < 15) lim = 15;   // (a radius that was not read, or a small one: still made when this Link stands by it)
        in_range = sqrtf(dx * dx + dy * dy + dz * dz) < lim;
    }
    if (st->state == ST_NOT_SHOWN && now < st->backoff_until) return;   // (before the maker's guard: a failed make keeps its back-off)
    { int ctn_make_is(Eid root); if (ctn_make_is(st->root)) { st->state = ST_MAKING, st->pass_ms = now; return; } }   // (the maker has it: coop_ctn.c)
    if (missing && !in_range) { st->state = ST_FAR, st->why = WHY_NONE, st->passes = 0; return; }
    if (st->state == ST_FAR) st->passes = 0, st->backoff = 0, st->rung2_done = 0;   // (came into range)
    if (missing && !making) {
        // a create waits when the newest pose is old, a POSE has been seen for the root, and the owner is not paused
        int stale = now - st->pose_ms > 2000 && st->cls_ms && !ent_peer_paused(st->from);
        if (stale) { st->state = ST_DIFFER; return; }
        int paired = 0;   // (a member the mirror may have an actor for is bound, not made: one by one below)
        for (int k = 0; k < n; k++) paired |= m[k]->mir_id != 0;
        if (missing == live && st->rec_n && live >= 2 && !paired) {   // nothing of it is here: made whole by the maker (S 9.2)
            float reach = max_rel + 1.5f;
            if (ctn_make_busy() || ctn_make_waits(st->root_m, reach)) {
                extern const char *ctn_make_wait_why;
                static int64_t said;   // (an island that waits for the maker is not shown: why, every 5 s)
                if (st->state == ST_WAIT_MAKER && now - said > 5000) {
                    char wb[24];
                    said = now, rt_log("coop: ent: %lld the island %s waits for the copy maker: %s", (long long)now, ent_eid_str(st->root, wb), ctn_make_busy() ? "it is making another" : ctn_make_wait_why);
                }
                st->state = ST_WAIT_MAKER;
                return;
            }
            if (ctn_make_start(c, st_i)) { st->state = ST_MAKING, st->pass_ms = now; return; }
            backoff(st, WHY_MAKER_FAILED);
            return;
        }
        for (int k = 0; k < st->n; k++) {   // each missing member by a plain create, or the actor the mirror has for it
            Ent *e = ent_by_id(st->m[k].id);
            if (!e || e->owner != st->from || e->actor || e->making || e->life != LIFE_LIVE) continue;
            if (e->death_ms && now - e->death_ms < 5000) continue;   // (its death was just replayed here, or its create just failed)
            if (e->mir_id && !e->mir_resolved) { if (now - e->entered_ms < 1500) continue; }   // (the mirror is asked: the tick answers)
            if (e->mir_id && e->mir_resolved && e->mir_actor && ctn_alive(e->mir_actor)) {
                ent_bind(e, e->mir_actor);
                if (e->mir_copy) e->flags |= ENT_COPY;
                acted = 1;
                continue;
            }
            if (coop_create_refused(e->name)) { backoff(st, WHY_CREATE_FAILED); return; }   // (a name no statement makes here)
            float w[12], rot[9], pos[3];
            ctn_m34_mul(w, st->root_m, st->m[k].rel_root);
            for (int i = 0; i < 3; i++) { pos[i] = w[i * 4 + 3]; for (int j = 0; j < 3; j++) rot[i * 3 + j] = w[i * 4 + j]; }
            int fin = 1;
            for (int i = 0; i < 12; i++) fin &= isfinite(w[i]) && fabsf(w[i]) < 1e5f;
            if (!fin) { backoff(st, WHY_CREATE_FAILED); return; }
            memset(e->handle, 0, sizeof e->handle), memset(e->link, 0, sizeof e->link);
            int ok = (e->flags & ENT_GREEN) ? coop_request_create_autobuilt(c, e->name, pos, rot, e->handle) : coop_request_create(c, e->name, pos, rot, e->handle);
            if (ok) e->making = 1, e->make_ms = now, e->flags |= ENT_COPY, acted = 1;
            else { backoff(st, WHY_CREATE_FAILED); return; }
        }
        st->state = ST_DIFFER;
        return;
    }
    if (making) { st->state = ST_MAKING; return; }
    // bonds: break what the statement lacks, make what has no edge
    {
        static Bond mine[ENT_BONDS];
        int nm = ent_island_bonds(m, n, mine, ENT_BONDS);
        for (int i = 0; i < nm; i++) {
            const Bond *b = &mine[i];
            int listed = 0;
            for (int k = 0; k < st->nb && !listed; k++) listed = eid_eq(st->bond[k].a, b->a) && eid_eq(st->bond[k].b, b->b) && st->bond[k].sa == b->sa && st->bond[k].sb == b->sb;
            if (listed) continue;
            Ent *ea = ent_by_id(b->a), *eb = ent_by_id(b->b), *other = ea && ea->owner == st->from ? eb : ea;
            if (ea && eb && ea->owner == st->from && eb->owner == st->from) other = NULL;   // both the sender's
            if (other) {
                // its other end is this game's, or held under an open request (ended only by an operation's answer);
                // or another owner's whose statement lists the bond, or that has no statement here at all
                if (other->owner == me || own_req_open_for(other)) continue;
                if (stated_lists(other->owner, b) || (other->st < 0 && !other->st_digest)) continue;
            }
            rec_break(c, b), acted = 1;
        }
        for (int k = 0; k < st->nb && !st->bond_step; k++) {
            const Bond *sb = &st->bond[k];
            if (my_bond(sb->a, sb->b, sb->sa, sb->sb)) continue;
            Ent *ea = ent_by_id(sb->a), *eb = ent_by_id(sb->b);
            if (!ea || !eb || !ea->actor || !eb->actor) continue;
            Ent *other = ea->owner == st->from ? eb : ea;
            // its other end is this game's: the game that glued sends a bond request; made here after 2 s without one
            if (other->owner == me && ea->owner != eb->owner && now - st->ms < 2000) continue;
            if (own_req_open_for(ea) || own_req_open_for(eb)) continue;
            st->bond_step = 1, st->bond_ms = now, st->bond_now = *sb;
        }
    }
    if (st->bond_step) {
        Ent *ea = ent_by_id(st->bond_now.a), *eb = ent_by_id(st->bond_now.b);
        uint8_t why = WHY_BOND_NOT_READY;
        Ent *fa[64], *fb[64];
        int na = ea ? ent_island_members(ea, fa, 64) : 0, nb = eb ? ent_island_members(eb, fb, 64) : 0;
        if (!ea || !eb || !ea->actor || !eb->actor || my_bond(st->bond_now.a, st->bond_now.b, st->bond_now.sa, st->bond_now.sb)) {
            for (int k = 0; k < na; k++) fa[k]->bond_wanted = 0;
            for (int k = 0; k < nb; k++) fb[k]->bond_wanted = 0;
            st->bond_step = 0;
        } else if (now - st->bond_ms > 5000) {   // (5 s for all of it: placing, the preconditions, the read-back)
            st->bond_step = 0, st->why = why, st->passes++, acted = 1;
        } else if (st->bond_step == 1) {   // steps 1 and 2: both fragments out of the drive, the smaller placed
            if (place_for_bond(c, ea, st->bond_now.sa, eb, st->bond_now.sb, st->bond_now.rel)) st->bond_step = 2;
            acted = 1;
        } else if (st->bond_step == 2) {   // steps 3 to 5, a frame later
            if (rec_make(c, ea, st->bond_now.sa, eb, st->bond_now.sb, st->bond_now.rel, &why)) st->bond_step = 3, acted = 1;
            else if (why == WHY_FULL22) { st->bond_step = 0; backoff(st, why); return; }
            else if (why == WHY_BOND_POSE) st->bond_step = 1;   // (placed again)
        }   // (step 3: called; the bond is among this world's once the edges are read again, which ends the make)
    }
    // the verdict of this pass
    if (st->my_digest == st->digest) {
        // layout (S 3.5): each member against rel_root, but for members a bond at a body other than the main one leads to
        int off = -1;
        float inv[12];
        if (m[0]->actor && ctn_m34_inv(inv, m[0]->m))
            for (int k = 1; k < st->n && off < 0; k++) {
                Ent *e = ent_by_id(st->m[k].id);
                float rr[12];
                int keyed = 0;
                if (!e || !e->actor || e->owner != st->from) continue;
                for (int q = 0; q < st->nb; q++)
                    keyed |= (eid_eq(st->bond[q].a, e->id) || eid_eq(st->bond[q].b, e->id)) && (st->bond[q].sa || st->bond[q].sb);
                if (keyed) continue;
                ctn_m34_mul(rr, inv, e->m);
                if (m34_dist(rr, st->m[k].rel_root) > 0.3f || m34_angle_deg(rr, st->m[k].rel_root) > 15) off = k;
            }
        if (off < 0) { st->state = ST_AGREE, st->why = WHY_NONE, st->passes = 0, st->layout_ms = 0, st->backoff = 0; return; }
        if (!st->layout_ms) st->layout_ms = now;
        if (now - st->layout_ms < 1000) { st->state = ST_AGREE; return; }
        if (!st->rung2_done) {   // rung 2: the stated bonds of the member that is off are broken; the make then places it
            Ent *e = ent_by_id(st->m[off].id);
            for (int i = 0; e && i < bond_n; i++) {
                Ent *x = ent_by_id(bonds[i].a), *y = ent_by_id(bonds[i].b);
                if (!x || !y || x->owner != st->from || y->owner != st->from) continue;   // (only bonds inside the sender's island)
                if (eid_eq(bonds[i].a, e->id) || eid_eq(bonds[i].b, e->id)) rec_break(c, &bonds[i]);
            }
            st->rung2_done = 1, st->state = ST_DIFFER, st->layout_ms = now;
            return;
        }
        if (now - st->layout_ms > 3000) acted = 1, st->layout_ms = now;   // (still off 3 s after rung 2: a failed pass)
    }
    st->state = ST_DIFFER;
    if (!acted) return;   // (a pass counts when it acted and the read-back, next pass, still differs)
    if (++st->passes < 4) return;
    // rung 3: every run-time actor of the island here that this game does not own is deleted, and the island is made
    // whole from the recipe. Not without a recipe, with a map member, or with a member this game owns.
    uint8_t skip = !st->rec_n ? WHY_NO_RECIPE : WHY_NONE;
    for (int k = 0; k < st->n && !skip; k++) {
        Ent *e = ent_by_id(st->m[k].id);
        if (e && (e->flags & ENT_MAP)) skip = WHY_MAP_MEMBER;
        if (e && e->owner == me) skip = WHY_OWN_MEMBER;
    }
    if (skip || st->passes > 4) { backoff(st, skip ? skip : st->why ? st->why : WHY_MAKER_FAILED); return; }
    for (int k = 0; k < n; k++) if (m[k]->actor) ent_delete_actor(c, m[k]);
    char b[24];
    rt_log("coop: ent: %lld the island %s still differs after three passes: its copy here is deleted and made whole again", (long long)now, ent_eid_str(st->root, b));
}

// A foreign island of one: the same passes on a statement built from the entity's row.
static void rec_single(Cpu *c, Ent *e) {
    static Stated tmp;
    memset(&tmp, 0, offsetof(Stated, rec));
    memset((char *)&tmp + offsetof(Stated, rec_n), 0, sizeof tmp - offsetof(Stated, rec_n));   // (the reconcile state after rec: not the last entity's)
    tmp.used = 1, tmp.from = e->owner, tmp.root = e->id, tmp.digest = e->st_digest, tmp.n = 1;
    tmp.m[0].id = e->id, tmp.m[0].life = e->life, tmp.m[0].death_kind = e->death_kind, tmp.m[0].green = (e->flags & ENT_GREEN) != 0, tmp.m[0].radius = e->radius;
    tmp.m[0].rel_root[0] = tmp.m[0].rel_root[5] = tmp.m[0].rel_root[10] = 1;
    memcpy(tmp.root_m, e->root_m, sizeof tmp.root_m), tmp.pose_ms = e->pose_ms, tmp.cls = e->cls, tmp.cls_ms = e->cls_ms;
    tmp.state = e->state, tmp.why = e->why, tmp.passes = e->passes, tmp.backoff = e->backoff, tmp.backoff_until = e->backoff_until, tmp.ms = e->stated_ms;
    tmp.rec_n = 0;
    rec_one(c, &tmp, -1);
    e->state = tmp.state, e->why = tmp.why, e->passes = tmp.passes, e->backoff = tmp.backoff, e->backoff_until = tmp.backoff_until, e->digest = tmp.my_digest;
}

// Which entities are out of the drive because a bond is being made on their fragment: derived each frame from the
// makes under way and the requests in service, so nothing can leave one behind.
static void bond_wanted_derive(void) {
    for (int i = 0; i < ENT_N; i++) ents[i].bond_wanted = 0;
    for (int i = 0; i < STATED_N + SERVE_N; i++) {
        Ent *e2[2] = {NULL, NULL}, *set[64];
        if (i < STATED_N) { if (stated[i].used && stated[i].bond_step) e2[0] = ent_by_id(stated[i].bond_now.a), e2[1] = ent_by_id(stated[i].bond_now.b); }
        else if (serves[i - STATED_N].used) e2[0] = ent_by_id(serves[i - STATED_N].x);
        for (int s = 0; s < 2; s++) {
            int n = e2[s] ? ent_island_members(e2[s], set, 64) : 0;
            for (int k = 0; k < n; k++) set[k]->bond_wanted = 1;
        }
    }
}
void rec_pass(Cpu *c) {
    int64_t now = ent_ms();
    uint64_t me = coop_ctr_me();
    if (!me) return;
    bond_wanted_derive();
    for (int i = 0; i < 16; i++) {   // breaks read back: asked again for 10 frames, then logged
        if (!breaking[i].edge || now - breaking[i].ms < 170) continue;
        const Bond *b = NULL;
        for (int k = 0; k < bond_n && !b; k++) if (bonds[k].edge == breaking[i].edge) b = &bonds[k];
        if (!b) { breaking[i].edge = 0; continue; }
        if (breaking[i].tries >= 10) { rt_log("coop: ent: a glue edge this game had to break is still there after ten break messages"); breaking[i].edge = 0; continue; }
        rec_break(c, b);
    }
    for (int i = 0; i < ENT_N; i++) {   // a DEATH from an owner: replayed on the copy at once (S 6.10)
        Ent *e = &ents[i];
        if (!e->used || e->death_due != 1 || e->owner == me) continue;
        e->death_due = 0;
        if (e->actor && e->life == LIFE_LIVE && e->death_kind != 0xff) ctn_death_put(e->actor, e->gen, e->death_kind);
    }
    for (int i = 0; i < STATED_N; i++) {
        Stated *st = &stated[i];
        if (!st->used) continue;
        int owned = 0;   // (a statement none of whose members its sender owns any more is freed: S 5.0)
        for (int k = 0; k < st->n && !owned; k++) { Ent *e = ent_by_id(st->m[k].id); owned = e && e->owner == st->from; }
        if (!owned) {
            for (int k = 0; k < ENT_N; k++) if (ents[k].used && ents[k].st == i) ents[k].st = -1;
            st->used = 0;
            continue;
        }
        int hot = st->bond_step || st->state == ST_MAKING || now - st->ms < 200;
        if (!hot && st->state == ST_AGREE && now - st->pass_ms < 1000) continue;
        if (!hot && now - st->pass_ms < (st->state == ST_AGREE ? 1000 : 250)) continue;
        st->pass_ms = now;
        rec_one(c, st, i);
    }
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || e->owner == me || !e->owner || e->st >= 0 || !e->st_digest || !e->mir_known) continue;
        if (now - e->pass_ms < (e->state == ST_AGREE ? 1000 : 250)) continue;
        e->pass_ms = now;
        rec_single(c, e);
    }
}
void rec_owned_now(Cpu *c, Ent **members, int n) {
    // Entities this game came to own: their copies are originals of this game from now on in every way but ENT_COPY,
    // which stays (it says who made the actor). The strain fields of their bonds (strand +0x1d4, +0x1d8, +0x1dc) are
    // left as they are: no code read depends on them but the strain look (research notes sync-v4/strand-cap, 6), and
    // the strand of an edge is not found without a walk whose layout is not read.
    (void)c;
    for (int i = 0; i < n; i++) members[i]->bond_wanted = 0, members[i]->st = -1, members[i]->state = ST_AGREE;
}

// ---- The glue across owners (S 8) ----------------------------------------------------------------------------------------------------
void rec_glue_made(Cpu *c, const Ev *ev) {   // a hand glue's bond was made here: both ends are entities from now on
    uint64_t a = ctn_body_actor(ev->actor), b = ctn_body_actor(ev->a2);
    if (a && !ent_by_actor(a)) ent_enter_glued(c, a);
    if (b && !ent_by_actor(b)) ent_enter_glued(c, b);
    Ent *e = a ? ent_by_actor(a) : NULL;
    if (e) ent_mark_due(e);
}
static Op *op_for(Eid x, Eid t) {
    for (int i = 0; i < OP_N; i++)
        if (ops[i].state != OP_FREE && ((eid_eq(ops[i].x, x) && eid_eq(ops[i].t, t)) || (eid_eq(ops[i].x, t) && eid_eq(ops[i].t, x)))) return &ops[i];
    return NULL;
}
static int served(Eid x, Eid t) {
    for (int i = 0; i < SERVE_N; i++)
        if (serves[i].used && ((eid_eq(serves[i].x, x) && eid_eq(serves[i].t, t)) || (eid_eq(serves[i].x, t) && eid_eq(serves[i].t, x)))) return 1;
    return 0;
}
static void op_send(Op *o) {
    Ent *x = ent_by_id(o->x), *t = ent_by_id(o->t);
    if (!x || !t || !t->owner) return;
    BondReq q = {o->op, o->x, o->t, o->sx, o->st, x->epoch, {0}};
    memcpy(q.rel, o->rel, sizeof q.rel);
    o->to = t->owner, o->sent = ent_ms();
    own_send_to(o->to, ENT_BOND_REQ, COOP_CTR_RELIABLE, &q, sizeof q);
}
// An edge of this world between an entity this game owns (or holds under an open request) and one it does not, with
// no operation: one opens at once (S 8.1).
void rec_edge_found(Cpu *c, const Bond *b) {
    uint64_t me = coop_ctr_me();
    Ent *ea = ent_by_id(b->a), *eb = ent_by_id(b->b);
    (void)c;
    if (!ea || !eb || !ea->mir_known || !eb->mir_known || op_for(b->a, b->b) || served(b->a, b->b)) return;
    int a_mine = ea->owner == me, b_mine = eb->owner == me, a_asked = !a_mine && own_req_open_for(ea), b_asked = !b_mine && own_req_open_for(eb);
    if ((a_mine && b_mine) || (!a_mine && !b_mine && !a_asked && !b_asked)) return;
    if (!a_mine && !b_mine && ea->owner == eb->owner) return;   // (inside one island of another owner: its own bond, asked or not)
    Op *o = NULL;
    for (int i = 0; i < OP_N && !o; i++) if (ops[i].state == OP_FREE) o = &ops[i];
    if (!o) { ent_full("operation"); return; }
    memset(o, 0, sizeof *o);
    o->op = op_next++, o->opened = ent_ms(), o->edge = b->edge;
    if (!op_next) op_next = 1;
    // X is this game's end; when one end is only asked, X is the asked end and nothing is sent until its GRANT
    int x_is_a = a_asked ? 1 : b_asked ? 0 : a_mine;
    if (x_is_a) o->x = b->a, o->t = b->b, o->sx = b->sa, o->st = b->sb, memcpy(o->rel, b->rel, sizeof o->rel);
    else o->x = b->b, o->t = b->a, o->sx = b->sb, o->st = b->sa, ctn_m34_inv(o->rel, b->rel);
    o->state = (a_asked || b_asked) ? OP_WAIT_GRANT_X : OP_SENT;
    char p[24], q[24];
    rt_log("coop: ent: %lld operation %u: a glue here between %s (this game's%s) and %s", (long long)ent_ms(), o->op, ent_eid_str(o->x, p),
           o->state == OP_WAIT_GRANT_X ? ", being taken" : "", ent_eid_str(o->t, q));
    if (o->state == OP_SENT) op_send(o);
}
static void bond_answer(uint64_t to, uint32_t op, int ack, uint8_t reason, uint64_t who, uint32_t epoch) {
    BondAns a = {op, ack ? 0 : reason, who, epoch};
    int f = 0;
    for (int i = 0; i < 16; i++) { if (bond_mem[i].from == to && bond_mem[i].op == op) { f = i; break; } if (bond_mem[i].ms < bond_mem[f].ms) f = i; }
    bond_mem[f].from = to, bond_mem[f].op = op, bond_mem[f].ack = (uint8_t)ack, bond_mem[f].reason = reason, bond_mem[f].who = who, bond_mem[f].epoch = epoch, bond_mem[f].ms = ent_ms();
    own_send_to(to, ack ? ENT_BOND_ACK : ENT_BOND_NAK, COOP_CTR_RELIABLE, &a, sizeof a);
}
// Rule 6: one owner for the merged island. The GRANT names every member of both islands.
static void serve_finish(Cpu *c, Serve *s) {
    uint64_t me = coop_ctr_me();
    Ent *x = ent_by_id(s->x), *t = ent_by_id(s->t), *set[64], *mem[2 * ENT_MEMBERS];
    int n = 0;
    (void)c;
    if (!x || !t) { bond_answer(s->from, s->op, 0, NAK_GONE, 0, 0), s->used = 0; return; }
    int ns = ent_island_members(t, set, 64);
    uint32_t ep = s->epoch_x;
    for (int k = 0; k < ns && n < 2 * ENT_MEMBERS; k++) if (set[k]->owner == me || set[k]->owner == s->from) { mem[n++] = set[k]; if (set[k]->epoch > ep) ep = set[k]->epoch; }
    ns = ent_island_members(x, set, 64);
    for (int k = 0; k < ns && n < 2 * ENT_MEMBERS; k++) {
        int have = 0;
        for (int q = 0; q < n; q++) have |= mem[q] == set[k];
        if (!have && (set[k]->owner == me || set[k]->owner == s->from)) { mem[n++] = set[k]; if (set[k]->epoch > ep) ep = set[k]->epoch; }
    }
    int keep = ent_this_link_holds(t) || ent_this_link_rides(t);
    char b[24];
    rt_log("coop: ent: %lld operation %u of %04x: bond with %s in place; the merged island (%d members) is %s", (long long)ent_ms(), s->op, (unsigned)(s->from & 0xffff),
           ent_eid_str(s->t, b), n, keep ? "this game's (it holds or rides it)" : "granted to the gluing game");
    own_grant(keep ? me : s->from, 0, keep ? me : s->from, mem, n, ep + 1, ANS_BOND);
    bond_answer(s->from, s->op, 1, 0, 0, 0);
    s->used = 0;
}
void rec_bond_req(Cpu *c, const CoopCtrMsg *m) {
    BondReq q;
    uint64_t me = coop_ctr_me();
    if (m->len < (int)sizeof q) return;
    memcpy(&q, m->d, sizeof q);
    for (int i = 0; i < 16; i++)   // a known operation gets the same answer again
        if (bond_mem[i].from == m->from && bond_mem[i].op == q.op && ent_ms() - bond_mem[i].ms < 30000) {
            bond_answer(m->from, q.op, bond_mem[i].ack, bond_mem[i].reason, bond_mem[i].who, bond_mem[i].epoch);
            return;
        }
    for (int i = 0; i < SERVE_N; i++) if (serves[i].used && serves[i].from == m->from && serves[i].op == q.op) return;   // (in service)
    Ent *x = ent_by_id(q.x), *t = ent_by_id(q.t);
    if (x && (x->owner == me || (x->owner != m->from && x->epoch >= q.epoch_x))) { bond_answer(m->from, q.op, 1, 0, 0, 0); return; }   // rule 0
    if (t && t->owner != me) { bond_answer(m->from, q.op, 0, NAK_NOT_OWNER, t->owner, t->epoch); return; }                              // rule 1
    if (!t || t->life != LIFE_LIVE) { bond_answer(m->from, q.op, 0, NAK_GONE, 0, 0); return; }                                           // rule 2
    if (x && x->actor && t->actor && rec_part_count(c, x) + rec_part_count(c, t) > ENT_MEMBERS && !my_bond(eid_lt(q.x, q.t) ? q.x : q.t, eid_lt(q.x, q.t) ? q.t : q.x, eid_lt(q.x, q.t) ? q.sx : q.st, eid_lt(q.x, q.t) ? q.st : q.sx)) {
        bond_answer(m->from, q.op, 0, NAK_FULL, 0, 0);                                                                                   // rule 3
        return;
    }
    Serve *s = NULL;
    for (int i = 0; i < SERVE_N && !s; i++) if (!serves[i].used) s = &serves[i];
    if (!s) { ent_full("bond request in service"); return; }   // (asked again by the gluing game in a second)
    memset(s, 0, sizeof *s);
    s->used = 1, s->from = m->from, s->op = q.op, s->x = q.x, s->t = q.t, s->sx = q.sx, s->st = q.st, s->epoch_x = q.epoch_x, s->since = ent_ms();
    memcpy(s->rel, q.rel, sizeof s->rel);
    if (x) x->named = 1;
}
static void serve_step(Cpu *c, Serve *s) {   // rules 4 and 5, over as many frames as they take (3 s)
    int64_t now = ent_ms();
    Ent *x = ent_by_id(s->x), *t = ent_by_id(s->t);
    if (!t || t->owner != coop_ctr_me() || t->life != LIFE_LIVE) { bond_answer(s->from, s->op, 0, t ? NAK_NOT_OWNER : NAK_GONE, t ? t->owner : 0, t ? t->epoch : 0), s->used = 0; return; }
    if (!x || !x->actor || !t->actor) { if (now - s->since > 3000) bond_answer(s->from, s->op, 0, NAK_NOT_HERE, 0, 0), s->used = 0; return; }
    int x_low = eid_lt(s->x, s->t);
    Ent *a = x_low ? x : t, *b = x_low ? t : x;
    uint8_t sa = x_low ? s->sx : s->st, sb = x_low ? s->st : s->sx, why = 0;
    float rel[12];
    if (x_low) memcpy(rel, s->rel, sizeof rel); else ctn_m34_inv(rel, s->rel);   // rel runs from the lower entity's body
    if (my_bond(a->id, b->id, sa, sb)) { serve_finish(c, s); return; }
    if (s->made) { if (now - s->since > 3000) bond_answer(s->from, s->op, 0, NAK_POSE, 0, 0), s->used = 0; return; }   // (made, not yet among the edges read)
    Ent *fx[64];
    int nx = ent_island_members(x, fx, 64);
    for (int k = 0; k < nx; k++) fx[k]->bond_wanted = 1;
    if (!place_for_bond(c, a, sa, b, sb, rel)) { if (now - s->since > 3000) bond_answer(s->from, s->op, 0, NAK_POSE, 0, 0), s->used = 0; return; }
    if (rec_make(c, a, sa, b, sb, rel, &why)) { s->made = 1; return; }   // (called: the edge is among this world's next pass)
    if (why == WHY_FULL22) { bond_answer(s->from, s->op, 0, NAK_FULL, 0, 0), s->used = 0; return; }
    if (now - s->since > 3000) {
        for (int k = 0; k < nx; k++) fx[k]->bond_wanted = 0;
        bond_answer(s->from, s->op, 0, NAK_POSE, 0, 0), s->used = 0;
    }
}
static void op_close(Op *o, const char *how) {
    rt_log("coop: ent: %lld operation %u closed: %s", (long long)ent_ms(), o->op, how);
    o->state = OP_FREE;
}
void rec_bond_ans(Cpu *c, const CoopCtrMsg *m, int ack) {
    BondAns a;
    (void)c;
    if (m->len < (int)sizeof a) return;
    memcpy(&a, m->d, sizeof a);
    Op *o = NULL;
    for (int i = 0; i < OP_N && !o; i++) if (ops[i].state != OP_FREE && ops[i].op == a.op) o = &ops[i];
    if (!o || o->state == OP_BREAKING) return;
    if (ack || (a.reason == NAK_NOT_OWNER && a.who == coop_ctr_me())) { op_close(o, "the bond is in both games"); return; }
    if (a.reason == NAK_NOT_OWNER && a.who && o->forwards < 2) {
        Ent *t = ent_by_id(o->t);
        if (t) ent_apply_owner(t, a.who, a.epoch);
        o->forwards++, op_send(o);
        return;
    }
    static const char *why[] = {"", "not the owner", "the other thing is gone", "the build would pass 21 parts", "this game's part did not arrive there", "the parts could not be brought into place there"};
    rt_log("coop: ent: %lld operation %u refused (%s): this game's glue is broken", (long long)ent_ms(), o->op, a.reason < 6 ? why[a.reason] : "?");
    o->state = OP_BREAKING, o->break_tries = 0;
}
// Step 7: the operations of this game and the requests it serves.
void rec_ops(Cpu *c) {
    int64_t now = ent_ms();
    uint64_t me = coop_ctr_me();
    if (!me) return;
    for (int i = 0; i < bond_n; i++) rec_edge_found(c, &bonds[i]);
    for (int i = 0; i < SERVE_N; i++) if (serves[i].used) serve_step(c, &serves[i]);
    for (int i = 0; i < OP_N; i++) {
        Op *o = &ops[i];
        if (o->state == OP_FREE) continue;
        Ent *x = ent_by_id(o->x), *t = ent_by_id(o->t);
        int x_low = eid_lt(o->x, o->t);
        const Bond *b = my_bond(x_low ? o->x : o->t, x_low ? o->t : o->x, x_low ? o->sx : o->st, x_low ? o->st : o->sx);
        if (!x || !t) { op_close(o, "an end is no longer known"); continue; }
        if (o->state == OP_BREAKING) {
            if (!b) { op_close(o, "the glue is broken here"); continue; }
            if (o->break_tries++ > 10 * 10) { rt_log("coop: ent: operation %u: the refused glue is still there: a bond in one game only", o->op); op_close(o, "the break failed"); continue; }
            rec_break(c, b);
            continue;
        }
        if (!b && now - o->opened > 500) { op_close(o, "the edge is gone here"); continue; }
        if (x->owner == me && t->owner == me) { op_close(o, "this game owns both ends"); continue; }
        if (o->state == OP_WAIT_GRANT_X) {
            if (x->owner == me) { o->state = OP_SENT, op_send(o); continue; }
            if (!own_req_open_for(x)) { o->state = OP_BREAKING; rt_log("coop: ent: operation %u: the part glued was not granted to this game: the glue is broken", o->op); }
            continue;
        }
        if (x->owner != me) { op_close(o, "this game no longer owns its end"); continue; }
        // the other end's owner is gone, loading, or paused and silent for 1 s: its island is claimed (S 5.7)
        Stated *st = t->st >= 0 ? &stated[t->st] : NULL;
        uint8_t cls = st ? st->cls : t->cls;
        if (!ent_peer_here(t->owner) || ent_peer_loading(t->owner) || (now - o->opened > 1000 && ent_peer_paused(t->owner) && cls == CLS_IDLE)) {
            Ent *set[64], *mem[ENT_MEMBERS];
            int ns = ent_island_members(t, set, 64), n = 0;
            for (int k = 0; k < ns; k++) if (set[k]->owner == t->owner && n < ENT_MEMBERS) mem[n++] = set[k];
            own_claim(mem, n, "glued onto, and its owner is gone, loading or paused");
            op_close(o, "the other end was claimed");
            continue;
        }
        if (now - o->opened > 10000) { o->state = OP_BREAKING; rt_log("coop: ent: operation %u: no answer in 10 s: the glue is broken", o->op); continue; }
        if (now - o->sent > 1000) op_send(o);
    }
}

// ---- Strain and the cap of 21 belong to the owner (S 7.6) ------------------------------------------------------------------------------
// The strain check main+0x190f034(strand), the glue manager's list add main+0x191642c(manager, strand) and the cap's
// kill main+0x190d924(strand) are skipped for a strand whose edge (strand +0x50) has an end body of an entity another
// game owns. Returning at the strain check's entry is the game's own exempt path (research notes sync-v4/strand-cap).
int coop_ent_strand_hook(Cpu *c, uint64_t off) {
    uint64_t strand = off == 0x191642c ? c->x[1] : c->x[0], edge = strand ? coop_rd_ptr(strand + 0x50) : 0;
    if (!edge) return 0;
    uint64_t a = coop_rd_ptr(edge + 0x100), b = coop_rd_ptr(edge + 0x108);
    return ent_set_has(ENT_SET_FOLLOWER_BODY, a) || ent_set_has(ENT_SET_FOLLOWER_BODY, b);
}
