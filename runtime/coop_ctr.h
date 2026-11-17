// Co-op: the contraption channel (coop_ctr.c): incarnations, reliable streams, latest-only slots and the heartbeat.
#pragma once
#include <stdint.h>

#include "coop_net.h"

// Message types (docs/coop/entity-sync.md, section 12.4). 200 and up are tests.
enum {
    COOP_CTR_STATE = 1, COOP_CTR_ACK = 2, COOP_CTR_CENSUS = 3, COOP_CTR_HB = 4, COOP_CTR_HELLO = 5,
    COOP_CTR_TAKE = 10, COOP_CTR_GRANT = 11, COOP_CTR_COMMIT = 12, COOP_CTR_REFUSE = 13, COOP_CTR_ABORT = 14,
    COOP_CTR_RECLAIM = 15, COOP_CTR_OFFER = 16, COOP_CTR_OFFER_ACK = 17, COOP_CTR_OFFER_DENY = 18,
    COOP_CTR_RELINQUISH = 19, COOP_CTR_VERSION = 20, COOP_CTR_PULL = 21, COOP_CTR_EDIT_REQ = 22, COOP_CTR_RETIRE = 23,
    COOP_CTR_TOGGLE = 30, COOP_CTR_HIT = 31, COOP_CTR_PUSH = 32, COOP_CTR_IMPULSE = 33, COOP_CTR_EVENT = 34,
    COOP_CTR_BLOB = 35,
};
// Flags of a message.
enum { COOP_CTR_RELIABLE = 1, COOP_CTR_SPEC = 2, COOP_CTR_BULK = 4 };
#define COOP_CTR_PAYLOAD (COOP_CTR_MAX - 24)

typedef struct {
    uint64_t from;      // the sender's incarnation
    int from_id;        // its relay id (the route) when it came
    uint8_t type, flags;
    int len;
    uint8_t d[COOP_CTR_PAYLOAD];
} CoopCtrMsg;

// The heartbeat each game sends 30 times a second from the co-op poll, menus and loads included.
enum { COOP_CTR_HB_PAUSED = 1, COOP_CTR_HB_LOADING = 2, COOP_CTR_HB_ENEMY = 4 };   // ENEMY: it runs the enemy sync of coop_enemy.c
typedef struct {
    uint64_t key;        // 0 (its latest-only slot)
    int64_t world_us;    // this game's world clock
    uint64_t wall_ns;    // wall clock when sent (the receiver's clock offset)
    uint32_t t3_count;   // frames of the contraption pass (later phases)
    uint32_t flags;      // COOP_CTR_HB_*: PAUSED is set here when the world clock stood 0.3 s
} CoopCtrHb;

void coop_ctr_tick(void);   // once per co-op poll

// Sending: to a relay id (0: every other player here), or to an incarnation. Reliable messages are queued per stream
// and sent again until acknowledged; 0 when nothing could be queued (no session, nobody to send to, a full queue).
int coop_ctr_send(int to, int type, int flags, const void *d, int len);
// Receiving: reliable messages in order per stream (1 one taken, 0 none); unreliable ones as the latest per (sender,
// type, key = the payload's first 8 bytes); its length, 0 none.
int coop_ctr_recv(CoopCtrMsg *m);
int coop_ctr_latest(int from_id, int type, uint64_t key, void *out, int max, uint64_t *age_ns);
// Players: this game's incarnation, a relay id's, the relay id an incarnation is on (0 none), and players coming and
// going by incarnation (1 one taken: joined 1 came, 0 left).
uint64_t coop_ctr_me(void);
uint64_t coop_ctr_incarnation(int id);
int coop_ctr_route(uint64_t inc);
int coop_ctr_peer_event(uint64_t *inc, int *id, int *joined);
// The heartbeat: what this game adds (its flags beyond PAUSED, its T3 count), and another player's latest.
void coop_ctr_hb_set(const CoopCtrHb *hb);
int coop_ctr_hb(int from_id, CoopCtrHb *out, uint64_t *age_ns);
