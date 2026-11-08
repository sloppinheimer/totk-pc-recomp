// Co-op: the devices of one contraption (an autobuild's, or any bonded together) made one unit in the other games
// too. TotK 1.4.3 main offsets; docs/coop/re-notes.md, "Autobuild copies", has how each was found.
//
// The game's contraption: each device's SpecialPowerReceiverComponent ([[actor+0x228]+0x480]) +0x28 points to one
// group object all its devices share (pooled; vtable main+0x361f1b0). The game fills it from its connection graph:
// bond edges between the devices' bodies (an autobuild makes them in an actor update, main+0x1c05b84 ->
// main+0x1916830), each frame's graph job (main+0x181e40) joins the bonded bodies into one set and gives every member
// that set's group (main+0x1d93978). With the group, a hit on one fan turns on all of them (each fan's behaviour graph
// reads it), as in the builder's game. A copy made by name has no bond and no group: one hit there turned one copy
// fan on (the tester's report, 2026-11-08).
//
// Here:
// - the owner (any game) sends, once a second, the groups its own sent objects form (two or more members), each member
//   by its number across the games (coop_players.c coop_obj_ident) and name ("CB" packets). A member stays listed 3 s
//   after it was last seen in its group: the field reads 0 at times (the hit fan's, for a while after a hit).
// - another game bonds its copies of a group's members to the first one (by number) found here, once each, 2 s after
//   the group was first heard, with the game's own call
//   main+0x1c05a98 (the member's glue component, its body index 0, the first member's main body, both bodies' world
//   matrices (body +0x98, 3x4 rows); it takes the bond's place from the two bodies, main+0x299daec, and makes the bond
//   as main+0x1c05b84). A body whose +0x68 has bit 24 set is skipped by a bond (main+0xa2d39c; every copy's is set
//   here and no original's, cause not found): cleared on both first. The graph then gives the copies one group of
//   their own (autobuild_3rd.txt pair test: the proxy's hit turned on all three copy fans).
// - a bonded copy no longer in its group in the owner's list for 5 s is made again (coop_copy_remake): no call to undo
//   such a bond is known.
// Only copies are bonded (a game's own objects are left as they are). TOTK_COOP_BONDS=0 turns it off.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"

#define SPR_COMP 0x480             // [actor+0x228]+: the SpecialPowerReceiverComponent
#define SPR_GROUP 0x28             // its contraption group
#define BOND_BODIES 0x1c05a98      // (component, body index, other body, world matrix, other's world matrix)
#define BODY_SKIP_BIT (1ull << 24) // body +0x68: a bond skips such a body (main+0xa2d39c)
#define BONDS_KIND 0x4243          // "CB"

int coop_own_sent_objects(uint64_t *out, int max);   // coop_players.c
int coop_is_copy(uint64_t a);
void coop_copy_remake(Cpu *c, uint64_t a);
int coop_obj_ident(uint64_t a, int *owner, uint32_t *id);
uint64_t coop_obj_find(int owner, uint32_t id, const char *name);

#define NAME_N 40
typedef struct { uint32_t owner, id; char name[NAME_N]; } BondObj;
typedef struct { uint32_t group, pad; BondObj o; } BondEntry;
#define ENTRY_N ((COOP_HIT_MAX - 16) / (int)sizeof(BondEntry))
typedef struct { uint32_t kind, n, pad[2]; BondEntry e[ENTRY_N]; } BondPacket;
_Static_assert(sizeof(BondPacket) <= COOP_HIT_MAX, "bond packet fits");

static int bonds_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_BONDS"); on = !(e && *e == '0'); }
    return on;
}
static uint64_t comp_of(uint64_t a) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0;
    return t ? coop_rd_ptr(t + SPR_COMP) : 0;
}
static uint64_t main_body(uint64_t a) {   // [[[actor+0x228]+0x50]+0x20]+0x150
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, ph = t ? coop_rd_ptr(t + 0x50) : 0, ctl = ph ? coop_rd_ptr(ph + 0x20) : 0;
    return ctl ? coop_rd_ptr(ctl + 0x150) : 0;
}
static uint32_t actor_uid(uint64_t a) {   // [actor+0x10]: another actor made later in a gone one's memory has another
    uint32_t id = 0;
    return a && coop_rd(a + 0x10, &id, 4) ? id : 0;
}
static int alive(uint64_t a, uint32_t uid) {
    int32_t st = 0;
    return a && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 && actor_uid(a) == uid;
}
static uint64_t call5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {   // as coop_guest_call
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 5, a0, a1, a2, a3, a4);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}

