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
#include "coop_ctn.h"
#include "coop_ctr.h"
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
            if (!strcmp(nm, "SpObj_WindGenerator_A_01")) {
                // (TOTK_COOP_TEST_ABHIT_FANX=1: the fan furthest along +x, the same one in every run, not the first found)
                float pa[3], pb[3];
                if (fan < 0) fan = i;
                else if (cfg_get("TOTK_COOP_TEST_ABHIT_FANX") && coop_rd(atomic_load(&tracked[i]) + COOP_ACTOR_POS, pa, 12) &&
                         coop_rd(atomic_load(&tracked[fan]) + COOP_ACTOR_POS, pb, 12) && pa[0] > pb[0])
                    fan = i;
            }
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
        // (TOTK_COOP_TEST_ABHIT_DIST=<m>: that far from it instead of 1 m; a stand-in put inside the build shoves it)
        const char *ds = cfg_get("TOTK_COOP_TEST_ABHIT_DIST");
        float dist = ds ? (float)atof(ds) : 1.0f;
        float m[12] = {dz, 0, dx, f[0] + ux * dist, 0, 1, 0, f[1] - 0.2f, -dx, 0, dz, f[2] + uz * dist};
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

// TOTK_COOP_TEST_ABPART=<s>[,<player>] (by default game A, the first joined): at that world time the build's last
// fan (in game B: a fan of its copy) is taken off its
// contraption as Ultrahand's shake does: removeScotch (main+0x1c042bc: the fan's glue component, its id
// [actor+0x380], 0x2051000a), whose message makes every actor bonded to it break its bonds with it (main+0x1c0aefc).
static void part_test(Cpu *c, int n) {
    static double at = -1;
    static int done, who = 1;
    if (at < 0) {
        const char *e = cfg_get("TOTK_COOP_TEST_ABPART"), *k = e ? strchr(e, ',') : NULL;
        at = e ? atof(e) : 0, who = k ? atoi(k + 1) : 1;
    }
    if (done || at <= 0 || coop_net_id() != who || frame_step_world_us() / 1e6 < at) return;
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
// TOTK_COOP_TEST_DEVSWEEP=<s> (with ABLOG, any idle script): from then, every 10 s, one Zonai device kind made loose 3 m
// in front of this Link (each 2.5 m to the side of the last); 3 s on, component 166's switch (+0x8d bit 3) set, again
// at 6 s, its state logged before each and at 9 s: which kinds the switch turns on and off (re-notes table).
static const char *const sweep_kind[] = {
    "SpObj_WindGenerator_A_01", "SpObj_FastWheel_A_01", "SpObj_Rocket_A_01", "SpObj_FlameThrower_A_01", "SpObj_SnowMachine_A_01",
    "SpObj_ElectricBoxGenerator", "SpObj_Beamos_A_01", "SpObj_FlashLight_A_01", "SpObj_Cannon_A_01", "SpObj_BalloonEnvelope_A_01",
    "SpObj_SpringPiston_A_01", "SpObj_SlipBoard_A_01", "SpObj_ControlStick_A_01", "SpObj_EnergyBank_A_01", "SpObj_Cart_A_01",
    "SpObj_LiftGeneratorWing_A_01", "SpObj_WaterPump_A_01", "SpObj_TimerBomb_A_01"};
#define SWEEP_N (int)(sizeof sweep_kind / sizeof *sweep_kind)
static void sweep_log(uint64_t a, const char *kind, const char *when) {
    uint64_t t = a ? coop_rd_ptr(a + 0x228) : 0, c166 = t ? coop_rd_ptr(t + 0x530) : 0, c11 = t ? coop_rd_ptr(t + 8 * 11) : 0;
    uint8_t b60 = 0, b8d = 0;
    uint32_t on11 = 0, e28 = 0, s64 = 0;
    int32_t st = 0;
    if (c166) coop_rd(c166 + 0x60, &b60, 1), coop_rd(c166 + 0x8d, &b8d, 1), coop_rd(c166 + 0x64, &s64, 4);
    dev_on(a, &on11, &e28);
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    uint64_t fl = a ? on_flag(a) : 0;
    rt_log("coop: test: devsweep: %s %s: state %d, component 166 %s (+0x60 %02x, +0x64 %u, +0x8d %02x), component 11 %s, on %#x, energy %u, on bool %d",
           kind, when, st, c166 ? "yes" : "none", b60, s64, b8d, c11 ? "yes" : "none", on11, e28, fl ? LD8(fl) : -1);
}
static void sweep_test(Cpu *c, uint64_t me) {
    static double at = -1;
    static const char *kinds[32];   // TOTK_COOP_TEST_DEVSWEEPKINDS=<name>,<name>...: those instead, each 3 m ahead
    static int kinds_n = -1;
    static char kbuf[512];
    if (kinds_n < 0) {
        kinds_n = 0;
        const char *e = cfg_get("TOTK_COOP_TEST_DEVSWEEPKINDS");
        if (e) {
            snprintf(kbuf, sizeof kbuf, "%s", e);
            for (char *t = strtok(kbuf, ","); t && kinds_n < 32; t = strtok(NULL, ",")) kinds[kinds_n++] = t;
        }
    }
    const char *const *kind = kinds_n ? kinds : sweep_kind;
    int kind_n = kinds_n ? kinds_n : SWEEP_N;
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_DEVSWEEP"); at = e ? atof(e) : 0; }
    if (at <= 0) return;
    double now = frame_step_world_us() / 1e6;
    static int k = 0, phase = 0;
    static uint64_t a;
    static _Alignas(16) uint8_t h[0x40], link[0x40];
    if (k >= kind_n || now < at + 10 * k) return;
    double t = now - (at + 10 * k);
    if (phase == 0) {
        float p[3], r[9], rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
        float fx = r[2], fz = r[8], side = kinds_n ? (float)(k - kind_n / 2) * 1.8f : (float)(k % 6 - 3) * 2.2f, fwd = kinds_n ? 3 : 3 + (float)(k / 6) * 2.5f;
        float pos[3] = {p[0] + fwd * fx + side * fz, p[1] + 0.3f, p[2] + fwd * fz - side * fx};
        memset(h, 0, sizeof h);
        rt_log("coop: test: devsweep: %s asked for at %.2f %.2f %.2f: %d", kind[k], pos[0], pos[1], pos[2], coop_request_create(c, kind[k], pos, rot, h));
        a = 0, phase = 1;
    } else if (phase == 1) {
        if (coop_handle_status(h) == 3) {
            uint64_t ref;
            a = coop_claim(c, h, link, &ref), phase = 2;
        } else if (t > 3) {
            rt_log("coop: test: devsweep: %s not made (status %d)", kind[k], coop_handle_status(h));
            k++, phase = 0;
        }
    } else if (phase >= 2 && phase <= 3 && t >= 3 * (phase - 1)) {
        sweep_log(a, kind[k], phase == 2 ? "made, before the switch" : "3 s after the switch, before the second");
        uint64_t tt = a ? coop_rd_ptr(a + 0x228) : 0, c166 = tt ? coop_rd_ptr(tt + 0x530) : 0;
        float ap[3], ar[9], q[4];   // (TOTK_COOP_TEST_DEVSWEEPNUDGE=1: first moved up 5 cm by speed, to wake it)
        void coop_obj_force(Cpu *c, uint64_t a, const float pos[3], const float q[4]);
        void coop_mat_quat(const float r[9], float q[4]);
        if (cfg_get("TOTK_COOP_TEST_DEVSWEEPNUDGE") && a && coop_rd(a + COOP_ACTOR_POS, ap, 12) && coop_rd(a + COOP_ACTOR_ROT, ar, 36))
            ap[1] += 0.05f, coop_mat_quat(ar, q), coop_obj_force(c, a, ap, q);
        if (c166) ST8(c166 + 0x8d, (uint8_t)(LD8(c166 + 0x8d) | 0x08));
        phase++;
    } else if (phase == 4 && t >= 9) {
        sweep_log(a, kind[k], "3 s after the second switch");
        k++, phase = 0;
    }
}

// TOTK_COOP_TEST_GLUENEAR=<s>[,<m>] (with autobuild_glue_3rd.txt-style scripts, which hold the build with Ultrahand from
// 40.5 s and press A at 43.5 s): at that time a loose fan is made <m> (1.0) beyond the tracked device farthest to this
// Link's right, at its height: something for the held build to be glued to by hand (the release's own glue).
static void gluenear_test(Cpu *c, uint64_t me, int n) {
    static double at = -1, gap = 1.0;
    static _Alignas(16) uint8_t h[0x40];
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_GLUENEAR"); at = 0; if (e && sscanf(e, "%lf,%lf", &at, &gap) < 1) at = 0; }
    if (at <= 0 || frame_step_world_us() / 1e6 < at) return;
    at = 0;
    float lp[3], r[9];
    if (!coop_rd(me + COOP_ACTOR_POS, lp, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    float rx = r[0], rz = r[6], best = -1e9f, bp[3] = {0};
    char nm[48];
    for (int i = 0; i < n; i++) {
        float p[3];
        if (!live_device(i, nm, sizeof nm) || strncmp(nm, "SpObj_", 6) || !coop_rd(atomic_load(&tracked[i]) + COOP_ACTOR_POS, p, 12)) continue;
        float d = (p[0] - lp[0]) * rx + (p[2] - lp[2]) * rz;
        if (d > best) best = d, memcpy(bp, p, 12);
    }
    if (best < -1e8f) return;
    float pos[3] = {bp[0] + rx * (float)gap, bp[1], bp[2] + rz * (float)gap}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    rt_log("coop: test: gluenear: a loose fan asked for at %.2f %.2f %.2f (%.1f m right of the build's right-most part): %d", pos[0], pos[1],
           pos[2], gap, coop_request_create(c, "SpObj_WindGenerator_A_01", pos, rot, h));
}

// TOTK_COOP_TEST_COMPDUMP=<s>,<comp>,<comp>...: at that time, those components (0x200 bytes each, by index in
// [actor+0x228]) of the first live tracked fan, as words, for a diff between an autobuilt fan and its copy
static void compdump_test(int n) {
    const char *e = cfg_get("TOTK_COOP_TEST_COMPDUMP");
    static int done;
    if (!e || done) return;
    double at = atof(e);
    if (frame_step_world_us() / 1e6 < at) return;
    char nm[48];
    for (int i = 0; i < n; i++) {
        if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_WindGenerator_A_01")) continue;
        uint64_t a = atomic_load(&tracked[i]), t = coop_rd_ptr(a + 0x228);
        done = 1;
        for (int ci = 0; ci < 256 && t; ci++) {   // each component's name ([comp+8], a string)
            uint64_t comp = coop_rd_ptr(t + 8 * (uint64_t)ci);
            char nm2[64] = "";
            if (comp && coop_rd(coop_rd_ptr(comp + 8), nm2, 63) && nm2[0] > 0x20 && nm2[0] < 0x7f) rt_log("coop: test: compname %d %s", ci, nm2);
        }
        for (const char *p = strchr(e, ','); p && t; p = strchr(p + 1, ',')) {
            int ci = atoi(p + 1);
            uint64_t comp = coop_rd_ptr(t + 8 * (uint64_t)ci), w[64] = {0};
            if (!comp || !coop_rd(comp, w, sizeof w)) { rt_log("coop: test: compdump comp %d none", ci); continue; }
            for (int r = 0; r < 64; r += 8)
                rt_log("coop: test: compdump comp %d +%03x: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx", ci, r * 8,
                       (unsigned long long)w[r], (unsigned long long)w[r + 1], (unsigned long long)w[r + 2], (unsigned long long)w[r + 3],
                       (unsigned long long)w[r + 4], (unsigned long long)w[r + 5], (unsigned long long)w[r + 6], (unsigned long long)w[r + 7]);
        }
        return;
    }
}

// TOTK_COOP_TEST_REBUILD=<s> (with TOTK_COOP_TEST_CREATEDUMP, after an autobuild): main+0x1d3b470, the build step the
// Autobuild ability's update (main+0x9a2c00) runs, called again at that time on the object it ran on: whether the
// game's own build can be run from code (the other game's copies made as the game makes them)
uint64_t coop_ab_obj;
uint8_t coop_ab_cai[0x1820];   // the whole CmbAct: parts from +0xc, bond records from +0x1704
// The ability update's preparation, replayed (main+0x9a2c00 before its call at main+0x9a70ec): the blueprint put back
// as it was at the build (its part count, +9, reads 0 after it), the build's entry pool released (+0x4d10 counter
// stepped, +0x4d10+0x220 cleared, the +0x4f48 used entries put back on the free list at +0x4f58), then per part an entry
// from the pool at +0x1b0 (main+0x1d3cfc8) with the record's +0x98 and +0xd8, its +0x90 and +0x94, the record's +0xd7
// and +0x117 cleared; then main+0x1d3b470.
// TOTK_COOP_TEST_ABSAVE=<s>,<file> (game A, after a build kept with TOTK_COOP_TEST_CREATEDUMP=1): at that time its
// blueprint (the whole CmbAct) and the build's base matrix (BuildObject +0x120) written to <file>.
// TOTK_COOP_TEST_ABLOAD=<s>,<file> (game B, which has built nothing): at that time this game's own Autobuild build
// object ([ability +0x51f0], the ability caught in its update) taken as the one to build with, the file's blueprint
// kept as if built here and its base matrix put in the object, so TOTK_COOP_TEST_REBUILD and REBUILDGO make A's
// contraption here. A test of building another game's contraption with the game's own Autobuild; the file stands in
// for the network.
extern _Atomic uint64_t coop_ab_ability;
static void abfile_test(void) {
    for (int mode = 0; mode < 2; mode++) {
        static double at[2] = {-1, -1};
        static char path[2][260];
        const char *name = mode ? "TOTK_COOP_TEST_ABLOAD" : "TOTK_COOP_TEST_ABSAVE";
        if (at[mode] < 0) {
            const char *e = cfg_get(name), *k = e ? strchr(e, ',') : NULL;
            at[mode] = e && k ? atof(e) : 0;
            if (k) snprintf(path[mode], sizeof path[mode], "%s", k + 1);
        }
        if (at[mode] <= 0 || frame_step_world_us() / 1e6 < at[mode]) continue;
        at[mode] = 0;
        float m[12];
        if (!mode) {
            if (!coop_ab_obj || !coop_ab_cai[9] || !coop_rd(coop_ab_obj + 0x120, m, sizeof m)) { rt_log("coop: test: absave: no build kept"); continue; }
            FILE *f = fopen(path[0], "wb");
            int ok = f && fwrite(coop_ab_cai, sizeof coop_ab_cai, 1, f) == 1 && fwrite(m, sizeof m, 1, f) == 1;
            if (f) fclose(f);
            rt_log("coop: test: absave: %s: blueprint (%u parts, %u bonds) and base at %.2f %.2f %.2f: %d", path[0], coop_ab_cai[9], coop_ab_cai[10], m[3], m[7], m[11], ok);
            continue;
        }
        uint64_t ab = atomic_load(&coop_ab_ability), o = ab ? coop_rd_ptr(ab + 0x51f0) : 0;
        FILE *f = fopen(path[1], "rb");
        int ok = f && fread(coop_ab_cai, sizeof coop_ab_cai, 1, f) == 1 && fread(m, sizeof m, 1, f) == 1;
        if (f) fclose(f);
        if (!ok || !o) { rt_log("coop: test: abload: %s read %d, build object %llx: nothing", path[1], ok, (unsigned long long)o); continue; }
        if (!coop_rd_ptr(o + 0x3c50)) ST64(o + 0x3c50, o + 0xc50);   // (the blueprint buffer, as a load sets it)
        memcpy((void *)(uintptr_t)(o + 0x120), m, sizeof m);
        coop_ab_obj = o;
        rt_log("coop: test: abload: %s: blueprint (%u parts, %u bonds) and base %.2f %.2f %.2f into this game's build object %llx (blueprint at %llx)",
               path[1], coop_ab_cai[9], coop_ab_cai[10], m[3], m[7], m[11], (unsigned long long)o, (unsigned long long)coop_rd_ptr(o + 0x3c50));
    }
}
static void rebuild_test(Cpu *c) {
    static double at = -1;
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_REBUILD"); at = e ? atof(e) : 0; }
    if (at <= 0 || frame_step_world_us() / 1e6 < at) return;
    at = 0;
    uint64_t o = coop_ab_obj, cai = o ? coop_rd_ptr(o + 0x3c50) : 0;
    if (!cai || !coop_ab_cai[9]) { rt_log("coop: test: rebuild: no build kept (object %llx)", (unsigned long long)o); return; }
    int n = coop_ab_cai[9];
    memcpy((void *)(uintptr_t)cai, coop_ab_cai, sizeof coop_ab_cai);
    {   // TOTK_COOP_TEST_REBUILDBASE=<m>: the build's base matrix (BuildObject +0x120, three rows [R|t]) moved <m> along z,
        // so the parts are made beside the first build (main+0x1d3b470 composes it with each record's frame 0)
        const char *bs = cfg_get("TOTK_COOP_TEST_REBUILDBASE");
        float m[12];
        if (bs && coop_rd(o + 0x120, m, sizeof m)) {
            rt_log("coop: test: rebuild: base matrix t %.2f %.2f %.2f, moved %s m along z", m[3], m[7], m[11], bs);
            m[11] += (float)atof(bs);
            memcpy((void *)(uintptr_t)(o + 0x120), m, sizeof m);
        }
    }
    {   // TOTK_COOP_TEST_REBUILDDX=<m>: each float triplet in the records within 15 m of this Link moved <m> along x (the
        // parts' places, wherever the record keeps them), so the second build stands beside the first
        const char *dxs = cfg_get("TOTK_COOP_TEST_REBUILDDX");
        uint64_t me = coop_player_actor();
        float lp[3];
        int moved = 0;
        if (dxs && me && coop_rd(me + COOP_ACTOR_POS, lp, 12))
            for (int w = 0; w + 12 <= 0x5000; w += 4) {   // (the records keep places relative to the build: its object then)
                uint64_t fa = o + (uint64_t)w;
                if (fa >= cai && fa < cai + 0xc + (uint64_t)n * 0x118) continue;
                float f[3];
                if (!coop_rd(fa, f, 12) || !(fabsf(f[0] - lp[0]) < 15 && fabsf(f[1] - lp[1]) < 15 && fabsf(f[2] - lp[2]) < 15)) continue;
                rt_log("coop: test: rebuild: the build's object +%#x: %.2f %.2f %.2f moved", w, f[0], f[1], f[2]);
                f[0] += (float)atof(dxs), moved++;
                memcpy((void *)(uintptr_t)fa, f, 4);
                w += 8;
            }
        if (dxs) rt_log("coop: test: rebuild: %d places moved by %s m", moved, dxs);
    }
    uint64_t pc = o + 0x4d10, pu = o + 0x4f48;
    uint32_t ctr = LD32(pc), used = LD32(pu);
    ST64(pc + 0x220, 0);
    ctr++;
    ST32(pc, ctr);
    if ((int32_t)used >= 1) {
        uint64_t head = LD64(o + 0x4f58);
        for (uint32_t k = 0; k < used; k++) {
            uint64_t ent = LD64(LD64(o + 0x4f50) + 8 * (uint64_t)k);
            ST64(ent, head);
            ST64(o + 0x4f58, ent);
            head = ent;
        }
    }
    ST32(pu, 0);
    if (ctr == 0x7fffffff) ST32(pc, 0);
    int got = 0;
    for (int k = 0; k < n; k++) {
        uint64_t e = coop_guest_call(c, MOD_MAIN_BASE + 0x1d3cfc8, o + 0x1b0, 0, 0, 0);
        if (!e) continue;
        uint64_t r = cai + 0xc + (uint64_t)k * 0x118;
        ST8(r + 0xd7, 0), ST64(e, r + 0x98), ST8(r + 0x117, 0), ST64(e + 8, r + 0xd8);
        ST32(e + 0x10, LD32(r + 0x90)), ST32(e + 0x14, LD32(r + 0x94));
        got++;
    }
    rt_log("coop: test: rebuild: blueprint put back (%d parts), %d pool entries; main+0x1d3b470 on %llx", n, got, (unsigned long long)o);
    coop_guest_call(c, MOD_MAIN_BASE + 0x1d3b470, o, 0, 0, 0);
    rt_log("coop: test: rebuild: returned");
}
// TOTK_COOP_TEST_REBUILDOK=<s>: the confirm the ability's update runs when the build is confirmed (main+0x9a2c00 at
// main+0x9a7260: main+0x1d3c0d4 on [ability+0x51f0], the build's object), called on the kept build object then
static void rebuild_ok(Cpu *c) {
    static double at = -1;
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_REBUILDOK"); at = e ? atof(e) : 0; }
    if (at <= 0 || frame_step_world_us() / 1e6 < at || !coop_ab_obj) return;
    at = 0;
    // its first check (main+0x9a83dc) fails at once while the object's byte +0x3c70 is set; else it walks the part
    // entries (+0x1b0 count, +0x1b8 list). TOTK_COOP_TEST_REBUILDOK70=1: that byte cleared first
    uint8_t b70 = 0;
    coop_rd(coop_ab_obj + 0x3c70, &b70, 1);
    int32_t ne = 0;
    coop_rd(coop_ab_obj + 0x1b0, &ne, 4);
    rt_log("coop: test: rebuild: before the confirm: +0x3c70 %u, part entries %d", b70, ne);
    if (b70 && cfg_get("TOTK_COOP_TEST_REBUILDOK70")) ST8(coop_ab_obj + 0x3c70, 0);
    {   // the confirm reads the blueprint's +9 (parts) and +0xa (3 for the Fanplane: its bonds, it seems), both zeroed again
        // by the build step: put back as at the build
        uint64_t cai = coop_rd_ptr(coop_ab_obj + 0x3c50);
        if (cai && coop_ab_cai[9]) memcpy((void *)(uintptr_t)cai, coop_ab_cai, sizeof coop_ab_cai);
    }
    // TOTK_COOP_TEST_REBUILDCLAIM=1: first each part's create handle (its pool entry +0x60, which main+0x1d3b470 gives
    // requestCreateActor) put in the world (coop_claim): the new parts stayed in state 3, made and not put in
    static _Alignas(16) uint8_t links[0x15][0x40];
    uint64_t list = coop_rd_ptr(coop_ab_obj + 0x1b8);
    for (int k = 0; cfg_get("TOTK_COOP_TEST_REBUILDCLAIM") && list && k < ne && k < 0x15; k++) {
        uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)k), ref = 0;
        if (!ent) continue;
        int st = coop_handle_status((const uint8_t *)(uintptr_t)(ent + 0x60));
        uint64_t a = st == 3 ? coop_claim(c, (uint8_t *)(uintptr_t)(ent + 0x60), links[k], &ref) : 0;
        char nm[48] = "";
        rt_log("coop: test: rebuild: part %d's handle status %d: put in the world %s %llx", k, st, a ? coop_actor_name(a, nm, sizeof nm) : "-", (unsigned long long)a);
    }
    uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + 0x1d3c0d4, coop_ab_obj, 0, 0, 0);
    rt_log("coop: test: rebuild: the confirm (main+0x1d3c0d4) on %llx returned %llx", (unsigned long long)coop_ab_obj, (unsigned long long)r);
}

