// Co-op Phase 1 spawn test: numpad 1 / 2 / 3 ask the game's actor manager for an apple, an NPC or a second Player
// 2 m in front of Link (docs/coop/re-notes.md, coop_ActorMgr_requestCreateActor). Runs on the game thread that
// polls the gamepad (ov_input.c npad_states), whose Cpu rt_call needs. Test code: Windows keys only.

#include <stdint.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_net.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "patches.h"
#include "platform/platform.h"

#define SCENE_SLOT 0x3add238      // [main+] -> +0x1e8 scene (runtime/frame_rate.c player_pos)
#define ACTOR_MGR_SLOT 0x395e008  // [[main+]] = the actor manager the AI node passes as x0
#define REQUEST_CREATE 0x599670   // coop_ActorMgr_requestCreateActor
#define CLAIM_ACTOR 0x1877160     // (handle, link out, 1, 0): what the AI node calls once the handle reads 3
#define SET_DEPENDENCY 0x5b811c   // (actor system, parent, child): queues a parent -> child update dependency
#define ACTOR_SYSTEM_SLOT 0x3ad6d80   // [main+] = the actor system SET_DEPENDENCY takes
#define INPUT_MGR_SLOT 0x3960150      // [[main+]] = the game's controller manager; getController main+0x37ccc0
#define EDGE_TABLE_SLOT 0x395da68     // [[main+]] +0x2f0 count, +0x2f8 edges {parent, child} (16 bytes each)
#define ACTOR_NAME 0x218
#define ACTOR_POS 0x2b4
#define ACTOR_ROT 0x2c0

static const char *names[3] = {"Item_Fruit_A", "Npc_Kokiri007", "Player"};   // [1]: TOTK_COOP_TEST_SPAWN can change it
// The handle the manager fills when the actor exists; it keeps the address, so it outlives the call.
static _Alignas(16) uint8_t handles[3][0x40];
static _Alignas(16) uint8_t links[3][0x40];   // the actor link the claim fills (the node keeps it at node+0x28)
static int last_status[3] = {-1, -1, -1};
int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
void coop_watch_disarm(void);
static uint64_t second_player;          // Player#2's actor once claimed
static uint64_t second_ref;             // Player#2's reference object (the claim's link [1]; +0x40 actor, +0x48 id)
static uint64_t player_actor_cached;    // Link#1, refreshed by coop_tick for the calc wrapper
static int dump_ticks;
static _Atomic uint64_t copy_dst[16];   // input objects the input copy (coop_hook) has written to

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t rd_ptr(uint64_t a) {
    uint64_t p = 0;
    return rd(a, &p, 8) ? p : 0;
}

// The resident actor named "Player", as runtime/frame_rate.c player_pos finds it.
static uint64_t player_actor(void) {
    uint64_t module = rd_ptr(MOD_MAIN_BASE + SCENE_SLOT), scene = module ? rd_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !rd(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t comps = rd_ptr(scene + 0x58), resident = comps ? rd_ptr(comps + 13 * 8) : 0;
    if (!resident || !rd(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = rd_ptr(resident + 0x28);
    for (int i = 0; links && i < count; i++) {
        uint64_t data = rd_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? rd_ptr(data + 0x40) : 0;
        uint64_t name = actor ? rd_ptr(actor + ACTOR_NAME) : 0;
        char text[7];
        if (name && rd(name, text, 7) && !memcmp(text, "Player", 7)) return actor;
    }
    return 0;
}

// Calls a guest function from inside a host call: the caller's x registers and sp come back unchanged.
static uint64_t guest_call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 4, a0, a1, a2, a3);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}

// Logs the resident actor list (scene component 13): per entry its name (+0x20), id (+0x10), the reference object (+8:
// +0x40 actor, +0x48 id) and that actor's state (+0x24: under 2 not ready, 7..11 going away) and name.
static void dump_resident(const char *tag) {
    uint64_t module = rd_ptr(MOD_MAIN_BASE + SCENE_SLOT), scene = module ? rd_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !rd(scene + 0x50, &count, 4) || count < 14) return;
    uint64_t comps = rd_ptr(scene + 0x58), resident = comps ? rd_ptr(comps + 13 * 8) : 0;
    if (!resident || !rd(resident + 0x20, &count, 4) || count < 0 || count > 64) return;
    uint64_t entries = rd_ptr(resident + 0x28);
    rt_log("coop: resident actors %s: %d", tag, count);
    for (int i = 0; entries && i < count; i++) {
        uint64_t e = entries + (uint64_t)i * 0x70, ref = rd_ptr(e + 8), nm = rd_ptr(e + 0x20), actor = 0, an = 0;
        int32_t id = 0, ref_id = 0, state = -1;
        char name[40] = "", aname[40] = "";
        if (nm) rd(nm, name, sizeof name - 1);
        rd(e + 0x10, &id, 4);
        if (ref) { actor = rd_ptr(ref + 0x40); rd(ref + 0x48, &ref_id, 4); }
        if (actor) { rd(actor + 0x24, &state, 4); if ((an = rd_ptr(actor + ACTOR_NAME))) rd(an, aname, sizeof aname - 1); }
        rt_log("coop:   [%d] %s id %d ref %llx (id %d) actor %llx state %d '%s'", i, name, id, (unsigned long long)ref,
               ref_id, (unsigned long long)actor, state, aname);
    }
}

static const char *actor_name(uint64_t actor, char *buf, size_t n) {
    uint64_t nm = actor ? rd_ptr(actor + ACTOR_NAME) : 0;
    buf[0] = 0;
    if (nm) rd(nm, buf, n - 1), buf[n - 1] = 0;
    return buf;
}

// Gives `copy` the update dependencies `model` has: every edge of model's list (actor +0x1c0 u16 ring, +0x1c8
// capacity, +0x1cc start, +0x1d0 count, indices into the edge table) is queued again with copy in model's place.
// ActorLink's getActor (main+0x2804d0) hands an actor out on a worker thread only to an actor connected to it by
// such edges; a second Player without them gets null for ThrowBox and crashes (main+0x4b46a4).
static int heapish(uint64_t v) { return v >= 0x100000000ull && v <= 0x7fffffffffull && !(v & 7); }

// The resident actor named `want` and its reference object (entry +8), or 0.
static uint64_t resident_named(const char *want, uint64_t *ref_out) {
    uint64_t module = rd_ptr(MOD_MAIN_BASE + SCENE_SLOT), scene = module ? rd_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !rd(scene + 0x50, &count, 4) || count < 14) return 0;
    uint64_t comps = rd_ptr(scene + 0x58), resident = comps ? rd_ptr(comps + 13 * 8) : 0;
    if (!resident || !rd(resident + 0x20, &count, 4) || count < 0 || count > 64) return 0;
    uint64_t entries = rd_ptr(resident + 0x28);
    for (int i = 0; entries && i < count; i++) {
        uint64_t ref = rd_ptr(entries + (uint64_t)i * 0x70 + 8), actor = ref ? rd_ptr(ref + 0x40) : 0;
        char nm[40];
        if (actor && !strcmp(actor_name(actor, nm, sizeof nm), want)) {
            if (ref_out) *ref_out = ref;
            return actor;
        }
    }
    return 0;
}

// Where PlayerCamera keeps the Player it follows: words in its first 0x4000 bytes, and one pointer level below
// (0x800 bytes each), equal to Link#1's actor or its resident reference object (logged for the follow switch).
static void camera_scan(void) {
    uint64_t ref1 = 0, cam = resident_named("PlayerCamera", NULL), one = resident_named("Player", &ref1);
    if (!cam || !one) { rt_log("coop: camera scan: no PlayerCamera or Player"); return; }
    static uint64_t a0[0x4000 / 8], a1[0x800 / 8];
    if (!rd(cam, a0, sizeof a0)) return;
    int hits = 0;
    for (int i = 0; i < 0x4000 / 8 && hits < 32; i++) {
        if (a0[i] == one || a0[i] == ref1) {
            rt_log("coop: PlayerCamera +%#x = Link#1 %s", i * 8, a0[i] == one ? "actor" : "reference");
            hits++;
            continue;
        }
        if (!heapish(a0[i]) || !rd(a0[i], a1, sizeof a1)) continue;
        for (int j = 0; j < 0x800 / 8 && hits < 32; j++)
            if (a1[j] == one || a1[j] == ref1) {
                rt_log("coop: PlayerCamera [+%#x] +%#x = Link#1 %s", i * 8, j * 8, a1[j] == one ? "actor" : "reference");
                hits++;
            }
    }
    rt_log("coop: camera scan: %d places (camera %llx, Link#1 %llx, reference %llx)", hits, (unsigned long long)cam,
           (unsigned long long)one, (unsigned long long)ref1);
}

// The camera follow switch: the four places in PlayerCamera that hold the followed Player (camera_scan, 2026-11-04):
// [cam+0x1a8]+0x160, [cam+0x858]+0x100, [cam+0xa20]+0x790, [cam+0x1fc0]+0x1c0. TOTK_COOP_CAMMASK picks which are
// written (bits 0-3, default all); written every poll while the camera follows Link#2.
static const uint16_t cam_slot[4][2] = {{0x1a8, 0x160}, {0x858, 0x100}, {0xa20, 0x790}, {0x1fc0, 0x1c0}};
static void camera_follow(uint64_t target, uint64_t from) {
    static int mask = -1;
    // Off unless TOTK_COOP_CAMMASK is set: the places move between runs (another scan found one reference elsewhere)
    // and writing them stripped Link#1's hair, Zonai arm and armor (the tester, 2026-11-04).
    if (mask < 0) { const char *e = cfg_get("TOTK_COOP_CAMMASK"); mask = e ? (int)strtol(e, NULL, 0) : 0; }
    uint64_t cam = resident_named("PlayerCamera", NULL);
    for (int i = 0; cam && i < 4; i++) {
        uint64_t obj = rd_ptr(cam + cam_slot[i][0]);
        if (!(mask >> i & 1) || !heapish(obj)) continue;
        uint64_t at = obj + cam_slot[i][1];
        if (rd_ptr(at) == from) ST64(at, target);
    }
}

// Whether an actor is in the resident actor list (scene component 13).
static int is_resident(uint64_t actor) {
    uint64_t module = rd_ptr(MOD_MAIN_BASE + SCENE_SLOT), scene = module ? rd_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !rd(scene + 0x50, &count, 4) || count < 14) return 0;
    uint64_t comps = rd_ptr(scene + 0x58), resident = comps ? rd_ptr(comps + 13 * 8) : 0;
    if (!resident || !rd(resident + 0x20, &count, 4) || count < 0 || count > 64) return 0;
    uint64_t entries = rd_ptr(resident + 0x28);
    for (int i = 0; entries && i < count; i++) {
        uint64_t ref = rd_ptr(entries + (uint64_t)i * 0x70 + 8);
        if (ref && rd_ptr(ref + 0x40) == actor) return 1;
    }
    return 0;
}

// Logs an actor's update-dependency edges (the ring of copy_dependencies) by the other actor's name.
static void list_dependencies(const char *who, uint64_t actor) {
    uint64_t tbl = rd_ptr(rd_ptr(MOD_MAIN_BASE + EDGE_TABLE_SLOT)), ring = rd_ptr(actor + 0x1c0);
    uint64_t edges = tbl ? rd_ptr(tbl + 0x2f8) : 0;
    int32_t cap = 0, start = 0, count = 0;
    uint32_t n_edges = 0;
    rd(actor + 0x1c8, &cap, 4), rd(actor + 0x1cc, &start, 4), rd(actor + 0x1d0, &count, 4);
    if (tbl) rd(tbl + 0x2f0, &n_edges, 4);
    if (!ring || !edges || cap <= 0 || count < 0 || count > cap) return;
    char line[2048];
    int n = snprintf(line, sizeof line, "coop: %s edges (%d):", who, count);
    for (int i = 0; i < count && n < (int)sizeof line - 60; i++) {
        int slot = start + i >= cap ? start + i - cap : start + i;
        uint16_t idx = 0;
        rd(ring + (uint64_t)slot * 2, &idx, 2);
        uint64_t e[2] = {0, 0};
        rd(edges + (uint64_t)(idx < n_edges ? idx : 0) * 16, e, 16);
        char a[40];
        uint64_t o = e[0] == actor ? e[1] : e[0];
        n += snprintf(line + n, sizeof line - n, " %s%s@%llx", e[0] == actor ? "->" : "<-", actor_name(o, a, 40),
                      (unsigned long long)(o & 0xffffff));
    }
    rt_log("%s", line);
}

