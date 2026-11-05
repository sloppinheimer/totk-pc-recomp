// Co-op frame tracer (TOTK_COOP_TRACE_AT): for a short window (default 70 ms, two frames at 30 fps) every guest
// function entered with x0..x3 pointing into an object of a Link (this PC's and each co-op Link's) is recorded with
// its thread, time, caller and stack depth, and the Link data that matters (position, rotation, model world matrix,
// skeleton buffers) is checked at each recorded call so a change shows up between two calls. The objects are found
// by walking pointers from each actor (3 levels) plus the known paths (components, model, body skeleton) and every
// skeleton below the actor. Written as a CSV next to the log; tools/coop/trace_report.py turns it into a report.
// Test code: costs nothing outside the window (one branch on g_fnprof per call).

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "cpu.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c

#define MAX_LINKS (1 + COOP_MAX_PLAYERS)
#define MAX_NODES 300000
#define PER_LINK 60000
#define MAX_REC (2u << 20)
#define MAX_WATCH 64

typedef struct {
    uint64_t addr;
    uint32_t size, parent;   // parent: node index, ~0 a root
    uint32_t off;            // where the parent holds it
    uint8_t link, depth, shared, kind;   // kind: 0 plain, 1 named root, 2 actor (a name at +0x218)
    char name[40];
} Node;
typedef struct {
    uint64_t t, fn, lr, sp;
    uint32_t node, off, tid;
    uint8_t arg, mask, type, link;   // type 0 call, 1 a watched value changed (node = watch index)
    float v[3];
} Rec;
typedef struct { uint64_t addr; uint32_t len; uint8_t link; char what[27]; } Watch;

static Node *nodes;
static uint32_t nnodes;
static uint64_t *order;   // (addr, index) pairs sorted by addr for the lookup
static Rec *recs;
static _Atomic uint32_t nrec;
static _Atomic uint64_t dropped;
static Watch watches[MAX_WATCH];
static _Atomic uint64_t watch_hash[MAX_WATCH];
static int nwatch;
static _Atomic int tracing;
static uint64_t t_end, t_start;
static int trace_no;

// Objects main's writable data points at (singletons, managers): never part of a Link's map.
static uint64_t *globals;
static size_t nglobals;
static int cmp_u64(const void *a, const void *b);
static void collect_globals(void) {
    uint64_t lo = MOD_MAIN_BASE + 0x357dee8, hi = MOD_MAIN_BASE + 0x3adefd0;   // main's .data/.bss (coop_spawn.c p3_report)
    if (!globals) globals = malloc((hi - lo) / 8 * sizeof *globals);
    nglobals = 0;
    static uint64_t buf[0x10000 / 8];
    for (uint64_t a = lo; globals && a < hi; a += sizeof buf) {
        uint64_t len = hi - a < sizeof buf ? hi - a : sizeof buf;
        if (!coop_rd(a, buf, len)) continue;
        for (uint64_t i = 0; i < len / 8; i++)
            if (buf[i] >= 0x100000000ull && buf[i] < 0x8000000000ull) globals[nglobals++] = buf[i];
    }
    if (globals) qsort(globals, nglobals, 8, cmp_u64);
}
static int is_global(uint64_t q) {
    return globals && bsearch(&q, globals, nglobals, 8, cmp_u64) != NULL;
}
static int readable_ptr(uint64_t q) {
    return q >= 0x100000000ull && q < 0x8000000000ull && !(q >= MOD_MAIN_BASE && q < MOD_MAIN_BASE + 0x4000000);
}

