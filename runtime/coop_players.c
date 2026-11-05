// Co-op players: up to COOP_MAX_PLAYERS - 1 extra Links in this PC's world, each a spawned Player actor with a
// controller of its own (docs/coop/design.md "Sessions").
//
// A slot's input comes from a second gamepad on this PC (couch test; Numpad 3 or TOTK_COOP_AUTOSPAWN) or from a remote
// player over the relay (coop_net.c): it appears when the player's first state arrives and is let go when they leave.
// While a slot's Player updates (actor calc, wrapped through the entry hook), the controller manager hands out the
// slot's controller as controller 0: a copy of the real one with the slot's buttons and sticks (the real one is
// rewritten by the game while actors update; this one is not). The first Player, this PC's Link, keeps the real one.
// A remote Link follows its owner's input (sticks turned from the owner's camera into this PC's camera) and is put
// back at the owner's position when it drifts (COOP_SNAP_MOVING_M, COOP_SNAP_IDLE_M; at most every 10 polls).

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"

#define COOP_SNAP_MOVING_M 1.5f   // a remote Link is put back past this while its owner moves
#define COOP_SNAP_IDLE_M 0.25f    // ... and past this once its owner stands still (no stick, no buttons)

enum { FREE, WANTED, REQUESTED, LIVE, LET_GO };
enum { SRC_PAD, SRC_NET };
typedef struct {
    int state, source, net_id;
    _Alignas(16) uint8_t handle[0x40], link[0x40];
    uint64_t actor, ref;
    _Alignas(16) uint8_t ctl[0x400];   // the slot's controller (a copy of controller 0 each frame, then its input)
    uint32_t prev;
    float held[32];
    uint64_t buttons;                  // this frame's input, set by coop_players_tick
    float stick[4];
    float want_pos[3], want_rot[9];    // net: the owner's reported transform
    float vel[3];                      // the owner's velocity from the last two reports (puppet mode's prediction)
    uint32_t last_seq;
    uint64_t last_ns;                  // when that report was sent, as plat_now_ns
    struct { uint64_t ns; float pos[3]; } seen[32];   // puppet mode: where the puppet was, for the error log
    struct { uint64_t ns; float pos[3], q[4]; } hist[16];   // puppet mode: the owner's last reports (arrival time)
    unsigned nhist;                    // under lock
    float jit_prev[2][3], jit_sum, jit_max;   // puppet mode: frame-to-frame jerk of the placed position (log)
    int jit_n, jit_have;
    int nseen;
    float err_max, err_sum;
    int err_n;
    int have_want, idle, snap_wait;
    unsigned snaps;
    char gear_want[COOP_GEAR_SLOTS][32], gear_have[COOP_GEAR_SLOTS][32];   // the owner's gear; what this Link was given
    _Alignas(16) uint8_t gear_str[COOP_GEAR_SLOTS][0x60];   // the names handed to the creates, kept while they run
} Slot;
static Slot slots[COOP_MAX_PLAYERS];
static PlatMutex lock = PLAT_MUTEX_INIT;   // input fields, read by the calc wrapper on worker threads

// This PC's input as the game read it this poll (ov_input.c npad_states), for the relay.
static uint64_t local_buttons;
static int32_t local_sticks[4];
void coop_local_input(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    local_buttons = buttons, local_sticks[0] = lx, local_sticks[1] = ly, local_sticks[2] = rx, local_sticks[3] = ry;
}

int coop_p2_connected(void);
int coop_p2_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

// The camera's horizontal forward and right (PlayerCamera's rotation: it looks along -Z, X is its right; 2026-11-04).
static int camera_frame(float fwd[2], float right[2]) {
    uint64_t cam = coop_resident_named("PlayerCamera", NULL);
    float r[9];
    if (!cam || !coop_rd(cam + COOP_ACTOR_ROT, r, sizeof r)) return 0;
    fwd[0] = -r[2], fwd[1] = -r[8], right[0] = r[0], right[1] = r[6];
    float lf = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]), lr = sqrtf(right[0] * right[0] + right[1] * right[1]);
    if (lf < 1e-3f || lr < 1e-3f) return 0;
    fwd[0] /= lf, fwd[1] /= lf, right[0] /= lr, right[1] /= lr;
    return 1;
}

// ---- name tags over the other players' Links (drawn by ui/coop_tags.cpp with the overlay).
// The camera: PlayerCamera's position is the eye, its rotation's columns right, up and back (it looks along -Z);
// the vertical field of view is TOTK_COOP_TAG_FOV degrees (50 until it is found in the game).
typedef struct { float x, y, dist; char name[17]; } CoopTag;   // x, y: 0..1 of the screen
int coop_name_tags(CoopTag *out, int max, float aspect) {
    uint64_t cam = coop_resident_named("PlayerCamera", NULL);
    float cp[3], r[9];
    if (!cam || !coop_rd(cam + COOP_ACTOR_POS, cp, 12) || !coop_rd(cam + COOP_ACTOR_ROT, r, 36)) return 0;
    const char *fv = cfg_get("TOTK_COOP_TAG_FOV");
    float fovy = (fv && *fv ? (float)atof(fv) : 50.0f) * 3.14159265f / 180.0f, ty = tanf(fovy / 2), tx = ty * aspect;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int nn = coop_net_players(ids, names, COOP_MAX_PLAYERS), n = 0;
    uint64_t self_test = cfg_get("TOTK_COOP_TEST_TAGSELF") ? coop_player_actor() : 0;   // test: tag this PC's Link
    for (int i = 0; i <= COOP_MAX_PLAYERS && n < max; i++) {
        uint64_t a;
        const char *nm = "";
        if (i == COOP_MAX_PLAYERS) {
            if (!self_test) break;
            a = self_test, nm = "(you)";
        } else {
            Slot *s = &slots[i];
            if (s->state != LIVE || s->source != SRC_NET || !s->actor) continue;
            a = s->actor;
            for (int k = 0; k < nn; k++) if (ids[k] == s->net_id) nm = names[k];
        }
        float p[3];
        if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) continue;
        p[1] += 2.0f;   // above the head
        float d[3] = {p[0] - cp[0], p[1] - cp[1], p[2] - cp[2]};
        float x = d[0] * r[0] + d[1] * r[3] + d[2] * r[6], y = d[0] * r[1] + d[1] * r[4] + d[2] * r[7];
        float z = -(d[0] * r[2] + d[1] * r[5] + d[2] * r[8]);
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (z < 0.3f || dist > 150.0f) continue;
        CoopTag *t = &out[n++];
        t->x = 0.5f + 0.5f * x / (z * tx), t->y = 0.5f - 0.5f * y / (z * ty), t->dist = dist;
        snprintf(t->name, sizeof t->name, "%s", nm[0] ? nm : "Player");
    }
    return n;
}

