// Co-op horses and animals (coop_mounts.c): which actors the shared world syncs besides enemies, who runs a ridden
// horse, and where a remote rider sits on this game's horse.
#pragma once
#include <stdint.h>

#include "cpu.h"

// What the shared world (coop_world.c) syncs an actor as, by its name; 0: not synced there.
#define COOP_KIND_ENEMY 1    // "Enemy_..." (always)
#define COOP_KIND_HORSE 2    // "GameRomHorse..." (not its saddles and reins) and "Animal_Horse...", TOTK_COOP_MOUNTS=1
#define COOP_KIND_ANIMAL 3   // the other "Animal_..." that walk (not fish, insects, birds or swarms), TOTK_COOP_MOUNTS=1
int coop_world_kind(const char *name);
const char *coop_world_kind_word(int kind);   // "enemy", "horse", "animal" (for the logs)
int coop_mounts_on(void);                     // TOTK_COOP_MOUNTS=1 and the native enemy model
// Kinds whose run-time-made actors the spawn sync makes in the other games: enemies, horses (a horse called at a
// stable or by whistle is made at run time); animals only with TOTK_COOP_MOUNTS_SPAWN=all (an area makes its own in
// each game, so a copy of each would double them).
int coop_world_spawn_kind(const char *name);
// Kinds whose action requests (main+0x50ad74) the copies replay from their owner: enemies and horses (TotK Online's
// BSA list has GameRomHorse and no Animal; read for behaviour only).
int coop_world_action_kind(const char *name);

// Each frame, from coop_world.c's peer_tick: this Link's ride, and the horses this game has.
void coop_mounts_tick(Cpu *c, uint64_t me);
// The player whose Link rides the horse of that key (this game's id when it is this Link), 0 none.
int coop_mount_rider(uint32_t key, int my_id);
// This Link rides a horse: its key and where this Link is in the horse's frame (for NetState.ride_*).
int coop_mount_riding(uint32_t *key, float off[3]);
// A remote rider's place here: on this game's horse of that key, at off in its frame. 0: this game has no such horse.
int coop_mount_place(uint32_t key, const float off[3], float out[3]);

// coop_world.c, for coop_mounts.c: each live actor it syncs that is not an enemy, with its key and kind; who runs
// a key here (0 this game, a player id, -1 not known).
void coop_world_each_mount(void (*fn)(uint64_t actor, const char *name, uint32_t key, int kind, void *arg), void *arg);
int coop_world_owner_of(uint32_t key);
// coop_players.c, for coop_mounts.c: the remote player whose state says they ride the horse of that key, 0 none.
int coop_player_riding_mount(uint32_t key);
