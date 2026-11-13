// Co-op: entities, ownership and the messages (docs/coop/contraption-sync-v4-spec.md sections 5 and 6, cited as S).
//
// One game owns an entity and simulates it. Ownership moves by a request that is always answered (TAKE, then GRANT
// or REFUSE), by a claim when the owner is gone, loading, paused and silent, or offers the island, and never any
// other way: ent_apply_owner is the only code that takes an owner from a message, and it takes a pair only if it is
// greater by (epoch, then incarnation). The owner states each island it owns (ISLAND), sends its pose and device
// state (POSE), and the other games answer with the digest of what they have (APPLIED). INDEX finds what a follower
// still holds that its owner no longer has.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_ent_msg.h"
#include "coop_game.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);

Req reqs[REQ_N];
static Ask asks[ASK_N];
static Ans ans[ANS_N];
static uint32_t req_next = 1, stmt_number;
static uint8_t ent_owed[ENT_N];             // bit p: a statement of a DEAD or GONE entity is owed to peer p
static int16_t became_mine[64];
static int became_n;
static int64_t gone_since[ENT_N];           // since when an entity's owner has not been in the session
static uint32_t index_end_due[ENT_PEERS];   // the number of a NEED(0, 0) whose answer is being sent
static uint32_t index_end_count[ENT_PEERS], index_end_hash[ENT_PEERS];
static struct { Eid e; uint8_t kind; int64_t ms; } death_wait[16];
static struct { Eid root; uint64_t from; int64_t first, asked; } need_root[32];

// ---- Sending: per peer; what a peer's queue refused is owed (S 6) ------------------------------------------------------------
static int test_drop(int flags) {   // TOTK_COOP_ENT_DROP=n: every n-th reliable message dropped before it is queued
    static int n = -1, k;
    if (n < 0) { const char *e = cfg_get("TOTK_COOP_ENT_DROP"); n = e ? atoi(e) : 0; }
    return n > 0 && (flags & COOP_CTR_RELIABLE) && ++k % n == 0;
}
int own_send_to(uint64_t inc, int type, int flags, const void *d, int len) {
    int id = inc ? coop_ctr_route(inc) : 0;
    if (!id) return 0;
    if (test_drop(flags)) return 1;
    return coop_ctr_send(id, type, flags, d, len);
}
static int send_all(int type, int flags, const void *d, int len) {   // 1: every peer here took it
    int ok = 1;
    for (int id = 1; id <= ENT_PEERS; id++) {
        uint64_t inc = coop_ctr_incarnation(id);
        if (inc && inc != coop_ctr_me()) ok &= own_send_to(inc, type, flags, d, len) != 0;
    }
    return ok;
}

// ---- S 5.0: the one rule ----------------------------------------------------------------------------------------------------------
int ent_apply_owner(Ent *e, uint64_t owner, uint32_t epoch) {
    if (epoch < e->epoch || (epoch == e->epoch && owner <= e->owner)) return 0;
    uint64_t me = coop_ctr_me(), was = e->owner;
    e->owner = owner, e->epoch = epoch;
    if (was == me && owner != me) {
        if (ent_this_link_holds(e)) e->end_hold = 1;
        if (ent_this_link_rides(e)) e->end_ride = 1;
        e->flags |= e->actor && !(e->flags & ENT_MAP) ? 0 : 0;   // (an original stays an original: only GONE decides, S 7.2)
    }
    if (owner == me && was != me && became_n < 64) became_mine[became_n++] = (int16_t)ent_i(e);
    ent_mark_due(e);
    return 1;
}
static uint32_t max_epoch(Ent **m, int n) {
    uint32_t ep = 0;
    for (int i = 0; i < n; i++) if (m[i]->epoch > ep) ep = m[i]->epoch;
    return ep;
}
void own_claim(Ent **members, int n, const char *why) {
    uint64_t me = coop_ctr_me();
    uint32_t ep = max_epoch(members, n) + 1;
    char b[24];
    if (!me || !n) return;
    for (int i = 0; i < n; i++) {
        Ent *e = members[i];
        if (e->owner == me && e->epoch == ep) continue;
        if (e->owner != me && became_n < 64) became_mine[became_n++] = (int16_t)ent_i(e);
        e->owner = me, e->epoch = ep;
        if (e->life == LIFE_FREE) e->life = LIFE_LIVE;
        e->st = -1, e->state = ST_AGREE;
    }
    ent_mark_due(members[0]);
    rt_log("coop: ent: %lld claimed %s and %d more at epoch %u: %s", (long long)ent_ms(), ent_eid_str(members[0]->id, b), n - 1, ep, why);
}
void own_owe(Island *is, int peer) {
    for (int p = 0; p < ENT_PEERS; p++) if (peer < 0 || peer == p) is->owed[p] = 1;
}
void own_owe_ent(Ent *e, int peer) {
    if (e->isl >= 0 && e->life == LIFE_LIVE) { own_owe(&isls[e->isl], peer); return; }
    ent_owed[ent_i(e)] |= peer < 0 ? 0xff : (uint8_t)(1u << peer);
}

// ---- Requests of this game (S 5.2, 5.5) ----------------------------------------------------------------------------------------------
static Req *req_find(uint32_t id) {
    for (int i = 0; id && i < REQ_N; i++) if (reqs[i].state != RQ_FREE && reqs[i].id == id) return &reqs[i];
    return NULL;
}
uint32_t own_req_open_for(const Ent *e) {
    for (int i = 0; i < REQ_N; i++) {
        if (reqs[i].state != RQ_OPEN) continue;
        Ent *m = ent_by_id(reqs[i].member);
        Ent *set[64];
        int n = m ? ent_island_members(m, set, 64) : 0;
        for (int k = 0; k < n; k++) if (set[k] == e) return reqs[i].id;
    }
    return 0;
}
static Req *req_open(Ent *e, uint8_t reason) {
    Req *r = NULL;
    int64_t now = ent_ms();
    for (int i = 0; i < REQ_N; i++) {
        if (reqs[i].state == RQ_OPEN && eid_eq(reqs[i].member, e->id)) return &reqs[i];
        if (reqs[i].state != RQ_FREE && reqs[i].state != RQ_OPEN && now - reqs[i].closed > 20000) reqs[i].state = RQ_FREE;
        if (reqs[i].state == RQ_FREE && !r) r = &reqs[i];
    }
    if (!r) { ent_full("request"); return NULL; }
    memset(r, 0, sizeof *r);
    r->id = req_next++, r->state = RQ_OPEN, r->reason = reason, r->member = e->id, r->to = e->owner, r->opened = now;
    if (!req_next) req_next = 1;
    return r;
}
static void req_send(Req *r) {
    Ent *e = ent_by_id(r->member);
    if (!e) return;
    Ent *set[64], *mem[ENT_MEMBERS];
    int ns = ent_island_members(e, set, 64), n = 0;
    for (int k = 0; k < ns && n < ENT_MEMBERS; k++) if (set[k]->owner == e->owner) mem[n++] = set[k];
    Stated *st = e->st >= 0 ? &stated[e->st] : NULL;
    TakeMsg t = {r->id, r->member, e->epoch, ent_digest_of(mem, n), r->reason, (uint8_t)(st ? !st->layout_ms : 1)};
    own_send_to(r->to, ENT_TAKE, COOP_CTR_RELIABLE, &t, sizeof t);
    r->sent = ent_ms();
}
static void req_close(Req *r, uint8_t state) { r->state = state, r->closed = ent_ms(); }

void own_grab(Cpu *c, Ent *e) {   // this Link's Ultrahand took hold of e
    uint64_t me = coop_ctr_me();
    char b[24];
    (void)c;
    if (!me || e->owner == me) return;
    if (!e->owner) {   // a FREE map entity: claimed (S 5.1), with the members of its island nobody owns
        Ent *set[64], *mem[ENT_MEMBERS];
        int ns = ent_island_members(e, set, 64), n = 0;
        for (int k = 0; k < ns; k++) if (!set[k]->owner && n < ENT_MEMBERS) mem[n++] = set[k];
        own_claim(mem, n, "grabbed, and nobody owned it");
        return;
    }
    if (e->life != LIFE_LIVE) return;
    // Its owner holds or rides it (the class from POSE, under 2 s old, or frozen while that owner is paused): the
    // grab is ended at once, with no TAKE (S 5.5).
    Stated *st = e->st >= 0 ? &stated[e->st] : NULL;
    uint8_t cls = st ? st->cls : e->cls;
    int64_t cls_ms = st ? st->cls_ms : e->cls_ms;
    if (cls != CLS_IDLE && (ent_ms() - cls_ms < 2000 || ent_peer_paused(e->owner))) {
        rt_log("coop: ent: %lld grab of %s refused here: its owner %s it", (long long)ent_ms(), ent_eid_str(e->id, b), cls == CLS_HOLD ? "holds" : "rides");
        e->end_hold = 1;
        return;
    }
    Req *r = req_open(e, TAKE_GRAB);
    if (r && !r->sent) req_send(r), rt_log("coop: ent: %lld TAKE %u of %s (grab) to %04x", (long long)ent_ms(), r->id, ent_eid_str(e->id, b), (unsigned)(r->to & 0xffff));
}

