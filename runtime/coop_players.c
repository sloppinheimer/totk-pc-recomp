// Co-op players: up to COOP_MAX_PLAYERS - 1 other players' Links in this PC's world, each shown by a stand-in, a
// PlayerPuppet actor (docs/coop/proxy-plan.md). A slot is a remote player over the relay (coop_net.c): its stand-in
// appears when the player's first state arrives and is let go when they leave. It plays its owner's animation and
// pose, is put where its owner is (owner_smoothed) and holds its owner's gear. While its update runs (actor calc,
// wrapped through the entry hook), the controller manager hands out an empty controller as controller 0, so nothing
// in it reads this PC's pad.

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_chars.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_mounts.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"

enum { FREE, WANTED, REQUESTED, LIVE, LET_GO };
enum { SRC_NET = 1 };
typedef struct {
    int state, source, net_id;
    int leaving;                       // its player left while its Player was being made: deleted once it is there
    _Alignas(16) uint8_t handle[0x40], link[0x40];
    uint64_t actor, ref;
    _Alignas(16) uint8_t ctl[0x400];   // the slot's controller (a copy of controller 0 each frame, then its input)
    uint32_t prev;
    float held[32];
    float want_pos[3], want_rot[9];    // net: the owner's reported transform
    float vel[3];                      // the owner's velocity from the last two reports (puppet mode's prediction)
    uint32_t last_seq;
    uint64_t last_ns;                  // when that report was sent, as plat_now_ns
    struct { uint64_t ns; float pos[3]; } seen[32];   // puppet mode: where the puppet was, for the error log
    struct { uint64_t ns; int64_t w_us; float pos[3], q[4]; } hist[16];   // puppet mode: the owner's last reports
                                       // (arrival time, and the owner's world clock then, -1 none)
    unsigned nhist;                    // under lock
    int primed;                        // a PlayerPuppet stand-in's empty weapon, bow and shield slots given an item once
    float placed_pos[3];               // where the stand-in was last placed (the physics' own move is measured from it)
    int have_placed;
    int64_t woff_us;                   // this game's world clock less the owner's, the least delayed estimate
    int have_woff;
    float jit_prev[2][3], jit_sum, jit_max;   // puppet mode: frame-to-frame jerk of the placed position (log)
    int jit_n, jit_have;
    int nseen;
    float err_max, err_sum;
    int err_n;
    int have_want, idle;
    char gear_want[COOP_GEAR_SLOTS][32], gear_have[COOP_GEAR_SLOTS][32];   // the owner's gear; what this Link was given
    char character[COOP_CHAR_KEY];     // the owner's character (NetState.character; "" Link) and the stand-in actor
    char standin[48];                  // spawned for it (coop_chars.c: that character's set, else PlayerPuppet)
    int recharacter;                   // its stand-in is being deleted to be made again as another actor
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
        uint8_t blowing;               // set off here (its life taken): runs its own update to do so
        uint8_t launched;              // a beam's copy (beam_copy): 0 new, 1 its start known, 2 flying on its own
        float b0[3], bv[3];            // ... its start, and its speed
        float lpos[3], lrot[9];        // with COOP_OBJ_LOCAL: its place in the owner's bow's frame
        float pos[3], rot[9];
        _Alignas(16) uint8_t handle[0x40], link[0x40];
        uint64_t actor, ref;
        int ticks;
    } mir[COOP_OBJS];
    _Alignas(16) uint8_t att_str[3][0x60];     // the material names handed to the weapon creates
    int shield_placed;                 // the owner's shield is off its back and out of hand (a shield surf):
    uint64_t shield_actor;             // the proxy's shield while shield_place puts it there (its own update skipped)
    float shield_local[3], shield_rot[9];   // where, in the owner's frame (NetState flags bit 11)
    uint8_t hold_bone[3];              // weapon, bow, shield in its owner's hand: the bone, and the item's matrix in
    float hold[3][12];                 // that bone's frame (NetState.hold_*; hold_place)
    int8_t dye_want[COOP_GEAR_SLOTS], dye_have[COOP_GEAR_SLOTS];   // the armor's dye colour, the same way (-1 none)
    int out_want, out_have;            // puppet mode: bits 0..2 weapon, bow, shield in the owner's hands; as last made
    int attack_want;                   // the owner's weapon attack is on (NetState flags bit 13; the stand-in's)
    float phys_pos[3];                 // puppet mode: where its physics step put it (before placement), for snap
    int since_snap;                    // ticks since its body was last teleported
    float body_pos[3], body_rot[9];    // its physics body's position and rotation (mLastTransform)
    int have_body;
    float top_last;                    // its highest body bone last frame (jitter watch)
    int have_phys;
    int live_ticks;                    // ticks since it went LIVE
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
// x, y: 0..1 of the screen (the overlay's guess, TOTK_COOP_TAG_FOV); world: the anchor (vk_tags.c projects it with the
// frame's own camera); id: 0 the self test
typedef struct { float x, y, dist, world[3]; int id; char name[17]; } CoopTag;
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
        int id = 0;
        if (i == COOP_MAX_PLAYERS) {
            if (!self_test) break;
            a = self_test, nm = "(you)";
        } else {
            Slot *s = &slots[i];
            if (s->state != LIVE || s->source != SRC_NET || !s->actor) continue;
            a = s->actor, id = s->net_id;
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
        t->x = 0.5f + 0.5f * x / (z * tx), t->y = 0.5f - 0.5f * y / (z * ty), t->dist = dist, t->id = id;
        t->world[0] = p[0], t->world[1] = p[1], t->world[2] = p[2];
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
#define EQ_ATTACKING 0x660       // EquipmentUser byte: 1 while the weapon's attack is on (a swing; TOTK_COOP_TEST_EQDIFF)
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
#define TIME_FOLLOW_S 60.0f        // game seconds off the host before this game is set to its time (60 a real second)

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

// Set again every 2 s while the weather here is not the host's: set once, it went back to this game's forecast (it
// read 0, 3, 1, 0 and 2 while the host's stayed 7, a play session, 2026-11-06).
static void follow_host_weather(Cpu *c, int32_t host) {   // host -1: no host (alone, or it left)
    uint64_t wx = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + WX_SLOT));
    static uint64_t last_ms;
    uint64_t now_ms = plat_now_ns() / 1000000;
    int again = host >= 0 && host == weather_forced && weather_now >= 0 && weather_now != host && now_ms - last_ms > 2000;
    if (!wx || (host == weather_forced && !again)) return;
    last_ms = now_ms;
    if (host < 0) {
        rt_log("coop: weather back to this game's forecast");
        coop_guest_call(c, MOD_MAIN_BASE + WX_RELEASE, wx, 0, 0, 0);
    } else {
        static int said;
        if (said++ < 40) rt_log("coop: weather %d, the host's %d: set to the host's%s", weather_now, host, again ? " (again)" : "");
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
    static uint64_t last_ns, mine_at;
    static float last_mine = -1;
    float mine = day_seconds();
    if (mine < 0 || host < 0) return;
    // Not while this game's clock stands (a fast travel's load, a menu): set every second all through a load, the
    // load hung, and the game came back only once it stopped (a play session, 2026-11-06: "this Link is paused"
    // and "set to the host's" every second). The clock has to have moved since the last look, 0.5 s or more ago.
    uint64_t t = plat_now_ns();
    if (t - mine_at < 500000000ull) return;
    int clock_runs = last_mine >= 0 && mine != last_mine;
    last_mine = mine, mine_at = t;
    if (!clock_runs) return;
    // ... nor while the host's stands (its map or a menu open): this game's clock ran a minute ahead each second and
    // was set back, and the sky, light and weather jumped with it, the frame rate dropping at each (a play session,
    // 2026-11-08: the map open in both games, then closed in one). Its time unchanged for 2 s: it stands. (0.9 s was
    // too short: in the Depths at a low frame rate the clock moved in steps about a second apart, the host's clock read
    // as standing most of the time, and this game fell 5 game minutes behind it; a play session, 2026-11-08.)
    static float last_host = -1;
    static uint64_t host_at;
    if (host != last_host) last_host = host, host_at = t;
    if (t - host_at > 2000000000ull) return;
    float d = host - mine;
    if (d > 43200.0f) d -= 86400.0f;
    if (d < -43200.0f) d += 86400.0f;
    if (!time_following) time_following = 1, own_offset = -d;
    uint64_t now = plat_now_ns();
    // (checked once a second: every 5 s, with the host's clock stopped in a menu, this game ran up to 3 game minutes
    // ahead and jumped back; the sync audit, fuse test, 2026-11-06)
    if (fabsf(d) < TIME_FOLLOW_S || now - last_ns < 1000000000ull) return;
    last_ns = now;
    static int said;
    if (said++ < 40) rt_log("coop: time of day %.0f s, the host's %.0f s: set to the host's", mine, host);
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
    // (The cached owner is kept: checked again whenever the EquipmentUser still pointed to it, as a code review asked,
    // a co-op Link's one that did not for a moment was looked for again and found this PC's Link in it first (its
    // words hold a pointer to it too), and this PC's gear was read from the co-op Link's: each game took the other
    // player's weapon and fused material for its own; a play session, 2026-11-06. A gone EquipmentUser is dropped
    // where its Player goes, coop_players_tick.)
    for (int i = 0; i < 32; i++) if (eq_owner[i].eq == eq) return eq_owner[i].actor;
    uint64_t words[0x400 / 8];
    if (!coop_rd(eq, words, sizeof words)) return 0;
    // A co-op Link's before this PC's: a co-op Link's EquipmentUser holds a pointer to this PC's Link as well.
    uint64_t found = 0, found_me = 0;
    for (int w = 0; w < 0x400 / 8 && !found; w++) {
        if (words[w] == me) found_me = me;
        for (int i = 0; i < COOP_MAX_PLAYERS && !found; i++)
            if (slots[i].actor && words[w] == slots[i].actor) found = slots[i].actor;
    }
    if (!found) found = found_me;
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
            memset(s->vel, 0, sizeof s->vel), s->last_seq = 0, s->last_ns = 0, s->nseen = 0, s->err_n = 0, s->primed = 0;
            s->nhist = 0, s->jit_n = 0, s->jit_have = 0, s->have_woff = 0;
            memset(s->held, 0, sizeof s->held);
            memset(s->gear_want, 0, sizeof s->gear_want), memset(s->gear_have, 0, sizeof s->gear_have);
            memset(s->fuse_want, 0, sizeof s->fuse_want), memset(s->fuse_have, 0, sizeof s->fuse_have);
            memset(s->objs_want, 0, sizeof s->objs_want), memset(s->mir, 0, sizeof s->mir);
            memset(s->dye_want, -1, sizeof s->dye_want), memset(s->dye_have, -1, sizeof s->dye_have);
            s->out_want = 0, s->out_have = -1, s->have_phys = 0, s->top_last = -1e9f, s->have_body = 0, s->shield_actor = 0, s->shield_placed = 0;
            s->live_ticks = 0;
            s->character[0] = 0, s->standin[0] = 0;
            s->leaving = 0, s->ride_owner = 0;   // (left from the slot's last player, code review)
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
int coop_player_positions_ids(float (*out)[3], int *ids, int max) {
    int n = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS && n < max; i++)
        if (slots[i].state == LIVE && slots[i].source == SRC_NET && slots[i].have_want) {
            if (ids) ids[n] = slots[i].net_id;
            memcpy(out[n++], slots[i].want_pos, 12);
        }
    return n;
}
// The remote players' places as their owners last reported them (position and rotation), their ids and actors. A
// PlayerPuppet stand-in's own actor position (+0x2b4) is not where it is drawn: its body is moved, the actor field
// stays at the place it was made (the stand-in foe test, 2026-11-07), so tests take the place from here.
int coop_player_places(float (*pos)[3], float (*rot)[9], int *ids, uint64_t *actors, int max) {
    int n = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS && n < max; i++)
        if (slots[i].state == LIVE && slots[i].source == SRC_NET && slots[i].have_want) {
            memcpy(pos[n], slots[i].want_pos, 12), memcpy(rot[n], slots[i].want_rot, 36);
            ids[n] = slots[i].net_id, actors[n] = slots[i].actor, n++;
        }
    return n;
}
int coop_player_positions(float (*out)[3], int max) { return coop_player_positions_ids(out, NULL, max); }
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
// The model push: main+0x24cc30 (model) makes the body bones' world matrices from the pose and the model's world
// matrix (+0x1f8, with its dirty bit) and puts the parts hung on body bones (items) on them.
#define PROXY_MODEL_PUSH 0x24cc30
static void owner_matrix(Slot *s, float m[12]);
static float body_top(uint64_t actor);
static float body_top_root(uint64_t actor, float *root_y);
static void top_log(const char *who, uint64_t actor);
uint64_t coop_puppet_pose_obj(uint64_t skel);   // coop_world.c
static uint64_t body_skeleton(uint64_t actor) {
    static const uint32_t path[8] = {0x48, 0x2a8, 0x20, 0x20, 0, 0xf8, 0x58, 0xb0};
    uint64_t sk = actor;
    for (int k = 0; k < 8 && sk; k++) sk = coop_rd_ptr(sk + path[k]);
    // A PlayerPuppet's: [[[actor+0x238]+0x3a8]+0x3b0] (TOTK_COOP_TEST_SKELFIND, 2026-11-08: 127 bones, as a Link's)
    if (!sk && actor) {
        uint64_t a = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x238) + 0x3a8) + 0x3b0);
        uint16_t nb = 0;
        if (a && coop_rd(a + 0x48, &nb, 2) && nb == 127) sk = a;
    }
    return sk;
}
// Test (TOTK_COOP_TEST_SKELFIND=1): once, for the first stand-in, the pointer paths from its actor (3 levels, 0x400
// bytes each) to an object whose +0x48 u16 is 127 (a Link's body bone count) and whose +0x20 points somewhere: where a
// PlayerPuppet's body skeleton is (body_skeleton's path is a Player's and gives none on it).
static void skel_find(uint64_t actor) {
    static int done;
    if (done || !cfg_get("TOTK_COOP_TEST_SKELFIND") || !actor) return;
    done = 1;
    static uint64_t l0[0x80], l1[0x80], l2[0x80];
    int found = 0;
    if (!coop_rd(actor, l0, sizeof l0)) return;
    for (int a = 0; a < 0x80 && found < 8; a++) {
        if (l0[a] < 0x100000000ull || l0[a] > 0x800000000ull || !coop_rd(l0[a], l1, sizeof l1)) continue;
        for (int b = 0; b < 0x80 && found < 8; b++) {
            if (l1[b] < 0x100000000ull || l1[b] > 0x800000000ull || !coop_rd(l1[b], l2, sizeof l2)) continue;
            for (int d = 0; d < 0x80 && found < 8; d++) {
                uint64_t o = l2[d];
                uint16_t nb = 0;
                if (o < 0x100000000ull || o > 0x800000000ull || !coop_rd(o + 0x48, &nb, 2) || nb != 127 || !coop_rd_ptr(o + 0x20)) continue;
                rt_log("coop: test: skeleton of the stand-in at actor +%#x +%#x +%#x (%llx)", a * 8, b * 8, d * 8, (unsigned long long)o);
                found++;
            }
        }
    }
    if (!found) rt_log("coop: test: no 127-bone skeleton within 3 levels of the stand-in");
}
static uint64_t model_of(uint64_t a);
static int ride_place(const Slot *s, float out[3]);   // the owner rides an object: where, here
static void shield_place(Cpu *c, Slot *s);
static void hold_place(Cpu *c, Slot *s);
static void hold_send(uint64_t me, uint64_t eq, NetState *st);
static uint64_t eq_item(uint64_t eq, int kind);
static void watch_old_item(uint64_t eq, int k, uint64_t old, int remade);
static int item_local(uint64_t link, uint64_t item, float local[3]);
static void beam_dump(uint64_t a, int copy);
static void beam_log(uint64_t a, uint32_t id, const char *who);
static void beam_end_watch(const float at[3]);
static void place_copy(Cpu *c, Slot *s, int k);
static void beam_copy(Cpu *c, Slot *s, int k, float m[12]);
static void set_driven(uint64_t a);
int coop_bonds_claimed(uint64_t actor);   // coop_bonds.c: a copy of a contraption this game runs now
#define COPY_WARMUP 5   // ticks a copy runs its own update after it is made (arrow_copies)
// Where a Link's bow is drawn: its model's world matrix (model +0x1f8, 3x4 rows), which the model push puts on the
// hand bone each frame. A nocked arrow is sent and shown in this frame (COOP_OBJ_LOCAL). 1 when it has a bow.
static int bow_frame(uint64_t link, float rot[9], float pos[3]) {
    uint64_t bow = link ? eq_item(eq_of_actor(link), 1) : 0;
    uint64_t model = bow ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(bow + 0x228) + 0x8) + 0x28) : 0;
    float w[12];
    if (!model || !coop_rd(model + 0x1f8, w, sizeof w)) return 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) rot[i * 3 + j] = w[i * 4 + j];
        pos[i] = w[i * 4 + 3];
    }
    return 1;
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
    // TOTK_COOP_TEST_FREEZE_PHASES=0: the freeze stops the calc only; these phases (model, physics) still run
    static int keep_phases = -1;
    if (keep_phases < 0) { const char *e = cfg_get("TOTK_COOP_TEST_FREEZE_PHASES"); keep_phases = e && *e == '0'; }
    if (frozen() && !keep_phases) return 1;
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 4) rt_log("coop: a co-op Link's update phase main+%#llx counts as its calc", (unsigned long long)(addr - MOD_MAIN_BASE));
    uint64_t phase_actor = c->x[0];   // (x0 is the call's result after it)
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    // A PlayerPuppet stand-in, after its model phase (the push that puts items on their bones): its shield where its
    // owner's is in a shield surf, and the copies that ride on it (a nocked arrow on its bow, its paraglider), as
    // the stand-in's owner shows them.
    Slot *ps = slot_of_actor(phase_actor);
    static int pp_on = -1;   // TOTK_COOP_STANDIN_PHASEPLACE=0: none of it (a test, the sword staying on the back)
    if (pp_on < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_PHASEPLACE"); pp_on = !(e && *e == '0'); }
    if (pp_on && ps && addr == MOD_MAIN_BASE + 0x245030 && ps->state == LIVE && ps->source == SRC_NET) {
        // its owner's final pose (TOTK_COOP_STANDIN_POSE=0: only the clips it plays), then the push that makes its
        // bones' world matrices from it and puts its items on them
        static int pose_on = -1;
        if (pose_on < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_POSE"); pose_on = !(e && *e == '0'); }
        int coop_standin_put_pose(uint64_t skel, int id);   // coop_world.c
        uint64_t psk = pose_on ? body_skeleton(ps->actor) : 0, pmodel = model_of(ps->actor);
        if (psk && pmodel && coop_standin_put_pose(psk, ps->net_id)) coop_guest_call(c, MOD_MAIN_BASE + PROXY_MODEL_PUSH, pmodel, 0, 0, 0);
        hold_place(c, ps);
        shield_place(c, ps);
        static int said_sh, said_in;
        if (!said_in++) rt_log("coop: stand-in: slot %d: placed after its model phase (shield placed %d)", (int)(ps - slots), ps->shield_placed);
        if (ps->shield_placed && said_sh++ < 5)
            rt_log("coop: stand-in: slot %d: its shield %llx put where its owner's is (%.2f %.2f %.2f in its frame)", (int)(ps - slots),
                   (unsigned long long)ps->shield_actor, ps->shield_local[0], ps->shield_local[1], ps->shield_local[2]);
        for (int k = 0; k < COOP_OBJS; k++)
            if (ps->mir[k].state == 3 && ps->mir[k].ticks >= COPY_WARMUP && (ps->mir[k].flags & (COOP_OBJ_LOCAL | COOP_OBJ_ONLINK))) {
                static int said_pc;
                if (said_pc++ < 20) rt_log("coop: stand-in: slot %d: copy %s placed on it after its model phase", (int)(ps - slots), ps->mir[k].name);
                place_copy(c, ps, k);
            }
    }
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
    uint64_t sk = body_skeleton(actor);
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
    if (!s || s->state != LIVE || s->source != SRC_NET || !s->have_want) return 0;
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
    {   // its owner rides an object: on this game's one of it
        float rp[3];
        if (ride_place(s, rp)) {
            m[3] = rp[0], m[7] = rp[1], m[11] = rp[2];
            static int said_r;
            if (!said_r++) rt_log("coop: stand-in: slot %d rides its owner's object here", (int)(s - slots));
        }
    }
    {   // how far the physics step's transform is from the owner's (what placement covers), logged every 60 frames
        float pp[3], pr[9];
        static __thread float dp_max, dr_max;   // (per thread: the puppets are placed on several, code review)
        static __thread int dn;
        if (coop_rd(actor + COOP_ACTOR_POS, pp, 12) && coop_rd(actor + COOP_ACTOR_ROT, pr, 36)) {
            memcpy(s->phys_pos, pp, 12), s->have_phys = 1;
            for (int k = 0; k < 3; k++) { float d = fabsf(pp[k] - m[k * 4 + 3]); if (d > dp_max) dp_max = d; }
            for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) { float d = fabsf(pr[r * 3 + k] - m[r * 4 + k]); if (d > dr_max) dr_max = d; }
            // and how far the physics moved it since it was last placed (its own drag, apart from its owner's motion),
            // vertical and level
            static __thread float pv_sum, ph_sum, pv_max, ph_max;
            static __thread int pn;
            if (s->have_placed) {
                float dv = pp[1] - s->placed_pos[1], dx = pp[0] - s->placed_pos[0], dz = pp[2] - s->placed_pos[2], dh = sqrtf(dx * dx + dz * dz);
                pv_sum += fabsf(dv), ph_sum += dh, pn++;
                if (fabsf(dv) > pv_max) pv_max = fabsf(dv);
                if (dh > ph_max) ph_max = dh;
            }
            if (++dn % 60 == 0) {
                rt_log("coop: puppet: physics vs owner before placement: position %.1f mm, rotation %.4f (max over 60 frames); "
                       "moved by its physics since placed: vertical mean %.1f max %.1f mm, level mean %.1f max %.1f mm",
                       dp_max * 1000, dr_max, pn ? pv_sum / pn * 1000 : 0, pv_max * 1000, pn ? ph_sum / pn * 1000 : 0, ph_max * 1000);
                dp_max = dr_max = 0, pv_sum = ph_sum = pv_max = ph_max = 0, pn = 0;
            }
        }
    }
    for (int r = 0; r < 3; r++) {
        pos[r] = m[r * 4 + 3];
        for (int k = 0; k < 3; k++) rot[r * 3 + k] = m[r * 4 + k];
    }
    memcpy(s->placed_pos, pos, 12), s->have_placed = 1;
    skel_find(actor);
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
        uint64_t sk = body_skeleton(actor);
        uint16_t nb = 0;
        uint64_t wb = sk ? coop_rd_ptr(sk + 0x20) : 0;
        static __thread float rec[127][16];   // (per thread: two puppets' placing overwrote each other's, code review)
        static __thread int jumps, frames;
        if (wb && coop_rd(sk + 0x48, &nb, 2) && nb == 127 && coop_rd(wb, rec, sizeof rec)) {
            float top = -1e9f;
            for (int b = 0; b < 127; b++) if (rec[b][13] > top) top = rec[b][13];
            frames++;
            if (s->top_last > -1e8f && fabsf(top - s->top_last) > 0.03f && ++jumps <= 200)
                rt_log("coop: puppet: slot %d body top moved %+.1f cm in a frame at %.3f s (body teleported %d ticks ago)",
                       (int)(s - slots), (top - s->top_last) * 100, frame_step_world_us() / 1e6, s->since_snap);
            if (frames % 1800 == 0) rt_log("coop: puppet: slot %d: %d body jumps over 3 cm in %d frames", (int)(s - slots), jumps, frames);
            s->top_last = top;
            // Whether the drawn body follows the placement: the root bone's record (its own origin, metres off) less
            // the placed position, frame to frame. Steady: the mesh is where it is placed; changing: it is drawn from
            // somewhere else (its physics), the stutter. Logged every 60 frames: mean and largest change.
            static __thread float off_last[3], ch_sum, ch_max;
            static __thread int off_have, ch_n;
            float off[3] = {rec[0][12] - pos[0], rec[0][13] - pos[1], rec[0][14] - pos[2]};
            if (off_have) {
                float dx = off[0] - off_last[0], dy = off[1] - off_last[1], dz = off[2] - off_last[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d < 5) { ch_sum += d, ch_n++; if (d > ch_max) ch_max = d; }   // (a teleport of its own origin: not counted)
            }
            memcpy(off_last, off, 12), off_have = 1;
            if (ch_n >= 60) {
                rt_log("coop: puppet: slot %d drawn root against placed: change per frame mean %.1f mm, max %.1f mm", (int)(s - slots),
                       ch_sum / ch_n * 1000, ch_max * 1000);
                ch_sum = ch_max = 0, ch_n = 0;
            }
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
int coop_players_any(void) {   // a co-op Link in the world
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) if (slots[i].actor && (slots[i].state == LIVE || slots[i].state == LET_GO)) return 1;
    return 0;
}
int coop_players_calc(Cpu *c, uint64_t addr) {
    Slot *s;
    if (!inside && (copy_place(c, c->x[0], addr, 1) || surf_shield(c->x[0]))) return 1;
    if (inside || !(s = slot_of_actor(c->x[0]))) return 0;
    {
        uint64_t me = coop_player_actor();
        if (me) keep_player_eq(me, eq_of_actor(me));
    }
    if (frozen()) {
        static int said;
        if (!said++) rt_log("coop: test: the co-op Links' updates no longer run");
        return 1;
    }
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_INPUT_MGR_SLOT));
    if (!mgr) return 0;
    // One update with a swapped controller at a time, and not while this PC's Link's runs (coop_spawn.c, calc_wrap).
    void coop_ctl_enter(void), coop_ctl_leave(void);   // coop_spawn.c
    coop_ctl_enter();
    uint64_t real = LD64(mgr + 0xa0), own = (uint64_t)(uintptr_t)s->ctl;
    if (!real) { coop_ctl_leave(); return 0; }   // no controller yet (early in a load): copying from it read address 0 (code review, 2026-11-06)
    memcpy(s->ctl, (void *)(uintptr_t)real, sizeof s->ctl);
    // The stand-in gets no input: an empty controller in place of this PC's while its update runs, so nothing in it
    // reads this PC's pad.
    uint64_t buttons = 0;
    float stick[4] = {0, 0, 0, 0};
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
    coop_ctl_leave();
    return 1;
}

