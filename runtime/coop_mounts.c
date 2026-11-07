// Co-op horses and animals: synced as the enemies are, and riding a horse with another player there.
//
// TotK Online (read for behaviour only, no code of it here) syncs every actor whose name starts with one of its
// filter's prefixes, Animal and GameRomHorse among them, by the same owner/copy model as its enemies: one game owns
// each (the nearest player's, handed over every few seconds), the others keep their own copy running its own AI and
// pull it to the owner's place. GameRomHorse is also in its BSA list, so a horse copy's action requests are replaced
// by its owner's as an enemy copy's are; Animal is not. Riding does not link actors there: a ridden horse goes to the
// rider's game because that player is the nearest.
//
// Here (TOTK_COOP_MOUNTS=1, the native enemy model only) coop_world.c's enemy sync takes them in as more kinds
// (coop_world_kind): the same keys (placement hash, else name and first place), owner choice and hand-over,
// native_follow's easing, the owner's life, hits sent to the owner, and the spawn sync for a horse made at run time.
// What is the mounts' own, in this file:
// - the kinds: horses (GameRomHorse*, Animal_Horse*, not GameRomHorseSaddle_/Reins_: those ride on their horse in
//   each game) and the animals that walk; fish, insects, birds and swarms are left out (many, small, and they would
//   take the enemies' room in the 28-entry world packet; the mod syncs them);
// - a ridden horse is its rider's game's, whoever had it (coop_mount_rider, used by coop_world.c peer_one), and is not
//   handed over while ridden;
// - this Link's ride: found by place, as no riding flag of 1.4.3 is known: this Link inside a box above a horse's
//   origin, in the horse's frame, for MOUNT_HOLD_TICKS ticks in a row (coop_mounts_tick). Sent in NetState with
//   ride_owner COOP_RIDE_MOUNT, ride_id the horse's key, ride_off this Link in its frame;
// - a remote rider's stand-in (co-op Link or PlayerPuppet) put at that offset on this game's copy of the horse
//   (coop_mount_place, from coop_players.c owner_matrix), not at its owner's delayed place, so it sits on the copy
//   the native model eases after the owner's horse.
// None of it has run in a game yet (2026-11-07): the box is a guess, to be set from TOTK_COOP_TEST_MOUNT's log.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_mounts.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int coop_mounts_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_MOUNTS"), *m = cfg_get("TOTK_COOP_ENEMY_MODEL");
        on = e && *e == '1' && !(m && !strcmp(m, "puppet"));   // (the frozen-copy model poses by bones: enemies only)
        if (on) rt_log("coop: mounts: on (horses and animals synced as the enemies are)");
    }
    return on;
}

static int is_horse(const char *nm) {
    if (!strncmp(nm, "GameRomHorse", 12)) return strncmp(nm + 12, "Saddle", 6) && strncmp(nm + 12, "Reins", 5);
    return !strncmp(nm, "Animal_Horse", 12);
}
// Animal_ names left out: fish, insects, the birds, and every swarm.
static int animal_skipped(const char *nm) {
    static const char *skip[] = {"Fish", "Insect", "LittleBird", "SkyLittleBird", "Crow", "Gull", "SkyGull", "Pigeon",
                                 "SkyPigeon", "Hawk", "Heron", "WildDuck", "Ptarmigan"};
    if (strstr(nm, "_Swarm")) return 1;
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++) {
        size_t n = strlen(skip[i]);
        if (!strncmp(nm + 7, skip[i], n) && (nm[7 + n] == '_' || !nm[7 + n])) return 1;
    }
    return 0;
}
int coop_world_kind(const char *nm) {
    if (!strncmp(nm, "Enemy_", 6)) return COOP_KIND_ENEMY;
    if (!coop_mounts_on()) return 0;
    if (is_horse(nm)) return COOP_KIND_HORSE;
    if (!strncmp(nm, "Animal_", 7) && !animal_skipped(nm)) return COOP_KIND_ANIMAL;
    return 0;
}
const char *coop_world_kind_word(int kind) {
    return kind == COOP_KIND_HORSE ? "horse" : kind == COOP_KIND_ANIMAL ? "animal" : "enemy";
}
int coop_world_spawn_kind(const char *nm) {
    int k = coop_world_kind(nm);
    if (k != COOP_KIND_ANIMAL) return k;
    static int all = -1;
    if (all < 0) { const char *e = cfg_get("TOTK_COOP_MOUNTS_SPAWN"); all = e && !strcmp(e, "all"); }
    return all ? k : 0;
}
int coop_world_action_kind(const char *nm) {
    int k = coop_world_kind(nm);
    return k == COOP_KIND_ENEMY || k == COOP_KIND_HORSE ? k : 0;
}

