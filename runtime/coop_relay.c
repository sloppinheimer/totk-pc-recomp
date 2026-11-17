// The co-op relay inside the game (the Host button): the same protocol as server/coop_relay.py, on a thread of its
// own. It hands out player ids on HELLO, forwards STATE / WORLD / HIT / POSE / OBJS / EPOSE packets to everyone else
// (CTR packets to the player header byte 7 names, everyone else when 0) and drops players
// that stay quiet for 5 s. The hosting game joins it itself through 127.0.0.1 like any other player.
// The port may be open to the internet (UPnP), so a seat is given only to a HELLO that carries this session's code: a
// random code made at Host, shown in the Co-op tab with the address, typed by the joining players after it. Without
// it, anyone who reached the port took a seat, received every player's state and could send anything to every game,
// and a HELLO from a forged address made the relay stream the game to that address.
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
#include <netinet/in.h>
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

enum { HELLO = 1, WELCOME = 2, STATE = 3, LEAVE = 4, BYE = 5, WORLD = 6, HIT = 7, POSE = 8, OBJS = 9, EPOSE = 10, CTR = 11, HEADER = 8 };

static volatile int relay_gen;    // a new value stops the running relay thread
static volatile int relay_port;   // nonzero while a relay runs here
static volatile int relay_players;
static char relay_code[COOP_CODE_LEN + 1];   // this session's code (set before the thread starts)
static PlatMutex host_lock = PLAT_MUTEX_INIT;   // the router's forward opened by the relay thread, closed by Leave

typedef struct { struct sockaddr_in addr; int id; char name[17]; uint64_t seen_ns; } Peer;

static void put_header(uint8_t *p, int kind, int id) {
    memcpy(p, "TKCO", 4), p[4] = COOP_NET_VERSION, p[5] = (uint8_t)kind, p[6] = (uint8_t)id, p[7] = 0;
}