// ---- the object map
static uint64_t *seen_set;   // open addressing, per link
#define SEEN_BITS 20
static int first_seen(uint64_t a) {
    uint32_t h = (uint32_t)((a >> 3) * 0x9E3779B97F4A7C15ull >> (64 - SEEN_BITS));
    for (;;) {
        if (seen_set[h] == a) return 0;
        if (!seen_set[h]) { seen_set[h] = a; return 1; }
        h = (h + 1) & ((1u << SEEN_BITS) - 1);
    }
}
static uint32_t add_node(uint64_t a, uint32_t size, uint32_t parent, uint32_t off, int link, int depth, int kind, const char *name) {
    if (nnodes >= MAX_NODES) return ~0u;
    Node *n = &nodes[nnodes];
    memset(n, 0, sizeof *n);
    n->addr = a, n->size = size, n->parent = parent, n->off = off, n->link = (uint8_t)link, n->depth = (uint8_t)depth, n->kind = (uint8_t)kind;
    if (name) snprintf(n->name, sizeof n->name, "%s", name);
    return nnodes++;
}
static int actor_name(uint64_t a, char *out, int max) {
    uint64_t p = coop_rd_ptr(a + 0x218);
    char s[40] = {0};
    if (!readable_ptr(p) || !coop_rd(p, s, sizeof s - 1)) return 0;
    int n = 0;
    while (s[n] && n < 38 && ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_')) n++;
    if (n < 4 || s[n] != 0 || s[0] < 'A' || s[0] > 'Z' || !strchr(s, '_') ) return 0;
    snprintf(out, max, "%s", s);
    return 1;
}
static int is_skeleton(uint64_t s) {
    uint64_t w[0xd];
    uint16_t n = 0;
    if (!coop_rd(s, w, sizeof w) || !coop_rd(s + 0x48, &n, 2)) return 0;
    return n >= 20 && n <= 600 && w[4] && w[4] == w[0xc] && (w[3] - w[4] == (uint64_t)n * 0x40 || w[0xb] - w[4] == (uint64_t)n * 0x40);
}
static void add_watch(int link, uint64_t a, uint32_t len, const char *what) {
    uint8_t probe[0x80];
    if (!a || nwatch >= MAX_WATCH || len > sizeof probe || !coop_rd(a, probe, len)) return;
    Watch *w = &watches[nwatch++];
    w->addr = a, w->len = len, w->link = (uint8_t)link;
    snprintf(w->what, sizeof w->what, "%s", what);
}
// The skeleton's own buffers as named nodes, and its first world record watched.
static int nskel[MAX_LINKS];
static void add_skeleton(int link, uint64_t s, uint32_t parent, uint32_t off, const char *how) {
    uint16_t n = 0;
    coop_rd(s + 0x48, &n, 2);
    char nm[40];
    snprintf(nm, sizeof nm, "skeleton%d(%u bones)%s", nskel[link], n, how);
    uint32_t i = add_node(s, 0x100, parent, off, link, 0, 1, nm);
    static const struct { uint32_t at, per; const char *what; } bufs[] = {{0x18, 0x60, "world"}, {0x20, 0x40, "local"}, {0x58, 0x60, "world2"}, {0x60, 0x40, "local2"}};
    for (int k = 0; k < 4; k++) {
        uint64_t b = coop_rd_ptr(s + bufs[k].at);
        if (!readable_ptr(b) || !first_seen(b)) continue;
        char bn[40];
        snprintf(bn, sizeof bn, "skel%d.%s[%u]", nskel[link], bufs[k].what, n);
        add_node(b, (uint32_t)n * bufs[k].per, i, bufs[k].at, link, 0, 1, bn);
        if (k == 0) {
            char wn[27];
            snprintf(wn, sizeof wn, "skel%d bone0", nskel[link]);
            add_watch(link, b, 0x60, wn);
            snprintf(wn, sizeof wn, "skel%d bone%u", nskel[link], n / 2);
            add_watch(link, b + (uint64_t)(n / 2) * 0x60, 0x60, wn);
        }
    }
    nskel[link]++;
}
static void find_skeletons(int link, uint64_t obj, int depth, uint32_t parent, uint32_t off, const char *owner) {
    if (depth > 7) return;
    uint64_t buf[0x400 / 8];
    if (!coop_rd(obj, buf, sizeof buf)) return;
    for (int i = 0; i < 0x400 / 8; i++) {
        uint64_t q = buf[i];
        if (!readable_ptr(q) || !first_seen(q)) continue;
        if (is_skeleton(q)) {
            char how[24];
            snprintf(how, sizeof how, " %.20s", owner);
            add_skeleton(link, q, parent, (uint32_t)(off << 16 | i * 8), how);
            continue;
        }
        char an[40];
        find_skeletons(link, q, depth + 1, parent, off, actor_name(q, an, sizeof an) ? an : owner);
    }
}
static const uint32_t skel_path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};
static void map_link(int link, uint64_t actor) {
    memset(seen_set, 0, sizeof(uint64_t) << SEEN_BITS);
    uint32_t first = nnodes;
    char an[40] = "actor";
    actor_name(actor, an, sizeof an);
    uint32_t root = add_node(actor, 0x4000, ~0u, 0, link, 0, 1, an);
    first_seen(actor);
    // Named: the component table and its entries, the model object, the body skeleton's path.
    uint64_t tab = coop_rd_ptr(actor + 0x228);
    if (readable_ptr(tab) && first_seen(tab)) {
        uint32_t ti = add_node(tab, 0x800, root, 0x228, link, 1, 1, "components");
        for (int j = 0; j < 0x100; j++) {
            uint64_t comp = coop_rd_ptr(tab + j * 8);
            if (!readable_ptr(comp) || !first_seen(comp)) continue;
            char cn[40];
            snprintf(cn, sizeof cn, "comp+%#x", j * 8);
            uint32_t ci = add_node(comp, 0x400, ti, j * 8, link, 1, 1, cn);
            if (j == 1) {
                uint64_t model = coop_rd_ptr(comp + 0x28);
                if (readable_ptr(model) && first_seen(model)) {
                    add_node(model, 0x400, ci, 0x28, link, 1, 1, "model");
                    add_watch(link, model + 0x1f8, 0x30, "model matrix");
                    add_watch(link, model + 0x240, 4, "model dirty");
                }
            }
        }
    }
    add_watch(link, actor + COOP_ACTOR_POS, 12, "actor pos");
    add_watch(link, actor + COOP_ACTOR_ROT, 36, "actor rot");
    uint64_t s = actor, body = 0;
    uint32_t prev = root;
    for (int k = 0; k < 8 && s; k++) {
        uint64_t nx = coop_rd_ptr(s + skel_path[k]);
        if (!readable_ptr(nx)) break;
        if (k == 7 && is_skeleton(nx)) { first_seen(nx); body = nx; add_skeleton(link, nx, prev, skel_path[k], " body"); break; }
        char pn[40];
        snprintf(pn, sizeof pn, "skelpath%d", k + 1);
        if (first_seen(nx)) prev = add_node(nx, 0x400, prev, skel_path[k], link, 1, 1, pn);
        s = nx;
    }
    // Every other skeleton below the actor (head, hair, held items), then a plain 3-level walk of everything.
    memset(seen_set, 0, sizeof(uint64_t) << SEEN_BITS);   // its own walk: through the model and components too
    first_seen(actor);
    if (body) first_seen(body);
    find_skeletons(link, actor, 0, root, 0, "own");
    memset(seen_set, 0, sizeof(uint64_t) << SEEN_BITS);
    for (uint32_t i = first; i < nnodes; i++) first_seen(nodes[i].addr);
    for (uint32_t i = first; i < nnodes && nnodes - first < PER_LINK; i++) {
        Node n = nodes[i];
        if (n.depth >= 3 || n.kind == 2 || (n.kind == 1 && !strncmp(n.name, "skelpath", 8)) || (n.kind == 1 && strstr(n.name, "skel") && strchr(n.name, '['))) continue;   // not into the bone arrays
        uint32_t scan = n.size > 0x4000 ? 0x4000 : n.size;
        static uint64_t buf[0x4000 / 8];
        if (!coop_rd(n.addr, buf, scan)) continue;
        for (uint32_t o = 0; o < scan / 8 && nnodes - first < PER_LINK; o++) {
            uint64_t q = buf[o];
            if (!readable_ptr(q) || !first_seen(q)) continue;
            char nm[40] = "";
            int act = actor_name(q, nm, sizeof nm);
            if (!act && is_global(q)) snprintf(nm, sizeof nm, "global-held");   // a singleton: labelled, still walked
            uint8_t probe[8];
            if (!act && !coop_rd(q, probe, 8)) continue;
            add_node(q, act ? 0x4000 : 0x400, i, o * 8, link, n.depth + 1, act ? 2 : nm[0] ? 3 : 0, act || nm[0] ? nm : NULL);
        }
    }
    rt_log("coop: trace: %s Link %llx: %u objects, %d skeletons", link ? "co-op" : "this", (unsigned long long)actor, nnodes - first, nskel[link]);
}
static int cmp_order(const void *a, const void *b) {
    const uint64_t *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}
