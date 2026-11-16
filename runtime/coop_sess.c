// Co-op: session state the syncs share (docs/coop/enemy-sync-spec.md sections 1, 6.4 and 16.2).
//
// - The frame stamp: the performance counter read once a frame at phase 0 (GameDataMgr::calc, outside the parallel
//   actor update). Every state read and every capture of that frame carries it. One clock for every game on a PC,
//   never stopped by a menu and never shortened by a hitch, which the game's world clock is.
// - Paused and loading, for this game; running, paused, loading or gone, for a peer, from its heartbeat.
// - The hold of a claim: how long to wait for a rival's claim, from the transport's round trips.
// - The clock estimate per peer: how far that peer's frame stamps are from this game's, and the time T at which its
//   states are shown.
// TOTK_COOP_TEST_SESSCLOCK=1: the estimate is walked through a steady stream, a latency step up, a step down, a jump
// and a pause at the first phase 0, and the log says what it did ("coop: sess: clock test").
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctr.h"
#include "coop_game.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int coop_ctn_core(void);                                         // coop_ctn.c: the entity sync is on (it sets the heartbeat flags then)
int coop_ctr_rtt(int id, double *srtt, double *rttvar);          // coop_ctr.c

static _Atomic uint64_t frame_ns, phase0_at;
static _Atomic uint32_t frame_no;

// ---- The clock estimate (S 6.4) --------------------------------------------------------------------------------------
#define MS 1000000ll
typedef struct {
    int have;                 // an estimate exists
    int64_t est;              // this game's stamp - the peer's stamp, ns
    int64_t bucket[10];       // the smallest sample of each of the last ten seconds (INT64_MAX: none)
    uint64_t bucket_at;       // the second (of this game's stamp) bucket[0] is for
    int64_t dev[128];         // sample - estimate, the last ones
    uint64_t dev_at[128];
    uint32_t dev_w;
    int64_t delay;            // ns
    uint32_t teleports;       // times the estimate was set at once: every copy of that peer takes its next pose as one
} Clock;
static Clock clocks[COOP_MAX_PLAYERS + 1];

static void clock_clear(Clock *k) {
    uint32_t t = k->teleports;
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 10; i++) k->bucket[i] = INT64_MAX;
    k->teleports = t;
}
static void clock_sample(Clock *k, uint64_t now, int64_t sample) {
    uint64_t sec = now / 1000000000ull;
    if (!k->bucket_at) { for (int i = 0; i < 10; i++) k->bucket[i] = INT64_MAX; k->bucket_at = sec; }
    while (k->bucket_at < sec) {   // a new second: the buckets move on
        memmove(&k->bucket[1], &k->bucket[0], 9 * sizeof k->bucket[0]);
        k->bucket[0] = INT64_MAX, k->bucket_at++;
        if (sec - k->bucket_at > 10) k->bucket_at = sec;
    }
    if (sample < k->bucket[0]) k->bucket[0] = sample;
    if (!k->have) k->have = 1, k->est = sample, k->delay = 50 * MS, k->teleports++;
    k->dev[k->dev_w % 128] = sample - k->est, k->dev_at[k->dev_w % 128] = now, k->dev_w++;
}
// Once a frame: the estimate moves toward the smallest sample of the last 3 s by at most 2 ms, or is set at once when
// it has been more than 250 ms off for a second; the delay follows the 95th percentile of the last 2 s of deviations plus two state
// intervals, between 50 and 150 ms, by at most 1 ms. Returns T on the peer's clock, 0 with no estimate.
static int64_t clock_step(Clock *k, uint64_t now, uint64_t interval) {
    if (!k->have) return 0;
    // The target is the smallest sample of the last 3 s (a rise of the offset, as a hitch of this game's or a longer
    // route gives, is followed after that long; the delay covers up to 150 ms of it meanwhile). A rise or a fall of
    // over 250 ms that has lasted a whole second is taken at once.
    int64_t target = INT64_MAX, fast = k->bucket[1] != INT64_MAX ? (k->bucket[0] < k->bucket[1] ? k->bucket[0] : k->bucket[1]) : INT64_MAX;
    for (int i = 0; i < 3; i++) if (k->bucket[i] < target) target = k->bucket[i];
    if (fast != INT64_MAX && (fast - k->est > 250 * MS || fast - k->est < -250 * MS)) k->est = fast, k->teleports++;
    else if (target != INT64_MAX) {
        int64_t d = target - k->est;
        k->est += d > 2 * MS ? 2 * MS : d < -2 * MS ? -2 * MS : d;
    }
    int64_t v[128];
    int n = 0;
    for (uint32_t i = 0; i < 128 && i < k->dev_w; i++) {
        uint32_t at = (k->dev_w - 1 - i) % 128;
        if (now - k->dev_at[at] > 2000000000ull) break;
        v[n++] = k->dev[at];
    }
    int64_t want = k->delay;
    if (n >= 8) {
        for (int i = 1; i < n; i++) { int64_t x = v[i]; int j = i; while (j > 0 && v[j - 1] > x) v[j] = v[j - 1], j--; v[j] = x; }
        want = v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] + 2 * (int64_t)interval;
    }
    if (want < 50 * MS) want = 50 * MS;
    if (want > 150 * MS) want = 150 * MS;
    k->delay += want - k->delay > MS ? MS : want - k->delay < -MS ? -MS : want - k->delay;
    return (int64_t)now - k->est - k->delay;
}

