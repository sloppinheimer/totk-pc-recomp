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
// - a bonded copy in no group of the owner's list for 5 s is made again (coop_copy_remake): no call to undo such a
//   bond is known. A part taken off an autobuilt contraption with Ultrahand is deleted by the game itself (state 11;
//   autobuild_part_3rd.txt), and its copy with it.
// Only copies are bonded (a game's own objects are left as they are). TOTK_COOP_BONDS=0 turns it off.
#include <math.h>
#include <stdatomic.h>
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
#define CLAIM_KIND 0x4c43          // "CL" (claims, below)
static int claim_take(const void *data, int n, int from);

int coop_own_sent_objects(uint64_t *out, int max);   // coop_players.c
int coop_is_copy(uint64_t a);
void coop_copy_remake(Cpu *c, uint64_t a);
int coop_obj_ident(uint64_t a, int *owner, uint32_t *id);
uint64_t coop_obj_find(int owner, uint32_t id, const char *name);

#define NAME_N 40
typedef struct { uint32_t owner, id; char name[NAME_N]; } BondObj;
typedef struct { uint32_t group, flags; BondObj o; } BondEntry;   // flags: 2 a device with the switch, 1 it is on
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
// ---- a Zonai device's on state and its switch (re-notes "Device state on the copies"). On: [comp 11]+0x348 bit 0x2000
// (as the fan tests read it). Switch: bit 3 of byte +0x8d of component 166 ([actor+0x228]+0x530), the bit a hit sets;
// the device's update (main+0x4a3500, when +0x60 is set) takes it, toggles the device and clears it. Set on one device
// of an off contraption it turns them all on; to turn one off it has to be set on every device that is on, at once
// (one alone is turned on again by the others; autobuild_3rd.txt with TOTK_COOP_TEST_DEV8D).
static int dev_is_on(uint64_t a) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c11 = t ? coop_rd_ptr(t + 8 * 11) : 0;
    uint32_t v = 0;
    return c11 && coop_rd(c11 + 0x348, &v, 4) && (v & 0x2000);
}
static uint64_t dev_switch(uint64_t a) {   // component 166, when the device takes the switch (+0x60 set), else 0
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c166 = t ? coop_rd_ptr(t + 0x530) : 0;
    uint8_t b60 = 0;
    return c166 && coop_rd(c166 + 0x60, &b60, 1) && b60 ? c166 : 0;
}
// A contraption whose devices should be on or off as another game has them: dev_follow toggles the ones that are not,
// once they have differed for 0.8 s (the other player's stand-in swing may toggle a copy itself first: a hit in the
// owner's game reaches the copy through it) and at most once every 2.5 s (a device takes about 1 s to turn on).
typedef struct { int64_t differ_us, toggled_us; } Follow;
static int dev_follow(uint64_t *dev, int n, int want_on, Follow *f, int64_t now, const char *what) {
    int on = 0, sw = 0;
    for (int i = 0; i < n; i++)
        if (dev_switch(dev[i])) sw++, on |= dev_is_on(dev[i]);
    if (!sw || on == want_on) { f->differ_us = 0; return 0; }
    if (!f->differ_us) f->differ_us = now;
    if (now - f->differ_us < 800000 || now - f->toggled_us < 2500000) return 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint64_t c166 = dev_switch(dev[i]);
        if (c166 && dev_is_on(dev[i]) == on) __atomic_fetch_or((uint8_t *)(uintptr_t)(c166 + 0x8d), 0x08, __ATOMIC_SEQ_CST), k++;
    }
    f->toggled_us = now, f->differ_us = 0;
    static int said;
    if (said < 40) said++, rt_log("coop: bonds: %s turned %s here (%d of %d devices switched)", what, want_on ? "on" : "off", k, n);
    return 1;
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