// The game's own way to end a hold (S 5.5): the refusal message 0x20510006 queued on the held actor's channel, named
// to this Link. Word 0 is the receiver; the channel's actor is the sender, which is what the Ultrahand part compares
// with the actor it holds (research notes sync-v4/seat-and-refusal, 6). Read back and sent again: the actor message
// ring drops silently when full.
static struct { uint64_t actor; int64_t sent; int tries; } hold_end;
void own_end_hold(Cpu *c, Ent *e) {
    uint64_t held = ent_held_actor(), link = coop_player_actor();
    (void)e;
    if (!held || !link) return;
    static _Alignas(16) uint8_t msg[16];
    uint32_t key = 0, id = 0x20510006u;
    if (!coop_rd(link + 0x380, &key, 4)) return;
    memset(msg, 0, sizeof msg), memcpy(msg, &key, 4), memcpy(msg + 4, &id, 4);
    coop_guest_call(c, MOD_MAIN_BASE + 0x2b76c34, held + 0x378, (uint64_t)(uintptr_t)msg, 0, 0);
    if (hold_end.actor != held) hold_end.actor = held, hold_end.tries = 0;
    hold_end.sent = ent_ms(), hold_end.tries++;
}
// A ride is ended with the game's own get-off (S 5.6): main+0xc448c8(rider = [[Link +0x228] +0x410], 1, 1).
void own_end_ride(Cpu *c) {
    uint64_t link = coop_player_actor(), rider = link ? coop_rd_ptr(coop_rd_ptr(link + 0x228) + 0x410) : 0;
    if (rider) coop_guest_call(c, MOD_MAIN_BASE + 0xc448c8, rider, 1, 1, 0);
}

// ---- Statements (S 6.2) ---------------------------------------------------------------------------------------------------------------
#define STMT_MAX 16384
#define PIECE_MAX (COOP_CTR_PAYLOAD - (int)sizeof(IslandHdr))
static int stmt_build(Cpu *c, Island *is, Ent *one, uint8_t *buf, IslandHdr *h) {
    Ent *mem[ENT_MEMBERS];
    int n = 0;
    uint64_t me = coop_ctr_me();
    memset(h, 0, sizeof *h);
    if (is) for (int k = 0; k < is->n; k++) mem[n++] = &ents[is->m[k]];
    else mem[n++] = one;
    static Bond b[ENT_BONDS];
    int nb = is ? ent_island_bonds(mem, n, b, ENT_BONDS) : 0, nf = 0, len = 0;
    float inv[12];
    int have_inv = ctn_m34_inv(inv, mem[0]->m);
    for (int k = 0; k < n; k++) {
        IslandMember r = {0};
        Ent *e = mem[k];
        r.id = e->id, r.owner = e->owner, r.epoch = e->epoch, r.life = e->life, r.death_kind = e->death_kind, r.green = (e->flags & ENT_GREEN) != 0;
        r.mir_owner = e->mir_owner, r.mir_id = e->mir_id, memcpy(r.name, e->name, sizeof r.name), memcpy(r.scale, e->scale, sizeof r.scale);
        r.radius = e->radius;
        if (have_inv && e->actor) ctn_m34_mul(r.rel_root, inv, e->m);
        else r.rel_root[0] = r.rel_root[5] = r.rel_root[10] = 1;
        memcpy(buf + len, &r, sizeof r), len += (int)sizeof r;
    }
    IslandFar far[ENT_MEMBERS];
    for (int i = 0; i < nb; i++) {
        IslandBond r = {b[i].a, b[i].b, b[i].sa, b[i].sb, b[i].ed, 0, {0}};
        memcpy(r.rel, b[i].rel, sizeof r.rel);
        memcpy(buf + len, &r, sizeof r), len += (int)sizeof r;
        for (int side = 0; side < 2; side++) {
            Ent *o = ent_by_id(side ? b[i].b : b[i].a);
            int have = 0;
            if (!o || o->owner == me) continue;
            for (int q = 0; q < nf; q++) have |= eid_eq(far[q].id, o->id);
            if (!have && nf < ENT_MEMBERS) far[nf++] = (IslandFar){o->id, o->owner, o->epoch};
        }
    }
    memcpy(buf + len, far, sizeof far[0] * (size_t)nf), len += (int)(sizeof far[0] * (size_t)nf);
    // the recipe: for an owned island of 2 to 21 members with no far end (S 9.1), taken again when the digest changed
    if (is && n >= 2 && !is->has_far) {
        Recipe *r = is->rec >= 0 ? &recipes[is->rec] : NULL;
        if (!r || r->digest != is->digest) {
            if (!r) for (int q = 0; q < REC_N && !r; q++) if (!recipes[q].used) r = &recipes[q], is->rec = (int16_t)q;
            if (r) {
                r->used = 1, r->digest = is->digest;
                r->n = (uint8_t)ctn_recipe(c, mem, n, r->rec, r->member);
            } else ent_full("recipe");
        }
        if (r && r->n) {
            h->rec_n = r->n;
            memcpy(buf + len, r->rec, BP_SIZE), len += BP_SIZE;
            memcpy(buf + len, r->member, sizeof(Eid) * r->n), len += (int)(sizeof(Eid) * r->n);
        }
    }
    h->root = mem[0]->id, h->n = (uint16_t)n, h->nb = (uint16_t)nb, h->nf = (uint16_t)nf, h->total = (uint32_t)len;
    h->digest = is ? is->digest : ent_digest_of(mem, 1);
    memcpy(h->root_m, mem[0]->m, sizeof h->root_m);
    h->pieces = (uint16_t)((len + PIECE_MAX - 1) / PIECE_MAX);
    if (!h->pieces) h->pieces = 1;
    return len;
}
static int stmt_send(Cpu *c, Island *is, Ent *one, int p) {   // 1: every piece queued for peer p
    static uint8_t buf[STMT_MAX], pkt[COOP_CTR_PAYLOAD];
    uint64_t inc = coop_ctr_incarnation(p + 1);
    IslandHdr h;
    if (!inc || inc == coop_ctr_me()) return 1;
    int len = stmt_build(c, is, one, buf, &h);
    h.counter = peers[p].sent_counter + 1;
    for (int k = 0; k < h.pieces; k++) {
        int at = k * PIECE_MAX, pl = len - at < PIECE_MAX ? len - at : PIECE_MAX;
        h.piece = (uint16_t)k;
        memcpy(pkt, &h, sizeof h), memcpy(pkt + sizeof h, buf + at, (size_t)pl);
        if (!coop_ctr_send(p + 1, ENT_ISLAND, COOP_CTR_RELIABLE | COOP_CTR_BULK, pkt, (int)sizeof h + pl)) return 0;
    }
    if (test_drop(COOP_CTR_RELIABLE)) { /* (a whole statement dropped: the counter still rises, the pieces were sent) */ }
    peers[p].sent_counter = h.counter;
    return 1;
}

