// Co-op: the element sync (docs/coop/element-sync-design.md, cited as D; element-sync-spec.md, cited as S).
//
// One game decides each fire, and each element state of an enemy or a placed object, and states it; the others make
// it so and never decide. Two kinds of thing are stated:
//   - ground cells: the chemical system's grid of burnable ground, where fire on grass lives (D 1.2). A cell's state,
//     heat, burn time and wetness are written into the copying game's own cell before the game's driver runs, and the
//     game makes the flame, the sound and the burnt grass from that by itself.
//   - things: placed actors with a chemical component, named by their placement hash, and enemies of the enemy sync.
//     Per element the status (burning, frozen, burnt out), electrified and wet. A copy is given its state with the
//     requests the game's own behaviour nodes use, before its component's update, and takes no reaction of its own.
// TotK 1.4.3 main offsets. On with TOTK_COOP_ELEM=1; with it off every hook passes through.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctn.h"
#include "coop_ctr.h"
#include "coop_enemy.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// ---- the game (D 1.2, 1.3) ---------------------------------------------------------------------------------------
#define FN_DRIVER 0x2dd070        // (system, cell context, step record): every live cell's step, then every cell's spread
#define FN_SPREAD 0x1f10adc       // (system, cell, cell context)
#define FN_CONTACT 0x1f0eb94      // (system, cell context, shape, element): an element touching the ground
#define FN_CELL_FIND 0x1f0ff60    // (system + 0x18, id): the cell's record, made when there is none; 0 outside the window
#define FN_CELL_INIT 0x1f100e8    // (system, cell, cell context): bit 0 set when the ground there can burn
#define FN_COMP_UPDATE 0x30efa0   // (component, step)
#define FN_REACT 0x1906768        // (target element, 0, context, reaction)
#define FN_IGNITE 0x1b34a44       // (component, 2, element index)
#define FN_REQ_B 0xb2b804         // (component, 0 extinguish | 2 burn out, element index)
#define FN_FREEZE 0x18dcf9c       // (element + 0x50, 1)
#define FN_CHARGE 0x18dcfd4       // (element + 0x50)
#define VT_GRASS_FIRE 0x36bdc60   // the class of a burning cell's fire element; its cell at element + 0x60
#define S_FIRST 0xc6d8            // the live cells: first node here, the sentinel at S_FIRST - 8, the link offset below
#define S_LINK 0xc6e4
#define R_FLAGS 0x9
#define R_STATE 0xa
#define R_ID 0xc
#define R_WET 0x28
#define R_CENTRE 0x2c
#define R_FIRE 0x80               // fire contacts of this frame (two bytes)
#define R_HEAT 0x84
#define R_BURN 0x88
#define CELLS_MAX 254

static int elem_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = cfg_get("TOTK_COOP_ELEM"); v = e && *e == '1'; }
    return v;
}
static int wr(uint64_t a, const void *v, size_t n) {
    uint8_t probe;
    if (!a || !coop_rd(a, &probe, 1) || !coop_rd(a + n - 1, &probe, 1)) return 0;
    memcpy((void *)(uintptr_t)a, v, n);
    return 1;
}
static float rdf(uint64_t a) { float f = 0; coop_rd(a, &f, 4); return f; }
static uint8_t rd8(uint64_t a) { uint8_t b = 0; coop_rd(a, &b, 1); return b; }

// ---- messages (S 3) ----------------------------------------------------------------------------------------------
enum { ELM_CELLS = 90, ELM_THINGS = 91 };
typedef struct { uint64_t page; uint32_t round; uint16_t total, n; } ElmHead;
typedef struct { float x, z; uint16_t burn; uint8_t heat, state_wet; } ElmCell;
typedef struct { uint64_t hash; uint8_t n, el[7]; } ElmThing;
#define CELLS_PAGE 96
#define THINGS_PAGE 72
#define PAGES 4
#define EL_ELEC 8
#define EL_WET 16
#define EL_NONE 0x40              // an element that is switched off: nothing is said or asked of it

static PlatMutex lk = PLAT_MUTEX_INIT;

// ---- cells -------------------------------------------------------------------------------------------------------
typedef struct { ElmCell c; uint8_t from; } Want;
static ElmCell mine[CELLS_MAX];           // this game's active cells that are no copies (the driver hook writes, phase 0 sends)
static int mine_n;
static Want want[CELLS_MAX * 2];          // what the other games state (phase 0 writes, the driver hook reads)
static int want_n;
static _Atomic uint64_t copy_rec[CELLS_MAX];   // the records written as copies in this frame's driver call
static float copy_at[CELLS_MAX][2];       // their centres (x, z), written before copy_n says how many
static _Atomic int copy_n;
static _Atomic uint32_t n_mirrors, n_not_shown, n_contact_drop, n_react_drop, n_req, n_cell_copies, n_spread_undone;
static uint64_t sys_seen;                 // the chemical system of the last driver call (a new one: a new world)
static _Atomic int world_new;             // the driver saw a new system: phase 0 empties its tables
// Test (TOTK_COOP_ELEM_TEST_IGNITE): a place the driver hook heats as a fire contact does, for some frames
static float test_at[2];
static _Atomic int test_frames;

