// Co-op: Ultrahand glue made in one game made again in the others (TotK Online's ActorScotchSync, read for behaviour
// only; its notes: side-session notes mod-re-4 section 3). TotK 1.4.3 main offsets; how each was found is in
// docs/coop/proxy-plan.md ("Ultrahand glue").
//
// The game's glue ("scotch"): the SpecialPowerReceiverComponent of an actor (component #144, [[actor+0x228]+0x480])
// keeps a list of bond entries (+0x158 the list's head, +0x160 its first node; a node's +8 the next, +0x10 the entry).
// A message to the component (main+0x1c0bf20, type 0x2051000e) makes an entry from a request (createScotch,
// main+0x1c0b94c: (component, request), the request's +0x20 an argument object), and the next (0x2051000f) glues it
// (main+0x1c0ba8c, which ends in main+0x1d61a6c and its tail call into doScotch, main+0x1d66e34). An entry: +0x420
// link to the component's actor (A), +0x438 the handle of A's glued body, +0x448 a 3x4 matrix, +0x488 link to the
// other actor (B), +0x4a0 the handle of B's body, +0x4b0 a 3x4 matrix, +0x550 its state (2 glued), +0x554 flags.
// doScotch (entry+0x558, matrix A, matrix B, body A, body B, entry+0x488, flag).
//
// Here, as TotK Online does:
// - capture: doScotch's entry (it has one caller, the tail call above) gives the two actors, the two bodies, the two
//   matrices and the flag. The game that made the bond (its maker) numbers it and sends it ("GA" packets) to the
//   others, again every 3 s while it holds, and the list of its bonds every second ("GL").
// - apply: a game told of a bond finds the two objects by their numbers across the games (coop_players.c
//   coop_obj_ident / coop_obj_find), builds a request of its own (the argument's base class, constructor
//   main+0x1c0c4ec) with its own bodies' handles, calls createScotch, then doScotch on the new entry, the steps of
//   main+0x1d61a6c. Retried once a second until the entry is glued, and checked every 2 s after.
// - removal: a bond missing from two of its maker's lists in a row is taken apart here with removeScotch
//   (main+0x1c042bc: (component, the actor's id [actor+0x380], 0x2051000a, 0), as the message 0x2051000b does), which
//   parts the actor from everything glued to it; the bonds of it that still hold are then made again by the retries.
// - veto: a break of a bond made again here from another game's (the destroy step main+0x1c0bb54: (component, the
//   other's body), which hands it to main+0x2999fe8, TotK Online's hook in 1.2.1) is refused, unless this game parts
//   it for its maker (removal, above) in the last 2 s.
// Unlike TotK Online: no server links the glued actors into one owner group here; each object keeps the owner the
// prop sync gives it. Its maker drops a bond from its list once its own entry is no longer glued (two checks a second
// apart), or one of the two actors is gone.
// TOTK_COOP_GLUE=0 turns it off. TOTK_COOP_TEST_GLUE=<n1>,<n2> (tools/coop/glue_pair_3rd.txt): the game named A glues
// two objects it makes in front of its Link (glue_test).
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
#include "platform/platform.h"

#define SCOTCH_COMP 0x480         // [actor+0x228]+: the SpecialPowerReceiverComponent
#define CREATE_SCOTCH 0x1c0b94c   // (component, request): the bond's entry (1.2.1 main+0x165710c, the same code)
#define DO_SCOTCH 0x1d66e34       // (entry+0x558, matrix A, matrix B, body A, body B, entry+0x488, flag) (1.2.1 0x176fbac)
#define REMOVE_SCOTCH 0x1c042bc   // (component, actor id, 0x2051000a, 0) (1.2.1 0x1650c30)
#define DESTROY_STEP 0x1c0bb54    // (component, body): a bond's break, given to main+0x2999fe8 (1.2.1 0x16575e0)
#define ARG_CTOR 0x1c0c4ec        // the request argument's base constructor (vtable main+0x367d5c8; 1.2.1 0x1657ce8)
#define LINK_SET 0x155ec          // (link, actor, strong): an ActorLink to the actor
#define ENTRY_BODY_A 0x1d61b08    // (entry): A's body from its handle
#define ENTRY_BODY_B 0x1d61b5c    // (entry): B's body
#define BODY_TABLE_GOT 0x395dac0  // [[[[main+]]+0xc8]+0xf8]: the body table (+0x38 count, +0x40 entries 0xc8 each)
#define ACTOR_ID 0x380            // an actor's id, as the scotch messages name their target

#define GLUE_ADD 0x4147    // "GA": bonds, full
#define GLUE_LIST 0x4c47   // "GL": the numbers of its maker's bonds now
#define NAME_N 40
typedef struct { uint32_t owner, id; char name[NAME_N]; } GlueObj;
typedef struct {
    uint32_t bond;                        // the maker's number for it
    uint8_t flag, pad[3];                 // doScotch's last argument
    GlueObj a, b;                         // a: the actor whose component holds the bond; b: the other
    uint16_t body_a[2], body_b[2];        // where the glued body is from the actor's physics controller (body_ref)
    float ma[12], mb[12];                 // the request's two matrices (argument +0x3c, +0x94)
} GlueBond;
#define ADD_N 4
typedef struct { uint32_t kind, n, pad[2]; GlueBond b[ADD_N]; } GlueAddPacket;
#define LIST_N 64
typedef struct { uint32_t kind, n, pad[2]; uint32_t bond[LIST_N]; } GlueListPacket;
_Static_assert(sizeof(GlueBond) == 208, "GlueBond wire size");
_Static_assert(sizeof(GlueAddPacket) <= COOP_HIT_MAX && sizeof(GlueListPacket) <= COOP_HIT_MAX, "glue packets fit");

static PlatMutex glue_lock = PLAT_MUTEX_INIT;   // never held across a game call
static __thread int in_apply;                   // this thread makes a bond for another game: not captured

static int glue_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_GLUE"); on = !(e && *e == '0'); }
    return on;
}