static struct { uint32_t counter; Eid root; uint32_t have; uint8_t buf[STMT_MAX]; } asm_buf[ENT_PEERS];
static void stated_free(Stated *st) {
    int si = (int)(st - stated);
    for (int i = 0; i < ENT_N; i++) if (ents[i].used && ents[i].st == si) ents[i].st = -1;
    st->used = 0;
}
static void stmt_apply(uint64_t from, int p, const IslandHdr *h, const uint8_t *body) {
    int64_t now = ent_ms();
    const IslandMember *mr = (const IslandMember *)body;
    const IslandBond *br = (const IslandBond *)(mr + h->n);
    const IslandFar *fr = (const IslandFar *)(br + h->nb);
    const uint8_t *rec = (const uint8_t *)(fr + h->nf);
    Ent *mem[ENT_MEMBERS];
    int n = 0;
    char b24[24];
    if (h->n > ENT_MEMBERS || h->nb > ENT_BONDS || h->nf > ENT_MEMBERS) return;
    if (p >= 0 && h->counter > peers[p].taken_counter) peers[p].taken_counter = h->counter;
    for (int k = 0; k < h->nf; k++) {   // far ends: not members; their owner and epoch as the sender knows them
        Ent *e = ent_new(fr[k].id);
        if (e) ent_apply_owner(e, fr[k].owner, fr[k].epoch), e->far_seen = 1;
    }
    for (int k = 0; k < h->n; k++) {
        Ent *e = ent_new(mr[k].id);
        if (!e) return;
        ent_apply_owner(e, mr[k].owner, mr[k].epoch);
        if (e->owner != from || mr[k].owner != from) continue;   // (the sender does not own it by this table: dropped for it)
        if (!e->name[0]) memcpy(e->name, mr[k].name, sizeof e->name), e->name[sizeof e->name - 1] = 0;
        memcpy(e->scale, mr[k].scale, sizeof e->scale), e->radius = mr[k].radius;
        if (mr[k].green) e->flags |= ENT_GREEN;
        if (mr[k].mir_id && !e->mir_id) e->mir_owner = mr[k].mir_owner, e->mir_id = mr[k].mir_id;
        e->mir_known = 1, e->named = 1, e->stated_ms = now;
        if (mr[k].life == LIFE_LIVE && e->life != LIFE_LIVE) e->life = LIFE_LIVE, e->death_due = 0, e->death_ms = 0;   // (claimed again, or made again there)
        if (mr[k].life != LIFE_LIVE && e->life == LIFE_LIVE) {
            e->life = mr[k].life, e->death_kind = mr[k].death_kind, e->dead_ms = now, e->death_due = e->actor != 0;
            if (mr[k].life == LIFE_FREE) e->owner = 0;
        }
        mem[n++] = e;
    }
    if (!n) return;
    // every statement of this sender that shares a member and is older goes
    for (int i = 0; i < STATED_N; i++) {
        Stated *st = &stated[i];
        int share = 0;
        if (!st->used || st->from != from || st->counter >= h->counter) continue;
        for (int k = 0; k < n && !share; k++) share = mem[k]->st == i;
        if (share) stated_free(st);
    }
    if (h->n == 1 && !h->nb) {   // an island of one: kept in the entity's row
        Ent *e = mem[0];
        int same1 = e->st < 0 && e->st_digest == h->digest;
        e->st = -1, e->st_digest = h->digest;
        if (!e->pose_ms || now - e->pose_ms > 2000) memcpy(e->root_m, h->root_m, sizeof e->root_m), e->pose_ms = now;
        if (!same1) {   // (the same statement again keeps its reconcile state)
            if (e->state == ST_AGREE || e->state == ST_NOT_SHOWN) e->state = ST_DIFFER;
            e->passes = 0, e->backoff = 0, e->backoff_until = 0;
        }
        return;
    }
    Stated *st = NULL, *oldest_far = NULL;
    for (int i = 0; i < STATED_N; i++) {
        if (stated[i].used && stated[i].from == from && eid_eq(stated[i].root, h->root)) { st = &stated[i]; break; }
        if (!stated[i].used && !st) st = &stated[i];
        if (stated[i].used && stated[i].state == ST_FAR && (!oldest_far || stated[i].ms < oldest_far->ms)) oldest_far = &stated[i];
    }
    if (!st && oldest_far) {   // a full table frees the far one that was stated longest ago (NEED brings it back)
        rt_log("coop: ent: the statement table is full: the far island %s is forgotten", ent_eid_str(oldest_far->root, b24));
        stated_free(oldest_far), st = oldest_far;
    }
    if (!st) { ent_full("statement"); return; }
    Stated keep_st;
    int same = st->used && st->digest == h->digest;
    if (same) memcpy(&keep_st, st, offsetof(Stated, rec));   // (the same structure stated again: its reconcile state stays)
    uint8_t cls = st->used ? st->cls : 0, offer = st->used ? st->offer : 0;
    int64_t cls_ms = st->used ? st->cls_ms : 0, pose_ms = st->used ? st->pose_ms : 0;
    float keep_m[12];
    memcpy(keep_m, st->root_m, sizeof keep_m);
    int si = (int)(st - stated);
    for (int i = 0; i < ENT_N; i++) if (ents[i].used && ents[i].st == si) ents[i].st = -1;
    memset(st, 0, sizeof *st);
    st->used = 1, st->from = from, st->root = h->root, st->counter = h->counter, st->digest = h->digest, st->ms = now;
    st->cls = cls, st->offer = offer, st->cls_ms = cls_ms, st->state = ST_DIFFER;
    if (pose_ms && now - pose_ms < 2000) memcpy(st->root_m, keep_m, sizeof keep_m), st->pose_ms = pose_ms;
    else memcpy(st->root_m, h->root_m, sizeof st->root_m), st->pose_ms = now;
    for (int k = 0; k < h->n; k++) {
        Ent *e = ent_by_id(mr[k].id);
        if (!e || e->owner != from) continue;
        st->m[st->n].id = mr[k].id, st->m[st->n].life = mr[k].life, st->m[st->n].death_kind = mr[k].death_kind, st->m[st->n].green = mr[k].green;
        memcpy(st->m[st->n].rel_root, mr[k].rel_root, sizeof mr[k].rel_root), st->m[st->n].radius = mr[k].radius;
        st->n++, e->st = (int16_t)si;
    }
    for (int k = 0; k < h->nb; k++) {
        Bond *bd = &st->bond[st->nb++];
        memset(bd, 0, sizeof *bd);
        bd->a = br[k].a, bd->b = br[k].b, bd->sa = br[k].sa, bd->sb = br[k].sb, bd->ed = br[k].ed, memcpy(bd->rel, br[k].rel, sizeof bd->rel);
    }
    for (int k = 0; k < h->nf; k++) st->far[st->nf].id = fr[k].id, st->far[st->nf].owner = fr[k].owner, st->far[st->nf].epoch = fr[k].epoch, st->nf++;
    if (h->rec_n && h->rec_n <= ENT_MEMBERS) st->rec_n = h->rec_n, memcpy(st->rec, rec, BP_SIZE), memcpy(st->rec_member, rec + BP_SIZE, sizeof(Eid) * h->rec_n);
    if (same) {
        st->state = keep_st.state, st->why = keep_st.why, st->passes = keep_st.passes, st->rung2_done = keep_st.rung2_done;
        st->backoff = keep_st.backoff, st->backoff_until = keep_st.backoff_until, st->layout_ms = keep_st.layout_ms, st->pass_ms = keep_st.pass_ms;
        st->my_digest = keep_st.my_digest, st->bond_step = keep_st.bond_step, st->bond_ms = keep_st.bond_ms, st->bond_now = keep_st.bond_now;
    }
}
static void island_in(const CoopCtrMsg *m) {
    IslandHdr h;
    int p = m->from_id - 1;
    if (m->len < (int)sizeof h || p < 0 || p >= ENT_PEERS) return;
    memcpy(&h, m->d, sizeof h);
    int pl = m->len - (int)sizeof h, at = h.piece * PIECE_MAX;
    if (h.total > STMT_MAX || h.pieces > 32 || h.piece >= h.pieces || at + pl > STMT_MAX) return;
    if (h.counter <= peers[p].taken_counter && !(h.counter == asm_buf[p].counter && asm_buf[p].have)) return;   // (not above the last taken)
    if (asm_buf[p].counter != h.counter || h.piece == 0) asm_buf[p].counter = h.counter, asm_buf[p].root = h.root, asm_buf[p].have = 0;
    memcpy(asm_buf[p].buf + at, m->d + sizeof h, (size_t)pl), asm_buf[p].have |= 1u << h.piece;
    if (asm_buf[p].have != (h.pieces >= 32 ? ~0u : (1u << h.pieces) - 1)) return;
    asm_buf[p].have = 0;
    uint32_t need = (uint32_t)(sizeof(IslandMember) * h.n + sizeof(IslandBond) * h.nb + sizeof(IslandFar) * h.nf + (h.rec_n ? BP_SIZE + sizeof(Eid) * h.rec_n : 0));
    if (need != h.total) return;
    stmt_apply(m->from, p, &h, asm_buf[p].buf);
}

