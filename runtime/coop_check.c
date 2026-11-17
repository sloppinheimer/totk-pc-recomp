// Co-op: the mismatch checker's record (docs/coop/PLAN.md step 1; tools/coop/check.py reads it).
//
// Each game writes what it shows of the enemies near the Links, read from the game's own memory and never from the
// enemy sync's tables, to logs/check.jsonl:
// - Once a second, at the turn of a second of plat_now_ns (the performance counter, one clock for every game on this
//   PC; the sync audit's turn too), a line "s": every actor of the enemy sync's listed kinds (enemies, and horses and
//   animals when that sync is on) within 120 m of any Link shown here, synced or not: its name, place, rotation, life,
//   actor state, death word, ragdoll state, whether it is drawn, each animation slot's command and clock, and each
//   equipment slot's held thing and state.
// - Every frame, a line "f": place, rotation and the first animation slot's command and clock of those within 40 m,
//   for lag, stutter and the animation's phase.
// An actor is named by its placement hash ("p..."), else by the key the spawn sync gave one made at run time ("r...",
// the one thing taken from the sync: two games' run-time actors have nothing else in common), else by its address
// ("a...", this game's alone: an actor neither placed nor synced, which check.py lists as shown in one game only).
// "r" says what the enemy sync does with it here (o runs it for all, c a copy of another game's, l its own and not
// synced, h held, n new), so check.py knows which game is the owner; it judges nothing by it.
// On with TOTK_COOP_DEBUG=1 (the pair scripts set it); TOTK_COOP_CHECK=0 turns it off, =1 on alone.
//
// TOTK_COOP_TEST_NUDGE=<t>,<s>,<m> (a test of the checker, PLAN.md step 1): from t world seconds, the first enemy copy
// this game shows is put m metres east (+x) of where its owner's state places it, for s seconds, through the copy's
// own shown place (coop_enemy_copy.c, en_copy_phase0), so the game draws it there. check.py must list that enemy and
// that time and no other.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_sess.h"
#include "coop_world_int.h"
#include "host.h"
#include "platform/platform.h"

#define CHECK_FAR 120.f    // the once-a-second line: actors within this of a Link (horizontal)
#define CHECK_NEAR 40.f    // the per-frame line

static void put_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fputc('\\', f), fputc(ch, f);
        else if (ch >= 0x20 && ch < 0x7f) fputc(ch, f);
    }
    fputc('"', f);
}

static int ident(const EnCell *m, uint64_t a, char *out, size_t n) {
    uint64_t h = en_hash_of(a);
    if (h) return snprintf(out, n, "p%016llx", (unsigned long long)h);
    if (m->rt && m->hash) return snprintf(out, n, "r%016llx", (unsigned long long)m->hash);
    return snprintf(out, n, "a%llx", (unsigned long long)a);
}
static char role(int st) {
    switch (st) {
    case EM_OWNED: return 'o';
    case EM_COPY: return 'c';
    case EM_OWN: return 'l';
    case EM_HELD: return 'h';
    default: return 'n';
    }
}

// The Links shown here: this game's and the stand-ins of the others'.
static int links_of(uint64_t me, float lp[][3]) {
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = 0;
    if (coop_rd(me + COOP_ACTOR_POS, lp[n], 12)) n++;
    int k = coop_player_actors(acts, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < k; i++)
        if (coop_rd(acts[i] + COOP_ACTOR_POS, lp[n], 12)) n++;
    return n;
}
static float near_link(const float p[3], float lp[][3], int nl) {
    float best = 1e30f;
    for (int i = 0; i < nl; i++) {
        float dx = p[0] - lp[i][0], dz = p[2] - lp[i][2], d = sqrtf(dx * dx + dz * dz);
        if (d < best) best = d;
    }
    return best;
}

static void snap_actor(FILE *f, int first, const EnCell *m, uint64_t a, const char *id, const float pose[7]) {
    char nm[48] = "";
    coop_actor_name(a, nm, sizeof nm);
    int32_t st = 0, rag = 0;
    uint32_t dw = 0, fl = 0;
    uint8_t hide = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    uint64_t comps = en_comps(a), dc = comps ? coop_rd_ptr(comps + 0x1c8) : 0, rg = comps ? coop_rd_ptr(comps + 0x3e8) : 0,
             gmc = comps ? coop_rd_ptr(comps + 0x280) : 0;
    if (dc) coop_rd(dc + 0x20, &dw, 4);          // the death word (what the sync states as dstate)
    if (rg) coop_rd(rg + 0x20, &rag, 4);         // the ragdoll state
    if (gmc) coop_rd(gmc + 0x9d, &hide, 1);      // the model controller's "not drawn" mask
    coop_rd(a + 0x444, &fl, 4);                  // bit 23: the actor's own drawn bit
    fprintf(f, "%s{\"id\":\"%s\",\"n\":", first ? "" : ",", id);
    put_str(f, nm);
    fprintf(f, ",\"r\":\"%c\",\"p\":[%.3f,%.3f,%.3f],\"q\":[%.4f,%.4f,%.4f,%.4f],\"hp\":%d,\"st\":%d,\"ds\":%u,\"rag\":%d,\"hide\":%u,\"vis\":%u",
            role(atomic_load_explicit(&m->state, memory_order_relaxed)), pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6],
            en_hp_of(a), st, dw, rag, hide, (fl >> 23) & 1);
    char cmd[EN_SLOT_N][40];
    float clk[EN_SLOT_N];
    int ns = en_read_anim(a, cmd, clk);
    for (int i = 0; i < ns; i++) if (!isfinite(clk[i])) clk[i] = -1;
    fputs(",\"anim\":[", f);
    for (int i = 0; i < ns; i++) {
        fprintf(f, "%s[", i ? "," : "");
        put_str(f, cmd[i]);
        fprintf(f, ",%.3f]", clk[i]);
    }
    char held[EN_EQ_N][40];
    uint8_t eqs[EN_EQ_N];
    en_read_equip(a, held, eqs);
    fputs("],\"eq\":[", f);
    for (int i = 0; i < EN_EQ_N; i++) {
        fprintf(f, "%s[", i ? "," : "");
        put_str(f, held[i]);
        fprintf(f, ",%u]", eqs[i]);
    }
    fputs("]}", f);
}

