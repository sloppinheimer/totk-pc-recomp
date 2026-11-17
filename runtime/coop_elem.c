// Co-op: the element sync (docs/coop/facts/elements.md, cited as D; facts/elements.md, cited as S).
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
#define FN_FREEZE 0x1b34e48       // (component, 4, element index): the cause the component itself asks with
#define FN_THAW 0x1b34f10         // (component, 0, element index)
#define FN_CHARGE 0x18dcfd4       // (element + 0x50)
#define FN_STRIKE 0x21c43b4       // (thunder manager, position, level, sage, element or 0): a lightning strike, every caller's
#define VT_OBJECT 0x3625660       // a chemical object an element's update makes: what reactions and ground contacts carry
#define VT_SOURCE 0x3626048       // its source at +0x60; the source's +0x60 an owner object, whose +0x10 is the component
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
enum { ELM_CELLS = 90, ELM_THINGS = 91, ELM_STRIKE = 92, ELM_LINK = 93 };
typedef struct { uint64_t zero; uint8_t n, el[7], ice, cond, pad[6]; } ElmLink;   // 24 bytes: this game's own Link
typedef struct { uint64_t slot; uint32_t serial; float pos[3]; uint8_t level, sage, pad[2]; } ElmStrike;   // 28 bytes; slot = serial % 8
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
        // (the step dries a cell before it tests the wetness, so the water contacts of the frame are written full as
        // well: with them it adds and stays at 1)
        if (state == 3) burn = 4000.0f, wr(r + R_BURN, &burn, 4);
        else { uint8_t full = 0xff; wet = 1.0f, wr(r + R_WET, &wet, 4), wr(r + R_HEAT, &heat, 4), wr(r + R_FIRE + 2, &full, 1); }
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
    uint64_t now_copy[CELLS_MAX], live[CELLS_MAX];
    uint8_t now_burns[CELLS_MAX];
    int nlive = cells_walk(s, live);
    for (int i = 0; i < n && made < CELLS_MAX; i++) {
        uint32_t id = cell_id(s, w[i].c.x, w[i].c.z);
        int skip = 0;
        if ((w[i].c.state_wet & 3) == 3) {
            // A burnt cell is only ever the end of a cell that burns here: no record is made for it. (Made again, the
            // record lived another 45 frames as this game's own burnt cell, was stated back, and the two games kept
            // each other's burnt cells alive.)
            uint64_t r = 0;
            uint32_t rid = 0;
            for (int k = 0; k < nlive && !r; k++) if (coop_rd(live[k] + R_ID, &rid, 4) && rid == id) r = live[k];
            if (r && rd8(r + R_STATE) == 2) cell_put(r, &w[i].c), now_copy[made] = r, now_burns[made++] = 1;
            continue;
        }
        for (int k = 0; k < 64 && !skip; k++) skip = failed[k].id == id && frame - failed[k].frame < 30;
        if (skip) { miss++; continue; }
        uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + FN_CELL_FIND, s + 0x18, id, 0, 0);
        if (r && !rd8(r + R_STATE) && !(coop_guest_call(c, MOD_MAIN_BASE + FN_CELL_INIT, s, r, ctx, 0) & 1)) r = 0;
        if (!r) { failed[id % 64].id = id, failed[id % 64].frame = frame, miss++; continue; }
        int was = 0;
        for (int k = 0; k < was_n && !was; k++) was = was_copy[k] == r;
        // (a cell this game lit too: the lower player id states it, S 4.3)
        int st_here = rd8(r + R_STATE);
        if (!was && (st_here == 2 || (st_here == 1 && rdf(r + R_HEAT) > 0)) && me && me < w[i].from) continue;
        cell_put(r, &w[i].c);
        // (a copy takes no heat of this game's: what touched it here since the last driver call is taken off, or a
        // copy held short of igniting would be lit by a fire of this game's beside it)
        uint16_t none = 0;
        wr(r + R_FIRE, &none, 2);
        now_copy[made] = r, now_burns[made++] = rd8(r + R_STATE) == 2;
    }
    for (int i = 0; i < made; i++) {
        atomic_store_explicit(&copy_rec[i], now_copy[i], memory_order_relaxed);
        // (only a burning copy has a fire to keep from the ground: its place is far off otherwise)
        copy_at[i][0] = now_burns[i] ? rdf(now_copy[i] + R_CENTRE) : 1e9f, copy_at[i][1] = rdf(now_copy[i] + R_CENTRE + 8);
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
// ---- lightning (D 1.4, S 5.6) ---------------------------------------------------------------------------------------
// A strike is an event, not a state: the game it falls in says where, and the others make the same strike there with
// the game's own function. The strike's blast is an actor from a pool ("LightningStrike") that burns and shocks what
// is near: in a game that only shows the strike that actor is inert for a few seconds, so that what the real strike
// lit is stated by its game and nothing is lit twice.
#define STRIKES 8
static struct { float pos[3]; uint8_t level, sage; } strike_out[STRIKES];   // this game's strikes not yet sent (under lk)
static int strike_out_n;
static struct { float pos[3]; uint32_t frame; } strike_shown[STRIKES];      // strikes of other games made here
static _Atomic uint32_t strike_shown_frame;                                 // the frame of the last one (0: none yet)
static _Atomic uint32_t n_strikes_mine, n_strikes_shown, n_src_drop;
static _Atomic uint64_t my_link;                                            // this game's Link, as phase 0 last saw it
static _Atomic uint64_t tm_seen;                                            // the thunder manager, as a strike's hook saw it
static __thread int strike_showing;                                         // inside this file's own call of the strike
static int strike_blast(uint64_t actor) {   // the blast actor of a strike this game only shows
    uint32_t last = atomic_load_explicit(&strike_shown_frame, memory_order_relaxed), frame = coop_sess_frame_no();
    char nm[48];
    float p[3];
    if (!last || frame - last > 150 || strncmp(coop_actor_name(actor, nm, sizeof nm), "LightningStrike", 15) || !coop_rd(actor + COOP_ACTOR_POS, p, 12)) return 0;
    for (int i = 0; i < STRIKES; i++)
        if (frame - strike_shown[i].frame <= 150 && fabsf(p[0] - strike_shown[i].pos[0]) < 3 && fabsf(p[2] - strike_shown[i].pos[2]) < 3) return 1;
    return 0;
}
// An actor that is shown only: named to coop_elem_inert, a projectile of an enemy another game runs, a shown strike's blast.
static _Atomic uint64_t standins[COOP_MAX_PLAYERS];   // the other players' stand-in Links here, as phase 0 last saw them
static int is_standin(uint64_t actor) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) if (atomic_load_explicit(&standins[i], memory_order_relaxed) == actor) return 1;
    return 0;
}
// A strike of this game's own aimed at another player (S 5.6): that player's id, else 0; *dist how far from its
// stand-in. A strike with an element (the component route, main+0x21c4358: what the element belongs to asked for it)
// is that player's when the element's actor is the stand-in or something it holds (its owner object +0x60, that
// object's component +0x10, the component's actor +0x18, checked back). A strike without one (the storm's, main+0x21c3a6c
// and main+0x21c4074), or one whose actor is not found, is that player's when it falls within STRIKE_NEAR of a stand-in
// and nearer it than this game's Link: the ground around a player is that player's own storm's.
#define STRIKE_NEAR 12.0f
static _Atomic uint32_t n_strikes_theirs;
static uint64_t comp_of(uint64_t actor);
static int strike_theirs(uint64_t el, const float p[3], float *dist) {
    int coop_standin_holds(uint64_t actor);
    if (el) {
        uint64_t o = coop_rd_ptr(el + 0x60), comp = o ? coop_rd_ptr(o + 0x10) : 0, actor = comp ? coop_rd_ptr(comp + 0x18) : 0;
        if (actor && comp_of(actor) == comp) { *dist = -1; return coop_standin_holds(actor); }
    }
    uint64_t link = atomic_load_explicit(&my_link, memory_order_relaxed);
    float l[3], s[3];
    if (!link || !coop_rd(link + COOP_ACTOR_POS, l, 12)) return 0;
    float dl = hypotf(p[0] - l[0], p[2] - l[2]);
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        uint64_t a = atomic_load_explicit(&standins[i], memory_order_relaxed);
        if (!a || !coop_rd(a + COOP_ACTOR_POS, s, 12)) continue;
        float d = hypotf(p[0] - s[0], p[2] - s[2]);
        if (d < STRIKE_NEAR && d < dl) {
            int who = coop_standin_holds(a);
            if (who) { *dist = d; return who; }
        }
    }
    return 0;
}
static int shown_only(uint64_t actor) {
    int coop_enemy_shot_mirror(uint64_t proj);
    return is_inert(actor) || is_standin(actor) || strike_blast(actor) || coop_enemy_shot_mirror(actor);
}
// The actor behind a chemical object (a reaction, a ground contact), 0 none: object -> source -> owner -> component.
static uint64_t object_actor(uint64_t obj) {
    uint64_t q = coop_rd_ptr(obj) == MOD_MAIN_BASE + VT_OBJECT ? coop_rd_ptr(obj + 0x60) : 0;
    uint64_t o = q && coop_rd_ptr(q) == MOD_MAIN_BASE + VT_SOURCE ? coop_rd_ptr(q + 0x60) : 0, comp = o ? coop_rd_ptr(o + 0x10) : 0;
    uint64_t actor = comp ? coop_rd_ptr(comp + 0x18) : 0, t = actor ? coop_rd_ptr(actor + 0x228) : 0;
    return t && coop_rd_ptr(t + 0x180) == comp ? actor : 0;
}
static uint64_t comp_of(uint64_t actor) {
    uint64_t t = actor ? coop_rd_ptr(actor + 0x228) : 0;
    return t ? coop_rd_ptr(t + 0x180) : 0;
}

