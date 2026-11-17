// Co-op: the contraption channel (docs/coop/entity-sync.md, sections 6.1, 10.6, 12 and 13). Incarnations, a
// reliable layer over the net layer's CTR packets, latest-only slots for streamed state, and the heartbeat (HB).
//
// It runs on the co-op poll (coop_spawn.c, "T2" in the plan), which also runs in menus and loads, and makes no guest
// calls. Everything here is behind one lock; callers on other threads only copy in or out.
//
// Packets: a 24-byte header (CtrHdr) and a payload. Each player is named by its incarnation, a random 64-bit value drawn
// whenever this game joins a session (and, later, at each save load); the relay id is only the route, since the relay
// hands a freed id to the next player who joins. A packet from a known relay id with another incarnation is a new
// player: everything kept for the old one is dropped.
//
// Reliable messages travel in streams, one per (sender, receiver, channel), with two channels: control and bulk, so
// control never waits behind bulk. A stream numbers its messages; the receiver keeps every message it acknowledges
// until it has handed it on in order, and acknowledges nothing it could not keep, so the sender sends it again.
// Unreliable messages (streamed state, HB, census) go to "latest per (sender, type, key)" slots instead of a queue;
// their key is the payload's first 8 bytes.

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_ctr.h"
#include "coop_net.h"
#include "host.h"
#include "platform/platform.h"

int64_t frame_step_world_us(void);

typedef struct {
    char magic[4];       // "CTR1"
    uint8_t type, flags;   // flags: COOP_CTR_RELIABLE, COOP_CTR_SPEC, COOP_CTR_BULK
    uint16_t pad;
    uint64_t inc;        // the sender's incarnation
    uint16_t seq, ack;   // a reliable message's number in its stream; (ACK messages carry theirs in the payload)
    uint32_t ack_bits;
} CtrHdr;
_Static_assert(sizeof(CtrHdr) == 24, "CTR header size");

typedef struct {   // an ACK's payload: the stream's next expected number, which later ones arrived, the delay
    uint8_t chan, pad;
    uint16_t next;
    uint32_t bits;       // bit i: message next + 1 + i is kept
    uint32_t delay_us;   // from the newest arrival to this ACK
} CtrAck;

#define WIN 64        // messages in flight per stream (and kept out of order per stream)
#define PENDING 128   // messages waiting for room in the window, per stream
#define INBOX 256     // reliable messages handed on in order, waiting for the game side
#define SLOTS 1024    // latest-only slots (the entity sync keeps one pose per island of each player: coop_ent_own.c)
#define PAYLOAD (COOP_CTR_MAX - (int)sizeof(CtrHdr))

typedef struct { uint16_t seq; uint8_t type, flags; int len, tries; uint64_t sent_ns; uint8_t d[PAYLOAD]; } OutMsg;
typedef struct { uint8_t type, flags; int len; uint8_t d[PAYLOAD]; } PendMsg;
typedef struct {
    uint16_t next;                 // the number the next new message gets
    int n;                         // in flight (win holds them by seq % WIN)
    uint8_t used[WIN];
    OutMsg win[WIN];
    PendMsg pend[PENDING];
    int pend_head, pend_n;
} OutStream;
typedef struct {
    uint16_t next;                 // the next number to hand on
    uint8_t have[WIN];             // kept, by seq % WIN (seq in [next, next + WIN))
    struct { uint8_t type, flags; int len; uint8_t d[PAYLOAD]; } buf[WIN];
    int ack_owed;
    uint64_t last_arrival_ns;
} InStream;
typedef struct {
    uint64_t inc;                  // 0: not heard from yet
    uint32_t presence;             // coop_net_presence when last looked
    OutStream out[2];
    InStream in[2];
    double srtt, rttvar;           // seconds; srtt 0: no sample yet
    uint64_t hello_ns;             // HELLO_C sent to it (0 not yet)
    uint32_t resent, refused;      // counters for the log
} Peer;

