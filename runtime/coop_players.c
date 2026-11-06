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
    int ride_owner;                    // the owner's Link rides that player's object ride_id at ride_off (proxy_drive)
    uint32_t ride_id;
    float ride_off[3];
    char fuse_want[3][32], fuse_have[3][32];   // weapon, bow, shield: the owner's fused material; as last made here
    CoopObj objs_want[COOP_OBJS];      // the owner's objects, as its OBJS packets last said (objs_receive)
    int objs_heard[COOP_OBJS];         // when each was last heard of
    struct {                           // their copies here (arrow_copies)
        uint32_t id;
        int state;                     // 0 free, 1 wanted, 2 requested, 3 live
        char name[48];
        uint8_t flags;                 // the owner's CoopObj.flags
        float pos[3], rot[9];
        _Alignas(16) uint8_t handle[0x40], link[0x40];
        uint64_t actor, ref;
        int ticks;
    } mir[COOP_OBJS];
    _Alignas(16) uint8_t att_str[3][0x60];     // the material names handed to the weapon creates
    int shield_placed;                 // the owner's shield is off its back and out of hand (a shield surf):
    uint64_t shield_actor;             // the proxy's shield while shield_place puts it there (its own update skipped)
    float shield_local[3], shield_rot[9];   // where, in the owner's frame (NetState flags bit 11)
    int8_t dye_want[COOP_GEAR_SLOTS], dye_have[COOP_GEAR_SLOTS];   // the armor's dye colour, the same way (-1 none)
    int out_want, out_have;            // puppet mode: bits 0..2 weapon, bow, shield in the owner's hands; as last made
    float phys_pos[3];                 // puppet mode: where its physics step put it (before placement), for snap
    int since_snap;                    // ticks since its body was last teleported
    float body_pos[3], body_rot[9];    // its physics body's position and rotation (mLastTransform)
    int have_body;
    float top_last;                    // its highest body bone last frame (jitter watch)
    int have_phys;
    int live_ticks;
    int thaw;                          // proxy mode: frames of its own update still to run (a gear change)
    int atk_thaw;                      // proxy mode: of those, frames the owner's attack button is let through                    // ticks since it went LIVE (proxy mode freezes it after PROXY_WARMUP_TICKS)
    float px_top, px_y, px_root;              // proxy mode: its body top and world height last frame (jump watch)
    int px_have;
    _Alignas(16) uint8_t gear_str[COOP_GEAR_SLOTS][0x60];   // the names handed to the creates, kept while they run
} Slot;
static Slot slots[COOP_MAX_PLAYERS];
static int ride_owner_now;      // what this Link rides, for the state out (0 none; ride_carry)
static uint32_t ride_id_now;
static float ride_off_now[3];
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
// This PC's armor dye colours, from its own armor creates (main+0xc4961c's 4th argument; the slot build main+0xc4a5f4
// reads it from the save: -1 undyed, else a colour index 0..14).
static int8_t my_dye[COOP_GEAR_SLOTS] = {-1, -1, -1, -1, -1, -1};
// This PC's weapon, bow and shield's fused materials, from its own weapon creates (main+0xaf100c's 7th argument, a
// string object: AsbObj_RockParts_C_S_01 for a rock-fused stick; "" none). The proxy's copy was made with "" and
// showed no material (a play session, 2026-11-05).
static char my_fuse[3][32];

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
// out (weapons): 1 in hand, 0 put away, -1 as this Link has it now.
static void give_gear(Cpu *c, uint64_t eq, int k, const char *name, int dye, int out_want, uint8_t st[0x60], const char *fuse,
                      uint8_t att_st[0x60]) {
    uint64_t slot = (uint64_t)coop_gear_slot[k], np = gear_string(st, name);
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    if (k <= 2) {
        static const uint64_t hand_of[3] = {0, 3, 4};
        int out = out_want >= 0 ? out_want : (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, hand_of[k], 0, 0) & 1);
        uint64_t att = gear_string(att_st, fuse ? fuse : "");
        uint64_t sp = (sp0 - 0x100) & ~0xfull;
        uint64_t stack[6] = {0, 0, 0, (uint64_t)(uint32_t)-1, 0, (uint64_t)(uint32_t)-1};   // arguments 9..14
        memcpy((void *)(uintptr_t)sp, stack, sizeof stack);
        c->sp = sp;
        rt_call(c, MOD_MAIN_BASE + GEAR_WEAPON_CREATE, 8, eq, slot, np, (uint64_t)(out ? 1 : 2), (uint64_t)(uint32_t)-1,
                (uint64_t)(uint32_t)-1, att, (uint64_t)1);
    } else {
        c->sp = (sp0 - 0x100) & ~0xfull;
        rt_call(c, MOD_MAIN_BASE + GEAR_ARMOR_CREATE, 6, eq, slot, np, (uint64_t)(uint32_t)dye, (uint64_t)0, (uint64_t)1);
    }
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
}
static uint64_t unplaced_eq[16];   // EquipmentUsers whose Player was not known when their gear was made
static int8_t unplaced_dye[16][COOP_GEAR_SLOTS];   // their armor creates' dyes, kept until they are matched
static uint8_t unplaced_dye_set[16];               // which of those were seen (bit per gear slot)
static struct { uint64_t eq, actor; } eq_owner[32];   // EquipmentUser -> its Player, learned from their creates

