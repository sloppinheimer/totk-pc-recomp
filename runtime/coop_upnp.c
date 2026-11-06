// Opening the co-op port on the host's router (UPnP Internet Gateway Device), so players over the internet can
// join without forwarding UDP 41900 by hand. On Host, a thread of its own:
//   1. SSDP: an M-SEARCH to 239.255.255.250:1900 for an internet gateway; the answer's LOCATION is its description.
//   2. HTTP GET of the description; the WANIPConnection (or WANPPPConnection) service and its control URL.
//   3. SOAP AddPortMapping: UDP 41900 -> this PC's LAN address, a one-hour lease renewed every 30 minutes;
//      GetExternalIPAddress for the address to give the others.
// On Leave, DeletePortMapping. Whatever fails is said in the Co-op tab (coop_upnp_status): many routers ship with
// UPnP off, and behind a provider's shared address (CGNAT, an external address that is itself private) no router
// setting helps.
#include <ctype.h>
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define closesocket close
#endif

#include "coop_net.h"
#include "host.h"
#include "platform/platform.h"

static PlatMutex lock = PLAT_MUTEX_INIT;
static char status_arg[160];    // lock: the state's detail (the address, or why it failed)
static char external_ip[64];    // lock; "" until known
static volatile int upnp_gen;   // a new value stops the running thread (and its mapping is deleted)
static int upnp_state;          // lock; COOP_UPNP_*

// A thread of an ended session (Leave, or a new Host) no longer sets the status: it went on to "open" or "failed"
// after the session had ended (code review, 2026-11-06). The thread's own session, -1 outside one.
static __thread int cur_gen = -1;
static void set_status(int state, const char *arg) {
    if (cur_gen >= 0 && cur_gen != upnp_gen) return;
    plat_mutex_lock(&lock);
    upnp_state = state;
    snprintf(status_arg, sizeof status_arg, "%s", arg ? arg : "");
    plat_mutex_unlock(&lock);
    static const char *const names[] = {"none", "working", "open", "open, no address given", "open, shared address", "failed"};
    rt_log("coop upnp: %s %s", names[state], arg ? arg : "");
}

static void set_timeouts(Sock s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
}

// A header's value in an HTTP answer ("" if none), case-insensitive name.
static void header_value(const char *msg, const char *name, char *out, size_t n) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = msg; *p; p++) {
        if ((p == msg || p[-1] == '\n') && !strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ') p++;
            size_t k = 0;
            while (p[k] && p[k] != '\r' && p[k] != '\n' && k + 1 < n) out[k] = p[k], k++;
            out[k] = 0;
            return;
        }
    }
}

// http://host[:port]/path -> its parts.
static int split_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn) {
    if (strncmp(url, "http://", 7)) return 0;
    const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
    if (!slash) slash = h + strlen(h);
    if (colon && colon < slash) {
        snprintf(host, hn, "%.*s", (int)(colon - h), h);
        *port = atoi(colon + 1);
    } else {
        snprintf(host, hn, "%.*s", (int)(slash - h), h);
        *port = 80;
    }
    snprintf(path, pn, "%s", *slash ? slash : "/");
    return host[0] != 0;
}

// One HTTP request; the whole answer in out (NUL-terminated), its length or -1.
static int http(const char *host, int port, const char *request, char *out, int max) {
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_BAD) { freeaddrinfo(res); return -1; }
    set_timeouts(s, 3000);
    int n = -1;
    if (!connect(s, res->ai_addr, (int)res->ai_addrlen) && send(s, request, (int)strlen(request), 0) > 0) {
        n = 0;
        for (int r; n < max - 1 && (r = (int)recv(s, out + n, max - 1 - n, 0)) > 0;) n += r;
        out[n] = 0;
    }
    closesocket(s);
    freeaddrinfo(res);
    return n;
}

// The text between <tag> and </tag> after from (namespaces ignored: "<m:tag>" matches too), or NULL.
static const char *xml_value(const char *from, const char *tag, char *out, size_t n) {
    char open[64];
    snprintf(open, sizeof open, "%s>", tag);
    for (const char *p = strstr(from, open); p; p = strstr(p + 1, open)) {
        if (p == from || (p[-1] != '<' && p[-1] != ':')) continue;
        const char *v = p + strlen(open), *e = strchr(v, '<');
        if (!e) return NULL;
        snprintf(out, n, "%.*s", (int)(e - v), v);
        return e;
    }
    return NULL;
}

typedef struct { char host[128], path[256], service[128]; int port; } Gateway;

