// Co-op networking: one UDP socket to the relay (server/coop_relay.py), on a thread of its own so no game thread ever
// waits on the network. TOTK_COOP_RELAY=<host>[:<port>] turns it on (port 41900 by default), TOTK_COOP_NAME names
// this player. The game side (coop_spawn.c) publishes this player's state with coop_net_publish and reads the other
// players' latest states with coop_net_remote; both only copy under a lock.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
#define SOCK_BAD INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define closesocket close
#endif

#include "cfg.h"
#include "coop_net.h"
#include "host.h"
#include "platform/platform.h"

enum { HELLO = 1, WELCOME = 2, STATE = 3, LEAVE = 4, BYE = 5, WORLD = 6, HIT = 7, POSE = 8, OBJS = 9, EPOSE = 10, VERSION = COOP_NET_VERSION, HEADER = 8 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static NetState mine;                 // lock
static int mine_set;                  // lock
static struct { NetState st; uint64_t at_ns; int live; char name[17]; } others[COOP_MAX_PLAYERS + 1];   // lock; by id
static volatile int my_id;            // 0 until WELCOME
static int started;                   // the settings were looked at (TOTK_COOP_RELAY joins at start)
static volatile int net_gen;          // a new value stops the running network thread
static volatile int net_active;       // a network thread runs (joined or joining)
static char target[256], my_name[17];   // the relay host[:port], this player's name; set before a thread starts
static char last_error[160];           // why the last join failed ("" none)
// The shared world: the latest WORLD packet out (the enemies this game runs) and in from each player, and HIT packets
// both ways (a game's damage to another's enemies), queued.
#define WORLD_MAX 1800
static uint8_t world_out[WORLD_MAX];   // lock
static int world_out_len, world_out_new;   // lock
static struct { uint8_t d[WORLD_MAX]; int len; uint64_t at_ns; } world_in[COOP_MAX_PLAYERS + 1];   // lock; by id
// Enemy poses (EPOSE packets, coop_world.c): queued both ways.
#define EPQ_OUT 24
#define EPQ_IN 96
static struct { uint8_t d[COOP_EPOSE_MAX]; int len, from; uint64_t at_ns; } epose_out[EPQ_OUT], epose_in[EPQ_IN];   // lock
static int epose_out_n, epose_in_n;   // lock
// The skeleton pose (coop_world.c packs it): this player's latest out, each other player's latest in.
static uint8_t pose_out[COOP_POSE_MAX];   // lock
static int pose_out_len, pose_out_new;   // lock
static struct { uint8_t d[COOP_POSE_MAX]; int len; uint64_t at_ns; } pose_in[COOP_MAX_PLAYERS + 1];   // lock; by id
#define OBJQ 64   // world-object packets, queued both ways
static struct { uint8_t d[COOP_OBJS_PACKET_MAX]; int len, from; } objs_out[OBJQ], objs_in[OBJQ];   // lock
static int objs_out_n, objs_in_n;   // lock
#define HITQ 64
static struct { uint8_t d[COOP_HIT_MAX]; int len, from; } hit_out[HITQ], hit_in[HITQ];   // lock
static int hit_out_n, hit_in_n;   // lock

static void header(uint8_t *p, int kind) { memcpy(p, "TKCO", 4), p[4] = VERSION, p[5] = (uint8_t)kind, p[6] = (uint8_t)my_id, p[7] = 0; }

// The same address and port (IPv4 or IPv6).
static int same_addr(const struct sockaddr_storage *a, socklen_t alen, const struct sockaddr *b, int blen) {
    if (a->ss_family != b->sa_family) return 0;
    if (a->ss_family == AF_INET && alen >= (socklen_t)sizeof(struct sockaddr_in) && blen >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6 && alen >= (socklen_t)sizeof(struct sockaddr_in6) && blen >= (int)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
        return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr);
    }
    return 0;
}

static void net_thread(void *arg) {
    int gen = (int)(intptr_t)arg;
    const char *name = my_name;
    char host[256];
    snprintf(host, sizeof host, "%s", target);
    const char *port = "41900";
    char *colon = strrchr(host, ':');
    if (colon) *colon = 0, port = colon + 1;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) {
        rt_log("coop net: cannot resolve the relay %s:%s; co-op networking off", host, port);
        snprintf(last_error, sizeof last_error, "cannot find %s", host);
        if (net_gen == gen) net_active = 0;
        return;
    }
    Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) {
        rt_log("coop net: no socket; co-op networking off");
        snprintf(last_error, sizeof last_error, "no network socket");
        if (net_gen == gen) net_active = 0;
        freeaddrinfo(res);
        return;
    }
