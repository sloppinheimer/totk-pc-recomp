// Co-op: autobuild research (TOTK_COOP_TEST_ABLOG=1; docs/coop/re-notes.md, "Autobuild copies"). TotK 1.4.3 main offsets.
//
// main+0x6898dc is the constructor of the actor class an autobuild device is made as (x0 the actor, x1 its create
// information). It labels the actor "ImaginaryAutoBuildedActor" (actor +0x18) and sets actor +0x668 bit 0x10000000
// when the create information's +0x78 holds the autobuild's type object main+0x3a4e008 (coop_game.c
// coop_request_create_autobuilt). Logged here: each actor so labelled; the game's glue calls (createScotch
// main+0x1c0b94c, the glue step main+0x1c0ba8c, its end main+0x1d61a6c; an autobuild makes none); the call that gives a
// device its contraption's group (main+0x1d93978: the SpecialPowerReceiverComponent's +0x28, the object all devices of
// one contraption share); every 2 s each labelled device's state, glue entries and group; 12 s after the first one a
// search of the devices' memory for each other (bond_scan) and a write watch on the first one's group field.
// TOTK_COOP_TEST_ABHIT=<s>: a sword hit on one of the build's fans (hit_test); TOTK_COOP_TEST_ABGLUE=1: the game that
// did not build glues its copies with Ultrahand glue (glue_try). tools/coop/autobuild_3rd.txt plays the build.
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

static int ablog_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_TEST_ABLOG"); on = e && *e && *e != '0'; }
    return on;
}

static void dump(const char *what, uint64_t a, int n) {
    uint64_t w[0x40];
    if (n > (int)sizeof w) n = (int)sizeof w;
    if (!coop_rd(a, w, (size_t)n)) { rt_log("coop: ablog: %s %llx unreadable", what, (unsigned long long)a); return; }
    for (int i = 0; i < n / 8; i += 4) {
        char line[200];
        int k = snprintf(line, sizeof line, "coop: ablog: %s +%03x:", what, i * 8);
        for (int j = i; j < i + 4 && j < n / 8; j++) {
            uint64_t v = w[j];
            if (v >= MOD_MAIN_BASE && v < MOD_MAIN_BASE + 0x4000000) k += snprintf(line + k, sizeof line - (size_t)k, " main+%llx", (unsigned long long)(v - MOD_MAIN_BASE));
            else k += snprintf(line + k, sizeof line - (size_t)k, " %016llx", (unsigned long long)v);
        }
        rt_log("%s", line);
    }
}

static int is_label(uint64_t a) {
    static const char tag[] = "ImaginaryAutoBuildedActor";
    char buf[sizeof tag];
    uint64_t s = coop_rd_ptr(a + 0x18);
    return s && coop_rd(s, buf, sizeof buf) && !memcmp(buf, tag, sizeof tag);
}
static uint64_t link_actor(uint64_t link) {   // an ActorLink (coop_glue.c link_actor)
    uint64_t ref = coop_rd_ptr(link + 8);
    uint32_t id = 0, rid = 0;
    if (!ref || !coop_rd(link + 0x10, &id, 4) || id == ~0u || !coop_rd(ref + 0x48, &rid, 4) || rid != id) return 0;
    return coop_rd_ptr(ref + 0x40);
}

#define TRACK_N 32
static _Atomic uint64_t tracked[TRACK_N];
static _Atomic int tracked_n;

