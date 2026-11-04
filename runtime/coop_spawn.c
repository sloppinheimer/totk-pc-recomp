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

static const char *const names[3] = {"Item_Fruit_A", "Npc_Kokiri007", "Player"};
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

void coop_fn_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
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
void coop_players_tick(Cpu *c, int numpad3);
uint64_t coop_players_first(void);

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
    return coop_players_calc(c, addr);   // a co-op Player (coop_players.c): run with its own controller
}

int coop_hook(Cpu *c, uint64_t addr) {
    if (addr == MOD_MAIN_BASE + ACTOR_CALC) return calc_wrap(c, addr);
    if (addr == MOD_MAIN_BASE + GET_ACTOR) return get_actor_wrap(c, addr);
    // ActorLink lock-and-get main+0x1afbe4 (out, link): a link is {+8 reference object, +0x10 its id}; inside
    // PlayerCamera's calls a link to Link#1 is handed over as a copy that names Link#2's reference (patch page +0x800;
    // only the camera thread uses it while the camera runs).
    if (addr == MOD_MAIN_BASE + 0x1afbe4 && in_camera && cam_two && second_ref) {
        uint64_t link = c->x[1], ref = link ? LD64(link + 8) : 0;
        if (ref && LD64(ref + 0x40) == player_actor_cached) {
            uint64_t tmp = RT_PATCH_PAGE + 0x800;
            memcpy((void *)(uintptr_t)tmp, (void *)(uintptr_t)link, 0x20);
            ST64(tmp + 8, second_ref);
            ST32(tmp + 0x10, LD32(second_ref + 0x48));
            c->x[1] = tmp;
            atomic_fetch_add_explicit(&cam_redirects, 1, memory_order_relaxed);
        }
        return 0;
    }
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

void coop_hide_console(void);   // coop_watch_win.c
void coop_tick(struct Cpu *c) {
    static int console_done;
    if (!console_done) {
        console_done = 1;
        const char *e = cfg_get("TOTK_COOP_HIDE_CONSOLE");
        if (e && *e == '1') coop_hide_console();
    }
    log_callers(c);
    player_actor_cached = player_actor();
    static int report_ticks;
    if (++report_ticks % 300 == 0) report_callers();
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
        rt_log("coop: TOTK_COOP_PROF=%s", e ? e : "(unset)");
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
