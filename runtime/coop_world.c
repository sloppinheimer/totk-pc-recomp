// Co-op shared world: the host's enemies on the guests' screens.
//
// Every live actor passes the per-actor calc (main+0x41bb20) each frame; coop_actor_seen records it with the world
// time it was last seen and where it was first seen. An enemy (actor name "Enemy_...") is known in every game by
// its name and that first position: placed enemies are created at the same spot in every game on a save (checked
// to 0.1 m), and the map's own placement hash was not found in the actor (re-notes).
//
// The host (coop_world_role 1) sends, about 10 times a second, every enemy within SYNC_M of any player: key,
// position, rotation, life, and those it saw die in the last 5 s marked dead. A guest (role 2) puts its own copy of
// each where the host has it when it drifts, keeps its life at the host's, and kills it when the host's died; when
// its copy loses life the host has not taken (the guest hit it), it sends the damage, and the host takes it from
// its enemy. Enemies the host does not report (too far from everyone, or only in this game) are left alone.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define SYNC_M 120.0f          // host: enemies this close to a player are sent
#define SNAP_M 1.5f            // guest: its copy is put at the host's position past this
#define DEAD_KEEP_US 5000000   // host: an enemy that died is reported dead this long

// ---- the actors this game updates
#define SEEN_N 16384
static struct {
    _Atomic uint64_t actor;
    _Atomic int64_t at_us;
    float home[3];        // where it was first seen
    _Atomic int home_ok;
} seen[SEEN_N];

void coop_actor_seen(uint64_t actor) {
    if (!actor) return;
    int64_t now = frame_step_world_us();
    uint32_t h = (uint32_t)((actor >> 4) * 2654435761u) & (SEEN_N - 1);
    for (int k = 0; k < 32; k++, h = (h + 1) & (SEEN_N - 1)) {
        uint64_t a = atomic_load_explicit(&seen[h].actor, memory_order_relaxed);
        if (a == actor) { atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed); return; }
        // An empty cell, or one whose actor has not been updated for 10 s (gone): take it.
        int64_t at = atomic_load_explicit(&seen[h].at_us, memory_order_relaxed);
        if ((!a || now - at > 10000000) && atomic_compare_exchange_strong(&seen[h].actor, &a, actor)) {
            atomic_store_explicit(&seen[h].home_ok, 0, memory_order_relaxed);
            atomic_store_explicit(&seen[h].at_us, now, memory_order_relaxed);
            return;
        }
    }
}

// Calls fn for each actor updated in the last second, with its cell (for the first position).
static void each_live(void (*fn)(int cell, uint64_t actor, void *arg), void *arg) {
    int64_t now = frame_step_world_us();
    for (int i = 0; i < SEEN_N; i++) {
        uint64_t a = atomic_load_explicit(&seen[i].actor, memory_order_relaxed);
        if (a && now - atomic_load_explicit(&seen[i].at_us, memory_order_relaxed) < 1000000) fn(i, a, arg);
    }
}

void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg) {
    int64_t now = frame_step_world_us();
    for (int i = 0; i < SEEN_N; i++) {
        uint64_t a = atomic_load_explicit(&seen[i].actor, memory_order_relaxed);
        if (a && now - atomic_load_explicit(&seen[i].at_us, memory_order_relaxed) < 1000000) fn(a, arg);
    }
}

// ---- enemies: key, life
// Life component [[actor+0x228] +0x2f8]: life u32 at [comp+0x1830] +8, max life at [comp+0x1838] +8 (AI query
// QueryActorLife; Link reads 14 of 152 with 3.5 of 38 hearts, quarter hearts) (2026-11-04).
static uint64_t life_word(uint64_t a, int k) {
    uint64_t lc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2f8);
    uint64_t p = lc ? coop_rd_ptr(lc + 0x1830 + 8 * (uint64_t)k) : 0;
    return p ? p + 8 : 0;
}
static int life_of(uint64_t a) {
    uint32_t v = 0;
    uint64_t p = life_word(a, 0);
    return p && coop_rd(p, &v, 4) ? (int)v : -1;
}
static void set_life(uint64_t a, int v) {
    uint64_t p = life_word(a, 0);
    if (p) ST32(p, (uint32_t)(v < 0 ? 0 : v));
}

static uint32_t enemy_key(const char *name, const float home[3]) {
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int k = 0; k < 3; k++) {
        int32_t q = (int32_t)lroundf(home[k] * 2.0f);   // half metres: both games create it at the same spot
        for (int b = 0; b < 4; b++) h = (h ^ (uint8_t)(q >> (8 * b))) * 16777619u;
    }
    return h ? h : 1;
}

// An enemy this game updates, with its key (0 if not an enemy).
static uint32_t enemy_of(int cell, uint64_t a, char *nm, size_t n) {
    coop_actor_name(a, nm, n);
    if (strncmp(nm, "Enemy_", 6)) return 0;
    if (!atomic_load_explicit(&seen[cell].home_ok, memory_order_relaxed)) {
        if (!coop_rd(a + COOP_ACTOR_POS, seen[cell].home, 12)) return 0;
        atomic_store_explicit(&seen[cell].home_ok, 1, memory_order_relaxed);
    }
    return enemy_key(nm, seen[cell].home);
}

typedef struct { uint32_t key; uint16_t life, flags; float pos[3], rot[9]; } NetEnemy;   // flags bit 0: dead
_Static_assert(sizeof(NetEnemy) == 56, "NetEnemy wire size");
#define MAX_SENT 30
typedef struct { uint16_t count, reserved; NetEnemy e[MAX_SENT]; } WorldPacket;
typedef struct { uint32_t key; int32_t damage; } HitPacket;

// ---- host
static struct { uint32_t key; int64_t died_us; } dead[64];   // enemies seen to die, reported a while
static struct { uint32_t key; uint64_t actor; int life; int64_t at_us; } host_known[256];   // as last seen

typedef struct { WorldPacket pk; float players[COOP_MAX_PLAYERS + 1][3]; int np; } HostScan;
static void host_one(int cell, uint64_t a, void *arg) {
    HostScan *h = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key || h->pk.count >= MAX_SENT) return;
    NetEnemy e = {.key = key};
    if (!coop_rd(a + COOP_ACTOR_POS, e.pos, 12) || !coop_rd(a + COOP_ACTOR_ROT, e.rot, 36)) return;
    int near = 0;
    for (int i = 0; i < h->np && !near; i++) {
        float dx = e.pos[0] - h->players[i][0], dz = e.pos[2] - h->players[i][2];
        near = dx * dx + dz * dz < SYNC_M * SYNC_M;
    }
    if (!near) return;
    int life = life_of(a);
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    e.life = (uint16_t)(life < 0 ? 0 : life > 65535 ? 65535 : life);
    if (life == 0 || st > 6) e.flags |= 1;
    h->pk.e[h->pk.count++] = e;
    // Remembered for the hits, and for reporting its death once it is gone.
    int64_t now = frame_step_world_us();
    int free_i = -1;
    for (int i = 0; i < 256; i++) {
        if (host_known[i].key == key) { host_known[i].actor = a, host_known[i].life = life, host_known[i].at_us = now; return; }
        if (free_i < 0 && !host_known[i].key) free_i = i;
    }
    if (free_i >= 0) host_known[free_i].key = key, host_known[free_i].actor = a, host_known[free_i].life = life,
                     host_known[free_i].at_us = now;
}