static int gear_index(int slot) {
    for (int k = 0; k < COOP_GEAR_SLOTS; k++) if (coop_gear_slot[k] == slot) return k;
    return -1;
}
// The Player that owns an EquipmentUser: a word in its first 0x400 bytes equal to this PC's Link or a slot's actor.
static uint64_t gear_owner(uint64_t eq, uint64_t me);
static Slot *slot_of_actor(uint64_t actor);
static int eq_off = -1;   // where a Player keeps its EquipmentUser: found on this PC's Link once its gear is built
// An EquipmentUser still belongs to this actor: a word in its first 0x400 bytes is the actor (a reloaded save makes
// new ones, and an actor can come back at an old address).
static int eq_points_to(uint64_t eq, uint64_t actor) {
    uint64_t words[0x400 / 8];
    if (!actor || !coop_rd(eq, words, sizeof words)) return 0;
    for (int w = 0; w < 0x400 / 8; w++) if (words[w] == actor) return 1;
    return 0;
}
static uint64_t eq_of_actor(uint64_t actor) {
    for (int i = 0; i < 32; i++)
        if (eq_owner[i].actor == actor && actor) {
            if (eq_points_to(eq_owner[i].eq, actor)) return eq_owner[i].eq;
            eq_owner[i].eq = eq_owner[i].actor = 0;   // stale: that EquipmentUser is gone
        }
    return eq_off >= 0 && actor ? coop_rd_ptr(actor + (uint64_t)eq_off) : 0;
}
// This PC's Link's EquipmentUser when the game built its gear before it was known (a save loading): matched now,
// and the dyes its armor creates had become this PC's.
static void match_my_eq(uint64_t me) {
    if (!me || eq_of_actor(me)) return;
    for (int u = 0; u < 16; u++) {
        if (!unplaced_eq[u] || gear_owner(unplaced_eq[u], me) != me) continue;
        for (int k = 0; k < COOP_GEAR_SLOTS; k++)
            if (unplaced_dye_set[u] >> k & 1) {
                my_dye[k] = unplaced_dye[u][k];
                rt_log("coop: my gear slot %d has dye %d (made before this Link was known)", coop_gear_slot[k], my_dye[k]);
            }
        unplaced_eq[u] = 0, unplaced_dye_set[u] = 0;
        return;
    }
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

static void keep_player_eq(uint64_t me, uint64_t my_eq);
int coop_players_gear(Cpu *c, uint64_t addr) {
    uint64_t eq = c->x[0], me = coop_player_actor();
    if (me) keep_player_eq(me, eq_of_actor(me));   // a new Player's gear is being built: the field may just have moved
    int k = gear_index((int)(uint32_t)c->x[1]);
    if (k < 0) return 0;
    uint64_t owner = me ? gear_owner(eq, me) : 0;
    static int eqdbg = -1;   // TOTK_COOP_TEST_EQLOG=1: every gear create seen here
    if (eqdbg < 0) eqdbg = cfg_get("TOTK_COOP_TEST_EQLOG") != NULL;
    if (eqdbg) {
        uint64_t np0 = coop_rd_ptr(c->x[2]);
        char nm0[32] = "";
        if (np0) coop_rd(np0, nm0, sizeof nm0 - 1);
        char chain[300];
        int m = 0;
        uint64_t fp = c->x[29];
        for (int d = 0; d < 12 && fp && m < 280; d++) {   // the guest frame chain (x29 links), return addresses
            uint64_t next = coop_rd_ptr(fp), ret = coop_rd_ptr(fp + 8);
            if (!ret) break;
            m += snprintf(chain + m, sizeof chain - m, " %llx", (unsigned long long)(ret - MOD_MAIN_BASE));
            if (next <= fp) break;
            fp = next;
        }
        chain[m] = 0;
        char att0[48] = "";   // a weapon create's 7th argument: the attachment (fused material) name, as a string object
        if (addr == MOD_MAIN_BASE + GEAR_WEAPON_CREATE) { uint64_t ap = coop_rd_ptr(c->x[6]); if (ap) coop_rd(ap, att0, sizeof att0 - 1); }
        rt_log("coop: test: create main+%#llx eq %llx slot %d %s dye %d att '%s', me %llx, owner %llx%s; from%s", (unsigned long long)(addr - MOD_MAIN_BASE),
               (unsigned long long)eq, (int)(uint32_t)c->x[1], nm0, (int)(int32_t)c->x[3], att0, (unsigned long long)me, (unsigned long long)owner,
               owner && owner != me ? " (a co-op Link)" : "", chain);
    }
    if (!owner) {   // a Player not claimed yet (a new co-op Link builds its gear first, or this PC's Link while a
                    // save loads): matched after its claim or by match_my_eq; its armor dyes are kept for then
        for (int i = 0; i < 16; i++)
            if (unplaced_eq[i] == eq || !unplaced_eq[i]) {
                if (unplaced_eq[i] != eq) unplaced_dye_set[i] = 0;
                unplaced_eq[i] = eq;
                if (k > 2 && addr == MOD_MAIN_BASE + 0xc4961c)
                    unplaced_dye[i][k] = (int8_t)(int32_t)c->x[3], unplaced_dye_set[i] |= (uint8_t)(1 << k);
                break;
            }
        return 0;
    }
    uint64_t np = coop_rd_ptr(c->x[2]);
    char name[32] = "";
    if (np) coop_rd(np, name, sizeof name - 1);
    if (owner == me) {   // this PC's Link: its gear is read from its table, its armor's dye here
        learn_eq_off(me, eq);
        if (k > 2 && addr == MOD_MAIN_BASE + 0xc4961c && (int8_t)(int32_t)c->x[3] != my_dye[k]) {
            my_dye[k] = (int8_t)(int32_t)c->x[3];
            rt_log("coop: my gear slot %d (%s) has dye %d", coop_gear_slot[k], name, my_dye[k]);
        }
        if (k <= 2 && addr == MOD_MAIN_BASE + GEAR_WEAPON_CREATE) {   // the fused material
            char att[32] = "";
            uint64_t ap = coop_rd_ptr(c->x[6]);
            if (ap) coop_rd(ap, att, sizeof att - 1);
            if (strcmp(att, my_fuse[k])) {
                memcpy(my_fuse[k], att, 32);
                rt_log("coop: my gear slot %d (%s) is fused with %s", coop_gear_slot[k], name, att[0] ? att : "nothing");
            }
        }
        return 0;
    }
    // A remote Link's own build (its first gear, or the game rebuilding a slot): the owner's item in place of the save's.
    Slot *s = slot_of_actor(owner);
    if (!s || !s->gear_want[k][0]) return 0;
    const char *want = strcmp(s->gear_want[k], GEAR_NONE) ? s->gear_want[k] : "";
    if (k > 2 && addr == MOD_MAIN_BASE + 0xc4961c && want[0]) c->x[3] = (uint64_t)(uint32_t)(int32_t)s->dye_want[k];   // the owner's dye
    if (k <= 2 && addr == MOD_MAIN_BASE + GEAR_WEAPON_CREATE && want[0]) c->x[6] = gear_string(s->att_str[k], s->fuse_want[k]);   // its material
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
            memset(s->fuse_want, 0, sizeof s->fuse_want), memset(s->fuse_have, 0, sizeof s->fuse_have);
            memset(s->objs_want, 0, sizeof s->objs_want), memset(s->mir, 0, sizeof s->mir);
            memset(s->dye_want, -1, sizeof s->dye_want), memset(s->dye_have, -1, sizeof s->dye_have);
            s->out_want = 0, s->out_have = -1, s->have_phys = 0, s->top_last = -1e9f, s->have_body = 0, s->shield_actor = 0, s->shield_placed = 0;
            s->live_ticks = 0, s->px_have = 0, s->thaw = 0;
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
// ---- proxy mode (the default; TOTK_COOP_PROXY=0 off, docs/coop/proxy-plan.md stage 1): after PROXY_WARMUP_TICKS of normal updates
// (its gear and its input pose object are made then) a remote player's Link runs none of its own update: its calc and
// update phases are skipped (but for a short thaw after the owner changes gear or takes out or puts away an item:
// that is the update's own work). In place of its update phase main+0x245030 the driver below puts it where its owner is
// (actor transform, model world matrix +0x1f8 with the dirty bit), poses its body (main+0x283c750 with the input pose
// object kept from before; the hook on it puts the owner's pose in) and pushes the model matrix to the model units
// (main+0x24cc30 (model), which also places parts hung on body bones).
#define PROXY_WARMUP_TICKS 180
#define PROXY_THAW_TICKS 30   // a thaw: about half a second of its own update (a gear or held-item change)
#define PROXY_MODEL_PUSH 0x24cc30
#define PROXY_POSE_WRITER 0x283c750
static void owner_matrix(Slot *s, float m[12]);
static float body_top(uint64_t actor);
static float body_top_root(uint64_t actor, float *root_y);
static void top_log(const char *who, uint64_t actor);
int coop_proxy_mode(void);   // coop_world.c
uint64_t coop_puppet_pose_obj(uint64_t skel);   // coop_world.c
static uint64_t body_skeleton(uint64_t actor) {
    static const uint32_t path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};
    uint64_t sk = actor;
    for (int k = 0; k < 8 && sk; k++) sk = coop_rd_ptr(sk + path[k]);
    return sk;
}
static int proxied(const Slot *s) {
    return coop_proxy_mode() && s->source == SRC_NET && s->state == LIVE && s->have_want && s->live_ticks > PROXY_WARMUP_TICKS &&
           coop_puppet_pose_obj(body_skeleton(s->actor));
}
static int ride_place(const Slot *s, float out[3]);   // the owner rides an object: where, here
static void shield_place(Cpu *c, Slot *s);
static void proxy_drive(Cpu *c, Slot *s) {
    uint64_t actor = s->actor;
    float m[12], pos[3], rot[9];
    owner_matrix(s, m);
    {   // its owner rides an object: put where it stands on this game's one of it, not at the delayed place
        float rp[3];
        if (ride_place(s, rp)) m[3] = rp[0], m[7] = rp[1], m[11] = rp[2];
    }
    for (int r = 0; r < 3; r++) {
        pos[r] = m[r * 4 + 3];
        for (int k = 0; k < 3; k++) rot[r * 3 + k] = m[r * 4 + k];
    }
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), pos, 12);
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_ROT), rot, 36);
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x8) + 0x28);
    if (model) {
        memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        ST8(model + 0x240, LD8(model + 0x240) | 1);
    }
    uint64_t sk = body_skeleton(actor), pose = coop_puppet_pose_obj(sk);
    static int no_pose = -1;   // TOTK_COOP_PROXY_NOPOSE=1: the body pose is not driven (control)
    if (no_pose < 0) no_pose = cfg_get("TOTK_COOP_PROXY_NOPOSE") != NULL;
    int coop_proxy_put_records(uint64_t skel);   // coop_world.c
    if (sk && !no_pose && !coop_proxy_put_records(sk) && pose) coop_guest_call(c, MOD_MAIN_BASE + PROXY_POSE_WRITER, pose, sk, 0, 0);
    if (model) coop_guest_call(c, MOD_MAIN_BASE + PROXY_MODEL_PUSH, model, 0, 0, 0);
    // A shield surf: the shield where the owner's is. After the push, which puts items on their body bones (on the
    // back for a shield out of hand): put there from the tick, it was back there every other frame (surf.txt).
    shield_place(c, s);
    // Jump watch: its body top (model space, skeleton +0x20) and its world height, frame to frame; standing = the
    // owner's reported velocity under 5 cm/s with no stick or buttons held. Summary every 600 frames, each standing jump over 1 cm logged.
    float root_y = 0, top = body_top_root(actor, &root_y);
    float v2 = s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2];
    int standing = v2 < 0.05f * 0.05f && s->idle;   // and no stick or buttons (running into a wall keeps it in place)
    static int frames, stand_frames, stand_jumps, top_changed, said;
    static float stand_max;
    frames++;
    if (s->px_have) {
        float dt = fabsf(top - s->px_top), dy = fabsf(pos[1] - s->px_y), dr = fabsf(root_y - s->px_root), d = dt > dy ? dt : dy;
        if (dr > d) d = dr;
        if (top != s->px_top) top_changed++;
        if (standing) {
            stand_frames++;
            if (d > stand_max) stand_max = d;
            if (d > 0.01f && ++stand_jumps <= 100)
                rt_log("coop: proxy: slot %d standing jump at %.3f s: body top %+.1f cm, root bone %+.1f cm, height %+.1f cm", (int)(s - slots),
                       frame_step_world_us() / 1e6, (top - s->px_top) * 100, (root_y - s->px_root) * 100, (pos[1] - s->px_y) * 100);
        }
    }
    s->px_top = top, s->px_y = pos[1], s->px_root = root_y, s->px_have = 1;
    if (frames % 600 == 0) {
        rt_log("coop: proxy: slot %d: %d frames, body top changed in %d; standing %d frames, %d jumps over 1 cm, largest %.2f cm",
               (int)(s - slots), frames, top_changed, stand_frames, stand_jumps, stand_max * 100);
        top_changed = 0, stand_frames = 0, stand_max = 0;
    }
    if (!said++) rt_log("coop: proxy: slot %d is driven from %.3f s (its update no longer runs; pose object %llx, model %llx)", (int)(s - slots), frame_step_world_us() / 1e6,
                        (unsigned long long)pose, (unsigned long long)model);
    top_log("puppet", actor);
}
static void keep_player_eq(uint64_t me, uint64_t my_eq);
static int copy_place(Cpu *c, uint64_t actor, uint64_t addr, int calc);
// Picking up another player's item (the tester's list, 2026-11-05). A frozen copy could not be picked up
// (pickup_b_3rd.txt: three A presses beside a copied apple at rest, nothing). An item's copy (Item_, Weapon_) runs its
// own update instead; when it goes away here (state 7..11, or its actor is something else now: picked up), this game
// tells the owner (an OBJS packet with header word 1: the entries' ids, each with its owner's player id in flags),
// which deletes its own, and does not make the copy again while the owner still reports it (5 s).
static int pickable(const char *nm) { return !strncmp(nm, "Item_", 5) || !strncmp(nm, "Weapon_", 7); }
static struct { int owner; uint32_t id; int until; } taken[32];
static int taken_tick;
static int was_taken(int owner, uint32_t id) {
    for (int i = 0; i < 32; i++) if (taken[i].id == id && taken[i].owner == owner && taken[i].until > taken_tick) return 1;
    return 0;
}
static void take_note(int owner, uint32_t id) {
    int f = 0;
    for (int i = 0; i < 32; i++) if (taken[i].until < taken[f].until) f = i;
    taken[f].owner = owner, taken[f].id = id, taken[f].until = taken_tick + 300;
    CoopObj o = {.id = id, .flags = (uint8_t)owner};
    snprintf(o.name, sizeof o.name, "taken");
    uint8_t pk[4 + sizeof o];
    uint16_t h[2] = {1, 1};
    memcpy(pk, h, 4), memcpy(pk + 4, &o, sizeof o);
    coop_net_objs_send(pk, sizeof pk);
}
static void place_copy(Cpu *c, Slot *s, int k);
// The proxy's shield during a shield surf runs none of its own update: put on its body bone by it, it was back on
// the proxy's back each frame (surf.txt, 2026-11-05). shield_place puts it where the owner's is instead.
static int surf_shield(uint64_t actor) {
    for (int i = 0; actor && i < COOP_MAX_PLAYERS; i++) if (slots[i].actor && slots[i].shield_actor == actor) return 1;
    return 0;
}
int coop_players_phase(Cpu *c, uint64_t addr) {
    if (!inside && (copy_place(c, c->x[0], addr, 0) || surf_shield(c->x[0]))) return 1;
    if (inside || !slot_of_actor(c->x[0])) return 0;
    {   // before any co-op Link's update: the save-load rebuild runs in one (keep_player_eq)
        uint64_t me = coop_player_actor();
        if (me) keep_player_eq(me, eq_of_actor(me));
    }
    {
        Slot *ps = slot_of_actor(c->x[0]);
        if (proxied(ps)) {
            if (ps->thaw > 0) {   // a gear change: its own update phase runs, then the driver as always
                inside = 1;
                c->pc = addr;
                rt_dispatch(c);
                inside = 0;
                // Counted down here, in frames its update ran: counted per tick, a change that came while this game
                // was in a menu (no updates) ran out unseen and the proxy kept its old armor (menu_3rd.txt, 2026-11-05).
                if (addr == MOD_MAIN_BASE + 0x245030) ps->thaw--, ps->atk_thaw -= ps->atk_thaw > 0;
            }
            if (addr == MOD_MAIN_BASE + 0x245030) proxy_drive(c, ps);
            return 1;
        }
    }
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
// The highest body bone of an actor's body skeleton, above its root bone (bone 0); -1e9 unknown. The skeleton's
// +0x20 matrices (0x40 a bone, translation in row 3) are world-space but from an origin of the game's own (the
// numbers differ from the actor position by metres, and by different amounts in two games), so only differences
// between bones compare across games. root_y (optional): the root bone's own height in those matrices.
static __thread int top_bone;   // the bone body_top_root found highest
static float body_top_root(uint64_t actor, float *root_y) {
    static const uint32_t path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};
    uint64_t sk = actor;
    for (int k = 0; k < 8 && sk; k++) sk = coop_rd_ptr(sk + path[k]);
    uint16_t nb = 0;
    uint64_t wb = sk ? coop_rd_ptr(sk + 0x20) : 0;
    static __thread float rec[600][16];
    if (!wb || !coop_rd(sk + 0x48, &nb, 2) || nb == 0 || nb > 600 || !coop_rd(wb, rec, (size_t)nb * 64)) return -1e9f;
    float top = -1e9f;
    int tb = 0;
    for (int b = 0; b < nb; b++) if (rec[b][13] > top) top = rec[b][13], tb = b;
    if (root_y) *root_y = rec[0][13];
    top_bone = tb;
    return top - rec[0][13];
}
static float body_top(uint64_t actor) { return body_top_root(actor, NULL); }
// TOTK_COOP_TOP_LOG=<s>: for 4 s from then, every frame, this PC's Link's and each puppet's body_top (to tell the
// owner's own movement from jitter: game A's own Link against game B's puppet of it).
// TOTK_COOP_TOP_LOG=<s>[,<length s>] (default 4 s). Also the actor's position (tools/coop/top_compare.py lines the
// owner's log up with the other game's).
static void top_log(const char *who, uint64_t actor) {
    static double at = -2, len = 4;
    if (at == -2) {
        const char *e = cfg_get("TOTK_COOP_TOP_LOG");
        at = e ? atof(e) : -1;
        if (e && strchr(e, ',')) len = atof(strchr(e, ',') + 1);
    }
    double s = frame_step_world_us() / 1e6;
    if (at < 0 || s < at || s > at + len) return;
    float p[3] = {0};
    coop_rd(actor + COOP_ACTOR_POS, p, 12);
    float root_y = 0, top = body_top_root(actor, &root_y);
    rt_log("coop: top %s %.3f %.4f %.4f %.4f %.4f %.4f bone %d", who, s, top, p[0], p[1], p[2], root_y, top_bone);
    static int dumped[2];
    int w = who[0] == 'm';
    if (s > at + 1 && !dumped[w]++) {   // a few of the body skeleton's +0x20 records as they are (layout check)
        uint64_t sk = actor;
        static const uint32_t path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};
        for (int k = 0; k < 8 && sk; k++) sk = coop_rd_ptr(sk + path[k]);
        uint64_t wb = sk ? coop_rd_ptr(sk + 0x20) : 0;
        static const int bones[5] = {0, 1, 2, 50, 100};
        for (int i = 0; wb && i < 5; i++) {
            float f[16];
            if (!coop_rd(wb + bones[i] * 64, f, 64)) break;
            rt_log("coop: skel %s bone %d: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", who, bones[i],
                   f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        }
    }
}
int coop_puppet_place(Cpu *c, uint64_t addr) {
    static int tl = -1;
    if (tl < 0) tl = cfg_get("TOTK_COOP_TOP_LOG") != NULL;
    if (tl && c->x[0] && c->x[0] == coop_player_actor()) top_log("mine", c->x[0]);
    void coop_test_ikdiff(uint64_t me);   // coop_world.c
    if (c->x[0] && c->x[0] == coop_player_actor()) {
        coop_test_ikdiff(c->x[0]);
        void coop_proxy_pack_mine(uint64_t me);   // coop_world.c: proxy mode sends this Link's final local records
        coop_proxy_pack_mine(c->x[0]);
    }
    Slot *s = slot_of_actor(c->x[0]);
    int coop_puppet_mode(void);   // coop_world.c
    if (!s || s->state != LIVE || s->source != SRC_NET || !s->have_want || !coop_puppet_mode()) return 0;
    static int place = -1;   // TOTK_COOP_PUPPET_PLACE: bit 0 the actor's transform, bit 1 the model's (default 3; 0 off)
    if (place < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_PLACE"); place = e ? atoi(e) : 3; }
    if (!place) return 0;
    static __thread int in;
    if (in) return 0;
    uint64_t actor = c->x[0];
    // Test (TOTK_COOP_PUPPET_SKIPPHYS=1): the step does not run for a puppet. Worse (2026-11-05): the skeleton's
    // matrices then jump by up to 2.8 m from frame to frame; the step does more than copy the body's transform.
    static int skip = -1;
    if (skip < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_SKIPPHYS"); skip = e && *e == '1'; }
    if (!skip) {
        in = 1;
        c->pc = addr;
        rt_dispatch(c);
        in = 0;
    } else c->x[0] = 1;   // as when it moved the actor
    {   // the physics body's own position (main+0x45ce70's [[actor+0x228]+0x50]+0x20 -> +0x150; its mLastTransform at
        // +0x98, 3x4 rows, main+0x25a91c), for snap's distance check
        uint64_t ctl = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50) + 0x20), body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
        float bm[12];
        if (body && coop_rd(body + 0x98, bm, sizeof bm)) {
            s->body_pos[0] = bm[3], s->body_pos[1] = bm[7], s->body_pos[2] = bm[11], s->have_body = 1;
            for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) s->body_rot[r * 3 + k] = bm[r * 4 + k];
        }
    }
    float m[12], pos[3] = {0}, rot[9];
    owner_matrix(s, m);
    {   // how far the physics step's transform is from the owner's (what placement covers), logged every 60 frames
        float pp[3], pr[9];
        static float dp_max, dr_max;
        static int dn;
        if (coop_rd(actor + COOP_ACTOR_POS, pp, 12) && coop_rd(actor + COOP_ACTOR_ROT, pr, 36)) {
            memcpy(s->phys_pos, pp, 12), s->have_phys = 1;
            for (int k = 0; k < 3; k++) { float d = fabsf(pp[k] - m[k * 4 + 3]); if (d > dp_max) dp_max = d; }
            for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) { float d = fabsf(pr[r * 3 + k] - m[r * 4 + k]); if (d > dr_max) dr_max = d; }
            if (++dn % 60 == 0) {
                rt_log("coop: puppet: physics vs owner before placement: position %.1f mm, rotation %.4f (max over 60 frames)", dp_max * 1000, dr_max);
                dp_max = dr_max = 0;
            }
        }
    }
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
    {   // Jitter watch: the puppet's highest body bone (model space, skeleton +0x20, row 3) frame to frame; a jump
        // over 3 cm is logged with what this frame did (whether its body was teleported since, the pose's age).
        static const uint32_t path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};
        uint64_t sk = actor;
        for (int k = 0; k < 8 && sk; k++) sk = coop_rd_ptr(sk + path[k]);
        uint16_t nb = 0;
        uint64_t wb = sk ? coop_rd_ptr(sk + 0x20) : 0;
        static float rec[127][16];
        static int jumps, frames;
        if (wb && coop_rd(sk + 0x48, &nb, 2) && nb == 127 && coop_rd(wb, rec, sizeof rec)) {
            float top = -1e9f;
            for (int b = 0; b < 127; b++) if (rec[b][13] > top) top = rec[b][13];
            frames++;
            if (s->top_last > -1e8f && fabsf(top - s->top_last) > 0.03f && ++jumps <= 200)
                rt_log("coop: puppet: slot %d body top moved %+.1f cm in a frame at %.3f s (body teleported %d ticks ago)",
                       (int)(s - slots), (top - s->top_last) * 100, frame_step_world_us() / 1e6, s->since_snap);
            if (frames % 1800 == 0) rt_log("coop: puppet: slot %d: %d body jumps over 3 cm in %d frames", (int)(s - slots), jumps, frames);
            s->top_last = top;
        }
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
    top_log("puppet", actor);
    static int said;
    if (!said++) rt_log("coop: puppet: slot %d is placed at its owner after its physics step", (int)(s - slots));
    return 1;
}
int coop_players_calc(Cpu *c, uint64_t addr) {
    Slot *s;
    if (!inside && (copy_place(c, c->x[0], addr, 1) || surf_shield(c->x[0]))) return 1;
    if (inside || !(s = slot_of_actor(c->x[0]))) return 0;
    {
        uint64_t me = coop_player_actor();
        if (me) keep_player_eq(me, eq_of_actor(me));
    }
    if (proxied(s) && s->thaw <= 0) return 1;   // proxy mode: its calc never runs (proxy_drive), but in a thaw
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
    // Proxy mode: no input, except the owner's attack button in an attack thaw, so its sword really swings here and
    // what it hits (a Zonai device's copy) gets the game's own hit (the tester: "try the proxy's sword", 2026-11-05).
    if (noinput) buttons = s->atk_thaw > 0 ? buttons & (1ull << 3) : 0, memset(stick, 0, sizeof stick);
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

static int attack_thaw_on(void) {   // TOTK_COOP_PROXY_SWING=0: the proxy does not swing here when its owner attacks
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_PROXY_SWING"); on = !(e && *e == '0'); }
    return on;
}
static void set_input(Slot *s, uint64_t buttons, const int32_t sticks[4]) {
    plat_mutex_lock(&lock);
    s->buttons = buttons;
    for (int k = 0; k < 4; k++) s->stick[k] = sticks[k] / 32767.0f;
    plat_mutex_unlock(&lock);
}
// Autobuild. The game labels each device it makes for an autobuild with the string "ImaginaryAutoBuildedActor" at
// actor +0x18 (where other actors carry their own name) and sets actor +0x668 bit 0x10000000, both when it creates
// it (main+0x6898dc); its code tests the label by name (main+0x9e6af0, main+0x1b37958; the AI query
// QuerySpecialPowerIsImaginaryAutoBuildedActor). The label stays once the build is confirmed: devices made from
// Zonaite keep it, and the game draws them green.
static int is_imaginary(uint64_t a) {
    static const char tag[] = "ImaginaryAutoBuildedActor";
    char buf[sizeof tag];
    uint64_t s = coop_rd_ptr(a + 0x18);
    return s && coop_rd(s, buf, sizeof buf) && !memcmp(buf, tag, sizeof tag);
}
// The preview, before the build is confirmed: the see-through devices; copied, it showed solid in the other game
// (a play session, 2026-11-05). A memory diff over a build (a play session, 2026-11-05) found what confirming does
// to every device at once: actor +0x440 gains bit 0x10 and +0x5d0 (a position, NaN until then) is filled in.
static int is_preview(uint64_t a) {
    uint32_t f440 = 0, p = 0;
    if (!is_imaginary(a) || !coop_rd(a + 0x440, &f440, 4) || !coop_rd(a + 0x5d0, &p, 4)) return 0;
    return !(f440 & 0x10) && p == 0x7fc00000;
}

// Test (TOTK_COOP_TEST_ARMORPROBE=1): what holds this Link's worn armor after a save loads. Once before (12 s into
// the world) and 3 s and 10 s after this Link moves more than 100 m in a tick (a load): the live armor and shield
// actors within 5 m of it, and for each the words in guest memory pointing at it and at its reference object (the
// word 0x40 before such a pointer), placed against this Link's known EquipmentUser and its table rows.
void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
struct probe_list { uint64_t me, a[16]; int n; };
static void probe_fn(uint64_t a, void *arg) {
    struct probe_list *pl = arg;
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    if (!strcmp(nm, "Player")) {
        float pp[3] = {0};
        coop_rd(a + COOP_ACTOR_POS, pp, 12);
        rt_log("coop: test: armorprobe: live Player %llx at %.1f %.1f %.1f%s", (unsigned long long)a, pp[0], pp[1], pp[2],
               a == pl->me ? " (this Link)" : slot_of_actor(a) ? " (a co-op Link)" : "");
    }
    if (strncmp(nm, "Armor_", 6) || strstr(nm, "Extra") || pl->n >= 16) return;
    float p[3], q[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(pl->me + COOP_ACTOR_POS, q, 12)) return;
    float d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
    if (d2 < 25) pl->a[pl->n++] = a;
    else if (strstr(nm, "_Upper")) rt_log("coop: test: armorprobe: %s %llx at %.1f %.1f %.1f, %.1f m from this Link", nm, (unsigned long long)a, p[0], p[1], p[2], sqrtf(d2));
}
static int heap_holders(uint64_t v, uint64_t *out, int max) {
    static uint64_t buf[0x10000 / 8];
    int n = 0;
    for (uint64_t a = 0x100000000ull; a < 0x240000000ull && n < max; a += sizeof buf) {
        if (!plat_mem_read(a, buf, sizeof buf)) continue;
        for (size_t i = 0; i < sizeof buf / 8 && n < max; i++) if (buf[i] == v) out[n++] = a + i * 8;
    }
    return n;
}
static void where(char *o, size_t n, uint64_t h, uint64_t eq) {
    if (eq && h >= eq && h < eq + 0x5000) {
        uint64_t off = h - eq;
        if (off >= GEAR_TABLE && off < GEAR_TABLE + 24 * 0x18) snprintf(o, n, "eq+%#llx (row %llu +%#llx)", (unsigned long long)off,
                 (unsigned long long)((off - GEAR_TABLE) / 0x18), (unsigned long long)((off - GEAR_TABLE) % 0x18));
        else snprintf(o, n, "eq+%#llx", (unsigned long long)off);
    } else snprintf(o, n, "%llx", (unsigned long long)h);
}
static void armor_probe(uint64_t me) {
    static int on = -1, done[3];
    static uint64_t load_ns;
    static float last[3];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ARMORPROBE") != NULL;
    if (!on) return;
    float p[3];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    float d2 = (p[0] - last[0]) * (p[0] - last[0]) + (p[1] - last[1]) * (p[1] - last[1]) + (p[2] - last[2]) * (p[2] - last[2]);
    if (last[0] != 0 && d2 > 100 * 100 && !load_ns) load_ns = plat_now_ns(), rt_log("coop: test: armorprobe: this Link moved %.0f m (a load)", sqrtf(d2));
    memcpy(last, p, 12);
    int which = -1;
    if (load_ns && !done[1] && plat_now_ns() - load_ns > 20000000000ull) which = 1;
    else if (load_ns && !done[2] && plat_now_ns() - load_ns > 40000000000ull) which = 2;
    if (which < 0) return;
    done[which] = 1;
    uint64_t eq = eq_of_actor(me);
    struct probe_list pl = {me, {0}, 0};
    coop_actors_live(probe_fn, &pl);
    rt_log("coop: test: armorprobe %d: Link %llx, EquipmentUser %llx, %d armor / shield actors within 5 m", which, (unsigned long long)me,
           (unsigned long long)eq, pl.n);
    for (int i = 0; i < pl.n; i++) {
        char nm[48];
        int32_t st = -1;
        coop_actor_name(pl.a[i], nm, sizeof nm);
        coop_rd(pl.a[i] + COOP_ACTOR_STATE, &st, 4);
        static uint64_t h[32], h2[32];
        int n = heap_holders(pl.a[i], h, 32);
        char line[900];
        int m = snprintf(line, sizeof line, "coop: test: armorprobe %d %s %llx state %d, held by %d:", which, nm, (unsigned long long)pl.a[i], st, n);
        for (int k = 0; k < n && m < 860; k++) {
            char w[64];
            where(w, sizeof w, h[k], eq);
            m += snprintf(line + m, sizeof line - m, " %s", w);
            {   // h[k] - 0x40 may be its reference object (a gear table row's first word is one)
                int n2 = heap_holders(h[k] - 0x40, h2, 32);
                if (n2) m += snprintf(line + m, sizeof line - m, " [ref %llx held by", (unsigned long long)(h[k] - 0x40));
                for (int j = 0; j < n2 && m < 860; j++) {
                    where(w, sizeof w, h2[j], eq);
                    m += snprintf(line + m, sizeof line - m, " %s", w);
                }
                if (n2) m += snprintf(line + m, sizeof line - m, "]");
            }
        }
        rt_log("%s", line);
    }
}
// The player's EquipmentUser as the game knows it: [[[main+0x39601e8]] +0x480]. After a save loads, main+0xd97d70
// rebuilds the gear of that one from the save (main+0xd97dac: c4a5f4 for slots 0, 8, 7, 9, 10, 11; called from
// main+0x1fe4ed4 in a Player's update). A co-op Link made
// later takes the global over, so the load's rebuild went to the co-op Link and this Link kept the first save's armor
// (load_3rd.txt: the 999 / 1043 / 1043 creates landed on the co-op Link's EquipmentUser, frame chain through
// main+0xd97d90; alone they land on this Link's, 2026-11-05). So whenever the global holds a co-op Link's
// EquipmentUser it is given this Link's back: every tick, and right before each co-op Link's calc and update phase
// (from the tick alone it came too late in one of two games: the rebuild had run on the new co-op Link already).
#define PLAYER_EQ_GLOBAL 0x39601e8
static void keep_player_eq(uint64_t me, uint64_t my_eq) {
    static int off = -1;   // TOTK_COOP_TEST_NOKEEPEQ=1: not done (test)
    if (off < 0) off = cfg_get("TOTK_COOP_TEST_NOKEEPEQ") != NULL;
    if (off) return;
    uint64_t obj = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + PLAYER_EQ_GLOBAL)), var = obj ? obj + 0x480 : 0, g = var ? coop_rd_ptr(var) : 0;
    static uint64_t last = 1;
    static unsigned fixes;
    if (g != last) {
        const char *who = !g ? "none" : g == my_eq ? "this Link's" : "another";
        for (int i = 0; i < COOP_MAX_PLAYERS; i++)
            if (slots[i].actor && (slots[i].state == LIVE || slots[i].state == LET_GO) && eq_points_to(g, slots[i].actor)) who = "a co-op Link's";
        rt_log("coop: the player's EquipmentUser ([[[main+0x39601e8]]+0x480]) is now %llx (%s; this Link's %llx)", (unsigned long long)g, who, (unsigned long long)my_eq);
        last = g;
    }
    // Any EquipmentUser that is not this Link's is put back: in one of two games the field changed before the new
    // co-op Link was claimed, so a check for "a co-op Link's" let it pass and the rebuild ran on it first.
    if (!g || !my_eq || g == my_eq || !eq_points_to(my_eq, me) || eq_points_to(g, me)) return;
    ST64(var, my_eq);
    if (++fixes <= 20) rt_log("coop: the player's EquipmentUser was another Player's (%llx): this Link's put back (%u)", (unsigned long long)g, fixes);
    last = my_eq;
}
// Test (TOTK_COOP_TEST_ARROWLOG=<from s>,<to s>): every 15 ticks in that window, each live actor with "Arrow" in its
// name: address, name, position, distance to this Link (what a bow's arrows are, held and in flight).
static void arrow_fn(uint64_t a, void *arg) {
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    float p[3] = {0}, q[3] = {0};
    coop_rd(a + COOP_ACTOR_POS, p, 12);
    coop_rd(*(uint64_t *)arg + COOP_ACTOR_POS, q, 12);
    float d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    if (!strstr(nm, "Arrow") && (d > 3 || !strcmp(nm, "Player"))) return;   // arrows, and anything near this Link
    int32_t st = -1;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    rt_log("coop: test: arrow %.2f s %llx %s at %.2f %.2f %.2f, %.2f m from this Link, state %d", frame_step_world_us() / 1e6,
           (unsigned long long)a, nm, p[0], p[1], p[2], d, st);
}
static void arrow_log(uint64_t me) {
    const char *e = cfg_get("TOTK_COOP_TEST_ARROWLOG");
    static int t;
    if (!e || ++t % 15) return;
    double now = frame_step_world_us() / 1e6, a = atof(e), b = strchr(e, ',') ? atof(strchr(e, ',') + 1) : a + 10;
    if (now < a || now > b) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);
    coop_actors_live(arrow_fn, &me);
}
// ---- arrows (the tester's list, 2026-11-05). A bow's arrow is an actor (NormalArrow...): made at the bow when it is drawn
// (1.4 m from Link), held there, and on release it flies (21 m in 0.12 s) and sticks where it hits (TOTK_COOP_TEST_
// ARROWLOG, bow_shoot_3rd.txt). Each game reports the arrows that were first seen within 3 m of its own Link, up to
// COOP_OBJS, with position and rotation, in its state, and a fused arrow's material with them; the other games show a
// copy of each: made with the same name, then after COPY_WARMUP ticks its own update is off and it is put where the
// owner's is every tick (place_copy), so it does not fly, hit or get picked up here. A copy goes when its arrow is no
// longer reported.
#define COPY_WARMUP 5
static struct { uint64_t actor; uint32_t id; int mine, seen, fused_to; } my_arrows[32];
static struct { uint64_t actor; int seen; } near_actors[64];   // other actors within 3 m of this Link, last tick seen
static uint32_t next_arrow_id = 1;
static int arrow_tick;
// Last tick ZR (npad bit 9) was held here. A bow is drawn with ZR: the first arrow is made on the press, and the next
// one goes on the bow 46 ticks after the release (bow_shoot_3rd.txt, 2026-11-05). An arrow made more than 3 s after
// ZR is not this Link's: each game made two arrows 0.45 m from each Link at load, and one with no other Link in yet
// took them as its own.
static int zr_tick = -1000;
static int copies_live[COOP_MAX_PLAYERS];   // per slot, its copies in state 3 (arrow_copies): copy_of runs for every actor
static int copy_of(uint64_t actor, Slot **out_s, int *out_k) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int k = 0; k < COOP_OBJS && actor && copies_live[i]; k++)
            if (slots[i].mir[k].state == 3 && slots[i].mir[k].actor == actor) { *out_s = &slots[i], *out_k = k; return 1; }
    return 0;
}
static void arrow_track_fn(uint64_t a, void *arg) {
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    Slot *cs;
    int ck;
    if (copy_of(a, &cs, &ck)) return;   // not one of the copies made here
    for (int i = 0; i < 32; i++) if (my_arrows[i].actor == a) { my_arrows[i].seen = arrow_tick; return; }
    float p[3], q[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(*(uint64_t *)arg + COOP_ACTOR_POS, q, 12)) return;
    float d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]), dco = 1e18f;
    if (!strstr(nm, "Arrow")) {
        // A fused arrow's material is an actor of its own: on choosing BombFruit while drawing, a BombFruit was made
        // 1.0 m from the arrow, moved with it, and was gone once the arrow hit (bow_fuse_3rd.txt, 2026-11-05). An
        // actor first seen within 1.5 m of an arrow of this Link, while ZR is in use, is sent with the arrows. The
        // actors near this Link are kept, so one that was already there (the Link's own gear) is not taken.
        if (d2 >= 9) return;
        int free = -1;
        for (int i = 0; i < 64; i++) {
            if (near_actors[i].actor == a) { near_actors[i].seen = arrow_tick; return; }
            if (free < 0 && (!near_actors[i].actor || arrow_tick - near_actors[i].seen > 30)) free = i;
        }
        if (free >= 0) near_actors[free].actor = a, near_actors[free].seen = arrow_tick;
        if (arrow_tick - zr_tick > 90 || arrow_tick < 30 || !strcmp(nm, "Player") || !strncmp(nm, "Armor_", 6) ||
            !strncmp(nm, "Weapon_", 7) || !strncmp(nm, "Accessory_", 10) || !strncmp(nm, "PlayerRaul", 10)) return;
        int by = 0;
        for (int i = 0; i < 32 && !by; i++) {
            float r[3];
            if (my_arrows[i].actor && my_arrows[i].mine && my_arrows[i].fused_to == 0 && coop_rd(my_arrows[i].actor + COOP_ACTOR_POS, r, 12))
                by = (p[0] - r[0]) * (p[0] - r[0]) + (p[1] - r[1]) * (p[1] - r[1]) + (p[2] - r[2]) * (p[2] - r[2]) < 2.25f ? (int)my_arrows[i].id : 0;
        }
        if (!by) return;
        for (int i = 0; i < 32; i++)
            if (!my_arrows[i].actor) {
                my_arrows[i].actor = a, my_arrows[i].mine = 1, my_arrows[i].seen = arrow_tick, my_arrows[i].fused_to = by;
                my_arrows[i].id = next_arrow_id++;
                rt_log("coop: my arrow %d is fused with %s (%u, %llx)", by, nm, my_arrows[i].id, (unsigned long long)a);
                return;
            }
        return;
    }
    // Mine only when nearer this Link than any co-op Link: with both standing together, each game took the other
    // proxy's quiver arrows and the other's copies as its own, and the two fed each other (2026-11-05). A co-op Link
    // counts before it is LIVE too.
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        float r[3];
        if (slots[i].actor && coop_rd(slots[i].actor + COOP_ACTOR_POS, r, 12)) {
            float e = (p[0] - r[0]) * (p[0] - r[0]) + (p[1] - r[1]) * (p[1] - r[1]) + (p[2] - r[2]) * (p[2] - r[2]);
            if (e < dco) dco = e;
        }
    }
    for (int i = 0; i < 32; i++)
        if (!my_arrows[i].actor) {
            my_arrows[i].actor = a, my_arrows[i].mine = arrow_tick - zr_tick <= 90 && d2 < 9 && d2 < dco, my_arrows[i].seen = arrow_tick;
            my_arrows[i].fused_to = 0;
            my_arrows[i].id = my_arrows[i].mine ? next_arrow_id++ : 0;
            if (my_arrows[i].mine) rt_log("coop: my arrow %u is %s (%llx)", my_arrows[i].id, nm, (unsigned long long)a);
            return;
        }
}
static void mat_quat(const float *r, float q[4]);
static void quat_mat(const float q[4], float *r);
static int report_arrows(uint64_t me, CoopObj *out) {
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    arrow_tick++;
    if (local_buttons >> 9 & 1) zr_tick = arrow_tick;
    coop_actors_live(arrow_track_fn, &me);
    int n = 0;
    memset(out, 0, sizeof(CoopObj) * COOP_OBJS);
    for (int i = 0; i < 32; i++) {
        if (!my_arrows[i].actor) continue;
        if (arrow_tick - my_arrows[i].seen > 30) { my_arrows[i].actor = 0; continue; }   // gone (not updated for 1 s)
        if (my_arrows[i].seen != arrow_tick) continue;
        if (!my_arrows[i].mine || n >= COOP_OBJS) continue;
        CoopObj *o = &out[n];
        float r[9];
        if (!coop_rd(my_arrows[i].actor + COOP_ACTOR_POS, o->pos, 12) || !coop_rd(my_arrows[i].actor + COOP_ACTOR_ROT, r, 36)) continue;
        o->id = my_arrows[i].id;
        coop_actor_name(my_arrows[i].actor, o->name, sizeof o->name);
        mat_quat(r, o->q);
        n++;
    }
    return n;
}
static int key_missing(uint32_t id);
// Test (TOTK_COOP_TEST_ARROWWATCH=1): the first arrow copy made here: its actor state (+0x24) and +0x28 every tick for
// 30 ticks, and a write watch on those 8 bytes for the same time (which game functions write them: what puts the copy
// to sleep, state 6, where this game's own arrows stay at 4).
static uint64_t aw_actor;
static int aw_ticks = -1;
static void arrow_watch_start(uint64_t a, const char *nm) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ARROWWATCH") != NULL;
    if (!on || aw_ticks >= 0 || !a || !strstr(nm, "Arrow")) return;
    int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
    extern int coop_watch_writes;
    coop_watch_writes = 1;
    aw_actor = a, aw_ticks = 0;
    coop_watch_arm(a + 0x24, 8);
}
static void arrow_watch_tick(void) {
    if (aw_ticks < 0 || aw_ticks > 30) return;
    int32_t st = (int32_t)LD32(aw_actor + COOP_ACTOR_STATE);   // read through the watch (coop_rd does not)
    uint32_t w = LD32(aw_actor + 0x28);
    rt_log("coop: test: arrow watch tick %d: state %d, +0x28 %08x", aw_ticks, st, w);
    if (++aw_ticks > 30) { void coop_watch_disarm(void); coop_watch_disarm(); }
}
static void arrow_copies(Cpu *c, Slot *s) {
    if (s == &slots[0]) arrow_watch_tick();
    for (int k = 0; k < COOP_OBJS; k++) {   // copies whose arrow is gone
        if (!s->mir[k].state) continue;
        int still = 0;
        for (int j = 0; j < COOP_OBJS && !still; j++) still = s->objs_want[j].id == s->mir[k].id;
        if (still) continue;
        if (s->mir[k].actor) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->mir[k].actor, COOP_DELETE_REASON, 0, 0);
        if (s->mir[k].state == 2) continue;   // its create is still pending: claimed (then deleted) once it is there
        s->mir[k].state = 0, s->mir[k].actor = 0;
    }
    for (int j = 0; j < COOP_OBJS; j++) {   // new arrows, and where each one is now
        const CoopObj *o = &s->objs_want[j];
        if (!o->id || ((o->id & COOP_OBJ_KEYED) && !key_missing(o->id))) continue;   // keyed: this game's own is moved
        if (was_taken(s->net_id, o->id)) continue;   // picked up here: the owner deletes its own
        int k = 0;
        while (k < COOP_OBJS && !(s->mir[k].state && s->mir[k].id == o->id)) k++;
        if (k == COOP_OBJS) {
            for (k = 0; k < COOP_OBJS && s->mir[k].state; k++) {}
            if (k == COOP_OBJS) continue;
            s->mir[k].id = o->id, s->mir[k].state = 1, s->mir[k].ticks = 0, s->mir[k].actor = 0;
            memcpy(s->mir[k].name, o->name, sizeof s->mir[k].name), s->mir[k].name[sizeof s->mir[k].name - 1] = 0;
        }
        memcpy(s->mir[k].pos, o->pos, 12);
        quat_mat(o->q, s->mir[k].rot);
        s->mir[k].flags = o->flags;
    }
    for (int k = 0; k < COOP_OBJS; k++) {
        if (s->mir[k].state == 1 && coop_request_create(c, s->mir[k].name, s->mir[k].pos, s->mir[k].rot, s->mir[k].handle))
            s->mir[k].state = 2;
        else if (s->mir[k].state == 2) {
            int st = coop_handle_status(s->mir[k].handle);
            if (st == 4) { rt_log("coop: slot %d: the copy of object %u could not be made", (int)(s - slots), s->mir[k].id); s->mir[k].state = 0; }
            else if (st == 3) {
                s->mir[k].actor = coop_claim(c, s->mir[k].handle, s->mir[k].link, &s->mir[k].ref);
                s->mir[k].state = s->mir[k].actor ? 3 : 0;
                static int said;
                if (s->mir[k].actor && said++ < 20) rt_log("coop: slot %d: a copy of object %u (%s) is here", (int)(s - slots), s->mir[k].id, s->mir[k].name);
                arrow_watch_start(s->mir[k].actor, s->mir[k].name);
            }
        } else if (s->mir[k].state == 3 && ++s->mir[k].ticks >= COPY_WARMUP) {
            // picked up here? An arrow's copy too: awake, a stuck arrow's copy could be picked up again and again
            // (a play session, 2026-11-05)
            if (pickable(s->mir[k].name) || strstr(s->mir[k].name, "Arrow")) {
                int32_t st = 0;
                char nm[48];
                float me_p[3] = {1e9f, 1e9f, 1e9f}, *q = s->mir[k].pos;
                uint64_t me = coop_player_actor();
                if (me) coop_rd(me + COOP_ACTOR_POS, me_p, 12);
                float d2 = (me_p[0] - q[0]) * (me_p[0] - q[0]) + (me_p[1] - q[1]) * (me_p[1] - q[1]) + (me_p[2] - q[2]) * (me_p[2] - q[2]);
                int gone = !coop_rd(s->mir[k].actor + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 ||
                           strcmp(coop_actor_name(s->mir[k].actor, nm, sizeof nm), s->mir[k].name);
                if (gone && d2 > 9) { s->mir[k].state = 0, s->mir[k].actor = 0; continue; }   // gone, not by this Link (3 m)
                // A weapon picked up with the hand empty is not deleted: the copy becomes this Link's own (the tester's
                // session, 2026-11-05: the owner kept its sword and picked it up again). It is this Link's now.
                int held = 0;
                uint64_t my_eq = me ? eq_of_actor(me) : 0;
                for (int row = 0; my_eq && row < 24 && !held; row++) {
                    uint64_t ref = coop_rd_ptr(my_eq + GEAR_TABLE + 0x18 * (uint64_t)row);
                    held = ref && coop_rd_ptr(ref + 0x40) == s->mir[k].actor;
                }
                if (held) st = -1;
                if (gone || held) {
                    rt_log("coop: slot %d: the copy of object %u (%s) was picked up here (state %d, -1: now this Link's gear); the owner is told",
                           (int)(s - slots), s->mir[k].id, s->mir[k].name, st);
                    take_note(s->net_id, s->mir[k].id);
                    s->mir[k].state = 0, s->mir[k].actor = 0;
                    continue;
                }
            } else if (!strstr(s->mir[k].name, "Arrow")) {
                // Destroyed here (a bomb flower's copy shot by this game's Link blew up, a play session,
                // 2026-11-05): the owner's own goes too (the same notice as a pick-up), so it cannot be picked up
                // there again; it went off there only when the copy of this Link's arrow happened to touch it.
                int32_t st = 0;
                char nm[48];
                if (!coop_rd(s->mir[k].actor + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 ||
                    strcmp(coop_actor_name(s->mir[k].actor, nm, sizeof nm), s->mir[k].name)) {
                    rt_log("coop: slot %d: the copy of object %u (%s) went away here (state %d); the owner is told",
                           (int)(s - slots), s->mir[k].id, s->mir[k].name, st);
                    take_note(s->net_id, s->mir[k].id);
                    s->mir[k].state = 0, s->mir[k].actor = 0;
                    continue;
                }
            }
            place_copy(c, s, k);
        }
    }
    int live = 0;
    for (int k = 0; k < COOP_OBJS; k++) live += s->mir[k].state == 3;
    copies_live[s - slots] = live;
}
// A copy after its warmup runs no calc and no update phase (the hooks' part; returns 1 when actor is such a copy).
static int copy_place(Cpu *c, uint64_t actor, uint64_t addr, int calc) {
    Slot *s;
    int k;
    (void)c, (void)addr, (void)calc;
    // A Zonai device's copy runs its own update, so a hit here (the proxy's sword) turns it on, with its effects;
    // so does an item's, so this game's Link can pick it up (pickable). Both are still put at the owner's place every
    // tick (place_copy).
    // An arrow's copy is frozen too, after it is woken (place_copy): running its update, a nocked arrow's copy at the
    // other player's bow could be picked up again and again, and a fused arrow's kept falling asleep and did not
    // show (a play session, 2026-11-05).
    return copy_of(actor, &s, &k) && s->mir[k].ticks >= COPY_WARMUP && strncmp(s->mir[k].name, "SpObj_", 6) && !pickable(s->mir[k].name);
}
// Each tick, a copy is put where its arrow is: actor position and rotation, model matrix and the push to the model
// units, as the proxy. Done from the tick, not in one of its own updates: the game stopped calling a frozen arrow's
// calc after two frames (a fused BombFruit's went on), so in a hook only the BombFruit moved (TOTK_COOP_TEST_COPYLOG,
// 2026-11-05).
static void place_copy(Cpu *c, Slot *s, int k) {
    uint64_t actor = s->mir[k].actor;
    // An arrow's copy asks to sleep in its first tick (reason 0, through main+0x55251c: TOTK_COOP_TEST_ARROWWATCH)
    // and sleeps from the third (actor state 6, its model's +0x244 6), where this game's own arrows stay awake (4); a
    // sleeping arrow is not drawn (a play session, 2026-11-05). It is woken as the actor state machine
    // (main+0x54cb00) wakes one: +0x28's reason bits at 0x10, then bit 19 set and the actor queued for the machine
    // (main+0x2ae413c). Frozen after its warmup (copy_place), it does not ask again.
    if (strstr(s->mir[k].name, "Arrow")) {
        int32_t st = 0;
        if (coop_rd(actor + COOP_ACTOR_STATE, &st, 4) && st == 6) {
            uint32_t w = LD32(actor + 0x28);
            ST32(actor + 0x28, (w & ~0x7ffu & ~0x80000u) | 0x10u);
            coop_guest_call(c, MOD_MAIN_BASE + 0x2ae413c, actor, 19, 0, 0);
            static int said;
            if (said++ < 6) rt_log("coop: the copy of arrow %u was asleep: woken", s->mir[k].id);
        }
    }
    float m[12];
    for (int r = 0; r < 3; r++) {
        for (int j = 0; j < 3; j++) m[r * 4 + j] = s->mir[k].rot[r * 3 + j];
        m[r * 4 + 3] = s->mir[k].pos[r];
    }
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), s->mir[k].pos, 12);
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_ROT), s->mir[k].rot, 36);
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x8) + 0x28);
    static int copylog = -1, n;   // TOTK_COOP_TEST_COPYLOG=1: where each copy is put, every 30th time (test)
    if (copylog < 0) copylog = cfg_get("TOTK_COOP_TEST_COPYLOG") != NULL;
    if (copylog && n++ % 8 == 0) {
        float mt[12] = {0};
        if (model) coop_rd(model + 0x1f8, mt, sizeof mt);
        rt_log("coop: test: copy %.2f s %u %s at %.2f %.2f %.2f, model %llx at %.2f %.2f %.2f", frame_step_world_us() / 1e6, s->mir[k].id, s->mir[k].name,
               s->mir[k].pos[0], s->mir[k].pos[1], s->mir[k].pos[2], (unsigned long long)model, mt[3], mt[7], mt[11]);
    }
    if (model) {
        memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        ST8(model + 0x240, LD8(model + 0x240) | 1);
        coop_guest_call(c, MOD_MAIN_BASE + PROXY_MODEL_PUSH, model, 0, 0, 0);
    }
    // With riding on (TOTK_COOP_RIDE), its physics body too, so a Link can stand on it (ride_carry). Moved by speed,
    // not teleported: the body is given the speed that takes it from the last place sent to this one in the time
    // between (SetLinearVelocity main+0x567750, SetAngularVelocity main+0x25a1f0, both (body, float v[3]); found by
    // their "next_linear_velocity" / "next_angular_velocity" checks), so it is a moving floor to a Link standing on
    // it; teleported (COOP_FORCE_MATRIX) once a second, and when it is more than 1 m off, so it does not drift. The
    // body: [[[actor+0x228] +0x50] +0x20] +0x150 (as COOP_FORCE_MATRIX finds it). A moving body can push Links;
    // TOTK_COOP_RIDE=0 leaves copies bodiless as before.
    int ride_on(void);
    if (pickable(s->mir[k].name)) {   // an item runs its own physics: its body is put with it each tick, not driven
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m, 0, 0);
        return;
    }
    if (ride_on() && !strstr(s->mir[k].name, "Arrow")) {
        static struct { float pos[3], rot[9]; int64_t us, teleported; } last[COOP_MAX_PLAYERS][COOP_OBJS];
        typeof(&last[0][0]) l = &last[s - slots][k];
        int64_t now = frame_step_world_us();
        uint64_t phys = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
        uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
        float dt = l->us ? (float)(now - l->us) / 1e6f : 0;
        float dx = s->mir[k].pos[0] - l->pos[0], dy = s->mir[k].pos[1] - l->pos[1], dz = s->mir[k].pos[2] - l->pos[2];
        if (!l->us || !body || dt <= 0 || dt > 0.5f || dx * dx + dy * dy + dz * dz > 1 || now - l->teleported > 1000000) {
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m, 0, 0);
            l->teleported = now;
        }
        static int said;
        if (said < 4) said++, rt_log("coop: ride: copy %u (%s) body %llx", s->mir[k].id, s->mir[k].name, (unsigned long long)body);
        if (body && dt > 0 && dt <= 0.5f) {
            _Alignas(16) float v[4] = {dx / dt, dy / dt, dz / dt, 0};
            // angular: the turn R_now * R_last^T as an axis times its angle, over dt
            const float *a = s->mir[k].rot, *b = l->rot;
            float d[9];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) d[i * 3 + j] = a[i * 3 + 0] * b[j * 3 + 0] + a[i * 3 + 1] * b[j * 3 + 1] + a[i * 3 + 2] * b[j * 3 + 2];
            float ca = (d[0] + d[4] + d[8] - 1) * 0.5f;
            ca = ca > 1 ? 1 : ca < -1 ? -1 : ca;
            float ang = acosf(ca), sa = sinf(ang);
            _Alignas(16) float w[4] = {0, 0, 0, 0};
            if (ang > 1e-4f && sa > 1e-4f) {
                float f = ang / (2 * sa * dt);
                w[0] = (d[7] - d[5]) * f, w[1] = (d[2] - d[6]) * f, w[2] = (d[3] - d[1]) * f;
            }
            coop_guest_call(c, MOD_MAIN_BASE + 0x567750, body, (uint64_t)(uintptr_t)v, 0, 0);
            coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, body, (uint64_t)(uintptr_t)w, 0, 0);
        }
        memcpy(l->pos, s->mir[k].pos, 12), memcpy(l->rot, s->mir[k].rot, 36), l->us = now;
    }
}
// ---- world objects (the tester's list: dropped items, Ultrahand, autobuild, Zonai devices; 2026-11-05). Two kinds:
// - Made by this Link: an actor first seen within 6 m of it, nearer it than any co-op Link (a dropped weapon is the
//   equipped one's own actor, first seen at the equip, so it counts once it is no longer in this Link's gear). Sent
//   while within 20 m of a Link (this one or a co-op one), by number, and shown in the other games as a frozen copy, as an arrow. A save load or a
//   streamed-in area makes many at once: more than 20 new near this Link in one tick are taken as the world's.
// - One every game has (a placed rock or plank): known everywhere by its key (name and where it was first seen, as
//   the enemies'). Once it moves in this game within 20 m of this Link while no co-op player's report moves it, it
//   is sent with its key while within 20 m, at rest too (a game without it shows a copy, which went away 1 s after
//   the object came to rest when only moves were sent); the other games force theirs to that place
//   (COOP_FORCE_MATRIX), and do not send it themselves while they do. Taken by the nearer Link only, a rock lifted
//   by A with B's Link nearer was sent by neither (B's was at rest). Two games sending one: the lower player id's
//   report wins.
// Not checked in a game yet: which actors Ultrahand, autobuild and the Zonai devices move or make; the tester tests it.
#define PROPS_N 256
static struct { uint64_t actor; uint32_t key, id; int seen, moved, born, made, touched, near_me, said; float last[3]; char name[48]; } props[PROPS_N];
static struct { uint64_t actor; int until; } driven[64];   // actors this game forces to another's report
static int prop_tick;
// Keys another game moves that this game does not update (an object at rest runs no per-actor calc, so it is not in
// the live list: only about 70 actors were, none of the placed ones around the Link, 2026-11-05). Those are shown
// as a copy instead; this game's own one stays where it was, so the object can show twice.
static uint32_t missing_key[64];
static int missing_n;
static int key_missing(uint32_t id) {
    for (int i = 0; i < missing_n; i++) if (missing_key[i] == (id & ~COOP_OBJ_KEYED)) return 1;
    return 0;
}
static int is_driven(uint64_t a) {
    for (int i = 0; i < 64; i++) if (driven[i].actor == a && driven[i].until >= prop_tick) return 1;
    return 0;
}
static void set_driven(uint64_t a) {
    int free = 0;
    for (int i = 0; i < 64; i++) {
        if (driven[i].actor == a) { driven[i].until = prop_tick + 60; return; }
        if (driven[i].until < driven[free].until) free = i;
    }
    driven[free].actor = a, driven[free].until = prop_tick + 60;
}
// Not world objects: Links and what they wear and carry, people, animals, enemies (synced on their own), arrows.
static int prop_skip(const char *nm) {
    // Parasail: the Link's paraglider, made when it opens; a copy of it in the other game took that game down with
    // the Ultrahand crash (a play session, 2026-11-05)
    static const char *pre[] = {"Player", "Armor_", "Accessory_", "PlayerRaul", "Npc", "Animal_", "Enemy_",
                                "Weapon_Sheath", "Weapon_Quiver", "Parasail", "TBox_"};   // chests: each player's own
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++) if (!strncmp(nm, pre[i], strlen(pre[i]))) return 1;
    if (strstr(nm, "Arrow")) return 1;
    // Helpers that go with this Link and are not things in the world: EventCamera and CameraXLinkControl were sent
    // as moved objects (pair test, 2026-11-05).
    // (Not for Zonai devices: SpObj_ControlStick, the steering stick, has "Control" in it and was never sent.)
    static const char *in[] = {"Camera", "Event", "Control", "Manager", "Sensor", "Area", "System"};
    for (size_t i = 0; i < sizeof in / sizeof *in && strncmp(nm, "SpObj_", 6); i++) if (strstr(nm, in[i])) return 1;
    if (!strncmp(nm, "Weapon_", 7)) {   // gear still worn by this Link or given to a co-op Link
        for (int k = 0; k < 3; k++) if (!strcmp(nm, my_gear[k])) return 1;
        for (int i = 0; i < COOP_MAX_PLAYERS; i++)
            for (int k = 0; k < 3; k++) if (slots[i].actor && !strcmp(nm, slots[i].gear_want[k])) return 1;
    }
    return 0;
}
// A copy being made (requested, not claimed yet) near p: its actor shows up before copy_of knows it.
static int copy_pending_near(const float *p) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int k = 0; k < COOP_OBJS; k++) {
            const float *q = slots[i].mir[k].pos;
            if ((slots[i].mir[k].state == 1 || slots[i].mir[k].state == 2) &&
                (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]) < 1)
                return 1;
        }
    return 0;
}
typedef struct { float me[3]; int cand[32], n_cand, n_first_near; } PropScan;
static void prop_fn(uint64_t a, const char *nm, uint32_t key, int first, void *arg) {
    PropScan *ps = arg;
    Slot *cs;
    int ck;
    if (prop_skip(nm) || copy_of(a, &cs, &ck) || is_driven(a) || is_preview(a)) return;
    for (int i = 0; i < 32; i++) if (my_arrows[i].actor == a) return;   // an arrow's fused material
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float d2 = (p[0] - ps->me[0]) * (p[0] - ps->me[0]) + (p[1] - ps->me[1]) * (p[1] - ps->me[1]) + (p[2] - ps->me[2]) * (p[2] - ps->me[2]);
    float dco = 1e18f;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        float r[3];
        if (slots[i].actor && coop_rd(slots[i].actor + COOP_ACTOR_POS, r, 12)) {
            float e = (p[0] - r[0]) * (p[0] - r[0]) + (p[1] - r[1]) * (p[1] - r[1]) + (p[2] - r[2]) * (p[2] - r[2]);
            if (e < dco) dco = e;
        }
    }
    if (d2 > 400 && dco > 400) return;   // within 20 m of a Link, this one or a co-op one
    int e = -1, free = -1;
    for (int i = 0; i < PROPS_N && e < 0; i++) {
        if (props[i].actor == a) e = i;
        else if (free < 0 && (!props[i].actor || (prop_tick - props[i].seen > 30 && !props[i].made && !props[i].touched))) free = i;
    }
    if (e < 0) {
        if (free < 0) return;
        e = free;
        memset(&props[e], 0, sizeof props[e]);
        props[e].actor = a, props[e].key = key, props[e].moved = -1000, memcpy(props[e].last, p, 12);
        snprintf(props[e].name, sizeof props[e].name, "%s", nm);
        if (first && d2 < 36) {
            ps->n_first_near++;
            if (d2 < dco + 1 && !copy_pending_near(p) && ps->n_cand < 32) ps->cand[ps->n_cand++] = e;
        }
    } else {
        float dx = p[0] - props[e].last[0], dy = p[1] - props[e].last[1], dz = p[2] - props[e].last[2];
        if (dx * dx + dy * dy + dz * dz > 1e-4f) {
            props[e].moved = prop_tick, memcpy(props[e].last, p, 12);
            props[e].touched = 1;   // it moves in this game and no co-op player moves it: sent from now on (report_props)
        }
    }
    // Nearer this Link, or as near within 1 m: two Links on one spot each claimed nothing (props test, 2026-11-05).
    props[e].seen = prop_tick, props[e].near_me = d2 < dco + 1;
}
static float near2_any(const float *q, const float *me_p) {   // squared distance to the nearest Link, this one or co-op
    float best = (q[0] - me_p[0]) * (q[0] - me_p[0]) + (q[1] - me_p[1]) * (q[1] - me_p[1]) + (q[2] - me_p[2]) * (q[2] - me_p[2]);
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        float r[3];
        if (slots[i].actor && coop_rd(slots[i].actor + COOP_ACTOR_POS, r, 12)) {
            float e = (q[0] - r[0]) * (q[0] - r[0]) + (q[1] - r[1]) * (q[1] - r[1]) + (q[2] - r[2]) * (q[2] - r[2]);
            if (e < best) best = e;
        }
    }
    return best;
}
static int report_props(uint64_t me, CoopObj *out, int max) {
    void coop_actors_keyed(void (*fn)(uint64_t, const char *, uint32_t, int, void *), void *arg);   // coop_world.c
    prop_tick++;
    static PropScan ps;
    memset(&ps, 0, sizeof ps);
    if (!coop_rd(me + COOP_ACTOR_POS, ps.me, 12)) return 0;
    coop_actors_keyed(prop_fn, &ps);
    const float *me_p = ps.me;
    // More at once: a load or a streamed-in area, the world's own. A glider made 6+ at once (its parts): 20.
    if (ps.n_first_near <= 20 && frame_step_world_us() > 20000000)
        for (int i = 0; i < ps.n_cand; i++) {
            int e = ps.cand[i];
            props[e].made = 1, props[e].born = prop_tick, props[e].id = next_arrow_id++;
        }
    int n = 0;
    for (int e = 0; e < PROPS_N && n < max; e++) {
        if (!props[e].actor) continue;
        if ((props[e].made || props[e].touched) && props[e].seen != prop_tick) {
            // At rest it runs no calc and leaves the live list; it is still there until the game takes it away
            // (state 7..11) or the memory holds another actor. Sent while within 20 m of a Link (this one or a co-op
            // one: a glider flown 20 m from its maker with the other Link on it went away under that Link, ride
            // test at 5 m/s); one this Link moved is forgotten beyond that, or once another player moves it.
            int32_t st = 0;
            char nm[48];
            float q[3];
            if (!coop_rd(props[e].actor + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 ||
                strcmp(coop_actor_name(props[e].actor, nm, sizeof nm), props[e].name)) { props[e].actor = 0; continue; }
            if (!props[e].made && is_driven(props[e].actor)) { props[e].touched = 0; continue; }
            int far = !coop_rd(props[e].actor + COOP_ACTOR_POS, q, 12) || near2_any(q, me_p) > 400;
            if (far && !props[e].made) props[e].touched = 0;
            if (far) continue;
        } else if (props[e].seen != prop_tick) continue;
        Slot *cs;
        int ck;
        uint32_t id = 0;
        if (props[e].made) {
            if (prop_tick - props[e].born < 10 || prop_skip(props[e].name)) continue;   // a copy made here is known by now; gear worn again
            if (copy_of(props[e].actor, &cs, &ck)) { props[e].made = 0; continue; }
            id = props[e].id;
        } else if (props[e].touched)
            id = COOP_OBJ_KEYED | (props[e].key & ~COOP_OBJ_KEYED);
        if (!id) continue;
        CoopObj *o = &out[n];
        float r[9];
        if (!coop_rd(props[e].actor + COOP_ACTOR_POS, o->pos, 12) || !coop_rd(props[e].actor + COOP_ACTOR_ROT, r, 36)) continue;
        o->id = id;
        coop_actor_name(props[e].actor, o->name, sizeof o->name);
        o->flags = is_imaginary(props[e].actor) ? COOP_OBJ_AUTOBUILT : 0;   // not used here yet: copies are not green
        mat_quat(r, o->q);
        static int said;
        if (!props[e].said && said < 60) {
            props[e].said = 1, said++;
            rt_log("coop: %s %s (%s %08x)", props[e].made ? "I made" : "I move", o->name, props[e].made ? "number" : "key",
                   props[e].made ? props[e].id : props[e].key);
        }
        n++;
    }
    return n;
}
// The objects every game has that co-op players move: this game's own goes where theirs is.
typedef struct { uint32_t key[COOP_MAX_PLAYERS * COOP_OBJS]; uint64_t actor[COOP_MAX_PLAYERS * COOP_OBJS]; int n; } KeyFind;
static void keyfind_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    KeyFind *kf = arg;
    (void)nm;
    for (int i = 0; i < kf->n; i++) if (kf->key[i] == (key & ~COOP_OBJ_KEYED) && !kf->actor[i]) kf->actor[i] = a;   // 31 bits sent
}
// Keys found here, kept while their actor still has that name: the scan of every actor runs only for new keys, and
// an object once moved here is no longer where its key says.
static struct { uint32_t key; uint64_t actor; char name[48]; float pos[3], q[4]; int last, from; } key_found[128];
static uint64_t key_cached(uint32_t key, const char *name) {
    for (int i = 0; i < 128; i++)
        if (key_found[i].actor && key_found[i].key == key) {
            char nm[48];
            int32_t st = 0;
            if (coop_rd(key_found[i].actor + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 &&
                !strcmp(coop_actor_name(key_found[i].actor, nm, sizeof nm), key_found[i].name) && !strcmp(nm, name))
                return key_found[i].actor;
            key_found[i].actor = 0;
        }
    return 0;
}
// The rotation that takes rb to ra (ra * rb^T) as axis * angle, in w; 0 for none.
static void rot_delta(const float *ra, const float *rb, float w[3]) {
    float d[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) d[i * 3 + j] = ra[i * 3 + 0] * rb[j * 3 + 0] + ra[i * 3 + 1] * rb[j * 3 + 1] + ra[i * 3 + 2] * rb[j * 3 + 2];
    float ca = (d[0] + d[4] + d[8] - 1) * 0.5f;
    ca = ca > 1 ? 1 : ca < -1 ? -1 : ca;
    float ang = acosf(ca), sa = sinf(ang);
    w[0] = w[1] = w[2] = 0;
    if (ang > 1e-4f && sa > 1e-4f) {
        float f = ang / (2 * sa);
        w[0] = (d[7] - d[5]) * f, w[1] = (d[2] - d[6]) * f, w[2] = (d[3] - d[1]) * f;
    }
}
// A placed object another player moves goes where they have it: by speed, so what stands on it is carried smoothly
// (teleported every tick, a Link standing on it jiggled heavily, the tester's autobuild test, 2026-11-05): the speed
// from the last place sent to this one, plus a fifth of a second's worth of the gap to where it is; teleported
// (COOP_FORCE_MATRIX) when it is more than 0.5 m off or has no physics body.
static void force_to(Cpu *c, uint64_t a, const float pos[3], const float q[4]) {
    static struct { uint64_t actor; float pos[3], rot[9]; int64_t us; } last[128];
    float p[3], ar[9], r[9];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, ar, 36)) return;
    quat_mat(q, r);
    int k = 0, free = -1;
    for (; k < 128 && last[k].actor != a; k++) if (free < 0 && !last[k].actor) free = k;
    if (k == 128) k = free >= 0 ? free : (int)((a >> 4) & 127), last[k].actor = a, last[k].us = 0;
    int64_t now = frame_step_world_us();
    if (last[k].us == now) return;   // the tick runs twice at one world time: once a frame
    float dt = last[k].us ? (float)(now - last[k].us) / 1e6f : 0;
    float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
    uint64_t phys = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
    if (!body || dx * dx + dy * dy + dz * dz > 0.25f || dt <= 0 || dt > 0.5f) {
        if (dx * dx + dy * dy + dz * dz >= 4e-4f || !body) {
            float m[12];
            for (int row = 0; row < 3; row++) {
                for (int j = 0; j < 3; j++) m[row * 4 + j] = r[row * 3 + j];
                m[row * 4 + 3] = pos[row];
            }
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, a, (uint64_t)(uintptr_t)m, 0, 0);
        }
    } else {
        float wf[3], wc[3];
        rot_delta(r, last[k].rot, wf), rot_delta(r, ar, wc);
        _Alignas(16) float v[4] = {(pos[0] - last[k].pos[0]) / dt - dx * 5, (pos[1] - last[k].pos[1]) / dt - dy * 5,
                                   (pos[2] - last[k].pos[2]) / dt - dz * 5, 0};
        _Alignas(16) float w[4] = {wf[0] / dt + wc[0] * 5, wf[1] / dt + wc[1] * 5, wf[2] / dt + wc[2] * 5, 0};
        coop_guest_call(c, MOD_MAIN_BASE + 0x567750, body, (uint64_t)(uintptr_t)v, 0, 0);   // SetLinearVelocity
        coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, body, (uint64_t)(uintptr_t)w, 0, 0);   // SetAngularVelocity
    }
    memcpy(last[k].pos, pos, 12), memcpy(last[k].rot, r, 36), last[k].us = now;
}
static void props_hold(Cpu *c);
static int my_report(uint64_t a) {   // this game sends a as moved
    for (int e = 0; e < PROPS_N; e++) if (props[e].actor == a) return props[e].touched && !props[e].made;
    return 0;
}
static void props_receive(Cpu *c) {
    static KeyFind kf;
    kf.n = 0;
    const CoopObj *want[COOP_MAX_PLAYERS * COOP_OBJS];
    static int want_from[COOP_MAX_PLAYERS * COOP_OBJS];
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        if (slots[i].state != LIVE || slots[i].source != SRC_NET) continue;
        for (int j = 0; j < COOP_OBJS; j++)
            if (slots[i].objs_want[j].id & COOP_OBJ_KEYED) {
                want[kf.n] = &slots[i].objs_want[j], want_from[kf.n] = slots[i].net_id;
                kf.key[kf.n] = slots[i].objs_want[j].id & ~COOP_OBJ_KEYED, kf.actor[kf.n] = 0, kf.n++;
            }
    }
    props_hold(c);
    if (!kf.n) return;
    int need = 0;
    for (int i = 0; i < kf.n; i++) need |= !(kf.actor[i] = key_cached(kf.key[i], want[i]->name));
    static int last_scan = -1000;
    if (need && prop_tick - last_scan >= 15) {   // every actor, at rest too: at most twice a second
        void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
        last_scan = prop_tick;
        coop_actors_all(keyfind_fn, &kf);
        for (int i = 0; i < kf.n; i++)
            if (kf.actor[i] && !key_cached(kf.key[i], want[i]->name)) {
                static int slot;
                key_found[slot].key = kf.key[i], key_found[slot].actor = kf.actor[i], key_found[slot].last = 0;
                coop_actor_name(kf.actor[i], key_found[slot].name, sizeof key_found[slot].name);
                slot = (slot + 1) % 128;
            }
    }
    for (int i = 0; i < kf.n; i++) {
        uint64_t a = kf.actor[i];
        static uint32_t said[64];
        static int n_said;
        int known = 0;
        for (int k = 0; k < n_said; k++) known |= said[k] == kf.key[i];
        if (!known && n_said < 64 && (a || last_scan == prop_tick)) {
            said[n_said++] = kf.key[i];
            rt_log("coop: a co-op player moves %s (key %08x)%s", want[i]->name, kf.key[i], a ? "" : ", not updated here: a copy");
        }
        if (!a) {
            if (last_scan == prop_tick && !key_missing(kf.key[i])) {   // a scan of every actor found none
                missing_key[missing_n % 64] = kf.key[i];
                if (missing_n < 64) missing_n++;
            }
            continue;
        }
        if (coop_net_id() < want_from[i] && my_report(a)) continue;   // this game sends it too, and wins
        for (int k = 0; k < 128; k++)   // the last place sent, for after the reports stop
            if (key_found[k].actor == a)
                memcpy(key_found[k].pos, want[i]->pos, 12), memcpy(key_found[k].q, want[i]->q, 16), key_found[k].last = prop_tick,
                key_found[k].from = want_from[i];
        set_driven(a);
        force_to(c, a, want[i]->pos, want[i]->q);
    }
}
// The owner sends a moved object until 1 s after it stops; its copy here, moved by this game's physics in between,
// rolled on for 2 m after; held 2 s, it still slid 1 m down the slope after that (props test, 2026-11-05). For 5 s
// after the last report it is held at the last place sent.
static void props_hold(Cpu *c) {
    for (int k = 0; k < 128; k++) {
        int age = prop_tick - key_found[k].last;
        if (!key_found[k].actor || !key_found[k].last || age < 1 || age > 150) continue;
        if (!key_cached(key_found[k].key, key_found[k].name)) continue;
        set_driven(key_found[k].actor);
        force_to(c, key_found[k].actor, key_found[k].pos, key_found[k].q);
    }
}
// Test (TOTK_COOP_TEST_PROPS=1, in the game that joined as player 1): at 24 s into the world it makes a BombFruit
// 1 m above this Link (an object of its own), and from 28 s to 30 s it lifts the nearest resting AsbObj_ (a rock)
// within 20 m by 1.5 m (one every game has); the other game's log shows both arriving. Then every 30 ticks both games log
// where that object is, for comparing.
static float lift_from[3], lift_best, lift_at[3];
static uint64_t lift_obj;
static uint32_t lift_key;
static void lift_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    (void)arg;
    float q[3];
    if (strncmp(nm, "AsbObj_", 7) || !coop_rd(a + COOP_ACTOR_POS, q, 12)) return;
    float d2 = (q[0] - lift_from[0]) * (q[0] - lift_from[0]) + (q[1] - lift_from[1]) * (q[1] - lift_from[1]) + (q[2] - lift_from[2]) * (q[2] - lift_from[2]);
    if (d2 < lift_best) lift_best = d2, lift_obj = a, lift_key = key, memcpy(lift_at, q, 12);
}
static void props_test(Cpu *c, uint64_t me) {
    static int on = -1, made, picked;
    static uint64_t obj;
    static float base[3];
    static uint32_t obj_key;
    static _Alignas(16) uint8_t handle[0x40];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_PROPS") != NULL;
    if (!on) return;
    double now = frame_step_world_us() / 1e6;
    float p[3];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    if (coop_net_id() == 1 && !made && now > 24) {
        float at[3] = {p[0], p[1] + 1, p[2]}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        made = coop_request_create(c, "BombFruit", at, rot, handle) ? 1 : 0;
        rt_log("coop: test: props: BombFruit asked for at %.2f %.2f %.2f (%d)", at[0], at[1], at[2], made);
    }
    static _Alignas(16) uint8_t link[0x40];
    if (made == 1 && coop_handle_status(handle) == 3) {
        uint64_t ref, a = coop_claim(c, handle, link, &ref);
        made = 2;
        rt_log("coop: test: props: BombFruit is here (%llx)", (unsigned long long)a);
    }
    if (coop_net_id() == 1 && !picked && now > 28) {
        void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
        memcpy(lift_from, p, 12), lift_best = 400, lift_obj = 0;
        coop_actors_all(lift_fn, NULL);   // the nearest placed object (AsbObj_ rocks), at rest
        obj = lift_obj, obj_key = lift_key & ~COOP_OBJ_KEYED, memcpy(base, lift_at, 12);
        picked = 1;
        char nm[48] = "";
        if (obj) coop_actor_name(obj, nm, sizeof nm);
        rt_log("coop: test: props: lifting %s (key %08x) from %.2f %.2f %.2f", nm, obj_key, base[0], base[1], base[2]);
    }
    if (obj && now > 28 && now < 30) {
        float m[12] = {1, 0, 0, base[0], 0, 1, 0, base[1] + (float)(now - 28) * 0.75f, 0, 0, 1, base[2]};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, obj, (uint64_t)(uintptr_t)m, 0, 0);
    }
    // TOTK_COOP_TEST_PROPS=kill: at 32 s the other game deletes its copy of the BombFruit, as a blast would; the
    // first game's own one should go too (the destroyed-copy notice).
    static int killed;
    if (!killed && now > 32 && coop_net_id() != 1 && !strcmp(cfg_get("TOTK_COOP_TEST_PROPS"), "kill"))
        for (int i = 0; i < COOP_MAX_PLAYERS && !killed; i++)
            for (int k = 0; k < COOP_OBJS && !killed; k++)
                if (slots[i].mir[k].state == 3 && !strcmp(slots[i].mir[k].name, "BombFruit")) {
                    coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, slots[i].mir[k].actor, COOP_DELETE_REASON, 0, 0);
                    killed = 1;
                    rt_log("coop: test: props: the copy of BombFruit %u is deleted here", slots[i].mir[k].id);
                }
}
// Test (TOTK_COOP_TEST_PICKUP=1, or =sword for a Weapon_Sword_001, both games on the 100% save; pickup_b_3rd.txt in game B): at 21 s both Links are
// put on the flat ground the fan test uses, 1.2 m apart and facing each other (player 1 at z 1510.2 facing +z, the
// other at z 1511.4 facing -z); at 22 s player 1 makes an apple (Item_Fruit_A) between them, 0.5 m from the other;
// from 22 s to 40 s every 15 ticks both games log each Item_ actor within 3 m: the original, its copy, and whether
// the copy is still there after B tries to pick it up.
static void pickup_test(Cpu *c, uint64_t me) {
    static int on = -1, made;
    static _Alignas(16) uint8_t handle[0x40], link[0x40];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_PICKUP") != NULL;
    if (!on) return;
    double now = frame_step_world_us() / 1e6;
    float p[3], r[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    static int put;
    if (!put && now > 21) {
        float m1[12] = {1, 0, 0, 31.5f, 0, 1, 0, 1514.8f, 0, 0, 1, 1510.2f}, m2[12] = {-1, 0, 0, 31.5f, 0, 1, 0, 1514.8f, 0, 0, -1, 1511.4f};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)(coop_net_id() == 1 ? m1 : m2), 0, 0);
        put = 1;
    }
    if (coop_net_id() == 1 && !made && now > 22) {
        float at[3] = {31.5f, 1515.0f, 1510.9f}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const char *what = !strcmp(cfg_get("TOTK_COOP_TEST_PICKUP"), "sword") ? "Weapon_Sword_001" : "Item_Fruit_A";
        made = coop_request_create(c, what, at, rot, handle) ? 1 : 0;
        rt_log("coop: test: pickup: %s asked for at %.2f %.2f %.2f (%d)", what, at[0], at[1], at[2], made);
    }
    if (made == 1 && coop_handle_status(handle) == 3) {
        uint64_t ref, a = coop_claim(c, handle, link, &ref);
        made = 2;
        rt_log("coop: test: pickup: apple is here (%llx)", (unsigned long long)a);
    }
    static int near_put;   // the other game: at 28.5 s (the apple rolls 9 m down to rest) its Link 0.5 m from the copy, facing it
    if (coop_net_id() != 1 && !near_put && now > 28.5)
        for (int k = 0; k < COOP_OBJS && !near_put; k++)
            if (slots[0].mir[k].state == 3 && pickable(slots[0].mir[k].name)) {
                float q[3];
                if (!coop_rd(slots[0].mir[k].actor + COOP_ACTOR_POS, q, 12)) continue;
                float m[12] = {0, 0, 1, q[0] - 0.5f, 0, 1, 0, q[1] + 0.1f, -1, 0, 0, q[2]};   // facing +x
                coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
                near_put = 1;
                rt_log("coop: test: pickup: this Link put by the copy at %.2f %.2f %.2f", q[0], q[1], q[2]);
            }
    static int t;
    if (now < 22 || now > 40 || ++t % 15) return;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int k = 0; k < COOP_OBJS; k++)
            if (slots[i].mir[k].state && pickable(slots[i].mir[k].name)) {
                char nm[48] = "";
                float q[3] = {0};
                uint64_t a = slots[i].mir[k].actor;
                if (a) coop_actor_name(a, nm, sizeof nm), coop_rd(a + COOP_ACTOR_POS, q, 12);
                rt_log("coop: test: pickup %.2f s: copy %u state %d actor %llx (%s) at %.2f %.2f %.2f", now, slots[i].mir[k].id,
                       slots[i].mir[k].state, (unsigned long long)a, nm, q[0], q[1], q[2]);
            }
    for (int e = 0; e < PROPS_N; e++)
        if (props[e].actor && props[e].made && pickable(props[e].name)) {
            char nm[48] = "";
            float q[3] = {0};
            coop_actor_name(props[e].actor, nm, sizeof nm), coop_rd(props[e].actor + COOP_ACTOR_POS, q, 12);
            rt_log("coop: test: pickup %.2f s: mine %u actor %llx (%s) at %.2f %.2f %.2f, seen %d ticks ago", now, props[e].id,
                   (unsigned long long)props[e].actor, nm, q[0], q[1], q[2], prop_tick - props[e].seen);
        }
}
// Test (TOTK_COOP_TEST_CHEST=1, both games on the 100% save; chest_a_3rd.txt / chest_b_3rd.txt): at 21 s this Link
// is put on the flat ground the fan test uses (player 1 at x 31.5, the other 3 m along x), facing +z; at 22 s a chest
// (TBox_Field_Iron) of this game's own is made 1.2 m in front of it. The scripts open it (player 1 at 25 s, player 2
// at 30 s) and walk at 36 s; every 30 ticks from 24 s to 42 s this Link's place is logged. For the input freeze when
// player 2 opened a chest (a play session, 2026-11-05).
static void chest_test(Cpu *c, uint64_t me) {
    static int on = -1, stage;
    static _Alignas(16) uint8_t handle[0x40], link[0x40];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_CHEST") != NULL;
    if (!on) return;
    double now = frame_step_world_us() / 1e6;
    float x = coop_net_id() == 1 ? 31.5f : 34.5f;
    if (stage == 0 && now > 21) {
        float m[12] = {1, 0, 0, x, 0, 1, 0, 1514.8f, 0, 0, 1, 1510.2f};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        stage = 1;
    }
    if (stage == 1 && now > 22) {
        float at[3] = {x, 1514.9f, 1511.4f}, rot[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};   // its front to the Link
        stage = coop_request_create(c, "TBox_Field_Iron", at, rot, handle) ? 2 : 9;
        rt_log("coop: test: chest: asked for at %.2f %.2f %.2f (%d)", at[0], at[1], at[2], stage);
    }
    if (stage == 2 && coop_handle_status(handle) == 3) {
        uint64_t ref, a = coop_claim(c, handle, link, &ref);
        stage = 3;
        rt_log("coop: test: chest: here (%llx)", (unsigned long long)a);
    }
    static int t;
    float p[3];
    if (now < 24 || now > 42 || ++t % 30 || !coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    rt_log("coop: test: chest %.2f s: this Link at %.2f %.2f %.2f", now, p[0], p[1], p[2]);
}
// Test (TOTK_COOP_TEST_ARROWDUMP=1, both games on bow_shoot_3rd.txt): once after 24 s, when this game has an arrow of
// its own Link's and a copy of the other player's at the same time, both are written to logs/arrowdump.bin: the actor
// (0x800 bytes), its model ([[[actor+0x228]+8]+0x28], 0x800) and 0x200 bytes of each of its first 64 components;
// own first, then the copy. Copies of arrows did not show in a play session (2026-11-05); diffed offline.
static void arrow_dump(void) {
    static int on = -1, done;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ARROWDUMP") != NULL;
    if (!on || done || frame_step_world_us() < 24000000) return;
    uint64_t own = 0, copy = 0;
    for (int i = 0; i < 32 && !own; i++) if (my_arrows[i].actor && my_arrows[i].mine && arrow_tick - my_arrows[i].seen < 2) own = my_arrows[i].actor;
    for (int i = 0; i < COOP_MAX_PLAYERS && !copy; i++)
        for (int k = 0; k < COOP_OBJS && !copy; k++)
            if (slots[i].mir[k].state == 3 && slots[i].mir[k].ticks > COPY_WARMUP + 10 && strstr(slots[i].mir[k].name, "Arrow")) copy = slots[i].mir[k].actor;
    if (!own || !copy) return;
    done = 1;
    FILE *f = fopen("logs/arrowdump.bin", "wb");
    static uint8_t buf[0x800 + 0x800 + 64 * 0x200];
    for (int w = 0; w < 2 && f; w++) {
        uint64_t a = w ? copy : own, tbl = coop_rd_ptr(a + 0x228), m1 = tbl ? coop_rd_ptr(tbl + 8) : 0, md = m1 ? coop_rd_ptr(m1 + 0x28) : 0;
        memset(buf, 0, sizeof buf);
        coop_rd(a, buf, 0x800);
        if (md) coop_rd(md, buf + 0x800, 0x800);
        for (int k = 0; k < 64 && tbl; k++) {
            uint64_t cp = coop_rd_ptr(tbl + 8 * (uint64_t)k);
            if (cp && !(cp & 7)) coop_rd(cp, buf + 0x1000 + k * 0x200, 0x200);
        }
        fwrite(buf, sizeof buf, 1, f);
    }
    if (f) fclose(f);
    char n1[48], n2[48];
    rt_log("coop: test: arrow dump: own %s %llx, copy %s %llx", coop_actor_name(own, n1, sizeof n1), (unsigned long long)own,
           coop_actor_name(copy, n2, sizeof n2), (unsigned long long)copy);
}
// Both games: where the object a co-op player moved last is here (for the test above).
static void props_test_where(void) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_PROPS") != NULL;
    if (!on || ++t % 30) return;
    for (int e = 0; e < PROPS_N; e++) {
        float q[3];
        if (props[e].actor && !props[e].made && prop_tick - props[e].moved < 60 && coop_rd(props[e].actor + COOP_ACTOR_POS, q, 12))
            rt_log("coop: test: props: moved here %08x at %.2f %.2f %.2f", props[e].key & ~COOP_OBJ_KEYED, q[0], q[1], q[2]);
    }
    for (int i = 0; i < 64; i++) {
        float q[3];
        if (driven[i].actor && driven[i].until >= prop_tick && coop_rd(driven[i].actor + COOP_ACTOR_POS, q, 12))
            rt_log("coop: test: props: driven here %llx at %.2f %.2f %.2f", (unsigned long long)driven[i].actor, q[0], q[1], q[2]);
    }
}
// Test (TOTK_COOP_TEST_REFPOOL=1): at 25 s into the world, once, the memory 8 MiB around this Link's reference
// object is searched for other reference objects (a word at +0x40 that is an actor: a readable name, a state 0..11),
// to learn whether every actor's reference sits in one pool (then walking it lists the actors at rest too).
static int plausible_name(const char *s) {
    int n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_')) return 0;
    return n >= 3;
}
static void refpool_test(void) {
    static int on = -1, done;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_REFPOOL") != NULL;
    if (!on || done || frame_step_world_us() < 25000000) return;
    done = 1;
    uint64_t ref = 0, me = coop_resident_named("Player", &ref);
    if (!me || !ref) return;
    rt_log("coop: test: refpool: Player %llx ref %llx id %x", (unsigned long long)me, (unsigned long long)ref, (unsigned)LD32(ref + 0x48));
    int hits = 0, near_me = 0;
    uint64_t lo = ref - 0x800000, hi = ref + 0x800000, first = 0, last = 0, prev = 0;
    static uint64_t steps[8];
    static int step_n[8];
    for (uint64_t r = lo & ~7ull; r < hi; r += 8) {
        uint64_t a;
        if (!coop_rd(r + 0x40, &a, 8) || a < 0x1000 || (a & 7)) continue;
        uint64_t np;
        int32_t st;
        char nm[48];
        if (!coop_rd(a + COOP_ACTOR_NAME, &np, 8) || !np || !coop_rd(a + COOP_ACTOR_STATE, &st, 4) || st < 0 || st > 11) continue;
        memset(nm, 0, sizeof nm);
        if (!coop_rd(np, nm, 40) || !plausible_name(nm)) continue;
        // its back link: an actor's reference names it (the Player's does); keep those whose actor points back
        hits++;
        if (!first) first = r;
        last = r;
        if (prev) {
            uint64_t d = r - prev;
            int k = 0;
            while (k < 8 && step_n[k] && steps[k] != d) k++;
            if (k < 8) steps[k] = d, step_n[k]++;
        }
        prev = r;
        float p[3], q[3];
        if (coop_rd(a + COOP_ACTOR_POS, p, 12) && coop_rd(me + COOP_ACTOR_POS, q, 12)) {
            float d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
            if (d2 < 400 && near_me++ < 40)
                rt_log("coop: test: refpool: %+lld %s state %d at %.1f %.1f %.1f (%.1f m)", (long long)(r - ref), nm, st, p[0], p[1], p[2], sqrtf(d2));
        }
        if (hits <= 10) rt_log("coop: test: refpool: hit %+lld %s", (long long)(r - ref), nm);
    }
    rt_log("coop: test: refpool: %d hits from %+lld to %+lld, %d within 20 m", hits, (long long)(first - ref), (long long)(last - ref), near_me);
    {   // the pool from its start, if an id is the entry number
        uint32_t id = LD32(ref + 0x48);
        uint64_t base = ref - (uint64_t)(id & 0xffff) * 0x60;
        int valid = 0, last_i = -1, match = 0, shown = 0, gap = 0;
        for (int i = 0; i < 65536 && gap < 4096; i++) {
            uint64_t r = base + (uint64_t)i * 0x60, a = 0, np = 0;
            uint32_t rid = 0;
            char nm[48] = {0};
            if (!coop_rd(r + 0x40, &a, 8) || !coop_rd(r + 0x48, &rid, 4)) break;
            if (!a || (a & 7) || !coop_rd(a + COOP_ACTOR_NAME, &np, 8) || !np || !coop_rd(np, nm, 40) || !plausible_name(nm)) { gap++; continue; }
            gap = 0, valid++, last_i = i, match += (int)(rid & 0xffff) == i;
            if (shown++ < 12) rt_log("coop: test: refpool: entry %d id %x %s", i, rid, nm);
        }
        rt_log("coop: test: refpool: from base %+lld: %d actors, last entry %d, %d with id == entry", (long long)(base - ref), valid, last_i, match);
    }
    for (int k = 0; k < 8 && step_n[k]; k++) rt_log("coop: test: refpool: step %#llx x%d", (unsigned long long)steps[k], step_n[k]);
}
// ---- world objects on the network (OBJS packets, coop_net.c). In the 30-a-second state, 24 objects were already
// 1.8 KB a packet; now an object is sent when it moved (1 cm) or turned, again once a second while it rests, and
// once with an empty name when it is gone; up to 100 entries a tick, COOP_OBJS_PER_PACKET to a packet. A receiver
// forgets an object not heard of for 2.5 s (a lost "gone").
#define OBJS_REFRESH 30
#define OBJS_PER_TICK 100
static struct { uint32_t id; float pos[3], q[4]; int tick, here; } objs_sent[COOP_OBJS];
static int objs_sent_n, objs_tick;
static void objs_send(const CoopObj *cur, int n) {
    static uint8_t pk[COOP_OBJS_PACKET_MAX];
    int in_pk = 0, budget = OBJS_PER_TICK;
    objs_tick++;
#define OBJS_PUT(o) do { memcpy(pk + 4 + in_pk * sizeof(CoopObj), (o), sizeof(CoopObj)); \
        if (++in_pk == COOP_OBJS_PER_PACKET) { uint16_t h[2] = {(uint16_t)in_pk, 0}; memcpy(pk, h, 4); \
            coop_net_objs_send(pk, 4 + in_pk * (int)sizeof(CoopObj)); in_pk = 0; } budget--; } while (0)
    for (int j = 0; j < objs_sent_n; j++) objs_sent[j].here = 0;
    for (int pass = 0; pass < 2; pass++)   // what changed first, then the once-a-second refreshes
        for (int i = 0; i < n && budget > 0; i++) {
            const CoopObj *o = &cur[i];
            int j = 0;
            while (j < objs_sent_n && objs_sent[j].id != o->id) j++;
            if (j < objs_sent_n) objs_sent[j].here = 1;
            int changed = j == objs_sent_n;
            if (!changed) {
                float dx = o->pos[0] - objs_sent[j].pos[0], dy = o->pos[1] - objs_sent[j].pos[1], dz = o->pos[2] - objs_sent[j].pos[2];
                float dq = fabsf(o->q[0] - objs_sent[j].q[0]) + fabsf(o->q[1] - objs_sent[j].q[1]) + fabsf(o->q[2] - objs_sent[j].q[2]) +
                           fabsf(o->q[3] - objs_sent[j].q[3]);
                changed = dx * dx + dy * dy + dz * dz > 1e-4f || dq > 2e-3f;
            }
            int due = !changed && objs_tick - objs_sent[j].tick >= OBJS_REFRESH;
            if (pass == 0 ? !changed : !due) continue;
            if (j == objs_sent_n) {
                if (objs_sent_n == COOP_OBJS) continue;
                objs_sent_n++, objs_sent[j].here = 1;
            }
            objs_sent[j].id = o->id, objs_sent[j].tick = objs_tick;
            memcpy(objs_sent[j].pos, o->pos, 12), memcpy(objs_sent[j].q, o->q, 16);
            OBJS_PUT(o);
        }
    for (int j = 0; j < objs_sent_n;) {   // gone: said once, then forgotten
        if (objs_sent[j].here) { j++; continue; }
        CoopObj gone = {.id = objs_sent[j].id};
        if (budget > 0) OBJS_PUT(&gone);
        objs_sent[j] = objs_sent[--objs_sent_n];
    }
    if (in_pk) {
        uint16_t h[2] = {(uint16_t)in_pk, 0};
        memcpy(pk, h, 4);
        coop_net_objs_send(pk, 4 + in_pk * (int)sizeof(CoopObj));
    }