// ---- the owner: its groups, sent once a second
#define MEMBER_N 128
static struct { uint64_t actor, group; uint32_t uid; int64_t seen_us; } member[MEMBER_N];
static void send_groups(int64_t now) {
    uint64_t own[256];
    int n = coop_own_sent_objects(own, 256);
    for (int i = 0; i < n; i++) {
        uint64_t a = own[i], comp = comp_of(a), g = comp ? coop_rd_ptr(comp + SPR_GROUP) : 0;
        if (!g || coop_is_copy(a)) continue;
        int f = -1, free = -1;
        for (int k = 0; k < MEMBER_N && f < 0; k++) {
            if (member[k].actor == a && member[k].uid == actor_uid(a)) f = k;
            else if (free < 0 && (!member[k].actor || now - member[k].seen_us > 3000000)) free = k;
        }
        if (f < 0 && (f = free) < 0) continue;
        member[f].actor = a, member[f].uid = actor_uid(a), member[f].group = g, member[f].seen_us = now;
    }
    static BondPacket p;
    memset(&p, 0, sizeof p);
    p.kind = BONDS_KIND;
    static int said;
    for (int k = 0; k < MEMBER_N; k++) {   // each group with two or more members listed, whole, in member order
        if (!member[k].actor || now - member[k].seen_us > 3000000 || !alive(member[k].actor, member[k].uid)) continue;
        int first = 1, count = 0;
        for (int j = 0; j < MEMBER_N; j++)
            if (member[j].actor && member[j].group == member[k].group && now - member[j].seen_us <= 3000000 && alive(member[j].actor, member[j].uid)) {
                if (j < k) first = 0;
                count++;
            }
        if (!first || count < 2) continue;
        if ((int)p.n + count > ENTRY_N) {   // the next packet
            if (p.n) coop_net_hit_send(&p, (int)(offsetof(BondPacket, e) + p.n * sizeof(BondEntry)));
            p.n = 0;
            if (count > ENTRY_N) continue;
        }
        char names[256] = "";
        for (int j = 0; j < MEMBER_N; j++) {
            if (!member[j].actor || member[j].group != member[k].group || now - member[j].seen_us > 3000000 || !alive(member[j].actor, member[j].uid)) continue;
            BondEntry *e = &p.e[p.n];
            int owner = 0;
            if (!coop_obj_ident(member[j].actor, &owner, &e->o.id)) continue;
            e->o.owner = (uint32_t)owner, e->group = (uint32_t)(member[k].group >> 3);
            coop_actor_name(member[j].actor, e->o.name, sizeof e->o.name);
            if (said < 10 && strlen(names) < 200) snprintf(names + strlen(names), sizeof names - strlen(names), " %s", e->o.name);
            p.n++;
        }
        if (said < 10) said++, rt_log("coop: bonds: a contraption of mine, sent:%s", names);
    }
    if (p.n) coop_net_hit_send(&p, (int)(offsetof(BondPacket, e) + p.n * sizeof(BondEntry)));
}