// ---- game structures
static uint64_t link_actor(uint64_t link) {   // an ActorLink: +8 the reference object ([+0x40] actor, +0x48 id), +0x10 id
    uint64_t ref = coop_rd_ptr(link + 8);
    uint32_t id = 0, rid = 0;
    if (!ref || !coop_rd(link + 0x10, &id, 4) || id == ~0u || !coop_rd(ref + 0x48, &rid, 4) || rid != id) return 0;
    return coop_rd_ptr(ref + 0x40);
}
static uint64_t scotch_comp(uint64_t a) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0;
    return t ? coop_rd_ptr(t + SCOTCH_COMP) : 0;
}
static uint64_t phys_ctl(uint64_t a) {   // [[actor+0x228]+0x50]+0x20: the physics controller (+0x150 its main body)
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, ph = t ? coop_rd_ptr(t + 0x50) : 0;
    return ph ? coop_rd_ptr(ph + 0x20) : 0;
}
static uint64_t body_table(void) {
    uint64_t g = coop_rd_ptr(MOD_MAIN_BASE + BODY_TABLE_GOT);
    g = g ? coop_rd_ptr(g) : 0;
    g = g ? coop_rd_ptr(g + 0xc8) : 0;
    return g ? coop_rd_ptr(g + 0xf8) : 0;
}
// A body handle's body (main+0x29a5a08 without its lock): the low 16 bits pick a 0xc8-byte record, whose +0x50 must be
// the whole handle and +0x28 bit 0 clear; +0x58 the body.
static uint64_t handle_body(uint32_t h) {
    uint64_t t = body_table();
    uint32_t n = 0;
    if (!t || h == ~0u || !coop_rd(t + 0x38, &n, 4) || (h & 0xffff) >= n) return 0;
    uint64_t e = coop_rd_ptr(t + 0x40) + (uint64_t)(h & 0xffff) * 0xc8;
    uint8_t fl = 1;
    uint32_t have = 0;
    if (!coop_rd(e + 0x28, &fl, 1) || (fl & 1) || !coop_rd(e + 0x50, &have, 4) || have != h) return 0;
    return coop_rd_ptr(e + 0x58);
}
static uint32_t body_handle(uint64_t body) {   // the other way: the record that holds it
    uint64_t t = body_table();
    uint32_t n = 0;
    if (!t || !body || !coop_rd(t + 0x38, &n, 4)) return ~0u;
    uint64_t base = coop_rd_ptr(t + 0x40);
    for (uint32_t i = 0; base && i < n && i < 0x10000; i++) {
        uint64_t e = base + (uint64_t)i * 0xc8;
        uint8_t fl = 1;
        uint32_t h = 0;
        if (coop_rd_ptr(e + 0x58) == body && coop_rd(e + 0x28, &fl, 1) && !(fl & 1) && coop_rd(e + 0x50, &h, 4)) return h;
    }
    return ~0u;
}
// Where an actor's body is, from its physics controller: the word at +o (b[1] 0xffff), or the word at +b[1] of the
// object at +o. Two actors of one kind keep their bodies in the same places, so another game finds its own the same way.
static int body_ref(uint64_t a, uint64_t body, uint16_t out[2]) {
    uint64_t ctl = phys_ctl(a), w[0x800 / 8], v[0x200 / 8];
    if (!ctl || !body || !coop_rd(ctl, w, sizeof w)) return 0;
    for (int i = 0; i < 0x800 / 8; i++)
        if (w[i] == body) { out[0] = (uint16_t)(i * 8), out[1] = 0xffff; return 1; }
    for (int i = 0; i < 0x400 / 8; i++) {
        if (w[i] < 0x100000000ull || w[i] >= 0x8000000000ull || (w[i] & 7) || !coop_rd(w[i], v, sizeof v)) continue;
        for (int j = 0; j < 0x200 / 8; j++)
            if (v[j] == body) { out[0] = (uint16_t)(i * 8), out[1] = (uint16_t)(j * 8); return 1; }
    }
    return 0;
}
static uint64_t body_at(uint64_t a, const uint16_t ref[2]) {
    uint64_t ctl = phys_ctl(a);
    if (!ctl) return 0;
    uint64_t p = coop_rd_ptr(ctl + ref[0]);
    return ref[1] == 0xffff ? p : p ? coop_rd_ptr(p + ref[1]) : 0;
}
// The entry of the component's list for these two bodies; NULL if none.
static uint64_t find_entry(uint64_t comp, uint32_t ha, uint32_t hb) {
    uint64_t head = comp + 0x158, node = coop_rd_ptr(comp + 0x160);
    for (int n = 0; node && node != head && n < 256; n++) {
        uint64_t e = coop_rd_ptr(node + 0x10);
        uint32_t a = 0, b = 0;
        if (e && coop_rd(e + 0x438, &a, 4) && coop_rd(e + 0x4a0, &b, 4) && a == ha && b == hb) return e;
        node = coop_rd_ptr(node + 8);
    }
    return 0;
}
static int entry_glued(uint64_t e) {
    uint32_t st = 0;
    return e && coop_rd(e + 0x550, &st, 4) && st == 2;
}
static int alive_named(uint64_t a, const char *name) {
    int32_t st = 0;
    char nm[48];
    return a && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 && !strcmp(coop_actor_name(a, nm, sizeof nm), name);
}
// An actor's id, [actor+0x10], the one an ActorLink keeps and checks against its reference (main+0x155ec): another
// actor made later in a gone one's memory, with the same name, has another (code review, 2026-11-07). 0: unreadable.
static uint32_t actor_uid(uint64_t a) {
    uint32_t id = 0;
    return a && coop_rd(a + 0x10, &id, 4) ? id : 0;
}
static int alive_same(uint64_t a, const char *name, uint32_t uid) {   // still that actor
    return alive_named(a, name) && uid && actor_uid(a) == uid;
}
static uint64_t guest_call7(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                            uint64_t a6) {
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 7, a0, a1, a2, a3, a4, a5, a6);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}

