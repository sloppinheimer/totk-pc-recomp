// Co-op: actor lifetime hooks for contraption sync v3 (docs/coop/contraption-sync-v3.md, sections 4 G4, 7 and 10).
//
// - The delete request main+0x63baa8(actor, reason) and its deferring wrapper main+0x63b6b0, the death request
//   main+0x1953184(actor, kind, 0) and the death component main+0x1b4b738 (the inlined lapse in main+0x2f48a0 calls it
//   directly), BondMaker main+0x1916830 (returns 0 when the bond pool is empty): logged for bondable actors (those
//   with a glue component, [actor+0x228]+0x480) with TOTK_COOP_TEST_X15=1 (experiment X15a: which paths delete parts on
//   an unload, at what distance, one by one or together, and the pool and group table sizes).
// - main+0x209b754 (the item add/remove routine the Autobuild confirm charges its Zonaite with): skipped while a
//   copy's confirm runs (coop_life_free_confirm), so a copy costs nothing; the confirm ignores its result.
// - TOTK_COOP_TEST_X6=1 (experiment X6): once, a few seconds into the world, whether this save has Autobuild
//   (IsGet.Obj_AutoBuilder through main+0x3d01a0) and the Autobuild ability singleton [main+0x3a62ef0] with its two
//   BuildObjects (+0x51f0 the build, +0x51f8 the menu thumbnail; +0x3c80 1 marks the thumbnail).
//
// The research behind the addresses is outside this repository (docs/coop/contraption-sync-v3.md, notation).

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "cpu.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int64_t frame_step_world_us(void);

#define DELETE_REQUEST 0x63baa8
#define DELETE_DEFER 0x63b6b0
#define DEATH_REQUEST 0x1953184
#define DEATH_COMPONENT 0x1b4b738
#define BOND_MAKER 0x1916830
#define ITEM_CHANGE 0x209b754
#define FLAG_GET_BOOL 0x3d01a0         // (member hash) -> bool, in the game-data struct IsGet
#define ISGET_AUTOBUILDER 0x5d930d12u  // IsGet.Obj_AutoBuilder
#define ISGET_ULTRAHAND 0xdc817e80u    // IsGet.Obj_UltraHand
#define AB_SINGLETON 0x3a62ef0         // the Autobuild ability (a game manager made at boot)
#define GROUP_TABLE 0x395f6f8          // [[main+0x395f6f8]]: +0x20 count, +0x28 groups (0xa70 bytes each)
#define BOND_POOL_ROOT 0x3ad27c8       // [[[[root]+0xc8]+0xe0]+0x188]: +0x20 free, +0x38 used

static __thread int free_confirm;   // a copy's confirm runs on this thread (coop_life_free_confirm): an item change
                                   // another thread makes meanwhile (the player picking something up) still counts

void coop_life_free_confirm(int on) { free_confirm = on; }

uint64_t coop_life_ability(void) { return coop_rd_ptr(MOD_MAIN_BASE + AB_SINGLETON); }

static int x15_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_X15") != NULL;
    return on;
}

static int bondable(uint64_t actor) { return actor && coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x480) != 0; }

// x30 and three frames up the x29 chain, as main+ offsets (0 outside main)
static void lr_chain(Cpu *c, uint64_t out[4]) {
    out[0] = c->x[30];
    uint64_t fp = c->x[29];
    for (int i = 1; i < 4; i++) {
        out[i] = fp ? coop_rd_ptr(fp + 8) : 0;
        fp = fp ? coop_rd_ptr(fp) : 0;
    }
    for (int i = 0; i < 4; i++) out[i] = out[i] >= MOD_MAIN_BASE && out[i] < MOD_MAIN_BASE + 0x10000000 ? out[i] - MOD_MAIN_BASE : 0;
}

