// Co-op: autobuild research (TOTK_COOP_TEST_ABLOG=1; docs/coop/re-notes.md, "Autobuild copies"). TotK 1.4.3 main offsets.
// The sync this led to is coop_bonds.c.
//
// main+0x6898dc is the constructor of the actor class an autobuild device is made as (x0 the actor, x1 its create
// information). It labels the actor "ImaginaryAutoBuildedActor" (actor +0x18) and sets actor +0x668 bit 0x10000000
// when the create information's +0x78 holds the autobuild's type object main+0x3a4e008 (coop_game.c
// coop_request_create_autobuilt). Logged here: each actor so labelled; the game's glue calls (createScotch
// main+0x1c0b94c, the glue step main+0x1c0ba8c, its end main+0x1d61a6c; an autobuild makes none); the call that gives a
// device its contraption's group (main+0x1d93978: the SpecialPowerReceiverComponent's +0x28, the object all devices of
// one contraption share); every 2 s each tracked device's state, glue entries, group and on state; 12 s after the
// first one a search of the devices' memory for each other (bond_scan) and a write watch on the first one's group
// field (or, with TOTK_COOP_TEST_ABWATCH=<hex address>,<length>,<from s>,<to s>, on that range).
// TOTK_COOP_TEST_ABHIT=<s> (and TOTK_COOP_TEST_ABHIT_B=<s>): sword hits on the build's fans in each game (hit_test);
// TOTK_COOP_TEST_ABGLUE=1: the game that did not build glues its copies with Ultrahand glue (glue_try);
// TOTK_COOP_TEST_ABDUMP=1: a fan's model, actor and components logged for a diff (fan_dump); TOTK_COOP_TEST_ABTRACE=1:
// a fan's actor and components every tick through the hit (trace_tick);
// TOTK_COOP_TEST_ABPART=<s>: a fan taken off with removeScotch (part_test); TOTK_COOP_TEST_ABPLACE=<s>: game A's Link
// put on flat ground. tools/coop/autobuild_3rd.txt
// plays the build, tools/coop/autobuild_b_3rd.txt game B's swing.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
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

#define TRACK_N 128   // (an autobuild screen scrolled through makes previews of each schematic it passes)
static _Atomic uint64_t tracked[TRACK_N];
static _Atomic int tracked_n;