// ---- captured here (doScotch's entry, any thread), sent by the tick
#define PEND_N 16
static struct { uint64_t entry, a, b, body_a, body_b; uint32_t ha, hb; uint8_t flag; float ma[12], mb[12]; } pend[PEND_N];
static int pend_n;
#define MINE_N LIST_N
// remake_until: this game parted a or b for another game (part_here), which parts it from this game's own bonds too:
// until then each check makes this bond again instead of dropping it.
static struct { int used, gone_checks; GlueBond g; uint64_t a, b; uint32_t ha, hb, ua, ub; int64_t sent_us, remake_from, remake_until; } mine[MINE_N];
static uint32_t next_bond = 1;
// Bonds of the other games made here, under glue_lock (the veto reads them on game threads).
#define THEIRS_N 128
static struct {
    int used, from, made, tries, missed;
    GlueBond g;
    uint64_t a, b;                 // the two actors here, once found
    uint32_t ua, ub;               // ... and their ids (actor_uid)
    uint32_t ha, hb;               // their bodies' handles here, once made
    int64_t try_us, check_us;
} theirs[THEIRS_N];
static struct { uint64_t a; int64_t until_us; } parting[16];   // actors this game parts for a maker (removal): no veto

static int theirs_pair(uint64_t x, uint64_t y) {   // under glue_lock: a bond of another game's made here on x and y
    for (int i = 0; x && y && i < THEIRS_N; i++)
        if (theirs[i].used && theirs[i].made && ((theirs[i].a == x && theirs[i].b == y) || (theirs[i].b == x && theirs[i].a == y)))
            return i;
    return -1;
}

static void capture(Cpu *c) {
    if (in_apply || !glue_on()) return;
    uint64_t entry = c->x[0] - 0x558;
    uint64_t a = link_actor(entry + 0x420), b = link_actor(entry + 0x488);
    static int ablog = -1;   // TOTK_COOP_TEST_ABLOG (coop_autobuild.c): every doScotch entry
    if (ablog < 0) ablog = cfg_get("TOTK_COOP_TEST_ABLOG") != NULL;
    if (ablog) {
        static _Atomic int said;
        char n1[48], n2[48];
        if (atomic_fetch_add(&said, 1) < 60)
            rt_log("coop: ablog: doScotch %s %llx to %s %llx from main+%llx", coop_actor_name(a, n1, sizeof n1), (unsigned long long)a,
                   coop_actor_name(b, n2, sizeof n2), (unsigned long long)b, (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
    }
    if (!a || !b || !coop_world_role()) return;
    plat_mutex_lock(&glue_lock);
    int remote = theirs_pair(a, b) >= 0;   // this game remaking another's bond itself is not a bond of its own
    if (!remote && pend_n >= PEND_N) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 5) rt_log("coop: glue: more than %d bonds made here in one tick; one is not sent", PEND_N);
    }
    if (!remote && pend_n < PEND_N) {
        __typeof__(pend[0]) *p = &pend[pend_n++];
        p->entry = entry, p->a = a, p->b = b, p->body_a = c->x[3], p->body_b = c->x[4], p->flag = (uint8_t)(c->x[6] & 1);
        coop_rd(entry + 0x438, &p->ha, 4), coop_rd(entry + 0x4a0, &p->hb, 4);
        coop_rd(c->x[1], p->ma, sizeof p->ma), coop_rd(c->x[2], p->mb, sizeof p->mb);
    }
    plat_mutex_unlock(&glue_lock);
}

// The break of a bond (main+0x1c0bb54: x0 the component, x1 the other side's body): refused for one made here from
// another game's. The body's actor: its +0x100 user object, whose +0x110 object holds a link at +0x18 (main+0x1cf54d4,
// which sends this break, reads it so). 1: refused.
static int veto(Cpu *c) {
    if (!glue_on()) return 0;
    uint64_t x = coop_rd_ptr(c->x[0] + 0x18), u = coop_rd_ptr(c->x[1] + 0x100), o = u ? coop_rd_ptr(u + 0x110) : 0;
    uint64_t y = o ? link_actor(o + 0x18) : 0;
    if (!x) return 0;
    // The other side not known: let through. Taken as "any bond on x", every break on x was refused, ones of this
    // game's own glue too (code review, 2026-11-07). TotK Online refuses only with both actors known.
    if (!y) {
        static _Atomic int said_y;
        char n1[48];
        if (atomic_fetch_add(&said_y, 1) < 5)
            rt_log("coop: glue: a break on %s whose other side is not known is let through", coop_actor_name(x, n1, sizeof n1));
        return 0;
    }
    int64_t now = frame_step_world_us();
    plat_mutex_lock(&glue_lock);
    int k = -1, allowed = 0;
    for (int i = 0; i < 16; i++)
        if (parting[i].a && now < parting[i].until_us && (parting[i].a == x || parting[i].a == y)) allowed = 1;
    if (!allowed) k = theirs_pair(x, y);
    uint32_t bond = k >= 0 ? theirs[k].g.bond : 0;
    int from = k >= 0 ? theirs[k].from : 0;
    plat_mutex_unlock(&glue_lock);
    if (k < 0) return 0;
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 20) {
        char n1[48], n2[48] = "?";
        coop_actor_name(x, n1, sizeof n1);
        if (y) coop_actor_name(y, n2, sizeof n2);
        rt_log("coop: glue: a break of bond %u (player %d's) between %s and %s refused here", bond, from, n1, n2);
    }
    c->x[0] = 0;
    return 1;
}

int coop_glue_hook(Cpu *c, uint64_t addr) {   // coop_spawn.c coop_hook
    if (addr == MOD_MAIN_BASE + DO_SCOTCH) { capture(c); return 0; }
    if (addr == MOD_MAIN_BASE + DESTROY_STEP) return veto(c);
    return 0;
}

