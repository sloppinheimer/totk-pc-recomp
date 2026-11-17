// Co-op: where the entity sync (coop_ent*.c, the contraption sync) is run from each frame, and the hook that keeps a
// copy of another player's contraption from moving or charging anything here (coop_phase0_hook). The Autobuild
// research tests (coop_autobuild.c) run after the sync from the same places.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "coop_ctn.h"
#include "coop_ent.h"
#include "coop_game.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

void coop_ent_saved_tick(Cpu *c);                 // coop_ent_id.c: the devices of a save
void coop_ent_bench_tick(Cpu *c, uint64_t me);    // coop_ent_bench.c: TOTK_COOP_BENCH
void coop_ent_log_tick(Cpu *c, uint64_t me);      // coop_ent_log.c: the session record
void coop_autobuild_seq_tests(Cpu *c);            // coop_autobuild.c
void coop_autobuild_tick(Cpu *c, uint64_t me);    // coop_autobuild.c
int coop_ab_log_on(void);                         // coop_autobuild.c: TOTK_COOP_TEST_ABLOG

// From GameDataMgr::calc, at actor phase 0 outside the parallel actor update, where every actor link resolves
// (coop_spawn.c coop_hook).
void coop_ent_seq_calc(Cpu *c) {
    coop_ent_phase0(c);
    coop_autobuild_seq_tests(c);
}

// The co-op tick while another player is in the session (coop_hitchan.c coop_hitchan_tick).
void coop_ent_tick(Cpu *c, uint64_t me) {
    coop_ent_mirror_tick();   // the object mirror's answers to the entity sync (this is the tick its tables live on)
    coop_ent_drive(c);
    coop_ent_saved_tick(c);
    coop_ent_bench_tick(c, me);
    coop_ent_log_tick(c, me);
    coop_autobuild_tick(c, me);
}

// The contraption copies' gates (coop_phase0_hook, run from coop_spawn.c coop_hook and coop_life.c): while the
// contraption sync is on (coop_ctn_core), a part of another player's contraption made here is moved only by the game
// that owns it. Its fan pushes nothing, its device nodes do not write its bodies, its wheel and cart motors are skipped,
// a hit's toggle on it goes to its owner, and its on/off node charges no gauge here when another player pays for it.
#define APPLY_IMPULSE_FN 0x2487104
#define GAUGE_CONSUME 0x1fa4ca8
#define GAUGE_CONSUME_RATE 0x1fa4ed4
#define ONOFF_CALC 0x9f9088
#define ONOFF_SET 0x9fad68
static __thread int in_copy_node, in_copy_force, in_paid_node, in_force_inert;
int coop_ab_force_inert(void) { return in_force_inert; }
// The wheel that runs is ExecuteFastWheelMotion (wrapper main+0x22fa1b0, x0 = its node); main+0x22fc5f8 in motor_nodes
// below is a twin no shipped graph uses (research notes sync-v3/m2/p5/wheels, section 0). A followed wheel's node
// runs once with its inert test forced true: that is the game's own disabled branch, which takes the three motor
// constraints off their bodies and disables them. After that the node is skipped, so the spin and steering floats the
// model reads stay as coop_ctn.c writes them from the game that runs the build. A wheel no longer followed leaves the
// table and its node runs again, re-anchoring by itself on its next working tick.
// A slot holds the actor's id (actor +0x10) too: a new copy made at a freed copy's address is another wheel. A slot
// whose wheel is no longer followed (its copy deleted, remade or unloaded: its node never runs again to free it) is
// taken back when a new one needs room (review 1 findings 26, 60, 82).
#define WHEEL_N 64
static struct { _Atomic uint64_t node, actor; _Atomic uint32_t gen; } wheel_off[WHEEL_N];
static uint32_t actor_gen(uint64_t a) {
    uint32_t g = 0;
    return a && coop_rd(a + 0x10, &g, 4) ? g : 0;
}
static PlatMutex wheel_lock = PLAT_MUTEX_INIT;   // (two game threads claiming and sweeping at once: review 2 finding 18)
static int wheel_slot_locked(uint64_t node, uint64_t actor, int add);
static int wheel_slot(uint64_t node, uint64_t actor, int add) {
    plat_mutex_lock(&wheel_lock);
    int k = wheel_slot_locked(node, actor, add);
    plat_mutex_unlock(&wheel_lock);
    return k;
}
static int wheel_slot_locked(uint64_t node, uint64_t actor, int add) {
    uint32_t gen = actor_gen(actor);
    for (int i = 0; i < WHEEL_N; i++)
        if (atomic_load(&wheel_off[i].node) == node && atomic_load(&wheel_off[i].actor) == actor && atomic_load(&wheel_off[i].gen) == gen) return i;
    for (int pass = 0; add && pass < 2; pass++) {
        for (int i = 0; i < WHEEL_N; i++) {
            uint64_t none = 0;
            if (atomic_compare_exchange_strong(&wheel_off[i].node, &none, node)) {
                atomic_store(&wheel_off[i].gen, gen), atomic_store(&wheel_off[i].actor, actor);
                return i;
            }
        }
        for (int i = 0; i < WHEEL_N; i++) {   // (full: the slots of wheels no longer followed freed)
            uint64_t a = atomic_load(&wheel_off[i].actor);
            if (!a || !coop_ctn_driven(a) || actor_gen(a) != atomic_load(&wheel_off[i].gen)) atomic_store(&wheel_off[i].actor, 0), atomic_store(&wheel_off[i].node, 0);
        }
    }
    if (add) {
        static _Atomic int said;
        if (!atomic_fetch_add(&said, 1)) rt_log("coop: contraption: the table of followed wheels is full (%d): a wheel's node skipped without its inert run", WHEEL_N);
    }
    return -1;
}
// The device nodes that push their own part (research devices2/: rocket, sled, balloon, wing, hover stone, stabilizer,
// spring, stake, homing cart, steering stick): on a copy their body writes are skipped; the wheel's and the cart's
// motor nodes drive joints, and are skipped whole
static const uint32_t force_nodes[] = {0x24343f4, 0x2458320, 0x222d694, 0x221cd04, 0x230a3fc, 0x24730dc, 0x221a7dc, 0x221420c,
                                       0x225c52c, 0x226abd4};
