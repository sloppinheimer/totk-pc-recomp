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
//    option "aim1=<fan>[@<s>]" / "aim2=<fan>[@<s>]" (fan 0 to 2; "near1" / "near2" is fan 0): that player's Link is
//    put 2.5 m from that fan, and from world second <s> the middle of its picture is brought onto the fan
//    (bench_aim_tick), for an input script that takes the fan with Ultrahand as a player does (tools/coop/hand_*.txt:
//    L, a tilt down, then A; <s> is after the tilt and before the A).
//    option "on=@<s>": at that second game 1 switches on the devices of everything it owns (the path a remote
//    player's hit takes at the owner, ctn_toggle_put): a build that moves by its own devices.
//    option "saved": at 16 s each game makes fans that get no identity, as devices that come with a save: two in
//    game 1, one in game 2 (the host's are to be in both games after, the other's gone: coop_ent_saved_tick).
//    option "nofans": the Links are placed and no fan is made (a script that makes its own parts, as an Autobuild).
//    option "join1=@<s>" / "join2=@<s>": from that second what that Link holds is brought next to the nearest part of
//    another island (bench_join_tick), for a script whose next A attaches it.
// The scenario moves the owner's object; what the other game shows of it is the thing under test. "drive" writes
// velocities, which no player does: it tests the following and the resting, not a grab, a hold or a hand-over.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctn.h"

// The z of the first fan of the row; the others, the Links and the fans of "saved" are placed from it. It was 1509.2
// until a suite run's pictures showed a construct one can talk to standing at the third fan's place (z 1512 to 1513):
// each game moves its own, nothing sends it, and in the second game it pushed that fan's copy 0.49 m off and the Link
// aside, while the owner's fan lay 5 degrees up on it.
#define BENCH_Z 1507.2f
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"

int ctn_alive(uint64_t a);
void coop_ent_test_mint(uint64_t actor);   // coop_ent_id.c
void coop_lock_wait(PlatMutex *m);

// ---- The audit ---------------------------------------------------------------------------------------------------------
typedef struct { FILE *f; int n; } LooseScan;
static void loose_fn(uint64_t a, void *arg) {
    LooseScan *s = arg;
    char nm[48];
    float p[3];
    if (!ent_is_loose(a, nm, p)) return;
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
        {   // its device as its island's row has it: with a switch, and on (at the owner computed, at a follower from POSE)
            int sw = 0, on = 0;
            coop_lock_wait(&grp_lock);
            for (int g = 0; g < GRP_N; g++) {
                if (grps[g].used != 1) continue;
                for (int r = 0; r < grps[g].n && r < ENT_MEMBERS; r++)
                    if (grps[g].id[r].inc == e.id.inc && grps[g].id[r].serial == e.id.serial) sw = grps[g].sw >> r & 1, on = grps[g].on >> r & 1;
            }
            plat_mutex_unlock(&grp_lock);
            fprintf(f, ",\"sw\":%d,\"on\":%d", sw, on);
        }
        if (have) {   // the SPR's flag words, as they are (what marks a part made from zonaite is read from them)
            uint64_t spr = ctn_spr(e.actor), f20 = 0;
            uint32_t f150 = 0;
            if (spr) coop_rd(spr + 0x20, &f20, 8), coop_rd(spr + 0x150, &f150, 4);
            fprintf(f, ",\"spr20\":\"%llx\",\"spr150\":\"%x\"", (unsigned long long)f20, f150);
        }
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
    // (a game in a menu moves nothing, though its actor phase 0 may go on: the report judges no place then)
    int coop_runs_for_ms(void);   // coop_players.c: -1 while this game sits in a menu or a load
    int coop_ent_log_mark(void);   // coop_ent_log.c: the tester's marks so far
    fprintf(f, ",\"paused\":%d,\"loading\":%d,\"menu\":%d,\"mark\":%d", ent_paused(), ent_loading(), coop_runs_for_ms() < 0, coop_ent_log_mark());
}