static int is_copy_rec(uint64_t r) {
    int n = atomic_load_explicit(&copy_n, memory_order_relaxed);
    for (int i = 0; i < n && i < CELLS_MAX; i++)
        if (atomic_load_explicit(&copy_rec[i], memory_order_relaxed) == r) return 1;
    return 0;
}
static int cell_active(int state, float heat) { return state == 2 || state == 3 || (state == 1 && heat > 0); }
// The live cells' records, in list order.
static int cells_walk(uint64_t s, uint64_t *out) {
    int32_t link = 0;
    uint64_t node = coop_rd_ptr(s + S_FIRST), end = s + S_FIRST - 8;
    int n = 0;
    if (!coop_rd(s + S_LINK, &link, 4)) return 0;
    while (node && node != end && n < CELLS_MAX) {
        out[n++] = node - (int64_t)link;
        node = coop_rd_ptr(node + 8);
    }
    return n;
}
// A place's cell id in this game, as the game's own wind contact computes it (main+0x1f0f71c).
static uint32_t cell_id(uint64_t s, float x, float z) {
    float scale = rdf(s + 8), ox = rdf(s + 0xc), oz = rdf(s + 0x10);
    int32_t bias = 0, shift = 0;
    coop_rd(s + 0x14, &bias, 4), coop_rd(s + 0x3c, &shift, 4);
    if (!(scale > 0) || shift < 0 || shift > 15) return 0;
    int ix = (int16_t)(int)((x - ox) / scale + (float)bias) >> shift, iz = (int16_t)(int)((z - oz) / scale + (float)bias) >> shift;
    return (uint32_t)(ix & 0xffff) | (uint32_t)(iz & 0xffff) << 16;
}
static void cell_put(uint64_t r, const ElmCell *c) {
    int state = c->state_wet & 3, is = rd8(r + R_STATE);
    float heat = c->heat / 8.0f, burn = c->burn / 32.0f, wet = (c->state_wet >> 2) / 63.0f;
    if (is == 2 && state != 2) {
        // A burning cell is ended by the game's own step, which removes its fire element on the change: burnt by a
        // burn time past every limit, put out by full wetness. A state byte written over it would leave the fire.
        if (state == 3) burn = 4000.0f, wr(r + R_BURN, &burn, 4);
        else wet = 1.0f, wr(r + R_WET, &wet, 4), wr(r + R_HEAT, &heat, 4);
        return;
    }
    // (a cell that is being heated is kept short of igniting: the deciding game says when it burns. Written at its
    // owner's 30, it lit here a frame early, was put out by the next statement and lit again)
    if (state == 1 && heat > 29.0f) heat = 29.0f;
    uint8_t st = (uint8_t)state;
    wr(r + R_STATE, &st, 1), wr(r + R_HEAT, &heat, 4), wr(r + R_WET, &wet, 4);
    if (state == 2) wr(r + R_BURN, &burn, 4);
}
// Before the driver: the other games' cells written into this game's (S 4.3), and the test's heat.
static void cells_pre(Cpu *c, uint64_t s, uint64_t ctx) {
    static Want w[CELLS_MAX * 2];
    static uint64_t was_copy[CELLS_MAX];
    static int was_n;
    static struct { uint32_t id; uint32_t frame; } failed[64];
    int n, made = 0, me = coop_net_id();
    uint32_t frame = coop_sess_frame_no(), miss = 0;
    plat_mutex_lock(&lk);
    n = want_n, memcpy(w, want, (size_t)n * sizeof *w);
    plat_mutex_unlock(&lk);
    uint64_t now_copy[CELLS_MAX];
    for (int i = 0; i < n && made < CELLS_MAX; i++) {
        uint32_t id = cell_id(s, w[i].c.x, w[i].c.z);
        int skip = 0;
        for (int k = 0; k < 64 && !skip; k++) skip = failed[k].id == id && frame - failed[k].frame < 30;
        if (skip) { miss++; continue; }
        uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + FN_CELL_FIND, s + 0x18, id, 0, 0);
        if (r && !rd8(r + R_STATE) && !(coop_guest_call(c, MOD_MAIN_BASE + FN_CELL_INIT, s, r, ctx, 0) & 1)) r = 0;
        if (!r) { failed[id % 64].id = id, failed[id % 64].frame = frame, miss++; continue; }
        int was = 0;
        for (int k = 0; k < was_n && !was; k++) was = was_copy[k] == r;
        // (a cell this game lit too: the lower player id states it, S 4.3)
        if (!was && cell_active(rd8(r + R_STATE), rdf(r + R_HEAT)) && me && me < w[i].from) continue;
        cell_put(r, &w[i].c);
        now_copy[made++] = r;
    }
    for (int i = 0; i < made; i++) {
        atomic_store_explicit(&copy_rec[i], now_copy[i], memory_order_relaxed);
        copy_at[i][0] = rdf(now_copy[i] + R_CENTRE), copy_at[i][1] = rdf(now_copy[i] + R_CENTRE + 8);
    }
    atomic_store(&copy_n, made);
    memcpy(was_copy, now_copy, (size_t)made * sizeof *was_copy), was_n = made;
    atomic_store(&n_not_shown, miss), atomic_store(&n_cell_copies, (uint32_t)made);
    if (atomic_load(&test_frames) > 0) {
        atomic_fetch_sub(&test_frames, 1);
        uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + FN_CELL_FIND, s + 0x18, cell_id(s, test_at[0], test_at[1]), 0, 0);
        int ok = r && (rd8(r + R_STATE) || (coop_guest_call(c, MOD_MAIN_BASE + FN_CELL_INIT, s, r, ctx, 0) & 1));
        uint8_t five = 5;
        if (ok) wr(r + R_FIRE, &five, 1);
        static int said;
        if (!said++) rt_log("coop: elem: test: heating the cell at %.1f %.1f: %s", test_at[0], test_at[1], ok ? "a cell is there" : "no burnable ground there");
    }
}
// After the driver: this game's own active cells, for the next statement, and the log of fires starting and ending.
static void cells_post(uint64_t s) {
    static uint8_t last_state[256];
    static uint64_t last_rec[256];
    uint64_t rec[CELLS_MAX];
    ElmCell out[CELLS_MAX];
    int n = cells_walk(s, rec), m = 0;
    for (int i = 0; i < n; i++) {
        uint64_t r = rec[i];
        int state = rd8(r + R_STATE) & 3, copy = is_copy_rec(r);
        float heat = rdf(r + R_HEAT), burn = rdf(r + R_BURN), wet = rdf(r + R_WET), x = rdf(r + R_CENTRE), z = rdf(r + R_CENTRE + 8);
        int k = (int)((r >> 3) % 256);
        if (last_rec[k] != r) last_rec[k] = r, last_state[k] = 0;
        if ((state == 2) != (last_state[k] == 2))
            rt_log("coop: elem: cell %.1f %.1f %s (%s), burn %.0f, heat %.0f, wet %.2f, ground %.2f %.2f %.2f, flags %x, at %llu ms", x, z,
                   state == 2 ? "burns" : state == 3 ? "is burnt" : "is out", copy ? "a copy" : "mine", burn, heat, wet, rdf(r + 0x1c), rdf(r + 0x20),
                   rdf(r + 0x24), rd8(r + R_FLAGS), (unsigned long long)(plat_now_ns() / 1000000));
        last_state[k] = (uint8_t)state;
        if (copy || !cell_active(state, heat)) continue;
        ElmCell *o = &out[m++];
        o->x = x, o->z = z;
        o->burn = (uint16_t)(burn < 0 ? 0 : burn > 2040 ? 65280 : burn * 32);
        o->heat = (uint8_t)(heat < 0 ? 0 : heat > 30 ? 240 : heat * 8);
        o->state_wet = (uint8_t)(state | (int)((wet < 0 ? 0 : wet > 1 ? 1 : wet) * 63) << 2);
    }
    plat_mutex_lock(&lk);
    memcpy(mine, out, (size_t)m * sizeof *out), mine_n = m;
    plat_mutex_unlock(&lk);
}
// A copied cell's spread: it runs (its burn broadcast is what burns the grass away), and the heat it gave to other
// cells is taken back, so that only the deciding game's spread lights the next cell (S 4.4).
static int spread_hook(Cpu *c, uint64_t addr) {
    static __thread int inner;
    if (inner) { inner = 0; return 0; }
    uint64_t s = c->x[0], r = c->x[1];
    if (!is_copy_rec(r)) return 0;
    uint64_t rec[CELLS_MAX], after[CELLS_MAX];
    uint16_t fire[CELLS_MAX];
    int n = cells_walk(s, rec);
    for (int i = 0; i < n; i++) fire[i] = 0, coop_rd(rec[i] + R_FIRE, &fire[i], 2);
    inner = 1, c->pc = addr;
    rt_dispatch(c);
    int m = cells_walk(s, after), undone = 0;
    for (int i = 0; i < m; i++) {
        uint16_t f = 0, now = 0;
        for (int k = 0; k < n; k++) if (rec[k] == after[i]) { f = fire[k]; break; }
        if (coop_rd(after[i] + R_FIRE, &now, 2) && now != f) wr(after[i] + R_FIRE, &f, 2), undone++;
    }
    if (undone) atomic_fetch_add(&n_spread_undone, (uint32_t)undone);
    return 1;
}

