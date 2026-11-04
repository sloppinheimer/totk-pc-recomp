// Co-op Phase 1 spawn test: numpad 1 / 2 / 3 ask the game's actor manager for an apple, an NPC or a second Player
// 2 m in front of Link (docs/coop/re-notes.md, coop_ActorMgr_requestCreateActor). Runs on the game thread that
// polls the gamepad (ov_input.c npad_states), whose Cpu rt_call needs. Test code: Windows keys only.

#include <stdint.h>
#include <string.h>

#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"

#define SCENE_SLOT 0x3add238      // [main+] -> +0x1e8 scene (runtime/frame_rate.c player_pos)
#define ACTOR_MGR_SLOT 0x395e008  // [[main+]] = the actor manager the AI node passes as x0
#define REQUEST_CREATE 0x599670   // coop_ActorMgr_requestCreateActor
#define CLAIM_ACTOR 0x1877160     // (handle, link out, 1, 0): what the AI node calls once the handle reads 3
#define SET_DEPENDENCY 0x5b811c   // (actor system, parent, child): queues a parent -> child update dependency
#define ACTOR_SYSTEM_SLOT 0x3ad6d80   // [main+] = the actor system SET_DEPENDENCY takes
#define EDGE_TABLE_SLOT 0x395da68     // [[main+]] +0x2f0 count, +0x2f8 edges {parent, child} (16 bytes each)
#define ACTOR_NAME 0x218
#define ACTOR_POS 0x2b4
#define ACTOR_ROT 0x2c0

static const char *const names[3] = {"Item_Fruit_A", "Npc_Kokiri007", "Player"};
// The handle the manager fills when the actor exists; it keeps the address, so it outlives the call.
static _Alignas(16) uint8_t handles[3][0x40];
static _Alignas(16) uint8_t links[3][0x40];   // the actor link the claim fills (the node keeps it at node+0x28)
static int last_status[3] = {-1, -1, -1};
static int dump_ticks;   // resident-list dumps left after the Player claim, one per poll

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
        uint64_t ok = guest_call(c, MOD_MAIN_BASE + SET_DEPENDENCY, sys, parent, child, 0);
        rt_log("coop:   edge %u: %s -> %s copied as %s -> %s: %s", idx, actor_name(e[0], a, 40),
               actor_name(e[1], b, 40), parent == copy ? "Player#2" : a, child == copy ? "Player#2" : b,
               ok & 1 ? "queued" : "refused");
    }
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

void coop_tick(struct Cpu *c) {
    static int prev;
    int keys = input_coop_keys(), pressed = keys & ~prev;
    prev = keys;
    if (dump_ticks) { dump_ticks--; dump_resident("a poll after the Player claim"); }
    for (int k = 0; k < 3; k++) {
        if (pressed & 1 << k) spawn(c, k);
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
                dump_ticks = 5;
            }
        }
    }
}
