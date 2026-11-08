// Co-op: the other players on the map screen and on the HUD minimap (drawn by ui/coop_markers.cpp with the overlay).
//
// The game draws both with one screen, the AppMap screen (layout AppMap_00 in UI/LayoutArchive/Common.Product.140):
// the full map is its first pose, and its animation "MiniMap" shrinks it to the HUD corner (N_Cut_00..03 move to
// layout (765, -360) and N_Cut_00 shrinks from 2250x1080 to 249x249; Pa_Map_00 and N_IconArea_00 scale to 0.9).
// The map picture is the part Pa_Map_00 (layout PaMap_00), whose ground tiles are 3000-unit pictures laid out over
// 36000x30000 units under the pane N_GroundT_00 (P_Ground_00_00 at (-16500, 13500)): 3 layout units a metre, x east,
// y north (world -z), the world's (0, 0) in the middle. The game pans and zooms the map by moving and scaling the panes
// above the tiles, so N_GroundT_00's global matrix takes a world point to the map's layout position:
//   map = GlbMtx(N_GroundT_00) * (3 x, -3 z), screen = centre + layout * (screen height / 1080), y down.
// On the full map, layout = map. The minimap is not drawn there: its matrix still puts this PC's Link at (0, 0)
// (pair test 2026-11-07: "this Link at (0, 0)" in every minimap frame), so the map is drawn centred and shown in the
// corner as a picture (P_MapCapture_00 in N_Cut_01, at (765, -360)). The game's own icons are live panes in
// N_IconArea_00, which the MiniMap pose moves to (765, -360) and scales 0.9 like the map; there the markers take the
// same way: layout = GlbMtx(N_IconArea_00) * map. The minimap is round: a player outside it sits on the circle.
// Nothing is added to the game: no pin, stamp or pane, nothing in the save. The markers are drawn over the game's
// picture, clipped to N_Cut_00's rectangle (the map's visible area in either pose), and a player outside it sits on its
// edge.
//
// Finding the panes (1.4.3, read from the code; docs/coop/re-notes.md "Map and minimap markers"):
// - main+0x96c1ac is the AppMap screen's layout set-up (a vtable method, the table holding it at main+0x372b3f0 also
//   points at "LayoutMsg/AppMap_00"): x0 the screen, [screen+0x28] its layout. coop_hook (coop_spawn.c) hands the
//   screen to coop_markers_screen.
// - A layout: +0x18 its root pane; +0x48 the list head of its parts (first node [+0x50], next [node+8], the part's
//   name inline at node-0x28, its layout at [node+0x10]) (main+0x406760, the game's "Part/Pane" lookup).
// - nn::ui2d::Pane: +0x18 parent, +0x20 the head of its children (first node [+0x28], next [node+8], the child at
//   node-8; main+0x6070 FindPaneByName), +0x50 size, +0x58 flags (bit 0 visible), +0x59 alpha, +0x70 the global
//   matrix (3x4 rows, set to identity by the constructor main+0x10d6080), +0xb0 the name (main+0x402b30).
// All of it is read with coop_rd from the overlay's thread; every pane is checked by name each frame and looked up
// again when one no longer matches (a screen rebuilt), and the screen is dropped when this Link changes (a freed one).

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"

#define LYT_ROOT 0x18
#define LYT_PARTS 0x48
#define PANE_PARENT 0x18
#define PANE_CHILDREN 0x20
#define PANE_SIZE 0x50
#define PANE_FLAG 0x58
#define PANE_ALPHA 0x59
#define PANE_GLBMTX 0x70
#define PANE_NAME 0xb0
#define MAP_UNITS_PER_M 3.0f     // PaMap_00: 36000x30000 layout units over Hyrule's 12000x10000 m
#define MINI_MAX_HALF 400.0f     // N_Cut_00 narrower than this (249 in the minimap, 2250 on the map): the minimap