static void copy_dependencies(Cpu *c, uint64_t model, uint64_t copy) {
    uint64_t sys = rd_ptr(MOD_MAIN_BASE + ACTOR_SYSTEM_SLOT), tbl = rd_ptr(rd_ptr(MOD_MAIN_BASE + EDGE_TABLE_SLOT));
    uint64_t ring = rd_ptr(model + 0x1c0), edges = tbl ? rd_ptr(tbl + 0x2f8) : 0;
    int32_t cap = 0, start = 0, count = 0;
    uint32_t n_edges = 0;
    rd(model + 0x1c8, &cap, 4), rd(model + 0x1cc, &start, 4), rd(model + 0x1d0, &count, 4);
    if (tbl) rd(tbl + 0x2f0, &n_edges, 4);
    rt_log("coop: Player dependencies: %d edges (ring cap %d), table %u, system %llx", count, cap, n_edges,
           (unsigned long long)sys);
    if (!sys || !ring || !edges || cap <= 0 || count < 0 || count > cap) return;
    for (int i = 0; i < count; i++) {
        int slot = start + i >= cap ? start + i - cap : start + i;
        uint16_t idx = 0;
        rd(ring + (uint64_t)slot * 2, &idx, 2);
        uint64_t e[2] = {0, 0};
        rd(edges + (uint64_t)(idx < n_edges ? idx : 0) * 16, e, 16);
        uint64_t parent = e[0] == model ? copy : e[0], child = e[1] == model ? copy : e[1];
        char a[40], b[40];
        if (e[0] != model && e[1] != model) {
            rt_log("coop:   edge %u: %s -> %s does not involve the Player, skipped", idx, actor_name(e[0], a, 40),
                   actor_name(e[1], b, 40));
            continue;
        }
        // Only edges to resident actors are copied (TOTK_COOP_EDGES=all: every edge): Player#2 makes its own weapons,
        // armor, devices, Ultrahand and rope helpers and links them itself (edge lists, 2026-11-03); the resident
        // helpers (ThrowBox, CarryBox, Parasail, the revival fairy) are one per game and need the link copied.
        static int all = -1;
        if (all < 0) { const char *v = cfg_get("TOTK_COOP_EDGES"); all = v && !strcmp(v, "all"); }
        uint64_t other = e[0] == model ? e[1] : e[0];
        if (!all && !is_resident(other)) {
            rt_log("coop:   edge %u: %s is Player#1's own, not copied", idx, actor_name(other, b, 40));
            continue;
        }
        uint64_t ok = guest_call(c, MOD_MAIN_BASE + SET_DEPENDENCY, sys, parent, child, 0);
        rt_log("coop:   edge %u: %s -> %s copied as %s -> %s: %s", idx, actor_name(e[0], a, 40),
               actor_name(e[1], b, 40), parent == copy ? "Player#2" : a, child == copy ? "Player#2" : b,
               ok & 1 ? "queued" : "refused");
    }
}

// Where an actor keeps the main controller: getController (main+0x37ccc0) returns [mgr+0xa0] for index 0,
// [mgr+0xf0 + 8*(i-1)] for 1..3, [mgr+0xa8] for 4. Logs every qword equal to the index-0 controller in the actor's
// first 0x3000 bytes and one pointer level below it (0x800 bytes of each object it points to).
// Looks for pointers to the input objects the copy fills (copy_dst) in the actor: its first 0x3000 bytes, the 0x800 bytes
// of each object those point to, and one more level (0x400 bytes).
static int is_input_obj(uint64_t v) {
    for (int i = 0; i < 16; i++) { uint64_t d = atomic_load(&copy_dst[i]); if (!d) break; if (v == d) return i + 1; }
    return 0;
}
static void find_input(const char *who, uint64_t actor) {
    static uint64_t a0[0x3000 / 8], a1[0x800 / 8], a2[0x400 / 8];
    if (!actor || !rd(actor, a0, sizeof a0)) return;
    int hits = 0;
    for (int i = 0; i < 0x3000 / 8 && hits < 32; i++) {
        int k;
        if ((k = is_input_obj(a0[i]))) { rt_log("coop: %s +%#x = input object %d", who, i * 8, k - 1); hits++; continue; }
        if (!heapish(a0[i]) || !rd(a0[i], a1, sizeof a1)) continue;
        for (int j = 0; j < 0x800 / 8 && hits < 32; j++) {
            if ((k = is_input_obj(a1[j]))) { rt_log("coop: %s [+%#x] +%#x = input object %d", who, i * 8, j * 8, k - 1); hits++; continue; }
            if (!heapish(a1[j]) || !rd(a1[j], a2, sizeof a2)) continue;
            for (int m = 0; m < 0x400 / 8 && hits < 32; m++)
                if ((k = is_input_obj(a2[m]))) {
                    rt_log("coop: %s [[+%#x] +%#x] +%#x = input object %d", who, i * 8, j * 8, m * 8, k - 1);
                    hits++;
                }
        }
    }
    rt_log("coop: %s: %d places point at an input object", who, hits);
}

static void find_controller(const char *who, uint64_t actor) {
    uint64_t mgr = rd_ptr(rd_ptr(MOD_MAIN_BASE + INPUT_MGR_SLOT)), want = mgr ? rd_ptr(mgr + 0xa0) : 0;
    if (!want || !actor) return;
    static uint64_t top[0x3000 / 8], sub[0x800 / 8];
    if (!rd(actor, top, sizeof top)) return;
    int hits = 0;
    for (int i = 0; i < 0x3000 / 8 && hits < 24; i++) {
        if (top[i] == want) { rt_log("coop: %s +%#x = controller 0", who, i * 8); hits++; continue; }
        if (top[i] < 0x100000000ull || top[i] > 0x7fffffffffull || (top[i] & 7)) continue;
        if (!rd(top[i], sub, sizeof sub)) continue;
        for (int j = 0; j < 0x800 / 8 && hits < 24; j++)
            if (sub[j] == want) { rt_log("coop: %s [+%#x] +%#x = controller 0", who, i * 8, j * 8); hits++; }
    }
    rt_log("coop: %s: %d places hold controller 0 (%llx); mgr %llx, controllers 1..3 %llx %llx %llx, 4 %llx", who, hits,
           (unsigned long long)want, (unsigned long long)mgr, (unsigned long long)rd_ptr(mgr + 0xf0),
           (unsigned long long)rd_ptr(mgr + 0xf8), (unsigned long long)rd_ptr(mgr + 0x100),
           (unsigned long long)rd_ptr(mgr + 0xa8));
}

static void spawn(Cpu *c, int k) {
    if (k == 2) dump_resident("before the Player request");
    uint64_t actor = player_actor();
    float pos[3], rot[9];
    if (!actor || !rd(actor + ACTOR_POS, pos, sizeof pos) || !rd(actor + ACTOR_ROT, rot, sizeof rot) ||
        (pos[0] == 0 && pos[1] == 0 && pos[2] == 0)) {
        rt_log("coop: spawn %s: no player in the world yet", names[k]);
        return;
    }
    uint64_t mgr = rd_ptr(rd_ptr(MOD_MAIN_BASE + ACTOR_MGR_SLOT));
    if (!mgr) {
        rt_log("coop: spawn %s: no actor manager", names[k]);
        return;
    }
    // Guess: the rotation is row-major (MacNativeGames force_at) and Link faces his local +Z, column 2.
    float at[3] = {pos[0] + 2 * rot[2], pos[1] + 0.5f, pos[2] + 2 * rot[8]};

    // The frame the AI node builds (main+0x231fa58): stack args at sp, the name pointer at sp+0x20, the create
    // argument at sp+0x28 (position, rotation, scale 1, a string at +0x40, flags at +0x90).
    uint64_t saved_x[31], sp0 = c->sp, sp = (sp0 - 0x200) & ~0xfull;
    memcpy(saved_x, c->x, sizeof saved_x);
    memset((void *)(uintptr_t)sp, 0, 0x100);
    ST64(sp + 0x20, (uint64_t)(uintptr_t)names[k]);
    uint8_t *arg = (uint8_t *)(uintptr_t)(sp + 0x28);
    const float one[3] = {1, 1, 1};
    memcpy(arg + 0x00, at, 12);
    memcpy(arg + 0x0c, rot, 36);
    memcpy(arg + 0x30, one, 12);
    static const char empty[1] = "";
    ST64(sp + 0x28 + 0x40, (uint64_t)(uintptr_t)empty);   // a string the manager reads without a null check (main+0x59a9f0)
    arg[0x90] = 3;   // flags: bit 0 position, bit 1 rotation given (bit 2 would be scale)
    memset(handles[k], 0, sizeof handles[k]);
    c->sp = sp;
    uint64_t ok = rt_call(c, MOD_MAIN_BASE + REQUEST_CREATE, 8, mgr, sp + 0x20, sp + 0x28,
                          (uint64_t)(uintptr_t)handles[k], (uint64_t)1, (uint64_t)0, (uint64_t)0, (uint64_t)0);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    rt_log("coop: spawn %s at %.1f %.1f %.1f (Link %.1f %.1f %.1f, facing %.2f %.2f): request %s", names[k], at[0],
           at[1], at[2], pos[0], pos[1], pos[2], rot[2], rot[8], ok & 1 ? "queued" : "refused");
    last_status[k] = -1;
}

// The handle's status (low 16 bits of the word it points to): 0 none, 3 created and waiting to be claimed, 4 failed.
static int handle_status(int k) {
    uint64_t w = rd_ptr((uint64_t)(uintptr_t)handles[k]);
    uint32_t st = 0;
    return w && rd(w, &st, 4) ? (int)(st & 0xffff) : 0;
}

// Once per call site of the gamepad read: the guest call chain (x30, then the frame-pointer chain from x29), as main+
// offsets, to find the code that hands controller input to the Player.
static void log_callers(Cpu *c) {
    static uint64_t seen[8];
    uint64_t ra = c->x[30];
    for (int i = 0; i < 8; i++) {
        if (seen[i] == ra) return;
        if (!seen[i]) { seen[i] = ra; break; }
        if (i == 7) return;
    }
    char line[512];
    int n = snprintf(line, sizeof line, "coop: gamepad read from main+%llx", (unsigned long long)(ra - MOD_MAIN_BASE));
    uint64_t fp = c->x[29];
    for (int d = 0; d < 10 && fp && n < (int)sizeof line - 32; d++) {
        uint64_t next = rd_ptr(fp), ret = rd_ptr(fp + 8);
        if (!ret) break;
        n += snprintf(line + n, sizeof line - n, " <- main+%llx", (unsigned long long)(ret - MOD_MAIN_BASE));
        if (next <= fp) break;
        fp = next;
    }
    rt_log("%s", line);
}

// ---- functions called with Player#2 (TOTK_COOP_PROF=1): the function profiler (cpu.h FN_ENTER, g_fnprof) runs for
// about a second after the spawn and records every guest function entered with x0 = Player#2's actor, or a pointer
// into its first 0x3000 bytes (an embedded part), with a caller.
static _Atomic uint64_t prof_actor;
static struct { _Atomic uint64_t fn; _Atomic uint32_t hits; uint32_t off; uint64_t lr; } prof_seen[1024];

