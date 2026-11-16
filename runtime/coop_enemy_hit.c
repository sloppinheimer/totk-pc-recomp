// Co-op: the enemy sync's hits and deaths (docs/coop/enemy-sync-spec.md sections 8 and 9).
//
// Life. An enemy's life points are its owner's: a copy's are written from the owner's states before every calc.
// A hit is decided in the game that saw it: the copy's damage step runs there as the game wrote it (so the blow, the
// flinch and the numbers show at once); what the copy's life lost since it was last written is that game's hits, sent
// to the owner (HIT, reliable), and for a second and a half the copy shows the lower of the stated life and the life
// after that hit, so the life does not rise again while the hit travels. The owner takes the same points from its enemy; at 0 it asks the game for the death.
//
// Death. It is the owner's enemy that dies (its drops, its kill record). The owner states its death word in every
// STATE and DEATH once, reliably, when the word becomes valid; every game then keeps a tombstone for the placement. A
// copy never dies by itself (its death component refuses every request but this file's and reserves no drops): when
// the owner's word is valid the copy's death is asked for with the owner's causes, it waits at its third phase as a
// switched-off enemy's does, and it is given the signal the action graph would give when the owner's death has passed
// that phase. The copy's own death tick then ends it. An actor this game's placement makes at a tombstoned place is
// put away by the placement's own dead call and a delete request.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"

#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

#define SEC 1000000000ull
#define FN_DEATH_REQ 0x1b4b738      // (death component, cause, second cause, flag)
#define FN_SET_DEAD 0x195321c       // (actor, flag): the placement stays away; flag bit 0 writes the save

typedef struct { Enid id; uint32_t epoch, serial; int32_t dmg; uint32_t pad; } MHit;
typedef struct { Enid id; uint32_t dstate; uint8_t kill, end, pad[2]; } MDeath;

static uint32_t n_hits_sent, n_hits_taken, n_hits_lost, n_deaths;

static uint64_t dc_of(uint64_t actor) { return coop_rd_ptr(en_comps(actor) + 0x1c8); }
static uint64_t hp_word(uint64_t actor) {
    uint64_t l = coop_rd_ptr(en_comps(actor) + 0x2f8), h = l ? coop_rd_ptr(l + 0x1830) : 0;
    return h ? h + 8 : 0;
}
static void hp_write(uint64_t actor, int32_t v) {
    uint64_t p = hp_word(actor);
    int32_t was = 0;
    if (p && coop_rd(p, &was, 4) && was != v) memcpy((void *)(uintptr_t)p, &v, 4);
}

// ---- The actor's own thread ---------------------------------------------------------------------------------------------
void en_hit_leave(uint64_t actor, EnCell *m) {
    uint64_t dc = dc_of(actor);
    uint32_t w = 0;
    uint16_t sw = 0;
    if (!dc || !m->dc_saved || !coop_rd(dc + 0x20, &w, 4) || !coop_rd(dc + 0x2c, &sw, 2)) { m->dc_saved = 0; return; }
    if (!(w & 1)) {   // alive: it makes its drops and takes requests as before; dying: the owner's death made the drops
        sw = (uint16_t)((sw & ~0x81u) | m->dc_bits);
        memcpy((void *)(uintptr_t)(dc + 0x2c), &sw, 2);
    }
    m->dc_saved = 0, m->req_tries = 0;
}

