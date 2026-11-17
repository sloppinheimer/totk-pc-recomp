// The rotating ground's sync: moved out of coop_world.c as it was (docs/coop/enemy-sync.md B.28: the world file
// is taken apart so the older enemy sync in it can go). Nothing here is the enemy sync's. Its packets come in on the
// HIT channel, which the world file's peer_tick reads and hands to ground_take.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// ---- rotating ground (TotK Online's ActorGroundControlSync, read for behaviour only): a GroundControlActor_Rotate
// actor turns the platforms tied to it. Two nodes in 1.4.3 drive the same turn helper (main+0x298a20c: (helper, input,
// out)): the actor's own logic node ActorLogicGroundControlRotate (its parameters name it; enter main+0x1cf0160,
// update main+0x1cf0260: x0 the node, x1 the frame time; node+8 the actor, helper at node+0x28), and the AI node
// ExecuteGroundControlRotate (vtable main+0x37733c0; enter main+0x2329078, update main+0x2329170: x1+0xc the frame
// time; actor [[[node+8]+8]+0x1f8], helper at node+0x20). Helper: +8 top speed, +0xc acceleration (RotateSpeedX,
// RotateAccelX times pi/180, set by the enter), +0x10 speed now, +0x14 the angle (radians, kept in 0..2 pi), +0x18
// the ground control's index. Its input: two bytes, turn and turn the other way, then the frame time. With either on,
// the speed goes to the top speed; with neither, to 0; while the speed is not 0 it adds speed * time to the angle and
// puts the turn on the ground control (main+0x2988330). The logic node reads its two bytes each update from the
// actor's blackboard, RotateAxisX and RotateAxisXInv (main+0x1b13c9c, the two calls before the helper): a placed one
// whose switch is off stands still (the H-5 one in a pair test: the helper ran each frame, the turn was never set,
// 2026-11-07). The logic node's angle at node+0x3c is TotK Online's 1.2.1 place.
// Each update's entry is hooked (coop_ground_hook, coop_spawn.c). The host (the lowest player id, world role 1) sends
// each node's angle, speed and two switch bytes ten times a second ("GR" packets, up to GR_MAX), and in the others a
// node's update first takes the host's: its angle moved on by the host's speed for the time since it came, the speed,
// and (logic nodes) the switch bytes, answered in place of its own blackboard's, so the platforms turn and stand as
// the host's. TOTK_COOP_GROUND=0: not.
// Test: TOTK_COOP_TEST_GROUND=<s>[,x,y,z] (both games, tools/coop/ground_pair_3rd.txt): at that world time this Link
// is put at x y z (default 4 m above the placed one at 2394.5 267.0 520.8, Banc/MainField/H-5_Static, RotateSpeedX
// 6); every second each node's state and what turns ground near the Link are logged (ground_probe_log).
// TOTK_COOP_TEST_GROUND_SPIN=1: the host turns its logic nodes on (RotateAxisX answered 1), for a test where the
// platform's switch is off.
#define GH_SPEED 0x10   // helper+
#define GH_ANGLE 0x14
#define GH_TOP 0x8
#define GROUND_N 32
// The nodes seen (the updates run on game threads): node, its actor, its helper, when last seen, the switch bytes its
// blackboard last gave (logic nodes); key and home set by the tick.
static struct {
    _Atomic uint64_t node, actor, helper;
    _Atomic int64_t seen_us;
    _Atomic uint32_t key;
    _Atomic uint8_t on, inv, logic;
    float home[3];
} gnodes[GROUND_N];
static struct { _Atomic uint32_t key; GroundState s; _Atomic uint64_t ns; } ground_in[GR_MAX];   // the host's
static int ground_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_GROUND"); on = !(e && *e == '0'); }
    return on;
}
static int ground_test_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_GROUND") != NULL;
    return on;
}
static int ground_spin(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_TEST_GROUND_SPIN"); on = e && *e == '1'; }
    return on;
}
static float wrap_2pi(float a) {
    const float tp = 6.28318531f;
    a = fmodf(a, tp);
    return a < 0 ? a + tp : a;
}
// Test counters (ground_probe_log): node updates and enters of each kind, helper runs, turns set (with their callers).
static _Atomic uint32_t gp_update[2], gp_enter[2], gp_helper, gp_set;
static struct { _Atomic uint64_t lr; _Atomic uint32_t n, idx; } gp_callers[16];
// While a logic node's update runs on this thread: its slot, how many blackboard bytes it has read, and what to answer.
static __thread int bb_slot = -1, bb_reads, bb_force;
static __thread uint8_t bb_want[2];
static int ground_slot(uint64_t node, uint64_t actor, uint64_t helper, int logic) {
    int f = -1;
    for (int i = 0; i < GROUND_N && f < 0; i++) if (atomic_load_explicit(&gnodes[i].node, memory_order_acquire) == node) f = i;
    for (int i = 0; i < GROUND_N && f < 0; i++) {
        uint64_t z = 0;
        if (atomic_compare_exchange_strong(&gnodes[i].node, &z, node)) f = i, atomic_store(&gnodes[i].key, 0);
    }
    if (f < 0) return -1;
    atomic_store_explicit(&gnodes[f].actor, actor, memory_order_relaxed);
    atomic_store_explicit(&gnodes[f].helper, helper, memory_order_relaxed);
    atomic_store_explicit(&gnodes[f].logic, (uint8_t)logic, memory_order_relaxed);
    atomic_store_explicit(&gnodes[f].seen_us, frame_step_world_us(), memory_order_release);
    return f;
}
// The host's state for this key, moved on to now; 0 if none fresh.
static int ground_host(uint32_t key, GroundState *out) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < GR_MAX; i++) {
        uint64_t ns = atomic_load_explicit(&ground_in[i].ns, memory_order_acquire);
        if (!ns || atomic_load_explicit(&ground_in[i].key, memory_order_relaxed) != key || now - ns > 1000000000ull) continue;
        *out = ground_in[i].s;
        if (atomic_load_explicit(&ground_in[i].ns, memory_order_acquire) != ns) return 0;   // changed while read
        out->angle = wrap_2pi(out->angle + out->speed * (float)(now - ns) / 1e9f);
        return 1;
    }
    return 0;
}
static void ground_take_host(int f, uint64_t helper) {   // a guest: the host's angle and speed before the update
    uint32_t key = atomic_load_explicit(&gnodes[f].key, memory_order_acquire);
    GroundState s;
    if (!key || coop_world_role() == 1 || !ground_host(key, &s)) return;
    uint32_t b;
    memcpy(&b, &s.angle, 4), ST32(helper + GH_ANGLE, b);
    memcpy(&b, &s.speed, 4), ST32(helper + GH_SPEED, b);
    bb_force = 1, bb_want[0] = s.on, bb_want[1] = s.inv;
    static _Atomic int said;
    if (!atomic_fetch_add(&said, 1)) rt_log("coop: ground %08x turns as the host's (angle %.3f, speed %.3f, on %d %d)", key, s.angle, s.speed, s.on, s.inv);
}
int coop_ground_hook(Cpu *c, uint64_t off) {   // coop_spawn.c coop_hook, with the main offset entered
    if (off == 0x1b13c9c && bb_slot < 0) return 0;   // (a read every system makes: out at once)
    if (!ground_on() || !coop_world_role()) return 0;
    int test = ground_test_on();
    if (off == 0x298a20c) { if (test) atomic_fetch_add_explicit(&gp_helper, 1, memory_order_relaxed); return 0; }
    if (off == 0x2329078 || off == 0x1cf0160) { if (test) atomic_fetch_add_explicit(&gp_enter[off == 0x1cf0160], 1, memory_order_relaxed); return 0; }
    if (off == 0x2988330) {
        if (!test) return 0;
        atomic_fetch_add_explicit(&gp_set, 1, memory_order_relaxed);
        uint16_t idx = 0xffff;
        coop_rd(c->x[1], &idx, 2);
        for (int i = 0; i < 16; i++) {
            uint64_t have = atomic_load(&gp_callers[i].lr), lr = c->x[30];
            if (have == lr || (!have && atomic_compare_exchange_strong(&gp_callers[i].lr, &have, lr))) {
                atomic_fetch_add(&gp_callers[i].n, 1), atomic_store(&gp_callers[i].idx, idx);
                break;
            }
        }
        return 0;
    }
    if (off == 0x2329170) {   // the AI node: its switch bytes are its own inputs, not taken here
        if (test) atomic_fetch_add_explicit(&gp_update[0], 1, memory_order_relaxed);
        uint64_t node = c->x[0], a = node ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(node + 8) + 8) + 0x1f8) : 0;
        int f = a ? ground_slot(node, a, node + 0x20, 0) : -1;
        if (f >= 0) ground_take_host(f, node + 0x20), bb_force = 0;
        return 0;
    }
    if (off == 0x1cf0260) {   // the logic node: run here, its two blackboard reads watched (and answered in a guest)
        static __thread int inside;
        if (inside) return 0;
        if (test) atomic_fetch_add_explicit(&gp_update[1], 1, memory_order_relaxed);
        uint64_t node = c->x[0], a = node ? coop_rd_ptr(node + 8) : 0;
        int f = a ? ground_slot(node, a, node + 0x28, 1) : -1;
        if (f < 0) return 0;
        bb_force = 0, bb_reads = 0;
        ground_take_host(f, node + 0x28);
        if (!bb_force && coop_world_role() == 1 && ground_spin()) bb_force = 1, bb_want[0] = 1, bb_want[1] = 0;
        bb_slot = f;
        inside = 1;
        c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        inside = 0;
        bb_slot = -1, bb_force = 0;
        return 1;
    }
    if (off == 0x1b13c9c) {   // the blackboard's byte read: only the first two inside a logic node's update
        static __thread int inside;
        if (inside || bb_slot < 0 || bb_reads >= 2) return 0;
        int k = bb_reads++, f = bb_slot;
        inside = 1;
        c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        inside = 0;
        // (the byte the node gets, after the answer: kept as the blackboard's own, the host sent 0 0 while
        // TOTK_COOP_TEST_GROUND_SPIN turned it, and the guest's speed fell to 0 and set no turn; pair test 2026-11-07)
        if (bb_force) c->x[0] = (c->x[0] & ~1ull) | bb_want[k];
        uint8_t v = (uint8_t)(c->x[0] & 1);
        if (k == 0) atomic_store_explicit(&gnodes[f].on, v, memory_order_relaxed);
        else atomic_store_explicit(&gnodes[f].inv, v, memory_order_relaxed);
        return 1;
    }
    return 0;
}
typedef struct { float me[3]; int n; char line[1400]; int len; } GroundNear;
static void ground_near_one(uint64_t a, const char *nm, uint32_t key, void *arg) {
    GroundNear *g = arg;
    (void)key;
    if (!strstr(nm, "GroundControl")) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float d = sqrtf((p[0] - g->me[0]) * (p[0] - g->me[0]) + (p[1] - g->me[1]) * (p[1] - g->me[1]) + (p[2] - g->me[2]) * (p[2] - g->me[2]));
    if (d > 80 || g->n >= 12 || g->len > (int)sizeof g->line - 160) return;
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    uint64_t t = coop_rd_ptr(a + 0x228), ph = t ? coop_rd_ptr(t + 0x50) : 0, ctl = ph ? coop_rd_ptr(ph + 0x20) : 0;
    uint64_t gc = ctl ? coop_rd_ptr(ctl + 0x140) : 0;
    uint16_t idx = 0xffff;
    if (gc) coop_rd(gc + 0xc, &idx, 2);
    g->len += snprintf(g->line + g->len, sizeof g->line - g->len, " %s at %.1f m (%.1f %.1f %.1f) state %d%s index %d;", nm, d, p[0], p[1], p[2], st,
                       coop_world_live_now(a) ? " live" : " at rest", idx == 0xffff ? -1 : idx);
    g->n++;
}
// Test (TOTK_COOP_TEST_GROUND), each second: the GroundControl actors within 80 m of this Link (every actor, at rest
// too), and the calls since the last line: node updates and enters (AI, logic), helper runs, turns set with their
// callers' return addresses and indices.
static void ground_probe_log(uint64_t me) {
    GroundNear g = {.n = 0, .len = 0};
    if (!coop_rd(me + COOP_ACTOR_POS, g.me, 12)) return;
    coop_actors_all(ground_near_one, &g);
    rt_log("coop: test: ground: this Link at %.1f %.1f %.1f; GroundControl actors within 80 m: %d;%s", g.me[0], g.me[1], g.me[2], g.n, g.line);
    char cl[600];
    int n = 0;
    cl[0] = 0;
    for (int i = 0; i < 16 && n < (int)sizeof cl - 60; i++) {
        uint64_t lr = atomic_load(&gp_callers[i].lr);
        if (!lr) break;
        n += snprintf(cl + n, sizeof cl - n, " main+%#llx x%u (index %u);", (unsigned long long)(lr - MOD_MAIN_BASE), atomic_exchange(&gp_callers[i].n, 0),
                      atomic_load(&gp_callers[i].idx));
    }
    rt_log("coop: test: ground: in the last second: logic node update %u, enter %u; AI node update %u, enter %u; turn helper %u; turn set %u;%s",
           atomic_exchange(&gp_update[1], 0), atomic_exchange(&gp_enter[1], 0), atomic_exchange(&gp_update[0], 0), atomic_exchange(&gp_enter[0], 0),
           atomic_exchange(&gp_helper, 0), atomic_exchange(&gp_set, 0), cl);
}
void ground_tick(Cpu *c, uint64_t me) {
    if (!ground_on() || !coop_net_on()) return;
    static int64_t test_at = -2;
    static float test_p[3] = {2394.5f, 271.0f, 520.8f};
    int test = ground_test_on();
    if (test_at == -2) {
        const char *v = cfg_get("TOTK_COOP_TEST_GROUND");
        test_at = v ? (int64_t)(atof(v) * 1e6) : -1;
        const char *xyz = v ? strchr(v, ',') : NULL;
        if (xyz) sscanf(xyz + 1, "%f,%f,%f", &test_p[0], &test_p[1], &test_p[2]);
    }
    int64_t now_us = frame_step_world_us();
    if (test_at >= 0 && now_us >= test_at) {   // put there once, as TOTK_COOP_TEST_TRAVEL does (coop_notices.c)
        test_at = -1;
        float m[12] = {1, 0, 0, test_p[0], 0, 1, 0, test_p[1], 0, 0, 1, test_p[2]};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        rt_log("coop: test: ground: this Link put at %.1f %.1f %.1f", test_p[0], test_p[1], test_p[2]);
    }
    static uint64_t last_send, last_test;
    uint64_t now = plat_now_ns();
    int log = test && now - last_test > 1000000000ull;
    if (log) last_test = now, ground_probe_log(me);
    GroundPacket p = {.kind = GROUND_KIND};
    for (int i = 0; i < GROUND_N; i++) {
        uint64_t node = atomic_load_explicit(&gnodes[i].node, memory_order_acquire), a = atomic_load(&gnodes[i].actor);
        uint64_t helper = atomic_load(&gnodes[i].helper);
        if (!node || !a || !helper) continue;
        if (now_us - atomic_load(&gnodes[i].seen_us) > 2000000) {   // not updated for 2 s: gone (or asleep)
            atomic_store(&gnodes[i].key, 0), atomic_store_explicit(&gnodes[i].node, 0, memory_order_release);
            continue;
        }
        char nm[64];
        coop_actor_name(a, nm, sizeof nm);
        uint32_t key = atomic_load(&gnodes[i].key);
        int logic = atomic_load(&gnodes[i].logic);
        if (!key) {   // keyed once, by where it was first seen (a placed one by its placement: actor_key)
            if (!coop_rd(a + COOP_ACTOR_POS, gnodes[i].home, 12)) continue;
            key = coop_world_actor_key(a, nm, gnodes[i].home);
            atomic_store_explicit(&gnodes[i].key, key, memory_order_release);
            rt_log("coop: ground: %s %08x at %.1f %.1f %.1f is turned by its %s node here", nm, key, gnodes[i].home[0], gnodes[i].home[1],
                   gnodes[i].home[2], logic ? "logic" : "AI");
        }
        GroundState s = {.key = key, .on = atomic_load(&gnodes[i].on), .inv = atomic_load(&gnodes[i].inv)};
        float top = 0;
        coop_rd(helper + GH_ANGLE, &s.angle, 4), coop_rd(helper + GH_SPEED, &s.speed, 4), coop_rd(helper + GH_TOP, &top, 4);
        if (log) {
            GroundState h;
            int have = ground_host(key, &h);
            rt_log("coop: test: ground %s %08x (%s node) angle %.3f speed %.3f (top %.3f) switch %d %d%s; host's %s %.3f speed %.3f switch %d %d", nm, key,
                   logic ? "logic" : "AI", s.angle, s.speed, top, s.on, s.inv, coop_world_role() == 1 ? " (host)" : "", have ? "now" : "none",
                   have ? h.angle : 0.0f, have ? h.speed : 0.0f, have ? h.on : 0, have ? h.inv : 0);
        }
        if (coop_world_role() == 1 && p.n < GR_MAX) p.g[p.n++] = s;
    }
    if (coop_world_role() == 1 && p.n && now - last_send > 100000000ull) {
        last_send = now;
        coop_net_hit_send(&p, (int)(offsetof(GroundPacket, g) + p.n * sizeof p.g[0]));
    }
}
void ground_take(const GroundPacket *p, int n) {
    if (n != (int)(offsetof(GroundPacket, g) + p->n * sizeof p->g[0]) || p->n > GR_MAX) return;
    uint64_t now = plat_now_ns();
    for (uint32_t k = 0; k < p->n; k++) {
        int f = -1;
        for (int i = 0; i < GR_MAX && f < 0; i++) if (atomic_load(&ground_in[i].key) == p->g[k].key) f = i;
        for (int i = 0; i < GR_MAX && f < 0; i++) if (now - atomic_load(&ground_in[i].ns) > 2000000000ull) f = i;
        if (f < 0) continue;
        atomic_store(&ground_in[f].ns, 0);   // (a reader skips it, or sees the change and drops what it read)
        ground_in[f].s = p->g[k];
        atomic_store(&ground_in[f].key, p->g[k].key);
        atomic_store_explicit(&ground_in[f].ns, now, memory_order_release);
    }
}