int coop_autobuild_hook(Cpu *c, uint64_t addr) {
    static __thread int inside;
    if (inside || !ablog_on()) return 0;
    uint64_t off = addr - MOD_MAIN_BASE, lr = c->x[30];
    if (off == 0x1d93978) {   // a component's group call (x0 its owner, [x0+8] the actor; x2+0x90 the group's holder)
        static _Atomic int said;
        uint64_t actor = coop_rd_ptr(c->x[0] + 8);
        if (!is_label(actor) || atomic_fetch_add(&said, 1) >= 24) return 0;
        char line[400], nm[48];
        uint64_t holder = c->x[2] ? coop_rd_ptr(c->x[2] + 0x90) : 0;
        int k = snprintf(line, sizeof line, "coop: ablog: group call on %s %llx: x1 %llx x2 %llx (+0x90 %llx, vtable main+%llx), %.2f s, lr main+%llx",
                         coop_actor_name(actor, nm, sizeof nm), (unsigned long long)actor, (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                         (unsigned long long)holder, (unsigned long long)(coop_rd_ptr(holder) - MOD_MAIN_BASE), frame_step_world_us() / 1e6,
                         (unsigned long long)(lr - MOD_MAIN_BASE));
        uint64_t fp = c->x[29];
        for (int d = 0; d < 14 && fp && k < (int)sizeof line - 24; d++) {
            uint64_t next = coop_rd_ptr(fp), ret = coop_rd_ptr(fp + 8);
            if (!ret) break;
            k += snprintf(line + k, sizeof line - (size_t)k, " <- %llx", (unsigned long long)(ret - MOD_MAIN_BASE));
            if (next <= fp) break;
            fp = next;
        }
        rt_log("%s", line);
        if (atomic_load(&said) <= 2 && holder) dump("holder", holder, 0x80), dump("x2", c->x[2], 0x100);
        return 0;
    }
    if (off != 0x6898dc) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 80) {
            char n0[48] = "", n1[48] = "";
            uint64_t a0 = coop_rd_ptr(c->x[0] + 0x18);   // a component's actor (the glue veto reads it so)
            coop_actor_name(a0, n0, sizeof n0);
            if (off == 0x1c0b94c) {   // createScotch (component, request): the request's +0x20 the argument, +0x78 its link B
                uint64_t arg = coop_rd_ptr(c->x[1] + 0x20);
                coop_actor_name(arg ? link_actor(arg + 0x78) : 0, n1, sizeof n1);
            }
            rt_log("coop: ablog: main+%llx x0 %llx (%s) x1 %llx (%s) from main+%llx, %.2f s", (unsigned long long)off,
                   (unsigned long long)c->x[0], n0, (unsigned long long)c->x[1], n1, (unsigned long long)(lr - MOD_MAIN_BASE),
                   frame_step_world_us() / 1e6);
        }
        return 0;
    }
    uint64_t actor = c->x[0], info = c->x[1];
    uint64_t rec = coop_rd_ptr(info + 0x78);
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    if (!is_label(actor)) return 1;
    int n = atomic_fetch_add(&tracked_n, 1);
    if (n < TRACK_N) atomic_store(&tracked[n], actor);
    if (n < 40)
        rt_log("coop: ablog: labelled actor %llx made from main+%llx, record %llx (vtable main+%llx), %.2f s", (unsigned long long)actor,
               (unsigned long long)(lr - MOD_MAIN_BASE), (unsigned long long)rec, (unsigned long long)(coop_rd_ptr(rec) - MOD_MAIN_BASE),
               frame_step_world_us() / 1e6);
    if (n < 2) dump("info", info, 0x100), dump("record", rec, 0x100);
    return 1;
}

