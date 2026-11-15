// Co-op: the session record of the entity sync. What a play session needs written down so that a fault a player saw
// can be found afterwards without running it again. On in every session of the entity sync; TOTK_COOP_RECORD=0: off.
//
// In the log (logs/totk.log):
//   coop: clock: <hh:mm:ss> t <s> world <s> ms <ms>     once a second: the PC's time of day, the clock the audit and
//       the motion file carry ("t", one for every game on this PC), the world clock and the entity sync's clock (the
//       number its own lines start with). The lines between two of these happened in that second.
//   coop: perf: ...                                     every 10 s the frames of that time (mean, longest, how many over
//       33 ms); at once a frame of over 100 ms.
//   coop: ent: jump: ...                                an entity's main body moved further in one frame than its
//       velocity explains (over 0.5 m more): a teleport, a placement, a throw by a contact.
//   coop: ent: change: <eid> <name>: ...                any change of what this game holds of an entity: its owner, its
//       life, its flags, the state of a copy, its owner's class (idle, held, ridden), its device on or off, its actor
//       here or gone, the drive's state for it (moving, at rest, held by the drive, put in place n times).
//   coop: ent: hand: ... / coop: ent: ride: ...         what this Link's Ultrahand holds, what this Link rides.
//   coop: ent: loose: ...                               the Zonai devices here that are no entity, when that set changes.
//   coop: MARK <n> at <hh:mm:ss> ...                    the tester pressed Home (or numpad 0): written by every game on
//       this PC at that moment, with a picture of each (logs/mark-<n>.png) and an audit line.
// In logs/motion.jsonl: ten times a second, while anything moves, where every moving entity is here (place, rotation,
// velocity, the drive's state), this Link and the stand-ins. tools/coop/ent_marks.py reads all of it around a mark.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_ent.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"

void coop_lock_wait(PlatMutex *m);
int coop_runs_for_ms(void);                           // coop_players.c: -1 while this game sits in a menu or a load
int coop_players_all(uint64_t *out, int max);         // every live stand-in actor
int input_mark_key(void);                             // input_win.c: Home or numpad 0 is down (in any window)
void rt_mark_shot(const char *path);                  // boot.c: the next picture presented is saved there
void ctn_follow_peek(const struct FollowState *fs, int *rest, int *no_rest, int *placed);   // coop_ctn.c

static int rec_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_RECORD"); on = !(e && *e == '0'); }
    return on && ent_on();
}

// An actor as the log names it: an entity by its Eid, its name and whose it is; a stand-in; else the actor's name.
const char *coop_ent_label(uint64_t actor, char *buf, int n) {
    char b[24], nm[48] = "";
    Ent *e = ent_on() && actor ? ent_by_actor(actor) : NULL;
    if (e) { snprintf(buf, (size_t)n, "%s %s (%s)", ent_eid_str(e->id, b), e->name, e->owner == coop_ctr_me() ? "this game's" : "a copy"); return buf; }
    uint64_t st[8];
    int ns = coop_players_all(st, 8);
    for (int i = 0; i < ns; i++) if (st[i] == actor) { snprintf(buf, (size_t)n, "a stand-in"); return buf; }
    coop_actor_name(actor, nm, sizeof nm);
    snprintf(buf, (size_t)n, "%s (no entity)", nm[0] ? nm : "?");
    return buf;
}

static int mark_n, mark_audit;
int coop_ent_log_mark(void) { return mark_n; }
int coop_ent_log_take_mark(void) { int m = mark_audit; mark_audit = 0; return m; }   // the audit writes a line at once for a mark

static void wall_str(char *out, size_t n) {
    struct tm tm;
    if (plat_localtime((int64_t)time(NULL), &tm)) snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else snprintf(out, n, "??:??:??");
}