static _Atomic uint64_t g_screen;   // the AppMap screen, from its set-up (coop_hook)
// This PC's Link (the "Player" actor) when the screen was set up, 0 when there was none yet (a load). Nothing tells
// this code that the screen is freed (back to the title, a save loaded), so it is taken as gone once the Link is not
// that one any more: none (the title, a load) or another (the world after a load), until the next set-up. Without
// that, a freed screen's panes still carry their names and the markers would read them.
static _Atomic uint64_t g_screen_me;
void coop_markers_screen(uint64_t screen) {
    uint64_t was = atomic_exchange(&g_screen, screen);
    atomic_store(&g_screen_me, coop_player_actor());
    if (was != screen) rt_log("coop: markers: the map screen was set up (%#llx)", (unsigned long long)screen);
}

int coop_markers_on(void) {
    const char *v = cfg_get("TOTK_COOP_MARKERS");
    return !(v && (!strcmp(v, "0") || !strcmp(v, "off")));
}

static int name_is(uint64_t pane, const char *name) {
    char b[25];
    if (!pane || !coop_rd(pane + PANE_NAME, b, 24)) return 0;
    b[24] = 0;
    return !strcmp(b, name);
}

// FindPaneByName (main+0x6070): this pane or one under it, depth first. The budget stops a walk of garbage.
static uint64_t find_pane(uint64_t pane, const char *name, int depth, int *budget) {
    if (!pane || depth > 32 || --*budget < 0) return 0;
    if (name_is(pane, name)) return pane;
    uint64_t head = pane + PANE_CHILDREN, node = coop_rd_ptr(head + 8);
    while (node && node != head && *budget > 0) {
        uint64_t r = find_pane(node - 8, name, depth + 1, budget);
        if (r) return r;
        node = coop_rd_ptr(node + 8);
    }
    return 0;
}

// A part's layout by the part's name (main+0x406760's walk of the parts list).
static uint64_t find_part(uint64_t lyt, const char *name) {
    uint64_t head = lyt + LYT_PARTS, node = coop_rd_ptr(head + 8);
    for (int i = 0; node && node != head && i < 512; i++, node = coop_rd_ptr(node + 8)) {
        char b[33];
        if (!coop_rd(node - 0x28, b, 32)) return 0;
        b[32] = 0;
        if (!strcmp(b, name)) return coop_rd_ptr(node + 0x10);
    }
    return 0;
}

static const char *const T_NAMES[3] = {"N_GroundT_00", "N_UndergroundT_00", "N_SkyT_00"};
static struct {
    uint64_t screen, root, cut, part, icons, t[3];
    int miss;   // a lookup that found nothing: tried again after 120 frames, not every frame
} P;

static int panes_ok(void) {
    if (!P.root || !name_is(P.root, "RootPane") || !name_is(P.cut, "N_Cut_00") || !name_is(P.part, "Pa_Map_00") ||
        !name_is(P.icons, "N_IconArea_00")) return 0;
    for (int i = 0; i < 3; i++)
        if (P.t[i] && !name_is(P.t[i], T_NAMES[i])) return 0;
    return P.t[0] || P.t[1] || P.t[2];
}

static int resolve(void) {
    uint64_t screen = atomic_load(&g_screen), me = coop_player_actor(), was_me = atomic_load(&g_screen_me);
    if (screen && me && !was_me) atomic_compare_exchange_strong(&g_screen_me, &was_me, me), was_me = me;   // set up in a load
    if (screen && (!me || me != was_me)) {
        static uint64_t said_for;
        if (said_for != screen) said_for = screen, rt_log("coop: markers: this Link is %s: the map screen %#llx is taken as gone",
                                                          me ? "another one" : "gone", (unsigned long long)screen);
        memset(&P, 0, sizeof P);
        return 0;
    }
    if (!screen) return 0;
    if (screen == P.screen && panes_ok()) return 1;
    if (screen == P.screen && P.miss && --P.miss) return 0;
    memset(&P, 0, sizeof P);
    P.screen = screen;
    P.miss = 120;
    uint64_t lyt = coop_rd_ptr(screen + 0x28), root = lyt ? coop_rd_ptr(lyt + LYT_ROOT) : 0;
    if (!name_is(root, "RootPane")) return 0;
    int budget = 4096;
    P.root = root;
    P.cut = find_pane(root, "N_Cut_00", 0, &budget);
    P.part = find_pane(root, "Pa_Map_00", 0, &budget);
    P.icons = find_pane(root, "N_IconArea_00", 0, &budget);
    uint64_t map = find_part(lyt, "Pa_Map_00"), mroot = map ? coop_rd_ptr(map + LYT_ROOT) : 0;
    for (int i = 0; i < 3 && mroot; i++) budget = 4096, P.t[i] = find_pane(mroot, T_NAMES[i], 0, &budget);
    static int said;
    if (said++ < 4)
        rt_log("coop: markers: map panes: root %#llx, N_Cut_00 %#llx, N_IconArea_00 %#llx, Pa_Map_00 %#llx (layout %#llx), "
               "ground %#llx, depths %#llx, sky %#llx", (unsigned long long)root, (unsigned long long)P.cut,
               (unsigned long long)P.icons, (unsigned long long)P.part,
               (unsigned long long)map, (unsigned long long)P.t[0], (unsigned long long)P.t[1], (unsigned long long)P.t[2]);
    if (!panes_ok()) return 0;
    P.miss = 0;
    return 1;
}