// TOTK_COOP_PROF=3: every function entered on a thread inside a co-op Link's calc, with a sample x0, for finding
// what such a calc changes outside its own actor (HUD prompts). Reported every 10 s with the x0s that a global of
// main holds (a singleton), and cleared.
int coop_in_remote_calc(void);   // coop_players.c
int coop_players_phase(Cpu *c, uint64_t addr);   // coop_players.c: another update phase of a co-op Link
int coop_is_coop_life(uint64_t comp);   // coop_world.c: a co-op Link's life component
int coop_pose_is_target(uint64_t binding);   // coop_world.c: main+0x2e9c80 poses the co-op Link's model
void coop_pose_copy_now(void);   // coop_world.c: the copied pose onto it
int coop_pose_wanted(void);   // coop_world.c: a co-op Link to copy a pose onto
void coop_pose_saw(uint64_t binding);   // coop_world.c: an animation binding main+0x2e9c80 posed
void coop_pose_before_anim(uint64_t binding);
int coop_pose_arg_match(const uint64_t *args, int n);
int coop_pose_after_writer(const uint64_t *args, int n);
int coop_puppet_before_writer(uint64_t pose, uint64_t skel);   // coop_world.c: pose out / a player's pose in
int coop_puppet_mode(void);   // coop_world.c: TOTK_COOP_PUPPET   // coop_world.c: copies onto the co-op Link's part skeleton named   // coop_world.c: x0..x3 name the co-op Link's skeleton or model   // coop_world.c: this Link's animation pose onto the co-op Link's
void coop_trace_tick(uint64_t me);   // coop_trace.c
void coop_actor_seen(uint64_t actor);   // coop_world.c
uint64_t coop_nearest_link_data(uint64_t enemy);   // coop_world.c: a co-op Link's perception data when nearer, else 0
static int prof3;
// TOTK_COOP_PROF=4: inside an enemy's calc (enemy_calc), the functions entered with x0 = this PC's Link or its reference
// object, with their call site: where enemy AI uses the player it fetched.
static int prof4;
static int prof7;   // TOTK_COOP_PROF=7, below coop_fn_hit
static __thread uint64_t enemy_calc;
static uint64_t prof4_ref;
static struct { _Atomic uint64_t fn; _Atomic uint32_t hits; uint64_t x0, lr; } p3[8192];
static void p3_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    uint32_t h = (uint32_t)((addr >> 2) * 2654435761u) & 8191;
    for (int k = 0; k < 64; k++, h = (h + 1) & 8191) {
        uint64_t have = atomic_load_explicit(&p3[h].fn, memory_order_relaxed);
        if (!have && atomic_compare_exchange_strong(&p3[h].fn, &have, addr)) { p3[h].x0 = x0, p3[h].lr = lr; have = addr; }
        if (have == addr) { atomic_fetch_add_explicit(&p3[h].hits, 1, memory_order_relaxed); return; }
    }
}
static int u64cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static void p3_report(void) {
    // Pointers main's writable data holds, and what they point at (one more level: [[main+x]] singletons).
    // Pairs {value, where}: where is the main offset of the global, with bit 63 set for one level down ([[main+x]]).
    static uint64_t *held;
    static size_t nheld;
    uint64_t lo = MOD_MAIN_BASE + 0x357dee8, hi = MOD_MAIN_BASE + 0x3adefd0;
    if (!held) held = malloc(((hi - lo) / 8) * 4 * sizeof *held);
    nheld = 0;
    for (uint64_t a = lo; held && a < hi; a += 8) {
        uint64_t q = 0;
        if (!rd(a, &q, 8) || q < 0x10000 || (q >> 40) > 0x80) continue;
        held[2 * nheld] = q, held[2 * nheld + 1] = a - MOD_MAIN_BASE, nheld++;
        uint64_t q2 = 0;
        if (rd(q, &q2, 8) && q2 > 0x10000 && (q2 >> 40) <= 0x80)
            held[2 * nheld] = q2, held[2 * nheld + 1] = (a - MOD_MAIN_BASE) | (1ull << 63), nheld++;
    }
    if (held) qsort(held, nheld, 2 * sizeof *held, u64cmp);
    char d[160], e[160];
    int n = 0, g = 0;
    for (int i = 0; i < 8192; i++) {
        uint64_t fn = atomic_load(&p3[i].fn);
        if (!fn) continue;
        n++;
        const uint64_t *hit = held ? bsearch(&p3[i].x0, held, nheld, 2 * sizeof *held, u64cmp) : NULL;
        int global = hit != NULL;
        char where[48] = "";
        if (hit) snprintf(where, sizeof where, (hit[1] >> 63) ? "[[main+%#llx]]" : "[main+%#llx]", (unsigned long long)(hit[1] & ~(1ull << 63)));
        if (prof7) {
            uint32_t slot = (uint32_t)(p3[i].x0 >> 32), off = (uint32_t)p3[i].x0;
            char what[48];
            if (slot == 0xffff) snprintf(what, sizeof what, "co-op Link+%#x", off);
            else snprintf(what, sizeof what, "component +%#x, +%#x", slot, off);
            rt_log("coop: prof7 %6u x %s, x0 %s, from %s", atomic_load(&p3[i].hits), ldr_describe(fn, d, sizeof d), what,
                   ldr_describe(p3[i].lr, e, sizeof e));
            atomic_store(&p3[i].hits, 0), atomic_store(&p3[i].fn, 0);
            continue;
        }
        if (prof4) {
            rt_log("coop: prof4 %6u x %s, x0 %s, from %s", atomic_load(&p3[i].hits), ldr_describe(fn, d, sizeof d),
                   p3[i].x0 == prof4_ref ? "Link's reference" : "Link", ldr_describe(p3[i].lr, e, sizeof e));
            if (p3[i].x0 != prof4_ref) rt_log("coop: prof4   (x0 = Link+%#llx)", (unsigned long long)(p3[i].x0 - player_actor_cached));
            atomic_store(&p3[i].hits, 0), atomic_store(&p3[i].fn, 0);
            continue;
        }
        // The control-guide (HUD prompt) code around its table loader main+0xd81910 and stack main+0xd82334.
        int guide = fn >= MOD_MAIN_BASE + 0xd78000 && fn < MOD_MAIN_BASE + 0xd8a000;
        if (guide)
            rt_log("coop: prof3 guide %6u x %s, x0 %llx, from %s", atomic_load(&p3[i].hits), ldr_describe(fn, d, sizeof d),
                   (unsigned long long)p3[i].x0, ldr_describe(p3[i].lr, e, sizeof e));
        if (global) {
            g++;
            rt_log("coop: prof3 %6u x %s, x0 %llx (held by a global %s), from %s", atomic_load(&p3[i].hits),
                   ldr_describe(fn, d, sizeof d), (unsigned long long)p3[i].x0, where, ldr_describe(p3[i].lr, e, sizeof e));
        }
        atomic_store(&p3[i].hits, 0), atomic_store(&p3[i].fn, 0);
    }
    rt_log("coop: prof3: %d functions in co-op Link calcs, %d with a global's object", n, g);
}

// TOTK_COOP_PROF=6: every function entered for the first time once the world has run TOTK_COOP_PROF_AFTER seconds
// (default 20), logged with its world time, caller and whether a co-op Link's calc is running on that thread: what
// a moment (a tip opening, a Game Over) runs that nothing ran before.
static int prof6;
static double prof6_after = 20;
static _Atomic uint64_t p6[1 << 18];
static void p6_hit(uint64_t addr, uint64_t lr) {
    uint32_t h = (uint32_t)((addr >> 2) * 2654435761u) & ((1 << 18) - 1);
    for (int k = 0; k < 256; k++, h = (h + 1) & ((1 << 18) - 1)) {
        uint64_t have = atomic_load_explicit(&p6[h], memory_order_relaxed);
        if (have == addr) return;
        if (!have && atomic_compare_exchange_strong(&p6[h], &have, addr)) {
            double s = frame_step_world_us() / 1e6;
            if (s >= prof6_after) {
                char d[160], e[160];
                rt_log("coop: prof6 new %.2f s: %s from %s%s", s, ldr_describe(addr, d, sizeof d), ldr_describe(lr, e, sizeof e),
                       coop_in_remote_calc() ? " (co-op Link calc)" : "");
            }
            return;
        }
        if (have == addr) return;
    }
}

// TOTK_COOP_PROF=7: functions entered OUTSIDE a co-op Link's calc with x0 = that Link (its first 0x4000 bytes) or
// one of its components (table [actor+0x228], their first 0x400 bytes): the other phases of its update (AI, AS,
// physics, ragdoll) that the calc flag does not cover. Reported every 10 s with p3_report's table.
static uint64_t p7_lo[COOP_MAX_PLAYERS * 160], p7_hi[COOP_MAX_PLAYERS * 160];
static uint32_t p7_slot[COOP_MAX_PLAYERS * 160];   // 0xffff the actor itself, else its component table offset
static _Atomic int p7_n;
int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c
static void p7_refresh(void) {
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(acts, ids, COOP_MAX_PLAYERS), k = 0;
    atomic_store(&p7_n, 0);
    for (int i = 0; i < n; i++) {
        p7_lo[k] = acts[i], p7_hi[k] = acts[i] + 0x4000, p7_slot[k] = 0xffff, k++;
        uint64_t t = rd_ptr(acts[i] + 0x228);
        for (int j = 0; t && j < 150; j++) {
            uint64_t comp = rd_ptr(t + j * 8);
            if (comp > 0x100000000ull && comp < 0x8000000000ull) p7_lo[k] = comp, p7_hi[k] = comp + 0x400, p7_slot[k] = j * 8, k++;
        }
    }
    atomic_store(&p7_n, k);
}