// ---- copies' elements: no reaction into them, no contact with the ground from them (S 4.5, 5.4) --------------------
#define ESET 4096
static struct { _Atomic uint64_t el; _Atomic uint32_t frame; } eset[ESET];
static uint32_t eset_slot(uint64_t el) { return (uint32_t)((el >> 4) * 0x9e3779b1u) & (ESET - 1); }
static void eset_put(uint64_t el, uint32_t frame) {
    uint32_t h = eset_slot(el), pick = h;
    for (int i = 0; i < 4; i++) {
        uint32_t k = (h + (uint32_t)i) & (ESET - 1);
        uint64_t at = atomic_load_explicit(&eset[k].el, memory_order_relaxed);
        if (at == el) { pick = k; break; }
        if (frame - atomic_load_explicit(&eset[k].frame, memory_order_relaxed) > 3) pick = k;
    }
    atomic_store_explicit(&eset[pick].frame, frame, memory_order_relaxed);
    atomic_store_explicit(&eset[pick].el, el, memory_order_relaxed);
}
static int eset_has(uint64_t el) {
    uint32_t h = eset_slot(el), frame = coop_sess_frame_no();
    for (int i = 0; i < 4; i++) {
        uint32_t k = (h + (uint32_t)i) & (ESET - 1);
        if (atomic_load_explicit(&eset[k].el, memory_order_relaxed) == el) return frame - atomic_load_explicit(&eset[k].frame, memory_order_relaxed) <= 3;
    }
    return 0;
}
#define INERT_N 64
static _Atomic uint64_t inert[INERT_N];
static int is_inert(uint64_t actor) {
    for (int i = 0; i < INERT_N; i++) if (atomic_load_explicit(&inert[i], memory_order_relaxed) == actor) return 1;
    return 0;
}
static uint64_t comp_of(uint64_t actor) {
    uint64_t t = actor ? coop_rd_ptr(actor + 0x228) : 0;
    return t ? coop_rd_ptr(t + 0x180) : 0;
}
// An actor that is shown only (the enemy sync's mirror of another game's projectile): chemically it does nothing
// (D 2.4). on 0 takes it out again; an actor that is gone is taken out by whoever made it inert.
void coop_elem_inert(uint64_t actor, int on) {
    if (!actor) return;
    int at = -1, free_at = -1;
    for (int i = 0; i < INERT_N; i++) {
        uint64_t a = atomic_load(&inert[i]);
        if (a == actor) at = i;
        if (!a && free_at < 0) free_at = i;
    }
    if (on && at < 0 && free_at >= 0) atomic_store(&inert[free_at], actor);
    if (!on && at >= 0) atomic_store(&inert[at], 0);
    uint64_t comp = comp_of(actor), word;
    if (comp && coop_rd(comp + 0x20, &word, 8)) {   // the game's own "does not interact with the world" (main+0x250cc48)
        _Atomic uint64_t *p = (_Atomic uint64_t *)(uintptr_t)(comp + 0x20);
        if (on) atomic_fetch_or(p, 1ull << 32); else atomic_fetch_and(p, ~(1ull << 32));
    }
}