// TOTK_COOP_TEST_FAILSPAM=<s>[,<n>]: from then, a MergedActorD100 asked for again each time its request fails
// (handle status 4), as copies of one were in a play session, <n> (2000) times; each failure released
// (coop_handle_release; a no-op with TOTK_COOP_TEST_NORELEASE=1, the old way). With DEVSWEEP: whether this game
// can still make an actor after.
static void failspam_test(Cpu *c, uint64_t me) {
    static double at = -1;
    static int left, fails, st0;
    static _Alignas(16) uint8_t h[0x40];
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_FAILSPAM"); at = 0, left = 2000; if (e) at = atof(e), sscanf(e, "%*f,%d", &left); }
    if (at <= 0 || frame_step_world_us() / 1e6 < at || left <= 0) return;
    int st = coop_handle_status(h);
    if (st == 4 || !st0) {
        if (st == 4) fails++, coop_handle_release(c, h), memset(h, 0, 8);   // (the old way leaves the record: see above)
        float p[3], rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
        p[0] += 3;
        int ok = coop_request_create(c, "MergedActorD100", p, rot, h);
        st0 = 1, left--;
        if (fails % 200 == 0 || !ok) rt_log("coop: test: failspam: %d failed so far; asked again: %d", fails, ok);
        if (!left) rt_log("coop: test: failspam: done, %d failed", fails);
    } else if (st == 3) {
        rt_log("coop: test: failspam: a MergedActorD100 was made (status 3): stopped");
        left = 0;
    }
}