static PlatMutex lock = PLAT_MUTEX_INIT;
static Peer peers[COOP_MAX_PLAYERS + 1];   // by relay id
static uint64_t my_inc;
static uint32_t seen_session;
static struct { uint64_t inc; int id; uint8_t type, flags; int len; uint8_t d[PAYLOAD]; } inbox[INBOX];
static int inbox_head, inbox_n;
static struct { int id; uint8_t type; uint64_t key; uint64_t at_ns; int len; uint8_t d[PAYLOAD]; } slots[SLOTS];
static struct { uint64_t inc; int id, joined; } events[64];
static int ev_head, ev_n;
static uint32_t stat_bad, stat_resent, stat_refused, stat_sent, stat_got;
static CoopCtrHb hb_mine;          // what this game's HB says beyond the clock (coop_ctr_hb_set)

static uint64_t mix64(uint64_t x) {   // splitmix64's finaliser
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
static uint64_t draw_incarnation(void) {
    static uint64_t salt;
    uint64_t v = 0;
    while (!v) v = mix64(plat_unix_time_ns() ^ mix64(plat_now_ns() ^ ((uint64_t)plat_process_id() << 32) ^ ++salt));
    return v;
}

static void event(uint64_t inc, int id, int joined) {   // (lock held)
    if (ev_n == 64) ev_head = (ev_head + 1) % 64, ev_n--;   // (the oldest goes: a reader that never reads loses news)
    int at = (ev_head + ev_n++) % 64;
    events[at].inc = inc, events[at].id = id, events[at].joined = joined;
}

static void peer_reset(int id) {   // (lock held) everything kept for that relay id goes
    uint32_t presence = peers[id].presence;
    memset(&peers[id], 0, sizeof peers[id]);
    peers[id].presence = presence;
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].id == id) slots[i].id = 0;
}


static void raw_send(int to, const CtrHdr *h, const void *d, int len) {   // (lock held)
    uint8_t b[COOP_CTR_MAX];
    memcpy(b, h, sizeof *h);
    if (len) memcpy(b + sizeof *h, d, len);
    if (coop_net_ctr_send(to, b, (int)sizeof *h + len)) stat_sent++;
}

static void header(CtrHdr *h, int type, int flags, uint16_t seq) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "CTR1", 4), h->type = (uint8_t)type, h->flags = (uint8_t)flags, h->inc = my_inc, h->seq = seq;
}

static double rto_s(const Peer *p) {   // RTO = 1.5 SRTT + 4 RTTVAR, clamped to 80 ms .. 1 s (0.3 s before a sample)
    double r = p->srtt > 0 ? 1.5 * p->srtt + 4 * p->rttvar : 0.3;
    return r < 0.08 ? 0.08 : r > 1.0 ? 1.0 : r;
}

static void fill_window(int id, int ch) {   // (lock held) pending messages into the window, sent at once
    OutStream *o = &peers[id].out[ch];
    // Only while the slot of the next number is free: with message s lost and the ones after it acknowledged by the
    // ACK's bits, the count alone let number s + WIN go out into s's slot, s was never sent again, and the receiver,
    // still waiting for s, refused everything from then on (a two-game run through a relay losing 8 % of the packets,
    // 2026-11-16: one stream dead after a minute, "refused" rising by 30 a second).
    while (o->pend_n && o->n < WIN && !o->used[o->next % WIN]) {
        PendMsg *pm = &o->pend[o->pend_head];
        uint16_t seq = o->next++;
        OutMsg *m = &o->win[seq % WIN];
        o->used[seq % WIN] = 1, o->n++;
        m->seq = seq, m->type = pm->type, m->flags = pm->flags, m->len = pm->len, m->tries = 1;
        memcpy(m->d, pm->d, pm->len);
        o->pend_head = (o->pend_head + 1) % PENDING, o->pend_n--;
        CtrHdr h;
        header(&h, m->type, m->flags, seq);
        m->sent_ns = plat_now_ns();
        raw_send(id, &h, m->d, m->len);
    }
}

static int queue_reliable(int id, int type, int flags, const void *d, int len) {   // (lock held)
    int ch = flags & COOP_CTR_BULK ? 1 : 0;
    OutStream *o = &peers[id].out[ch];
    if (o->pend_n == PENDING) return 0;
    PendMsg *pm = &o->pend[(o->pend_head + o->pend_n++) % PENDING];
    pm->type = (uint8_t)type, pm->flags = (uint8_t)(flags | COOP_CTR_RELIABLE), pm->len = len;
    memcpy(pm->d, d, len);
    fill_window(id, ch);
    return 1;
}