static int build_map(uint64_t me) {
    if (!nodes) {
        nodes = malloc(sizeof *nodes * MAX_NODES), order = malloc(sizeof *order * 2 * MAX_NODES);
        seen_set = malloc(sizeof(uint64_t) << SEEN_BITS), recs = malloc(sizeof *recs * MAX_REC);
        if (!nodes || !order || !seen_set || !recs) { rt_log("coop: trace: out of memory"); return 0; }
    }
    nnodes = 0, nwatch = 0;
    collect_globals();
    memset(nskel, 0, sizeof nskel);
    uint64_t acts[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(acts, ids, COOP_MAX_PLAYERS);
    map_link(0, me);
    for (int i = 0; i < n && i + 1 < MAX_LINKS; i++) map_link(i + 1, acts[i]);
    for (uint32_t i = 0; i < nnodes; i++) order[2 * i] = nodes[i].addr, order[2 * i + 1] = i;
    qsort(order, nnodes, 16, cmp_order);
    // An object reached from two Links is shared (world systems): marked, and calls on it are not a Link's.
    int nshared = 0;
    for (uint32_t i = 1; i < nnodes; i++)
        if (order[2 * i] == order[2 * i - 2] && nodes[order[2 * i + 1]].link != nodes[order[2 * i - 1]].link)
            nodes[order[2 * i + 1]].shared = nodes[order[2 * i - 1]].shared = 1, nshared++;
    for (int w = 0; w < nwatch; w++) atomic_store(&watch_hash[w], 0);
    rt_log("coop: trace: %u objects in all, %d shared, %d watched values, %d co-op Link(s)", nnodes, nshared, nwatch, n);
    return 1;
}
// The object x points into: the nearest start at or below it whose size covers it (a few candidates back).
static uint32_t lookup(uint64_t x, uint32_t *off) {
    if (x < 0x100000000ull) return ~0u;
    uint32_t lo = 0, hi = nnodes;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (order[2 * m] <= x) lo = m + 1; else hi = m; }
    for (int k = 0; k < 6 && lo > 0; k++, lo--) {
        const Node *n = &nodes[order[2 * (lo - 1) + 1]];
        if (x - n->addr < n->size) { *off = (uint32_t)(x - n->addr); return (uint32_t)order[2 * (lo - 1) + 1]; }
    }
    return ~0u;
}