// ---- The owner's answer to a TAKE (S 5.3), from phase 0 or, while PAUSED, from the net tick (S 5.4) -----------------------------
static Ans *ans_find(uint64_t from, uint32_t id, uint8_t kind) {
    for (int i = 0; i < ANS_N; i++) if (ans[i].used && ans[i].from == from && ans[i].id == id && ans[i].kind == kind) return &ans[i];
    return NULL;
}
static Ans *ans_new(uint64_t from, uint32_t id, uint8_t kind) {
    Ans *a = NULL;
    for (int i = 0; i < ANS_N; i++) if (!ans[i].used || !a || ans[i].ms < a->ms) { a = &ans[i]; if (!ans[i].used) break; }
    memset(a, 0, sizeof *a);
    a->used = 1, a->from = from, a->id = id, a->kind = kind, a->ms = ent_ms();
    return a;
}
static void ans_send(Ans *a) {
    if (a->type == ENT_GRANT) {
        GrantMsg g = {a->asker, a->id, a->owner, a->epoch, a->sw, a->on, a->payer, a->n, {{0}}};
        memcpy(g.m, a->m, sizeof(Eid) * a->n);
        send_all(ENT_GRANT, COOP_CTR_RELIABLE, &g, GRANT_LEN(a->n));
    } else if (a->type == ENT_REFUSE) {
        RefuseMsg r = {a->id, a->reason, a->owner, a->epoch};
        own_send_to(a->from, ENT_REFUSE, COOP_CTR_RELIABLE, &r, sizeof r);
    } else if (a->type == ENT_REQ_ANS) {
        ReqAns r = {a->id, a->reason};
        own_send_to(a->from, ENT_REQ_ANS, COOP_CTR_RELIABLE, &r, sizeof r);
    }
    a->sent = ent_ms();
}
static void refuse(uint64_t from, uint32_t id, uint8_t reason, uint64_t owner, uint32_t epoch) {
    Ans *a = ans_new(from, id, ANS_TAKE);
    a->type = ENT_REFUSE, a->reason = reason, a->owner = owner, a->epoch = epoch;
    ans_send(a);
}
// The grant of a set of this game's entities to another game: to all, applied here, repeated every 0.5 s until the new
// owner is heard at that epoch or 5 s have passed (S 5.3 rule 6).
void own_grant(uint64_t asker, uint32_t id, uint64_t new_owner, Ent **mem, int n, uint32_t epoch, uint8_t kind) {
    Ans *a = ans_new(asker, id, kind);
    a->type = ENT_GRANT, a->asker = asker, a->owner = new_owner, a->epoch = epoch, a->n = (uint8_t)(n > 2 * ENT_MEMBERS ? 2 * ENT_MEMBERS : n);
    for (int k = 0; k < a->n; k++) a->m[k] = mem[k]->id;
    Grp *g = NULL;
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < GRP_N && !g; i++) if (grps[i].used && grps[i].mine && eid_eq(grps[i].root, mem[0]->id)) g = &grps[i];
    if (g) a->sw = g->sw, a->on = g->on, a->payer = g->payer;
    plat_mutex_unlock(&grp_lock);
    for (int k = 0; k < n; k++) ent_apply_owner(mem[k], new_owner, epoch);
    if (new_owner != coop_ctr_me()) a->send = 1, a->until = ent_ms() + 5000;
    ans_send(a);
}
static void take_in(const CoopCtrMsg *m, int paused) {
    TakeMsg t;
    char b[24];
    uint64_t me = coop_ctr_me();
    if (m->len < (int)sizeof t) return;
    memcpy(&t, m->d, sizeof t);
    Ans *a = ans_find(m->from, t.id, ANS_TAKE);
    if (a) { ans_send(a); return; }
    Ent *e = ent_by_id(t.member);
    if (!e || e->owner != me) { refuse(m->from, t.id, REF_NOT_OWNER, e ? e->owner : 0, e ? e->epoch : 0); return; }
    Island *is = e->isl >= 0 ? &isls[e->isl] : NULL;
    if (!is) return;   // (an entity this game came to own while PAUSED has no stored island: no answer, S 5.4)
    if (t.epoch_seen < e->epoch) { refuse(m->from, t.id, REF_STALE, me, e->epoch), own_owe(is, m->from_id - 1); return; }
    if (is->cls != CLS_IDLE) { refuse(m->from, t.id, is->cls == CLS_HOLD ? REF_HELD : REF_RIDDEN, me, e->epoch); return; }
    if (is->frozen) return;   // (offered: it is claimed, not taken)
    if (t.digest != is->digest || !t.layout_ok) { refuse(m->from, t.id, REF_STALE, me, e->epoch), own_owe(is, m->from_id - 1); return; }
    Ent *mem[ENT_MEMBERS];
    for (int k = 0; k < is->n; k++) mem[k] = &ents[is->m[k]];
    rt_log("coop: ent: %lld GRANT of %s (%d members) to %04x at epoch %u%s", (long long)ent_ms(), ent_eid_str(is->root, b), is->n, (unsigned)(m->from & 0xffff),
           is->epoch + 1, paused ? " (answered while paused)" : "");
    own_grant(m->from, t.id, m->from, mem, is->n, is->epoch + 1, ANS_TAKE);
}
static int64_t grant_from_ms[ENT_PEERS];   // when a GRANT last came from each peer
static void grant_in(const CoopCtrMsg *m) {
    GrantMsg g;
    uint64_t me = coop_ctr_me();
    char b[24];
    if (m->len < GRANT_LEN(0)) return;
    memset(&g, 0, sizeof g), memcpy(&g, m->d, (size_t)(m->len < (int)sizeof g ? m->len : (int)sizeof g));
    if (g.n > 2 * ENT_MEMBERS || m->len < GRANT_LEN(g.n)) return;
    int took = 0;
    for (int k = 0; k < g.n; k++) {
        Ent *e = ent_new(g.m[k]);
        if (!e) continue;
        e->named = 1;
        took += ent_apply_owner(e, g.owner, g.epoch);
    }
    if (m->from_id >= 1 && m->from_id <= ENT_PEERS) grant_from_ms[m->from_id - 1] = ent_ms();
    if (g.asker == me) {
        Req *r = req_find(g.id);
        Ent *e = r ? ent_by_id(r->member) : NULL;
        if (r && r->state == RQ_OPEN && (took || (e && e->owner == me))) req_close(r, RQ_GRANTED);
    }
    if (g.owner == me && took)
        rt_log("coop: ent: %lld this game owns %s and %d more now (epoch %u, granted by %04x)", (long long)ent_ms(), ent_eid_str(g.m[0], b), g.n - 1, g.epoch,
               (unsigned)(m->from & 0xffff));
}
static void refuse_in(const CoopCtrMsg *m) {
    RefuseMsg f;
    char b[24];
    if (m->len < (int)sizeof f) return;
    memcpy(&f, m->d, sizeof f);
    Req *r = req_find(f.id);
    if (!r || r->state != RQ_OPEN) return;   // (a refusal of a granted or expired request is dropped)
    Ent *e = ent_by_id(r->member);
    if (f.reason == REF_NOT_OWNER && e && f.owner) {
        ent_apply_owner(e, f.owner, f.epoch);
        if (e->owner == coop_ctr_me()) { req_close(r, RQ_GRANTED); return; }   // (it names this game: a grant)
        if (!r->forwarded && f.owner != m->from) { r->forwarded = 1, r->to = f.owner, req_send(r); return; }
    }
    req_close(r, RQ_REFUSED);
    rt_log("coop: ent: %lld TAKE %u of %s refused: %s", (long long)ent_ms(), r->id, e ? ent_eid_str(e->id, b) : "?",
           f.reason == REF_HELD ? "its owner holds it" : f.reason == REF_RIDDEN ? "its owner rides it" : f.reason == REF_STALE ? "this copy is not as the owner's" : "not the owner");
    if (e) { if (r->reason == TAKE_GRAB) e->end_hold = 1; else e->end_ride = 1; }
}