static void clock_selftest(void) {
    Clock k;
    memset(&k, 0, sizeof k);
    clock_clear(&k);
    const uint64_t frame = 16666667ull;
    uint64_t now = 5000000000ull, peer0 = 900000000000ull;   // the peer's counter has another origin
    int64_t lat = 20 * MS, t_prev = 0;
    int back = 0, held = 0, frames = 0;
    uint32_t tele0;
    struct { const char *what; int frames; int64_t lat; int pause; } step[] = {
        { "steady at 20 ms", 600, 20 * MS, 0 }, { "latency up to 120 ms", 900, 120 * MS, 0 }, { "latency down to 20 ms", 900, 20 * MS, 0 },
        { "a jump of the offset by 400 ms", 300, 420 * MS, 0 }, { "this game paused 5 s, then cleared", 300, 20 * MS, 1 },
    };
    for (unsigned s = 0; s < sizeof step / sizeof *step; s++) {
        lat = step[s].lat, back = held = frames = 0, tele0 = k.teleports;
        if (step[s].pause) now += 5000000000ull, clock_clear(&k), t_prev = 0;
        int64_t newest = 0, worst_gap = 0;
        for (int f = 0; f < step[s].frames; f++, now += frame) {
            // a state stamped by the peer `lat` ago (with 0 to 4 ms of jitter) is taken now
            int64_t jit = (int64_t)((f * 7919u) % 5u) * MS;
            int64_t stamp = (int64_t)(peer0 + now) - lat - jit;
            clock_sample(&k, now, (int64_t)now - stamp);
            if (stamp > newest) newest = stamp;
            int64_t T = clock_step(&k, now, frame);
            if (t_prev && T < t_prev) back++;
            if (T > newest) held++;
            if (newest - T > worst_gap) worst_gap = newest - T;
            t_prev = T, frames++;
        }
        rt_log("coop: sess: clock test: %s: %d frames, T ran backwards %d times, was ahead of the newest state in %d frames, at most %.1f ms behind it; "
               "at the end the estimate is %.1f ms from the true offset + latency, the delay %.1f ms; set at once %u times",
               step[s].what, frames, back, held, (double)worst_gap / 1e6, (double)(k.est - (-(int64_t)peer0 + lat)) / 1e6, (double)k.delay / 1e6, k.teleports - tele0);
    }
}

