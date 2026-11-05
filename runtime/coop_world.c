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

void coop_world_tick(Cpu *c, uint64_t me) {
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