// Two objects of this game bonded as an autobuild bonds its devices (main+0x1c05a98 from a's glue component to b's main
// body, bit 24 cleared on both bodies first): the bond the graph joins into one group. For tests (coop_glue.c glue_test,
// TOTK_COOP_TEST_GLUEBOND). 1: called.
int coop_bonds_bond(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t comp = comp_of(a), mb = main_body(a), rb = main_body(b);
    static _Alignas(16) float wm[12], wr[12];
    if (!comp || !mb || !rb || !coop_rd(mb + 0x98, wm, sizeof wm) || !coop_rd(rb + 0x98, wr, sizeof wr)) return 0;
    __atomic_fetch_and((uint64_t *)(uintptr_t)(mb + 0x68), ~BODY_SKIP_BIT, __ATOMIC_SEQ_CST);
    __atomic_fetch_and((uint64_t *)(uintptr_t)(rb + 0x68), ~BODY_SKIP_BIT, __ATOMIC_SEQ_CST);
    call5(c, MOD_MAIN_BASE + BOND_BODIES, comp, 0, rb, (uint64_t)(uintptr_t)wm, (uint64_t)(uintptr_t)wr);
    return 1;
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
    // Logged when a group's members change (by a hash of their numbers), not only the first sends: a contraption glued
    // by hand grows part by part, and the first ten sends of the tester's play log came before its third part
    static struct { uint64_t group, hash; } said_g[32];
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
        char names[512] = "";
        uint64_t hash = 0;
        for (int j = 0; j < MEMBER_N; j++) {
            if (!member[j].actor || member[j].group != member[k].group || now - member[j].seen_us > 3000000 || !alive(member[j].actor, member[j].uid)) continue;
            BondEntry *e = &p.e[p.n];
            int owner = 0;
            if (!coop_obj_ident(member[j].actor, &owner, &e->o.id)) continue;
            e->o.owner = (uint32_t)owner, e->group = (uint32_t)(member[k].group >> 3);
            e->flags = dev_switch(member[j].actor) ? 2u | (uint32_t)dev_is_on(member[j].actor) : 0;
            coop_actor_name(member[j].actor, e->o.name, sizeof e->o.name);
            hash = hash * 1000003 + e->o.id;
            if (strlen(names) < 440) snprintf(names + strlen(names), sizeof names - strlen(names), " %s", e->o.name);
            p.n++;
        }
        int g = -1, oldest = 0;
        for (int i = 0; i < 32 && g < 0; i++) if (said_g[i].group == member[k].group) g = i;
        if (g < 0) g = oldest, memmove(&said_g[1], &said_g[0], 31 * sizeof said_g[0]), said_g[0].group = member[k].group, said_g[0].hash = 0;
        static int said;
        if (said_g[g].hash != hash && said < 200)
            said++, said_g[g].hash = hash, rt_log("coop: bonds: a contraption of mine (%d parts), sent:%s", count, names);
    }
    // A device alone (with the switch, in no group) is sent too, group 0: its copy follows its on and off (one hit
    // there reaches it only if the stand-in's swing lands)
    for (int i = 0; i < n; i++) {   // (own, n: this game's sent objects, above)
        uint64_t a = own[i], comp = comp_of(a);
        if (coop_is_copy(a) || !dev_switch(a) || (comp && coop_rd_ptr(comp + SPR_GROUP))) continue;
        if ((int)p.n >= ENTRY_N) coop_net_hit_send(&p, (int)(offsetof(BondPacket, e) + p.n * sizeof(BondEntry))), p.n = 0;
        BondEntry *e = &p.e[p.n];
        int owner = 0;
        if (!coop_obj_ident(a, &owner, &e->o.id)) continue;
        e->o.owner = (uint32_t)owner, e->group = 0, e->flags = 2u | (uint32_t)dev_is_on(a);
        coop_actor_name(a, e->o.name, sizeof e->o.name);
        p.n++;
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
        // By member, not by group: the owner's group object changes when its contraption is made again (a part taken
        // off, autobuild_part_3rd.txt), and the members still bonded here are not parted for that.
        int f = -1, free = -1;
        for (int k = 0; k < PAIR_N && f < 0; k++) {
            if (pair[k].used && pair[k].from == from && pair[k].ma == a && pair[k].mu == actor_uid(a)) f = k;
            else if (!pair[k].used && free < 0) free = k;
        }
        if (f >= 0 && (pair[f].ra != root || pair[f].ru != actor_uid(root))) {   // its group's first copy is another now:
            pair[f].used = 0;                                                     // bonded to that one too (no bond is undone)
            f = -1;
        }
        if (f < 0) {
            if ((f = free) < 0) continue;
            memset(&pair[f], 0, sizeof pair[f]);
            pair[f].used = 1, pair[f].from = from, pair[f].group = group, pair[f].m = e[i].o, pair[f].root = e[r].o;
            pair[f].ma = a, pair[f].mu = actor_uid(a), pair[f].ra = root, pair[f].ru = actor_uid(root), pair[f].first_us = now;
        }
        pair[f].heard_us = now, pair[f].group = group;
    }
}
#define GST_N 32
static struct { int from; uint32_t group; int owner_on, flagged; int64_t heard_us; Follow f; } gst[GST_N];
static void group_state(int from, uint32_t group, const BondEntry *e, int n, int64_t now) {
    int on = 0, flagged = 0, f = -1, free = -1;
    for (int i = 0; i < n; i++) flagged |= (e[i].flags & 2) != 0, on |= (e[i].flags & 3) == 3;
    for (int k = 0; k < GST_N && f < 0; k++) {
        if (gst[k].heard_us && gst[k].from == from && gst[k].group == group) f = k;
        else if (free < 0 && (!gst[k].heard_us || now - gst[k].heard_us > 5000000)) free = k;
    }
    if (f < 0) {
        if ((f = free) < 0) return;
        memset(&gst[f], 0, sizeof gst[f]);
        gst[f].from = from, gst[f].group = group;
    }
    gst[f].owner_on = on, gst[f].flagged = flagged, gst[f].heard_us = now;
}
static int group_copies(int from, uint32_t group, uint64_t *out, int max) {   // this game's bonded copies of that group
    int n = 0;
    for (int k = 0; k < PAIR_N; k++) {
        if (!pair[k].used || !pair[k].tries || pair[k].from != from || pair[k].group != group) continue;
        uint64_t two[2] = {pair[k].ra, pair[k].ma};
        for (int j = 0; j < 2; j++) {
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= out[i] == two[j];
            if (!dup && n < max) out[n++] = two[j];
        }
    }
    return n;
}
static int claimed_group(int from, uint32_t group);
#define SST_N 64
static struct { int from, owner_on; BondObj o; int64_t heard_us; Follow f; } sst[SST_N];   // devices alone, by owner
static void single_state(int from, const BondEntry *e, int64_t now) {
    int f = -1, free = -1;
    for (int k = 0; k < SST_N && f < 0; k++) {
        if (sst[k].heard_us && sst[k].from == from && sst[k].o.id == e->o.id && sst[k].o.owner == e->o.owner) f = k;
        else if (free < 0 && (!sst[k].heard_us || now - sst[k].heard_us > 5000000)) free = k;
    }
    if (f < 0) {
        if ((f = free) < 0) return;
        memset(&sst[f], 0, sizeof sst[f]);
        sst[f].from = from, sst[f].o = e->o;
    }
    sst[f].owner_on = (e->flags & 3) == 3, sst[f].heard_us = now;
}
static int device_sync(void) {   // TOTK_COOP_DEVICE_SYNC=0: devices' on and off not followed
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_DEVICE_SYNC"); on = !(e && *e == '0'); }
    return on;
}
int coop_bonds_member(uint64_t actor);
static void follow_owners(int64_t now) {   // this game's copies of each group on or off as their owner has them
    for (int k = 0; device_sync() && k < SST_N; k++) {
        if (!sst[k].heard_us || now - sst[k].heard_us > 3000000) continue;
        uint64_t a = coop_obj_find((int)sst[k].o.owner, sst[k].o.id, sst[k].o.name);
        if (!a || !coop_is_copy(a) || coop_bonds_member(a)) continue;
        char what[80];
        snprintf(what, sizeof what, "player %d's %s", sst[k].from, sst[k].o.name);
        dev_follow(&a, 1, sst[k].owner_on, &sst[k].f, now, what);
    }
    for (int k = 0; device_sync() && k < GST_N; k++) {
        if (!gst[k].heard_us || now - gst[k].heard_us > 3000000 || !gst[k].flagged || claimed_group(gst[k].from, gst[k].group)) continue;
        uint64_t dev[64];
        int n = group_copies(gst[k].from, gst[k].group, dev, 64);
        char what[64];
        snprintf(what, sizeof what, "player %d's contraption", gst[k].from);
        if (n) dev_follow(dev, n, gst[k].owner_on, &gst[k].f, now, what);
        static int log = -1;   // TOTK_COOP_TEST_DEVSYNCLOG=1: every 0.5 s, the owner's state and how many copies are on
        static int64_t logged[GST_N];
        if (log < 0) log = cfg_get("TOTK_COOP_TEST_DEVSYNCLOG") != NULL;
        if (log && n && now - logged[k] >= 500000) {
            int on = 0, sw = 0;
            for (int i = 0; i < n; i++)
                if (dev_switch(dev[i])) sw++, on += dev_is_on(dev[i]);
            logged[k] = now;
            rt_log("coop: test: devsync: %.1f s: player %d's group %08x: owner %s, %d of %d copy devices on", now / 1e6, gst[k].from,
                   gst[k].group, gst[k].owner_on ? "on" : "off", on, sw);
        }
    }
}