typedef struct {
    uint8_t used, life, flags, state, cls, sw, on, drive;   // drive: 0 none, 1 moving, 2 at rest, 3 held by the drive
    uint8_t placed;
    uint8_t have, had;        // its main body was read this frame; it had an actor at the last look for changes
    Eid id;
    uint64_t owner;
    float p[3], r[9], v[3];   // as read this frame
    float lp[3], lr[9];       // as last written to the motion file
    uint8_t moved;            // since the last motion line
    int64_t jump_said;
} Was;
static Was was[ENT_N];
static const char *cls_name(int c) { return c == CLS_HOLD ? "held" : c == CLS_RIDE ? "ridden" : "idle"; }
static const char *state_name(int s) {
    static const char *n[] = {"agree", "far", "waits for the maker", "being made", "differs", "not shown"};
    return s >= 0 && s < 6 ? n[s] : "?";
}
static const char *life_name(int l) { return l == LIFE_LIVE ? "live" : l == LIFE_DEAD ? "dead" : l == LIFE_GONE ? "gone" : "free"; }
static const char *drive_name(int d) { return d == 1 ? "moving" : d == 2 ? "at rest" : d == 3 ? "held by the drive" : "none"; }

typedef struct { int n; uint64_t x; char names[160]; int len; } LooseSet;
static void loose_fn(uint64_t a, void *arg) {
    LooseSet *s = arg;
    char nm[48];
    float p[3];
    coop_actor_name(a, nm, sizeof nm);
    if (strncmp(nm, "SpObj_", 6) || strstr(nm, "Capsule") || coop_ent_is(a) || !ctn_alive(a) || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    s->n++, s->x ^= a * 0x9e3779b97f4a7c15ull;
    if (s->len < 110) s->len += snprintf(s->names + s->len, sizeof s->names - (size_t)s->len, " %s@%.0f,%.0f,%.0f", nm + 6, p[0], p[1], p[2]);
}

void coop_ent_log_tick(Cpu *c, uint64_t me) {
    (void)c;
    if (!rec_on() || !me) return;
    uint64_t ns = plat_now_ns();
    int64_t us = frame_step_world_us();
    double t = ns / 1e9, world = us / 1e6;
    // ---- the tester's mark (before the world-clock gate: it is taken in a menu too)
    {
        static int down_was;
        int down = input_mark_key();
        if (down && !down_was) {
            char w[16], path[64];
            wall_str(w, sizeof w);
            mark_n++, mark_audit = 1;
            snprintf(path, sizeof path, "logs/mark-%03d.png", mark_n);
            rt_mark_shot(path);
            rt_log("coop: MARK %d at %s (t %.3f, world %.2f s, ms %lld): picture %s", mark_n, w, t, world, (long long)ent_ms(), path);
        }
        down_was = down;
    }
    static int64_t last_us;
    static uint64_t last_ns, clock_ns, perf_ns, scan_ns, loose_ns, line_ns;
    if (us == last_us) return;   // (the co-op tick comes twice a frame; a paused game's clock stands)
    double dt_world = last_us && us > last_us ? (us - last_us) / 1e6 : 1 / 30.0;
    if (dt_world > 0.2) dt_world = 0.2;
    last_us = us;
    // ---- the clock line and the frame times
    {
        static int frames, over33;
        static double sum_ms, max_ms;
        static uint64_t hitch_ns;
        double ms = last_ns ? (ns - last_ns) / 1e6 : 0;
        if (last_ns && coop_runs_for_ms() > 1000) {
            frames++, sum_ms += ms, over33 += ms > 33.4;
            if (ms > max_ms) max_ms = ms;
            if (ms > 100 && ns - hitch_ns > 1000000000ull) hitch_ns = ns, rt_log("coop: perf: a frame of %.0f ms at world %.2f s", ms, world);
        }
        if (ns - clock_ns >= 1000000000ull) {
            char w[16];
            wall_str(w, sizeof w);
            clock_ns = ns;
            rt_log("coop: clock: %s t %.3f world %.2f ms %lld", w, t, world, (long long)ent_ms());
        }
        if (ns - perf_ns >= 10000000000ull) {
            if (frames) rt_log("coop: perf: %d frames in 10 s: mean %.1f ms, longest %.0f ms, %d over 33 ms", frames, sum_ms / frames, max_ms, over33);
            perf_ns = ns, frames = over33 = 0, sum_ms = max_ms = 0;
        }
    }
    last_ns = ns;
    if (ent_loading()) return;
    // ---- every frame: where each entity's main body is, and a jump
    uint64_t self = coop_ctr_me();
    int any_moved = 0;
    for (int i = 0; i < ENT_N; i++) {
        Ent e = ents[i];   // (a copy: phase 0 owns the table)
        Was *w = &was[i];
        float m[12], v[3] = {0};
        int have = e.used && e.actor && ctn_alive_as(e.actor, e.gen) && e.main_body && coop_rd(e.main_body + 0x98, m, sizeof m);
        if (!have) { w->have = 0; continue; }
        coop_rd(e.main_body + 0xf8, v, sizeof v);
        float p[3] = {m[3], m[7], m[11]};
        if (w->have && w->used && eid_eq(e.id, w->id)) {
            float d = sqrtf((p[0] - w->p[0]) * (p[0] - w->p[0]) + (p[1] - w->p[1]) * (p[1] - w->p[1]) + (p[2] - w->p[2]) * (p[2] - w->p[2]));
            float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), sp0 = sqrtf(w->v[0] * w->v[0] + w->v[1] * w->v[1] + w->v[2] * w->v[2]);
            if (d > 0.5f + 3 * (sp > sp0 ? sp : sp0) * (float)dt_world && us - w->jump_said > 500000) {
                char b[24];
                w->jump_said = us;
                rt_log("coop: ent: jump: %s %s (%s, class %s) moved %.2f m in one frame of %.0f ms: %.2f %.2f %.2f -> %.2f %.2f %.2f, velocity %.1f m/s before, %.1f after",
                       ent_eid_str(e.id, b), e.name, e.owner == self ? "this game's" : "a copy", cls_name(w->cls), d, dt_world * 1000, w->p[0], w->p[1], w->p[2],
                       p[0], p[1], p[2], sp0, sp);
            }
            float dl = fabsf(p[0] - w->lp[0]) + fabsf(p[1] - w->lp[1]) + fabsf(p[2] - w->lp[2]);
            float dr = fabsf(m[0] - w->lr[0]) + fabsf(m[5] - w->lr[4]) + fabsf(m[10] - w->lr[8]) + fabsf(m[1] - w->lr[1]) + fabsf(m[6] - w->lr[5]);
            if (dl > 0.004f || dr > 0.004f) w->moved = 1;
        } else w->moved = 1;
        any_moved |= w->moved;
        memcpy(w->p, p, sizeof p), memcpy(w->v, v, sizeof v);
        w->r[0] = m[0], w->r[1] = m[1], w->r[2] = m[2], w->r[3] = m[4], w->r[4] = m[5], w->r[5] = m[6], w->r[6] = m[8], w->r[7] = m[9], w->r[8] = m[10];
        w->have = 1, w->id = e.id;
    }
    // ---- ten times a second: what changed of each entity, this Link's hand and ride, the motion file
    if (ns - scan_ns < 100000000ull) return;
    scan_ns = ns;
    static uint8_t g_sw[ENT_N], g_on[ENT_N], g_drive[ENT_N], g_placed[ENT_N];
    memset(g_sw, 0, sizeof g_sw), memset(g_on, 0, sizeof g_on), memset(g_drive, 0, sizeof g_drive), memset(g_placed, 0, sizeof g_placed);
    static int16_t live[ENT_N];
    int nlive = 0;
    for (int i = 0; i < ENT_N; i++) if (ents[i].used) live[nlive++] = (int16_t)i;
    coop_lock_wait(&grp_lock);
    for (int g = 0; g < GRP_N; g++) {
        if (grps[g].used != 1) continue;
        int rest = 0, no_rest = 0, placed = 0;
        if (!grps[g].mine) ctn_follow_peek(grps[g].fs, &rest, &no_rest, &placed);
        for (int r = 0; r < grps[g].n && r < ENT_MEMBERS; r++)
            for (int k = 0; k < nlive; k++) {   // (by a scan: the table's hash is phase 0's)
                int i = live[k];
                if (!eid_eq(ents[i].id, grps[g].id[r])) continue;
                g_sw[i] = grps[g].sw >> r & 1, g_on[i] = grps[g].on >> r & 1;
                if (!grps[g].mine && !grps[g].asked) g_drive[i] = no_rest ? 3 : rest ? 2 : 1, g_placed[i] = (uint8_t)(placed > 255 ? 255 : placed);
                break;
            }
    }
    plat_mutex_unlock(&grp_lock);
    uint64_t held = ent_held_actor();
    Eid ride = {0, 0};
    for (int i = 0; i < ENT_N; i++) {
        Ent e = ents[i];
        Was *w = &was[i];
        char b[24], line[400];
        int n = 0;
        if (!e.used) {
            if (w->used) rt_log("coop: ent: change: %s: its row is freed", ent_eid_str(w->id, b)), memset(w, 0, sizeof *w);
            continue;
        }
        int cls = e.owner == self ? (e.isl >= 0 && e.isl < ISL_N ? isls[e.isl].cls : 0) : e.st >= 0 && e.st < STATED_N ? stated[e.st].cls : e.cls;
        int state = e.owner == self ? 0 : e.st >= 0 && e.st < STATED_N ? stated[e.st].state : e.state;
        int have = e.actor && ctn_alive_as(e.actor, e.gen);
        if (eid_zero(ride) && e.life == LIFE_LIVE && ent_this_link_rides(&ents[i])) ride = e.id;
#define SAY(...) (n += n < (int)sizeof line - 1 ? snprintf(line + n, sizeof line - (size_t)n, __VA_ARGS__) : 0)
        if (!w->used || !eid_eq(w->id, e.id)) {
            SAY("new here: owner %04x (%s), %s, flags %d, %s", (unsigned)(e.owner & 0xffff), e.owner == self ? "this game" : "another", life_name(e.life), e.flags,
                have ? "with an actor" : "no actor");
            if (!eid_eq(w->id, e.id)) memset(w, 0, sizeof *w);   // (else the frame pass has its place already)
        } else {
            if (w->owner != e.owner) SAY("owner %04x -> %04x (%s); ", (unsigned)(w->owner & 0xffff), (unsigned)(e.owner & 0xffff), e.owner == self ? "this game" : "another");
            if (w->life != e.life) SAY("life %s -> %s; ", life_name(w->life), life_name(e.life));
            if (w->flags != e.flags) SAY("flags %d -> %d%s; ", w->flags, e.flags, (w->flags & ENT_PREVIEW) && !(e.flags & ENT_PREVIEW) ? " (no preview any more)" : "");
            if (w->state != state) SAY("copy state %s -> %s; ", state_name(w->state), state_name(state));
            if (w->cls != cls) SAY("class %s -> %s; ", cls_name(w->cls), cls_name(cls));
            if (w->sw != g_sw[i] || w->on != g_on[i]) SAY("device %s%s; ", g_sw[i] ? "" : "(no switch) ", g_on[i] ? "on" : "off");
            if (w->had != have && !have) SAY("its actor is gone here; ");
            if (w->had != have && have) SAY("it has an actor here now; ");
            if (w->drive != g_drive[i]) SAY("drive %s -> %s; ", drive_name(w->drive), drive_name(g_drive[i]));
            if (w->placed != g_placed[i] && g_placed[i]) SAY("put in place %d times in this rest; ", g_placed[i]);
        }
#undef SAY
        if (n) rt_log("coop: ent: change: %s %s: %s", ent_eid_str(e.id, b), e.name, line);
        w->used = 1, w->id = e.id, w->owner = e.owner, w->life = e.life, w->flags = e.flags, w->state = (uint8_t)state, w->cls = (uint8_t)cls;
        w->sw = g_sw[i], w->on = g_on[i], w->drive = g_drive[i], w->placed = g_placed[i], w->had = (uint8_t)have;
    }
    {
        static uint64_t held_was;
        static Eid ride_was;
        char b[24], lab[96];
        if (held != held_was) rt_log("coop: ent: hand: this Link's Ultrahand holds %s", held ? coop_ent_label(held, lab, sizeof lab) : "nothing"), held_was = held;
        if (!eid_eq(ride, ride_was)) {
            if (eid_zero(ride)) rt_log("coop: ent: ride: this Link rides nothing");
            else rt_log("coop: ent: ride: this Link rides %s", ent_eid_str(ride, b));
            ride_was = ride;
        }
    }
    if (ns - loose_ns >= 1000000000ull) {   // once a second: the devices here that are no entity
        static int n_was = -1;
        static uint64_t x_was;
        LooseSet s = {0};
        loose_ns = ns;
        coop_actors_live(loose_fn, &s);
        if (s.n != n_was || s.x != x_was) rt_log("coop: ent: loose: %d Zonai devices here are no entity:%s%s", s.n, s.n ? s.names : " none", s.len >= 110 ? " ..." : ""), n_was = s.n, x_was = s.x;
    }
    // ---- the motion file: the entities that moved since its last line; a line a second while one is held or ridden
    static FILE *f;
    static int failed;
    if (failed || (!any_moved && !((held || !eid_zero(ride)) && ns - line_ns >= 1000000000ull))) return;
    if (!f && !(f = fopen("logs/motion.jsonl", "w"))) { failed = 1; return; }
    line_ns = ns;
    char hb[24] = "", rb[24] = "";
    Ent *he = held ? ent_by_actor(held) : NULL;
    float lp[3] = {0};
    coop_rd(me + COOP_ACTOR_POS, lp, 12);
    fprintf(f, "{\"t\":%.3f,\"id\":%d,\"world\":%.2f,\"mark\":%d,\"me\":[%.3f,%.3f,%.3f],\"hand\":\"%s\",\"ride\":\"%s\",\"links\":[", t, coop_net_id(), world, mark_n,
            lp[0], lp[1], lp[2], he ? ent_eid_str(he->id, hb) : held ? "?" : "", eid_zero(ride) ? "" : ent_eid_str(ride, rb));
    uint64_t st[8];
    int nst = coop_players_all(st, 8);
    for (int i = 0, k = 0; i < nst; i++) {
        float q[3];
        if (coop_rd(st[i] + COOP_ACTOR_POS, q, 12)) fprintf(f, "%s[%.3f,%.3f,%.3f]", k++ ? "," : "", q[0], q[1], q[2]);
    }
    fputs("],\"e\":[", f);
    for (int i = 0, k = 0; i < ENT_N; i++) {
        Was *w = &was[i];
        char b[24];
        if (!w->used || !w->have || !w->moved) continue;
        fprintf(f, "%s{\"id\":\"%s\",\"o\":\"%04x\",\"m\":%d,\"c\":%d,\"d\":%d,\"p\":[%.3f,%.3f,%.3f],\"v\":[%.2f,%.2f,%.2f],\"r\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]}",
                k++ ? "," : "", ent_eid_str(w->id, b), (unsigned)(w->owner & 0xffff), w->owner == self, w->cls, w->drive, w->p[0], w->p[1], w->p[2], w->v[0], w->v[1], w->v[2],
                w->r[0], w->r[1], w->r[2], w->r[3], w->r[4], w->r[5], w->r[6], w->r[7], w->r[8]);
        memcpy(w->lp, w->p, sizeof w->lp), memcpy(w->lr, w->r, sizeof w->lr), w->moved = 0;
    }
    fputs("]}\n", f);
    fflush(f);
}