// ---- REQ: a toggle or a switch event asked of the owner (S 6.9) ------------------------------------------------------------------
void own_req(Ent *e, uint8_t kind, uint32_t value) {
    Ask *a = NULL;
    for (int i = 0; i < ASK_N; i++) if (!asks[i].used && !a) a = &asks[i];
    if (!a) { ent_full("ask"); return; }
    a->used = 1, a->id = req_next++, a->e = e->id, a->kind = kind, a->value = value, a->opened = ent_ms(), a->sent = 0;
    if (!req_next) req_next = 1;
}
static void req_in(Cpu *c, const CoopCtrMsg *m) {
    ReqMsg q;
    if (m->len < (int)sizeof q) return;
    memcpy(&q, m->d, sizeof q);
    Ans *a = ans_find(m->from, q.id, ANS_REQ);
    if (a) { ans_send(a); return; }
    Ent *e = ent_by_id(q.e);
    a = ans_new(m->from, q.id, ANS_REQ), a->type = ENT_REQ_ANS, a->reason = REQ_REFUSED;
    if (e && e->owner == coop_ctr_me() && e->actor) {
        extern uint64_t ctn_payer_hint;   // (the asker pays for what it switches on)
        ctn_payer_hint = m->from;
        if (q.kind == REQ_TOGGLE) ctn_toggle_put(c, e->actor, (int)q.value);
        else ctn_event_put(e->actor, e->gen, (uint8_t)q.value);
        a->reason = REQ_DONE;
    }
    ans_send(a);
}
static void req_ans_in(const CoopCtrMsg *m) {
    ReqAns r;
    if (m->len < (int)sizeof r) return;
    memcpy(&r, m->d, sizeof r);
    for (int i = 0; i < ASK_N; i++) if (asks[i].used && asks[i].id == r.id) asks[i].used = 0;
}

// ---- DEATH, NEED, INDEX_END, OFFER_ALL ----------------------------------------------------------------------------------------------
void own_death(Ent *e, uint8_t kind) {
    DeathMsg d = {e->id, kind};
    send_all(ENT_DEATH, COOP_CTR_RELIABLE, &d, sizeof d);
}
// A DEATH is "replay this death on your copy": the copy plays the blast or the burning out as the owner's does. The
// entity's life changes by the owner's statement, once the game there has accepted the delete.
static void death_apply(Ent *e, uint8_t kind) {
    if (e->life != LIFE_LIVE || !e->actor) return;
    e->death_kind = kind, e->death_due = 1, e->death_ms = ent_ms();
}
static void death_in(const CoopCtrMsg *m) {
    DeathMsg d;
    if (m->len < (int)sizeof d) return;
    memcpy(&d, m->d, sizeof d);
    Ent *e = ent_by_id(d.e);
    if (e) { if (e->owner == m->from) death_apply(e, d.kind); return; }   // (from another game than its owner: dropped)
    for (int i = 0; i < 16; i++) if (!death_wait[i].ms || ent_ms() - death_wait[i].ms > 2000) { death_wait[i].e = d.e, death_wait[i].kind = d.kind, death_wait[i].ms = ent_ms(); break; }
}
static void need_in(const CoopCtrMsg *m) {
    NeedMsg q;
    int p = m->from_id - 1;
    if (m->len < (int)sizeof q || p < 0 || p >= ENT_PEERS) return;
    memcpy(&q, m->d, sizeof q);
    if (eid_zero(q.id)) {
        uint32_t count = 0, hash = 2166136261u;
        for (int i = 0; i < ISL_N; i++) if (isls[i].used) isls[i].owed[p] = 1;
        for (int i = 0; i < ENT_N; i++) {   // (ascending Eid is not needed for an order-free sum: the hashes are added)
            Ent *e = &ents[i];
            if (!e->used || e->owner != coop_ctr_me() || e->life != LIFE_LIVE || (e->flags & ENT_PREVIEW)) continue;
            uint32_t h1 = ent_fnv(ent_fnv(ent_fnv(2166136261u, &e->id.inc, 8), &e->id.serial, 4), &e->epoch, 4);
            count++, hash += h1;
        }
        index_end_due[p] = q.number ? q.number : 1, index_end_count[p] = count, index_end_hash[p] = hash;
        return;
    }
    Ent *e = ent_by_id(q.id);
    if (e && e->owner == coop_ctr_me()) own_owe_ent(e, p);
}
static void index_end_in(const CoopCtrMsg *m) {
    IndexEnd x;
    int p = m->from_id - 1;
    char b[24];
    if (m->len < (int)sizeof x || p < 0 || p >= ENT_PEERS) return;
    memcpy(&x, m->d, sizeof x);
    if (x.number != peers[p].need_number) return;
    uint32_t count = 0, hash = 2166136261u;
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || e->owner != m->from || e->life != LIFE_LIVE || !e->named) continue;
        count++, hash += ent_fnv(ent_fnv(ent_fnv(2166136261u, &e->id.inc, 8), &e->id.serial, 4), &e->epoch, 4);
    }
    if (count != x.count || hash != x.hash) return;   // (the answer is not all here as it was queued: the next INDEX decides)
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || e->owner != m->from || e->life != LIFE_LIVE || e->named) continue;
        rt_log("coop: ent: %lld %s (%s) is no longer among what %04x owns: gone", (long long)ent_ms(), ent_eid_str(e->id, b), e->name, (unsigned)(m->from & 0xffff));
        e->life = LIFE_GONE, e->dead_ms = ent_ms(), e->death_due = e->actor != 0;
        ent_mark_due(e);
    }
}
static void offer_all_in(const CoopCtrMsg *m) {   // the sender loads: every entity of its that is here is claimed (S 5.8)
    static uint8_t seen[ENT_N];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || seen[i] || e->owner != m->from || !e->actor || e->life != LIFE_LIVE) continue;
        Ent *set[64], *mem[ENT_MEMBERS];
        int ns = ent_island_members(e, set, 64), n = 0;
        for (int k = 0; k < ns; k++) if (set[k]->owner == m->from && set[k]->actor && n < ENT_MEMBERS) mem[n++] = set[k], seen[ent_i(set[k])] = 1;
        own_claim(mem, n, "its owner is loading");
    }
}

// ---- Receiving (S 6): one reliable message at phase 0 --------------------------------------------------------------------------------
void own_receive(Cpu *c, const CoopCtrMsg *m) {
    switch (m->type) {
    case ENT_ISLAND: island_in(m); break;
    case ENT_TAKE: take_in(m, 0); break;
    case ENT_GRANT: grant_in(m); break;
    case ENT_REFUSE: refuse_in(m); break;
    case ENT_OFFER_ALL: offer_all_in(m); break;
    case ENT_NEED: need_in(m); break;
    case ENT_BOND_REQ: rec_bond_req(c, m); break;
    case ENT_BOND_ACK: rec_bond_ans(c, m, 1); break;
    case ENT_BOND_NAK: rec_bond_ans(c, m, 0); break;
    case ENT_REQ: req_in(c, m); break;
    case ENT_REQ_ANS: req_ans_in(m); break;
    case ENT_DEATH: death_in(m); break;
    case ENT_INDEX_END: index_end_in(m); break;
    default: break;   // (a type this code does not know is dropped)
    }
}