// TOTK_COOP_TEST_ABDECK=<s>[,<name>]: at that time this Link put 1.2 m above the first live tracked device of that name
// (SpObj_ControlStick_A_01: a contraption's deck), 0.8 m behind it; then for 25 s, every 0.25 s, its place against that
// device's (in the device's frame) and how much that moved since the last: a rider on a contraption another player
// drives (stays on? jitters?).
static void deck_test(Cpu *c, uint64_t me, int n) {
    static double at = -1;
    static char want[48] = "SpObj_ControlStick_A_01";
    static int dev = -1;
    static float last[3];
    static double next;
    if (at < 0) { const char *e = cfg_get("TOTK_COOP_TEST_ABDECK"); at = 0; if (e) at = atof(e), sscanf(e, "%*f,%47s", want); }
    double now = frame_step_world_us() / 1e6;
    if (at <= 0 || now < at || now > at + 25) return;
    char nm[48];
    if (dev < 0) {
        for (int i = 0; i < n && dev < 0; i++) if (live_device(i, nm, sizeof nm) && !strcmp(nm, want)) dev = i;
        if (dev < 0) return;
        float p[3], r[9];
        uint64_t a = atomic_load(&tracked[dev]);
        if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36)) { dev = -1; return; }
        float m[12] = {1, 0, 0, p[0] - 0.8f * r[2], 0, 1, 0, p[1] + 1.2f, 0, 0, 1, p[2] - 0.8f * r[8]};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        rt_log("coop: test: abdeck: this Link put on device %d (%s) at %.2f %.2f %.2f", dev, want, p[0], p[1], p[2]);
        next = now + 0.25;
        return;
    }
    if (now < next) return;
    next = now + 0.25;
    float p[3], r[9], lp[3];
    uint64_t a = atomic_load(&tracked[dev]);
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36) || !coop_rd(me + COOP_ACTOR_POS, lp, 12)) return;
    float d[3] = {lp[0] - p[0], lp[1] - p[1], lp[2] - p[2]}, rel[3];
    for (int i = 0; i < 3; i++) rel[i] = r[0 + i] * d[0] + r[3 + i] * d[1] + r[6 + i] * d[2];   // in the device's frame (rows)
    float dm = sqrtf((rel[0] - last[0]) * (rel[0] - last[0]) + (rel[1] - last[1]) * (rel[1] - last[1]) + (rel[2] - last[2]) * (rel[2] - last[2]));
    rt_log("coop: test: abdeck: %.2f s: device at %.2f %.2f %.2f; this Link in its frame %.2f %.2f %.2f (moved %.2f m)", now, p[0], p[1], p[2],
           rel[0], rel[1], rel[2], dm);
    memcpy(last, rel, 12);
}

// TOTK_COOP_TEST_PLAYERDIFF=<s1>,<s2>: this Link's actor (0x1000 bytes) and components (0x400 each) kept at s1 and s2;
// each 32-bit word that changed is logged as an int and a float: where the Link's battery lives (Zonai devices it ran
// in between drew it down)
static void playerdiff_test(uint64_t me) {
    static double t1 = -1, t2;
    static uint8_t (*snap)[257][0x400];
    static int stage;
    if (t1 < 0) { const char *e = cfg_get("TOTK_COOP_TEST_PLAYERDIFF"); t1 = 0; if (e && sscanf(e, "%lf,%lf", &t1, &t2) != 2) t1 = 0; }
    double now = frame_step_world_us() / 1e6;
    if (t1 <= 0 || stage >= 2 || now < (stage ? t2 : t1)) return;
    if (!snap) snap = calloc(2, sizeof *snap);
    if (!snap) { t1 = 0; return; }
    uint64_t t = coop_rd_ptr(me + 0x228);
    for (int j = 0; j < 4; j++) coop_rd(me + 0x400 * (uint64_t)j, snap[stage][256 - 4 + j], 0x400);   // (the actor in the last rows)
    for (int j = 0; j < 252; j++) { uint64_t comp = t ? coop_rd_ptr(t + 8 * (uint64_t)j) : 0; if (comp) coop_rd(comp, snap[stage][j], 0x400); }
    if (++stage < 2) return;
    int said = 0;
    for (int j = 0; j < 256 && said < 400; j++)
        for (int w = 0; w < 0x400; w += 4) {
            uint32_t a, b;
            memcpy(&a, &snap[0][j][w], 4), memcpy(&b, &snap[1][j][w], 4);
            if (a == b) continue;
            float fa, fb;
            memcpy(&fa, &a, 4), memcpy(&fb, &b, 4);
            if (fabsf(fa) > 1e6f || fabsf(fb) > 1e6f || (a >> 20) == 0x1a || (a >> 20) == 0x19) continue;   // (pointers, noise)
            // TOTK_COOP_TEST_PLAYERDIFFDOWN=1: only floats in (0, 5000) that went down by 0.01..500 (a battery drawn)
            if (cfg_get("TOTK_COOP_TEST_PLAYERDIFFDOWN") && !(fa > 0 && fa < 5000 && fb < fa && fa - fb > 0.01f && fa - fb < 500 && fb >= 0)) continue;
            rt_log("coop: test: playerdiff %s %d (%llx) +%03x: %08x -> %08x (%g -> %g)", j >= 252 ? "actor" : "comp", j >= 252 ? (j - 252) : j,
                   (unsigned long long)(j >= 252 ? me : coop_rd_ptr(coop_rd_ptr(me + 0x228) + 8 * (uint64_t)j)),
                   j >= 252 ? (j - 252) * 0x400 + w : w, a, b, fa, fb);
            said++;
        }
    rt_log("coop: test: playerdiff: %d words", said);
}

