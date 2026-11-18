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
// synced while this game runs it, r its own and not synced while nothing runs it here, h held, n new), so check.py
// knows which game is the owner; it judges nothing by it. An enemy that neither game runs (parked by the game's battle
// LOD manager, or both Links far) rests as "r" in both, and is no owner finding (research R30 2). The second's line
// carries "v":2 since that role came; check.py reads an older record's "l" as either.
// The per-frame line also carries each stand-in of another player's Link ("sl": its id, its weapon's attack byte,
// EquipmentUser +0x660, which coop_players.c sets from its owner's state, and its weapon's place), so a blow of that
// player missing in this game can be told apart: no attack on, or the weapon not where the enemy was (research R31 1.4).
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
static char role(EnCell *m) {
    int coop_enemy_cell_runs(EnCell *m);   // coop_enemy.c
    switch (atomic_load_explicit(&m->state, memory_order_relaxed)) {
    case EM_OWNED: return 'o';
    case EM_COPY: return 'c';
    case EM_OWN: return coop_enemy_cell_runs(m) ? 'l' : 'r';
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

static void snap_actor(FILE *f, int first, EnCell *m, uint64_t a, const char *id, const float pose[7]) {
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
            role(m), pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6],
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

// ---- What the enemies do, seen where the game does it (any thread): attacks armed, effects and sounds by key, blows
// between an enemy and a Link. The hooks look an actor up in a table the tick makes of the listed actors near the
// Links (its SensorHit component [comps+0x428], its effect and sound users [[comps+0x68]+0x70] and +0x78), so they
// cost a short scan, and nothing at all before the record is on. The table holds the first USERS_N of those within
// 120 m of a Link, in the map's order; one left out is counted ("over" in the second's line) and fails the check.
static _Atomic int ev_on;
#define USERS_N 256
static struct { _Atomic uint64_t actor, eu, su; } users[2][USERS_N];
static _Atomic int users_cur, users_n[2];
static uint64_t user_actor(uint64_t p, int what) {   // what: 1 the effect user, 2 the sound user
    int b = atomic_load_explicit(&users_cur, memory_order_acquire), n = atomic_load_explicit(&users_n[b], memory_order_relaxed);
    for (int i = 0; p && i < n; i++) {
        uint64_t v = atomic_load_explicit(what == 1 ? &users[b][i].eu : &users[b][i].su, memory_order_relaxed);
        if (v == p) return atomic_load_explicit(&users[b][i].actor, memory_order_relaxed);
    }
    return 0;
}
// Attacks armed (main+0x1bf20cc, made again every frame while an attack is on, by the clip's event or an AI node, or
// by the enemy sync's replay on a copy): the attack types armed, per actor, with the frame.
// The actor is the component's +0x18 (as coop_enemy_arm.c en_arm_hook reads it). Per actor (open addressing, 8 probes):
// the frame and the mask in one word, so a call in a new frame starts the mask afresh without losing another thread's
// bit of that frame.
#define ARMS_N 1024
static struct { _Atomic uint64_t actor, fm; } arms[ARMS_N];   // fm: frame << 32 | mask
static _Atomic uint32_t arms_full;
void coop_check_arm(Cpu *c) {
    if (!atomic_load_explicit(&ev_on, memory_order_relaxed)) return;
    uint64_t a = c->x[0] > 0x10000 ? coop_rd_ptr(c->x[0] + 0x18) : 0;
    uint32_t type = 0;
    if (a <= 0x10000 || !en_cell_of(a) || !coop_rd(c->x[1] + 4, &type, 4)) return;
    uint32_t fr = coop_sess_frame_no(), h = (uint32_t)((a >> 4) * 2654435761u);
    for (int p = 0; p < 8; p++) {
        uint32_t k = (h + (uint32_t)p) % ARMS_N;
        uint64_t was = atomic_load_explicit(&arms[k].actor, memory_order_relaxed);
        if (was != a) {
            // (a slot whose actor has not armed for a second is free again)
            uint64_t fm = atomic_load_explicit(&arms[k].fm, memory_order_relaxed);
            if (was && fr - (uint32_t)(fm >> 32) < 60) continue;
            if (!atomic_compare_exchange_strong(&arms[k].actor, &was, a)) continue;
        }
        uint64_t old = atomic_load_explicit(&arms[k].fm, memory_order_relaxed), nv;
        do {
            uint32_t m = (uint32_t)(old >> 32) == fr ? (uint32_t)old : 0;
            nv = (uint64_t)fr << 32 | m | 1u << (type & 31);
        } while (!atomic_compare_exchange_weak(&arms[k].fm, &old, nv));
        return;
    }
    atomic_fetch_add(&arms_full, 1);
}
static uint32_t arm_mask(uint64_t a) {   // armed in this frame or the one before
    uint32_t h = (uint32_t)((a >> 4) * 2654435761u), fr = coop_sess_frame_no();
    for (int p = 0; p < 8; p++) {
        uint32_t k = (h + (uint32_t)p) % ARMS_N;
        if (atomic_load_explicit(&arms[k].actor, memory_order_relaxed) != a) continue;
        uint64_t fm = atomic_load_explicit(&arms[k].fm, memory_order_relaxed);
        return fr - (uint32_t)(fm >> 32) <= 1 ? (uint32_t)fm : 0;
    }
    return 0;
}
// Events (effects, sounds, blows): a ring the hooks fill and the tick writes out as "e" lines.
#define EV_N 1024
typedef struct { _Atomic int ready; char kind; uint8_t cls; uint32_t aid; uint64_t ns; char a[24], b[24], key[40]; } Ev;
static Ev evs[EV_N];
static _Atomic uint32_t ev_w, ev_lost;
static Ev *ev_take(void) {
    Ev *e = &evs[atomic_fetch_add(&ev_w, 1) % EV_N];
    if (atomic_load_explicit(&e->ready, memory_order_acquire)) { atomic_fetch_add(&ev_lost, 1); return NULL; }
    e->ns = plat_now_ns();
    return e;
}
static int link_ident(uint64_t a, char *out, size_t n) {   // "L<player id>": this game's Link or another's stand-in
    extern uint64_t coop_world_me;
    if (!a) return 0;
    if (a == coop_world_me) return snprintf(out, n, "L%d", coop_net_id());
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], k = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < k; i++) if (co[i] == a) return snprintf(out, n, "L%d", ids[i]);
    return 0;
}
static int any_ident(uint64_t a, char *out, size_t n) {   // a Link, or an actor of the listed kinds
    if (link_ident(a, out, n)) return 1;
    EnCell *m = a ? en_cell_of(a) : NULL;
    if (!m) return 0;
    ident(m, a, out, n);
    return 1;
}
// An effect (main+0xa98cf8) or a sound (main+0xa98d00) started by key on a user (x0), the key's string at x1: by an
// AI node, the enemy sync's replay of one, or anything else that emits by key.
void coop_check_emit(Cpu *c, int sound) {
    if (!atomic_load_explicit(&ev_on, memory_order_relaxed)) return;
    uint64_t a = user_actor(c->x[0], sound ? 2 : 1);
    char id[24];
    if (!a || !any_ident(a, id, sizeof id)) return;
    Ev *e = ev_take();
    if (!e) return;
    e->kind = sound ? 's' : 'x', e->cls = 0, e->aid = 0, e->b[0] = 0;
    memcpy(e->a, id, sizeof id);
    memset(e->key, 0, sizeof e->key);
    if (c->x[1]) coop_rd(c->x[1], e->key, sizeof e->key - 1);
    for (char *q = e->key; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
    atomic_store_explicit(&e->ready, 1, memory_order_release);
}
// A blow between an enemy and a Link, from the contacts a body's hit update (main+0x300a30) is about to build hits from,
// before coop_contact.c takes any out: in the game that runs the enemy its blow lands on the other player's stand-in,
// in that player's game on their Link; both are kept. The entry's class byte +0x0b (0 hit, 1 guarded, 3 guarded at the
// right moment, 4 avoided) and its attack id +0x38.
void coop_check_hits(uint64_t dc) {
    if (!atomic_load_explicit(&ev_on, memory_order_relaxed)) return;
    uint64_t v = coop_rd_ptr(dc + 0x198), o = coop_rd_ptr(dc + 0x1910), l = o ? coop_rd_ptr(o + 0x50) : 0, base;
    int32_t cnt = 0, cap = 0;
    if (!l || !coop_rd(l + 0x18, &cnt, 4) || cnt <= 0 || !coop_rd(l + 8, &cap, 4) || cap <= 0) return;
    if (cnt > cap) cnt = cap;
    if (cnt > 64 || !(base = coop_rd_ptr(l + 0x10))) return;
    char vid[24], bid[24];
    int vlink = link_ident(v, vid, sizeof vid);
    if (!vlink && !any_ident(v, vid, sizeof vid)) return;
    for (int k = 0; k < cnt; k++) {
        uint64_t en = base + (uint64_t)k * 0x1c0, r2 = coop_rd_ptr(en + 0x80), by = r2 ? coop_rd_ptr(r2 + 0x40) : 0;
        int blink = link_ident(by, bid, sizeof bid);
        if (!blink && !any_ident(by, bid, sizeof bid)) continue;
        if (vlink == blink) continue;   // (a Link and a Link, or an enemy and an enemy: not this check's)
        Ev *e = ev_take();
        if (!e) return;
        e->kind = 'h', e->key[0] = 0, e->cls = 0, e->aid = 0;
        memcpy(e->a, vid, sizeof vid), memcpy(e->b, bid, sizeof bid);
        coop_rd(en + 0x0b, &e->cls, 1), coop_rd(en + 0x38, &e->aid, 4);
        atomic_store_explicit(&e->ready, 1, memory_order_release);
    }
}
static void ev_flush(FILE *f) {
    static uint32_t r;
    uint32_t w = atomic_load(&ev_w);
    if (w - r > EV_N) r = w - EV_N;
    for (; r != w; r++) {
        Ev *e = &evs[r % EV_N];
        if (!atomic_load_explicit(&e->ready, memory_order_acquire)) {
            if (w - r < EV_N / 2) break;   // (still being filled: next tick)
            continue;                      // (a hole: its event was lost, ev_lost counts it; not waited on for good)
        }
        fprintf(f, "{\"k\":\"e\",\"t\":%.6f,\"w\":\"%c\",\"a\":\"%s\"", e->ns / 1e9, e->kind, e->a);
        if (e->kind == 'h') fprintf(f, ",\"by\":\"%s\",\"cls\":%u,\"aid\":%u}\n", e->b, e->cls, e->aid);
        else fputs(",\"key\":", f), put_str(f, e->key), fputs("}\n", f);
        atomic_store_explicit(&e->ready, 0, memory_order_release);
    }
    static uint32_t lost_said;
    uint32_t lost = atomic_load(&ev_lost);
    if (lost != lost_said) fprintf(f, "{\"k\":\"lost\",\"n\":%u}\n", lost), lost_said = lost;
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
        atomic_store(&ev_on, on);
    }
    if (!on) return;
    ev_flush(f);
    if (!me) {   // (out of the world: no table, so no stale address names an event)
        atomic_store(&users_n[0], 0), atomic_store(&users_n[1], 0);
        return;
    }
    int ub = 1 - atomic_load(&users_cur), un = 0, over = 0;   // the users' table made again in the half the hooks do not read
    float lp[COOP_MAX_PLAYERS + 1][3];
    int nl = links_of(me, lp);
    uint64_t now = plat_now_ns(), sec = now / 1000000000ull;
    int snap = sec != last_s;
    if (snap) {
        last_s = sec;
        fprintf(f, "{\"k\":\"s\",\"v\":2,\"t\":%.6f,\"me\":%d,\"world\":%.2f,\"links\":[", now / 1e9, coop_net_id(), frame_step_world_us() / 1e6);
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
        if (un < USERS_N) {
            uint64_t comps = en_comps(a), xl = comps ? coop_rd_ptr(comps + 0x68) : 0;
            atomic_store_explicit(&users[ub][un].actor, a, memory_order_relaxed);
            atomic_store_explicit(&users[ub][un].eu, xl ? coop_rd_ptr(xl + 0x70) : 0, memory_order_relaxed);
            atomic_store_explicit(&users[ub][un].su, xl ? coop_rd_ptr(xl + 0x78) : 0, memory_order_relaxed);
            un++;
        } else over++;
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
            fo += snprintf(fl + fo, sizeof fl - fo, "%s[\"%s\",%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,\"%08x\",%.3f,%u]", fn++ ? "," : "", id,
                           pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], ns && cmd[0][0] ? h : 0u, ns ? clk[0] : 0.f, arm_mask(a));
        }
    }
    atomic_store_explicit(&users_n[ub], un, memory_order_relaxed);
    atomic_store_explicit(&users_cur, ub, memory_order_release);
    if (snap) fprintf(f, "],\"over\":%d,\"arms_full\":%u}\n", over, atomic_load(&arms_full));   // (actors the hooks' tables left out)
    // the stand-ins, on the frames that have an enemy within 40 m: id, weapon attack byte, the weapon's place (none: no weapon)
    char sb[COOP_MAX_PLAYERS * 96];
    int so = 0, nsl = 0;
    {
        uint64_t coop_eq_row_ref(uint64_t actor, int row);   // coop_players.c
        uint64_t acts[COOP_MAX_PLAYERS];
        int ids[COOP_MAX_PLAYERS], k = coop_player_actors(acts, ids, COOP_MAX_PLAYERS);
        for (int i = 0; fn && i < k && so < (int)sizeof sb - 96; i++) {
            uint64_t eq = coop_rd_ptr(en_comps(acts[i]) + 0x230), ref = coop_eq_row_ref(acts[i], 0), w = ref ? coop_rd_ptr(ref + 0x40) : 0;
            uint8_t att = 0;
            float wp[3];
            if (eq) coop_rd(eq + 0x660, &att, 1);
            so += snprintf(sb + so, sizeof sb - so, "%s[\"L%d\",%u", nsl++ ? "," : "", ids[i], att);
            if (w && coop_rd(w + COOP_ACTOR_POS, wp, 12) && isfinite(wp[0]) && isfinite(wp[1]) && isfinite(wp[2]))
                so += snprintf(sb + so, sizeof sb - so, ",%.3f,%.3f,%.3f", wp[0], wp[1], wp[2]);
            so += snprintf(sb + so, sizeof sb - so, "]");
        }
    }
    if (fn && fo < (int)sizeof fl - 4) {
        fputs(fl, f), fputs("]", f);
        if (nsl) fprintf(f, ",\"sl\":[%s]", sb);
        fputs("}\n", f);
    }
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