static void log_actor_event(Cpu *c, const char *what, uint64_t actor, uint64_t arg) {
    if (!bondable(actor)) return;
    uint64_t lr[4];
    lr_chain(c, lr);
    // A far delete the wrapper defers is asked again every frame: each (actor, caller, reason) is logged once, with a
    // count of the repeats when it changes (the first run's 4000-line cap went on one wheel 100 m away).
    static struct { uint64_t actor, lr, arg; uint32_t n; } seen[256];
    static PlatMutex seen_lock = PLAT_MUTEX_INIT;
    plat_mutex_lock(&seen_lock);
    int at = -1, free_at = -1;
    for (int i = 0; i < 256 && at < 0; i++) {
        if (seen[i].actor == actor && seen[i].lr == lr[0] && seen[i].arg == arg) at = i;
        else if (!seen[i].actor && free_at < 0) free_at = i;
    }
    int repeat = at >= 0;
    if (repeat) seen[at].n++;
    else if (free_at >= 0) seen[free_at].actor = actor, seen[free_at].lr = lr[0], seen[free_at].arg = arg, seen[free_at].n = 1;
    plat_mutex_unlock(&seen_lock);
    if (repeat) return;
    static _Atomic int lines;
    if (atomic_fetch_add(&lines, 1) > 4000) return;   // (a cap all the same)
    char nm[48];
    coop_actor_name(actor, nm, sizeof nm);
    uint32_t st = 0, f28 = 0, a444 = 0;
    uint64_t a448 = 0;
    float p[3] = {0}, me[3] = {0};
    coop_rd(actor + COOP_ACTOR_STATE, &st, 4), coop_rd(actor + 0x28, &f28, 4), coop_rd(actor + 0x444, &a444, 4);
    coop_rd(actor + 0x448, &a448, 8), coop_rd(actor + COOP_ACTOR_POS, p, 12);
    uint64_t link = coop_player_actor();
    if (link) coop_rd(link + COOP_ACTOR_POS, me, 12);
    float d2 = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
    uint64_t spr = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x480), grp = spr ? coop_rd_ptr(spr + 0x28) : 0;
    uint32_t gn = 0;
    if (grp) coop_rd(grp + 0x58, &gn, 4);
    uint64_t stamps = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x3ad7498) + 0x150);
    rt_log("coop: x15: %.3f s %s %llx '%s' arg %llu lr main+%#llx <- %#llx <- %#llx <- %#llx; state %u, +0x28 %#x, +0x444 %#x, "
           "+0x448 %#llx (current %#llx), %.1f m from the Link, group %llx of %u",
           frame_step_world_us() / 1e6, what, (unsigned long long)actor, nm, (unsigned long long)arg, (unsigned long long)lr[0],
           (unsigned long long)lr[1], (unsigned long long)lr[2], (unsigned long long)lr[3], st, f28, a444, (unsigned long long)a448,
           (unsigned long long)stamps, __builtin_sqrtf(d2), (unsigned long long)grp, gn);
}

static uint64_t bond_pool(void) {
    uint64_t r = coop_rd_ptr(MOD_MAIN_BASE + BOND_POOL_ROOT);
    r = r ? coop_rd_ptr(r + 0xc8) : 0;
    r = r ? coop_rd_ptr(r + 0xe0) : 0;
    return r ? coop_rd_ptr(r + 0x188) : 0;
}

// Once a second with TOTK_COOP_TEST_X15: the group table's slot count and the bond pool's free and used counts.
static void x15_counts(void) {
    static int64_t last;
    int64_t now = frame_step_world_us();
    if (now - last < 1000000) return;
    last = now;
    uint64_t gt = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + GROUP_TABLE)), pool = bond_pool();
    int32_t slots = -1, fr = -1, used = -1;
    if (gt) coop_rd(gt + 0x20, &slots, 4);
    if (pool) coop_rd(pool + 0x20, &fr, 4), coop_rd(pool + 0x38, &used, 4);
    static int32_t s0 = -2, f0 = -2, u0 = -2;
    if (slots != s0 || fr != f0 || used != u0)
        rt_log("coop: x15: %.1f s group table %d slots; bond pool %d free, %d used", now / 1e6, slots, fr, used), s0 = slots, f0 = fr, u0 = used;
}

