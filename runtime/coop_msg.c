// Co-op: the transport's one inbox, shared by the syncs that read it (docs/coop/enemy-sync-spec.md section 16.1).
//
// coop_ctr.c has one inbox of reliable messages for every type and one queue of join and leave events, and until now
// the entity sync (coop_ent*.c) took all of both and dropped a type it did not know. Here each reader asks for its
// own: a message of another reader's type range, met on the way, is put in that reader's queue (in arrival order),
// and every join or leave is given to each reader. Nothing is dropped: when another reader's queue is full the
// caller gets nothing more until that reader has taken some (the transport then waits, as it does when its own inbox
// is full), and the log says so once.
//
// Readers: COOP_MSG_ENT (the entity sync: every type but the enemy sync's) and COOP_MSG_ENEMY (types 60 to 89). A
// reader that has not been opened (coop_msg_open) has no queue: its messages are dropped, as an unknown type was
// before, and its events are not kept.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctr.h"
#include "coop_msg.h"
#include "host.h"
#include "platform/platform.h"

#define MQ_N 512    // messages waiting for one reader
#define EQ_N 64     // join and leave events waiting for one reader

typedef struct { uint64_t inc; int id, joined; } PeerEv;
static struct {
    int open;
    CoopCtrMsg *q;
    uint32_t r, w;
    PeerEv ev[EQ_N];
    uint32_t er, ew;
    uint32_t dropped, said_full;
} rd[COOP_MSG_READERS];
static CoopCtrMsg held;   // a message for a reader whose queue is full: offered again at the next call
static int have_held;
static PlatMutex lock = PLAT_MUTEX_INIT;

static void lock_take(void) { plat_mutex_lock(&lock); }

static int reader_of(int type) { return type >= 60 && type <= 89 ? COOP_MSG_ENEMY : COOP_MSG_ENT; }

void coop_msg_open(int reader) {
    if (reader < 0 || reader >= COOP_MSG_READERS) return;
    lock_take();
    rd[reader].open = 1;
    plat_mutex_unlock(&lock);
}

// 1: the message stays with the caller (m is its reader's, or nobody's). 0: it was put in another reader's queue.
// -1: that queue is full; the message is held and nothing more is taken from the transport for now.
static int route(int reader, CoopCtrMsg *m) {
    int to = reader_of(m->type);
    if (to == reader) return 1;
    if (!rd[to].open) { rd[to].dropped++; return 0; }
    if (!rd[to].q) rd[to].q = calloc(MQ_N, sizeof *rd[to].q);
    if (!rd[to].q || rd[to].w - rd[to].r >= MQ_N) {
        held = *m, have_held = 1;
        if (!rd[to].said_full++) rt_log("coop: msg: the queue of reader %d is full (%d messages): the transport waits until it reads", to, MQ_N);
        return -1;
    }
    rd[to].q[rd[to].w++ % MQ_N] = *m;
    return 0;
}

int coop_msg_recv(int reader, CoopCtrMsg *m) {
    if (reader < 0 || reader >= COOP_MSG_READERS) return 0;
    // Test (TOTK_COOP_MSG_DIRECT=1): the entity sync reads the transport itself, as before this file, for comparing.
    static int direct = -1;
    if (direct < 0) direct = cfg_get("TOTK_COOP_MSG_DIRECT") != NULL;
    if (direct && reader == COOP_MSG_ENT) return coop_ctr_recv(m);
    // With no other reader open there is nothing to set aside: the one reader takes from the transport as it always
    // did (the same call, no lock of this file), so a session without the enemy sync is as it was before this file.
    if (!rd[reader == COOP_MSG_ENT ? COOP_MSG_ENEMY : COOP_MSG_ENT].open && !have_held && !(rd[reader].q && rd[reader].r != rd[reader].w))
        return coop_ctr_recv(m);
    lock_take();
    int got = 0;
    if (rd[reader].q && rd[reader].r != rd[reader].w) {
        *m = rd[reader].q[rd[reader].r++ % MQ_N], got = 1;
    } else {
        for (;;) {
            if (have_held) {   // the message that found a full queue: its reader may have read since
                CoopCtrMsg h = held;
                have_held = 0;
                int k = route(reader, &h);
                if (k < 0) break;
                if (k > 0) { *m = h, got = 1; break; }
                continue;
            }
            if (!coop_ctr_recv(m)) break;
            int k = route(reader, m);
            if (k > 0) { got = 1; break; }
            if (k < 0) break;
        }
    }
    plat_mutex_unlock(&lock);
    return got;
}

int coop_msg_peer_event(int reader, uint64_t *inc, int *id, int *joined) {
    if (reader < 0 || reader >= COOP_MSG_READERS) return 0;
    lock_take();
    PeerEv e;
    while (coop_ctr_peer_event(&e.inc, &e.id, &e.joined))   // every event to every open reader (and to the asker)
        for (int k = 0; k < COOP_MSG_READERS; k++)
            if ((rd[k].open || k == reader) && rd[k].ew - rd[k].er < EQ_N) rd[k].ev[rd[k].ew++ % EQ_N] = e;
    int got = 0;
    if (rd[reader].er != rd[reader].ew) {
        e = rd[reader].ev[rd[reader].er++ % EQ_N];
        *inc = e.inc, *id = e.id, *joined = e.joined, got = 1;
    }
    plat_mutex_unlock(&lock);
    return got;
}