static void host_tick(Cpu *c, uint64_t me) {
    (void)c;
    // Hits from the guests: take the damage from this game's enemy.
    HitPacket hp;
    int from = 0, n;
    while ((n = coop_net_hit_take(&hp, sizeof hp, &from)) != 0) {
        if (n != (int)sizeof hp) continue;
        for (int i = 0; i < 256; i++)
            if (host_known[i].key == hp.key && host_known[i].actor) {
                int life = life_of(host_known[i].actor);
                if (life > 0) {
                    set_life(host_known[i].actor, life - hp.damage);
                    rt_log("coop: player %d hit enemy %08x for %d (life %d -> %d)", from, hp.key, hp.damage, life,
                           life - hp.damage < 0 ? 0 : life - hp.damage);
                }
                break;
            }
    }
    static int t;
    if (++t % 3) return;   // the rest about 10 times a second
    static HostScan h;
    memset(&h.pk, 0, sizeof h.pk);
    h.np = coop_player_positions(h.players, COOP_MAX_PLAYERS);
    if (coop_rd(me + COOP_ACTOR_POS, h.players[h.np], 12)) h.np++;
    each_live(host_one, &h);
    // Known enemies no longer seen: gone. Those whose life was 0 when last seen died: reported dead a while.
    int64_t now = frame_step_world_us();
    for (int i = 0; i < 256; i++) {
        if (!host_known[i].key || now - host_known[i].at_us < 400000) continue;
        if (host_known[i].life == 0)
            for (int d = 0; d < 64; d++)
                if (dead[d].key == host_known[i].key || !dead[d].key || now - dead[d].died_us > DEAD_KEEP_US) {
                    dead[d].key = host_known[i].key, dead[d].died_us = now;
                    break;
                }
        host_known[i].key = 0, host_known[i].actor = 0;
    }
    for (int d = 0; d < 64 && h.pk.count < MAX_SENT; d++)
        if (dead[d].key && now - dead[d].died_us < DEAD_KEEP_US) {
            NetEnemy e = {.key = dead[d].key, .flags = 1};
            h.pk.e[h.pk.count++] = e;
        }
    coop_net_world_send(&h.pk, 4 + h.pk.count * (int)sizeof(NetEnemy));
}

// ---- guest
static struct { uint32_t key; int life; } guest_last[256];   // the life this game last set or saw for each
static int *guest_life_slot(uint32_t key) {
    int free_i = -1;
    for (int i = 0; i < 256; i++) {
        if (guest_last[i].key == key) return &guest_last[i].life;
        if (free_i < 0 && !guest_last[i].key) free_i = i;
    }
    if (free_i < 0) free_i = (int)(key & 255);
    guest_last[free_i].key = key, guest_last[free_i].life = -1;
    return &guest_last[free_i].life;
}

typedef struct { Cpu *c; WorldPacket pk; int snaps; } GuestScan;
static void guest_one(int cell, uint64_t a, void *arg) {
    GuestScan *g = arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    if (!key) return;
    const NetEnemy *e = NULL;
    for (int i = 0; i < g->pk.count; i++) if (g->pk.e[i].key == key) { e = &g->pk.e[i]; break; }
    if (!e) return;   // the host does not have it near anyone: this game's own
    int life = life_of(a), *last = guest_life_slot(key);
    if (life < 0) return;
    if (e->flags & 1) {   // dead on the host
        // Life 0 and the game's own death follows (as on the host); a delete request instead lets the map make a
        // fresh one at the same spot at once (pair test 2026-11-04).
        if (life > 0) {
            set_life(a, 0);
            rt_log("coop: enemy %s %08x died on the host", nm, key);
        }
        *last = 0;
        return;
    }
    // Test (TOTK_COOP_FAKEHIT=<n>): at 20 s and 30 s into the world, the first synced enemy loses n life here.
    static int fake = -1, fake_done;
    if (fake < 0) { const char *f = getenv("TOTK_COOP_FAKEHIT"); fake = f ? atoi(f) : 0; }
    int64_t w = frame_step_world_us();
    if (fake > 0 && fake_done < 2 && w > (fake_done ? 30000000 : 20000000) && life > 0) {
        set_life(a, life - fake), life = life - fake < 0 ? 0 : life - fake, fake_done++;
        rt_log("coop: test: enemy %s %08x loses %d here", nm, key, fake);
    }
    // This game's hits: life lost since the host's value was last set here goes to the host.
    if (*last >= 0 && life < *last) {
        HitPacket hp = {key, *last - life};
        coop_net_hit_send(&hp, sizeof hp);
        rt_log("coop: hit enemy %s %08x for %d, sent to the host", nm, key, hp.damage);
    }
    if (life != (int)e->life && (*last < 0 || life >= *last)) set_life(a, e->life), life = e->life;
    *last = life;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float dx = p[0] - e->pos[0], dy = p[1] - e->pos[1], dz = p[2] - e->pos[2];
    if (dx * dx + dy * dy + dz * dz > SNAP_M * SNAP_M) {
        float m[12];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) m[r * 4 + k] = e->rot[r * 3 + k];
            m[r * 4 + 3] = e->pos[r];
        }
        coop_guest_call(g->c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, a, (uint64_t)(uintptr_t)m, 0, 0);
        g->snaps++;
    }
}

static void guest_tick(Cpu *c) {
    static GuestScan g;
    int from = 0;
    uint64_t age = 0;
    int n = coop_net_world(&g.pk, sizeof g.pk, &from, &age);
    if (n < 4 || age > 1000000000ull || 4 + g.pk.count * (int)sizeof(NetEnemy) > n) return;
    g.c = c, g.snaps = 0;
    each_live(guest_one, &g);
    static int t, snaps;
    snaps += g.snaps;
    if (++t % 300 == 0) {
        rt_log("coop: the host's world: %d enemies near players; %d put back here in the last 10 s", g.pk.count, snaps);
        snaps = 0;
    }
}

// ---- PROBE (TOTK_COOP_TRACE=enemy): enemies within 150 m of this PC's Link, with their key and life.
static uint64_t probe_me;
static void probe_one(int cell, uint64_t a, void *arg) {
    (void)arg;
    char nm[64];
    uint32_t key = enemy_of(cell, a, nm, sizeof nm);
    float p[3], q[3];
    if (!key || !coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(probe_me + COOP_ACTOR_POS, q, 12)) return;
    float dx = p[0] - q[0], dz = p[2] - q[2];
    if (dx * dx + dz * dz > 150.f * 150.f) return;
    rt_log("coop: enemy %s %08x at %.1f %.1f %.1f, life %d; this Link at %.1f %.1f %.1f", nm, key, p[0], p[1], p[2], life_of(a), q[0], q[1], q[2]);
}

