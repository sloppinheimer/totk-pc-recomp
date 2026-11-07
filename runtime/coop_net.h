// Co-op networking (coop_net.c): this player's state out, the other players' states in.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define COOP_MAX_PLAYERS 8
#define COOP_NET_VERSION 20   // packet header version: games and relays of another version ignore each other

// One player's state, sent about 30 times a second. Positions and directions are world space; the sticks are as the
// player's own camera saw them, with the camera's horizontal forward and right so a receiver can turn them into its
// own camera's frame.
// A world object the player shot, made or moved (coop_players.c, "world objects"). Its own (arrows, fused materials,
// dropped and made objects): shown in the other games as a copy, by the owner's number. One every game has (a
// placed rock it moves): COOP_OBJ_KEYED | its key, and the other games move their own to match.
// Sent in OBJS packets of their own (coop_net_objs_send): one when it changes, again once a second, and once with an
// empty name when it is gone. COOP_OBJS a player at most; COOP_OBJS_PER_PACKET in one packet.
#define COOP_OBJS 256
#define COOP_OBJS_PER_PACKET 16
#define COOP_OBJ_KEYED 0x80000000u
typedef struct {
    uint32_t id;                      // the owner's number for it, or COOP_OBJ_KEYED | key (0: an empty entry)
    char name[47];                    // its actor name (NormalArrow, SpObj_LiftGeneratorWing_A_01...)
    uint8_t flags;                    // COOP_OBJ_AUTOBUILT: a device made by the owner's autobuild; COOP_OBJ_LOCAL:
                                      // pos and q are in the frame of the owner's bow's model (a nocked arrow)
    float pos[3], q[4];               // world position, rotation as a quaternion x y z w
} CoopObj;
#define COOP_OBJ_AUTOBUILT 1
#define COOP_OBJ_LOCAL 2
#define COOP_OBJ_FUSED 4   // a material fused to one of the owner's arrows
#define COOP_OBJ_ONLINK 8  // pos and q are in the owner's Link's frame (its paraglider)
// One slot of a Link's animation player (TotK Online sends seven to its stand-in; read for behaviour only): the clip
// (0xffff none), its weight * 65535, its frame and how many frames a second it moves.
#define COOP_ANIM_SLOTS 7
typedef struct { uint16_t idx, weight; float frame, rate; } CoopAnim;
typedef struct {
    uint32_t seq, flags;              // flags bits 8..10: weapon, bow, shield in hand (puppet mode draws them);
                                      // bit 11: the shield is placed as shield_pos / shield_q say (a shield surf)
                                      // bit 12: lightning is about to strike this Link (its sparks; coop_players.c)
    float pos[3], rot[9];
    float cam_fwd[2], cam_right[2];   // x, z
    uint64_t buttons;                 // Switch buttons (input_map.h B_*)
    int32_t sticks[4];                // lx, ly, rx, ry (-32767..32767)
    char gear[6][32];                 // equipped actor names: weapon, bow, shield, head, body, legs ("" unknown, "-" none)
    float day_seconds;                // time of day, 0..86400 (-1 unknown): the host's is followed
    int32_t weather;                  // weather type at the player (-1 unknown): the host's is followed
    int8_t dye[6];                    // per gear slot: the armor's dye colour (the create's 4th argument; -1 none)
    uint8_t ride_owner, pad;          // riding: the player whose object this Link stands on (0 none)
    char fuse[3][32];                 // weapon, bow, shield: the fused material's actor name ("" none; the weapon
                                      // create's 7th argument, e.g. AsbObj_RockParts_C_S_01)
    uint32_t ride_id;                 // riding: that object's id as its owner sends it (CoopObj.id)
    float ride_off[3];                // riding: where this Link is in the object's frame
    float shield_pos[3], shield_q[4]; // with flags bit 11: the shield in this Link's frame (rotation as x y z w)
    uint16_t life, life_max;          // this Link's life and most life, quarter hearts (0: unknown)
    float shield_fuse_scale[3];       // the scale (actor +0x2e4) of the actor fused to the shield (0: none or unknown)
    CoopAnim anim[COOP_ANIM_SLOTS];   // this Link's animation player's first slots (coop_world.c, coop_anim_pack)
    uint32_t anim_pad;
} NetState;
_Static_assert(sizeof(NetState) == 464 + 12 * COOP_ANIM_SLOTS + 4, "NetState wire size");
#define COOP_GEAR_SLOTS 6
extern const int coop_gear_slot[COOP_GEAR_SLOTS];   // the EquipmentUser slots they are (coop_players.c)