// ---- gear. A Player's EquipmentUser builds each slot through main+0xc4a5f4 (eq, slot), which reads the item from the
// save's inventory and creates it by name: weapons through main+0xaf100c (eq, slot, &name, ... 14 arguments), armor
// through main+0xc48ac0 (eq, slot, &name, dye, 0, 1) and on to main+0xc4961c; &name points at a string whose first
// word is the name (2026-11-04). It keeps what it made in a table at +0x28 (0x18 a row, the row's first word a
// reference whose +0x40 is the item actor), cleared when an item is put away. This PC's Link's table is read every
// frame and sorted by item name (menu equips, unequips and broken weapons all show there); a remote Link's slot is
// made with its owner's item by calling the create itself, as the save may have nothing for that slot.
const int coop_gear_slot[COOP_GEAR_SLOTS] = {0, 7, 8, 9, 10, 11};   // weapon, bow, shield, head, body, legs
#define GEAR_WEAPON_CREATE 0xaf100c
#define GEAR_ARMOR_CREATE 0xc48ac0
#define GEAR_IN_HAND 0x3e322c    // (eq, 0 weapon / 3 bow / 4 shield): 1 when out; c4a5f4 passes 1 for it, else 2
#define GEAR_TABLE 0x28
#define GEAR_NONE "-"            // on the wire: the slot is empty ("" is not known yet)
static char my_gear[COOP_GEAR_SLOTS][32];

// ---- the host's world. Time of day: the world manager [main+0x3a83968] +0x98 is the time object; +0x28 the second
// of the day now (float, 0..86400), +0x48 the last time asked for. main+0x21cd794 (world manager, s0 = second of the
// day) asks for a new time, as AI node ExecuteWmSetTime does (hour * 3600 + minute * 60); the game applies it later
// (2026-11-04).
#define WM_SLOT 0x3a83968
#define WM_TIME 0x98
#define TIME_NOW 0x28
#define WM_SET_TIME 0x21cd794
#define TIME_FOLLOW_S 90.0f        // game seconds off the host before this game is set to its time (60 a real second)

static float day_seconds(void) {
    uint64_t tm = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + WM_SLOT) + WM_TIME);
    float t = -1;
    return tm && coop_rd(tm + TIME_NOW, &t, 4) && t >= 0 && t <= 86400.0f ? t : -1;
}

// Weather: the weather manager [[main+0x395dc20]]. AI query QueryWorldInfoCheckCurrentWeather reads the weather at
// an actor as main+0x1c2fd1c (main+0x4f8d60 (actor)), once main+0x21cd370 (manager) says it is ready; flowchart node
// OneShotWmSetWeatherForFlowchart forces one with main+0x21d0798 (manager, type, flag), OneShotWmResetWeather...
// gives it back to the forecast with main+0x21d0c18 (manager) (2026-11-04).
#define WX_SLOT 0x395dc20
#define WX_READY 0x21cd370
#define WX_CONTEXT 0x4f8d60
#define WX_AT 0x1c2fd1c
#define WX_FORCE 0x21d0798
#define WX_RELEASE 0x21d0c18
static int32_t weather_now = -1;   // the weather at this PC's Link, read once a second
static int32_t weather_forced = -1;   // the host's weather this game was told to show (-1: the forecast's own)

static void read_weather(Cpu *c, uint64_t me) {
    static int t;
    if (++t % 30) return;
    uint64_t wx = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + WX_SLOT));
    if (!wx || !(coop_guest_call(c, MOD_MAIN_BASE + WX_READY, wx, 0, 0, 0) & 1)) { weather_now = -1; return; }
    uint64_t ctx = coop_guest_call(c, MOD_MAIN_BASE + WX_CONTEXT, me, 0, 0, 0);
    int32_t w = ctx ? (int32_t)(uint32_t)coop_guest_call(c, MOD_MAIN_BASE + WX_AT, ctx, 0, 0, 0) : -1;
    if (w != weather_now) rt_log("coop: weather here is now %d", w);
    weather_now = w;
}

static void follow_host_weather(Cpu *c, int32_t host) {   // host -1: no host (alone, or it left)
    uint64_t wx = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + WX_SLOT));
    if (!wx || host == weather_forced) return;
    if (host < 0) {
        rt_log("coop: weather back to this game's forecast");
        coop_guest_call(c, MOD_MAIN_BASE + WX_RELEASE, wx, 0, 0, 0);
    } else {
        rt_log("coop: weather %d, the host's %d: set to the host's", weather_now, host);
        coop_guest_call(c, MOD_MAIN_BASE + WX_FORCE, wx, (uint64_t)(uint32_t)host, 0, 0);
    }
    weather_forced = host;
}

static int set_day_seconds(Cpu *c, float t) {
    uint64_t wm = coop_rd_ptr(MOD_MAIN_BASE + WM_SLOT);
    if (!wm) return 0;
    while (t < 0) t += 86400.0f;
    while (t >= 86400.0f) t -= 86400.0f;
    __uint128_t v0 = c->v[0];
    uint32_t bits;
    memcpy(&bits, &t, 4);
    c->v[0] = bits;
    coop_guest_call(c, MOD_MAIN_BASE + WM_SET_TIME, wm, 0, 0, 0);
    c->v[0] = v0;
    return 1;
}

// The guest's own clock: how far it was from the host's when the session began, given back when it ends, so the
// guest's world (and save) keeps its own time of day.
static int time_following;
static float own_offset;

static void follow_host_time(Cpu *c, float host) {
    static uint64_t last_ns;
    float mine = day_seconds();
    if (mine < 0 || host < 0) return;
    float d = host - mine;
    if (d > 43200.0f) d -= 86400.0f;
    if (d < -43200.0f) d += 86400.0f;
    if (!time_following) time_following = 1, own_offset = -d;
    uint64_t now = plat_now_ns();
    if (fabsf(d) < TIME_FOLLOW_S || now - last_ns < 5000000000ull) return;
    last_ns = now;
    rt_log("coop: time of day %.0f s, the host's %.0f s: set to the host's", mine, host);
    set_day_seconds(c, host);
}