// A placed actor's name across games: its placement hash (0: it has none, or not yet).
static uint64_t hash_of(uint64_t actor) {
    uint64_t p = coop_rd_ptr(actor + 0x370), h = 0;
    return p && coop_rd(p + 0x88, &h, 8) && h != ~0ull ? h : 0;
}
// ---- things (S 5) ------------------------------------------------------------------------------------------------
#define THING_N 1024
#define EL_MAX 7
typedef struct {
    uint64_t comp, actor, hash;
    uint32_t seen, first, differs_frame, asked[EL_MAX];   // frames: last visit, first visit, last frame an element differed from rest, last request
    uint8_t n, cur[EL_MAX], rest[EL_MAX], wanted[EL_MAX];
    uint8_t want_from;            // the player whose statement this game follows for it (0: none)
    uint8_t enemy, skip;
    float pos[3];
} Thing;
static Thing things[THING_N];
static uint32_t thing_slot(uint64_t comp) { return (uint32_t)((comp >> 4) * 0x9e3779b1u) & (THING_N - 1); }
// The row of a component (made when absent; an old row is taken over). Under the lock.
static Thing *thing_row(uint64_t comp, uint64_t actor, uint32_t frame) {
    uint32_t h = thing_slot(comp);
    Thing *old = NULL;
    for (int i = 0; i < 8; i++) {
        Thing *t = &things[(h + (uint32_t)i) & (THING_N - 1)];
        if (t->comp == comp && t->actor == actor) return t;
        if (!old && !t->comp) old = t;
    }
    if (!old) return NULL;
    memset(old, 0, sizeof *old);
    old->comp = comp, old->actor = actor, old->seen = frame, old->n = 0xff;
    return old;
}
static int el_read(uint64_t comp, uint64_t *els, uint8_t *cur) {
    int32_t link = 0;
    uint32_t count = 0;
    uint64_t node = coop_rd_ptr(comp + 0x48), end = comp + 0x40;
    int n = 0;
    if (!coop_rd(comp + 0x50, &count, 4) || !coop_rd(comp + 0x54, &link, 4)) return 0;
    for (uint32_t i = 0; node && node != end && i < count && n < EL_MAX; i++) {
        uint64_t el = coop_rd_ptr(node - (int64_t)link + 0x10);
        if (!el) break;
        int8_t kind = -1, idx = 0;
        uint8_t f8 = 0, f95 = 0;
        uint16_t f90 = 0;
        coop_rd(el + 0x7d, &kind, 1), coop_rd(el + 8, &f8, 1), coop_rd(el + 0x90, &f90, 2), coop_rd(el + 0x95, &f95, 1), coop_rd(el + 0x251, &idx, 1);
        uint8_t e = EL_NONE;
        if ((f8 & 4) && !(f90 & 0x14) && kind >= 0 && kind <= 4) {
            float wet = rdf(el + 0xc0 + (idx == 1 ? 200 : 0) + 0x68);
            e = (uint8_t)(kind | (f95 & 2 ? EL_ELEC : 0) | (wet > 0.5f ? EL_WET : 0));
        }
        els[n] = el, cur[n++] = e;
        node = coop_rd_ptr(node + 8);
    }
    return n;
}
// What a copy's element is asked to become (S 5.3): 0 nothing, else an ASK_*; and the asking, with the game's own
// requests.
enum { ASK_IGNITE = 1, ASK_BURN_OUT, ASK_FREEZE, ASK_PUT_OUT, ASK_CHARGE = 8 };
static int ask_of(uint8_t cur, uint8_t wanted) {
    if (cur == EL_NONE || wanted == EL_NONE) return 0;
    int w = wanted & 7, is = cur & 7, what = 0;
    if (w != is) what = w == 2 ? ASK_IGNITE : w == 3 ? ASK_BURN_OUT : w == 1 ? ASK_FREEZE : w == 0 && is == 2 ? ASK_PUT_OUT : 0;
    if (wanted & EL_ELEC && !(cur & EL_ELEC)) what |= ASK_CHARGE;
    return what;
}
static void ask_do(Cpu *c, uint64_t comp, uint64_t el, int i, int what) {
    int k = what & 7;
    {
        static const char *nm[8] = {"", "ignite", "burn out", "freeze", "go out", "", "", ""};
        char an[48];
        rt_log("coop: elem: asks %s (hash %llx) element %d to %s%s", coop_actor_name(coop_rd_ptr(comp + 0x18), an, sizeof an),
               (unsigned long long)hash_of(coop_rd_ptr(comp + 0x18)), i, nm[k], what & ASK_CHARGE ? " and take a charge" : "");
    }
    if (k == ASK_IGNITE) coop_guest_call(c, MOD_MAIN_BASE + FN_IGNITE, comp, 2, (uint64_t)i, 0);
    else if (k == ASK_BURN_OUT) coop_guest_call(c, MOD_MAIN_BASE + FN_REQ_B, comp, 2, (uint64_t)i, 0);
    else if (k == ASK_FREEZE) coop_guest_call(c, MOD_MAIN_BASE + FN_FREEZE, el + 0x50, 1, 0, 0);
    else if (k == ASK_PUT_OUT) coop_guest_call(c, MOD_MAIN_BASE + FN_REQ_B, comp, 0, (uint64_t)i, 0);
    if (what & ASK_CHARGE) coop_guest_call(c, MOD_MAIN_BASE + FN_CHARGE, el + 0x50, 0, 0, 0);
    atomic_fetch_add(&n_req, 1);
}
// A component's update: the thing is read; a copy is given what its owner states and its elements are closed to
// reactions (S 5.3, 5.4).
static int comp_hook(Cpu *c) {
    uint64_t comp = c->x[0], actor = coop_rd_ptr(comp + 0x18), els[EL_MAX];
    uint8_t cur[EL_MAX], wanted[EL_MAX];
    if (!actor) return 0;
    uint32_t frame = coop_sess_frame_no();
    uint8_t before[EL_MAX] = {0};
    int n = el_read(comp, els, cur), copy = 0, ask = 0, changed = 0, what[EL_MAX] = {0};
    if (!n) return 0;
    // (a projectile shot by an enemy another game runs is the copy's own actor, made by the same calls as the owner's
    // (coop_enemy_shot.c): it is shown only, so it is inert here as an actor named to coop_elem_inert is)
    int coop_enemy_shot_mirror(uint64_t proj);
    int mirror = coop_enemy_shot_mirror(actor), in = mirror || is_inert(actor), es = in ? EM_FREE : en_cell_state(actor);
    if (mirror) {
        uint64_t word;
        if (coop_rd(comp + 0x20, &word, 8) && !(word >> 32 & 1)) {
            atomic_fetch_or((_Atomic uint64_t *)(uintptr_t)(comp + 0x20), 1ull << 32);
            atomic_fetch_add(&n_mirrors, 1);
        }
    }
    plat_mutex_lock(&lk);
    Thing *t = thing_row(comp, actor, frame);
    if (t) {
        if (t->n == 0xff) {   // first sight: its name across games, and its state as the rest state
            t->hash = hash_of(actor);
            memcpy(t->rest, cur, sizeof cur);
            t->skip = actor == coop_player_actor(), t->first = frame;
        }
        // (its first second settles what its rest state is: a brazier lit by its own data starts unlit)
        if (frame - t->first < 30 && !t->want_from) memcpy(t->rest, cur, sizeof cur);
        for (int i = 0; i < n && t->n == n && t->hash; i++) if (t->cur[i] != cur[i]) changed |= 1 << i, before[i] = t->cur[i];
        t->n = (uint8_t)n, t->seen = frame, t->enemy = es != EM_FREE;
        memcpy(t->cur, cur, sizeof cur);
        coop_rd(actor + COOP_ACTOR_POS, t->pos, 12);
        if (memcmp(cur, t->rest, (size_t)n)) t->differs_frame = frame;
        copy = in || es == EM_COPY || (!t->enemy && t->want_from);
        if (es == EM_COPY && !t->want_from) memset(t->wanted, 0, sizeof t->wanted);   // an enemy copy nobody states is at rest
        ask = (es == EM_COPY || t->want_from) && !in;
        memcpy(wanted, t->wanted, sizeof wanted);
        if (ask) for (int i = 0; i < n; i++) {
            what[i] = ask_of(cur[i], wanted[i]);
            if (what[i] && frame - t->asked[i] < 30) what[i] = 0;
            if (what[i]) t->asked[i] = frame;
        }
    }
    plat_mutex_unlock(&lk);
    if (copy) for (int i = 0; i < n; i++) eset_put(els[i], frame);
    for (int i = 0; i < n; i++) if (changed >> i & 1 && !(before[i] & EL_NONE)) {
        static const char *kind[8] = {"normal", "frozen", "burning", "burnt out", "detonated", "?", "?", "?"};
        char nm[48];
        rt_log("coop: elem: thing %s element %d: %s%s%s -> %s%s%s (%s), at %llu ms", coop_actor_name(actor, nm, sizeof nm), i,
               before[i] & EL_NONE ? "off" : kind[before[i] & 7], before[i] & EL_ELEC ? ", electrified" : "", before[i] & EL_WET ? ", wet" : "",
               cur[i] & EL_NONE ? "off" : kind[cur[i] & 7], cur[i] & EL_ELEC ? ", electrified" : "", cur[i] & EL_WET ? ", wet" : "",
               copy ? "a copy" : "mine", (unsigned long long)(plat_now_ns() / 1000000));
    }
    if (!ask || coop_ctn_driven(actor) || coop_ctn_follows(actor)) return 0;
    for (int i = 0; i < n; i++) if (what[i]) ask_do(c, comp, els[i], i, what[i]);
    return 0;
}