#ifdef _WIN32
    // The state goes out every frame (16 ms, as TotK Online sends its stand-in's pose: read for behaviour only), so the
    // receive waits at most 4 ms. Every 33 ms with 10 ms waits it went about 24 times a second with gaps to 80 ms,
    // and the stand-ins moved and animated in steps.
    DWORD tmo = 4;
#else
    struct timeval tmo = {0, 4000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    int rcvbuf = 1 << 20;   // enemy poses come in bursts of up to a dozen packets a frame (coop_world.c)
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    rt_log("coop net: relay %s:%s, name \"%s\"", host, port, name ? name : "Player");
    uint64_t last_hello = 0, last_state = 0, seq = 0, beat = 0, started_ns = plat_now_ns(), heard_ns = 0;
    int joined = 0;   // a WELCOME came: say bye on leaving
    uint8_t buf[8192];
    while (net_gen == gen) {
        uint64_t now = plat_now_ns();
        if (!my_id && now - last_hello > 1000000000ull) {
            last_hello = now;
            header(buf, HELLO);
            memset(buf + HEADER, 0, 16);
            snprintf((char *)buf + HEADER, 16, "%s", name ? name : "Player");
            sendto(s, (const char *)buf, HEADER + 16, 0, res->ai_addr, (int)res->ai_addrlen);
        }
        if (my_id && now - last_state >= 16000000ull) {
            last_state = now;
            NetState st;
            plat_mutex_lock(&lock);
            int have = mine_set;
            st = mine;
            plat_mutex_unlock(&lock);
            if (have) {
                st.seq = (uint32_t)++seq;
                header(buf, STATE);
                memcpy(buf + HEADER, &st, sizeof st);
                sendto(s, (const char *)buf, HEADER + (int)sizeof st, 0, res->ai_addr, (int)res->ai_addrlen);
            }
            // Once a second (every 60th pass), state or none: keep the relay's seat. (Counted by the states sent, it
            // went once and never again while there was no state, and the relay let the seat go, code review.)
            if (++beat % 60 == 0) {
                header(buf, HELLO);
                memset(buf + HEADER, 0, 16);
                snprintf((char *)buf + HEADER, 16, "%s", name ? name : "Player");
                sendto(s, (const char *)buf, HEADER + 16, 0, res->ai_addr, (int)res->ai_addrlen);
            }
        }
        if (my_id) {   // the world packet (host, about 10 a second) and queued hits, as they come
            uint8_t w[HEADER + WORLD_MAX];
            int wl = 0, nh = 0;
            static struct { uint8_t d[COOP_HIT_MAX]; int len; } hits[HITQ];
            plat_mutex_lock(&lock);
            if (world_out_new) memcpy(w + HEADER, world_out, world_out_len), wl = world_out_len, world_out_new = 0;
            for (int i = 0; i < hit_out_n; i++) memcpy(hits[i].d, hit_out[i].d, hit_out[i].len), hits[i].len = hit_out[i].len;
            nh = hit_out_n, hit_out_n = 0;
            plat_mutex_unlock(&lock);
            if (wl) header(w, WORLD), sendto(s, (const char *)w, HEADER + wl, 0, res->ai_addr, (int)res->ai_addrlen);
            uint8_t pp[HEADER + COOP_POSE_MAX];
            int pl = 0;
            plat_mutex_lock(&lock);
            if (pose_out_new) memcpy(pp + HEADER, pose_out, pose_out_len), pl = pose_out_len, pose_out_new = 0;
            plat_mutex_unlock(&lock);
            if (pl) header(pp, POSE), sendto(s, (const char *)pp, HEADER + pl, 0, res->ai_addr, (int)res->ai_addrlen);
            {   // enemy poses: every queued packet
                static uint8_t ep[HEADER + COOP_EPOSE_MAX];
                for (;;) {
                    int el = 0;
                    plat_mutex_lock(&lock);
                    if (epose_out_n) {
                        memcpy(ep + HEADER, epose_out[0].d, epose_out[0].len), el = epose_out[0].len;
                        memmove(epose_out, epose_out + 1, (size_t)(epose_out_n - 1) * sizeof epose_out[0]), epose_out_n--;
                    }
                    plat_mutex_unlock(&lock);
                    if (!el) break;
                    header(ep, EPOSE), sendto(s, (const char *)ep, HEADER + el, 0, res->ai_addr, (int)res->ai_addrlen);
                }
            }
            {   // world objects: every queued packet
                static uint8_t ob[HEADER + COOP_OBJS_PACKET_MAX];
                for (;;) {
                    int ol = 0;
                    plat_mutex_lock(&lock);
                    if (objs_out_n) {
                        memcpy(ob + HEADER, objs_out[0].d, objs_out[0].len), ol = objs_out[0].len;
                        memmove(objs_out, objs_out + 1, (size_t)(objs_out_n - 1) * sizeof objs_out[0]), objs_out_n--;
                    }
                    plat_mutex_unlock(&lock);
                    if (!ol) break;
                    header(ob, OBJS), sendto(s, (const char *)ob, HEADER + ol, 0, res->ai_addr, (int)res->ai_addrlen);
                }
            }
            for (int i = 0; i < nh; i++) {
                header(w, HIT);
                memcpy(w + HEADER, hits[i].d, hits[i].len);
                sendto(s, (const char *)w, HEADER + hits[i].len, 0, res->ai_addr, (int)res->ai_addrlen);
            }
        }
        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        int n = (int)recvfrom(s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &from_len);
        // Only the relay's: any datagram that read as ours was taken, from anyone who could reach this port, and
        // could set this player's id or feed states, objects and hits (code review, 2026-11-06).
        if (n >= 0 && !same_addr(&from, from_len, res->ai_addr, (int)res->ai_addrlen)) continue;
        // The relay went quiet (the host left, or the network dropped) or never answered: end here and say why.
        uint64_t t_now = plat_now_ns();
        if ((joined && t_now - heard_ns > 6000000000ull) || (!joined && t_now - started_ns > 15000000000ull)) {
            if (net_gen != gen) break;   // (left already: the new session's error stays)
            snprintf(last_error, sizeof last_error, joined ? "the session ended (the host left or cannot be reached)"
                                                           : "no answer from %s", host);
            rt_log("coop net: %s", last_error);
            if (net_gen == gen) {
                net_active = 0, my_id = 0;
                plat_mutex_lock(&lock);
                for (int i = 0; i <= COOP_MAX_PLAYERS; i++) others[i].live = 0;
                plat_mutex_unlock(&lock);
            }
            joined = 0;
            break;
        }
        if (net_gen != gen) break;
        if (n < HEADER || memcmp(buf, "TKCO", 4) || buf[4] != VERSION) continue;
        heard_ns = plat_now_ns();
        int kind = buf[5], id = buf[6];
        if (kind == WELCOME) {
            if (id < 1 || id > COOP_MAX_PLAYERS) continue;
            if (net_gen != gen) break;   // (a left session's thread does not set the new one's id)
            if (!my_id) rt_log("coop net: joined as player %d", id);
            joined = 1;
            my_id = id;
            for (int off = HEADER; off + 17 <= n; off += 17) {
                int other = buf[off];
                if (other < 1 || other > COOP_MAX_PLAYERS) continue;
                plat_mutex_lock(&lock);
                memcpy(others[other].name, buf + off + 1, 16);
                plat_mutex_unlock(&lock);
            }
        } else if (kind == STATE && id >= 1 && id <= COOP_MAX_PLAYERS && n >= HEADER + (int)sizeof(NetState)) {
            plat_mutex_lock(&lock);
            if (!others[id].live) rt_log("coop net: player %d is here", id);
            memcpy(&others[id].st, buf + HEADER, sizeof(NetState));
            others[id].at_ns = plat_now_ns();
            others[id].live = 1;
            plat_mutex_unlock(&lock);
        } else if (kind == WORLD && id >= 1 && id <= COOP_MAX_PLAYERS && n - HEADER <= WORLD_MAX) {
            plat_mutex_lock(&lock);
            memcpy(world_in[id].d, buf + HEADER, n - HEADER), world_in[id].len = n - HEADER, world_in[id].at_ns = plat_now_ns();
            plat_mutex_unlock(&lock);
        } else if (kind == EPOSE && id >= 1 && id <= COOP_MAX_PLAYERS && n - HEADER <= COOP_EPOSE_MAX) {
            plat_mutex_lock(&lock);
            if (epose_in_n == EPQ_IN)   // full (the game did not take them): the oldest goes
                memmove(epose_in, epose_in + 1, (size_t)(EPQ_IN - 1) * sizeof epose_in[0]), epose_in_n--;
            memcpy(epose_in[epose_in_n].d, buf + HEADER, n - HEADER), epose_in[epose_in_n].len = n - HEADER;
            epose_in[epose_in_n].from = id, epose_in[epose_in_n].at_ns = plat_now_ns(), epose_in_n++;
            plat_mutex_unlock(&lock);
        } else if (kind == POSE && id >= 1 && id <= COOP_MAX_PLAYERS && n - HEADER <= COOP_POSE_MAX) {
            plat_mutex_lock(&lock);
            memcpy(pose_in[id].d, buf + HEADER, n - HEADER), pose_in[id].len = n - HEADER, pose_in[id].at_ns = plat_now_ns();
            plat_mutex_unlock(&lock);
        } else if (kind == OBJS && id >= 1 && id <= COOP_MAX_PLAYERS && n - HEADER <= COOP_OBJS_PACKET_MAX) {
            plat_mutex_lock(&lock);
            if (objs_in_n < OBJQ) memcpy(objs_in[objs_in_n].d, buf + HEADER, n - HEADER), objs_in[objs_in_n].len = n - HEADER,
                                  objs_in[objs_in_n].from = id, objs_in_n++;
            plat_mutex_unlock(&lock);
        } else if (kind == HIT && n - HEADER <= COOP_HIT_MAX) {
            plat_mutex_lock(&lock);
            if (hit_in_n < HITQ) memcpy(hit_in[hit_in_n].d, buf + HEADER, n - HEADER), hit_in[hit_in_n].len = n - HEADER,
                                 hit_in[hit_in_n].from = id, hit_in_n++;
            plat_mutex_unlock(&lock);
        } else if (kind == LEAVE && id >= 1 && id <= COOP_MAX_PLAYERS) {
            plat_mutex_lock(&lock);
            if (others[id].live) rt_log("coop net: player %d left", id);
            others[id].live = 0;
            plat_mutex_unlock(&lock);
        }
    }
    // Left (coop_net_leave, or a new join): say bye so the others drop this player at once.
    if (joined) {
        header(buf, BYE);
        sendto(s, (const char *)buf, HEADER, 0, res->ai_addr, (int)res->ai_addrlen);
    }
    closesocket(s);
    freeaddrinfo(res);
    rt_log("coop net: left %s", host);
}