// TOTK_COOP_TEST_SERIAL=<s> (contraption-sync.md, Phase 0 item 2): at that world time, with nothing held, the
// game's history writer (main+0x1913694, the call the Autobuild ability makes on the contraption Ultrahand holds) is
// run on the first live tracked part, as the ability calls it: x0 = [[main+0x395f6f8]], x1 = an ActorLink to the
// part (main+0x155ec), x2 = an alignment matrix (identity here), x3 = the record buffer (the ability gives
// ability+0x52e0, 0x1a20 bytes written), x4 = a matrix out. Logs what it wrote: counts, each part's name, scale and
// frame 0 translation, each bond record; and the same part's world place, to compare.
#define HISTORY_WRITE 0x1913694
#define LINK_SET_FN 0x155ec
#define HISTORY_MGR_SLOT 0x395f6f8
static uint64_t serial_call5(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {   // as coop_guest_call
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 5, a0, a1, a2, a3, a4);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}
static _Atomic int serial_due;
static int tracked_part(uint64_t a);
static void serial_run(Cpu *c, int n);
static void serial_test(Cpu *c, int n) {   // the tick: due at that time; run in this Link's update (coop_autobuild_link_calc)
    (void)c, (void)n;
    static double at = -1;
    if (at == -1) at = cfg_get("TOTK_COOP_TEST_SERIAL") ? atof(cfg_get("TOTK_COOP_TEST_SERIAL")) : 0;
    if (at <= 0 || frame_step_world_us() / 1e6 < at) return;
    at = 0;
    atomic_store(&serial_due, 1);
}
// The actor manager's phase byte ([[main+0x395da68]]+0x424): 2 while the actors update in parallel, when an actor
// link resolves (main+0x45efd4) only to an actor the calling job may touch; at other values it resolves to any.
static int actor_phase(void) {
    uint64_t m = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395da68));
    uint8_t b = 0xff;
    if (m) coop_rd(m + 0x424, &b, 1);
    return b;
}
static void serial_try(Cpu *c, const char *where) {
    static int seen[256];
    int ph = actor_phase();
    if (!atomic_load(&serial_due)) return;
    if (!seen[ph & 0xff]++) rt_log("coop: test: serial: due, %s at actor phase %d", where, ph);
    // TOTK_COOP_TEST_SERIALPHASE=1: run at once, with the phase byte set to 4 for the call (both callers only ever see
    // 2): a test of what the writer makes when the lookup lets it, not a way to call it in the game.
    int force = ph == 2 && cfg_get("TOTK_COOP_TEST_SERIALPHASE");
    if (ph == 2 && !force) return;
    if (!atomic_exchange(&serial_due, 0)) return;
    rt_log("coop: test: serial: run from %s at actor phase %d%s", where, ph, force ? ", set to 4 for the call" : "");
    uint64_t m = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395da68));
    if (force) *(volatile uint8_t *)(uintptr_t)(m + 0x424) = 4;
    int n = atomic_load(&tracked_n);
    serial_run(c, n < TRACK_N ? n : TRACK_N);
    if (force) *(volatile uint8_t *)(uintptr_t)(m + 0x424) = 2;
}
// Called at the start of this PC's Link's update (coop_spawn.c calc_wrap), on the actor job's thread. The actor-link
// lookup the history writer starts with (main+0x45efd4) answers only on the game's job threads: called from the co-op
// tick, it gave no actor and the writer returned 0.
// TOTK_COOP_TEST_REBUILDGO=<s> (with TOTK_COOP_TEST_REBUILD made earlier, TOTK_COOP_TEST_CREATEDUMP=1 to keep the
// blueprint): the second build's parts finished as the confirm would, from this Link's update (the Autobuild ability's
// own job runs there): each part's create handle (pool entry +0x60) put in the world and linked at the entry's +0x48
// (the hologram link the ready check main+0x9a83dc wants valid), each entry's state (+0x1c) 3 ("made from the
// currency"), the object's +0x3c70 cleared, the whole blueprint put back, and main+0x1d3c0d4 called. Then for 4 s every
// 0.5 s: each new part's group, green bit (SPR +0x150 bit 0) and place against the first build's.
static double cfg_at(const char *name, double *second);
static uint64_t go_parts[0x15];
static int go_n;
static void rebuild_go(Cpu *c) {
    static double at = -1, claim_at;
    static int claimed;
    // TOTK_COOP_TEST_REBUILDGOCLAIM=<s>: the parts put in the world at that earlier time (as a real build's previews
    // are before its confirm): claimed in the confirm's own tick, they entered the world a tick after it, and their SPR
    // fields (the mark among them) came back cleared
    if (at < 0) at = cfg_at("TOTK_COOP_TEST_REBUILDGO", NULL), claim_at = cfg_at("TOTK_COOP_TEST_REBUILDGOCLAIM", NULL);
    double now_s = frame_step_world_us() / 1e6;
    uint64_t o = coop_ab_obj, list = o ? coop_rd_ptr(o + 0x1b8) : 0;
    int32_t ne = 0;
    if (o) coop_rd(o + 0x1b0, &ne, 4);
    static _Alignas(16) uint8_t tmp[0x15][0x40];
    if (!claimed && claim_at > 0 && o && now_s >= claim_at) {
        claimed = 1;
        for (int k = 0; list && k < ne && k < 0x15; k++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)k), ref = 0;
            int st = ent ? coop_handle_status((const uint8_t *)(uintptr_t)(ent + 0x60)) : 0;
            uint64_t a = st == 3 ? coop_claim(c, (uint8_t *)(uintptr_t)(ent + 0x60), tmp[k], &ref) : 0;
            if (a) go_parts[go_n++] = a;
            rt_log("coop: test: rebuildgo: %.2f s entry %d claimed early: %llx", now_s, k, (unsigned long long)a);
        }
    }
    if (at <= 0 || !o || now_s < at) return;
    at = 0;
    for (int k = 0; claimed && list && k < ne && k < 0x15 && k < go_n; k++) {   // (claimed early: only linked now)
        uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)k);
        if (!ent) continue;
        uint8_t *l = (uint8_t *)(uintptr_t)(ent + 0x48);
        memset(l + 0x10, 0xff, 4), l[0x14] = 1;
        coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, ent + 0x48, go_parts[k], 1, 0);
        ST32(ent + 0x1c, 3);
    }
    for (int k = 0; !claimed && list && k < ne && k < 0x15; k++) {
        uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)k), ref = 0;
        if (!ent) continue;
        int st = coop_handle_status((const uint8_t *)(uintptr_t)(ent + 0x60));
        uint64_t a = st == 3 ? coop_claim(c, (uint8_t *)(uintptr_t)(ent + 0x60), tmp[k], &ref) : 0;
        if (a) {
            uint8_t *l = (uint8_t *)(uintptr_t)(ent + 0x48);
            memset(l + 0x10, 0xff, 4), l[0x14] = 1;
            coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, ent + 0x48, a, 1, 0);
            ST32(ent + 0x1c, 3);
            go_parts[go_n++] = a;
        }
        char nm[48] = "";
        rt_log("coop: test: rebuildgo: entry %d handle status %d: %s %llx", k, st, a ? coop_actor_name(a, nm, sizeof nm) : "-", (unsigned long long)a);
    }
    ST8(o + 0x3c70, 0);
    uint64_t cai = coop_rd_ptr(o + 0x3c50);
    if (cai && coop_ab_cai[9]) memcpy((void *)(uintptr_t)cai, coop_ab_cai, sizeof coop_ab_cai);
    // TOTK_COOP_TEST_REBUILDGOPHASE=1: the actor manager's phase byte set to 4 for the call (a test of what the confirm
    // does when every actor link resolves; Phase 0 item 2 found the lookup refuses other jobs' actors in phase 2)
    // TOTK_COOP_TEST_REBUILDGOFREE=1: no currency charged: the confirm charges each part's cost (the record
    // main+0xae6e04 finds for the entry's name in the table [[[main+0x395f900]]+8], its +0x48) through main+0x209b754;
    // each record's cost set to 0 for the call and put back after
    uint64_t costs_at[0x15] = {0};
    uint32_t costs_were[0x15] = {0};
    int ncost = 0;
    if (cfg_get("TOTK_COOP_TEST_REBUILDGOFREE")) {
        uint64_t tab = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395f900)) + 8);
        static uint64_t key;
        for (int k = 0; tab && list && k < ne && k < 0x15; k++) {
            uint64_t ent = coop_rd_ptr(list + 8 * (uint64_t)k);
            if (!ent) continue;
            key = coop_rd_ptr(ent);
            uint64_t rec = coop_guest_call(c, MOD_MAIN_BASE + 0xae6e04, tab, (uint64_t)(uintptr_t)&key, 0, 0);
            int seen = 0;
            for (int i = 0; i < ncost; i++) seen |= costs_at[i] == rec;
            if (!rec || seen) continue;
            costs_at[ncost] = rec, costs_were[ncost] = LD32(rec + 0x48), ST32(rec + 0x48, 0);
            char nm[64] = "";
            coop_rd(key, nm, sizeof nm - 1);
            rt_log("coop: test: rebuildgo: %s costs %u: 0 for the confirm", nm, costs_were[ncost]);
            ncost++;
        }
    }
    uint64_t am = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395da68));
    int force = cfg_get("TOTK_COOP_TEST_REBUILDGOPHASE") && am && actor_phase() == 2;
    if (force) *(volatile uint8_t *)(uintptr_t)(am + 0x424) = 4;
    uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + 0x1d3c0d4, o, 0, 0, 0);
    if (force) *(volatile uint8_t *)(uintptr_t)(am + 0x424) = 2;
    for (int i = 0; i < ncost; i++) ST32(costs_at[i] + 0x48, costs_were[i]);
    rt_log("coop: test: rebuildgo: %.2f s the confirm (main+0x1d3c0d4) returned %llu at actor phase %d", frame_step_world_us() / 1e6,
           (unsigned long long)(r & 1), actor_phase());
    // TOTK_COOP_TEST_REBUILDGOMARK=1: each part then given the confirm's own mark (main+0x1c07114: autobuilt, green,
    // preview off), in case the confirm's own marking did not reach them
    // TOTK_COOP_TEST_REBUILDGOMARK=2: the mark without its green bit (SPR +0x27c bit 0, +0x20 bit 28 on and bit 51 off,
    // actor +0x5ac bit 2 off), the green (+0x150 bit 0) only once the part has a group (rebuild_go_log): green with no
    // group is "a part broken off", which the game destroys (state 11 within a second, found with =1)
    const char *mk = cfg_get("TOTK_COOP_TEST_REBUILDGOMARK");
    for (int k = 0; mk && k < go_n; k++) {
        uint64_t spr = coop_rd_ptr(coop_rd_ptr(go_parts[k] + 0x228) + 0x480);
        if (!spr) continue;
        if (*mk == '2') {
            __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x27c), 1u, __ATOMIC_SEQ_CST);
            uint64_t f20 = LD64(spr + 0x20);
            ST64(spr + 0x20, (f20 & ~(1ull << 51)) | (1ull << 28));
            __atomic_fetch_and((uint32_t *)(uintptr_t)(go_parts[k] + 0x5ac), ~4u, __ATOMIC_SEQ_CST);
        } else coop_guest_call(c, MOD_MAIN_BASE + 0x1c07114, spr, 0, 0, 0);
    }
    for (int k = 0; k < go_n; k++) {   // straight after it: what it marked and queued on each part's SPR
        uint64_t spr = coop_rd_ptr(coop_rd_ptr(go_parts[k] + 0x228) + 0x480);
        uint32_t f150 = 0, f27c = 0, q = 0;
        uint64_t f20 = 0;
        if (spr) coop_rd(spr + 0x150, &f150, 4), coop_rd(spr + 0x27c, &f27c, 4), coop_rd(spr + 0x2b0, &q, 4), coop_rd(spr + 0x20, &f20, 8);
        rt_log("coop: test: rebuildgo: part %d spr %llx: +0x150 %#x +0x27c %#x +0x20 %#llx, bonds queued %u", k, (unsigned long long)spr, f150,
               f27c, (unsigned long long)f20, q);
    }
}
// TOTK_COOP_TEST_PARTCMP=<s1>,<s2>: one line per part, for the real build's parts (tracked) at s1 and the rebuilt ones
// (rebuild_go) at s2: the fields an Autobuild's confirm and what follows it set, to see what the rebuilt copy lacks:
// actor state +0x24, actor +0x444 and +0x5ac, SPR +0x20 / +0x27c / +0x150 / +0x28 (group) / +0x2b0 (queued bonds),
// the main body's flags +0x68 and its pending request's motion type, and the body's linear speed (+0x320 of the actor).
static void part_line(const char *what, int k, uint64_t a) {
    uint64_t t = coop_rd_ptr(a + 0x228), spr = t ? coop_rd_ptr(t + 0x480) : 0;
    uint64_t body = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(t + 0x50) + 0x20) + 0x150);
    int32_t st = 0;
    uint32_t a444 = 0, a5ac = 0, f27c = 0, f150 = 0, q = 0, rq = 0, rt = 0;
    uint64_t f20 = 0, g = 0, bf = 0, req = 0;
    coop_rd(a + 0x24, &st, 4), coop_rd(a + 0x444, &a444, 4), coop_rd(a + 0x5ac, &a5ac, 4);
    if (spr) coop_rd(spr + 0x20, &f20, 8), coop_rd(spr + 0x27c, &f27c, 4), coop_rd(spr + 0x150, &f150, 4), g = coop_rd_ptr(spr + 0x28), coop_rd(spr + 0x2b0, &q, 4);
    if (body) coop_rd(body + 0x68, &bf, 8), req = coop_rd_ptr(body + 0x60);
    if (req) coop_rd(req + 0xd4, &rq, 4), coop_rd(req + 0x80, &rt, 4);
    float v[3] = {0};
    coop_rd(a + 0x320, v, 12);
    char nm[48];
    rt_log("coop: test: partcmp: %s %d %.12s state %d +0x444 %#x +0x5ac %#x | spr +0x20 %#llx +0x27c %#x +0x150 %#x group %llx queued %u | body flags %#llx req %#x type %u | v %.2f %.2f %.2f",
           what, k, coop_actor_name(a, nm, sizeof nm) + 6, st, a444, a5ac, (unsigned long long)f20, f27c, f150, (unsigned long long)(g & 0xffffff), q,
           (unsigned long long)bf, rq, rt, v[0], v[1], v[2]);
}
static void partcmp_test(int n) {
    static double s1 = -1, s2;
    static int d1, d2;
    if (s1 < 0) s1 = cfg_at("TOTK_COOP_TEST_PARTCMP", &s2);
    if (s1 <= 0) return;
    double t = frame_step_world_us() / 1e6;
    char nm[48];
    if (!d1 && t >= s1) {
        d1 = 1;
        for (int i = 0; i < n; i++) if (live_device(i, nm, sizeof nm)) part_line("real", i, atomic_load(&tracked[i]));
    }
    if (!d2 && t >= s2 && go_n) {
        d2 = 1;
        for (int k = 0; k < go_n; k++) part_line("rebuilt", k, go_parts[k]);
    }
}
static void rebuild_go_watch(void) {   // TOTK_COOP_TEST_REBUILDGOWATCH=<from>,<to>: writes to rebuilt part 1's SPR +0x150
    static double from = -1, to;
    static int st;
    if (from < 0) from = cfg_at("TOTK_COOP_TEST_REBUILDGOWATCH", &to);
    if (from <= 0 || go_n < 2) return;
    double t = frame_step_world_us() / 1e6;
    int coop_watch_arm(uint64_t addr, uint64_t len);
    void coop_watch_disarm(void);
    extern int coop_watch_writes;
    uint64_t spr = coop_rd_ptr(coop_rd_ptr(go_parts[1] + 0x228) + 0x480);
    if (st == 0 && t >= from && spr) { coop_watch_writes = 1; rt_log("coop: test: rebuildgo: watching %llx +0x150: %d", (unsigned long long)spr, coop_watch_arm(spr + 0x150, 4)); st = 1; }
    if (st == 1 && t >= to) { coop_watch_disarm(); st = 2; }
}
static void rebuild_go_log(void) {
    static double next, until;
    double t = frame_step_world_us() / 1e6;
    if (!go_n) return;
    if (!until) until = t + 10.2;
    if (t < next || t > until) return;
    next = t + 0.25;
    char line[700];
    int len = snprintf(line, sizeof line, "coop: test: rebuildgo: %.2f s", t);
    {   // (mark mode 2) green only once in a group
        const char *mk = cfg_get("TOTK_COOP_TEST_REBUILDGOMARK");
        for (int k = 0; mk && *mk == '2' && k < go_n; k++) {
            uint64_t spr = coop_rd_ptr(coop_rd_ptr(go_parts[k] + 0x228) + 0x480);
            if (spr && coop_rd_ptr(spr + 0x28)) __atomic_fetch_or((uint32_t *)(uintptr_t)(spr + 0x150), 1u, __ATOMIC_SEQ_CST);
        }
    }
    float p0[3] = {0};
    coop_rd(go_parts[0] + COOP_ACTOR_POS, p0, 12);
    for (int k = 0; k < go_n; k++) {
        uint64_t spr = coop_rd_ptr(coop_rd_ptr(go_parts[k] + 0x228) + 0x480), g = spr ? coop_rd_ptr(spr + 0x28) : 0;
        uint32_t gn = 0, f150 = 0;
        if (g) coop_rd(g + 0x58, &gn, 4);
        if (spr) coop_rd(spr + 0x150, &f150, 4);
        float p[3] = {0};
        coop_rd(go_parts[k] + COOP_ACTOR_POS, p, 12);
        uint32_t q = 0;
        if (spr) coop_rd(spr + 0x2b0, &q, 4);
        int32_t ast = 0;
        coop_rd(go_parts[k] + 0x24, &ast, 4);
        len += snprintf(line + len, sizeof line - (size_t)len, " | %d st %d group %llx (%u) green %u queued %u at %.2f %.2f %.2f (from part 0 %.2f %.2f %.2f)", k,
                        ast, (unsigned long long)(g & 0xffffff), gn, f150 & 1, q, p[0], p[1], p[2], p[0] - p0[0], p[1] - p0[1], p[2] - p0[2]);
    }
    rt_log("%s", line);
}
// The contraption sync itself is coop_ctn.c (docs/coop/contraption-sync-v3.md section 27).
static int contraption_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_CONTRAPTION") && *cfg_get("TOTK_COOP_CONTRAPTION") == '1';
    return on;
}
// TOTK_COOP_TEST_REBUILDGOIN=gamedata: the rebuild's confirm (and the history writer, TOTK_COOP_TEST_SERIAL) run from
// GameDataMgr::calc instead, at actor phase 0 (coop_spawn.c coop_hook)
void coop_autobuild_seq_calc(Cpu *c) {
    coop_ctn_phase0(c);
    rebuild_go(c);
    serial_try(c, "GameDataMgr::calc");
}
void coop_autobuild_link_calc(Cpu *c) {
    if (!cfg_get("TOTK_COOP_TEST_REBUILDGOIN")) rebuild_go(c);
    static int from_link = -1;
    if (from_link < 0) { const char *in = cfg_get("TOTK_COOP_TEST_SERIALIN"); from_link = !in || strcmp(in, "part"); }
    if (from_link) serial_try(c, "the Link's update");
}
// The same from inside the update of a part of the contraption: the job that updates a contraption's parts may be the
// one the actor access check lets reach all of them (TOTK_COOP_TEST_SERIALIN=part). Cheap when off: one cached read.
void coop_autobuild_part_calc(Cpu *c, uint64_t actor) {
    static int on = -1;
    if (on < 0) { const char *in = cfg_get("TOTK_COOP_TEST_SERIALIN"); on = in && !strcmp(in, "part"); }
    if (!on || !atomic_load(&serial_due) || !tracked_part(actor) || !atomic_exchange(&serial_due, 0)) return;
    rt_log("coop: test: serial: run from a part's update at actor phase %d", actor_phase());
    int n = atomic_load(&tracked_n);
    serial_run(c, n < TRACK_N ? n : TRACK_N);
}
static void serial_run(Cpu *c, int n) {
    char nm[48];
    uint64_t part = 0;
    for (int i = 0; i < n; i++)   // (the writer refuses a part whose SPR +0x28 is empty: main+0x1c03a60 gives -1)
        if (live_device(i, nm, sizeof nm)) {
            uint64_t a = atomic_load(&tracked[i]), g = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x480) + 0x28);
            rt_log("coop: test: serial: part %d %s: group %llx", i, nm, (unsigned long long)g);
            if (g && !part) part = a;
        }
    if (part) coop_actor_name(part, nm, sizeof nm);
    if (!part) { rt_log("coop: test: serial: no live part"); return; }
    static _Alignas(16) uint8_t link[0x40], rec[0x3000];
    // the alignment is a 3x3 rotation, nine floats (the Autobuild ability fills sp+0x1a0..0x1c0 with a heading basis
    // before its call at main+0x9a36c4); given here as identity
    static _Alignas(16) float align[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, info[12];
    memset(link, 0, sizeof link), memset(rec, 0, sizeof rec);
    memset(link + 0x10, 0xff, 4), link[0x14] = 1;   // an empty link: id -1, +0x14 set (main+0x155ec does nothing without it)
    coop_guest_call(c, MOD_MAIN_BASE + LINK_SET_FN, (uint64_t)(uintptr_t)link, part, 1, 0);   // (1: a strong link; the writer resolves it with 1)
    uint64_t lw[3];
    memcpy(lw, link, sizeof lw);
    uint64_t ref_actor = lw[1] ? coop_rd_ptr(lw[1] + 0x40) : 0;
    rt_log("coop: test: serial: link %016llx %016llx %016llx, its actor %llx (part %llx)", (unsigned long long)lw[0],
           (unsigned long long)lw[1], (unsigned long long)lw[2], (unsigned long long)ref_actor, (unsigned long long)part);
    if (ref_actor != part) { rt_log("coop: test: serial: the link does not lead to the part: not called"); return; }
    {   // the writer's first step by hand: main+0x45efd4(link, 0, 1) with its result block in x8
        static _Alignas(16) uint64_t out[4];
        memset(out, 0, sizeof out);
        uint64_t saved_x[31], sp0 = c->sp;
        memcpy(saved_x, c->x, sizeof saved_x);
        c->sp = (sp0 - 0x100) & ~0xfull, c->x[8] = (uint64_t)(uintptr_t)out;
        rt_call(c, MOD_MAIN_BASE + 0x45efd4, 3, (uint64_t)(uintptr_t)link, 0, 1);
        c->sp = sp0, memcpy(c->x, saved_x, sizeof saved_x);
        int32_t st = -1;
        coop_rd(part + 0x24, &st, 4);
        uint32_t rid = 0, aid = 0, lid = 0;
        coop_rd(coop_rd_ptr((uint64_t)(uintptr_t)link + 8) + 0x48, &rid, 4), coop_rd(part + 0x10, &aid, 4), memcpy(&lid, link + 0x10, 4);
        uint8_t sys = 0;
        coop_rd(coop_rd_ptr(MOD_MAIN_BASE + 0x395da68) ? coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395da68)) + 0x424 : 0, &sys, 1);
        rt_log("coop: test: serial: link lookup gave %016llx %016llx %016llx (part state %d; ids: ref %x actor %x link %x; system byte %u)",
               (unsigned long long)out[0], (unsigned long long)out[1], (unsigned long long)out[2], st, rid, aid, lid, sys);
        // unlocked as the writer does after its own lookup (main+0x1913760: when byte +0x10 is set, main+0x2bb7a70([+0]+0x20))
        if ((uint8_t)out[2] && out[0]) coop_guest_call(c, MOD_MAIN_BASE + 0x2bb7a70, out[0] + 0x20, 0, 0, 0);
    }
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + HISTORY_MGR_SLOT));
    extern _Atomic int coop_call_trace;
    extern volatile int g_fnprof;
    int prof = g_fnprof;
    extern Cpu *volatile coop_call_trace_cpu;
    extern volatile uint64_t coop_call_trace_lo, coop_call_trace_hi;
    coop_call_trace_lo = HISTORY_WRITE, coop_call_trace_hi = HISTORY_WRITE + 0xd00;   // (the writer ends before main+0x1914394)
    coop_call_trace_cpu = c, g_fnprof = 1, atomic_store(&coop_call_trace, 400);
    uint64_t ok = serial_call5(c, MOD_MAIN_BASE + HISTORY_WRITE, mgr, (uint64_t)(uintptr_t)link, (uint64_t)(uintptr_t)align,
                                   (uint64_t)(uintptr_t)rec, (uint64_t)(uintptr_t)info);
    atomic_store(&coop_call_trace, 0), g_fnprof = prof;
    {   // what the writer checks first: the part's SPR, its group's id (+0x20) and count (+0x58), the table entry
        uint64_t spr = coop_rd_ptr(coop_rd_ptr(part + 0x228) + 0x480), g = spr ? coop_rd_ptr(spr + 0x28) : 0;
        uint32_t gid = 0, gn = 0, tn = 0, rid = 0, rn = 0;
        if (g) coop_rd(g + 0x20, &gid, 4), coop_rd(g + 0x58, &gn, 4);
        coop_rd(mgr + 0x20, &tn, 4);
        uint64_t tab = coop_rd_ptr(mgr + 0x28), rec_g = tab ? tab + (uint64_t)(int16_t)gid * 0xa70 : 0;
        if (rec_g) coop_rd(rec_g + 0x20, &rid, 4), coop_rd(rec_g + 0x58, &rn, 4);
        uint64_t r2 = coop_guest_call(c, MOD_MAIN_BASE + 0x2bdd3c, part, 0, 0, 0);
        rt_log("coop: test: serial: spr %llx group %llx id %u count %u; table %u at %llx, entry %llx (id %u count %u); 2bdd3c %llx",
               (unsigned long long)spr, (unsigned long long)g, gid, gn, tn, (unsigned long long)tab, (unsigned long long)rec_g, rid, rn,
               (unsigned long long)r2);
    }
    float pp[3] = {0};
    coop_rd(part + COOP_ACTOR_POS, pp, 12);
    rt_log("coop: test: serial: writer returned %llu on %s at %.2f %.2f %.2f; header %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
           (unsigned long long)(ok & 1), nm, pp[0], pp[1], pp[2], rec[0], rec[1], rec[2], rec[3], rec[4], rec[5], rec[6], rec[7], rec[8], rec[9], rec[10], rec[11]);
    int parts = rec[9], bonds = rec[10];
    for (int i = 0; i < parts && i < 21; i++) {
        const uint8_t *r = rec + 0xc + i * 0x118;
        float f[12], sc;
        memcpy(f, r, sizeof f), memcpy(&sc, r + 0x90, 4);
        rt_log("coop: test: serial: part %d %.40s scale %.2f on %u frame0 words %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
               i, (const char *)r + 0x98, sc, r[0x94], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
    }
    for (int i = 0; i < bonds && i < 21; i++) {
        const uint8_t *b = rec + 0x1704 + i * 12;
        uint32_t sa, sb;
        memcpy(&sa, b, 4), memcpy(&sb, b + 4, 4);
        rt_log("coop: test: serial: bond %d: part %u body %u - part %u body %u", i, b[8], sa, b[9], sb);
    }
    rt_log("coop: test: serial: info words %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", info[0], info[1], info[2], info[3],
           info[4], info[5], info[6], info[7], info[8], info[9], info[10], info[11]);
    for (int i = 0; i < n; i++)   // each live part's body matrix, to compare with the frames
        if (live_device(i, nm, sizeof nm)) {
            uint64_t a = atomic_load(&tracked[i]);
            uint64_t body = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) + 0x150);
            float w[12] = {0};
            if (body) coop_rd(body + 0x98, w, sizeof w);
            rt_log("coop: test: serial: body %d %s words %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", i, nm, w[0], w[1], w[2], w[3],
                   w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
        }
    // the same parts' world places now, to compare with the frames
    for (int i = 0; i < n; i++)
        if (live_device(i, nm, sizeof nm)) {
            float q[3] = {0};
            coop_rd(atomic_load(&tracked[i]) + COOP_ACTOR_POS, q, 12);
            rt_log("coop: test: serial: live %d %s at %.3f %.3f %.3f", i, nm, q[0], q[1], q[2]);
        }
}