static int same(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void relay_thread(void *arg) {
    int gen = (int)(intptr_t)arg, port = relay_port;
    Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET, me.sin_port = htons((uint16_t)port), me.sin_addr.s_addr = htonl(INADDR_ANY);
    // (tried for up to 0.6 s: a relay stopped just before keeps the port until its receive times out, 0.2 s, and a
    // relay started right after found it taken; code review, 2026-11-06)
    int bound = 0;
    for (int t = 0; s != SOCK_BAD && t < 6 && !(bound = !bind(s, (struct sockaddr *)&me, sizeof me)) && relay_gen == gen; t++) plat_sleep_ms(100);
    if (s == SOCK_BAD || !bound) {
        rt_log("coop relay: cannot use UDP port %d (another relay or game there?)", port);
        if (s != SOCK_BAD) closesocket(s);
        if (relay_gen == gen) relay_port = 0;
        return;
    }
#ifdef _WIN32
    DWORD tmo = 200;
#else
    struct timeval tmo = {0, 200000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    int rcvbuf = 1 << 20;   // enemy poses come in bursts (coop_world.c)
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    rt_log("coop relay: listening on UDP %d", port);
    // The router is asked to forward the port only once the relay has it: a relay that could not bind it left a
    // forward to whatever held the port, and Leave did not take it back (Leave now always does).
    plat_mutex_lock(&host_lock);   // (with coop_end: a Leave between the test and the open left a forward nobody closed)
    if (relay_gen == gen) coop_upnp_open();
    plat_mutex_unlock(&host_lock);
    Peer peers[COOP_MAX_PLAYERS];
    int np = 0;
    uint8_t buf[8192], out[8192];
    while (relay_gen == gen) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = (int)recvfrom(s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        uint64_t now = plat_now_ns();
        for (int i = 0; i < np;)   // quiet for 5 s: gone
            if (now - peers[i].seen_ns > 5000000000ull) {
                rt_log("coop relay: player %d \"%s\" left (timed out)", peers[i].id, peers[i].name);
                put_header(out, LEAVE, peers[i].id);
                for (int j = 0; j < np; j++)
                    if (j != i) sendto(s, (const char *)out, HEADER, 0, (struct sockaddr *)&peers[j].addr, sizeof peers[j].addr);
                peers[i] = peers[--np];
            } else
                i++;
        relay_players = np;
        if (n < HEADER || memcmp(buf, "TKCO", 4) || buf[4] != COOP_NET_VERSION) continue;
        int kind = buf[5], at = -1;
        for (int i = 0; i < np; i++) if (same(&peers[i].addr, &from)) at = i;
        if (kind == HELLO) {
            if (at < 0) {
                char code[COOP_CODE_LEN + 1] = "";
                if (n >= HEADER + COOP_HELLO_LEN) memcpy(code, buf + HEADER + 16, COOP_CODE_LEN);
                for (int i = 0; code[i]; i++) if (code[i] >= 'a' && code[i] <= 'z') code[i] -= 32;
                if (strcmp(code, relay_code)) {   // (logged at most 5 times, then every 1000th: a flood must not fill the log)
                    static unsigned refused;
                    if (refused++ < 5 || refused % 1000 == 0) {
                        char ip[64];
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                        rt_log("coop relay: a HELLO from %s without this session's code was refused (%u so far)", ip, refused);
                    }
                    continue;
                }
                if (np == COOP_MAX_PLAYERS) {
                    static unsigned full;
                    if (full++ < 5 || full % 1000 == 0) rt_log("coop relay: a player was refused: %d already", np);
                    continue;
                }
                int id = 1;
                for (int used = 1; used;) {
                    used = 0;
                    for (int i = 0; i < np; i++) if (peers[i].id == id) used = 1, id++;
                }
                at = np++;
                memset(&peers[at], 0, sizeof peers[at]);
                peers[at].addr = from, peers[at].id = id;
                memcpy(peers[at].name, buf + HEADER, n - HEADER >= 16 ? 16 : n - HEADER);
                coop_name_clean(peers[at].name, 16);
                char ip[64];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                rt_log("coop relay: player %d \"%s\" joined from %s:%d; %d connected", id, peers[at].name, ip,
                       ntohs(from.sin_port), np);
            }
            peers[at].seen_ns = now;
            put_header(out, WELCOME, peers[at].id);
            int o = HEADER;
            for (int i = 0; i < np; i++)
                if (i != at) out[o] = (uint8_t)peers[i].id, memcpy(out + o + 1, peers[i].name, 16), o += 17;
            sendto(s, (const char *)out, o, 0, (struct sockaddr *)&from, sizeof from);
        } else if ((kind == STATE || kind == WORLD || kind == HIT || kind == POSE || kind == OBJS || kind == EPOSE || kind == CTR) && at >= 0) {
            peers[at].seen_ns = now;
            int to = kind == CTR ? buf[7] : 0;   // (CTR: one player, or everyone else when 0)
            memcpy(out, buf, n);
            put_header(out, kind, peers[at].id);
            out[7] = (uint8_t)to;
            for (int i = 0; i < np; i++)
                if (i != at && (!to || peers[i].id == to))
                    sendto(s, (const char *)out, n, 0, (struct sockaddr *)&peers[i].addr, sizeof peers[i].addr);
        } else if (kind == BYE && at >= 0) {
            rt_log("coop relay: player %d \"%s\" left (said bye)", peers[at].id, peers[at].name);
            put_header(out, LEAVE, peers[at].id);
            for (int i = 0; i < np; i++)
                if (i != at) sendto(s, (const char *)out, HEADER, 0, (struct sockaddr *)&peers[i].addr, sizeof peers[i].addr);
            peers[at] = peers[--np];
        }
    }
    closesocket(s);
    rt_log("coop relay: stopped");
}

int coop_relay_start(int port) {
    if (relay_port) return 1;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    relay_port = port, relay_players = 0;
    plat_thread_spawn(relay_thread, (void *)(intptr_t)++relay_gen, "coop-relay", 0);
    return 1;
}

void coop_relay_stop(void) {
    relay_gen++;
    relay_port = 0;
    relay_players = 0;
}

int coop_relay_port(void) { return relay_port; }
const char *coop_relay_code(void) { return relay_port ? relay_code : ""; }

// A new session code: COOP_CODE_LEN (8) characters from an alphabet without look-alikes (0/O, 1/I): 40 bits, so
// guessing it over the internet takes years, not hours.
// TOTK_COOP_HOST_CODE sets it (the test scripts, which join by script).
static void new_code(void) {
    static const char abc[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const char *fixed = cfg_get("TOTK_COOP_HOST_CODE");
    if (fixed && *fixed) {
        int k = 0;   // (letters and digits only, upper case: what a join string can carry after its space)
        for (; *fixed && k < COOP_CODE_LEN; fixed++)
            if ((*fixed >= '0' && *fixed <= '9') || (*fixed >= 'A' && *fixed <= 'Z') || (*fixed >= 'a' && *fixed <= 'z'))
                relay_code[k++] = (char)(*fixed >= 'a' ? *fixed - 32 : *fixed);
        relay_code[k] = 0;
        if (k) return;
    }
    uint8_t r[COOP_CODE_LEN] = {0};
    if (!plat_random(r, sizeof r)) {   // (no system randomness: the clock, which is weaker but still unknown outside)
        uint64_t t = plat_now_ns() * 0x9e3779b97f4a7c15ull;
        memcpy(r, &t, sizeof r);
    }
    for (int i = 0; i < COOP_CODE_LEN; i++) relay_code[i] = abc[r[i] & 31];
    relay_code[COOP_CODE_LEN] = 0;
}

// This PC's addresses to give the other players: the one its traffic to the internet leaves from (a UDP "connect"
// sends nothing), which is the LAN address behind a router.
int coop_local_address(char *out, size_t n) {
    out[0] = 0;
    Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) return 0;
    struct sockaddr_in to = {0}, me = {0};
    to.sin_family = AF_INET, to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    socklen_t ml = sizeof me;
    int ok = !connect(s, (struct sockaddr *)&to, sizeof to) && !getsockname(s, (struct sockaddr *)&me, &ml);
    if (ok) inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n);
    closesocket(s);
    return ok;
}

// Host: the relay here, joined through 127.0.0.1 (the menu's Host button and TOTK_COOP_TEST_HOST).
void coop_host(const char *name) {
    if (!coop_relay_port()) new_code();
    coop_relay_start(COOP_PORT);   // (it asks the router to forward the port once it has bound it)
    char self[48];
    snprintf(self, sizeof self, "127.0.0.1:%d %s", COOP_PORT, relay_code);
    coop_net_join(self, name);
}

void coop_end(void) {
    coop_net_leave();
    plat_mutex_lock(&host_lock);
    coop_upnp_close();   // (always: a forward asked for is taken back whether or not the relay ever bound the port)
    coop_relay_stop();
    plat_mutex_unlock(&host_lock);
}

// Tests of the menu's actions without a menu: TOTK_COOP_TEST_HOST=<s>, TOTK_COOP_TEST_JOIN=<s>:<address>,
// TOTK_COOP_TEST_LEAVE=<s> act that many seconds into the world (called each poll).
void coop_session_tests(int64_t world_us) {
    static int done[3];
    const char *h = cfg_get("TOTK_COOP_TEST_HOST"), *j = cfg_get("TOTK_COOP_TEST_JOIN"), *l = cfg_get("TOTK_COOP_TEST_LEAVE");
    double s = world_us / 1e6;
    if (h && !done[0] && s >= atof(h)) done[0] = 1, rt_log("coop: test: host"), coop_host(cfg_get("TOTK_COOP_NAME"));
    if (j && !done[1] && s >= atof(j)) {
        const char *addr = strchr(j, ':');
        done[1] = 1;
        rt_log("coop: test: join %s", addr ? addr + 1 : "?");
        if (addr) coop_net_join(addr + 1, cfg_get("TOTK_COOP_NAME"));
    }
    if (l && !done[2] && s >= atof(l)) done[2] = 1, rt_log("coop: test: leave"), coop_end();
}