int coop_ctr_send(int to, int type, int flags, const void *d, int len) {
    if (len < 0 || len > PAYLOAD || to < 0 || to > COOP_MAX_PLAYERS || type <= 0 || type > 255) return 0;
    int ok = 0;
    plat_mutex_lock(&lock);
    if (my_inc && coop_net_id()) {
        for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
            if ((to && id != to) || id == coop_net_id() || !coop_net_live(id)) continue;
            if (flags & COOP_CTR_RELIABLE) ok |= queue_reliable(id, type, flags, d, len);
            else {
                CtrHdr h;
                header(&h, type, flags, 0);
                raw_send(id, &h, d, len), ok = 1;
            }
        }
    }
    plat_mutex_unlock(&lock);
    return ok;
}

int coop_ctr_route(uint64_t inc) {
    int id = 0;
    plat_mutex_lock(&lock);
    for (int i = 1; i <= COOP_MAX_PLAYERS && !id; i++)
        if (inc && peers[i].inc == inc) id = i;
    plat_mutex_unlock(&lock);
    return id;
}

uint64_t coop_ctr_incarnation(int id) {
    if (id == coop_net_id()) return my_inc;
    if (id < 1 || id > COOP_MAX_PLAYERS) return 0;
    plat_mutex_lock(&lock);
    uint64_t inc = peers[id].inc;
    plat_mutex_unlock(&lock);
    return inc;
}

uint64_t coop_ctr_me(void) { return my_inc; }

int coop_ctr_recv(CoopCtrMsg *m) {
    plat_mutex_lock(&lock);
    int got = inbox_n > 0;
    if (got) {
        int at = inbox_head;
        m->from = inbox[at].inc, m->from_id = inbox[at].id, m->type = inbox[at].type, m->flags = inbox[at].flags;
        m->len = inbox[at].len, memcpy(m->d, inbox[at].d, inbox[at].len);
        inbox_head = (inbox_head + 1) % INBOX, inbox_n--;
    }
    plat_mutex_unlock(&lock);
    return got;
}

int coop_ctr_recv_type(int type, CoopCtrMsg *m) {   // the oldest reliable message of that type (the others stay in order)
    plat_mutex_lock(&lock);
    int got = 0;
    for (int i = 0; i < inbox_n && !got; i++) {
        int at = (inbox_head + i) % INBOX;
        if (inbox[at].type != type) continue;
        m->from = inbox[at].inc, m->from_id = inbox[at].id, m->type = inbox[at].type, m->flags = inbox[at].flags;
        m->len = inbox[at].len, memcpy(m->d, inbox[at].d, inbox[at].len);
        for (int j = i; j + 1 < inbox_n; j++) inbox[(inbox_head + j) % INBOX] = inbox[(inbox_head + j + 1) % INBOX];
        inbox_n--, got = 1;
    }
    plat_mutex_unlock(&lock);
    return got;
}

// The latest-only slots of one type, one after another: *it starts at 0; 0 when there are no more.
int coop_ctr_slot_next(int type, int *it, CoopCtrMsg *m, uint64_t *age_ns) {
    int got = 0;
    plat_mutex_lock(&lock);
    for (int i = *it; i < SLOTS && !got; i++) {
        *it = i + 1;
        if (!slots[i].id || slots[i].type != type || slots[i].len > COOP_CTR_PAYLOAD) continue;
        m->from_id = slots[i].id, m->type = (uint8_t)type, m->flags = 0, m->len = slots[i].len;
        memcpy(m->d, slots[i].d, (size_t)slots[i].len);
        if (age_ns) *age_ns = plat_now_ns() - slots[i].at_ns;
        got = 1;
    }
    if (!got) *it = SLOTS;
    plat_mutex_unlock(&lock);
    if (got) m->from = coop_ctr_incarnation(m->from_id);
    return got;
}
int coop_ctr_latest(int from_id, int type, uint64_t key, void *out, int max, uint64_t *age_ns) {
    int n = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].id == from_id && slots[i].type == type && slots[i].key == key && slots[i].len <= max) {
            n = slots[i].len, memcpy(out, slots[i].d, n);
            if (age_ns) *age_ns = plat_now_ns() - slots[i].at_ns;
            break;
        }
    plat_mutex_unlock(&lock);
    return n;
}

