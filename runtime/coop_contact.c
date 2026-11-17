// A blow's contacts: a stand-in takes no hits, an enemy copy keeps only this game's player's blows; the contact kept for a Zonai device's hit test. Moved out of coop_world.c as
// it was (docs/coop/enemy-sync-spec.md B.28); the packets come in through that file's reader of the HIT channel.
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

// An actor's main physics body ([[comps+0x50]+0x20]+0x150): a contact names the body it struck, and one sent to
// another game says where that body sat from this one.
static uint64_t enemy_body(uint64_t a) {
    uint64_t phys = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    return ctl ? coop_rd_ptr(ctl + 0x150) : 0;
}

// An actor's reference object (+0x40 the actor), from a pointer in the actor's first 0x1000 bytes: what a hit record
// names its attacker by. 0 none found.
uint64_t coop_actor_ref(uint64_t a) {
    static uint64_t w[0x1000 / 8];
    if (!a || !coop_rd(a, w, sizeof w)) return 0;
    for (int i = 0; i < 0x1000 / 8; i++)
        if (w[i] > 0x100000000ull && w[i] < 0x8000000000ull && coop_rd_ptr(w[i] + 0x40) == a) return w[i];
    return 0;
}
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
// Test (TOTK_COOP_TEST_CONTACTS=1): every damage component's hit update with contacts waiting: its actor (dc+0x198),
// and each contact's kind (+0x90), power (+0xa0), body (+0x58) and attacker handles' actors; 40 lines.
static void contacts_test(uint64_t dc) {
    static int on = -1;
    static _Atomic int said;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_CONTACTS") != NULL;
    if (!on || atomic_load(&said) > 40) return;
    uint64_t l = contact_list(dc), base;
    int32_t cnt = 0;
    if (!l || !coop_rd(l + 0x18, &cnt, 4) || cnt <= 0 || !(base = coop_rd_ptr(l + 0x10))) return;
    atomic_fetch_add(&said, 1);
    char an[64] = "", line[600];
    coop_actor_name(coop_rd_ptr(dc + 0x198), an, sizeof an);
    int m = snprintf(line, sizeof line, "coop: test: contacts on %s (%d; its main body %llx):", an, cnt,
                     (unsigned long long)enemy_body(coop_rd_ptr(dc + 0x198)));
    for (int k = 0; k < cnt && k < 3; k++) {
        uint8_t e[0xb0];
        if (!coop_rd(base + (uint64_t)k * CONTACT_SZ, e, sizeof e)) break;
        uint64_t body, r1, r2;
        memcpy(&body, e + 0x58, 8), memcpy(&r1, e + 0x68, 8), memcpy(&r2, e + 0x80, 8);
        char n1[48] = "", n2[48] = "";
        if (r1) coop_actor_name(coop_rd_ptr(r1 + 0x40), n1, sizeof n1);
        if (r2) coop_actor_name(coop_rd_ptr(r2 + 0x40), n2, sizeof n2);
        uint32_t pw;
        memcpy(&pw, e + 0xa0, 4);
        // whose body: the first actor its first 0x200 bytes point to (directly or one level down), and where
        char bn[48] = "?";
        int boff = -1, blv = 0;
        for (int w = 0; w < 0x200 / 8 && boff < 0; w++) {
            uint64_t q = coop_rd_ptr(body + 8 * w);
            for (int lv = 0; lv < 2 && q && boff < 0; lv++) {
                char t[48] = "";
                coop_actor_name(q, t, sizeof t);
                if (t[0] && (!strncmp(t, "Player", 6) || !strncmp(t, "Weapon_", 7) || !strncmp(t, "Armor_", 6)))
                    memcpy(bn, t, sizeof bn), boff = w * 8, blv = lv;
                q = coop_rd_ptr(q);
            }
        }
        m += snprintf(line + m, sizeof line - m, " [kind %u power %u body %llx (%s at +%#x%s) by %s / %s]", e[0x90], pw,
                      (unsigned long long)body, bn, boff, blv ? " ->+0" : "", n1, n2);
    }
    rt_log("%s", line);
    {   // and the first contact's every word, against the main body: a blocked one beside an unblocked one
        static uint8_t e[CONTACT_SZ];
        if (coop_rd(base, e, sizeof e)) {
            char w2[1600];
            int m2 = snprintf(w2, sizeof w2, "coop: test: contact words on %s:", an);
            for (int w = 0; w < CONTACT_SZ / 4 && m2 < 1560; w++) {
                uint32_t v;
                memcpy(&v, e + 4 * w, 4);
                if (v && v != 0xbf800000u) m2 += snprintf(w2 + m2, sizeof w2 - m2, " %x:%x", 4 * w, v);
            }
            rt_log("%s", w2);
        }
    }
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
                // A device another game runs (followed here), or its shot, striking a copy: the game that runs the
                // device sees the same blow and counts it; kept here too, the copy's lost life went to the owner a
                // second time. (A followed device striking an enemy this game runs is still counted in both: [open].)
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
// ---- a hit's contact replayed on a Zonai device (research, TOTK_COOP_TEST_DEVHIT). dev_contact keeps the first contact
// a hit leaves on a device's damage component (its waiting list, as for an enemy) and, once coop_dev_hit names another
// device, copies it onto that one's list at its next hit update, on the body the real hit landed on. The device runs the hit reaction (main+0x1b914e8) but does not turn on: see
// docs/coop/re-notes.md "Device state on the copies".
static _Alignas(16) uint8_t dev_tpl[CONTACT_SZ];
static _Atomic int dev_tpl_ok;
static uint8_t dev_tpl_b1cf;   // the hit device's damage component byte +0x1cf (0x41 on a hit fan, 0 on the others)
static int64_t dev_tpl_brel;   // the hit body from its device's main body: a fan's hit lands on a body of its own
static _Atomic uint64_t dev_hit_want;   // the device to start, 0 none
static void dev_contact(uint64_t dc) {
    uint64_t a = coop_rd_ptr(dc + 0x198), l, base;
    int32_t cnt = 0, cap = 0;
    uint64_t want = atomic_load_explicit(&dev_hit_want, memory_order_relaxed);
    if (!a || (!want && atomic_load_explicit(&dev_tpl_ok, memory_order_relaxed))) return;
    char nm[48];
    if (strncmp(coop_actor_name(a, nm, sizeof nm), "SpObj_", 6)) return;
    if (!(l = contact_list(dc)) || !coop_rd(l + 0x18, &cnt, 4) || !coop_rd(l + 8, &cap, 4) || !(base = coop_rd_ptr(l + 0x10)) || cnt < 0) return;
    if (cnt > 0 && !atomic_load(&dev_tpl_ok)) {
        memcpy(dev_tpl, (const void *)(uintptr_t)base, CONTACT_SZ);
        uint64_t hb;
        memcpy(&hb, dev_tpl + 0x58, 8);
        dev_tpl_brel = (int64_t)(hb - enemy_body(a));
        coop_rd(dc + 0x1cf, &dev_tpl_b1cf, 1);
        atomic_store(&dev_tpl_ok, 1);
        uint64_t r1, r2;
        memcpy(&r1, dev_tpl + 0x68, 8), memcpy(&r2, dev_tpl + 0x80, 8);
        char n1[48] = "", n2[48] = "";
        if (r1) coop_actor_name(coop_rd_ptr(r1 + 0x40), n1, sizeof n1);
        if (r2) coop_actor_name(coop_rd_ptr(r2 + 0x40), n2, sizeof n2);
        rt_log("coop: devhit: a contact on %s kept (%d waiting; by %s with %s, kind %u)", nm, cnt, n2, n1, dev_tpl[0x90]);
    }
    if (want != a || !atomic_load(&dev_tpl_ok) || cnt >= cap) return;
    if (!atomic_compare_exchange_strong(&dev_hit_want, &want, 0)) return;
    uint8_t *e = (uint8_t *)(uintptr_t)(base + (uint64_t)cnt * CONTACT_SZ);
    memcpy(e, dev_tpl, CONTACT_SZ);
    uint64_t body = enemy_body(a) + (uint64_t)dev_tpl_brel;   // (the main body itself took the contact and did nothing)
    memcpy(e + 0x58, &body, 8);
    static uint32_t bump;   // a new contact id (+0x38) each time: one already taken may be passed over
    uint32_t id;
    memcpy(&id, e + 0x38, 4), id += ++bump * 0x1000, memcpy(e + 0x38, &id, 4);
    cnt++;
    memcpy((void *)(uintptr_t)(l + 0x18), &cnt, 4);
    uint8_t was = 0;
    const char *b1 = cfg_get("TOTK_COOP_TEST_DEVHIT1CF");   // =<hex>: that byte written there too (the hit fan had 0x41)
    if (b1 && coop_rd(dc + 0x1cf, &was, 1)) {
        uint8_t v = (uint8_t)strtoul(b1, NULL, 16);
        ST8(dc + 0x1cf, v);
        rt_log("coop: devhit: its +0x1cf %02x -> %02x (the kept hit's device had %02x)", was, v, dev_tpl_b1cf);
    }
    rt_log("coop: devhit: a contact put on %s %llx (body %llx)", nm, (unsigned long long)a, (unsigned long long)body);
}
int coop_dev_hit(uint64_t device) {   // 1: a contact is kept; 0: none yet (put on once the first one is kept)
    atomic_store(&dev_hit_want, device);
    return atomic_load(&dev_tpl_ok);
}
void coop_hit_update(Cpu *c) {
    dev_contact(c->x[0]);
    contacts_test(c->x[0]);
    contacts_filter(c->x[0]);
}
// Test (TOTK_COOP_TEST_HITWATCH=<from>-<to>[@<off>]): from that many seconds into the world to the other, a write
// watch (coop_watch_win.c) on this Link's damage component at +<off> (default 0x1a4, the hit record's state word), 8
// bytes: which game functions write it when an enemy's hit lands (logged when it is taken off).