// ---- X3 (plan 15.3): what separates bonded parts. While a watch window is open (coop_life_watch_parts, from the copy
// maker: the copy's parts, from its confirm for a few seconds) every call of the edge-removal paths is logged once per
// caller chain (and again at each power of two), with which watched part's actor, body or glue component its first
// three arguments name: ConnectionGraph::removeEdge main+0x2995024 and its detach main+0x2995068, main+0x2999fe8 (the
// edge's own removal), main+0x18ac48 (a flush detaching every edge of a body: request bit 22), main+0x8aaab0 (what
// queues that bit), removeScotch main+0x1c042bc, and the glue-edge break main+0x1cf54d4.
#define X3_N 21
static struct { uint64_t actor[X3_N], body[X3_N], spr[X3_N]; int n; int64_t until; } x3w;
static PlatMutex x3_lock = PLAT_MUTEX_INIT;
void coop_life_watch_parts(const uint64_t *parts, int n, int64_t until_us) {
    plat_mutex_lock(&x3_lock);
    x3w.n = n < X3_N ? n : X3_N, x3w.until = until_us;
    for (int i = 0; i < x3w.n; i++) {
        uint64_t a = parts[i];
        x3w.actor[i] = a;
        x3w.body[i] = a ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) + 0x150) : 0;
        x3w.spr[i] = a ? coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) : 0;
    }
    plat_mutex_unlock(&x3_lock);
    rt_log("coop: x3: watching %d parts until %.2f s", n, until_us / 1e6);
}
static int x3_names(uint64_t v, char *out, size_t sz) {   // which watched thing v is (or points 0x150 into a body of)
    for (int i = 0; v && i < x3w.n; i++) {
        if (v == x3w.actor[i]) return snprintf(out, sz, "(part %d actor)", i);
        if (v == x3w.body[i]) return snprintf(out, sz, "(part %d body)", i);
        if (v == x3w.body[i] + 0x150) return snprintf(out, sz, "(part %d body+0x150)", i);
        if (v == x3w.spr[i]) return snprintf(out, sz, "(part %d glue)", i);
    }
    return 0;
}
static void x3_hook(Cpu *c, uint64_t off) {
    char who[3][32] = {"", "", ""};
    int any = 0;
    plat_mutex_lock(&x3_lock);
    int open = x3w.n && frame_step_world_us() <= x3w.until;
    for (int a = 0; open && a < 3; a++) any |= x3_names(c->x[a], who[a], sizeof who[a]) > 0;
    plat_mutex_unlock(&x3_lock);
    if (!open) return;
    uint64_t lr[4];
    lr_chain(c, lr);
    static struct { uint64_t off, lr0, lr1; uint32_t n; } seen[128];
    static PlatMutex sl = PLAT_MUTEX_INIT;
    plat_mutex_lock(&sl);
    int at = -1;
    for (int i = 0; i < 128 && at < 0; i++) {
        if (seen[i].off == off && seen[i].lr0 == lr[0] && seen[i].lr1 == lr[1]) at = i;
        else if (!seen[i].off) seen[i].off = off, seen[i].lr0 = lr[0], seen[i].lr1 = lr[1], at = i;
    }
    uint32_t n = at >= 0 ? ++seen[at].n : 0;
    plat_mutex_unlock(&sl);
    if (n == 1 || any || (n & (n - 1)) == 0)
        rt_log("coop: x3: %.3f s main+%#llx (call %u from this chain) lr main+%#llx <- %#llx <- %#llx <- %#llx; x0 %llx %s, x1 %llx %s, x2 %llx %s",
               frame_step_world_us() / 1e6, (unsigned long long)off, n, (unsigned long long)lr[0], (unsigned long long)lr[1],
               (unsigned long long)lr[2], (unsigned long long)lr[3], (unsigned long long)c->x[0], who[0], (unsigned long long)c->x[1], who[1],
               (unsigned long long)c->x[2], who[2]);
}

// X3: the bond-strain check main+0x190f034(dt in s0, strand x0, ..., &actor A x4, &actor B x5), run each frame for each
// glued bond in a glue component's +0x170 list. The strand keeps a strain value at +0x1d4: while the distance between
// its two pivots (plus an angle term) exceeds a threshold scaled by the parts' bond strength it rises, at 1 the edge is
// removed (main+0x2999fe8); below it falls 10 a second. +0x1e2 set exempts the strand. Logged, while the watch window is
// open, for strands of watched parts: each strand once, then whenever its strain rises.
static void x3_strain(Cpu *c) {
    uint64_t st = c->x[0], aa = coop_rd_ptr(c->x[4]), ab = coop_rd_ptr(c->x[5]);
    char wa[32] = "", wb[32] = "";
    plat_mutex_lock(&x3_lock);
    int open = x3w.n && frame_step_world_us() <= x3w.until;
    int ours = open && (x3_names(aa, wa, sizeof wa) | x3_names(ab, wb, sizeof wb));
    plat_mutex_unlock(&x3_lock);
    if (!ours) return;
    float s0;
    uint32_t s0b = (uint32_t)c->v[0];
    memcpy(&s0, &s0b, 4);
    float acc = 0, prev_err = 0, min_err = 0;
    uint8_t exempt = 0, flags = 0, e0 = 0;
    coop_rd(st + 0x1d4, &acc, 4), coop_rd(st + 0x1d8, &prev_err, 4), coop_rd(st + 0x1dc, &min_err, 4);
    coop_rd(st + 0x1e2, &exempt, 1), coop_rd(st + 0x28, &flags, 1), coop_rd(st + 0x1e0, &e0, 1);
    static struct { uint64_t st; float acc; } seen[64];
    static PlatMutex sl = PLAT_MUTEX_INIT;
    plat_mutex_lock(&sl);
    int at = -1, say = 0;
    for (int i = 0; i < 64 && at < 0; i++) {
        if (seen[i].st == st) at = i;
        else if (!seen[i].st) seen[i].st = st, seen[i].acc = -1, at = i;
    }
    if (at >= 0 && (seen[at].acc < 0 || acc > seen[at].acc + 0.01f)) say = 1;
    if (at >= 0) seen[at].acc = acc;
    plat_mutex_unlock(&sl);
    if (say) {   // (each actor's main body: its flags at +0x68, as the original's are logged by the mirror)
        uint64_t ba = aa ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(aa + 0x228) + 0x50) + 0x20) + 0x150) : 0;
        uint64_t bb = ab ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ab + 0x228) + 0x50) + 0x20) + 0x150) : 0;
        uint64_t fa = 0, fb = 0;
        if (ba) coop_rd(ba + 0x68, &fa, 8);
        if (bb) coop_rd(bb + 0x68, &fb, 8);
        rt_log("coop: x3: body flags %#llx and %#llx", (unsigned long long)fa, (unsigned long long)fb);
    }
    if (say)
        rt_log("coop: x3: %.3f s strand %llx between %s and %s: strain %.3f, last error %.3f m (lowest %.3f), +0x28 %#x, +0x1e0 %u, "
               "exempt %u, dt %.4f", frame_step_world_us() / 1e6, (unsigned long long)st, wa[0] ? wa : "another actor", wb[0] ? wb : "another actor",
               acc, prev_err, min_err, flags, e0, exempt, (double)s0);
}