// Phase 0 items 4 and 5 of contraption-sync.md, hooks run from coop_spawn.c coop_hook:
// TOTK_COOP_TEST_NOSELFPUSH=<s>: from that world time, while a fan of a tracked contraption runs its push
// (WindGenerator_ApplyImpulse main+0x2487104), its velocity writes (main+0x567750) and impulses (main+0x29ae114) on a
// body of a tracked part are skipped and the others let through, as a copy's fans would: the copy moved by its owner,
// the rest of the world pushed as usual. Every 0.5 s: skipped and passed counts and the first part's place.
// TOTK_COOP_TEST_DRAINMOVE=<s>,<s2>: from s, a charge to the Link's gauge (consume main+0x1fa4ca8, consumeRate
// main+0x1fa4ed4) from a tracked part's on/off node (main+0x9f9088, main+0x9fad68) is kept, not applied; at s2 the kept
// total is charged at once through consume. Every 0.5 s: the gauge's numbers, to see it stay and then drop.
#define APPLY_IMPULSE_FN 0x2487104
#define SET_LINVEL_FN 0x567750
#define LIN_IMPULSE_FN 0x29ae114
#define GAUGE_CONSUME 0x1fa4ca8
#define GAUGE_CONSUME_RATE 0x1fa4ed4
#define ONOFF_CALC 0x9f9088
#define ONOFF_SET 0x9fad68
static __thread int in_push, in_node, in_copy_node, in_copy_force, in_paid_node;
// The device nodes that push their own part (research devices2/: rocket, sled, balloon, wing, hover stone, stabilizer,
// spring, stake, homing cart, steering stick): on a copy their body writes are skipped; the wheel's and the cart's
// motor nodes drive joints, and are skipped whole
static const uint32_t force_nodes[] = {0x24343f4, 0x2458320, 0x222d694, 0x221cd04, 0x230a3fc, 0x24730dc, 0x221a7dc, 0x221420c,
                                       0x225c52c, 0x226abd4};