// How much of the pane shows: 0 when it or a pane above it is hidden, else the product of their alphas. The climb goes
// from the part's panes on through the part pane (Pa_Map_00) when the part's root has no parent.
static float shown(uint64_t pane) {
    float a = 1;
    int crossed = 0;
    for (int i = 0; pane && i < 64; i++) {
        uint8_t fa[2];
        if (!coop_rd(pane + PANE_FLAG, fa, 2)) return 0;
        if (!(fa[0] & 1)) return 0;
        a *= fa[1] / 255.0f;
        uint64_t up = coop_rd_ptr(pane + PANE_PARENT);
        if (!up && !crossed && pane != P.root) up = P.part, crossed = 1;
        pane = up;
    }
    return a;
}

static int glb(uint64_t pane, float m[12]) { return coop_rd(pane + PANE_GLBMTX, m, 48) && isfinite(m[0]) && isfinite(m[3]); }

// A world position to the layout: through the ground pane's matrix m to the map's position, then through ic (the
// icon area's matrix on the minimap, identity on the full map).
static void to_layout(const float m[12], const float ic[12], const float p[3], float *X, float *Y) {
    float lx = MAP_UNITS_PER_M * p[0], ly = -MAP_UNITS_PER_M * p[2];
    float mx = m[0] * lx + m[1] * ly + m[3], my = m[4] * lx + m[5] * ly + m[7];
    *X = ic[0] * mx + ic[1] * my + ic[3], *Y = ic[4] * mx + ic[5] * my + ic[7];
}

typedef struct { float x, y; int id, edge; char name[17]; } CoopMarker;   // x, y: pixels; edge: held at the map's edge
typedef struct { int mode; float x0, y0, x1, y1, alpha; } CoopMarkView;   // mode 0 nothing shown, 1 map, 2 minimap

static int g_full_open;   // the full map shows (ui/coop_tags.cpp hides the name tags meanwhile)
int coop_markers_map_open(void) { return g_full_open; }
static int last_mode = -1;   // for the log: what showed last frame