static void own_time_back(Cpu *c) {   // the session ended here: this game's own clock again
    float mine = day_seconds();
    if (!time_following || mine < 0) return;
    time_following = 0;
    rt_log("coop: time of day back to this game's own (%.0f s from the host's)", own_offset);
    if (fabsf(own_offset) >= TIME_FOLLOW_S) set_day_seconds(c, mine + own_offset);
}

// Which of the synced slots an item belongs to, from its actor name; -1 for the rest (sheath, quiver, devices).
static int gear_kind(const char *n) {
    size_t len = strlen(n);
    if (!strncmp(n, "Weapon_Sword_", 13) || !strncmp(n, "Weapon_Lsword_", 14) || !strncmp(n, "Weapon_Spear_", 13)) return 0;
    if (!strncmp(n, "Weapon_Bow_", 11)) return 1;
    if (!strncmp(n, "Weapon_Shield_", 14)) return 2;
    if (!strncmp(n, "Armor_", 6) && len > 5) {
        if (!strcmp(n + len - 5, "_Head")) return 3;
        if (!strcmp(n + len - 6, "_Upper")) return 4;
        if (!strcmp(n + len - 6, "_Lower")) return 5;
    }
    return -1;
}

// What an EquipmentUser holds now, per synced slot (GEAR_NONE for an empty one).
static void eq_gear(uint64_t eq, char now[COOP_GEAR_SLOTS][32]) {
    for (int k = 0; k < COOP_GEAR_SLOTS; k++) strcpy(now[k], GEAR_NONE);
    for (int row = 0; row < 24; row++) {
        uint64_t ref = coop_rd_ptr(eq + GEAR_TABLE + 0x18 * (uint64_t)row);
        char nm[32];
        coop_actor_name(ref ? coop_rd_ptr(ref + 0x40) : 0, nm, sizeof nm);
        int k = nm[0] ? gear_kind(nm) : -1;
        if (k >= 0) memcpy(now[k], nm, 32);
    }
}

static void read_my_gear(uint64_t eq) {
    char now[COOP_GEAR_SLOTS][32];
    eq_gear(eq, now);
    for (int k = 0; k < COOP_GEAR_SLOTS; k++)
        if (strcmp(now[k], my_gear[k])) {
            rt_log("coop: my gear slot %d is now %s", coop_gear_slot[k], now[k]);
            memcpy(my_gear[k], now[k], 32);
        }
}

// The string shape the creates take: +0 pointer to the text, +8 capacity 64, the text kept here.
static uint64_t gear_string(uint8_t st[0x60], const char *text) {
    memset(st, 0, 0x60);
    snprintf((char *)st + 0x10, 0x40, "%s", text);
    uint64_t tp = (uint64_t)(uintptr_t)(st + 0x10);
    int32_t cap = 0x40;
    memcpy(st, &tp, 8), memcpy(st + 8, &cap, 4);
    return (uint64_t)(uintptr_t)st;
}

// Makes slot k of a remote Link's EquipmentUser the item name ("" empties a weapon slot), as c4a5f4 would from a save.
static void give_gear(Cpu *c, uint64_t eq, int k, const char *name, uint8_t st[0x60]) {
    uint64_t slot = (uint64_t)coop_gear_slot[k], np = gear_string(st, name);
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    if (k <= 2) {
        static const uint64_t hand_of[3] = {0, 3, 4};
        int out = (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, hand_of[k], 0, 0) & 1);
        static _Alignas(16) uint8_t no_attachment[0x60];
        uint64_t att = gear_string(no_attachment, "");
        uint64_t sp = (sp0 - 0x100) & ~0xfull;
        uint64_t stack[6] = {0, 0, 0, (uint64_t)(uint32_t)-1, 0, (uint64_t)(uint32_t)-1};   // arguments 9..14
        memcpy((void *)(uintptr_t)sp, stack, sizeof stack);
        c->sp = sp;
        rt_call(c, MOD_MAIN_BASE + GEAR_WEAPON_CREATE, 8, eq, slot, np, (uint64_t)(out ? 1 : 2), (uint64_t)(uint32_t)-1,
                (uint64_t)(uint32_t)-1, att, (uint64_t)1);
    } else {
        c->sp = (sp0 - 0x100) & ~0xfull;
        rt_call(c, MOD_MAIN_BASE + GEAR_ARMOR_CREATE, 6, eq, slot, np, (uint64_t)(uint32_t)-1, (uint64_t)0, (uint64_t)1);
    }
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
}
static uint64_t unplaced_eq[16];   // EquipmentUsers whose Player was not known when their gear was made
static struct { uint64_t eq, actor; } eq_owner[32];   // EquipmentUser -> its Player, learned from their creates

static int gear_index(int slot) {
    for (int k = 0; k < COOP_GEAR_SLOTS; k++) if (coop_gear_slot[k] == slot) return k;
    return -1;
}
// The Player that owns an EquipmentUser: a word in its first 0x400 bytes equal to this PC's Link or a slot's actor.
static uint64_t gear_owner(uint64_t eq, uint64_t me);
static Slot *slot_of_actor(uint64_t actor);
static int eq_off = -1;   // where a Player keeps its EquipmentUser: found on this PC's Link once its gear is built
static uint64_t eq_of_actor(uint64_t actor) {
    for (int i = 0; i < 32; i++) if (eq_owner[i].actor == actor) return eq_owner[i].eq;
    return eq_off >= 0 && actor ? coop_rd_ptr(actor + (uint64_t)eq_off) : 0;
}
static void learn_eq_off(uint64_t me, uint64_t eq) {
    if (eq_off != -1) return;
    uint64_t words[0x4000 / 8];
    if (!coop_rd(me, words, sizeof words)) return;
    for (int w = 0; w < 0x4000 / 8; w++)
        if (words[w] == eq) { eq_off = w * 8; rt_log("coop: a Player keeps its EquipmentUser at +%#x", eq_off); return; }
    eq_off = -2;
    rt_log("coop: no pointer to the EquipmentUser in the Player's first 0x4000 bytes");
}

