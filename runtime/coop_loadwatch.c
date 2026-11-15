// Co-op: a report when a load stays in the game's "delete all actors" step.
//
// A fast travel that changes the area runs the sequence node SeqExecuteDeleteAllActor (enter main+0x1fd9750, update
// main+0xea8aec). Its enter sets bit 1 of the actor manager's byte +0xe1 ([[main+0x395e008]]); only
// SeqOneShotAfterDeleteAllActor (main+0x1fe7824) clears it, and requestCreateActor (main+0x599670) refuses every create
// while it is set. The update finishes only when all of these hold (docs/coop/re-notes.md, "Game 2 stuck in a load
// after a fast travel"):
//   - the resource system H = [[main+0x3ad74b8]+8] has no unfinished job (main+0x67522c, H+0x678) and H+0x600 bit 5;
//   - the actor system S = [main+0x3ad6d80]: its delete lists are empty (+0x3d0, +0x3e8, +0x400), its going-away list
//     is empty (+0x398, drained each frame by main+0x6ecce4), and its creation queue counters [S+0x430]+0xe0 are 0;
//   - every actor still registered with state 2 or more is a persistent one (+0x440 bit 20) in state 2..6.
// Game 2 of the play6 pair (2026-11-10) sat there for minutes after a fast travel with every create refused. When bit 1
// stays set for STUCK_S seconds, this logs those values and every actor that keeps the step from finishing (state
// 7..11, or not persistent), with its delete flags (+0x28; bit 17: a delete was taken) and its holder lanes
// ([actor+0x1a8]+0x50: a delete for reasons 0, 3, 4, 6, 8 waits while its lane is nonzero; +0x4c the link lock).
// Read only. Once at STUCK_S, then every REPEAT_S while it lasts. TOTK_COOP_LOADWATCH=0 turns it off.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define ACTOR_SYSTEM_CELL 0x3ad6d80   // S
#define RESOURCE_CELL 0x3ad74b8       // [[+]+8] = H
#define PERSISTENT_BIT 0x100000u      // actor +0x440
#define STUCK_S 15
#define REPEAT_S 60
#define LIST_MAX 40

static uint32_t rd32(uint64_t a) {
    uint32_t v = 0;
    if (a) coop_rd(a, &v, 4);
    return v;
}
static uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    if (a) coop_rd(a, &v, 8);
    return v;
}

// Every actor in the reference pool (as coop_world.c coop_actors_all finds it, from the resident Player's reference),
// at any state: those that keep the delete-all step from finishing are logged.
static void list_blockers(void) {
    uint64_t ref = 0;
    if (!coop_resident_named("Player", &ref) || !ref) { rt_log("coop: loadwatch: no resident Player, actors not listed"); return; }
    uint32_t id = rd32(ref + 0x48);
    uint64_t base = ref - (uint64_t)(id & 0xffff) * 0x60;
    int listed = 0, total = 0, persistent = 0;
    for (int i = 0, gap = 0; i < 65536 && gap < 4096; i++) {
        uint64_t a = 0;
        if (!coop_rd(base + (uint64_t)i * 0x60 + 0x40, &a, 8)) break;
        int32_t st = 0;
        if (!a || (a & 7) || !coop_rd(a + COOP_ACTOR_STATE, &st, 4)) { gap++; continue; }
        gap = 0;
        if (st < 2) continue;
        total++;
        uint32_t flags440 = rd32(a + 0x440);
        int going = st >= 7 && st <= 11;
        if (!going && st <= 6 && (flags440 & PERSISTENT_BIT)) { persistent++; continue; }
        if (listed++ >= LIST_MAX) continue;
        char nm[48];
        uint64_t holder = rd64(a + 0x1a8);
        rt_log("coop: loadwatch:   %s %llx state %d, +0x440 %08x, +0x28 %08x, lanes %016llx, link lock %u", coop_actor_name(a, nm, sizeof nm),
               (unsigned long long)a, st, flags440, rd32(a + 0x28), (unsigned long long)rd64(holder ? holder + 0x50 : 0),
               rd32(holder ? holder + 0x4c : 0));
    }
    rt_log("coop: loadwatch: %d actors at state 2 or more, %d persistent and settled, %d keep the step%s", total, persistent, listed,
           listed > LIST_MAX ? " (the first ones listed)" : "");
}

void coop_loadwatch_tick(void) {
    static int on = -1;
    if (on < 0) { const char *v = cfg_get("TOTK_COOP_LOADWATCH"); on = !(v && !strcmp(v, "0")); }
    if (!on) return;
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_ACTOR_MGR_SLOT));
    uint8_t b = 0;
    int closed = mgr && coop_rd(mgr + 0xe1, &b, 1) && (b & 2);
    static uint64_t since_ns, next_ns;
    static int reported;
    uint64_t now = plat_now_ns();
    if (!closed) {
        if (reported) rt_log("coop: loadwatch: the delete-all step ended after %.1f s", (now - since_ns) / 1e9);
        since_ns = next_ns = 0, reported = 0;
        return;
    }
    if (!since_ns) since_ns = now, next_ns = now + (uint64_t)STUCK_S * 1000000000ull;
    if (now < next_ns) return;
    next_ns = now + (uint64_t)REPEAT_S * 1000000000ull, reported = 1;
    uint64_t S = coop_rd_ptr(MOD_MAIN_BASE + ACTOR_SYSTEM_CELL), H = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + RESOURCE_CELL) + 8);
    uint64_t q = S ? coop_rd_ptr(S + 0x430) : 0;
    rt_log("coop: loadwatch: the game has been in its delete-all step for %.0f s (creates refused): resources +0x600 %08x "
           "+0x678 %llx jobs %u; actor system going away %u, delete lists %u %u %u, creation queue %08x",
           (now - since_ns) / 1e9, rd32(H ? H + 0x600 : 0), (unsigned long long)rd64(H ? H + 0x678 : 0), rd32(H ? H + 0x568 : 0),
           rd32(S ? S + 0x398 : 0), rd32(S ? S + 0x3d0 : 0), rd32(S ? S + 0x3e8 : 0), rd32(S ? S + 0x400 : 0), rd32(q ? q + 0xe0 : 0));
    list_blockers();
}
