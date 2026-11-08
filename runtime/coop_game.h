// Co-op: game structures and calls shared by coop_players.c (the feature) and coop_spawn.c (tests and tracing).
// Addresses are TotK 1.4.3 main offsets; docs/coop/re-notes.md says how each was found.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#define COOP_SCENE_SLOT 0x3add238      // [main+] -> +0x1e8 scene; scene component 13 = resident actors
#define COOP_ACTOR_MGR_SLOT 0x395e008  // [[main+]] = the actor manager requestCreateActor takes
#define COOP_REQUEST_CREATE 0x599670   // ActorMgr requestCreateActor
#define COOP_CLAIM_ACTOR 0x1877160     // (handle, link out, 1, 0) once the handle reads 3
#define COOP_SET_DEPENDENCY 0x5b811c   // (actor system, parent, child)
#define COOP_ACTOR_SYSTEM_SLOT 0x3ad6d80
#define COOP_EDGE_TABLE_SLOT 0x395da68
#define COOP_INPUT_MGR_SLOT 0x3960150  // [[main+]] = controller manager; [mgr+0xa0] controller 0
#define COOP_FORCE_MATRIX 0x45ce70     // (actor, const float m[12] rows rotation | position, 0): teleport
#define COOP_ACTOR_CALC 0x41bb20       // per-actor update, x0 = actor
#define COOP_REQUEST_DELETE 0x63baa8   // (actor, reason): ignored once the actor is going away (state 7..11)
#define COOP_DELETE_REASON 3           // what OneShotGenericDeleteThisActor passes
#define COOP_ACTOR_NAME 0x218
#define COOP_ACTOR_POS 0x2b4
#define COOP_ACTOR_ROT 0x2c0
#define COOP_ACTOR_STATE 0x24          // under 2 not ready, 7..11 going away

int coop_rd(uint64_t a, void *out, size_t n);
uint64_t coop_rd_ptr(uint64_t a);
const char *coop_actor_name(uint64_t actor, char *buf, size_t n);
uint64_t coop_resident_named(const char *name, uint64_t *ref_out);   // actor (and its reference object) or 0
int coop_is_resident(uint64_t actor);
uint64_t coop_player_actor(void);                                    // the resident "Player": this PC's Link
uint64_t coop_guest_call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

// Spawning: a request fills handle (0x40 bytes, kept alive until claimed); coop_handle_status reads 3 when the actor
// exists; coop_claim puts it in the world and returns its actor, with its reference object in *ref.
int coop_request_create(Cpu *c, const char *name, const float pos[3], const float rot[9], uint8_t *handle);
// The same, made as an autobuild makes its devices: the create argument's +0x80 (the create information's +0x78 the
// actor's constructor reads) holds the autobuild's type object (coop_game.c).
int coop_request_create_autobuilt(Cpu *c, const char *name, const float pos[3], const float rot[9], uint8_t *handle);
int coop_handle_status(const uint8_t *handle);
uint64_t coop_claim(Cpu *c, uint8_t *handle, uint8_t *link, uint64_t *ref);
// Gives a new Player the first Player's update links to the resident helpers (ThrowBox, CarryBox, ...).
void coop_copy_dependencies(Cpu *c, uint64_t model, uint64_t copy, int verbose);

// A controller's input fields (controller layout in re-notes): Switch buttons (input_map.h B_*) and sticks -1..1.
// prev and held carry the previous frame's mask and the per-bit hold counters for this controller.
// The shared world (coop_world.c): every actor the per-actor calc updates, and the per-frame sync.
void coop_actor_seen(uint64_t actor);
void coop_actors_live(void (*fn)(uint64_t actor, void *arg), void *arg);
void coop_world_tick(Cpu *c, uint64_t me);
int coop_world_role(void);                              // coop_players.c: 0 alone, 1 host, 2 guest
int coop_player_positions(float (*out)[3], int max);   // the remote Links' positions
int coop_player_positions_ids(float (*out)[3], int *ids, int max);   // ... and their players' ids

void coop_fill_controller(uint64_t ctl, uint64_t buttons, const float stick[4], uint32_t *prev, float held[32]);