// ---- The latest-only slots (S 1.1 step 1, 6.1, 6.11, 6.12) -----------------------------------------------------------------------------
static int64_t pose_age_us;   // how long the POSE being taken has been in its slot
static void pose_in(const CoopCtrMsg *m) {
    PoseMsg s;
    int64_t now = ent_ms();
    if (m->len < POSE_LEN(0)) return;
    memset(&s, 0, sizeof s), memcpy(&s, m->d, (size_t)(m->len < (int)sizeof s ? m->len : (int)sizeof s));
    Ent *e = ent_by_id(s.root);
    if (!m->from) return;
    if (!e || (e->st < 0 && !e->st_digest && e->owner != coop_ctr_me())) {   // a root this game has no statement of: NEED after 0.5 s, once a second (S 6.7)
        int f = -1;
        for (int i = 0; i < 32; i++) {
            if (eid_eq(need_root[i].root, s.root) && need_root[i].from == m->from) { f = i; break; }
            if (f < 0 && (!need_root[i].first || now - need_root[i].first > 10000)) f = i;
        }
        if (f < 0) return;
        if (!eid_eq(need_root[f].root, s.root) || need_root[f].from != m->from) need_root[f].root = s.root, need_root[f].from = m->from, need_root[f].first = now, need_root[f].asked = 0;
        if (now - need_root[f].first > 500 && now - need_root[f].asked > 1000) {
            NeedMsg q = {s.root, 0};
            own_send_to(m->from, ENT_NEED, COOP_CTR_RELIABLE, &q, sizeof q), need_root[f].asked = now;
        }
        if (!e) return;
    }
    ent_apply_owner(e, m->from, s.epoch);
    if (e->owner != m->from) return;   // (not its owner after that: dropped)
    e->named = 1;
    Stated *st = e->st >= 0 ? &stated[e->st] : NULL;
    uint8_t offer = (s.flags & POSE_OFFER) != 0;
    int fresh = s.us != e->pose_us;   // (the slot is read every frame: its pose and class are new only when its clock moved)
    e->pose_us = s.us;
    if (fresh) {
        if (st) memcpy(st->root_m, s.m, sizeof s.m), st->pose_ms = now, st->cls = s.cls, st->cls_ms = now, st->offer = offer;
        else memcpy(e->root_m, s.m, sizeof s.m), e->pose_ms = now, e->cls = s.cls, e->cls_ms = now, e->offer = offer;
    }
    if (offer) {   // offered: claimed by a game that has an actor of it (S 5.8)
        Ent *set[64], *mem[ENT_MEMBERS];
        int ns = ent_island_members(e, set, 64), n = 0, here = 0;
        for (int k = 0; k < ns; k++) if (set[k]->owner == m->from && n < ENT_MEMBERS) mem[n++] = set[k], here |= set[k]->actor != 0;
        if (here) { own_claim(mem, n, "offered by its owner"); return; }
    }
    // the drive's samples and the device state, into the group view
    int64_t wnow = frame_step_world_us(), off = wnow - pose_age_us - s.us;   // (the offset at the message's arrival)
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < GRP_N; i++) {
        Grp *g = &grps[i];
        if (!g->used || g->mine || g->owner != m->from || !eid_eq(g->root, s.root)) continue;
        if (s.us > g->hus[2]) {
            g->heard_us = wnow;
            memmove(g->hm[0], g->hm[1], 2 * sizeof g->hm[0]), memmove(g->hus, g->hus + 1, 2 * sizeof g->hus[0]);
            memcpy(g->hm[2], s.m, sizeof s.m), g->hus[2] = s.us;
            if (!g->clock_off_at || off < g->clock_off || wnow - g->clock_off_at > 10000000) g->clock_off = off, g->clock_off_at = wnow;
        }
        uint32_t mine_digest = st ? st->my_digest : e->digest;
        if (mine_digest == s.digest && s.n == g->n) {
            g->sw = s.sw, g->on = s.on, g->fw = s.fw, g->fk = s.fk, g->fb = s.fb, g->fp = s.fp, g->burn = s.burn, g->payer = s.payer;
            memcpy(g->pf, s.pf, sizeof(float) * 4 * (size_t)(s.n > ENT_MEMBERS ? ENT_MEMBERS : s.n));
        } else g->sw = g->on = g->fw = g->fk = g->fb = g->fp = g->burn = 0;   // (the members do not line up: no device state is used)
        break;
    }
    plat_mutex_unlock(&grp_lock);
}
static void applied_in(const CoopCtrMsg *m) {
    AppliedMsg a;
    int p = m->from_id - 1;
    int64_t now = ent_ms();
    if (m->len < (int)sizeof a || p < 0 || p >= ENT_PEERS) return;
    memcpy(&a, m->d, sizeof a);
    Ent *e = ent_by_id(a.root);
    Island *is = e && e->isl >= 0 ? &isls[e->isl] : NULL;
    if (!is || !eid_eq(is->root, a.root)) return;
    is->peer[p].digest = a.digest, is->peer[p].state = a.state, is->peer[p].why = a.why, is->peer[p].ms = now;
    // S 7.5: a peer that differs for 1 s is owed the statement again, once a second at most. Not while an operation
    // is open on the island, and not for a copy that is far, waits for its maker or is being made.
    int differs = (a.state == ST_DIFFER || a.state == ST_NOT_SHOWN) && a.digest != is->digest && !rec_op_open_for(e);
    if (!differs) { is->peer[p].diff_since = 0; return; }
    if (!is->peer[p].diff_since) is->peer[p].diff_since = now;
    if (now - is->peer[p].diff_since > 1000 && now - is->peer[p].owed_ms > 1000) is->owed[p] = 1, is->peer[p].owed_ms = now;
}
static uint32_t index_hash(uint64_t owner, uint32_t *count) {
    uint32_t n = 0, hash = 2166136261u;
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || e->owner != owner || e->life != LIFE_LIVE || (e->flags & ENT_PREVIEW)) continue;
        n++, hash += ent_fnv(ent_fnv(ent_fnv(2166136261u, &e->id.inc, 8), &e->id.serial, 4), &e->epoch, 4);
    }
    *count = n;
    return hash;
}
static void index_in(const CoopCtrMsg *m, uint64_t age_ns) {
    IndexMsg x;
    int p = m->from_id - 1;
    static int64_t seen_us[ENT_PEERS], grant_ms[ENT_PEERS];
    int64_t now = ent_ms();
    (void)grant_ms;
    if (m->len < (int)sizeof x || p < 0 || p >= ENT_PEERS || age_ns > 3000000000ull) return;
    memcpy(&x, m->d, sizeof x);
    if (now - seen_us[p] < 1900) return;   // (each INDEX once: they come every 2 s)
    seen_us[p] = now;
    if (peers[p].taken_counter < x.counter) return;   // (its statements up to that counter are not all here yet)
    if (now - grant_from_ms[p] < 1000) return;         // (ownership is moving between the two games right now)
    uint32_t count, hash = index_hash(m->from, &count);
    if (count == x.count && hash == x.hash) { peers[p].index_miss = 0; return; }
    if (++peers[p].index_miss < 2 || now - peers[p].need_ms < 10000) return;
    peers[p].index_miss = 0, peers[p].need_ms = now, peers[p].need_number++;
    if (!peers[p].need_number) peers[p].need_number = 1;
    for (int i = 0; i < ENT_N; i++) if (ents[i].used && ents[i].owner == m->from) ents[i].named = 0;
    NeedMsg q = {{0, 0}, peers[p].need_number};
    own_send_to(m->from, ENT_NEED, COOP_CTR_RELIABLE, &q, sizeof q);
    rt_log("coop: ent: %lld what %04x owns is not what this game holds for it (%u here, %u there): asked for all of it", (long long)now, (unsigned)(m->from & 0xffff), count, x.count);
}
void own_slots(void) {
    static CoopCtrMsg m;
    uint64_t age = 0;
    int it = 0;
    while (coop_ctr_slot_next(ENT_POSE, &it, &m, &age)) if (age < 2000000000ull) pose_age_us = (int64_t)(age / 1000), pose_in(&m);
    it = 0;
    while (coop_ctr_slot_next(ENT_APPLIED, &it, &m, &age)) if (age < 3000000000ull) applied_in(&m);
    it = 0;
    while (coop_ctr_slot_next(ENT_INDEX, &it, &m, &age)) index_in(&m, age);
}

// ---- Peers coming and going -------------------------------------------------------------------------------------------------------------
void own_peer_events(void) {
    uint64_t inc;
    int id, joined;
    while (coop_ctr_peer_event(&inc, &id, &joined)) {
        int p = id - 1;
        if (p < 0 || p >= ENT_PEERS) continue;
        memset(&peers[p], 0, sizeof peers[p]), asm_buf[p].have = 0, asm_buf[p].counter = 0, index_end_due[p] = 0;
        for (int i = 0; i < ISL_N; i++) {
            if (!isls[i].used) continue;
            isls[i].owed[p] = (uint8_t)(joined != 0), memset(&isls[i].peer[p], 0, sizeof isls[i].peer[p]);
        }
        rt_log("coop: ent: %lld player %d (%04x) %s", (long long)ent_ms(), id, (unsigned)(inc & 0xffff), joined ? "joined: everything owned here is stated to it" : "left");
    }
}