int coop_players_gear(Cpu *c, uint64_t addr) {
    uint64_t eq = c->x[0], me = coop_player_actor();
    int k = gear_index((int)(uint32_t)c->x[1]);
    if (k < 0 || !me) return 0;
    uint64_t owner = gear_owner(eq, me);
    if (!owner) {   // a Player not claimed yet (a new co-op Link builds its gear first): matched after its claim
        for (int i = 0; i < 16; i++)
            if (unplaced_eq[i] == eq || !unplaced_eq[i]) { unplaced_eq[i] = eq; break; }
        return 0;
    }
    uint64_t np = coop_rd_ptr(c->x[2]);
    char name[32] = "";
    if (np) coop_rd(np, name, sizeof name - 1);
    if (owner == me) { learn_eq_off(me, eq); return 0; }   // this PC's Link: its gear is read from its table
    // A remote Link's own build (its first gear, or the game rebuilding a slot): the owner's item in place of the save's.
    Slot *s = slot_of_actor(owner);
    if (!s || !s->gear_want[k][0]) return 0;
    const char *want = strcmp(s->gear_want[k], GEAR_NONE) ? s->gear_want[k] : "";
    if (!strcmp(want, name) || (!want[0] && k > 2)) return 0;   // armor always has a piece: none is the save's default
    c->x[2] = gear_string(s->gear_str[k], want);
    rt_log("coop: slot %d's Link gets %s in gear slot %d (this save has %s)", (int)(s - slots), want[0] ? want : "nothing",
           coop_gear_slot[k], name[0] ? name : "nothing");
    (void)addr;
    return 0;
}

static uint64_t gear_owner(uint64_t eq, uint64_t me) {
    for (int i = 0; i < 32; i++) if (eq_owner[i].eq == eq) return eq_owner[i].actor;
    uint64_t words[0x400 / 8];
    if (!coop_rd(eq, words, sizeof words)) return 0;
    uint64_t found = 0;
    for (int w = 0; w < 0x400 / 8 && !found; w++) {
        if (words[w] == me) found = me;
        for (int i = 0; i < COOP_MAX_PLAYERS && !found; i++)
            if (slots[i].actor && words[w] == slots[i].actor) found = slots[i].actor;
    }
    if (!found) return 0;
    for (int i = 0; i < 32; i++)
        if (!eq_owner[i].eq || eq_owner[i].actor == found) {
            eq_owner[i].eq = eq, eq_owner[i].actor = found;
            rt_log("coop: EquipmentUser %llx belongs to %s", (unsigned long long)eq, found == me ? "this PC's Link" : "a co-op Link");
            break;
        }
    return found;
}

static Slot *slot_of_actor(uint64_t actor) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if ((slots[i].state == LIVE || slots[i].state == LET_GO) && slots[i].actor == actor) return &slots[i];
    return NULL;
}
static Slot *slot_for(int source, int net_id) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state != FREE && slots[i].state != LET_GO && slots[i].source == source && slots[i].net_id == net_id)
            return &slots[i];
    return NULL;
}
static Slot *slot_new(int source, int net_id) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state == FREE) {
            Slot *s = &slots[i];
            s->source = source, s->net_id = net_id, s->prev = 0, s->have_want = 0;
            memset(s->vel, 0, sizeof s->vel), s->last_seq = 0, s->last_ns = 0, s->nseen = 0, s->err_n = 0;
            s->nhist = 0, s->jit_n = 0, s->jit_have = 0;
            memset(s->held, 0, sizeof s->held);
            memset(s->gear_want, 0, sizeof s->gear_want), memset(s->gear_have, 0, sizeof s->gear_have);
            s->state = WANTED;
            rt_log("coop: player slot %d for %s %d", i, source == SRC_NET ? "remote player" : "local pad", net_id);
            return s;
        }
    return NULL;
}