int coop_net_on(void);                                         // starts the thread once; 1 when networking is on
int coop_net_id(void);                                         // this player's id, 0 before the relay answered
void coop_net_publish(const NetState *st);                     // this player's latest state
int coop_net_remote(int id, NetState *out, uint64_t *age_ns);  // player id's latest state; 0 if not here

// The shared world (coop_world.c): each game sends the enemies it runs (latest wins) and hits on the others' (queued).
void coop_net_world_send(const void *data, int len);               // up to 1800 bytes
int coop_net_world(int id, void *out, int max, uint64_t *age_ns);  // the latest from player id; 0 if none
// The skeleton pose (POSE packets, coop_world.c packs and applies them): at most COOP_POSE_MAX bytes.
#define COOP_POSE_MAX 4000
void coop_net_pose_send(const void *data, int len);
// Enemy poses (EPOSE packets, coop_world.c's enemy puppets): queued both ways, at most COOP_EPOSE_MAX bytes each.
#define COOP_EPOSE_MAX (244 + COOP_POSE_MAX)
void coop_net_epose_send(const void *data, int len);
int coop_net_epose_take(void *out, int max, int *from, uint64_t *age_ns);   // the oldest; its length, -1 too big, 0 none
int coop_net_pose(int id, void *out, int max, uint64_t *age_ns);   // the latest from player id; its length (0 none)
// World objects (OBJS packets, coop_players.c): a u16 count, u16 0, then that many CoopObj. Queued both ways.
#define COOP_OBJS_PACKET_MAX (4 + COOP_OBJS_PER_PACKET * (int)sizeof(CoopObj))
void coop_net_objs_send(const void *data, int len);
int coop_net_objs_take(void *out, int max, int *from);             // the oldest received; its length, 0 when none
#define COOP_HIT_MAX 1024   // a hit on a Link carries the hit as delivered and its damage record's changed words (coop_world.c, link_hits_send)
void coop_net_hit_send(const void *data, int len);                 // up to COOP_HIT_MAX bytes
int coop_net_hit_take(void *out, int max, int *from);              // the oldest hit received; 0 when none

// Joining and leaving while the game runs (the menu's Co-op tab; TOTK_COOP_RELAY joins at start).
void coop_net_join(const char *relay, const char *name);   // relay: host[:port], 41900 by default
void coop_net_leave(void);
const char *coop_net_target(void);                         // the relay joined or being joined, "" none
const char *coop_net_error(void);                          // why the last join failed, "" none
int coop_net_players(int *ids, char (*names)[17], int max);   // the other players here

// The relay inside this game (coop_relay.c): Host starts it and joins it at 127.0.0.1.
int coop_relay_start(int port);
void coop_relay_stop(void);
int coop_relay_port(void);       // 0 when none runs here
int coop_relay_players(void);
int coop_local_address(char *out, size_t n);   // this PC's LAN address, for the other players
#define COOP_PORT 41900
void coop_host(const char *name);   // the relay here, joined at 127.0.0.1
void coop_end(void);                // leave, and stop the relay here if any
void coop_session_tests(int64_t world_us);

// The router (coop_upnp.c): Host asks it to forward UDP COOP_PORT here (UPnP), Leave takes that back.
enum { COOP_UPNP_NONE, COOP_UPNP_WORKING, COOP_UPNP_OPEN, COOP_UPNP_OPEN_NOIP, COOP_UPNP_SHARED, COOP_UPNP_FAILED };
void coop_upnp_open(void);
void coop_upnp_close(void);
int coop_upnp_status(char *detail, size_t n);   // COOP_UPNP_*; detail: the internet address, or why it failed