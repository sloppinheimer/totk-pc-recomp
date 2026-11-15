// Co-op: the enemy sync's hooks and its first two test runs (docs/coop/enemy-sync-design.md sections 4 and 14).
//
// coop_hooks.h COOP_HOOKED_EN registers every function entry the enemy sync will use; they all arrive in
// coop_enemy_hook. Until the sync is built this file only holds the tests that settle what the readings left open, and
// with no test asked for every hook returns at once.
//
// TOTK_COOP_TEST_ENPROBE=1 (run 1): which code runs which behaviour graph of one enemy. The first enemy whose name
// starts with TOTK_COOP_TEST_ENPROBE_NAME (default Enemy_Bokoblin) within 40 m of this Link is the target. Its graph
// controllers are written down once (the nine units of its BSA, the units of its scheduler, the record the actor's calc
// runs inline, the move-control controller), and then every entry of the four runner bodies, of the node tick, of a
// node's enter and of the scheduler that belongs to the target is counted by (hook, controller, caller). The log says
// once a second which controller was run from where, and how often per frame:
//   coop: enprobe: target ...            the target and its controllers, each with the actor its +0x1f8 names
//   coop: enprobe: <hook> ctrl <label> from main+<caller>: <n> in <frames> frames
//   coop: enprobe: state ...             once a second: where the target is, how far from this Link, its life, brain
//       verb and the command of its first animation slot, and how often its calc and this Link's ran (the frame rate)
// TOTK_COOP_TEST_ENPROBE=2 (run 2): the same, and from 4 s to 34 s after the target was chosen the copy switch of
// section 4 is on for it (hooks H1 to H6 turn the runners away, the four writes empty the inline runner before the
// calc), then off again. The log shows what each hook turned away and whether anything still reached a node; the
// tester sees whether the enemy stands, keeps animating and takes up its behaviour again afterwards.
// TOTK_COOP_TEST_ENPROBE=3: as 2, and when the switch goes off the interrupt graph, which the four writes emptied, is
// given root 0 again ([ctrl+0x19c] = 0: the runner's own root switch then starts it).
// TOTK_COOP_TEST_ENPROBE=4 or 5 (run 3, design section 5.1): as 3, and while the switch is on the target is moved as a
// copy would be: its main body is made keyframed (4) or static (5) with main+0xae4300 and its pose is set before
// every calc (ForceSetMatrix) along a circle of 2 m radius at 1.5 m/s, facing along it. Every second:
//   coop: enprobe: pose: ...             how far the actor and its body were from the pose asked the frame before
//       (mean and worst of the second). At the end the body is made dynamic again.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

enum { H_TICK, H_50B404, H_42E0C8, H_2BEB80, H_SCHED, H_REQUEST, H_ENTER, H_N };
static const char *const h_name[H_N] = { "node tick", "runner 50b404", "runner 42e0c8", "runner 2beb80", "scheduler", "action request", "node enter" };

static int probe_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = cfg_get("TOTK_COOP_TEST_ENPROBE"); m = e && *e >= '1' && *e <= '5' ? *e - '0' : 0; }
    return m;
}

static _Atomic uint64_t target;        // the enemy under test
static _Atomic int switch_on;          // run 2: the copy switch is on for it
static atomic_flag tab_busy = ATOMIC_FLAG_INIT;
typedef struct { uint64_t ctrl, lr; uint32_t n, turned; uint8_t hook, used; } Row;
static Row rows[256];
static uint32_t frames, frames_at_dump;
static _Atomic uint32_t calc_t, calc_me;   // calcs of the target and of this Link since the last dump
static _Atomic uint64_t me_actor;
static uint64_t picked_ns, last_dump_ns;
// run 3 (the pose-driven body)
static float pose_p0[3], pose_last[3], pose_err_sum, pose_err_max, pose_berr_max;
static int pose_n, pose_started, pose_asked;
static uint64_t pose_t0;

typedef struct { uint64_t ctrl; char label[40]; } Known;
static Known known[96];
static int known_n;

static void know(uint64_t ctrl, const char *fmt, int i) {
    if (!ctrl || known_n >= 96) return;
    for (int k = 0; k < known_n; k++) if (known[k].ctrl == ctrl) { size_t l = strlen(known[k].label); snprintf(known[k].label + l, sizeof known[k].label - l, "="), l++; snprintf(known[k].label + l, sizeof known[k].label - l, fmt, i); return; }
    known[known_n].ctrl = ctrl;
    snprintf(known[known_n].label, sizeof known[known_n].label, fmt, i);
    known_n++;
}
static const char *label_of(uint64_t ctrl) {
    for (int k = 0; k < known_n; k++) if (known[k].ctrl == ctrl) return known[k].label;
    return "unknown";
}