void coop_net_leave(void) {
    net_gen++;
    net_active = 0;
    my_id = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i <= COOP_MAX_PLAYERS; i++) others[i].live = 0;
    mine_set = 0, hit_out_n = hit_in_n = 0, objs_out_n = objs_in_n = 0, epose_out_n = epose_in_n = 0;
    // (and what was waiting to go or came for the last session: its world and pose went first in the next one,
    // code review 2026-11-06)
    world_out_new = 0, pose_out_new = 0;
    for (int i = 0; i <= COOP_MAX_PLAYERS; i++) pose_in[i].len = 0, pose_in[i].at_ns = 0, world_in[i].len = 0, others[i].name[0] = 0;
    plat_mutex_unlock(&lock);
}

void coop_net_join(const char *relay, const char *name) {
    coop_net_leave();
    started = 1;
    snprintf(target, sizeof target, "%s", relay);
    snprintf(my_name, sizeof my_name, "%s", name && *name ? name : "Player");
    last_error[0] = 0;
    net_active = 1;
    plat_thread_spawn(net_thread, (void *)(intptr_t)++net_gen, "coop-net", 0);
}

int coop_net_on(void) {
    if (!started) {   // TOTK_COOP_RELAY (the test scripts, play_two.ps1): join at start
        started = 1;
        const char *relay = cfg_get("TOTK_COOP_RELAY");
        if (relay && *relay) coop_net_join(relay, cfg_get("TOTK_COOP_NAME"));
    }
    return net_active;
}