// TOTK_COOP_PROF=8 (with TOTK_COOP_TRACE=calc, which tracks whose calc a thread runs): every function entered inside
// this PC's Link's calc with a name-like string in x1..x3 (a char pointer, or a string object's +0 / +8 pointer to
// one), kept as distinct (function, string) pairs and reported every 10 s by function, most strings first: where
// the Player starts animations (AS) by name, for a proxy that replays them (docs/coop/proxy-plan.md stage 4).
static int prof8;
static struct { _Atomic uint64_t fn; uint32_t hash, hits; char s[40]; } p8[8192];
static __thread uint64_t calc_actor;   // the Player whose ACTOR_CALC this thread runs (TOTK_COOP_TRACE set)
static int name_like(uint64_t p, char out[40]) {
    if (p < 0x100000000ull || p >= 0x8000000000ull) return 0;
    char b[40];
    if (!rd(p, b, sizeof b)) return 0;
    int n = 0;
    while (n < 39 && ((b[n] >= 'A' && b[n] <= 'Z') || (b[n] >= 'a' && b[n] <= 'z') || (b[n] >= '0' && b[n] <= '9') || b[n] == '_')) n++;
    if (n < 3 || n >= 39 || b[n] != 0) return 0;
    memcpy(out, b, n + 1);
    return 1;
}
// TOTK_COOP_PROF=9: every call of main+0x4dcc1c (x0 an AS manager, x1 the AS name as a string object, x2 the
// request: +0x28 slot, +0x34 blend time) inside this PC's Link's calc, logged with the world time.
static int prof9;
// TOTK_COOP_TEST_ASPLAY=<name>,<slot>@<s> (with PROF=9): this Link's AS manager, name string object and request are
// copied from one of its own calls; at that world time main+0x4dcc1c is called for the first co-op Link's manager
// (found at the same component path as this Link's) with that name in that slot: can a Link be made to play an AS.
static _Alignas(16) uint8_t as_name_obj[0x40], as_req[0x80];
static uint64_t as_my_mgr;
static char as_text[48];
uint64_t coop_as_template(uint64_t *mgr, uint8_t **name_obj, uint8_t **req, char **text) {
    *mgr = as_my_mgr, *name_obj = as_name_obj, *req = as_req, *text = as_text;
    return as_my_mgr;
}
// TOTK_COOP_TEST_FLAGCALLER=1 (played; the profiler runs from the world on, slow): each call of the named-bool setter
// main+0x9fb82c (handle x0: [[[x0+8]+8]+0x200] the bool table, [x0+0xc4] the index, w1 the value) that writes the
// on bool of the fan coop_players.c watches (coop_fan_flag) is logged with the guest frame chain, up to 12 times: the
// code a hit runs to turn a Zonai device on.
uint64_t coop_fan_flag;   // coop_players.c fantoggle / fanwatch: the on bool of the nearest fan
static int flagcaller = -1;
// TOTK_COOP_TEST_FANHIT=<hit s> (with TOTK_COOP_TEST_FANSET=9 and fan_hits_3rd.txt): the functions entered with x0 or
// x1 = the test's fan (coop_fan_actor) in the 0.6 s from the hit, that were not entered so in the 3 s before it, with a
// frame chain each: where a sword hit reaches a Zonai device.
uint64_t coop_fan_actor;
static double fanhit_at = -2;
static struct { _Atomic uint64_t fn; _Atomic int base, win; char chain[200]; } fh[1024];
static void fanhit_note(uint64_t addr, Cpu *c) {
    double t = frame_step_world_us() / 1e6;
    int base = t > fanhit_at - 3 && t < fanhit_at, win = t >= fanhit_at && t < fanhit_at + 0.6;
    if (!base && !win) return;
    uint32_t i = (uint32_t)((addr * 0x9e3779b97f4a7c15ull) >> 54);
    for (int k = 0; k < 1024; k++, i = (i + 1) & 1023) {
        uint64_t have = atomic_load(&fh[i].fn);
        if (!have && atomic_compare_exchange_strong(&fh[i].fn, &have, addr)) have = addr;
        if (have != addr) continue;
        if (base) atomic_fetch_add(&fh[i].base, 1);
        if (win && atomic_fetch_add(&fh[i].win, 1) == 0) {
            int n = snprintf(fh[i].chain, sizeof fh[i].chain, "lr main+%llx", (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
            uint64_t fp = c->x[29];
            for (int d = 0; d < 6 && fp && n < (int)sizeof fh[i].chain - 24; d++) {
                uint64_t next = rd_ptr(fp), ret = rd_ptr(fp + 8);
                if (!ret) break;
                n += snprintf(fh[i].chain + n, sizeof fh[i].chain - n, " <- %llx", (unsigned long long)(ret - MOD_MAIN_BASE));
                if (next <= fp) break;
                fp = next;
            }
        }
        return;
    }
}
static void fanhit_report(void) {
    static int done;
    if (done || fanhit_at < 0 || frame_step_world_us() / 1e6 < fanhit_at + 1) return;
    done = 1;
    int n = 0;
    for (int i = 0; i < 1024; i++)
        if (atomic_load(&fh[i].fn) && atomic_load(&fh[i].win) && !atomic_load(&fh[i].base) && n++ < 80)
            rt_log("coop: test: fanhit: main+%llx x%d (%s)", (unsigned long long)(atomic_load(&fh[i].fn) - MOD_MAIN_BASE),
                   atomic_load(&fh[i].win), fh[i].chain);
    rt_log("coop: test: fanhit: %d functions new at the hit", n);
}
void coop_fn_args(uint64_t addr, Cpu *c) {
    if (fanhit_at > 0 && coop_fan_actor && (c->x[0] == coop_fan_actor || c->x[1] == coop_fan_actor)) fanhit_note(addr, c);
    if (flagcaller > 0 && addr == MOD_MAIN_BASE + 0x9fb82c && coop_fan_flag) {
        uint64_t h = c->x[0], bb = rd_ptr(rd_ptr(rd_ptr(h + 8) + 8) + 0x200);
        uint32_t idx = 0;
        rd(h + 0xc4, &idx, 4);
        static int said;
        if (bb && rd_ptr(bb + 0x1a0) + idx == coop_fan_flag && said < 12) {
            said++;
            char line[512];
            int n = snprintf(line, sizeof line, "coop: test: flagcaller %.2f s value %llu lr main+%llx chain", frame_step_world_us() / 1e6,
                             (unsigned long long)(c->x[1] & 0xff), (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
            uint64_t fp = c->x[29];
            for (int d = 0; d < 12 && fp && n < (int)sizeof line - 40; d++) {
                uint64_t next = rd_ptr(fp), ret = rd_ptr(fp + 8);
                if (!ret) break;
                n += snprintf(line + n, sizeof line - n, " <- main+%llx", (unsigned long long)(ret - MOD_MAIN_BASE));
                if (next <= fp) break;
                fp = next;
            }
            rt_log("%s", line);
        }
    }
    if (prof9 && addr == MOD_MAIN_BASE + 0x4dcc1c && calc_actor && calc_actor == player_actor_cached && !as_my_mgr) {
        rd(c->x[1], as_name_obj, 0x10), rd(c->x[2], as_req, sizeof as_req);
        as_my_mgr = c->x[0];
    }
    if (prof9 && addr == MOD_MAIN_BASE + 0x4dcc1c && calc_actor && calc_actor == player_actor_cached) {
        char nm[40] = "?";
        if (!name_like(rd_ptr(c->x[1]), nm)) name_like(c->x[1], nm);
        int32_t slot = -1;
        float blend = 0;
        rd(c->x[2] + 0x28, &slot, 4), rd(c->x[2] + 0x34, &blend, 4);
        rt_log("coop: as %.3f %s slot %d blend %.2f mgr %llx (actor+%#llx)", frame_step_world_us() / 1e6, nm, slot, blend,
               (unsigned long long)c->x[0], (unsigned long long)(c->x[0] - calc_actor));
    }
    if (!prof8 || !calc_actor || calc_actor != player_actor_cached) return;
    for (int r = 1; r <= 3; r++) {
        char nm[40];
        uint64_t p = c->x[r];
        if (!name_like(p, nm) && !name_like(rd_ptr(p), nm) && !name_like(rd_ptr(p + 8), nm)) continue;
        uint32_t h = 2166136261u;
        for (const char *q = nm; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
        h ^= (uint32_t)r;
        uint32_t i = (uint32_t)(((addr * 0x9e3779b97f4a7c15ull) >> 40) ^ h) & 8191;
        for (int k = 0; k < 64; k++, i = (i + 1) & 8191) {
            uint64_t have = atomic_load_explicit(&p8[i].fn, memory_order_relaxed);
            if (have == addr && p8[i].hash == h) { p8[i].hits++; break; }
            if (!have && atomic_compare_exchange_strong(&p8[i].fn, &have, addr)) {
                p8[i].hash = h, p8[i].hits = 1;
                snprintf(p8[i].s, sizeof p8[i].s, "x%d:%s", r, nm);
                break;
            }
        }
    }
}
static void p8_report(void) {
    static uint64_t fns[512];
    static int cnt[512];
    int nf = 0;
    for (int i = 0; i < 8192; i++) {
        uint64_t f = atomic_load(&p8[i].fn);
        if (!f) continue;
        int k = 0;
        while (k < nf && fns[k] != f) k++;
        if (k == nf && nf < 512) fns[nf] = f, cnt[nf++] = 0;
        if (k < nf) cnt[k]++;
    }
    for (int shown = 0; shown < 25; shown++) {
        int best = -1;
        for (int k = 0; k < nf; k++) if (cnt[k] > 0 && (best < 0 || cnt[k] > cnt[best])) best = k;
        if (best < 0 || cnt[best] < 3) break;
        char line[700], d[160];
        int m = snprintf(line, sizeof line, "coop: prof8 %s: %d strings:", ldr_describe(fns[best], d, sizeof d), cnt[best]);
        for (int i = 0; i < 8192 && m < 650; i++)
            if (atomic_load(&p8[i].fn) == fns[best]) m += snprintf(line + m, sizeof line - m, " %s(%u)", p8[i].s, p8[i].hits);
        rt_log("%s", line);
        cnt[best] = -cnt[best];
    }
}
void coop_fn_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    if (prof6) { p6_hit(addr, lr); return; }
    if (prof7) {
        if (coop_in_remote_calc() || !x0) return;
        int n = atomic_load_explicit(&p7_n, memory_order_relaxed);
        for (int i = 0; i < n; i++)
            if (x0 >= p7_lo[i] && x0 < p7_hi[i]) { p3_hit(addr, lr, ((uint64_t)p7_slot[i] << 32) | (x0 - p7_lo[i])); return; }
        return;
    }
    if (prof3 && coop_in_remote_calc()) p3_hit(addr, lr, x0);
    static _Atomic uint64_t enemy_fns;
    if (prof4 && enemy_calc) {
        atomic_fetch_add_explicit(&enemy_fns, 1, memory_order_relaxed);
        uint64_t me = player_actor_cached;
        if (x0 && ((x0 >= me && x0 < me + 0x4000) || x0 == prof4_ref)) p3_hit(addr, lr, x0);
    }
    if (prof4 && !(atomic_load(&enemy_fns) & 0xfffff)) {
        static _Atomic uint64_t said;
        if (atomic_fetch_add(&said, 1) < 8) rt_log("coop: prof4: %llu function entries inside enemy calcs so far",
                                                     (unsigned long long)atomic_load(&enemy_fns));
    }
    uint64_t a = atomic_load_explicit(&prof_actor, memory_order_relaxed);
    if (!a || x0 < a || x0 >= a + 0x4000) return;
    for (int i = 0; i < 1024; i++) {
        uint64_t have = atomic_load_explicit(&prof_seen[i].fn, memory_order_relaxed);
        if (!have && atomic_compare_exchange_strong(&prof_seen[i].fn, &have, addr)) {
            prof_seen[i].off = (uint32_t)(x0 - a), prof_seen[i].lr = lr;
            have = addr;
        }
        if (have == addr) { atomic_fetch_add_explicit(&prof_seen[i].hits, 1, memory_order_relaxed); return; }
    }
}

static void prof_report(void) {
    char d[160], e[160];
    int n = 0;
    for (int i = 0; i < 1024 && prof_seen[i].fn; i++, n++)
        rt_log("coop: prof %6u x %s, x0 = actor+%#x, from %s", prof_seen[i].hits,
               ldr_describe(prof_seen[i].fn, d, sizeof d), prof_seen[i].off, ldr_describe(prof_seen[i].lr, e, sizeof e));
    rt_log("coop: prof: %d functions took Player#2", n);
}

// ---- entry hooks (cpu.h COOP_HOOKED)
#define GET_CONTROLLER 0x37ccc0
#define INPUT_COPY 0x37d330   // (input object, controller): the per-frame update (main+0x37c944) fills its consumers
// getController's callers: return address, index, calls since the last report. Filled from any game thread.
static struct { _Atomic uint64_t lr; _Atomic uint32_t index, calls; } gc_callers[64];

#define ACTOR_CALC 0x41bb20   // per-actor update from the actor job (main+0x2f175c), x0 = actor (profiler, 2026-11-03)
static __thread int in_calc;      // this thread runs ACTOR_CALC through the wrapper
static _Atomic int calc_log_left = 40;
static uint8_t neutral[0x200];    // the input object while nothing is held: coop_tick takes it 3 s into the world
static _Atomic int neutral_ok;

int coop_players_calc(Cpu *c, uint64_t addr);    // coop_players.c
void coop_ctl_enter(void);                       // the controller lock (below)
void coop_ctl_leave(void);
int coop_puppet_place(Cpu *c, uint64_t addr);   // coop_players.c: a puppet put at its owner after its physics step
void coop_players_tick(Cpu *c, int numpad3);
uint64_t coop_players_first(void);
int coop_players_gear(Cpu *c, uint64_t addr);

// Camera follow (Numpad 4): while PlayerCamera's calc runs on a thread (in_camera), ActorLink getActor
// (main+0x2804d0) answers Link#2 where it would answer Link#1. Nothing in memory changes.
static __thread int in_camera;
static uint64_t camera_actor;   // PlayerCamera, refreshed by coop_tick
static volatile int cam_two;    // the camera follows Link#2
static _Atomic uint32_t cam_redirects;
#define GET_ACTOR 0x2804d0

static _Atomic uint32_t cam_lookups, cam_wraps;
// The watchpoint counts a fault only on a thread inside PlayerCamera's calls when TOTK_COOP_WATCH=4.
static int watch_camera_only;
int coop_watch_counts(void) { return !watch_camera_only || in_camera; }

static int get_actor_wrap(Cpu *c, uint64_t addr) {
    static __thread int inside;
    if (inside || !in_camera || !cam_two || !second_player) return 0;
    atomic_fetch_add_explicit(&cam_lookups, 1, memory_order_relaxed);
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    if (c->x[0] && c->x[0] == player_actor_cached) {
        c->x[0] = second_player;
        atomic_fetch_add_explicit(&cam_redirects, 1, memory_order_relaxed);
    }
    return 1;
}

static int calc_wrap(Cpu *c, uint64_t addr) {
    uint64_t actor = c->x[0], two = second_player;
    if (!in_camera && actor && actor == camera_actor && cam_two && two) {
        atomic_fetch_add_explicit(&cam_wraps, 1, memory_order_relaxed);
        in_camera = 1;
        c->pc = addr;
        rt_dispatch(c);
        in_camera = 0;
        return 1;
    }
    (void)in_calc;
    // This PC's Link: its update takes the controller lock (above), so no co-op Link's controller is in the manager
    // while it reads it. Only while there are co-op Links.
    static __thread int in_mine;
    int coop_players_any(void);   // coop_players.c
    if (!in_mine && actor && actor == player_actor_cached && coop_players_any()) {
        coop_ctl_enter();
        in_mine = 1;
        c->pc = addr;
        rt_dispatch(c);
        in_mine = 0;
        coop_ctl_leave();
        return 1;
    }
    return coop_players_calc(c, addr);   // a co-op Player (coop_players.c): run with its own controller
}

// requestCreateActor calls (TOTK_COOP_TRACE=create): the actor name and whose calc it ran in (calc_actor: the actor
// whose ACTOR_CALC encloses the call on this thread, tracked for Players only).
static int create_trace(Cpu *c) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "create"); }
    if (!on) return 0;
    char nm[64] = "", who[40] = "";
    uint64_t np = rd_ptr(c->x[1]);
    if (np) rd(np, nm, sizeof nm - 1);
    uint64_t one = player_actor_cached;
    // The guest frame chain of gear creates (x29 links), to find where the item name comes from.
    if (!strncmp(nm, "Weapon_", 7) || !strncmp(nm, "Armor_", 6)) {
        char line[512];
        int n = snprintf(line, sizeof line, "coop: create '%s' chain: x0..x3 %llx %llx %llx %llx; ", nm, (unsigned long long)c->x[19],
                         (unsigned long long)c->x[20], (unsigned long long)c->x[21], (unsigned long long)c->x[22]);
        uint64_t fp = c->x[29];
        for (int d = 0; d < 9 && fp && n < (int)sizeof line - 40; d++) {
            uint64_t next = rd_ptr(fp), ret = rd_ptr(fp + 8);
            if (!ret) break;
            n += snprintf(line + n, sizeof line - n, " <- main+%llx", (unsigned long long)(ret - MOD_MAIN_BASE));
            if (next <= fp) break;
            fp = next;
        }
        rt_log("%s", line);
    }
    rt_log("coop: create '%s' inside %s (lr main+%llx)", nm,
           !calc_actor ? "no Player calc" : calc_actor == one ? "Link#1's calc" : "another Player's calc",
           (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
    (void)who;
    return 0;
}

// main+0x916860 (x0 an Ultrahand part of a Player): draws the Ultrahand beam into one global pool of segments
// ([main+0x395f6f8]: +0x1f0 count, +0x1f4 capacity, +0x1f8 the used list, +0x200 the free list), first putting every
// used segment back on the free list. With a co-op Link there are two Players, and both ran it at once on two worker
// threads while a player held something with Ultrahand: the free list broke (a read of -1 in main+0x19164e0, the
// push; both games of a play session, 2026-11-05). One thread at a time runs it now.
static PlatMutex beam_lock = PLAT_MUTEX_INIT;
static _Atomic int beam_waits;
// A lock held across a game call is waited for with this thread's virtual core given up: the holder can give its
// core up inside the call (kern_checkpoint), and a waiter holding one on the host lock could leave it none (code
// review, 2026-11-06).
void coop_lock_wait(PlatMutex *m) {
    if (plat_mutex_trylock(m)) return;
    int away = kern_core_away();
    plat_mutex_lock(m);
    kern_core_back(&away);
}
// The input manager's controller ([mgr+0xa0]) is swapped for a co-op Link's around its update
// (coop_players_calc); this PC's Link's update and the co-op Links' run on worker threads, so they take turns: two
// at once could leave the manager on a co-op Link's controller, or let this PC's Link read one (code review).
// Taken again on a thread that holds it (one update inside another's), it only counts.
static PlatMutex coop_ctl_lock = PLAT_MUTEX_INIT;
static __thread int ctl_depth;
void coop_ctl_enter(void) {
    if (!ctl_depth++) coop_lock_wait(&coop_ctl_lock);
}
void coop_ctl_leave(void) {
    if (!--ctl_depth) plat_mutex_unlock(&coop_ctl_lock);
}
static int beam_hook(Cpu *c, uint64_t addr) {
    static __thread int inside;
    if (inside) return 0;
    if (!plat_mutex_trylock(&beam_lock)) {
        if (atomic_fetch_add(&beam_waits, 1) < 5) rt_log("coop: the Ultrahand beam was drawn by two threads at once; one waits");
        coop_lock_wait(&beam_lock);
    }
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    plat_mutex_unlock(&beam_lock);
    return 1;
}
int coop_hook(Cpu *c, uint64_t addr) {
    if (addr == MOD_MAIN_BASE + 0x916860) return beam_hook(c, addr);
    if (addr == MOD_MAIN_BASE + 0x599670) return create_trace(c);
    // HUD control guide (bottom-right prompts such as "Dive R"): main+0x2192c50 (type) shows one, main+0x4cb540 (type)
    // hides one, on the one CtrlGuide screen. Every Player's calc calls them; a co-op Link's must not change this
    // PC's HUD, so they do nothing inside one (2026-11-04).
    // An actor's other per-frame update phases besides ACTOR_CALC (its vtable holds main+0x41bb20, main+0x245030,
    // main+0x194fd7c; the actor job also runs main+0x2f9910): a co-op Link's run as its calc (TOTK_COOP_PROF=7 found
    // them: its ragdoll and tips ran there, outside the calc flag) (2026-11-04).
    // (PlayerCamera's main+0x245030 while the camera follows Link#2 goes on to the camera bracket below: taken here
    // always, that bracket's case for it never ran; code review, 2026-11-06)
    int cam_phase = addr == MOD_MAIN_BASE + 0x245030 && !in_camera && c->x[0] && c->x[0] == camera_actor && cam_two && second_player;
    if ((addr == MOD_MAIN_BASE + 0x245030 && !cam_phase) || addr == MOD_MAIN_BASE + 0x2f9910 || addr == MOD_MAIN_BASE + 0x194fd7c)
        return coop_players_phase(c, addr);
    // main+0x2e9c80 (animation binding): Havok's pose into a g3d model. For the co-op Link's model the copied pose
    // (TOTK_COOP_TEST_SKELCOPY) goes over its own right after, so its own animation does not undo it (2026-11-05).
    static int skelcopy = -1;   // (looked up once: it was on every call, code review 2026-11-06)
    if (skelcopy < 0 && addr == MOD_MAIN_BASE + 0x2e9c80) skelcopy = cfg_get("TOTK_COOP_TEST_SKELCOPY") != NULL;
    if (addr == MOD_MAIN_BASE + 0x2e9c80 && skelcopy > 0 && coop_pose_wanted()) {
        // the distinct models it poses once the co-op Link is there (up to 24), for the target check
        static _Atomic uint64_t seen[24];
        coop_pose_saw(c->x[0]);
        uint64_t a = rd_ptr(c->x[0] + 8), b = a ? rd_ptr(a + 8) : 0, h = rd_ptr(c->x[0]);
        for (int i = 0; b && i < 24; i++) {
            uint64_t have = atomic_load(&seen[i]);
            if (have == b) break;
            if (!have && atomic_compare_exchange_strong(&seen[i], &have, b)) {
                rt_log("coop: test: animation binding %llx (vtable main+%#llx) -> %llx -> %llx%s", (unsigned long long)c->x[0],
                       (unsigned long long)(h - MOD_MAIN_BASE), (unsigned long long)a, (unsigned long long)b,
                       coop_pose_is_target(c->x[0]) ? " (the co-op Link's)" : "");
                break;
            }
        }
    }
    // The later writers of a body skeleton's world matrices (write watch, re-notes): main+0x283c750, 0x289620,
    // 0x2925e0, 0x43e450. When one is called for the co-op Link's skeleton or model (an argument names it), the
    // copied pose goes on after it (TOTK_COOP_TEST_SKELCOPY) (2026-11-05).
    // Puppet mode: main+0x283c750 (x0 the input pose, x1 the SkeletonObj) builds a body's local matrices. This
    // Link's input goes out; a co-op Link's is replaced by its player's before it runs (2026-11-05).
    if (addr == MOD_MAIN_BASE + 0x283c750 && coop_puppet_mode()) {
        // This Link's pose is packed here (before writer returns 0 for it). For a co-op Link's skeleton the call runs
        // as usual first, then once more with its player's pose put into the input: written before the first run
        // it did not show (2026-11-05).
        static __thread int in4;
        if (in4) return 0;
        int coop_puppet_is_co_skeleton(uint64_t skel);   // coop_world.c
        if (!coop_puppet_is_co_skeleton(c->x[1])) { coop_puppet_before_writer(c->x[0], c->x[1]); return 0; }
        static __thread Cpu saved;
        saved = *c;
        in4 = 1;
        c->pc = addr;
        rt_dispatch(c);
        static __thread Cpu after;
        after = *c;
        *c = saved;
        if (coop_puppet_before_writer(c->x[0], c->x[1])) {
            c->pc = addr;
            rt_dispatch(c);
            static _Atomic int said_p;
            if (atomic_fetch_add(&said_p, 1) == 0) rt_log("coop: puppet mode: a player's pose went onto their co-op Link (second run)");
        } else *c = after;
        in4 = 0;
        return 1;
    }
    if ((addr == MOD_MAIN_BASE + 0x283c750 || addr == MOD_MAIN_BASE + 0x289620 || addr == MOD_MAIN_BASE + 0x2925e0 ||
         addr == MOD_MAIN_BASE + 0x43e450) && coop_pose_wanted()) {
        static __thread int in2;
        int m = in2 ? 0 : coop_pose_arg_match(c->x, 4);
        uint64_t args[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
        if (!in2 && addr == MOD_MAIN_BASE + 0x283c750) {   // every part of the co-op Link (body, head, hair...)
            in2 = 1;
            c->pc = addr;
            rt_dispatch(c);
            in2 = 0;
            if (coop_pose_after_writer(args, 4)) {
                static _Atomic int said_parts;
                if (atomic_fetch_add(&said_parts, 1) == 0) rt_log("coop: test: part poses copied after main+0x283c750");
            } else if (m) {
                coop_pose_copy_now();
                static _Atomic int said_body;
                if (atomic_fetch_add(&said_body, 1) == 0) rt_log("coop: test: pose copied after main+0x283c750 (body)");
            }
            return 1;
        }
        if (m) {
            in2 = 1;
            c->pc = addr;
            rt_dispatch(c);
            in2 = 0;
            coop_pose_copy_now();
            static _Atomic int said[4];
            int w = addr == MOD_MAIN_BASE + 0x283c750 ? 0 : addr == MOD_MAIN_BASE + 0x289620 ? 1 : addr == MOD_MAIN_BASE + 0x2925e0 ? 2 : 3;
            if (atomic_fetch_add(&said[w], 1) == 0)
                rt_log("coop: test: pose copied after main+%#llx (x%d is the co-op Link's #%d)", (unsigned long long)(addr - MOD_MAIN_BASE), (m - 1) / 8, (m - 1) % 8);
            return 1;
        }
    }
    if (addr == MOD_MAIN_BASE + 0x2e9c80 && coop_pose_is_target(c->x[0])) {
        static __thread int in;
        const char *mode = cfg_get("TOTK_COOP_TEST_SKELCOPY");
        if (in || !mode || !strcmp(mode, "tick")) return 0;
        coop_pose_before_anim(c->x[0]);
        in = 1;
        c->pc = addr;
        rt_dispatch(c);
        in = 0;
        if (!strcmp(mode, "after")) coop_pose_copy_now();   // =after: the skeleton copy after it as well
        static _Atomic int applied;
        if (atomic_fetch_add(&applied, 1) == 0) rt_log("coop: test: the copied pose goes on after the co-op Link's animation");
        return 1;
    }
    if (addr == MOD_MAIN_BASE + 0x2192c50 || addr == MOD_MAIN_BASE + 0x4cb540) return coop_in_remote_calc();
    // The ability screen effects ([main+0x3a5e878], the manager at its +0x3a8 ability): main+0x2056f3c starts one,
    // main+0x205719c ends it ("UltraHand_End", "ReverseRecorder_End"...), main+0x2057384 focus on/off. A co-op Link's
    // Ultrahand tinted this game's world red (tools/coop/tour2.txt) (2026-11-04).
    if ((addr == MOD_MAIN_BASE + 0x2056f3c || addr == MOD_MAIN_BASE + 0x205719c || addr == MOD_MAIN_BASE + 0x2057384) &&
        coop_in_remote_calc()) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 6) rt_log("coop: a co-op Link's ability screen effect main+%#llx skipped", (unsigned long long)(addr - MOD_MAIN_BASE));
        return 1;
    }
    // main+0x4400b0 (life component): its per-frame update applies damage to the life at [comp+0x1830] +8. A co-op
    // Link's life is its own game's: here it takes no damage, so it cannot die here and bring this game's Game Over
    // (its fall killed it here too) (2026-11-04). The life is lifted for the call and put back (healing kept).
    // TOTK_COOP_TEST_LIFE_KEEP: the damage is applied as before (a control for tests).
    if (addr == MOD_MAIN_BASE + 0x4400b0 && coop_is_coop_life(c->x[0])) {
        static __thread int in;
        static int keep = -1;
        if (keep < 0) keep = cfg_get("TOTK_COOP_TEST_LIFE_KEEP") != NULL;
        uint64_t p = rd_ptr(c->x[0] + 0x1830);
        uint32_t before = 0;
        if (in || keep || !p || !rd(p + 8, &before, 4)) return 0;
        ST32(p + 8, 0x3fffffffu);
        in = 1;
        c->pc = addr;
        rt_dispatch(c);
        in = 0;
        uint32_t after = LD32(p + 8);
        if (after < 0x3fffffffu && after > 0x3fff0000u) {   // damage taken off the lifted value (not the clamp to max)
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 8) rt_log("coop: a co-op Link's damage %u not taken here", 0x3fffffffu - after);
        }
        // Put back: life lifted for the call and taken off it is not taken; healed above it, the heal is kept; any
        // other value (the clamp of the lifted value to most life, every update) is the lift's, not the game's, and is
        // put back. (Keeping a value set outright, unless it was the most life, kept the clamp: the most life could not
        // be read in here, and the proxy went to full life in each update it ran; the sync audit, 2026-11-06.)
        ST32(p + 8, after > 0x3fffffffu ? before + (after - 0x3fffffffu) : before);
        return 1;
    }
    // main+0x207ea50 (instant tips manager, tip id) queues an instant tip ("To shield surf...") and records the action
    // (main+0x207eba0). A co-op Link's actions call it from its calc; its own game shows the tip (2026-11-04).
    if (addr == MOD_MAIN_BASE + 0x207ea50 && coop_in_remote_calc()) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 8) rt_log("coop: a co-op Link's instant tip %llu not queued", (unsigned long long)(uint32_t)c->x[1]);
        return 1;
    }
    // main+0x923d20 (game data, 1, &key, value) sets a game-data enum. A Player's calc records its actions in this
    // game's data with it (main+0x207eba0: the last action, e.g. a shield surf), and the instant tips show from those
    // ("To shield surf..."). A co-op Link's actions are its own player's, so nothing is written inside one (2026-11-04).
    if (addr == MOD_MAIN_BASE + 0x923d20) {   // TOTK_COOP_TRACE=gdata: every write, with its time, key, value, caller
        static int tr = -1;
        if (tr < 0) { const char *e = getenv("TOTK_COOP_TRACE"); tr = e && strstr(e, "gdata"); }
        if (tr) {
            uint32_t key = 0;
            rd(c->x[2], &key, 4);
            char from[96];
            rt_log("coop: gdata %.2f s: enum %08x = %llu from %s%s", frame_step_world_us() / 1e6, key,
                   (unsigned long long)(uint32_t)c->x[3], ldr_describe(c->x[30], from, sizeof from),
                   coop_in_remote_calc() ? " (in a co-op Link's calc)" : "");
        }
    }
    static int gdata_keep = -1;   // TOTK_COOP_TEST_GDATA_KEEP: the writes are not skipped (a control for tests)
    if (gdata_keep < 0) gdata_keep = cfg_get("TOTK_COOP_TEST_GDATA_KEEP") != NULL;
    // All the game-data setters a co-op Link's update calls (the action tour, TOTK_COOP_PROF=3 with the game-data
    // manager [[main+0x395dff0]] as x0; the ones that return nothing and take a value, 2026-11-04): main+0x170210,
    // 0x16fdcc (from the player's state code main+0x281d40 / 0x282a0c), 0x170490, 0x282280 (main+0x281d28),
    // 0x2aabf84 (main+0x21919b0), 0x923d20. The getters it calls (main+0x1b02f8, 0x19f9d4, 0x735698, 0x980288,
    // 0x9eee70, 0x16fac0, 0x6e76d0, 0x2aab9b4, 0x282ed8, and the key lookups 0x4561c, 0x478c0, 0x3cffe8) still run.
    // The abilities tour (tools/coop/tour2.txt) added main+0x16ee70 (mgr, value, key hash) and 0x31d824 (value, mgr,
    // key hash).
    if ((addr == MOD_MAIN_BASE + 0x923d20 || addr == MOD_MAIN_BASE + 0x170210 || addr == MOD_MAIN_BASE + 0x16fdcc ||
         addr == MOD_MAIN_BASE + 0x170490 || addr == MOD_MAIN_BASE + 0x282280 || addr == MOD_MAIN_BASE + 0x2aabf84 ||
         addr == MOD_MAIN_BASE + 0x16ee70 || addr == MOD_MAIN_BASE + 0x31d824) &&
        !gdata_keep && coop_in_remote_calc()) {
        static _Atomic int skipped;
        char from[96];
        if (atomic_fetch_add(&skipped, 1) < 12)
            rt_log("coop: a co-op Link's game-data write main+%#llx skipped (from %s)", (unsigned long long)(addr - MOD_MAIN_BASE),
                   ldr_describe(c->x[30], from, sizeof from));
        return 1;
    }
    // main+0x1a30670 (actor system, out link, &name): the one actor of that name (the resident "Player" for AI that
    // wants the player, e.g. QueryTestGetPlayerActor). TOTK_COOP_TRACE=player counts its callers for "Player".
    // main+0x446a84 (emitter object: +0x18 its actor, +0x70 what main+0x45b930 gets): one perception emitter's
    // update. TOTK_COOP_TRACE=emit counts the calls per Link and logs the objects once.
    // main+0x49f5f8 (emitter component: +0x18 its actor, +0x70 its data, +0x88 a sub-object whose virtual +0x10 says
    // skip): the per-actor calc's emitter update (main+0x41bb20). TOTK_COOP_TRACE=emit: per Link, the calls and the
    // skip check's answer.
    // main+0x45b930: one perception receiver's check of the player. It takes the player emitter's data from the
    // perception manager [main+0x3a6ea80] +0x58 (this Link's, the one player the game knows). Its caller (the emitter
    // updates main+0x446a84 / main+0x49f5f8) keeps the receiver's component in x19, whose +0x18 is its actor. For an
    // enemy whose nearest Link is a co-op one, the manager holds that Link's data for this call only: enemies then
    // sense, chase and attack the player nearest to them (coop_world.c coop_nearest_link).
    if (addr == MOD_MAIN_BASE + 0x45b930) {
        static int redirect = -1;   // off by default: it did not change whom enemies attack (re-notes); TOTK_COOP_ENEMY_REDIRECT=1
        if (redirect < 0) redirect = cfg_get("TOTK_COOP_ENEMY_REDIRECT") != NULL;
        if (!redirect) return 0;
        static __thread int inside45;
        if (inside45) return 0;
        uint64_t mgr = rd_ptr(MOD_MAIN_BASE + 0x3a6ea80), actor = rd_ptr(c->x[19] + 0x18);
        uint64_t other = mgr && actor ? coop_nearest_link_data(actor) : 0;
        if (!other) return 0;
        static _Atomic uint32_t redirects, checks;
        uint32_t r = atomic_fetch_add(&redirects, 1);
        if (!(r & 1023)) rt_log("coop: perception: %u checks taken to a co-op Link so far", r + 1);
        (void)checks;
        // One swap at a time: two receivers' checks on two threads could leave [mgr+0x58] on a co-op Link's data
        // for good (code review, 2026-11-06).
        static PlatMutex perc_lock = PLAT_MUTEX_INIT;
        coop_lock_wait(&perc_lock);
        uint64_t mine = rd_ptr(mgr + 0x58);
        inside45 = 1;
        ST64(mgr + 0x58, other);
        c->pc = addr;
        rt_dispatch(c);
        ST64(mgr + 0x58, mine);
        inside45 = 0;
        plat_mutex_unlock(&perc_lock);
        return 1;
    }
    if (addr == MOD_MAIN_BASE + 0x49f5f8) {
        static int on = -1;
        if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "emit"); }
        if (on) {
            static _Atomic int raw;
            if (atomic_fetch_add(&raw, 1) < 8) {
                uint64_t o = c->x[0];
                char line[512];
                int n = snprintf(line, sizeof line, "coop: emitter2 %llx:", (unsigned long long)o);
                for (int k = 0; k < 0x30; k += 8) {
                    char nm[16];
                    uint64_t v = rd_ptr(o + k);
                    n += snprintf(line + n, sizeof line - n, " +%#x=%llx(%s)", k, (unsigned long long)v, actor_name(v, nm, 16));
                }
                rt_log("%s; this Link %llx", line, (unsigned long long)player_actor_cached);
            }
            uint64_t o = c->x[0], act = rd_ptr(o + 0x18);
            char nm[16];
            actor_name(act, nm, sizeof nm);
            if (!strcmp(nm, "Player")) {
                int mine = act == player_actor_cached;
                static _Atomic uint32_t calls[2];
                static _Atomic int shown[2];
                atomic_fetch_add(&calls[mine], 1);
                if (atomic_fetch_add(&shown[mine], 1) < 3) {
                    uint64_t sub = o + 0x88, fn = rd_ptr(rd_ptr(sub) + 0x10);
                    uint64_t skip = guest_call(c, fn, sub, 0, 0, 0);
                    char d[96];
                    rt_log("coop: player emitter update for %s Link: component %llx, data %llx, skip check %s says %llu",
                           mine ? "this" : "a co-op", (unsigned long long)o, (unsigned long long)rd_ptr(o + 0x70),
                           ldr_describe(fn, d, sizeof d), (unsigned long long)(skip & 1));
                }
                static uint64_t last;
                uint64_t now = plat_now_ns();
                if (now - last > 10000000000ull) {
                    last = now;
                    rt_log("coop: player emitter updates in 10 s: this Link %u, co-op Links %u", atomic_exchange(&calls[1], 0), atomic_exchange(&calls[0], 0));
                }
            }
        }
        return 0;
    }
    if (addr == MOD_MAIN_BASE + 0x446a84) {
        static int on = -1;
        if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "emit"); }
        if (on) {
            uint64_t o = c->x[0], act = rd_ptr(o + 0x18);
            static _Atomic int raw;
            if (atomic_fetch_add(&raw, 1) < 6) {
                char n1[16], n2[16], n3[16];
                rt_log("coop: emitter object %llx: +0x10 %llx (%s / [+0x150] %s), +0x18 %llx (%s), +0x70 %llx; this Link %llx", (unsigned long long)o,
                       (unsigned long long)rd_ptr(o + 0x10), actor_name(rd_ptr(o + 0x10), n1, 16), actor_name(rd_ptr(rd_ptr(o + 0x10) + 0x150), n2, 16),
                       (unsigned long long)act, actor_name(act, n3, 16), (unsigned long long)rd_ptr(o + 0x70), (unsigned long long)player_actor_cached);
            }
            char nm[16];
            actor_name(act, nm, sizeof nm);
            if (!strcmp(nm, "Player")) {
                int mine = act == player_actor_cached;
                static _Atomic uint32_t calls[2];
                static _Atomic int shown[2];
                atomic_fetch_add(&calls[mine], 1);
                if (atomic_fetch_add(&shown[mine], 1) < 2) {
                    uint64_t comp = rd_ptr(rd_ptr(act + 0x228) + 0x378);
                    rt_log("coop: emitter update for %s Link %llx: object %llx (+0x70 %llx), perception comp %llx (+0x30 %llx)",
                           mine ? "this" : "a co-op", (unsigned long long)act, (unsigned long long)o,
                           (unsigned long long)rd_ptr(o + 0x70), (unsigned long long)comp, (unsigned long long)rd_ptr(comp + 0x30));
                }
                static uint64_t last;
                uint64_t now = plat_now_ns();
                if (now - last > 10000000000ull) {
                    last = now;
                    rt_log("coop: emitter updates in 10 s: this Link %u, co-op Links %u", atomic_exchange(&calls[1], 0), atomic_exchange(&calls[0], 0));
                }
            }
        }
        return 0;
    }
    if (addr == MOD_MAIN_BASE + 0x1a30670) {
        static int on = -1;
        if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "player"); }
        if (on) {
            char nm[16] = "";
            rd(rd_ptr(c->x[2]), nm, 15);
            static _Atomic int seen_n;
            if (atomic_fetch_add(&seen_n, 1) < 20) rt_log("coop: named lookup of '%s'", nm);
            if (!strcmp(nm, "Player")) p3_hit(c->x[30], 0, enemy_calc);
        }
        return 0;
    }
    if (addr == MOD_MAIN_BASE + 0xc4961c || addr == MOD_MAIN_BASE + 0xaf100c) return coop_players_gear(c, addr);
    if (addr == MOD_MAIN_BASE + ACTOR_CALC) {
        coop_actor_seen(c->x[0]);   // every live actor passes here each frame (coop_world.c)
    if (prof4 && !enemy_calc && c->x[0]) {   // an enemy's calc, bracketed for the profiler
        char en[16];
        actor_name(c->x[0], en, sizeof en);
        static _Atomic int shown;
        if (!strncmp(en, "Enemy_", 6) && atomic_fetch_add(&shown, 1) < 6) rt_log("coop: prof4: bracketing the calc of '%s' (profiler %d)", en, g_fnprof);
        if (!strncmp(en, "Enemy_", 6)) {
            enemy_calc = c->x[0];
            c->pc = addr;
            rt_dispatch(c);
            enemy_calc = 0;
            return 1;
        }
    }
        // Track which Player's calc this thread runs, for the create trace.
        char nm[16];
        uint64_t a = c->x[0];
        // (getenv once: it ran for every actor's update every frame, on every thread, code review 2026-11-06)
        static int trace_on = -1;
        if (trace_on < 0) trace_on = getenv("TOTK_COOP_TRACE") != NULL;
        if (trace_on && !calc_actor && a && !strcmp(actor_name(a, nm, sizeof nm), "Player")) {
            calc_actor = a;
            int r = calc_wrap(c, addr);
            if (!r) { c->pc = addr; static __thread int deep; if (!deep) { deep = 1; rt_dispatch(c); deep = 0; r = 1; } }
            calc_actor = 0;
            return r;
        }
        return calc_wrap(c, addr);
    }
    if (addr == MOD_MAIN_BASE + GET_ACTOR) return get_actor_wrap(c, addr);
    // ActorLink lock-and-get main+0x1afbe4 (out, link): a link is {+8 reference object, +0x10 its id}; inside
    // PlayerCamera's calls a link to Link#1 is handed over as a copy that names Link#2's reference (patch page +0x800;
    // only the camera thread uses it while the camera runs).
    // (Only while second_ref still names the Player#2 in the world: it was never cleared, and after that Link went it
    // named a dead object. The copy goes in one of 32 slots in turn, not one shared by the camera's threads, and only
    // when the patch page exists; code review, 2026-11-06.)
    if (addr == MOD_MAIN_BASE + 0x1afbe4 && in_camera && cam_two && second_ref && second_player && rt_patch_page() &&
        rd_ptr(second_ref + 0x40) == second_player) {
        uint64_t link = c->x[1], ref = link ? LD64(link + 8) : 0;
        if (ref && LD64(ref + 0x40) == player_actor_cached) {
            static _Atomic uint32_t next_tmp;
            uint64_t tmp = RT_PATCH_PAGE + 0x800 + 0x20 * (atomic_fetch_add(&next_tmp, 1) & 31);
            memcpy((void *)(uintptr_t)tmp, (void *)(uintptr_t)link, 0x20);
            ST64(tmp + 8, second_ref);
            ST32(tmp + 0x10, LD32(second_ref + 0x48));
            c->x[1] = tmp;
            atomic_fetch_add_explicit(&cam_redirects, 1, memory_order_relaxed);
        }
        return 0;
    }
    if (addr == MOD_MAIN_BASE + 0x257310 && coop_puppet_place(c, addr)) return 1;   // coop_players.c: puppet mode
    // The other functions the profiler saw entered with PlayerCamera (TOTK_COOP_PROF=2): bracket them for the
    // camera too.
    if (addr == MOD_MAIN_BASE + 0x2e27b0 || addr == MOD_MAIN_BASE + 0x245030 || addr == MOD_MAIN_BASE + 0x257310 ||
        addr == MOD_MAIN_BASE + 0x2f7f70 || addr == MOD_MAIN_BASE + 0x19557fc || addr == MOD_MAIN_BASE + 0x6c9ad0) {
        uint64_t actor = c->x[0];
        if (in_camera || !actor || actor != camera_actor || !cam_two || !second_player) return 0;
        atomic_fetch_add_explicit(&cam_wraps, 1, memory_order_relaxed);
        in_camera = 1;
        c->pc = addr;
        rt_dispatch(c);
        in_camera = 0;
        return 1;
    }
    if (addr == MOD_MAIN_BASE + INPUT_COPY) {
        uint64_t dst = c->x[0];
        for (int i = 0; i < 16; i++) {
            uint64_t have = atomic_load_explicit(&copy_dst[i], memory_order_relaxed);
            if (have == dst) return 0;
            if (!have && atomic_compare_exchange_strong(&copy_dst[i], &have, dst)) {
                rt_log("coop: input copy into %llx (controller %llx, from main+%llx)", (unsigned long long)dst,
                       (unsigned long long)c->x[1], (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
                return 0;
            }
        }
        return 0;
    }
    if (addr != MOD_MAIN_BASE + GET_CONTROLLER) return 0;
    uint64_t lr = c->x[30];
    uint32_t index = (uint32_t)c->x[1];
    for (int i = 0; i < 64; i++) {
        uint64_t have = atomic_load_explicit(&gc_callers[i].lr, memory_order_relaxed);
        if (!have && atomic_compare_exchange_strong(&gc_callers[i].lr, &have, lr)) {
            atomic_store(&gc_callers[i].index, index);
            char name[48] = "";
            uint64_t np = rd_ptr(c->x[2]);
            if (np) rd(np, name, sizeof name - 1);
            rt_log("coop: getController first called from main+%llx, index %u, user '%s'",
                   (unsigned long long)(lr - MOD_MAIN_BASE), index, name);
        }
        if (have == lr || atomic_load_explicit(&gc_callers[i].lr, memory_order_relaxed) == lr) {
            atomic_fetch_add_explicit(&gc_callers[i].calls, 1, memory_order_relaxed);
            return 0;
        }
    }
    return 0;
}

static void report_callers(void) {
    char line[1024];
    int n = snprintf(line, sizeof line, "coop: getController calls in the last ~5 s:");
    for (int i = 0; i < 64 && gc_callers[i].lr && n < (int)sizeof line - 48; i++) {
        uint32_t k = atomic_exchange(&gc_callers[i].calls, 0);
        if (k) n += snprintf(line + n, sizeof line - n, " main+%llx(i%u)x%u",
                             (unsigned long long)(gc_callers[i].lr - MOD_MAIN_BASE), gc_callers[i].index, k);
    }
    rt_log("%s", line);
}

// ---- player 2's input. TOTK_COOP_P2=pad (the default): the second connected gamepad (input.c input_pad2_read);
// =walk: a test pattern, left stick forward from 4 to 10 s of the world clock, then right; =off: none.
static int p2_mode = -1;   // 0 off, 1 walk, 2 pad
int coop_p2_connected(void) {
    if (p2_mode < 0) {
        const char *e = cfg_get("TOTK_COOP_P2");
        p2_mode = !e || !*e || !strcmp(e, "pad") ? 2 : !strcmp(e, "walk") ? 1 : 0;
    }
    return p2_mode > 0 && frame_step_world_us() > 0;   // plugged in once the save is in the world: with it from the
                                                      // start the title never reached the world (2026-11-03)
}
// Whether the game is told about player 2 as Npad id 1 as well (TOTK_COOP_P2_NPAD=1). Off by default: player 2's state
// reaches the game through controller 1 (p2_fill) either way.
int coop_p2_npad(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_P2_NPAD"); on = e && *e == '1'; }
    return on && coop_p2_connected();
}
int coop_p2_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!coop_p2_connected()) return 0;
    *buttons = 0, *lx = 0, *ly = 0, *rx = 0, *ry = 0;
    if (p2_mode == 2) {
        int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
        int ok = input_pad2_read(buttons, sl, sr);
        *lx = sl[0], *ly = sl[1], *rx = sr[0], *ry = sr[1];
        return ok;
    }
    int64_t t = frame_step_world_us();
    if (t > 4000000 && t < 10000000) *ly = 32767;
    else if (t >= 10000000 && t < 14000000) *lx = 32767;
    return 1;
}

