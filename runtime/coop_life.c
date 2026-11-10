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
static _Atomic uint32_t g4_refused;   // all refusals since the start (coop_life_refused, the clean-run line)
uint32_t coop_life_refused(void) { return atomic_load(&g4_refused); }
static __thread int death_allowed;   // (coop_ctn.c asks a copy the death its part had where the build runs)
void coop_life_death_allow(int on) { death_allowed = on; }
static int g4_refuse(Cpu *c, const char *what, uint64_t actor, uint64_t arg) {
    if (death_allowed || !g4_on() || !coop_contraption_follower(actor)) return 0;
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
    atomic_fetch_add(&g4_refused, 1);
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
    // A bond made or broken here: the builds recorded at once (coop_ctn.c). Before the x3 branch, which returns for the
    // break addresses (review 1 findings 68 and 79: breaks never reached it).
    if (off == BOND_MAKER || off == 0x1cf54d4 || off == 0x2999fe8 || off == 0x1c042bc) {
        void coop_ctn_structure_event(void);
        coop_ctn_structure_event();
    }
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
    if (off == DEATH_REQUEST) { void coop_ctn_death_seen(uint64_t actor, uint32_t kind); coop_ctn_death_seen(c->x[0], (uint32_t)c->x[1]); }
    if (off == DEATH_COMPONENT) {
        // x0 is the death component. The game's function takes the actor from component +0x18 (research notes
        // sync-v3/m2/p5/rocket-timebomb, section 3); this read +0x8 before, which that reading does not support.
        uint64_t a = coop_rd_ptr(c->x[0] + 0x18);
        if (!bondable(a)) a = coop_rd_ptr(c->x[0] + 8);
        if (g4_refuse(c, "death (component)", a, c->x[1])) return 1;
        void coop_ctn_death_seen(uint64_t actor, uint32_t kind);
        coop_ctn_death_seen(a, (uint32_t)c->x[1]);
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
    else if (off == DEATH_COMPONENT) {   // (x0 is the component; its actor is at +0x18: logged as is when not)
        uint64_t a = coop_rd_ptr(c->x[0] + 0x18);
        if (!bondable(a)) a = coop_rd_ptr(c->x[0] + 8);
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

// The contraption sync v3 hooks (cpu.h COOP_HOOKED_V3): the gates G1 to G8, the touch hooks of section 5 and the
// stand-in hooks of section 9. Each phase gives its own their behaviour; until then they let the game run as is.
int coop_ctn_sender_entry(uint64_t group);
int coop_ctn_driven(uint64_t a);
int coop_phase0_hook(Cpu *c, uint64_t addr);
int coop_ab_force_inert(void);
static __thread int v3_inner, in_follow_sender, in_follow_cannon;
static _Atomic uint32_t sender_dropped, shots_kept;
uint32_t coop_life_sender_dropped(void) { return atomic_load(&sender_dropped); }
uint32_t coop_life_shots_kept(void) { return atomic_load(&shots_kept); }
static uint64_t v3_node_actor(uint64_t node) {   // an AI node's actor ([[node +8] +8] +0x1f8)
    return node ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(node + 8) + 8) + 0x1f8) : 0;
}
// The hit filter of the device toggle (main+0x1d38a00, x0 = the device, result in w0): it walks the device's contact
// records (list [[[device +0x228] +0x428] +0x50]: base +0x10, count min(+8, +0x18), 0x1c0 bytes each) and answers 1 when
// one is a weapon, shield or arrow contact of a Player. The attacker of a record is the actor of its handle at +0x60
// (who owns the other body: the weapon), or, when byte +0x94 bit 0 is set, of the handle at +0x78 (the weapon's
// topmost owner: who wields it). A handle is {+8 reference, +0x10 serial}; the reference's +0x48 is the serial while it
// is good and its +0x40 the actor. Research notes sync-v3/m2/p6/hit-attacker.
// Another player's stand-in swings in this game too, and its weapon reaches the real devices here; the same swing in
// its own game is sent as the state wanted. So a hit whose every contact is a stand-in's is no hit here: counted once,
// where the player is. (A frame in which a stand-in's and this Link's contacts are both on one device is taken as this
// Link's; the three receiver-side toggle copies read no actor and are not covered.)
int coop_players_all(uint64_t *out, int max);
static uint64_t hnd_actor(uint64_t h) {
    int32_t serial = -1, have = 0, st = 0;
    uint64_t ref = coop_rd_ptr(h + 8);
    if (!ref || !coop_rd(h + 0x10, &serial, 4) || serial == -1 || !coop_rd(ref + 0x48, &have, 4) || have != serial) return 0;
    uint64_t a = coop_rd_ptr(ref + 0x40);
    return a && coop_rd(a + 0x24, &st, 4) && (st < 7 || st > 11) ? a : 0;
}
static uint64_t owner_step(uint64_t a) {   // who holds or shot a: the first four owner links of main+0x1d19380, in its order
    static const uint32_t link[4][2] = {{0x208, 0x20}, {0x508, 0x530}, {0x120, 0x2f0}, {0x440, 0x70}};
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0;
    for (int k = 0; t && k < 4; k++) {
        uint64_t comp = coop_rd_ptr(t + link[k][0]), o = comp ? hnd_actor(comp + link[k][1]) : 0;
        if (o) return o;
    }
    return 0;
}
static int contact_of_standin(uint64_t rec, const uint64_t *st, int n) {
    uint8_t f94 = 0;
    uint64_t a = hnd_actor(rec + 0x60), b = coop_rd(rec + 0x94, &f94, 1) && (f94 & 1) ? hnd_actor(rec + 0x78) : 0;
    for (int i = 0; i < n; i++) if (st[i] == a || st[i] == b) return 1;
    for (int step = 0; a && step < 4; step++) {
        a = owner_step(a);
        for (int i = 0; a && i < n; i++) if (st[i] == a) return 1;
    }
    return 0;
}
static _Atomic uint32_t standin_hits;
uint32_t coop_life_standin_hits(void) { return atomic_load(&standin_hits); }
static int only_standin_contacts(uint64_t victim) {
    uint64_t st[8];
    int n = coop_players_all(st, 8);
    uint64_t t = victim ? coop_rd_ptr(victim + 0x228) : 0, atk = t ? coop_rd_ptr(t + 0x428) : 0, l = atk ? coop_rd_ptr(atk + 0x50) : 0;
    uint32_t cap = 0, cnt = 0;
    uint64_t base = l ? coop_rd_ptr(l + 0x10) : 0;
    if (n <= 0 || !base || !coop_rd(l + 8, &cap, 4) || !coop_rd(l + 0x18, &cnt, 4)) return 0;
    if (cnt > cap) cnt = cap;
    if (!cnt || cnt > 64) return 0;
    for (uint32_t k = 0; k < cnt; k++) if (!contact_of_standin(base + (uint64_t)k * 0x1c0, st, n)) return 0;
    return 1;
}
int coop_ctn_part(uint64_t a), coop_ctn_follows(uint64_t a), coop_is_copy(uint64_t a);
int coop_own_sent_objects(uint64_t *out, int max);
static int synced_device(uint64_t a) {
    if (coop_ctn_part(a) || coop_ctn_follows(a) || coop_is_copy(a)) return 1;
    uint64_t own[256];
    int n = coop_own_sent_objects(own, 256);
    for (int i = 0; i < n; i++) if (own[i] == a) return 1;
    return 0;
}
void coop_ctn_note_fuse(uint64_t node, uint64_t actor);
int coop_life_v3(Cpu *c, uint64_t off) {
    if (off == 0x1d38a00) {
        static __thread int inner;
        if (inner) { inner = 0; return 0; }
        uint64_t victim = c->x[0];
        inner = 1;
        c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        // (only on a device the core syncs: a part of a build, a lone device followed here, or an object the object
        // mirror sends or copies. A device the map placed in each game is its own in each, and a stand-in's hit there
        // is the hit its player made in that game too: review 1 finding 85)
        if ((c->x[0] & 1) && synced_device(victim) && only_standin_contacts(victim)) {
            if (atomic_fetch_add(&standin_hits, 1) < 8) rt_log("coop: contraption: a stand-in's hit on a device here is not taken as a hit (its own game sends it)");
            c->x[0] = 0;
        }
        return 1;
    }
    if (off == 0x2474478) { coop_ctn_note_fuse(c->x[0], v3_node_actor(c->x[0])); return 0; }   // (a time bomb's fuse node: coop_ctn.c)
    // The group's on/off sender (x0 = group). On a build another game runs it sends nothing here: its rules (some
    // devices on and some off: ON to all; a hit while one is off: ON to all; the gauge cannot pay: OFF) would switch
    // a copy against the game that runs it, whose state comes by STATE. The sender still runs, so the group flags it
    // consumes are consumed; each message it queues (main+0x2b76c34) is dropped. Research notes
    // sync-v3/m2/p5/setoff-not-taking, section 3.
    if (off == 0x32eb90) {
        if (v3_inner) { v3_inner = 0; return 0; }
        if (!coop_ctn_sender_entry(c->x[0])) return 0;
        v3_inner = 1, in_follow_sender = 1;
        c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        in_follow_sender = 0;
        return 1;
    }
    if (off == 0x2b76c34) {
        if (!in_follow_sender) return 0;
        atomic_fetch_add(&sender_dropped, 1);
        c->x[0] = 0;
        return 1;
    }
    // The wheel's inert test (the call that returns to main+0x22fa5e8): true for the one run that puts a followed
    // wheel's motors off (coop_autobuild.c, the wheel node)
    if (off == 0x1c02914) {
        if (!coop_ab_force_inert() || c->x[30] - MOD_MAIN_BASE != 0x22fa5e8) return 0;
        c->x[0] = 1;
        return 1;
    }
    if (off == 0x22fa1b0) return coop_phase0_hook(c, MOD_MAIN_BASE + off);
    // A followed stake does not pin itself to this game's ground (main+0x2215364, x0 = its node, points its
    // "ToGround" constraint at the body it hit): where it runs it is pinned, and its pose comes from there.
    if (off == 0x2215364) return coop_ctn_driven(v3_node_actor(c->x[0]));
    // A followed cannon shoots no ball here: its fire function (main+0x224e690, x0 = its node) runs, and the one call
    // that launches a ball, Shooter::shoot main+0x1bf8df4, answers "none to take" (-1) inside it. The ball is the
    // object of the game that runs the cannon. Research notes sync-v3/m2/p5/spawns.
    if (off == 0x224e690) {
        if (v3_inner) { v3_inner = 0; return 0; }
        if (!coop_ctn_driven(v3_node_actor(c->x[0]))) return 0;
        v3_inner = 1, in_follow_cannon = 1;
        c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        in_follow_cannon = 0;
        return 1;
    }
    if (off == 0x1bf8df4) {
        if (!in_follow_cannon) return 0;
        atomic_fetch_add(&shots_kept, 1);
        c->x[0] = (uint64_t)-1;
        return 1;
    }
    return 0;
}

// I16 (contraption-sync-v3.md section 9): a stand-in never collides with a contraption part, in any game. The game
// decides every body pair through one predicate, main+0x40a240(filter, record A, record B, flags) -> 1 collide / 0 not.
// Its two callers pass each body's collision filter record: main+0x6ab7d8 [body+0x140] + 8 (the first physics world),
// main+0x75fc94 [body+0x148] + 8 (the second, for a body with flag bit 5 at +0x68). The hook answers 0 for a pair of
// one stand-in body and one part body and leaves every other pair to the game: the stand-in still stands on the ground
// and the parts still meet everything else.
//
// "A stand-in body" is every rigid body of the stand-in's Player actor and of the gear actors it holds (sword, shield
// and bow are actors of their own, each with a solid body warped to the hand every frame). A Player's bodies are not
// one pointer: its physics object Ctl = [[[actor+0x228]+0x50]+0x20] holds a main body, a character controller's body,
// a named body list, entity sets, sensor sets, a provider of more sets and an extra holder with two lists, and the
// game's own "every body of this actor" walk main+0x3cc9b4 (with main+0x2962c58) visits them all. all_bodies below is
// that walk, read instruction by instruction (research notes player-physics/all_bodies.c, where each line carries the
// address it was read at). The first version of this veto listed [Ctl+0x150] alone; a second body of the stand-in's
// then arrived inside a build's steering stick, pushed it out of its place and held it there until the stick's bond
// strained through and the game removed it (plan section 28; research notes physics/README.md section 8).
// The same walk lists the parts' bodies.
//
// The two sets of record holders are rebuilt four times a second from the co-op tick (coop_life_veto_rebuild) and read
// here, on physics job threads, without a lock: two buffers and an index, each with a small bit filter in front so
// that the predicate, which runs for every pair the engine considers, costs two bit tests when neither body is a
// stand-in's. A remote player's weight on a build is not given by its stand-in's body (plan 9: an explicit force, P6).
// TOTK_COOP_I16=0: off.
#define VETO_S 384
#define VETO_P 2560
static struct { uint64_t s[VETO_S], p[VETO_P], sbits[4], pbits[16]; int ns, np; } veto_buf[2];
static _Atomic int veto_cur = -1;
static _Atomic uint32_t veto_count, veto_seen;   // pairs refused; pairs with a stand-in body at all
static unsigned veto_hash(uint64_t h) { return (unsigned)((h >> 4) ^ (h >> 13)); }
static int veto_has(const uint64_t *l, int n, uint64_t v) {
    for (int i = 0; i < n; i++) if (l[i] == v) return 1;
    return 0;
}
// ---- every rigid body of an actor (main+0x3cc9b4 and main+0x2962c58) ----
#define AB_CAP 256   // (ours: the game checks no count)
static void ab_add(uint64_t *out, int *n, int max, uint64_t body) {
    if (!body) return;
    for (int i = 0; i < *n; i++) if (out[i] == body) return;
    if (*n < max) out[(*n)++] = body;
}
static void ab_set(uint64_t set, uint64_t *out, int *n, int max) {   // a body set: count +0x10, slots +0x18 of 0x18 bytes, body at slot +8
    int32_t cnt = 0;
    if (!set || !coop_rd(set + 0x10, &cnt, 4) || cnt <= 0) return;
    if (cnt > AB_CAP) cnt = AB_CAP;
    uint64_t slots = coop_rd_ptr(set + 0x18);
    for (int k = 0; slots && k < cnt; k++) ab_add(out, n, max, coop_rd_ptr(slots + (uint64_t)k * 0x18 + 8));
}
// entity sets (count Ctl+0x18, records Ctl+0x20) or sensor sets (Ctl+0x38, Ctl+0x40): the local ones, set = [record+8],
// then those of the provider [Ctl+0x128], asked as the game asks: count = vtable+0x80 / +0xa0 (provider), set =
// vtable+0xd0 / +0xe0 (provider, index past the local ones)
static void ab_sets(Cpu *c, uint64_t ctl, int sensor, uint64_t *out, int *n, int max) {
    uint64_t cnt_off = sensor ? 0x38 : 0x18, arr_off = sensor ? 0x40 : 0x20, v_count = sensor ? 0xa0 : 0x80, v_get = sensor ? 0xe0 : 0xd0;
    int32_t local = 0;
    if (!coop_rd(ctl + cnt_off, &local, 4) || local < 0) local = 0;
    int32_t total = local;
    uint64_t prov = coop_rd_ptr(ctl + 0x128), vt = prov ? coop_rd_ptr(prov) : 0;
    uint64_t f_count = vt ? coop_rd_ptr(vt + v_count) : 0, f_get = vt ? coop_rd_ptr(vt + v_get) : 0;
    if (f_count && f_get) total += (int32_t)coop_guest_call(c, f_count, prov, 0, 0, 0);
    if (total > AB_CAP) total = AB_CAP;
    uint64_t arr = coop_rd_ptr(ctl + arr_off);
    for (int32_t i = 0; i < total; i++) {
        uint64_t set;
        if (i < local) {
            uint64_t rec = arr ? coop_rd_ptr(arr + 8 * (uint64_t)i) : 0;
            set = rec ? coop_rd_ptr(rec + 8) : 0;
        } else
            set = coop_guest_call(c, f_get, prov, (uint32_t)(i - local), 0, 0);
        ab_set(set, out, n, max);
    }
}
// one of the extra holder's two lists: head link L+8, first link [L+0x10], link offset the int at L+0x1c, node = link -
// offset, body [node+8], next link [link+8]; the second list takes a node only when the u32 at its +0 is 2 or more
static void ab_list(uint64_t L, int need2, uint64_t *out, int *n, int max) {
    int32_t off = 0;
    if (!L) return;
    uint64_t head = L + 8, first = coop_rd_ptr(L + 0x10);
    if (!first || first == head || !coop_rd(L + 0x1c, &off, 4)) return;
    uint64_t node = first - (uint64_t)(int64_t)off, end = head - (uint64_t)(int64_t)off;
    for (int guard = 0; node != end && guard < AB_CAP; guard++) {
        uint64_t body = coop_rd_ptr(node + 8);
        uint32_t w = 0;
        if (body && (!need2 || (coop_rd(node, &w, 4) && w >= 2))) ab_add(out, n, max, body);
        uint64_t next = coop_rd_ptr(node + (uint64_t)(int64_t)off + 8);
        if (!next) return;
        node = next - (uint64_t)(int64_t)off;
    }
}
static int all_bodies(Cpu *c, uint64_t actor, uint64_t *out, int max) {
    int n = 0;
    uint64_t ctl = actor ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50) + 0x20) : 0;
    if (!ctl) return 0;
    ab_add(out, &n, max, coop_rd_ptr(ctl + 0x150));                       // the main body
    uint64_t H = coop_rd_ptr(ctl + 0x10), P = H ? coop_rd_ptr(H + 8) : 0;
    if (P) ab_add(out, &n, max, coop_rd_ptr(P + 0x18));                   // the character controller's
    int32_t hn = 0;
    if (H && coop_rd(H + 0x10, &hn, 4) && hn >= 1) {                      // the named list: count H+0x10, array H+0x18
        uint64_t arr = coop_rd_ptr(H + 0x18);
        for (int i = 0; arr && i < hn && i < AB_CAP; i++) ab_add(out, &n, max, coop_rd_ptr(arr + 8 * (uint64_t)i));
    }
    ab_sets(c, ctl, 0, out, &n, max);
    ab_sets(c, ctl, 1, out, &n, max);
    uint64_t X = coop_rd_ptr(ctl + 0x160);                                // the extra holder
    if (X) ab_list(coop_rd_ptr(X + 8), 0, out, &n, max), ab_list(coop_rd_ptr(X + 0x10), 1, out, &n, max);
    return n;
}
// the record holders of every body of an actor, of both physics worlds
static int actor_holders(Cpu *c, uint64_t a, uint64_t *out, int n, int max) {
    uint64_t b[96];
    int nb = all_bodies(c, a, b, 96);
    for (int i = 0; i < nb; i++)
        for (int w = 0; w < 2; w++) {
            uint64_t h = coop_rd_ptr(b[i] + (w ? 0x148 : 0x140));
            if (h && n < max && !veto_has(out, n, h)) out[n++] = h;
        }
    return n;
}
void coop_life_veto_rebuild(Cpu *c, const uint64_t *parts, int nparts) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_I16"); on = !(e && *e == '0'); }
    if (!on) return;
    static int64_t last;
    int64_t now = frame_step_world_us();
    if (now - last < 250000 && now >= last) return;
    last = now;
    int coop_players_all(uint64_t *out, int max);
    uint64_t coop_eq_row_ref(uint64_t actor, int row);
    uint64_t sa[8];
    int nsa = coop_players_all(sa, 8), w = atomic_load(&veto_cur) == 0 ? 1 : 0, gear = 0;
    int ns = 0, np = 0;
    for (int i = 0; i < nsa; i++) {
        ns = actor_holders(c, sa[i], veto_buf[w].s, ns, VETO_S);
        for (int row = 0; row < 24; row++) {   // the gear actors it holds (EquipmentUser rows, as coop_players.c reads them)
            uint64_t ref = coop_eq_row_ref(sa[i], row), item = ref ? coop_rd_ptr(ref + 0x40) : 0;
            if (item && item != sa[i]) ns = actor_holders(c, item, veto_buf[w].s, ns, VETO_S), gear++;
        }
    }
    for (int i = 0; i < nparts; i++) np = actor_holders(c, parts[i], veto_buf[w].p, np, VETO_P);
    memset(veto_buf[w].sbits, 0, sizeof veto_buf[w].sbits), memset(veto_buf[w].pbits, 0, sizeof veto_buf[w].pbits);
    for (int i = 0; i < ns; i++) { unsigned h = veto_hash(veto_buf[w].s[i]) & 255; veto_buf[w].sbits[h >> 6] |= 1ull << (h & 63); }
    for (int i = 0; i < np; i++) { unsigned h = veto_hash(veto_buf[w].p[i]) & 1023; veto_buf[w].pbits[h >> 6] |= 1ull << (h & 63); }
    veto_buf[w].ns = ns, veto_buf[w].np = np;
    atomic_store(&veto_cur, ns && np ? w : -1);
    static int64_t said;
    if (now - said > 10000000 && ns && np) {
        said = now;
        rt_log("coop: i16: %d bodies' records of %d stand-ins and %d gear actors kept off %d records of %d contraption parts; %u pairs refused so far, "
               "%u stand-in pairs looked at", ns, nsa, gear, np, nparts, atomic_load(&veto_count), atomic_load(&veto_seen));
    }
}
int coop_life_veto(Cpu *c) {   // main+0x40a240's entry: 1 when the pair is refused (x0 = 0, the caller reads no collision)
    int cur = atomic_load_explicit(&veto_cur, memory_order_acquire);
    if (cur < 0) return 0;
    const uint64_t ha = c->x[1] - 8, hb = c->x[2] - 8;
    const uint64_t *sb = veto_buf[cur].sbits;
    unsigned qa = veto_hash(ha) & 255, qb = veto_hash(hb) & 255;
    int ma = (sb[qa >> 6] >> (qa & 63)) & 1, mb = (sb[qb >> 6] >> (qb & 63)) & 1;
    if (!ma && !mb) return 0;   // (neither can be a stand-in's)
    const uint64_t *s = veto_buf[cur].s, *p = veto_buf[cur].p;
    int ns = veto_buf[cur].ns, np = veto_buf[cur].np;
    int as = ma && veto_has(s, ns, ha), bs = !as && mb && veto_has(s, ns, hb);
    if (!as && !bs) return 0;
    atomic_fetch_add_explicit(&veto_seen, 1, memory_order_relaxed);
    uint64_t other = as ? hb : ha;
    unsigned qo = veto_hash(other) & 1023;
    if (!((veto_buf[cur].pbits[qo >> 6] >> (qo & 63)) & 1) || !veto_has(p, np, other)) return 0;
    atomic_fetch_add_explicit(&veto_count, 1, memory_order_relaxed);
    c->x[0] = 0;
    return 1;
}