// X3: messages posted to an actor's queue, main+0x1c0c9b0(the receiver's queue: actor+0x378, x1, &{sender id, message id,
// flag}, &arg). The glue messages 0x20510000..0x2051000f (0x2051000c breaks a glue edge, main+0x1c0bf20 handles them)
// posted to a watched part are logged with the poster's chain: the poster, not the queue's later delivery.
static void x3_message(Cpu *c) {
    uint32_t id = 0;
    if (!coop_rd(c->x[2] + 4, &id, 4) || (id & 0xfffffff0u) != 0x20510000u) return;
    char who[32] = "";
    plat_mutex_lock(&x3_lock);
    int open = x3w.n && frame_step_world_us() <= x3w.until, ours = open && x3_names(c->x[0] - 0x378, who, sizeof who) > 0;
    plat_mutex_unlock(&x3_lock);
    if (!ours) return;
    uint64_t lr[4];
    lr_chain(c, lr);
    int32_t sender = 0;
    coop_rd(c->x[2], &sender, 4);
    rt_log("coop: x3: %.3f s message %#x to %s from actor id %d: lr main+%#llx <- %#llx <- %#llx <- %#llx", frame_step_world_us() / 1e6, id, who,
           sender, (unsigned long long)lr[0], (unsigned long long)lr[1], (unsigned long long)lr[2], (unsigned long long)lr[3]);
}

// G4 (contraption-sync-v3.md section 4): a part of a copy this game does not run takes no death from this game's own
// rules: every death request on it (main+0x1953184, and the death component main+0x1b4b738 the inlined lapse calls
// directly) is refused, and so are the deletes the green kill and the lapse fall back to (from main+0x272178,
// main+0x2f9264, main+0x2f7528). The game that runs the contraption decides its parts' deaths (P4 replays them here).
// TOTK_COOP_G4=0: off (a test). Each refusal is counted by (kind, caller) and logged the first time and at powers of two.
int coop_contraption_follower(uint64_t a);
static int g4_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_G4"); on = !(e && *e == '0'); }
    return on;
}
static int g4_refuse(Cpu *c, const char *what, uint64_t actor, uint64_t arg) {
    if (!g4_on() || !coop_contraption_follower(actor)) return 0;
    uint64_t lr[4];
    lr_chain(c, lr);
    static struct { uint64_t lr; uint64_t arg; uint32_t n; } seen[64];
    static PlatMutex sl = PLAT_MUTEX_INIT;
    plat_mutex_lock(&sl);
    uint32_t n = 0;
    for (int i = 0; i < 64; i++) {
        if (seen[i].lr == lr[0] && seen[i].arg == arg) { n = ++seen[i].n; break; }
        if (!seen[i].lr) { seen[i].lr = lr[0], seen[i].arg = arg, n = seen[i].n = 1; break; }
    }
    plat_mutex_unlock(&sl);
    if (n && (n & (n - 1)) == 0) {
        char nm[48];
        rt_log("coop: g4: %s %llu of a follower part %llx '%s' refused (%u from main+%#llx <- %#llx)", what, (unsigned long long)arg,
               (unsigned long long)actor, coop_actor_name(actor, nm, sizeof nm), n, (unsigned long long)lr[0], (unsigned long long)lr[1]);
    }
    c->x[0] = 0;
    return 1;
}