const char *coop_net_target(void) { return net_active ? target : ""; }
const char *coop_net_error(void) { return last_error; }

// The other players here: their ids and names, up to max; returns how many.
int coop_net_players(int *ids, char (*names)[17], int max) {
    int n = 0;
    plat_mutex_lock(&lock);
    for (int i = 1; i <= COOP_MAX_PLAYERS && n < max; i++)
        if (others[i].live && i != my_id) ids[n] = i, memcpy(names[n], others[i].name, 17), names[n][16] = 0, n++;
    plat_mutex_unlock(&lock);
    return n;
}

int coop_net_id(void) { return my_id; }

void coop_net_publish(const NetState *st) {
    plat_mutex_lock(&lock);
    mine = *st;
    mine_set = 1;
    plat_mutex_unlock(&lock);
}

int coop_net_remote(int id, NetState *out, uint64_t *age_ns) {
    if (id < 1 || id > COOP_MAX_PLAYERS || id == my_id) return 0;
    plat_mutex_lock(&lock);
    int live = others[id].live;
    if (live) {
        *out = others[id].st;
        *age_ns = plat_now_ns() - others[id].at_ns;
    }
    plat_mutex_unlock(&lock);
    return live;
}

void coop_net_world_send(const void *data, int len) {
    if (len < 0 || len > WORLD_MAX) return;
    plat_mutex_lock(&lock);
    memcpy(world_out, data, len), world_out_len = len, world_out_new = 1;
    plat_mutex_unlock(&lock);
}