// A placed actor's name across games: its placement hash (0: it has none, or not yet).
static uint64_t hash_of(uint64_t actor) {
    uint64_t p = coop_rd_ptr(actor + 0x370), h = 0;
    return p && coop_rd(p + 0x88, &h, 8) && h != ~0ull ? h : 0;
}
// A thing's name across games: an enemy the enemy sync knows has that sync's key (the placement hash and a life
// number, or for one made at run time the spawn sync's key), folded into one number; another placed actor its
// placement hash. 0: none yet (an enemy made at run time has no key for its first 2 s).
static uint64_t name_of(uint64_t actor, int enemy) {
    int coop_enemy_key(uint64_t actor, uint64_t *a, uint32_t *b);
    uint64_t a = 0;
    uint32_t b = 0;
    if (enemy) return coop_enemy_key(actor, &a, &b) ? (a ^ (uint64_t)b * 0x9e3779b97f4a7c15ull) | 1 : 0;
    return hash_of(actor);
}
// ---- things (S 5) ------------------------------------------------------------------------------------------------
#define THING_N 1024
#define EL_MAX 7
typedef struct {
    uint64_t comp, actor, hash;
    uint32_t ended[EL_MAX];       // the frame a copy's element last stopped burning or thawed
    uint32_t seen, first, differs_frame, asked[EL_MAX];   // frames: last visit, first visit, last frame an element differed from rest, last request
    uint8_t n, cur[EL_MAX], rest[EL_MAX], wanted[EL_MAX];
    uint8_t want_from;            // the player whose statement this game follows for it (0: none)
    uint8_t enemy, skip;
    uint8_t cond, cond_wanted;    // an enemy's condition (burn 1, shock 2, wet 4, frozen 8) as read; as its owner states it
    uint32_t cond_asked, cond_ended;   // the frames its shock was last written, and last ended
    uint8_t ice, ice_wanted, ice_put;   // an enemy's ice look (0..255) as read; as its owner states it; whether this game wrote it
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
// Test (TOTK_COOP_ELEM_TEST_LOG=1): what the element's update reads when it decides on a request.
static void el_dump(const char *when, uint64_t comp, uint64_t el) {
    static int log = -1;
    if (log < 0) log = cfg_get("TOTK_COOP_ELEM_TEST_LOG") != NULL;
    if (!log) return;
    uint8_t b[16] = {0}, ob[8] = {0};
    uint16_t f90 = 0, f92 = 0;
    uint32_t f94 = 0, f8 = 0;
    uint64_t word = 0, o = coop_rd_ptr(el + 0x60), o28 = 0;
    int8_t idx = 0;
    coop_rd(el + 0x7c, b, 16), coop_rd(el + 0x90, &f90, 2), coop_rd(el + 0x92, &f92, 2), coop_rd(el + 0x94, &f94, 4), coop_rd(el + 8, &f8, 4);
    coop_rd(comp + 0x20, &word, 8), coop_rd(o + 0x28, &o28, 8), coop_rd(o + 0x50, ob, 8), coop_rd(el + 0x251, &idx, 1);
    uint64_t rec = el + 0xc0 + (idx == 1 ? 200 : 0);
    char an[48];
    rt_log("coop: elem: dump %s: %s hash %llx el %llx class %llx: bytes 7c.. %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; "
           "+8 %x, +90 %x, +92 %x, +94 %x; comp word %llx; owner %llx class %llx +28 %llx +50 %02x %02x %02x %02x; record flags %x, burn %.1f, wet %.2f, +94 %.1f, +98 %.1f",
           when, coop_actor_name(coop_rd_ptr(comp + 0x18), an, sizeof an), (unsigned long long)hash_of(coop_rd_ptr(comp + 0x18)), (unsigned long long)el,
           (unsigned long long)(coop_rd_ptr(el) - MOD_MAIN_BASE), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
           f8, f90, f92, f94, (unsigned long long)word, (unsigned long long)o, (unsigned long long)(o ? coop_rd_ptr(o) - MOD_MAIN_BASE : 0), (unsigned long long)o28,
           ob[0], ob[1], ob[2], ob[3], (unsigned)coop_rd_ptr(rec + 8) & 0xffffffffu, rdf(rec + 0xbc), rdf(rec + 0x68), rdf(rec + 0x94), rdf(rec + 0x98));
}
// An enemy's ice look: a float of its model controller [[actor+0x228]+0x280] at +0x4c, set by the behaviour nodes of
// a frozen enemy through main+0x1b87a9c (which also sets bit 0x1000 of the controller's word +0xac). A copy runs no
// such node, so its owner states the amount and the copy is written.
static uint8_t ice_read(uint64_t actor) {
    uint64_t t = coop_rd_ptr(actor + 0x228), gmc = t ? coop_rd_ptr(t + 0x280) : 0;
    float f = gmc ? rdf(gmc + 0x4c) : 0;
    return (uint8_t)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255);
}
static void ice_write(uint64_t actor, uint8_t amount) {
    uint64_t t = coop_rd_ptr(actor + 0x228), gmc = t ? coop_rd_ptr(t + 0x280) : 0;
    float f = amount / 255.0f;
    uint32_t w = 0;
    if (!gmc || !coop_rd(gmc + 0xac, &w, 4)) return;
    w |= 0x1000;
    wr(gmc + 0x4c, &f, 4), wr(gmc + 0xac, &w, 4);
}
// An enemy's conditions: its condition component [[actor+0x228]+0x1a8] has slots (count +0x140, 0x100 bytes each from
// [+0x148]) whose state word +0x2c says burn (bit 0), shock (1), wet (2), frozen (3). The slot update (main+0x322450)
// takes burn, wet and frozen from the elements every frame, so a copy whose elements are given their state shows those.
// Shock is different: it is set once, when an element's record carries the mark of an electric reaction of that frame
// (main+0x326434), and then lasts by the slot's own timer. A copy takes no reaction, so its owner states the condition
// and the copy's slots are written as that code writes them.
static uint8_t cond_read(uint64_t actor) {
    uint64_t t = coop_rd_ptr(actor + 0x228), cc = t ? coop_rd_ptr(t + 0x1a8) : 0, slots = cc ? coop_rd_ptr(cc + 0x148) : 0;
    uint32_t n = 0, w = 0, all = 0;
    if (!slots || !coop_rd(cc + 0x140, &n, 4)) return 0;
    for (uint32_t i = 0; i < n && i < 8; i++) if (coop_rd(slots + i * 0x100 + 0x2c, &w, 4)) all |= w;
    return (uint8_t)(all & 0xf);
}
static void shock_write(uint64_t actor, int on) {
    uint64_t t = coop_rd_ptr(actor + 0x228), cc = t ? coop_rd_ptr(t + 0x1a8) : 0, slots = cc ? coop_rd_ptr(cc + 0x148) : 0;
    uint32_t n = 0;
    if (!slots || !coop_rd(cc + 0x140, &n, 4)) return;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint64_t sl = slots + i * 0x100;
        uint32_t ev = 0, st = 0, ctl = 0, timer = 0;
        if (!coop_rd(sl + 0x28, &ev, 4) || !coop_rd(sl + 0x2c, &st, 4) || !coop_rd(sl + 0x24, &ctl, 4)) continue;
        if (on) {   // (as main+0x326464: the state, the one-shot event, and the timer from its length)
            coop_rd(sl + 0xa8, &timer, 4);
            ev |= 0x40, st |= 2;
            wr(sl + 0xac, &timer, 4), wr(sl + 0x28, &ev, 4), wr(sl + 0x2c, &st, 4);
        } else if (st & 2) {   // (as main+0x3264f0: the state off, the control bit and the rest timer)
            coop_rd(sl + 0xb0, &timer, 4);
            st &= ~2u, ctl |= 8;
            wr(sl + 0x2c, &st, 4), wr(sl + 0x24, &ctl, 4), wr(sl + 0xb4, &timer, 4);
        }
    }
}
// What a copy's element is asked to become (S 5.3): 0 nothing, else an ASK_*; and the asking, with the game's own
// requests.
enum { ASK_IGNITE = 1, ASK_BURN_OUT, ASK_FREEZE, ASK_PUT_OUT, ASK_THAW, ASK_CHARGE = 8 };
static int ask_of(uint8_t cur, uint8_t wanted) {
    if (cur == EL_NONE || wanted == EL_NONE) return 0;
    int w = wanted & 7, is = cur & 7, what = 0;
    // (a burnt-out or detonated element is at its end: a statement a moment old that still says it burns asks nothing)
    if (is == 3 || is == 4) return 0;
    if (w != is) what = w == 2 ? ASK_IGNITE : w == 3 ? ASK_BURN_OUT : w == 1 ? ASK_FREEZE : w == 0 && is == 2 ? ASK_PUT_OUT : w == 0 && is == 1 ? ASK_THAW : 0;
    if (wanted & EL_ELEC && !(cur & EL_ELEC)) what |= ASK_CHARGE;
    return what;
}
static void ask_do(Cpu *c, uint64_t comp, uint64_t el, int i, int what) {
    int k = what & 7;
    {
        static const char *nm[8] = {"", "ignite", "burn out", "freeze", "go out", "thaw", "", ""};
        char an[48];
        uint16_t f90 = 0;
        uint64_t word = 0;
        coop_rd(el + 0x90, &f90, 2), coop_rd(comp + 0x20, &word, 8);
        // (the game's setters drop a request when bit 0 of the element's word +0x90 is clear: said here, for a copy
        // that does not take one)
        rt_log("coop: elem: asks %s (hash %llx) element %d to %s%s (element flags %x, component word %llx)", coop_actor_name(coop_rd_ptr(comp + 0x18), an, sizeof an),
               (unsigned long long)hash_of(coop_rd_ptr(comp + 0x18)), i, nm[k], what & ASK_CHARGE ? " and take a charge" : "", f90, (unsigned long long)word);
    }
    if (k == ASK_IGNITE) coop_guest_call(c, MOD_MAIN_BASE + FN_IGNITE, comp, 2, (uint64_t)i, 0);
    else if (k == ASK_BURN_OUT) coop_guest_call(c, MOD_MAIN_BASE + FN_REQ_B, comp, 2, (uint64_t)i, 0);
    else if (k == ASK_FREEZE) coop_guest_call(c, MOD_MAIN_BASE + FN_FREEZE, comp, 4, (uint64_t)i, 0);
    else if (k == ASK_THAW) coop_guest_call(c, MOD_MAIN_BASE + FN_THAW, comp, 0, (uint64_t)i, 0);
    else if (k == ASK_PUT_OUT) coop_guest_call(c, MOD_MAIN_BASE + FN_REQ_B, comp, 0, (uint64_t)i, 0);
    if (what & ASK_CHARGE) coop_guest_call(c, MOD_MAIN_BASE + FN_CHARGE, el + 0x50, 0, 0, 0);
    atomic_fetch_add(&n_req, 1);
    el_dump("after the request", comp, el);
}
static void thing_said(uint64_t actor, int i, uint8_t before, uint8_t now, int copy) {
    static const char *kind[8] = {"normal", "frozen", "burning", "burnt out", "detonated", "?", "?", "?"};
    char nm[48];
    if (before & EL_NONE) return;
    rt_log("coop: elem: thing %s (hash %llx) element %d: %s%s%s -> %s%s%s (%s), at %llu ms", coop_actor_name(actor, nm, sizeof nm),
           (unsigned long long)hash_of(actor), i, kind[before & 7], before & EL_ELEC ? ", electrified" : "", before & EL_WET ? ", wet" : "",
           now & EL_NONE ? "off" : kind[now & 7], now & EL_ELEC ? ", electrified" : "", now & EL_WET ? ", wet" : "",
           copy ? "a copy" : "mine", (unsigned long long)(plat_now_ns() / 1000000));
}
// This game's own Link as its component's update last read it (under lk): stated to the others, who show it on
// their stand-in of this player (S 5.7).
static int link_n;
static uint8_t link_cur[EL_MAX], link_ice, link_cond;
static uint32_t link_frame;
// A component's update: the thing is read; a copy is given what its owner states and its elements are closed to
// reactions (S 5.3, 5.4).
static int comp_hook(Cpu *c) {
    uint64_t comp = c->x[0], actor = coop_rd_ptr(comp + 0x18), els[EL_MAX];
    uint8_t cur[EL_MAX], wanted[EL_MAX];
    if (!actor) return 0;
    uint32_t frame = coop_sess_frame_no();
    uint8_t before[EL_MAX] = {0};
    int n = el_read(comp, els, cur), copy = 0, ask = 0, changed = 0, what[EL_MAX] = {0}, ice_set = 0, shock_set = 0, cond_said = 0, link_changed = 0;
    uint8_t ice_now = 0, cond_was = 0, cond_is = 0;
    if (!n) return 0;
    // (a projectile shot by an enemy another game runs is the copy's own actor, made by the same calls as the owner's
    // (coop_enemy_shot.c): it is shown only, so it is inert here as an actor named to coop_elem_inert is)
    int coop_enemy_shot_mirror(uint64_t proj);
    int mirror = coop_enemy_shot_mirror(actor), in = mirror || is_inert(actor) || strike_blast(actor), es = in ? EM_FREE : en_cell_state(actor);
    {   // Test (TOTK_COOP_ELEM_TEST_LOG=1): each kind of actor without a placement hash whose component is updated, once,
        // with what the enemy sync says of it as a projectile
        static int log = -1;
        static _Atomic uint32_t seen[64];
        if (log < 0) log = cfg_get("TOTK_COOP_ELEM_TEST_LOG") != NULL;
        if (log && !hash_of(actor) && coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x440)) {
            char nm[48];
            uint32_t h = 2166136261u;
            for (const char *q = coop_actor_name(actor, nm, sizeof nm); *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
            h = (h | 1) ^ (uint32_t)mirror ^ (uint32_t)(frame / 150 * 2);
            for (int i = 0; i < 64; i++) {
                uint32_t v = atomic_load(&seen[i]);
                if (v == h) break;
                if (!v) {
                    uint64_t sc = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x440);
                    atomic_store(&seen[i], h);
                    uint64_t ref = sc ? coop_rd_ptr(sc + 0x78) : 0, sh = ref ? coop_rd_ptr(ref + 0x40) : 0;
                    char sn[48];
                    int coop_enemy_role(uint64_t a);
                    uint64_t coop_enemy_holder(uint64_t a);
                    uint64_t hd = sh ? coop_enemy_holder(sh) : 0;
                    char hn[48];
                    rt_log("coop: elem: test: component of %s updated: mirror %d, shooter %llx (%s), role %d; the shooter's holder %llx (%s), cell state %d, role %d",
                           nm, mirror, (unsigned long long)sh, sh ? coop_actor_name(sh, sn, sizeof sn) : "", sh ? coop_enemy_role(sh) : -1,
                           (unsigned long long)hd, hd ? coop_actor_name(hd, hn, sizeof hn) : "", hd ? en_cell_state(hd) : -1, hd ? coop_enemy_role(hd) : -1);
                    (void)sc, (void)ref;
                    break;
                }
            }
        }
    }
    {   // The game's own "does not interact with the world" bit is NOT set on a mirror's component (enemy-sync.md
        // B.1): the component's update turns it into the bit the element step reads to skip its whole contact step, this
        // game's Link included, and a copy's flame or fireball then burned no player in any game (a play session,
        // 2026-11-16). A mirror is kept from the world by the reaction hook (it reaches this game's Link alone) and the
        // ground contact hook. TOTK_COOP_ELEM_TEST_MIRRORBIT=1 sets the bit as before, to compare. A bit set earlier
        // is taken off when the actor is no mirror any more: projectiles are made once and used again.
        static _Atomic uint64_t marked[64];
        static int bit = -1;
        if (bit < 0) bit = cfg_get("TOTK_COOP_ELEM_TEST_MIRRORBIT") != NULL;
        uint64_t word;
        uint32_t k = (uint32_t)((comp >> 4) * 0x9e3779b1u) & 63;
        int mine = atomic_load_explicit(&marked[k], memory_order_relaxed) == comp;
        if (bit && mirror && coop_rd(comp + 0x20, &word, 8) && !(word >> 32 & 1)) {
            atomic_fetch_or((_Atomic uint64_t *)(uintptr_t)(comp + 0x20), 1ull << 32);
            atomic_store_explicit(&marked[k], comp, memory_order_relaxed);
            atomic_fetch_add(&n_mirrors, 1);
        } else if (!mirror && mine && !is_inert(actor)) {
            atomic_fetch_and((_Atomic uint64_t *)(uintptr_t)(comp + 0x20), ~(1ull << 32));
            atomic_store_explicit(&marked[k], 0, memory_order_relaxed);
        }
    }
    plat_mutex_lock(&lk);
    Thing *t = thing_row(comp, actor, frame);
    if (t) {
        if (t->n == 0xff) {   // first sight: its name across games, and its state as the rest state
            memcpy(t->rest, cur, sizeof cur);
            t->skip = actor == coop_player_actor(), t->first = frame;
        }
        // (its first second settles what its rest state is: a brazier lit by its own data starts unlit)
        if (frame - t->first < 30 && !t->want_from) memcpy(t->rest, cur, sizeof cur);
        for (int i = 0; i < n && t->n == n && t->hash; i++) if (t->cur[i] != cur[i]) {
            changed |= 1 << i, before[i] = t->cur[i];
            if ((t->cur[i] & 7) && !(cur[i] & 7)) t->ended[i] = frame ? frame : 1;
        }
        if (!t->hash || t->enemy != (es != EM_FREE)) t->hash = name_of(actor, es != EM_FREE);
        // (asked at every visit: at an actor's first one the player may not be there yet)
        t->skip = actor == atomic_load_explicit(&my_link, memory_order_relaxed);
        if (t->skip) {
            for (int i = 0; i < n && link_n == n; i++) if (link_cur[i] != cur[i]) link_changed |= 1 << i, before[i] = link_cur[i];
            link_n = n, memcpy(link_cur, cur, sizeof link_cur), link_ice = ice_read(actor), link_cond = cond_read(actor), link_frame = frame;
        }
        t->n = (uint8_t)n, t->seen = frame, t->enemy = es != EM_FREE;
        memcpy(t->cur, cur, sizeof cur);
        coop_rd(actor + COOP_ACTOR_POS, t->pos, 12);
        // (wetness is stated but is no claim: rain in one game would make every wet thing there a copy elsewhere)
        for (int i = 0; i < n; i++) if ((cur[i] ^ t->rest[i]) & ~EL_WET) t->differs_frame = frame;
        if (t->enemy) {
            uint8_t cond = cond_read(actor);
            if (cond != t->cond) cond_was = t->cond, cond_is = cond, cond_said = 1;
            if ((t->cond & 2) && !(cond & 2)) t->cond_ended = frame ? frame : 1;
            t->cond = cond;
            // (a copy: shocked when its owner's is and it is not, at most once a second; let go of it when the owner's ended)
            // (and not back into a shock that just ended here by its own timer: the statement that still says so is a
            // moment old)
            if (es == EM_COPY && t->want_from && ((t->cond_wanted ^ cond) & 2) && frame - t->cond_asked >= 30 &&
                !((t->cond_wanted & 2) && t->cond_ended && frame - t->cond_ended < 30))
                shock_set = t->cond_wanted & 2 ? 1 : -1, t->cond_asked = frame;
            t->ice = ice_read(actor);
            if (es == EM_COPY && (t->want_from ? t->ice_wanted : 0) != t->ice && (t->want_from || t->ice_put))
                ice_now = t->want_from ? t->ice_wanted : 0, ice_set = 1, t->ice_put = ice_now != 0;
        }
        copy = in || es == EM_COPY || (!t->enemy && t->want_from);
        if (es == EM_COPY && !t->want_from) memset(t->wanted, 0, sizeof t->wanted);   // an enemy copy nobody states is at rest
        ask = (es == EM_COPY || t->want_from) && !in;
        memcpy(wanted, t->wanted, sizeof wanted);
        if (ask) for (int i = 0; i < n; i++) {
            what[i] = ask_of(cur[i], wanted[i]);
            // (an element that just stopped burning or thawed by itself is not asked back into it for a second: the
            // statement that still says so is a moment old, and the copy would burn again until the next one)
            if ((what[i] & 7) == ASK_IGNITE || (what[i] & 7) == ASK_FREEZE) if (t->ended[i] && frame - t->ended[i] < 30) what[i] &= ~7;
            if (what[i] && frame - t->asked[i] < 30) what[i] = 0;
            if (what[i]) t->asked[i] = frame;
        }
    }
    plat_mutex_unlock(&lk);
    if (copy) for (int i = 0; i < n; i++) eset_put(els[i], frame);
    if (ice_set) ice_write(actor, ice_now);
    if (shock_set) shock_write(actor, shock_set > 0), atomic_fetch_add(&n_req, 1);
    if (cond_said) {
        char nm[48];
        rt_log("coop: elem: enemy %s condition %x -> %x (burn 1, shock 2, wet 4, frozen 8; %s%s), at %llu ms", coop_actor_name(actor, nm, sizeof nm), cond_was, cond_is,
               es == EM_COPY ? "a copy" : "mine", shock_set ? shock_set > 0 ? ", shock written" : ", shock taken off" : "", (unsigned long long)(plat_now_ns() / 1000000));
    }
    for (int i = 0; i < n; i++) if ((changed | link_changed) >> i & 1) thing_said(actor, i, before[i], cur[i], copy);
    if (!ask || coop_ctn_driven(actor) || coop_ctn_follows(actor)) return 0;
    for (int i = 0; i < n; i++) if (what[i]) ask_do(c, comp, els[i], i, what[i]);
    return 0;
}