// A graph unit's resource path, as far as it reads as text (the loader keeps it at unit+0x28; a pointer or inline).
static void unit_path(uint64_t unit, char *out, size_t n) {
    out[0] = 0;
    if (!unit) return;
    char b[64] = "";
    uint64_t p = coop_rd_ptr(unit + 0x28);
    if (p > 0x10000 && coop_rd(p, b, sizeof b - 1) && b[0] >= 0x20 && b[0] < 0x7f) {
        b[sizeof b - 1] = 0;
        for (char *q = b; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
        snprintf(out, n, "%s", b);
    }
}

static void pick(uint64_t actor, uint64_t sched) {
    uint64_t bsa = sched - 0x1a88, comps = coop_rd_ptr(actor + 0x228);
    char nm[64] = "";
    coop_actor_name(actor, nm, sizeof nm);
    rt_log("coop: enprobe: target %llx %s: BSA %llx (the actor's own BSA component %llx), custom scheduler byte %d",
           (unsigned long long)actor, nm, (unsigned long long)bsa, (unsigned long long)coop_rd_ptr(comps + 0x158),
           (int)(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(bsa + 0x25d8) + 0x150) + 0xa8) & 0xff));
    known_n = 0;
    static const char *const unit_name[9] = { "main action", "sub action", "SP", "SP2", "SP3", "brain", "utility", "utility 2", "post-sensor utility" };
    uint64_t holder = coop_rd_ptr(bsa + 0x1e0);
    uint32_t count = 0;
    coop_rd(holder, &count, 4);
    uint64_t arr = coop_rd_ptr(holder + 8);
    for (uint32_t k = 0; k < count && k < 9; k++) {
        uint64_t unit = coop_rd_ptr(arr + 8ull * k), ctrl = unit ? coop_rd_ptr(unit + 0x18) : 0;
        char path[64];
        unit_path(unit, path, sizeof path);
        rt_log("coop: enprobe: target: BSA unit %u (%s): unit %llx controller %llx, its actor %llx, path '%s'", k, unit_name[k],
               (unsigned long long)unit, (unsigned long long)ctrl, (unsigned long long)(ctrl ? coop_rd_ptr(ctrl + 0x1f8) : 0), path);
        know(ctrl, "bsa%d", (int)k);
    }
    uint32_t sn = 0;
    coop_rd(sched + 0xbf8, &sn, 4);
    uint64_t sarr = coop_rd_ptr(sched + 0xc00);
    for (uint32_t k = 0; k < sn && k < 80; k++) {
        uint64_t unit = coop_rd_ptr(sarr + 8ull * k), ctrl = unit ? coop_rd_ptr(unit + 0x18) : 0;
        char path[64];
        unit_path(unit, path, sizeof path);
        if (k < 4 || path[0]) rt_log("coop: enprobe: target: scheduler unit %u of %u: controller %llx, its actor %llx, path '%s'", k, sn,
               (unsigned long long)ctrl, (unsigned long long)(ctrl ? coop_rd_ptr(ctrl + 0x1f8) : 0), path);
        know(ctrl, "sched%d", (int)k);
    }
    uint64_t rec = coop_rd_ptr(bsa + 0x2618), rc = rec ? coop_rd_ptr(rec + 0x18) : 0, mc = coop_rd_ptr(bsa + 0x2620),
             cur = coop_rd_ptr(bsa + 0x2608), c48 = coop_rd_ptr(comps + 0x48), c90 = coop_rd_ptr(comps + 0x90);
    know(rc, "inline-record", 0);
    know(mc, "bsa+0x2620", 0);
    int32_t idx48 = -2;
    if (c48) coop_rd(c48 + 0x10, &idx48, 4);
    rt_log("coop: enprobe: target: the record the calc runs inline %llx (controller %llx = %s), [BSA+0x2620] %llx (%s), running action sequence record %llx, comps+0x48 %llx (first index %d), comps+0x90 %llx",
           (unsigned long long)rec, (unsigned long long)rc, label_of(rc), (unsigned long long)mc, label_of(mc), (unsigned long long)cur,
           (unsigned long long)c48, (int)idx48, (unsigned long long)c90);
    memset(rows, 0, sizeof rows);
    frames = frames_at_dump = 0;
    picked_ns = last_dump_ns = plat_now_ns();
    atomic_store(&me_actor, coop_player_actor());
    atomic_store(&target, actor);
}