// ---- the horses this game has (refreshed about 10 times a second from coop_world.c's synced actors)
#define HORSE_N 32
static struct { _Atomic uint64_t actor; _Atomic uint32_t key; char name[48]; } horses[HORSE_N];
static _Atomic int n_horses;
typedef struct { int n; uint64_t a[HORSE_N]; uint32_t k[HORSE_N]; char nm[HORSE_N][48]; } HorseScan;
static void horse_one(uint64_t a, const char *nm, uint32_t key, int kind, void *arg) {
    HorseScan *s = arg;
    if (kind != COOP_KIND_HORSE || s->n >= HORSE_N) return;
    s->a[s->n] = a, s->k[s->n] = key, snprintf(s->nm[s->n], sizeof s->nm[0], "%s", nm), s->n++;
}
static uint64_t horse_of_key(uint32_t key, char *nm, size_t n) {
    int cnt = atomic_load(&n_horses);
    for (int i = 0; i < cnt; i++)
        if (atomic_load_explicit(&horses[i].key, memory_order_relaxed) == key) {
            if (nm) snprintf(nm, n, "%s", horses[i].name);
            return atomic_load_explicit(&horses[i].actor, memory_order_relaxed);
        }
    return 0;
}
// Who runs a key, for the logs: "here", "by player <n>", "nowhere yet".
static const char *run_by(uint32_t key, char *buf, size_t n) {
    int ow = coop_world_owner_of(key);
    if (ow == 0) snprintf(buf, n, "here");
    else if (ow < 0) snprintf(buf, n, "nowhere yet");
    else snprintf(buf, n, "by player %d", ow);
    return buf;
}
static int live(uint64_t a) {
    int32_t st = 0;
    return a && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6;
}
// p (world) in the frame of an actor at hp with rotation r (rows): r^T (p - hp).
static void in_frame(const float hp[3], const float r[9], const float p[3], float out[3]) {
    float d[3] = {p[0] - hp[0], p[1] - hp[1], p[2] - hp[2]};
    for (int j = 0; j < 3; j++) out[j] = r[0 * 3 + j] * d[0] + r[1 * 3 + j] * d[1] + r[2 * 3 + j] * d[2];
}

// ---- this Link's ride
// The box (TOTK_COOP_MOUNT_BOX=x,ylo,yhi,z, metres in the horse's frame: |x| across, ylo..yhi up, |z| along). A
// GameRomHorse01's origin is about half a metre above the ground it stands on: a Link standing beside it was at
// -0.83 -0.5 0 in its frame, and behind it -0.4 -1.0 -2.2 (suite case mount, 2026-11-07). On its back a Link is
// across 0, so the box is narrow across and loose up and down; the riding numbers are not seen yet.
#define MOUNT_HOLD_TICKS 10   // a third of a second at 30 ticks a second: not a Link jumping past
static float box[4] = {0.45f, -0.3f, 2.0f, 0.9f};
static uint32_t ride_key;      // the horse this Link rides (0 none)
static float ride_off[3];
static PlatMutex ride_lock = PLAT_MUTEX_INIT;
int coop_mount_riding(uint32_t *key, float off[3]) {
    plat_mutex_lock(&ride_lock);
    uint32_t k = ride_key;
    if (k) *key = k, memcpy(off, ride_off, 12);
    plat_mutex_unlock(&ride_lock);
    return k != 0;
}
int coop_mount_rider(uint32_t key, int my_id) {
    if (!key || !coop_mounts_on()) return 0;
    plat_mutex_lock(&ride_lock);
    int mine = ride_key == key;
    plat_mutex_unlock(&ride_lock);
    if (mine) return my_id;
    int r = coop_player_riding_mount(key);
    // (logged when a horse's remote rider changes: peer_one logs only an owner change, and the rider's game may have
    // run the horse already)
    static struct { uint32_t key; int rider; } last[16];
    static int nlast;
    int i = 0;
    while (i < nlast && last[i].key != key) i++;
    if (i == nlast && r && nlast < 16) last[nlast].key = key, last[nlast++].rider = 0;
    if (i < nlast && last[i].rider != r) {
        char nm[48] = "-";
        horse_of_key(key, nm, sizeof nm);
        if (r) rt_log("coop: mounts: player %d rides %s %08x", r, nm, key);
        else rt_log("coop: mounts: player %d is off %s %08x", last[i].rider, nm, key);
        last[i].rider = r;
    }
    return r;
}