int coop_elem_hook(Cpu *c, uint64_t off) {
    if (!elem_on()) return 0;
    if (off == FN_REACT) {
        if (!eset_has(c->x[0])) return 0;
        atomic_fetch_add_explicit(&n_react_drop, 1, memory_order_relaxed);
        return 1;
    }
    if (off == FN_COMP_UPDATE) return comp_hook(c);
    if (off == FN_CONTACT) {
        // What touches the ground is a chemical object, not an element (classes main+0x3625660 and main+0x36295d0 in a
        // run). A burning cell's own fire is one, centred on the cell: an object centred on a copied cell is taken
        // for that cell's fire and does not reach the cells (S 4.5), so a copied fire lights no neighbour here.
        float at[3] = {0, 0, 0};
        int drop = 0, n = atomic_load_explicit(&copy_n, memory_order_relaxed);
        if (n && coop_rd(c->x[2] + 8, at, 12))
            for (int i = 0; i < n && i < CELLS_MAX && !drop; i++) drop = fabsf(at[0] - copy_at[i][0]) < 0.25f && fabsf(at[2] - copy_at[i][1]) < 0.25f;
        {   // Test (TOTK_COOP_ELEM_TEST_LOG=1): the classes of the objects kept from the ground, each once
            static int log = -1;
            static _Atomic uint64_t seen[8];
            uint64_t vt = coop_rd_ptr(c->x[3]);
            if (log < 0) log = cfg_get("TOTK_COOP_ELEM_TEST_LOG") != NULL;
            for (int i = 0; log && drop && i < 8; i++) {
                uint64_t v = atomic_load(&seen[i]);
                if (v == vt) break;
                if (!v) { atomic_store(&seen[i], vt); rt_log("coop: elem: test: an object of class main+%llx on a copied cell is kept from the ground", (unsigned long long)(vt - MOD_MAIN_BASE)); break; }
            }
        }
        if (drop) atomic_fetch_add_explicit(&n_contact_drop, 1, memory_order_relaxed);
        return drop;
    }
    if (off == FN_SPREAD) return spread_hook(c, MOD_MAIN_BASE + off);
    if (off == FN_DRIVER) {
        static __thread int inner;
        if (inner) { inner = 0; return 0; }
        uint64_t s = c->x[0], ctx = c->x[1];
        if (s != sys_seen) sys_seen = s, atomic_store(&copy_n, 0), atomic_store(&world_new, 1);
        else if (!coop_sess_loading()) cells_pre(c, s, ctx);
        inner = 1, c->pc = MOD_MAIN_BASE + off;
        rt_dispatch(c);
        cells_post(s);
        return 1;
    }
    return 0;
}