// ---- PROBE (TOTK_COOP_TRACE=target): where an enemy keeps the Link it is after. For enemies within 40 m: words in
// the actor's first 0x1000 bytes, and in its components' first 0x800 (the table at actor +0x228), that equal a Link
// actor (this PC's or a co-op one); logged with the offsets, every 2 s.
int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c
static uint64_t tp_links[COOP_MAX_PLAYERS + 1], tp_refs[COOP_MAX_PLAYERS + 1];
static uint32_t tp_ids[COOP_MAX_PLAYERS + 1];   // perception ids
// An actor's perception id: its perception component [[actor+0x228] +0x378], then [[comp+0x30] +0x10] (AI query
// QueryActorGetPerceptionID, main+0x1b0c67c); 0xffffffff none.
static uint32_t perception_id(uint64_t a) {
    uint64_t comp = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x378), d = comp ? coop_rd_ptr(comp + 0x30) : 0;
    uint32_t id = 0xffffffffu;
    if (d) coop_rd(d + 0x10, &id, 4);
    return id;
}
static int tp_n;
static const char *link_of(uint64_t v) {
    static char s[32];
    for (int i = 0; i < tp_n; i++) {
        if (v && v == tp_links[i]) { snprintf(s, sizeof s, i ? "co-op Link %d" : "this Link", i); return s; }
        if (v && v == tp_refs[i]) { snprintf(s, sizeof s, i ? "ref of co-op Link %d" : "ref of this Link", i); return s; }
        if (tp_ids[i] != 0xffffffffu && tp_ids[i] > 16 && ((uint32_t)v == tp_ids[i] || (uint32_t)(v >> 32) == tp_ids[i])) {
            snprintf(s, sizeof s, i ? "id of co-op Link %d (%s)" : "id of this Link (%s)", i, (uint32_t)v == tp_ids[i] ? "low" : "high");
            return s;
        }
    }
    return NULL;
}
// A Link's reference object (an ActorLink's +8; [ref+0x40] is the actor): this PC's from the resident list.
static void target_probe_one(int cell, uint64_t a, void *arg) {
    (void)arg;
    char nm[64];
    if (!enemy_of(cell, a, nm, sizeof nm)) return;
    float p[3], q[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(tp_links[0] + COOP_ACTOR_POS, q, 12)) return;
    float dx = p[0] - q[0], dz = p[2] - q[2];
    if (dx * dx + dz * dz > 60.f * 60.f) return;
    char line[2048];
    int n = snprintf(line, sizeof line, "coop: target probe %s at %.1f m:", nm, sqrtf(dx * dx + dz * dz));
    for (int i = 1; i < tp_n; i++) {   // the co-op Links: how far, and their life (an enemy attacking one takes it)
        float r[3];
        if (!coop_rd(tp_links[i] + COOP_ACTOR_POS, r, 12)) continue;
        float ex = p[0] - r[0], ez = p[2] - r[2];
        n += snprintf(line + n, sizeof line - n, " co-op Link %d at %.1f m, life %d;", i, sqrtf(ex * ex + ez * ez), life_of(tp_links[i]));
    }
    n += snprintf(line + n, sizeof line - n, " this Link life %d;", life_of(tp_links[0]));
    static uint64_t w[0x1000 / 8], cw[0x800 / 8], tw[0x800 / 8];
    if (coop_rd(a, w, sizeof w))
        for (int j = 0; j < 0x1000 / 8 && n < 1900; j++)
            if (link_of(w[j])) n += snprintf(line + n, sizeof line - n, " actor+%#x=%s", j * 8, link_of(w[j]));
    // One level down from the actor itself: each object it points to, its first 0x400 bytes.
    static uint64_t ow[0x400 / 8];
    for (int j = 0; j < 0x1000 / 8 && n < 1900; j++) {
        uint64_t o = w[j];
        if (o < 0x100000000ull || o >= 0x8000000000ull || o == a || !coop_rd(o, ow, sizeof ow)) continue;
        for (int i = 0; i < 0x400 / 8 && n < 1900; i++)
            if (link_of(ow[i])) n += snprintf(line + n, sizeof line - n, " [actor+%#x]+%#x=%s", j * 8, i * 8, link_of(ow[i]));
    }
    uint64_t table = coop_rd_ptr(a + 0x228);
    if (table && coop_rd(table, tw, sizeof tw))
        for (int k = 0; k < 0x800 / 8 && n < 1900; k++) {
            uint64_t comp = tw[k];
            if (comp < 0x100000000ull || comp >= 0x8000000000ull || !coop_rd(comp, cw, sizeof cw)) continue;
            for (int j = 0; j < 0x800 / 8 && n < 1900; j++)
                if (link_of(cw[j])) n += snprintf(line + n, sizeof line - n, " comp[+%#x]+%#x=%s", k * 8, j * 8, link_of(cw[j]));
        }
    rt_log("%s", line);
    // The enemy's own perception data: the id slots its per-actor calc resets to -1 (+0xf8/+0xfc, +0x138/+0x13c)
    // and the words and flags reset with them; one of them may be the id of the player it is after.
    uint64_t pd = coop_rd_ptr(coop_rd_ptr(table + 0x378) + 0x30);
    if (pd) {
        uint32_t u[0x160 / 4] = {0};
        coop_rd(pd, u, sizeof u);
        const uint8_t *b = (const uint8_t *)u;
        rt_log("coop: target slots %s: id %#x; +f8 %#x +fc %#x +110 %#x +138 %#x +13c %#x +150 %#x; flags f5 %d 105 %d 135 %d 146 %d",
               nm, u[0x10 / 4], u[0xf8 / 4], u[0xfc / 4], u[0x110 / 4], u[0x138 / 4], u[0x13c / 4], u[0x150 / 4],
               b[0xf5], b[0x105], b[0x135], b[0x146]);
    }
    // The enemy's components around its perception one (table +0x360..+0x3a0): their u32s equal to this Link's id.
    for (int k = 0x360 / 8; table && k < 0x3a0 / 8; k++) {
        uint64_t comp = tw[k];
        if (comp < 0x100000000ull || comp >= 0x8000000000ull) continue;
        static uint32_t u[0x2000 / 4];
        if (!coop_rd(comp, u, sizeof u)) continue;
        char l2[1024];
        int m = snprintf(l2, sizeof l2, "coop: target probe %s comp[+%#x] %llx u32 == %#x at:", nm, k * 8,
                         (unsigned long long)comp, tp_ids[0]);
        for (int j = 0; j < 0x2000 / 4 && m < 950; j++)
            if (u[j] == tp_ids[0]) m += snprintf(l2 + m, sizeof l2 - m, " +%#x", j * 4);
        rt_log("%s", l2);
    }
}

int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c: the readers of a range, reported on disarm
void coop_watch_disarm(void);

// Test (TOTK_COOP_TEST_WATCH5=<s>): from that many seconds into the world, for 10 s, the game functions that read
// slot 5 ([main+0x3ad9830] +0x250, this Link's perception data where enemies seem to take the player from).
static const uint32_t skel_path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};   // actor to its body skeleton (g3d)
static void watch_slot5(void) {
    const char *e = cfg_get("TOTK_COOP_TEST_WATCH5");
    static int phase;
    if (!e || phase == 2) return;
    double s = frame_step_world_us() / 1e6;
    if (!phase && s >= atof(e)) {
        uint64_t obj = coop_rd_ptr(MOD_MAIN_BASE + 0x3ad9830);
        const char *what = cfg_get("TOTK_COOP_TEST_WATCHWHAT");   // =data: this Link's perception data (0x300) instead
        if (what && !strcmp(what, "slot3")) {   // the perception manager's player emitter field
            uint64_t m = coop_rd_ptr(MOD_MAIN_BASE + 0x3a6ea80);
            if (m && coop_watch_arm(m + 0x58, 8)) phase = 1, rt_log("coop: test: watching the perception manager's player field %llx", (unsigned long long)(m + 0x58));
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "coopmodel")) {   // who reads the first co-op Link's model world matrix
            extern int coop_watch_writes;
            uint64_t ca[1]; int ci[1];
            uint64_t m = coop_player_actors(ca, ci, 1) ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ca[0] + 0x228) + 0x8) + 0x28) : 0;
            coop_watch_writes = 0;
            if (m && coop_watch_arm(m + 0x1f8, 0x30)) phase = 1, rt_log("coop: test: watching the co-op Link's model %llx world matrix", (unsigned long long)m);
            else if (!m) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "coopskel")) {   // who reads the first co-op Link's body skeleton world matrices
            extern int coop_watch_writes;
            uint64_t ca[1]; int ci[1];
            uint64_t s = 0;
            if (coop_player_actors(ca, ci, 1)) {
                s = ca[0];
                for (int i = 0; i < 8 && s; i++) s = coop_rd_ptr(s + skel_path[i]);
            }
            uint64_t wm = s ? coop_rd_ptr(s + 0x18) : 0;
            coop_watch_writes = cfg_get("TOTK_COOP_TEST_WATCHWRITES") != NULL;   // =1: only who writes them
            if (wm && coop_watch_arm(wm, 127 * 0x30)) phase = 1, rt_log("coop: test: watching the co-op Link's skeleton %llx world matrices %llx", (unsigned long long)s, (unsigned long long)wm);
            else if (!wm) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "cooplife")) {   // who writes the first co-op Link's life (writes only)
            extern int coop_watch_writes;
            uint64_t ca[1]; int ci[1];
            uint64_t w = coop_player_actors(ca, ci, 1) ? life_word(ca[0], 0) : 0;
            coop_watch_writes = 1;
            if (w && coop_watch_arm(w, 4)) phase = 1, rt_log("coop: test: watching writes to the co-op Link's life %llx (%d)", (unsigned long long)w, life_of(ca[0]));
            else if (!w) return;
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "coopdata")) {   // the first co-op Link's perception data
            uint64_t ca[1]; int ci[1];
            uint64_t d = coop_player_actors(ca, ci, 1) ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ca[0] + 0x228) + 0x378) + 0x30) : 0;
            if (d && coop_watch_arm(d, 0x300)) phase = 1, rt_log("coop: test: watching the co-op Link's perception data %llx", (unsigned long long)d);
            else if (!d) return;   // no co-op Link yet: try again next tick
            else phase = 2;
            return;
        }
        if (what && !strcmp(what, "data")) {
            uint64_t me = coop_player_actor(), d = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x378) + 0x30);
            if (d && coop_watch_arm(d, 0x300)) phase = 1, rt_log("coop: test: watching this Link's perception data %llx", (unsigned long long)d);
            else phase = 2;
            return;
        }
        if (obj && coop_watch_arm(obj + 0x250, 8)) phase = 1, rt_log("coop: test: watching slot 5 at %llx", (unsigned long long)(obj + 0x250));
        else phase = 2;
    } else if (phase == 1 && s >= atof(e) + 10) {
        coop_watch_disarm();
        phase = 2;
    }
}