void ci_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {   // bond_scan: CombinedActorIntegrator actors
    struct { uint64_t a[8]; int n; } *ci = arg;
    (void)key;
    if (!strcmp(nm, "CombinedActorIntegrator") && ci->n < 8) ci->a[ci->n++] = a;
}
// Where a device keeps the others of its build: each live tracked device's actor (0x1000 bytes) and its components
// ([actor+0x228] + 8 j, 0x800 bytes each), and one level down (0x200 bytes at each pointer there), searched for
// another tracked device's actor, its reference object ([actor+0x1a8]) or its id ([actor+0x10]).
static void bond_scan(int n) {
    uint64_t act[TRACK_N], ref[TRACK_N];
    uint32_t uid[TRACK_N];
    int live = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = atomic_load(&tracked[i]);
        int32_t st = 0;
        act[i] = ref[i] = 0, uid[i] = 0;
        if (!coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6) continue;
        act[i] = a, ref[i] = coop_rd_ptr(a + 0x1a8), coop_rd(a + 0x10, &uid[i], 4);
        live++;
    }
    rt_log("coop: ablog: bond scan over %d live devices", live);
    {   // every CombinedActorIntegrator here, and whether it points at a device or the first device's group
        void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
        static struct { uint64_t a[8]; int n; } ci;
        ci.n = 0;
        void ci_fn(uint64_t a, const char *nm, uint32_t key, void *arg);
        coop_actors_all(ci_fn, &ci);
        uint64_t t0 = act[0] ? coop_rd_ptr(act[0] + 0x228) : 0, c0 = t0 ? coop_rd_ptr(t0 + 0x480) : 0, g0 = c0 ? coop_rd_ptr(c0 + 0x28) : 0;
        for (int i = 0; i < ci.n; i++) {
            float p[3] = {0};
            coop_rd(ci.a[i] + COOP_ACTOR_POS, p, 12);
            int hit = 0;
            for (int k = 0; k < 0x1000 / 8; k++) {
                uint64_t v = coop_rd_ptr(ci.a[i] + 8 * (uint64_t)k);
                for (int o = 0; o < n; o++)
                    if (v && (v == act[o] || v == ref[o])) hit++, rt_log("coop: ablog: integrator %d +%x: device %d", i, k * 8, o);
                if (v && v == g0) hit++, rt_log("coop: ablog: integrator %d +%x: the group", i, k * 8);
            }
            rt_log("coop: ablog: CombinedActorIntegrator %d %llx at %.2f %.2f %.2f, %d words to the build", i, (unsigned long long)ci.a[i], p[0], p[1], p[2], hit);
        }
    }
    for (int i = 0; i < n; i++) {   // the glue component's +0x28 (device 0's held the others' references)
        if (!act[i]) continue;
        uint64_t t = coop_rd_ptr(act[i] + 0x228), comp = t ? coop_rd_ptr(t + 0x480) : 0, g = comp ? coop_rd_ptr(comp + 0x28) : 0;
        char nm[48];
        rt_log("coop: ablog: device %d %s: glue component %llx, its +0x28 %llx", i, coop_actor_name(act[i], nm, sizeof nm),
               (unsigned long long)comp, (unsigned long long)g);
        if (comp) dump("component", comp, 0x80);
        if (g) dump("group", g, 0x100);
    }
    int hits = 0;
    static uint64_t w[0x1000 / 8], v[0x200 / 8];
    for (int i = 0; i < n && hits < 400; i++) {
        if (!act[i]) continue;
        uint64_t t = coop_rd_ptr(act[i] + 0x228);
        for (int j = -1; j < 256 && hits < 400; j++) {   // -1: the actor itself
            uint64_t base = j < 0 ? act[i] : coop_rd_ptr(t + 8 * (uint64_t)j);
            int len = j < 0 ? 0x1000 : 0x800;
            if (!base || (base & 7) || !coop_rd(base, w, (size_t)len)) continue;
            for (int k = 0; k < len / 8 && hits < 400; k++) {
                for (int o = 0; o < n; o++) {
                    if (o == i || !act[o]) continue;
                    const char *what = w[k] == act[o] ? "actor" : w[k] == ref[o] && ref[o] ? "reference" : 0;
                    if (what) { hits++; rt_log("coop: ablog:   device %d comp %d +%x: device %d's %s", i, j, k * 8, o, what); }
                }
                if (w[k] < 0x100000000ull || w[k] >= 0x8000000000ull || (w[k] & 7) || !coop_rd(w[k], v, sizeof v)) continue;
                for (int m = 0; m < 0x200 / 8 && hits < 400; m++)
                    for (int o = 0; o < n; o++) {
                        if (o == i || !act[o]) continue;
                        const char *what = v[m] == act[o] ? "actor" : v[m] == ref[o] && ref[o] ? "reference" : 0;
                        if (what) {
                            hits++;
                            uint64_t vt = v[0];
                            rt_log("coop: ablog:   device %d comp %d +%x -> %llx (word 0 %s%llx) +%x: device %d's %s", i, j, k * 8,
                                   (unsigned long long)w[k], vt >= MOD_MAIN_BASE && vt < MOD_MAIN_BASE + 0x4000000 ? "main+" : "",
                                   (unsigned long long)(vt >= MOD_MAIN_BASE && vt < MOD_MAIN_BASE + 0x4000000 ? vt - MOD_MAIN_BASE : vt), m * 8, o, what);
                        }
                    }
            }
        }
    }
    rt_log("coop: ablog: bond scan: %d hits", hits);
}