// Called from the actor calc hook: 1 when it ran a slot's Player itself (with the slot's controller handed out).
static int world_role;   // coop_world_role: 0 alone, 1 host (others follow this game), 2 guest
int coop_world_role(void) { return world_role; }
// The remote players' Links' positions (for the host's sync radius).
// The remote players' Links (actor, and their net id), for the enemy sync.
int coop_player_actors(uint64_t *actors, int *net_ids, int max) {
    int n = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS && n < max; i++)
        if (slots[i].state == LIVE && slots[i].source == SRC_NET && slots[i].actor)
            actors[n] = slots[i].actor, net_ids[n] = slots[i].net_id, n++;
    return n;
}
int coop_player_positions(float (*out)[3], int max) {
    int n = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS && n < max; i++)
        if (slots[i].state == LIVE && slots[i].source == SRC_NET && slots[i].have_want) memcpy(out[n++], slots[i].want_pos, 12);
    return n;
}
static __thread int inside;
// ---- TOTK_COOP_TRACE=diff (a test): which qwords of main's writable data (.data to .bss) a co-op Link's calc changes;
// counted over calcs and reported every 600 calcs, the most often changed first.
#define DIFF_LO 0x357dee8
#define DIFF_HI 0x3adefd0
#define DIFF_N ((DIFF_HI - DIFF_LO) / 8)
static uint64_t *diff_snap;
static uint16_t *diff_count;
static int diff_calcs;
static void diff_before(void) {
    if (!diff_snap) diff_snap = malloc(DIFF_N * 8), diff_count = calloc(DIFF_N, 2);
    if (diff_snap) memcpy(diff_snap, (void *)(uintptr_t)(MOD_MAIN_BASE + DIFF_LO), DIFF_N * 8);
}
static void diff_after(void) {
    if (!diff_snap || !diff_count) return;
    const uint64_t *now = (const uint64_t *)(uintptr_t)(MOD_MAIN_BASE + DIFF_LO);
    for (size_t i = 0; i < DIFF_N; i++)
        if (now[i] != diff_snap[i] && diff_count[i] < 65535) diff_count[i]++;
    if (++diff_calcs < 600) return;
    int shown = 0;
    for (int want = 600; want > 0 && shown < 60; want = want > 60 ? want - 60 : want - 10)
        for (size_t i = 0; i < DIFF_N && shown < 60; i++)
            if (diff_count[i] >= want && diff_count[i] < want + (want > 60 ? 60 : 10)) {
                rt_log("coop: diff main+%#llx changed in %u of 600 calcs, now %llx", (unsigned long long)(DIFF_LO + i * 8),
                       diff_count[i], (unsigned long long)now[i]);
                shown++;
            }
    rt_log("coop: diff: %d qwords shown", shown);
    memset(diff_count, 0, DIFF_N * 2);
    diff_calcs = 0;
}
int coop_in_remote_calc(void) { return inside; }   // this thread runs a co-op Link's calc (coop_spawn.c profiler)
// The actor's other per-frame update phases (coop_spawn.c hooks them): run as inside a co-op Link's calc when x0 is
// one, so what keeps a co-op Link from changing this game (HUD, game data, tips) covers them too. No controller swap:
// the calc alone reads the pad.
// Test (TOTK_COOP_TEST_FREEZE=<s>): from that many seconds into the world, a co-op Link's update phases do not run at
// all: whether it still shows, and follows ForceSetMatrix, with none of its gameplay running.
static int frozen(void) {
    static double at = -2;
    if (at == -2) { const char *e = cfg_get("TOTK_COOP_TEST_FREEZE"); at = e ? atof(e) : -1; }
    return at >= 0 && frame_step_world_us() / 1e6 >= at;
}
int coop_players_phase(Cpu *c, uint64_t addr) {
    if (inside || !slot_of_actor(c->x[0])) return 0;
    // TOTK_COOP_TEST_FREEZE_PHASES=0: the freeze stops the calc only; these phases (model, physics) still run
    static int keep_phases = -1;
    if (keep_phases < 0) { const char *e = cfg_get("TOTK_COOP_TEST_FREEZE_PHASES"); keep_phases = e && *e == '0'; }
    if (frozen() && !keep_phases) return 1;
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 4) rt_log("coop: a co-op Link's update phase main+%#llx counts as its calc", (unsigned long long)(addr - MOD_MAIN_BASE));
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    return 1;
}
// Puppet mode, from the hook on main+0x257310 (x0 the actor): that step copies the physics body's transform into the
// actor's position and rotation (+0x2b4, +0x2c0) and its model's world matrix. For a puppet the body is only near
// its owner (it falls, can't climb), so after it runs the owner's transform goes over all three; the model's dirty
// bit makes main+0x24cc30 push it to the model units later in the same phase. The body itself is still moved by
// snap() every tick. Returns 1 when it ran the step.
static void owner_matrix(Slot *s, float m[12]);
static int puppet_delay_ms(void);
int coop_puppet_place(Cpu *c, uint64_t addr) {
    Slot *s = slot_of_actor(c->x[0]);
    int coop_puppet_mode(void);   // coop_world.c
    if (!s || s->state != LIVE || s->source != SRC_NET || !s->have_want || !coop_puppet_mode()) return 0;
    static int place = -1;   // TOTK_COOP_PUPPET_PLACE: bit 0 the actor's transform, bit 1 the model's (default 3; 0 off)
    if (place < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_PLACE"); place = e ? atoi(e) : 3; }
    if (!place) return 0;
    static __thread int in;
    if (in) return 0;
    uint64_t actor = c->x[0];
    in = 1;
    c->pc = addr;
    rt_dispatch(c);
    in = 0;
    float m[12], pos[3] = {0}, rot[9];
    owner_matrix(s, m);
    for (int r = 0; r < 3; r++) {
        pos[r] = m[r * 4 + 3];
        for (int k = 0; k < 3; k++) rot[r * 3 + k] = m[r * 4 + k];
    }
    if (place & 1) {
        memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), pos, 12);
        memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_ROT), rot, 36);
    }
    uint64_t model = place & 2 ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x8) + 0x28) : 0;
    if (model) {
        memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        ST8(model + 0x240, LD8(model + 0x240) | 1);
    }
    // Jerk: how far each frame's placed position is from the straight line through the two frames before it.
    if (s->jit_have >= 2) {
        float e = 0;
        for (int k = 0; k < 3; k++) { float d = pos[k] - 2 * s->jit_prev[1][k] + s->jit_prev[0][k]; e += d * d; }
        e = sqrtf(e);
        s->jit_sum += e, s->jit_n++;
        if (e > s->jit_max) s->jit_max = e;
        if (s->jit_n >= 60) {
            rt_log("coop: puppet: slot %d jerk per frame: mean %.1f mm, max %.1f mm (delay %d ms)", (int)(s - slots),
                   s->jit_sum / s->jit_n * 1000, s->jit_max * 1000, puppet_delay_ms());
            s->jit_sum = s->jit_max = 0, s->jit_n = 0;
        }
    }
    memcpy(s->jit_prev[0], s->jit_prev[1], 12), memcpy(s->jit_prev[1], pos, 12);
    if (s->jit_have < 2) s->jit_have++;
    static int said;
    if (!said++) rt_log("coop: puppet: slot %d is placed at its owner after its physics step", (int)(s - slots));
    return 1;
}
int coop_players_calc(Cpu *c, uint64_t addr) {
    Slot *s;
    if (inside || !(s = slot_of_actor(c->x[0]))) return 0;
    if (frozen()) {
        static int said;
        if (!said++) rt_log("coop: test: the co-op Links' updates no longer run");
        return 1;
    }
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_INPUT_MGR_SLOT));
    if (!mgr) return 0;
    uint64_t real = LD64(mgr + 0xa0), own = (uint64_t)(uintptr_t)s->ctl;
    memcpy(s->ctl, (void *)(uintptr_t)real, sizeof s->ctl);
    plat_mutex_lock(&lock);
    uint64_t buttons = s->buttons;
    float stick[4];
    memcpy(stick, s->stick, sizeof stick);
    plat_mutex_unlock(&lock);
    // Test (TOTK_COOP_TEST_NOINPUT): the co-op Link gets no buttons and no stick (the idle body for a copied pose)
    static int noinput = -1;
    int coop_puppet_mode(void);   // coop_world.c: puppet mode gives no input either
    if (noinput < 0) noinput = cfg_get("TOTK_COOP_TEST_NOINPUT") != NULL || coop_puppet_mode();
    if (noinput) buttons = 0, memset(stick, 0, sizeof stick);
    coop_fill_controller(own, buttons, stick, &s->prev, s->held);
    ST64(mgr + 0xa0, own);
    static int diff_on = -1;   // TOTK_COOP_TRACE=diff: main's writable data the co-op Link's calc changes
    if (diff_on < 0) { const char *e = getenv("TOTK_COOP_TRACE"); diff_on = e && strstr(e, "diff"); }
    if (diff_on) diff_before();
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    if (diff_on) diff_after();
    ST64(mgr + 0xa0, real);
    return 1;
}