// ---- The scene and the scenario ------------------------------------------------------------------------------------------
#define FANS 3
static double bench_join_at[2];   // per player: the world second from which what its Link holds is brought to a neighbour

// What this Link's Ultrahand holds brought next to the nearest entity that is not of its island (join<id>=@<s>, for
// 2.5 s from that second), for a script whose next A attaches it: walking a held part to another by timed stick input
// ended beside it or short of it by the run. The held part follows its Link, so the Link is moved by what the two
// centres are apart less 0.9 m, at most 0.6 m every sixth frame.
static void bench_join_tick(Cpu *c, uint64_t me, int id, double now) {
    int w = id == 1 ? 0 : 1;
    double t0 = bench_join_at[w];
    static int n;
    static double said;
    if (t0 <= 0 || now < t0 || now > t0 + 2.5 || n++ % 6) return;
    uint64_t held = ent_held_actor();
    Ent *he = held ? ent_by_actor(held) : NULL, *set[64];
    float hm[12], best = 1e9f, T[3] = {0};
    if (!he || !he->main_body || !coop_rd(he->main_body + 0x98, hm, sizeof hm)) { if (now - said > 1) said = now, rt_log("coop: bench: %.2f s: join: this Link holds no entity", now); return; }
    int ns = ent_island_members(he, set, 64);
    for (int i = 0; i < ENT_N; i++) {
        Ent *e = &ents[i];
        float m[12];
        int same = 0;
        if (!e->used || e == he || !e->actor || e->life != LIFE_LIVE || (e->flags & ENT_PREVIEW) || !e->main_body || !coop_rd(e->main_body + 0x98, m, sizeof m)) continue;
        for (int k = 0; k < ns; k++) same |= set[k] == e;
        if (same) continue;
        float d = sqrtf((m[3] - hm[3]) * (m[3] - hm[3]) + (m[11] - hm[11]) * (m[11] - hm[11]));
        if (d < best) best = d, T[0] = m[3], T[1] = m[7], T[2] = m[11];
    }
    if (best > 1e8f) return;
    if (best < 1.0f) { if (now - said > 0.5) said = now, rt_log("coop: bench: %.2f s: join: the held part is %.2f m from its neighbour", now, best); return; }
    float s = (best - 0.9f) / best, dx = (T[0] - hm[3]) * s, dz = (T[2] - hm[11]) * s, l = sqrtf(dx * dx + dz * dz);
    if (l > 0.6f) dx *= 0.6f / l, dz *= 0.6f / l;
    float p[3], lr[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, lr, 36)) return;
    float m[12] = {lr[0], lr[1], lr[2], p[0] + dx, lr[3], lr[4], lr[5], p[1], lr[6], lr[7], lr[8], p[2] + dz};
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
    if (now - said > 0.5) said = now, rt_log("coop: bench: %.2f s: join: the held part is %.2f m from its neighbour: this Link moved by %.2f %.2f", now, best, dx, dz);
}
static int bench_scene, bench_drive, bench_nofans, bench_saved;
static double bench_on_at;
static int bench_aim[2] = {-1, -1};   // per player (id 1, id 2): the fan its Link is put in front of, -1 none
static double bench_aim_at[2];        // and the world second from which the middle of its picture is brought onto that fan