int coop_elem_hook(Cpu *c, uint64_t off) {
    if (!elem_on()) return 0;
    if (off == FN_REACT) {
        if (eset_has(c->x[0])) { atomic_fetch_add_explicit(&n_react_drop, 1, memory_order_relaxed); return 1; }
        // A reaction FROM an actor that is shown only (a mirrored projectile, a shown strike's blast, an inert actor)
        // reaches nothing but this game's own Link: each game decides the hits on its own player, and what the real
        // one did to anything else is stated by its game. Asked per component at most once a frame.
        static struct { _Atomic uint64_t comp; _Atomic uint32_t stamp; } memo[64];
        uint64_t r = c->x[3], q = coop_rd_ptr(r) == MOD_MAIN_BASE + VT_OBJECT ? coop_rd_ptr(r + 0x60) : 0;
        uint64_t o = q && coop_rd_ptr(q) == MOD_MAIN_BASE + VT_SOURCE ? coop_rd_ptr(q + 0x60) : 0, comp = o ? coop_rd_ptr(o + 0x10) : 0;
        if (!comp) return 0;
        uint32_t k = (uint32_t)((comp >> 4) * 0x9e3779b1u) & 63, frame = coop_sess_frame_no(), stamp = atomic_load_explicit(&memo[k].stamp, memory_order_relaxed);
        int shown;
        if (atomic_load_explicit(&memo[k].comp, memory_order_relaxed) == comp && stamp >> 1 == (frame & 0x7fffffff)) shown = (int)(stamp & 1);
        else {
            uint64_t actor = coop_rd_ptr(comp + 0x18);
            shown = actor && comp_of(actor) == comp && shown_only(actor);
            atomic_store_explicit(&memo[k].stamp, frame << 1 | (uint32_t)shown, memory_order_relaxed);
            atomic_store_explicit(&memo[k].comp, comp, memory_order_relaxed);
        }
        if (!shown) return 0;
        uint64_t to = coop_rd_ptr(c->x[0] + 0x60), tc = to ? coop_rd_ptr(to + 0x10) : 0, link = atomic_load_explicit(&my_link, memory_order_relaxed);
        if (link && tc && coop_rd_ptr(tc + 0x18) == link) return 0;
        atomic_fetch_add_explicit(&n_src_drop, 1, memory_order_relaxed);
        return 1;
    }
    if (off == FN_STRIKE) {   // a strike of this game's own (the weather's, an arrow's, a node's): told to the others
        float p[3], d = 0;
        if (strike_showing || !coop_rd(c->x[1], p, 12)) return 0;
        int who = strike_theirs(c->x[4], p, &d);
        if (who) {   // aimed at another player: that player's game decides it (S 5.6); not made here, not sent
            static _Atomic int said;
            atomic_fetch_add_explicit(&n_strikes_theirs, 1, memory_order_relaxed);
            if (atomic_fetch_add(&said, 1) < 40)
                rt_log("coop: elem: lightning at %.1f %.1f %.1f is aimed at player %d (%s %.1f m): not made here, at %llu ms", p[0], p[1], p[2], who,
                       d < 0 ? "its stand-in or what it holds asked for it," : "nearer its stand-in than this Link, at", d < 0 ? 0.0f : d,
                       (unsigned long long)(plat_now_ns() / 1000000));
            return 1;
        }
        plat_mutex_lock(&lk);
        if (strike_out_n < STRIKES) memcpy(strike_out[strike_out_n].pos, p, 12), strike_out[strike_out_n].level = (uint8_t)c->x[2], strike_out[strike_out_n++].sage = (uint8_t)(c->x[3] & 1);
        plat_mutex_unlock(&lk);
        atomic_store_explicit(&tm_seen, c->x[0], memory_order_relaxed);
        return 0;
    }
    if (off == FN_COMP_UPDATE) return comp_hook(c);
    if (off == FN_CONTACT) {
        // What touches the ground is a chemical object an element's update makes (class main+0x3625660). Its +0x60 is
        // its source (class main+0x3626048 in the runs), whose +0x60 is an owner object with the component at +0x10.
        // When that leads to an actor, the actor says: a mirrored projectile, an inert actor and a copied enemy do not
        // reach the cells (S 4.5); what the real one lights is stated by its game. A burning cell's own fire leads to
        // no actor and is told by its place, as is the warm air over it (class main+0x36295d0): an object centred on
        // a copied, burning cell is that cell's.
        uint64_t obj = c->x[3], actor = object_actor(obj), vt = coop_rd_ptr(coop_rd_ptr(obj + 0x60));
        int drop = 0, how = 3;
        if (actor) drop = shown_only(actor) || en_cell_state(actor) == EM_COPY, how = 2;
        else {
            float at[3] = {0, 0, 0};
            int n = atomic_load_explicit(&copy_n, memory_order_relaxed);
            actor = 0;
            if (n && coop_rd(c->x[2] + 8, at, 12))
                for (int i = 0; i < n && i < CELLS_MAX && !drop; i++) drop = fabsf(at[0] - copy_at[i][0]) < 0.25f && fabsf(at[2] - copy_at[i][1]) < 0.25f;
        }
        {   // Test (TOTK_COOP_ELEM_TEST_LOG=1): each way an object was told, once per element class and answer
            static int log = -1;
            static _Atomic uint64_t seen[16];
            uint64_t key = ((vt ? vt : coop_rd_ptr(obj)) ^ (actor ? coop_rd_ptr(actor + COOP_ACTOR_NAME) * 31 : 0)) * 8 + (uint64_t)how * 2 + (uint64_t)drop;
            if (log < 0) log = cfg_get("TOTK_COOP_ELEM_TEST_LOG") != NULL;
            for (int i = 0; log && i < 16; i++) {
                uint64_t v = atomic_load(&seen[i]);
                if (v == key) break;
                if (!v) {
                    atomic_store(&seen[i], key);
                    char an[48];
                    rt_log("coop: elem: test: ground contact by an object of class main+%llx, source class main+%llx: told %s (%s), %s",
                           (unsigned long long)(coop_rd_ptr(obj) - MOD_MAIN_BASE), (unsigned long long)(vt ? vt - MOD_MAIN_BASE : 0),
                           how == 2 ? "by its actor" : "by its place", actor ? coop_actor_name(actor, an, sizeof an) : "no actor", drop ? "kept from the ground" : "let through");
                    break;
                }
            }
        }
        {   // Test (TOTK_COOP_ELEM_TEST_CONTACTS=1): what reaches the ground, by actor, at most once a second each
            static int log = -1;
            static struct { _Atomic uint64_t actor; _Atomic uint32_t frame; } said[32];
            if (log < 0) log = cfg_get("TOTK_COOP_ELEM_TEST_CONTACTS") != NULL;
            if (log && !drop) {
                uint64_t key = actor ? actor : obj;
                uint32_t k = (uint32_t)((key >> 4) * 0x9e3779b1u) & 31, frame = coop_sess_frame_no();
                if (atomic_load(&said[k].actor) != key || frame - atomic_load(&said[k].frame) >= 30) {
                    char an[48];
                    float at[3] = {0, 0, 0};
                    coop_rd(c->x[2] + 8, at, 12);
                    atomic_store(&said[k].actor, key), atomic_store(&said[k].frame, frame);
                    rt_log("coop: elem: test: reaches the ground at %.1f %.1f %.1f: %s (object class main+%llx), at %llu ms", at[0], at[1], at[2],
                           actor ? coop_actor_name(actor, an, sizeof an) : "no actor", (unsigned long long)(coop_rd_ptr(obj) - MOD_MAIN_BASE),
                           (unsigned long long)(plat_now_ns() / 1000000));
                }
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
// Test (TOTK_COOP_ELEM_TEST_ENEMY=<s>,<ignite|freeze|charge|thaw|out>[/<s>,<what>...]): at each of those world times
// the first game asks the nearest enemy it decides for, within 40 m of its Link, with the game's own request.
static void test_enemy(Cpu *c) {
    static struct { double at; char what[12]; int done; } step[6];
    static int n = -1;
    if (n < 0) {
        const char *e = cfg_get("TOTK_COOP_ELEM_TEST_ENEMY");
        for (n = 0; e && *e && n < 6; n++) {
            step[n].at = atof(e);
            const char *k = strchr(e, ','), *end = strchr(e, '/');
            if (!k) break;
            snprintf(step[n].what, sizeof step[n].what, "%.*s", (int)((end ? end : k + strlen(k)) - k - 1), k + 1);
            e = end ? end + 1 : "";
        }
    }
    double t = frame_step_world_us() / 1e6;
    uint64_t link = coop_player_actor();
    float p[3];
    for (int i = 0; i < n; i++) {
        if (step[i].done || t < step[i].at || !link || !lowest_here() || !coop_rd(link + COOP_ACTOR_POS, p, 12)) continue;
        step[i].done = 1;
        uint64_t comp = 0, actor = 0;
        float bd = 40.0f * 40.0f;
        plat_mutex_lock(&lk);
        for (int k = 0; k < THING_N; k++) {
            Thing *th = &things[k];
            if (!th->comp || !th->enemy || th->n == 0xff || en_cell_state(th->actor) == EM_COPY || comp_of(th->actor) != th->comp) continue;
            coop_rd(th->actor + COOP_ACTOR_POS, th->pos, 12);
            float d = (th->pos[0] - p[0]) * (th->pos[0] - p[0]) + (th->pos[1] - p[1]) * (th->pos[1] - p[1]) + (th->pos[2] - p[2]) * (th->pos[2] - p[2]);
            if (d < bd) bd = d, comp = th->comp, actor = th->actor;
        }
        plat_mutex_unlock(&lk);
        char nm[48];
        rt_log("coop: elem: test: enemy %s, %.1f m away, asked to %s", actor ? coop_actor_name(actor, nm, sizeof nm) : "none within 40 m", actor ? sqrtf(bd) : 0.0f, step[i].what);
        if (!comp) continue;
        uint64_t els[EL_MAX];
        uint8_t cur[EL_MAX];
        int ne = el_read(comp, els, cur);
        if (!strcmp(step[i].what, "ignite")) coop_guest_call(c, MOD_MAIN_BASE + FN_IGNITE, comp, 2, (uint64_t)-1, 0);
        else if (!strcmp(step[i].what, "out")) coop_guest_call(c, MOD_MAIN_BASE + FN_REQ_B, comp, 0, (uint64_t)-1, 0);
        else if (!strcmp(step[i].what, "freeze")) coop_guest_call(c, MOD_MAIN_BASE + FN_FREEZE, comp, 4, (uint64_t)-1, 0);
        else if (!strcmp(step[i].what, "thaw")) coop_guest_call(c, MOD_MAIN_BASE + FN_THAW, comp, 0, (uint64_t)-1, 0);
        else if (!strcmp(step[i].what, "charge")) for (int k = 0; k < ne; k++) coop_guest_call(c, MOD_MAIN_BASE + FN_CHARGE, els[k] + 0x50, 0, 0, 0);
        for (int k = 0; k < ne; k++) el_dump("the test's enemy, after the request", comp, els[k]);
    }
}
static void tests(Cpu *c) {
    test_enemy(c);
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
        for (int i = 0; i < THING_N; i++) {
            Thing *th = &things[i];
            uint64_t els[EL_MAX];
            uint8_t cur[EL_MAX];
            char tn[48];
            if (!th->comp || !th->hash || !*prefix || strncmp(coop_actor_name(th->actor, tn, sizeof tn), prefix, strlen(prefix))) continue;
            if (el_read(th->comp, els, cur)) el_dump(th->comp == comp ? "the test lights this one" : "its like, not lit by the test", th->comp, els[0]);
        }
        char nm[48];
        rt_log("coop: elem: test: the nearest placed thing is %s, %.1f m away (%d rows, %d with a placement hash)",
               a ? coop_actor_name(a, nm, sizeof nm) : "none within 150 m", a ? sqrtf(bd) : 0.0f, rows, named);
    }
}

static int lowest_here(void);
static uint64_t thunder_mgr(void) {   // [[[main+0x395dc20]]+0xd0] the weather system, its +0x228 the thunder manager
    uint64_t wm = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395dc20)), w = wm ? coop_rd_ptr(wm + 0xd0) : 0;
    return w ? coop_rd_ptr(w + 0x228) : 0;
}
static void strikes_phase0(Cpu *c, const int *ids, int np, uint32_t frame) {
    static ElmStrike sent[STRIKES];           // this game's last strikes, sent again for a second
    static uint32_t sent_frame[STRIKES], serial, played[COOP_MAX_PLAYERS + 1];
    static uint8_t played_any[COOP_MAX_PLAYERS + 1];
    atomic_store(&my_link, coop_player_actor());
    plat_mutex_lock(&lk);
    for (int i = 0; i < strike_out_n; i++) {
        ElmStrike *m = &sent[++serial % STRIKES];
        memset(m, 0, sizeof *m);
        m->slot = serial % STRIKES, m->serial = serial, memcpy(m->pos, strike_out[i].pos, 12), m->level = strike_out[i].level, m->sage = strike_out[i].sage;
        sent_frame[serial % STRIKES] = frame ? frame : 1;
        atomic_fetch_add(&n_strikes_mine, 1);
        rt_log("coop: elem: lightning strikes here at %.1f %.1f %.1f (level %d%s): told to the others as strike %u, at %llu ms", m->pos[0], m->pos[1], m->pos[2], m->level,
               m->sage ? ", a sage's" : "", serial, (unsigned long long)(plat_now_ns() / 1000000));
    }
    strike_out_n = 0;
    plat_mutex_unlock(&lk);
    for (int i = 0; i < STRIKES; i++)   // (unreliable: each strike five times over a second, in a slot of its own)
        if (sent_frame[i] && frame - sent_frame[i] < 30 && (frame - sent_frame[i]) % 6 == 0) coop_ctr_send(0, ELM_STRIKE, 0, &sent[i], sizeof sent[i]);
    for (int p = 0; p < np; p++) {
        int id = ids[p];
        if (id < 1 || id > COOP_MAX_PLAYERS) continue;
        for (int pass = 0; pass < STRIKES; pass++) {   // the oldest one not yet made, then the next
            ElmStrike best = {0}, m;
            uint64_t age = 0;
            int have = 0;
            for (int k = 0; k < STRIKES; k++)
                if (coop_ctr_latest(id, ELM_STRIKE, (uint64_t)k, &m, sizeof m, &age) == (int)sizeof m && age < 2000000000ull &&
                    (!played_any[id] || (int32_t)(m.serial - played[id]) > 0) && (!have || (int32_t)(m.serial - best.serial) < 0)) best = m, have = 1;
            if (!have) break;
            played[id] = best.serial, played_any[id] = 1;
            uint64_t tm = thunder_mgr();
            if (!tm || coop_sess_loading()) continue;
            static _Alignas(16) float pos[4];
            memcpy(pos, best.pos, 12), pos[3] = 0;
            int k = (int)(atomic_fetch_add(&n_strikes_shown, 1) % STRIKES);
            memcpy(strike_shown[k].pos, best.pos, 12), strike_shown[k].frame = frame;
            atomic_store(&strike_shown_frame, frame ? frame : 1);
            strike_showing = 1;
            {   // (five arguments: the element is 0, as for the weather's own strikes)
                uint64_t x[31], sp = c->sp, pc = c->pc;
                memcpy(x, c->x, sizeof x);
                c->sp = (sp - 0x100) & ~0xfull;
                rt_call(c, MOD_MAIN_BASE + FN_STRIKE, 5, tm, (uint64_t)(uintptr_t)pos, (uint64_t)best.level, (uint64_t)best.sage, (uint64_t)0);
                memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
            }
            strike_showing = 0;
            rt_log("coop: elem: player %d's lightning strike %u shown at %.1f %.1f %.1f (level %d), at %llu ms", id, best.serial, best.pos[0], best.pos[1], best.pos[2],
                   best.level, (unsigned long long)(plat_now_ns() / 1000000));
        }
    }
    {   // Test (TOTK_COOP_ELEM_TEST_STRIKE=<s>[,<dx>,<dz>]): at that world time the first game makes a strike that far from its
        // Link. <s>,standin: at <s> a strike on the first stand-in here (S 5.6: not made, logged), at <s>+5 one 8 m from
        // this Link (made and shown as before)
        static double at = -2;
        static float dx = 8, dz;
        static int done, on_standin;
        if (at < -1) {
            const char *e = cfg_get("TOTK_COOP_ELEM_TEST_STRIKE"), *k = e ? strchr(e, ',') : NULL;
            at = e ? atof(e) : -1;
            if (k && !strncmp(k + 1, "standin", 7)) on_standin = 1;
            else if (k) { dx = (float)atof(k + 1); k = strchr(k + 1, ','); if (k) dz = (float)atof(k + 1); }
        }
        uint64_t link = coop_player_actor(), tm = thunder_mgr(), sa = atomic_load(&standins[0]);
        static _Alignas(16) float pos[4];
        if (on_standin == 1 && at >= 0 && frame_step_world_us() / 1e6 >= at && sa && tm && lowest_here() && coop_rd(sa + COOP_ACTOR_POS, pos, 12)) {
            on_standin = 2, at += 5;
            uint64_t x[31], sp = c->sp, pc = c->pc;
            memcpy(x, c->x, sizeof x);
            c->sp = (sp - 0x100) & ~0xfull;
            rt_log("coop: elem: test: a strike asked on the stand-in at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
            rt_call(c, MOD_MAIN_BASE + FN_STRIKE, 5, tm, (uint64_t)(uintptr_t)pos, (uint64_t)2, (uint64_t)0, (uint64_t)0);
            memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
        }
        if (on_standin != 1 && at >= 0 && !done && frame_step_world_us() / 1e6 >= at && link && tm && lowest_here() && coop_rd(link + COOP_ACTOR_POS, pos, 12)) {
            done = 1, pos[0] += dx, pos[2] += dz;
            uint64_t x[31], sp = c->sp, pc = c->pc;
            memcpy(x, c->x, sizeof x);
            c->sp = (sp - 0x100) & ~0xfull;
            rt_call(c, MOD_MAIN_BASE + FN_STRIKE, 5, tm, (uint64_t)(uintptr_t)pos, (uint64_t)2, (uint64_t)0, (uint64_t)0);
            memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
            rt_log("coop: elem: test: a strike asked at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
        }
    }
}

// Each player's own Link on the other games' stand-ins (S 5.7). A stand-in's update does not run, so its chemical
// component is updated from here, one frame's step, while it has something to show and for 2 s after; requests and
// the ice look as for any copy. A stand-in is shown only: it takes and gives no reaction and lights no ground.
static void links_phase0(Cpu *c, const int *ids, int np, uint32_t frame) {
    uint64_t coop_player_standin(int net_id);
    static uint32_t last_any, busy_until[COOP_MAX_PLAYERS + 1], asked[COOP_MAX_PLAYERS + 1][EL_MAX];
    static uint8_t shown[COOP_MAX_PLAYERS + 1][EL_MAX], ice_put[COOP_MAX_PLAYERS + 1];
    ElmLink m = {0};
    plat_mutex_lock(&lk);
    int fresh = link_n && frame - link_frame < 30;
    m.n = (uint8_t)(fresh ? link_n : 0), memcpy(m.el, link_cur, sizeof m.el), m.ice = link_ice, m.cond = link_cond;
    plat_mutex_unlock(&lk);
    int any = m.ice != 0;
    for (int i = 0; i < m.n; i++) any |= m.el[i] != EL_NONE && (m.el[i] & 0xf);
    if (any) last_any = frame ? frame : 1;
    if (fresh && last_any && frame - last_any < 60 && frame % 3 == 0 && !coop_sess_paused()) coop_ctr_send(0, ELM_LINK, 0, &m, sizeof m);
    {   // Test (TOTK_COOP_ELEM_TEST_LINK=<s>,<ignite|freeze|thaw|out>): at that world time the first game asks its own Link
        static double at = -2;
        static char what[12];
        static int done;
        if (at < -1) {
            const char *e = cfg_get("TOTK_COOP_ELEM_TEST_LINK"), *k = e ? strchr(e, ',') : NULL;
            at = e ? atof(e) : -1;
            if (k) snprintf(what, sizeof what, "%s", k + 1);
        }
        uint64_t link = coop_player_actor(), comp = link ? comp_of(link) : 0;
        if (at >= 0 && !done && frame_step_world_us() / 1e6 >= at && comp && lowest_here()) {
            done = 1;
            if (!strcmp(what, "ignite")) coop_guest_call(c, MOD_MAIN_BASE + FN_IGNITE, comp, 2, (uint64_t)-1, 0);
            else if (!strcmp(what, "freeze")) coop_guest_call(c, MOD_MAIN_BASE + FN_FREEZE, comp, 4, (uint64_t)-1, 0);
            else if (!strcmp(what, "thaw")) coop_guest_call(c, MOD_MAIN_BASE + FN_THAW, comp, 0, (uint64_t)-1, 0);
            else if (!strcmp(what, "out")) coop_guest_call(c, MOD_MAIN_BASE + FN_REQ_B, comp, 0, (uint64_t)-1, 0);
            rt_log("coop: elem: test: this game's Link asked to %s", what);
        }
    }
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) atomic_store(&standins[i], i < np ? coop_player_standin(ids[i]) : 0);
    for (int p = 0; p < np; p++) {
        int id = ids[p];
        uint64_t actor = id >= 1 && id <= COOP_MAX_PLAYERS ? coop_player_standin(id) : 0, comp = actor ? comp_of(actor) : 0, els[EL_MAX], age = 0;
        uint8_t cur[EL_MAX];
        ElmLink w = {0};
        int n = comp ? el_read(comp, els, cur) : 0, got = coop_ctr_latest(id, ELM_LINK, 0, &w, sizeof w, &age);
        {   // (said when what is known of a player's stand-in changes: no actor, no component, no elements, no statement)
            static int said[COOP_MAX_PLAYERS + 1];
            int state = 1 + (actor != 0) + (comp != 0) + (n != 0) + 8 * (got == (int)sizeof w);
            if (said[id] != state) said[id] = state, rt_log("coop: elem: player %d's stand-in: actor %llx, chemical component %llx, %d elements; its Link's statement: %s", id,
                                                           (unsigned long long)actor, (unsigned long long)comp, n, got == (int)sizeof w ? "taken" : "none yet");
        }
        if (!comp) continue;
        if (got != (int)sizeof w || age > 1000000000ull) memset(&w, 0, sizeof w);
        {   // A stand-in has no chemical elements (seen in a run: a component with an empty list), so it cannot burn by
            // itself: while its player's Link burns, the game's flame effect is kept on it, started, placed and stopped
            // with the calls a burning ground cell's fire uses (main+0x1f115f0, main+0x1f11548).
            static struct { uint8_t h[16]; int on; } fx[COOP_MAX_PLAYERS + 1];
            int burns = 0;
            for (int i = 0; i < w.n && i < EL_MAX; i++) burns |= w.el[i] != EL_NONE && (w.el[i] & 7) == 2;
            uint8_t *h = fx[id].h;
            float at[3];
            // (an empty handle: two parts of 8 bytes, each a pool byte, an index of -1 at +2 and a serial at +4; the
            // game's "is it playing" test main+0xb9b684 reads both)
            if (!fx[id].on) memset(h, 0, 16), h[0] = h[8] = 0xff, h[2] = h[3] = h[10] = h[11] = 0xff;
            if (burns && coop_rd(actor + COOP_ACTOR_POS, at, 12)) {
                uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + 0x395f110));
                if (mgr && !(coop_guest_call(c, MOD_MAIN_BASE + 0xb9b684, (uint64_t)(uintptr_t)h, 0, 0, 0) & 1)) {
                    coop_guest_call(c, MOD_MAIN_BASE + 0x6e34e8, (uint64_t)(uintptr_t)h, (uint64_t)-1, 0, 0);
                    coop_guest_call(c, MOD_MAIN_BASE + 0x6ec950, mgr, MOD_MAIN_BASE + 0x2cc09f3, (uint64_t)(uintptr_t)h, 2);   // "Chemical_Fire"
                    if (!fx[id].on) rt_log("coop: elem: player %d's Link burns: the flame is put on its stand-in, at %llu ms", id, (unsigned long long)(plat_now_ns() / 1000000));
                    fx[id].on = 1;
                }
                if (coop_guest_call(c, MOD_MAIN_BASE + 0xb9b684, (uint64_t)(uintptr_t)h, 0, 0, 0) & 1) {
                    static _Alignas(16) float m[12], sc[4];
                    memset(m, 0, sizeof m);
                    m[0] = m[5] = m[10] = 1.0f, m[3] = at[0], m[7] = at[1] + 0.9f, m[11] = at[2];
                    sc[0] = sc[1] = sc[2] = 1.0f, sc[3] = 0;
                    coop_guest_call(c, MOD_MAIN_BASE + 0x28e33e8, (uint64_t)(uintptr_t)h, (uint64_t)(uintptr_t)m, 0, 0);
                    coop_guest_call(c, MOD_MAIN_BASE + 0x1a511e4, (uint64_t)(uintptr_t)h, (uint64_t)(uintptr_t)sc, 0, 0);
                }
            } else if (fx[id].on) {
                coop_guest_call(c, MOD_MAIN_BASE + 0x6e34e8, (uint64_t)(uintptr_t)h, (uint64_t)-1, 0, 0);
                fx[id].on = 0;
                rt_log("coop: elem: player %d's Link burns no more: the flame is taken off its stand-in, at %llu ms", id, (unsigned long long)(plat_now_ns() / 1000000));
            }
        }
        int want_any = w.ice != 0, did = 0;
        for (int i = 0; i < n; i++) {
            uint8_t want = i < w.n ? w.el[i] : 0;
            want_any |= want != EL_NONE && (want & 0xf);
            if (cur[i] != shown[id][i]) {
                thing_said(actor, i, shown[id][i], cur[i], 1);
                shown[id][i] = cur[i];
            }
            eset_put(els[i], frame);
            int what = ask_of(cur[i], want);
            if (!what || frame - asked[id][i] < 30) continue;
            asked[id][i] = frame, did = 1;
            ask_do(c, comp, els[i], i, what);
        }
        if (want_any || did) busy_until[id] = frame + 60;
        if ((w.ice != 0) != (ice_put[id] != 0) || (w.ice && ice_read(actor) != w.ice)) ice_write(actor, w.ice), ice_put[id] = w.ice;
        if (busy_until[id] && (int32_t)(busy_until[id] - frame) > 0) {
            static _Alignas(16) float step[4];
            step[0] = 1.0f, step[1] = step[2] = step[3] = 0;
            coop_guest_call(c, MOD_MAIN_BASE + FN_COMP_UPDATE, comp, (uint64_t)(uintptr_t)step, 0, 0);
        }
    }
}