// ---- the window (cpu.h FN_ENTER -> main.c fnprof_hit, g_fnprof on)
static uint64_t hash_of(const Watch *w, float v[3]) {
    uint8_t b[0x80];
    memcpy(b, (const void *)(uintptr_t)w->addr, w->len);   // checked readable when the map was built
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < w->len; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    if (w->len == 0x60) memcpy(&v[0], b + 0x2c, 4), memcpy(&v[1], b + 0x3c, 4), memcpy(&v[2], b + 0x4c, 4);   // a bone record's
    else if (w->len >= 0x30) memcpy(&v[0], b + 0xc, 4), memcpy(&v[1], b + 0x1c, 4), memcpy(&v[2], b + 0x2c, 4);   // a 3x4's translation
    else memcpy(v, b, w->len >= 12 ? 12 : w->len);
    return h | 1;
}
void coop_trace_hit(uint64_t addr, Cpu *c) {
    if (!atomic_load_explicit(&tracing, memory_order_relaxed)) return;
    uint64_t now = plat_counter();
    if (now > t_end) { atomic_store(&tracing, 0); return; }
    uint32_t node = ~0u, off = 0;
    int arg = -1, mask = 0;
    for (int a = 0; a < 4; a++) {
        uint32_t o, n = lookup(c->x[a], &o);
        if (n == ~0u) continue;
        mask |= 1 << a;
        if (arg < 0 || (nodes[node].shared && !nodes[n].shared)) node = n, off = o, arg = a;
    }
    if (arg < 0) return;
    uint32_t tid = plat_thread_id();
    for (int w = 0; w < nwatch; w++) {
        float v[3] = {0};
        uint64_t h = hash_of(&watches[w], v), was = atomic_exchange_explicit(&watch_hash[w], h, memory_order_relaxed);
        if (was == h) continue;
        uint32_t i = atomic_fetch_add_explicit(&nrec, 1, memory_order_relaxed);
        if (i >= MAX_REC) { atomic_fetch_add(&dropped, 1); continue; }
        Rec *r = &recs[i];
        r->t = now, r->fn = addr, r->lr = c->x[30], r->sp = c->sp, r->node = (uint32_t)w, r->off = 0, r->tid = tid;
        r->arg = 0, r->mask = 0, r->type = 1, r->link = watches[w].link;
        memcpy(r->v, v, sizeof v);
    }
    uint32_t i = atomic_fetch_add_explicit(&nrec, 1, memory_order_relaxed);
    if (i >= MAX_REC) { atomic_fetch_add(&dropped, 1); return; }
    Rec *r = &recs[i];
    r->t = now, r->fn = addr, r->lr = c->x[30], r->sp = c->sp, r->node = node, r->off = off, r->tid = tid;
    r->arg = (uint8_t)arg, r->mask = (uint8_t)mask, r->type = 0, r->link = nodes[node].link;
}