int coop_ctr_peer_event(uint64_t *inc, int *id, int *joined) {
    plat_mutex_lock(&lock);
    int got = ev_n > 0;
    if (got) *inc = events[ev_head].inc, *id = events[ev_head].id, *joined = events[ev_head].joined, ev_head = (ev_head + 1) % 64, ev_n--;
    plat_mutex_unlock(&lock);
    return got;
}

void coop_ctr_hb_set(const CoopCtrHb *hb) {
    plat_mutex_lock(&lock);
    hb_mine = *hb;
    plat_mutex_unlock(&lock);
}

// A peer's smoothed round trip and its variance, seconds (0: no sample yet): what a claim's hold is taken from.
int coop_ctr_rtt(int id, double *srtt, double *rttvar) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return 0;
    plat_mutex_lock(&lock);
    double s = peers[id].srtt, v = peers[id].rttvar;
    plat_mutex_unlock(&lock);
    if (s <= 0) return 0;
    *srtt = s, *rttvar = v;
    return 1;
}
int coop_ctr_hb(int from_id, CoopCtrHb *out, uint64_t *age_ns) {
    uint8_t b[PAYLOAD];
    int n = coop_ctr_latest(from_id, COOP_CTR_HB, 0, b, sizeof b, age_ns);
    if (n < (int)sizeof *out) return 0;
    memcpy(out, b, sizeof *out);
    return 1;
}

// ---- receiving

static void take_ack(int id, const CtrAck *a) {   // (lock held)
    if (a->chan > 1) return;
    Peer *p = &peers[id];
    OutStream *o = &p->out[a->chan];
    uint64_t now = plat_now_ns();
    for (int k = 0; k < WIN; k++) {
        if (!o->used[k]) continue;
        OutMsg *m = &o->win[k];
        int16_t d = (int16_t)(m->seq - a->next);
        int acked = d < 0 || (d >= 1 && d <= 32 && (a->bits >> (d - 1) & 1));
        if (!acked) continue;
        if (m->tries == 1 && now > m->sent_ns) {   // (Karn: a sample only from a message sent once)
            double s = (now - m->sent_ns) / 1e9 - a->delay_us / 1e6;
            if (s < 0) s = 0;
            if (p->srtt <= 0) p->srtt = s, p->rttvar = s / 2;
            else {
                double e = s - p->srtt;
                p->rttvar += 0.25 * ((e < 0 ? -e : e) - p->rttvar);
                p->srtt += 0.125 * e;
            }
        }
        o->used[k] = 0, o->n--;
    }
    fill_window(id, a->chan);
}

static void take_reliable(int id, const CtrHdr *h, const uint8_t *d, int len) {   // (lock held)
    int ch = h->flags & COOP_CTR_BULK ? 1 : 0;
    InStream *in = &peers[id].in[ch];
    int16_t ahead = (int16_t)(h->seq - in->next);
    in->last_arrival_ns = plat_now_ns();
    if (ahead < 0) { in->ack_owed = 1; return; }   // (handed on already: its ACK was lost)
    if (ahead >= WIN) { peers[id].refused++, stat_refused++; return; }   // (no room to keep it: no ACK, it comes again)
    int k = h->seq % WIN;
    if (!in->have[k]) {
        in->have[k] = 1, in->buf[k].type = h->type, in->buf[k].flags = h->flags, in->buf[k].len = len;
        memcpy(in->buf[k].d, d, len);
    }
    in->ack_owed = 1;
    while (in->have[in->next % WIN] && inbox_n < INBOX) {   // in order into the inbox, while it has room
        k = in->next % WIN;
        int at = (inbox_head + inbox_n++) % INBOX;
        inbox[at].inc = peers[id].inc, inbox[at].id = id, inbox[at].type = in->buf[k].type, inbox[at].flags = in->buf[k].flags;
        inbox[at].len = in->buf[k].len, memcpy(inbox[at].d, in->buf[k].d, in->buf[k].len);
        in->have[k] = 0, in->next++;
    }
}