static const uint32_t motor_nodes[] = {0x22fc5f8, 0x2251440};
static const uint32_t body_setters[] = {0x567750, 0x25a1f0, 0x29ae114, 0x29ae7a8, 0x29ae7fc, 0x29a4afc};
static int in_list(const uint32_t *l, int n, uint64_t off) {
    for (int i = 0; i < n; i++) if (l[i] == off) return 1;
    return 0;
}
#define DEVSWITCH_UPDATE 0x4a3500
static uint64_t node_actor(uint64_t node) {   // an AI node's actor (main+0x9a89c4)
    return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(node + 8) + 8) + 0x1f8);
}
static int wrap_call(Cpu *c, uint64_t addr, int *flag, int on) {   // run the function with *flag set
    int was = *flag;
    *flag = on;
    c->pc = addr;
    rt_dispatch(c);
    *flag = was;
    return 1;
}
int coop_phase0_hook(Cpu *c, uint64_t addr) {
    uint64_t off = addr - MOD_MAIN_BASE;
    static __thread int inner;   // (the hooked function's own entry, reached again through rt_dispatch)
    if (off == GAUGE_CONSUME || off == GAUGE_CONSUME_RATE) coop_ctn_gauge_seen(c->x[0]);
    if (inner) { inner = 0; return 0; }
    // A part of another player's contraption made here (TOTK_COOP_CONTRAPTION): its fan pushes nothing (the owner's game
    // moves the contraption; the wind capsule and the look still run), and its on/off node charges no gauge
    if (off == APPLY_IMPULSE_FN && coop_ctn_core() && coop_ctn_driven(node_actor(c->x[0]))) return 1;
    if (coop_ctn_core()) {
        if (off == ONOFF_CALC) {   // (the node of each device, and a switch event put on a followed one here: coop_ctn.c)
            uint64_t actor = node_actor(c->x[0]);
            coop_ctn_note_onoff(c->x[0], actor);
            if (coop_ctn_follows(actor)) coop_ctn_event_seen(actor);
        }
        if (off == 0x22fa1b0) {
            uint64_t node = c->x[0], actor = node_actor(node);
            if (!coop_ctn_driven(actor)) {
                plat_mutex_lock(&wheel_lock);
                int k = wheel_slot_locked(node, actor, 0);
                if (k >= 0) atomic_store(&wheel_off[k].actor, 0), atomic_store(&wheel_off[k].node, 0);
                plat_mutex_unlock(&wheel_lock);
                return 0;
            }
            if (wheel_slot(node, actor, 0) >= 0) return 1;
            if (wheel_slot(node, actor, 1) < 0) return 1;
            inner = 1;
            return wrap_call(c, addr, &in_force_inert, 1);
        }
        if (in_list(body_setters, 6, off) && in_copy_force) {   // (a copy's device moving its own body: not here)
            // but a followed spring's launch is made here: its two velocity writes (the calls that return to
            // main+0x221a9c0 and 0x221aa14) are the head thrown out and the base pushed back, and what stands on the
            // head is thrown by that contact in this game (research notes sync-v3/m2/p5/spring-stake)
            uint64_t lr = c->x[30] - MOD_MAIN_BASE;
            if (off == 0x567750 && (lr == 0x221a9c0 || lr == 0x221aa14)) return 0;
            return 1;
        }
        if (in_list(motor_nodes, 2, off) && coop_ctn_driven(node_actor(c->x[0]))) return 1;
        if (in_list(force_nodes, 10, off) && coop_ctn_driven(node_actor(c->x[0]))) {
            inner = 1;
            return wrap_call(c, addr, &in_copy_force, 1);
        }
        if (off == DEVSWITCH_UPDATE) {   // a toggle asked on a copy's device (a hit here): taken to the owner instead
            uint64_t comp = c->x[0], actor = coop_rd_ptr(comp + 0x18);
            uint8_t b8d = 0;
            {   // (the hook's view of a toggle request on a contraption part: the first few, for the toggle test)
                uint8_t bb = 0;
                static _Atomic int said;
                if (coop_rd(comp + 0x8d, &bb, 1) && (bb & 8) && coop_contraption_made(actor) && atomic_fetch_add(&said, 1) < 6)
                    rt_log("coop: contraption: switch update of a copy part with a toggle request: driven %d, ours %d", coop_ctn_driven(actor), 0);
            }
            if (coop_ctn_follows(actor) && coop_rd(comp + 0x8d, &b8d, 1) && (b8d & 8)) {
                __atomic_fetch_and((uint8_t *)(uintptr_t)(comp + 0x8d), (uint8_t)~8, __ATOMIC_SEQ_CST);
                coop_ctn_toggle_asked(comp, actor);
            }
            // A device another player pays for may always be on here. The update ends by writing "this game's gauge
            // can pay, and the device is in range of this game's Link" into +0x8d bit 0, and the on/off node forces
            // a device off and refuses on while it is clear: a watcher's empty gauge or distance would switch its
            // copy off. The bit is set again after every update (research notes sync-v3/m2/p5/energy, section 0).
            if (coop_ctn_no_charge(actor)) {
                static __thread int dummy;
                inner = 1;
                wrap_call(c, addr, &dummy, 1);
                __atomic_fetch_or((uint8_t *)(uintptr_t)(comp + 0x8d), (uint8_t)1, __ATOMIC_SEQ_CST);
                return 1;
            }
            return 0;
        }
    }
    if ((off == ONOFF_CALC || off == ONOFF_SET) && coop_ctn_core() && coop_ctn_no_charge(node_actor(c->x[0]))) {   // (another player pays)
        inner = 1;
        return wrap_call(c, addr, &in_copy_node, 1);
    }
    if ((off == ONOFF_CALC || off == ONOFF_SET) && coop_ctn_core() && coop_contraption_made(node_actor(c->x[0])) && coop_ab_log_on()) {
        inner = 1;   // (TOTK_COOP_TEST_ABLOG: a copy's device whose activator is this player: its charges logged)
        return wrap_call(c, addr, &in_paid_node, 1);
    }
    if ((off == GAUGE_CONSUME || off == GAUGE_CONSUME_RATE) && in_paid_node) {
        static _Atomic int64_t said_paid;
        int64_t now = frame_step_world_us();
        if (now - atomic_load(&said_paid) > 2000000) {
            float amount, g = 0;
            uint32_t bits = (uint32_t)c->v[0];
            memcpy(&amount, &bits, 4);
            coop_rd(c->x[0] + 0x54, &g, 4);
            atomic_store(&said_paid, now), rt_log("coop: contraption: a copy's device charges this player: %s %.3f, gauge %.1f", off == GAUGE_CONSUME ? "consume" : "consumeRate", amount, g);
        }
    }
    if ((off == GAUGE_CONSUME || off == GAUGE_CONSUME_RATE) && in_copy_node) {
        static _Atomic int said_copy;
        if (atomic_fetch_add(&said_copy, 1) < 3) rt_log("coop: contraption: a charge from a copy's device not made (%s)", off == GAUGE_CONSUME ? "consume" : "consumeRate");
        return 1;
    }
    return 0;
}