// ---- making a bond here
static int ident(uint64_t a, GlueObj *o) {
    int owner = 0;
    uint32_t id = 0;
    int coop_obj_ident(uint64_t a, int *owner, uint32_t *id);   // coop_players.c
    memset(o, 0, sizeof *o);
    coop_actor_name(a, o->name, sizeof o->name);
    if (!o->name[0] || strlen(o->name) >= NAME_N - 1 || !coop_obj_ident(a, &owner, &id)) return 0;
    o->owner = (uint32_t)owner, o->id = id;
    return 1;
}
// The request createScotch takes, built here (the argument's base class: its flag call answers 0, so createScotch's
// tail, main+0x1d61858, reads none of its vectors), then doScotch as main+0x1d61a6c calls it. 1: the entry is glued.
static int make_bond(Cpu *c, uint64_t a, uint64_t b, uint32_t ha, uint32_t hb, const float ma[12], const float mb[12], int flag,
                     uint64_t *entry_out) {
    uint64_t comp = scotch_comp(a);
    if (!comp || !scotch_comp(b)) return 0;
    uint64_t e = find_entry(comp, ha, hb);
    if (entry_glued(e)) { if (entry_out) *entry_out = e; return 1; }
    static _Alignas(16) uint8_t arg[0x100], req[0x40];   // the tick's only
    memset(arg, 0, sizeof arg), memset(req, 0, sizeof req);
    uint64_t ap = (uint64_t)(uintptr_t)arg;
    coop_guest_call(c, MOD_MAIN_BASE + ARG_CTOR, ap, 0, 0, 0);
    coop_guest_call(c, MOD_MAIN_BASE + LINK_SET, ap + 0x20, a, 0, 0);
    coop_guest_call(c, MOD_MAIN_BASE + LINK_SET, ap + 0x78, b, 0, 0);
    if (link_actor(ap + 0x78) != b) return 0;
    ST32(ap + 0x38, ha), memcpy(arg + 0x3c, ma, 48);
    ST32(ap + 0x90, hb), memcpy(arg + 0x94, mb, 48);
    ST64((uint64_t)(uintptr_t)req + 0x20, ap);
    coop_guest_call(c, MOD_MAIN_BASE + CREATE_SCOTCH, comp, (uint64_t)(uintptr_t)req, 0, 0);
    e = find_entry(comp, ha, hb);
    if (!e) return 0;
    ST32(e + 0x550, 2), ST8(e + 0x554, LD8(e + 0x554) | 8);
    uint64_t ra = coop_guest_call(c, MOD_MAIN_BASE + ENTRY_BODY_A, e, 0, 0, 0), rb = coop_guest_call(c, MOD_MAIN_BASE + ENTRY_BODY_B, e, 0, 0, 0);
    if (!ra || !rb) return 0;
    guest_call7(c, MOD_MAIN_BASE + DO_SCOTCH, e + 0x558, ap + 0x3c, ap + 0x94, ra, rb, e + 0x488, (uint64_t)(flag & 1));
    if (entry_out) *entry_out = e;
    return entry_glued(e);
}

// ---- packets
static void send_add(const GlueBond *g, int n) {
    GlueAddPacket p;
    memset(&p, 0, sizeof p);
    p.kind = GLUE_ADD, p.n = (uint32_t)n;
    memcpy(p.b, g, (size_t)n * sizeof *g);
    coop_net_hit_send(&p, (int)(offsetof(GlueAddPacket, b) + (size_t)n * sizeof *g));
}
static void send_list(void) {
    GlueListPacket p;
    memset(&p, 0, sizeof p);
    p.kind = GLUE_LIST;
    for (int i = 0; i < MINE_N; i++) if (mine[i].used) p.bond[p.n++] = mine[i].g.bond;
    coop_net_hit_send(&p, (int)(offsetof(GlueListPacket, bond) + p.n * sizeof p.bond[0]));
}
static int list_has(const GlueListPacket *p, uint32_t bond) {
    for (uint32_t i = 0; i < p->n; i++) if (p->bond[i] == bond) return 1;
    return 0;
}
static int pending_list_from[COOP_MAX_PLAYERS * 4], n_lists;   // makers whose list came this tick
static GlueListPacket lists[COOP_MAX_PLAYERS * 4];
int coop_glue_take(const void *data, int n, int from) {   // coop_world.c peer_tick: 1 if it was a glue packet
    if (n < 16) return 0;
    uint32_t kind = 0, cnt = 0;
    memcpy(&kind, data, 4), memcpy(&cnt, (const uint8_t *)data + 4, 4);
    if (kind == GLUE_ADD && cnt >= 1 && cnt <= ADD_N && n == (int)(offsetof(GlueAddPacket, b) + cnt * sizeof(GlueBond))) {
        if (!glue_on()) return 1;
        const GlueAddPacket *p = data;
        plat_mutex_lock(&glue_lock);
        for (uint32_t j = 0; j < cnt; j++) {
            GlueBond g = p->b[j];
            g.a.name[NAME_N - 1] = g.b.name[NAME_N - 1] = 0;
            int f = -1, free = -1;
            for (int i = 0; i < THEIRS_N && f < 0; i++) {
                if (theirs[i].used && theirs[i].from == from && theirs[i].g.bond == g.bond) f = i;
                else if (!theirs[i].used && free < 0) free = i;
            }
            if (f >= 0) { theirs[f].missed = 0; continue; }   // known (a resend)
            if (free < 0) {
                static int said;
                if (said++ < 5) rt_log("coop: glue: bond %u from player %d not kept: %d bonds of the others already", g.bond, from, THEIRS_N);
                continue;
            }
            memset(&theirs[free], 0, sizeof theirs[free]);
            theirs[free].used = 1, theirs[free].from = from, theirs[free].g = g;
            rt_log("coop: glue: bond %u from player %d: %s (player %u's %08x) to %s (player %u's %08x)", g.bond, from, g.a.name, g.a.owner,
                   g.a.id, g.b.name, g.b.owner, g.b.id);
        }
        plat_mutex_unlock(&glue_lock);
        return 1;
    }
    if (kind == GLUE_LIST && cnt <= LIST_N && n == (int)(offsetof(GlueListPacket, bond) + cnt * sizeof(uint32_t))) {
        if (glue_on() && n_lists < (int)(sizeof lists / sizeof lists[0])) {
            memcpy(&lists[n_lists], data, (size_t)n);
            pending_list_from[n_lists++] = from;
        }
        return 1;
    }
    return 0;
}