void coop_mounts_tick(Cpu *c, uint64_t me) {
    (void)c;
    if (!coop_mounts_on()) return;
    static int t, cfg_done;
    if (!cfg_done) {
        cfg_done = 1;
        const char *e = cfg_get("TOTK_COOP_MOUNT_BOX");
        float b[4];
        if (e && sscanf(e, "%f,%f,%f,%f", &b[0], &b[1], &b[2], &b[3]) == 4) memcpy(box, b, sizeof box);
    }
    if (t++ % 3 == 0) {   // the horses here
        static HorseScan s;
        s.n = 0;
        coop_world_each_mount(horse_one, &s);
        atomic_store(&n_horses, 0);
        for (int i = 0; i < s.n; i++) {
            atomic_store_explicit(&horses[i].actor, s.a[i], memory_order_relaxed);
            atomic_store_explicit(&horses[i].key, s.k[i], memory_order_relaxed);
            memcpy(horses[i].name, s.nm[i], sizeof horses[i].name);
        }
        atomic_store(&n_horses, s.n);
    }
    float lp[3];
    if (!me || !coop_rd(me + COOP_ACTOR_POS, lp, 12)) return;
    // Which horse this Link is on: the one whose box it is in, nearest its origin across.
    int cnt = atomic_load(&n_horses), on = -1;
    float on_off[3] = {0}, best_d = 1e30f, near_d = 1e30f, near_off[3] = {0};
    int near_i = -1;
    for (int i = 0; i < cnt; i++) {
        uint64_t a = atomic_load_explicit(&horses[i].actor, memory_order_relaxed);
        float hp[3], hr[9], o[3];
        if (!live(a) || !coop_rd(a + COOP_ACTOR_POS, hp, 12) || !coop_rd(a + COOP_ACTOR_ROT, hr, 36)) continue;
        in_frame(hp, hr, lp, o);
        float d = sqrtf(o[0] * o[0] + o[2] * o[2]);
        if (d + fabsf(o[1]) < near_d) near_d = d + fabsf(o[1]), near_i = i, memcpy(near_off, o, 12);
        if (fabsf(o[0]) < box[0] && o[1] > box[1] && o[1] < box[2] && fabsf(o[2]) < box[3] && d < best_d)
            best_d = d, on = i, memcpy(on_off, o, 12);
    }
    static int held;
    static uint32_t held_key;
    uint32_t k = on >= 0 ? atomic_load_explicit(&horses[on].key, memory_order_relaxed) : 0;
    if (k && k == held_key) held++;
    else held_key = k, held = k ? 1 : 0;
    plat_mutex_lock(&ride_lock);
    uint32_t was = ride_key;
    if (k && held >= MOUNT_HOLD_TICKS) ride_key = k, memcpy(ride_off, on_off, 12);
    else if (!k) ride_key = 0;   // (on another horse's box: kept on the old one until the new one has held)
    uint32_t now_key = ride_key;
    plat_mutex_unlock(&ride_lock);
    if (now_key != was) {
        char nm[48] = "-";
        if (now_key) horse_of_key(now_key, nm, sizeof nm);
        char rb[24];
        if (now_key)
            rt_log("coop: mounts: this Link rides %s %08x (%.2f %.2f %.2f in its frame; run %s)", nm, now_key, on_off[0],
                   on_off[1], on_off[2], run_by(now_key, rb, sizeof rb));
        else rt_log("coop: mounts: this Link is off horse %08x", was);
    }
    // Test (TOTK_COOP_TEST_MOUNT_PUT=<s>[:<player id>], that player's game only when given): at <s>, <s>+2 and <s>+4 s
    // into the world, while this Link rides nothing,
    // it is put 0.9 m beside the nearest horse (its left, on its ground) facing it, where the game offers to mount
    // with A (the script presses A then; walking up to a horse that wanders did not get there in time, 2026-11-07).
    static float put_at = -2;
    static int put_id;
    if (put_at == -2) {
        const char *e = cfg_get("TOTK_COOP_TEST_MOUNT_PUT"), *col = e ? strchr(e, ':') : NULL;
        put_at = e ? (float)atof(e) : -1, put_id = col ? atoi(col + 1) : 0;
    }
    if (put_id && coop_net_id() != put_id) put_at = -1;
    static int puts;
    float wt = frame_step_world_us() / 1e6f;
    if (put_at >= 0 && puts < 3 && wt >= put_at + 2 * puts && near_i >= 0 && !now_key && c) {
        puts++;
        uint64_t a = atomic_load_explicit(&horses[near_i].actor, memory_order_relaxed);
        float hp[3], hr[9];
        if (live(a) && coop_rd(a + COOP_ACTOR_POS, hp, 12) && coop_rd(a + COOP_ACTOR_ROT, hr, 36)) {
            const float side[3] = {-0.9f, -0.5f, 0};
            float m[12], f[3] = {hr[0], 0, hr[6]};   // facing the horse: its +x (column 0), level
            float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
            if (fl > 1e-3f) {
                f[0] /= fl, f[2] /= fl;
                // columns: x = up cross forward, y up, z forward (a Link faces its local +Z)
                const float col[3][3] = {{f[2], 0, -f[0]}, {0, 1, 0}, {f[0], 0, f[2]}};
                for (int r = 0; r < 3; r++) {
                    for (int k = 0; k < 3; k++) m[r * 4 + k] = col[k][r];
                    m[r * 4 + 3] = hp[r] + hr[r * 3 + 0] * side[0] + hr[r * 3 + 1] * side[1] + hr[r * 3 + 2] * side[2];
                }
                coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
                rt_log("coop: test: mount: this Link put beside %s at %.1f s (%.2f %.2f %.2f)", horses[near_i].name, wt, m[3], m[7], m[11]);
            }
        }
    }
    // Test (TOTK_COOP_TEST_MOUNT=1): once a second, the nearest horse and where this Link is in its frame (the box's
    // numbers come from these), and who runs it.
    static int test = -1;
    if (test < 0) test = cfg_get("TOTK_COOP_TEST_MOUNT") != NULL;
    if (test && t % 30 == 0 && near_i >= 0) {
        uint32_t nk = atomic_load_explicit(&horses[near_i].key, memory_order_relaxed);
        char rb[24];
        rt_log("coop: test: mount: nearest %s %08x, this Link at %.2f %.2f %.2f in its frame%s (run %s)", horses[near_i].name,
               nk, near_off[0], near_off[1], near_off[2], now_key == nk ? ", riding" : "", run_by(nk, rb, sizeof rb));
    }
}

int coop_mount_place(uint32_t key, const float off[3], float out[3]) {
    if (!coop_mounts_on()) return 0;
    char nm[48];
    uint64_t a = horse_of_key(key, nm, sizeof nm);
    float p[3], r[9];
    if (!live(a) || !coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36)) return 0;
    for (int i = 0; i < 3; i++) out[i] = p[i] + r[i * 3 + 0] * off[0] + r[i * 3 + 1] * off[1] + r[i * 3 + 2] * off[2];
    static _Atomic uint32_t said_key;
    if (atomic_exchange(&said_key, key) != key) {   // (called on the stand-ins' threads)
        char rb[24];
        rt_log("coop: mounts: a rider's stand-in is put on this game's %s %08x (run %s)", nm, key, run_by(key, rb, sizeof rb));
    }
    return 1;
}