// Steps 1 and 2: the gateway's control URL and service type.
static int find_gateway(Gateway *gw, char *why, size_t wn) {
    Sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) { snprintf(why, wn, "no network socket"); return 0; }
    set_timeouts(s, 1000);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET, to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    static const char *const targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                          "urn:schemas-upnp-org:service:WANIPConnection:1",
                                          "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    // Every answer in the time given (several devices can call themselves gateways: mesh nodes, extenders): the
    // first whose description has a port forwarding service is the one.
    char locs[8][512], msg[2048];
    int nloc = 0;
    for (int round = 0; round < 2 && !nloc; round++) {
        for (int t = 0; t < 3; t++) {
            snprintf(msg, sizeof msg,
                     "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n",
                     targets[t]);
            sendto(s, msg, (int)strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
        }
        uint64_t until = plat_now_ns() + 2500000000ull;
        while (plat_now_ns() < until && nloc < 8) {
            int n = (int)recvfrom(s, msg, sizeof msg - 1, 0, NULL, NULL);
            if (n <= 0) continue;
            msg[n] = 0;
            char loc[512];
            header_value(msg, "LOCATION", loc, sizeof loc);
            int seen = !loc[0];
            for (int i = 0; i < nloc && !seen; i++) seen = !strcmp(locs[i], loc);
            if (!seen) snprintf(locs[nloc++], sizeof locs[0], "%s", loc);
        }
    }
    closesocket(s);
    if (!nloc) { snprintf(why, wn, "no router answered (UPnP is off, or not supported)"); return 0; }
    snprintf(why, wn, "the router has no port forwarding service");
    for (int li = 0; li < nloc; li++) {
        char host[128], path[256], req[1024];
        int port = 80;
        if (!split_url(locs[li], host, sizeof host, &port, path, sizeof path)) continue;
        snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
        static char desc[65536];
        if (http(host, port, req, desc, sizeof desc) <= 0) continue;
        static const char *const services[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                               "urn:schemas-upnp-org:service:WANIPConnection:1",
                                               "urn:schemas-upnp-org:service:WANPPPConnection:1"};
        for (int k = 0; k < 3; k++) {
            const char *at = strstr(desc, services[k]);
            char ctl[256];
            if (!at || !xml_value(at, "controlURL", ctl, sizeof ctl)) continue;
            snprintf(gw->service, sizeof gw->service, "%s", services[k]);
            snprintf(gw->host, sizeof gw->host, "%s", host);
            gw->port = port;
            if (!strncmp(ctl, "http://", 7)) {
                char h2[128];
                split_url(ctl, h2, sizeof h2, &gw->port, gw->path, sizeof gw->path);
                snprintf(gw->host, sizeof gw->host, "%s", h2);
            } else
                snprintf(gw->path, sizeof gw->path, "%s%s", ctl[0] == '/' ? "" : "/", ctl);
            rt_log("coop upnp: %d device(s) answered; using %s", nloc, locs[li]);
            return 1;
        }
    }
    return 0;
}

// A SOAP action on the gateway; the answer in out. Returns 1 on HTTP 200.
static int soap(const Gateway *gw, const char *action, const char *args, char *out, int max) {
    char body[2048], req[3072];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n",
             action, gw->service, args, action);
    snprintf(req, sizeof req,
             "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "SOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
             gw->path, gw->host, gw->port, gw->service, action, (int)strlen(body), body);
    int n = http(gw->host, gw->port, req, out, max);
    return n > 12 && !strncmp(out + 9, "200", 3);
}

static int private_v4(const char *ip) {
    unsigned a, b;
    if (sscanf(ip, "%u.%u", &a, &b) != 2) return 0;
    return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128);
}

static int lease_used = -1;   // the lease the router took: 3600, or 0 (until removed)
static int add_mapping(const Gateway *gw, const char *lan, char *out, int max) {
    char args[768];
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>TotK co-op</NewPortMappingDescription><NewLeaseDuration>3600</NewLeaseDuration>",
             COOP_PORT, COOP_PORT, lan);
    if (soap(gw, "AddPortMapping", args, out, max)) { lease_used = 3600; return 1; }
    // Some routers take only lease 0 (until removed); the mapping is removed on Leave either way.
    char *lease = strstr(args, "<NewLeaseDuration>3600");
    if (lease) memcpy(lease, "<NewLeaseDuration>0   ", 22);
    int ok = soap(gw, "AddPortMapping", args, out, max);
    if (ok) lease_used = 0;
    return ok;
}