// ---- the others' groups: copies bonded here
#define PAIR_N 128
static struct {
    int used, from, tries, done;
    uint32_t group;
    BondObj m, root;               // the member, and the member it is bonded to (its group's first found here)
    uint64_t ma, ra;               // their copies here
    uint32_t mu, ru;               // ... and their ids
    int64_t heard_us, tried_us, first_us;
} pair[PAIR_N];
static int entry_cmp(const void *x, const void *y) {
    const BondEntry *a = x, *b = y;
    return a->o.owner != b->o.owner ? (a->o.owner < b->o.owner ? -1 : 1) : a->o.id < b->o.id ? -1 : a->o.id > b->o.id;
}
static void take_group(int from, uint32_t group, BondEntry *e, int n, int64_t now) {
    qsort(e, (size_t)n, sizeof *e, entry_cmp);   // the same first member, whatever order the owner listed them in
    uint64_t root = 0;
    int r = -1;
    for (int i = 0; i < n && !root; i++) {
        uint64_t a = coop_obj_find((int)e[i].o.owner, e[i].o.id, e[i].o.name);
        if (a && coop_is_copy(a)) root = a, r = i;
    }
    if (!root) return;
    for (int i = 0; i < n; i++) {
        if (i == r) continue;
        uint64_t a = coop_obj_find((int)e[i].o.owner, e[i].o.id, e[i].o.name);
        if (!a || !coop_is_copy(a)) continue;
        int f = -1, free = -1;
        for (int k = 0; k < PAIR_N && f < 0; k++) {
            if (pair[k].used && pair[k].from == from && pair[k].group == group && pair[k].ma == a && pair[k].mu == actor_uid(a)) f = k;
            else if (!pair[k].used && free < 0) free = k;
        }
        if (f >= 0 && (pair[f].ra != root || pair[f].ru != actor_uid(root))) {   // its group's first copy is another now
            pair[f].used = 0;
            f = -1;
        }
        if (f < 0) {
            if ((f = free) < 0) continue;
            memset(&pair[f], 0, sizeof pair[f]);
            pair[f].used = 1, pair[f].from = from, pair[f].group = group, pair[f].m = e[i].o, pair[f].root = e[r].o;
            pair[f].ma = a, pair[f].mu = actor_uid(a), pair[f].ra = root, pair[f].ru = actor_uid(root), pair[f].first_us = now;
        }
        pair[f].heard_us = now;
    }
}
int coop_bonds_take(const void *data, int n, int from) {   // coop_world.c peer_tick: 1 if it was a bond packet
    if (n < 16) return 0;
    uint32_t kind = 0, cnt = 0;
    memcpy(&kind, data, 4), memcpy(&cnt, (const uint8_t *)data + 4, 4);
    if (kind != BONDS_KIND) return 0;
    if (cnt > ENTRY_N || n != (int)(offsetof(BondPacket, e) + cnt * sizeof(BondEntry)) || !bonds_on()) return 1;
    static BondPacket p;
    memcpy(&p, data, (size_t)n);
    int64_t now = frame_step_world_us();
    for (uint32_t i = 0; i < cnt;) {   // runs of one group
        uint32_t j = i;
        while (j < cnt && p.e[j].group == p.e[i].group) p.e[j].o.name[NAME_N - 1] = 0, j++;
        take_group(from, p.e[i].group, &p.e[i], (int)(j - i), now);
        i = j;
    }
    return 1;
}
static void bond_pairs(Cpu *c, int64_t now) {
    static int said;
    for (int k = 0; k < PAIR_N; k++) {
        if (!pair[k].used) continue;
        if (!alive(pair[k].ma, pair[k].mu) || !alive(pair[k].ra, pair[k].ru)) { pair[k].used = 0; continue; }
        if (now - pair[k].heard_us > 5000000) {   // no longer in that group there: parted here by making the copy again
            if (pair[k].tries) {
                rt_log("coop: bonds: %s (player %u's %08x) left its contraption there; its copy is made again", pair[k].m.name, pair[k].m.owner,
                       pair[k].m.id);
                coop_copy_remake(c, pair[k].ma);
            }
            pair[k].used = 0;
            continue;
        }
        // (2 s after its group was first heard: a member the owner lists late, as the wing whose group field read 0 for
        // a while, would make another the first; autobuild_3rd.txt pair test)
        if (!pair[k].tries && now - pair[k].first_us < 2000000) continue;
        uint64_t comp = comp_of(pair[k].ma), mb = main_body(pair[k].ma), rb = main_body(pair[k].ra);
        if (!comp || !mb || !rb) continue;
        if (pair[k].tries) {   // bonded once: a second bond would be a second edge. 3 s on, whether the graph joined them (logged)
            if (!pair[k].done && now - pair[k].tried_us > 3000000) {
                uint64_t mg = coop_rd_ptr(comp + SPR_GROUP), rg = coop_rd_ptr(comp_of(pair[k].ra) + SPR_GROUP);
                pair[k].done = 1;
                if (said < 40)
                    said++, rt_log("coop: bonds: %s and %s (player %u's contraption) %s", pair[k].m.name, pair[k].root.name, pair[k].m.owner,
                                   mg && mg == rg ? "share a group here" : "show no shared group here yet (the field reads 0 at times)");
            }
            continue;
        }
        __atomic_fetch_and((uint64_t *)(uintptr_t)(mb + 0x68), ~BODY_SKIP_BIT, __ATOMIC_SEQ_CST);
        __atomic_fetch_and((uint64_t *)(uintptr_t)(rb + 0x68), ~BODY_SKIP_BIT, __ATOMIC_SEQ_CST);
        static _Alignas(16) float wm[12], wr[12];
        if (!coop_rd(mb + 0x98, wm, sizeof wm) || !coop_rd(rb + 0x98, wr, sizeof wr)) continue;
        call5(c, MOD_MAIN_BASE + BOND_BODIES, comp, 0, rb, (uint64_t)(uintptr_t)wm, (uint64_t)(uintptr_t)wr);
        pair[k].tries++, pair[k].tried_us = now;
        if (said < 40) said++, rt_log("coop: bonds: %s bonded to %s (player %u's contraption)", pair[k].m.name, pair[k].root.name, pair[k].m.owner);
    }
}

void coop_bonds_tick(Cpu *c) {   // coop_world.c peer_tick
    if (!bonds_on() || !coop_net_on()) return;
    int64_t now = frame_step_world_us();
    static int64_t last;
    if (now - last >= 1000000) last = now, send_groups(now);
    bond_pairs(c, now);
}
