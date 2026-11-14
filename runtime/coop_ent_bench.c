// Co-op: the test bench of the entity sync. Two parts, both for tests only.
//
// 1. The audit (coop_ent_audit): once a second coop_players.c writes what this game shows to logs/audit.jsonl; this adds
//    every entity (its Eid, owner, life, whether it has an actor here, the place and rotation of its main body) and
//    every Zonai device in the world that is no entity ("loose": a device the sync does not know, as one that came
//    with a save). tools/coop/ent_report.py pairs the two games' lines and says where they differ.
//
// 2. The scene and the scenario (TOTK_COOP_BENCH=<scene>[,<scenario>], both games):
//    scene "fans": 15 s into the world each Link is put on the flat ground of the fan tests, facing -x, the two 2 m
//    apart; at 17 s the game that joined first makes three fans in a row 2 m in front of them and gives each an
//    identity as a device its player put out.
//    scenario "drive" (the game that made the fans, 8 s after the last is made): fan 0 is moved by velocities written
//    to its main body, with no controller: 12 s along a figure of eight 1.5 m above its place while it turns about y,
//    12 s left alone (it falls and rests), a throw with a turn about x for 0.5 s that tips it, 12 s left alone. Every step logs
//    where the fan is against where it was sent, and a last line says whether the action happened as meant, so a
//    scenario that did not land is not read as a result.
//    option "near1" / "near2": that player's Link is put 2.5 m from fan 0, facing it, for an input script that takes
//    the fan with Ultrahand as a player does (tools/coop/hand_*.txt); the other Link stands 2.5 m to the side.
// The scenario moves the owner's object; what the other game shows of it is the thing under test. "drive" writes
// velocities, which no player does: it tests the following and the resting, not a grab, a hold or a hand-over.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"

int ctn_alive(uint64_t a);
void coop_ent_test_mint(uint64_t actor);   // coop_ent_id.c