static void count(int hook, uint64_t ctrl, uint64_t lr, int turned) {
    while (atomic_flag_test_and_set(&tab_busy)) { }
    Row *free_row = NULL, *r = NULL;
    for (int i = 0; i < 256; i++) {
        if (!rows[i].used) { if (!free_row) free_row = &rows[i]; continue; }
        if (rows[i].hook == hook && rows[i].ctrl == ctrl && rows[i].lr == lr) { r = &rows[i]; break; }
    }
    if (!r && free_row) r = free_row, r->used = 1, r->hook = (uint8_t)hook, r->ctrl = ctrl, r->lr = lr;
    if (r) r->n++, r->turned += (uint32_t)turned;
    atomic_flag_clear(&tab_busy);
}

static void dump(void) {
    uint64_t now = plat_now_ns();
    uint64_t every = now - picked_ns < 90000000000ull ? 1000000000ull : 10000000000ull;
    if (now - last_dump_ns < every) return;
    last_dump_ns = now;
    while (atomic_flag_test_and_set(&tab_busy)) { }
    uint32_t f = frames - frames_at_dump;
    frames_at_dump = frames;
    Row copy[256];
    memcpy(copy, rows, sizeof rows);
    for (int i = 0; i < 256; i++) rows[i].n = rows[i].turned = 0;
    atomic_flag_clear(&tab_busy);
    rt_log("coop: enprobe: %.0f s after the target was chosen, %u frames, the copy switch %s", (double)(now - picked_ns) / 1e9, f, atomic_load(&switch_on) ? "ON" : "off");
    {   // the target's state, read here (in its own update, at the scheduler's entry)
        uint64_t t = atomic_load(&target), me = atomic_load(&me_actor), comps = coop_rd_ptr(t + 0x228), bsa = coop_rd_ptr(comps + 0x158);
        float a[3] = {0}, b[3] = {0};
        coop_rd(t + COOP_ACTOR_POS, a, 12), coop_rd(me + COOP_ACTOR_POS, b, 12);
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        int32_t verb = -1, life = -1;
        coop_rd(bsa + 0x58, &verb, 4);
        coop_rd(coop_rd_ptr(comps + 0x2f8) + 0x1830, &life, 4);
        uint64_t g = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(comps + 0x40) + 0x18) + 0x18), s0 = g ? coop_rd_ptr(coop_rd_ptr(g + 0x20)) : 0;
        char cmd[40] = "";
        uint64_t cp = s0 ? coop_rd_ptr(s0 + 0x10) : 0;
        if (cp && coop_rd(cp, cmd, sizeof cmd - 1)) { cmd[sizeof cmd - 1] = 0; for (char *q = cmd; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; } }
        uint8_t st = 0xff;
        coop_rd(t + COOP_ACTOR_STATE, &st, 1);
        rt_log("coop: enprobe: state: at %.1f %.1f %.1f, %.1f m from this Link, life %d, brain verb 0x%x, slot 0 plays '%s', actor state %u; calcs: the target %u, this Link %u",
               a[0], a[1], a[2], sqrtf(dx * dx + dy * dy + dz * dz), (int)life, (unsigned)verb, cmd, st,
               atomic_exchange(&calc_t, 0), atomic_exchange(&calc_me, 0));
        if (pose_n) {
            rt_log("coop: enprobe: pose: %d frames: the actor was %.3f m (mean) and %.3f m (worst) from the pose asked a frame before, its body %.3f m (worst)",
                   pose_n, pose_err_sum / (float)pose_n, pose_err_max, pose_berr_max);
            pose_n = 0, pose_err_sum = pose_err_max = pose_berr_max = 0;
        }
    }
    for (int i = 0; i < 256; i++) {
        if (!copy[i].used || !copy[i].n) continue;
        rt_log("coop: enprobe: %s ctrl %llx (%s) from main+%llx: %u in %u frames, %u turned away", h_name[copy[i].hook],
               (unsigned long long)copy[i].ctrl, label_of(copy[i].ctrl), (unsigned long long)(copy[i].lr - MOD_MAIN_BASE), copy[i].n, f, copy[i].turned);
    }
}

