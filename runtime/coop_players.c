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
    int have_want, idle, snap_wait;
    unsigned snaps;
    char gear_want[COOP_GEAR_SLOTS][32], gear_have[COOP_GEAR_SLOTS][32];   // the owner's gear; what this Link was given
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

// ---- gear. A Player's EquipmentUser builds each slot through main+0xc4a5f4 (eq, slot), which reads the item from the
// save's inventory and creates it by name: armor through main+0xc4961c (eq, slot, &name, ...), weapons through
// main+0xaf100c (eq, slot, &name, ...); &name points at a string whose first word is the name (2026-11-04). This PC's
// Link's names are recorded per slot and sent; a remote Link's slot is rebuilt when its owner's name changes, and its
// create gets the owner's name in place of the save's.
const int coop_gear_slot[COOP_GEAR_SLOTS] = {0, 9, 10, 11};
#define GEAR_REBUILD 0xc4a5f4
static char my_gear[COOP_GEAR_SLOTS][32];
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
    if (owner == me) {
        learn_eq_off(me, eq);
        if (name[0] && strcmp(my_gear[k], name)) {
            memcpy(my_gear[k], name, sizeof name);
            rt_log("coop: my gear slot %d is now %s", coop_gear_slot[k], name);
        }
        return 0;
    }
    Slot *s = slot_of_actor(owner);
    if (!s || !s->gear_want[k][0] || !strcmp(s->gear_want[k], name)) return 0;
    // The owner's item in place of this save's: a string of the same shape (+0 pointer, +8 capacity 64, the text).
    static _Alignas(16) uint8_t str[COOP_MAX_PLAYERS][COOP_GEAR_SLOTS][0x60];
    uint8_t *st = str[s - slots][k];
    memset(st, 0, 0x60);
    char *text = (char *)st + 0x10;
    memcpy(text, s->gear_want[k], 32);
    uint64_t tp = (uint64_t)(uintptr_t)text;
    memcpy(st, &tp, 8);
    int32_t cap = 0x40;
    memcpy(st + 8, &cap, 4);
    c->x[2] = (uint64_t)(uintptr_t)st;
    rt_log("coop: slot %d's Link gets %s in gear slot %d (this save has %s)", (int)(s - slots), s->gear_want[k],
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
        if (slots[i].state == LIVE && slots[i].actor == actor) return &slots[i];
    return NULL;
}
static Slot *slot_for(int source, int net_id) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state != FREE && slots[i].source == source && slots[i].net_id == net_id) return &slots[i];
    return NULL;
}
static Slot *slot_new(int source, int net_id) {
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (slots[i].state == FREE || (slots[i].state == LET_GO && slots[i].actor)) {
            Slot *s = &slots[i];
            int reuse = s->state == LET_GO;   // a Link left behind by an earlier player: driven again, not respawned
            s->source = source, s->net_id = net_id, s->prev = 0, s->have_want = 0;
            memset(s->held, 0, sizeof s->held);
            s->state = reuse ? LIVE : WANTED;
            rt_log("coop: player slot %d for %s %d%s", i, source == SRC_NET ? "remote player" : "local pad", net_id,
                   reuse ? " (the Link left behind is reused)" : "");
            return s;
        }
    return NULL;
}

// Called from the actor calc hook: 1 when it ran a slot's Player itself (with the slot's controller handed out).
static __thread int inside;
int coop_players_calc(Cpu *c, uint64_t addr) {
    Slot *s;
    if (inside || !(s = slot_of_actor(c->x[0]))) return 0;
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_INPUT_MGR_SLOT));
    if (!mgr) return 0;
    uint64_t real = LD64(mgr + 0xa0), own = (uint64_t)(uintptr_t)s->ctl;
    memcpy(s->ctl, (void *)(uintptr_t)real, sizeof s->ctl);
    plat_mutex_lock(&lock);
    uint64_t buttons = s->buttons;
    float stick[4];
    memcpy(stick, s->stick, sizeof stick);
    plat_mutex_unlock(&lock);
    coop_fill_controller(own, buttons, stick, &s->prev, s->held);
    ST64(mgr + 0xa0, own);
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
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

// The owner's transform as a ForceSetMatrix argument: rows of rotation | position.
static void snap(Cpu *c, Slot *s) {
    float m[12];
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) m[r * 4 + k] = s->want_rot[r * 3 + k];
        m[r * 4 + 3] = s->want_pos[r];
    }
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
    uint64_t me = coop_player_actor();
    if (!me) return;

    // This player's state out, when on a relay.
    if (coop_net_on()) {
        NetState st = {0};
        coop_rd(me + COOP_ACTOR_POS, st.pos, 12), coop_rd(me + COOP_ACTOR_ROT, st.rot, 36);
        camera_frame(st.cam_fwd, st.cam_right);
        st.buttons = local_buttons;
        memcpy(st.gear, my_gear, sizeof st.gear);
        const char *fake = cfg_get("TOTK_COOP_FAKEGEAR");   // test: report this as the weapon in hand
        if (fake && *fake) snprintf(st.gear[0], 32, "%s", fake);
        memcpy(st.sticks, local_sticks, sizeof st.sticks);
        coop_net_publish(&st);
    }
    float fwd[2], right[2];
    int have_cam = camera_frame(fwd, right);

    // Remote players in, slots for new ones, let go of the ones that left.
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        NetState st;
        uint64_t age = 0;
        Slot *s = slot_for(SRC_NET, id);
        if (!coop_net_on() || !coop_net_remote(id, &st, &age)) {
            if (s && s->state == LIVE) {
                s->state = LET_GO;
                set_input(s, 0, (const int32_t[4]){0, 0, 0, 0});
                rt_log("coop: remote player %d left; their Link stays idle", id);
            } else if (s && s->state != LET_GO) s->state = FREE;
            continue;
        }
        if (!s && !(s = slot_new(SRC_NET, id))) continue;
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
            if (s->actor && coop_rd(s->actor + COOP_ACTOR_STATE, &st, 4) && (st < 2 || st > 6)) {
                rt_log("coop: slot %d: its Player went away (state %d)", i, st);
                s->actor = 0, s->state = s->state == LIVE ? WANTED : FREE;
                continue;
            }
            // The owner changed an item: rebuild that slot (its create then takes the owner's name).
            uint64_t eq = s->state == LIVE && s->source == SRC_NET ? eq_of_actor(s->actor) : 0;
            for (int k = 0; eq && k < COOP_GEAR_SLOTS; k++)
                if (s->gear_want[k][0] && strcmp(s->gear_want[k], s->gear_have[k])) {
                    memcpy(s->gear_have[k], s->gear_want[k], 32);
                    rt_log("coop: slot %d: owner's gear slot %d is %s, rebuilding", i, coop_gear_slot[k], s->gear_want[k]);
                    coop_guest_call(c, MOD_MAIN_BASE + GEAR_REBUILD, eq, (uint64_t)coop_gear_slot[k], 0, 0);
                }
            if (s->state == LIVE && s->source == SRC_NET && s->have_want) {
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
