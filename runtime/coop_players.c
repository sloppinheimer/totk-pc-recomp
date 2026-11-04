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
            rt_log("coop: slot %d: Player %llx in the world", i, (unsigned long long)s->actor);
        } else if (s->state == LIVE || s->state == LET_GO) {
            int32_t st = -1;
            if (s->actor && coop_rd(s->actor + COOP_ACTOR_STATE, &st, 4) && (st < 2 || st > 6)) {
                rt_log("coop: slot %d: its Player went away (state %d)", i, st);
                s->actor = 0, s->state = s->state == LIVE ? WANTED : FREE;
                continue;
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