// ---- the tick (coop_world.c peer_tick)
static void glue_test(Cpu *c, uint64_t me);
static void take_pending(void) {
    __typeof__(pend[0]) got[PEND_N];
    plat_mutex_lock(&glue_lock);
    int n = pend_n;
    memcpy(got, pend, (size_t)n * sizeof got[0]);
    pend_n = 0;
    plat_mutex_unlock(&glue_lock);
    for (int k = 0; k < n; k++) {
        __typeof__(pend[0]) *p = &got[k];
        int dup = 0;
        for (int i = 0; i < MINE_N; i++) dup |= mine[i].used && mine[i].a == p->a && mine[i].b == p->b && mine[i].ha == p->ha && mine[i].hb == p->hb;
        if (dup) continue;
        GlueBond g;
        memset(&g, 0, sizeof g);
        int ok_a = ident(p->a, &g.a), ok_b = ident(p->b, &g.b);
        int ref_a = body_ref(p->a, p->body_a, g.body_a), ref_b = body_ref(p->b, p->body_b, g.body_b);
        if (!ok_a || !ok_b || !ref_a || !ref_b) {
            static int said;
            if (said++ < 10)
                rt_log("coop: glue: a bond made here between %s and %s is not sent: %s", g.a.name, g.b.name,
                       !ok_a || !ok_b ? "an object with no number across the games" : "a body not found from its physics controller");
            continue;
        }
        // (a check of the handles against the bodies doScotch got: the table read above is main+0x29a5a08's)
        if (handle_body(p->ha) != p->body_a || handle_body(p->hb) != p->body_b)
            rt_log("coop: glue: the handles %08x %08x do not give doScotch's bodies (%llx %llx, table %llx %llx)", p->ha, p->hb,
                   (unsigned long long)p->body_a, (unsigned long long)p->body_b, (unsigned long long)handle_body(p->ha),
                   (unsigned long long)handle_body(p->hb));
        int f = -1;
        for (int i = 0; i < MINE_N && f < 0; i++) if (!mine[i].used) f = i;
        if (f < 0) { rt_log("coop: glue: more than %d bonds made here; this one is not sent", MINE_N); continue; }
        g.bond = next_bond++, g.flag = p->flag;
        memcpy(g.ma, p->ma, sizeof g.ma), memcpy(g.mb, p->mb, sizeof g.mb);
        memset(&mine[f], 0, sizeof mine[f]);
        mine[f].used = 1, mine[f].g = g, mine[f].a = p->a, mine[f].b = p->b, mine[f].ha = p->ha, mine[f].hb = p->hb;
        mine[f].ua = actor_uid(p->a), mine[f].ub = actor_uid(p->b);
        mine[f].sent_us = frame_step_world_us();
        float pa[3] = {0}, pb[3] = {0};
        coop_rd(p->a + COOP_ACTOR_POS, pa, 12), coop_rd(p->b + COOP_ACTOR_POS, pb, 12);
        rt_log("coop: glue: made here: bond %u, %s (player %u's %08x, body +%#x/%#x) to %s (player %u's %08x, body +%#x/%#x), flag %d",
               g.bond, g.a.name, g.a.owner, g.a.id, g.body_a[0], g.body_a[1], g.b.name, g.b.owner, g.b.id, g.body_b[0], g.body_b[1], g.flag);
        // What the two matrices are (each body's own frame or the world's): their places against the actors'.
        rt_log("coop: glue: bond %u: A at %.2f %.2f %.2f, B at %.2f %.2f %.2f; matrix A place %.2f %.2f %.2f, matrix B place %.2f %.2f %.2f",
               g.bond, pa[0], pa[1], pa[2], pb[0], pb[1], pb[2], g.ma[3], g.ma[7], g.ma[11], g.mb[3], g.mb[7], g.mb[11]);
        send_add(&g, 1);
        send_list();
    }
}
static void mine_tick(Cpu *c, int64_t now) {
    static int64_t last_us;
    if (now - last_us < 1000000) return;
    last_us = now;
    GlueBond out[ADD_N];
    int n = 0, changed = 0;
    for (int i = 0; i < MINE_N; i++) {
        if (!mine[i].used) continue;
        int gone = !alive_same(mine[i].a, mine[i].g.a.name, mine[i].ua) || !alive_same(mine[i].b, mine[i].g.b.name, mine[i].ub);
        uint64_t e = gone ? 0 : find_entry(scotch_comp(mine[i].a), mine[i].ha, mine[i].hb);
        if (!gone && !entry_glued(e) && now < mine[i].remake_until) {   // parted with another game's bond: made again
            if (now >= mine[i].remake_from) {
                in_apply = 1;   // (not captured as a new bond)
                int ok = make_bond(c, mine[i].a, mine[i].b, mine[i].ha, mine[i].hb, mine[i].g.ma, mine[i].g.mb, mine[i].g.flag, NULL);
                in_apply = 0;
                rt_log("coop: glue: bond %u (%s to %s), parted here with another game's, %s", mine[i].g.bond, mine[i].g.a.name, mine[i].g.b.name,
                       ok ? "made again" : "not made again yet");
            }
            mine[i].gone_checks = 0;
            continue;
        }
        mine[i].gone_checks = gone || !entry_glued(e) ? mine[i].gone_checks + 1 : 0;
        if (gone || mine[i].gone_checks >= 2) {
            rt_log("coop: glue: bond %u (%s to %s) is %s here: taken off the list", mine[i].g.bond, mine[i].g.a.name, mine[i].g.b.name,
                   gone ? "on an object gone" : "no longer glued");
            mine[i].used = 0, changed = 1;
            continue;
        }
        if (now - mine[i].sent_us >= 3000000 && n < ADD_N) out[n++] = mine[i].g, mine[i].sent_us = now;
    }
    if (n) send_add(out, n);
    (void)changed;
    send_list();
}
static void part_here(Cpu *c, uint64_t a, int64_t now) {   // removeScotch on a, for its maker
    uint64_t comp = scotch_comp(a);
    uint32_t id = 0;
    if (!comp || !coop_rd(a + ACTOR_ID, &id, 4)) return;
    plat_mutex_lock(&glue_lock);
    int f = 0;
    for (int i = 0; i < 16; i++) if (parting[i].until_us < parting[f].until_us) f = i;
    parting[f].a = a, parting[f].until_us = now + 2000000;
    plat_mutex_unlock(&glue_lock);
    // removeScotch parts a from every bond, this game's own too: those are made again from 1 s on (its breaks come as
    // messages, later; the veto lets breaks on a through for 2 s) until 5 s on. Dropped, they left this game's list and
    // the others parted them as well (code review, 2026-11-07).
    for (int i = 0; i < MINE_N; i++)
        if (mine[i].used && (mine[i].a == a || mine[i].b == a)) mine[i].remake_from = now + 1000000, mine[i].remake_until = now + 5000000;
    in_apply = 1;
    coop_guest_call(c, MOD_MAIN_BASE + REMOVE_SCOTCH, comp, id, 0x2051000a, 0);
    in_apply = 0;
}
static void theirs_tick(Cpu *c, int64_t now) {
    // The makers' lists: a bond missing from two in a row is parted here.
    for (int l = 0; l < n_lists; l++) {
        uint64_t part[THEIRS_N];
        int np = 0;
        plat_mutex_lock(&glue_lock);
        for (int i = 0; i < THEIRS_N; i++) {
            if (!theirs[i].used || theirs[i].from != pending_list_from[l]) continue;
            if (list_has(&lists[l], theirs[i].g.bond)) { theirs[i].missed = 0; continue; }
            if (++theirs[i].missed < 2) continue;
            rt_log("coop: glue: bond %u from player %d (%s to %s) is gone there%s", theirs[i].g.bond, theirs[i].from, theirs[i].g.a.name,
                   theirs[i].g.b.name, theirs[i].made ? ": parted here" : "");
            // only an actor still the bond's (its pointer is from the last check, up to 2 s old: the object may be gone)
            if (theirs[i].made && alive_same(theirs[i].a, theirs[i].g.a.name, theirs[i].ua)) part[np++] = theirs[i].a;
            theirs[i].used = 0;
        }
        // the other bonds on a parted actor: checked again at once (removeScotch parts it from all of them)
        for (int i = 0; i < THEIRS_N; i++)
            for (int k = 0; k < np; k++)
                if (theirs[i].used && (theirs[i].a == part[k] || theirs[i].b == part[k])) theirs[i].check_us = 0, theirs[i].try_us = 0;
        plat_mutex_unlock(&glue_lock);
        for (int k = 0; k < np; k++) part_here(c, part[k], now);
    }
    n_lists = 0;
    // Each bond: its objects found, made here, retried once a second, checked every 2 s once made.
    for (int i = 0; i < THEIRS_N; i++) {
        plat_mutex_lock(&glue_lock);
        if (!theirs[i].used) { plat_mutex_unlock(&glue_lock); continue; }
        GlueBond g = theirs[i].g;
        int made = theirs[i].made, from = theirs[i].from;
        uint64_t a = theirs[i].a, b = theirs[i].b;
        uint32_t ua = theirs[i].ua, ub = theirs[i].ub;
        int64_t due = made ? theirs[i].check_us + 2000000 : theirs[i].try_us + 1000000;
        plat_mutex_unlock(&glue_lock);
        if (now < due) continue;
        uint64_t coop_obj_find(int owner, uint32_t id, const char *name);   // coop_players.c
        if (!alive_same(a, g.a.name, ua)) a = coop_obj_find((int)g.a.owner, g.a.id, g.a.name), ua = actor_uid(a);
        if (!alive_same(b, g.b.name, ub)) b = coop_obj_find((int)g.b.owner, g.b.id, g.b.name), ub = actor_uid(b);
        uint32_t ha = a ? body_handle(body_at(a, g.body_a)) : ~0u, hb = b ? body_handle(body_at(b, g.body_b)) : ~0u;
        int glued = 0, tried = 0;
        uint64_t e = 0;
        if (a && b && ha != ~0u && hb != ~0u) {
            glued = entry_glued(find_entry(scotch_comp(a), ha, hb));
            if (!glued) {
                tried = 1;
                in_apply = 1;
                glued = make_bond(c, a, b, ha, hb, g.ma, g.mb, g.flag, &e);
                in_apply = 0;
            }
        }
        plat_mutex_lock(&glue_lock);
        if (theirs[i].used && theirs[i].from == from && theirs[i].g.bond == g.bond) {
            int was = theirs[i].made;
            theirs[i].a = a, theirs[i].b = b, theirs[i].ua = ua, theirs[i].ub = ub, theirs[i].ha = ha, theirs[i].hb = hb, theirs[i].made = glued;
            if (made) theirs[i].check_us = now;
            if (!glued) theirs[i].try_us = now;
            if (tried) theirs[i].tries++;
            int t = theirs[i].tries;
            plat_mutex_unlock(&glue_lock);
            if (glued && !was)
                rt_log("coop: glue: bond %u from player %d made again here: %s to %s (try %d)", g.bond, from, g.a.name, g.b.name, t);
            else if (was && !glued)
                rt_log("coop: glue: bond %u from player %d no longer glued here: made again from the next second", g.bond, from);
            else if (!glued && (t <= 3 || t % 10 == 0))
                rt_log("coop: glue: bond %u from player %d not made here yet (try %d): %s", g.bond, from, t,
                       !a ? "the first object is not here" : !b ? "the second object is not here" : ha == ~0u || hb == ~0u ? "a body is not found"
                       : !e ? "createScotch made no entry" : "doScotch left it unglued");
        } else
            plat_mutex_unlock(&glue_lock);
    }
}
// The bonds of a player who left: no longer theirs to keep or part (no more lists come), so they are let go of once a
// second. The glue stays as it is here, and the veto no longer guards it. A player who comes back with the same id
// starts its bond numbers again from 1, which would otherwise be taken as resends of these.
static void left_tick(int64_t now) {
    static int64_t last_us;
    if (now - last_us < 1000000) return;
    last_us = now;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int n = coop_net_players(ids, names, COOP_MAX_PLAYERS);
    if (!coop_net_id()) return;   // not in a session (no relay answer yet): nothing known of who is here
    plat_mutex_lock(&glue_lock);
    for (int i = 0; i < THEIRS_N; i++) {
        if (!theirs[i].used) continue;
        int here = 0;
        for (int k = 0; k < n; k++) here |= ids[k] == theirs[i].from;
        if (here) continue;
        rt_log("coop: glue: bond %u from player %d let go of: the player left", theirs[i].g.bond, theirs[i].from);
        theirs[i].used = 0;
    }
    plat_mutex_unlock(&glue_lock);
}
void coop_glue_tick(Cpu *c, uint64_t me) {
    if (!glue_on()) return;
    int64_t now = frame_step_world_us();
    left_tick(now);
    glue_test(c, me);
    take_pending();
    mine_tick(c, now);
    theirs_tick(c, now);
}