// ---- Step 6: timers whose end changes the world or a request (S 1.1) -------------------------------------------------------------------
void own_timers(Cpu *c) {
    int64_t now = ent_ms();
    uint64_t me = coop_ctr_me();
    // world changes noted by a message or by the net tick
    int end_h = 0, end_r = 0;
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used) continue;
        if (e->end_hold) { e->end_hold = 0; end_h |= ent_this_link_holds(e) || (e->actor && ent_held_actor() == e->actor); }
        if (e->end_ride) { e->end_ride = 0; end_r |= ent_this_link_rides(e); }
    }
    if (end_h) own_end_hold(c, NULL);
    if (end_r) own_end_ride(c);
    if (hold_end.actor) {   // read back: still held 10 frames later, sent again, three times at most
        if (ent_held_actor() != hold_end.actor) hold_end.actor = 0;
        else if (now - hold_end.sent > 170) {
            Ent *e = ent_by_actor(hold_end.actor);
            if (hold_end.tries >= 3 || !e) { rt_log("coop: ent: a hold this game had to end is still there after three refusal messages"); hold_end.actor = 0; }
            else own_end_hold(c, e);
        }
    }
    if (became_n) {   // entities this game came to own: their bonds' strain starts at 0 (S 7.6)
        Ent *mem[64];
        int n = 0;
        for (int i = 0; i < became_n; i++) if (ents[became_mine[i]].used && ents[became_mine[i]].owner == me) mem[n++] = &ents[became_mine[i]];
        became_n = 0;
        if (n) rec_owned_now(c, mem, n);
    }
    // requests: expiry (the cause over, or 1.5 s), and the claim from a silent loading or paused owner (S 5.7)
    for (int i = 0; i < REQ_N; i++) {
        Req *r = &reqs[i];
        if (r->state != RQ_OPEN) continue;
        Ent *e = ent_by_id(r->member);
        int cause = e && (r->reason == TAKE_GRAB ? ent_this_link_holds(e) : ent_this_link_rides(e));
        if (!e || e->owner == me) { req_close(r, RQ_GRANTED); continue; }
        if (!cause) { req_close(r, RQ_EXPIRED); continue; }
        Stated *st = e->st >= 0 ? &stated[e->st] : NULL;
        uint8_t cls = st ? st->cls : e->cls;
        int silent = now - r->opened > 1000, gone = !ent_peer_here(e->owner);
        if (gone || ent_peer_loading(e->owner) || (silent && ent_peer_paused(e->owner) && cls == CLS_IDLE)) {
            Ent *set[64], *mem[ENT_MEMBERS];
            int ns = ent_island_members(e, set, 64), n = 0;
            for (int k = 0; k < ns; k++) if (set[k]->owner == e->owner && n < ENT_MEMBERS) mem[n++] = set[k];
            req_close(r, RQ_GRANTED);
            own_claim(mem, n, gone ? "its owner left" : "its owner is loading, or paused and silent");
            continue;
        }
        if (now - r->opened > 1500) {
            req_close(r, RQ_EXPIRED);
            if (r->reason == TAKE_GRAB) own_end_hold(c, e); else own_end_ride(c);
            continue;
        }
        if (now - r->sent > 300) req_send(r);
    }
    for (int i = 0; i < ASK_N; i++) {   // REQ: sent every 0.5 s for 3 s; a paused or loading owner: claimed, the value applied here
        Ask *a = &asks[i];
        if (!a->used) continue;
        Ent *e = ent_by_id(a->e);
        if (!e || !e->actor) { a->used = 0; continue; }
        if (e->owner == me) { if (a->kind == REQ_TOGGLE) ctn_toggle_put(c, e->actor, (int)a->value); else ctn_event_put(e->actor, e->gen, (uint8_t)a->value); a->used = 0; continue; }
        Stated *st = e->st >= 0 ? &stated[e->st] : NULL;
        uint8_t cls = st ? st->cls : e->cls;
        if (now - a->opened > 1000 && (ent_peer_loading(e->owner) || (ent_peer_paused(e->owner) && cls == CLS_IDLE) || !ent_peer_here(e->owner))) {
            Ent *set[64], *mem[ENT_MEMBERS];
            int ns = ent_island_members(e, set, 64), n = 0;
            for (int k = 0; k < ns; k++) if (set[k]->owner == e->owner && n < ENT_MEMBERS) mem[n++] = set[k];
            own_claim(mem, n, "a switch was asked of an owner that is paused, loading or gone");
            continue;   // (applied on the next pass, as the owner)
        }
        if (now - a->opened > 3000) { a->used = 0; continue; }
        if (now - a->sent > 500) {
            ReqMsg q = {a->id, a->e, a->kind, a->value};
            own_send_to(e->owner, ENT_REQ, COOP_CTR_RELIABLE, &q, sizeof q), a->sent = now;
        }
    }
    // an owner that left: a game with an actor of the island claims after 2 s (S 5.7)
    static uint8_t seen[ENT_N];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || !e->owner || e->owner == me || ent_peer_here(e->owner)) { gone_since[i] = 0; continue; }
        if (!gone_since[i]) gone_since[i] = now;
        if (!e->actor && !e->making && now - gone_since[i] > 10000) { ent_drop(e); continue; }   // (nothing of it here, and nobody to state it)
        if (seen[i] || !e->actor || e->life != LIFE_LIVE || now - gone_since[i] < 2000) continue;
        Ent *set[64], *mem[ENT_MEMBERS];
        int ns = ent_island_members(e, set, 64), n = 0;
        for (int k = 0; k < ns; k++) if (set[k]->owner == e->owner && n < ENT_MEMBERS) mem[n++] = set[k], seen[ent_i(set[k])] = 1;
        own_claim(mem, n, "its owner left the session");
    }
    // frozen islands (S 5.8): after 2 s with no claim, each member whose actor went is stated gone
    for (int i = 0; i < ISL_N; i++) {
        Island *is = &isls[i];
        if (!is->used || !is->frozen || now - is->offer_ms < 2000) continue;
        is->frozen = 0;
        for (int k = 0; k < is->n; k++) {
            Ent *e = &ents[is->m[k]];
            if (e->actor || e->owner != me) continue;
            e->life = e->flags & ENT_MAP ? LIFE_FREE : LIFE_GONE, e->dead_ms = now;
            if (e->flags & ENT_MAP) e->owner = 0;
            own_owe_ent(e, -1);
        }
        ent_mark_due(&ents[is->m[0]]);
    }
    // a DEATH kept for a statement to arrive (2 s)
    for (int i = 0; i < 16; i++) {
        if (!death_wait[i].ms) continue;
        Ent *e = ent_by_id(death_wait[i].e);
        if (e) death_apply(e, death_wait[i].kind), death_wait[i].ms = 0;
        else if (now - death_wait[i].ms > 2000) death_wait[i].ms = 0;
    }
    // S 4.7: a DEAD or GONE run-time entity is stated for 10 s by its owner, then dropped; a follower drops it 10 s
    // after the last statement of it
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        if (!e->used || (e->life != LIFE_DEAD && e->life != LIFE_GONE) || e->actor) continue;
        int64_t since = e->owner == me ? e->dead_ms : e->stated_ms > e->dead_ms ? e->stated_ms : e->dead_ms;
        if (now - since > 10000 && !ent_owed[i] && !e->making) ent_drop(e);
    }
}
void own_unload(Cpu *c, Ent *e) {   // an accepted unload of an owned entity: its island is offered for 2 s (S 5.8)
    (void)c;
    if (e->isl < 0) return;
    Island *is = &isls[e->isl];
    if (!is->frozen) is->frozen = 1, is->offer_ms = ent_ms(), is->class_sends = 3;
}

// ---- Step 7: this game's own causes: a grab or a ride of a thing another game owns (S 5.2) ------------------------------------------
void own_causes(Cpu *c) {
    static uint64_t last_held;
    static int64_t ride_closed;
    uint64_t me = coop_ctr_me(), held = ent_held_actor();
    int64_t now = ent_ms();
    for (int i = 0; i < ENT_N; i++) ents[i].asked = 0;
    for (int i = 0; i < REQ_N; i++) {
        if (reqs[i].state != RQ_OPEN) continue;
        Ent *m = ent_by_id(reqs[i].member), *set[64];
        int n = m ? ent_island_members(m, set, 64) : 0;
        for (int k = 0; k < n; k++) set[k]->asked = 1;
    }
    if (held != last_held) {
        Ent *e = held ? ent_by_actor(held) : NULL;
        if (e || !held) last_held = held;   // (a thing held that is no entity yet is asked again next frame)
        if (e) own_grab(c, e);
    }
    for (int i = 0; i < ENT_N; i++) {   // a stick this Link rides whose island is another game's
        Ent *e = &ents[i];
        if (!e->used || !e->actor || e->owner == me || !e->owner || !strstr(e->name, "Stick") || !ent_this_link_rides(e)) continue;
        if (own_req_open_for(e)) break;
        if (now - ride_closed < 1000) break;
        Req *r = req_open(e, TAKE_RIDE);
        if (r && !r->sent) req_send(r);
        break;
    }
    for (int i = 0; i < REQ_N; i++) if (reqs[i].state != RQ_OPEN && reqs[i].state != RQ_FREE && reqs[i].reason == TAKE_RIDE && reqs[i].closed > ride_closed) ride_closed = reqs[i].closed;
}