int coop_bonds_take(const void *data, int n, int from) {   // coop_world.c peer_tick: 1 if it was a bond packet
    if (n < 16) return 0;
    uint32_t kind = 0, cnt = 0;
    memcpy(&kind, data, 4), memcpy(&cnt, (const uint8_t *)data + 4, 4);
    if (kind == CLAIM_KIND) return claim_take(data, n, from);
    if (kind != BONDS_KIND) return 0;
    if (cnt > ENTRY_N || n != (int)(offsetof(BondPacket, e) + cnt * sizeof(BondEntry)) || !bonds_on()) return 1;
    static BondPacket p;
    memcpy(&p, data, (size_t)n);
    int64_t now = frame_step_world_us();
    for (uint32_t i = 0; i < cnt;) {   // runs of one group
        if (!p.e[i].group) { p.e[i].o.name[NAME_N - 1] = 0, single_state(from, &p.e[i], now), i++; continue; }   // a device alone
        uint32_t j = i;
        while (j < cnt && p.e[j].group == p.e[i].group) p.e[j].o.name[NAME_N - 1] = 0, j++;
        take_group(from, p.e[i].group, &p.e[i], (int)(j - i), now);
        group_state(from, p.e[i].group, &p.e[i], (int)(j - i), now);
        i = j;
    }
    return 1;
}
// An autobuilt part's look: the Autobuild's confirm (main+0x1d3c0d4, at main+0x1d3c4e0) calls main+0x1c07114 on each
// part's SpecialPowerReceiverComponent; it ORs bit 0 into the component's +0x150 (the green, as the "green_begone" mod
// shows: read for behaviour only, no licence) and +0x27c, sets +0x264/+0x268 and the +0x20 flags, and clears bit 2 of
// the actor's +0x5ac. Called once on each copy of an autobuilt part (coop_players.c copy flag COOP_OBJ_AUTOBUILT) when it
// is bonded. TOTK_COOP_TEST_GREEN=0: not called; =all: on every bonded copy.
#define MARK_AUTOBUILT 0x1c07114
int coop_copy_autobuilt(uint64_t actor);   // coop_players.c
static void mark_autobuilt(Cpu *c, uint64_t a) {
    static const char *mode = (const char *)1;
    if (mode == (const char *)1) mode = cfg_get("TOTK_COOP_TEST_GREEN");
    if (mode && *mode == '0') return;
    if (!(mode && !strcmp(mode, "all")) && !coop_copy_autobuilt(a)) return;
    static uint64_t done[128];
    static int n;
    for (int i = 0; i < n; i++) if (done[i] == a) return;
    uint64_t comp = comp_of(a);
    if (!comp) return;
    uint32_t b = 0;
    coop_rd(comp + 0x150, &b, 4);
    if (!(b & 1)) coop_guest_call(c, MOD_MAIN_BASE + MARK_AUTOBUILT, comp, 0, 0, 0);
    done[n++ % 128] = a;
    static int said;
    char nm[48];
    if (said++ < 20) rt_log("coop: bonds: %s's copy marked autobuilt (+0x150 %x before)", coop_actor_name(a, nm, sizeof nm), b);
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
        mark_autobuilt(c, pair[k].ma), mark_autobuilt(c, pair[k].ra);
        if (said < 40) said++, rt_log("coop: bonds: %s bonded to %s (player %u's contraption)", pair[k].m.name, pair[k].root.name, pair[k].m.owner);
    }
}