static void delete_mapping(const Gateway *gw, char *out, int max) {
    char args[256];
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>",
             COOP_PORT);
    int ok = soap(gw, "DeletePortMapping", args, out, max);
    if (!ok) {   // some routers (this PC's, 2026-11-04) answer 402 Invalid Args to an empty remote host but take "*"
        snprintf(args, sizeof args,
                 "<NewRemoteHost>*</NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                 COOP_PORT);
        ok = soap(gw, "DeletePortMapping", args, out, max);
    }
    char code[32] = "", desc[96] = "", status[48] = "";
    if (!ok) {
        xml_value(out, "errorCode", code, sizeof code), xml_value(out, "errorDescription", desc, sizeof desc);
        snprintf(status, sizeof status, "%.*s", (int)strcspn(out, "\r\n") < 40 ? (int)strcspn(out, "\r\n") : 40, out);
    }
    rt_log("coop upnp: %s UDP %d on the router%s%s%s%s%s", ok ? "removed the forwarding of" : "could not remove the forwarding of",
           COOP_PORT, ok ? "" : " (", status, code[0] ? ", error " : "", code, ok ? "" : ")");
    if (desc[0]) rt_log("coop upnp: the router said: %s", desc);
}

static void upnp_session(int gen);
// One at a time: a new session's thread waits for the last one, which removes its forwarding first. Overlapping, the
// old one's removal of the same port could come after the new one's forwarding, and both used the same buffers
// (code review, 2026-11-06).
static PlatMutex run_lock = PLAT_MUTEX_INIT;
static void upnp_thread(void *arg) {
    int gen = (int)(intptr_t)arg;
    plat_mutex_lock(&run_lock);
    cur_gen = gen;
    if (upnp_gen == gen) upnp_session(gen);
    plat_mutex_unlock(&run_lock);
}
static void upnp_session(int gen) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    static char out[16384];
    char why[128], lan[64], ip[64] = "";
    Gateway gw;
    set_status(COOP_UPNP_WORKING, NULL);
    if (!coop_local_address(lan, sizeof lan)) { set_status(COOP_UPNP_FAILED, "no network address on this PC"); return; }
    if (!find_gateway(&gw, why, sizeof why)) {
        set_status(COOP_UPNP_FAILED, why);
        return;
    }
    if (soap(&gw, "GetExternalIPAddress", "", out, sizeof out)) xml_value(out, "NewExternalIPAddress", ip, sizeof ip);
    if (!add_mapping(&gw, lan, out, sizeof out)) {
        char code[32] = "";
        xml_value(out, "errorCode", code, sizeof code);
        snprintf(why, sizeof why, "the router refused (error %s)", code[0] ? code : "?");
        set_status(COOP_UPNP_FAILED, why);
        return;
    }
    plat_mutex_lock(&lock);
    if (upnp_gen == gen) snprintf(external_ip, sizeof external_ip, "%s", ip);
    plat_mutex_unlock(&lock);
    if (ip[0] && private_v4(ip))
        set_status(COOP_UPNP_SHARED, ip);
    else if (ip[0])
        set_status(COOP_UPNP_OPEN, ip);
    else
        set_status(COOP_UPNP_OPEN_NOIP, NULL);
    rt_log("coop upnp: lease %d s, control %s:%d%s (%s)", lease_used, gw.host, gw.port, gw.path, gw.service);
    // Keep it: renew the lease every 30 minutes; remove it when the session ends.
    uint64_t renew = plat_now_ns() + 1800000000000ull;
    while (upnp_gen == gen) {
        plat_sleep_ms(200);
        if (plat_now_ns() > renew) add_mapping(&gw, lan, out, sizeof out), renew = plat_now_ns() + 1800000000000ull;
    }
    delete_mapping(&gw, out, sizeof out);
}

void coop_upnp_open(void) {
    plat_mutex_lock(&lock);
    external_ip[0] = 0;
    plat_mutex_unlock(&lock);
    plat_thread_spawn(upnp_thread, (void *)(intptr_t)++upnp_gen, "coop-upnp", 0);
}

void coop_upnp_close(void) {
    upnp_gen++;
    plat_mutex_lock(&lock);
    upnp_state = COOP_UPNP_NONE, status_arg[0] = 0, external_ip[0] = 0;
    plat_mutex_unlock(&lock);
}

int coop_upnp_status(char *out, size_t n) {
    plat_mutex_lock(&lock);
    snprintf(out, n, "%s", status_arg);
    int st = upnp_state;
    plat_mutex_unlock(&lock);
    return st;
}
