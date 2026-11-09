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

int coop_life_hook(Cpu *c, uint64_t addr) {
    uint64_t off = addr - MOD_MAIN_BASE;
    if (off == ITEM_CHANGE) {
        if (!free_confirm) return 0;
        static int said;
        if (!said++) rt_log("coop: autobuild: a copy's confirm charged nothing (main+%#x skipped)", ITEM_CHANGE);
        c->x[0] = 0;
        return 1;   // (the confirm ignores the result)
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