#undef OBJS_PUT
}
static void objs_receive(Cpu *c) {
    static int rtick;
    taken_tick++;
    static uint8_t pk[COOP_OBJS_PACKET_MAX];
    int from = 0, n;
    rtick++;
    while ((n = coop_net_objs_take(pk, sizeof pk, &from)) != 0) {
        if (n < 4) continue;
        uint16_t h[2];
        memcpy(h, pk, 4);
        if (h[0] > COOP_OBJS_PER_PACKET || 4 + h[0] * (int)sizeof(CoopObj) > n) continue;
        if (h[1] == 1) {   // another player picked up objects: this game's own ones are deleted (take_note)
            for (int e = 0; e < h[0]; e++) {
                CoopObj o;
                memcpy(&o, pk + 4 + e * sizeof(CoopObj), sizeof o);
                if (o.flags != coop_net_id()) continue;
                for (int i = 0; i < 32; i++)   // an arrow of this Link's (picked up where it stuck)
                    if (my_arrows[i].actor && my_arrows[i].mine && my_arrows[i].id == o.id) {
                        rt_log("coop: player %d picked up my arrow (number %u); mine is deleted", from, o.id);
                        coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, my_arrows[i].actor, COOP_DELETE_REASON, 0, 0);
                        my_arrows[i].actor = 0;
                    }
                for (int i = 0; i < PROPS_N; i++)   // one it made (by number) or a placed one it moved (by key)
                    if (props[i].actor && ((props[i].made && props[i].id == o.id) ||
                                           (!props[i].made && (o.id & COOP_OBJ_KEYED) && (COOP_OBJ_KEYED | (props[i].key & ~COOP_OBJ_KEYED)) == o.id))) {
                        rt_log("coop: player %d picked up or destroyed my %s (%08x); mine is deleted", from, props[i].name, o.id);
                        coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, props[i].actor, COOP_DELETE_REASON, 0, 0);
                        set_driven(props[i].actor);   // not taken for a new object of this game's while it goes away
                        props[i].actor = 0;
                    }
            }
            continue;
        }
        Slot *s = slot_for(SRC_NET, from);
        if (!s) continue;
        for (int e = 0; e < h[0]; e++) {
            CoopObj o;
            memcpy(&o, pk + 4 + e * sizeof(CoopObj), sizeof o);
            if (!o.id) continue;
            o.name[sizeof o.name - 1] = 0;
            int j = 0, free = -1;
            for (; j < COOP_OBJS && s->objs_want[j].id != o.id; j++)
                if (free < 0 && !s->objs_want[j].id) free = j;
            if (!o.name[0]) {   // gone
                if (j < COOP_OBJS) s->objs_want[j].id = 0;
                continue;
            }
            if (j == COOP_OBJS) j = free;
            if (j < 0) continue;
            s->objs_want[j] = o, s->objs_heard[j] = rtick;
        }
    }
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int j = 0; j < COOP_OBJS; j++)
            if (slots[i].objs_want[j].id && rtick - slots[i].objs_heard[j] > 75) slots[i].objs_want[j].id = 0;
}
// Test (TOTK_COOP_TEST_STAND=1, one game): what records the thing Link stands on. At 22 s a glider
// (SpObj_LiftGeneratorWing_A_01) is made 2.5 m beside this Link and 1 m up; at 26 s the Link is put 0.6 m above
// it; at 29 s (standing on it) this Link's first 0x4000 bytes, and 0x1000 bytes of each block a word there points
// to, are searched for the glider's actor and its reference object; at 33 s the Link is put back on the ground and
// the same words are read again (they should no longer hold it).
static void stand_test(Cpu *c, uint64_t me) {
    static int on = -1, stage;
    static _Alignas(16) uint8_t handle[0x40], link[0x40];
    static uint64_t wing, wing_ref;
    static float start[3];
    static struct { uint64_t at; char path[48]; } hit[64];
    static int n_hit;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_STAND") != NULL;
    if (!on) return;
    double now = frame_step_world_us() / 1e6;
    float p[3];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    if (stage == 0 && now > 22) {
        memcpy(start, p, 12);
        float at[3] = {p[0] + 2.5f, p[1] + 1, p[2]}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        stage = coop_request_create(c, "SpObj_LiftGeneratorWing_A_01", at, rot, handle) ? 1 : 9;
        rt_log("coop: test: stand: glider asked for at %.2f %.2f %.2f (%d)", at[0], at[1], at[2], stage);
    }
    if (stage == 1 && coop_handle_status(handle) == 3) {
        wing = coop_claim(c, handle, link, &wing_ref);
        stage = wing ? 2 : 9;
        rt_log("coop: test: stand: glider %llx ref %llx", (unsigned long long)wing, (unsigned long long)wing_ref);
    }
    if (stage == 2 && now > 26) {
        float w[3];
        if (!coop_rd(wing + COOP_ACTOR_POS, w, 12)) { stage = 9; return; }
        float m[12] = {1, 0, 0, w[0], 0, 1, 0, w[1] + 0.6f, 0, 0, 1, w[2]};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        rt_log("coop: test: stand: Link put on the glider at %.2f %.2f %.2f", w[0], w[1] + 0.6f, w[2]);
        stage = 3;
    }
    {   // the words around [Link+0x3b8]+0xd68, which held the glider while the Link stood on it, at each stage
        static int said_stage = -1;
        int at_stage = now > 20 && stage == 0 ? 0 : (stage == 3 && now > 28.9) ? 3 : (stage == 6) ? 6 : -1;
        if (at_stage >= 0 && at_stage != said_stage) {
            said_stage = at_stage;
            uint64_t comp = coop_rd_ptr(me + 0x3b8), comp2 = coop_rd_ptr(me + 0xa68);
            char line[600];
            int n = snprintf(line, sizeof line, "coop: test: stand: stage %d, [Link+0x3b8] %llx, [Link+0xa68] %llx:", at_stage,
                             (unsigned long long)comp, (unsigned long long)comp2);
            for (uint64_t o = 0xd40; o < 0xdc0 && n < (int)sizeof line - 20; o += 8)
                n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)coop_rd_ptr(comp + o));
            rt_log("%s", line);
        }
    }
    if ((stage == 3 && now > 29) || (stage == 5 && now > 36)) {
        float w[3] = {0};
        coop_rd(wing + COOP_ACTOR_POS, w, 12);
        rt_log("coop: test: stand: %s: Link at %.2f %.2f %.2f, glider at %.2f %.2f %.2f", stage == 3 ? "on it" : "off it",
               p[0], p[1], p[2], w[0], w[1], w[2]);
        if (stage == 3) {
            n_hit = 0;
            for (uint64_t o = 0; o < 0x4000 && n_hit < 64; o += 8) {
                uint64_t v = 0;
                if (!coop_rd(me + o, &v, 8)) continue;
                if (v == wing || v == wing_ref) {
                    hit[n_hit].at = me + o, snprintf(hit[n_hit].path, sizeof hit[n_hit].path, "Link+%#llx", (unsigned long long)o), n_hit++;
                    continue;
                }
                if (v < 0x10000000 || (v & 7)) continue;
                for (uint64_t q = 0; q < 0x1000 && n_hit < 64; q += 8) {
                    uint64_t u = 0;
                    if (!coop_rd(v + q, &u, 8)) break;
                    if (u == wing || u == wing_ref)
                        hit[n_hit].at = v + q, snprintf(hit[n_hit].path, sizeof hit[n_hit].path, "[Link+%#llx]+%#llx", (unsigned long long)o, (unsigned long long)q), n_hit++;
                }
            }
            for (int i = 0; i < n_hit; i++) rt_log("coop: test: stand: on it, %s holds the glider's %s", hit[i].path, LD64(hit[i].at) == wing ? "actor" : "reference");
            rt_log("coop: test: stand: %d words hold it", n_hit);
            float m[12] = {1, 0, 0, start[0], 0, 1, 0, start[1] + 0.3f, 0, 0, 1, start[2]};
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
            stage = 5;
        } else {
            for (int i = 0; i < n_hit; i++) {
                uint64_t v = LD64(hit[i].at);
                rt_log("coop: test: stand: off it, %s now %llx%s", hit[i].path, (unsigned long long)v, v == wing || v == wing_ref ? " (still the glider)" : "");
            }
            stage = 6;
        }
    }
}
// ---- riding another player's object (the tester: ride a flying Zonai contraption together, 2026-11-05). This Link
// rides an object a co-op player sends (a copy, or this game's own one moved to their report) when it stands on it:
// within 1.6 m of it across and 0-1.6 m above its origin. While it rides, each tick the object's move since the last
// tick (its turn about its origin and its shift) is put on this Link too (COOP_FORCE_MATRIX), so it goes along as on
// a moving floor and still walks on it. Where the game keeps what Link stands on was not found: no word in the
// Link's first 0x4000 bytes or one level down held the glider it stood on (TOTK_COOP_TEST_STAND).
// What it took (ride test, TOTK_COOP_TEST_RIDE=<m/s>: A moves a glider along x and 0.3 m/s up for 10 s, B's Link is
// put on its copy): the copy moved by speed, not teleported, holds the Link up (teleported, it sank through); this
// runs once a frame (the tick ran twice at one world time, and the second undid the first: the Link fell behind at
// half the glider's speed); objects are sent within 20 m of any Link (a glider flown 20 m from its maker went away
// under its rider). At 5 m/s B rode the 50 m, 0.1-0.2 m from where it stood, and came down with the glider after.
// Seen in the other games the rider is put on their one of the object (ride_place). TOTK_COOP_RIDE=0 turns it off.
typedef struct { uint64_t actor; float pos[3], rot[9]; int owner; uint32_t id; } RideObj;
static int ride_objects(RideObj *out, int max) {
    int n = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int k = 0; k < COOP_OBJS && n < max && copies_live[i]; k++)
            // not ridden: arrows, and items (an item's copy runs its own physics and is put back at its owner's
            // place; a Link carried by it went through the floor, a play session, 2026-11-05)
            if (slots[i].mir[k].state == 3 && slots[i].mir[k].ticks >= COPY_WARMUP && !strstr(slots[i].mir[k].name, "Arrow") &&
                !pickable(slots[i].mir[k].name)) {
                out[n].actor = slots[i].mir[k].actor, out[n].owner = slots[i].net_id, out[n].id = slots[i].mir[k].id;
                memcpy(out[n].pos, slots[i].mir[k].pos, 12), memcpy(out[n].rot, slots[i].mir[k].rot, 36);
                n++;
            }
    for (int k = 0; k < 128 && n < max; k++)
        if (key_found[k].actor && key_found[k].last && prop_tick - key_found[k].last < 150 && !pickable(key_found[k].name)) {
            out[n].actor = key_found[k].actor, out[n].owner = key_found[k].from, out[n].id = COOP_OBJ_KEYED | key_found[k].key;
            memcpy(out[n].pos, key_found[k].pos, 12), quat_mat(key_found[k].q, out[n].rot);
            n++;
        }
    return n;
}
// Where a slot's owner stands, here, when it rides an object: this game's own one (an object this game sends: made
// here, or a placed one it moves) or its copy of a third player's, its place now, and the owner's offset in its
// frame. Seen from the owner of a glider moved 1 m/s, the rider was 0.1 m behind where it stood in its own game
// (the state's delay); faster, more (ride test, 2026-11-05).
static int ride_place(const Slot *s, float out[3]) {
    if (!s->ride_owner || !s->ride_id) return 0;
    uint64_t a = 0;
    float p[3], r[9];
    if (s->ride_owner == coop_net_id()) {
        for (int e = 0; e < PROPS_N && !a; e++)
            if (props[e].actor && ((props[e].made && props[e].id == s->ride_id) ||
                                   (!props[e].made && props[e].touched && (COOP_OBJ_KEYED | (props[e].key & ~COOP_OBJ_KEYED)) == s->ride_id)))
                a = props[e].actor;
        if (!a || !coop_rd(a + COOP_ACTOR_POS, p, 12) || !coop_rd(a + COOP_ACTOR_ROT, r, 36)) return 0;
    } else {
        Slot *o = slot_for(SRC_NET, s->ride_owner);
        int k = 0;
        while (o && k < COOP_OBJS && !(o->mir[k].state == 3 && o->mir[k].id == s->ride_id)) k++;
        if (!o || k == COOP_OBJS) return 0;
        memcpy(p, o->mir[k].pos, 12), memcpy(r, o->mir[k].rot, 36);
    }
    for (int i = 0; i < 3; i++) out[i] = p[i] + r[i * 3 + 0] * s->ride_off[0] + r[i * 3 + 1] * s->ride_off[1] + r[i * 3 + 2] * s->ride_off[2];
    return 1;
}
int ride_on(void) {   // riding a co-op player's object: on unless TOTK_COOP_RIDE=0
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_RIDE");
        on = !(e && *e == '0');
    }
    return on;
}
static void ride_carry(Cpu *c, uint64_t me) {
    if (!ride_on()) return;
    // Once a frame: the tick ran twice at one world time, and the second, reading the Link where it was (a forced place
    // shows only after the game's step) and the object unmoved, put it back; it fell behind by half the object's
    // speed (per-tick ride log, 2026-11-05).
    static int64_t last_us = -1;
    int64_t now_us = frame_step_world_us();
    if (now_us == last_us) return;
    last_us = now_us;
    static int nocarry = -1;   // TOTK_COOP_TEST_NOCARRY=1: no carrying here, to see whether the moving copy carries the Link itself
    if (nocarry < 0) nocarry = cfg_get("TOTK_COOP_TEST_NOCARRY") != NULL;
    static uint64_t riding;
    static float last_pos[3], last_rot[9];
    static int said;
    RideObj objs[64];
    int n = ride_objects(objs, 64);
    float p[3], r[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    static float stand_y;   // this Link's height above the object's origin, in its frame, when it stepped on
    const RideObj *on = NULL;
    for (int i = 0; i < n && !on; i++) {
        float dx = p[0] - objs[i].pos[0], dy = p[1] - objs[i].pos[1], dz = p[2] - objs[i].pos[2];
        // on it: above its origin; once riding, down to 0.5 m below the boarding height too (it sinks in)
        float lo = objs[i].actor == riding ? stand_y - 0.5f : 0;
        if (dx * dx + dz * dz < 1.6f * 1.6f && dy > lo && dy < 1.6f) on = &objs[i];
    }
    if (!on) {
        if (riding && said < 40) said++, rt_log("coop: ride: off %llx", (unsigned long long)riding);
        riding = 0, ride_owner_now = 0, ride_id_now = 0;
        return;
    }
    if (on->actor != riding) {   // stepped on: carried from the next tick
        float off[3] = {p[0] - on->pos[0], p[1] - on->pos[1], p[2] - on->pos[2]};
        stand_y = on->rot[1] * off[0] + on->rot[4] * off[1] + on->rot[7] * off[2];   // local y (rows rotate local to world)
        if (said < 40) said++, rt_log("coop: ride: on %llx, %.2f m above it", (unsigned long long)on->actor, stand_y);
        riding = on->actor;
    } else {
        // delta = R_now * R_last^T: the offset from the object's origin turns with it, the Link's facing too
        float d[9], off[3] = {p[0] - last_pos[0], p[1] - last_pos[1], p[2] - last_pos[2]}, np[3], nr[9];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                d[i * 3 + j] = on->rot[i * 3 + 0] * last_rot[j * 3 + 0] + on->rot[i * 3 + 1] * last_rot[j * 3 + 1] + on->rot[i * 3 + 2] * last_rot[j * 3 + 2];
        for (int i = 0; i < 3; i++) np[i] = on->pos[i] + d[i * 3 + 0] * off[0] + d[i * 3 + 1] * off[1] + d[i * 3 + 2] * off[2];
        // No height of its own: the forced place's height is not taken (the Link's own physics keeps its up and
        // down; it went on falling while the forced height stayed), and holding it at its boarding height only
        // jolted it now and then (it bobbed 0.25 m twice a second, the tester: "jiggles heavily"). The copy's body, moved
        // by speed, holds it up.
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) nr[i * 3 + j] = d[i * 3 + 0] * r[0 * 3 + j] + d[i * 3 + 1] * r[1 * 3 + j] + d[i * 3 + 2] * r[2 * 3 + j];
        float mv = (np[0] - p[0]) * (np[0] - p[0]) + (np[1] - p[1]) * (np[1] - p[1]) + (np[2] - p[2]) * (np[2] - p[2]);
        {   // TOTK_COOP_TEST_RIDE: each tick for 3 s after stepping on, where the Link was, where it is put, the object
            static int dbg = -1, n_dbg;
            static float put[3];
            if (dbg < 0) dbg = cfg_get("TOTK_COOP_TEST_RIDE") != NULL;
            if (dbg && n_dbg < 90) {
                n_dbg++;
                rt_log("coop: test: ride: tick %.3f s: Link %.3f %.3f %.3f (put %.3f %.3f %.3f last tick), object moved %.3f %.3f %.3f, put at %.3f %.3f %.3f",
                       frame_step_world_us() / 1e6, p[0], p[1], p[2], put[0], put[1], put[2], on->pos[0] - last_pos[0],
                       on->pos[1] - last_pos[1], on->pos[2] - last_pos[2], np[0], np[1], np[2]);
                memcpy(put, np, 12);
            }
        }
        if (mv > 1e-6f && !nocarry) {
            float m[12];
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) m[i * 4 + j] = nr[i * 3 + j];
                m[i * 4 + 3] = np[i];
            }
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        }
    }
    memcpy(last_pos, on->pos, 12), memcpy(last_rot, on->rot, 36);
    {   // for the state out: the Link in the object's frame (from this tick's place, as the owner sent the object)
        float q[3], o[3];
        if (coop_rd(me + COOP_ACTOR_POS, q, 12)) {
            for (int i = 0; i < 3; i++) o[i] = q[i] - on->pos[i];
            for (int j = 0; j < 3; j++) ride_off_now[j] = on->rot[0 * 3 + j] * o[0] + on->rot[1 * 3 + j] * o[1] + on->rot[2 * 3 + j] * o[2];
            ride_owner_now = on->owner, ride_id_now = on->id;
        }
    }
}
// Test (TOTK_COOP_TEST_RIDE=1, two games): the game that joined as player 1 makes a glider 3 m beside its Link at
// 22 s and from 30 s moves it along x (the variable's value in m/s) and 0.3 m/s up for 10 s (COOP_FORCE_MATRIX each
// tick); the other game
// puts its Link 0.6 m above its copy at 27 s, and every 15 ticks logs its Link's place on the copy.
static void ride_test(Cpu *c, uint64_t me) {
    static int on = -1, stage;
    static _Alignas(16) uint8_t handle[0x40], link[0x40];
    static uint64_t wing;
    static float base[3];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_RIDE") != NULL;
    if (!on) return;
    double now = frame_step_world_us() / 1e6;
    float p[3];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    if (coop_net_id() == 1) {
        if (stage == 0 && now > 22) {
            float at[3] = {p[0] + 3, p[1] + 1, p[2]}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            stage = coop_request_create(c, "SpObj_LiftGeneratorWing_A_01", at, rot, handle) ? 1 : 9;
        }
        if (stage == 1 && coop_handle_status(handle) == 3) {
            uint64_t ref;
            wing = coop_claim(c, handle, link, &ref);
            stage = wing ? 2 : 9;
            rt_log("coop: test: ride: glider %llx", (unsigned long long)wing);
        }
        if (stage == 2 && now > 30) coop_rd(wing + COOP_ACTOR_POS, base, 12), stage = 3;
        if (stage == 3 && now < 40) {
            float sp = (float)atof(cfg_get("TOTK_COOP_TEST_RIDE"));   // its value: the speed along x, m/s (1 m/s for "1")
            if (sp <= 0) sp = 1;
            float t = (float)(now - 30), m[12] = {1, 0, 0, base[0] + sp * t, 0, 1, 0, base[1] + 0.3f * t, 0, 0, 1, base[2]};
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, wing, (uint64_t)(uintptr_t)m, 0, 0);
        }
        static int t1;
        float w[3], o[3];
        if (stage >= 2 && ++t1 % 15 == 0 && now < 45 && slots[0].actor && coop_rd(wing + COOP_ACTOR_POS, w, 12) &&
            coop_rd(slots[0].actor + COOP_ACTOR_POS, o, 12))
            rt_log("coop: test: ride: %.2f s glider at %.2f %.2f %.2f, the other Link %+.2f %+.2f %+.2f from it", now, w[0], w[1], w[2],
                   o[0] - w[0], o[1] - w[1], o[2] - w[2]);
        return;
    }
    RideObj objs[64];
    int n = ride_objects(objs, 64);
    if (!n) return;
    const RideObj *g = &objs[0];
    if (stage == 0 && now > 27) {
        float m[12] = {1, 0, 0, g->pos[0], 0, 1, 0, g->pos[1] + 0.6f, 0, 0, 1, g->pos[2]};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        rt_log("coop: test: ride: this Link put on the copy at %.2f %.2f %.2f", g->pos[0], g->pos[1] + 0.6f, g->pos[2]);
        stage = 1;
    }
    static int t;
    if (stage == 1 && ++t % 15 == 0 && now < 45)
        rt_log("coop: test: ride: %.2f s copy at %.2f %.2f %.2f, Link %+.2f %+.2f %+.2f from it", now, g->pos[0], g->pos[1], g->pos[2],
               p[0] - g->pos[0], p[1] - g->pos[1], p[2] - g->pos[2]);
}
// Test (TOTK_COOP_TEST_ZONAI=<actor name>, one game, tools/coop/zonai_hit_3rd.txt): what turns a Zonai device on.
// At 21 s this Link is turned to face +x, at 22 s the device is made 1.1 m in front of it; its first 0x3000 bytes are kept at 24 s (before the hits
// at 25 s), at 28 s (on) and at 45 s (off again, if it stopped); each word that is one thing before, another while
// on and the first again after (or that only changed on) is logged, with the device's place every 30 ticks.
static void zonai_test(Cpu *c, uint64_t me) {
    static const char *name;
    static int stage;
    static _Alignas(16) uint8_t handle[0x40], link[0x40];
    static uint64_t dev;
    static uint32_t snap[3][0x3000 / 4];
    if (!name) name = cfg_get("TOTK_COOP_TEST_ZONAI") ? cfg_get("TOTK_COOP_TEST_ZONAI") : "";
    if (!*name) return;
    double now = frame_step_world_us() / 1e6;
    float p[3], r[9];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12) || !coop_rd(me + COOP_ACTOR_ROT, r, 36)) return;
    static int turned;
    if (!turned && now > 21) {   // face +x (its forward is the rotation's third column), where the stairs go on level
        float m[12] = {0, 0, 1, p[0], 0, 1, 0, p[1], -1, 0, 0, p[2]};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
        turned = 1;
        return;
    }
    if (stage == 0 && now > 22) {
        float at[3] = {p[0] + 1.1f, p[1] + 0.3f, p[2]}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        rt_log("coop: test: zonai: this Link faces %.2f %.2f %.2f", r[2], r[5], r[8]);
        stage = coop_request_create(c, name, at, rot, handle) ? 1 : 9;
        rt_log("coop: test: zonai: %s asked for at %.2f %.2f %.2f", name, at[0], at[1], at[2]);
    }
    if (stage == 1 && coop_handle_status(handle) == 3) {
        uint64_t ref;
        dev = coop_claim(c, handle, link, &ref);
        stage = dev ? 2 : 9;
        rt_log("coop: test: zonai: %s is %llx", name, (unsigned long long)dev);
    }
    if (!dev) return;
    static int t;
    float q[3];
    uint32_t w[8] = {0};
    if (++t % 15 == 0 && now < 50 && coop_rd(dev + COOP_ACTOR_POS, q, 12) && coop_rd(dev + 0x400, w, 32))
        rt_log("coop: test: zonai: %.2f s at %.2f %.2f %.2f, +0x400.. %x %x %x %x %x %x %x %x", now, q[0], q[1], q[2],
               w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    int k = stage == 2 && now > 24 ? 0 : stage == 3 && now > 28 ? 1 : stage == 4 && now > 45 ? 2 : -1;
    if (k < 0) return;
    coop_rd(dev, snap[k], sizeof snap[k]);
    stage++;
    if (k == 0) return;
    int n = 0;
    for (int i = 0; i < 0x3000 / 4 && n < 120; i++) {
        uint32_t a = snap[0][i], b = snap[1][i], cc = snap[2][i];
        if (k == 1 && a != b && a < 0x10000 && b < 0x10000)
            rt_log("coop: test: zonai: on: +%#x %#x -> %#x", i * 4, a, b), n++;
        if (k == 2 && a != b && cc == a && a < 0x10000 && b < 0x10000)
            rt_log("coop: test: zonai: on and back: +%#x %#x -> %#x -> %#x", i * 4, a, b, cc), n++;
    }
    rt_log("coop: test: zonai: snapshot %d, %d words logged", k, n);
}
// Test (TOTK_COOP_TEST_ZONAILOG=1; the tester plays it: drop a Zonai device, hit it to turn it on, let it stop, again):
// which words turn a device on. Every 10 ticks, for up to 8 devices (actors named SpObj_..., not a capsule) within
// 15 m of this Link: their first 0x3000 bytes, and 0x400 bytes of each of their first 128 components ([actor+0x228]
// + 8 * i), are compared with the last look; a byte that goes between 0 and 1 is logged as
// "zonai: <name> <where> a -> b", each place at most 30 times, with the device's place and speed, so the words that
// go on with the device and off when it stops can be picked out of the log afterwards.
#define ZL_DEV 8
#define ZL_ACT 0x3000
#define ZL_COMP 128
#define ZL_CB 0x400
typedef struct {
    uint64_t actor;
    char name[48];
    uint32_t act[ZL_ACT / 4];
    uint64_t comp[ZL_COMP];
    uint32_t cw[ZL_COMP][ZL_CB / 4];
    uint8_t said[ZL_ACT + ZL_COMP * ZL_CB];   // per byte
    float last[3];
    int seen, have;
} ZlDev;
static ZlDev *zl;
static int zl_tick;
static void zl_fn(uint64_t a, void *arg) {
    const float *me = arg;
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    if (strncmp(nm, "SpObj_", 6) || strstr(nm, "Capsule")) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    if ((p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]) > 225) return;
    int d = -1, free = -1;
    for (int i = 0; i < ZL_DEV && d < 0; i++) {
        if (zl[i].actor == a) d = i;
        else if (free < 0 && (!zl[i].actor || zl_tick - zl[i].seen > 300)) free = i;
    }
    if (d < 0) {
        if (free < 0) return;
        d = free;
        memset(&zl[d], 0, sizeof zl[d]);
        zl[d].actor = a, snprintf(zl[d].name, sizeof zl[d].name, "%s", nm);
        rt_log("coop: zonai: watching %s %llx at %.2f %.2f %.2f", nm, (unsigned long long)a, p[0], p[1], p[2]);
    }
    ZlDev *z = &zl[d];
    z->seen = zl_tick;
    static uint32_t buf[ZL_ACT / 4];
    if (!coop_rd(a, buf, sizeof buf)) return;
    float sp = z->have ? sqrtf((p[0] - z->last[0]) * (p[0] - z->last[0]) + (p[1] - z->last[1]) * (p[1] - z->last[1]) +
                               (p[2] - z->last[2]) * (p[2] - z->last[2])) * 3 : 0;   // m/s over the 10 ticks (about 1/3 s)
    double now = frame_step_world_us() / 1e6;
    // Bytes that go between 0 and 1 (a bool): a fan's on state was not a small word (first played run, 2026-11-05).
    const uint8_t *nb = (const uint8_t *)buf, *ob = (const uint8_t *)z->act;
    for (int i = 0; i < ZL_ACT && z->have; i++)
        if (nb[i] != ob[i] && nb[i] < 2 && ob[i] < 2 && z->said[i] < 30)
            z->said[i]++, rt_log("coop: zonai: %.2f s %s actor+%#x %u -> %u (at %.1f %.1f %.1f, %.1f m/s)", now, z->name, i,
                                 ob[i], nb[i], p[0], p[1], p[2], sp);
    memcpy(z->act, buf, sizeof buf);
    uint64_t tbl = coop_rd_ptr(a + 0x228);
    for (int k = 0; k < ZL_COMP && tbl; k++) {
        uint64_t c = coop_rd_ptr(tbl + 8 * (uint64_t)k);
        static uint32_t cb[ZL_CB / 4];
        if (!c || (c & 7) || !coop_rd(c, cb, sizeof cb)) { z->comp[k] = 0; continue; }
        if (z->have && z->comp[k] == c)
            for (int i = 0; i < ZL_CB; i++) {
                const uint8_t *n8 = (const uint8_t *)cb, *o8 = (const uint8_t *)z->cw[k];
                uint8_t *sd = &z->said[ZL_ACT + k * ZL_CB + i];
                if (n8[i] != o8[i] && n8[i] < 2 && o8[i] < 2 && *sd < 30)
                    (*sd)++, rt_log("coop: zonai: %.2f s %s comp%d+%#x %u -> %u (at %.1f %.1f %.1f, %.1f m/s)", now, z->name, k, i,
                                    o8[i], n8[i], p[0], p[1], p[2], sp);
            }
        z->comp[k] = c;
        memcpy(z->cw[k], cb, sizeof cb);
    }
    memcpy(z->last, p, 12), z->have = 1;
}
static void zonai_log(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ZONAILOG") != NULL;
    if (!on || ++zl_tick % 10) return;
    if (!zl) zl = calloc(ZL_DEV, sizeof *zl);
    float p[3];
    if (!zl || !coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(zl_fn, p);
}
// Test (TOTK_COOP_TEST_ZONAIDUMP=1): every 30 ticks, the first device within 15 m of this Link whose name starts
// SpObj_WindGenerator (a fan) is written to logs/zonai_dump.bin (beside the game): a header {double world seconds, u64 actor, float
// pos[3], u32 0}, then its first 0x3000 bytes and 0x400 bytes of each of its first 128 components (zeros where a
// component is missing). The bytes that hold one value while it is on and another while it is off are picked out
// afterwards (tools/coop/zonai_dump.py) with the hit times from the logger.
static void zd_fn(uint64_t a, void *arg) {
    uint64_t *found = arg;
    char nm[48];
    if (found[1]) return;
    coop_actor_name(a, nm, sizeof nm);
    if (strncmp(nm, "SpObj_WindGenerator", 19)) return;
    float p[3], q[3];
    memcpy(q, &found[2], 12);
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12) ||
        (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]) > 625) return;   // 25 m
    found[1] = a;
}
// An EquipmentUser's item of a synced slot kind (0 weapon, 1 bow, 2 shield): its actor, or 0.
static uint64_t eq_item(uint64_t eq, int kind) {
    for (int row = 0; eq && row < 24; row++) {
        uint64_t ref = coop_rd_ptr(eq + GEAR_TABLE + 0x18 * (uint64_t)row), a = ref ? coop_rd_ptr(ref + 0x40) : 0;
        char nm[32];
        if (a && gear_kind(coop_actor_name(a, nm, sizeof nm)) == kind) return a;
    }
    return 0;
}
// Where an item is relative to a Link, in the Link's own frame (x right, y up, z forward, as its rotation rows give
// them): world = rot * local.
static int item_local(uint64_t link, uint64_t item, float local[3]) {
    float lp[3], r[9], ip[3];
    if (!coop_rd(link + COOP_ACTOR_POS, lp, 12) || !coop_rd(link + COOP_ACTOR_ROT, r, 36) || !coop_rd(item + COOP_ACTOR_POS, ip, 12)) return 0;
    float d[3] = {ip[0] - lp[0], ip[1] - lp[1], ip[2] - lp[2]};
    for (int j = 0; j < 3; j++) local[j] = r[j] * d[0] + r[3 + j] * d[1] + r[6 + j] * d[2];
    return 1;
}
// Shield surf. The surfing Link's shield is under its feet, out of hand (surf.txt: 0.14 m up in the Link's frame,
// against 1.2 m on its back; main+0x3e322c says not in hand), and the frozen proxy kept it on its back (the tester's
// session, 2026-11-05). The owner sends where its shield is whenever it is out of hand and lower than 0.5 m in its
// frame (NetState flags bit 11); the other games put the proxy's shield there each frame, after the proxy's model
// push, with the shield's own update off (shield_place, surf_shield).
static void shield_send(uint64_t me, uint64_t eq, NetState *st) {
    uint64_t sh = eq_item(eq, 2);
    float l[3], r[9], sr[9], m[9];
    if (!sh || (st->flags >> 10 & 1) || !item_local(me, sh, l) || l[1] > 0.5f) return;
    if (!coop_rd(me + COOP_ACTOR_ROT, r, 36) || !coop_rd(sh + COOP_ACTOR_ROT, sr, 36)) return;
    for (int i = 0; i < 3; i++)   // the shield's rotation in the Link's frame: R^T S
        for (int j = 0; j < 3; j++) m[i * 3 + j] = r[0 * 3 + i] * sr[0 * 3 + j] + r[1 * 3 + i] * sr[1 * 3 + j] + r[2 * 3 + i] * sr[2 * 3 + j];
    st->flags |= 1u << 11;
    memcpy(st->shield_pos, l, 12);
    mat_quat(m, st->shield_q);
}
static void shield_place(Cpu *c, Slot *s) {
    s->shield_actor = 0;
    if (!s->shield_placed || !s->actor) return;
    uint64_t sh = eq_item(eq_of_actor(s->actor), 2);
    float p[3], r[9], w[9], m[12];
    if (!sh || !coop_rd(s->actor + COOP_ACTOR_POS, p, 12) || !coop_rd(s->actor + COOP_ACTOR_ROT, r, 36)) return;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) w[i * 3 + j] = r[i * 3 + 0] * s->shield_rot[0 * 3 + j] + r[i * 3 + 1] * s->shield_rot[1 * 3 + j] + r[i * 3 + 2] * s->shield_rot[2 * 3 + j];
        float t = p[i] + r[i * 3 + 0] * s->shield_local[0] + r[i * 3 + 1] * s->shield_local[1] + r[i * 3 + 2] * s->shield_local[2];
        for (int j = 0; j < 3; j++) m[i * 4 + j] = w[i * 3 + j];
        m[i * 4 + 3] = t;
    }
    float t3[3] = {m[3], m[7], m[11]};
    s->shield_actor = sh;
    memcpy((void *)(uintptr_t)(sh + COOP_ACTOR_POS), t3, 12);
    memcpy((void *)(uintptr_t)(sh + COOP_ACTOR_ROT), w, 36);
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(sh + 0x228) + 0x8) + 0x28);
    if (model) {
        memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        ST8(model + 0x240, LD8(model + 0x240) | 1);
        coop_guest_call(c, MOD_MAIN_BASE + PROXY_MODEL_PUSH, model, 0, 0, 0);
    }
    // its physics body too: it wrote the actor's place back to where the body was (surf.txt, 2026-11-05)
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, sh, (uint64_t)(uintptr_t)m, 0, 0);
}
// Test (TOTK_COOP_TEST_SURFLOG=1): every 15 ticks, for this Link and each co-op Link here, where its shield is
// (relative to it), whether the shield is in hand, and its EquipmentUser's per-slot bytes +0x4620..+0x4627. For the
// shield surf, which the proxy showed with the shield on its back (a play session, 2026-11-05).
static void surf_log(Cpu *c, uint64_t me) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_SURFLOG") != NULL;
    if (!on || ++t % 15) return;
    for (int i = -1; i < COOP_MAX_PLAYERS; i++) {
        uint64_t link = i < 0 ? me : slots[i].actor, eq = link ? eq_of_actor(link) : 0, sh = eq_item(eq, 2);
        float l[3];
        uint8_t b[8] = {0};
        if (!sh || !item_local(link, sh, l)) continue;
        coop_rd(eq + 0x4620, b, 8);
        float mt[12] = {0}, ml[3] = {0}, lp[3], lr[9];
        uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(sh + 0x228) + 0x8) + 0x28);
        if (model && coop_rd(model + 0x1f8, mt, sizeof mt) && coop_rd(link + COOP_ACTOR_POS, lp, 12) && coop_rd(link + COOP_ACTOR_ROT, lr, 36)) {
            float d[3] = {mt[3] - lp[0], mt[7] - lp[1], mt[11] - lp[2]};
            for (int j = 0; j < 3; j++) ml[j] = lr[j] * d[0] + lr[3 + j] * d[1] + lr[6 + j] * d[2];
        }
        int hand = (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, 4, 0, 0) & 1);
        rt_log("coop: test: surf %.2f s %s: shield at %.2f %.2f %.2f (model %.2f %.2f %.2f), in hand %d, slots %02x %02x %02x %02x %02x %02x %02x %02x",
               frame_step_world_us() / 1e6, i < 0 ? "this Link" : "co-op Link", l[0], l[1], l[2], ml[0], ml[1], ml[2], hand, b[0], b[1], b[2], b[3],
               b[4], b[5], b[6], b[7]);
    }
}
static void zonai_dump(uint64_t me) {
    static int on = -1, t;
    static FILE *f;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ZONAIDUMP") != NULL;
    if (!on || ++t % 30) return;
    uint64_t found[4] = {0};
    if (!coop_rd(me + COOP_ACTOR_POS, &found[2], 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(zd_fn, found);
    uint64_t a = found[1];
    if (!a) return;
    if (!f) f = fopen("logs/zonai_dump.bin", "wb");
    if (!f) return;
    static uint8_t buf[0x3000 + 128 * 0x400];
    memset(buf, 0, sizeof buf);
    coop_rd(a, buf, 0x3000);
    uint64_t tbl = coop_rd_ptr(a + 0x228);
    for (int k = 0; k < 128 && tbl; k++) {
        uint64_t c = coop_rd_ptr(tbl + 8 * (uint64_t)k);
        if (c && !(c & 7)) coop_rd(c, buf + 0x3000 + k * 0x400, 0x400);
    }
    struct { double t; uint64_t actor; float pos[3]; uint32_t pad; } h = {frame_step_world_us() / 1e6, a, {0}, 0};
    coop_rd(a + COOP_ACTOR_POS, h.pos, 12);
    fwrite(&h, sizeof h, 1, f), fwrite(buf, sizeof buf, 1, f), fflush(f);
}
// Test (TOTK_COOP_TEST_FANSET=<1..9>, one game; 9: no writes, this Link faces the fan for the script's hits): which of a fan's three on/off words (found by the played dump
// test, 2026-11-05) turns it on. At 21 s this Link is put at 31.5 1514.6 1510.2 (the flat ground where the tester's fans
// stood still), at 22 s a fan (SpObj_WindGenerator_A_01) is made beside it; at 30 s the chosen word is written to
// its "on" value (1: [comp11]+0x348 |= 0x2000; 2: [comp28]+0x398 and +0x39c = 10; 3: [comp107]+0x398 = 0x40), at
// 40 s back to "off"; every 15 ticks from 24 s to 50 s all three are logged, to see whether the others follow.
// A device's on flag, as main+0x2f8b70 reads it: the actor's component 0, its +0x10 (a table of named bools): a hash
// table at +0x1c8 of {u32 name hash, u32 index}, +0x1d4 entries, probed from hash % entries; the entry 0xbcd1015f
// gives the index into the bools at +0x1a0 (+0x198 of them). Its address, or 0.
static uint64_t dev_on_flag_at(uint64_t actor, uint64_t *bb_out, uint32_t *index_out);
static uint64_t dev_on_flag(uint64_t actor) { return dev_on_flag_at(actor, NULL, NULL); }
static uint64_t dev_on_flag_at(uint64_t actor, uint64_t *bb_out, uint32_t *index_out) {
    uint64_t tbl = coop_rd_ptr(actor + 0x228), c0 = tbl ? coop_rd_ptr(tbl) : 0, bb = c0 ? coop_rd_ptr(c0 + 0x10) : 0;
    uint32_t n = 0, nb = 0;
    if (!bb || !coop_rd(bb + 0x1d4, &n, 4) || !n || n > 65536 || !coop_rd(bb + 0x198, &nb, 4)) return 0;
    uint64_t ht = coop_rd_ptr(bb + 0x1c8), bools = coop_rd_ptr(bb + 0x1a0);
    if (!ht || !bools) return 0;
    uint32_t start = 0xbcd1015fu % n, i = start;
    for (;;) {
        uint32_t e[2] = {0, 0};
        if (!coop_rd(ht + 8 * (uint64_t)i, e, 8) || !e[0]) return 0;
        if (e[0] == 0xbcd1015fu) {
            if (bb_out) *bb_out = bb;
            if (index_out) *index_out = e[1];
            return (int32_t)e[1] >= 0 && e[1] < nb ? bools + e[1] : 0;
        }
        i = i + 1 < n ? i + 1 : 0;
        if (i == start) return 0;
    }
}
static uint64_t fan_comp(uint64_t a, int k) { uint64_t t = coop_rd_ptr(a + 0x228); return t ? coop_rd_ptr(t + 8 * (uint64_t)k) : 0; }
static void fanset_test(Cpu *c, uint64_t me) {
    static int which = -1, stage;
    static _Alignas(16) uint8_t handle[0x40], link[0x40];
    static uint64_t fan;
    if (which < 0) which = cfg_get("TOTK_COOP_TEST_FANSET") ? atoi(cfg_get("TOTK_COOP_TEST_FANSET")) : 0;
    if (!which || (coop_net_on() && coop_net_id() != 1)) return;   // on a relay, only in the game that joined first
    double now = frame_step_world_us() / 1e6;
    if (stage == 0 && now > 21) {   // 9: facing -x, 1 m from where the fan will be (scripted sword hits reach it)
        float m9[12] = {0, 0, -1, 31.3f, 0, 1, 0, 1514.8f, 1, 0, 0, 1510.7f};
        float m[12] = {1, 0, 0, 31.5f, 0, 1, 0, 1514.8f, 0, 0, 1, 1510.2f};
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)(which == 9 ? m9 : m), 0, 0);
        stage = 1;
    }
    if (stage == 1 && now > 22) {
        float at[3] = {30.3f, 1515.0f, 1510.7f}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        stage = coop_request_create(c, "SpObj_WindGenerator_A_01", at, rot, handle) ? 2 : 9;
    }
    if (stage == 2 && coop_handle_status(handle) == 3) {
        uint64_t ref;
        fan = coop_claim(c, handle, link, &ref);
        stage = fan ? 3 : 9;
        rt_log("coop: test: fanset: fan %llx", (unsigned long long)fan);
        extern uint64_t coop_fan_actor;   // coop_spawn.c TOTK_COOP_TEST_FANHIT
        coop_fan_actor = fan;
    }
    // Wind probes: four BombFruits 1.5 m from the fan on each side, made at 25 s; their places are logged with the
    // rest, so the side the fan blows to shows as one that moves once it is on.
    static _Alignas(16) uint8_t ph[4][0x40], pl[4][0x40];
    static uint64_t probe[4];
    static int probe_stage;
    static const float off[4][2] = {{1.5f, 0}, {-1.5f, 0}, {0, 1.5f}, {0, -1.5f}};
    if (fan && probe_stage == 0 && now > 25 && (which == 6 || which == 7)) {
        for (int i = 0; i < 4; i++) {
            float at[3] = {30.3f + off[i][0], 1514.9f, 1510.7f + off[i][1]}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            coop_request_create(c, "BombFruit", at, rot, ph[i]);
        }
        probe_stage = 1;
    }
    if (probe_stage == 1) {
        int all = 1;
        for (int i = 0; i < 4; i++)
            if (!probe[i]) {
                if (coop_handle_status(ph[i]) == 3) { uint64_t ref; probe[i] = coop_claim(c, ph[i], pl[i], &ref); }
                else all = 0;
            }
        if (all) probe_stage = 2;
    }
    {
        static int tp;
        float q[4][3] = {{0}};
        if (probe_stage == 2 && ++tp % 30 == 0 && now < 50) {
            for (int i = 0; i < 4; i++) if (probe[i]) coop_rd(probe[i] + COOP_ACTOR_POS, q[i], 12);
            rt_log("coop: test: fanset: %.2f s probes +x %.2f %.2f %.2f, -x %.2f %.2f %.2f, +z %.2f %.2f %.2f, -z %.2f %.2f %.2f", now,
                   q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2], q[2][0], q[2][1], q[2][2], q[3][0], q[3][1], q[3][2]);
        }
    }
    if (!fan) return;
    uint64_t c11 = fan_comp(fan, 11), c28 = fan_comp(fan, 28), c107 = fan_comp(fan, 107);
    if (!c11 || !c28 || !c107) return;
    if (which == 9) {   // 9: no writes; the script's sword hits (fan_hits_3rd.txt, 34 s) turn it on. From 33.5 s to 35 s,
        // every frame, the words of its damage component ([actor+0x228]+0x2f8, 0x2000 bytes) that changed are logged
        // Replay: the frame the hit record appears (+0x1a4 nonzero), dc+0x1a0..+0x300 is kept; at 38 s it is written
        // into a second fan made at 22 s 1.8 m to +z (its hit position moved with it), to see whether that turns it on.
        static uint8_t rec[0x160], prev[0x160], chg[0x160 / 4];
        static int have_rec, replayed, have_prev;
        static _Alignas(16) uint8_t h2[0x40], l2[0x40];
        static uint64_t fan2;
        static int f2;
        if (f2 == 0 && !coop_net_on()) {   // alone only (the replay test)
            float at[3] = {30.3f, 1515.0f, 1512.5f}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            f2 = coop_request_create(c, "SpObj_WindGenerator_A_01", at, rot, h2) ? 1 : 9;
        }
        if (f2 == 1 && coop_handle_status(h2) == 3) { uint64_t ref; fan2 = coop_claim(c, h2, l2, &ref); f2 = fan2 ? 2 : 9; }
        {
            uint64_t d0 = coop_rd_ptr(coop_rd_ptr(fan + 0x228) + 0x2f8);
            uint32_t flag = 0;
            if (!have_rec && d0 && coop_rd(d0 + 0x1a4, &flag, 4) && flag && have_prev) {
                coop_rd(d0 + 0x1a0, rec, sizeof rec), have_rec = 1;
                int n = 0;
                for (int i = 0; i < 0x160 / 4; i++) n += chg[i] = memcmp(rec + 4 * i, prev + 4 * i, 4) != 0;
                rt_log("coop: test: fanset: hit record kept at %.3f s, %d words changed", now, n);
            } else if (!have_rec && d0 && coop_rd(d0 + 0x1a0, prev, sizeof prev)) have_prev = 1;
            uint64_t d2 = fan2 ? coop_rd_ptr(coop_rd_ptr(fan2 + 0x228) + 0x2f8) : 0;
            if (have_rec && !replayed && now > 38 && d2) {
                float p1[3], p2[3], hp[3];
                coop_rd(fan + COOP_ACTOR_POS, p1, 12), coop_rd(fan2 + COOP_ACTOR_POS, p2, 12);
                memcpy(hp, rec + 0x5c, 12);   // dc+0x1fc: where it was hit
                for (int k = 0; k < 3; k++) hp[k] += p2[k] - p1[k];
                memcpy(rec + 0x5c, hp, 12);
                // only the words the hit changed; an address inside fan 1's damage component or actor is moved to fan 2's
                for (int i = 0; i < 0x160 / 4; i++) {
                    if (!chg[i]) continue;
                    if (i % 2 == 0 && i + 1 < 0x160 / 4 && chg[i + 1]) {   // a 64-bit word that may be such an address
                        uint64_t v;
                        memcpy(&v, rec + 4 * i, 8);
                        if (v >= d0 && v < d0 + 0x2000) v = v - d0 + d2;
                        else if (v >= fan && v < fan + 0x3000) v = v - fan + fan2;
                        memcpy(rec + 4 * i, &v, 8);
                    }
                    memcpy((void *)(uintptr_t)(d2 + 0x1a0 + 4 * i), rec + 4 * i, 4);
                }
                replayed = 1;
                rt_log("coop: test: fanset: hit record written into fan 2 at %.3f s", now);
            }
            {   // fan 2's dc words that change in the 0.5 s after the write: is the record read (cleared) at all?
                static uint32_t l2w[0x400 / 4];
                static int h2w;
                static int64_t lu;
                int64_t us2 = frame_step_world_us();
                if (d2 && now > 37.9 && now < 38.6 && us2 != lu) {
                    static uint32_t cw[0x400 / 4];
                    lu = us2;
                    if (coop_rd(d2 + 0x100, cw, sizeof cw)) {
                        int n = 0;
                        for (int i = 0; i < 0x400 / 4 && h2w && n < 40; i++)
                            if (cw[i] != l2w[i]) rt_log("coop: test: fan2dc: %.3f s dc+%#x %#x -> %#x", now, 0x100 + i * 4, l2w[i], cw[i]), n++;
                        memcpy(l2w, cw, sizeof cw), h2w = 1;
                    }
                }
            }
            static int t2;
            if (fan2 && ++t2 % 30 == 0 && now > 36 && now < 46) {
                uint64_t f = dev_on_flag(fan2), c28 = fan_comp(fan2, 28), c11 = fan_comp(fan2, 11);
                rt_log("coop: test: fanset: %.2f s fan 2 on bool %d, comp11+0x348 %#x, comp28+0x398 %u", now, f ? LD8(f) : -1,
                       c11 ? LD32(c11 + 0x348) : 0, c28 ? LD32(c28 + 0x398) : 0);
            }
        }
        static uint32_t last[0x2000 / 4];
        static int have;
        static int64_t last_us;
        int64_t us = frame_step_world_us();
        uint64_t dc = coop_rd_ptr(coop_rd_ptr(fan + 0x228) + 0x2f8);
        if (now > 33.5 && now < 35 && dc && us != last_us) {
            static uint32_t cur[0x2000 / 4];
            last_us = us;
            if (coop_rd(dc, cur, sizeof cur)) {
                int n = 0;
                for (int i = 0; i < 0x2000 / 4 && have && n < 80; i++)
                    if (cur[i] != last[i]) rt_log("coop: test: fanhitdc: %.3f s dc+%#x %#x -> %#x", now, i * 4, last[i], cur[i]), n++;
                memcpy(last, cur, sizeof cur), have = 1;
            }
        }
        return;
    }
    if (which == 10) {   // 10: at 30 s an arrow made 2 m from the fan (+z) and sent at it at 30 m/s: does a hit by a
        static _Alignas(16) uint8_t ah[0x40], al[0x40];   // thrown arrow turn it on? (fanstate logs it)
        static uint64_t arrow;
        static int ast, av;
        float fp[3];
        if (ast == 0 && now > 30 && coop_rd(fan + COOP_ACTOR_POS, fp, 12)) {
            float at[3] = {fp[0], fp[1] + 0.3f, fp[2] + 2.0f}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            ast = coop_request_create(c, "NormalArrow", at, rot, ah) ? 1 : 9;
        }
        if (ast == 1 && coop_handle_status(ah) == 3) {
            uint64_t ref;
            arrow = coop_claim(c, ah, al, &ref);
            ast = arrow ? 2 : 9;
            rt_log("coop: test: fanset: arrow %llx", (unsigned long long)arrow);
        }
        if (ast == 2 && av++ < 6) {
            uint64_t phys = coop_rd_ptr(coop_rd_ptr(arrow + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
            uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
            _Alignas(16) float v[4] = {0, 0, -30, 0};
            if (body) coop_guest_call(c, MOD_MAIN_BASE + 0x567750, body, (uint64_t)(uintptr_t)v, 0, 0);
            float q[3] = {0};
            coop_rd(arrow + COOP_ACTOR_POS, q, 12);
            rt_log("coop: test: fanset: arrow body %llx at %.2f %.2f %.2f", (unsigned long long)body, q[0], q[1], q[2]);
        }
        return;
    }
    if (which == 4) {   // 4: which code writes [comp107]+0x398 (the word the game puts back): a write watch for 1 s at 30 s
        int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
        void coop_watch_disarm(void);
        extern int coop_watch_writes;
        if (stage == 3 && now > 30) coop_watch_writes = 1, coop_watch_arm(c107 + 0x398, 4), stage = 4, rt_log("coop: test: fanset: watching writes");
        else if (stage == 4 && now > 31) coop_watch_disarm(), stage = 5;
        return;
    }
    if ((stage == 3 && now > 30) || (stage == 4 && now > 40)) {
        int set_on = stage == 3;
        if (which == 1) ST32(c11 + 0x348, set_on ? LD32(c11 + 0x348) | 0x2000 : LD32(c11 + 0x348) & ~0x2000u);
        if (which == 2) ST32(c28 + 0x398, set_on ? 10 : 0), ST32(c28 + 0x39c, set_on ? 10 : 0);
        if (which == 3) ST32(c107 + 0x398, set_on ? 0x40 : 0x80);
        if (which == 7) {   // the bool as the game's setter main+0x9fb82c writes it: the value, then the change count
            uint64_t bb = 0;   // for that index (bb+0x1f0, bb+0x1e8 of them) goes up by one
            uint32_t idx = 0, nch = 0;
            uint64_t f = dev_on_flag_at(fan, &bb, &idx);
            if (f) {
                ST8(f, set_on ? 1 : 0);
                uint64_t chg = coop_rd_ptr(bb + 0x1f0);
                if (chg && coop_rd(bb + 0x1e8, &nch, 4) && idx < nch) ST8(chg + idx, (uint8_t)(LD8(chg + idx) + 1));
            }
        }
        if (which == 6) {   // the named bool main+0x2f8b70 reads (dev_on_flag)
            uint64_t f = dev_on_flag(fan);
            if (f) ST8(f, set_on ? 1 : 0);
        }
        if (which == 5) {   // [[comp107]+0x270] +0x878: main+0x2f8b70 sets the on bits from its bit 11 (on) and 21 (off)
            uint64_t o = coop_rd_ptr(c107 + 0x270);
            if (o) ST32(o + 0x878, set_on ? (LD32(o + 0x878) | 0x800) & ~0x200000u : (LD32(o + 0x878) & ~0x800u) | 0x200000);
        }
        rt_log("coop: test: fanset: %.2f s word %d set %s", now, which, set_on ? "on" : "off");
        stage++;
    }
    static int t;
    float p[3];
    if (++t % 15 == 0 && now > 24 && now < 50 && coop_rd(fan + COOP_ACTOR_POS, p, 12))
        rt_log("coop: test: fanset: %.2f s at %.2f %.2f %.2f: comp11+0x348 %#x, comp28+0x398 %u, comp107+0x398 %#x, on bool %#x", now, p[0], p[1], p[2],
               LD32(c11 + 0x348), LD32(c28 + 0x398), LD32(c107 + 0x398), dev_on_flag(fan) ? LD8(dev_on_flag(fan)) : 0xff);
}
// The object main+0x2f8b70 reads to set a device's on bits: in the actor's component +0xa8 (8 * 21), a table at
// +0x38 (+0x30 its count) of entries sorted by their first u32, the one 0x9b84ce69; its +8, then +0x150; its +0x7c is
// compared with 0xd and 0x17 there (2026-11-05). 0 when not found.
static uint64_t dev_state_obj(uint64_t actor) {
    uint64_t tbl = coop_rd_ptr(actor + 0x228), comp = tbl ? coop_rd_ptr(tbl + 0xa8) : 0;
    int32_t n = 0;
    if (!comp || !coop_rd(comp + 0x30, &n, 4) || n <= 0 || n > 4096) return 0;
    uint64_t arr = coop_rd_ptr(comp + 0x38);
    for (int i = 0; arr && i < n; i++) {
        uint64_t e = coop_rd_ptr(arr + 8 * (uint64_t)i);
        uint32_t h = 0;
        if (e && coop_rd(e, &h, 4) && h == 0x9b84ce69u) {
            uint64_t x = coop_rd_ptr(e + 8);
            return x ? coop_rd_ptr(x + 0x150) : 0;
        }
    }
    return 0;
}
// Test (TOTK_COOP_TEST_FANTOGGLE=1, played): every 10 s the on bool of the nearest fan within 15 m is flipped (with its
// change count, as main+0x9fb82c writes it); the player sees whether a fan from a capsule starts and stops. A fan
// made by requestCreateActor showed "on" in its flags but did not blow (no energy, likely).
static void fan_flag_publish(uint64_t me) {   // for coop_spawn.c's TOTK_COOP_TEST_FLAGCALLER: the nearest fan's on bool
    extern uint64_t coop_fan_flag;
    static int t;
    if (!cfg_get("TOTK_COOP_TEST_FLAGCALLER") || ++t % 30) return;
    uint64_t found[4] = {0};
    if (!coop_rd(me + COOP_ACTOR_POS, &found[2], 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(zd_fn, found);
    coop_fan_flag = found[1] ? dev_on_flag(found[1]) : 0;
}
static void fantoggle(uint64_t me) {
    static int on = -1, t;
    fan_flag_publish(me);
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_FANTOGGLE") != NULL;
    if (!on || ++t % 600) return;   // the tick runs twice a frame: about 10 s
    uint64_t found[4] = {0};
    if (!coop_rd(me + COOP_ACTOR_POS, &found[2], 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(zd_fn, found);
    uint64_t bb = 0;
    uint32_t idx = 0, nch = 0;
    uint64_t f = found[1] ? dev_on_flag_at(found[1], &bb, &idx) : 0;
    if (!f) return;
    uint8_t v = LD8(f) ? 0 : 1;
    ST8(f, v);
    uint64_t chg = coop_rd_ptr(bb + 0x1f0);
    if (chg && coop_rd(bb + 0x1e8, &nch, 4) && idx < nch) ST8(chg + idx, (uint8_t)(LD8(chg + idx) + 1));
    uint64_t c11 = fan_comp(found[1], 11), c28 = fan_comp(found[1], 28);
    rt_log("coop: test: fantoggle: %.2f s fan %llx set %s (comp11+0x348 %#x, comp28+0x398 %u)", frame_step_world_us() / 1e6,
           (unsigned long long)found[1], v ? "on" : "off", c11 ? LD32(c11 + 0x348) : 0, c28 ? LD32(c28 + 0x398) : 0);
}
// Test (TOTK_COOP_TEST_FANWATCH=1, played): the code that turns a fan on. While a fan is within 15 m, a write watch
// is on its on bool (dev_on_flag); when the bool changes (a hit), the watch is taken off, which logs the functions
// that wrote it ("coop: ... reads/writes by main+..."). Slow while armed (the page faults).
static void fanwatch(uint64_t me) {
    static int on = -1, armed, done;
    static uint64_t flag;
    static uint8_t was;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_FANWATCH") != NULL;
    if (!on || done >= 3) return;
    int coop_watch_arm(uint64_t addr, uint64_t len);   // coop_watch_win.c
    void coop_watch_disarm(void);
    extern int coop_watch_writes;
    if (armed) {
        static int t;
        uint8_t v = LD8(flag);
        if (v != was || ++t > 30 * 60) {   // a change, or a minute without one
            coop_watch_disarm(), armed = 0, t = 0;
            rt_log("coop: test: fanwatch: on bool %u -> %u", was, v);
            if (v != was) done++;
        }
        return;
    }
    uint64_t found[4] = {0};
    if (!coop_rd(me + COOP_ACTOR_POS, &found[2], 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(zd_fn, found);
    if (!found[1] || !(flag = dev_on_flag(found[1]))) return;
    was = LD8(flag);
    coop_watch_writes = 1;
    if (coop_watch_arm(flag, 1)) armed = 1, rt_log("coop: test: fanwatch: watching the on bool (%u) of %llx", was, (unsigned long long)found[1]);
}
// Test (TOTK_COOP_TEST_FANSTATE=1, played): every 15 ticks, for the first fan within 15 m, that object's +0x78..+0x8f
// and the three on/off words, logged when any of them changed.
static void fanstate_log(uint64_t me) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_FANSTATE") != NULL;
    if (!on || ++t % 15) return;
    uint64_t found[4] = {0};
    if (!coop_rd(me + COOP_ACTOR_POS, &found[2], 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(zd_fn, found);
    uint64_t a = found[1];
    if (!a) return;
    uint64_t so = dev_state_obj(a), c11 = fan_comp(a, 11), c28 = fan_comp(a, 28), c107 = fan_comp(a, 107);
    uint32_t w[6] = {0};
    if (so) coop_rd(so + 0x78, w, 24);
    uint64_t fo = dev_on_flag(a);
    uint32_t v[3] = {c11 ? LD32(c11 + 0x348) : 0, c28 ? LD32(c28 + 0x398) : 0, fo ? LD8(fo) : 0xff};
    static uint32_t last[9];
    uint32_t now9[9] = {w[0], w[1], w[2], w[3], w[4], w[5], v[0], v[1], v[2]};
    if (!memcmp(now9, last, sizeof last)) return;
    memcpy(last, now9, sizeof last);
    rt_log("coop: fanstate: %.2f s obj %llx +0x78.. %x %x %x %x %x %x | %#x %u %#x", frame_step_world_us() / 1e6,
           (unsigned long long)so, w[0], w[1], w[2], w[3], w[4], w[5], v[0], v[1], v[2]);
}
static int this_paused;   // this game sits in a menu (the clock and this Link both still; set by the tick)
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
        memcpy(st.dye, my_dye, sizeof st.dye);
        static CoopObj cur[COOP_OBJS];
        int n_objs = report_arrows(me, cur);
        n_objs += report_props(me, cur + n_objs, COOP_OBJS - n_objs);
        objs_send(cur, n_objs);
        objs_receive(c);
        for (int k = 0; k < 3; k++) memcpy(st.fuse[k], strcmp(my_gear[k], GEAR_NONE) ? my_fuse[k] : "", 32);   // none when unequipped
        match_my_eq(me);
        uint64_t my_eq = eq_of_actor(me);   // weapon, bow, shield in hand
        static const uint64_t hand_of[3] = {0, 3, 4};
        for (int k = 0; my_eq && k < 3; k++)
            if (coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, my_eq, hand_of[k], 0, 0) & 1) st.flags |= 1u << (8 + k);
        shield_send(me, my_eq, &st);
        // TOTK_COOP_FAKE_AT=<s>: the fakes below apply only from that world time (a change during the session)
        const char *fat = cfg_get("TOTK_COOP_FAKE_AT");
        int fakes = !fat || frame_step_world_us() / 1e6 >= atof(fat);
        const char *fh = fakes ? cfg_get("TOTK_COOP_FAKEHAND") : NULL;   // test: these in hand instead (bits 0..2 weapon, bow, shield)
        if (fh) st.flags = (st.flags & ~0x700u) | (uint32_t)(atoi(fh) & 7) << 8;
        const char *fdye = fakes ? cfg_get("TOTK_COOP_FAKEDYE") : NULL;   // test: report these dyes instead (slot order, comma-separated)
        for (int k = 0; fdye && *fdye && k < COOP_GEAR_SLOTS; k++) {
            if (*fdye != ',') st.dye[k] = (int8_t)atoi(fdye);
            fdye += strcspn(fdye, ","), fdye += *fdye == ',';
        }
        // Test: report these items instead, comma-separated in slot order (weapon, bow, shield, head, body, legs);
        // an empty field keeps the real one.
        const char *fake = fakes ? cfg_get("TOTK_COOP_FAKEGEAR") : NULL;
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
        this_paused = paused;
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
        st.ride_owner = (uint8_t)ride_owner_now, st.ride_id = ride_id_now, memcpy(st.ride_off, ride_off_now, 12);
        const char *fw = cfg_get("TOTK_COOP_FAKEWEATHER");   // test: report this weather type instead
        if (fw && *fw) st.weather = atoi(fw);
        coop_net_publish(&st);
        if (coop_proxy_mode()) props_receive(c), ride_carry(c, me);   // placed objects co-op players move; riding them
        props_test(c, me), props_test_where(), ride_test(c, me), pickup_test(c, me), chest_test(c, me), arrow_dump();
    }
    armor_probe(me);
    arrow_log(me);
    refpool_test();
    stand_test(c, me);
    zonai_test(c, me);
    zonai_log(me);
    zonai_dump(me);
    surf_log(c, me);
    fanset_test(c, me);
    fanstate_log(me);
    fanwatch(me);
    fantoggle(me);
    uint64_t my_eq = eq_of_actor(me);
    keep_player_eq(me, my_eq);
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
                for (int k = 0; k < COOP_OBJS; k++)   // their arrows' copies too
                    if (s->mir[k].actor) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->mir[k].actor, COOP_DELETE_REASON, 0, 0);
                memset(s->mir, 0, sizeof s->mir);
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
        s->ride_owner = st.ride_owner, s->ride_id = st.ride_id, memcpy(s->ride_off, st.ride_off, 12);
        s->shield_placed = (st.flags >> 11) & 1;
        if (s->shield_placed) memcpy(s->shield_local, st.shield_pos, 12), quat_mat(st.shield_q, s->shield_rot);
        for (int k = 0; k < COOP_GEAR_SLOTS; k++) {
            memcpy(s->gear_want[k], st.gear[k], 32);
            s->gear_want[k][31] = 0;
            s->dye_want[k] = st.dye[k];
            if (k < 3) memcpy(s->fuse_want[k], st.fuse[k], 32), s->fuse_want[k][31] = 0;
            if (k < 3) s->out_want = (s->out_want & ~(1 << k)) | (int)((st.flags >> (8 + k)) & 1) << k;
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
        {   // the owner presses attack (Y): its proxy runs its own update for 40 frames with Y let through
            static uint64_t was[COOP_MAX_PLAYERS];
            int k = (int)(s - slots);
            int ride_on(void);
            if (proxied(s) && (st.buttons >> 3 & 1) && !(was[k] >> 3 & 1) && attack_thaw_on()) {
                if (s->thaw < 40) s->thaw = 40;
                s->atk_thaw = 40;
                rt_log("coop: proxy: slot %d swings (its own update for 40 frames, Y through)", k);
            }
            was[k] = st.buttons;
        }
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
            // Proxy mode: no gear is made while this game is in a menu; it waits until the game runs again. Made
            // then, armor did not show on the proxy after the menu closed, also when made again later (menu_3rd.txt,
            // a play session, 2026-11-05).
            if (this_paused && proxied(s)) eq = 0;
            char held[COOP_GEAR_SLOTS][32];
            if (eq) eq_gear(eq, held);
            for (int k = 0; eq && k < COOP_GEAR_SLOTS; k++)
                if (s->gear_want[k][0] && (strcmp(s->gear_want[k], s->gear_have[k]) || (k > 2 && s->dye_want[k] != s->dye_have[k]) ||
                                           (k <= 2 && strcmp(s->fuse_want[k], s->fuse_have[k])))) {
                    int redye = (k > 2 && s->dye_want[k] != s->dye_have[k]) || (k <= 2 && strcmp(s->fuse_want[k], s->fuse_have[k]));
                    memcpy(s->gear_have[k], s->gear_want[k], 32), s->dye_have[k] = s->dye_want[k];
                    if (k <= 2) memcpy(s->fuse_have[k], s->fuse_want[k], 32);
                    int none = !strcmp(s->gear_want[k], GEAR_NONE);
                    if ((none && k > 2) || (!strcmp(held[k], s->gear_want[k]) && !redye)) continue;   // nothing to change
                    rt_log("coop: slot %d: owner's gear slot %d is %s (dye %d%s%s)", i, coop_gear_slot[k], s->gear_want[k], s->dye_want[k],
                           k <= 2 && s->fuse_want[k][0] ? ", fused with " : "", k <= 2 ? s->fuse_want[k] : "");
                    give_gear(c, eq, k, none ? "" : s->gear_want[k], s->dye_want[k], -1, s->gear_str[k], k <= 2 ? s->fuse_want[k] : NULL,
                              s->att_str[k < 3 ? k : 0]);
                    // Proxy mode: armor made on the frozen proxy did not show (the hood and tunic stayed as they
                    // were; the default mode shows them, 2026-11-05). Its own update runs for a while to take it.
                    if (proxied(s)) s->thaw = PROXY_THAW_TICKS, rt_log("coop: proxy: slot %d runs its own update for %d ticks (gear)", i, PROXY_THAW_TICKS);
                }
            if (s->state == LIVE) s->live_ticks++;
            int coop_puppet_mode(void);   // coop_world.c
            // Puppet mode: it gets no input, so it never draws an item itself. The EquipmentUser's per-slot byte
            // +0x4620 (0 weapon, 3 bow, 4 shield) bit 1 is what main+0x3e322c also reads as "in hand"; set while the
            // owner holds that item, the item goes to the puppet's hand (checked with the sword, 2026-11-05).
            if (eq && coop_puppet_mode()) {
                static const int slot_of[3] = {0, 3, 4};
                for (int k = 0; k < 3; k++) {
                    uint8_t *f = (uint8_t *)(uintptr_t)(eq + 0x4620 + slot_of[k]);
                    if ((s->out_want >> k) & 1) *f |= 2; else *f &= (uint8_t)~2;
                }
            }
            // Proxy mode: taking an item out or putting it away is the Link's own update's work: with the flag alone
            // a lowered shield stayed in the frozen proxy's hand and then was gone from hand and back; making it
            // again (in either state) did not help (2026-11-05). Its update runs for a while on each change.
            if (eq && proxied(s)) {
                static int skip_putaway = -1;   // TOTK_COOP_TEST_NOTHAW_PUTAWAY=1: no thaw when an item is put away (test)
                if (skip_putaway < 0) skip_putaway = cfg_get("TOTK_COOP_TEST_NOTHAW_PUTAWAY") != NULL;
                if (s->out_have >= 0 && s->out_have != s->out_want && !(skip_putaway && (s->out_want & ~s->out_have) == 0)) {
                    s->thaw = PROXY_THAW_TICKS;
                    rt_log("coop: proxy: slot %d runs its own update for %d ticks (in hand %#x -> %#x)", i, PROXY_THAW_TICKS, s->out_have, s->out_want);
                    // An item taken out is also made again in hand (the weapon create's "1 in hand"): with the flag and
                    // the thaw alone a sword made on the back stayed there while the proxy swung with an empty hand
                    // (a play session; equip_draw_3rd.txt, 2026-11-05). Made in hand it is held (stage 1's test).
                    static int no_remake = -1;   // TOTK_COOP_TEST_NODRAWREMAKE=1: not done (test)
                    if (no_remake < 0) no_remake = cfg_get("TOTK_COOP_TEST_NODRAWREMAKE") != NULL;
                    for (int k = 0; k < 3 && !no_remake; k++)
                        if ((s->out_want & ~s->out_have) >> k & 1 && s->gear_want[k][0] && strcmp(s->gear_want[k], GEAR_NONE)) {
                            give_gear(c, eq, k, s->gear_want[k], -1, 1, s->gear_str[k], s->fuse_want[k], s->att_str[k]);
                            rt_log("coop: proxy: slot %d: %s made again in hand", i, s->gear_want[k]);
                        }
                    // ... and one put away is made again put away (the create's "2"): made in hand, the thaw no longer
                    // took it out of the hand ("holds 0x5, owner 0" through four retries; the next item drawn was held
                    // with it, a play session, 2026-11-05).
                    for (int k = 0; k < 3 && !no_remake; k++)
                        if ((s->out_have & ~s->out_want) >> k & 1 && s->gear_want[k][0] && strcmp(s->gear_want[k], GEAR_NONE)) {
                            give_gear(c, eq, k, s->gear_want[k], -1, 0, s->gear_str[k], s->fuse_want[k], s->att_str[k]);
                            rt_log("coop: proxy: slot %d: %s made again put away", i, s->gear_want[k]);
                        }
                }
                s->out_have = s->out_want;
            }
            static int hand_ticks, hand_said = -1;
            if (eq && coop_puppet_mode() && ++hand_ticks % 30 == 0) {
                static const uint64_t hand_of[3] = {0, 3, 4};
                int now = 0;
                for (int k = 0; k < 3; k++) now |= (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, hand_of[k], 0, 0) & 1) << k;
                if ((now | s->out_want << 4) != hand_said)
                    rt_log("coop: puppet: slot %d holds %#x, owner %#x", i, now, s->out_want), hand_said = now | s->out_want << 4;
                // Proxy mode: a put-away that did not take (a play session: "holds 0x4, owner 0" stayed after sword and
                // shield were put away together, and the shield was then held with the bow, 2026-11-05). Once a thaw is
                // over and the proxy still holds something else than its owner, its own update runs again (at most 4
                // times for one mismatch).
                // Proxy mode, gear by state: what the proxy's EquipmentUser holds against what its owner reports,
                // each slot, and any that differs made again with a thaw (at most 4 times in a row). A change made
                // while this game sat in a menu stayed unseen (menu_3rd.txt, a play session, 2026-11-05).
                static int gear_fixes;
                if (proxied(s) && s->thaw <= 0 && gear_fixes < 4) {
                    char held2[COOP_GEAR_SLOTS][32];
                    eq_gear(eq, held2);
                    int fixed = 0;
                    for (int k = 0; k < COOP_GEAR_SLOTS; k++) {
                        if (!s->gear_want[k][0] || (k > 2 && !strcmp(s->gear_want[k], GEAR_NONE))) continue;
                        if (!strcmp(held2[k], s->gear_want[k])) continue;
                        rt_log("coop: proxy: slot %d gear slot %d holds %s, owner %s: made again", i, coop_gear_slot[k], held2[k], s->gear_want[k]);
                        int none = !strcmp(s->gear_want[k], GEAR_NONE);
                        give_gear(c, eq, k, none ? "" : s->gear_want[k], s->dye_want[k], k <= 2 ? (s->out_want >> k & 1) : -1, s->gear_str[k],
                                  k <= 2 ? s->fuse_want[k] : NULL, s->att_str[k < 3 ? k : 0]);
                        fixed = 1;
                    }
                    if (fixed) s->thaw = PROXY_THAW_TICKS, gear_fixes++;
                    else gear_fixes = 0;
                }
                static int retries;
                if (proxied(s) && s->thaw <= 0 && now != s->out_want && retries < 4 && !cfg_get("TOTK_COOP_TEST_NORETRY")) {
                    s->thaw = PROXY_THAW_TICKS, retries++;
                    rt_log("coop: proxy: slot %d holds %#x, owner %#x: its own update runs again (%d)", i, now, s->out_want, retries);
                } else if (now == s->out_want) retries = 0;
            }
            if (s->state == LIVE && s->source == SRC_NET && coop_proxy_mode()) arrow_copies(c, s);
            if (proxied(s)) {   // proxy mode: placed by proxy_drive, no teleports
                int k = s->nseen++ % 32;
                if (coop_rd(s->actor + COOP_ACTOR_POS, s->seen[k].pos, 12)) s->seen[k].ns = plat_now_ns();
                else s->seen[k].ns = 0;
            } else if (s->state == LIVE && s->source == SRC_NET && s->have_want && coop_puppet_mode()) {
                // Puppet mode: it gets no input, so its body is moved to its owner, but only once it is 10 cm off:
                // a teleport every tick makes it lose and find the ground on alternate frames, and the foot
                // placement then drops its hips (the jitter seen on stairs, 2026-11-05).
                float om[12];
                owner_matrix(s, om);
                const float *bp = s->have_body ? s->body_pos : s->phys_pos;
                float dx = bp[0] - om[3], dy = bp[1] - om[7], dz = bp[2] - om[11];

                static float lim = -1;   // TOTK_COOP_PUPPET_SNAP_M: that distance in m (default 0.1; 0 every tick)
                if (lim < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_SNAP_M"); lim = e ? (float)atof(e) : 0.1f; }
                // ... or once it faces another way: the step writes the body's rotation before placement goes over
                // it, and a body left turned 12 degrees away flickered between the two (a play session, 2026-11-05).
                float rd = 0;
                for (int r = 0; s->have_body && r < 3; r++)
                    for (int k = 0; k < 3; k++) { float d = fabsf(s->body_rot[r * 3 + k] - om[r * 4 + k]); if (d > rd) rd = d; }
                static int said_body;
                if (s->have_body && said_body++ % 300 == 0)
                    rt_log("coop: puppet: slot %d body %.2f %.2f %.2f, owner %.2f %.2f %.2f, rotation off %.3f", i, bp[0], bp[1], bp[2],
                           om[3], om[7], om[11], rd);
                if ((!s->have_phys && !s->have_body) || dx * dx + dy * dy + dz * dz > lim * lim || rd > 0.02f) snap(c, s), s->since_snap = 0;
                else s->since_snap++;
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