void en_hit_precalc(Cpu *c, uint64_t actor, EnCell *m, int off) {
    uint64_t dc = dc_of(actor);
    if (atomic_load_explicit(&m->flinch, memory_order_relaxed)) {   // a flinch not of this game's making: asked as the damage step asks
        uint64_t rag = coop_rd_ptr(en_comps(actor) + 0x3e8);
        atomic_store(&m->flinch, 0);
        if (rag) __atomic_fetch_or((uint8_t *)(uintptr_t)(rag + 0x149), 1, __ATOMIC_SEQ_CST);
    }
    if (!off) {
        if (atomic_load_explicit(&m->kill_req, memory_order_relaxed) && dc) {   // the owner's enemy lost its last life to another game's hit
            uint32_t w = 0;
            if (coop_rd(dc + 0x20, &w, 4) && !(w & 1) && m->req_tries < 120) {
                uint64_t x[31], sp = c->sp, pc = c->pc;
                memcpy(x, c->x, sizeof x);
                uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + FN_DEATH_REQ, dc, 0x10, 1, 0);
                memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
                if (!m->req_tries++) rt_log("coop: en: death: asked of this game's %llx at 0 life (the request answered %llu)", (unsigned long long)actor, (unsigned long long)(r & 1));
            } else atomic_store(&m->kill_req, 0), m->req_tries = 0;
        }
        if (atomic_load_explicit(&m->put_away, memory_order_relaxed) && !m->put_done) {
            uint32_t w = 0;
            int32_t ast = 0;
            m->put_done = 1;
            // (not one that is dying by itself; the delete only of a ready actor with its lock object, as coop_ctn.c part_delete)
            if ((!dc || (coop_rd(dc + 0x20, &w, 4) && !(w & 1))) && coop_rd(actor + COOP_ACTOR_STATE, &ast, 4) && ast >= 2 && ast <= 6 && coop_rd_ptr(actor + 0x1a8)) {
                uint64_t x[31], sp = c->sp, pc = c->pc;
                memcpy(x, c->x, sizeof x);
                coop_guest_call(c, MOD_MAIN_BASE + FN_SET_DEAD, actor, 1, 0, 0);
                uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, actor, 0, 0, 0);
                memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
                rt_log("coop: en: death: %llx stands at a placement whose enemy died in another game: put away (the delete answered %llu)", (unsigned long long)actor, (unsigned long long)(r & 1));
            }
        }
        return;
    }
    // A copy: its life is its owner's.
    // What the copy lost since its life was last written is this game's hits: the game takes a hit's damage from the
    // life in the calc after the damage step that found it (two-game run: the life did not change inside the step).
    int32_t hp = m->show_hp, cur = en_hp_of(actor);
    if (m->hp_written > 0 && cur >= 0 && cur < m->hp_written) {
        atomic_fetch_add(&m->hit_dmg, m->hp_written - cur);
        m->hit_hp = cur, m->hit_ns = en_now();
    }
    if (en_now() - m->hit_ns < 3 * SEC / 2 && m->hit_hp < hp) hp = m->hit_hp;
    if (hp >= 0 && !en_test_skip('h')) hp_write(actor, hp), m->hp_written = hp;
    if (!dc) return;
    uint32_t w = 0, want = m->show_dstate;
    uint16_t sw = 0;
    if (!coop_rd(dc + 0x20, &w, 4) || !coop_rd(dc + 0x2c, &sw, 2)) return;
    if (!m->dc_saved) m->dc_saved = 1, m->dc_bits = (uint8_t)(sw & 0x81);
    if (!(w & 1) && (sw & 0x81) != 0x81 && !en_test_skip('d')) sw |= 0x81, memcpy((void *)(uintptr_t)(dc + 0x2c), &sw, 2);   // no drops, no request but this file's
    int dying = atomic_load_explicit(&m->dying, memory_order_relaxed) || (want & 1);
    if (dying && !(w & 1) && m->req_tries < 60) {
        uint32_t cause = want & 1 ? (want >> 4) & 0x1f : 0x10, second = want & 1 ? (want >> 9) & 0x1f : 1, flag = want & 1 ? !((want >> 15) & 1) : 0;
        uint16_t open = (uint16_t)(sw & ~0x80u);
        uint64_t x[31], sp = c->sp, pc = c->pc;
        memcpy(x, c->x, sizeof x);
        memcpy((void *)(uintptr_t)(dc + 0x2c), &open, 2);
        uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + FN_DEATH_REQ, dc, cause, second, flag);
        memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
        if (coop_rd(dc + 0x2c, &sw, 2)) sw |= 0x81, memcpy((void *)(uintptr_t)(dc + 0x2c), &sw, 2);
        if (!m->req_tries++ || (r & 1)) rt_log("coop: en: death: asked of the copy %llx (cause %u, second %u, flag %u): the request answered %llu", (unsigned long long)actor, cause, second, flag, (unsigned long long)(r & 1));
        coop_rd(dc + 0x20, &w, 4);
    }
    // The signal the action graph gives when the Die action has run: the owner's death has passed its third phase.
    if ((w & 1) && (w & 0xe) == 6 && (want & 1) && ((want >> 1) & 7) >= 4) {
        uint32_t to = (w & ~0xeu) | 8;
        if (__atomic_compare_exchange_n((uint32_t *)(uintptr_t)(dc + 0x20), &w, to, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            rt_log("coop: en: death: the copy %llx is given the signal (its owner's death is in phase %u)", (unsigned long long)actor, (want >> 1) & 7);
    }
}

// ---- Phase 0 --------------------------------------------------------------------------------------------------------------
void en_dead(En *e, const char *why) {
    MDeath d;
    memset(&d, 0, sizeof d);
    d.id = e->id, d.end = 1;
    en_send_all(ENM_DEATH_END, &d, sizeof d);
    en_log_own(e, "dead", why);
    en_entry_free(e);
}

void en_hit_phase0(void) {
    uint64_t me = coop_ctr_me(), now = en_now();
    {   // Test (TOTK_COOP_EN_TEST_KILL=<s>): that long after this game first ran an enemy, its first one near this Link
        // loses all its life as if to another player's hit.
        static int at = -1, done;
        static uint64_t first_ns;
        if (at < 0) { const char *v = cfg_get("TOTK_COOP_EN_TEST_KILL"); at = v ? atoi(v) : 0; }
        uint64_t link = coop_player_actor();
        float lp[3];
        for (int i = 0; at && !done && link && coop_rd(link + COOP_ACTOR_POS, lp, 12) && i < EN_N; i++) {
            En *e = &en_tab[i];
            if (!e->used || e->owner != me || e->life != EN_LIVE || e->cell < 0) continue;
            if (!first_ns) first_ns = now;
            if (now - first_ns < (uint64_t)at * SEC) break;
            float dx = e->pose[0] - lp[0], dz = e->pose[2] - lp[2];
            if (dx * dx + dz * dz > 60.f * 60.f) continue;
            e->pend_dmg += 100000, done = 1;
            rt_log("coop: en: test: %s %016llx is made to lose all its life", e->name, (unsigned long long)e->id.a);
        }
    }
    {   // Test (TOTK_COOP_EN_TEST_HIT=<s>,<points>): that long after this game first had a copy, it sends a hit of that
        // many points to the owner of its first live copy, as if its player had struck it.
        static int at = -1, pts, done;
        static uint64_t first_ns;
        if (at < 0) { const char *v = cfg_get("TOTK_COOP_EN_TEST_HIT"); at = v ? atoi(v) : 0; const char *k = v ? strchr(v, ',') : NULL; pts = k ? atoi(k + 1) : 1000; }
        for (int i = 0; at && !done && i < EN_N; i++) {
            En *e = &en_tab[i];
            if (!e->used || e->owner == me || e->life != EN_LIVE || e->cell < 0) continue;
            if (!first_ns) first_ns = now;
            if (now - first_ns < (uint64_t)at * SEC) break;
            atomic_fetch_add(&en_map[e->cell].hit_dmg, pts), done = 1;
            rt_log("coop: en: test: a hit of %d points is made up on the copy of %s %016llx", pts, e->name, (unsigned long long)e->id.a);
        }
    }
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->life == EN_UNSYNCED || e->cell < 0) continue;
        EnCell *m = &en_map[e->cell];
        if (e->owner != me) {
            int32_t d = atomic_exchange(&m->hit_dmg, 0);
            if (e->life == EN_DYING) atomic_store(&m->dying, 1);
            if (d <= 0) continue;
            MHit h = { e->id, e->epoch, ++e->hit_serial, d, 0 };
            en_send(en_peer_id(e->owner), ENM_HIT, &h, sizeof h);
            e->my_hit_ns = now, n_hits_sent++;
            rt_log("coop: en: hit: this player took %d life from the copy of %s %016llx (hit %u): sent to its owner", d, e->name, (unsigned long long)e->id.a, h.serial);
            continue;
        }
        if (e->pend_dmg > 0 && e->life == EN_LIVE) {
            int32_t hp = en_hp_of(e->actor), to = hp - e->pend_dmg;
            if (to < 0) to = 0;
            if (hp > 0) {
                hp_write(e->actor, to);
                atomic_store(&m->flinch, 1);
                if (!to) atomic_store(&m->kill_req, 1);
                rt_log("coop: en: hit: another player's hits take %d life from %s %016llx: %d -> %d", e->pend_dmg, e->name, (unsigned long long)e->id.a, hp, to);
            }
        }
        e->pend_dmg = 0;
        uint64_t dc = dc_of(e->actor);
        uint32_t w = 0;
        if (dc && coop_rd(dc + 0x20, &w, 4) && (w & 1) && (w & 0xe) && e->life == EN_LIVE) {   // its death began (valid, and in a phase)
            MDeath d;
            memset(&d, 0, sizeof d);
            d.id = e->id, d.dstate = w, d.kill = 1;
            e->life = EN_DYING, e->offered = 0, n_deaths++;
            if (e->id.b != EN_RT) en_tomb_add(e->id.a, e->id.b, 1);   // (a placement's: one made at run time has none to keep away)
            en_send_all(ENM_DEATH, &d, sizeof d);
            en_log_own(e, "dying", "its death began in this game");
        }
    }
    static uint64_t said;
    if (now - said >= 10 * SEC && (n_hits_sent || n_hits_taken || n_deaths)) {
        said = now;
        rt_log("coop: en: hits: %u sent to owners, %u taken from other players, %u not applied (not this game's enemy); %u deaths of this game's enemies", n_hits_sent, n_hits_taken, n_hits_lost, n_deaths);
    }
}

