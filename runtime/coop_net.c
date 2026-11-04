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

enum { HELLO = 1, WELCOME = 2, STATE = 3, LEAVE = 4, BYE = 5, VERSION = 2, HEADER = 8 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static NetState mine;                 // lock
static int mine_set;                  // lock
static struct { NetState st; uint64_t at_ns; int live; char name[17]; } others[COOP_MAX_PLAYERS + 1];   // lock; by id
static volatile int my_id;            // 0 until WELCOME
static int started;

static void header(uint8_t *p, int kind) { memcpy(p, "TKCO", 4), p[4] = VERSION, p[5] = (uint8_t)kind, p[6] = (uint8_t)my_id, p[7] = 0; }

static void net_thread(void *arg) {
    (void)arg;
    const char *relay = cfg_get("TOTK_COOP_RELAY"), *name = cfg_get("TOTK_COOP_NAME");
    char host[256];
    snprintf(host, sizeof host, "%s", relay);
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
        return;
    }
    Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) { rt_log("coop net: no socket; co-op networking off"); freeaddrinfo(res); return; }
#ifdef _WIN32
    DWORD tmo = 10;   // ms: the loop sends about every 33 ms
#else
    struct timeval tmo = {0, 10000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    rt_log("coop net: relay %s:%s, name \"%s\"", host, port, name ? name : "Player");
    uint64_t last_hello = 0, last_state = 0, seq = 0;
    uint8_t buf[2048];
    for (;;) {
        uint64_t now = plat_now_ns();
        if (!my_id && now - last_hello > 1000000000ull) {
            last_hello = now;
            header(buf, HELLO);
            memset(buf + HEADER, 0, 16);
            snprintf((char *)buf + HEADER, 16, "%s", name ? name : "Player");
            sendto(s, (const char *)buf, HEADER + 16, 0, res->ai_addr, (int)res->ai_addrlen);
        }
        if (my_id && now - last_state > 33000000ull) {
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
            if (seq % 30 == 0) {   // once a second: keep the relay's seat when there is no state to send
                header(buf, HELLO);
                memset(buf + HEADER, 0, 16);
                snprintf((char *)buf + HEADER, 16, "%s", name ? name : "Player");
                sendto(s, (const char *)buf, HEADER + 16, 0, res->ai_addr, (int)res->ai_addrlen);
                if (!have) seq++;
            }
        }
        int n = (int)recvfrom(s, (char *)buf, sizeof buf, 0, NULL, NULL);
        if (n < HEADER || memcmp(buf, "TKCO", 4) || buf[4] != VERSION) continue;
        int kind = buf[5], id = buf[6];
        if (kind == WELCOME) {
            if (!my_id) rt_log("coop net: joined as player %d", id);
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
        } else if (kind == LEAVE && id >= 1 && id <= COOP_MAX_PLAYERS) {
            plat_mutex_lock(&lock);
            if (others[id].live) rt_log("coop net: player %d left", id);
            others[id].live = 0;
            plat_mutex_unlock(&lock);
        }
    }
}

int coop_net_on(void) {
    if (!started) {
        started = 1;
        const char *relay = cfg_get("TOTK_COOP_RELAY");
        if (relay && *relay) plat_thread_spawn(net_thread, NULL, "coop-net", 0);
        else started = 2;
    }
    return started == 1;
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