static const uint32_t motor_nodes[] = {0x22fc5f8, 0x2251440};
static const uint32_t body_setters[] = {0x567750, 0x25a1f0, 0x29ae114, 0x29ae7a8, 0x29ae7fc, 0x29a4afc};
static int in_list(const uint32_t *l, int n, uint64_t off) {
    for (int i = 0; i < n; i++) if (l[i] == off) return 1;
    return 0;
}
#define DEVSWITCH_UPDATE 0x4a3500
static int contraption_on(void);
int coop_contraption_made(uint64_t a);
static __thread uint64_t push_body;   // the pushing fan's own body (a copy's fan never pushes its own contraption)
static _Atomic int push_skipped, push_passed, drain_kept_n;
static _Atomic uint64_t drain_mgr;
static double drain_kept;
static PlatMutex drain_lock = PLAT_MUTEX_INIT;
static double cfg_at(const char *name, double *second) {
    const char *e = cfg_get(name);
    if (!e) return 0;
    if (second) { const char *k = strchr(e, ','); *second = k ? atof(k + 1) : 0; }
    return atof(e);
}
static uint64_t node_actor(uint64_t node) {   // an AI node's actor (main+0x9a89c4)
    return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(node + 8) + 8) + 0x1f8);
}
static int tracked_part(uint64_t a) {
    int n = atomic_load(&tracked_n);
    for (int i = 0; a && i < n && i < TRACK_N; i++)
        if (atomic_load(&tracked[i]) == a) return 1;
    return 0;
}
static int tracked_body(uint64_t body) {
    int n = atomic_load(&tracked_n);
    for (int i = 0; body && i < n && i < TRACK_N; i++) {
        uint64_t a = atomic_load(&tracked[i]);
        if (a && coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) + 0x150) == body) return 1;
    }
    return 0;
}
static int wrap_call(Cpu *c, uint64_t addr, int *flag, int on) {   // run the function with *flag set
    int was = *flag;
    *flag = on;
    c->pc = addr;
    rt_dispatch(c);
    *flag = was;
    return 1;
}
int coop_phase0_hook(Cpu *c, uint64_t addr) {
    static double push_at = -1, drain_at = -1, drain_to;
    if (push_at < 0) push_at = cfg_at("TOTK_COOP_TEST_NOSELFPUSH", NULL), drain_at = cfg_at("TOTK_COOP_TEST_DRAINMOVE", &drain_to);
    double t = frame_step_world_us() / 1e6;
    uint64_t off = addr - MOD_MAIN_BASE;
    static __thread int inner;   // (the hooked function's own entry, reached again through rt_dispatch)
    if (inner) { inner = 0; return 0; }
    // A part of another player's contraption made here (TOTK_COOP_CONTRAPTION): its fan pushes nothing (the owner's game
    // moves the contraption; the wind capsule and the look still run), and its on/off node charges no gauge
    if (off == APPLY_IMPULSE_FN && contraption_on() && coop_ctn_driven(node_actor(c->x[0]))) return 1;
    if (contraption_on()) {
        if (in_list(body_setters, 6, off) && in_copy_force) return 1;   // (a copy's device moving its own body: not here)
        if (in_list(motor_nodes, 2, off) && coop_ctn_driven(node_actor(c->x[0]))) return 1;
        if (in_list(force_nodes, 10, off) && coop_ctn_driven(node_actor(c->x[0]))) {
            inner = 1;
            return wrap_call(c, addr, &in_copy_force, 1);
        }
        if (off == DEVSWITCH_UPDATE) {   // a toggle asked on a copy's device (a hit here): taken to the owner instead
            uint64_t comp = c->x[0], actor = coop_rd_ptr(comp + 0x18);
            uint8_t b8d = 0;
            {   // (the hook's view of a toggle request on a contraption part: the first few, for the toggle test)
                uint8_t bb = 0;
                static _Atomic int said;
                if (coop_rd(comp + 0x8d, &bb, 1) && (bb & 8) && coop_contraption_made(actor) && atomic_fetch_add(&said, 1) < 6)
                    rt_log("coop: contraption: switch update of a copy part with a toggle request: driven %d, ours %d", coop_ctn_driven(actor), 0);
            }
            if (coop_ctn_driven(actor) && coop_rd(comp + 0x8d, &b8d, 1) && (b8d & 8)) {
                __atomic_fetch_and((uint8_t *)(uintptr_t)(comp + 0x8d), (uint8_t)~8, __ATOMIC_SEQ_CST);
                coop_ctn_toggle_asked(comp, actor);
            }
            return 0;
        }
    }
    if ((off == ONOFF_CALC || off == ONOFF_SET) && contraption_on() && coop_ctn_no_charge(node_actor(c->x[0]))) {   // (another player pays)
        inner = 1;
        return wrap_call(c, addr, &in_copy_node, 1);
    }
    if ((off == ONOFF_CALC || off == ONOFF_SET) && contraption_on() && coop_contraption_made(node_actor(c->x[0])) && ablog_on()) {
        inner = 1;   // (TOTK_COOP_TEST_ABLOG: a copy's device whose activator is this player: its charges logged)
        return wrap_call(c, addr, &in_paid_node, 1);
    }
    if ((off == GAUGE_CONSUME || off == GAUGE_CONSUME_RATE) && in_paid_node) {
        static _Atomic int64_t said_paid;
        int64_t now = frame_step_world_us();
        if (now - atomic_load(&said_paid) > 2000000) {
            float amount, g = 0;
            uint32_t bits = (uint32_t)c->v[0];
            memcpy(&amount, &bits, 4);
            coop_rd(c->x[0] + 0x54, &g, 4);
            atomic_store(&said_paid, now), rt_log("coop: contraption: a copy's device charges this player: %s %.3f, gauge %.1f", off == GAUGE_CONSUME ? "consume" : "consumeRate", amount, g);
        }
    }
    if ((off == GAUGE_CONSUME || off == GAUGE_CONSUME_RATE) && in_copy_node) {
        static _Atomic int said_copy;
        if (atomic_fetch_add(&said_copy, 1) < 3) rt_log("coop: contraption: a charge from a copy's device not made (%s)", off == GAUGE_CONSUME ? "consume" : "consumeRate");
        return 1;
    }
    if (off == APPLY_IMPULSE_FN && push_at > 0 && t >= push_at) {
        extern uint64_t coop_fan_actor;   // (coop_players.c TOTK_COOP_TEST_FANSET's loose fan counts as one of ours)
        uint64_t a = node_actor(c->x[0]);
        int mine = tracked_part(a) || (a && a == coop_fan_actor);
        uint64_t was = push_body;
        push_body = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) + 0x150);
        inner = 1;
        wrap_call(c, addr, &in_push, mine);
        push_body = was;
        return 1;
    }
    if ((off == SET_LINVEL_FN || off == LIN_IMPULSE_FN) && in_push) {
        if (tracked_body(c->x[0]) || (push_body && c->x[0] == push_body)) { atomic_fetch_add(&push_skipped, 1); return 1; }   // skipped
        atomic_fetch_add(&push_passed, 1);
        return 0;
    }
    if ((off == ONOFF_CALC || off == ONOFF_SET) && drain_at > 0 && t >= drain_at) {
        int mine = tracked_part(node_actor(c->x[0]));
        inner = 1;
        return wrap_call(c, addr, &in_node, mine);
    }
    if ((off == GAUGE_CONSUME || off == GAUGE_CONSUME_RATE) && in_node && drain_at > 0 && t < drain_to) {
        float amount;
        uint32_t bits = (uint32_t)c->v[0];
        memcpy(&amount, &bits, 4);
        plat_mutex_lock(&drain_lock);
        drain_kept += amount * (off == GAUGE_CONSUME_RATE ? -1 : 1);   // (the rate form is kept apart: negative)
        plat_mutex_unlock(&drain_lock);
        atomic_store(&drain_mgr, c->x[0]);
        if (atomic_fetch_add(&drain_kept_n, 1) < 5)
            rt_log("coop: test: drainmove: %s %.4f from a tracked part's node kept", off == GAUGE_CONSUME ? "consume" : "consumeRate", amount);
        return 1;
    }
    return 0;
}
// TOTK_COOP_TEST_RECBUILD=<s> (Phase 0 item 3): at that time the tracked contraption is made again 6 m to the side
// (+z) from its parts' body matrices at that moment: each part requested (coop_request_create), each copy held at its
// original's matrix plus the offset every tick until all are claimed, then every fan bonded to the wing in one pass
// (coop_bonds_bond, the game's plain bond from the two bodies, main+0x1c05a98), held 2 ticks more and let go. For 4 s:
// each copy's group and member count, and how far each copy sits from the wing copy against the originals.
#define RB_N 8
static struct { uint64_t orig, copy; float m[12]; char name[48]; uint8_t handle[0x40]; int wing; } rb[RB_N];
static double prev_solid_at;
static void recbuild_test(Cpu *c, int n) {
    static double at = -1, done_at;
    static int st, rn, hold;
    static double holding = 1;   // TOTK_COOP_TEST_RECBUILD=<s>,0: the copies not held (made at the right place already)
    if (at < 0) at = cfg_at("TOTK_COOP_TEST_RECBUILD", &holding), holding = strchr(cfg_get("TOTK_COOP_TEST_RECBUILD") ? cfg_get("TOTK_COOP_TEST_RECBUILD") : "", ',') ? holding : 1;
    if (at <= 0) return;
    double t = frame_step_world_us() / 1e6;
    if (t < at) return;
    if (st == 0) {
        char nm[48];
        for (int i = 0; i < n && rn < RB_N; i++) {
            if (!live_device(i, nm, sizeof nm)) continue;
            uint64_t a = atomic_load(&tracked[i]);
            uint64_t body = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50) + 0x20) + 0x150);
            if (!body || !coop_rd(body + 0x98, rb[rn].m, sizeof rb[rn].m)) continue;
            float pos[3] = {0};
            coop_rd(a + COOP_ACTOR_POS, pos, 12);
            rb[rn].orig = a, rb[rn].wing = !strcmp(nm, "SpObj_LiftGeneratorWing_A_01");
            snprintf(rb[rn].name, sizeof rb[rn].name, "%s", nm);
            rt_log("coop: test: recbuild: %s body matrix %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f pos %.2f %.2f %.2f (actor %.2f %.2f %.2f)",
                   nm, rb[rn].m[0], rb[rn].m[1], rb[rn].m[2], rb[rn].m[4], rb[rn].m[5], rb[rn].m[6], rb[rn].m[8], rb[rn].m[9],
                   rb[rn].m[10], rb[rn].m[3], rb[rn].m[7], rb[rn].m[11], pos[0], pos[1], pos[2]);
            rb[rn].m[11] += 6.0f;
            float p[3] = {rb[rn].m[3], rb[rn].m[7], rb[rn].m[11]}, r[9] = {rb[rn].m[0], rb[rn].m[1], rb[rn].m[2], rb[rn].m[4],
                                                                              rb[rn].m[5], rb[rn].m[6], rb[rn].m[8], rb[rn].m[9], rb[rn].m[10]};
            if (cfg_get("TOTK_COOP_TEST_RECBUILD_IGNORE")) p[1] += 3.0f * (float)(rn + 1);   // made apart, put together later
            // TOTK_COOP_TEST_RECBUILD_AB=1: made as the Autobuild makes its holograms (its type object, with
            // TOTK_COOP_TEST_ABARGS=1 its other create fields, and the callback that marks it autobuilt), made solid
            // by the confirm's own call 0.1 s after the bonds
            if (cfg_get("TOTK_COOP_TEST_RECBUILD_AB")) coop_request_create_autobuilt(c, nm, p, r, rb[rn].handle);
            else coop_request_create(c, nm, p, r, rb[rn].handle);
            rn++;
        }
        st = 1;
        return;
    }
    int all = 1;
    for (int k = 0; k < rn; k++) {
        if (!rb[k].copy && coop_handle_status(rb[k].handle) == 3) {
            static _Alignas(16) uint8_t link[0x40];
            uint64_t ref = 0;
            rb[k].copy = coop_claim(c, rb[k].handle, link, &ref);
            // TOTK_COOP_TEST_RECBUILD_PREVIEW=1: made a hologram at once, as an Autobuild's parts are (main+0x1c071f0,
            // the preview mark), and solid only after the bonds, by the confirm's own call (main+0x1c07114)
            uint64_t spr = rb[k].copy ? coop_rd_ptr(coop_rd_ptr(rb[k].copy + 0x228) + 0x480) : 0;
            if (spr && cfg_get("TOTK_COOP_TEST_RECBUILD_PREVIEW")) coop_guest_call(c, MOD_MAIN_BASE + 0x1c071f0, spr, 0, 0, 0);
        }
        if (!rb[k].copy) all = 0;
    }
    if (holding && (st == 1 || hold > 0))   // held at the exact place
        for (int k = 0; k < rn; k++)
            if (rb[k].copy) coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, rb[k].copy, (uint64_t)(uintptr_t)rb[k].m, 0, 0);
    // TOTK_COOP_TEST_RECBUILD_IGNORE=1: once all are made (apart), every pair set to ignore each other
    // (main+0x2ae6cf4, the pair manager Ultrahand's release uses), two ticks later all put at their places, bonded the
    // tick after
    static int ig_ticks = -1;
    if (st == 1 && all && cfg_get("TOTK_COOP_TEST_RECBUILD_IGNORE") && ig_ticks < 0) {
        static uint64_t self;
        int pairs = 0;
        for (int i = 0; i < rn; i++)
            for (int j = i + 1; j < rn; j++) {
                self = rb[i].copy;
                pairs += (int)(coop_guest_call(c, MOD_MAIN_BASE + 0x2ae6cf4, (uint64_t)(uintptr_t)&self, rb[j].copy, 0, 0) & 1);
            }
        rt_log("coop: test: recbuild: %.2f s %d pairs set to ignore each other (%d answered 1)", t, rn * (rn - 1) / 2, pairs);
        ig_ticks = 0;
        return;
    }
    if (ig_ticks >= 0 && st == 1) {
        if (++ig_ticks == 3)
            for (int k = 0; k < rn; k++) coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, rb[k].copy, (uint64_t)(uintptr_t)rb[k].m, 0, 0);
        if (ig_ticks < 4) return;
    }
    if (st == 1 && all) {
        int w = -1, made = 0;
        for (int k = 0; k < rn; k++) if (rb[k].wing) w = k;
        int coop_bonds_bond(Cpu *c, uint64_t a, uint64_t b), coop_bonds_bond_at(Cpu *c, uint64_t a, uint64_t b);
        int local = cfg_get("TOTK_COOP_TEST_RECBUILD_LOCAL") != NULL;   // the frames in each body's space (coop_bonds_bond_at)
        for (int k = 0; w >= 0 && k < rn; k++)
            if (k != w) made += local ? coop_bonds_bond_at(c, rb[k].copy, rb[w].copy) : coop_bonds_bond(c, rb[k].copy, rb[w].copy);
        rt_log("coop: test: recbuild: %.2f s all %d copies made; %d bonds made to the wing copy in one pass", t, rn, made);
        if (cfg_get("TOTK_COOP_TEST_RECBUILD_PREVIEW") || cfg_get("TOTK_COOP_TEST_RECBUILD_AB")) prev_solid_at = t + 0.1;
        st = 2, hold = 2, done_at = t;
        return;
    }
    if (hold > 0) hold--;
    if (prev_solid_at > 0 && t >= prev_solid_at) {   // the confirm's mark: preview off, solid (and green)
        for (int k = 0; k < rn; k++) {
            uint64_t spr = coop_rd_ptr(coop_rd_ptr(rb[k].copy + 0x228) + 0x480);
            if (spr) coop_guest_call(c, MOD_MAIN_BASE + 0x1c07114, spr, 0, 0, 0);
        }
        rt_log("coop: test: recbuild: %.2f s the copies made solid (main+0x1c07114)", t);
        prev_solid_at = 0;
    }
    static int ticks;
    if (st == 2 && ticks < 40) {   // each tick for 40 ticks: the copies' group, device state (+0x28 of comp166) and speed
        ticks++;
        char line[600];
        int len = snprintf(line, sizeof line, "coop: test: recbuild: tick %d (%.3f s)", ticks, t);
        for (int k = 0; k < rn; k++) {
            uint64_t a = rb[k].copy, tab = coop_rd_ptr(a + 0x228);
            uint64_t g = coop_rd_ptr(coop_rd_ptr(tab + 0x480) + 0x28), d = coop_rd_ptr(tab + 0x530);
            int32_t ds = -1;
            if (d) coop_rd(d + 0x28, &ds, 4);
            float v[3] = {0};
            coop_rd(a + 0x320, v, 12);
            len += snprintf(line + len, sizeof line - (size_t)len, " | %.10s g %llx state %d v %.1f %.1f %.1f", rb[k].name + 6,
                            (unsigned long long)(g & 0xffffff), ds, v[0], v[1], v[2]);
        }
        rt_log("%s", line);
    }
    static double next;
    if (st == 2 && t >= next && t < done_at + 4.2) {
        next = t + 0.5;
        int w = -1;
        for (int k = 0; k < rn; k++) if (rb[k].wing) w = k;
        if (w < 0) return;
        float ow[3], cw[3];
        coop_rd(rb[w].orig + COOP_ACTOR_POS, ow, 12), coop_rd(rb[w].copy + COOP_ACTOR_POS, cw, 12);
        char line[600];
        int len = snprintf(line, sizeof line, "coop: test: recbuild: %.2f s", t);
        for (int k = 0; k < rn; k++) {
            float o[3], cp[3];
            coop_rd(rb[k].orig + COOP_ACTOR_POS, o, 12), coop_rd(rb[k].copy + COOP_ACTOR_POS, cp, 12);
            float e = 0;
            for (int i = 0; i < 3; i++) { float d = (cp[i] - cw[i]) - (o[i] - ow[i]); e += d * d; }
            uint64_t g = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(rb[k].copy + 0x228) + 0x480) + 0x28);
            uint32_t gn = 0;
            if (g) coop_rd(g + 0x58, &gn, 4);
            len += snprintf(line + len, sizeof line - (size_t)len, " | %.10s group %llx (%u) off by %.3f m at %.1f %.1f %.1f",
                            rb[k].name + 6, (unsigned long long)g, gn, sqrtf(e), cp[0], cp[1], cp[2]);
        }
        rt_log("%s", line);
    }
}
// TOTK_COOP_TEST_BONDQ=<from>,<to>: who empties the bond queue the Autobuild confirm fills (each part A's SPR +0x2a0,
// an intrusive list, count at +0x2b0; main+0x1c07604 appends): from <from> to <to> world seconds the writes to the
// first fan's list head are watched (coop_watch_win.c; the writers are logged when it ends), and the count every 0.1 s.
static void bondq_test(int n) {
    static double from = -1, to;
    static int st;
    static uint64_t spr;
    if (from < 0) from = cfg_at("TOTK_COOP_TEST_BONDQ", &to);
    if (from <= 0) return;
    double t = frame_step_world_us() / 1e6;
    int coop_watch_arm(uint64_t addr, uint64_t len);
    void coop_watch_disarm(void);
    extern int coop_watch_writes;
    if (st == 0 && t >= from) {
        char nm[48];
        for (int i = 0; i < n && !spr; i++)
            if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_WindGenerator_A_01"))
                spr = coop_rd_ptr(coop_rd_ptr(atomic_load(&tracked[i]) + 0x228) + 0x480);
        if (!spr) return;
        coop_watch_writes = 1;
        uint32_t cnt = 0;
        coop_rd(spr + 0x2b0, &cnt, 4);
        rt_log("coop: test: bondq: queued bonds before: %u", cnt);
        rt_log("coop: test: bondq: watching the fan's SPR %llx +0x2a0 from %.2f s: %d", (unsigned long long)spr, t,
               coop_watch_arm(spr + 0x2a0, 0x18));
        st = 1;
    }
    // (the count is read only outside the watch: a read of the watched page from here would fault too)
    if (st == 1 && t >= to) {
        coop_watch_disarm();
        uint32_t cnt = 0;
        coop_rd(spr + 0x2b0, &cnt, 4);
        rt_log("coop: test: bondq: %.2f s watch ended, queued bonds now %u", t, cnt);
        st = 2;
    }
}
// TOTK_COOP_TEST_FINDAB=<s>: at that time, where this game's Autobuild ability is (the Ultrahand part, vtable
// main+0x3695798, 0xd0b8 bytes; the Autobuild's build object hangs at its +0x51f0): searched from this Link, its first
// 0x2000 bytes and each component's first 0x800, as a direct pointer to the part or one pointer away (an object whose
// +0x80 or +0x88 is it, as the power system's). Each place logged, with the part's +0x51f0 and the build object kept
// by a build here (coop_ab_obj), to compare.
_Atomic uint64_t coop_ab_ability;   // this game's Autobuild ability, from its update (coop_spawn.c coop_hook)
static int is_uh_part(uint64_t p) {   // the Autobuild ability (vtable main+0x3695200: its update main+0x9a2c00 at +0x38)
    return p && !(p & 7) && p > 0x100000000ull && p < 0x8000000000ull && coop_rd_ptr(p) == MOD_MAIN_BASE + 0x3695200;
}
static uint64_t ab_via(uint64_t q) {   // q itself, or an object q points to within its first 0x400 bytes
    if (is_uh_part(q)) return q;
    if (!q || (q & 7) || q < 0x100000000ull || q >= 0x8000000000ull) return 0;
    for (uint64_t o = 0; o < 0x400; o += 8) { uint64_t r = coop_rd_ptr(q + o); if (is_uh_part(r)) return r; }
    return 0;
}
static void findab_test(uint64_t me) {
    static double at = -1;
    if (at < 0) at = cfg_at("TOTK_COOP_TEST_FINDAB", NULL);
    if (at <= 0 || !me || frame_step_world_us() / 1e6 < at) return;
    at = 0;
    int found = 0;
    uint64_t tab = coop_rd_ptr(me + 0x228);
    for (int src = -1; src < 0x200 && found < 12; src++) {
        uint64_t base = src < 0 ? me : coop_rd_ptr(tab + 8 * (uint64_t)src);
        uint64_t len = src < 0 ? 0x2000 : 0x800;
        if (!base || (src >= 0 && !tab)) continue;
        for (uint64_t off = 0; off < len && found < 12; off += 8) {
            uint64_t q = coop_rd_ptr(base + off);
            if (!q || (q & 7) || q < 0x100000000ull || q >= 0x8000000000ull) continue;
            uint64_t part = ab_via(q);
            const char *how = is_uh_part(q) ? "direct" : "one pointer on";
            if (!part) continue;
            rt_log("coop: test: findab: %s%#x +%#llx: the Autobuild ability %llx (%s); its +0x51f0 %llx, +0x51f8 %llx; this game's build object %llx",
                   src < 0 ? "Link" : "component ", src < 0 ? 0 : src * 8, (unsigned long long)off, (unsigned long long)part, how,
                   (unsigned long long)coop_rd_ptr(part + 0x51f0), (unsigned long long)coop_rd_ptr(part + 0x51f8), (unsigned long long)coop_ab_obj);
            found++;
        }
    }
    rt_log("coop: test: findab: %d places", found);
}
static void phase0_tick(Cpu *c, int n) {
    recbuild_test(c, n);
    bondq_test(n);
    double t = frame_step_world_us() / 1e6;
    static double next;
    if (t < next) return;
    next = t + 0.5;
    if (cfg_get("TOTK_COOP_TEST_NOSELFPUSH")) {
        extern uint64_t coop_fan_actor;
        float p[3] = {0};
        uint64_t who = n ? atomic_load(&tracked[0]) : coop_fan_actor;
        coop_rd(who + COOP_ACTOR_POS, p, 12);
        // TOTK_COOP_TEST_FANON=<s>: the loose test fan (TOTK_COOP_TEST_FANSET) switched on at that time by its toggle
        // request (DeviceSwitch +0x8d bit 3), as a hit does
        static int on_done;
        double on_at = cfg_at("TOTK_COOP_TEST_FANON", NULL);
        if (!on_done && on_at > 0 && t >= on_at && coop_fan_actor) {
            uint64_t d = coop_rd_ptr(coop_rd_ptr(coop_fan_actor + 0x228) + 0x530);
            if (d) __atomic_fetch_or((uint8_t *)(uintptr_t)(d + 0x8d), 8, __ATOMIC_SEQ_CST);
            rt_log("coop: test: noselfpush: %.1f s the test fan's toggle request set (switch %llx)", t, (unsigned long long)d);
            on_done = 1;
        }
        rt_log("coop: test: noselfpush: %.1f s skipped %d passed %d, part 0 at %.2f %.2f %.2f", t, atomic_load(&push_skipped),
               atomic_load(&push_passed), p[0], p[1], p[2]);
    }
    double to = 0, at = cfg_at("TOTK_COOP_TEST_DRAINMOVE", &to);
    if (at > 0) {
        uint64_t mgr = atomic_load(&drain_mgr);
        static int paid;
        if (mgr && !paid && t >= to) {   // the kept one-shot charges, at once
            plat_mutex_lock(&drain_lock);
            double k = drain_kept;
            plat_mutex_unlock(&drain_lock);
            float amount = (float)k;
            uint32_t bits;
            memcpy(&bits, &amount, 4);
            c->v[0] = bits;
            coop_guest_call(c, MOD_MAIN_BASE + GAUGE_CONSUME, mgr, 0, 0, 0);
            rt_log("coop: test: drainmove: %.1f s: the kept %.4f charged at once (%d charges kept)", t, k, atomic_load(&drain_kept_n));
            paid = 1;
        }
        if (mgr) {
            float f54 = 0, f58 = 0;
            int32_t pend = 0;
            coop_rd(mgr + 0x54, &f54, 4), coop_rd(mgr + 0x58, &f58, 4), coop_rd(mgr + 0x30, &pend, 4);
            plat_mutex_lock(&drain_lock);
            double k = drain_kept;
            plat_mutex_unlock(&drain_lock);
            rt_log("coop: test: drainmove: %.1f s gauge +0x54 %.1f +0x58 %.1f pending %d, kept %.4f", t, f54, f58, pend, k);
        }
    }
}