int coop_life_hook(Cpu *c, uint64_t addr) {
    uint64_t off = addr - MOD_MAIN_BASE;
    if (off == 0x2995024 || off == 0x2995068 || off == 0x2999fe8 || off == 0x18ac48 || off == 0x8aaab0 || off == 0x1c042bc ||
        off == 0x1cf54d4) {
        x3_hook(c, off);
        return 0;
    }
    if (off == 0x190f034) { x3_strain(c); return 0; }
    if (off == 0x1c0c9b0) { x3_message(c); return 0; }
    if (off == ITEM_CHANGE) {
        if (!free_confirm) return 0;
        static int said;
        if (!said++) rt_log("coop: autobuild: a copy's confirm charged nothing (main+%#x skipped)", ITEM_CHANGE);
        c->x[0] = 0;
        return 1;   // (the confirm ignores the result)
    }
    if (off == DEATH_REQUEST && g4_refuse(c, "death", c->x[0], c->x[1])) return 1;
    if (off == DEATH_COMPONENT) {   // (x0 the component; its actor at +0x8)
        uint64_t a = coop_rd_ptr(c->x[0] + 8);
        if (g4_refuse(c, "death (component)", a, c->x[1])) return 1;
    }
    if (off == DELETE_DEFER || off == DELETE_REQUEST) {   // only the fallbacks of the green kill and the lapse
        uint64_t lr = c->x[30] - MOD_MAIN_BASE;
        if ((lr == 0x272178 || lr == 0x2f9264 || lr == 0x2f7528) && g4_refuse(c, "delete", c->x[0], c->x[1])) return 1;
    }
    if (!x15_on()) return 0;
    x15_counts();
    if (off == DELETE_REQUEST) log_actor_event(c, "delete", c->x[0], c->x[1]);
    else if (off == DELETE_DEFER) log_actor_event(c, "delete (deferring)", c->x[0], c->x[1]);
    else if (off == DEATH_REQUEST) log_actor_event(c, "death", c->x[0], c->x[1]);
    else if (off == DEATH_COMPONENT) {   // (x0 is the component; its actor is at +0x8 like other components: logged as is when not)
        uint64_t a = coop_rd_ptr(c->x[0] + 8);
        log_actor_event(c, "death (component)", bondable(a) ? a : c->x[0], c->x[1]);
    } else if (off == BOND_MAKER) {
        uint64_t pool = bond_pool();
        int32_t fr = -1;
        if (pool) coop_rd(pool + 0x20, &fr, 4);
        if (fr < 1) rt_log("coop: x15: %.3f s BondMaker called with the bond pool empty (%d free)", frame_step_world_us() / 1e6, fr);
    }
    return 0;
}

// TOTK_COOP_TEST_X6: from GameDataMgr::calc (actor phase 0), 8 s into the world, once.
void coop_life_x6_probe(Cpu *c) {
    static int done = -1;
    if (done < 0) done = cfg_get("TOTK_COOP_TEST_X6") ? 0 : 1;
    if (done || frame_step_world_us() < 8000000) return;
    done = 1;
    int ab = (int)(coop_guest_call(c, MOD_MAIN_BASE + FLAG_GET_BOOL, ISGET_AUTOBUILDER, 0, 0, 0) & 1);
    int uh = (int)(coop_guest_call(c, MOD_MAIN_BASE + FLAG_GET_BOOL, ISGET_ULTRAHAND, 0, 0, 0) & 1);
    uint64_t a = coop_life_ability(), o = a ? coop_rd_ptr(a + 0x51f0) : 0, t = a ? coop_rd_ptr(a + 0x51f8) : 0;
    uint64_t vt = a ? coop_rd_ptr(a) : 0;
    uint8_t ot = 0xff, tt = 0xff;
    if (o) coop_rd(o + 0x3c80, &ot, 1);
    if (t) coop_rd(t + 0x3c80, &tt, 1);
    rt_log("coop: x6: this save has Autobuild %d, Ultrahand %d; the ability %llx (vtable main+%#llx), build object %llx "
           "(thumbnail flag %u), menu object %llx (thumbnail flag %u)",
           ab, uh, (unsigned long long)a, (unsigned long long)(vt ? vt - MOD_MAIN_BASE : 0), (unsigned long long)o, ot,
           (unsigned long long)t, tt);
}
