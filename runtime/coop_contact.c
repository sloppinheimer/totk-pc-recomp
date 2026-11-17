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
                // A device another game runs (followed here), or its shot, striking an enemy the sync shares: the game
                // that runs the device sees the same blow on its own actor of that enemy and counts it there (on its
                // copy, sent here as a hit when this game owns the enemy). Kept here too, the blow counted twice: on a
                // copy its lost life went to the owner a second time; on an enemy this game owns it took the damage
                // here and again from the hit message (code review, 2026-11-17).
                int coop_ctn_emitter_of_followed(uint64_t a);   // coop_ctn.c
                int en_cell_state(uint64_t actor);              // coop_enemy.c
                int victim_owned = en_cell_state(a) == 4;       // (EM_OWNED: run here, a copy in another game)
                if ((victim_copy || victim_owned) && ((w && coop_ctn_emitter_of_followed(w)) || (by && coop_ctn_emitter_of_followed(by)))) {
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
    contacts_filter(c->x[0]);
}