// ---- Test (TOTK_COOP_TEST_GLUE=<name1>,<name2>, both games; tools/coop/glue_pair_3rd.txt): the game named A
// (TOTK_COOP_NAME) makes the two 1.5 m in front of its Link at 30 s, 0.8 m apart side by side, and 1 s after both
// exist glues them with the game's own calls (make_bond: createScotch and doScotch on their main bodies, each matrix
// that body's frame at the point between them, a guess at what the game passes); doScotch's entry then captures it as
// any glue made here. 20 s later it parts the first with removeScotch, as a shake of Ultrahand would. Each step is
// logged; it stops if either goes away. TOTK_COOP_TEST_GLUE=<n1>,<n2>,make: only made, for a player to glue them by
// hand with Ultrahand (docs/coop/proxy-plan.md says how). Objects that rest: AsbObj_WoodSquare_A_M_01,AsbObj_WoodRectangle_A_M_01
// (building planks); a Zonai cart or wheel rolls off the In-isa stairs.
static void glue_test(Cpu *c, uint64_t me) {
    static const char *spec;
    static int stage;
    static char names[2][48];
    static _Alignas(16) uint8_t handle[2][0x40], link[2][0x40];
    static uint64_t obj[2];
    static double glued_at;
    static int make_only;   // stage 2: when both were there; stage 3 on: when they were glued
    static int stack;
    if (!spec) {
        spec = cfg_get("TOTK_COOP_TEST_GLUE") ? cfg_get("TOTK_COOP_TEST_GLUE") : "";
        const char *who = cfg_get("TOTK_COOP_NAME");
        if (!who || strcmp(who, "A")) spec = "";
        const char *comma = strchr(spec, ',');
        if (comma && comma - spec < 47 && strlen(comma + 1) < 47) {
            memcpy(names[0], spec, (size_t)(comma - spec)), names[0][comma - spec] = 0;
            snprintf(names[1], sizeof names[1], "%s", comma + 1);
            char *third = strchr(names[1], ',');   // <n1>,<n2>,make: made only, for a player to glue by hand
            // <n1>,<n2>,stack: made only, the second 0.7 m above the first (it falls on it), for a scripted Ultrahand glue
            if (third) make_only = !strcmp(third + 1, "make") || (stack = !strcmp(third + 1, "stack")), *third = 0;
        } else if (*spec) {
            rt_log("coop: test: glue: TOTK_COOP_TEST_GLUE wants two names, a comma between");
            spec = "";
        }
    }
    if (!*spec || stage >= 9) return;
    double now = frame_step_world_us() / 1e6;
    float p[3], r[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    if (stage == 0 && now > 30) {   // forward: the rotation's third column; right: its first
        static const float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (int k = 0; k < 2; k++) {
            float s = stack ? 0 : k ? 0.4f : -0.4f;
            float at[3] = {p[0] + r[2] * 1.5f + r[0] * s, p[1] + 0.5f + (stack && k ? 0.7f : 0), p[2] + r[8] * 1.5f + r[6] * s};
            if (!coop_request_create(c, names[k], at, rot, handle[k])) { rt_log("coop: test: glue: %s not asked for", names[k]); stage = 9; return; }
            rt_log("coop: test: glue: %s asked for at %.2f %.2f %.2f", names[k], at[0], at[1], at[2]);
        }
        stage = 1;
    }
    if (stage == 1) {
        for (int k = 0; k < 2; k++)
            if (!obj[k] && coop_handle_status(handle[k]) == 3) {
                uint64_t ref;
                obj[k] = coop_claim(c, handle[k], link[k], &ref);
                rt_log("coop: test: glue: %s is %llx", names[k], (unsigned long long)obj[k]);
            }
        if (obj[0] && obj[1]) stage = make_only ? 9 : 2, glued_at = now;
        if (obj[0] && obj[1] && make_only) rt_log("coop: test: glue: both made; glue them with Ultrahand");
        if (now > 45) { rt_log("coop: test: glue: the two were not made in 15 s"); stage = 9; return; }
    }
    // Both still there (state 2..6, the name theirs): a Zonai cart made on the In-isa stairs rolled off and went away
    // 1.5 s after it was made, and the glue at 40 s read a gone actor (2026-11-07).
    if (stage >= 2 && stage <= 4)
        for (int k = 0; k < 2; k++)
            if (!alive_named(obj[k], names[k])) {
                rt_log("coop: test: glue: %s went away (%.1f s into the world); the test stops", names[k], now);
                stage = 9;
                return;
            }
    if (stage == 2 && now > glued_at + 1) {   // 1 s after both exist: before anything moves them far
        stage = 3, glued_at = now;
        uint64_t ba = coop_rd_ptr(phys_ctl(obj[0]) + 0x150), bb = coop_rd_ptr(phys_ctl(obj[1]) + 0x150);
        uint32_t ha = body_handle(ba), hb = body_handle(bb);
        float pa[3], pb[3], ra[9], rb[9];
        if (!scotch_comp(obj[0]) || !scotch_comp(obj[1]) || ha == ~0u || hb == ~0u || !coop_rd(obj[0] + COOP_ACTOR_POS, pa, 12) ||
            !coop_rd(obj[1] + COOP_ACTOR_POS, pb, 12) || !coop_rd(obj[0] + COOP_ACTOR_ROT, ra, 36) || !coop_rd(obj[1] + COOP_ACTOR_ROT, rb, 36)) {
            rt_log("coop: test: glue: not glued: scotch component %llx %llx, bodies %llx %llx, handles %08x %08x",
                   (unsigned long long)scotch_comp(obj[0]), (unsigned long long)scotch_comp(obj[1]), (unsigned long long)ba, (unsigned long long)bb, ha, hb);
            stage = 9;
            return;
        }
        // Each matrix: the body's frame at the middle point (rotation identity in the body, place in its frame).
        float mid[3] = {(pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2, (pa[2] + pb[2]) / 2}, ma[12], mb[12];
        const float *rr[2] = {ra, rb}, *pp[2] = {pa, pb};
        float *mm[2] = {ma, mb};
        for (int k = 0; k < 2; k++) {
            float d[3] = {mid[0] - pp[k][0], mid[1] - pp[k][1], mid[2] - pp[k][2]};
            for (int row = 0; row < 3; row++) {
                for (int j = 0; j < 3; j++) mm[k][row * 4 + j] = row == j;
                mm[k][row * 4 + 3] = rr[k][0 * 3 + row] * d[0] + rr[k][1 * 3 + row] * d[1] + rr[k][2 * 3 + row] * d[2];
            }
        }
        uint64_t e = 0;
        int ok = make_bond(c, obj[0], obj[1], ha, hb, ma, mb, 0, &e);   // in_apply 0: captured as glue made here
        rt_log("coop: test: glue: %s to %s: entry %llx, %s; its constraint object (doScotch's [entry+0x560]) %llx", names[0], names[1],
               (unsigned long long)e, ok ? "glued" : "not glued", (unsigned long long)(e ? coop_rd_ptr(e + 0x560) : 0));
        if (!ok) stage = 9;
    }
    {   // their glue components' group (+0x28: an autobuild's devices share one; coop_autobuild.c), 1 s and 4 s after
        static int said;
        if (stage == 3 && ((said == 0 && now > glued_at + 1) || (said == 1 && now > glued_at + 4))) {
            said++;
            uint32_t ha = body_handle(coop_rd_ptr(phys_ctl(obj[0]) + 0x150)), hb = body_handle(coop_rd_ptr(phys_ctl(obj[1]) + 0x150));
            uint64_t e = find_entry(scotch_comp(obj[0]), ha, hb), edge = e ? coop_rd_ptr(e + 0x560) : 0, w[2] = {0, 0};
            coop_rd(edge + 0xe8, &w[0], 8), coop_rd(edge + 0x28, &w[1], 8);
            rt_log("coop: test: glue: %.0f s after: groups %llx %llx; the edge %llx: +0xe8 %llx, +0x28 %llx, +0x70 %llx", now - glued_at,
                   (unsigned long long)coop_rd_ptr(scotch_comp(obj[0]) + 0x28), (unsigned long long)coop_rd_ptr(scotch_comp(obj[1]) + 0x28),
                   (unsigned long long)edge, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)coop_rd_ptr(edge + 0x70));
        }
    }
    if (stage == 3 && now > glued_at + 20) {
        stage = 4;
        uint64_t comp = scotch_comp(obj[0]);
        uint32_t id = 0;
        coop_rd(obj[0] + ACTOR_ID, &id, 4);
        uint32_t ha = body_handle(coop_rd_ptr(phys_ctl(obj[0]) + 0x150)), hb = body_handle(coop_rd_ptr(phys_ctl(obj[1]) + 0x150));
        coop_guest_call(c, MOD_MAIN_BASE + REMOVE_SCOTCH, comp, id, 0x2051000a, 0);
        uint64_t e = find_entry(comp, ha, hb);
        uint32_t st = 0;
        if (e) coop_rd(e + 0x550, &st, 4);
        rt_log("coop: test: glue: removeScotch on %s: its entry %s (state %u right after; the break is a message, later)", names[0],
               e ? "still listed" : "gone", st);
    }
    if (stage == 4 && now > glued_at + 22) {
        stage = 9;
        uint32_t ha = body_handle(coop_rd_ptr(phys_ctl(obj[0]) + 0x150)), hb = body_handle(coop_rd_ptr(phys_ctl(obj[1]) + 0x150));
        uint64_t e = find_entry(scotch_comp(obj[0]), ha, hb);
        uint32_t st = 0;
        if (e) coop_rd(e + 0x550, &st, 4);
        rt_log("coop: test: glue: 2 s after removeScotch: entry %s, state %u", e ? "still listed" : "gone", st);
    }
}

// Two actors here glued as the glue test glues them (each body's frame at the middle point between the actors), not
// captured as a bond of this game's (it is not sent). 1: glued. For coop_autobuild.c.
uint64_t coop_glue_last_entry;   // the entry coop_glue_pair last made
int coop_glue_pair(Cpu *c, uint64_t a, uint64_t b) {
    uint64_t ba = coop_rd_ptr(phys_ctl(a) + 0x150), bb = coop_rd_ptr(phys_ctl(b) + 0x150);
    uint32_t ha = body_handle(ba), hb = body_handle(bb);
    float pa[3], pb[3], ra[9], rb[9];
    if (!scotch_comp(a) || !scotch_comp(b) || ha == ~0u || hb == ~0u || !coop_rd(a + COOP_ACTOR_POS, pa, 12) ||
        !coop_rd(b + COOP_ACTOR_POS, pb, 12) || !coop_rd(a + COOP_ACTOR_ROT, ra, 36) || !coop_rd(b + COOP_ACTOR_ROT, rb, 36))
        return 0;
    float mid[3] = {(pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2, (pa[2] + pb[2]) / 2}, ma[12], mb[12];
    const float *rr[2] = {ra, rb}, *pp[2] = {pa, pb};
    float *mm[2] = {ma, mb};
    for (int k = 0; k < 2; k++) {
        float d[3] = {mid[0] - pp[k][0], mid[1] - pp[k][1], mid[2] - pp[k][2]};
        for (int row = 0; row < 3; row++) {
            for (int j = 0; j < 3; j++) mm[k][row * 4 + j] = row == j;
            mm[k][row * 4 + 3] = rr[k][0 * 3 + row] * d[0] + rr[k][1 * 3 + row] * d[1] + rr[k][2 * 3 + row] * d[2];
        }
    }
    in_apply = 1;
    uint64_t e = 0;
    int ok = make_bond(c, a, b, ha, hb, ma, mb, 0, &e);
    in_apply = 0;
    coop_glue_last_entry = e;
    return ok;
}