void coop_check_tick(Cpu *c, uint64_t me) {
    (void)c;
    static int on = -1;
    static FILE *f;
    static uint64_t last_s;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_CHECK");
        on = e ? strcmp(e, "0") != 0 : coop_debug();
        if (on && !(f = fopen("logs/check.jsonl", "w"))) on = 0;
        if (on) rt_log("coop: check: recording to logs/check.jsonl");
    }
    if (!on || !me) return;
    float lp[COOP_MAX_PLAYERS + 1][3];
    int nl = links_of(me, lp);
    uint64_t now = plat_now_ns(), sec = now / 1000000000ull;
    int snap = sec != last_s;
    if (snap) {
        last_s = sec;
        fprintf(f, "{\"k\":\"s\",\"t\":%.6f,\"me\":%d,\"world\":%.2f,\"links\":[", now / 1e9, coop_net_id(), frame_step_world_us() / 1e6);
        for (int i = 0; i < nl; i++) fprintf(f, "%s[%.3f,%.3f,%.3f]", i ? "," : "", lp[i][0], lp[i][1], lp[i][2]);
        fputs("],\"a\":[", f);
    }
    // the per-frame line is built beside the second's, then written after it
    static char fl[64 * 1024];
    int fo = snprintf(fl, sizeof fl, "{\"k\":\"f\",\"t\":%.6f,\"a\":[", now / 1e9), fn = 0, sn = 0;
    for (int i = 0; i < EN_MAP_N; i++) {
        EnCell *m = &en_map[i];
        uint64_t a = atomic_load_explicit(&m->actor, memory_order_relaxed);
        uint32_t gen = atomic_load_explicit(&m->gen, memory_order_relaxed), g = 0;
        if (a <= 1 || !coop_rd(a + 0x10, &g, 4) || g != gen) continue;   // (the cell's actor is gone or another now)
        float pose[7];
        en_pose_of(a, pose);
        for (int k = 0; k < 7; k++) if (!isfinite(pose[k])) pose[k] = 0;   // (nan or inf is no JSON: the line would be lost)
        float d = near_link(pose, lp, nl);
        if (d > CHECK_FAR) continue;
        char id[24];
        ident(m, a, id, sizeof id);
        if (snap) snap_actor(f, !sn++, m, a, id, pose);
        if (d <= CHECK_NEAR && fo < (int)sizeof fl - 256) {
            char cmd[EN_SLOT_N][40];
            float clk[EN_SLOT_N];
            int ns = en_read_anim(a, cmd, clk);
            if (ns && !isfinite(clk[0])) clk[0] = -1;
            uint32_t h = 2166136261u;   // the first slot's command, as its FNV-1a hash (0: none)
            for (const char *s = cmd[0]; ns && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
            fo += snprintf(fl + fo, sizeof fl - fo, "%s[\"%s\",%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,\"%08x\",%.3f]", fn++ ? "," : "", id,
                           pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], ns && cmd[0][0] ? h : 0u, ns ? clk[0] : 0.f);
        }
    }
    if (snap) fputs("]}\n", f);
    if (fn && fo < (int)sizeof fl - 4) fputs(fl, f), fputs("]}\n", f);
    if (snap) fflush(f);
}

// TOTK_COOP_TEST_NUDGE (see the top): called by en_copy_phase0 for each copy it places, with where its owner's state
// puts it; moves it while the test holds that copy.
void coop_check_nudge(uint64_t actor, uint64_t key, float pos[3]) {
    static int on = -1;
    static double t0, len, metres;
    static uint64_t who;
    static int state;   // 0 waiting, 1 holding, 2 done
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_TEST_NUDGE");
        on = e && sscanf(e, "%lf,%lf,%lf", &t0, &len, &metres) == 3;
    }
    if (!on || state == 2 || !actor) return;
    double now = frame_step_world_us() / 1e6;
    if (state == 0 && now >= t0) {
        who = key, state = 1;
        char nm[48] = "";
        coop_actor_name(actor, nm, sizeof nm);
        rt_log("coop: check: test: nudge: %s %016llx put %.1f m east from %.1f s for %.1f s", nm, (unsigned long long)key, metres, now, len);
    }
    if (state != 1 || key != who) return;
    if (now >= t0 + len) {
        state = 2;
        rt_log("coop: check: test: nudge: %016llx let go at %.1f s", (unsigned long long)key, now);
        return;
    }
    pos[0] += (float)metres;
}
