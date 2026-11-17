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

// A blow's contacts (coop_contact.c). brel: where the contact's body sat from the struck actor's main body, plus 1.
#define CONTACT_SZ 0x1c0
#define CONTACT_MAX 2
typedef struct { uint32_t key, to, kind, n, brel[CONTACT_MAX]; uint8_t e[CONTACT_MAX][CONTACT_SZ]; } ContactPacket;
_Static_assert(sizeof(ContactPacket) <= COOP_HIT_MAX, "ContactPacket fits a HIT packet");
#define ECONTACT_KIND 0x4543   // "CE"
#define ECONTACT_ARROW 0x4541  // "AE"
void coop_contact_take(const ContactPacket *cp, int from);   // a "CE"/"AE" packet from player `from`
uint64_t coop_actor_ref(uint64_t a);   // an actor's reference object (+0x40 the actor), 0 none found
extern uint64_t coop_world_me;         // coop_world.c: this game's Link
uint32_t coop_world_enemy_key(const char *name, const float home[3]);   // coop_world.c: the key of an actor placed nowhere
uint32_t coop_world_name_hash(uint64_t a);                             // coop_world.c: an actor's name's hash
int coop_actor_placed(uint64_t a);                                      // coop_world.c: an actor of the map's placement
extern uint64_t coop_world_enemies[512];                                // coop_world.c: every live enemy here, as last listed
extern int coop_world_n_enemies;
int coop_life_of(uint64_t a);                                           // coop_world.c: an actor's life points, -1 without a life component

// The sync of enemies made at run time (coop_spawn_sync.c).
uint32_t coop_world_spawn_key(uint64_t a);   // 0: not an actor the spawn sync announced or made
#define SPAWN_KIND 0x5053   // "SP"
#define SPAWN_GONE 0x4753   // "SG"
typedef struct { uint32_t key, to, kind, pad; char name[48]; float pos[3], rot[9]; } SpawnPacket;
_Static_assert(sizeof(SpawnPacket) <= COOP_HIT_MAX, "SpawnPacket fits a HIT packet");
void spawn_tick(Cpu *c);
void spawn_take(Cpu *c, const SpawnPacket *sp, int from);

#endif