static void spawn_slot(Cpu *c, Slot *s) {
    uint64_t me = coop_player_actor();
    float pos[3], rot[9];
    if (!me || !coop_rd(me + COOP_ACTOR_POS, pos, 12) || !coop_rd(me + COOP_ACTOR_ROT, rot, 36)) return;
    if (s->have_want) memcpy(pos, s->want_pos, 12), memcpy(rot, s->want_rot, 36);
    else pos[0] += 2 * rot[2], pos[2] += 2 * rot[8];   // 2 m in front of this PC's Link
    pos[1] += 0.5f;
    static const char player[] = "Player";
    if (coop_request_create(c, player, pos, rot, s->handle)) {
        s->state = REQUESTED;
        rt_log("coop: slot %d: Player requested at %.1f %.1f %.1f", (int)(s - slots), pos[0], pos[1], pos[2]);
    }
}

// The owner's transform as a ForceSetMatrix argument: rows of rotation | position. In puppet mode the position is
// where the owner will be when the move lands: their last report moved on by its age and one tick (the move is
// applied in the puppet's next update) at their velocity, at most 0.25 s ahead.
// Rotation matrix (rows, as actor +0x2c0 holds it) to a unit quaternion x y z w, and back.
static void mat_quat(const float *r, float q[4]) {
    float tr = r[0] + r[4] + r[8], s;
    if (tr > 0) s = sqrtf(tr + 1) * 2, q[3] = s / 4, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s;
    else if (r[0] > r[4] && r[0] > r[8]) s = sqrtf(1 + r[0] - r[4] - r[8]) * 2, q[3] = (r[7] - r[5]) / s, q[0] = s / 4, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s;
    else if (r[4] > r[8]) s = sqrtf(1 + r[4] - r[0] - r[8]) * 2, q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = s / 4, q[2] = (r[5] + r[7]) / s;
    else s = sqrtf(1 + r[8] - r[0] - r[4]) * 2, q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = s / 4;
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; k++) q[k] = l > 0 ? q[k] / l : k == 3;
}
static void quat_mat(const float q[4], float *r) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
// Puppet mode, smoothing (TOTK_COOP_PUPPET_DELAY_MS, default 60; 0 = the latest report, predicted): the puppet is
// shown where its owner was that long ago, interpolated between the two reports around that moment (position
// linearly, rotation by normalised quaternion blend). Past the newest report it carries on at the owner's velocity
// for at most 0.1 s. Reports arrive about 30 times a second and frames do not line up with them, so taking the latest
// one each frame moves the puppet in uneven steps (the jitter).
static int puppet_delay_ms(void) {
    static int d = -1;
    if (d < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_DELAY_MS"); d = e ? atoi(e) : 60; }
    return d;
}
static int owner_smoothed(Slot *s, float m[12]) {
    int coop_puppet_mode(void);   // coop_world.c
    if (!coop_puppet_mode() || puppet_delay_ms() <= 0) return 0;
    uint64_t t = plat_now_ns() - (uint64_t)puppet_delay_ms() * 1000000ull;
    float pos[3], q[4];
    int ok = 0;
    plat_mutex_lock(&lock);
    unsigned n = s->nhist < 16 ? s->nhist : 16;
    if (n) {
        unsigned newest = (s->nhist - 1) % 16;
        if (t >= s->hist[newest].ns) {   // past the newest: carry on at its velocity, briefly
            double a = (double)(t - s->hist[newest].ns) / 1e9;
            if (a > 0.1) a = 0.1;
            for (int k = 0; k < 3; k++) pos[k] = s->hist[newest].pos[k] + s->vel[k] * (float)a;
            memcpy(q, s->hist[newest].q, sizeof q);
            ok = 1;
        } else {
            for (unsigned i = 1; i < n && !ok; i++) {   // newest pair first
                unsigned b = (s->nhist - i) % 16, a = (s->nhist - i - 1) % 16;
                if (s->hist[a].ns > t) continue;
                float f = s->hist[b].ns > s->hist[a].ns ? (float)((double)(t - s->hist[a].ns) / (double)(s->hist[b].ns - s->hist[a].ns)) : 1;
                const float *qa = s->hist[a].q, *qb = s->hist[b].q;
                float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3], sg = dot < 0 ? -1.0f : 1.0f, l = 0;
                for (int k = 0; k < 3; k++) pos[k] = s->hist[a].pos[k] + (s->hist[b].pos[k] - s->hist[a].pos[k]) * f;
                for (int k = 0; k < 4; k++) q[k] = qa[k] * (1 - f) + qb[k] * sg * f, l += q[k] * q[k];
                l = sqrtf(l);
                for (int k = 0; k < 4; k++) q[k] = l > 0 ? q[k] / l : k == 3;
                ok = 1;
            }
            if (!ok) {   // older than everything kept: the oldest
                unsigned o = (s->nhist - n) % 16;
                memcpy(pos, s->hist[o].pos, 12), memcpy(q, s->hist[o].q, 16);
                ok = 1;
            }
        }
    }
    plat_mutex_unlock(&lock);
    if (!ok) return 0;
    float r[9];
    quat_mat(q, r);
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) m[i * 4 + k] = r[i * 3 + k];
        m[i * 4 + 3] = pos[i];
    }
    return 1;
}
static void owner_matrix(Slot *s, float m[12]) {
    if (owner_smoothed(s, m)) return;
    float ahead = 0;
    int coop_puppet_mode(void);   // coop_world.c
    static int predict = -1;   // TOTK_COOP_PUPPET_PREDICT=0: no prediction (for comparing)
    if (predict < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_PREDICT"); predict = !e || *e != '0'; }
    if (predict && coop_puppet_mode() && s->last_ns) {
        double a = (double)(plat_now_ns() - s->last_ns) / 1e9 + 1.0 / 30;
        ahead = a > 0.25 ? 0.25f : (float)a;
    }
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) m[r * 4 + k] = s->want_rot[r * 3 + k];
        m[r * 4 + 3] = s->want_pos[r] + s->vel[r] * ahead;
    }
}
static void snap(Cpu *c, Slot *s) {
    float m[12];
    owner_matrix(s, m);
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, s->actor, (uint64_t)(uintptr_t)m, 0, 0);
}

static void set_input(Slot *s, uint64_t buttons, const int32_t sticks[4]) {
    plat_mutex_lock(&lock);
    s->buttons = buttons;
    for (int k = 0; k < 4; k++) s->stick[k] = sticks[k] / 32767.0f;
    plat_mutex_unlock(&lock);
}

