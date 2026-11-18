// A blow's contacts: a stand-in takes no hits, an enemy copy keeps only this game's player's blows; the contact kept for a Zonai device's hit test. Moved out of coop_world.c as
// it was (docs/coop/enemy-sync.md B.28); the packets come in through that file's reader of the HIT channel.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int coop_enemy_new(void);   // coop_enemy.c: the enemy sync is on

// An actor's reference object (+0x40 the actor), from a pointer in the actor's first 0x1000 bytes: what a hit record
// names its attacker by. 0 none found.
// A body's hits are built in its damage component's hit update, main+0x300a30, from the contacts waiting in the list
// at [dc+0x1910]+0x50 (count +0x18, capacity +8, 0x1c0-byte records from +0x10; main+0x1cd0c80 runs main+0x1cd263c on
// each): the attack's power (+0xa0), its kind (+0x90), the attacker's handles (+0x60, +0x78 with +0x94 bit 0), and
// the receiving body's own state, through main+0x1cd1b1c, which can cancel the hit (damage 0): a raised shield.
// brel: where the contact's body (+0x58) sat from the proxy's main body (enemy_body), plus 1; 0 not near it (a Link's
// is that body +0xb558, guarding or not, both Links, every run: TOTK_COOP_TEST_CONTACTS, 2026-11-06).
static uint64_t contact_list(uint64_t dc) {
    uint64_t o = coop_rd_ptr(dc + 0x1910);
    return o ? coop_rd_ptr(o + 0x50) : 0;
}
// Whose Link an actor is: 1 this game's, 2 a co-op Link (another player's stand-in here), 0 neither.
static int ct_is_link(uint64_t a) {
    if (!a) return 0;
    if (a == coop_world_me) return 1;
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) if (co[i] == a) return 2;
    return 0;
}
uint32_t coop_sess_frame_no(void);   // coop_sess.c

// The record of a blow on this game's Link that an enemy struck, by its own body or by what it holds, once per attack
// id: the enemy and whether this game runs it; the contact's attacker ([entry+0x80]+0x40) and striking object
// ([entry+0x68]+0x40) with their places, their distances from this Link and the striking object's holder; the enemy's
// ragdoll state ([comps+0x3e8]+0x20); the contact's victim body (+0x58), type, level and the two bytes after
// (+0x90..+0x93), power (+0xa0, an int: main+0x1cd263c multiplies it into the damage), attack id (+0x38) and class
// (+0x0b) (fields: research R14). Step 3's third gate run had a blow on a Link named after a copy that had nothing armed,
// 6.6 m off, while an enemy this game ran stood 2.4 m from the Link with a rising blow armed; the record does not say
// which body struck (research R34 3). 60 lines, and every one of the enemy TOTK_COOP_EN_LOG_KEY names.
static void enemy_blow_log(uint64_t e) {
    int coop_enemy_role(uint64_t actor), coop_enemy_key(uint64_t actor, uint64_t *a, uint32_t *b), coop_enemy_log_all(uint64_t key);
    uint64_t coop_enemy_holder(uint64_t actor);
    uint64_t r1 = coop_rd_ptr(e + 0x68), r2 = coop_rd_ptr(e + 0x80), by = r2 ? coop_rd_ptr(r2 + 0x40) : 0, w = r1 ? coop_rd_ptr(r1 + 0x40) : 0;
    uint64_t en = by && coop_enemy_role(by) ? by : by ? coop_enemy_holder(by) : 0;
    if (!en && w) en = coop_enemy_role(w) ? w : coop_enemy_holder(w);
    if (!en || !coop_world_me) return;
    uint32_t aid = 0;
    coop_rd(e + 0x38, &aid, 4);
    static _Atomic uint32_t seen[16], seen_w;   // (a contact waits in several hit updates: one line per attack id)
    for (int k = 0; aid && k < 16; k++) if (atomic_load_explicit(&seen[k], memory_order_relaxed) == aid) return;
    if (aid) atomic_store_explicit(&seen[atomic_fetch_add(&seen_w, 1) % 16], aid, memory_order_relaxed);
    uint64_t ka = 0;
    uint32_t kb = 0;
    coop_enemy_key(en, &ka, &kb);
    static _Atomic int said;
    if (!coop_enemy_log_all(ka) && atomic_fetch_add(&said, 1) >= 60) return;
    uint8_t tl[4] = {0}, cls = 0;
    int32_t power = 0, rs = -1;
    coop_rd(e + 0x90, tl, 4), coop_rd(e + 0xa0, &power, 4), coop_rd(e + 0x0b, &cls, 1);
    uint64_t rag = coop_rd_ptr(coop_rd_ptr(en + 0x228) + 0x3e8);   // (the enemy's components, then its ragdoll)
    if (rag) coop_rd(rag + 0x20, &rs, 4);
    float lp[3] = {0}, bp[3] = {0}, wp[3] = {0};
    coop_rd(coop_world_me + COOP_ACTOR_POS, lp, 12);
    if (by) coop_rd(by + COOP_ACTOR_POS, bp, 12);
    if (w) coop_rd(w + COOP_ACTOR_POS, wp, 12);
    float db = sqrtf((bp[0] - lp[0]) * (bp[0] - lp[0]) + (bp[1] - lp[1]) * (bp[1] - lp[1]) + (bp[2] - lp[2]) * (bp[2] - lp[2]));
    float dw = sqrtf((wp[0] - lp[0]) * (wp[0] - lp[0]) + (wp[1] - lp[1]) * (wp[1] - lp[1]) + (wp[2] - lp[2]) * (wp[2] - lp[2]));
    uint64_t wh = w ? coop_enemy_holder(w) : 0;
    char en_n[48], by_n[48], w_n[48], wh_n[48];
    rt_log("coop: contact: this Link struck by the enemy %s %016llx (%s): attacker %s at %.2f %.2f %.2f, %.1f m from this Link; "
           "striking object %s at %.2f %.2f %.2f, %.1f m, held by %s; the enemy's ragdoll state %d; victim body %llx, type %u level %u, %u %u, power %d, attack id %u, class %u",
           coop_actor_name(en, en_n, sizeof en_n), (unsigned long long)ka, coop_enemy_role(en) == 2 ? "a copy here" : "this game decides for it",
           by ? coop_actor_name(by, by_n, sizeof by_n) : "-", bp[0], bp[1], bp[2], by ? db : -1.f,
           w ? coop_actor_name(w, w_n, sizeof w_n) : "-", wp[0], wp[1], wp[2], w ? dw : -1.f, wh ? coop_actor_name(wh, wh_n, sizeof wh_n) : "nothing",
           (int)rs, (unsigned long long)coop_rd_ptr(e + 0x58), tl[0], tl[1], tl[2], tl[3], (int)power, aid, cls);
}