// ---- the file
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static void write_csv(double at) {
    char path[600];
    const char *log = rt_log_path();
    if (!log) { rt_log("coop: trace: no log directory"); return; }
    snprintf(path, sizeof path, "%s", log);
    char *cut = strrchr(path, '/'), *cut2 = strrchr(path, '\\');
    if (cut2 > cut) cut = cut2;
    if (!cut) return;
    snprintf(cut + 1, sizeof path - (cut + 1 - path), "trace_%d_%.0fs_%u.csv", trace_no, at, plat_thread_id());
    FILE *f = fopen(path, "w");
    if (!f) { rt_log("coop: trace: cannot write %s", path); return; }
    uint32_t n = atomic_load(&nrec);
    if (n > MAX_REC) n = MAX_REC;
    double hz = (double)plat_counter_hz();
    fprintf(f, "# coop trace at %.2f s world time, %.1f ms, %u records, %llu dropped\n", at, (t_end - t_start) * 1e3 / hz, n,
            (unsigned long long)atomic_load(&dropped));
    // Only the objects a record names, and their parents (for the paths).
    uint8_t *want = calloc(nnodes, 1);
    for (uint32_t i = 0; i < n; i++)
        if (recs[i].type == 0)
            for (uint32_t k = recs[i].node; k != ~0u && k < nnodes && !want[k]; k = nodes[k].parent) want[k] = 1;
    for (uint32_t i = 0; i < nnodes; i++)
        if (want && want[i])
            fprintf(f, "N,%u,%llx,%#x,%d,%d,%d,%#x,%d,%s\n", i, (unsigned long long)nodes[i].addr, nodes[i].size, nodes[i].link,
                    nodes[i].parent == ~0u ? -1 : (int)nodes[i].parent, nodes[i].shared, nodes[i].off, nodes[i].kind, nodes[i].name);
    free(want);
    for (int w = 0; w < nwatch; w++)
        fprintf(f, "W,%d,%d,%llx,%u,%s\n", w, watches[w].link, (unsigned long long)watches[w].addr, watches[w].len, watches[w].what);
    // Function names, once each.
    uint64_t *fns = malloc(sizeof *fns * 2 * (n + 1));
    uint32_t nf = 0;
    for (uint32_t i = 0; fns && i < n; i++) fns[nf++] = recs[i].fn, fns[nf++] = recs[i].lr;
    if (fns) qsort(fns, nf, 8, cmp_u64);
    char d[200];
    for (uint32_t i = 0; fns && i < nf; i++)
        if (!i || fns[i] != fns[i - 1]) fprintf(f, "F,%llx,%s\n", (unsigned long long)fns[i], ldr_describe(fns[i], d, sizeof d));
    free(fns);
    for (uint32_t i = 0; i < n; i++) {
        const Rec *r = &recs[i];
        double us = ((double)r->t - (double)t_start) * 1e6 / hz;
        if (r->type == 0)
            fprintf(f, "C,%.1f,%u,%llx,%llx,%llx,%u,%#x,%d,%d\n", us, r->tid, (unsigned long long)r->fn, (unsigned long long)r->lr,
                    (unsigned long long)r->sp, r->node, r->off, r->arg, r->mask);
        else
            fprintf(f, "V,%.1f,%u,%llx,%llx,%llx,%u,%g,%g,%g\n", us, r->tid, (unsigned long long)r->fn, (unsigned long long)r->lr,
                    (unsigned long long)r->sp, r->node, r->v[0], r->v[1], r->v[2]);
    }
    fclose(f);
    rt_log("coop: trace %d written: %s (%u records)", trace_no, path, n);
}

