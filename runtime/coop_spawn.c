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
#include "coop_chars.h"
#include "coop_game.h"
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

static const char *names[2] = {"Item_Fruit_A", "Npc_Kokiri007"};   // [1]: TOTK_COOP_TEST_SPAWN can change it
// The handle the manager fills when the actor exists; it keeps the address, so it outlives the call.
static _Alignas(16) uint8_t handles[2][0x40];
static _Alignas(16) uint8_t links[2][0x40];
static uint64_t foe;   // the actor TOTK_COOP_TEST_SPAWN made (slot 1), for foe_log   // the actor link the claim fills (the node keeps it at node+0x28)
static int last_status[2] = {-1, -1};
int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
void coop_watch_disarm(void);
static uint64_t player_actor_cached;    // this PC's Link, refreshed by coop_tick for the calc wrapper
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







int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c
int coop_player_places(float (*pos)[3], float (*rot)[9], int *ids, uint64_t *actors, int max);   // coop_players.c
// Test (TOTK_COOP_TEST_FOELOG=1): every second, the enemy TOTK_COOP_TEST_SPAWN made: its awareness words (component
// +0x158, the words coop_world.c AWARE_OFF sends: 0xc182 unaware, 0xc082 aware in the first), its distance to this
// PC's Link and to the first other player's stand-in, and the life of both (a stand-in fought by it loses life
// only in its owner's game: contacts_filter), so a test can tell whom it noticed and went for.
static void foe_log(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_FOELOG") != NULL;
    static uint64_t last_us;
    int64_t us = frame_step_world_us();
    if (!on || !foe || us < 0 || (uint64_t)us < last_us + 1000000) return;
    last_us = (uint64_t)us;
    static const uint16_t off[6] = {0x24, 0x58, 0x60, 0x64, 0x70, 0x84};
    uint32_t w[6] = {0};
    uint64_t aw = rd_ptr(rd_ptr(foe + 0x228) + 0x158);
    for (int i = 0; aw && i < 6; i++) rd(aw + off[i], &w[i], 4);
    float fp[3], mp[3], ps[COOP_MAX_PLAYERS][3], rs[COOP_MAX_PLAYERS][9];
    uint64_t me = player_actor(), acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_places(ps, rs, ids, acts, COOP_MAX_PLAYERS);
    const float *sp = ps[0];   // the owner's reported place (the stand-in's actor position stays where it was made)
    if (!rd(foe + ACTOR_POS, fp, 12)) { rt_log("coop: test: foe gone"); foe = 0; return; }
    float dme = -1, dst = -1;
    if (me && rd(me + ACTOR_POS, mp, 12)) dme = sqrtf((fp[0] - mp[0]) * (fp[0] - mp[0]) + (fp[1] - mp[1]) * (fp[1] - mp[1]) + (fp[2] - mp[2]) * (fp[2] - mp[2]));
    if (n > 0) dst = sqrtf((fp[0] - sp[0]) * (fp[0] - sp[0]) + (fp[1] - sp[1]) * (fp[1] - sp[1]) + (fp[2] - sp[2]) * (fp[2] - sp[2]));
    char nm[48];
    float ap[3] = {0};   // the stand-in's own actor position: not its owner's place in the first test (enemies may sense it there)
    if (n > 0 && acts[0]) rd(acts[0] + ACTOR_POS, ap, 12);
    rt_log("coop: test: foe %s at %.1f %.1f %.1f (world %.0f s): awareness %x %x %x %x %x %x; %.1f m from this Link, "
           "%.1f m from player %d's stand-in (its actor position %.1f %.1f %.1f)", actor_name(foe, nm, sizeof nm), fp[0], fp[1],
           fp[2], us / 1e6, w[0], w[1], w[2], w[3], w[4], w[5], dme, dst, n > 0 ? ids[0] : 0, ap[0], ap[1], ap[2]);
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
    {   // TOTK_COOP_TEST_SPAWN_AT=x,y,z: at that world place instead (the same enemy in two games: its key is its place);
        // x,y,z,1: and facing the other way from the Link (towards it, when placed in front of it).
        // =standin: 3 m in front of the first other player's stand-in instead, facing it (an enemy next to that
        // player only, in the game that does not own them; tools/coop/standin_foe_3rd.txt). =self: 3 m in front of
        // this PC's Link, facing it: the same place against this game's own player, the control for =standin
        const char *e = k == 1 ? cfg_get("TOTK_COOP_TEST_SPAWN_AT") : NULL;
        float q[3];
        int back = 0;
        if (e && !strcmp(e, "standin")) {
            uint64_t acts[COOP_MAX_PLAYERS];
            int ids[COOP_MAX_PLAYERS];
            float ps[COOP_MAX_PLAYERS][3], rs[COOP_MAX_PLAYERS][9], ap[3] = {0};
            if (coop_player_places(ps, rs, ids, acts, COOP_MAX_PLAYERS) < 1) {
                rt_log("coop: spawn %s: no stand-in to put it beside", names[k]);
                return;
            }
            const float *sp = ps[0], *sr = rs[0];   // the owner's place: the stand-in's actor position is not it
            if (acts[0]) rd(acts[0] + ACTOR_POS, ap, 12);
            rt_log("coop: spawn: the stand-in's actor position reads %.1f %.1f %.1f", ap[0], ap[1], ap[2]);
            memcpy(rot, sr, sizeof rot);
            at[0] = sp[0] + 3 * sr[2], at[1] = sp[1] + 0.5f, at[2] = sp[2] + 3 * sr[8];
            back = 1;
            rt_log("coop: spawn %s 3 m in front of player %d's stand-in at %.1f %.1f %.1f", names[k], ids[0], sp[0], sp[1], sp[2]);
        } else if (e && !strcmp(e, "self")) {
            at[0] = pos[0] + 3 * rot[2], at[1] = pos[1] + 0.5f, at[2] = pos[2] + 3 * rot[8];
            back = 1;
            rt_log("coop: spawn %s 3 m in front of this Link at %.1f %.1f %.1f", names[k], pos[0], pos[1], pos[2]);
        } else if (e && sscanf(e, "%f,%f,%f,%d", &q[0], &q[1], &q[2], &back) >= 3) memcpy(at, q, sizeof at);
        if (back == 1)
            for (int r = 0; r < 3; r++) rot[r * 3 + 0] = -rot[r * 3 + 0], rot[r * 3 + 2] = -rot[r * 3 + 2];
    }

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
void coop_trace_tick(uint64_t me);   // coop_trace.c
void coop_actor_seen(uint64_t actor);   // coop_world.c
uint64_t coop_nearest_link_data(uint64_t enemy);   // coop_world.c: a co-op Link's perception data when nearer, else 0
static int prof3;
// TOTK_COOP_PROF=4: inside an enemy's calc (enemy_calc), the functions entered with x0 = this PC's Link or its reference
// object, with their call site: where enemy AI uses the player it fetched.
static int prof4;
static int prof7;   // TOTK_COOP_PROF=7, below coop_fn_hit
// TOTK_COOP_PROF=L: every function entered inside a co-op Link's life update (the life hook, main+0x4400b0), all of
// them reported: for finding what shows its life on this game's hearts (they filled and emptied at each of its
// updates, the tester's capture, 2026-11-06).
static int profL;
static __thread int in_coop_life;
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
        if (profL) {
            rt_log("coop: profL %6u x %s, x0 %llx%s%s, from %s", atomic_load(&p3[i].hits), ldr_describe(fn, d, sizeof d),
                   (unsigned long long)p3[i].x0, global ? " held by a global " : "", where, ldr_describe(p3[i].lr, e, sizeof e));
        } else if (global) {
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
    if (profL) { if (in_coop_life) p3_hit(addr, lr, x0); return; }
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
}


// ---- entry hooks (cpu.h COOP_HOOKED)
#define GET_CONTROLLER 0x37ccc0
#define INPUT_COPY 0x37d330   // (input object, controller): the per-frame update (main+0x37c944) fills its consumers
// getController's callers: return address, index, calls since the last report. Filled from any game thread.
static struct { _Atomic uint64_t lr; _Atomic uint32_t index, calls; } gc_callers[64];

#define ACTOR_CALC 0x41bb20   // per-actor update from the actor job (main+0x2f175c), x0 = actor (profiler, 2026-11-03)
static __thread int in_calc;      // this thread runs ACTOR_CALC through the wrapper
static uint8_t neutral[0x200];    // the input object while nothing is held: coop_tick takes it 3 s into the world
static _Atomic int neutral_ok;

int coop_players_calc(Cpu *c, uint64_t addr);    // coop_players.c
void coop_ctl_enter(void);                       // the controller lock (below)
void coop_ctl_leave(void);
int coop_puppet_place(Cpu *c, uint64_t addr);   // coop_players.c: a puppet put at its owner after its physics step
void coop_players_tick(Cpu *c);
uint64_t coop_players_first(void);
int coop_players_gear(Cpu *c, uint64_t addr);

int coop_watch_counts(void) { return 1; }   // (coop_watch_win.c: every fault counts)


static int calc_wrap(Cpu *c, uint64_t addr) {
    uint64_t actor = c->x[0];
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
    return coop_players_calc(c, addr);   // a stand-in (coop_players.c): run with an empty controller
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
    // Shared world progress (coop_gamedata.c, TOTK_COOP_SHARE_WORLD=1): GameDataMgr::calc (main+0xa72b00, x0 the
    // manager) and setEventBool (main+0xc6a9f0).
    if (addr == MOD_MAIN_BASE + 0xa72b00) { void coop_gamedata_calc(uint64_t gmd, uint64_t me); coop_gamedata_calc(c->x[0], player_actor_cached); return 0; }
    if (addr == MOD_MAIN_BASE + 0xc6a9f0) { int coop_gamedata_event_bool(Cpu *c, uint64_t addr); return coop_gamedata_event_bool(c, addr); }
    // Ultrahand glue (coop_glue.c): doScotch main+0x1d66e34 (a bond made: captured), a bond's break main+0x1c0bb54
    // Autobuild research (coop_autobuild.c, TOTK_COOP_TEST_ABLOG): the autobuild device class's constructor
    // main+0x6898dc, createScotch main+0x1c0b94c, the glue step main+0x1c0ba8c and its end main+0x1d61a6c, and the
    // component call that gives a device its contraption's group, main+0x1d93978.
    if (addr == MOD_MAIN_BASE + 0x6898dc || addr == MOD_MAIN_BASE + 0x1c0b94c || addr == MOD_MAIN_BASE + 0x1d61a6c || addr == MOD_MAIN_BASE + 0x1c0ba8c ||
        addr == MOD_MAIN_BASE + 0x1d93978) { int coop_autobuild_hook(Cpu *c, uint64_t addr); return coop_autobuild_hook(c, addr); }
    if (addr == MOD_MAIN_BASE + 0x1d66e34 || addr == MOD_MAIN_BASE + 0x1c0bb54) { int coop_glue_hook(Cpu *c, uint64_t addr); return coop_glue_hook(c, addr); }
    // rotating ground (coop_world.c): the Rotate logic and AI nodes' updates and enters, their turn helper, the turn
    // set, and the blackboard byte read the logic node takes its switch from
    if (addr == MOD_MAIN_BASE + 0x1cf0260 || addr == MOD_MAIN_BASE + 0x1cf0160 || addr == MOD_MAIN_BASE + 0x2329170 || addr == MOD_MAIN_BASE + 0x2329078 ||
        addr == MOD_MAIN_BASE + 0x298a20c || addr == MOD_MAIN_BASE + 0x2988330 || addr == MOD_MAIN_BASE + 0x1b13c9c) {
        int coop_ground_hook(Cpu *c, uint64_t off);
        return coop_ground_hook(c, addr - MOD_MAIN_BASE);
    }
    // Test (TOTK_COOP_TEST_HELDCALL=1): who calls main+0x45a460 (a held item's component placing it in its holder's
    // hand; x1 points to the holder actor) and for which holder: the stand-in's sword reached its hand only while this
    // game's Link had a weapon (the tester, 2026-11-07). Each caller and holder once, at most 40 lines.
    if (addr == MOD_MAIN_BASE + 0x45a460) {
        // A held item's component placing it (x0 the component, x1 points to its holder). Before it reads the holder's
        // in-hand byte (EquipmentUser +0x4620) it follows eq+0x1620, an object every EquipmentUser shares, and with that
        // object's +0x2cc set asks game data (key 0x90b81999, by slot) for this game's player's own item in that slot;
        // with none it never reads the byte, and a PlayerPuppet stand-in's sword stayed on its back in its owner's
        // swings while this game's Link had no weapon (the tester, 2026-11-07: equipped, the stand-in held it). For a
        // stand-in's item the call runs with the stand-in's eq+0x1620 on a copy of that object with +0x2f4 bit 0 set
        // (its chain ends there) and +0x2cc clear (no game data asked), so it goes to the byte. A copy, not 0: another
        // thread read the field during the call and crashed on 0 (main+0xc9ad84, 2026-11-07). TOTK_COOP_STANDIN_HELDFIX=0:
        // not.
        static int fix = -1;
        if (fix < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_HELDFIX"); fix = !(e && *e == '0'); }
        static __thread int inside_held;
        if (fix && !inside_held && c->x[1]) {
            uint64_t holder = coop_rd_ptr(c->x[1]);
            char hn[16] = "";
            if (holder && coop_char_is_standin(coop_actor_name(holder, hn, sizeof hn))) {
                uint64_t eq = coop_rd_ptr(coop_rd_ptr(holder + 0x228) + 0x230);
                uint64_t saved = eq ? coop_rd_ptr(eq + 0x1620) : 0;
                static __thread _Alignas(16) uint8_t copy[0x800];
                if (saved && coop_rd(saved, copy, sizeof copy)) {
                    copy[0x2f4] |= 1, copy[0x2cc] = 0;
                    ST64(eq + 0x1620, (uint64_t)(uintptr_t)copy);
                    inside_held = 1;
                    c->pc = addr;
                    rt_dispatch(c);
                    inside_held = 0;
                    ST64(eq + 0x1620, saved);
                    return 1;
                }
            }
        }
        static int on = -1;
        if (on < 0) on = cfg_get("TOTK_COOP_TEST_HELDCALL") != NULL;
        if (on) {
            static struct { uint64_t lr, holder; } seen[40];
            static int n;
            uint64_t lr = c->x[30], holder = c->x[1] ? coop_rd_ptr(c->x[1]) : 0;
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= seen[i].lr == lr && seen[i].holder == holder;
            if (!dup && n < 40) {
                seen[n].lr = lr, seen[n].holder = holder, n++;
                char hn[48] = "", wn[48] = "";
                coop_actor_name(holder, hn, sizeof hn), coop_actor_name(coop_rd_ptr(c->x[0] + 0x18), wn, sizeof wn);
                rt_log("coop: test: held call from main+%#llx: holder %llx '%s', item '%s', world %.1f s", (unsigned long long)(lr - MOD_MAIN_BASE),
                       (unsigned long long)holder, hn, wn, frame_step_world_us() / 1e6);
            }
        }
        if (on) {   // the values its branches read, for a PlayerPuppet holder's item (first 6 calls)
            static int k;
            uint64_t holder = c->x[1] ? coop_rd_ptr(c->x[1]) : 0;
            char hn[48] = "";
            coop_actor_name(holder, hn, sizeof hn);
            if (coop_char_is_standin(hn) && k++ < 6) {
                uint64_t comp = c->x[0], wa = coop_rd_ptr(comp + 0x18), eq = coop_rd_ptr(coop_rd_ptr(holder + 0x228) + 0x230);
                uint32_t af = 0, row = 0, rf = 0;
                uint8_t b505 = 0, b516 = 0, b1456 = 0;
                uint64_t f4600 = 0, owner = eq ? coop_rd_ptr(eq + 0x18) : 0, oc = owner ? coop_rd_ptr(coop_rd_ptr(owner + 0x228) + 0x3a8) : 0;
                if (wa) coop_rd(wa + 0x28, &af, 4);
                coop_rd(comp + 0x488, &row, 4), coop_rd(comp + 0x505, &b505, 1), coop_rd(comp + 0x516, &b516, 1);
                if (eq) coop_rd(eq + 0x570 + (uint64_t)(row < 8 ? row : 0) * 0x100 + 0xf0, &rf, 4), coop_rd(eq + 0x4600, &f4600, 8);
                if (oc) coop_rd(oc + 0x1456, &b1456, 1);
                rt_log("coop: test: held call state: actor +0x28 %#x, row %u, record +0xf0 %#x, comp +0x505 %u +0x516 %#x, eq +0x4600 %#llx, owner %llx comp+0x3a8 %llx +0x1456 %#x",
                       af, row, rf, b505, b516, (unsigned long long)f4600, (unsigned long long)owner, (unsigned long long)oc, b1456);
            }
        }
        return 0;
    }
    // the SwordBlur component's update (coop_world.c): a stand-in's runs with its owner's WeaponBlur event
    if (addr == MOD_MAIN_BASE + 0x275954) { int coop_blur_hook(Cpu *c, uint64_t addr); return coop_blur_hook(c, addr); }
    if (addr == MOD_MAIN_BASE + 0x31dbc0) { void coop_hit_delivered(Cpu *c); coop_hit_delivered(c); return 0; }   // coop_world.c
    if (addr == MOD_MAIN_BASE + 0x300a30) { void coop_hit_update(Cpu *c); coop_hit_update(c); return 0; }        // coop_world.c
    if (addr == MOD_MAIN_BASE + 0x50ad74) { void coop_enemy_action(Cpu *c); coop_enemy_action(c); return 0; }    // coop_world.c
    if (addr == MOD_MAIN_BASE + 0x96c1ac) { void coop_markers_screen(uint64_t); coop_markers_screen(c->x[0]); return 0; }   // coop_markers.c: the AppMap screen's set-up
    // main+0x1d1fc70 (x0 an actor link): the 1.4.3 copy of the function TotK Online makes return 1 for PlayerPuppet
    // (1.2.1 main+0x1736dec; the same code, matched by hand 2026-11-07; read for behaviour only). It is bow code, not
    // a target check: its one caller is main+0x1d1f9e8, in the function that loads "IsAdditionalArrowBow", and it
    // looks the actor's name up in a table ([main+0x39601e8], main+0xd62b58). So a sword fight never calls it;
    // enemies find the stand-in through its perception emitter (tools/coop/make_puppet_pack.py EMITTER).
    if (addr == MOD_MAIN_BASE + 0x1d1fc70) {
        uint64_t ref = rd_ptr(c->x[0] + 8), a = ref ? rd_ptr(ref + 0x40) : 0;
        uint64_t q = c->x[1] ? rd_ptr(c->x[1] + 0x18) : 0, b = q ? rd_ptr(q + 0x18) : 0;
        char n1[16] = "", n2[16] = "";
        if (a) actor_name(a, n1, sizeof n1);
        if (b) actor_name(b, n2, sizeof n2);
        static int tst = -1;   // TOTK_COOP_TEST_ISPLAYER=1: the first 30 calls' two names
        if (tst < 0) tst = cfg_get("TOTK_COOP_TEST_ISPLAYER") != NULL;
        static _Atomic int said_t;
        if (tst && atomic_fetch_add(&said_t, 1) < 30)
            rt_log("coop: test: is-player check from main+%#llx: link's actor '%s', query's '%s'", (unsigned long long)(c->x[30] - MOD_MAIN_BASE), n1, n2);
        if (!coop_char_is_standin(n1) && !coop_char_is_standin(n2)) return 0;
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 5)
            rt_log("coop: stand-in: the game asks whether it is a player (from main+%#llx; %s): yes", (unsigned long long)(c->x[30] - MOD_MAIN_BASE),
                   coop_char_is_standin(n1) ? "the link" : "the query");
        c->x[0] = 1;
        return 1;
    }
    // ActorCreateInfo::getActor (main+0x50d6bc) from the create queue (returning to main+0x898420): an actor the game
    // hands over; run here, then its result goes to the spawn sync (coop_world.c coop_actor_created)
    if (addr == MOD_MAIN_BASE + 0x50d6bc) {   // Test (TOTK_COOP_TEST_GETACTOR=1): which call sites run, every 1000 calls
        static int on = -1;
        if (on < 0) on = cfg_get("TOTK_COOP_TEST_GETACTOR") != NULL;
        static struct { _Atomic uint64_t lr; _Atomic uint32_t n; } site[32];
        static _Atomic uint32_t total;
        for (int i = 0; on && i < 32; i++) {
            uint64_t have = atomic_load(&site[i].lr), lr = c->x[30];
            if (have == lr || (!have && atomic_compare_exchange_strong(&site[i].lr, &have, lr))) { atomic_fetch_add(&site[i].n, 1); break; }
        }
        if (on && (atomic_fetch_add(&total, 1) + 1) % 50 == 1)
            for (int i = 0; i < 32 && atomic_load(&site[i].lr); i++)
                rt_log("coop: test: getActor from main+%#llx: %u calls", (unsigned long long)(atomic_load(&site[i].lr) - MOD_MAIN_BASE), atomic_load(&site[i].n));
    }
    // (the create queues that hand actors over this way in 1.4.3, each "status 3, getActor(handle, 0, 0), handle
    // cleared" as 1.2.1's main+0xcd7d40: main+0x50d364 (the async create manager's, most of them), 0x61a8b4,
    // 0x6609b4, 0x89841c; TOTK_COOP_TEST_GETACTOR counted the sites, 2026-11-07)
    uint64_t qlr = c->x[30] - MOD_MAIN_BASE;
    if (addr == MOD_MAIN_BASE + 0x50d6bc && (qlr == 0x50d368 || qlr == 0x61a8b8 || qlr == 0x6609b8 || qlr == 0x898420)) {
        static __thread int inside;
        if (inside) return 0;
        inside = 1;
        c->pc = addr;
        rt_dispatch(c);
        inside = 0;
        void coop_actor_created(uint64_t a);
        coop_actor_created(c->x[0]);
        return 1;
    }
    if (addr == MOD_MAIN_BASE + 0x599670) return create_trace(c);
    // HUD control guide (bottom-right prompts such as "Dive R"): main+0x2192c50 (type) shows one, main+0x4cb540 (type)
    // hides one, on the one CtrlGuide screen. Every Player's calc calls them; a co-op Link's must not change this
    // PC's HUD, so they do nothing inside one (2026-11-04).
    // An actor's other per-frame update phases besides ACTOR_CALC (its vtable holds main+0x41bb20, main+0x245030,
    // main+0x194fd7c; the actor job also runs main+0x2f9910): a co-op Link's run as its calc (TOTK_COOP_PROF=7 found
    // them: its ragdoll and tips ran there, outside the calc flag) (2026-11-04).
    if (addr == MOD_MAIN_BASE + 0x245030 || addr == MOD_MAIN_BASE + 0x2f9910 || addr == MOD_MAIN_BASE + 0x194fd7c) {
        int coop_enemy_puppet_phase(Cpu *c, uint64_t addr);   // coop_world.c: an enemy puppet's phases (driven, or skipped)
        if (coop_enemy_puppet_phase(c, addr)) return 1;
        return coop_players_phase(c, addr);
    }
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
    // An enemy puppet's skeleton (coop_world.c): its local matrices are the owner's pose, written by its drive; built
    // here from its own frozen animation as well, the two took turns and the copy jiggled (a play session; held on
    // one pose, a Chuchu's copy moved 2 mm a frame where the Chuchu moved 0.1 mm, 2026-11-06).
    if (addr == MOD_MAIN_BASE + 0x283c750) {
        int coop_enemy_puppet_skel(uint64_t skel, uint64_t lr);   // coop_world.c
        if (coop_enemy_puppet_skel(c->x[1], c->x[30])) return 1;
    }
    if (addr == MOD_MAIN_BASE + 0x289620 || addr == MOD_MAIN_BASE + 0x2925e0 || addr == MOD_MAIN_BASE + 0x43e450 || addr == MOD_MAIN_BASE + 0x2e9c80) {
        int coop_enemy_puppet_writer(uint64_t addr, const uint64_t *x, uint64_t lr);   // coop_world.c: test
        if (coop_enemy_puppet_writer(addr, c->x, c->x[30])) return 1;
    }
    // Puppet mode: main+0x283c750 (x0 the input pose, x1 the SkeletonObj) builds a body's local matrices. This
    // Link's input goes out; a co-op Link's is replaced by its player's before it runs (2026-11-05).
    if (addr == MOD_MAIN_BASE + 0x283c750) {
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
    if (addr == MOD_MAIN_BASE + 0x2192c50 && !coop_in_remote_calc()) {
        // Test (TOTK_COOP_TEST_GUIDELOG=1): each distinct HUD prompt this Link's game shows (x1 its type) and the
        // call chain that shows it, 30 of them: what decides the Sneakstrike prompt (an enemy not aware of Link).
        static int on = -1;
        static uint64_t seen[30];
        static _Atomic int n;
        if (on < 0) on = cfg_get("TOTK_COOP_TEST_GUIDELOG") != NULL;
        uint64_t key = (c->x[1] & 0xffff) << 32 | ((c->x[30] - MOD_MAIN_BASE) & 0xffffffff);
        int had = 0;
        for (int i = 0; on && i < atomic_load(&n) && i < 30; i++) had |= seen[i] == key;
        if (on && !had && atomic_load(&n) < 30) {
            seen[atomic_fetch_add(&n, 1)] = key;
            char chain[300];
            int m = 0;
            uint64_t fp = c->x[29];
            for (int d = 0; d < 8 && fp && m < 280; d++) {
                uint64_t next = 0, ret = 0;
                if (!plat_mem_read(fp, &next, 8) || !plat_mem_read(fp + 8, &ret, 8) || !ret) break;
                m += snprintf(chain + m, sizeof chain - m, " %llx", (unsigned long long)(ret - MOD_MAIN_BASE));
                if (next <= fp) break;
                fp = next;
            }
            chain[m] = 0;
            rt_log("coop: test: guide prompt x0 %llx x1 %#llx x2 %#llx at %.1f s; lr main+%llx, frames%s", (unsigned long long)c->x[0],
                   (unsigned long long)c->x[1], (unsigned long long)c->x[2], frame_step_world_us() / 1e6,
                   (unsigned long long)(c->x[30] - MOD_MAIN_BASE), chain);
        }
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
        // Its life update does not run at all (TOTK_COOP_LIFT=lift or none: as before, below). Any change it made to
        // the life, the lift's clamp to most life or a quarter heart, showed on this game's hearts as a fill (the tester's
        // capture and session, 2026-11-06); its life is its owner's, set from the owner's reports.
        static int run_it = -1;
        if (run_it < 0) { const char *e = cfg_get("TOTK_COOP_LIFT"); run_it = e && (!strcmp(e, "lift") || !strcmp(e, "none")); }
        if (!run_it) return 1;
        // TOTK_COOP_LIFT=none: the update runs on the life as it is, and a drop is put back after (a lethal hit inside
        // one update can then kill it here). Lifted (the default), this game's hearts filled and emptied at each of
        // its updates (each of its owner's swings; the tester's capture, 2026-11-06): tried to tell which.
        static int nolift = -1;
        if (nolift < 0) { const char *e = cfg_get("TOTK_COOP_LIFT"); nolift = e && !strcmp(e, "none"); }
        if (!nolift) ST32(p + 8, 0x3fffffffu);
        in = 1, in_coop_life = 1;
        c->pc = addr;
        rt_dispatch(c);
        in = 0, in_coop_life = 0;
        uint32_t after = LD32(p + 8);
        if (after < 0x3fffffffu && after > 0x3fff0000u) {   // damage taken off the lifted value (not the clamp to max)
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 8) rt_log("coop: a co-op Link's damage %u not taken here", 0x3fffffffu - after);
        }
        // Put back: life lifted for the call and taken off it is not taken; healed above it, the heal is kept; any
        // other value (the clamp of the lifted value to most life, every update) is the lift's, not the game's, and is
        // put back. (Keeping a value set outright, unless it was the most life, kept the clamp: the most life could not
        // be read in here, and the proxy went to full life in each update it ran; the sync audit, 2026-11-06.)
        if (nolift) ST32(p + 8, after < before ? before : after);
        else ST32(p + 8, after > 0x3fffffffu ? before + (after - 0x3fffffffu) : before);
        static int lifelog = -1;   // TOTK_COOP_TEST_LIFELOG: each co-op Link life update that changed the lifted value
        if (lifelog < 0) lifelog = cfg_get("TOTK_COOP_TEST_LIFELOG") != NULL;
        static _Atomic int said_lu;
        if (lifelog && after != 0x3fffffffu && atomic_fetch_add(&said_lu, 1) < 40)
            rt_log("coop: test: a co-op Link's life update: %u before, %#x after", before, after);
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
        int coop_enemy_puppet_calc(uint64_t actor);   // coop_world.c: an enemy another game runs, frozen here
        if (coop_enemy_puppet_calc(c->x[0])) return 1;
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
    if (addr == MOD_MAIN_BASE + 0x257310 && coop_puppet_place(c, addr)) return 1;   // coop_players.c: puppet mode
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
    static int prof_phase;
    if (!prof_phase) {
        const char *e = cfg_get("TOTK_COOP_PROF");
        prof_phase = 1;
        if (e && *e == '3') prof3 = 1, g_fnprof = 1;
        if (e && *e == 'L') prof3 = profL = 1, g_fnprof = 1;
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
    // (from 3.5 s before the hit only: on from the world's start, it slowed the game too much for a scripted autobuild)
    if (fanhit_at > 0 && frame_step_world_us() / 1e6 > fanhit_at - 3.5) g_fnprof = 1;
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
    coop_players_tick(c);
    foe_log();
    for (int k = 0; k < 2; k++) {
        // Test: TOTK_COOP_TEST_SPAWN=<actor>@<s> spawns that actor in slot 1 (Numpad 2's) that many seconds into the world.
        static int test_spawned;
        const char *ts = cfg_get("TOTK_COOP_TEST_SPAWN");
        const char *at = ts ? strchr(ts, '@') : NULL;
        // name@t spawns one actor at t world seconds; name1+name2+...@t one after the other, 4 s apart (a test of
        // several actors in one run: the character sets, tools/coop/charmod.py).
        static int test_next;
        if (k == 1 && at && !test_spawned && frame_step_world_us() / 1e6 >= atof(at + 1) + 4.0 * test_next) {
            static char nm[64];
            const char *p = ts;
            for (int i = 0; i < test_next && p && p < at; i++) p = strchr(p, '+'), p = p && p < at ? p + 1 : NULL;
            if (!p || p >= at) test_spawned = 1;
            else {
                const char *e = strchr(p, '+');
                if (!e || e > at) e = at;
                snprintf(nm, sizeof nm, "%.*s", (int)(e - p), p);
                names[1] = nm, test_next++;
                last_status[1] = -1;
                rt_log("coop: test spawn %d: %s", test_next, nm);
                spawn(c, 1);
                if (e == at) test_spawned = 1;
            }
        }
        if (pressed & 1 << k) spawn(c, k);
        if (!rd_ptr((uint64_t)(uintptr_t)handles[k])) continue;
        int st = handle_status(k);
        if (st != last_status[k]) rt_log("coop: %s handle status %d", names[k], st);
        last_status[k] = st;
        if (st == 3) {
            memset(links[k], 0, sizeof links[k]);
            uint64_t got = guest_call(c, MOD_MAIN_BASE + CLAIM_ACTOR, (uint64_t)(uintptr_t)handles[k],
                                      (uint64_t)(uintptr_t)links[k], 1, 0);
            uint64_t l[2], name = 0;
            memcpy(l, links[k], sizeof l);
            char text[32] = "";
            if (l[0] && (name = rd_ptr(l[0] + ACTOR_NAME))) rd(name, text, sizeof text - 1);
            rt_log("coop: %s claimed: %s; link %016llx %016llx (name at link[0] '%s')", names[k],
                   got & 1 ? "ok" : "failed", (unsigned long long)l[0], (unsigned long long)l[1], text);
            if (k == 1 && (got & 1) && l[1]) foe = rd_ptr(l[1] + 0x40);
        }
    }
}