// The middle of this game's picture brought onto a fan (aim<id>=<fan>@<s>, for 1.2 s from that second): Ultrahand takes
// what the middle of the picture is on, and where the camera looks after a load, a placement and a stick tilt is not
// the same in every run. The camera keeps its place relative to its Link, so the Link is moved sideways by what the
// camera's line of sight misses the fan by at the fan's height, every sixth frame, until it is within 0.1 m. The fan is
// the entity nearest the place fan <fan> was made at.
static void bench_aim_tick(Cpu *c, uint64_t me, int id, double now) {
    int w = id == 1 ? 0 : 1, k = bench_aim[w];
    double t0 = bench_aim_at[w];
    static int n, on_target, said_fail;
    if (k < 0 || t0 <= 0 || now < t0 || now > t0 + 1.2 || on_target) {
        if (k >= 0 && t0 > 0 && now > t0 + 1.2 && !on_target && !said_fail) said_fail = 1, rt_log("coop: bench: %.2f s: aim: the picture's middle was not brought onto fan %d: the grab will not land", now, k);
        return;
    }
    if (n++ % 6) return;
    uint64_t cam = coop_resident_named("PlayerCamera", NULL);
    float C[3], r[9], spot[3] = {29.8f, 1514.5f, BENCH_Z + 1.5f * k}, T[3], best = 1.5f;
    int found = 0;
    if (!cam || !coop_rd(cam + COOP_ACTOR_POS, C, 12) || !coop_rd(cam + COOP_ACTOR_ROT, r, 36)) return;
    for (int i = 0; i < ENT_N; i++) {
        Ent e = ents[i];
        float m[12];
        if (!e.used || !e.actor || !ctn_alive_as(e.actor, e.gen) || !e.main_body || !coop_rd(e.main_body + 0x98, m, sizeof m)) continue;
        float d = sqrtf((m[3] - spot[0]) * (m[3] - spot[0]) + (m[11] - spot[2]) * (m[11] - spot[2]));
        if (d < best) best = d, T[0] = m[3], T[1] = m[7], T[2] = m[11], found = 1;
    }
    if (!found) { rt_log("coop: bench: %.2f s: aim: no entity within 1.5 m of where fan %d was made", now, k); return; }
    float F[3] = {-r[2], -r[5], -r[8]};   // (it looks along -Z: the third column, negated)
    if (F[1] > -0.05f) { rt_log("coop: bench: %.2f s: aim: the camera looks level or up (%.2f): tilt it down first", now, F[1]); return; }
    float t = (T[1] - C[1]) / F[1], dx = T[0] - (C[0] + F[0] * t), dz = T[2] - (C[2] + F[2] * t), miss = sqrtf(dx * dx + dz * dz);
    if (miss < 0.1f) { on_target = 1; rt_log("coop: bench: %.2f s: aim: the picture's middle is on fan %d (%.2f m off, %.1f m from the camera)", now, k, miss, t); return; }
    float p[3], lr[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, lr, 36)) return;
    float m[12] = {lr[0], lr[1], lr[2], p[0] + dx, lr[3], lr[4], lr[5], p[1], lr[6], lr[7], lr[8], p[2] + dz};
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
    rt_log("coop: bench: %.2f s: aim: %.2f m off fan %d at %.1f m: this Link moved by %.2f %.2f", now, miss, k, t, dx, dz);
}
static void bench_read(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = cfg_get("TOTK_COOP_BENCH");
    if (!e || !*e) return;
    bench_scene = !strncmp(e, "fans", 4);
    bench_drive = bench_scene && strstr(e, ",drive") != NULL;
    bench_nofans = bench_scene && strstr(e, ",nofans") != NULL;
    bench_saved = bench_scene && strstr(e, ",saved") != NULL;
    { const char *o = bench_scene ? strstr(e, ",on=@") : NULL; if (o) bench_on_at = atof(o + 5); }
    for (int k = 0; bench_scene && k < 2; k++) {
        char key[16];
        snprintf(key, sizeof key, ",aim%d=", k + 1);
        const char *a = strstr(e, key);
        if (a && a[6] >= '0' && a[6] < '0' + FANS) {
            bench_aim[k] = a[6] - '0';
            if (a[7] == '@') bench_aim_at[k] = atof(a + 8);
        }
        snprintf(key, sizeof key, ",join%d=@", k + 1);
        if ((a = strstr(e, key))) bench_join_at[k] = atof(a + 8);
        snprintf(key, sizeof key, ",near%d", k + 1);
        if (strstr(e, key)) bench_aim[k] = 0;
    }
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
    if (!placed && now > 15) {
        // Facing -x, the fans' row 2 m ahead. A Link that aims at a fan (aim<id>=<fan>, near<id>) is put 2.5 m from
        // where that fan is made, on the line its camera looks along, facing as the camera does: Ultrahand takes
        // what the middle of the picture is on, and the camera keeps its direction when its Link is moved. (A ZL
        // press to bring the camera round locked onto the other player's stand-in instead, in one run of three.)
        float z = id == 1 ? BENCH_Z + 0.5f : BENCH_Z + 2.5f, x = 31.8f, fx = -1, fz = 0;
        int aim = bench_aim[id == 1 ? 0 : 1];
        if (aim >= 0) {
            uint64_t cam = coop_resident_named("PlayerCamera", NULL);
            float r[9];
            if (cam && coop_rd(cam + COOP_ACTOR_ROT, r, sizeof r)) {   // (it looks along -Z: the third column, negated)
                float l = sqrtf(r[2] * r[2] + r[8] * r[8]);
                if (l > 1e-3f) fx = -r[2] / l, fz = -r[8] / l;
            }
            x = 29.8f - 2.5f * fx, z = BENCH_Z + 1.5f * aim - 2.5f * fz;
        }
        float m[12] = {fz, 0, fx, x, 0, 1, 0, 1514.8f, -fx, 0, fz, z};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        placed = 1;
        rt_log("coop: bench: %.1f s: this Link (player %d) put at %.1f 1514.8 %.1f, facing %.2f %.2f%s", now, id, x, z, fx, fz, aim >= 0 ? " as its camera, at a fan" : "");
    }
    {   // saved: at 16 s each game makes fans with no identity, as a save's devices are: two in game 1, one in game 2
        static _Alignas(16) uint8_t sh[2][0x40], sl[2][0x40];
        static int sst[2];
        int want = !bench_saved ? 0 : id == 1 ? 2 : 1;
        for (int k = 0; k < want && now > 16; k++) {
            if (sst[k] == 0) {
                // (in a row across both Links' view, turned to face them, so a picture shows which are there)
                float at[3] = {28.3f, 1515.0f, id == 1 ? BENCH_Z + 0.8f + 1.2f * k : BENCH_Z + 3.4f}, rot[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
                sst[k] = coop_request_create(c, "SpObj_WindGenerator_A_01", at, rot, sh[k]) ? 1 : 9;
            }
            if (sst[k] == 1 && coop_handle_status(sh[k]) == 3) {
                uint64_t ref, a = coop_claim(c, sh[k], sl[k], &ref);
                sst[k] = 2;
                rt_log("coop: bench: %.1f s: a fan with no identity made in this game (player %d), as a save's: %llx", now, id, (unsigned long long)a);
            }
        }
    }
    bench_aim_tick(c, me, id, now);
    bench_join_tick(c, me, id, now);
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
    {   // on=@<s>: at that second the devices of everything this game owns are switched on, as a hit on one does
        static int done_on;
        if (bench_on_at > 0 && now > bench_on_at && !done_on) {
            int n = 0;
            done_on = 1;
            for (int i = 0; i < ENT_N; i++) {
                Ent e = ents[i];
                if (e.used && e.owner == coop_ctr_me() && e.actor && e.life == LIFE_LIVE && ctn_alive_as(e.actor, e.gen)) ctn_toggle_put(c, e.actor, 1), n++;
            }
            rt_log("coop: bench: %.1f s: the devices of %d entities of this game are switched on", now, n);
        }
    }
    if (!ready && bench_nofans && now > 17) ready = 1, ready_at = now, rt_log("coop: bench: %.1f s: scene ready: no fans asked for", now);
    if (!ready && now > 17) {
        int all = 1;
        for (int k = 0; k < FANS; k++) {
            if (stage[k] == 0) {
                float at[3] = {29.8f, 1515.0f, BENCH_Z + 1.5f * k}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
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