// ---- Step 8: what this game says (S 6.1, 6.2, 6.11, 6.12) ---------------------------------------------------------------------------------
void own_send(Cpu *c) {
    int64_t now = ent_ms();
    uint64_t me = coop_ctr_me();
    if (!me) return;
    {   // (a statement owed to a seat nobody sits in is owed to no one: else a dead entity's row is never dropped)
        uint8_t present = 0;
        for (int p = 0; p < ENT_PEERS; p++) { uint64_t inc = coop_ctr_incarnation(p + 1); if (inc && inc != me) present |= (uint8_t)(1u << p); }
        for (int i = 0; i < ENT_N; i++) ent_owed[i] &= present;
    }
    for (int p = 0; p < ENT_PEERS; p++) {
        uint64_t inc = coop_ctr_incarnation(p + 1);
        if (!inc || inc == me) continue;
        int owed_left = 0;
        for (int i = 0; i < ISL_N; i++) {
            Island *is = &isls[i];
            if (!is->used || !is->owed[p] || is->frozen) continue;
            if (stmt_send(c, is, NULL, p)) is->owed[p] = 0, is->stated_digest = is->digest;
            else { owed_left = 1; break; }   // (the queue is full: the rest stays owed, in order)
        }
        for (int i = 0; i < ENT_N && !owed_left; i++) {
            if (!(ent_owed[i] >> p & 1)) continue;
            if (!ents[i].used || ents[i].owner != me) { ent_owed[i] &= (uint8_t)~(1u << p); continue; }
            if (stmt_send(c, NULL, &ents[i], p)) ent_owed[i] &= (uint8_t)~(1u << p);
            else owed_left = 1;
        }
        if (!owed_left && index_end_due[p]) {
            IndexEnd x = {index_end_due[p], index_end_count[p], index_end_hash[p]};
            if (coop_ctr_send(p + 1, ENT_INDEX_END, COOP_CTR_RELIABLE | COOP_CTR_BULK, &x, sizeof x)) index_end_due[p] = 0;
        }
        static int64_t index_ms[ENT_PEERS];
        if (!owed_left && now - index_ms[p] > 2000) {
            IndexMsg x = {0x494e444558ull, peers[p].sent_counter, 0, 0};
            x.hash = index_hash(me, &x.count);
            coop_ctr_send(p + 1, ENT_INDEX, 0, &x, sizeof x), index_ms[p] = now;
        }
    }
    // POSE of each owned island: 30 a second while a member moved in the last second, 1 a second at rest, at once on a
    // change of class
    coop_lock_wait(&grp_lock);
    for (int i = 0; i < ISL_N; i++) {
        Island *is = &isls[i];
        if (!is->used) continue;
        int moving = now - is->moved_ms < 1000;
        if (!is->class_sends && now - is->pose_ms < (moving ? 33 : 1000)) continue;
        Ent *root = &ents[is->m[0]];
        if (!root->actor && !is->frozen) continue;
        PoseMsg s = {0};
        s.key = eid_hash(is->root), s.root = is->root, s.epoch = is->epoch, s.digest = is->digest, s.us = frame_step_world_us();
        memcpy(s.m, root->m, sizeof s.m), s.cls = is->cls, s.n = (uint8_t)is->n, s.flags = is->frozen ? POSE_OFFER : 0, s.payer = me;
        for (int g = 0; g < GRP_N; g++)
            if (grps[g].used && grps[g].mine && eid_eq(grps[g].root, is->root)) {
                Grp *v = &grps[g];
                s.sw = v->sw, s.on = v->on, s.fw = v->fw, s.fk = v->fk, s.fb = v->fb, s.fp = v->fp, s.burn = v->burn, s.payer = v->payer ? v->payer : me;
                memcpy(s.pf, v->pf, sizeof(float) * 4 * (size_t)is->n);
                break;
            }
        send_all(ENT_POSE, 0, &s, POSE_LEN(is->n));
        is->pose_ms = now;
        if (is->class_sends) is->class_sends--;
    }
    plat_mutex_unlock(&grp_lock);
    // APPLIED: once a second per island of another owner here, to all
    static int64_t applied_ms;
    if (now - applied_ms > 1000) {
        applied_ms = now;
        for (int i = 0; i < STATED_N; i++) {
            if (!stated[i].used) continue;
            AppliedMsg a = {eid_hash(stated[i].root), stated[i].root, stated[i].my_digest, stated[i].state, stated[i].why};
            send_all(ENT_APPLIED, 0, &a, sizeof a);
        }
        for (int i = 0; i < ENT_N; i++) {
            Ent *e = &ents[i];
            if (!e->used || e->owner == me || !e->owner || e->st >= 0 || e->life != LIFE_LIVE || !e->st_digest) continue;
            AppliedMsg a = {eid_hash(e->id), e->id, e->digest, e->state, e->why};
            send_all(ENT_APPLIED, 0, &a, sizeof a);
        }
    }
}

// ---- The net tick (S 1.3, 5.4): the pause queue, the answer to a TAKE while paused, repeats that only send -----------------------------
#define PQ_N 1024
static CoopCtrMsg *pq;
static int pq_r, pq_w;
int own_pq_take(CoopCtrMsg *m) {
    while (pq && pq_r != pq_w) {
        CoopCtrMsg *s = &pq[pq_r++ % PQ_N];
        if (!s->type) continue;   // (taken by the net tick already)
        *m = *s;
        return 1;
    }
    return 0;
}
void own_net(void) {
    static int was_loading;
    int64_t now = ent_ms();
    int paused = ent_paused(), loading = ent_loading();
    if (!coop_ctr_me()) return;
    coop_lock_wait(&ent_lock);
    static int64_t offer_all_ms;
    if (loading && (!was_loading || now - offer_all_ms > 500)) {   // every entity of this game is offered at once (S 5.8), and again while it loads
        uint8_t all = 1;
        send_all(ENT_OFFER_ALL, COOP_CTR_RELIABLE, &all, 1), offer_all_ms = now;
    }
    was_loading = loading;
    if (paused || loading) {
        if (!pq) pq = calloc(PQ_N, sizeof *pq);
        static int said;
        while (pq && pq_w - pq_r < PQ_N && coop_ctr_recv(&pq[pq_w % PQ_N])) {
            CoopCtrMsg *m = &pq[pq_w++ % PQ_N];
            if (m->type == ENT_GRANT) grant_in(m), m->type = 0;
            else if (m->type == ENT_REFUSE) refuse_in(m), m->type = 0;
            else if (m->type == ENT_TAKE && paused && !loading) take_in(m, 1), m->type = 0;
        }
        while (pq && pq_r != pq_w && !pq[pq_r % PQ_N].type) pq_r++;   // (handled here already: their places are free again)
        if (pq && pq_w - pq_r >= PQ_N && !said++) rt_log("coop: ent: the pause queue is full (%d messages): the transport waits", PQ_N);
    }
    for (int i = 0; i < ANS_N; i++) {   // the repeat of a GRANT (S 5.3 rule 6)
        Ans *a = &ans[i];
        if (!a->used || !a->send) continue;
        Ent *e = a->n ? ent_by_id(a->m[0]) : NULL;
        int heard = e && (e->epoch > a->epoch || (e->epoch == a->epoch && e->named && e->stated_ms > a->ms) || (e->pose_ms > a->ms && e->owner == a->owner));
        if (heard || now > a->until) { a->send = 0; continue; }
        if (now - a->sent > 500) ans_send(a);
    }
    for (int i = 0; i < ANS_N; i++) {
        int64_t keep = ans[i].kind == ANS_BOND ? 30000 : 10000;
        if (ans[i].used && !ans[i].send && now - ans[i].ms > keep) ans[i].used = 0;
    }
    (void)stmt_number;
    plat_mutex_unlock(&ent_lock);
}