// Run 3: the pose-driven body.
static void pose_drive(Cpu *c, uint64_t actor, int on) {
    uint64_t comps = coop_rd_ptr(actor + 0x228), phys = coop_rd_ptr(comps + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
    uint64_t x[32], sp = c->sp, pc = c->pc;
    memcpy(x, c->x, sizeof x);
    if (on && !pose_started) {
        pose_started = 1, pose_t0 = plat_now_ns(), pose_asked = 0;
        coop_rd(actor + COOP_ACTOR_POS, pose_p0, 12);
        uint64_t r = body ? coop_guest_call(c, MOD_MAIN_BASE + 0xae4300, body, probe_mode() == 5 ? 0 : 1, 0, 0) : 0;
        rt_log("coop: enprobe: pose: the target's main body %llx is made %s (the call answered %llx); the circle starts at %.2f %.2f %.2f",
               (unsigned long long)body, probe_mode() == 5 ? "static" : "keyframed", (unsigned long long)r, pose_p0[0], pose_p0[1], pose_p0[2]);
    }
    if (!on) {
        if (pose_started) {
            pose_started = 0;
            uint64_t r = body ? coop_guest_call(c, MOD_MAIN_BASE + 0xae4300, body, 2, 0, 0) : 0;
            rt_log("coop: enprobe: pose: the target's main body is made dynamic again (the call answered %llx)", (unsigned long long)r);
        }
        memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
        return;
    }
    if (pose_asked) {   // where the actor and its body are now, against what was asked before the last calc
        float a[3] = {0}, bt[12] = {0};
        coop_rd(actor + COOP_ACTOR_POS, a, 12);
        if (body) coop_rd(body + 0x98, bt, sizeof bt);
        float e = sqrtf((a[0] - pose_last[0]) * (a[0] - pose_last[0]) + (a[1] - pose_last[1]) * (a[1] - pose_last[1]) + (a[2] - pose_last[2]) * (a[2] - pose_last[2]));
        float be = sqrtf((bt[3] - pose_last[0]) * (bt[3] - pose_last[0]) + (bt[7] - pose_last[1]) * (bt[7] - pose_last[1]) + (bt[11] - pose_last[2]) * (bt[11] - pose_last[2]));
        pose_err_sum += e, pose_n++;
        if (e > pose_err_max) pose_err_max = e;
        if (be > pose_berr_max) pose_berr_max = be;
    }
    float w = 0.75f * (float)((double)(plat_now_ns() - pose_t0) / 1e9), fx = cosf(w), fz = sinf(w);
    _Alignas(16) float m[12] = { fz, 0, fx, pose_p0[0] + 2.f * sinf(w),  0, 1, 0, pose_p0[1],  -fx, 0, fz, pose_p0[2] + 2.f * (1.f - cosf(w)) };
    pose_last[0] = m[3], pose_last[1] = m[7], pose_last[2] = m[11], pose_asked = 1;
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m, 0, 0);
    memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
}

// Section 4's four writes: the runner the actor's calc holds inline finds nothing to run.
static void empty_inline(uint64_t actor) {
    uint64_t bsa = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x158), rec = bsa ? coop_rd_ptr(bsa + 0x2618) : 0;
    uint64_t ctrl = rec ? coop_rd_ptr(rec + 0x18) : 0, inst = ctrl ? coop_rd_ptr(coop_rd_ptr(ctrl + 0x20)) : 0;
    if (!ctrl || !inst) return;
    int32_t m1 = -1;
    int16_t s1 = -1;
    uint32_t zero = 0;
    uint16_t w = 0;
    memcpy((void *)(uintptr_t)(ctrl + 0x19c), &m1, 4);
    memcpy((void *)(uintptr_t)(inst + 0xde), &s1, 2);
    memcpy((void *)(uintptr_t)(inst + 0x90), &zero, 4);
    coop_rd(inst + 0xd8, &w, 2);
    w &= (uint16_t)~1u;
    memcpy((void *)(uintptr_t)(inst + 0xd8), &w, 2);
}