// The other player's stand-in is TotK Online's kind (read for behaviour only), an actor of its own, PlayerPuppet
// (tools/coop/make_puppet_pack.py: Link's model, animations, equipment user and body, no Player component, so nothing
// of its own moves it or its gear).
static void spawn_slot(Cpu *c, Slot *s) {
    uint64_t me = coop_player_actor();
    float pos[3], rot[9];
    if (!me || !coop_rd(me + COOP_ACTOR_POS, pos, 12) || !coop_rd(me + COOP_ACTOR_ROT, rot, 36)) return;
    if (s->have_want) memcpy(pos, s->want_pos, 12), memcpy(rot, s->want_rot, 36);
    else pos[0] += 2 * rot[2], pos[2] += 2 * rot[8];   // 2 m in front of this PC's Link
    pos[1] += 0.5f;
    // The owner's character's stand-in (coop_chars.c): its set's PlayerPuppet_<tag>, else the plain PlayerPuppet.
    const char *player = coop_char_actor(s->character);
    snprintf(s->standin, sizeof s->standin, "%s", player);
    s->recharacter = 0;
    // A new stand-in has none of the gear its slot gave the last one (made again after a load or as another character):
    // the gear code below gives it its owner's again.
    memset(s->gear_have, 0, sizeof s->gear_have), memset(s->fuse_have, 0, sizeof s->fuse_have);
    memset(s->dye_have, -1, sizeof s->dye_have);
    if (coop_request_create(c, player, pos, rot, s->handle)) {
        s->state = REQUESTED;
        rt_log("coop: slot %d: %s requested at %.1f %.1f %.1f", (int)(s - slots), player, pos[0], pos[1], pos[2]);
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
// for at most 0.1 s. Reports arrive about 60 times a second (coop_net.c) and frames do not line up with them, so taking the latest
// one each frame moves the puppet in uneven steps (the jitter).
static int puppet_delay_ms(void) {
    static int d = -1;
    if (d < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_DELAY_MS"); d = e ? atoi(e) : 60; }
    return d;
}
static int owner_smoothed(Slot *s, float m[12]) {
    if (puppet_delay_ms() <= 0) return 0;
    float pos[3], q[4];
    int ok = 0;
    plat_mutex_lock(&lock);
    unsigned n = s->nhist < 16 ? s->nhist : 16;
    // On the owner's world clock when every kept report has it: this game's frames step its world clock evenly, so
    // the stand-in moves with the frame as everything else here does; by arrival time an uneven frame moved it by
    // that frame's own length, a jolt (game 1's stand-in: 20 to 90 mm off a straight line each frame, the tester's
    // session, 2026-11-07). TOTK_COOP_PUPPET_WORLDTIME=0: arrival time, as before.
    static int wt = -1;
    if (wt < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_WORLDTIME"); wt = !(e && *e == '0'); }
    int64_t lw = frame_step_world_us();
    int use_w = wt && s->have_woff && lw >= 0 && n;
    for (unsigned i = 0; use_w && i < n; i++) use_w = s->hist[(s->nhist - 1 - i) % 16].w_us >= 0;
    #define HT(x) (use_w ? s->hist[x].w_us * 1000 : (int64_t)s->hist[x].ns)
    int64_t t = use_w ? (lw - s->woff_us) * 1000 - (int64_t)puppet_delay_ms() * 1000000
                      : (int64_t)(plat_now_ns() - (uint64_t)puppet_delay_ms() * 1000000ull);
    if (n) {
        unsigned newest = (s->nhist - 1) % 16;
        if (t >= HT(newest)) {   // past the newest: carry on at its velocity, briefly
            double a = (double)(t - HT(newest)) / 1e9;
            if (a > 0.1) a = 0.1;
            for (int k = 0; k < 3; k++) pos[k] = s->hist[newest].pos[k] + s->vel[k] * (float)a;
            memcpy(q, s->hist[newest].q, sizeof q);
            ok = 1;
        } else {
            for (unsigned i = 1; i < n && !ok; i++) {   // newest pair first
                unsigned b = (s->nhist - i) % 16, a = (s->nhist - i - 1) % 16;
                if (HT(a) > t) continue;
                float f = HT(b) > HT(a) ? (float)((double)(t - HT(a)) / (double)(HT(b) - HT(a))) : 1;
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
    #undef HT
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
static void owner_matrix_raw(Slot *s, float m[12]);
// Where the owner's Link is here. A rider of a horse (ride_owner COOP_RIDE_MOUNT) at its place on this game's copy
// of that horse (coop_mounts.c coop_mount_place), which the native model eases after the owner's horse, so the
// stand-in, co-op Link or PlayerPuppet, sits on that copy and not at its owner's delayed place beside it.
static void owner_matrix(Slot *s, float m[12]) {
    owner_matrix_raw(s, m);
    float rp[3];
    if (s->ride_owner == COOP_RIDE_MOUNT && coop_mount_place(s->ride_id, s->ride_off, rp)) m[3] = rp[0], m[7] = rp[1], m[11] = rp[2];
}
// The remote player whose state says they ride the horse of that key (coop_mounts.c), 0 none.
int coop_player_riding_mount(uint32_t key) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state == LIVE && slots[i].source == SRC_NET && slots[i].ride_owner == COOP_RIDE_MOUNT && slots[i].ride_id == key)
            return slots[i].net_id;
    return 0;
}
static void owner_matrix_raw(Slot *s, float m[12]) {
    if (owner_smoothed(s, m)) return;
    float ahead = 0;
    static int predict = -1;   // TOTK_COOP_PUPPET_PREDICT=0: no prediction (for comparing)
    if (predict < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_PREDICT"); predict = !e || *e != '0'; }
    if (predict && s->last_ns) {
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
// TOTK_COOP_AUTOBUILT=1: a copy of an owner's autobuilt device is made as an autobuild makes it
// (coop_request_create_autobuilt), so it carries the label too. Off by default: the label alone neither tints the copy
// green nor joins the copies into one contraption (docs/coop/re-notes.md, "Autobuild copies"), and what else in the
// game treats labelled devices differently is not known.
static int autobuilt_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_AUTOBUILT"); on = e && *e == '1'; }
    return on;
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
// shot: has left the bow; enemy: shot by an enemy this game runs (sent as it flies, in the world)
static struct { uint64_t actor; uint32_t id; int mine, seen, fused_to, shot, gone, enemy; char name[48]; } my_arrows[32];
static struct { uint64_t actor; int seen; } near_actors[64];   // other actors within 3 m of this Link, last tick seen
static uint32_t next_arrow_id = 1;
static int arrow_tick;
// Last tick ZR (npad bit 9) was held here. A bow is drawn with ZR: the first arrow is made on the press, and the next
// one goes on the bow 46 ticks after the release (bow_shoot_3rd.txt, 2026-11-05). An arrow made more than 3 s after
// ZR is not this Link's: each game made two arrows 0.45 m from each Link at load, and one with no other Link in yet
// took them as its own.
static int zr_tick = -1000;
// The fused materials set off here (arrow_copies), for 10 s: owner net id << 32 | number, and until when (ms). (Not
// for good: a game started again numbers its objects from 1 again.)
static struct { uint64_t key, until_ms; } blown[16];
static int blown_n;
static int copies_live[COOP_MAX_PLAYERS];   // per slot, its copies in state 3 (arrow_copies): copy_of runs for every actor
// A copy's actor is still the one made: alive (state 2..6) and of the copy's name. The game can free a copy's actor
// (a paraglider's went away by itself) and give its memory to another; one not checked was still placed, woken and
// deleted (code review, 2026-11-06).
static int actor_is(uint64_t a, const char *name) {   // alive, and of that name (not another actor in its memory)
    int32_t st = 0;
    char nm[48];
    return a && coop_rd(a + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6 && !strcmp(coop_actor_name(a, nm, sizeof nm), name);
}
static int copy_alive(const Slot *s, int k) { return actor_is(s->mir[k].actor, s->mir[k].name); }
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
            // (a stale entry is not this actor: a new BombFruit got the address of one that had gone 2 s before and
            // was not taken for the next arrow's material, bow_fuse_3rd.txt)
            if (near_actors[i].actor == a && arrow_tick - near_actors[i].seen <= 30) { near_actors[i].seen = arrow_tick; return; }
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
                my_arrows[i].actor = a, my_arrows[i].mine = 1, my_arrows[i].seen = arrow_tick, my_arrows[i].fused_to = by, my_arrows[i].shot = 0, my_arrows[i].gone = 0;
                my_arrows[i].id = next_arrow_id++;
                snprintf(my_arrows[i].name, sizeof my_arrows[i].name, "%s", nm);
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
            my_arrows[i].fused_to = 0, my_arrows[i].shot = 0, my_arrows[i].gone = 0, my_arrows[i].enemy = 0;
            // An arrow made within 3 m of an enemy this game runs, at least 1 m from every Link (a Link's quiver
            // arrows are made 0.45 m from it at load): that enemy's, sent so the other games see it (they saw none:
            // a play session, 2026-11-07).
            int coop_near_my_enemy(const float p[3], float m);   // coop_world.c
            if (!my_arrows[i].mine && arrow_tick >= 30 && d2 >= 1 && dco >= 1 && coop_near_my_enemy(p, 3))
                my_arrows[i].mine = 1, my_arrows[i].enemy = 1, my_arrows[i].shot = 1;
            my_arrows[i].id = my_arrows[i].mine ? next_arrow_id++ : 0;
            snprintf(my_arrows[i].name, sizeof my_arrows[i].name, "%s", nm);
            if (my_arrows[i].mine) rt_log("coop: my arrow %u is %s (%llx)", my_arrows[i].id, nm, (unsigned long long)a);
            return;
        }
}
static void mat_quat(const float *r, float q[4]);
static void quat_mat(const float q[4], float *r);
static int this_paused;   // (set by the tick; defined below)
static int report_arrows(Cpu *c, uint64_t me, CoopObj *out) {
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    arrow_tick++;
    if (local_buttons >> 9 & 1) zr_tick = arrow_tick;
    coop_actors_live(arrow_track_fn, &me);
    int n = 0;
    memset(out, 0, sizeof(CoopObj) * COOP_OBJS);
    // Whether the bow is in hand: an arrow near this Link is sent only while it is. After the bow was put away the
    // nocked arrow's copy stayed at the proxy's hand for a while (a play session, 2026-11-05): the arrow is still
    // there in this game for a moment.
    // (only on this Link's own EquipmentUser, and not while the game is paused: during a save load eq_of_actor gave a
    // co-op Link's half-built one and main+0x3e322c on it crashed, load_3rd.txt; unknown counts as in hand)
    uint64_t eq = eq_of_actor(me);
    int bow_out = 1;
    if (eq && !this_paused && eq_points_to(eq, me)) bow_out = (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, 3, 0, 0) & 1);
    float mp[3] = {0};
    coop_rd(me + COOP_ACTOR_POS, mp, 12);
    for (int i = 0; i < 32; i++) {
        if (!my_arrows[i].actor) continue;
        if (arrow_tick - my_arrows[i].seen > 30) { my_arrows[i].actor = 0; continue; }   // gone (not updated for 1 s)
        if (my_arrows[i].seen != arrow_tick) continue;
        char anm[48];   // another actor in its memory (below)
        if (strcmp(coop_actor_name(my_arrows[i].actor, anm, sizeof anm), my_arrows[i].name)) { my_arrows[i].actor = 0; continue; }
        // Going away (state 7..11: a fused material set off where its arrow hit): not sent, so the other games hear of
        // it at once (it stays in the live list about 0.5 s more, and the copy's blast came 1 s late, TOTK_COOP_TEST_
        // IMPACT, bow_fuse_3rd.txt). It is kept, no longer this Link's, so it is not taken again as new while it is
        // still listed; once alive again, its memory holds another actor of the same name (the game reuses a
        // BombFruit's for the next one), and the entry is forgotten so that one is taken as new. A name changed (above)
        // is another actor too. Kept as this Link's, a fused BombFruit's number was sent again after its blast, as a
        // new BombFruit 5 m off and then as the PouchPlayer at 0 0 0, and the other game set off a second blast there;
        // kept for good, the entry took every later BombFruit and the next bomb arrows were not sent (a play session,
        // 2026-11-06).
        int32_t ast = 0;
        int alive = coop_rd(my_arrows[i].actor + COOP_ACTOR_STATE, &ast, 4) && ast >= 2 && ast <= 6;
        if (!alive) { my_arrows[i].gone = 1, my_arrows[i].mine = 0; continue; }
        if (my_arrows[i].gone) { my_arrows[i].actor = 0; continue; }
        if (!my_arrows[i].mine || n >= COOP_OBJS) continue;
        CoopObj *o = &out[n];
        float r[9];
        if (!coop_rd(my_arrows[i].actor + COOP_ACTOR_POS, o->pos, 12) || !coop_rd(my_arrows[i].actor + COOP_ACTOR_ROT, r, 36)) continue;
        if (my_arrows[i].enemy) {   // an enemy's: in the world, whatever this Link's bow does
            o->id = my_arrows[i].id;
            coop_actor_name(my_arrows[i].actor, o->name, sizeof o->name);
            mat_quat(r, o->q);
            n++;
            continue;
        }
        // Nocked: within 1.5 m of the bow and never farther since it was made. Once farther it is shot for good: a
        // shot arrow lying near its owner was taken for a nocked one, sent on the bow, and followed the owner about
        // on the other screen (a play session, 2026-11-05).
        float br[9], bp[3];
        int on_bow = bow_frame(me, br, bp) && (o->pos[0] - bp[0]) * (o->pos[0] - bp[0]) + (o->pos[1] - bp[1]) * (o->pos[1] - bp[1]) +
                                                  (o->pos[2] - bp[2]) * (o->pos[2] - bp[2]) < 1.5f * 1.5f;
        if (!on_bow && bow_out) my_arrows[i].shot = 1;   // (made at the ZR press, it can come before the bow is in hand)
        if (my_arrows[i].fused_to)   // a fused material (about 1 m from its arrow) is nocked or shot as its arrow is
            for (int j = 0; j < 32; j++)
                if (my_arrows[j].actor && my_arrows[j].id == (uint32_t)my_arrows[i].fused_to) my_arrows[i].shot = my_arrows[j].shot;
        int nocked = !my_arrows[i].shot;
        if (!bow_out && nocked) continue;   // put away
        o->id = my_arrows[i].id;
        coop_actor_name(my_arrows[i].actor, o->name, sizeof o->name);
        if (my_arrows[i].fused_to) o->flags |= COOP_OBJ_FUSED;   // shown and kept out of reach as its arrow is
        // Within 2.5 m of this Link (nocked): sent in the frame of its bow's model, so the other games put it on the
        // proxy's bow as they show it. Sent as a world place, with this Link's own place in another packet, it shook
        // 0.3 m about the proxy's bow as the owner walked (bow_walk_3rd.txt); in the Link's frame it still trailed the
        // bow a little, which comes with the proxy's pose (a play session, 2026-11-05).
        float lp[3], lr[9], mp[3];
        if (nocked && coop_rd(me + COOP_ACTOR_POS, mp, 12) && bow_frame(me, lr, lp)) {
            float d[3] = {o->pos[0] - lp[0], o->pos[1] - lp[1], o->pos[2] - lp[2]};
            {
                float rl[9];
                for (int j = 0; j < 3; j++) o->pos[j] = lr[0 * 3 + j] * d[0] + lr[1 * 3 + j] * d[1] + lr[2 * 3 + j] * d[2];
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++) rl[a * 3 + b] = lr[0 * 3 + a] * r[0 * 3 + b] + lr[1 * 3 + a] * r[1 * 3 + b] + lr[2 * 3 + a] * r[2 * 3 + b];
                memcpy(r, rl, sizeof rl);
                o->flags |= COOP_OBJ_LOCAL;
            }
        }
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
        // A shot arrow's fused material that goes in its owner's game went off where the arrow hit (a bomb flower's
        // blast; the tester's list: all arrow effects): its copy here is set off the same way, its life taken and its
        // own update let run, instead of being deleted. (A nocked one goes when the bow is put away: deleted.)
        if (s->mir[k].state == 3 && s->mir[k].actor && (s->mir[k].flags & COOP_OBJ_FUSED) && !(s->mir[k].flags & COOP_OBJ_LOCAL) &&
            !s->mir[k].blowing && copy_alive(s, k)) {
            int coop_life_of(uint64_t a);
            void coop_set_life(uint64_t a, int v);
            s->mir[k].blowing = 1, s->mir[k].ticks = COPY_WARMUP;   // ticks: from now on, how long it has been going off
            blown[blown_n % 16].key = (uint64_t)s->net_id << 32 | s->mir[k].id, blown[blown_n++ % 16].until_ms = plat_now_ns() / 1000000 + 10000;
            // back where it is shown first: its place and body were kept 300 m under it (place_copy), and it goes off there
            uint64_t a = s->mir[k].actor;
            _Alignas(16) float mb[12];
            for (int r = 0; r < 3; r++) {
                for (int j = 0; j < 3; j++) mb[r * 4 + j] = s->mir[k].rot[r * 3 + j];
                mb[r * 4 + 3] = s->mir[k].pos[r];
            }
            memcpy((void *)(uintptr_t)(a + COOP_ACTOR_POS), s->mir[k].pos, 12);
            uint64_t phys = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0, body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
            if (body) coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, body, (uint64_t)(uintptr_t)mb, 0, 0);
            int life = coop_life_of(s->mir[k].actor);
            coop_set_life(s->mir[k].actor, 0);
            static int said;
            if (said++ < 20) rt_log("coop: slot %d: %.2f s: the fused %s (%u) went off in its owner's game: set off here (life %d, at %.2f %.2f %.2f; wall %llu ms)",
                                    (int)(s - slots), frame_step_world_us() / 1e6, s->mir[k].name, s->mir[k].id, life, s->mir[k].pos[0],
                                    s->mir[k].pos[1], s->mir[k].pos[2], (unsigned long long)(plat_now_ns() / 1000000));
            continue;
        }
        // (A beam's copy is deleted when the owner's beam ends. Left to fly on 0.25 s so it could burst where it hits
        // here too, it flew on through the wall: a copy has no hit of its own, TOTK_COOP_TEST_BEAMLOG, a play session,
        // 2026-11-06. The burst where the owner's beam hits is not shown here: what makes it is not found, see
        // beam_end_watch.)
        if (s->mir[k].blowing && s->mir[k].state == 3) {   // going off: left to do it, then let go (3 s)
            int32_t st = 0;
            int alive = coop_rd(s->mir[k].actor + COOP_ACTOR_STATE, &st, 4) && st >= 2 && st <= 6;
            if (++s->mir[k].ticks < COPY_WARMUP + 180 && alive) continue;
            static int said_go;
            if (said_go++ < 40)
                rt_log("coop: slot %d: the copy of %s (%u) %s %d ticks after it was set off", (int)(s - slots), s->mir[k].name, s->mir[k].id,
                       alive ? "had not gone off" : "went off", s->mir[k].ticks - COPY_WARMUP);
            if (copy_alive(s, k)) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->mir[k].actor, COOP_DELETE_REASON, 0, 0);
            set_driven(s->mir[k].actor);   // (below)
            s->mir[k].state = 0, s->mir[k].actor = 0;
            continue;
        }
        // A copy let go is not taken for this game's own object while it goes away: a fused BombFruit's copy, left
        // after its blast, was sent back as a placed BombFruit this game moved, and the other game showed a copy of it,
        // a bomb that could go off there (a play session, 2026-11-06).
        if (s->mir[k].actor && copy_alive(s, k))   // (not another actor in its memory)
            coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->mir[k].actor, COOP_DELETE_REASON, 0, 0), set_driven(s->mir[k].actor);
        if (s->mir[k].state == 2) continue;   // its create is still pending: claimed (then deleted) once it is there
        s->mir[k].state = 0, s->mir[k].actor = 0;
    }
    for (int j = 0; j < COOP_OBJS; j++) {   // new arrows, and where each one is now
        const CoopObj *o = &s->objs_want[j];
        if (!o->id || ((o->id & COOP_OBJ_KEYED) && !key_missing(o->id))) continue;   // keyed: this game's own is moved
        if (was_taken(s->net_id, o->id)) continue;   // picked up here: the owner deletes its own
        int went = 0;   // set off here already: not made again (one was, and went off a second time, see report_arrows)
        for (int b = 0; b < 16 && !went; b++)
            went = blown[b].key == ((uint64_t)s->net_id << 32 | o->id) && blown[b].until_ms > plat_now_ns() / 1000000;
        if (went) continue;
        int k = 0;
        while (k < COOP_OBJS && !(s->mir[k].state && s->mir[k].id == o->id)) k++;
        if (k == COOP_OBJS) {
            for (k = 0; k < COOP_OBJS && s->mir[k].state; k++) {}
            if (k == COOP_OBJS) continue;
            s->mir[k].id = o->id, s->mir[k].state = 1, s->mir[k].ticks = 0, s->mir[k].actor = 0, s->mir[k].blowing = 0, s->mir[k].launched = 0;
            memcpy(s->mir[k].name, o->name, sizeof s->mir[k].name), s->mir[k].name[sizeof s->mir[k].name - 1] = 0;
        }
        s->mir[k].flags = o->flags;
        if (o->flags & (COOP_OBJ_LOCAL | COOP_OBJ_ONLINK)) {   // in the owner's bow's or Link's frame: put on the proxy (place_copy)
            memcpy(s->mir[k].lpos, o->pos, 12);
            quat_mat(o->q, s->mir[k].lrot);
            float pp[3] = {0}, pr[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            if (s->actor && ((o->flags & COOP_OBJ_ONLINK) || !bow_frame(s->actor, pr, pp)))
                coop_rd(s->actor + COOP_ACTOR_POS, pp, 12), coop_rd(s->actor + COOP_ACTOR_ROT, pr, 36);
            for (int i = 0; i < 3; i++) s->mir[k].pos[i] = pp[i] + pr[i * 3] * o->pos[0] + pr[i * 3 + 1] * o->pos[1] + pr[i * 3 + 2] * o->pos[2];
            memcpy(s->mir[k].rot, pr, 36);
        } else {
            memcpy(s->mir[k].pos, o->pos, 12);
            quat_mat(o->q, s->mir[k].rot);
        }
    }
    for (int k = 0; k < COOP_OBJS; k++) {
        // Far from every player (coop_far.c): a live copy is deleted and goes back to wanted; a wanted one is not made
        // until it is nearer (TotK Online's server disposes of an actor 1000 m from every player).
        if (s->mir[k].state == 3 && !s->mir[k].blowing && copy_alive(s, k) && coop_far_from_players(s->mir[k].pos)) {
            rt_log("coop: far: slot %d: the copy of %s (%u) is %.0f m from every player: deleted", (int)(s - slots), s->mir[k].name,
                   s->mir[k].id, coop_far_nearest(s->mir[k].pos));
            coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->mir[k].actor, COOP_DELETE_REASON, 0, 0), set_driven(s->mir[k].actor);
            s->mir[k].state = 1, s->mir[k].actor = 0, s->mir[k].ticks = 0;
            continue;
        }
        if (s->mir[k].state == 1 && coop_far_from_players(s->mir[k].pos)) continue;
        // An owner's autobuilt device: made as an autobuild makes it with TOTK_COOP_AUTOBUILT=1 (autobuilt_on).
        if (s->mir[k].state == 1 && ((s->mir[k].flags & COOP_OBJ_AUTOBUILT) && autobuilt_on()
                                         ? coop_request_create_autobuilt(c, s->mir[k].name, s->mir[k].pos, s->mir[k].rot, s->mir[k].handle)
                                         : coop_request_create(c, s->mir[k].name, s->mir[k].pos, s->mir[k].rot, s->mir[k].handle)))
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
            if (s->mir[k].ticks == 5 && !strcmp(s->mir[k].name, "PlayerBeam")) beam_dump(s->mir[k].actor, 1);
            if (!strcmp(s->mir[k].name, "PlayerBeam") && cfg_get("TOTK_COOP_TEST_BEAMLOG")) beam_log(s->mir[k].actor, s->mir[k].id, "copy");
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
            } else if (!strstr(s->mir[k].name, "Arrow") && !(s->mir[k].flags & COOP_OBJ_ONLINK) && strcmp(s->mir[k].name, "PlayerBeam")) {
                // Destroyed here (a bomb flower's copy shot by this game's Link blew up, a play session,
                // 2026-11-05): the owner's own goes too (the same notice as a pick-up), so it cannot be picked up
                // there again; it went off there only when the copy of this Link's arrow happened to touch it.
                int32_t st = 0;
                char nm[48];
                if (!coop_rd(s->mir[k].actor + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 ||
                    strcmp(coop_actor_name(s->mir[k].actor, nm, sizeof nm), s->mir[k].name)) {
                    if (coop_bonds_claimed(s->mir[k].actor)) {   // part of a contraption run here: made again, not passed on
                        rt_log("coop: slot %d: the copy of object %u (%s) went away here (state %d) while this game ran its contraption; made again",
                               (int)(s - slots), s->mir[k].id, s->mir[k].name, st);
                        s->mir[k].state = 1, s->mir[k].actor = 0, s->mir[k].ticks = 0;
                        continue;
                    }
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
    // (a paraglider's copy from its first tick: it went away by itself about 0.5 s after it was made, glide.txt)
    int coop_bonds_member(uint64_t actor);   // coop_bonds.c: bonded into a contraption here
    if (coop_bonds_member(actor)) return 0;   // (one rigid contraption: all its parts run their update, placed or not)
    return copy_of(actor, &s, &k) && (s->mir[k].ticks >= COPY_WARMUP || (s->mir[k].flags & COOP_OBJ_ONLINK)) &&
           strncmp(s->mir[k].name, "SpObj_", 6) && !pickable(s->mir[k].name) && !s->mir[k].blowing && strcmp(s->mir[k].name, "PlayerBeam") &&
           strcmp(s->mir[k].name, "LightningStrike");   // (a strike's copy plays out by itself)
}
// A beam's copy. Made at scale 1, it showed much larger: the owner's beam is at 0.3 (actor +0x2e4..+0x2ec, set by the
// swing that makes it; TOTK_COOP_TEST_BEAMDUMP, a play session, 2026-11-06). Driven from the reports, its speed went
// 0, 50 and 100 m/s from tick to tick (the reports come unevenly), where the owner's beam goes a steady 50 m/s
// (+0x320 and +0x338, TOTK_COOP_TEST_BEAMLOG), and it spun much faster than the owner's. So it is put at its first
// place until it has moved 0.4 m, then given that way at 50 m/s and left to fly; put back on the reported place only
// when it is more than 3 m off.
static void beam_copy(Cpu *c, Slot *s, int k, float m[12]) {
    uint64_t actor = s->mir[k].actor;
    float sc[3] = {0.3f, 0.3f, 0.3f};
    memcpy((void *)(uintptr_t)(actor + 0x2e4), sc, sizeof sc);
    uint64_t phys = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
    const float *q = s->mir[k].pos;
    _Alignas(16) float v[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
    if (s->mir[k].launched == 0) memcpy(s->mir[k].b0, q, 12), s->mir[k].launched = 1;
    if (s->mir[k].launched == 1) {
        float d[3] = {q[0] - s->mir[k].b0[0], q[1] - s->mir[k].b0[1], q[2] - s->mir[k].b0[2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m, 0, 0);
        if (l > 0.4f) {
            for (int i = 0; i < 3; i++) s->mir[k].bv[i] = d[i] / l * 50.0f;
            s->mir[k].launched = 2;
        }
    }
    if (s->mir[k].launched == 2) {
        memcpy(v, s->mir[k].bv, 12);
        memcpy((void *)(uintptr_t)(actor + 0x320), v, 12), memcpy((void *)(uintptr_t)(actor + 0x338), v, 12);
        float p[3] = {0};
        coop_rd(actor + COOP_ACTOR_POS, p, 12);
        float e2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
        if (!s->mir[k].blowing && e2 > 9) coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m, 0, 0);
    }
    if (body) {
        coop_guest_call(c, MOD_MAIN_BASE + 0x567750, body, (uint64_t)(uintptr_t)v, 0, 0);   // SetLinearVelocity
        coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, body, (uint64_t)(uintptr_t)w, 0, 0);   // SetAngularVelocity
    }
}
// Each tick, a copy is put where its arrow is: actor position and rotation, model matrix and the push to the model
// units, as the proxy. Done from the tick, not in one of its own updates: the game stopped calling a frozen arrow's
// calc after two frames (a fused BombFruit's went on), so in a hook only the BombFruit moved (TOTK_COOP_TEST_COPYLOG,
// 2026-11-05).
static void place_copy(Cpu *c, Slot *s, int k) {
    uint64_t actor = s->mir[k].actor;
    if (!copy_alive(s, k)) {   // gone, or its memory is another actor's now: forgotten, nothing done to it
        s->mir[k].state = 0, s->mir[k].actor = 0;
        return;
    }
    if (coop_bonds_claimed(actor)) return;   // this game runs that contraption now: its copies move by themselves
    // An arrow's copy asks to sleep in its first tick (reason 0, through main+0x55251c: TOTK_COOP_TEST_ARROWWATCH)
    // and sleeps from the third (actor state 6, its model's +0x244 6), where this game's own arrows stay awake (4); a
    // sleeping arrow is not drawn (a play session, 2026-11-05). It is woken as the actor state machine
    // (main+0x54cb00) wakes one: +0x28's reason bits at 0x10, then bit 19 set and the actor queued for the machine
    // (main+0x2ae413c). Frozen after its warmup (copy_place), it does not ask again.
    // (the paraglider's, the beam's, and a fused material's set off: asleep, it did not go off until this Link's camera
    // turned to it, about half the blasts did not show, and one came late enough to hurt this Link, a play session,
    // 2026-11-06)
    if (strstr(s->mir[k].name, "Arrow") || (s->mir[k].flags & COOP_OBJ_ONLINK) || !strcmp(s->mir[k].name, "PlayerBeam") || s->mir[k].blowing) {
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
    if (!strcmp(s->mir[k].name, "PlayerBeam")) { beam_copy(c, s, k, m); return; }
    // An arrow the owner sends in its bow's frame (COOP_OBJ_LOCAL: nocked): put on the proxy's bow as it is drawn here
    // (also from proxy_drive, right after the push that puts the bow on the hand). Put at the reported world place,
    // it lagged behind the proxy's bow as the owner moved (a play session, 2026-11-05).
    float pp[3], pr[9];
    if (((s->mir[k].flags & COOP_OBJ_LOCAL) && s->actor && bow_frame(s->actor, pr, pp)) ||
        ((s->mir[k].flags & COOP_OBJ_ONLINK) && s->actor && coop_rd(s->actor + COOP_ACTOR_POS, pp, 12) && coop_rd(s->actor + COOP_ACTOR_ROT, pr, 36))) {
        const float *l = s->mir[k].lpos, *lr = s->mir[k].lrot;
        for (int i = 0; i < 3; i++) {   // on the proxy: P R_local, P t_local
            for (int j = 0; j < 3; j++) m[i * 4 + j] = pr[i * 3 + 0] * lr[0 * 3 + j] + pr[i * 3 + 1] * lr[1 * 3 + j] + pr[i * 3 + 2] * lr[2 * 3 + j];
            m[i * 4 + 3] = pp[i] + pr[i * 3 + 0] * l[0] + pr[i * 3 + 1] * l[1] + pr[i * 3 + 2] * l[2];
            s->mir[k].pos[i] = m[i * 4 + 3];
        }
    }
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), s->mir[k].pos, 12);
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_ROT), s->mir[k].rot, 36);
    // An arrow's copy at the other player's bow could still be taken (it stayed there; a sparkling spot with an
    // invisible arrow was left where it was taken, and gave an arrow again each time the owner shot; the tester's
    // session, 2026-11-05). The pick-up goes by the actor's place: that is kept 300 m under it, as its body; it is
    // drawn from its model matrix, below.
    // (A material fused to an arrow too: it could be taken off the arrow the same way, a play session.)
    // (the paraglider too: held over the proxy, it is not ridden or taken)
    int arrowish = (strstr(s->mir[k].name, "Arrow") || (s->mir[k].flags & (COOP_OBJ_FUSED | COOP_OBJ_ONLINK))) &&
                   !s->mir[k].blowing;   // not one going off
    if (arrowish) {
        float low[3] = {s->mir[k].pos[0], s->mir[k].pos[1] - 300.0f, s->mir[k].pos[2]};
        memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), low, 12);
    }
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x8) + 0x28);
    static int copylog = -1, n;   // TOTK_COOP_TEST_COPYLOG=1: where each copy is put, every 30th time (test)
    if (copylog < 0) copylog = cfg_get("TOTK_COOP_TEST_COPYLOG") != NULL;
    if (copylog && n++ % 8 == 0) {
        float mt[12] = {0};
        if (model) coop_rd(model + 0x1f8, mt, sizeof mt);
        float pp[3] = {0}, bp[3] = {0}, br[9];
        if (s->actor) coop_rd(s->actor + COOP_ACTOR_POS, pp, 12);
        if (s->actor && bow_frame(s->actor, br, bp)) memcpy(pp, bp, 12);   // from its bow when it has one
        float dm = sqrtf((m[3] - pp[0]) * (m[3] - pp[0]) + (m[7] - pp[1]) * (m[7] - pp[1]) + (m[11] - pp[2]) * (m[11] - pp[2]));
        int32_t cst = 0;
        coop_rd(actor + COOP_ACTOR_STATE, &cst, 4);
        rt_log("coop: test: copy %.2f s %u %s at %.2f %.2f %.2f, model %llx at %.2f %.2f %.2f; put %.2f m from the proxy's bow; state %d", frame_step_world_us() / 1e6,
               s->mir[k].id, s->mir[k].name, s->mir[k].pos[0], s->mir[k].pos[1], s->mir[k].pos[2], (unsigned long long)model, mt[3], mt[7], mt[11], dm, cst);
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
    if (arrowish) {
        // An arrow's copy could be taken once where it was first made, and left a "Take" prompt on the floor there
        // (a play session, 2026-11-05): its model is moved, but its body stayed. The body is kept 300 m under it,
        // out of any Link's reach, set on the body itself (main+0x25a450 (body, m), which the physics takes; the
        // actor's own ForceSetMatrix is applied in its update, which a frozen copy does not run); the frozen copy
        // does not take its place from the body.
        _Alignas(16) float mb[12];
        memcpy(mb, m, sizeof mb);
        mb[7] -= 300.0f;
        uint64_t phys = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
        uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
        if (body) coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, body, (uint64_t)(uintptr_t)mb, 0, 0);
        static int said;
        float bt[12] = {0};
        if (body) coop_rd(body + 0x98, bt, sizeof bt);   // its last transform (3x4 rows)
        if (said++ % 200 == 0 && said < 1000)
            rt_log("coop: arrow copy %u: its body %llx is kept 300 m under it (body at y %.2f, the copy at %.2f)", s->mir[k].id,
                   (unsigned long long)body, bt[7], m[7]);
        return;
    }
    if (pickable(s->mir[k].name)) {   // an item runs its own physics: its body is put with it each tick, not driven
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m, 0, 0);
        // ForceSetMatrix takes in the actor's own update, which an item at rest no longer runs (it sleeps): a dropped
        // monster part's copy stayed 1.75 m off for minutes, a spear's 6.6 m (the sync audit, a play session,
        // 2026-11-06). More than 0.3 m off, its body and place are set at once, as an arrow copy's are.
        float ap[3];
        if (coop_rd(actor + COOP_ACTOR_POS, ap, 12) &&
            (ap[0] - m[3]) * (ap[0] - m[3]) + (ap[1] - m[7]) * (ap[1] - m[7]) + (ap[2] - m[11]) * (ap[2] - m[11]) > 0.09f) {
            _Alignas(16) float mb[12];
            memcpy(mb, m, sizeof mb);
            uint64_t ph = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50), ct = ph ? coop_rd_ptr(ph + 0x20) : 0, bd = ct ? coop_rd_ptr(ct + 0x150) : 0;
            if (bd) coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, bd, (uint64_t)(uintptr_t)mb, 0, 0);
            memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), s->mir[k].pos, 12);
        }
        // ... and stopped there: with its speed kept, a rolling apple's copy rolled on 1.5 m past where the owner's
        // came to rest (the sync audit, pickup test, 2026-11-06)
        uint64_t phys = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
        uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
        if (body) {
            _Alignas(16) float z[4] = {0, 0, 0, 0};
            coop_guest_call(c, MOD_MAIN_BASE + 0x567750, body, (uint64_t)(uintptr_t)z, 0, 0);   // SetLinearVelocity
            coop_guest_call(c, MOD_MAIN_BASE + 0x25a1f0, body, (uint64_t)(uintptr_t)z, 0, 0);   // SetAngularVelocity
        }
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
static float bend_at[3] = {1e9f, 1e9f, 1e9f};   // where this Link's last beam ended, and when (beam_end_watch)
static int bend_tick = -1000, bend_ticks;
static struct { uint64_t actor; uint32_t key, id; int seen, moved, born, made, touched, near_me, said, ended; float last[3]; char name[48]; } props[PROPS_N];
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
// (The Link's own attack effects, PlayerBeam, the Master Sword's beam, and PlayerAttackGrave, the Earthwake
// Technique's shockwave, are left out with the "Player" names: sent as copies they played out wrong, the beam spun in
// place and the shockwave's glow stayed in the air, a play session, 2026-11-05. The proxy makes its own: its
// attack thaw lasts while its owner holds the button, and its life follows its owner's, see "proxy attacks".)
// The beam, though, is sent: the proxy made none (it seems to go by this game's own hearts). It has no model: what
// shows is its effect, on its body. Frozen and kept 300 m under as an arrow's copy is, the effect stayed spinning
// where it was made (TOTK_COOP_TEST_BEAMLOG: the copy was put where the owner's beam is, 30 m/s along). So its copy
// runs its own update and its body is driven at the owner's beam's speed (as a ridden copy's is).
static int is_beam(const char *nm) { return !strcmp(nm, "PlayerBeam"); }
// Test (TOTK_COOP_TEST_BEAMLOG=1): a beam's (this Link's, "own", or a copy's) place, turn (its rotation's first row),
// state and the words that differed between the owner's beam and its copy (TOTK_COOP_TEST_BEAMDUMP), each tick. For the
// copy spinning faster than the beam and showing no hit (a play session, 2026-11-06).
static void beam_log(uint64_t a, uint32_t id, const char *who) {
    float p[3] = {0}, r[9] = {0}, v320[6] = {0};
    int32_t st = 0;
    uint32_t w28 = 0;
    coop_rd(a + COOP_ACTOR_POS, p, 12), coop_rd(a + COOP_ACTOR_ROT, r, 36), coop_rd(a + COOP_ACTOR_STATE, &st, 4), coop_rd(a + 0x28, &w28, 4);
    coop_rd(a + 0x320, v320, 12), coop_rd(a + 0x338, v320 + 3, 12);
    uint64_t c11 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 8 * 11);
    float f188 = 0, f1ec = 0, f150 = 0;
    if (c11) coop_rd(c11 + 0x188, &f188, 4), coop_rd(c11 + 0x1ec, &f1ec, 4), coop_rd(c11 + 0x150, &f150, 4);
    rt_log("coop: test: beam %s %u %.3f s: at %.2f %.2f %.2f, row0 %.3f %.3f %.3f, state %d +28 %x, +320 %.2f %.2f %.2f +338 %.2f %.2f %.2f, c11 %.2f %.2f %.2f",
           who, id, frame_step_world_us() / 1e6, p[0], p[1], p[2], r[0], r[1], r[2], st, w28, v320[0], v320[1], v320[2], v320[3], v320[4],
           v320[5], f188, f1ec, f150);
}
// Test (TOTK_COOP_TEST_BEAMDUMP=1): the first beam of this Link's 5 ticks after it woke, and the first beam copy here
// 5 ticks after it was made, each written once to logs/beamdump.bin (tag 0 own, 1 copy): the actor's first 0x800
// bytes and 0x200 of each of its first 64 components. The copy showed much larger than the owner's beam (the tester's
// session, 2026-11-06); diffed offline, one game's own against the other's copy.
static void beam_dump(uint64_t a, int copy) {
    static int on = -1, done[2];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_BEAMDUMP") != NULL;
    if (!on || done[copy]) return;
    done[copy] = 1;
    static uint8_t buf[0x800 + 64 * 0x200];
    memset(buf, 0, sizeof buf);
    coop_rd(a, buf, 0x800);
    uint64_t tbl = coop_rd_ptr(a + 0x228);
    for (int k = 0; k < 64 && tbl; k++) {
        uint64_t cp = coop_rd_ptr(tbl + 8 * (uint64_t)k);
        if (cp && !(cp & 7)) coop_rd(cp, buf + 0x800 + k * 0x200, 0x200);
    }
    FILE *f = fopen("logs/beamdump.bin", "ab");
    if (!f) return;
    uint32_t tag = (uint32_t)copy;
    fwrite(&tag, 4, 1, f), fwrite(buf, sizeof buf, 1, f), fclose(f);
    rt_log("coop: test: beam dump: %s %llx", copy ? "a copy" : "this Link's", (unsigned long long)a);
}