// ---- Messages (the net tick) ------------------------------------------------------------------------------------------------
void en_hit_msg(const CoopCtrMsg *m, int from) {
    uint64_t me = coop_ctr_me();
    switch (m->type) {
    case ENM_HIT: {
        const MHit *h = (const MHit *)m->d;
        if (m->len != (int)sizeof *h) break;
        En *e = NULL;
        for (int i = 0; i < EN_N; i++) if (en_tab[i].used && enid_eq(en_tab[i].id, h->id)) e = &en_tab[i];
        if (!e || e->owner != me || e->life != EN_LIVE || h->dmg <= 0 || h->dmg > 100000) { n_hits_lost++; break; }
        e->pend_dmg += h->dmg, n_hits_taken++;
        e->hit_by = from, e->hit_by_ns = en_now();
        break;
    }
    case ENM_DEATH: case ENM_DEATH_END: {
        const MDeath *d = (const MDeath *)m->d;
        if (m->len != (int)sizeof *d) break;
        if (d->id.b != EN_RT && d->id.b >= en_life_no()) en_tomb_add(d->id.a, d->id.b, 1);
        for (int i = 0; i < EN_N; i++) {
            En *e = &en_tab[i];
            if (!e->used || !enid_eq(e->id, d->id) || e->life == EN_UNSYNCED) continue;
            if (m->type == ENM_DEATH) {   // a death is a fact, whoever states it (S 9.2)
                if (e->life == EN_LIVE) e->life = EN_DYING, en_log_own(e, "dying", "its death was stated");
                if (e->cell >= 0) atomic_store(&en_map[e->cell].dying, 1);
            } else {
                en_log_own(e, "dead", "its owner's actor is gone");
                en_entry_free(e);   // (a copy still dying here ends by itself: it thinks again and its Die action runs)
            }
        }
        break;
    }
    default: break;
    }
}