void coop_elem_phase0(Cpu *c) {
    if (!elem_on()) return;
    static uint32_t round, last_any;
    static int said_on;
    uint32_t frame = coop_sess_frame_no();
    int me = coop_net_id();
    if (!said_on++) rt_log("coop: elem: the element sync is on");
    if (atomic_exchange(&world_new, 0) || coop_sess_loading() || !me) {
        // (the things' rows are not emptied: a thing at rest is visited once, when it is made, which may be before the
        // first driver call of its world; a row ends when its actor has gone)
        plat_mutex_lock(&lk);
        want_n = 0, mine_n = 0;
        for (int i = 0; i < THING_N; i++) {   // (a row whose actor went with the old world, and nothing followed)
            Thing *t = &things[i];
            if (t->comp && comp_of(t->actor) != t->comp) t->comp = 0;
            t->want_from = 0;
        }
        plat_mutex_unlock(&lk);
        for (int i = 0; i <= COOP_MAX_PLAYERS; i++) {
            if (cell_list[i]) cell_list[i]->n = 0;
            if (thing_list[i]) thing_list[i]->n = 0;
        }
        if (coop_sess_loading() || !me) return;
    }
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
        if (st == COOP_SESS_LOADING) continue;   // (its world is gone: what it stated ended with it)
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
    strikes_phase0(c, ids, np, frame);
    links_phase0(c, ids, np, frame);
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
            if (!t->hash) t->hash = name_of(t->actor, t->enemy);   // (an actor's first update may come before it has its name)
            coop_rd(t->actor + COOP_ACTOR_POS, t->pos, 12);
        }
        if (!t->hash || t->skip) continue;
        // (an enemy is stated while it is not at rest; another thing while it differs from how it was first seen, and
        // for 2 s after)
        int rest = 1, mine_claim;
        for (int k = 0; k < t->n; k++) rest &= t->cur[k] == EL_NONE || !(t->cur[k] & 0xf);
        mine_claim = t->enemy ? !rest || t->ice || (t->cond & 2) : t->differs_frame && frame - t->differs_frame < 60;
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
            t->ice_wanted = t->enemy && st->n < EL_MAX ? st->el[EL_MAX - 1] : 0;
            t->cond_wanted = t->enemy && st->n < EL_MAX - 1 ? st->el[EL_MAX - 2] : 0;
            n_copy++;
            if (frame - t->seen > 3) {   // its component rests: read and asked here, not in its update's hook
                uint64_t els[EL_MAX];
                uint8_t cur[EL_MAX];
                if (comp_of(t->actor) != t->comp) { t->comp = 0; continue; }   // (its actor went since the last look)
                int n = el_read(t->comp, els, cur);
                if (n && !coop_ctn_driven(t->actor) && !coop_ctn_follows(t->actor)) {
                    // (read here, a change is said here: the hook, which says it for a thing whose component is
                    // updated, then finds nothing new)
                    for (int k = 0; k < n && t->n == n; k++) if (t->cur[k] != cur[k]) {
                        thing_said(t->actor, k, t->cur[k], cur[k], 1);
                        if ((t->cur[k] & 7) && !(cur[k] & 7)) t->ended[k] = frame ? frame : 1;
                    }
                    t->n = (uint8_t)n, memcpy(t->cur, cur, sizeof cur);
                    for (int k = 0; k < n; k++) eset_put(els[k], frame);
                    for (int k = 0; k < n; k++) if (t->asked[k] && (frame - t->asked[k] == 3 || frame - t->asked[k] == 20)) el_dump("some frames later", t->comp, els[k]);
                    for (int k = 0; k < n && nj < JOBS; k++) {
                        int what = ask_of(cur[k], t->wanted[k]);
                        if (((what & 7) == ASK_IGNITE || (what & 7) == ASK_FREEZE) && t->ended[k] && frame - t->ended[k] < 30) what &= ~7;
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
        if (t->enemy && t->n < EL_MAX) out[tn].el[EL_MAX - 1] = t->ice;   // (an enemy's ice look in the last byte, when it has fewer than 7 elements)
        if (t->enemy && t->n < EL_MAX - 1) out[tn].el[EL_MAX - 2] = t->cond;   // (and its condition in the one before, with fewer than 6)
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
        rt_log("coop: elem: cells mine %d, copied %u, not shown %u; things mine %d, copied %d; requests %u, reactions dropped %u, contacts dropped %u, spread heat undone %u, mirrored projectiles made inert %u; strikes mine %u, shown %u, at another player not made %u, reactions from shown-only actors dropped %u",
               cn, atomic_load(&n_cell_copies), atomic_load(&n_not_shown), tn, n_copy, atomic_load(&n_req), atomic_load(&n_react_drop),
               atomic_load(&n_contact_drop), atomic_load(&n_spread_undone), atomic_load(&n_mirrors), atomic_load(&n_strikes_mine), atomic_load(&n_strikes_shown),
               atomic_load(&n_strikes_theirs), atomic_load(&n_src_drop));
    }
    tests(c);
}