static void take_unreliable(int id, const CtrHdr *h, const uint8_t *d, int len) {   // (lock held)
    uint64_t key = 0;
    if (len >= 8) memcpy(&key, d, 8);
    int at = -1, oldest = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].id == id && slots[i].type == h->type && slots[i].key == key) { at = i; break; }
        if (!slots[i].id && at < 0) at = -2 - i;   // (a free one, if no match)
        if (slots[i].at_ns < slots[oldest].at_ns) oldest = i;
    }
    if (at < -1) at = -2 - at;
    else if (at == -1) at = oldest;
    slots[at].id = id, slots[at].type = h->type, slots[at].key = key, slots[at].at_ns = plat_now_ns(), slots[at].len = len;
    memcpy(slots[at].d, d, len);
}

static void receive(void) {   // (lock held)
    uint8_t b[COOP_CTR_MAX];
    int from, n;
    while ((n = coop_net_ctr_take(b, sizeof b, &from)) > 0) {
        CtrHdr h;
        if (n < (int)sizeof h || from < 1 || from > COOP_MAX_PLAYERS) { stat_bad++; continue; }
        memcpy(&h, b, sizeof h);
        if (memcmp(h.magic, "CTR1", 4) || !h.inc) { stat_bad++; continue; }
        stat_got++;
        if (h.inc != peers[from].inc) {   // a player not seen on this id yet, or a new one on a freed id
            if (peers[from].inc) {
                rt_log("coop: ctr: player %d is a new incarnation %016llx (was %016llx)", from, (unsigned long long)h.inc,
                       (unsigned long long)peers[from].inc);
                event(peers[from].inc, from, 0);
            } else
                rt_log("coop: ctr: player %d is incarnation %016llx", from, (unsigned long long)h.inc);
            // A new incarnation's streams start again. The first one heard of from a relay id keeps what was queued
            // to it already: hello() may have sent its HELLO as message 0 before any packet came back, and a reset sent
            // the next message as 0 again, which the peer then took as a repeat and dropped (review 1 finding 16).
            if (peers[from].inc) peer_reset(from);
            peers[from].inc = h.inc;
            event(h.inc, from, 1);
        }
        const uint8_t *d = b + sizeof h;
        int len = n - (int)sizeof h;
        if (h.type == COOP_CTR_ACK) {
            if (len >= (int)sizeof(CtrAck)) {
                CtrAck a;
                memcpy(&a, d, sizeof a);
                take_ack(from, &a);
            }
        } else if (h.flags & COOP_CTR_RELIABLE)
            take_reliable(from, &h, d, len);
        else
            take_unreliable(from, &h, d, len);
    }
}

static void send_acks(void) {   // (lock held) one ACK per stream that is owed one
    uint64_t now = plat_now_ns();
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++)
        for (int ch = 0; ch < 2; ch++) {
            InStream *in = &peers[id].in[ch];
            if (!in->ack_owed || !peers[id].inc) continue;
            CtrAck a = {(uint8_t)ch, 0, in->next, 0, (uint32_t)((now - in->last_arrival_ns) / 1000)};
            for (int i = 0; i < 32 && i + 1 < WIN; i++)
                if (in->have[(uint16_t)(in->next + 1 + i) % WIN]) a.bits |= 1u << i;
            CtrHdr h;
            header(&h, COOP_CTR_ACK, 0, 0);
            raw_send(id, &h, &a, sizeof a);
            in->ack_owed = 0;
        }
}

static void resend(void) {   // (lock held) messages unacknowledged past their timeout, again
    uint64_t now = plat_now_ns();
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        Peer *p = &peers[id];
        double rto = rto_s(p);
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < WIN; k++) {
                OutMsg *m = &p->out[ch].win[k];
                if (!p->out[ch].used[k]) continue;
                double wait = rto * (double)(1 << (m->tries - 1 < 3 ? m->tries - 1 : 3));
                if (wait > 1.0) wait = 1.0;
                if ((now - m->sent_ns) / 1e9 < wait) continue;
                CtrHdr h;
                header(&h, m->type, m->flags, m->seq);
                m->sent_ns = now, m->tries++;
                raw_send(id, &h, m->d, m->len);
                p->resent++, stat_resent++;
            }
    }
}