// ---- claims: the game whose Link rides a contraption of another player's runs it (TotK Online's exclusive claim,
// read for behaviour only: there the player holding a glued group with Ultrahand owns it). While this Link rides a copy
// of player P's contraption (one of P's groups, as its "CB" packets list them) and P's own Link is not on it, this game
// claims the whole group: its copies are no longer put at P's places (coop_players.c place_copy), frozen copies run
// their update again (copy_place) and the Link rides them by their own physics (ride_carry); their places go to P
// twenty times a second ("CL" packets), and P moves its contraption there as one body by speed (held_tick). So a steering stick held here steers the contraption in both games. Let go 2 s after the
// Link leaves it. Its devices' on or off goes with the claim ("CL" flags) and the owner's devices follow (dev_follow).
typedef struct { uint32_t id; char name[NAME_N]; float pos[3], q[4]; } ClaimEntry;
#define CLAIM_N ((COOP_HIT_MAX - 16) / (int)sizeof(ClaimEntry))
typedef struct { uint32_t kind, n, owner, flags; ClaimEntry e[CLAIM_N]; } ClaimPacket;   // flags: 2 sent, 1 its devices on
_Static_assert(sizeof(ClaimPacket) <= COOP_HIT_MAX, "claim packet fits");
int coop_ride_now(int *owner, uint32_t *id);   // coop_players.c
int coop_player_rides(int net_id, int *owner, uint32_t *id);
int coop_player_pos(int net_id, float out[3]);
void coop_obj_force(Cpu *c, uint64_t a, const float pos[3], const float q[4]);
void coop_mat_quat(const float r[9], float q[4]);
#define CLAIMED_N 32
static _Atomic uint64_t claimed[CLAIMED_N];   // this game's copies of the contraption it claims (read on game threads too)
static struct { int from; uint32_t group; int64_t until_us; } claim;
static int claimed_group(int from, uint32_t group) { return claim.until_us && claim.from == from && claim.group == group; }
int coop_bonds_claimed(uint64_t actor) {
    for (int i = 0; actor && i < CLAIMED_N; i++)
        if (atomic_load_explicit(&claimed[i], memory_order_relaxed) == actor) return 1;
    return 0;
}
int coop_bonds_member(uint64_t actor) {   // a copy bonded here into another player's contraption (any thread: a read)
    for (int k = 0; actor && k < PAIR_N; k++)
        if (pair[k].used && pair[k].tries && (pair[k].ma == actor || pair[k].ra == actor)) return 1;
    return 0;
}
static int in_group(int from, uint32_t group, uint32_t id) {   // one of player from's objects of that group, by number
    for (int k = 0; k < PAIR_N; k++)
        if (pair[k].used && pair[k].from == from && pair[k].group == group && (pair[k].m.id == id || pair[k].root.id == id)) return 1;
    return 0;
}
static void claim_tick(int64_t now) {
    int ro = 0, po = 0;
    uint32_t rid = 0, pid = 0;
    if (coop_ride_now(&ro, &rid)) {
        uint32_t g = 0;
        int found = 0;
        for (int k = 0; k < PAIR_N && !found; k++)
            if (pair[k].used && pair[k].from == ro && (pair[k].m.id == rid || pair[k].root.id == rid)) g = pair[k].group, found = 1;
        int owner_on = coop_player_rides(ro, &po, &pid) && po == ro && in_group(ro, g, pid);
        // ... and this Link 2 m nearer it than the owner's (TotK Online gives a group to the nearest player): standing
        // by a fan the owner had just hit counted as riding it, and the claim pulled the owner's flying contraption
        // back to this game's late copies (autobuild_3rd.txt pair test)
        float me_p[3], op[3], cp[3];
        uint64_t me = coop_player_actor(), cpy = 0;
        for (int k = 0; k < PAIR_N && !cpy; k++)
            if (pair[k].used && pair[k].from == ro && pair[k].group == g) cpy = pair[k].ra;
        int nearer = 0;
        if (me && cpy && coop_rd(me + COOP_ACTOR_POS, me_p, 12) && coop_rd(cpy + COOP_ACTOR_POS, cp, 12)) {
            float dm = (me_p[0] - cp[0]) * (me_p[0] - cp[0]) + (me_p[2] - cp[2]) * (me_p[2] - cp[2]), d_o = 1e9f;
            if (coop_player_pos(ro, op)) d_o = (op[0] - cp[0]) * (op[0] - cp[0]) + (op[2] - cp[2]) * (op[2] - cp[2]);
            nearer = sqrtf(dm) + 2.0f < sqrtf(d_o) || (claim.until_us && claim.from == ro && claim.group == g);   // (kept once claimed)
        }
        if (found && !owner_on && nearer) {
            if (!claim.until_us || claim.from != ro || claim.group != g)
                rt_log("coop: bonds: claimed player %d's contraption (its group %08x): this Link rides it", ro, g);
            claim.from = ro, claim.group = g, claim.until_us = now + 2000000;
        }
    }
    if (claim.until_us && now > claim.until_us) {
        rt_log("coop: bonds: player %d's contraption let go", claim.from);
        claim.until_us = 0;
    }
    int n = 0;   // the claimed copies, root and members
    static ClaimPacket p;
    memset(&p, 0, offsetof(ClaimPacket, e));
    p.kind = CLAIM_KIND, p.owner = (uint32_t)claim.from;
    for (int k = 0; k < PAIR_N && claim.until_us; k++) {
        if (!pair[k].used || pair[k].from != claim.from || pair[k].group != claim.group) continue;
        uint64_t two[2] = {pair[k].ra, pair[k].ma};
        const BondObj *ob[2] = {&pair[k].root, &pair[k].m};
        uint32_t uid[2] = {pair[k].ru, pair[k].mu};
        for (int j = 0; j < 2; j++) {
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= atomic_load(&claimed[i]) == two[j];
            if (dup || n >= CLAIMED_N || n >= CLAIM_N || !alive(two[j], uid[j])) continue;
            atomic_store(&claimed[n], two[j]);
            ClaimEntry *e = &p.e[n];
            float r[9];
            e->id = ob[j]->id;
            memcpy(e->name, ob[j]->name, NAME_N);
            if (coop_rd(two[j] + COOP_ACTOR_POS, e->pos, 12) && coop_rd(two[j] + COOP_ACTOR_ROT, r, 36)) coop_mat_quat(r, e->q);
            n++;
        }
    }
    for (int i = n; i < CLAIMED_N; i++) atomic_store(&claimed[i], 0);
    int sw = 0, on = 0;   // the claimed copies' devices on or off (a stick held here turns them on): the owner's follow
    for (int i = 0; i < n; i++) {
        uint64_t a = atomic_load(&claimed[i]);
        if (dev_switch(a)) sw = 1, on |= dev_is_on(a);
    }
    p.flags = sw ? 2u | (uint32_t)on : 0;
    static int64_t sent_us;
    if (n && now - sent_us >= 50000) {
        sent_us = now, p.n = (uint32_t)n;
        coop_net_hit_send(&p, (int)(offsetof(ClaimPacket, e) + (size_t)n * sizeof(ClaimEntry)));
    }
}
// The owner's side: where the claiming player has this game's objects; moved there each tick while heard (0.5 s).
#define HELD_N 64
static struct { int from; ClaimEntry e; int64_t heard_us; } held[HELD_N];
static struct { int from; uint32_t id, flags; Follow f; } held_root[8];   // (with the claimer's device state, flags)   // each claimer's first entry (its root): the others move with it
static int claim_take(const void *data, int n, int from) {
    uint32_t cnt = 0;
    memcpy(&cnt, (const uint8_t *)data + 4, 4);
    if (cnt > CLAIM_N || n != (int)(offsetof(ClaimPacket, e) + cnt * sizeof(ClaimEntry)) || !bonds_on()) return 1;
    static ClaimPacket p;
    memcpy(&p, data, (size_t)n);
    if ((int)p.owner != coop_net_id()) return 1;
    int64_t now = frame_step_world_us();
    if (cnt) {
        int r = -1;
        for (int k = 0; k < 8 && r < 0; k++) if (held_root[k].from == from || !held_root[k].from) r = k;
        if (r >= 0) held_root[r].from = from, held_root[r].id = p.e[0].id, held_root[r].flags = p.flags;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        p.e[i].name[NAME_N - 1] = 0;
        int f = -1, free = -1;
        for (int k = 0; k < HELD_N && f < 0; k++) {
            if (held[k].heard_us && held[k].from == from && held[k].e.id == p.e[i].id) f = k;
            else if (free < 0 && (!held[k].heard_us || now - held[k].heard_us > 500000)) free = k;
        }
        if (f < 0 && (f = free) < 0) continue;
        if (!held[f].heard_us || now - held[f].heard_us > 500000)
            rt_log("coop: bonds: player %d runs my %s (%08x) now: moved where it has it", from, p.e[i].name, p.e[i].id);
        held[f].from = from, held[f].e = p.e[i], held[f].heard_us = now;
    }
    return 1;
}
// Moved as one body: the claimer's place of the root against this game's own place of it gives one rigid move, put on
// the root's own place here (and, with TOTK_COOP_CLAIM_ALL=1, on every part's). Each part put at the claimer's place of it pulled this game's contraption apart (the
// claimer's copies sit a little differently), and an autobuilt part pulled off is deleted by the game: the whole
// contraption went in two seconds (autobuild_stick_3rd.txt pair test).
static void quat_rot(const float q[4], float r[9]) {   // x y z w, rows (as coop_players.c quat_mat)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
static void held_tick(Cpu *c, int64_t now) {
    for (int k = 0; k < HELD_N; k++)
        if (held[k].heard_us && now - held[k].heard_us > 500000) held[k].heard_us = 0;
    for (int g = 0; g < 8; g++) {
        if (!held_root[g].from) continue;
        int from = held_root[g].from, rk = -1;
        for (int k = 0; k < HELD_N && rk < 0; k++)
            if (held[k].heard_us && held[k].from == from && held[k].e.id == held_root[g].id) rk = k;
        uint64_t ra = rk >= 0 ? coop_obj_find(coop_net_id(), held[rk].e.id, held[rk].e.name) : 0;
        if (rk >= 0 && (held_root[g].flags & 2) && device_sync()) {   // this game's devices of it as the claimer has them
            uint64_t dev[64];
            int nd = 0;
            for (int k = 0; k < HELD_N && nd < 64; k++) {
                uint64_t a = held[k].heard_us && held[k].from == from ? coop_obj_find(coop_net_id(), held[k].e.id, held[k].e.name) : 0;
                if (a) dev[nd++] = a;
            }
            char what[64];
            snprintf(what, sizeof what, "my contraption run by player %d", from);
            if (nd) dev_follow(dev, nd, held_root[g].flags & 1, &held_root[g].f, now, what);
        }
        float pa[3], Ra[9], Rb[9], Rt[9], tt[3];
        if (!ra || !coop_rd(ra + COOP_ACTOR_POS, pa, 12) || !coop_rd(ra + COOP_ACTOR_ROT, Ra, 36)) continue;
        quat_rot(held[rk].e.q, Rb);
        for (int i = 0; i < 3; i++)   // Rt = Rb Ra^T, tt = pb - Rt pa
            for (int j = 0; j < 3; j++) Rt[i * 3 + j] = Rb[i * 3] * Ra[j * 3] + Rb[i * 3 + 1] * Ra[j * 3 + 1] + Rb[i * 3 + 2] * Ra[j * 3 + 2];
        for (int i = 0; i < 3; i++) tt[i] = held[rk].e.pos[i] - (Rt[i * 3] * pa[0] + Rt[i * 3 + 1] * pa[1] + Rt[i * 3 + 2] * pa[2]);
        for (int k = 0; k < HELD_N; k++) {
            // The root alone, the others carried by their bonds: with every part moved (TOTK_COOP_CLAIM_ALL=1), one
            // teleported part (force_to, more than 0.5 m off) broke off and was deleted (autobuild_stick_3rd.txt).
            static int root_only = -1;
            if (root_only < 0) root_only = cfg_get("TOTK_COOP_CLAIM_ALL") == NULL;
            if (!held[k].heard_us || held[k].from != from || (root_only && k != rk)) continue;
            uint64_t a = k == rk ? ra : coop_obj_find(coop_net_id(), held[k].e.id, held[k].e.name);
            float pj[3], Rj[9], pt[3], R2[9], q[4];
            if (!a || !coop_rd(a + COOP_ACTOR_POS, pj, 12) || !coop_rd(a + COOP_ACTOR_ROT, Rj, 36)) continue;
            for (int i = 0; i < 3; i++) {
                pt[i] = Rt[i * 3] * pj[0] + Rt[i * 3 + 1] * pj[1] + Rt[i * 3 + 2] * pj[2] + tt[i];
                for (int j = 0; j < 3; j++) R2[i * 3 + j] = Rt[i * 3] * Rj[j] + Rt[i * 3 + 1] * Rj[3 + j] + Rt[i * 3 + 2] * Rj[6 + j];
            }
            coop_mat_quat(R2, q);
            // at most 0.4 m a tick: force_to teleports past 0.5 m, and a teleported part broke off its contraption
            float d[3] = {pt[0] - pj[0], pt[1] - pj[1], pt[2] - pj[2]}, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l > 0.4f)
                for (int i = 0; i < 3; i++) pt[i] = pj[i] + d[i] * 0.4f / l;
            coop_obj_force(c, a, pt, q);
        }
    }
}