static int prop_skip(const char *nm) {
    if (is_beam(nm)) return 0;
    // A horse's parts: its mane (every Horse_ actor is a Horse_Link_Mane*), saddle and reins. Each game's horse has
    // its own; sent as moved objects, a horse's mane hung in the air beside the other Link (suite case mount,
    // 2026-11-07). With TOTK_COOP_MOUNTS=1 the horse itself is the shared world's (coop_mounts.c), not a moved object.
    if (!strncmp(nm, "Horse_", 6) || !strncmp(nm, "GameRomHorseSaddle_", 19) || !strncmp(nm, "GameRomHorseReins_", 18)) return 1;
    if (coop_mounts_on() && !strncmp(nm, "GameRomHorse", 12)) return 1;
    // (Parasail, the Link's paraglider, was left out here: a copy of it seemed to take the other game down with the
    // Ultrahand crash, which was the beam's race (beam_hook). It is sent in its Link's frame: COOP_OBJ_ONLINK.)
    // Sage_: the sages' companion spirits, and Weapon_Bow_128, Tulin's bow, which follow the Link: sent as moved
    // objects, their copies came and went over and over and the bow showed on the other Link (glide.txt on the Rito
    // save, the tester watching, 2026-11-05).
    static const char *pre[] = {"Player", "Armor_", "Accessory_", "PlayerRaul", "Npc", "Animal_", "Enemy_",
                                "Weapon_Sheath", "Weapon_Quiver", "TBox_", "Sage_", "Weapon_Bow_128",
                                // the Link's own helpers (its edges): a save load took the WakeBoardRope for a destroyed
                                // object, and the other game deleted its own (load_3rd.txt)
                                "WakeBoardRope", "Obj_OneTouch_Connection", "Dm_Npc_RevivalFairy", "CarryBox", "ThrowBox",
                                // what a warp brings in about the Link where it lands: the shrine entrance and its warp
                                // point, a cannon terminal and door; taken as made, each game had copies of the other's,
                                // and a warp to a place with a copied warp point hung about a minute (a play session,
                                // 2026-11-06)
                                "DgnObj_ZonauShrineEntrance", "DgnObj_WarpPoint", "ZonauShrine_EntranceGate", "FldObj_Cannon",
                                // the Gloom Hands (MiasmaSwarm and its MiasmaTentacle hands): made in one game when its
                                // Link came near, then sent as moved objects; the other game's copy of a MiasmaTentacle
                                // crashed it at once (access violation in main+0x4f2894, a play session, 2026-11-07)
                                "Miasma",
                                // the pooled helper actor the game gives each combined contraption (an autobuild's):
                                // the idle ones wait at the origin, so the other game took one of its own idle ones for
                                // it by key and moved it to the owner's build (autobuild_3rd.txt pair test, 2026-11-08)
                                "CombinedActorIntegrator"};
    // (UltraHand_Connection_Effect2, an effect Ultrahand makes on a contraption, is copied again: it was left out on
    // 2026-11-08 morning, and then the tester saw no glue on a hand-glued contraption in the other game; whether it is the
    // glue's look is not known)
    // (LightningStrike, a thunderstorm's strike, was left out here for a while; it is sent again, the one that hits
    // this Link, so the others see it struck: the tester's list, 2026-11-06. Its copy runs its own update, copy_place.)   // chests: each player's own
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
        // a weapon's sheath, whatever its name (Weapon_Lsheath_103, Weapon_SpearSheath_109: only "Weapon_Sheath" was
        // left out, and the others went to the other game as dropped objects over and over; a play session)
        if (strstr(nm, "sheath") || strstr(nm, "Sheath")) return 1;
    }
    // The material fused to a weapon, bow or shield of this Link or a co-op Link: an actor of its own, stuck to the
    // weapon. Sent as an object, the other game showed a loose copy of it on the proxy's weapon, over the fused one
    // the proxy's weapon has (a play session, 2026-11-06: "the real item is appended onto them").
    if (nm[0]) {
        for (int k = 0; k < 3; k++) if (my_fuse[k][0] && !strcmp(nm, my_fuse[k])) return 1;
        int old_fuse_name(const char *nm);   // the material of a co-op Link's old item, still watched (below)
        if (old_fuse_name(nm)) return 1;
        for (int i = 0; i < COOP_MAX_PLAYERS; i++)
            for (int k = 0; k < 3; k++)
                if (slots[i].actor && ((slots[i].fuse_have[k][0] && !strcmp(nm, slots[i].fuse_have[k])) ||
                                       (slots[i].fuse_want[k][0] && !strcmp(nm, slots[i].fuse_want[k]))))
                    return 1;
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
// A proxy's weapon, bow or shield taken away because its owner has none now (after loading another save): the
// proxy let go of it later, it lay on the ground, was sent as a world object and could be picked up in the owner's
// game (a play session, 2026-11-05: "holds Weapon_Shield_030, owner -" four times, then "I move Weapon_Shield_030").
// It is watched for 30 s; once it is out of the proxy's gear it is deleted, and it is never sent meanwhile.
// remade: one only watched after its item was made again (deleted once out of the gear, kept while held).
static struct { uint64_t actor, eq; int k, until, since, remade; uint32_t id; char name[48], fuse[32]; } old_items[16];
#define OLD_ITEM_ID 0x380   // an actor's own id (coop_glue.c ACTOR_ID): one made at a freed one's address has another
static char old_item_fuse[32];   // the next old item's fused material: the gear code sets it before regive_gear
static uint64_t dkilled[64];   // set off here for another game's notice, or deleted here by this code: not told as destroyed
static int dkilled_n, dtick;
int old_fuse_name(const char *nm) {   // (a loose one is not sent: old_items_tick deletes it)
    for (int i = 0; i < 16; i++) if (old_items[i].actor && old_items[i].fuse[0] && !strcmp(nm, old_items[i].fuse)) return 1;
    return 0;
}
static void watch_old_item(uint64_t eq, int k, uint64_t old, int remade) {
    int f = -1;
    uint32_t oid = 0;
    coop_rd(old + OLD_ITEM_ID, &oid, 4);
    for (int i = 0; i < 16; i++) {
        if (old_items[i].actor == old && old_items[i].id == oid) {   // (since stays: the first time)
            old_items[i].until = prop_tick + 900, old_items[i].remade &= remade;
            return;
        }
        if (old_items[i].actor == old) old_items[i].actor = 0;   // (another item at that address: the entry is stale)
        if (f < 0 && (!old_items[i].actor || old_items[i].until < prop_tick)) f = i;   // a free entry first (review)
    }
    if (f < 0)
        for (int i = f = 0; i < 16; i++) if (old_items[i].until < old_items[f].until) f = i;
    old_items[f].actor = old, old_items[f].eq = eq, old_items[f].k = k, old_items[f].until = prop_tick + 900, old_items[f].since = prop_tick;
    old_items[f].remade = remade;
    coop_actor_name(old, old_items[f].name, sizeof old_items[f].name);
    old_items[f].id = 0;
    coop_rd(old + OLD_ITEM_ID, &old_items[f].id, 4);
    memcpy(old_items[f].fuse, old_item_fuse, sizeof old_items[f].fuse);   // the material fused to it (set by the caller)
    old_item_fuse[0] = 0;
}
// An old item deleted, and the material fused to it: an actor of its own stuck to the item, which a delete of the item
// alone left lying in the world, where it could be picked up in that game only (a Silver Moblin Horn off a stand-in's
// stick when its owner went unarmed; a play session, 2026-11-08). Its fused part is the actor of that material's name
// within 1 m of the item.
typedef struct { float at[3]; const char *name; uint64_t found[4]; int n; float r2; } FuseFind;
static void fuse_find_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    (void)key;
    FuseFind *f = arg;
    float q[3];
    if (f->n >= 4 || strcmp(nm, f->name) || !coop_rd(a + COOP_ACTOR_POS, q, 12)) return;
    float dx = q[0] - f->at[0], dy = q[1] - f->at[1], dz = q[2] - f->at[2];
    if (dx * dx + dy * dy + dz * dz < (f->r2 > 0 ? f->r2 : 1.0f)) f->found[f->n++] = a;
}
// Whether an actor sits on (within 0.6 m of) an item a co-op Link or this Link holds now: the fused part of a weapon in
// use, not a loose one.
static int fuse_on_held(uint64_t a) {
    float q[3];
    if (!coop_rd(a + COOP_ACTOR_POS, q, 12)) return 1;
    // An item that is gear now, or being made as gear: a fused material can be a weapon (a stick fused to a sword), and
    // the stand-in's new Weapon_Spear_109 was taken for its old sword's fused Weapon_Spear_109 and deleted, so the
    // stand-in held nothing (a play session, 2026-11-08). Never deleted: one in an EquipmentUser, or of a name a
    // co-op Link's or this Link's gear has.
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    for (int k = 0; k < 3; k++) if (nm[0] && !strcmp(nm, my_gear[k])) return 1;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int k = 0; k < 3; k++)
            if (slots[i].actor && nm[0] && (!strcmp(nm, slots[i].gear_want[k]) || !strcmp(nm, slots[i].gear_have[k]))) return 1;
    uint64_t holders[1 + COOP_MAX_PLAYERS];
    int nh = 0;
    holders[nh++] = coop_player_actor();
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) if (slots[i].actor && slots[i].state == LIVE) holders[nh++] = slots[i].actor;
    for (int h = 0; h < nh; h++) {
        uint64_t eq = eq_of_actor(holders[h]);
        for (int k = 0; eq && k < 3; k++) {
            uint64_t it = eq_item(eq, k);
            float p[3];
            if (!it || !coop_rd(it + COOP_ACTOR_POS, p, 12)) continue;
            float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
            if (dx * dx + dy * dy + dz * dz < 0.6f * 0.6f) return 1;
        }
    }
    return 0;
}
static void delete_old_item(Cpu *c, int i) {
    FuseFind ff = {.name = old_items[i].fuse};
    int have = old_items[i].fuse[0] && coop_rd(old_items[i].actor + COOP_ACTOR_POS, ff.at, 12);
    if (have) {
        void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
        coop_actors_all(fuse_find_fn, &ff);
    }
    coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, old_items[i].actor, COOP_DELETE_REASON, 0, 0);
    dkilled[dkilled_n++ % 64] = old_items[i].actor;   // (this code's own delete: no destroy notice for it)
    for (int k = 0; k < ff.n; k++) {
        if (fuse_on_held(ff.found[k])) continue;   // (gear of a Link, or of a name a Link's gear has)
        coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, ff.found[k], COOP_DELETE_REASON, 0, 0);
        dkilled[dkilled_n++ % 64] = ff.found[k];
        rt_log("coop: proxy: its old %s's fused %s is deleted with it", old_items[i].name, old_items[i].fuse);
    }
}
// give_gear on a co-op Link, and its weapon, bow or shield as it was before watched as an old item (deleted once out
// of its gear, never sent meanwhile). Each item made again (taken out, put away, its gear fixed) is a new actor; the
// one before stayed in the world, the prop sync sent it, and the other player had swords, shields and fused parts
// lying about them (a play session, 2026-11-07).
static void regive_gear(Cpu *c, uint64_t eq, int k, const char *name, int dye, int out_want, uint8_t st[0x60], const char *fuse,
                        uint8_t att_st[0x60]) {
    uint64_t old = k <= 2 ? eq_item(eq, k) : 0;
    give_gear(c, eq, k, name, dye, out_want, st, fuse, att_st);
    // (the create's new item takes the slot later: the old one is watched until it is out of the gear, and kept while
    // it is held; an emptied slot's is let go of after 2 s even if still held, as before)
    if (old) watch_old_item(eq, k, old, name[0] != 0);
    old_item_fuse[0] = 0;   // (none taken: not left for another item)
}
// An item of a co-op Link taken out or put away the game's way (TotK Online's, read for behaviour only): its
// EquipmentUser's setEquipmentInHand (eq, row) and removeEquipmentInHand (eq, row, 0), rows 0 weapon, 3 bow, 4 shield
// (1.4.3 main+0x4f5700 and main+0x4f5194: tools/coop/xver_match.py from 1.2.1 0x68370c and 0x65aaf8; both work on
// the in-hand bytes at eq+0x4620). Off unless TOTK_COOP_GEAR_INHAND=1: on our co-op Link, a Player whose own update
// runs in bursts (a swing), the sword then sat on its back in 3 of 5 frames of a swing test against 1 of 5 made again
// (2026-11-07); TotK Online's stand-in has no AI and replays its owner's animation clips, which this needs as well.
#define GEAR_SET_IN_HAND 0x4f5700
#define GEAR_REMOVE_IN_HAND 0x4f5194
// Test (TOTK_COOP_TEST_INHAND=1): before each in-hand call, what setEquipmentInHand (main+0x4f5700) reads: the object
// at eq+0x1620 that its chain starts from, that object's +0x325 bit 0, its [+0x10], [+0x18] and the byte +0x2d2 that
// gates the hand attach; and the same for this game's own Link (the stand-in held its sword only while this Link had
// one equipped: the tester, 2026-11-07).
static void inhand_probe(uint64_t eq, const char *who) {
    static int on = -1, said;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_INHAND") != NULL;
    if (!on || !eq || said++ > 40) return;
    uint64_t o = coop_rd_ptr(eq + 0x1620), o2 = o ? coop_rd_ptr(o + 0x10) : 0, o3 = o ? coop_rd_ptr(o + 0x18) : 0;
    uint8_t b325 = 0, b2d2 = 0;
    if (o) coop_rd(o + 0x325, &b325, 1), coop_rd(o + 0x2d2, &b2d2, 1);
    char nm[48] = "";
    if (o) coop_actor_name(o, nm, sizeof nm);
    {   // what the record at +0x1620 points to: [+0x18] a reference object (+0x40 its actor), [+0x20] a serial
        uint64_t r = o ? coop_rd_ptr(o + 0x18) : 0, ra = r ? coop_rd_ptr(r + 0x40) : 0;
        uint32_t ser = 0, rser = 0;
        if (o) coop_rd(o + 0x20, &ser, 4);
        if (r) coop_rd(r + 0x48, &rser, 4);
        char an[48] = "";
        if (ra) coop_actor_name(ra, an, sizeof an);
        rt_log("coop: test: inhand %s: record's ref %llx (actor %llx '%s', serial %u, record's %u); this Link %llx, stand-in %llx", who,
               (unsigned long long)r, (unsigned long long)ra, an, rser, ser, (unsigned long long)coop_player_actor(),
               (unsigned long long)(slots[0].actor));
    }
    rt_log("coop: test: inhand %s: eq %llx +0x1620 %llx ('%s') +0x325 %u [+0x10] %llx [+0x18] %llx +0x2d2 %u", who,
           (unsigned long long)eq, (unsigned long long)o, nm, b325, (unsigned long long)o2, (unsigned long long)o3, b2d2);
    // Where an actor holds that object: its first 0x1000 bytes and each of its components ([actor+0x228] table,
    // 0x800 bytes each), for this game's Link and each stand-in, so the stand-in's own can be found at the same place.
    uint64_t actors[1 + COOP_MAX_PLAYERS];
    int na = 0;
    actors[na++] = coop_player_actor();
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) if (slots[i].actor && slots[i].state == LIVE) actors[na++] = slots[i].actor;
    for (int a = 0; o && a < na; a++) {
        static uint64_t buf[0x200];
        if (coop_rd(actors[a], buf, 0x1000))
            for (int w = 0; w < 0x200; w++) if (buf[w] == o) rt_log("coop: test: inhand: actor %d (%llx) +%#x holds it", a, (unsigned long long)actors[a], w * 8);
        uint64_t tbl = coop_rd_ptr(actors[a] + 0x228);
        for (int ci = 0; tbl && ci < 0x80; ci++) {
            uint64_t comp = coop_rd_ptr(tbl + (uint64_t)ci * 8);
            if (!comp) continue;
            if (comp == o) rt_log("coop: test: inhand: actor %d component %#x is it", a, ci * 8);
            if (!coop_rd(comp, buf, 0x800)) continue;
            for (int w = 0; w < 0x100; w++) if (buf[w] == o) rt_log("coop: test: inhand: actor %d component %#x (%llx) +%#x holds it", a, ci * 8, (unsigned long long)comp, w * 8);
        }
    }
}
static uint64_t eq_of_actor(uint64_t a);
static void gear_hand(Cpu *c, uint64_t eq, int k, int in) {
    static const uint64_t row[3] = {0, 3, 4};
    static int on2 = -1, said2;
    if (on2 < 0) on2 = cfg_get("TOTK_COOP_TEST_INHAND") != NULL;
    uint64_t mine = eq_of_actor(coop_player_actor());
    #define EQ_STATE(e, tag) do { if (on2 && (e) && said2 < 60) { uint8_t f28 = 0, f20 = 0, f4 = 0; uint32_t f0 = 0;         coop_rd((e) + 0x4628, &f28, 1), coop_rd((e) + 0x4620 + row[k], &f20, 1);         coop_rd((e) + 0x570 + row[k] * 0x100 + 0xf0, &f0, 4), coop_rd((e) + 0x570 + row[k] * 0x100 + 0xf4, &f4, 1);         said2++; rt_log("coop: test: inhand %s: +0x4628 %#x, row %llu +0x4620 %u, record +0xf0 %#x +0xf4 %u", tag, f28, (unsigned long long)row[k], f20, f0, f4); } } while (0)
    EQ_STATE(eq, "stand-in before");
    EQ_STATE(mine, "this Link");
    if (in) coop_guest_call(c, MOD_MAIN_BASE + GEAR_SET_IN_HAND, eq, row[k], 0, 0);
    EQ_STATE(eq, "stand-in after");
    #undef EQ_STATE
    if (!in) coop_guest_call(c, MOD_MAIN_BASE + GEAR_REMOVE_IN_HAND, eq, row[k], 0, 0);
}
static int old_item(uint64_t a) {
    for (int i = 0; i < 16; i++) if (old_items[i].actor == a && old_items[i].until >= prop_tick) return 1;
    return 0;
}
static void old_items_tick(Cpu *c) {
    for (int i = 0; i < 16; i++) {
        if (!old_items[i].actor) continue;
        // Its fused material come loose while it is watched (emptying a slot knocked a stand-in's horn off its spear,
        // and the horn lay there and was sent as a world object: a play session, 2026-11-08): deleted, within 1.5 m
        // of the item, unless it is on an item a co-op Link holds now. Checked every 10 ticks.
        if (old_items[i].fuse[0] && prop_tick % 10 == 0) {
            FuseFind ff = {.name = old_items[i].fuse};
            if (coop_rd(old_items[i].actor + COOP_ACTOR_POS, ff.at, 12)) {
                void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
                ff.r2 = 1.5f * 1.5f;
                coop_actors_all(fuse_find_fn, &ff);
                for (int k = 0; k < ff.n; k++) {
                    if (fuse_on_held(ff.found[k])) continue;
                    coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, ff.found[k], COOP_DELETE_REASON, 0, 0);
                    dkilled[dkilled_n++ % 64] = ff.found[k];
                    rt_log("coop: proxy: the %s come off its old %s is deleted", old_items[i].fuse, old_items[i].name);
                }
            }
        }
        // (gone, or another actor in its memory: forgotten, not deleted; code review, 2026-11-06)
        // ... or a new item of the same name made at its freed address (switching weapons fast: the stand-in's new
        // stick was taken for the old one and deleted in its hand, with the breaking effect; a play session,
        // 2026-11-08): its own id differs
        uint32_t oid = 0;
        if (!actor_is(old_items[i].actor, old_items[i].name) || !coop_rd(old_items[i].actor + OLD_ITEM_ID, &oid, 4) || oid != old_items[i].id) {
            old_items[i].actor = 0;
            continue;
        }
        if (old_items[i].until < prop_tick) {   // watched 30 s: deleted unless still held (sent as a world object else)
            if (eq_item(old_items[i].eq, old_items[i].k) != old_items[i].actor) {
                rt_log("coop: proxy: its old %s, unheld 30 s on, is deleted", old_items[i].name);
                delete_old_item(c, i);
            }
            old_items[i].actor = 0;
            continue;
        }
        // Still the proxy's: making its gear again with no item does not take it (load_3rd.txt: four tries, the
        // shield stayed in hand). After 2 s it is deleted all the same, as a broken weapon is, and the gear lets go.
        uint64_t cur = eq_item(old_items[i].eq, old_items[i].k);
        int held = cur == old_items[i].actor;
        if (held && (old_items[i].remade || prop_tick - old_items[i].since < 60)) continue;
        // One made again: deleted only once another item holds its slot. Out of the gear alone, it was deleted while
        // the slot was between items, and the co-op Link drew an empty hand (a play session, 2026-11-07).
        if (old_items[i].remade && !cur) continue;
        char nm[48];
        rt_log("coop: proxy: its old %s, %s, is deleted", coop_actor_name(old_items[i].actor, nm, sizeof nm), held ? "still in its gear" : "out of its gear");
        delete_old_item(c, i);
        old_items[i].actor = 0;
    }
}
typedef struct { float me[3]; int cand[32], n_cand, n_first_near; } PropScan;
static void prop_fn(uint64_t a, const char *nm, uint32_t key, int first, void *arg) {
    PropScan *ps = arg;
    Slot *cs;
    int ck;
    if (prop_skip(nm) || copy_of(a, &cs, &ck) || is_driven(a) || is_preview(a) || old_item(a)) return;
    for (int i = 0; i < 32; i++) if (my_arrows[i].actor == a) return;   // an arrow's fused material
    int32_t ast = 0;   // one going away (state 7..11) is not sent (a bomb after its blast, see arrow_copies)
    if (coop_rd(a + COOP_ACTOR_STATE, &ast, 4) && ast > 6 && !is_beam(nm)) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    int coop_enemy_within(const float p[3], float m);   // coop_world.c
    if (!strncmp(nm, "Weapon_", 7) && coop_enemy_within(p, 3.0f)) return;   // an enemy's (in its hand, or just let go of)
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
    // The beam (is_beam): a pooled actor, reused, so not new to the key scan: taken as made by this Link whenever it is
    // awake (state 4) within 6 m and nearer it, with a new number each time it wakes, and forgotten when it sleeps.
    if (is_beam(nm)) {
        int32_t est = 0;
        coop_rd(a + COOP_ACTOR_STATE, &est, 4);
        int ie = -1;
        for (int i = 0; i < PROPS_N && ie < 0; i++) if (props[i].actor == a) ie = i;
        if (est != 4) {
            if (ie >= 0) {
                beam_end_watch(props[ie].last);
                props[ie].actor = 0;
            }
            return;
        }

        if (ie < 0) {
            // within 6 m: this Link's. (Not "nearer it than a co-op Link" as other made objects: P1 standing in front of
            // P2 took P2's beams, which spawn 2.4 m ahead, for nobody's; a proxy makes no beam here, so any is ours.)
            if (d2 > 36) return;
            for (int i = 0; i < PROPS_N && ie < 0; i++) if (!props[i].actor) ie = i;
            if (ie < 0) return;
            memset(&props[ie], 0, sizeof props[ie]);
            props[ie].actor = a, props[ie].key = key, props[ie].made = 1, props[ie].born = prop_tick, props[ie].id = next_arrow_id++;
            snprintf(props[ie].name, sizeof props[ie].name, "%s", nm);
        }
        props[ie].seen = prop_tick, props[ie].near_me = 1;
        memcpy(props[ie].last, p, 12);
        if (prop_tick - props[ie].born == 5) beam_dump(a, 0);
        static int beamlog = -1, bn;   // TOTK_COOP_TEST_BEAMLOG=1: where the beam actor and its model are, while awake
        if (beamlog < 0) beamlog = cfg_get("TOTK_COOP_TEST_BEAMLOG") != NULL;
        if (beamlog && bn++ < 900) beam_log(a, props[ie].id, "own");
        return;
    }
    int e = -1, free = -1;
    for (int i = 0; i < PROPS_N && e < 0; i++) {
        // (an entry of another name at this address is another actor in a gone one's memory: the entry is let go, so
        // the new one is not sent under the old one's number; code review, 2026-11-06)
        if (props[i].actor == a && strcmp(props[i].name, nm)) props[i].actor = 0;
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
    // More at once: a load or a streamed-in area, the world's own. A glider made 6+ at once (its parts): 20. And none
    // for 10 s after this Link moved more than 30 m in a tick or became another actor (a warp or a load: the area
    // comes in over several seconds, a few at a time).
    static uint64_t last_me;
    static float last_p[3];
    static int made_quiet_until;
    float jump = (ps.me[0] - last_p[0]) * (ps.me[0] - last_p[0]) + (ps.me[1] - last_p[1]) * (ps.me[1] - last_p[1]) +
                 (ps.me[2] - last_p[2]) * (ps.me[2] - last_p[2]);
    if (me != last_me || jump > 30 * 30) made_quiet_until = prop_tick + 600;
    last_me = me, memcpy(last_p, ps.me, 12);
    if (ps.n_first_near <= 20 && frame_step_world_us() > 20000000 && prop_tick >= made_quiet_until)
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
            if (is_beam(props[e].name)) {   // a beam out of the live list has ended (asleep or going away)
                if (!props[e].ended) props[e].ended = 1, beam_end_watch(props[e].last);
                props[e].actor = 0;
                continue;
            }
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
        // (prop_skip for a moved one too: a world wheel fused to a shield, sent as moved before it was known fused,
        // went on being sent, and the other game dragged its own full-size wheel along with the proxy; the tester's
        // session, 2026-11-06)
        if (!props[e].made && prop_skip(props[e].name)) { props[e].touched = 0; continue; }
        if (props[e].made) {
            if ((prop_tick - props[e].born < 10 && !is_beam(props[e].name)) || prop_skip(props[e].name)) continue;   // a copy made here is known by now; gear worn again (the beam at once: it is short)
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
        o->flags = is_imaginary(props[e].actor) ? COOP_OBJ_AUTOBUILT : 0;   // (autobuilt_on)
        // The paraglider: sent in its Link's frame, so the other games hold it over the proxy as they show it (sent as
        // a world place it would trail the proxy, as a nocked arrow did).
        float lp[3], lr[9];
        if (!strncmp(o->name, "Parasail", 8)) {   // sent only while open: closed, it sleeps (state 6) far off (glide.txt)
            int32_t gst = 0;
            if (!coop_rd(props[e].actor + COOP_ACTOR_STATE, &gst, 4) || gst != 4) continue;
        }
        if (!strncmp(o->name, "Parasail", 8) && coop_rd(me + COOP_ACTOR_POS, lp, 12) && coop_rd(me + COOP_ACTOR_ROT, lr, 36)) {
            float d[3] = {o->pos[0] - lp[0], o->pos[1] - lp[1], o->pos[2] - lp[2]}, rl[9];
            for (int j = 0; j < 3; j++) o->pos[j] = lr[0 * 3 + j] * d[0] + lr[1 * 3 + j] * d[1] + lr[2 * 3 + j] * d[2];
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) rl[a * 3 + b] = lr[0 * 3 + a] * r[0 * 3 + b] + lr[1 * 3 + a] * r[1 * 3 + b] + lr[2 * 3 + a] * r[2 * 3 + b];
            memcpy(r, rl, sizeof rl);
            o->flags |= COOP_OBJ_ONLINK;
        }
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
typedef struct { uint32_t key[COOP_MAX_PLAYERS * COOP_OBJS]; uint64_t actor[COOP_MAX_PLAYERS * COOP_OBJS]; const char *name[COOP_MAX_PLAYERS * COOP_OBJS]; int n; } KeyFind;
// (the key and the name: by key alone, another actor with that key was taken, then refused for its name every scan,
// and forced to the place meanwhile; code review, 2026-11-06)
static void keyfind_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    KeyFind *kf = arg;
    for (int i = 0; i < kf->n; i++)
        if (kf->key[i] == (key & ~COOP_OBJ_KEYED) && !kf->actor[i] && (!kf->name[i] || !strcmp(kf->name[i], nm))) kf->actor[i] = a;   // 31 bits sent
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
    // far from every player (coop_far.c): not moved, the game's own (a placed object is never deleted for it)
    if (coop_far_from_players(p)) {
        static int said;
        if (said++ < 20) rt_log("coop: far: a placed object (%llx) is %.0f m from every player: not moved", (unsigned long long)a, coop_far_nearest(p));
        return;
    }
    quat_mat(q, r);
    int k = 0, free = -1;
    int64_t now = frame_step_world_us();
    // (an entry not used for 1 s is free again: kept for good, after 128 objects new ones took each other's places
    // and were teleported each time instead of moved by speed; code review, 2026-11-06)
    for (; k < 128 && last[k].actor != a; k++) if (free < 0 && (!last[k].actor || now - last[k].us > 1000000)) free = k;
    if (k == 128) k = free >= 0 ? free : (int)((a >> 4) & 127), last[k].actor = a, last[k].us = 0;
    if (last[k].us == now) return;   // the tick runs twice at one world time: once a frame
    float dt = last[k].us ? (float)(now - last[k].us) / 1e6f : 0;
    float dx = p[0] - pos[0], dy = p[1] - pos[1], dz = p[2] - pos[2];
    uint64_t phys = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    uint64_t body = ctl ? coop_rd_ptr(ctl + 0x150) : 0;
    if (!body || dx * dx + dy * dy + dz * dz > 0.25f || dt <= 0 || dt > 0.5f) {
        if (dx * dx + dy * dy + dz * dz >= 4e-4f || !body) {
            _Alignas(16) float m[12];
            for (int row = 0; row < 3; row++) {
                for (int j = 0; j < 3; j++) m[row * 4 + j] = r[row * 3 + j];
                m[row * 4 + 3] = pos[row];
            }
            coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, a, (uint64_t)(uintptr_t)m, 0, 0);
            // (an object at rest runs no update, where ForceSetMatrix takes: its body and place set at once too; a
            // rock part held at another's report stayed 12 m off, the sync audit, 2026-11-06)
            if (body && dx * dx + dy * dy + dz * dz > 0.09f) {
                coop_guest_call(c, MOD_MAIN_BASE + 0x25a450, body, (uint64_t)(uintptr_t)m, 0, 0);
                memcpy((void *)(uintptr_t)(a + COOP_ACTOR_POS), pos, 12);
            }
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
// ---- destroyed placed objects. A bomb arrow broke a wall in one game and not in the other (a play session,
// 2026-11-05): a placed object going away was not synced. Every 10 ticks this game notes the placed objects (by key)
// within 20 m of its Link; one that goes away (state 7..11, same name) is told to the other games (an OBJS packet with
// header word 3: each entry's id is COOP_OBJ_KEYED | key, with its name), which set theirs off the same way: its life
// taken when it has one (it breaks with its own effect), else deleted. More than 4 going in one scan is an area
// going away (a fast travel, a save load): not told. Objects this game set off for another's notice are not told
// back. Links, people, animals, enemies, arrows, chests and the like are left out (prop_skip), and copies.
static struct { uint64_t actor; uint32_t key; char name[48]; int seen; } dwatch[512];

static void dwatch_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    const float *me = arg;
    float p[3];
    Slot *cs;
    int ck;
    if (prop_skip(nm) || is_beam(nm) || !strncmp(nm, "Parasail", 8) || copy_of(a, &cs, &ck) || coop_is_resident(a) || old_item(a) ||
        !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    if ((p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]) > 400) return;
    for (int i = 0; i < 32; i++) if (my_arrows[i].actor == a) return;
    int free = -1;
    for (int i = 0; i < 512; i++) {
        if (dwatch[i].actor == a) { dwatch[i].seen = dtick; return; }
        if (free < 0 && !dwatch[i].actor) free = i;
    }
    if (free < 0) return;
    dwatch[free].actor = a, dwatch[free].key = key & ~COOP_OBJ_KEYED, dwatch[free].seen = dtick;
    snprintf(dwatch[free].name, sizeof dwatch[free].name, "%s", nm);
}
static void destroy_watch(uint64_t me) {
    if (++dtick % 10) return;
    float p[3];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    // A save load or a warp unloads the area over several scans: when this Link's actor changes or it moves more than
    // 30 m between scans, what is noted is forgotten and nothing is told for 3 s.
    static uint64_t last_me;
    static float last_p[3];
    static int quiet_until;
    float jump = (p[0] - last_p[0]) * (p[0] - last_p[0]) + (p[1] - last_p[1]) * (p[1] - last_p[1]) + (p[2] - last_p[2]) * (p[2] - last_p[2]);
    if (me != last_me || jump > 30 * 30) {
        memset(dwatch, 0, sizeof dwatch);
        quiet_until = dtick + 180;
    }
    last_me = me, memcpy(last_p, p, 12);
    void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
    CoopObj gone[COOP_OBJS_PER_PACKET];
    int ng = 0, going = 0;
    for (int i = 0; i < 512; i++) {   // the ones noted before: still there?
        if (!dwatch[i].actor) continue;
        int32_t st = 0;
        char nm[48];
        int read = coop_rd(dwatch[i].actor + COOP_ACTOR_STATE, &st, 4);
        int same = read && !strcmp(coop_actor_name(dwatch[i].actor, nm, sizeof nm), dwatch[i].name);
        if (same && st >= 2 && st <= 6) continue;
        if (same && st >= 7 && st <= 11) {
            going++;
            int ours = 0;
            static struct { uint32_t key; int at; } dsent[32];   // told already (an actor going away was seen twice)
            static int dsent_n;
            for (int k = 0; k < 64; k++) ours |= dkilled[k] == dwatch[i].actor;
            for (int k = 0; k < 32; k++) ours |= dsent[k].key == dwatch[i].key && dtick - dsent[k].at < 120;
            if (!ours) dsent[dsent_n % 32].key = dwatch[i].key, dsent[dsent_n % 32].at = dtick, dsent_n++;
            if (!ours && ng < COOP_OBJS_PER_PACKET) {
                CoopObj *o = &gone[ng++];
                memset(o, 0, sizeof *o);
                o->id = COOP_OBJ_KEYED | dwatch[i].key;
                snprintf(o->name, sizeof o->name, "%s", dwatch[i].name);
                coop_rd(dwatch[i].actor + COOP_ACTOR_POS, o->pos, 12);
            }
        }
        dwatch[i].actor = 0;
    }
    coop_actors_all(dwatch_fn, p);
    for (int i = 0; i < 512; i++) if (dwatch[i].actor && dtick - dwatch[i].seen > 60) dwatch[i].actor = 0;   // out of reach
    if (!ng || going > 4 || dtick < quiet_until) return;   // more at once: an area going away, not a blast
    uint8_t pk[4 + COOP_OBJS_PER_PACKET * sizeof(CoopObj)];
    uint16_t h[2] = {(uint16_t)ng, 3};
    memcpy(pk, h, 4), memcpy(pk + 4, gone, ng * sizeof(CoopObj));
    coop_net_objs_send(pk, 4 + ng * (int)sizeof(CoopObj));
    for (int i = 0; i < ng; i++) rt_log("coop: %s (key %08x) was destroyed here; the others are told", gone[i].name, gone[i].id & ~COOP_OBJ_KEYED);
}
// Another game's notice: its placed objects destroyed there go here too.
static void destroy_apply(Cpu *c, const CoopObj *o, int from) {
    KeyFind kf = {0};
    kf.key[0] = o->id & ~COOP_OBJ_KEYED, kf.name[0] = o->name, kf.n = 1;
    void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
    coop_actors_all(keyfind_fn, &kf);
    uint64_t a = kf.actor[0];
    char nm[48];
    // (never a Link's gear or a stand-in's, nor one of a name their gear has: a fused stick deleted in the other game
    // was told as destroyed, and the stand-in's held stick of that name broke here; a play session, 2026-11-08)
    if (!a || strcmp(coop_actor_name(a, nm, sizeof nm), o->name) || coop_is_resident(a) || !strncmp(nm, "Parasail", 8) || fuse_on_held(a)) {
        rt_log("coop: player %d destroyed %s (key %08x): not here (or one not to destroy)", from, o->name, o->id & ~COOP_OBJ_KEYED);
        return;
    }
    int coop_life_of(uint64_t a);
    void coop_set_life(uint64_t a, int v);
    int life = coop_life_of(a);
    if (life > 0) coop_set_life(a, 0);
    else coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, COOP_DELETE_REASON, 0, 0);
    dkilled[dkilled_n++ % 64] = a;
    rt_log("coop: player %d destroyed %s (key %08x): set off here too (%s)", from, o->name, o->id & ~COOP_OBJ_KEYED, life > 0 ? "life taken" : "deleted");
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
                kf.key[kf.n] = slots[i].objs_want[j].id & ~COOP_OBJ_KEYED, kf.actor[kf.n] = 0, kf.name[kf.n] = slots[i].objs_want[j].name, kf.n++;
            }
    }
    // (props_hold runs after this tick's reports, at the end: run first, it put each reported object at the place of
    // the report before, and the new report was then dropped as the same world time; code review, 2026-11-06)
    if (!kf.n) { props_hold(c); return; }
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
                static int missing_next;   // (a ring: once full, each new key took slot 0, code review 2026-11-06)
                missing_key[missing_next++ % 64] = kf.key[i];
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
    props_hold(c);
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
    // TOTK_COOP_TEST_PROPS=destroy: at 32 s the first game deletes the rock it lifted (as a blast would break it); the
    // other game should destroy its own (destroy_watch).
    static int destroyed;
    if (!destroyed && obj && now > 32 && coop_net_id() == 1 && !strcmp(cfg_get("TOTK_COOP_TEST_PROPS"), "destroy")) {
        coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, obj, COOP_DELETE_REASON, 0, 0);
        destroyed = 1;
        rt_log("coop: test: props: the lifted rock (key %08x) is deleted here", obj_key);
    }
    // TOTK_COOP_TEST_PROPS=blast: at 32 s the other game takes its copy's life (and lets it run its own update): does
    // a BombFruit's copy go off? This Link's life (it stands by it) is logged before and after.
    static int blasted;
    if (!blasted && now > 32 && coop_net_id() != 1 && !strcmp(cfg_get("TOTK_COOP_TEST_PROPS"), "blast"))
        for (int i = 0; i < COOP_MAX_PLAYERS && !blasted; i++)
            for (int k = 0; k < COOP_OBJS && !blasted; k++)
                if (slots[i].mir[k].state == 3 && !strcmp(slots[i].mir[k].name, "BombFruit")) {
                    int coop_life_of(uint64_t a);
                    void coop_set_life(uint64_t a, int v);
                    int before = coop_life_of(slots[i].mir[k].actor);
                    slots[i].mir[k].blowing = 1;
                    coop_set_life(slots[i].mir[k].actor, 0);
                    blasted = 1;
                    rt_log("coop: test: props: the copy of BombFruit %u: life %d -> 0; this Link's life %d", slots[i].mir[k].id, before,
                           coop_life_of(me));
                }
    static int after;
    if (blasted && !after && now > 35) after = 1, rt_log("coop: test: props: 3 s later this Link's life is %d", ({ int coop_life_of(uint64_t a); coop_life_of(me); }));
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
// Test (TOTK_COOP_TEST_PROXYARROWS=1): every 30 ticks, each actor (the reference pool: at rest too) with "Arrow" or a fused material in its name
// within 2 m of a co-op Link here: whether it is one of this game's copies, its state, how far from the Link. For
// the one arrow another player could still take at the proxy's bow (a play session, 2026-11-05).
typedef struct { float at[3]; int n; } ProxyArrowScan;
static void proxy_arrow_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    ProxyArrowScan *ps = arg;
    (void)key;
    if (!strstr(nm, "Arrow") && strcmp(nm, "BombFruit")) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float d2 = (p[0] - ps->at[0]) * (p[0] - ps->at[0]) + (p[1] - ps->at[1]) * (p[1] - ps->at[1]) + (p[2] - ps->at[2]) * (p[2] - ps->at[2]);
    if (d2 > 4) return;
    Slot *cs;
    int ck, mine = 0;
    for (int i = 0; i < 32; i++) mine |= my_arrows[i].actor == a;
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    if (ps->n++ < 8)
        rt_log("coop: test: proxy arrows %.2f s: %s %llx %s, state %d, %.2f m from the co-op Link", frame_step_world_us() / 1e6, nm,
               (unsigned long long)a, copy_of(a, &cs, &ck) ? "(a copy)" : mine ? "(this Link's)" : "(neither: the proxy's own?)", st, sqrtf(d2));
}
static void proxy_arrows_test(void) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_PROXYARROWS") != NULL;
    if (!on || ++t % 30) return;
    void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c: at rest too
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        ProxyArrowScan ps = {{0}, 0};
        if (!slots[i].actor || !coop_rd(slots[i].actor + COOP_ACTOR_POS, ps.at, 12)) continue;
        coop_actors_all(proxy_arrow_fn, &ps);
    }
}
// Test (TOTK_COOP_TEST_BONEARROW=1): every 30 ticks while this Link has an arrow within 2.5 m, which body bone is
// nearest it when a bone's world place is taken as the model's world matrix (model +0x1f8, 3x4 rows) applied to the
// bone's translation in skeleton +0x20 (4x4, translation at 12..14); and the arrow's place in that bone's frame
// (columns 0..2, 4..6, 8..10 as its axes). For putting a nocked arrow's copy on the proxy's hand.
static void bone_arrow_test(uint64_t me) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_BONEARROW") != NULL;
    if (!on || ++t % 30) return;
    uint64_t arrow = 0;
    for (int i = 0; i < 32 && !arrow; i++) if (my_arrows[i].actor && my_arrows[i].mine && arrow_tick - my_arrows[i].seen < 2) arrow = my_arrows[i].actor;
    float a[3], lp[3];
    if (!arrow || !coop_rd(arrow + COOP_ACTOR_POS, a, 12) || !coop_rd(me + COOP_ACTOR_POS, lp, 12)) return;
    if ((a[0] - lp[0]) * (a[0] - lp[0]) + (a[1] - lp[1]) * (a[1] - lp[1]) + (a[2] - lp[2]) * (a[2] - lp[2]) > 2.5f * 2.5f) return;
    uint64_t sk = body_skeleton(me), wb = sk ? coop_rd_ptr(sk + 0x20) : 0;
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x8) + 0x28);
    uint16_t nb = 0;
    static float rec[128][16];
    float w[12];
    if (!wb || !model || !coop_rd(sk + 0x48, &nb, 2) || nb == 0 || nb > 128 || !coop_rd(wb, rec, (size_t)nb * 64) ||
        !coop_rd(model + 0x1f8, w, sizeof w)) return;
    int best = -1;
    float bd = 1e9f, bw[3] = {0};
    for (int b = 0; b < nb; b++) {
        float q[3];
        for (int i = 0; i < 3; i++) q[i] = w[i * 4] * rec[b][12] + w[i * 4 + 1] * rec[b][13] + w[i * 4 + 2] * rec[b][14] + w[i * 4 + 3];
        float d = (q[0] - a[0]) * (q[0] - a[0]) + (q[1] - a[1]) * (q[1] - a[1]) + (q[2] - a[2]) * (q[2] - a[2]);
        if (d < bd) bd = d, best = b, memcpy(bw, q, 12);
    }
    rt_log("coop: test: bone arrow %.2f s: nearest bone %d, %.3f m (arrow %.2f %.2f %.2f, bone %.2f %.2f %.2f, Link %.2f %.2f %.2f)",
           frame_step_world_us() / 1e6, best, sqrtf(bd), a[0], a[1], a[2], bw[0], bw[1], bw[2], lp[0], lp[1], lp[2]);
}
// Test (TOTK_COOP_TEST_IMPACT=1): what a fused arrow makes where it hits. This Link's fused materials are watched
// each tick (their place, whether they are still there: state 2..6 and the same name); when one goes, the actors
// within 4 m of its last place are noted, and for 1.5 s after every actor within 4 m that was not among them is
// logged (name, place, state). For the arrows' effects on the other screens (a bomb arrow's blast; the tester's list).
typedef struct { float at[3]; uint64_t known[256]; int nk, logged; } ImpactScan;
static void impact_known_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    ImpactScan *is = arg;
    (void)nm, (void)key;
    float p[3];
    if (is->nk >= 256 || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    if ((p[0] - is->at[0]) * (p[0] - is->at[0]) + (p[1] - is->at[1]) * (p[1] - is->at[1]) + (p[2] - is->at[2]) * (p[2] - is->at[2]) < 16)
        is->known[is->nk++] = a;
}
static void impact_new_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    ImpactScan *is = arg;
    (void)key;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    if ((p[0] - is->at[0]) * (p[0] - is->at[0]) + (p[1] - is->at[1]) * (p[1] - is->at[1]) + (p[2] - is->at[2]) * (p[2] - is->at[2]) >= 16) return;
    for (int i = 0; i < is->nk; i++) if (is->known[i] == a) return;
    if (is->nk < 256) is->known[is->nk++] = a;   // logged once
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    if (is->logged++ < 40)
        rt_log("coop: test: impact %.2f s: new %s %llx at %.2f %.2f %.2f, state %d", frame_step_world_us() / 1e6, nm, (unsigned long long)a,
               p[0], p[1], p[2], st);
}
// Test (TOTK_COOP_TEST_LINKHIT=1): the hit record in each Link's damage component ([actor+0x228]+0x2f8, +0x1a0..;
// re-notes "The hit's mark on the device": +0x1a4 state, +0x1c8 the attacker's id, +0x1d0 flags, +0x1d8 type, +0x1dc
// power, +0x1fc the hit place) for this Link and each co-op Link, logged when +0x1a4 turns nonzero, with the life: what
// an enemy's hit leaves on a frozen proxy (its update, which takes the record, does not run) against this Link's.
int coop_life_of(uint64_t a);   // coop_world.c
static void link_hit_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_LINKHIT") != NULL;
    if (!on) return;
    static uint32_t last[COOP_MAX_PLAYERS + 1];
    static int said;
    for (int i = 0; i <= COOP_MAX_PLAYERS && said < 60; i++) {
        uint64_t a = i == COOP_MAX_PLAYERS ? me : slots[i].actor;
        if (!a || (i < COOP_MAX_PLAYERS && slots[i].state != LIVE)) continue;
        uint64_t dc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x2f8);
        uint32_t w[0x60 / 4];
        if (!dc || !coop_rd(dc + 0x1a0, w, sizeof w)) continue;
        uint32_t stt = w[1];
        if (i == COOP_MAX_PLAYERS) {   // this Link: its whole damage component frame to frame, what a real hit changes
            static uint32_t prev[0x2000 / 4], cur[0x2000 / 4];
            static int have, shown;
            if (coop_rd(dc, cur, sizeof cur)) {
                if (have && stt && stt != last[i] && shown++ < 4) {
                    char line[1600];
                    int m = 0;
                    for (int k = 0; k < 0x2000 / 4 && m < 1500; k++)
                        if (cur[k] != prev[k] && (k * 4 < 0x1a0 || k * 4 >= 0x300))
                            m += snprintf(line + m, sizeof line - m, " +%#x:%x>%x", k * 4, prev[k], cur[k]);
                    line[m] = 0;
                    rt_log("coop: test: linkhit this Link's damage component outside the record:%s", m ? line : " nothing");
                }
                memcpy(prev, cur, sizeof cur), have = 1;
            }
        }
        if (stt && stt != last[i]) {
            float pw, hp[3];
            memcpy(&pw, &w[0x3c / 4], 4), memcpy(hp, &w[0x5c / 4], 12);
            rt_log("coop: test: linkhit %s at %.3f s: state %u, attacker id %u, flags %#x, type %u, power %u (%.2f), place %.2f %.2f %.2f; life %d",
                   i == COOP_MAX_PLAYERS ? "this Link" : "a co-op Link", frame_step_world_us() / 1e6, stt, w[0x28 / 4], w[0x30 / 4], w[0x38 / 4],
                   w[0x3c / 4], pw, hp[0], hp[1], hp[2], coop_life_of(a));
            said++;
            if (said <= 6) {   // the whole record, and where the attacker's id and reference are on the attacker
                uint32_t r[0x160 / 4];
                if (coop_rd(dc + 0x1a0, r, sizeof r)) {
                    char line[1400];
                    int m = 0;
                    for (int k = 0; k < 0x160 / 4; k++) m += snprintf(line + m, sizeof line - m, "%s%x", k % 8 ? " " : " |", r[k]);
                    rt_log("coop: test: linkhit record:%s", line);
                    uint64_t ref;
                    memcpy(&ref, &r[0x20 / 4], 8);
                    uint64_t ra = coop_rd_ptr(ref + 0x40);   // a reference object's actor (+0x40) and id (+0x48), as Player#2's
                    uint32_t rid = 0;
                    coop_rd(ref + 0x48, &rid, 4);
                    char an[48] = "";
                    if (ra) coop_actor_name(ra, an, sizeof an);
                    rt_log("coop: test: linkhit attacker reference %llx: actor %llx '%s', id %u", (unsigned long long)ref, (unsigned long long)ra, an, rid);
                }
            }
        }
        last[i] = stt;
    }
}
static void impact_test(void) {
    static int on = -1;
    static struct { uint64_t actor; char name[48]; float last[3]; } w[8];
    static uint64_t done[32];   // materials already seen going (still in my_arrows for a while after)
    static int ndone;
    static ImpactScan is;
    static int64_t until;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_IMPACT") != NULL;
    if (!on) return;
    void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c: at rest too
    int64_t now = frame_step_world_us();
    for (int i = 0; i < 32; i++)   // fused materials of this Link's arrows, watched
        if (my_arrows[i].actor && my_arrows[i].fused_to) {
            int seen_go = 0;
            for (int d = 0; d < 32; d++) seen_go |= done[d] == my_arrows[i].actor;
            if (seen_go) continue;
            int k = 0, free = -1;
            while (k < 8 && w[k].actor != my_arrows[i].actor) { if (free < 0 && !w[k].actor) free = k; k++; }
            if (k == 8 && free >= 0) k = free, w[k].actor = my_arrows[i].actor, coop_actor_name(w[k].actor, w[k].name, sizeof w[k].name);
        }
    for (int k = 0; k < 8; k++) {
        if (!w[k].actor) continue;
        int32_t st = 0;
        char nm[48];
        int gone = !coop_rd(w[k].actor + COOP_ACTOR_STATE, &st, 4) || st < 2 || st > 6 || strcmp(coop_actor_name(w[k].actor, nm, sizeof nm), w[k].name);
        if (!gone) { coop_rd(w[k].actor + COOP_ACTOR_POS, w[k].last, 12); continue; }
        rt_log("coop: test: impact %.2f s: the fused %s went (state %d) at %.2f %.2f %.2f; wall %llu ms", now / 1e6, w[k].name, st, w[k].last[0],
               w[k].last[1], w[k].last[2], (unsigned long long)(plat_now_ns() / 1000000));
        memset(&is, 0, sizeof is);
        memcpy(is.at, w[k].last, 12);
        is.known[is.nk++] = w[k].actor;
        coop_actors_all(impact_known_fn, &is);
        until = now + 1500000;
        done[ndone++ % 32] = w[k].actor;
        w[k].actor = 0;
    }
    if (now < until) coop_actors_all(impact_new_fn, &is);
}
// Test (TOTK_COOP_TEST_GLIDELOG=1): every 15 ticks, where this Link's paraglider is in the Link's frame, its state and
// its model's +0x240 byte, open or not: what tells an open glider from a closed one.
static void glide_log_fn(uint64_t a, const char *nm, uint32_t key, void *arg) {
    (void)key;
    if (strncmp(nm, "Parasail", 8)) return;
    uint64_t me = *(uint64_t *)arg;
    float l[3];
    if (!item_local(me, a, l)) return;
    int32_t st = 0;
    coop_rd(a + COOP_ACTOR_STATE, &st, 4);
    uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x8) + 0x28);
    uint8_t mb[8] = {0};
    if (model) coop_rd(model + 0x240, mb, 8);
    rt_log("coop: test: glide %.2f s: %s %llx at %.2f %.2f %.2f in the Link's frame, state %d, model +0x240 %02x %02x %02x %02x", frame_step_world_us() / 1e6,
           nm, (unsigned long long)a, l[0], l[1], l[2], st, mb[0], mb[1], mb[2], mb[3]);
}
static void glide_log(uint64_t me) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_GLIDELOG") != NULL;
    if (!on || ++t % 15) return;
    void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
    coop_actors_all(glide_log_fn, &me);
}
// Test (TOTK_COOP_TEST_NEWNEAR=1): every actor that shows up within 5 m of this Link (in the live list: running its
// own calc), once, with its name and whether it is left out of the world objects (prop_skip) or a copy. For finding
// what the Master Sword's beam and the Earthwake Technique make (the tester's list, 2026-11-05). At most 400 lines.
static void newnear_fn(uint64_t a, void *arg) {
    static uint64_t seen[2048];
    static int n, logged;
    const float *me = arg;
    for (int i = 0; i < n; i++) if (seen[i] == a) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float d2 = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
    if (d2 > 25) return;
    seen[n++ % 2048] = a;
    if (n > 2048) n = 2048;
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    Slot *cs;
    int ck;
    if (logged++ < 400)
        rt_log("coop: test: new near %.2f s: %s %llx, %.2f m from this Link%s%s", frame_step_world_us() / 1e6, nm, (unsigned long long)a,
               sqrtf(d2), prop_skip(nm) ? " (left out)" : "", copy_of(a, &cs, &ck) ? " (a copy)" : "");
}
// Test (with TOTK_COOP_TEST_BEAMLOG): for 30 ticks after this Link's beam ends, every actor that shows up within 5 m
// of where it was last, as newnear_test logs them. For finding what makes the burst where a beam hits (its copy shows
// none, a play session, 2026-11-06). In that session a PlayerBeam woke 0.83 m from each end; taking any beam that
// wakes within 2 m of an end in the 20 ticks after as this Link's matched none in the next one, so it is not done.
static void beam_end_watch(const float at[3]) {
    memcpy(bend_at, at, 12), bend_tick = prop_tick;
    if (!cfg_get("TOTK_COOP_TEST_BEAMLOG")) return;
    bend_ticks = 30;
    rt_log("coop: test: this Link's beam ended at %.2f %.2f %.2f", at[0], at[1], at[2]);
}
static void beam_end_tick(void) {
    // an ended beam sleeps, so it drops out of the live list: ended once it has not been seen for 2 ticks
    for (int i = 0; i < PROPS_N; i++)
        if (props[i].actor && props[i].made && is_beam(props[i].name) && prop_tick - props[i].seen >= 2 && !props[i].ended)
            props[i].ended = 1, beam_end_watch(props[i].last);
    if (bend_ticks <= 0) return;
    bend_ticks--;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(newnear_fn, bend_at);
}
static void newnear_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_NEWNEAR") != NULL;
    float p[3];
    if (!on || !coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(newnear_fn, p);
}
// Test (TOTK_COOP_TEST_BEAM=1, with -FakeGearA Weapon_Sword_070): at 20 s this Link's life is set to its most, so a
// Master Sword swing makes its beam.
static void beam_test(uint64_t me) {
    static int on = -1, done;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_BEAM") != NULL;
    if (!on || done || frame_step_world_us() < 20000000) return;
    done = 1;
    int coop_life_max_of(uint64_t a);
    void coop_set_life(uint64_t a, int v);
    int m = coop_life_max_of(me);
    if (m > 0) coop_set_life(me, m);
    rt_log("coop: test: beam: this Link's life set to %d", m);
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
    // Marked here first, all of them: marked only by the loop that stops at the budget, the ones after it went for
    // gone, unsaid, and were sent as new the next tick (code review, 2026-11-06).
    for (int i = 0; i < n; i++)
        for (int j = 0; j < objs_sent_n; j++)
            if (objs_sent[j].id == cur[i].id) { objs_sent[j].here = 1; break; }
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
        if (budget <= 0) { j++; continue; }   // said when there is room (forgotten unsaid, its copy stayed)
        CoopObj gone = {.id = objs_sent[j].id};
        OBJS_PUT(&gone);
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
        if (h[1] == 3) {   // another player's placed objects destroyed there (destroy_watch)
            for (int e = 0; e < h[0]; e++) {
                CoopObj o;
                memcpy(&o, pk + 4 + e * sizeof(CoopObj), sizeof o);
                o.name[sizeof o.name - 1] = 0;
                destroy_apply(c, &o, from);
            }
            continue;
        }
        if (h[1] == 1) {   // another player picked up objects: this game's own ones are deleted (take_note)
            for (int e = 0; e < h[0]; e++) {
                CoopObj o;
                memcpy(&o, pk + 4 + e * sizeof(CoopObj), sizeof o);
                if (o.flags != coop_net_id()) continue;
                for (int i = 0; i < 32; i++)   // an arrow of this Link's (picked up where it stuck)
                    if (my_arrows[i].actor && my_arrows[i].mine && my_arrows[i].id == o.id && actor_is(my_arrows[i].actor, my_arrows[i].name)) {
                        rt_log("coop: player %d picked up my arrow (number %u); mine is deleted", from, o.id);
                        coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, my_arrows[i].actor, COOP_DELETE_REASON, 0, 0);
                        my_arrows[i].actor = 0;
                    }
                for (int i = 0; i < PROPS_N; i++)   // one it made (by number) or a placed one it moved (by key)
                    // (never a resident actor or the paraglider, which both Links here use: deleting the paraglider for
                    // a copy of it that went away by itself in the other game took this game down, glide.txt)
                    // (and only while it is still that object: an entry can outlive its actor, code review 2026-11-06)
                    if (props[i].actor && !coop_is_resident(props[i].actor) && strncmp(props[i].name, "Parasail", 8) &&
                        actor_is(props[i].actor, props[i].name) &&
                        ((props[i].made && props[i].id == o.id) ||
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
        if (!s) {
            static int said;
            if (cfg_get("TOTK_COOP_TEST_ABLOG") && said++ < 5) rt_log("coop: ablog: objects from player %d, who has no slot here", from);
            continue;
        }
        for (int e = 0; e < h[0]; e++) {
            CoopObj o;
            memcpy(&o, pk + 4 + e * sizeof(CoopObj), sizeof o);
            if (!o.id) continue;
            o.name[sizeof o.name - 1] = 0;
            {   // TOTK_COOP_TEST_ABLOG (coop_autobuild.c): the first objects heard of
                static int ab = -1, said;
                if (ab < 0) ab = cfg_get("TOTK_COOP_TEST_ABLOG") != NULL;
                if (ab && said < 40 && o.name[0]) said++, rt_log("coop: ablog: from player %d: object %08x %s flags %x", from, o.id, o.name, o.flags);
            }
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
                !pickable(slots[i].mir[k].name) && !(slots[i].mir[k].flags & COOP_OBJ_ONLINK) && strcmp(slots[i].mir[k].name, "PlayerBeam")) {
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
// ---- an object's number across the games, for the glue sync (coop_glue.c). An object this game sends (made here, or
// a placed one it moves): this player and its number (made) or COOP_OBJ_KEYED | its key. A copy of another player's:
// that player and the number it sends. A placed one another player moves: that player and its key. Any other actor
// with a name: COOP_OBJ_KEYED | its key from where it is (coop_world.c coop_actor_world_key: the key every game gives a
// resting placed object), owner this player. 0 for none.
// Run on the co-op tick only (the tables are the tick's).
// For coop_bonds.c (on the co-op tick): the objects this game sends (made here, or placed ones it moves) whose actor is
// still that object; whether an actor is a copy of another player's object; and a copy made again (deleted here and
// wanted anew, as a copy far from every player is), which parts it from what this game bonded it to.
int coop_own_sent_objects(uint64_t *out, int max) {
    int n = 0;
    for (int e = 0; e < PROPS_N && n < max; e++)
        if (props[e].actor && (props[e].made || props[e].touched) && actor_is(props[e].actor, props[e].name)) out[n++] = props[e].actor;
    return n;
}
int coop_is_copy(uint64_t a) {
    Slot *cs;
    int ck;
    return copy_of(a, &cs, &ck);
}
void coop_copy_remake(Cpu *c, uint64_t a) {
    Slot *s;
    int k;
    if (!copy_of(a, &s, &k)) return;
    if (copy_alive(s, k)) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, a, COOP_DELETE_REASON, 0, 0), set_driven(a);
    s->mir[k].state = 1, s->mir[k].actor = 0, s->mir[k].ticks = 0;
}
// For coop_bonds.c's claims: what this Link rides (another player's object: its owner and number), what another
// player's Link rides, and an object of this game's moved to where the player running its contraption has it.
int coop_ride_now(int *owner, uint32_t *id) {
    if (!ride_owner_now || ride_owner_now == COOP_RIDE_MOUNT) return 0;
    *owner = ride_owner_now, *id = ride_id_now;
    return 1;
}
int coop_player_rides(int net_id, int *owner, uint32_t *id) {
    Slot *s = slot_for(SRC_NET, net_id);
    if (!s || s->state != LIVE || !s->ride_owner || s->ride_owner == COOP_RIDE_MOUNT) return 0;
    *owner = s->ride_owner, *id = s->ride_id;
    return 1;
}
void coop_obj_force(Cpu *c, uint64_t a, const float pos[3], const float q[4]) { force_to(c, a, pos, q); }
int coop_player_pos(int net_id, float out[3]) {   // where another player's Link (its stand-in) is here
    Slot *s = slot_for(SRC_NET, net_id);
    return s && s->state == LIVE && s->actor && coop_rd(s->actor + COOP_ACTOR_POS, out, 12);
}
void coop_mat_quat(const float r[9], float q[4]) { mat_quat(r, q); }
int coop_obj_ident(uint64_t a, int *owner, uint32_t *id) {
    if (!a) return 0;
    for (int e = 0; e < PROPS_N; e++)
        if (props[e].actor == a && (props[e].made || props[e].touched)) {
            *owner = coop_net_id();
            *id = props[e].made ? props[e].id : COOP_OBJ_KEYED | (props[e].key & ~COOP_OBJ_KEYED);
            return 1;
        }
    Slot *cs;
    int ck;
    if (copy_of(a, &cs, &ck)) { *owner = cs->net_id, *id = cs->mir[ck].id; return 1; }
    for (int k = 0; k < 128; k++)
        if (key_found[k].actor == a && key_found[k].last) { *owner = key_found[k].from, *id = COOP_OBJ_KEYED | key_found[k].key; return 1; }
    uint32_t coop_actor_world_key(uint64_t a);   // coop_world.c
    uint32_t key = coop_actor_world_key(a);
    if (!key) return 0;
    *owner = coop_net_id(), *id = COOP_OBJ_KEYED | (key & ~COOP_OBJ_KEYED);
    return 1;
}
// The actor here that coop_obj_ident's (owner, id) names in its owner's game, with that name; 0 if none (yet). A key
// not found among the objects known here scans every actor (coop_actors_all) at most once a second.
uint64_t coop_obj_find(int owner, uint32_t id, const char *name) {
    if (!id) return 0;
    if (!(id & COOP_OBJ_KEYED) && owner == coop_net_id()) {
        for (int e = 0; e < PROPS_N; e++)
            if (props[e].actor && props[e].made && props[e].id == id && actor_is(props[e].actor, name)) return props[e].actor;
        return 0;
    }
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {   // a copy here of that player's object (a keyed one too, where this game lacks it)
        if (slots[i].state == FREE || slots[i].source != SRC_NET) continue;
        if (!(id & COOP_OBJ_KEYED) && slots[i].net_id != owner) continue;
        for (int k = 0; k < COOP_OBJS; k++)
            if (slots[i].mir[k].state == 3 && slots[i].mir[k].id == id && copy_alive(&slots[i], k)) return slots[i].mir[k].actor;
    }
    if (!(id & COOP_OBJ_KEYED)) return 0;
    uint32_t key = id & ~COOP_OBJ_KEYED;
    for (int e = 0; e < PROPS_N; e++)
        if (props[e].actor && (props[e].key & ~COOP_OBJ_KEYED) == key && actor_is(props[e].actor, name)) return props[e].actor;
    uint64_t a = key_cached(key, name);
    if (a) return a;
    static int64_t last_scan = -1000000000;
    int64_t now = frame_step_world_us();
    if (now - last_scan < 1000000) return 0;
    last_scan = now;
    static KeyFind kf;
    kf.n = 1, kf.key[0] = key, kf.actor[0] = 0, kf.name[0] = name;
    void coop_actors_all(void (*fn)(uint64_t, const char *, uint32_t, void *), void *arg);   // coop_world.c
    coop_actors_all(keyfind_fn, &kf);
    return kf.actor[0];
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
        if (mv > 1e-6f && !nocarry && !coop_bonds_claimed(on->actor)) {   // (a claimed copy carries it by its own physics)
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
// The reference object of an actor's EquipmentUser row (0 the weapon), or 0 (coop_world.c: a stand-in's sword's
// handle in a contact it sends on).
uint64_t coop_eq_row_ref(uint64_t actor, int row) {
    uint64_t eq = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x230);
    return eq && row >= 0 && row < 24 ? coop_rd_ptr(eq + GEAR_TABLE + 0x18 * (uint64_t)row) : 0;
}
// The reference object of the first EquipmentUser row holding a bow (gear_kind 1), or 0: a stand-in's bow, the
// weapon of an arrow hit its owner's game sends on (coop_world.c, ce_take).
uint64_t coop_eq_bow_ref(uint64_t actor) {
    uint64_t eq = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x230);
    for (int row = 0; eq && row < 24; row++) {
        uint64_t ref = coop_rd_ptr(eq + GEAR_TABLE + 0x18 * (uint64_t)row);
        char nm[32];
        coop_actor_name(ref ? coop_rd_ptr(ref + 0x40) : 0, nm, sizeof nm);
        if (nm[0] && gear_kind(nm) == 1) return ref;
    }
    return 0;
}
// An item of a Link's (in this Link's or a co-op Link's EquipmentUser) or a copy of another player's object: not an
// enemy's weapon (coop_world.c, enemy puppets).
int coop_is_players_item(uint64_t a) {
    Slot *s;
    int k;
    if (!a || copy_of(a, &s, &k)) return a != 0;
    uint64_t links[COOP_MAX_PLAYERS + 1];
    int n = 0;
    links[n++] = coop_player_actor();
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) if (slots[i].actor && slots[i].state == LIVE) links[n++] = slots[i].actor;
    for (int i = 0; i < n; i++) {
        uint64_t eq = links[i] ? eq_of_actor(links[i]) : 0;
        for (int row = 0; eq && row < 24; row++) {
            uint64_t ref = coop_rd_ptr(eq + GEAR_TABLE + 0x18 * (uint64_t)row);
            if (ref && coop_rd_ptr(ref + 0x40) == a) return 1;
        }
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
    static int said_ss;
    if (said_ss++ < 3) rt_log("coop: this Link's shield is out of hand under it (%.2f %.2f %.2f in its frame): sent as placed", l[0], l[1], l[2]);
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

// ---- Items in the stand-in's hands. The PlayerPuppet stand-in's weapon, bow and shield are taken out with the
// game's in-hand calls (setEquipmentInHand), and its EquipmentUser then reads exactly as its owner's (weapon row,
// attach record +0x570, in-hand bytes +0x4620: TOTK_COOP_TEST_EQCMP), but the model push keeps the item on its
// back bone: on a Link it is the Player component, which the stand-in has not, that holds it to the hand (the
// sword stayed still 0.7 m up at its side through every swing, TOTK_COOP_TEST_WEAPLOG; the first stand-in, 21165e1,
// did the same, watched live, 2026-11-07). So the owner sends, for each item in hand, the body bone nearest the
// item's model and the model's matrix in that bone's frame, and after the stand-in's model phase each item it holds
// is put at its own same bone times that matrix (actor place, model matrix and push, physics body), as shield_place
// does in a surf. A bone's world matrix: the body skeleton's record (+0x20, 0x40 a bone, columns: axes at 0, 4, 8,
// translation at 12), world-oriented but from an origin of its own, moved so the root bone is at the model's place
// (model +0x1f8, 3x4 rows); taken as model matrix times record, the hand was 11 m from the sword.
// Off unless TOTK_COOP_STANDIN_HOLD=1 (a test): the game places the stand-in's held items itself once its read of this
// Link's own equipment is taken out (coop_spawn.c, main+0x45a460), and placing items by hand on the old co-op Link had
// led to items taken out of hands and fused parts at the wrong size (the tester). Its bones are not found on a PlayerPuppet
// yet either (body_skeleton is a Player's path).
static int hold_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_HOLD"); on = e && *e == '1'; }
    return on;
}
static void mat34_mul(const float a[12], const float b[12], float out[12]) {   // a * b, both 3x4 rows (affine)
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j];
        out[i * 4 + 3] += a[i * 4 + 3];
    }
}
static int mat34_inv(const float m[12], float out[12]) {   // the inverse of an affine 3x4 (any 3x3 part)
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-12f) return 0;
    float r[9] = {(e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det, (f * g - d * k) / det, (a * k - c * g) / det,
                  (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i * 4 + j] = r[i * 3 + j];
        out[i * 4 + 3] = -(r[i * 3 + 0] * m[3] + r[i * 3 + 1] * m[7] + r[i * 3 + 2] * m[11]);
    }
    return 1;
}
static uint64_t model_of(uint64_t a) { return a ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x8) + 0x28) : 0; }
// The actor's body bones' world matrices (3x4 rows) into out; their count, 0 none.
#define HOLD_BONES 600
static int bones_world(uint64_t actor, float (*out)[12]) {
    uint64_t sk = body_skeleton(actor), wb = sk ? coop_rd_ptr(sk + 0x20) : 0, model = model_of(actor);
    uint16_t nb = 0;
    float w[12];
    static __thread float rec[HOLD_BONES][16];
    if (!wb || !model || !coop_rd(sk + 0x48, &nb, 2) || !nb || nb > HOLD_BONES || !coop_rd(wb, rec, (size_t)nb * 64) ||
        !coop_rd(model + 0x1f8, w, sizeof w)) return 0;
    // (the records are world-oriented but from an origin of their own, metres off: moved so the root bone is at the
    // model's place)
    float sh[3] = {w[3] - rec[0][12], w[7] - rec[0][13], w[11] - rec[0][14]};
    for (int b = 0; b < nb; b++)
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) out[b][i * 4 + j] = rec[b][j * 4 + i];
            out[b][i * 4 + 3] = rec[b][12 + i] + sh[i];
        }
    return nb;
}
static void hold_send(uint64_t me, uint64_t eq, NetState *st) {
    memset(st->hold_bone, 0xff, sizeof st->hold_bone);
    memset(st->hold, 0, sizeof st->hold);
    if (!hold_on() || !eq || !(st->flags >> 8 & 7)) return;
    static __thread float bw[HOLD_BONES][12];
    int nb = bones_world(me, bw);
    for (int k = 0; nb && k < 3; k++) {
        uint64_t item = (st->flags >> (8 + k) & 1) ? eq_item(eq, k) : 0, model = model_of(item);
        float m[12], inv[12];
        if (!model || !coop_rd(model + 0x1f8, m, sizeof m)) continue;
        int best = -1;
        float bd = 1e9f;
        for (int b = 0; b < nb; b++) {
            float d = (bw[b][3] - m[3]) * (bw[b][3] - m[3]) + (bw[b][7] - m[7]) * (bw[b][7] - m[7]) + (bw[b][11] - m[11]) * (bw[b][11] - m[11]);
            if (d < bd) bd = d, best = b;
        }
        if (best < 0 || best > 254 || bd > 0.5f * 0.5f || !mat34_inv(bw[best], inv)) {   // (over 0.5 m: not held by a bone)
            static int said_far;
            if (said_far++ < 3) rt_log("coop: this Link's item %d in hand: no bone within 0.5 m (nearest %d, %.2f m); not sent", k, best, sqrtf(bd));
            continue;
        }
        st->hold_bone[k] = (uint8_t)best;
        mat34_mul(inv, m, st->hold[k]);
        static int said[3];
        if (said[k]++ < 3) rt_log("coop: this Link's %s in hand: held by bone %d, %.3f m from it; sent", k == 0 ? "weapon" : k == 1 ? "bow" : "shield",
                                  best, sqrtf(bd));
    }
}
static void hold_place(Cpu *c, Slot *s) {
    if (!hold_on() || !s->actor) return;
    uint64_t eq = eq_of_actor(s->actor);
    static __thread float bw[HOLD_BONES][12];
    int nb = 0;
    for (int k = 0; eq && k < 3; k++) {
        static int why[3][5];   // (each reason it is not placed, logged once)
        if (!(s->out_want >> k & 1) || (k == 2 && s->shield_placed)) continue;
        if (s->hold_bone[k] == 0xff) { if (!why[k][0]++) rt_log("coop: stand-in: item %d in hand: no bone from its owner", k); continue; }
        uint64_t item = eq_item(eq, k), model = model_of(item);
        if (!model) { if (!why[k][1]++) rt_log("coop: stand-in: item %d in hand: no %s", k, item ? "model of its item" : "item"); continue; }
        if (!nb && !(nb = bones_world(s->actor, bw))) {
            if (!why[k][2]++) {
                uint64_t sk = body_skeleton(s->actor), wb = sk ? coop_rd_ptr(sk + 0x20) : 0;
                uint16_t n16 = 0;
                if (sk) coop_rd(sk + 0x48, &n16, 2);
                rt_log("coop: stand-in: item %d in hand: its bones not read (skeleton %llx, bones %llx, count %u, model %llx)", k,
                       (unsigned long long)sk, (unsigned long long)wb, n16, (unsigned long long)model_of(s->actor));
            }
            return;
        }
        if (s->hold_bone[k] >= nb) { if (!why[k][3]++) rt_log("coop: stand-in: item %d in hand: bone %d of %d", k, s->hold_bone[k], nb); continue; }
        float m[12], rot[9], pos[3];
        mat34_mul(bw[s->hold_bone[k]], s->hold[k], m);
        int ok = 1;
        for (int i = 0; i < 12; i++) ok &= m[i] == m[i];
        if (!ok) continue;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) rot[i * 3 + j] = m[i * 4 + j];
            pos[i] = m[i * 4 + 3];
        }
        memcpy((void *)(uintptr_t)(item + COOP_ACTOR_POS), pos, 12);
        memcpy((void *)(uintptr_t)(item + COOP_ACTOR_ROT), rot, 36);
        memcpy((void *)(uintptr_t)(model + 0x1f8), m, sizeof m);
        ST8(model + 0x240, LD8(model + 0x240) | 1);
        coop_guest_call(c, MOD_MAIN_BASE + PROXY_MODEL_PUSH, model, 0, 0, 0);
        coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, item, (uint64_t)(uintptr_t)m, 0, 0);
        static int said[3];
        if (said[k]++ < 3) rt_log("coop: stand-in: slot %d: its %s put in its hand (bone %d)", (int)(s - slots), k == 0 ? "weapon" : k == 1 ? "bow" : "shield",
                                  s->hold_bone[k]);
    }
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
static uint64_t runs_since_ns;   // when this game last came out of a pause (0 while paused)
// How long this game has run since its last pause (a menu, a load), in ms; -1 while paused (coop_world.c: enemies are
// not frozen as copies then).
int coop_runs_for_ms(void) { return runs_since_ns ? (int)((plat_now_ns() - runs_since_ns) / 1000000ull) : -1; }
// ---- the sync audit (tools/coop/sync_audit.py). Once a second, at the turn of a second of plat_now_ns (the
// performance counter, one clock for every game on this PC), each game writes what it shows to logs/audit.jsonl, one
// JSON line: this Link (place, life, gear, dyes, fused materials, hands), each co-op Link as shown here, the objects
// it sends (at their real place) and the copies it shows of others', the placed objects it holds at another game's
// report, the enemies near any Link, the time of day and weather. The script pairs the two games' lines by time and
// lists what does not match. TOTK_COOP_AUDIT=0: none.
// The material fused to this Link's shield. A weapon is made again with its material when one is fused (the weapon
// create's attachment, coop_players_gear); a shield is not: the material is attached to the shield there is, and its
// create says nothing (TOTK_COOP_TEST_EQLOG, a play session, 2026-11-06: a wheel and a plank fused to a shield went
// to the other game as loose objects). Taken here as the actor that stays within 0.8 m of the shield for 5 looks in a
// row (every 10 ticks), not a Link's, a weapon or armor; none for 3 looks, the shield has none. An actor nearer to the
// Link's weapon or bow than to its shield is that item's fused part: with the sword put away on the back, its rock lay
// within 0.8 m of the shield and was taken and sent as the shield's (a play session, 2026-11-08). The item's own
// place is not enough for that (the sword's is at its hilt, the rock at its tip, nearer the shield): the actor of the
// weapon's or bow's material name nearest that item, within 2 m, is skipped as well (shield_scan_skip).
typedef struct { float at[3]; uint64_t best; float best_d2; uint64_t shield, others[2], skip[2]; } ShieldScan;
typedef struct { float at[3]; const char *name; uint64_t best; float best_d2; } NamedNear;
static void named_near_fn(uint64_t a, void *arg) {
    NamedNear *n = arg;
    char nm[48];
    float p[3];
    coop_actor_name(a, nm, sizeof nm);
    if (strcmp(nm, n->name) || !coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float d2 = (p[0] - n->at[0]) * (p[0] - n->at[0]) + (p[1] - n->at[1]) * (p[1] - n->at[1]) + (p[2] - n->at[2]) * (p[2] - n->at[2]);
    if (d2 < n->best_d2) n->best = a, n->best_d2 = d2;
}
static void shield_scan_skip(ShieldScan *ss, char (*fuse)[32]) {   // fuse: weapon's and bow's material names
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    for (int k = 0; k < 2; k++) {
        NamedNear n = {.name = fuse[k], .best_d2 = 4.0f};
        if (!ss->others[k] || !fuse[k][0] || !coop_rd(ss->others[k] + COOP_ACTOR_POS, n.at, 12)) continue;
        coop_actors_live(named_near_fn, &n);
        ss->skip[k] = n.best;
    }
}
static void shield_fuse_fn(uint64_t a, void *arg) {
    ShieldScan *ss = arg;
    if (a == ss->shield || (a && (a == ss->skip[0] || a == ss->skip[1]))) return;
    float p[3];
    if (!coop_rd(a + COOP_ACTOR_POS, p, 12)) return;
    float d2 = (p[0] - ss->at[0]) * (p[0] - ss->at[0]) + (p[1] - ss->at[1]) * (p[1] - ss->at[1]) + (p[2] - ss->at[2]) * (p[2] - ss->at[2]);
    if (d2 > 0.64f || d2 >= ss->best_d2) return;
    for (int k = 0; k < 2; k++) {
        float q[3];
        if (!ss->others[k] || ss->others[k] == a || !coop_rd(ss->others[k] + COOP_ACTOR_POS, q, 12)) continue;
        if ((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]) < d2) return;
    }
    char nm[48];
    coop_actor_name(a, nm, sizeof nm);
    if (!nm[0] || !strncmp(nm, "Player", 6) || !strncmp(nm, "Weapon_", 7) || !strncmp(nm, "Armor_", 6) || !strncmp(nm, "Accessory_", 10) ||
        strstr(nm, "Arrow") || !strncmp(nm, "Parasail", 8) || !strncmp(nm, "Obj_OneTouch", 12))   // (the fuse's own link)
        return;
    Slot *cs;
    int ck;
    if (copy_of(a, &cs, &ck) || is_driven(a)) return;   // (not a copy, or this game's own one moved to another's report)
    ss->best = a, ss->best_d2 = d2;
}
// A placed object fused to this Link's shield (a world wheel picked up and fused) is gone from the world here, but
// stays an actor, so no game was told: the other game kept its own lying where it was (a play session,
// 2026-11-06). Told as destroyed (an OBJS packet, header word 3, as destroy_watch tells), by its key.
static void fused_from_world(uint64_t a) {
    for (int i = 0; i < 512; i++)
        if (dwatch[i].actor == a) {
            CoopObj o;
            memset(&o, 0, sizeof o);
            o.id = COOP_OBJ_KEYED | dwatch[i].key;
            snprintf(o.name, sizeof o.name, "%s", dwatch[i].name);
            coop_rd(a + COOP_ACTOR_POS, o.pos, 12);
            uint8_t pk[4 + sizeof(CoopObj)];
            uint16_t h[2] = {1, 3};
            memcpy(pk, h, 4), memcpy(pk + 4, &o, sizeof o);
            coop_net_objs_send(pk, (int)sizeof pk);
            rt_log("coop: %s (key %08x) was fused to my shield; the others are told it is gone", o.name, o.id & ~COOP_OBJ_KEYED);
            dwatch[i].actor = 0;   // (not told again as it goes)
            return;
        }
}
// Test (TOTK_COOP_TEST_FUSELOG): once a second, the actor on this Link's shield and on each co-op Link's, with its
// scale (+0x2e4) and place in the shield's frame: the proxy's shield made with a Zonai device showed it at full size.
static void fuse_log_one(const char *who, uint64_t shield) {
    ShieldScan ss = {.best_d2 = 4.0f, .shield = shield};
    if (!shield || !coop_rd(shield + COOP_ACTOR_POS, ss.at, 12)) return;
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(shield_fuse_fn, &ss);
    char sn[48], an[48] = "";
    coop_actor_name(shield, sn, sizeof sn);
    float sc[3] = {0}, ssc[3] = {0}, p[3] = {0}, r[9] = {0};
    if (ss.best) coop_actor_name(ss.best, an, sizeof an), coop_rd(ss.best + 0x2e4, sc, 12), coop_rd(ss.best + COOP_ACTOR_POS, p, 12);
    coop_rd(shield + 0x2e4, ssc, 12), coop_rd(shield + COOP_ACTOR_ROT, r, 36);
    float d[3] = {p[0] - ss.at[0], p[1] - ss.at[1], p[2] - ss.at[2]}, l[3];
    for (int j = 0; j < 3; j++) l[j] = r[j] * d[0] + r[3 + j] * d[1] + r[6 + j] * d[2];
    rt_log("coop: test: fuse %s: %s (scale %.3f) has %s %llx, scale %.3f %.3f %.3f, at %.3f %.3f %.3f in its frame (%.2f m)", who, sn, ssc[0],
           an[0] ? an : "nothing", (unsigned long long)ss.best, sc[0], sc[1], sc[2], l[0], l[1], l[2], sqrtf(ss.best_d2));
}
static void fuse_log(uint64_t me, uint64_t my_eq) {
    static int on = -1, t;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_FUSELOG") != NULL;
    if (!on || ++t % 60) return;
    if (my_eq && eq_points_to(my_eq, me)) fuse_log_one("mine", eq_item(my_eq, 2));
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        uint64_t eq = slots[i].actor ? eq_of_actor(slots[i].actor) : 0;
        if (eq && eq_points_to(eq, slots[i].actor)) fuse_log_one("the proxy's", eq_item(eq, 2));
    }
}
static uint64_t my_shield_fused;
// The actor on a proxy's shield (found as on this Link's, every 10 reports) given its owner's scale each report.
void proxy_fuse_scale(Slot *s, const float sc[3]) {
    static uint64_t found[COOP_MAX_PLAYERS];
    static int t[COOP_MAX_PLAYERS], said;
    int i = (int)(s - slots);
    uint64_t eq = s->actor ? eq_of_actor(s->actor) : 0;
    if (!eq || !eq_points_to(eq, s->actor)) return;
    uint64_t shield = eq_item(eq, 2);
    if (!shield) return;
    if (!found[i] || ++t[i] % 10 == 0) {
        ShieldScan ss = {.best_d2 = 2.25f, .shield = shield, .others = {eq_item(eq, 0), eq_item(eq, 1)}};   // (1.5 m: the full-size device's centre is farther out)
        if (!coop_rd(shield + COOP_ACTOR_POS, ss.at, 12)) return;
        shield_scan_skip(&ss, s->fuse_have);
        void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
        coop_actors_live(shield_fuse_fn, &ss);
        if (ss.best != found[i] && said++ < 20) {
            char nm[48] = "";
            coop_actor_name(ss.best, nm, sizeof nm);
            rt_log("coop: proxy: slot %d: its shield's fused %s (%llx) at its owner's scale %.3f", i, nm[0] ? nm : "none",
                   (unsigned long long)ss.best, sc[0]);
        }
        found[i] = ss.best;
    }
    if (found[i]) memcpy((void *)(uintptr_t)(found[i] + 0x2e4), sc, 12);
}   // the actor fused to this Link's shield (shield_fuse_tick), 0 none
static void shield_fuse_tick(uint64_t me, uint64_t my_eq) {
    static int t, same, none;
    static uint64_t last;
    if (++t % 10 || !my_eq || !eq_points_to(my_eq, me)) return;
    uint64_t shield = eq_item(my_eq, 2);
    ShieldScan ss = {.best_d2 = 1e9f, .shield = shield, .others = {eq_item(my_eq, 0), eq_item(my_eq, 1)}};
    if (!shield || !coop_rd(shield + COOP_ACTOR_POS, ss.at, 12)) { last = 0, same = 0; return; }
    shield_scan_skip(&ss, my_fuse);
    void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);   // coop_world.c
    coop_actors_live(shield_fuse_fn, &ss);
    if (ss.best && ss.best == last) same++;
    else same = ss.best ? 1 : 0;
    last = ss.best;
    none = ss.best ? 0 : none + 1;
    char nm[48] = "";
    if (same >= 5) coop_actor_name(ss.best, nm, sizeof nm);
    else if (none < 3) return;   // not sure yet either way
    my_shield_fused = nm[0] ? ss.best : 0;
    if (strcmp(nm, my_fuse[2])) {
        rt_log("coop: my shield (%s) is fused with %s", my_gear[2], nm[0] ? nm : "nothing");
        snprintf(my_fuse[2], sizeof my_fuse[2], "%s", nm);
        if (nm[0]) fused_from_world(ss.best);
    }
}
static void audit_vec(FILE *f, const char *key, const float *p) { fprintf(f, ",\"%s\":[%.3f,%.3f,%.3f]", key, p[0], p[1], p[2]); }
static void audit_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) if (*s != '"' && *s != '\\' && (unsigned char)*s >= 32) fputc(*s, f);
    fputc('"', f);
}
static void audit_gear(FILE *f, const char *key, char g[][32], int n) {
    fprintf(f, ",\"%s\":[", key);
    for (int k = 0; k < n; k++) fputs(k ? "," : "", f), audit_str(f, g[k]);
    fputc(']', f);
}
static uint64_t made_actor(uint32_t id) {   // the actor of an object this game sends, by its number or key
    for (int i = 0; i < 32; i++) if (my_arrows[i].actor && my_arrows[i].mine && my_arrows[i].id == id) return my_arrows[i].actor;
    for (int e = 0; e < PROPS_N; e++) {
        if (!props[e].actor) continue;
        if (props[e].made && props[e].id == id) return props[e].actor;
        if (!props[e].made && (id & COOP_OBJ_KEYED) && (COOP_OBJ_KEYED | (props[e].key & ~COOP_OBJ_KEYED)) == id) return props[e].actor;
    }
    return 0;
}
static void audit_tick(Cpu *c, uint64_t me, const CoopObj *sent, int n_sent, const NetState *st) {
    static int on = -1;
    static FILE *f;
    static uint64_t last_s;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_AUDIT");
        on = !(e && !strcmp(e, "0"));
    }
    uint64_t now = plat_now_ns(), sec = now / 1000000000ull;
    if (!on || sec == last_s) return;
    last_s = sec;
    if (!f && !(f = fopen("logs/audit.jsonl", "w"))) { on = 0; return; }
    int coop_life_of(uint64_t a), coop_life_max_of(uint64_t a);
    float p[3] = {0};
    coop_rd(me + COOP_ACTOR_POS, p, 12);
    fprintf(f, "{\"t\":%.3f,\"id\":%d,\"world\":%.2f,\"tod\":%.1f,\"weather\":%d", now / 1e9, coop_net_id(), frame_step_world_us() / 1e6,
            day_seconds(), weather_now);
    fprintf(f, ",\"me\":{\"life\":%d,\"max\":%d,\"hands\":%u", coop_life_of(me), coop_life_max_of(me), (st->flags >> 8) & 7);
    audit_vec(f, "p", p);
    audit_gear(f, "gear", my_gear, COOP_GEAR_SLOTS);
    char fz[3][32];
    for (int k = 0; k < 3; k++) snprintf(fz[k], 32, "%s", st->fuse[k]);
    audit_gear(f, "fuse", fz, 3);
    fprintf(f, ",\"dye\":[%d,%d,%d,%d,%d,%d]}", my_dye[0], my_dye[1], my_dye[2], my_dye[3], my_dye[4], my_dye[5]);
    // the co-op Links as shown here
    fputs(",\"links\":[", f);
    int first = 1;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        Slot *s = &slots[i];
        if (s->source != SRC_NET || (s->state != LIVE && s->state != LET_GO) || !s->actor) continue;
        float q[3] = {0};
        coop_rd(s->actor + COOP_ACTOR_POS, q, 12);
        char g[COOP_GEAR_SLOTS][32];
        for (int k = 0; k < COOP_GEAR_SLOTS; k++) strcpy(g[k], "");
        uint64_t eq = eq_of_actor(s->actor);
        unsigned hands = 0;
        if (eq && eq_points_to(eq, s->actor) && !this_paused) {
            eq_gear(eq, g);
            static const uint64_t hand_of[3] = {0, 3, 4};
            for (int k = 0; k < 3; k++) if (coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, hand_of[k], 0, 0) & 1) hands |= 1u << k;
        }
        fprintf(f, "%s{\"owner\":%d,\"life\":%d,\"max\":%d,\"hands\":%u", first ? "" : ",", s->net_id, coop_life_of(s->actor),
                coop_life_max_of(s->actor), hands);
        first = 0;
        audit_vec(f, "p", q);
        audit_gear(f, "gear", g, COOP_GEAR_SLOTS);
        audit_gear(f, "fuse", s->fuse_have, 3);
        fprintf(f, ",\"dye\":[%d,%d,%d,%d,%d,%d]}", s->dye_have[0], s->dye_have[1], s->dye_have[2], s->dye_have[3], s->dye_have[4], s->dye_have[5]);
    }
    // the objects this game sends, at their real place
    fputs("],\"sent\":[", f);
    for (int i = 0; i < n_sent; i++) {
        uint64_t a = made_actor(sent[i].id);
        float q[3];
        memcpy(q, sent[i].pos, 12);
        if (a) coop_rd(a + COOP_ACTOR_POS, q, 12);
        if (a && (sent[i].flags & COOP_OBJ_LOCAL)) {   // nocked: kept where its model is
            uint64_t model = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x8) + 0x28);
            float mt[12];
            if (model && coop_rd(model + 0x1f8, mt, sizeof mt)) q[0] = mt[3], q[1] = mt[7], q[2] = mt[11];
        }
        fprintf(f, "%s{\"id\":%u,\"name\":", i ? "," : "", sent[i].id);
        audit_str(f, sent[i].name);
        fprintf(f, ",\"flags\":%u", sent[i].flags);
        audit_vec(f, "p", q);
        fputc('}', f);
    }
    // the copies shown here of the others' objects (where they are shown) and the placed objects held here at
    // another game's report
    fputs("],\"copies\":[", f);
    first = 1;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        for (int k = 0; k < COOP_OBJS; k++) {
            Slot *s = &slots[i];
            if (s->mir[k].state != 3 || !s->mir[k].actor) continue;
            float q[3];
            memcpy(q, s->mir[k].pos, 12);   // where it is put (an arrow's actor is kept 300 m under)
            int arrowish = strstr(s->mir[k].name, "Arrow") || (s->mir[k].flags & (COOP_OBJ_FUSED | COOP_OBJ_ONLINK));
            if (!arrowish || s->mir[k].blowing) coop_rd(s->mir[k].actor + COOP_ACTOR_POS, q, 12);
            fprintf(f, "%s{\"owner\":%d,\"id\":%u,\"name\":", first ? "" : ",", s->net_id, s->mir[k].id);
            first = 0;
            audit_str(f, s->mir[k].name);
            fprintf(f, ",\"alive\":%d,\"blowing\":%d", copy_alive(s, k), s->mir[k].blowing);
            audit_vec(f, "p", q);
            fputc('}', f);
        }
    fputs("],\"held\":[", f);
    first = 1;
    for (int k = 0; k < 128; k++) {
        if (!key_found[k].actor || !key_found[k].last || prop_tick - key_found[k].last > 60) continue;
        float q[3];
        if (!coop_rd(key_found[k].actor + COOP_ACTOR_POS, q, 12)) continue;
        fprintf(f, "%s{\"owner\":%d,\"id\":%u,\"name\":", first ? "" : ",", key_found[k].from, COOP_OBJ_KEYED | key_found[k].key);
        first = 0;
        audit_str(f, key_found[k].name);
        audit_vec(f, "p", q);
        fputc('}', f);
    }
    fputs("],\"enemies\":", f);
    void coop_audit_enemies(FILE *f, uint64_t me);   // coop_world.c
    coop_audit_enemies(f, me);
    fputs("}\n", f);
    fflush(f);
}
void coop_players_tick(Cpu *c) {
    if (frame_step_world_us() <= 0) return;   // not in the world yet
    coop_session_tests(frame_step_world_us());
    uint64_t me = coop_player_actor();
    if (!me) return;
    {   // which Player actor this PC's Link is: a load that makes it again shows here (docs/coop/character-mods.md)
        static uint64_t me_was;
        if (me != me_was) rt_log("coop: this PC's Link is the Player at %llx%s", (unsigned long long)me, me_was ? " (a new one)" : "");
        me_was = me;
    }
    coop_world_tick(c, me);

    coop_chars_tick();   // the character fetch's job and prompt (coop_chars.c), in a session or not
    // This player's state out, when on a relay.
    if (coop_net_on()) {
        coop_notices_tick(c, me);   // a fast travel of this Link, told to the others (coop_notices.c)
        coop_far_test(c, me);       // TOTK_COOP_TEST_FAR (coop_far.c)
        NetState st = {0};
        st.world_us = frame_step_world_us();
        coop_rd(me + COOP_ACTOR_POS, st.pos, 12), coop_rd(me + COOP_ACTOR_ROT, st.rot, 36);
        camera_frame(st.cam_fwd, st.cam_right);
        st.buttons = local_buttons;
        memcpy(st.gear, my_gear, sizeof st.gear);
        memcpy(st.dye, my_dye, sizeof st.dye);
        const CoopChar *me_char = coop_char_local();
        snprintf(st.character, sizeof st.character, "%s", me_char->key);
        st.char_mod = me_char->mod_id;
        {   // TOTK_COOP_TEST_CHARKEY=<key>@<s>[,<key>@<s>...] (a test): from s world seconds this PC says its character
            // is key (the last one due wins), as a player who switched character mods and came back would (the others
            // remake its stand-in); "Link" or a key like gb:<file id>
            const char *e = cfg_get("TOTK_COOP_TEST_CHARKEY");
            double now_s = frame_step_world_us() / 1e6;
            for (const char *p = e; p && *p;) {
                const char *at = strchr(p, '@'), *comma = strchr(p, ',');
                if (!at || (comma && comma < at)) break;
                if (now_s >= atof(at + 1)) {
                    snprintf(st.character, sizeof st.character, "%.*s", (int)(at - p) < 23 ? (int)(at - p) : 23, p);
                    st.char_mod = 0;
                }
                p = comma ? comma + 1 : NULL;
            }
        }
        static CoopObj cur[COOP_OBJS];
        int n_objs = report_arrows(c, me, cur);
        n_objs += report_props(me, cur + n_objs, COOP_OBJS - n_objs);
        objs_send(cur, n_objs);
        objs_receive(c);
        // (none when unequipped; copied as a string: 32 bytes copied from "" sent what followed it as the name, code
        // review, 2026-11-06)
        for (int k = 0; k < 3; k++) snprintf(st.fuse[k], sizeof st.fuse[k], "%s", strcmp(my_gear[k], GEAR_NONE) ? my_fuse[k] : "");
        // The scale of what is fused to the shield, for the proxy's: the proxy's shield made with a Zonai device as its
        // material showed the device at its full size, far larger than on the owner's shield (a play session,
        // 2026-11-06). The owner's is made smaller by the fuse as it runs (the material's DrawnScale / SheathedScale
        // params; a scaling step, AI query QueryAttachmentIsScaling), which a frozen proxy does not run; its scale is
        // the owner's here (proxy_fuse_scale).
        if (my_shield_fused && st.fuse[2][0]) coop_rd(my_shield_fused + 0x2e4, st.shield_fuse_scale, 12);
        match_my_eq(me);
        uint64_t my_eq = eq_of_actor(me);   // weapon, bow, shield in hand (only on its own EquipmentUser: see report_arrows)
        if (my_eq && !eq_points_to(my_eq, me)) my_eq = 0;
        static const uint64_t hand_of[3] = {0, 3, 4};
        for (int k = 0; my_eq && k < 3; k++)
            if (coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, my_eq, hand_of[k], 0, 0) & 1) st.flags |= 1u << (8 + k);
        shield_send(me, my_eq, &st);
        hold_send(me, my_eq, &st);
        // Lightning about to strike this Link (its metal gear sparks): AI node OneShotEquipmentUserSetLightningWait sets
        // bit 16 of the EquipmentUser's flags at +0x4600 (and the wait time at +0x3f14 to 0; main+0x2529c3c, its
        // ResetLightningWait clears it). Sent as NetState flags bit 12, so the proxy sparks too (the tester's list).
        if (my_eq && eq_points_to(my_eq, me) && (LD64(my_eq + 0x4600) >> 16 & 1)) st.flags |= 1u << 12;
        // The weapon's attack is on (EquipmentUser +0x660, 1 for about 0.1 s in each swing in 1.4.3 as in 1.2.1:
        // TOTK_COOP_TEST_EQDIFF, 2026-11-07): flags bit 13. TotK Online writes it onto its stand-in every frame (read
        // for behaviour only), so the stand-in's swing hits there.
        if (my_eq && LD8(my_eq + EQ_ATTACKING)) st.flags |= 1u << 13;
        {   // test (TOTK_COOP_TEST_LIGHTNING): at 20 s this Link's lightning wait is set, as the AI node sets it; logged
            // each second while it lasts, and each LightningStrike that shows up near this Link
            static int on = -1, done, said;
            if (on < 0) on = cfg_get("TOTK_COOP_TEST_LIGHTNING") != NULL;
            if (on && my_eq && eq_points_to(my_eq, me)) {
                if (!done && frame_step_world_us() > 20000000) {
                    done = 1;
                    __atomic_fetch_or((uint64_t *)(uintptr_t)(my_eq + 0x4600), 1ull << 16, __ATOMIC_SEQ_CST), ST32(my_eq + 0x3f14, 0);
                    rt_log("coop: test: lightning: this Link's lightning wait set");
                }
                static int t;
                if (done && ++t % 30 == 0 && said++ < 30)
                    rt_log("coop: test: lightning: wait %d, time %.3f", (int)(LD64(my_eq + 0x4600) >> 16 & 1), *(float *)(uintptr_t)(my_eq + 0x3f14));
            }
        }
        shield_fuse_tick(me, my_eq);
        fuse_log(me, my_eq);
        audit_tick(c, me, cur, n_objs, &st);
        {   // test: this Link's life when it changes, with each proxy's and whether it runs its own update then (P1's
            // hearts went up and down while P2 swung, a play session, 2026-11-06)
            int coop_life_of(uint64_t a);
            static int was = -1, said;
            int l = coop_life_of(me);
            if (l != was && said < 300) {
                said++;
                char buf[160];
                int o = snprintf(buf, sizeof buf, "coop: %.2f s: my life %d -> %d", frame_step_world_us() / 1e6, was, l);
                for (int i = 0; i < COOP_MAX_PLAYERS && o < (int)sizeof buf; i++)
                    if (slots[i].actor)
                        o += snprintf(buf + o, sizeof buf - o, "; slot %d's stand-in %d", i, coop_life_of(slots[i].actor));
                rt_log("%s", buf);
            }
            was = l;
        }
        {   // proxy attacks: its life, so the proxies' match (the Master Sword's beam comes at full life only)
            int coop_life_of(uint64_t a), coop_life_max_of(uint64_t a);
            int l = coop_life_of(me), lm = coop_life_max_of(me);
            st.life = (uint16_t)(l > 0 && l < 65535 ? l : 0), st.life_max = (uint16_t)(lm > 0 && lm < 65535 ? lm : 0);
        }
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
        if (paused) runs_since_ns = 0;
        else if (!runs_since_ns) runs_since_ns = now_ns;
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
        {   // on a horse (coop_mounts.c): that, in place of an object
            uint32_t mk;
            float mo[3];
            if (coop_mount_riding(&mk, mo)) st.ride_owner = COOP_RIDE_MOUNT, st.ride_id = mk, memcpy(st.ride_off, mo, 12);
        }
        const char *fw = cfg_get("TOTK_COOP_FAKEWEATHER");   // test: report this weather type instead
        if (fw && *fw) st.weather = atoi(fw);
        void coop_anim_pack(uint64_t me, CoopAnim *out);   // coop_world.c: the stand-in's animation
        coop_anim_pack(me, st.anim);
        void coop_anim_masks_pack(uint64_t me, const CoopAnim *anim, CoopMask *out);   // coop_world.c
        coop_anim_masks_pack(me, st.anim, st.mask);
        void coop_blur_pack(Cpu *c, uint64_t me, NetState *st);   // coop_world.c: the sword trail
        coop_blur_pack(c, me, &st);
        coop_net_publish(&st);
        props_receive(c), ride_carry(c, me), destroy_watch(me), old_items_tick(c);   // placed objects co-op players move; riding them; destroyed
        props_test(c, me), props_test_where(), ride_test(c, me), pickup_test(c, me), chest_test(c, me), arrow_dump(), proxy_arrows_test(), bone_arrow_test(me), impact_test(), glide_log(me), newnear_test(me), beam_test(me), beam_end_tick(), link_hit_test(me);
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

    // The host's world (for now the player with the lowest id): its time of day, seen below.
    int host_id = coop_net_on() && coop_net_id() ? coop_net_id() : 0;
    float host_time = -1;
    int32_t host_weather = -1;
    int have_host = 0, host_far = 0;

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
                if (s->actor) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->actor, COOP_DELETE_REASON, 0, 0);
                for (int k = 0; k < COOP_OBJS; k++)   // their arrows' copies too (each still that copy)
                    if (s->mir[k].state == 3 && copy_alive(s, k)) coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->mir[k].actor, COOP_DELETE_REASON, 0, 0);
                memset(s->mir, 0, sizeof s->mir), copies_live[s - slots] = 0;
                rt_log("coop: remote player %d left; their Link is deleted", id);
            } else if (s && s->state == REQUESTED) s->leaving = 1;   // (a create on its way: deleted when it comes)
            else if (s && s->state != LET_GO) s->state = FREE;
            continue;
        }
        if (s) s->leaving = 0;   // back before its Player came
        if (id < host_id && age < 2000000000ull) {
            host_id = id, host_time = st.day_seconds, host_weather = st.weather, have_host = 1;
            // The weather is the place's: the host's only within 300 m of the host's Link. Followed from afar, a
            // Link warped away kept the cold of where the host stood and lost life to it (a play session, 2026-11-06).
            float mp[3];
            if (coop_rd(me + COOP_ACTOR_POS, mp, 12) &&
                (mp[0] - st.pos[0]) * (mp[0] - st.pos[0]) + (mp[1] - st.pos[1]) * (mp[1] - st.pos[1]) + (mp[2] - st.pos[2]) * (mp[2] - st.pos[2]) > 300.0f * 300.0f)
                host_weather = -1, host_far = 1;
        }
        if (!s && !(s = slot_new(SRC_NET, id))) continue;
        {   // its character: a change (its player switched mods and came back, a test) remakes the stand-in below
            char key[COOP_CHAR_KEY];
            memcpy(key, st.character, sizeof key);
            key[sizeof key - 1] = 0;
            if (strcmp(key, s->character)) {
                if (s->character[0]) rt_log("coop: slot %d: its owner's character is now %s (was %s)", (int)(s - slots), key[0] ? key : "Link", s->character);
                memcpy(s->character, key, sizeof key);
            }
        }
        coop_chars_seen(id, st.character[sizeof st.character - 1] ? "" : st.character, st.char_mod);
        int32_t ast = 0;   // (a live actor: one going away, or another at its address, gets no call; code review)
        if (s->state == LIVE && s->actor && age < 2000000000ull && coop_rd(s->actor + COOP_ACTOR_STATE, &ast, 4) &&
            ast >= 2 && ast <= 6 && actor_is(s->actor, s->standin[0] ? s->standin : "PlayerPuppet")) {   // (the name too: another actor at a freed one's address)
            void coop_anim_apply(Cpu *c, uint64_t standin, const CoopAnim *in, int64_t age_ns);   // coop_world.c
            static uint64_t y_was[COOP_MAX_PLAYERS];   // its owner's attack button down: logged (the suite's tour case)
            uint64_t y_now = st.buttons >> 3 & 1;
            if (y_now && !y_was[s - slots]) {
                static int said_y;
                if (said_y++ < 20) rt_log("coop: stand-in: slot %d swings (its owner's attack button)", (int)(s - slots));
            }
            y_was[s - slots] = y_now;
            void coop_anim_masks_apply(uint64_t standin, const CoopMask *in);   // coop_world.c
            coop_anim_masks_apply(s->actor, st.mask);
            // The state's age on the owner's clock, as the stand-in is placed, less the placing's delay: the clips play
            // at the same moment of the owner's as the body is shown (TOTK_COOP_PUPPET_DELAY_MS back). Played at the
            // newest state they ran 60 ms ahead of the body, a jump's take-off before it left the ground and each step
            // of a slow walk out of time with its motion (a play session, "choppy on jumps and slow walks", 2026-11-08).
            // Negative: the newest state's frame taken back by its rate (a clip just begun waits at its start).
            int64_t aage = (int64_t)age;
            int64_t lw2 = frame_step_world_us();
            if (s->have_woff && st.world_us >= 0 && lw2 >= 0) aage = (lw2 - s->woff_us - st.world_us) * 1000;
            aage -= (int64_t)puppet_delay_ms() * 1000000;
            coop_anim_apply(c, s->actor, st.anim, aage);
            void coop_blur_apply(uint64_t standin, const NetState *st, uint64_t age_ns);   // coop_world.c: the sword trail
            coop_blur_apply(s->actor, &st, age);
        }
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
            int64_t lw = frame_step_world_us();
            s->hist[w].w_us = st.world_us >= 0 && lw >= 0 ? st.world_us : -1;
            if (s->hist[w].w_us >= 0) {   // the offset: down at once to a less delayed state, up slowly (clock drift)
                int64_t sample = lw - st.world_us;
                if (!s->have_woff || sample < s->woff_us || sample - s->woff_us > 2000000) s->woff_us = sample, s->have_woff = 1;
                else s->woff_us += (sample - s->woff_us) / 64;
            }
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
        if (s->ride_owner) {   // a ride offset that is not a number, or a horse rider over 4 m from the horse's origin
            // (a saddle sits about 1 m from it), rides nothing: it would go into the stand-in's matrix as is, and a
            // horse ride also hands that horse to the sender's game (coop_world.c peer_one, coop_mount_rider)
            float mx = 0;
            int ok = 1;
            for (int j = 0; j < 3; j++) ok &= isfinite(s->ride_off[j]), mx = fmaxf(mx, fabsf(s->ride_off[j]));
            if (!ok || (s->ride_owner == COOP_RIDE_MOUNT && mx > 4.0f)) {
                static int said;
                if (!said++) rt_log("coop: slot %d: ride (owner %d, id %08x) with offset %g %g %g ignored", (int)(s - slots),
                                    s->ride_owner, s->ride_id, s->ride_off[0], s->ride_off[1], s->ride_off[2]);
                s->ride_owner = 0;
            }
        }
        s->shield_placed = (st.flags >> 11) & 1;
        memcpy(s->hold_bone, st.hold_bone, sizeof s->hold_bone), memcpy(s->hold, st.hold, sizeof s->hold);
        {   // its owner's lightning wait (flags bit 12) on the stand-in's EquipmentUser, as the AI node
            // OneShotEquipmentUserSetLightningWait sets it (bit 16 of +0x4600, the wait time +0x3f14 to 0), so its metal
            // gear sparks too; cleared when its owner's is (the tester's list)
            int want_l = (st.flags >> 12) & 1;
            uint64_t peq = s->actor && s->state == LIVE ? eq_of_actor(s->actor) : 0;
            if (peq && eq_points_to(peq, s->actor)) {
                int has_l = (int)(LD64(peq + 0x4600) >> 16 & 1);
                if (want_l != has_l) {
                    if (want_l) __atomic_fetch_or((uint64_t *)(uintptr_t)(peq + 0x4600), 1ull << 16, __ATOMIC_SEQ_CST), ST32(peq + 0x3f14, 0);
                    else __atomic_fetch_and((uint64_t *)(uintptr_t)(peq + 0x4600), ~(1ull << 16), __ATOMIC_SEQ_CST);
                    static int said_l;
                    if (said_l++ < 20) rt_log("coop: stand-in: slot %d: lightning %s its owner", (int)(s - slots), want_l ? "about to strike" : "no longer after");
                }
            }
        }
        if (s->shield_placed) memcpy(s->shield_local, st.shield_pos, 12), quat_mat(st.shield_q, s->shield_rot);
        for (int k = 0; k < COOP_GEAR_SLOTS; k++) {
            memcpy(s->gear_want[k], st.gear[k], 32);
            s->gear_want[k][31] = 0;
            s->dye_want[k] = st.dye[k];
            // Armor on another character's stand-in: this game has its armor fitted to its own Link's character only,
            // so that stand-in wears its own character's bare pieces (coop_chars.c; docs/coop/character-mods.md).
            const char *bare = k > 2 && s->gear_want[k][0] ? coop_char_bare(s->character, k - 3) : NULL;
            if (bare) snprintf(s->gear_want[k], sizeof s->gear_want[k], "%s", bare), s->dye_want[k] = -1;
            if (k < 3) memcpy(s->fuse_want[k], st.fuse[k], 32), s->fuse_want[k][31] = 0;
            if (k < 3) s->out_want = (s->out_want & ~(1 << k)) | (int)((st.flags >> (8 + k)) & 1) << k;
            if (k == 0) s->attack_want = (int)(st.flags >> 13 & 1);
        }
        s->idle = !st.buttons && abs(st.sticks[0]) < 3000 && abs(st.sticks[1]) < 3000;
    }
    int others = 0;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++) others += slots[i].state != FREE && slots[i].source == SRC_NET;
    world_role = !coop_net_on() || !coop_net_id() ? 0 : have_host ? 2 : others ? 1 : 0;
    if (host_time >= 0) follow_host_time(c, host_time);
    else if (!have_host) own_time_back(c);
    if (have_host && host_weather >= 0) follow_host_weather(c, host_weather);
    // (let go when the host left, is far, or this game is the host; not for a moment the host's weather is unknown,
    // its load or a menu: let go then, this game went back to its forecast and changed again a few seconds later,
    // the sync audit, 2026-11-06)
    else if (weather_forced >= 0 && (!have_host || host_far)) follow_host_weather(c, -1);


    for (int i = 0; i < COOP_MAX_PLAYERS; i++) {
        Slot *s = &slots[i];
        if (s->state == WANTED) spawn_slot(c, s);
        else if (s->state == REQUESTED) {
            int st = coop_handle_status(s->handle);
            if (st == 4) { rt_log("coop: slot %d: the game could not make the Player", i); s->state = s->leaving ? FREE : WANTED; }
            if (st != 3) continue;
            s->actor = coop_claim(c, s->handle, s->link, &s->ref);
            // Its player left while it was being made: set FREE then, the create was left unclaimed and an extra
            // Player stayed in the world (code review, 2026-11-06). Claimed and deleted now.
            if (s->leaving && s->actor && s->actor != me) {
                coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->actor, COOP_DELETE_REASON, 0, 0);
                rt_log("coop: slot %d: its player left while its Player was made; deleted", i);
                s->actor = 0, s->state = FREE, s->leaving = 0;
                continue;
            }
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
            // Its owner's character got a set of its own while the game runs (coop_chars.c, a fetched character in a
            // stand-in slot): the stand-in is deleted, and made again as the new actor when it is gone (WANTED below).
            if (s->state == LIVE && s->source == SRC_NET && s->standin[0] && !s->recharacter &&
                strcmp(coop_char_actor(s->character), s->standin)) {
                rt_log("coop: slot %d: its owner's character is now %s; the stand-in %s is made again", i,
                       coop_char_actor(s->character), s->standin);
                coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, s->actor, COOP_DELETE_REASON, 0, 0);
                s->recharacter = 1;
            }
            // The owner changed an item: make that slot the owner's item.
            uint64_t eq = s->state == LIVE && s->source == SRC_NET ? eq_of_actor(s->actor) : 0;
            // A new PlayerPuppet stand-in's EquipmentUser starts with this game's Link's weapon, bow and shield. A slot
            // this game's Link has empty is given an item once first, before its owner's: an item made into a slot
            // that never held one stayed on the stand-in's back in its owner's hand, whatever was asked (in-hand calls,
            // made again in hand, the in-hand bit, put at its hand bone), when both players had nothing equipped and
            // one equipped first; with this game's Link holding one it was held (the tester, 2026-11-07, who suggested
            // equipping one at load). The gear code below then makes it the owner's, or empties it.
            // Off: it did not help (the cause was this Link's own equipment read for the stand-in's item: coop_spawn.c,
            // main+0x45a460). TOTK_COOP_STANDIN_PRIME=1: done (a test).
            if (eq && !s->primed) {
                static int prime = -1;
                if (prime < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_PRIME"); prime = e && *e == '1'; }
                static const char *const first[3] = {"Weapon_Sword_001", "Weapon_Bow_001", "Weapon_Shield_001"};
                for (int k = 0; prime && k < 3; k++)
                    if (!eq_item(eq, k)) {
                        regive_gear(c, eq, k, first[k], -1, 0, s->gear_str[k], NULL, s->att_str[k]);
                        strcpy(s->gear_have[k], first[k]);   // (so the gear code below sees a change to the owner's)
                        rt_log("coop: stand-in: slot %d: its empty gear slot %d given %s first", i, coop_gear_slot[k], first[k]);
                    }
                s->primed = 1;
            }
            char held[COOP_GEAR_SLOTS][32];
            if (eq) eq_gear(eq, held);
            for (int k = 0; eq && k < COOP_GEAR_SLOTS; k++)
                if (s->gear_want[k][0] && (strcmp(s->gear_want[k], s->gear_have[k]) || (k > 2 && s->dye_want[k] != s->dye_have[k]) ||
                                           (k <= 2 && strcmp(s->fuse_want[k], s->fuse_have[k])))) {
                    int redye = (k > 2 && s->dye_want[k] != s->dye_have[k]) || (k <= 2 && strcmp(s->fuse_want[k], s->fuse_have[k]));
                    memcpy(s->gear_have[k], s->gear_want[k], 32), s->dye_have[k] = s->dye_want[k];
                    if (k <= 2) memcpy(old_item_fuse, s->fuse_have[k], 32), memcpy(s->fuse_have[k], s->fuse_want[k], 32);
                    int none = !strcmp(s->gear_want[k], GEAR_NONE);
                    if ((none && k > 2) || (!strcmp(held[k], s->gear_want[k]) && !redye)) continue;   // nothing to change
                    rt_log("coop: slot %d: owner's gear slot %d is %s (dye %d%s%s)", i, coop_gear_slot[k], s->gear_want[k], s->dye_want[k],
                           k <= 2 && s->fuse_want[k][0] ? ", fused with " : "", k <= 2 ? s->fuse_want[k] : "");
                    regive_gear(c, eq, k, none ? "" : s->gear_want[k], s->dye_want[k], -1, s->gear_str[k], k <= 2 ? s->fuse_want[k] : NULL,
                                s->att_str[k < 3 ? k : 0]);
                }
            if (s->state == LIVE) s->live_ticks++;
            // The PlayerPuppet stand-in: its items, made once, are taken out and put away
            // with the game's in-hand calls as its owner holds them (TotK Online's way, read for behaviour only):
            // nothing of the stand-in's own puts them back. Checked every 6 ticks.
            // Its main body asked for motion type 0 every tick (phive RigidBodyBase::requestChangeMotionType, 1.4.3
            // main+0xae4300 from 1.2.1's 0xe8be30, the same code; TotK Online does it, read for behaviour only), so
            // the physics does not push it about. TOTK_COOP_STANDIN_MOTION=0: not.
            if (s->state == LIVE && s->actor) {
                static int mo = -1;
                if (mo < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_MOTION"); mo = !(e && *e == '0'); }
                uint64_t body = coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(s->actor + 0x228) + 0x50) + 0x20) + 0x150);
                static int mt = -1;   // TOTK_COOP_STANDIN_MOTION_TYPE=<n>: the type asked for (a test; default 0)
                if (mt < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_MOTION_TYPE"); mt = e ? atoi(e) : 0; }
                if (mo && body) coop_guest_call(c, MOD_MAIN_BASE + 0xae4300, body, (uint64_t)mt, 0, 0);
            }
            // ... and its weapon's attack on while its owner's is (flags bit 13), every tick
            if (eq) {
                uint8_t att = (uint8_t)s->attack_want;
                static int also = -1;   // TOTK_COOP_TEST_ATK598=1: +0x598 too (it flips with +0x660 in a swing)
                if (also < 0) also = cfg_get("TOTK_COOP_TEST_ATK598") != NULL;
                if (also) ST8(eq + 0x598, att);
                if (LD8(eq + EQ_ATTACKING) != att) {
                    ST8(eq + EQ_ATTACKING, att);
                    static int said_a;
                    if (att && said_a++ < 10) rt_log("coop: stand-in: slot %d: its weapon's attack on, as its owner's", i);
                }
            }
            if (eq && s->live_ticks % 6 == 0) {
                static const uint64_t row[3] = {0, 3, 4};
                for (int k = 0; k < 3; k++) {
                    // An item its owner does not have: a new PlayerPuppet's EquipmentUser starts with this game's
                    // Link's weapon, bow or shield (the tour suite case: its owner had no shield, it had this game's
                    // Weapon_Shield_030). Made empty, as an emptied slot is (regive_gear, then let go of).
                    uint64_t has = eq_item(eq, k);
                    if (has && !strcmp(s->gear_want[k], GEAR_NONE)) {
                        char hn[48];
                        rt_log("coop: stand-in: slot %d: gear slot %d holds %s, its owner none: emptied", i, coop_gear_slot[k], coop_actor_name(has, hn, sizeof hn));
                        regive_gear(c, eq, k, "", -1, 0, s->gear_str[k], NULL, s->att_str[k]);
                        continue;
                    }
                    if (!s->gear_want[k][0] || !strcmp(s->gear_want[k], GEAR_NONE)) continue;
                    int want = s->out_want >> k & 1, now = (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, row[k], 0, 0) & 1);
                    if (want == now) continue;
                    gear_hand(c, eq, k, want);
                    static int said;
                    if (said++ < 40) rt_log("coop: stand-in: slot %d: %s %s", i, s->gear_want[k], want ? "taken out" : "put away");
                }
            }
            // Test (TOTK_COOP_STANDIN_HANDBIT=1): the EquipmentUser's per-slot byte +0x4620 (0 weapon, 3 bow, 4 shield)
            // bit 1, which main+0x3e322c reads as "in hand", set while the owner holds that item. It did not keep an item
            // made into an empty slot in hand (the cause: coop_spawn.c, main+0x45a460).
            static int handbit = -1;
            if (handbit < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_HANDBIT"); handbit = e && *e == '1'; }
            if (eq && handbit) {
                static const int slot_of[3] = {0, 3, 4};
                for (int k = 0; k < 3; k++) {
                    uint8_t *f = (uint8_t *)(uintptr_t)(eq + 0x4620 + slot_of[k]);
                    if ((s->out_want >> k) & 1) *f |= 2; else *f &= (uint8_t)~2;
                }
            }
            // Per slot: shared, the count went up once per slot each tick, so with two co-op Links only one ever came to
            // a multiple of 30, and one slot's tries used up or reset another's (code review, 2026-11-06).
            static int hand_ticks[COOP_MAX_PLAYERS], hand_said[COOP_MAX_PLAYERS] = {[0 ... COOP_MAX_PLAYERS - 1] = -1};
            if (eq && ++hand_ticks[i] % 30 == 0) {
                static const uint64_t hand_of[3] = {0, 3, 4};
                int now = 0;
                for (int k = 0; k < 3; k++) now |= (int)(coop_guest_call(c, MOD_MAIN_BASE + GEAR_IN_HAND, eq, hand_of[k], 0, 0) & 1) << k;
                if ((now | s->out_want << 4) != hand_said[i])
                    rt_log("coop: puppet: slot %d holds %#x, owner %#x", i, now, s->out_want), hand_said[i] = now | s->out_want << 4;
            }
            if (s->state == LIVE && s->source == SRC_NET) arrow_copies(c, s);
            if (s->state == LIVE && s->source == SRC_NET && s->have_want) {
                // Its body is moved to its owner: every tick (below), or once it is that far off.
                float om[12];
                owner_matrix(s, om);
                const float *bp = s->have_body ? s->body_pos : s->phys_pos;
                float dx = bp[0] - om[3], dy = bp[1] - om[7], dz = bp[2] - om[11];

                // TOTK_COOP_PUPPET_SNAP_M: that distance in m (default 0: every tick). Its body every tick: its own
                // root motion moved it 15-67 mm a frame, its mesh was drawn from that body, and snapped back at 10 cm
                // it went in a sawtooth, the drawn root's offset from its placing changing
                // by 20 mm a frame and up to 148 (the tester: choppy on slow walks and jumps); every tick 8 mm, worst 17
                // (the suite's tour walk, stairs included, 2026-11-08).
                static float lim = -1;
                if (lim < 0) { const char *e = cfg_get("TOTK_COOP_PUPPET_SNAP_M"); lim = e ? (float)atof(e) : 0.0f; }
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
            }
        }
    }
}

uint64_t coop_players_first(void) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state == LIVE) return slots[i].actor;
    return 0;
}