int coop_net_world(int id, void *out, int max, uint64_t *age_ns) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return 0;
    plat_mutex_lock(&lock);
    int n = world_in[id].len <= max ? world_in[id].len : 0;
    if (n) memcpy(out, world_in[id].d, n), *age_ns = plat_now_ns() - world_in[id].at_ns;
    plat_mutex_unlock(&lock);
    return n;
}

void coop_net_epose_send(const void *data, int len) {
    if (len <= 0 || len > COOP_EPOSE_MAX) return;
    plat_mutex_lock(&lock);
    if (epose_out_n < EPQ_OUT) memcpy(epose_out[epose_out_n].d, data, len), epose_out[epose_out_n].len = len, epose_out_n++;
    plat_mutex_unlock(&lock);
}

int coop_net_epose_take(void *out, int max, int *from, uint64_t *age_ns) {
    plat_mutex_lock(&lock);
    int n = 0;
    if (epose_in_n) {
        n = epose_in[0].len <= max ? epose_in[0].len : 0;
        if (n) memcpy(out, epose_in[0].d, n);
        *from = epose_in[0].from, *age_ns = plat_now_ns() - epose_in[0].at_ns;
        memmove(epose_in, epose_in + 1, (size_t)(epose_in_n - 1) * sizeof epose_in[0]), epose_in_n--;
        if (!n) n = -1;
    }
    plat_mutex_unlock(&lock);
    return n;
}

void coop_net_pose_send(const void *data, int len) {
    if (len <= 0 || len > COOP_POSE_MAX) return;
    plat_mutex_lock(&lock);
    memcpy(pose_out, data, len), pose_out_len = len, pose_out_new = 1;
    plat_mutex_unlock(&lock);
}

int coop_net_pose(int id, void *out, int max, uint64_t *age_ns) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return 0;
    plat_mutex_lock(&lock);
    int n = pose_in[id].len <= max ? pose_in[id].len : 0;
    if (n) memcpy(out, pose_in[id].d, n), *age_ns = plat_now_ns() - pose_in[id].at_ns;
    plat_mutex_unlock(&lock);
    return n;
}

void coop_net_objs_send(const void *data, int len) {
    if (len <= 0 || len > COOP_OBJS_PACKET_MAX) return;
    plat_mutex_lock(&lock);
    if (objs_out_n < OBJQ) memcpy(objs_out[objs_out_n].d, data, len), objs_out[objs_out_n].len = len, objs_out_n++;
    plat_mutex_unlock(&lock);
}

int coop_net_objs_take(void *out, int max, int *from) {
    plat_mutex_lock(&lock);
    int n = 0;
    if (objs_in_n) {
        n = objs_in[0].len <= max ? objs_in[0].len : 0;
        if (n) memcpy(out, objs_in[0].d, n);
        *from = objs_in[0].from;
        memmove(objs_in, objs_in + 1, (size_t)(objs_in_n - 1) * sizeof objs_in[0]), objs_in_n--;
        if (!n) n = -1;
    }
    plat_mutex_unlock(&lock);
    return n;
}

void coop_net_hit_send(const void *data, int len) {
    if (len < 0 || len > COOP_HIT_MAX) return;
    plat_mutex_lock(&lock);
    if (hit_out_n < HITQ) memcpy(hit_out[hit_out_n].d, data, len), hit_out[hit_out_n].len = len, hit_out_n++;
    plat_mutex_unlock(&lock);
}

int coop_net_hit_take(void *out, int max, int *from) {
    plat_mutex_lock(&lock);
    int n = 0;
    if (hit_in_n) {
        n = hit_in[0].len <= max ? hit_in[0].len : 0;
        memcpy(out, hit_in[0].d, n), *from = hit_in[0].from;
        memmove(hit_in, hit_in + 1, (size_t)(hit_in_n - 1) * sizeof hit_in[0]), hit_in_n--;
        if (!n) n = -1;   // dropped (too big): keep the caller looping
    }
    plat_mutex_unlock(&lock);
    return n;
}