void coop_tick(struct Cpu *c) {
    static int trace_on = -1;   // (getenv once)
    if (trace_on < 0) trace_on = getenv("TOTK_COOP_TRACE") != NULL;
    if (trace_on) log_callers(c);
    player_actor_cached = player_actor();
    coop_trace_tick(player_actor_cached);   // coop_trace.c: TOTK_COOP_TRACE_AT
    if (prof4) resident_named("Player", &prof4_ref);
    static int report_ticks;
    if (trace_on && ++report_ticks % 300 == 0) report_callers();   // input tracing only
    static int prev;
    int keys = input_coop_keys(), pressed = keys & ~prev;
    prev = keys;
    // Once: main's .data/.bss words holding the input object the copy fills (copy_dst[0]) or its owner (dst - 0x198),
    // so the generated C can be searched for the code that loads them.
    static int globals_done;
    uint64_t dst0 = atomic_load(&copy_dst[0]);
    if (dst0 && !globals_done) {
        globals_done = 1;
        static uint64_t page[0x1000 / 8];
        int found = 0;
        for (uint64_t a = MOD_MAIN_BASE + 0x3569000; a < MOD_MAIN_BASE + 0x3adf000 && found < 16; a += 0x1000) {
            if (!rd(a, page, sizeof page)) continue;
            for (int i = 0; i < 0x1000 / 8; i++)
                if (page[i] == dst0 || page[i] == dst0 - 0x198) {
                    rt_log("coop: main+%llx holds the input %s", (unsigned long long)(a + i * 8 - MOD_MAIN_BASE),
                           page[i] == dst0 ? "object" : "owner (object - 0x198)");
                    found++;
                }
        }
        rt_log("coop: input object %llx: %d global slots", (unsigned long long)dst0, found);
    }
    // TOTK_COOP_WATCH=1: once in the world, watch the input object (copy_dst[0], 0x200 bytes) for 90 polls, then the
    // index-0 controller (0x200 bytes) for 90 polls, and log which game functions touched each.
    static int watch_phase, watch_left;
    if (!watch_phase) { const char *e = cfg_get("TOTK_COOP_WATCH"); watch_phase = e && *e >= '1' && *e <= '5' ? 1 : -1; }
    if (watch_phase > 0 && frame_step_world_us() > 3000000) {
        if (watch_left && !--watch_left) { coop_watch_disarm(); watch_phase++; }
        if (!watch_left && watch_phase <= 2) {
            uint64_t mgr = rd_ptr(rd_ptr(MOD_MAIN_BASE + INPUT_MGR_SLOT));
            // TOTK_COOP_WATCH=1: the input object, then controller 0; =2: controller 1, then controller 0 (vtables logged)
            const char *mode = cfg_get("TOTK_COOP_WATCH");
            int first = mode && *mode == '2';
            uint64_t target = watch_phase == 1 ? (first ? (mgr ? rd_ptr(mgr + 0xf0) : 0) : atomic_load(&copy_dst[0]))
                                               : mgr ? rd_ptr(mgr + 0xa0) : 0;
            watch_camera_only = mode && (*mode == '4' || *mode == '5');
            if (mode && *mode == '5') {   // =5: Link#1's actor +0x200 (position +0x2b4), then +0x400, camera faults only
                uint64_t one = resident_named("Player", NULL);
                target = one + (watch_phase == 1 ? 0x200 : 0x400);
            } else
            if (mode && (*mode == '3' || *mode == '4')) {   // =3: Link#1's resident reference object, then Link#1's
                                                              // actor head; =4: the same, faults inside PlayerCamera only
                uint64_t ref1 = 0, one = resident_named("Player", &ref1);
                target = watch_phase == 1 ? ref1 : one;
            }
            rt_log("coop: watch phase %d: %s, vtable main+%llx", watch_phase,
                   watch_phase == 1 ? (first ? "controller 1" : "input object") : "controller 0",
                   (unsigned long long)(rd_ptr(target) - MOD_MAIN_BASE));
            if (target && coop_watch_arm(target, 0x200)) watch_left = 90;
            else watch_phase++;
        }
    }
    static int prof_phase, prof_left;
    static int prof_kind;   // TOTK_COOP_PROF=2: functions entered with PlayerCamera instead of Player#2
    if (!prof_phase) {
        const char *e = cfg_get("TOTK_COOP_PROF");
        prof_phase = e && (*e == '1' || *e == '2') ? 1 : -1;
        prof_kind = e && *e == '2';
        if (e && *e == '3') prof3 = 1, g_fnprof = 1;
    if (e && *e == '4') prof3 = prof4 = 1;   // the profiler from the world on (loading is too slow under it)
        if (e && *e == '7') prof7 = prof3 = 1, g_fnprof = 1;
        if (e && *e == '8') prof8 = 1;
        if (e && *e == '9') prof9 = 1;
        if (e && *e == '6') {
            const char *a = cfg_get("TOTK_COOP_PROF_AFTER");
            prof6 = 1, g_fnprof = 1;
            if (a) prof6_after = atof(a);
        }
        rt_log("coop: TOTK_COOP_PROF=%s", e ? e : "(unset)");
    }
    static int player_trace = -1;   // TOTK_COOP_TRACE=player: the callers of the named lookup for "Player", every 10 s
    if (player_trace < 0) { const char *e = getenv("TOTK_COOP_TRACE"); player_trace = e && strstr(e, "player"); }
    if (player_trace) {
        static int t2;
        if (++t2 % 300 == 0) {
            char d[160];
            int n = 0;
            for (int i = 0; i < 8192; i++) {
                uint64_t fn = atomic_load(&p3[i].fn);
                if (!fn) continue;
                rt_log("coop: \"Player\" looked up %u x from %s", atomic_load(&p3[i].hits), ldr_describe(fn, d, sizeof d));
                atomic_store(&p3[i].hits, 0), atomic_store(&p3[i].fn, 0), n++;
            }
            rt_log("coop: \"Player\" lookups: %d callers in 10 s", n);
        }
    }
    if (fanhit_at == -2) fanhit_at = cfg_get("TOTK_COOP_TEST_FANHIT") ? atof(cfg_get("TOTK_COOP_TEST_FANHIT")) : -1;
    if (fanhit_at > 0 && frame_step_world_us() > 0) g_fnprof = 1;
    fanhit_report();
    if (flagcaller < 0) flagcaller = cfg_get("TOTK_COOP_TEST_FLAGCALLER") != NULL;
    if (flagcaller > 0 && frame_step_world_us() > 0) g_fnprof = 1;
    if (prof6) g_fnprof = 1;
    if (prof9 && frame_step_world_us() > 0) g_fnprof = 1;
    if (prof8 && frame_step_world_us() > 0) {   // from the world on (loading is too slow under the profiler)
        static int t8;
        g_fnprof = 1;
        if (++t8 % 300 == 0) p8_report();
    }
    if (prof7) p7_refresh();   // resolution.c switches it off once the render size is set
    if (prof3) {   // reported every 10 s (coop_tick runs at 30 Hz)
        static int t;
        if (!prof4 || frame_step_world_us() > 0) g_fnprof = 1;   // resolution.c switches it off once the render size is set
        if (++t % 300 == 0) p3_report();
    }
    if (prof_phase == 1 && second_player && ++prof_left == 60) {   // 2 s after the claim: on for 30 polls
        atomic_store(&prof_actor, prof_kind ? resident_named("PlayerCamera", NULL) : second_player);
        g_fnprof = 1;
        rt_log("coop: prof on");
    } else if (prof_phase == 1 && prof_left == 90) {
        g_fnprof = 0;
        atomic_store(&prof_actor, 0);
        prof_phase = 2;
        prof_report();
    }
    if (!atomic_load(&neutral_ok) && atomic_load(&copy_dst[0]) && frame_step_world_us() > 3000000 &&
        rd(atomic_load(&copy_dst[0]), neutral, sizeof neutral)) {
        atomic_store(&neutral_ok, 1);
        rt_log("coop: idle input snapshot taken");
    }
    // TOTK_COOP_CTRLDUMP=1: every 20 polls in the world, the 32-bit words of controllers 0 and 1 that are nonzero
    // floats in (-2, 2) other than 1.0, to find the sticks and which Npad feeds controller 1.
    static int ctrldump = -1, cd_ticks;
    if (ctrldump < 0) { const char *e = cfg_get("TOTK_COOP_CTRLDUMP"); ctrldump = e ? atoi(e) : 0; }
    // =2: every 4 polls, each 32-bit word of controller 0 that differs from its value at 3 s into the world.
    static uint32_t base[0x200 / 4];
    static int base_ok;
    if (ctrldump == 2 && frame_step_world_us() > 3000000) {
        uint64_t mgr = rd_ptr(rd_ptr(MOD_MAIN_BASE + INPUT_MGR_SLOT)), ctl = mgr ? rd_ptr(mgr + 0xa0) : 0;
        uint32_t w[0x200 / 4];
        if (ctl && rd(ctl, w, sizeof w)) {
            if (!base_ok) { memcpy(base, w, sizeof w); base_ok = 1; }
            else if (++cd_ticks % 4 == 0) {
                char line[1024];
                int n = snprintf(line, sizeof line, "coop: t=%.2f ctrl0 diff", frame_step_world_us() / 1e6), any = 0;
                for (int i = 2; i < 0x200 / 4 && n < (int)sizeof line - 40; i++)
                    if (w[i] != base[i]) { n += snprintf(line + n, sizeof line - n, " +%#x=%08x", i * 4, w[i]); any = 1; }
                if (any) rt_log("%s", line);
            }
        }
    }
    if (ctrldump == 1 && frame_step_world_us() > 0 && ++cd_ticks % 20 == 0) {
        uint64_t mgr = rd_ptr(rd_ptr(MOD_MAIN_BASE + INPUT_MGR_SLOT));
        for (int k = 0; mgr && k < 2; k++) {
            uint64_t ctl = rd_ptr(mgr + (k ? 0xf0 : 0xa0));
            uint32_t w[0x200 / 4];
            if (!ctl || !rd(ctl, w, sizeof w)) continue;
            char line[1024];
            int n = snprintf(line, sizeof line, "coop: t=%.1f ctrl%d", frame_step_world_us() / 1e6, k);
            for (int i = 2; i < 0x200 / 4 && n < (int)sizeof line - 40; i++) {
                float f;
                memcpy(&f, &w[i], 4);
                if (w[i] && f > -2 && f < 2 && f != 1.0f && (f > 0.05f || f < -0.05f))
                    n += snprintf(line + n, sizeof line - n, " +%#x=%.2f", i * 4, f);
            }
            rt_log("%s", line);
        }
    }
    // TOTK_COOP_CTRLDUMP=3: once, 3 s into the world, every 32-bit word where controllers 0 and 1 differ (0x400 bytes).
    if (ctrldump == 3 && frame_step_world_us() > 3000000) {
        ctrldump = 0;
        uint64_t mgr = rd_ptr(rd_ptr(MOD_MAIN_BASE + INPUT_MGR_SLOT));
        uint32_t a0[0x400 / 4], a1[0x400 / 4];
        if (mgr && rd(rd_ptr(mgr + 0xa0), a0, sizeof a0) && rd(rd_ptr(mgr + 0xf0), a1, sizeof a1)) {
            char line[2048];
            int n = snprintf(line, sizeof line, "coop: ctrl0 vs ctrl1:");
            for (int i = 0; i < 0x400 / 4 && n < (int)sizeof line - 40; i++)
                if (a0[i] != a1[i]) n += snprintf(line + n, sizeof line - n, " +%#x %08x/%08x", i * 4, a0[i], a1[i]);
            rt_log("%s", line);
        }
    }
    if (dump_ticks) { dump_ticks--; dump_resident("a poll after the Player claim"); }
    // Camera: Numpad 4 switches which Link the camera follows; TOTK_COOP_CAMAT=<seconds> switches it to Link#2 at that
    // world-clock time (unattended runs).
    static int cam_at = -2;
    if (cam_at == -2) { const char *e = cfg_get("TOTK_COOP_CAMAT"); cam_at = e ? atoi(e) : -1; }
    if (pressed & 8) cam_two = !cam_two, rt_log("coop: camera follows Link#%d", cam_two + 1);
    if (cam_at > 0 && second_player && frame_step_world_us() > cam_at * 1000000ll) {
        cam_at = -1, cam_two = 1;
        rt_log("coop: camera follows Link#2");
    }
    static int cam_back = -2;   // TOTK_COOP_CAMBACK=<seconds>: back to Link#1 then
    if (cam_back == -2) { const char *e = cfg_get("TOTK_COOP_CAMBACK"); cam_back = e ? atoi(e) : -1; }
    if (cam_back > 0 && cam_two && frame_step_world_us() > cam_back * 1000000ll) {
        cam_back = -1, cam_two = 0;
        rt_log("coop: camera follows Link#1");
    }
    camera_actor = resident_named("PlayerCamera", NULL);
    static int cam_report;
    if (cam_two && ++cam_report % 300 == 0) rt_log("coop: camera: %u wrapped calls, %u lookups, %u redirected", atomic_load(&cam_wraps),
                                                     atomic_load(&cam_lookups), atomic_load(&cam_redirects));
    if (second_player && getenv("TOTK_COOP_CAMMASK")) {   // the old memory patch (harmful, kept for comparison)
        uint64_t one = player_actor();
        if (cam_two) camera_follow(second_player, one);
        else camera_follow(one, second_player);
    }
    static int edges_left = -1;
    if (second_player && edges_left < 0) edges_left = 400;
    if (edges_left > 0 && !--edges_left) {
        list_dependencies("Link#1", player_actor());
        list_dependencies("Link#2", second_player);
        // main's .data/.bss words that point at either Link (the HUD shows Link#2's prompts: some global took it).
        static uint64_t page[0x1000 / 8];
        uint64_t one = player_actor(), two = second_player;
        int found = 0;
        for (uint64_t a = MOD_MAIN_BASE + 0x3569000; a < MOD_MAIN_BASE + 0x3adf000 && found < 32; a += 0x1000) {
            if (!rd(a, page, sizeof page)) continue;
            for (int i = 0; i < 0x1000 / 8; i++)
                if (page[i] == one || page[i] == two) {
                    rt_log("coop: global main+%llx = Link#%d", (unsigned long long)(a + i * 8 - MOD_MAIN_BASE), page[i] == one ? 1 : 2);
                    found++;
                }
        }
        rt_log("coop: %d globals point at a Link", found);
        camera_scan();
    }
    // Both Links' position and facing about once a second (the gamepad is polled once a frame): does Player#2 move,
    // and with whose input?
    static int ticks;
    if (second_player && ++ticks % 60 == 0) {
        uint64_t one = player_actor();
        float p1[3] = {0}, p2[3] = {0}, r1[9] = {0}, r2[9] = {0};
        int32_t state = -1;
        rd(one + ACTOR_POS, p1, 12), rd(one + ACTOR_ROT, r1, 36);
        rd(second_player + ACTOR_POS, p2, 12), rd(second_player + ACTOR_ROT, r2, 36), rd(second_player + 0x24, &state, 4);
        {
            uint64_t cam = resident_named("PlayerCamera", NULL);
            float cp[3] = {0};
            int32_t n1 = 0;
            if (cam) rd(cam + ACTOR_POS, cp, 12);
            rd(one + 0x1d0, &n1, 4);
            float d1 = (cp[0] - p1[0]) * (cp[0] - p1[0]) + (cp[2] - p1[2]) * (cp[2] - p1[2]),
                  d2 = (cp[0] - p2[0]) * (cp[0] - p2[0]) + (cp[2] - p2[2]) * (cp[2] - p2[2]);
            rt_log("coop: camera %.1f m from Link#1, %.1f m from Link#2; Link#1 has %d update links", sqrtf(d1), sqrtf(d2), n1);
            float cr[9] = {0};
            if (cam) rd(cam + ACTOR_ROT, cr, 36);
            rt_log("coop: camera at %.1f %.1f %.1f rot cols x %.2f %.2f %.2f | z %.2f %.2f %.2f; Link#1 facing %.2f %.2f", cp[0],
                   cp[1], cp[2], cr[0], cr[3], cr[6], cr[2], cr[5], cr[8], r1[2], r1[8]);
        }
        rt_log("coop: Link#1 %.2f %.2f %.2f facing %.2f %.2f | Link#2 %.2f %.2f %.2f facing %.2f %.2f state %d",
               p1[0], p1[1], p1[2], r1[2], r1[8], p2[0], p2[1], p2[2], r2[2], r2[8], state);
        if (state < 2 || state > 6) second_player = 0;   // gone or going away: stop reading it
    }
    // TOTK_COOP_AUTOSPAWN=<polls>: spawn Player#2 that many gamepad polls (about 30 a second) after Link is in the
    // world (frame_step_world_us), for unattended runs (scripted input, no numpad).
    static int auto_left = -1, auto3;
    if (auto_left == -1) { const char *e = cfg_get("TOTK_COOP_AUTOSPAWN"); auto_left = e ? atoi(e) : 0; }
    if (auto_left > 0) {
        if (frame_step_world_us() > 0 && !--auto_left) {   // the world clock runs once the save is in the world
            rt_log("coop: autospawn (world clock %lld us, snapshot %d, read %d)", (long long)frame_step_world_us(),
                   atomic_load(&neutral_ok), rd(atomic_load(&copy_dst[0]), neutral, 16));
            auto3 = 1;
        }
    }
    coop_players_tick(c, (pressed & 4) || auto3);
    auto3 = 0;
    second_player = coop_players_first();   // the tracing below looks at the first co-op Player
    for (int k = 0; k < 3; k++) {
        // Test: TOTK_COOP_TEST_SPAWN=<actor>@<s> spawns that actor in slot 1 (Numpad 2's) that many seconds into the world.
        static int test_spawned;
        const char *ts = cfg_get("TOTK_COOP_TEST_SPAWN");
        const char *at = ts ? strchr(ts, '@') : NULL;
        if (k == 1 && at && !test_spawned && frame_step_world_us() / 1e6 >= atof(at + 1)) {
            static char nm[64];
            snprintf(nm, sizeof nm, "%.*s", (int)(at - ts), ts);
            names[1] = nm, test_spawned = 1;
            spawn(c, 1);
        }
        if (k < 2 && pressed & 1 << k) spawn(c, k);
        if (!rd_ptr((uint64_t)(uintptr_t)handles[k])) continue;
        int st = handle_status(k);
        if (st != last_status[k]) rt_log("coop: %s handle status %d", names[k], st);
        last_status[k] = st;
        if (st == 3) {
            if (k == 2) dump_resident("Player created, before the claim");
            memset(links[k], 0, sizeof links[k]);
            uint64_t got = guest_call(c, MOD_MAIN_BASE + CLAIM_ACTOR, (uint64_t)(uintptr_t)handles[k],
                                      (uint64_t)(uintptr_t)links[k], 1, 0);
            uint64_t l[2], name = 0;
            memcpy(l, links[k], sizeof l);
            char text[32] = "";
            if (l[0] && (name = rd_ptr(l[0] + ACTOR_NAME))) rd(name, text, sizeof text - 1);
            rt_log("coop: %s claimed: %s; link %016llx %016llx (name at link[0] '%s')", names[k],
                   got & 1 ? "ok" : "failed", (unsigned long long)l[0], (unsigned long long)l[1], text);
            if (k == 2) {
                dump_resident("right after the Player claim");
                uint64_t second = l[1] ? rd_ptr(l[1] + 0x40) : 0;   // the link's reference object: +0x40 actor
                char nm[40];
                rt_log("coop: Player#2 actor %llx '%s'", (unsigned long long)second, actor_name(second, nm, 40));
                if (second && second != player_actor()) copy_dependencies(c, player_actor(), second);
                second_player = second;
                second_ref = l[1];
                find_controller("Link#1", player_actor());
                find_controller("Link#2", second);
                find_input("Link#1", player_actor());
                find_input("Link#2", second);
                dump_ticks = 5;
            }
        }
    }
}