void coop_players_tick(Cpu *c, int numpad3) {
    if (frame_step_world_us() <= 0) return;   // not in the world yet
    coop_session_tests(frame_step_world_us());
    uint64_t me = coop_player_actor();
    if (!me) return;
    coop_world_tick(c, me);

    // This player's state out, when on a relay.
    if (coop_net_on()) {
        NetState st = {0};
        coop_rd(me + COOP_ACTOR_POS, st.pos, 12), coop_rd(me + COOP_ACTOR_ROT, st.rot, 36);
        camera_frame(st.cam_fwd, st.cam_right);
        st.buttons = local_buttons;
        memcpy(st.gear, my_gear, sizeof st.gear);
        // Test: report these items instead, comma-separated in slot order (weapon, bow, shield, head, body, legs);
        // an empty field keeps the real one.
        const char *fake = cfg_get("TOTK_COOP_FAKEGEAR");
        for (int k = 0; fake && *fake && k < COOP_GEAR_SLOTS; k++) {
            size_t n = strcspn(fake, ",");
            if (n && n < 32) memcpy(st.gear[k], fake, n), st.gear[k][n] = 0;
            fake += n + (fake[n] == ',');
        }
        memcpy(st.sticks, local_sticks, sizeof st.sticks);
        // A menu is open here when the clock has stopped (the game stops time in its menus) and Link has not moved
        // for 150 ms: the game keeps updating Link in the inventory, so that is no sign (pair test 2026-11-04);
        // where time stands still in play (if anywhere), Link moving with the input keeps it flowing.
        static float last_t = -2, last_p[3];
        static uint64_t t_moved, p_moved;
        uint64_t now_ns = plat_now_ns();
        float tnow = day_seconds();
        if (tnow != last_t) last_t = tnow, t_moved = now_ns;
        float dpx = st.pos[0] - last_p[0], dpy = st.pos[1] - last_p[1], dpz = st.pos[2] - last_p[2];
        if (dpx * dpx + dpy * dpy + dpz * dpz > 0.0004f) memcpy(last_p, st.pos, 12), p_moved = now_ns;
        int paused = now_ns - t_moved > 150000000ull && now_ns - p_moved > 150000000ull;
        static int was_paused = -1;
        if (paused != was_paused)
            rt_log("coop: this Link %s (input sent %s; clock still %.2f s, Link still %.2f s, time %.2f, at %.3f %.3f %.3f)",
                   paused ? "is paused" : "runs", paused ? "neutral" : "as is", (now_ns - t_moved) / 1e9,
                   (now_ns - p_moved) / 1e9, tnow, st.pos[0], st.pos[1], st.pos[2]);
        was_paused = paused;
        if (paused) {   // a menu is open here: the sticks and buttons move through it, not Link
            st.buttons = 0;
            memset(st.sticks, 0, sizeof st.sticks);
        }
        st.day_seconds = day_seconds();
        read_weather(c, me);
        st.weather = weather_now;
        const char *fw = cfg_get("TOTK_COOP_FAKEWEATHER");   // test: report this weather type instead
        if (fw && *fw) st.weather = atoi(fw);
        coop_net_publish(&st);
    }
    uint64_t my_eq = eq_of_actor(me);
    if (my_eq) read_my_gear(my_eq);
    float fwd[2], right[2];
    int have_cam = camera_frame(fwd, right);

    // The host's world (for now the player with the lowest id): its time of day, seen below.
    int host_id = coop_net_on() && coop_net_id() ? coop_net_id() : 0;
    float host_time = -1;
    int32_t host_weather = -1;
    int have_host = 0;

    // Remote players in, slots for new ones, let go of the ones that left.
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        NetState st;
        uint64_t age = 0;
        Slot *s = slot_for(SRC_NET, id);
        if (!coop_net_on() || !coop_net_remote(id, &st, &age)) {
            if (s && s->state == LIVE) {
                // Their Link goes: the game's own delete request (as AI node OneShotGenericDeleteThisActor makes it).
                // The slot stays LET_GO, driven with no input, until the actor reads as going away.
                s->state = LET_GO;
                set_input(s, 0, (const int32_t[4]){0, 0, 0, 0});
                if (s->actor) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->actor, COOP_DELETE_REASON, 0, 0);
                rt_log("coop: remote player %d left; their Link is deleted", id);
            } else if (s && s->state != LET_GO) s->state = FREE;
            continue;
        }
        if (id < host_id && age < 2000000000ull) host_id = id, host_time = st.day_seconds, host_weather = st.weather, have_host = 1;
        if (!s && !(s = slot_new(SRC_NET, id))) continue;
        if (st.seq != s->last_seq) {   // a new report: the owner's velocity since the previous one
            uint64_t at = plat_now_ns() - age;
            double dt = s->have_want && s->last_ns ? (double)(at - s->last_ns) / 1e9 : 0;
            float d[3] = {st.pos[0] - s->want_pos[0], st.pos[1] - s->want_pos[1], st.pos[2] - s->want_pos[2]};
            float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            for (int k = 0; k < 3; k++) s->vel[k] = dt > 0.005 && dt < 0.5 && d2 < 100 ? (float)(d[k] / dt) : 0;   // 10 m: a warp
            s->last_seq = st.seq, s->last_ns = at;
            plat_mutex_lock(&lock);
            unsigned w = s->nhist % 16;
            if (d2 >= 100) s->nhist = 0, w = 0;   // a warp: no blending across it
            s->hist[w].ns = at;
            memcpy(s->hist[w].pos, st.pos, 12);
            mat_quat(st.rot, s->hist[w].q);
            s->nhist++;
            plat_mutex_unlock(&lock);
            // Puppet mode: how far the puppet was from where its owner was at the same moment (the report's send time).
            int best = -1;
            for (int k = 0; k < 32 && k < s->nseen; k++) {
                int64_t dd = (int64_t)(s->seen[k].ns - at);
                if (dd < 0) dd = -dd;
                if (dd < 40000000 && (best < 0 || dd < llabs((int64_t)(s->seen[best].ns - at)))) best = k;
            }
            if (best >= 0) {
                float ex = s->seen[best].pos[0] - st.pos[0], ey = s->seen[best].pos[1] - st.pos[1], ez = s->seen[best].pos[2] - st.pos[2];
                float e = sqrtf(ex * ex + ey * ey + ez * ez);
                if (e > s->err_max) s->err_max = e;
                s->err_sum += e, s->err_n++;
            }
            if (s->err_n >= 30) {
                rt_log("coop: puppet: slot %d error vs its owner at the same moment: mean %.2f m, max %.2f m (owner y %.1f) at %.1f s",
                       (int)(s - slots), s->err_sum / s->err_n, s->err_max, st.pos[1], frame_step_world_us() / 1e6);
                s->err_max = s->err_sum = 0, s->err_n = 0;
            }
        }
        memcpy(s->want_pos, st.pos, 12), memcpy(s->want_rot, st.rot, 36), s->have_want = 1;
        for (int k = 0; k < COOP_GEAR_SLOTS; k++) {
            memcpy(s->gear_want[k], st.gear[k], 32);
            s->gear_want[k][31] = 0;
        }
        // The owner's stick in world space (their camera), then in this PC's camera frame.
        int32_t sticks[4];
        for (int k = 0; k < 4; k += 2) {
            float x = st.sticks[k] / 32767.0f, y = st.sticks[k + 1] / 32767.0f;
            float wx = y * st.cam_fwd[0] + x * st.cam_right[0], wz = y * st.cam_fwd[1] + x * st.cam_right[1];
            float lx = have_cam ? wx * right[0] + wz * right[1] : x, ly = have_cam ? wx * fwd[0] + wz * fwd[1] : y;
            if (k == 2) lx = x, ly = y;   // the right stick turns the owner's camera, not a direction: as is
            sticks[k] = (int32_t)(lx * 32767.0f), sticks[k + 1] = (int32_t)(ly * 32767.0f);
        }
        set_input(s, st.buttons, sticks);
        s->idle = !st.buttons && abs(st.sticks[0]) < 3000 && abs(st.sticks[1]) < 3000;
    }
    int others = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) others += slots[i].state != FREE && slots[i].source == SRC_NET;
    world_role = !coop_net_on() || !coop_net_id() ? 0 : have_host ? 2 : others ? 1 : 0;
    if (host_time >= 0) follow_host_time(c, host_time);
    else if (!have_host) own_time_back(c);
    if (have_host && host_weather >= 0) follow_host_weather(c, host_weather);
    else if (!have_host && weather_forced >= 0) follow_host_weather(c, -1);   // the host left, or this game is the host

    // A second gamepad on this PC drives one more Link (Numpad 3 spawns it; the pad alone does not).
    Slot *pad = slot_for(SRC_PAD, 0);
    if (numpad3 && !pad) pad = slot_new(SRC_PAD, 0);
    if (pad) {
        uint64_t b = 0;
        int32_t sticks[4] = {0, 0, 0, 0};
        if (!coop_p2_poll(&b, &sticks[0], &sticks[1], &sticks[2], &sticks[3])) b = 0;
        set_input(pad, b, sticks);
    }

    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        Slot *s = &slots[i];
        if (s->state == WANTED) spawn_slot(c, s);
        else if (s->state == REQUESTED) {
            int st = coop_handle_status(s->handle);
            if (st == 4) { rt_log("coop: slot %d: the game could not make the Player", i); s->state = WANTED; }
            if (st != 3) continue;
            s->actor = coop_claim(c, s->handle, s->link, &s->ref);
            if (!s->actor || s->actor == me) { rt_log("coop: slot %d: claim gave no new Player", i); s->state = WANTED; continue; }
            coop_copy_dependencies(c, me, s->actor, 0);
            s->state = LIVE;
            for (int u = 0; u < 16; u++)
                if (unplaced_eq[u] && gear_owner(unplaced_eq[u], me)) unplaced_eq[u] = 0;
            rt_log("coop: slot %d: Player %llx in the world", i, (unsigned long long)s->actor);
        } else if (s->state == LIVE || s->state == LET_GO) {
            int32_t st = -1;
            if (!s->actor || (coop_rd(s->actor + COOP_ACTOR_STATE, &st, 4) && (st < 2 || st > 6))) {
                rt_log("coop: slot %d: its Player went away (state %d)", i, st);
                for (int e = 0; e < 32; e++)   // its EquipmentUser goes with it; a new Player may reuse the address
                    if (s->actor && eq_owner[e].actor == s->actor) eq_owner[e].actor = eq_owner[e].eq = 0;
                s->actor = 0, s->state = s->state == LIVE ? WANTED : FREE;
                continue;
            }
            // The owner changed an item: make that slot the owner's item.
            uint64_t eq = s->state == LIVE && s->source == SRC_NET ? eq_of_actor(s->actor) : 0;
            char held[COOP_GEAR_SLOTS][32];
            if (eq) eq_gear(eq, held);
            for (int k = 0; eq && k < COOP_GEAR_SLOTS; k++)
                if (s->gear_want[k][0] && strcmp(s->gear_want[k], s->gear_have[k])) {
                    memcpy(s->gear_have[k], s->gear_want[k], 32);
                    int none = !strcmp(s->gear_want[k], GEAR_NONE);
                    if ((none && k > 2) || !strcmp(held[k], s->gear_want[k])) continue;   // nothing to change
                    rt_log("coop: slot %d: owner's gear slot %d is %s", i, coop_gear_slot[k], s->gear_want[k]);
                    give_gear(c, eq, k, none ? "" : s->gear_want[k], s->gear_str[k]);
                }
            int coop_puppet_mode(void);   // coop_world.c
            if (s->state == LIVE && s->source == SRC_NET && s->have_want && coop_puppet_mode()) {
                snap(c, s);   // puppet mode: it gets no input, so it is moved to its owner every tick
                static int said_snap;
                if (!said_snap++) rt_log("coop: puppet: slot %d follows its owner every tick", i);
                int k = s->nseen++ % 32;
                if (coop_rd(s->actor + COOP_ACTOR_POS, s->seen[k].pos, 12)) s->seen[k].ns = plat_now_ns();
                else s->seen[k].ns = 0;
            } else if (s->state == LIVE && s->source == SRC_NET && s->have_want) {
                float p[3];
                float lim = s->idle ? COOP_SNAP_IDLE_M : COOP_SNAP_MOVING_M;
                if (s->snap_wait) s->snap_wait--;
                else if (coop_rd(s->actor + COOP_ACTOR_POS, p, 12)) {
                    float dx = p[0] - s->want_pos[0], dy = p[1] - s->want_pos[1], dz = p[2] - s->want_pos[2];
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > lim * lim) {
                        snap(c, s);
                        s->snap_wait = 10;
                        if (++s->snaps <= 20 || s->snaps % 50 == 0)
                            rt_log("coop: slot %d: put back %.2f m (%s); %u so far", i, sqrtf(d2), s->idle ? "idle" : "moving", s->snaps);
                    }
                }
            }
        }
    }
}

uint64_t coop_players_first(void) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state == LIVE) return slots[i].actor;
    return 0;
}
