// What the files split out of coop_world.c share with it (docs/coop/enemy-sync-spec.md B.28). Declarations only.
#ifndef COOP_WORLD_INT_H
#define COOP_WORLD_INT_H
#include <stdint.h>
#include <stdatomic.h>
#include "cpu.h"

int coop_player_actors(uint64_t *actors, int *net_ids, int max);   // coop_players.c
void coop_actors_all(void (*fn)(uint64_t actor, const char *name, uint32_t key, void *arg), void *arg);   // coop_world.c: every actor of the reference pool
uint32_t coop_world_actor_key(uint64_t a, const char *name, const float home[3]);   // coop_world.c: an actor's key (an enemy's, a placed one's)
int coop_world_live_now(uint64_t a);   // coop_world.c: the actor ran its calc in the last second

// The rotating ground (coop_ground.c).
#define GROUND_KIND 0x5247   // "GR"
#define GR_MAX 16
typedef struct { uint32_t key; float angle, speed; uint8_t on, inv, pad[2]; } GroundState;
typedef struct { uint32_t kind, n; GroundState g[GR_MAX]; } GroundPacket;
_Static_assert(sizeof(GroundPacket) <= COOP_HIT_MAX, "GroundPacket fits a HIT packet");
void ground_tick(Cpu *c, uint64_t me);
void ground_take(const GroundPacket *p, int n);

// The players' body poses and the stand-in (coop_standin_pose.c); the older enemy sync's bone packets use its numbers.
#define POSE_BONE 12   // bytes a bone takes in a pose packet
int16_t coop_pose_q16(float v, float scale);
void coop_pose_w_of(const float *m, float A[3][3], float t[3]);
int coop_pose_inv3(const float a[3][3], float o[3][3]);
float coop_pose_fk_error(const float (*w)[16], const float (*rec)[24], int i, int p);
void coop_puppet_tick(uint64_t me);
void coop_find_pose_binding(uint64_t actor);
// coop_world.c: a Link's path from the actor to its body skeleton, the skeleton test, and what the pose tests found.
extern const uint32_t coop_skel_path[8];
int coop_is_skeleton(uint64_t s);
extern _Atomic uint64_t coop_pose_comp, coop_bindings_seen[16], coop_pose_binding;

#endif