static int64_t msg_from_us = INT64_MAX, msg_to_us;   // the trace's window (hit_test, trace_tick)
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
static int live_device(int i, char *nm, size_t n);
// TOTK_COOP_TEST_ABDUMP=1: the first live fan's actor (0x1000 bytes) and its components (0x200 bytes each) as
// "abdump <component> <offset> <word>" lines, one word a line where it is not 0, for a diff of an original against a copy.
static void fan_dump(int n) {
    char nm[48];
    for (int i = 0; i < n; i++) {
        if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_WindGenerator_A_01")) continue;
        uint64_t a = atomic_load(&tracked[i]), t = coop_rd_ptr(a + 0x228);
        static uint64_t w[0x1000 / 8];
        {   // its model ([[actor+0x228]+8]+0x28) and the objects that model points to, as "abmodel" lines
            uint64_t m = coop_rd_ptr(coop_rd_ptr(t + 8) + 0x28);
            if (m && coop_rd(m, w, 0x800))
                for (int k = 0; k < 0x800 / 8; k++) {
                    if (w[k]) rt_log("coop: abmodel -1 %x %016llx", k * 8, (unsigned long long)w[k]);
                    static uint64_t v[0x200 / 8];
                    if (k < 0x100 / 8 && w[k] >= 0x100000000ull && w[k] < 0x8000000000ull && !(w[k] & 7) && coop_rd(w[k], v, sizeof v))
                        for (int j = 0; j < 0x200 / 8; j++)
                            if (v[j]) rt_log("coop: abmodel %d %x %016llx", k * 8, j * 8, (unsigned long long)v[j]);
                }
        }
        for (int j = -1; j < 256; j++) {
            uint64_t base = j < 0 ? a : coop_rd_ptr(t + 8 * (uint64_t)j);
            int len = j < 0 ? 0x1000 : 0x200;
            if (!base || !coop_rd(base, w, (size_t)len)) continue;
            for (int k = 0; k < len / 8; k++)
                if (w[k]) rt_log("coop: abdump %d %x %016llx", j, k * 8, (unsigned long long)w[k]);
        }
        return;
    }
}
static void bond_scan(int n) {
    if (cfg_get("TOTK_COOP_TEST_ABDUMP")) fan_dump(n);
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
        uint64_t ph = t ? coop_rd_ptr(t + 0x50) : 0, ctl = ph ? coop_rd_ptr(ph + 0x20) : 0, body = ctl ? coop_rd_ptr(ctl + 0x150) : 0, bf = 0;
        int32_t bi = 0;
        coop_rd(body + 0x68, &bf, 8), coop_rd(body + 0x150, &bi, 4);
        rt_log("coop: ablog: device %d %s: glue component %llx, its +0x28 %llx; main body %llx (+0x68 %llx, +0x150 %d)", i,
               coop_actor_name(act[i], nm, sizeof nm), (unsigned long long)comp, (unsigned long long)g, (unsigned long long)body,
               (unsigned long long)bf, bi);
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
// and 4 s after it): at that world time game A's Link (the first joined) is put 1 m outside the build's first fan,
// facing it, so the second swing hits it; from then for 25 s, every 0.5 s, each live device's on state in both games.
// With TOTK_COOP_TEST_ABHIT_B=<s> and tools/coop/autobuild_b_3rd.txt game B's Link is put so by its copy of a fan.
// The game that did not build tracks no device when its copies carry no label (TOTK_COOP_AUTOBUILT off): for the hit
// test's log it takes the Zonai devices within 15 m of its Link instead.
typedef struct { float me[3]; uint64_t a[TRACK_N]; int n; } NearDev;
static void near_dev_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    NearDev *d = arg;
    float p[3];
    (void)key;
    if (d->n >= TRACK_N || strncmp(nm, "SpObj_", 6) || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float dx = p[0] - d->me[0], dy = p[1] - d->me[1], dz = p[2] - d->me[2];
    if (dx * dx + dy * dy + dz * dz < 225) d->a[d->n++] = a;
}
// TOTK_COOP_TEST_ABTRACE=1 (with ABHIT): from 3.5 s to 5 s after this Link is put by the fan, every tick, the actor
// (0x1000 bytes) and components (0x200 bytes each) of the build's second fan (one the hit does not touch) are kept;
// then each word that changed one to three times in that time is logged with when ("abtrace"), and when the fan's on
// bool changed: what a neighbouring device reads to turn itself on.
#define TR_N 120
#define TR_COMP 256
static uint64_t (*tr_snap)[TR_COMP + 12][0x200 / 8];   // (+8 the actor, +4 component 166 past its first 0x200 bytes)
static int64_t tr_us[TR_N];
static int tr_n, tr_dev = -1;
static uint64_t on_flag(uint64_t actor) {   // a device's on bool (coop_players.c dev_on_flag_at)
    uint64_t tbl = coop_rd_ptr(actor + 0x228), c0 = tbl ? coop_rd_ptr(tbl) : 0, bb = c0 ? coop_rd_ptr(c0 + 0x10) : 0;
    uint32_t n = 0, nb = 0;
    if (!bb || !coop_rd(bb + 0x1d4, &n, 4) || !n || n > 65536 || !coop_rd(bb + 0x198, &nb, 4)) return 0;
    uint64_t ht = coop_rd_ptr(bb + 0x1c8), bools = coop_rd_ptr(bb + 0x1a0);
    if (!ht || !bools) return 0;
    uint32_t start = 0xbcd1015fu % n, i = start;
    for (;;) {
        uint32_t e[2] = {0, 0};
        if (!coop_rd(ht + 8 * (uint64_t)i, e, 8) || !e[0]) return 0;
        if (e[0] == 0xbcd1015fu) return (int32_t)e[1] >= 0 && e[1] < nb ? bools + e[1] : 0;
        i = i + 1 < n ? i + 1 : 0;
        if (i == start) return 0;
    }
}
// TOTK_COOP_TEST_FANHITC166=1: FANHIT looks for the device's component 166 (its state at +0x28) as x0 / x1 instead
static uint64_t fanhit_target(uint64_t a) {
    uint64_t t = cfg_get("TOTK_COOP_TEST_FANHITC166") ? coop_rd_ptr(a + 0x228) : 0;
    return t ? coop_rd_ptr(t + 0x530) : a;
}
static int devhit_pick = -1;   // the device devhit_test gave a contact (TOTK_COOP_TEST_ABTRACE=devhit,<s> traces it)
static void trace_tick(int n) {
    // (TOTK_COOP_TEST_ABTRACE=<name>,<from s>: the first live tracked device of that name, from that time)
    const char *te = cfg_get("TOTK_COOP_TEST_ABTRACE");
    static char want[48];
    static double from_s = -1;
    if (!te) return;
    if (from_s < 0) { from_s = 0; if (sscanf(te, "%47[^,],%lf", want, &from_s) != 2) want[0] = 0, from_s = 0; }
    if (from_s > 0 && msg_from_us == INT64_MAX) msg_from_us = (int64_t)(from_s * 1e6);
    if (msg_from_us == INT64_MAX) return;
    int64_t now = frame_step_world_us();
    if (now < msg_from_us || tr_n < 0) return;
    char nm[48];
    if (tr_dev < 0) {
        int fans = 0;
        for (int i = 0; i < n && tr_dev < 0; i++)
            if (!strcmp(want, "devhit") ? i == devhit_pick
                : live_device(i, nm, sizeof nm) && (want[0] ? !strcmp(nm, want) : !strcmp(nm, "SpObj_WindGenerator_A_01") && ++fans == 2)) tr_dev = i;
        if (tr_dev < 0) return;
        tr_snap = calloc(TR_N, sizeof *tr_snap);
        if (!tr_snap) { tr_n = -1; return; }
    }
    uint64_t a = atomic_load(&tracked[tr_dev]), t = coop_rd_ptr(a + 0x228);
    if (tr_n < TR_N && now < msg_from_us + 1500000) {
        memset(tr_snap[tr_n], 0, sizeof tr_snap[tr_n]);   // (whole rows, the component 166 ones too)
        for (int j = 0; j < 8; j++) coop_rd(a + 0x200 * (uint64_t)j, tr_snap[tr_n][TR_COMP + j], 0x200);   // the actor
        uint64_t c166 = coop_rd_ptr(t + 0x530);
        for (int j = 0; j < 4 && c166; j++) coop_rd(c166 + 0x200 * (uint64_t)(j + 1), tr_snap[tr_n][TR_COMP + 8 + j], 0x200);
        for (int j = 0; j < TR_COMP; j++) {
            uint64_t comp = coop_rd_ptr(t + 8 * (uint64_t)j);
            if (comp) coop_rd(comp, tr_snap[tr_n][j], 0x200);
        }
        uint64_t fl = on_flag(a);
        tr_snap[tr_n][TR_COMP + 7][0x1f8 / 8] = fl ? LD8(fl) : 0xff;   // (the on bool, in the actor's last word)
        tr_us[tr_n++] = now;
        return;
    }
    int said = 0;
    for (int j = 0; j < TR_COMP + 12; j++)
        for (int w = 0; w < 0x200 / 8 && said < 300; w++) {
            int changes = 0;
            for (int k = 1; k < tr_n; k++) changes += tr_snap[k][j][w] != tr_snap[k - 1][j][w];
            if (changes < 1 || changes > 3) continue;
            char line[300];
            int len = j >= TR_COMP + 8
                          ? snprintf(line, sizeof line, "coop: abtrace comp 166 +%03x: %016llx", (j - TR_COMP - 8 + 1) * 0x200 + w * 8, (unsigned long long)tr_snap[0][j][w])
                          : snprintf(line, sizeof line, "coop: abtrace %s %d +%03x: %016llx", j >= TR_COMP ? "actor" : "comp",
                                     j >= TR_COMP ? 0 : j, (j >= TR_COMP ? (j - TR_COMP) * 0x200 : 0) + w * 8, (unsigned long long)tr_snap[0][j][w]);
            for (int k = 1; k < tr_n && len < (int)sizeof line - 40; k++)
                if (tr_snap[k][j][w] != tr_snap[k - 1][j][w])
                    len += snprintf(line + len, sizeof line - (size_t)len, " | %.3f s %016llx", tr_us[k] / 1e6, (unsigned long long)tr_snap[k][j][w]);
            rt_log("%s", line);
            said++;
        }
    rt_log("coop: abtrace: %d snapshots of device %d from %.3f s to %.3f s, %d words logged", tr_n, tr_dev, tr_us[0] / 1e6,
           tr_us[tr_n - 1] / 1e6, said);
    tr_n = -1;
}
static void hit_test(Cpu *c, uint64_t me, int n) {
    static double at = -1, place_at;
    static int placed;
    static int64_t last;
    if (at < 0) {   // TOTK_COOP_TEST_ABHIT_B=<s>: the other game puts its Link by a copy fan at that time (its script swings)
        const char *e = cfg_get("TOTK_COOP_TEST_ABHIT"), *b = cfg_get("TOTK_COOP_TEST_ABHIT_B");
        at = e ? atof(e) : 0;
        place_at = coop_net_id() == 1 ? at : b ? atof(b) : 0;
    }
    if (at <= 0) return;
    double now = frame_step_world_us() / 1e6;
    if (now < at || now > at + 25) return;
    char nm[48];
    if (!placed && place_at > 0 && now >= place_at) {
        int fan = -1, wing = -1;
        for (int i = 0; i < n; i++) {
            if (!live_device(i, nm, sizeof nm)) continue;
            if (fan < 0 && !strcmp(nm, "SpObj_WindGenerator_A_01")) fan = i;
            if (wing < 0 && !strcmp(nm, "SpObj_LiftGeneratorWing_A_01")) wing = i;
        }
        float f[3], w[3];
        if (fan < 0 || !coop_rd(atomic_load(&tracked[fan]) + COOP_ACTOR_POS, f, 12)) return;
        if (wing < 0 || !coop_rd(atomic_load(&tracked[wing]) + COOP_ACTOR_POS, w, 12)) {   // no wing: from this Link's side
            if (!coop_rd(me + COOP_ACTOR_POS, w, 12)) return;
            for (int i = 0; i < 3; i++) w[i] = 2 * f[i] - w[i];
        }
        float ux = f[0] - w[0], uz = f[2] - w[2], l = sqrtf(ux * ux + uz * uz);
        if (l < 0.1f) ux = 1, uz = 0, l = 1;
        ux /= l, uz /= l;
        float dx = -ux, dz = -uz;   // facing the fan
        float m[12] = {dz, 0, dx, f[0] + ux, 0, 1, 0, f[1] - 0.2f, -dx, 0, dz, f[2] + uz};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        placed = 1;
        {   // TOTK_COOP_TEST_ABLOOSE=1: a loose fan made 1.6 m to this Link's right, for its script to push the build into
            // with Ultrahand (autobuild_glue_3rd.txt): glue by hand
            static _Alignas(16) uint8_t lh[0x40];
            if (cfg_get("TOTK_COOP_TEST_ABLOOSE")) {
                float at[3] = {f[0] + ux + dz * 1.6f, f[1] + 0.3f, f[2] + uz - dx * 1.6f}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                rt_log("coop: test: abhit: a loose fan asked for at %.2f %.2f %.2f: %d", at[0], at[1], at[2],
                       coop_request_create(c, "SpObj_WindGenerator_A_01", at, rot, lh));
            }
        }
        if (msg_from_us == INT64_MAX) msg_from_us = (int64_t)((now + 3.5) * 1e6), msg_to_us = (int64_t)((now + 6) * 1e6);   // (unless ABTRACE gave a time)
        {   // TOTK_COOP_TEST_FANHIT (coop_spawn.c) on the build's second fan, which the hit does not touch: the
            // functions entered with it as an argument just after the hit, not just before (the switch's path)
            extern uint64_t coop_fan_actor;
            int fans = 0;
            // (TOTK_COOP_TEST_FANHITDEV=1: the hit fan itself instead)
            int want = cfg_get("TOTK_COOP_TEST_FANHITDEV") ? 1 : 2;
            for (int i = 0; i < n; i++)
                if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_WindGenerator_A_01") && ++fans == want) coop_fan_actor = fanhit_target(atomic_load(&tracked[i]));
        }
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
    {   // the words of the first grouped device's group object (0x400 bytes) that changed since the last line
        static uint64_t prev[0x400 / 8], g_prev;
        uint64_t g = 0, cur[0x400 / 8];
        for (int i = 0; i < n && !g; i++) {
            uint64_t a = atomic_load(&tracked[i]), t = coop_rd_ptr(a + 0x228), comp = t ? coop_rd_ptr(t + 0x480) : 0;
            if (live_device(i, nm, sizeof nm) && comp) g = coop_rd_ptr(comp + 0x28);
        }
        if (g && coop_rd(g, cur, sizeof cur)) {
            if (g == g_prev)
                for (int k2 = 0; k2 < 0x400 / 8; k2++)
                    if (cur[k2] != prev[k2])
                        rt_log("coop: test: abhit: group %llx +%03x %016llx -> %016llx", (unsigned long long)g, k2 * 8,
                               (unsigned long long)prev[k2], (unsigned long long)cur[k2]);
            memcpy(prev, cur, sizeof prev), g_prev = g;
        }
    }
}

// TOTK_COOP_TEST_ABGLUE=1 (experiment, the game that did not build): 4 s after the first device is tracked here,
// each fan glued to the wing with the glue test's call (coop_glue.c coop_glue_pair).
static uint64_t glue_edges[8];
static int glue_edge_n;
static void glue_try(Cpu *c, int n, int64_t since) {
    static int done, later;
    if (done && !later && frame_step_world_us() - since > 6500000) {   // the same edges 2.5 s on
        later = 1;
        for (int i = 0; i < glue_edge_n; i++) dump("glue edge later", glue_edges[i], 0x120);
    }
    if (done || !cfg_get("TOTK_COOP_TEST_ABGLUE") || coop_net_id() == 1 || frame_step_world_us() - since < 4000000) return;
    done = 1;
    int coop_glue_pair(Cpu *c, uint64_t a, uint64_t b);
    char nm[48];
    uint64_t wing = 0;
    for (int i = 0; i < n && !wing; i++)
        if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_LiftGeneratorWing_A_01")) wing = atomic_load(&tracked[i]);
    for (int i = 0; i < n && wing; i++)
        if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_WindGenerator_A_01"))
        {
            extern uint64_t coop_glue_last_entry;
            int ok = coop_glue_pair(c, atomic_load(&tracked[i]), wing);
            uint64_t edge = coop_glue_last_entry ? coop_rd_ptr(coop_glue_last_entry + 0x560) : 0;
            rt_log("coop: test: abglue: device %d to the wing: %s, its edge %llx (vtable main+%llx)", i, ok ? "glued" : "not glued",
                   (unsigned long long)edge, (unsigned long long)(coop_rd_ptr(edge) - MOD_MAIN_BASE));
            if (edge) dump("glue edge", edge, 0x120);
            if (edge && glue_edge_n < 8) glue_edges[glue_edge_n++] = edge;
        }
}

// TOTK_COOP_TEST_ABPART=<s> (game A, the first joined): at that world time the build's last fan is taken off its
// contraption as Ultrahand's shake does: removeScotch (main+0x1c042bc: the fan's glue component, its id
// [actor+0x380], 0x2051000a), whose message makes every actor bonded to it break its bonds with it (main+0x1c0aefc).
static void part_test(Cpu *c, int n) {
    static double at = -1;
    static int done;
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_ABPART"); at = e ? atof(e) : 0; }
    if (done || at <= 0 || coop_net_id() != 1 || frame_step_world_us() / 1e6 < at) return;
    done = 1;
    char nm[48];
    int fan = -1;
    for (int i = 0; i < n; i++) if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_WindGenerator_A_01")) fan = i;
    if (fan < 0) { rt_log("coop: test: abpart: no fan"); return; }
    uint64_t a = atomic_load(&tracked[fan]), t = coop_rd_ptr(a + 0x228), comp = t ? coop_rd_ptr(t + 0x480) : 0;
    uint32_t id = 0;
    coop_rd(a + 0x380, &id, 4);
    if (comp) coop_guest_call(c, MOD_MAIN_BASE + 0x1c042bc, comp, id, 0x2051000a, 0);
    rt_log("coop: test: abpart: device %d (a fan) taken off its contraption (component %llx, group %llx before)", fan,
           (unsigned long long)comp, (unsigned long long)(comp ? coop_rd_ptr(comp + 0x28) : 0));
}

// TOTK_COOP_TEST_ABSTICK=<s>,<player>: at that world time the game that joined as <player> (1 or 2) puts its Link
// 0.7 m behind the build's steering stick (its own or its copy), facing the way the stick faces, for its script's A to
// take it; from then for 20 s, every 0.5 s, each tracked device's on state, in both games.
static void stick_test(Cpu *c, uint64_t me, int n) {
    static double at = -1;
    static int who, placed;
    static int64_t last;
    if (at < 0) {
        const char *e = cfg_get("TOTK_COOP_TEST_ABSTICK");
        at = 0;
        if (e && sscanf(e, "%lf,%d", &at, &who) != 2) at = 0;
    }
    double now = frame_step_world_us() / 1e6;
    if (at <= 0 || now < at || now > at + 20) return;
    char nm[48];
    if (!placed && coop_net_id() == who) {
        for (int i = 0; i < n && !placed; i++) {
            if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_ControlStick_A_01")) continue;
            uint64_t a = atomic_load(&tracked[i]);
            float p[3], r[9];
            if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36)) continue;
            float fx = r[2], fz = r[8], l = sqrtf(fx * fx + fz * fz);
            if (l < 0.1f) fx = 0, fz = 1, l = 1;
            fx /= l, fz /= l;
            float m[12] = {fz, 0, fx, p[0] - 0.7f * fx, 0, 1, 0, p[1] + 0.1f, -fx, 0, fz, p[2] - 0.7f * fz};
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
            placed = 1;
            rt_log("coop: test: abstick: this Link put behind the stick (device %d at %.2f %.2f %.2f)", i, p[0], p[1], p[2]);
        }
    }
    {   // TOTK_COOP_TEST_ABSTICKON=<s>: from then on, every tick, this game's own stick's on bool held at 1 (its change
        // count stepped once): whether the stick turns its contraption on without a Link holding it
        static double on_at = -1;
        static int stepped;
        if (on_at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_ABSTICKON"); on_at = e ? atof(e) : 0; }
        for (int i = 0; on_at > 0 && now >= on_at && i < n; i++) {
            if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_ControlStick_A_01")) continue;
            uint64_t a = atomic_load(&tracked[i]), tbl = coop_rd_ptr(a + 0x228), c0 = tbl ? coop_rd_ptr(tbl) : 0, bb = c0 ? coop_rd_ptr(c0 + 0x10) : 0;
            uint64_t f = on_flag(a);
            if (!f || !bb) continue;
            if (!LD8(f)) {
                ST8(f, 1);
                uint64_t chg = coop_rd_ptr(bb + 0x1f0);
                uint32_t nch = 0, idx = (uint32_t)(f - coop_rd_ptr(bb + 0x1a0));
                if (!stepped && chg && coop_rd(bb + 0x1e8, &nch, 4) && idx < nch) ST8(chg + idx, (uint8_t)(LD8(chg + idx) + 1)), stepped = 1;
                rt_log("coop: test: abstick: the stick's on bool set at %.2f s", now);
            }
            break;
        }
    }
    int64_t t = frame_step_world_us();
    if (t - last < 500000) return;
    last = t;
    char line[600];
    int k = snprintf(line, sizeof line, "coop: test: abstick: %.1f s:", now);
    for (int i = 0; i < n && k < (int)sizeof line - 40; i++) {
        if (!live_device(i, nm, sizeof nm) || strncmp(nm, "SpObj_", 6)) continue;
        uint32_t on11, e28;
        dev_on(atomic_load(&tracked[i]), &on11, &e28);
        k += snprintf(line + k, sizeof line - (size_t)k, " %d %s %s/%u", i, strstr(nm, "Wind") ? "fan" : strstr(nm, "Stick") ? "stick" : nm + 6,
                      on11 ? "on" : "off", e28);
        float sp[3];
        uint64_t sf = strstr(nm, "Stick") ? on_flag(atomic_load(&tracked[i])) : 0;
        if (strstr(nm, "Stick") && coop_rd(atomic_load(&tracked[i]) + COOP_ACTOR_POS, sp, 12))
            k += snprintf(line + k, sizeof line - (size_t)k, " (bool %d) at %.2f %.2f %.2f", sf ? LD8(sf) : -1, sp[0], sp[1], sp[2]);
    }
    {   // where the other players' stand-ins are here (in the game that does not hold the stick: at its copy?)
        int coop_player_positions(float (*out)[3], int max);   // coop_players.c
        float pp[4][3];
        int np = coop_player_positions(pp, 4);
        for (int j = 0; j < np && k < (int)sizeof line - 40; j++)
            k += snprintf(line + k, sizeof line - (size_t)k, " | stand-in %d at %.2f %.2f %.2f", j, pp[j][0], pp[j][1], pp[j][2]);
    }
    rt_log("%s", line);
}

// TOTK_COOP_TEST_ABMSG=<s>,<hex type>[,<s2>,<hex type2>...]: at each time, a message of that type sent to every live
// tracked fan, as the steering stick's AI node sends 0x6b510003 (main+0x2b76c34 (actor +0x378, {sender id, type, 0},
// 0)), the sender this Link: whether a message switches a device.
static void msg_test(Cpu *c, uint64_t me, int n) {
    const char *e = cfg_get("TOTK_COOP_TEST_ABMSG");
    static double at[8];
    static uint32_t type[8];
    static int k_n = -1, done[8];
    if (!e) return;
    if (k_n < 0) {
        k_n = 0;
        const char *p = e;
        while (k_n < 8 && sscanf(p, "%lf,%x", &at[k_n], &type[k_n]) == 2) {
            k_n++;
            for (int commas = 0; *p && commas < 2; p++) commas += *p == ',';
        }
    }
    double now = frame_step_world_us() / 1e6;
    char nm[48];
    for (int k = 0; k < k_n; k++) {
        if (done[k] || now < at[k]) continue;
        done[k] = 1;
        uint32_t sender = 0;
        coop_rd(me + 0x380, &sender, 4);
        int sent = 0;
        for (int i = 0; i < n; i++) {
            if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_WindGenerator_A_01")) continue;
            static _Alignas(16) uint8_t msg[16];
            memset(msg, 0, sizeof msg);
            memcpy(msg, &sender, 4), memcpy(msg + 4, &type[k], 4);
            coop_guest_call(c, MOD_MAIN_BASE + 0x2b76c34, atomic_load(&tracked[i]) + 0x378, (uint64_t)(uintptr_t)msg, 0, 0);
            sent++;
        }
        rt_log("coop: test: abmsg: message %08x sent to %d fans at %.2f s", type[k], sent, now);
    }
}

// TOTK_COOP_TEST_DEVHIT=<s>[,<s>...] (up to 8): at each time, the contact kept from the first hit on a device here
// (coop_world.c coop_dev_hit) put on the build's first fan: whether a contact alone starts (or stops) it and its
// contraption.
static void devhit_test(int n) {
    const char *e = cfg_get("TOTK_COOP_TEST_DEVHIT");
    static double at[8];
    static int k_n = -1, done[8];
    if (!e) return;
    if (k_n < 0) {
        k_n = 0;
        for (const char *p = e; k_n < 8 && *p;) {
            at[k_n++] = atof(p);
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
    }
    double now = frame_step_world_us() / 1e6;
    char nm[48];
    int coop_dev_hit(uint64_t device);   // coop_world.c
    for (int k = 0; k < k_n; k++) {
        if (done[k] || now < at[k]) continue;
        done[k] = 1;
        int pick = -1;   // the first fan of a contraption that is off (the two-contraption test's unhit one), else the first fan
        for (int i = 0; i < n; i++) {
            if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_WindGenerator_A_01")) continue;
            uint64_t a = atomic_load(&tracked[i]), t = coop_rd_ptr(a + 0x228), comp = t ? coop_rd_ptr(t + 0x480) : 0;
            uint32_t on11, e28;
            dev_on(a, &on11, &e28);
            if (pick < 0) pick = i;
            if (!on11 && comp && coop_rd_ptr(comp + 0x28)) pick = i;   // (the last such: the second contraption's)
        }
        const char *bits = cfg_get("TOTK_COOP_TEST_DEV8D");   // =<hex>: instead, those bits set in its component 166 +0x8d
        if (pick >= 0 && bits) {
            devhit_pick = pick;
            // (TOTK_COOP_TEST_DEV8DALL=1: every live tracked device of its contraption that is on as it is, at once)
            uint64_t pa = atomic_load(&tracked[pick]), pt = coop_rd_ptr(pa + 0x228), pc = pt ? coop_rd_ptr(pt + 0x480) : 0;
            uint64_t pg = pc ? coop_rd_ptr(pc + 0x28) : 0, done_a[TRACK_N];
            uint32_t pon, pe;
            dev_on(pa, &pon, &pe);
            int nd = 0, all = cfg_get("TOTK_COOP_TEST_DEV8DALL") != NULL;
            for (int i = 0; i < n; i++) {
                if (i != pick && (!all || !live_device(i, nm, sizeof nm))) continue;
                uint64_t a = atomic_load(&tracked[i]), t = coop_rd_ptr(a + 0x228), c166 = t ? coop_rd_ptr(t + 0x530) : 0;
                uint64_t gc = t ? coop_rd_ptr(t + 0x480) : 0;
                uint32_t on, e;
                dev_on(a, &on, &e);
                if (i != pick && (!pg || !gc || coop_rd_ptr(gc + 0x28) != pg || on != pon)) continue;
                int dup = 0;
                for (int j = 0; j < nd; j++) dup |= done_a[j] == a;
                uint8_t b = 0, b60 = 0;
                if (dup || !c166 || !coop_rd(c166 + 0x8d, &b, 1) || !coop_rd(c166 + 0x60, &b60, 1)) continue;
                done_a[nd++] = a;
                ST8(c166 + 0x8d, (uint8_t)(b | strtoul(bits, NULL, 16)));
                rt_log("coop: test: dev8d: %.2f s: device %d's component 166 +0x8d %02x -> %02x (+0x60 %02x)", now, i, b,
                       (unsigned)(b | strtoul(bits, NULL, 16)), b60);
            }
        } else if (pick >= 0) {
            devhit_pick = pick;
            extern uint64_t coop_fan_actor;   // coop_spawn.c (TOTK_COOP_TEST_FANHIT)
            if (cfg_get("TOTK_COOP_TEST_FANHIT")) coop_fan_actor = fanhit_target(atomic_load(&tracked[pick]));
            uint32_t on11, e28;
            dev_on(atomic_load(&tracked[pick]), &on11, &e28);
            rt_log("coop: test: devhit: %.2f s: device %d (on %#x) %s", now, pick, on11,
                   coop_dev_hit(atomic_load(&tracked[pick])) ? "asked" : "asked before a contact is kept (put on at the first one)");
        }
    }
}

// Every 2 s while any device is tracked (for 60 s): each one's name, state, flags and glue entries.
void coop_autobuild_tick(Cpu *c, uint64_t me) {
    if (!ablog_on()) return;
    static int64_t last, first;
    int64_t now = frame_step_world_us();
    {   // TOTK_COOP_TEST_ABPLACE=<s>: game A's Link put on the flat ground of the fan tests (coop_players.c fanset_test),
        // facing -x, at that world time: things made in front of it there stay put (on the stairs they rolled away)
        static int done;
        const char *e = cfg_get("TOTK_COOP_TEST_ABPLACE");
        if (e && !done && coop_net_id() == 1 && now / 1e6 >= atof(e)) {
            float m9[12] = {0, 0, -1, 31.3f, 0, 1, 0, 1514.8f, 1, 0, 0, 1510.7f};
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m9, 0, 0);
            done = 1;
        }
    }
    int n = atomic_load(&tracked_n);
    if (n > TRACK_N) n = TRACK_N;
    if (n) hit_test(c, me, n), part_test(c, n), trace_tick(n), stick_test(c, me, n), msg_test(c, me, n), devhit_test(n);
    else if ((cfg_get("TOTK_COOP_TEST_ABHIT") || cfg_get("TOTK_COOP_TEST_ABSTICK")) && now > 20000000) {   // (near_dev_fn)
        static NearDev d;
        static int64_t last_scan;
        if (now - last_scan > 1000000) {
            last_scan = now;
            void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
            d.n = 0;
            coop_rd(me + COOP_ACTOR_POS, d.me, 12);
            coop_actors_all(near_dev_fn, &d);
            for (int i = 0; i < d.n && atomic_load(&tracked_n) < TRACK_N; i++) atomic_store(&tracked[atomic_fetch_add(&tracked_n, 1)], d.a[i]);
        }
    }
    {   // TOTK_COOP_TEST_ABWATCH=<hex address>,<length>,<from s>,<to s>: a write watch on that guest range (pooled game
        // objects sit at the same address run after run), in place of the group-field watch below
        static int st;
        static double from, to;
        static unsigned long long at, len;
        const char *e = cfg_get("TOTK_COOP_TEST_ABWATCH");
        if (e && st == 0 && sscanf(e, "%llx,%llx,%lf,%lf", &at, &len, &from, &to) == 4) st = 1;
        // (=c166,<from s>,<to s>: the first live fan's component 166 +0x28, its state, which a hit sets 8 -> 4)
        if (e && st == 0 && !strncmp(e, "c166,", 5) && sscanf(e + 5, "%lf,%lf", &from, &to) == 2) st = 4;
        if (st == 4 && now / 1e6 >= from) {
            char nm[48];
            for (int i = 0; i < n && st == 4; i++) {
                if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_WindGenerator_A_01")) continue;
                uint64_t t = coop_rd_ptr(atomic_load(&tracked[i]) + 0x228), c166 = t ? coop_rd_ptr(t + 0x530) : 0;
                if (c166) at = c166 + 0x28, len = 8, st = 1, rt_log("coop: test: abwatch: device %d's component 166 +0x28 (%llx)", i, at);
            }
        }
        int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
        void coop_watch_disarm(void);
        extern int coop_watch_writes;
        if (st == 1 && now / 1e6 >= from) { coop_watch_writes = 1; st = coop_watch_arm(at, len) ? 2 : 3; }
        if (st == 2 && now / 1e6 >= to) { coop_watch_disarm(); st = 3; }
    }
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
    if (watch_stage == 0 && cfg_get("TOTK_COOP_TEST_ABWATCH")) watch_stage = 2;
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