void coop_bonds_tick(Cpu *c) {   // coop_world.c peer_tick
    if (!bonds_on() || !coop_net_on()) return;
    int64_t now = frame_step_world_us();
    static int64_t last;
    static int64_t checked;
    static uint64_t sent_on;
    if (now - checked >= 100000) {   // a member device turned on or off: the groups sent now, not at the next second
        checked = now;
        uint64_t h = 0, own[256];
        for (int k = 0; k < MEMBER_N; k++)
            if (member[k].actor && dev_is_on(member[k].actor)) h = h * 31 + member[k].actor;
        int no = coop_own_sent_objects(own, 256);   // (devices alone too)
        for (int i = 0; i < no; i++)
            if (dev_switch(own[i]) && dev_is_on(own[i])) h = h * 31 + own[i];
        if (h != sent_on) sent_on = h, last = 0;
    }
    if (now - last >= 1000000) last = now, send_groups(now);
    follow_owners(now);
    bond_pairs(c, now);
    claim_tick(now);
    held_tick(c, now);
    static const char *ta = (const char *)1;   // test (TOTK_COOP_TEST_ABACTOR=<hex offset>,<hex mask>): ORed into each bonded copy's actor
    if (ta == (const char *)1) ta = cfg_get("TOTK_COOP_TEST_ABACTOR");
    unsigned long long off = 0, mask = 0;
    if (ta && sscanf(ta, "%llx,%llx", &off, &mask) == 2)
        for (int k = 0; k < PAIR_N; k++)
            if (pair[k].used && pair[k].tries) {
                __atomic_fetch_or((uint32_t *)(uintptr_t)(pair[k].ma + off), (uint32_t)mask, __ATOMIC_SEQ_CST);
                __atomic_fetch_or((uint32_t *)(uintptr_t)(pair[k].ra + off), (uint32_t)mask, __ATOMIC_SEQ_CST);
            }
}