// ---- enemies choose between players. The game's perception has one player emitter (the manager's +0x58, this
// Link's data); coop_spawn.c's hook on the receiver check main+0x45b930 asks here, per enemy, for a co-op Link
// nearer than this one, and puts its data there for that check. Cached once a tick (the hook runs per receiver).
static _Atomic uint64_t near_me;
static struct { _Atomic uint64_t actor, data; } near_links[COOP_MAX_PLAYERS];
static _Atomic int near_n;
static _Atomic uint64_t coop_life_comps[COOP_MAX_PLAYERS];   // the co-op Links' life components, for coop_spawn.c
static void cache_links(uint64_t me) {
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS];
    int n = coop_world_role() ? coop_player_actors(acts, ids, COOP_MAX_PLAYERS) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x378) + 0x30);
        atomic_store(&near_links[i].actor, acts[i]), atomic_store(&near_links[i].data, d);
        atomic_store(&coop_life_comps[i], coop_rd_ptr(coop_rd_ptr(acts[i] + 0x228) + 0x2f8));
    }
    for (int i = n; i < COOP_MAX_PLAYERS; i++) atomic_store(&coop_life_comps[i], 0);
    atomic_store(&near_n, n);
    atomic_store(&near_me, me);
}
// A co-op Link's life component: its life update (main+0x4400b0) must not take life in this game (coop_spawn.c).
int coop_is_coop_life(uint64_t comp) {
    for (int i = 0; comp && i < COOP_MAX_PLAYERS; i++)
        if (atomic_load_explicit(&coop_life_comps[i], memory_order_relaxed) == comp) return 1;
    return 0;
}
uint64_t coop_nearest_link_data(uint64_t enemy) {
    int n = atomic_load_explicit(&near_n, memory_order_relaxed);
    uint64_t me = atomic_load_explicit(&near_me, memory_order_relaxed);
    if (!n || !me || enemy == me) return 0;
    char nm[8];
    coop_actor_name(enemy, nm, sizeof nm);
    if (strncmp(nm, "Enemy_", 6)) return 0;
    float e[3], p[3];
    if (!coop_rd(enemy + COOP_ACTOR_POS, e, 12) || !coop_rd(me + COOP_ACTOR_POS, p, 12)) return 0;
    float best = (e[0] - p[0]) * (e[0] - p[0]) + (e[1] - p[1]) * (e[1] - p[1]) + (e[2] - p[2]) * (e[2] - p[2]);
    best = sqrtf(best) - 1.0f;   // a co-op Link must be over 1 m nearer: no flicker between two players at one distance
    uint64_t pick = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = atomic_load_explicit(&near_links[i].actor, memory_order_relaxed);
        uint64_t d = atomic_load_explicit(&near_links[i].data, memory_order_relaxed);
        if (!a || !d || !coop_rd(a + COOP_ACTOR_POS, p, 12)) continue;
        float dist = sqrtf((e[0] - p[0]) * (e[0] - p[0]) + (e[1] - p[1]) * (e[1] - p[1]) + (e[2] - p[2]) * (e[2] - p[2]));
        if (dist < best) best = dist, pick = d;
    }
    return pick;
}