static void contacts_filter(uint64_t dc) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_CONTACT_FILTER"); on = !(e && *e == '0'); }
    if (!on || !coop_net_on()) return;
    uint64_t a = coop_rd_ptr(dc + 0x198), l, base;
    int32_t cnt = 0, cap = 0;
    // (the list first: it is empty in most hit updates, and the copy check reads more; the count clamped to the
    // capacity as contact_send does; code review, 2026-11-07)
    if (!(l = contact_list(dc)) || !coop_rd(l + 0x18, &cnt, 4) || cnt <= 0 || !coop_rd(l + 8, &cap, 4) || cap <= 0) return;
    if (cnt > cap) cnt = cap;
    if (cnt > 64 || !(base = coop_rd_ptr(l + 0x10))) return;
    // A PlayerPuppet stand-in takes no hit here at all (TotK Online deletes every contact with an actor another game
    // owns): its owner's game has the attack on the real Link from its copy of the enemy. Hit here, it played its own
    // reaction, ended in a T pose and put its sword on its back (pair test, attack_enemy_3rd.txt, 2026-11-07).

    if (ct_is_link(a) == 1 && coop_enemy_new()) {
        // This game's Link is the one struck: how each blow of an enemy's copy ended is told to the copy's owner (the
        // entry's class byte +0x0b, set by the game when it builds the entry: 0 a hit, 1 guarded, 3 guarded at the
        // right moment, 4 avoided; research R11). The owner's game has only a stand-in of this player and learns
        // nothing of it by itself.
        void coop_enemy_outcome(uint64_t attacker, int cls, uint32_t attack_id);   // coop_enemy_hit.c
        for (int k = 0; k < cnt; k++) {
            uint64_t e = base + (uint64_t)k * CONTACT_SZ, r2 = coop_rd_ptr(e + 0x80), by = r2 ? coop_rd_ptr(r2 + 0x40) : 0;
            uint8_t cls = 0;
            uint32_t aid = 0;
            enemy_blow_log(e);
            if (by && coop_rd(e + 0x0b, &cls, 1) && coop_rd(e + 0x38, &aid, 4)) coop_enemy_outcome(by, cls, aid);
        }
    }
    if (ct_is_link(a) == 2) {
        int32_t zero = 0;
        memcpy((void *)(uintptr_t)(l + 0x18), &zero, 4);
        static _Atomic int said_s;
        if (atomic_fetch_add(&said_s, 1) < 10) rt_log("coop: contact on a stand-in: %d taken out (its owner's game counts them)", cnt);
        return;
    }
    {   // An enemy the enemy sync of coop_enemy.c knows (its own, a copy, or unsynced): a stand-in's hits on it are taken
        // out. The game of that stand-in's player has the same hit on its own actor of the enemy and states it; left
        // in, a watching game took the life from its copy too and sent it to the owner as its own player's hit, and
        // every blow counted twice (two-game run, 2026-11-15).
        int en_cell_state(uint64_t actor);   // coop_enemy.c
        if (coop_enemy_new() && en_cell_state(a)) {
            // On a COPY, the hits of other enemies are taken out too (an enemy's own blow or what it holds, and a
            // projectile it shot, in this game a copy's or this game's own): the game that runs the struck enemy has
            // the same blow on its own actor from its own actor of the attacker, and that one counts. Left in, the
            // life this copy lost was sent to its owner as this game's player's hit, and the blow counted twice.
            int coop_enemy_role(uint64_t actor), coop_enemy_shot_mirror(uint64_t proj);
            uint64_t coop_enemy_holder(uint64_t actor);
            int keep = 0, victim_copy = coop_enemy_role(a) == 2;
            for (int k = 0; k < cnt; k++) {
                uint64_t e = base + (uint64_t)k * CONTACT_SZ, r1 = coop_rd_ptr(e + 0x68), r2 = coop_rd_ptr(e + 0x80), by = r2 ? coop_rd_ptr(r2 + 0x40) : 0, w = r1 ? coop_rd_ptr(r1 + 0x40) : 0;
                if (ct_is_link(by) == 2) continue;
                if (victim_copy && ((by && by != a && (coop_enemy_role(by) || coop_enemy_holder(by))) || (w && (coop_enemy_holder(w) || coop_enemy_shot_mirror(w))))) {
                    static _Atomic int said_e;
                    char an[48], bn[48], wn[48];
                    if (atomic_fetch_add(&said_e, 1) < 20) rt_log("coop: contact on the copy of %s by another enemy (%s with %s): taken out (the game that runs the struck one counts it)", coop_actor_name(a, an, sizeof an), by ? coop_actor_name(by, bn, sizeof bn) : "-", w ? coop_actor_name(w, wn, sizeof wn) : "-");
                    continue;
                }
                // A device another game runs (followed here), or its shot, striking a copy: the game that runs the
                // device sees the same blow and counts it; kept here too, the copy's lost life went to the owner a
                // second time. (A followed device striking an enemy this game runs is still counted in both: [open]. It
                // is not left to the device's game, which has no copy of the enemy when it has not loaded it, and the
                // blow would then do nothing at all; a run settles which happens, PLAN.md step 0.)
                int coop_ctn_emitter_of_followed(uint64_t a);   // coop_ctn.c
                if (victim_copy && ((w && coop_ctn_emitter_of_followed(w)) || (by && coop_ctn_emitter_of_followed(by)))) {
                    static _Atomic int said_d;
                    char an[48];
                    if (atomic_fetch_add(&said_d, 1) < 10) rt_log("coop: contact on the copy of %s by a followed device: taken out (the game that runs the device counts it)", coop_actor_name(a, an, sizeof an));
                    continue;
                }
                if (keep != k) memmove((void *)(uintptr_t)(base + (uint64_t)keep * CONTACT_SZ), (void *)(uintptr_t)e, CONTACT_SZ);
                keep++;
            }
            if (keep != cnt) memcpy((void *)(uintptr_t)(l + 0x18), &keep, 4);
            return;
        }
    }
}
void coop_hit_update(Cpu *c) {
    void coop_check_hits(uint64_t dc);   // coop_check.c: the mismatch checker's record, before any is taken out
    coop_check_hits(c->x[0]);
    contacts_filter(c->x[0]);
}