// ---- This game ------------------------------------------------------------------------------------------------------
void coop_sess_phase0(void) {
    uint64_t now = plat_now_ns();
    atomic_store(&frame_ns, now), atomic_store(&phase0_at, now), atomic_fetch_add(&frame_no, 1);
    static int tested;
    if (!tested) { tested = 1; if (cfg_get("TOTK_COOP_TEST_SESSCLOCK")) clock_selftest(); }
}
uint64_t coop_sess_frame_ns(void) { return atomic_load(&frame_ns); }
uint32_t coop_sess_frame_no(void) { return atomic_load(&frame_no); }
int coop_sess_paused(void) { return plat_now_ns() - atomic_load(&phase0_at) > 300000000ull; }
int coop_sess_loading(void) {   // the game's teardown flag (as coop_ent.c ent_loading reads it), or no Link yet
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_ACTOR_MGR_SLOT));
    uint8_t b = 0;
    if (mgr && coop_rd(mgr + 0xe1, &b, 1) && (b & 2)) return 1;
    return !coop_player_actor();
}

// The net tick: with the entity sync off nothing else states this game's paused and loading flags.
void coop_sess_net(void) {
    if (coop_ctn_core() || !coop_ctr_me()) return;
    CoopCtrHb hb = {0};
    if (coop_sess_paused()) hb.flags |= COOP_CTR_HB_PAUSED;
    if (coop_sess_loading()) hb.flags |= COOP_CTR_HB_LOADING;
    coop_ctr_hb_set(&hb);
}

// ---- Peers ----------------------------------------------------------------------------------------------------------
int coop_sess_peer(int id) {
    CoopCtrHb hb;
    uint64_t age = 0;
    if (!coop_ctr_incarnation(id)) return COOP_SESS_GONE;
    if (!coop_ctr_hb(id, &hb, &age)) return COOP_SESS_SILENT;
    if (age > 3000000000ull) return COOP_SESS_GONE;
    if (hb.flags & COOP_CTR_HB_LOADING) return COOP_SESS_LOADING;
    if ((hb.flags & COOP_CTR_HB_PAUSED) || age > 1000000000ull) return COOP_SESS_PAUSED;
    return COOP_SESS_RUNS;
}
// The hold of a claim (S 4.2): the greatest, over peers that run, of (smoothed round trip + 4 x its variance) + 0.1 s,
// a peer with no sample counting 0.2 s, at most 0.4 s; 0 alone.
double coop_sess_hold_s(void) {
    double h = 0;
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (coop_sess_peer(id) != COOP_SESS_RUNS || coop_ctr_incarnation(id) == coop_ctr_me()) continue;
        double srtt = 0, var = 0, x = coop_ctr_rtt(id, &srtt, &var) ? srtt + 4 * var + 0.1 : 0.2;
        if (x > h) h = x;
    }
    return h > 0.4 ? 0.4 : h;
}

// ---- The clock, per peer (called at phase 0 only) --------------------------------------------------------------------
void coop_sess_clock_sample(int id, int64_t peer_stamp_ns) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return;
    uint64_t now = atomic_load(&frame_ns);
    clock_sample(&clocks[id], now, (int64_t)now - peer_stamp_ns);
}
void coop_sess_clock_clear(int id) {
    if (id >= 1 && id <= COOP_MAX_PLAYERS) clock_clear(&clocks[id]);
}
int coop_sess_clock(int id, uint64_t interval_ns, int64_t *t_ns, int64_t *delay_ns, uint32_t *teleports) {
    if (id < 1 || id > COOP_MAX_PLAYERS || !clocks[id].have) return 0;
    int64_t t = clock_step(&clocks[id], atomic_load(&frame_ns), interval_ns);
    if (t_ns) *t_ns = t;
    if (delay_ns) *delay_ns = clocks[id].delay;
    if (teleports) *teleports = clocks[id].teleports;
    return 1;
}

// Which syncs this game runs, in its heartbeat's flags (S 13): two games that differ say so and neither syncs enemies.
uint32_t coop_sess_caps(void) {
    int coop_enemy_new(void);   // coop_enemy.c
    return coop_enemy_new() ? COOP_CTR_HB_ENEMY : 0;
}