// Test (TOTK_COOP_TEST_MODELSCAN=<s>): once, that many seconds into the world, where each Link's position is kept
// besides the actor: the actor (0x4000), its components (the table at +0x228, 0x1000 each) and what they point to
// (0x800 each), as 3 floats in a row or with a stride of 16 (the last column of a 3x4 row matrix). For the frozen
// Link spike: the copies the model draws from.
static int near3(const float *a, const float *p) {
    return fabsf(a[0] - p[0]) < 0.02f && fabsf(a[1] - p[1]) < 0.02f && fabsf(a[2] - p[2]) < 0.02f && fabsf(p[0]) + fabsf(p[2]) > 1;
}
static uint64_t cand[256];   // the co-op Link's position copies (absolute), for TOTK_COOP_TEST_FOLLOW
static uint8_t cand_col[256];   // 1: a matrix column (stride 16), 0: 3 floats in a row
static int ncand, scanning_coop;
static void add_cand(uint64_t a, int col) {
    for (int i = 0; i < ncand; i++) if (cand[i] == a) return;
    if (ncand < 256) cand[ncand] = a, cand_col[ncand] = (uint8_t)col, ncand++;
}
static void scan_block(const char *who, const char *path, uint64_t base, size_t len, const float p[3], int *n) {
    static uint8_t buf[0x4000];
    if (len > sizeof buf || !coop_rd(base, buf, len)) return;
    for (size_t o = 0; o + 12 <= len && *n < 200; o += 4) {
        float a[3], b[3];
        memcpy(a, buf + o, 12);
        if (o + 36 <= len) memcpy(&b[0], buf + o, 4), memcpy(&b[1], buf + o + 16, 4), memcpy(&b[2], buf + o + 32, 4);
        if (near3(a, p)) { if (scanning_coop) add_cand(base + o, 0); else rt_log("coop: modelscan %s %s+%#zx: position (3 floats)", who, path, o); ++*n; }
        else if (o + 36 <= len && near3(b, p)) { if (scanning_coop) add_cand(base + o, 1); else rt_log("coop: modelscan %s %s+%#zx: position (matrix column)", who, path, o); ++*n; }
    }
}
// Runs of 3x4 row matrices (stride 0x30, or 0x40) whose translation is within 2.5 m of p, at least 30 long: bone
// world matrices. Logged with the block path, the start and the count.
static uint64_t bone_runs[2][8];
static int bone_counts[2][8], bone_stride[2][8], nbone[2];
static uint64_t visited[1 << 16];
static int first_visit(uint64_t a) {
    uint32_t h = (uint32_t)((a >> 4) * 2654435761u) & 0xffff;
    for (int i = 0; i < 0x10000; i++, h = (h + 1) & 0xffff) {
        if (visited[h] == a) return 0;
        if (!visited[h]) { visited[h] = a; return 1; }
    }
    return 0;
}
static int scan_bones(int k, const char *who, const char *path, uint64_t base, size_t len, const float p[3]) {
    static uint8_t buf[0x8000];
    if (!first_visit(base)) return 0;
    if (len > sizeof buf || !coop_rd(base, buf, len)) return 1;
    for (int stride = 0x30; stride <= 0x40; stride += 0x10)
        for (size_t s0 = 0; s0 < (size_t)stride; s0 += 4) {
            int run = 0;
            size_t start = 0;
            for (size_t o = s0; o + stride <= len; o += stride) {
                float m[12];
                memcpy(m, buf + o, 48);
                // translation as a row matrix (m[3], m[7], m[11]) or a column one (m[9..11]); near Link (world) or
                // near the origin (model space, not all zero)
                float r0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
                int ok = 0;
                for (int lay = 0; lay < 2 && !ok; lay++) {
                    float tx = lay ? m[9] : m[3], ty = lay ? m[10] : m[7], tz = lay ? m[11] : m[11];
                    float dx = tx - p[0], dy = ty - p[1], dz = tz - p[2];
                    float t2 = tx * tx + ty * ty + tz * tz;
                    ok = (dx * dx + dy * dy + dz * dz < 2.5f * 2.5f || (t2 < 2.5f * 2.5f && t2 > 1e-6f)) && r0 > 0.8f && r0 < 1.2f;
                }
                if (ok) { if (!run) start = o; run++; }
                if (!ok || o + 2 * stride > len) {
                    if (run >= 30) {
                        rt_log("coop: bonescan %s %s+%#zx: %d matrices, stride %#x", who, path, start, run, stride);
                        if (nbone[k] < 8) bone_runs[k][nbone[k]] = base + start, bone_counts[k][nbone[k]] = run, bone_stride[k][nbone[k]] = stride, nbone[k]++;
                    }
                    run = 0;
                }
            }
        }
    return 1;
}
// A g3d SkeletonObj (ModelObj::Initialize main+0x1f38c8 sets it up): +0x18 world matrices (3x4, 0x30 each) right
// after +0x20 local ones (0x40 each), both again at +0x58 / +0x60, the bone count u16 at +0x48. Found per Link.
static uint64_t skel_of[2][4];
static int nskel[2];
static int is_skeleton(uint64_t s) {
    uint64_t w[0xd];
    uint16_t n = 0;
    if (!coop_rd(s, w, sizeof w) || !coop_rd(s + 0x48, &n, 2)) return 0;
    return n >= 20 && n <= 600 && w[4] && w[4] == w[0xc] && (w[3] - w[4] == (uint64_t)n * 0x40 || w[0xb] - w[4] == (uint64_t)n * 0x40);
}
static void find_skeletons(int k, uint64_t obj, int depth, const char *path) {
    static uint64_t w[0x400 / 8];
    if (depth > 7) return;
    uint64_t buf[0x400 / 8];
    if (!coop_rd(obj, buf, sizeof buf)) return;
    (void)w;
    for (int i = 0; i < 0x400 / 8; i++) {
        uint64_t q = buf[i];
        if (q < 0x100000000ull || q >= 0x8000000000ull || !first_visit(q)) continue;
        char p2[160];
        snprintf(p2, sizeof p2, "%s+%#x", path, i * 8);
        if (is_skeleton(q)) {
            uint16_t n = 0;
            coop_rd(q + 0x48, &n, 2);
            uint64_t wm = coop_rd_ptr(q + 0x18);
            float m[12] = {0};
            coop_rd(wm, m, 48);
            rt_log("coop: skeleton %s %s -> %llx: %u bones, world matrices %llx, first at %.2f %.2f %.2f",
                   k ? "co-op Link" : "this Link", p2, (unsigned long long)q, n, (unsigned long long)wm, m[3], m[7], m[11]);
            if (nskel[k] < 4) skel_of[k][nskel[k]++] = q;
            continue;
        }
        find_skeletons(k, q, depth + 1, p2);
    }
}
// The objects on the way from a Link to its body skeleton (the path in TOTK_COOP_TEST_SKELCOPY): each one's first
// 0x1000 bytes searched for the Link's position (3 floats, or a 3x4 row matrix's last column), logged by step.
static void path_scan(const char *who, uint64_t a) {
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    uint64_t node = a;
    for (int step = 0; step <= 8 && node; step++) {
        static uint8_t buf[0x1000];
        if (coop_rd(node, buf, sizeof buf))
            for (size_t o = 0; o + 36 <= sizeof buf; o += 4) {
                float c3[3], col[3];
                memcpy(c3, buf + o, 12);
                memcpy(&col[0], buf + o, 4), memcpy(&col[1], buf + o + 16, 4), memcpy(&col[2], buf + o + 32, 4);
                if (near3(c3, p)) rt_log("coop: pathscan %s step %d (%llx) +%#zx: position (3 floats)", who, step, (unsigned long long)node, o);
                else if (near3(col, p)) rt_log("coop: pathscan %s step %d (%llx) +%#zx: position (matrix column)", who, step, (unsigned long long)node, o);
            }
        if (step < 8) node = coop_rd_ptr(node + skel_path[step]);
    }
}
static void model_scan(uint64_t me) {
    const char *e = cfg_get("TOTK_COOP_TEST_MODELSCAN");
    static int done;
    if (!e || done || frame_step_world_us() / 1e6 < atof(e)) return;
    done = 1;
    uint64_t links[2] = {me, 0};
    int ids[1];
    coop_player_actors(&links[1], ids, 1);
    for (int k = 0; k < 2 && links[k]; k++) {
        uint64_t a = links[k];
        float p[3];
        if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) continue;
        const char *who = k ? "co-op Link" : "this Link";
        scanning_coop = k;
        memset(visited, 0, sizeof visited);
        find_skeletons(k, a, 0, "actor");
        path_scan(k ? "co-op Link" : "this Link", a);
        rt_log("coop: skeleton %s: %d found", k ? "co-op Link" : "this Link", nskel[k]);
        memset(visited, 0, sizeof visited);
        int n = 0;
        rt_log("coop: modelscan %s %llx at %.2f %.2f %.2f", who, (unsigned long long)a, p[0], p[1], p[2]);
        scan_block(who, "actor", a, 0x4000, p, &n);
        uint64_t table = coop_rd_ptr(a + 0x228);
        for (int j = 0; table && j < 0x100; j++) {
            uint64_t comp = coop_rd_ptr(table + j * 8);
            if (comp < 0x100000000ull || comp >= 0x8000000000ull) continue;
            char path[64];
            snprintf(path, sizeof path, "comp[+%#x]", j * 8);
            scan_block(who, path, comp, 0x1000, p, &n);
            static uint64_t w[0x1000 / 8];
            if (!coop_rd(comp, w, sizeof w)) continue;
            for (int i = 0; i < 0x1000 / 8; i++) {
                uint64_t q = w[i];
                if (q < 0x100000000ull || q >= 0x8000000000ull || q == a) continue;
                snprintf(path, sizeof path, "[comp[+%#x]+%#x]", j * 8, i * 8);
                scan_block(who, path, q, 0x800, p, &n);
                if (!scan_bones(k, who, path, q, 0x8000, p)) continue;
                // and one level further: bone arrays hang off model objects
                static uint64_t w2[0x400 / 8];
                if (!coop_rd(q, w2, sizeof w2)) continue;
                for (int i2 = 0; i2 < 0x400 / 8; i2++) {
                    uint64_t q2 = w2[i2];
                    if (q2 < 0x100000000ull || q2 >= 0x8000000000ull || q2 == a || q2 == q) continue;
                    char p2[96];
                    snprintf(p2, sizeof p2, "[[comp[+%#x]+%#x]+%#x]", j * 8, i * 8, i2 * 8);
                    scan_bones(k, who, p2, q2, 0x8000, p);
                }
            }
        }
        rt_log("coop: modelscan %s: %d places", who, n);
        if (k) rt_log("coop: modelscan co-op Link: %d unique copies", ncand);
    }
}