void coop_autobuild_tick(Cpu *c, uint64_t me) {
    playerdiff_test(me);
    coop_ctn_tick(c);
    if (!ablog_on()) return;
    deck_test(c, me, atomic_load(&tracked_n) < TRACK_N ? atomic_load(&tracked_n) : TRACK_N);
    failspam_test(c, me);
    if (cfg_get("TOTK_COOP_TEST_FANHITAB") && atomic_load(&tracked_n) > 1) {   // FANHIT on the build's first fan from
        extern uint64_t coop_fan_actor;                                        // when it is made (the confirm's calls)
        char nm[48];
        for (int i = 0; i < atomic_load(&tracked_n) && i < TRACK_N; i++)
            if (live_device(i, nm, sizeof nm) && !strcmp(nm, "SpObj_WindGenerator_A_01")) { coop_fan_actor = fanhit_target(atomic_load(&tracked[i])); break; }
    }
    abfile_test();
    rebuild_test(c);
    rebuild_ok(c);
    compdump_test(atomic_load(&tracked_n) < TRACK_N ? atomic_load(&tracked_n) : TRACK_N);
    sweep_test(c, me);
    gluenear_test(c, me, atomic_load(&tracked_n));
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
    if (n) serial_test(c, n);
    phase0_tick(c, n);
    findab_test(me);
    rebuild_go_log();
    partcmp_test(n);
    rebuild_go_watch();
    if (!cfg_get("TOTK_COOP_TEST_SERIALIN")) serial_try(c, "the co-op tick");
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
        // (=c28,<from s>,<to s>: the first live fan's component 28 +0x398, its draw on the battery: every read and write)
        static int c28;
        if (e && st == 0 && !strncmp(e, "c28,", 4) && sscanf(e + 4, "%lf,%lf", &from, &to) == 2) st = 4, c28 = 1;
        if (st == 4 && now / 1e6 >= from) {
            char nm[48];
            for (int i = 0; i < n && st == 4; i++) {
                if (!live_device(i, nm, sizeof nm) || strcmp(nm, "SpObj_WindGenerator_A_01")) continue;
                uint64_t t = coop_rd_ptr(atomic_load(&tracked[i]) + 0x228), c166 = t ? coop_rd_ptr(t + (c28 ? 8 * 28 : 0x530)) : 0;
                if (c166) at = c166 + (c28 ? 0x398 : 0x28), len = c28 ? 4 : 8, st = 1,
                          rt_log("coop: test: abwatch: device %d's component %d +%#x (%llx)", i, c28 ? 28 : 166, c28 ? 0x398 : 0x28, at);
            }
        }
        int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
        void coop_watch_disarm(void);
        extern int coop_watch_writes;
        if (st == 1 && now / 1e6 >= from) { coop_watch_writes = !c28; st = coop_watch_arm(at, len) ? 2 : 3; }
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
    // TOTK_COOP_TEST_GLUEWATCH=1: a write watch on the first device's glue component +0x20..+0x30 (its group at +0x28)
    // for 14 s: who sets it. Off by default and never in pair runs: the watch makes the whole page inaccessible, so the
    // co-op's own reads of that part fail meanwhile (docs/coop/contraption-sync.md, "Redesign: contraption sync v3").
    static int watch_stage;
    if (watch_stage == 0 && (!cfg_get("TOTK_COOP_TEST_GLUEWATCH") || cfg_get("TOTK_COOP_TEST_ABWATCH") || cfg_get("TOTK_COOP_TEST_BONDQ") || cfg_get("TOTK_COOP_TEST_RECBUILD") ||
                             cfg_get("TOTK_COOP_TEST_SERIAL") || cfg_get("TOTK_COOP_TEST_REBUILDGO")))
        watch_stage = 2;   // (the one watch is another test's then; and it slows the game: every access to that page faults)
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