// ---- The audit ---------------------------------------------------------------------------------------------------------
typedef struct { FILE *f; int n; } LooseScan;
static void loose_fn(uint64_t a, void *arg) {
    LooseScan *s = arg;
    char nm[48];
    float p[3];
    coop_actor_name(a, nm, sizeof nm);
    if (strncmp(nm, "SpObj_", 6) || strstr(nm, "Capsule") || coop_ent_is(a) || !ctn_alive(a) || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    if (s->n >= 64) return;
    fprintf(s->f, "%s{\"name\":\"%s\",\"p\":[%.3f,%.3f,%.3f]}", s->n++ ? "," : "", nm, p[0], p[1], p[2]);
}
void coop_ent_audit(FILE *f) {
    if (!ent_on()) return;
    uint64_t me = coop_ctr_me();
    fputs(",\"ents\":[", f);
    int first = 1;
    for (int i = 0; i < ENT_N; i++) {
        Ent e = ents[i];   // (a copy: phase 0 owns the table; a row caught mid-change shows as one odd line)
        char b[24], nm[48];
        float m[12];
        if (!e.used) continue;
        int have = e.actor && ctn_alive_as(e.actor, e.gen) && e.main_body && coop_rd(e.main_body + 0x98, m, sizeof m);
        int k = 0;
        for (const char *s = e.name; *s && k < 47; s++) if (*s != '"' && *s != '\\' && (unsigned char)*s >= 32) nm[k++] = *s;
        nm[k] = 0;
        // the owner's class as this game has it: its own island's, or what the owner's POSE said
        int cls = e.owner == me ? (e.isl >= 0 && e.isl < ISL_N ? isls[e.isl].cls : 0) : e.st >= 0 && e.st < STATED_N ? stated[e.st].cls : e.cls;
        fprintf(f, "%s{\"id\":\"%s\",\"owner\":\"%04x\",\"mine\":%d,\"life\":%d,\"flags\":%d,\"state\":%d,\"cls\":%d,\"actor\":%d,\"name\":\"%s\"", first ? "" : ",",
                ent_eid_str(e.id, b), (unsigned)(e.owner & 0xffff), e.owner == me, e.life, e.flags, e.state, cls, have, nm);
        first = 0;
        if (have) fprintf(f, ",\"p\":[%.3f,%.3f,%.3f],\"r\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]", m[3], m[7], m[11], m[0], m[1], m[2], m[4], m[5], m[6],
                          m[8], m[9], m[10]);
        fputc('}', f);
    }
    fputs("],\"loose\":[", f);
    LooseScan s = {f, 0};
    coop_actors_live(loose_fn, &s);
    fputs("],\"bonds\":[", f);   // the bonds of this game's world, each as its two ends
    int nb = bond_n;
    for (int i = 0; i < nb && i < BOND_N; i++) {
        char a[24], b[24];
        Bond bd = bonds[i];
        fprintf(f, "%s[\"%s\",\"%s\"]", i ? "," : "", ent_eid_str(bd.a, a), ent_eid_str(bd.b, b));
    }
    fputc(']', f);
    uint64_t held = ent_held_actor();   // what this Link's Ultrahand holds
    Ent *he = held ? ent_by_actor(held) : NULL;
    char hb[24] = "";
    fprintf(f, ",\"hand\":\"%s\"", he ? ent_eid_str(he->id, hb) : held ? "?" : "");
}

// ---- The scene and the scenario ------------------------------------------------------------------------------------------
#define FANS 3
static int bench_scene, bench_drive, bench_near;   // near: the player id whose Link stands at fan 0 (0: none)
static void bench_read(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = cfg_get("TOTK_COOP_BENCH");
    if (!e || !*e) return;
    bench_scene = !strncmp(e, "fans", 4);
    bench_drive = bench_scene && strstr(e, ",drive") != NULL;
    bench_near = !bench_scene ? 0 : strstr(e, ",near1") ? 1 : strstr(e, ",near2") ? 2 : 0;
    if (!bench_scene) rt_log("coop: bench: TOTK_COOP_BENCH=%s: no such scene (fans[,drive])", e);
}
static void set_vel(Cpu *c, uint64_t body, const float v[3], const float w[3]) {
    _Alignas(16) float V[4] = {v[0], v[1], v[2], 0}, W[4] = {w[0], w[1], w[2], 0};
    coop_guest_call(c, MOD_MAIN_BASE + 0x567750, body, (uint64_t)(uintptr_t)V, 0, 0);   // SetLinearVelocity
    coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, body, (uint64_t)(uintptr_t)W, 0, 0);   // SetAngularVelocity
}
void coop_ent_bench_tick(Cpu *c, uint64_t me) {
    bench_read();
    if (!bench_scene || !ent_on() || !me) return;
    static int64_t last_us;
    int64_t us = frame_step_world_us();
    if (us == last_us) return;   // (the co-op tick comes twice a frame)
    last_us = us;
    double now = us / 1e6;
    int id = coop_net_id();
    if (id <= 0) return;
    static int placed;
    if (!placed && now > 15) {   // facing -x, the fans' row 2 m ahead; with near<id>: that Link 2.5 m from fan 0, in line with it
        float z = id == 1 ? 1509.7f : 1511.7f, x = 31.8f;
        if (bench_near == id) x = 32.3f, z = 1509.2f;
        else if (bench_near) z = 1511.7f;
        float m[12] = {0, 0, -1, x, 0, 1, 0, 1514.8f, 1, 0, 0, z};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        placed = 1;
        rt_log("coop: bench: %.1f s: this Link (player %d) put at %.1f 1514.8 %.1f", now, id, x, z);
    }
    {   // what this Link's Ultrahand holds, said at each change: whether a scripted grab landed, and when it ended
        static uint64_t held_was;
        uint64_t held = ent_held_actor();
        if (held != held_was) {
            char nm[48] = "", b[24] = "-";
            Ent *he = held ? ent_by_actor(held) : NULL;
            if (held) coop_actor_name(held, nm, sizeof nm);
            rt_log("coop: bench: %.2f s: this Link's Ultrahand holds %s%s%s", now, held ? nm : "nothing", he ? ", entity " : held ? ", no entity" : "", he ? ent_eid_str(he->id, b) : "");
            held_was = held;
        }
    }
    if (id != 1) return;
    static _Alignas(16) uint8_t handle[FANS][0x40], link[FANS][0x40];
    static uint64_t fan[FANS];
    static int stage[FANS], minted[FANS], ready;
    static double ready_at;
    if (!ready && now > 17) {
        int all = 1;
        for (int k = 0; k < FANS; k++) {
            if (stage[k] == 0) {
                float at[3] = {29.8f, 1515.0f, 1509.2f + 1.5f * k}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                stage[k] = coop_request_create(c, "SpObj_WindGenerator_A_01", at, rot, handle[k]) ? 1 : 9;
            }
            if (stage[k] == 1 && coop_handle_status(handle[k]) == 3) {
                uint64_t ref;
                fan[k] = coop_claim(c, handle[k], link[k], &ref);
                stage[k] = fan[k] ? 2 : 9;
            }
            if (stage[k] == 2 && !minted[k] && ctn_alive(fan[k])) coop_ent_test_mint(fan[k]), minted[k] = 1;
            if (stage[k] == 9) { rt_log("coop: bench: fan %d could not be made: the scene is not ready", k); bench_scene = 0; return; }
            all &= minted[k];
        }
        if (all) {
            ready = 1, ready_at = now;
            rt_log("coop: bench: %.1f s: scene ready: fans %llx %llx %llx", now, (unsigned long long)fan[0], (unsigned long long)fan[1], (unsigned long long)fan[2]);
        }
    }
    if (!ready || !bench_drive) return;
    // ---- scenario "drive" ----
    static float base[3], up0[3];
    static int have_base, tip_said, done;
    static double said;
    static float carry_max;
    double t = now - ready_at - 8;
    uint64_t body = ctn_alive(fan[0]) ? ctn_main_body(fan[0]) : 0;
    float m[12];
    if (done || t < 0) return;
    if (!body || !coop_rd(body + 0x98, m, sizeof m)) { rt_log("coop: bench: %.1f s: fan 0 is gone: the scenario did not land", now), done = 1; return; }
    float pos[3] = {m[3], m[7], m[11]};
    if (!have_base) memcpy(base, pos, sizeof base), have_base = 1, rt_log("coop: bench: %.1f s: drive starts: fan 0 at %.2f %.2f %.2f", now, pos[0], pos[1], pos[2]);
    if (t < 12) {   // the figure of eight
        double a = 2 * 3.14159265 * t / 5, rise = t < 1.5 ? t / 1.5 : 1;
        float tg[3] = {base[0] + 1.2f * (float)sin(a), base[1] + 1.5f * (float)rise, base[2] + 1.2f * (float)sin(2 * a)};
        float tv[3] = {1.2f * (float)(cos(a) * 2 * 3.14159265 / 5), t < 1.5 ? 1.0f : 0, 1.2f * (float)(cos(2 * a) * 4 * 3.14159265 / 5)};
        float v[3], w[3] = {0, 0.8f, 0};
        for (int i = 0; i < 3; i++) v[i] = tv[i] + 4 * (tg[i] - pos[i]);
        v[1] += 0.5f;   // (what a physics step takes off by gravity)
        set_vel(c, body, v, w);
        float off = sqrtf((tg[0] - pos[0]) * (tg[0] - pos[0]) + (tg[1] - pos[1]) * (tg[1] - pos[1]) + (tg[2] - pos[2]) * (tg[2] - pos[2]));
        if (t > 2 && off > carry_max) carry_max = off;
        if (now - said >= 0.5) said = now, rt_log("coop: bench: %.1f s: carry %.1f s: fan at %.2f %.2f %.2f, sent to %.2f %.2f %.2f, %.2f m off", now, t, pos[0], pos[1], pos[2], tg[0], tg[1], tg[2], off);
    } else if (t < 24) {
        if (now - said >= 2) said = now, rt_log("coop: bench: %.1f s: rest: fan at %.2f %.2f %.2f", now, pos[0], pos[1], pos[2]);
        up0[0] = m[1], up0[1] = m[5], up0[2] = m[9];   // its y axis in the world, as it rests
    } else if (t < 24.5) {   // thrown up 5 m/s for 0.1 s, turning 6 rad/s about x for 0.5 s (4 rad/s for 0.3 s on the ground only slid it)
        float v[3] = {0, t < 24.1 ? 5.0f : t < 24.3 ? 2.0f : 0, 0}, w[3] = {6, 0, 0};
        set_vel(c, body, v, w);
    } else if (t < 36) {
        if (now - said >= 2) said = now, rt_log("coop: bench: %.1f s: rest after the tip: fan at %.2f %.2f %.2f", now, pos[0], pos[1], pos[2]);
        if (!tip_said && t > 30) {
            float d = up0[0] * m[1] + up0[1] * m[5] + up0[2] * m[9];
            float turned = (float)(acos(d > 1 ? 1 : d < -1 ? -1 : d) * 180 / 3.14159265);
            tip_said = 1;
            rt_log("coop: bench: %.1f s: tip: fan 0 turned %.0f degrees: %s", now, turned, turned > 20 ? "landed" : "did not land");
            rt_log("coop: bench: carry: at most %.2f m off its path: %s", carry_max, carry_max < 0.5f ? "landed" : "did not land");
        }
    } else done = 1, rt_log("coop: bench: %.1f s: done", now);
}
