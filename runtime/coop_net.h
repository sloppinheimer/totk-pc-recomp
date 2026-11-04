// Co-op networking (coop_net.c): this player's state out, the other players' states in.
#pragma once
#include <stdint.h>

#define COOP_MAX_PLAYERS 8

// One player's state, sent about 30 times a second. Positions and directions are world space; the sticks are as the
// player's own camera saw them, with the camera's horizontal forward and right so a receiver can turn them into its
// own camera's frame.
typedef struct {
    uint32_t seq, flags;
    float pos[3], rot[9];
    float cam_fwd[2], cam_right[2];   // x, z
    uint64_t buttons;                 // Switch buttons (input_map.h B_*)
    int32_t sticks[4];                // lx, ly, rx, ry (-32767..32767)
    char gear[6][32];                 // equipped actor names: weapon, bow, shield, head, body, legs ("" unknown, "-" none)
    float day_seconds, reserved;      // time of day, 0..86400 (-1 unknown): the host's is followed
} NetState;
_Static_assert(sizeof(NetState) == 296, "NetState wire size");
#define COOP_GEAR_SLOTS 6
extern const int coop_gear_slot[COOP_GEAR_SLOTS];   // the EquipmentUser slots they are (coop_players.c)

int coop_net_on(void);                                         // starts the thread once; 1 when networking is on
int coop_net_id(void);                                         // this player's id, 0 before the relay answered
void coop_net_publish(const NetState *st);                     // this player's latest state
int coop_net_remote(int id, NetState *out, uint64_t *age_ns);  // player id's latest state; 0 if not here
