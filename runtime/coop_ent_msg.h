// Co-op: the messages of the entity sync (docs/coop/contraption-sync-v4-spec.md section 6), over coop_ctr.c.
// Every unreliable message starts with an 8-byte slot key: the transport keeps the latest per (sender, type, key).
#pragma once
#include <stdint.h>

#include "coop_ent.h"

enum {
    ENT_POSE = 40, ENT_ISLAND = 41, ENT_TAKE = 42, ENT_GRANT = 43, ENT_REFUSE = 44, ENT_OFFER_ALL = 45, ENT_NEED = 46,
    ENT_BOND_REQ = 47, ENT_BOND_ACK = 48, ENT_BOND_NAK = 49, ENT_REQ = 50, ENT_DEATH = 51, ENT_INDEX = 52, ENT_APPLIED = 53,
    ENT_REQ_ANS = 54, ENT_INDEX_END = 55,
};
enum { POSE_OFFER = 1, POSE_VEL = 2 };   // VEL: `v` was read from the root's body
enum { REF_NOT_OWNER = 1, REF_STALE, REF_HELD, REF_RIDDEN };
enum { NAK_NOT_OWNER = 1, NAK_GONE, NAK_FULL, NAK_NOT_HERE, NAK_POSE };
enum { REQ_TOGGLE = 1, REQ_EVENT };
enum { REQ_DONE = 1, REQ_REFUSED };

#pragma pack(push, 4)
typedef struct {
    uint64_t key;                    // hash of root
    Eid root; uint32_t epoch, digest;
    int64_t us;                      // the owner's world clock
    uint64_t payer;
    float m[12];                     // the root's main body matrix
    float v[3];                      // that body's linear velocity (body +0xf8), with POSE_VEL
    uint8_t cls, n, flags, pad;
    uint32_t sw, on, fw, fk, fb, fp, burn;   // bit r: member r in ascending Eid
    float pf[ENT_MEMBERS][4];        // sent for n members only
} PoseMsg;
#define POSE_LEN(n) ((int)(sizeof(PoseMsg) - sizeof(float) * 4 * (size_t)(ENT_MEMBERS - (n))))

typedef struct {                     // every piece carries it; the body follows: members, bonds, far rows, the recipe
    uint32_t counter; Eid root; uint16_t n, nb, nf; uint32_t digest; uint8_t rec_n, pad[3];
    float root_m[12]; uint32_t total; uint16_t piece, pieces;
} IslandHdr;
typedef struct {
    Eid id; uint64_t owner; uint32_t epoch; uint8_t life, death_kind, green, mir_owner;
    uint32_t mir_id; char name[48]; float scale[3], radius, rel_root[12];
} IslandMember;
typedef struct { Eid a, b; uint8_t sa, sb, ed, pad; float rel[12]; } IslandBond;
typedef struct { Eid id; uint64_t owner; uint32_t epoch; } IslandFar;

typedef struct { uint32_t id; Eid member; uint32_t epoch_seen, digest; uint8_t reason, layout_ok; } TakeMsg;
typedef struct { uint64_t asker; uint32_t id; uint64_t owner; uint32_t epoch, sw, on; uint64_t payer; uint8_t n; Eid m[2 * ENT_MEMBERS]; } GrantMsg;
#define GRANT_LEN(n) ((int)(sizeof(GrantMsg) - sizeof(Eid) * (size_t)(2 * ENT_MEMBERS - (n))))
typedef struct { uint32_t id; uint8_t reason; uint64_t owner; uint32_t epoch; } RefuseMsg;
typedef struct { Eid id; uint32_t number; } NeedMsg;            // id (0, 0): everything, with a number
typedef struct { uint32_t op; Eid x, t; uint8_t sx, st; uint32_t epoch_x; float rel[12]; } BondReq;
typedef struct { uint32_t op; uint8_t reason; uint64_t who; uint32_t epoch; } BondAns;   // ACK: reason 0
typedef struct { uint32_t id; Eid e; uint8_t kind; uint32_t value; } ReqMsg;
typedef struct { Eid e; uint8_t kind; } DeathMsg;
typedef struct { uint64_t key; uint32_t counter, count, hash; } IndexMsg;               // key: a constant
typedef struct { uint64_t key; Eid root; uint32_t digest; uint8_t state, why; } AppliedMsg;   // key: hash of root
typedef struct { uint32_t id; uint8_t ans; } ReqAns;
typedef struct { uint32_t number, count, hash; } IndexEnd;      // bulk stream, after the last ISLAND of the answer
#pragma pack(pop)

// coop_ctr.c: the latest-only slots of one type, one after another (*it starts at 0); 0 when there are no more
int coop_ctr_slot_next(int type, int *it, CoopCtrMsg *m, uint64_t *age_ns);