// The actor's calc (main+0x41bb20, x0 the actor) and the action request (main+0x50ad74, x0 the BSA), which other code
// hooks too: called first from coop_spawn.c coop_hook. Nonzero: the function is not run.
int coop_enemy_shared(Cpu *c, uint64_t off) {
    if (!probe_mode()) return 0;
    uint64_t t = atomic_load(&target);
    if (!t) return 0;
    if (off == 0x41bb20) {
        if (c->x[0] == atomic_load(&me_actor)) atomic_fetch_add(&calc_me, 1);
        if (c->x[0] != t) return 0;
        atomic_fetch_add(&calc_t, 1);
        if (probe_mode() >= 2) {
            uint64_t s = (plat_now_ns() - picked_ns) / 1000000000ull;
            int on = s >= 4 && s < 34;   // (a spawned enemy runs off within 10 s: the switch comes before)
            if (on != atomic_load(&switch_on)) {
                atomic_store(&switch_on, on);
                rt_log("coop: enprobe: the copy switch goes %s for the target", on ? "ON" : "off");
                if (!on && probe_mode() >= 3) {   // the interrupt graph is rooted again
                    uint64_t bsa = coop_rd_ptr(coop_rd_ptr(t + 0x228) + 0x158), rec = coop_rd_ptr(bsa + 0x2618), ctrl = rec ? coop_rd_ptr(rec + 0x18) : 0;
                    int32_t zero = 0;
                    if (ctrl) memcpy((void *)(uintptr_t)(ctrl + 0x19c), &zero, 4), rt_log("coop: enprobe: the interrupt graph's controller %llx is given root 0", (unsigned long long)ctrl);
                }
            }
            if (on) empty_inline(t);
            if (probe_mode() >= 4) pose_drive(c, t, on);
        }
        return 0;
    }
    if (coop_rd_ptr(c->x[0] + 0x18) != t) return 0;
    int turn = atomic_load(&switch_on);
    count(H_REQUEST, c->x[0], c->x[30], turn);
    if (turn) { c->x[0] = 2; return 1; }   // "no slot object": what a caller of a request without a slot gets
    return 0;
}

int coop_enemy_hook(Cpu *c, uint64_t off) {
    if (!probe_mode()) return 0;
    uint64_t t = atomic_load(&target);
    int hook;
    uint64_t ctrl = c->x[0], actor;
    switch (off) {
    case 0x1920: hook = H_TICK; ctrl = coop_rd_ptr(coop_rd_ptr(c->x[0] + 8) + 8); actor = ctrl ? coop_rd_ptr(ctrl + 0x1f8) : 0; break;
    case 0x1bb410: {   // a node's enter: which register holds the node is not read; either that leads to the target counts
        if (!t) return 0;
        hook = H_ENTER;
        actor = 0;
        for (int r = 0; r < 2 && actor != t; r++) {
            uint64_t n = c->x[r], inst = n > 0x10000 ? coop_rd_ptr(n + 8) : 0;
            ctrl = inst > 0x10000 ? coop_rd_ptr(inst + 8) : 0;
            actor = ctrl > 0x10000 ? coop_rd_ptr(ctrl + 0x1f8) : 0;
        }
        break;
    }
    case 0x50b404: hook = H_50B404; actor = coop_rd_ptr(ctrl + 0x1f8); break;
    case 0x42e0c8: hook = H_42E0C8; actor = coop_rd_ptr(ctrl + 0x1f8); break;
    case 0x2beb80: hook = H_2BEB80; actor = coop_rd_ptr(ctrl + 0x1f8); break;
    case 0x44ff30: {
        hook = H_SCHED;
        actor = coop_rd_ptr(c->x[0] + 0xb70);
        if (!t && actor) {   // choose the target: the scheduler runs once a frame for every enemy that thinks
            static const char *want;
            if (!want) { want = cfg_get("TOTK_COOP_TEST_ENPROBE_NAME"); if (!want || !*want) want = "Enemy_Bokoblin"; }
            char nm[64] = "";
            coop_actor_name(actor, nm, sizeof nm);
            uint64_t me = coop_player_actor();
            float a[3], b[3];
            if (me && !strncmp(nm, want, strlen(want)) && coop_rd(actor + COOP_ACTOR_POS, a, 12) && coop_rd(me + COOP_ACTOR_POS, b, 12)) {
                float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
                static atomic_flag picking = ATOMIC_FLAG_INIT;
                if (dx * dx + dy * dy + dz * dz < 40.f * 40.f && !atomic_flag_test_and_set(&picking)) pick(actor, c->x[0]), t = actor;
            }
        }
        if (actor && actor == t) frames++, dump();
        break;
    }
    default: return 0;   // registered for the sync, no test on it yet
    }
    if (!t || actor != t) return 0;
    int turn = atomic_load(&switch_on);
    count(hook, ctrl, c->x[30], turn);
    return turn;   // run 2: the runner is turned away (these functions return nothing a caller reads)
}