// A Zonai device's on state as the fan tests read it (coop_players.c fanset_test): [comp 11]+0x348 bit 0x2000 and
// [comp 28]+0x398 (its energy, 10 when on).
static void dev_on(uint64_t a, uint32_t *on11, uint32_t *e28) {
    uint64_t t = coop_rd_ptr(a + 0x228), c11 = t ? coop_rd_ptr(t + 8 * 11) : 0, c28 = t ? coop_rd_ptr(t + 8 * 28) : 0;
    *on11 = 0, *e28 = 0;
    if (c11) coop_rd(c11 + 0x348, on11, 4), *on11 &= 0x2000;
    if (c28) coop_rd(c28 + 0x398, e28, 4);
}
static int live_device(int i, char *nm, size_t n) {
    uint64_t a = atomic_load(&tracked[i]);
    int32_t st = 0;
    coop_actor_name(a, nm, n);
    return a && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6;
}
// TOTK_COOP_TEST_ABHIT=<s> (with TOTK_COOP_TEST_ABLOG and tools/coop/autobuild_3rd.txt, whose sword swings come 2 s
// and 4 s after it): at that world time this Link is put 1 m outside the build's first fan, facing it, so the second
// swing hits it; from then for 10 s, every 0.5 s, each live device's on state. In game A only (the first joined).
static void hit_test(Cpu *c, uint64_t me, int n) {
    static double at = -1;
    static int placed;
    static int64_t last;
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_ABHIT"); at = e ? atof(e) : 0; }
    if (at <= 0) return;
    double now = frame_step_world_us() / 1e6;
    if (now < at || now > at + 10) return;
    char nm[48];
    if (!placed && coop_net_id() == 1) {
        int fan = -1, wing = -1;
        for (int i = 0; i < n; i++) {
            if (!live_device(i, nm, sizeof nm)) continue;
            if (fan < 0 && !strcmp(nm, "SpObj_WindGenerator_A_01")) fan = i;
            if (wing < 0 && !strcmp(nm, "SpObj_LiftGeneratorWing_A_01")) wing = i;
        }
        float f[3], w[3];
        if (fan < 0 || wing < 0 || !coop_rd(atomic_load(&tracked[fan]) + COOP_ACTOR_POS, f, 12) ||
            !coop_rd(atomic_load(&tracked[wing]) + COOP_ACTOR_POS, w, 12))
            return;
        float ux = f[0] - w[0], uz = f[2] - w[2], l = sqrtf(ux * ux + uz * uz);
        if (l < 0.1f) ux = 1, uz = 0, l = 1;
        ux /= l, uz /= l;
        float dx = -ux, dz = -uz;   // facing the fan
        float m[12] = {dz, 0, dx, f[0] + ux, 0, 1, 0, f[1] - 0.2f, -dx, 0, dz, f[2] + uz};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        placed = 1;
        rt_log("coop: test: abhit: this Link put 1 m from device %d (a fan at %.2f %.2f %.2f), facing it", fan, f[0], f[1], f[2]);
    }
    int64_t t = frame_step_world_us();
    if (t - last < 500000) return;
    last = t;
    char line[400];
    int k = snprintf(line, sizeof line, "coop: test: abhit: %.1f s:", now);
    for (int i = 0; i < n && k < (int)sizeof line - 40; i++) {
        if (!live_device(i, nm, sizeof nm)) continue;
        uint32_t on11, e28;
        dev_on(atomic_load(&tracked[i]), &on11, &e28);
        k += snprintf(line + k, sizeof line - (size_t)k, " %d %s %s/%u", i, strstr(nm, "Wind") ? "fan" : strstr(nm, "Wing") ? "wing" : nm, on11 ? "on" : "off", e28);
    }
    rt_log("%s", line);
}

// TOTK_COOP_TEST_ABGLUE=1 (experiment, the game that did not build): 4 s after the first device is tracked here,
// each fan glued to the wing with the glue test's call (coop_glue.c coop_glue_pair).
static void glue_try(Cpu *c, int n, int64_t since) {
    static int done;
    if (done || !cfg_get("TOTK_COOP_TEST_ABGLUE") || coop_net_id() == 1 || frame_step_world_us() - since < 4000000) return;
    done = 1;
    int coop_glue_pair(Cpu *c, uint64_t a, uint64_t b);
    char nm[48];
    uint64_t wing = 0;
    for (int i = 0; i < n && !wing; i++)
        if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_LiftGeneratorWing_A_01")) wing = atomic_load(&tracked[i]);
    for (int i = 0; i < n && wing; i++)
        if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_WindGenerator_A_01"))
            rt_log("coop: test: abglue: device %d to the wing: %s", i, coop_glue_pair(c, atomic_load(&tracked[i]), wing) ? "glued" : "not glued");
}