// From coop_tick (30 Hz, the game thread polling the gamepad): TOTK_COOP_TRACE_AT=20,35,50 traces at those world
// seconds, TOTK_COOP_TRACE_MS long each (default 70).
void coop_trace_tick(uint64_t me) {
    static int init, pending, next;
    static double ats[16], cur_at;
    static int nats;
    static double ms = 70;
    if (!init) {
        init = 1;
        const char *e = cfg_get("TOTK_COOP_TRACE_AT"), *m = cfg_get("TOTK_COOP_TRACE_MS");
        for (const char *p = e; p && *p && nats < 16;) {
            ats[nats++] = atof(p);
            p += strcspn(p, ",");
            if (*p) p++;
        }
        if (m) ms = atof(m);
        if (nats) rt_log("coop: trace: %d window(s) of %.0f ms planned", nats, ms);
    }
    static int old_prof;
    if (pending && atomic_load(&tracing) && plat_counter() > t_end) atomic_store(&tracing, 0);
    if (pending && !atomic_load(&tracing)) {   // one tick after the window: late writers are done
        if (pending == 1) g_fnprof = old_prof;
        if (++pending > 2) { write_csv(cur_at); pending = 0; trace_no++; }
        return;
    }
    if (pending || next >= nats || !me) return;
    double s = frame_step_world_us() / 1e6;
    if (s < ats[next]) return;
    cur_at = ats[next++];
    if (!build_map(me)) return;
    atomic_store(&nrec, 0), atomic_store(&dropped, 0);
    t_start = plat_counter(), t_end = t_start + (uint64_t)(ms * 1e-3 * (double)plat_counter_hz());
    old_prof = g_fnprof, g_fnprof = 1;
    atomic_store(&tracing, 1);
    pending = 1;
    rt_log("coop: trace %d on at %.2f s", trace_no, s);
}
