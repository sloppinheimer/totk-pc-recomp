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
} NetState;
_Static_assert(sizeof(NetState) == 96, "NetState wire size");

int coop_net_on(void);                                         // starts the thread once; 1 when networking is on
int coop_net_id(void);                                         // this player's id, 0 before the relay answered
void coop_net_publish(const NetState *st);                     // this player's latest state
int coop_net_remote(int id, NetState *out, uint64_t *age_ns);  // player id's latest state; 0 if not here