// ---- the heartbeat: 30 a second, from this thread, so it goes on in menus and loads (plan 3.2, 5.8)

static void heartbeat(void) {   // (lock held)
    static uint64_t last_ns, clock_moved_ns;
    static int64_t last_world;
    uint64_t now = plat_now_ns();
    int64_t world = frame_step_world_us();
    if (world != last_world) last_world = world, clock_moved_ns = now;
    if (now - last_ns < 33000000ull) return;
    last_ns = now;
    CoopCtrHb hb = hb_mine;
    hb.key = 0, hb.world_us = world, hb.wall_ns = plat_unix_time_ns();
    if (now - clock_moved_ns > 300000000ull) hb.flags |= COOP_CTR_HB_PAUSED;   // (the world clock stood 0.3 s)
    { uint32_t coop_sess_caps(void); hb.flags |= coop_sess_caps(); }   // which syncs this game runs (coop_sess.c)
    CtrHdr h;
    header(&h, COOP_CTR_HB, 0, 0);
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++)
        if (id != coop_net_id() && coop_net_live(id)) raw_send(id, &h, &hb, sizeof hb);
}

// ---- the tick

static void session_check(void) {   // (lock held) a new session (or none): a new incarnation, everything reset
    uint32_t s = coop_net_session();
    if (s == seen_session) return;
    seen_session = s;
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (peers[id].inc) event(peers[id].inc, id, 0);
        memset(&peers[id], 0, sizeof peers[id]);
    }
    memset(slots, 0, sizeof slots);
    inbox_n = 0;
    my_inc = coop_net_id() ? draw_incarnation() : 0;
    if (my_inc) rt_log("coop: ctr: this game is incarnation %016llx as player %d", (unsigned long long)my_inc, coop_net_id());
}

static void presence_check(void) {   // (lock held) a player gone (LEAVE, the relay's timeout): its streams go
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        uint32_t g = coop_net_presence(id);
        if (g == peers[id].presence) continue;
        peers[id].presence = g;
        if (!coop_net_live(id) && peers[id].inc) {
            rt_log("coop: ctr: player %d (%016llx) left", id, (unsigned long long)peers[id].inc);
            event(peers[id].inc, id, 0);
            peer_reset(id);
        }
    }
}

static void hello(void) {   // (lock held) HELLO_C to each player here, once: both learn each other's incarnation
    uint64_t now = plat_now_ns();
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (id == coop_net_id() || !coop_net_live(id) || peers[id].hello_ns) continue;
        peers[id].hello_ns = now;
        uint64_t me = my_inc;
        queue_reliable(id, COOP_CTR_HELLO, 0, &me, sizeof me);
    }
}

void coop_ctr_tick(void) {
    // (the session's end is seen first: after Leave, with networking off, the old players, their slots and inbox stayed
    // until the next join, and the entity sync took them as still here; code review, 2026-11-17)
    int on = coop_net_on();
    plat_mutex_lock(&lock);
    session_check();
    if (!on) { plat_mutex_unlock(&lock); return; }
    if (my_inc) {
        presence_check();
        receive();
        hello();
        resend();
        send_acks();
        heartbeat();
    }
    plat_mutex_unlock(&lock);
    static uint64_t last_log;
    uint64_t now = plat_now_ns();
    if (my_inc && now - last_log > 5000000000ull) {   // drops and resends, when there were any
        last_log = now;
        static uint32_t od0, id0, rs0, rf0, bad0;
        uint32_t od, idr;
        coop_net_ctr_drops(&od, &idr);
        if (od != od0 || idr != id0 || stat_resent != rs0 || stat_refused != rf0 || stat_bad != bad0) {
            rt_log("coop: ctr: net drops out %u in %u, resent %u, refused %u, bad %u; %u packets sent, %u taken (all since the start)",
                   od, idr, stat_resent, stat_refused, stat_bad, stat_sent, stat_got);
            od0 = od, id0 = idr, rs0 = stat_resent, rf0 = stat_refused, bad0 = stat_bad;
        }
    }
}