// The copied pose (TOTK_COOP_TEST_SKELCOPY): this Link's body skeleton onto the first co-op Link's, its world
// matrices (both buffers, +0x18 and +0x58) and local ones (+0x20).
static _Atomic uint64_t pose_src, pose_dst, pose_comp;
static _Atomic int pose_bones;
void coop_pose_copy_now(void) {
    uint64_t s0 = atomic_load(&pose_src), s1 = atomic_load(&pose_dst);
    int n = atomic_load(&pose_bones);
    if (!s0 || !s1 || n <= 0 || n > 600) return;
    static __thread uint8_t tmp[600 * 0x40];
    static const int fields[3][2] = {{0x18, 0x30}, {0x58, 0x30}, {0x20, 0x40}};
    for (int f = 0; f < 3; f++) {
        uint64_t from = coop_rd_ptr(s0 + fields[f][0]), to = coop_rd_ptr(s1 + fields[f][0]);
        size_t len = (size_t)n * fields[f][1];
        if (from && to && len <= sizeof tmp && coop_rd(from, tmp, len)) memcpy((void *)(uintptr_t)to, tmp, len);
    }
}
// main+0x2e9c80 (x0: an animation binding; [[x0+8]+8] the model component it poses, the one at [actor+0x228] +0x8)
// has just written a model's pose: when it is the co-op Link's, the copied pose goes over it.
// The bindings main+0x2e9c80 has posed (coop_pose_saw, from its hook); the tick finds the one the co-op Link holds
// (a pointer to it in the actor or its components) and keeps it in pose_binding.
static _Atomic uint64_t bindings_seen[16], pose_binding;
void coop_pose_saw(uint64_t binding) {
    for (int i = 0; binding && i < 16; i++) {
        uint64_t have = atomic_load_explicit(&bindings_seen[i], memory_order_relaxed);
        if (have == binding) return;
        if (!have && atomic_compare_exchange_strong(&bindings_seen[i], &have, binding)) return;
    }
}
// Before main+0x2e9c80 poses the co-op Link's model: this Link's animation pose (the binding's [+8] +0x38, 0x1c a
// bone - rotation and translation - count at +0x30) copied over the co-op Link's, so its model is posed with it.
void coop_pose_before_anim(uint64_t binding) {
    uint64_t mine = atomic_load(&bindings_seen[0]);
    if (!mine || binding == mine) return;
    uint64_t s = coop_rd_ptr(mine + 8), d = coop_rd_ptr(binding + 8);
    uint32_t ns = 0, nd = 0;
    if (!s || !d || !coop_rd(s + 0x30, &ns, 4) || !coop_rd(d + 0x30, &nd, 4)) return;
    uint32_t n = ns < nd ? ns : nd;
    uint64_t ps = coop_rd_ptr(s + 0x38), pd = coop_rd_ptr(d + 0x38);
    static __thread uint8_t tmp[1024 * 0x1c];
    if (!ps || !pd || !n || n > 1024 || !coop_rd(ps, tmp, (size_t)n * 0x1c)) return;
    memcpy((void *)(uintptr_t)pd, tmp, (size_t)n * 0x1c);
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) == 0) rt_log("coop: test: animation pose copied before the co-op Link's: %u bones (%u, %u)", n, ns, nd);
}
int coop_pose_wanted(void) { return atomic_load_explicit(&pose_comp, memory_order_relaxed) != 0; }
int coop_pose_is_target(uint64_t binding) {
    return binding && binding == atomic_load_explicit(&pose_binding, memory_order_relaxed);
}
static void find_pose_binding(uint64_t actor) {
    if (atomic_load(&pose_binding)) return;
    static uint64_t w[0x4000 / 8], cw[0x1000 / 8];
    if (!coop_rd(actor, w, sizeof w)) return;
    uint64_t table = coop_rd_ptr(actor + 0x228);
    for (int i = 0; i < 16; i++) {
        uint64_t b = atomic_load(&bindings_seen[i]);
        if (!b) continue;
        for (int j = 0; j < 0x4000 / 8; j++)
            if (w[j] == b) { atomic_store(&pose_binding, b); rt_log("coop: test: the co-op Link's animation binding %llx at actor+%#x", (unsigned long long)b, j * 8); return; }
        for (int k = 0; table && k < 0x100; k++) {
            uint64_t comp = coop_rd_ptr(table + k * 8);
            if (comp < 0x100000000ull || comp >= 0x8000000000ull || !coop_rd(comp, cw, sizeof cw)) continue;
            for (int j = 0; j < 0x1000 / 8; j++)
                if (cw[j] == b) {
                    atomic_store(&pose_binding, b);
                    rt_log("coop: test: the co-op Link's animation binding %llx at comp[+%#x]+%#x", (unsigned long long)b, k * 8, j * 8);
                    return;
                }
        }
    }
    // Not referenced there: by elimination, for this test (this Link's binding is posed from the start, before the
    // co-op Link exists, so it is the first one seen; the next one is the co-op Link's).
    uint64_t second = atomic_load(&bindings_seen[1]);
    if (second) {
        atomic_store(&pose_binding, second);
        rt_log("coop: test: the co-op Link's animation binding %llx, by elimination (this Link's %llx)", (unsigned long long)second,
               (unsigned long long)atomic_load(&bindings_seen[0]));
    }
}
void coop_world_tick(Cpu *c, uint64_t me) {
    cache_links(me);
    watch_slot5();
    model_scan(me);
    // Test (TOTK_COOP_TEST_MOVE=1|2|3): every tick, the first co-op Link's owner position written into the two 3x4
    // matrices on the way to its skeleton: [actor+0x48] +0xa34 (1), [[actor+0x48]+0x2a8] +0x26c (2), or both (3)
    // (translation column: +0xc, +0x1c, +0x2c). With TOTK_COOP_TEST_FREEZE: what moves the frozen model.
    const char *mv = cfg_get("TOTK_COOP_TEST_MOVE");
    float owner[1][3];
    uint64_t ma[1];
    int mi[1];
    if (mv && atoi(mv) == 6 && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        // =6: the model (component +0x8, +0x28): its world matrix +0x1f8 (3x4 rows) takes the owner's position, and
        // its dirty bit (+0x240 bit 0) is set so main+0x24cc30 pushes it to the model units (needs that phase to run:
        // TOTK_COOP_TEST_FREEZE_PHASES=0).
        uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ma[0] + 0x228) + 0x8) + 0x28);
        if (model) {
            ST32(model + 0x1f8 + 0xc, *(uint32_t *)&owner[0][0]);
            ST32(model + 0x1f8 + 0x1c, *(uint32_t *)&owner[0][1]);
            ST32(model + 0x1f8 + 0x2c, *(uint32_t *)&owner[0][2]);
            ST8(model + 0x240, LD8(model + 0x240) | 1);
        }
        static int said6;
        if (!said6++) rt_log("coop: test: move 6: model %llx follows the owner", (unsigned long long)model);
    } else if (mv && atoi(mv) == 5 && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        // =5: the actor's own position (+0x2b4) set to the owner's every tick
        memcpy((void *)(uintptr_t)(ma[0] + COOP_ACTOR_POS), owner[0], 12);
        static int said5;
        if (!said5++) rt_log("coop: test: move 5: the co-op Link's actor position follows its owner");
    } else if (mv && atoi(mv) == 4 && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        // =4: the 4x4 row-vector draw matrices at [[comp table +0x8] +0x28] +0x5b0 (127, 0x40 each; translation in
        // the last row, from a world origin), shifted by the owner's movement since the first tick (snapshot kept).
        static uint8_t snap4[127 * 0x40];
        static float base[3];
        static uint64_t arr4;
        uint64_t arr = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(ma[0] + 0x228) + 0x8) + 0x28);
        if (arr && arr4 != arr + 0x5b0) {
            arr4 = arr + 0x5b0;
            coop_rd(arr4, snap4, sizeof snap4);
            coop_rd(ma[0] + COOP_ACTOR_POS, base, 12);
            rt_log("coop: test: move 4: draw matrices %llx taken at %.2f %.2f %.2f", (unsigned long long)arr4, base[0], base[1], base[2]);
        }
        if (arr4) {
            static uint8_t out[127 * 0x40];
            memcpy(out, snap4, sizeof out);
            for (int i = 0; i < 127; i++) {
                float *m = (float *)(out + i * 0x40);
                for (int k = 0; k < 3; k++) m[12 + k] += owner[0][k] - base[k];
            }
            memcpy((void *)(uintptr_t)arr4, out, sizeof out);
        }
    } else if (mv && coop_player_actors(ma, mi, 1) == 1 && coop_player_positions(owner, 1) == 1) {
        int which = atoi(mv);
        uint64_t n1 = coop_rd_ptr(ma[0] + 0x48), n2 = n1 ? coop_rd_ptr(n1 + 0x2a8) : 0;
        uint64_t mats[2] = {(which & 1) && n1 ? n1 + 0xa34 : 0, (which & 2) && n2 ? n2 + 0x26c : 0};
        for (int i = 0; i < 2; i++)
            if (mats[i]) ST32(mats[i] + 0xc, *(uint32_t *)&owner[0][0]), ST32(mats[i] + 0x1c, *(uint32_t *)&owner[0][1]), ST32(mats[i] + 0x2c, *(uint32_t *)&owner[0][2]);
        static int said;
        if (!said++) rt_log("coop: test: moving the co-op Link's matrices %llx %llx", (unsigned long long)mats[0], (unsigned long long)mats[1]);
    }
    // Test (TOTK_COOP_TEST_SKELCOPY=1): every tick, this Link's body skeleton (the g3d SkeletonObj at actor +0x48,
    // +0x2a8, +0x20, +0x20, +0, +0xf8, +0x58, +0xb0: 127 bones) copied onto the first co-op Link's: its world matrices
    // (both buffers, +0x18 and +0x58) and local ones (+0x20). With TOTK_COOP_TEST_FREEZE: a pose shown without the
    // co-op Link's update running.
    // The pose copy itself runs right after the co-op Link's animation step (coop_pose_after_anim, from coop_spawn.c's
    // hook on main+0x2e9c80); here the pointers it needs are cached once a tick.
    if (cfg_get("TOTK_COOP_TEST_SKELCOPY")) {
        uint64_t ol[1];
        int oi[1];
        if (coop_player_actors(ol, oi, 1) == 1) {
            uint64_t s0 = me, s1 = ol[0];
            for (int i = 0; i < 8 && s0 && s1; i++) s0 = coop_rd_ptr(s0 + skel_path[i]), s1 = coop_rd_ptr(s1 + skel_path[i]);
            uint16_t n0 = 0, n1 = 0;
            int ok = s0 && s1 && is_skeleton(s0) && is_skeleton(s1) && coop_rd(s0 + 0x48, &n0, 2) && coop_rd(s1 + 0x48, &n1, 2) && n0 == n1;
            atomic_store(&pose_src, ok ? s0 : 0), atomic_store(&pose_dst, ok ? s1 : 0), atomic_store(&pose_bones, ok ? n0 : 0);
            atomic_store(&pose_comp, coop_rd_ptr(coop_rd_ptr(ol[0] + 0x228) + 0x8));
            find_pose_binding(ol[0]);
            static int said;
            if (ok && !said++) rt_log("coop: test: skeleton copy %llx -> %llx, %u bones, after the animation of %llx",
                                      (unsigned long long)s0, (unsigned long long)s1, n0, (unsigned long long)atomic_load(&pose_comp));
            // TOTK_COOP_TEST_SKELCOPY=tick: the old way, copied here (for comparison)
            const char *mode = cfg_get("TOTK_COOP_TEST_SKELCOPY");
            if (ok && !strcmp(mode, "tick")) coop_pose_copy_now();
        }
    }
    // Test (TOTK_COOP_TEST_POSECOPY=<offset>,<count>,<stride>): every tick, the matrices at [[comp table +0x8] +0x28]
    // + offset (the bone scan's candidate) copied from this Link to the first co-op Link, raw.
    const char *pc = cfg_get("TOTK_COOP_TEST_POSECOPY");
    uint64_t other[1];
    int oid[1];
    if (pc && coop_player_actors(other, oid, 1) == 1) {
        unsigned off = (unsigned)strtoul(pc, NULL, 0), cnt = 127, str = 0x40;
        const char *q = strchr(pc, ',');
        if (q) cnt = (unsigned)strtoul(q + 1, NULL, 0), q = strchr(q + 1, ',');
        if (q) str = (unsigned)strtoul(q + 1, NULL, 0);
        uint64_t src = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x8) + 0x28);
        uint64_t dst = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(other[0] + 0x228) + 0x8) + 0x28);
        // TOTK_COOP_TEST_POSEPATH=<o1>,<o2>...: more pointer steps from there before the offset
        for (const char *pp = cfg_get("TOTK_COOP_TEST_POSEPATH"); pp && *pp && src && dst;) {
            unsigned o = (unsigned)strtoul(pp, NULL, 0);
            src = coop_rd_ptr(src + o), dst = coop_rd_ptr(dst + o);
            pp = strchr(pp, ',');
            if (pp) pp++;
        }
        static int said;
        if (src && dst) {
            static uint8_t tmp[0x8000];
            size_t len = (size_t)cnt * str;
            if (len <= sizeof tmp && coop_rd(src + off, tmp, len)) {
                if (!said++) {
                    float *m = (float *)tmp;
                    rt_log("coop: test: pose copy %#x x%u from %llx to %llx; first matrix %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f",
                           off, cnt, (unsigned long long)src, (unsigned long long)dst, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                           m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                }
                memcpy((void *)(uintptr_t)(dst + off), tmp, len);
            }
        }
    }
    // Test (TOTK_COOP_TEST_FOLLOW=lo-hi): after the scan, the co-op Link's position copies lo..hi (indices) take its
    // owner's position every tick (with TOTK_COOP_TEST_FREEZE: which copy the frozen model is drawn at).
    const char *fl = cfg_get("TOTK_COOP_TEST_FOLLOW");
    float want[1][3];
    if (fl && ncand && coop_player_positions(want, 1) == 1) {
        int lo = atoi(fl), hi = strchr(fl, '-') ? atoi(strchr(fl, '-') + 1) : ncand - 1;
        static int said;
        if (!said++) rt_log("coop: test: copies %d..%d of %d follow the owner", lo, hi, ncand);
        for (int i = lo; i <= hi && i < ncand; i++) {
            if (cand_col[i]) ST32(cand[i], *(uint32_t *)&want[0][0]), ST32(cand[i] + 16, *(uint32_t *)&want[0][1]), ST32(cand[i] + 32, *(uint32_t *)&want[0][2]);
            else memcpy((void *)(uintptr_t)cand[i], want[0], 12);
        }
    }
    static int ton = -1, tt;
    if (ton < 0) { const char *e = getenv("TOTK_COOP_TRACE"); ton = e && strstr(e, "target"); }
    if (ton && ++tt % 60 == 0) {
        int ids[COOP_MAX_PLAYERS];
        tp_links[0] = me;
        coop_resident_named("Player", &tp_refs[0]);
        tp_n = 1 + coop_player_actors(tp_links + 1, ids, COOP_MAX_PLAYERS);
        for (int i = 1; i < tp_n; i++) tp_refs[i] = 0;
        for (int i = 0; i < tp_n; i++) tp_ids[i] = perception_id(tp_links[i]);
        static int said;
        if (!said++ || tt % 600 == 0) rt_log("coop: perception id of this Link %#x%s", tp_ids[0], tp_n > 1 ? ", of co-op Links:" : "");
        for (int i = 1; i < tp_n && tt % 600 == 0; i++) rt_log("coop:   co-op Link %d: %#x", i, tp_ids[i]);
        // Test (TOTK_COOP_TEST_RESIDENT): the co-op Links' perception flags (data +0x14, +0x1c and the second record's
        // +0x27c, +0x284) made this Link's, to see whether enemies then take them as targets.
        if (cfg_get("TOTK_COOP_TEST_RESIDENT")) {
            uint64_t d0 = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(tp_links[0] + 0x228) + 0x378) + 0x30);
            for (int i = 1; i < tp_n && d0; i++) {
                uint64_t d1 = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(tp_links[i] + 0x228) + 0x378) + 0x30);
                static const int offs[4] = {0x14, 0x1c, 0x27c, 0x284};
                for (int k = 0; d1 && k < 4; k++) {
                    uint32_t v = 0, old = 0;
                    coop_rd(d0 + offs[k], &v, 4), coop_rd(d1 + offs[k], &old, 4);
                    if (v != old) ST32(d1 + offs[k], v), rt_log("coop: test: co-op Link %d perception +%#x %#x -> %#x", i, offs[k], old, v);
                }
            }
        }
        // The registry main+0x72b4e8 takes "the player" from: [main+0x3a75a40], count +0x38, sorted pointers +0x40 to
        // {u32 key, +8 object}; key 0x5d5d398d, then object +0x150. What that object and its +0x150 are. Once, at 20 s.
        static int reg_said;
        if (!reg_said && tt > 600) {
            reg_said = 1;
            uint64_t r = coop_rd_ptr(MOD_MAIN_BASE + 0x3a75a40), arr = r ? coop_rd_ptr(r + 0x40) : 0;
            uint32_t n = 0;
            if (r) coop_rd(r + 0x38, &n, 4);
            rt_log("coop: registry %llx: %u entries, +0x20 %llx -> +0x150 %llx", (unsigned long long)r, n,
                   (unsigned long long)coop_rd_ptr(r + 0x20), (unsigned long long)coop_rd_ptr(coop_rd_ptr(r + 0x20) + 0x150));
            for (uint32_t i = 0; arr && i < n && i < 4096; i++) {
                uint64_t e = coop_rd_ptr(arr + i * 8);
                uint32_t key = 0;
                if (!e || !coop_rd(e, &key, 4) || key != 0x5d5d398du) continue;
                uint64_t o = coop_rd_ptr(e + 8), p = o ? coop_rd_ptr(o + 0x150) : 0;
                char on[64] = "", pn[64] = "";
                coop_actor_name(o, on, sizeof on), coop_actor_name(p, pn, sizeof pn);
                rt_log("coop: registry player entry %u: %llx object %llx (%s) +0x150 %llx (%s); this Link %llx, its ref %llx",
                       i, (unsigned long long)e, (unsigned long long)o, on, (unsigned long long)p, pn,
                       (unsigned long long)tp_links[0], (unsigned long long)tp_refs[0]);
                const char *w = link_of(o) ? link_of(o) : link_of(p);
                rt_log("coop: registry player entry is %s", w ? w : "neither Link nor its ref");
                // Words of both objects (0x1000) and of what they point to (0x400) equal to a Link, its ref, or its
                // perception component or data.
                uint64_t pc = coop_rd_ptr(coop_rd_ptr(tp_links[0] + 0x228) + 0x378), pdat = pc ? coop_rd_ptr(pc + 0x30) : 0;
                uint64_t roots[2] = {o, p};
                for (int k = 0; k < 2; k++)
                    for (int j = 0; roots[k] && j < 0x1000 / 8; j++) {
                        uint64_t v = coop_rd_ptr(roots[k] + j * 8);
                        const char *s = link_of(v) ? link_of(v) : v && v == pc ? "this Link's perception component" : v && v == pdat ? "this Link's perception data" : NULL;
                        if (s) rt_log("coop:   %s +%#x = %s", k ? "+0x150 object" : "object", j * 8, s);
                        if (v < 0x100000000ull || v >= 0x8000000000ull) continue;
                        for (int i2 = 0; i2 < 0x400 / 8; i2++) {
                            uint64_t w2 = coop_rd_ptr(v + i2 * 8);
                            const char *s2 = link_of(w2) ? link_of(w2) : w2 && w2 == pc ? "this Link's perception component" : w2 && w2 == pdat ? "this Link's perception data" : NULL;
                            if (s2) rt_log("coop:   [%s +%#x]+%#x = %s", k ? "+0x150 object" : "object", j * 8, i2 * 8, s2);
                        }
                    }
            }
        }
        // Where the game keeps either Link's perception component or data: words of main's data (.data to .bss) and
        // of the objects they point to (first 0x800 bytes) equal to one of them. Once, after 20 s.
        static int scanned;
        if (tp_n > 1 && !scanned && tt > 600) {
            scanned = 1;
            uint64_t want[4];
            for (int i = 0; i < 2; i++) {
                want[i * 2] = coop_rd_ptr(coop_rd_ptr(tp_links[i] + 0x228) + 0x378);
                want[i * 2 + 1] = want[i * 2] ? coop_rd_ptr(want[i * 2] + 0x30) : 0;
            }
            static const char *const what[4] = {"this Link's component", "this Link's data", "co-op Link's component", "co-op Link's data"};
            int found = 0;
            static uint64_t obj[0x800 / 8];
            for (uint64_t g = MOD_MAIN_BASE + 0x357dee8; g < MOD_MAIN_BASE + 0x3adefd0 && found < 40; g += 8) {
                uint64_t v = coop_rd_ptr(g);
                for (int k = 0; k < 4; k++)
                    if (v && v == want[k]) rt_log("coop: perception scan: main+%#llx = %s", (unsigned long long)(g - MOD_MAIN_BASE), what[k]), found++;
                if (v < 0x100000000ull || v >= 0x8000000000ull || !coop_rd(v, obj, sizeof obj)) continue;
                for (int j = 0; j < 0x800 / 8; j++)
                    for (int k = 0; k < 4; k++)
                        if (obj[j] && obj[j] == want[k]) {
                            rt_log("coop: perception scan: [main+%#llx]+%#x = %s", (unsigned long long)(g - MOD_MAIN_BASE), j * 8, what[k]);
                            found++;
                        }
            }
            rt_log("coop: perception scan: %d places", found);
            for (uint64_t g = MOD_MAIN_BASE + 0x357dee8; g < MOD_MAIN_BASE + 0x3adefd0; g += 8) {   // who points at the objects
                uint64_t v = coop_rd_ptr(g);
                if (v == MOD_MAIN_BASE + 0x3ad9830 || v == MOD_MAIN_BASE + 0x3a6ea80 || v == MOD_MAIN_BASE + 0x3a4e6a8 ||
                    v == MOD_MAIN_BASE + 0x3a4e6e0 || v == MOD_MAIN_BASE + 0x3a83a30)
                    rt_log("coop: perception scan: main+%#llx holds &main+%#llx", (unsigned long long)(g - MOD_MAIN_BASE),
                           (unsigned long long)(v - MOD_MAIN_BASE));
            }
        }
        // Test (TOTK_COOP_TEST_SLOT=1..5): one of those five places made to hold the co-op Link's data from 20 s on.
        const char *slot = cfg_get("TOTK_COOP_TEST_SLOT");
        if (slot && tp_n > 1 && tt > 600) {
            static const uint32_t g_off[5][2] = {{0x3a4e6a8, 0x98}, {0x3a4e6e0, 0x370}, {0x3a6ea80, 0x58}, {0x3a83a30, 0x3b0}, {0x3ad9830, 0x250}};
            int k = atoi(slot) - 1;
            uint64_t obj = k >= 0 && k < 5 ? coop_rd_ptr(MOD_MAIN_BASE + g_off[k][0]) : 0;
            uint64_t d1 = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(tp_links[1] + 0x228) + 0x378) + 0x30);
            if (obj && d1 && coop_rd_ptr(obj + g_off[k][1]) != d1) {
                rt_log("coop: test: [main+%#x]+%#x = the co-op Link's perception data", g_off[k][0], g_off[k][1]);
                ST64(obj + g_off[k][1], d1);
            }
        }
        // Both Links' perception components ([actor+0x228] +0x378) and the object at their +0x30, side by side.
        static int dumped;
        if (tp_n > 1 && !dumped && tt > 600) {
            dumped = 1;
            for (int lvl = 0; lvl < 2; lvl++) {
                uint64_t c0 = coop_rd_ptr(coop_rd_ptr(tp_links[0] + 0x228) + 0x378), c1 = coop_rd_ptr(coop_rd_ptr(tp_links[1] + 0x228) + 0x378);
                if (lvl) c0 = coop_rd_ptr(c0 + 0x30), c1 = coop_rd_ptr(c1 + 0x30);
                uint64_t a0[0x300 / 8] = {0}, a1[0x300 / 8] = {0};
                coop_rd(c0, a0, sizeof a0), coop_rd(c1, a1, sizeof a1);
                rt_log("coop: perception %s: this Link's %llx, co-op Link's %llx", lvl ? "data (+0x30)" : "component", (unsigned long long)c0, (unsigned long long)c1);
                for (int j = 0; j < 0x300 / 8; j++) {
                    int ptr0 = a0[j] >= 0x100000000ull && a0[j] < 0x8000000000ull, ptr1 = a1[j] >= 0x100000000ull && a1[j] < 0x8000000000ull;
                    if (a0[j] != a1[j] && !(ptr0 && ptr1))
                        rt_log("coop:   +%#05x  %016llx  %016llx", j * 8, (unsigned long long)a0[j], (unsigned long long)a1[j]);
                }
            }
        }
        each_live(target_probe_one, NULL);
    }
    static int on = -1, t;
    if (on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); on = e && strstr(e, "enemy"); }
    if (on && ++t % 300 == 0) { probe_me = me; each_live(probe_one, NULL); }
    int role = coop_world_role();
    if (role == 1) host_tick(c, me);
    else if (role == 2) guest_tick(c);
}