// ---- phase 0: statements taken and sent (S 3, 4.2, 5.2) --------------------------------------------------------------
typedef struct { int n; uint32_t round; ElmCell c[CELLS_PAGE * PAGES]; } CellList;
typedef struct { int n; uint32_t round; ElmThing t[THINGS_PAGE * PAGES]; } ThingList;
static CellList *cell_list[COOP_MAX_PLAYERS + 1];
static ThingList *thing_list[COOP_MAX_PLAYERS + 1];

// A player's newest complete round of one type: 1 taken (n entries of size z into out), 0 nothing new or whole,
// -1 the player states nothing any more (no page for 3 s).
static int take(int id, int type, void *out, size_t z, int per_page, int *n, uint32_t *round) {
    uint8_t buf[COOP_CTR_PAYLOAD];
    uint64_t age = 0;
    ElmHead h;
    int len = coop_ctr_latest(id, type, 0, buf, sizeof buf, &age), got = 0;
    if (len < (int)sizeof h) return -1;
    if (age > 3000000000ull) return -1;
    memcpy(&h, buf, sizeof h);
    if (h.total > per_page * PAGES) return 0;
    for (int page = 0; got < h.total && page < PAGES; page++) {
        ElmHead ph;
        if (page) {
            len = coop_ctr_latest(id, type, (uint64_t)page, buf, sizeof buf, &age);
            if (len < (int)sizeof ph) return 0;
        }
        memcpy(&ph, buf, sizeof ph);
        if (ph.round != h.round || ph.n > per_page || len < (int)(sizeof ph + ph.n * z) || got + ph.n > h.total) return 0;
        memcpy((uint8_t *)out + (size_t)got * z, buf + sizeof ph, ph.n * z);
        got += ph.n;
    }
    if (got != h.total) return 0;
    *n = got, *round = h.round;
    return 1;
}
static void send_pages(int type, const void *e, size_t z, int n, int per_page, uint32_t round) {
    uint8_t buf[COOP_CTR_PAYLOAD];
    for (int page = 0, at = 0; page < PAGES && (at < n || !page); page++) {
        ElmHead h = {.page = (uint64_t)page, .round = round, .total = (uint16_t)n, .n = (uint16_t)(n - at < per_page ? n - at : per_page)};
        memcpy(buf, &h, sizeof h), memcpy(buf + sizeof h, (const uint8_t *)e + (size_t)at * z, h.n * z);
        coop_ctr_send(0, type, 0, buf, (int)(sizeof h + h.n * z));
        at += h.n;
    }
}
static int lowest_here(void) {
    int ids[COOP_MAX_PLAYERS], me = coop_net_id();
    char names[COOP_MAX_PLAYERS][17];
    int n = coop_net_players(ids, names, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) if (ids[i] < me) return 0;
    return me > 0;
}
static void tests(Cpu *c) {
    static double at_cell = -2, at_thing = -2;
    static float dx = 3, dz;
    static char prefix[32];
    static int done_cell, done_thing;
    if (at_cell < -1) {
        const char *e = cfg_get("TOTK_COOP_ELEM_TEST_IGNITE"), *f = cfg_get("TOTK_COOP_ELEM_TEST_THING");
        at_cell = e ? atof(e) : -1, at_thing = f ? atof(f) : -1;
        if (f && strchr(f, ',')) snprintf(prefix, sizeof prefix, "%s", strchr(f, ',') + 1);
        const char *k = e ? strchr(e, ',') : NULL;
        if (k) { dx = (float)atof(k + 1); k = strchr(k + 1, ','); if (k) dz = (float)atof(k + 1); }
    }
    double t = frame_step_world_us() / 1e6;
    uint64_t link = coop_player_actor();
    float p[3];
    if (!link || !lowest_here() || !coop_rd(link + COOP_ACTOR_POS, p, 12)) return;
    if (at_cell >= 0 && t >= at_cell && !done_cell) done_cell = 1, test_at[0] = p[0] + dx, test_at[1] = p[2] + dz, atomic_store(&test_frames, 90);
    if (at_thing >= 0 && t >= at_thing && !done_thing) {
        done_thing = 1;
        Thing *best = NULL;
        int rows = 0, named = 0;
        float bd = 150.0f * 150.0f;
        plat_mutex_lock(&lk);
        for (int i = 0; i < THING_N; i++) {
            Thing *th = &things[i];
            if (th->comp) rows++, named += th->hash != 0;
            if (!th->comp || !th->hash || th->enemy || th->skip || th->n == 0xff) continue;
            char tn[48];
            if (*prefix && strncmp(coop_actor_name(th->actor, tn, sizeof tn), prefix, strlen(prefix))) continue;
            float d = (th->pos[0] - p[0]) * (th->pos[0] - p[0]) + (th->pos[1] - p[1]) * (th->pos[1] - p[1]) + (th->pos[2] - p[2]) * (th->pos[2] - p[2]);
            if (d < bd) bd = d, best = th;
        }
        uint64_t a = best ? best->actor : 0, comp = best ? best->comp : 0;
        plat_mutex_unlock(&lk);
        if (comp) coop_guest_call(c, MOD_MAIN_BASE + FN_IGNITE, comp, 2, (uint64_t)-1, 0);
        char nm[48];
        rt_log("coop: elem: test: the nearest placed thing is %s, %.1f m away (%d rows, %d with a placement hash)",
               a ? coop_actor_name(a, nm, sizeof nm) : "none within 150 m", a ? sqrtf(bd) : 0.0f, rows, named);
    }
}