// The markers to draw this frame on a w x h screen; *v says where the map is. Called by the overlay each frame.
int coop_markers(CoopMarker *out, int max, float w, float h, CoopMarkView *v) {
    memset(v, 0, sizeof *v);
    g_full_open = 0;
    if (!coop_markers_on() || !resolve()) return 0;
    uint64_t t = 0;
    float alpha = 0;
    for (int i = 0; i < 3 && !t; i++)
        if (P.t[i] && (alpha = shown(P.t[i])) > 0.05f) t = P.t[i];
    float cut_alpha = t ? shown(P.cut) : 0;
    float m[12], c[12], ic[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, size[2];
    float k = h / 1080.0f;   // layout units to pixels: the layout's 1080 rows fill the screen's height
    float hx = 0, hy = 0;
    if (t && cut_alpha > 0.05f && glb(t, m) && glb(P.cut, c) && coop_rd(P.cut + PANE_SIZE, size, 8))
        hx = 0.5f * size[0] * sqrtf(c[0] * c[0] + c[4] * c[4]), hy = 0.5f * size[1] * sqrtf(c[1] * c[1] + c[5] * c[5]);
    if (!(hx > 1 && hy > 1)) {
        if (last_mode > 0) rt_log("coop: markers: no map shows");
        last_mode = 0;
        return 0;
    }
    v->mode = hx < MINI_MAX_HALF ? 2 : 1;
    if (v->mode == 2 && !glb(P.icons, ic)) return 0;
    v->x0 = w / 2 + (c[3] - hx) * k, v->x1 = w / 2 + (c[3] + hx) * k;
    v->y0 = h / 2 - (c[7] + hy) * k, v->y1 = h / 2 - (c[7] - hy) * k;
    if (v->x0 < 0) v->x0 = 0;
    if (v->y0 < 0) v->y0 = 0;
    if (v->x1 > w) v->x1 = w;
    if (v->y1 > h) v->y1 = h;
    v->alpha = fminf(alpha, cut_alpha);
    g_full_open = v->mode == 1;

    float pos[COOP_MAX_PLAYERS + 1][3];
    int ids[COOP_MAX_PLAYERS + 1], n = coop_player_positions_ids(pos, ids, COOP_MAX_PLAYERS);
    int self_test = cfg_get("TOTK_COOP_TEST_MARKSELF") != NULL;
    uint64_t me = coop_player_actor();
    float mine[3] = {0, 0, 0};
    int have_me = me && coop_rd(me + COOP_ACTOR_POS, mine, 12);
    if (self_test && have_me && n <= COOP_MAX_PLAYERS) memcpy(pos[n], mine, 12), ids[n++] = 0;
    int nids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int nn = coop_net_players(nids, names, COOP_MAX_PLAYERS), r = 0;
    float margin = (v->mode == 2 ? 6 : 10) * k;
    float ccx = w / 2 + c[3] * k, ccy = h / 2 - c[7] * k, rad = fminf(hx, hy) * k - margin;   // the minimap's circle
    for (int i = 0; i < n && r < max; i++) {
        float X, Y;
        to_layout(m, ic, pos[i], &X, &Y);
        if (!isfinite(X) || !isfinite(Y)) continue;   // a bad reported position: on neither map nor circle
        CoopMarker *o = &out[r++];
        o->x = w / 2 + X * k, o->y = h / 2 - Y * k, o->id = ids[i], o->edge = 0;
        if (v->mode == 2) {
            float dx = o->x - ccx, dy = o->y - ccy, d = hypotf(dx, dy);
            if (d > rad && rad > 0) o->x = ccx + dx * rad / d, o->y = ccy + dy * rad / d, o->edge = 1;
        } else {
            float cx = fminf(fmaxf(o->x, v->x0 + margin), v->x1 - margin), cy = fminf(fmaxf(o->y, v->y0 + margin), v->y1 - margin);
            if (cx != o->x || cy != o->y) o->x = cx, o->y = cy, o->edge = 1;
        }
        const char *nm = ids[i] ? "" : "(you)";
        for (int j = 0; j < nn; j++) if (nids[j] == ids[i]) nm = names[j];
        snprintf(o->name, sizeof o->name, "%s", nm[0] ? nm : "Player");
    }

    // The log: each change of what shows, and every 2 s with TOTK_COOP_TEST_MARKSELF. "this Link at" is where this
    // transform puts this PC's own Link; on the minimap the game keeps it in the middle, so "off" near 0 there says the
    // transform is right (the full map opens centred on it too, until the player pans).
    static int ticks;
    if (v->mode != last_mode || (self_test && ++ticks % 120 == 0)) {
        last_mode = v->mode;
        float X = 0, Y = 0;
        if (have_me) to_layout(m, ic, mine, &X, &Y);
        rt_log("coop: markers: %s shows (%s layer), N_Cut_00 at (%.0f, %.0f) %.0fx%.0f, zoom %.4f, icon area at "
               "(%.0f, %.0f) scale %.3f; this Link (%.0f, %.0f) at (%.0f, %.0f), off %.1f; %d marker(s)",
               v->mode == 2 ? "the minimap" : "the map", t == P.t[0] ? "ground" : t == P.t[1] ? "depths" : "sky", c[3],
               c[7], 2 * hx, 2 * hy, sqrtf(m[0] * m[0] + m[4] * m[4]), ic[3], ic[7], sqrtf(ic[0] * ic[0] + ic[4] * ic[4]),
               mine[0], mine[2], X, Y, hypotf(X - c[3], Y - c[7]), r);
    }
    return r;
}