// Every 2 s while any device is tracked (for 60 s): each one's name, state, flags and glue entries.
void coop_autobuild_tick(Cpu *c, uint64_t me) {
    if (!ablog_on()) return;
    static int64_t last, first;
    int64_t now = frame_step_world_us();
    int n = atomic_load(&tracked_n);
    if (n > TRACK_N) n = TRACK_N;
    if (n) hit_test(c, me, n);
    static int64_t seen_at;
    if (n && !seen_at) seen_at = now;
    if (n) glue_try(c, n, seen_at);
    if (!n || now - last < 2000000) return;
    if (!first) first = now;
    if (now - first > 60000000) return;
    last = now;
    static int scanned;
    if (!scanned && now - first > 12000000) { scanned = 1; bond_scan(n); }
    // A write watch on the first device's glue component +0x20..+0x30 (its group at +0x28) for 14 s: who sets it.
    static int watch_stage;
    if (watch_stage == 0) {
        uint64_t a = atomic_load(&tracked[0]), t = coop_rd_ptr(a + 0x228), comp = t ? coop_rd_ptr(t + 0x480) : 0;
        int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
        extern int coop_watch_writes;
        if (comp) { coop_watch_writes = 1; watch_stage = coop_watch_arm(comp + 0x20, 0x10) ? 1 : 2; }
    } else if (watch_stage == 1 && now - first > 14000000) {
        void coop_watch_disarm(void);
        coop_watch_disarm();
        watch_stage = 2;
    }
    for (int i = 0; i < n; i++) {
        uint64_t a = atomic_load(&tracked[i]);
        char nm[48];
        int32_t st = 0;
        uint32_t f440 = 0, f668 = 0, p5d0 = 0, uid = 0;
        float p[3] = {0};
        coop_rd(a + COOP_ACTOR_STATE, &st, 4), coop_rd(a + 0x440, &f440, 4), coop_rd(a + 0x668, &f668, 4), coop_rd(a + 0x5d0, &p5d0, 4);
        coop_rd(a + COOP_ACTOR_POS, p, 12), coop_rd(a + 0x10, &uid, 4);
        uint64_t t = coop_rd_ptr(a + 0x228), comp = t ? coop_rd_ptr(t + 0x480) : 0;
        uint32_t on11 = 0, e28 = 0;
        dev_on(a, &on11, &e28);
        rt_log("coop: ablog: %.0f s: device %d %llx %s id %08x state %d +440 %x +668 %x +5d0 %08x at %.2f %.2f %.2f, glue component %llx "
               "(group %llx), on %#x energy %u",
               now / 1e6, i, (unsigned long long)a, coop_actor_name(a, nm, sizeof nm), uid, st, f440, f668, p5d0, p[0], p[1], p[2],
               (unsigned long long)comp, (unsigned long long)(comp ? coop_rd_ptr(comp + 0x28) : 0), on11, e28);
        if (!comp) continue;
        uint64_t head = comp + 0x158, node = coop_rd_ptr(comp + 0x160);
        for (int k = 0; node && node != head && k < 16; k++) {
            uint64_t e = coop_rd_ptr(node + 0x10);
            uint32_t est = 0, ha = 0, hb = 0;
            uint8_t fl = 0;
            char na[48], nb[48];
            coop_rd(e + 0x550, &est, 4), coop_rd(e + 0x554, &fl, 1), coop_rd(e + 0x438, &ha, 4), coop_rd(e + 0x4a0, &hb, 4);
            uint64_t aa = link_actor(e + 0x420), bb = link_actor(e + 0x488);
            rt_log("coop: ablog:   entry %llx state %u flags %x: %s %llx (body %08x) to %s %llx (body %08x)", (unsigned long long)e, est, fl,
                   coop_actor_name(aa, na, sizeof na), (unsigned long long)aa, ha, coop_actor_name(bb, nb, sizeof nb), (unsigned long long)bb, hb);
            node = coop_rd_ptr(node + 8);
        }
    }
}