void coop_elem_phase0(Cpu *c) {
    if (!elem_on()) return;
    static uint32_t round, last_any;
    static int said_on;
    uint32_t frame = coop_sess_frame_no();
    int me = coop_net_id();
    if (!said_on++) rt_log("coop: elem: the element sync is on");
    if (atomic_exchange(&world_new, 0) || coop_sess_loading()) {
        // (the things' rows are not emptied: a thing at rest is visited once, when it is made, which may be before the
        // first driver call of its world; a row ends when its actor has gone)
        plat_mutex_lock(&lk);
        want_n = 0, mine_n = 0;
        plat_mutex_unlock(&lk);
        for (int i = 0; i <= COOP_MAX_PLAYERS; i++) {
            if (cell_list[i]) cell_list[i]->n = 0;
            if (thing_list[i]) thing_list[i]->n = 0;
        }
        if (coop_sess_loading()) return;
    }
    if (!me) return;
    {   // Test (TOTK_COOP_ELEM_TEST_DEAF=<s>): until that world time this game takes no statement, as a game does that
        // arrives then
        static double deaf = -2;
        if (deaf < -1) { const char *e = cfg_get("TOTK_COOP_ELEM_TEST_DEAF"); deaf = e ? atof(e) : -1; }
        if (deaf > 0 && frame_step_world_us() / 1e6 < deaf) { tests(c); return; }
    }
    // Taking: each other player's newest complete round; a player gone states nothing; one paused or loading keeps
    // what it last stated (S 4.2).
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int np = coop_net_players(ids, names, COOP_MAX_PLAYERS);
    uint8_t here[COOP_MAX_PLAYERS + 1] = {0};
    for (int i = 0; i < np; i++) {
        int id = ids[i], st = coop_sess_peer(id);
        if (id < 1 || id > COOP_MAX_PLAYERS || st == COOP_SESS_GONE) continue;
        here[id] = 1;
        if (st != COOP_SESS_RUNS) continue;
        if (!cell_list[id]) cell_list[id] = calloc(1, sizeof **cell_list);
        if (!thing_list[id]) thing_list[id] = calloc(1, sizeof **thing_list);
        if (!cell_list[id] || !thing_list[id]) continue;
        static ElmCell cb[CELLS_PAGE * PAGES];
        static ElmThing tb[THINGS_PAGE * PAGES];
        int n = 0;
        uint32_t r = 0;
        int k = take(id, ELM_CELLS, cb, sizeof *cb, CELLS_PAGE, &n, &r);
        if (k > 0) memcpy(cell_list[id]->c, cb, (size_t)n * sizeof *cb), cell_list[id]->n = n, cell_list[id]->round = r;
        else if (k < 0) cell_list[id]->n = 0;
        k = take(id, ELM_THINGS, tb, sizeof *tb, THINGS_PAGE, &n, &r);
        if (k > 0) memcpy(thing_list[id]->t, tb, (size_t)n * sizeof *tb), thing_list[id]->n = n, thing_list[id]->round = r;
        else if (k < 0) thing_list[id]->n = 0;
    }
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++)
        if (!here[id]) { if (cell_list[id]) cell_list[id]->n = 0; if (thing_list[id]) thing_list[id]->n = 0; }
    // The wanted cells: every list, the lower player id first; a place named twice is taken once.
    static Want w[CELLS_MAX * 2];
    int wn = 0;
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (id == me || !cell_list[id]) continue;
        for (int i = 0; i < cell_list[id]->n && wn < CELLS_MAX * 2; i++) {
            const ElmCell *e = &cell_list[id]->c[i];
            int dup = 0;
            for (int k = 0; k < wn && !dup; k++) dup = fabsf(w[k].c.x - e->x) < 0.1f && fabsf(w[k].c.z - e->z) < 0.1f;
            if (!dup) w[wn].c = *e, w[wn++].from = (uint8_t)id;
        }
    }
    // Things: what each row is to be, and this game's own statement.
    static ElmThing out[THINGS_PAGE * PAGES];
    static ElmCell cells[CELLS_MAX];
    enum { JOBS = 32 };
    struct { uint64_t comp, el; int i, what; } job[JOBS];
    int tn = 0, cn, n_copy = 0, nj = 0;
    plat_mutex_lock(&lk);
    memcpy(want, w, (size_t)wn * sizeof *w), want_n = wn;
    cn = mine_n, memcpy(cells, mine, (size_t)cn * sizeof *cells);
    for (int i = 0; i < THING_N; i++) {
        Thing *t = &things[i];
        if (!t->comp || t->n == 0xff) continue;
        // (a row lives while its actor has this component: a component at rest is not updated by the game, so the
        // time since its last visit says nothing)
        if ((frame + (uint32_t)i) % 30 == 0) {
            if (comp_of(t->actor) != t->comp) { t->comp = 0; continue; }
            if (!t->hash) t->hash = hash_of(t->actor);   // (an actor's first update may come before its placement is linked)
            coop_rd(t->actor + COOP_ACTOR_POS, t->pos, 12);
        }
        if (!t->hash || t->skip) continue;
        // (an enemy is stated while it is not at rest; another thing while it differs from how it was first seen, and
        // for 2 s after)
        int rest = 1, mine_claim;
        for (int k = 0; k < t->n; k++) rest &= t->cur[k] == EL_NONE || !(t->cur[k] & 0x1f);
        mine_claim = t->enemy ? !rest : t->differs_frame && frame - t->differs_frame < 60;
        int from = 0;
        const ElmThing *st = NULL;
        for (int id = 1; id <= COOP_MAX_PLAYERS && !st; id++) {
            if (id == me || !thing_list[id]) continue;
            for (int k = 0; k < thing_list[id]->n && !st; k++)
                if (thing_list[id]->t[k].hash == t->hash) st = &thing_list[id]->t[k], from = id;
        }
        // (both claim it: the lower player id states it. A thing that is a copy already has no claim of this game's:
        // what differs from its rest state there is what it was made to be)
        if (st && !t->enemy && !t->want_from && mine_claim && me < from) st = NULL, from = 0;
        if (st && (t->want_from || !t->enemy || en_cell_state(t->actor) == EM_COPY)) {
            if (!t->want_from) t->differs_frame = 0;   // what a copy is made to be is no claim of this game's
            t->want_from = (uint8_t)from;
            memset(t->wanted, EL_NONE, sizeof t->wanted);
            memcpy(t->wanted, st->el, st->n < EL_MAX ? st->n : EL_MAX);
            n_copy++;
            if (frame - t->seen > 3 && nj < JOBS) {   // its component rests: read and asked here, not in its update's hook
                uint64_t els[EL_MAX];
                uint8_t cur[EL_MAX];
                int n = el_read(t->comp, els, cur);
                if (n) {
                    t->n = (uint8_t)n, memcpy(t->cur, cur, sizeof cur);
                    for (int k = 0; k < n; k++) eset_put(els[k], frame);
                    for (int k = 0; k < n && nj < JOBS; k++) {
                        int what = ask_of(cur[k], t->wanted[k]);
                        if (!what || frame - t->asked[k] < 30) continue;
                        t->asked[k] = frame;
                        job[nj].comp = t->comp, job[nj].el = els[k], job[nj].i = k, job[nj++].what = what;
                    }
                }
            }
            continue;
        }
        if (t->want_from) t->want_from = 0, t->differs_frame = 0, memcpy(t->rest, t->cur, sizeof t->rest);   // let go: as it stands is its rest
        if (t->enemy && en_cell_state(t->actor) == EM_COPY) continue;
        if (!mine_claim || tn >= THINGS_PAGE * PAGES) continue;
        out[tn].hash = t->hash, out[tn].n = t->n;
        memset(out[tn].el, EL_NONE, sizeof out[tn].el), memcpy(out[tn].el, t->cur, t->n);
        tn++;
    }
    if (frame % 30 == 0)
        for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
            if (id == me || !thing_list[id]) continue;
            for (int k = 0; k < thing_list[id]->n; k++) {
                int rows = 0;
                for (int i = 0; i < THING_N; i++) rows += things[i].comp && things[i].hash == thing_list[id]->t[k].hash;
                if (rows != 1) rt_log("coop: elem: player %d states a thing (hash %llx, element 0: %x) that %d things here answer to", id,
                                      (unsigned long long)thing_list[id]->t[k].hash, thing_list[id]->t[k].el[0], rows);
            }
        }
    plat_mutex_unlock(&lk);
    for (int k = 0; k < nj; k++) ask_do(c, job[k].comp, job[k].el, job[k].i, job[k].what);
    // Sending: every 6th frame while there is something, and 2 s after (the empty rounds end what the others follow).
    if (cn || tn) last_any = frame;
    if (frame % 6 == 0 && !coop_sess_paused() && last_any && frame - last_any < 60) {
        round++;
        send_pages(ELM_CELLS, cells, sizeof *cells, cn, CELLS_PAGE, round);
        send_pages(ELM_THINGS, out, sizeof *out, tn, THINGS_PAGE, round);
    }
    static uint32_t said;
    if (frame - said >= 30 && (cn || tn || wn || n_copy)) {
        said = frame;
        rt_log("coop: elem: cells mine %d, copied %u, not shown %u; things mine %d, copied %d; requests %u, reactions dropped %u, contacts dropped %u, spread heat undone %u, mirrored projectiles made inert %u",
               cn, atomic_load(&n_cell_copies), atomic_load(&n_not_shown), tn, n_copy, atomic_load(&n_req), atomic_load(&n_react_drop),
               atomic_load(&n_contact_drop), atomic_load(&n_spread_undone), atomic_load(&n_mirrors));
    }
    tests(c);
}
