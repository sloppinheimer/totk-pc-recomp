// Co-op notices: a line on screen when a player joins, leaves or fast travels, with the place's name ("the tester
// travelled to Lookout Landing Skyview Tower"). TOTK_COOP_NOTICES=0 turns all of it off.
//
// Shown with the port's overlay (ui/coop_notices.cpp, the way ui/coop_tags.cpp draws the name tags), not the game's
// own banners: the game's location banner (layout Pa_LocationName, opened by the sequence node
// SeqOneShotRequestOpenLocation) takes a location, not a text, and putting our text into one of its panes was not
// found; docs/coop/proxy-plan.md, "Notices", has what was looked at.
//
// Joins and leaves: coop_net.c learns them (a player's first STATE, a LEAVE from the relay) and keeps the list that
// coop_net_players reads; each overlay frame compares that list with the last frame's. A player's name comes with the
// relay's WELCOME, about once a second, so a join waits up to 3 s for it.
//
// Fast travel, on the traveller's side: the game's warp request was not found (the sequence's WarpMgr and its warp
// events, DmF_SY_WarpOut / DmF_SY_WarpIn in Game/WarpInfoTable, were looked at), so a travel is this Link standing more
// than 250 m from where it stood a tick before, alive (a death's respawn is not one), and, 1.5 s later, within 40 m of a
// travel point: a shrine, a Skyview Tower or a lightroot. Nowhere near one (a travel medallion, a save loaded): "is now
// near" the nearest named place within 600 m, or nothing. The places are the game's map locations,
// Banc/MainField/LocationArea/MainField.locationarea.byml (and MinusField's for the Depths): each has a position and a
// LocationName "Work/Location/<label>.game__location__Location.gyml", and <label> is its message label in the
// message archive Mals/<language>.Product.<n>.sarc.zs, LocationMsg/Location.msbt (City_BaseCamp: "Lookout Landing",
// CheckPoint000: "Stakijat Lightroot"); a shrine's label is Dungeon<nnn> and its name is label 0<nnn> of
// LocationMsg/Dungeon.msbt. Every shrine, tower and lightroot of 1.4.3 has its name there (checked with the files).
// The label goes to the others (a CoopTravelPacket in a HIT packet, kind "TR"), and each game names the place in its
// own language.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "fs.h"
#include "host.h"
#include "layout.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"
#include "utf16.h"

int coop_life_of(uint64_t a);   // coop_world.c: -1 when it has no life component

int coop_notices_on(void) {
    const char *v = cfg_get("TOTK_COOP_NOTICES");
    return !(v && !strcmp(v, "0"));
}

// ---- the notices shown
#define NOTICES 8
static PlatMutex lock = PLAT_MUTEX_INIT;
static struct { char text[160]; uint64_t at_ns; uint32_t id; } shown[NOTICES];   // lock; oldest first
static int nshown;   // lock
static uint32_t told;   // lock

// fmt: a catalog text (menu_text) with %s for the names.
static void notice(const char *fmt, ...) {
    char t[160];
    va_list ap;
    va_start(ap, fmt);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(t, sizeof t, fmt, ap);
#pragma clang diagnostic pop
    va_end(ap);
    rt_log("coop: notice: %s", t);
    plat_mutex_lock(&lock);
    if (nshown == NOTICES) memmove(shown, shown + 1, (NOTICES - 1) * sizeof shown[0]), nshown--;
    snprintf(shown[nshown].text, sizeof shown[nshown].text, "%s", t);
    shown[nshown].id = ++told;
    shown[nshown++].at_ns = plat_now_ns();
    plat_mutex_unlock(&lock);
}

int coop_notices_get(CoopNotice *out, int max) {
    uint64_t now = plat_now_ns();
    int n = 0, live = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < nshown; i++) live += (now - shown[i].at_ns) / 1e9 < COOP_NOTICE_SECONDS;
    int skip = live > max ? live - max : 0;   // more live than asked for: the newest max of them
    for (int i = 0; i < nshown && n < max; i++) {
        float age = (float)((now - shown[i].at_ns) / 1e9);
        if (age >= COOP_NOTICE_SECONDS || skip-- > 0) continue;
        snprintf(out[n].text, sizeof out[n].text, "%s", shown[i].text);
        out[n].id = shown[i].id;
        out[n++].age_s = age;
    }
    plat_mutex_unlock(&lock);
    return n;
}

// ---- joins and leaves (render thread, each overlay frame)
static struct { int here; char name[17]; uint64_t since_ns; int told; } roster[COOP_MAX_PLAYERS + 1];   // by id

static const char *name_or(const char *name, int id, char *buf, size_t n) {
    if (name && name[0]) return name;
    snprintf(buf, n, "Player %d", id);
    return buf;
}

static void roster_tick(void) {
    static uint64_t joined_ns;   // when this game's own seat was given (the players already here are not "joining")
    static int was_on;
    int on = *coop_net_target() && coop_net_id();
    uint64_t now = plat_now_ns();
    if (!on) {   // not in a session (or left it, or it ended): forget the list without notices
        if (was_on) memset(roster, 0, sizeof roster);
        was_on = 0;
        return;
    }
    if (!was_on) joined_ns = now, was_on = 1;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int n = coop_net_players(ids, names, COOP_MAX_PLAYERS);
    int seen[COOP_MAX_PLAYERS + 1] = {0};
    char buf[24];
    for (int k = 0; k < n; k++) {
        int id = ids[k];
        if (id < 1 || id > COOP_MAX_PLAYERS) continue;
        seen[id] = 1;
        if (!roster[id].here) roster[id].here = 1, roster[id].since_ns = now, roster[id].told = 0;
        if (names[k][0]) memcpy(roster[id].name, names[k], 17);
        // told once its name is known, or after 3 s without one
        if (!roster[id].told && (roster[id].name[0] || now - roster[id].since_ns > 3000000000ull)) {
            roster[id].told = 1;
            const char *nm = name_or(roster[id].name, id, buf, sizeof buf);
            // in the first 5 s of this game's own seat the others were here before it
            if (roster[id].since_ns - joined_ns < 5000000000ull) notice(menu_text(UI_MENU_COOP_NOTICE_HERE), nm);
            else notice(menu_text(UI_MENU_COOP_NOTICE_JOINED), nm);
        }
    }
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++)
        if (roster[id].here && !seen[id]) {
            if (roster[id].told) notice(menu_text(UI_MENU_COOP_NOTICE_LEFT), name_or(roster[id].name, id, buf, sizeof buf));
            memset(&roster[id], 0, sizeof roster[id]);
        }
}

int coop_notices_wanted(void) {
    if (!coop_notices_on()) return 0;
    roster_tick();
    CoopNotice one;
    return coop_notices_get(&one, 1) > 0;
}

// ---- the places (loaded once, on first use; read-only after)
typedef struct { float p[3]; char label[40]; char name[64]; uint8_t travel; } Place;
static Place *places;
static int nplaces;

static uint32_t u32_at(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// SARC member `name` of p[0..n): its start and size; 0 when it is not there.
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x14 || memcmp(p, "SARC", 4)) return 0;
    uint16_t head, count;
    memcpy(&head, p + 4, 2);
    uint32_t data = u32_at(p + 12);
    if ((size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t nodes = (size_t)head + 12, names = nodes + (size_t)count * 16;
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return 0;
    names += 8;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + nodes + i * 16;
        size_t off = names + (size_t)(u32_at(e + 4) & 0xffffff) * 4;
        size_t lo = (size_t)data + u32_at(e + 8), hi = (size_t)data + u32_at(e + 12);
        if (off >= n || hi > n || lo > hi) continue;
        size_t len = strnlen((const char *)p + off, n - off);
        if (len == strlen(name) && !memcmp(p + off, name, len)) {
            *at = lo, *size = hi - lo;
            return 1;
        }
    }
    return 0;
}

// The RomFS file `rel` decompressed with the dictionary of Pack/ZsDic.pack.zs its frame names (malloc'd), or NULL.
static uint8_t *dicts;   // Pack/ZsDic.pack.zs decompressed (a SARC of the dictionaries)
static size_t dicts_n;
static uint8_t *read_zs(const char *rel, size_t *out_n) {
    size_t n;
    uint8_t *in = romfs_read_whole(rel, &n, "co-op notices name no places");
    if (!in) return NULL;
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    uint8_t *raw = rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn < (64u << 20) ? malloc((size_t)rn) : NULL;
    ZSTD_DDict *dd = NULL;
    static const char *const names[] = {"zs.zsdic", "bcett.byml.zsdic", "pack.zsdic"};
    for (size_t k = 0; raw && id && !dd && k < sizeof names / sizeof *names; k++) {
        size_t at, sz;
        if (sarc_member(dicts, dicts_n, names[k], &at, &sz) && ZSTD_getDictID_fromDict(dicts + at, sz) == id)
            dd = ZSTD_createDDict(dicts + at, sz);
    }
    ZSTD_DCtx *dc = raw ? ZSTD_createDCtx() : NULL;
    size_t got = !dc ? 0 : dd ? ZSTD_decompress_usingDDict(dc, raw, (size_t)rn, in, n, dd) : ZSTD_decompressDCtx(dc, raw, (size_t)rn, in, n);
    ZSTD_freeDCtx(dc);
    ZSTD_freeDDict(dd);
    free(in);
    if (!raw || ZSTD_isError(got) || got != rn) {
        rt_log("coop: notices: cannot decompress %s; no place names", rel);
        free(raw);
        return NULL;
    }
    *out_n = (size_t)rn;
    return raw;
}

// ---- BYML (version 7, little endian): just what the location areas need
typedef struct { const uint8_t *d; size_t n; uint32_t keys, strs; } Byml;
enum { BY_STR = 0xa0, BY_ARR = 0xc0, BY_DICT = 0xc1, BY_TAB = 0xc2, BY_FLOAT = 0xd2 };

static const char *by_str(const Byml *b, uint32_t tab, uint32_t i) {
    if (!tab || (size_t)tab + 4 > b->n || b->d[tab] != BY_TAB || i >= u32_at(b->d + tab) >> 8) return NULL;
    if ((size_t)tab + 8 + 4 * (size_t)i > b->n) return NULL;
    size_t o = (size_t)tab + u32_at(b->d + tab + 4 + 4 * i);
    return o < b->n && memchr(b->d + o, 0, b->n - o) ? (const char *)b->d + o : NULL;
}

static uint32_t by_count(const Byml *b, uint32_t off, int type) {
    return (size_t)off + 4 <= b->n && b->d[off] == type ? u32_at(b->d + off) >> 8 : 0;
}

static int by_dict(const Byml *b, uint32_t off, uint32_t i, const char **key, int *t, uint32_t *v) {
    size_t e = (size_t)off + 4 + 8 * (size_t)i;
    if (e + 8 > b->n) return 0;
    *key = by_str(b, b->keys, u32_at(b->d + e) & 0xffffff), *t = b->d[e + 3], *v = u32_at(b->d + e + 4);
    return *key != NULL;
}

static int by_get(const Byml *b, uint32_t off, const char *key, int *t, uint32_t *v) {
    uint32_t n = by_count(b, off, BY_DICT);
    const char *k;
    for (uint32_t i = 0; i < n; i++)
        if (by_dict(b, off, i, &k, t, v) && !strcmp(k, key)) return 1;
    return 0;
}

static int by_arr(const Byml *b, uint32_t off, uint32_t i, int *t, uint32_t *v) {
    uint32_t n = by_count(b, off, BY_ARR);
    size_t vals = (size_t)off + 4 + ((n + 3) & ~3u);
    if (i >= n || vals + 4 * (size_t)n > b->n) return 0;
    *t = b->d[off + 4 + i], *v = u32_at(b->d + vals + 4 * (size_t)i);
    return 1;
}

static int travel_kind(const char *category) {
    return !strcmp(category, "Shrine") || !strcmp(category, "Tower") || !strcmp(category, "CheckPoint");
}

// Shops share their town's place; a District is a whole region (its point says little).
static int named_kind(const char *category) { return strncmp(category, "Shop", 4) && strcmp(category, "District"); }

static void add_area(const char *rel) {
    size_t n;
    uint8_t *d = read_zs(rel, &n);
    if (!d) return;
    Byml b = {d, n, 0, 0};
    if (n < 16 || memcmp(d, "YB", 2)) {
        rt_log("coop: notices: %s is not a BYML file", rel);
        free(d);
        return;
    }
    b.keys = u32_at(d + 4), b.strs = u32_at(d + 8);
    uint32_t root = u32_at(d + 12), ncat = by_count(&b, root, BY_DICT);
    int before = nplaces;
    for (uint32_t c = 0; c < ncat; c++) {
        const char *cat;
        int t;
        uint32_t list;
        if (!by_dict(&b, root, c, &cat, &t, &list) || t != BY_ARR || !named_kind(cat)) continue;
        uint32_t ne = by_count(&b, list, BY_ARR);
        for (uint32_t e = 0; e < ne; e++) {
            int et, lt, tt;
            uint32_t ent, lv, trans;
            if (!by_arr(&b, list, e, &et, &ent) || et != BY_DICT || !by_get(&b, ent, "LocationName", &lt, &lv) || lt != BY_STR ||
                !by_get(&b, ent, "Trans", &tt, &trans) || tt != BY_ARR)
                continue;
            const char *ln = by_str(&b, b.strs, lv), *base = ln ? strrchr(ln, '/') : NULL;
            base = base ? base + 1 : ln;
            size_t len = base ? strcspn(base, ".") : 0;
            if (!len || len >= sizeof places[0].label) continue;
            uint32_t np = by_count(&b, trans, BY_ARR);
            for (uint32_t k = 0; k < np; k++) {   // one place for each point it is marked at
                int pt, ft;
                uint32_t pv, fv;
                float p[3];
                int ok = by_arr(&b, trans, k, &pt, &pv) && pt == BY_ARR && by_count(&b, pv, BY_ARR) == 3;
                for (uint32_t j = 0; ok && j < 3; j++)
                    if ((ok = by_arr(&b, pv, j, &ft, &fv) && ft == BY_FLOAT)) memcpy(&p[j], &fv, 4);
                if (!ok) continue;
                Place *np2 = realloc(places, (size_t)(nplaces + 1) * sizeof *places);
                if (!np2) break;
                places = np2;
                Place *pl = &places[nplaces++];
                memset(pl, 0, sizeof *pl);
                memcpy(pl->p, p, 12), memcpy(pl->label, base, len), pl->travel = (uint8_t)travel_kind(cat);
            }
        }
    }
    rt_log("coop: notices: %d places from %s", nplaces - before, rel);
    free(d);
}

// ---- MSBT: label -> text
typedef struct { const uint8_t *lbl, *txt; size_t lbl_n, txt_n; } Msbt;

static int msbt_open(const uint8_t *m, size_t n, Msbt *o) {
    memset(o, 0, sizeof *o);
    if (n < 0x20 || memcmp(m, "MsgStdBn", 8)) return 0;
    uint16_t nsec;
    memcpy(&nsec, m + 0xe, 2);
    size_t off = 0x20;
    for (int s = 0; s < nsec && off + 0x10 <= n; s++) {
        uint32_t size = u32_at(m + off + 4);
        size_t b = off + 0x10;
        if (b + size > n) return 0;
        if (!memcmp(m + off, "LBL1", 4)) o->lbl = m + b, o->lbl_n = size;
        if (!memcmp(m + off, "TXT2", 4)) o->txt = m + b, o->txt_n = size;
        off = b + ((size + 15) & ~(size_t)15);
    }
    return o->lbl && o->txt;
}

// The text of `label` as UTF-8 without the message's tags (a Japanese name's ruby, colours); 0 when it is not there.
static int msbt_text(const Msbt *m, const char *label, char *out, size_t cap) {
    if (m->lbl_n < 4) return 0;
    uint32_t groups = u32_at(m->lbl), want = (uint32_t)strlen(label);
    for (uint32_t g = 0; g < groups && 4 + 8 * (size_t)g + 8 <= m->lbl_n; g++) {
        uint32_t cnt = u32_at(m->lbl + 4 + 8 * g), p = u32_at(m->lbl + 8 + 8 * g);
        for (uint32_t k = 0; k < cnt && p < m->lbl_n; k++) {
            uint32_t len = m->lbl[p];
            if (p + 1 + len + 4 > m->lbl_n) return 0;
            if (len == want && !memcmp(m->lbl + p + 1, label, len)) {
                uint32_t idx = u32_at(m->lbl + p + 1 + len), ntxt = m->txt_n >= 4 ? u32_at(m->txt) : 0;
                if (idx >= ntxt || 4 + 4 * (size_t)ntxt > m->txt_n) return 0;
                size_t a = u32_at(m->txt + 4 + 4 * idx), z = idx + 1 < ntxt ? u32_at(m->txt + 8 + 4 * idx) : m->txt_n;
                if (a > z || z > m->txt_n) return 0;
                uint16_t u[128];
                size_t nu = 0;
                for (size_t i = a; i + 2 <= z && nu + 1 < sizeof u / sizeof *u;) {
                    uint16_t ch;
                    memcpy(&ch, m->txt + i, 2);
                    if (!ch) break;
                    if (ch == 0x0e) {   // a tag: group, type, parameter size, parameters
                        uint16_t grp = 0, type = 0, ps = 0, fl = 0;
                        if (i + 8 <= z) memcpy(&grp, m->txt + i + 2, 2), memcpy(&type, m->txt + i + 4, 2), memcpy(&ps, m->txt + i + 6, 2);
                        // group 0xc9 type 10: the name's grammatical forms (Russian, German, ...), each a byte count
                        // and its UTF-16 text; the first is the name as a subject. The whole name can be this tag.
                        if (grp == 0xc9 && type == 10 && ps >= 2 && i + 10 <= z) {
                            memcpy(&fl, m->txt + i + 8, 2);
                            for (size_t j = 0; j + 2 <= fl && j + 2 <= (size_t)ps - 2 && i + 10 + j + 2 <= z && nu + 1 < sizeof u / sizeof *u; j += 2)
                                memcpy(&u[nu++], m->txt + i + 10 + j, 2);
                        }
                        i += 8 + ps;
                        continue;
                    }
                    if (ch == 0x0f) {   // a closing tag: group, type
                        i += 6;
                        continue;
                    }
                    u[nu++] = ch, i += 2;
                }
                u[nu] = 0;
                return utf16_to_utf8(u, nu, out, cap) > 0;
            }
            p += 1 + len + 4;
        }
    }
    return 0;
}

// The Mals language file of a Switch language code (ov_language).
static const char *mals_code(const char *lang) {
    static const struct { const char *sw, *mals; } map[] = {
        {"en-US", "USen"}, {"en-GB", "EUen"}, {"ja", "JPja"}, {"fr", "EUfr"}, {"fr-CA", "USfr"}, {"de", "EUde"},
        {"it", "EUit"}, {"es", "EUes"}, {"es-419", "USes"}, {"pt-BR", "USpt"}, {"nl", "EUnl"}, {"ru", "EUru"},
        {"zh-CN", "CNzh"}, {"zh-Hans", "CNzh"}, {"zh-TW", "TWzh"}, {"zh-Hant", "TWzh"}, {"ko", "KRko"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (lang && !strcmp(lang, map[i].sw)) return map[i].mals;
    return "USen";
}

static void name_places(void) {
    const char *code = mals_code(ov_language());
    char prefix[32], mals[256] = "";
    snprintf(prefix, sizeof prefix, "Mals/%s.Product.", code);
    for (uint64_t i = 0, n = romfs_file_count(); i < n; i++) {   // the newest Product.<n> of that language
        const char *p = romfs_entry(i, NULL, NULL);
        size_t pl = p ? strlen(p) : 0;
        if (p && !strncmp(p, prefix, strlen(prefix)) && pl > 8 && !strcmp(p + pl - 8, ".sarc.zs") &&
            (!mals[0] || atoi(p + strlen(prefix)) > atoi(mals + strlen(prefix))))
            snprintf(mals, sizeof mals, "%s", p);
    }
    size_t n, la, ls, da, ds;
    uint8_t *sarc = mals[0] ? read_zs(mals, &n) : NULL;
    Msbt loc, dun;
    if (!sarc || !sarc_member(sarc, n, "LocationMsg/Location.msbt", &la, &ls) || !msbt_open(sarc + la, ls, &loc)) {
        rt_log("coop: notices: no LocationMsg/Location.msbt in %s; places go by their labels", mals[0] ? mals : prefix);
        free(sarc);
        return;
    }
    int have_dun = sarc_member(sarc, n, "LocationMsg/Dungeon.msbt", &da, &ds) && msbt_open(sarc + da, ds, &dun);
    int named = 0;
    for (int i = 0; i < nplaces; i++) {
        Place *p = &places[i];
        unsigned num;
        char lab[8];
        int shrine = sscanf(p->label, "Dungeon%3u", &num) == 1 && strlen(p->label) == 10;
        if (shrine && have_dun) snprintf(lab, sizeof lab, "%04u", num);
        if ((shrine && have_dun && msbt_text(&dun, lab, p->name, sizeof p->name)) || msbt_text(&loc, p->label, p->name, sizeof p->name))
            named++;
    }
    rt_log("coop: notices: %d of %d places named from %s", named, nplaces, mals);
    free(sarc);
}

static void load_places(void *arg) {
    (void)arg;
    size_t n;
    uint8_t *z = romfs_read_whole("Pack/ZsDic.pack.zs", &n, "co-op notices name no places");
    unsigned long long dn = z ? ZSTD_getFrameContentSize(z, n) : 0;
    if (z && dn != ZSTD_CONTENTSIZE_UNKNOWN && dn != ZSTD_CONTENTSIZE_ERROR && dn < (64u << 20) && (dicts = malloc((size_t)dn)) &&
        ZSTD_decompress(dicts, (size_t)dn, z, n) == dn)
        dicts_n = (size_t)dn;
    free(z);
    if (!dicts_n) {
        rt_log("coop: notices: no Pack/ZsDic.pack.zs; no place names");
        return;
    }
    add_area("Banc/MainField/LocationArea/MainField.locationarea.byml.zs");
    add_area("Banc/MinusField/LocationArea/MinusField.locationarea.byml.zs");
    name_places();
    free(dicts), dicts = NULL, dicts_n = 0;
}

static void places_up(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, load_places, NULL);
}

void coop_notices_glyphs(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx) {
    if (!coop_notices_on() || !lang || strcmp(lang, ui_text_lang(ov_language()))) return;
    places_up();
    for (int i = 0; i < nplaces; i++)
        if (places[i].name[0]) visit(ctx, places[i].name);
}

static float dist2(const float a[3], const float b[3]) {
    return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
}

// The nearest place of the kind asked for within r m of p, or NULL.
static const Place *nearest(const float p[3], int travel_only, float r) {
    const Place *best = NULL;
    float bd = r * r;
    for (int i = 0; i < nplaces; i++) {
        if (travel_only && !places[i].travel) continue;
        float d = dist2(p, places[i].p);
        if (d < bd) bd = d, best = &places[i];
    }
    return best;
}

static const char *place_name(const char *label) {
    for (int i = 0; i < nplaces; i++)
        if (!strcmp(places[i].label, label)) return places[i].name[0] ? places[i].name : places[i].label;
    return NULL;
}

// ---- fast travel: this game's Link (game thread)
#define TRAVEL_JUMP_M 250.0f
#define TRAVEL_POINT_M 40.0f
#define TRAVEL_NEAR_M 600.0f
#define TRAVEL_SETTLE_US 1500000

static struct { CoopTravelPacket p; int left; int64_t next_us; } out;   // the last travel, sent three times

static void travel_test(Cpu *c, uint64_t me, int64_t now) {
    // TOTK_COOP_TEST_TRAVEL=<s>[,x,y,z]: at that world time this Link is put at x y z, as a fast travel would put it,
    // and it is told as one. The default is 5 m from Ukouh Shrine's marker (274.9 1579.3 912.7) on the Great Sky
    // Island, 640 m from where the 100% save ("In-isa Shrine") starts.
    static int done;
    const char *v = cfg_get("TOTK_COOP_TEST_TRAVEL");
    if (done || !v || now < (int64_t)(atof(v) * 1e6)) return;
    done = 1;
    float m[12] = {1, 0, 0, 279.0f, 0, 1, 0, 1582.0f, 0, 0, 1, 915.0f};
    const char *xyz = strchr(v, ',');
    if (xyz) sscanf(xyz + 1, "%f,%f,%f", &m[3], &m[7], &m[11]);
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, me, (uint64_t)(uintptr_t)m, 0, 0);
    rt_log("coop: test: travel: this Link put at %.1f %.1f %.1f", m[3], m[7], m[11]);
}

static void places_thread(void *arg) { (void)arg, places_up(); }

void coop_notices_tick(Cpu *c, uint64_t me) {
    if (!coop_notices_on()) return;
    static int started;   // the places load on a thread of their own (about 2 MB read and unpacked), not in a game tick
    if (!started) started = 1, plat_thread_spawn(places_thread, NULL, "coop-places", 0);
    int64_t now = frame_step_world_us();
    travel_test(c, me, now);
    if (out.left && now >= out.next_us && coop_net_id()) {
        out.left--, out.next_us = now + 400000;
        coop_net_hit_send(&out.p, sizeof out.p);
    }
    static float last_p[3], from_p[3];
    static int have;
    static int64_t settle_us = -1, dead_us = -1, last_now;
    if (now < last_now) have = 0, settle_us = -1, dead_us = -1;   // a save loaded: the world clock began again
    last_now = now;
    float p[3];
    if (!coop_rd(me + COOP_ACTOR_POS, p, 12)) return;
    if (coop_life_of(me) == 0) dead_us = now;
    if (settle_us >= 0) {
        if (now < settle_us) return;
        settle_us = -1;
        memcpy(last_p, p, 12);
        places_up();
        const Place *at = nearest(p, 1, TRAVEL_POINT_M), *nr = at ? NULL : nearest(p, 0, TRAVEL_NEAR_M);
        rt_log("coop: notices: this Link moved %.0f m in a tick to %.1f %.1f %.1f: %s %s", sqrtf(dist2(p, from_p)), p[0], p[1],
               p[2], at ? "travel point" : nr ? "near" : "no place", at ? at->label : nr ? nr->label : "");
        if (!at && !nr) return;
        static uint32_t seq;
        memset(&out.p, 0, sizeof out.p);
        out.p.kind = COOP_TRAVEL_KIND, out.p.seq = ++seq, out.p.near = !at;
        memcpy(out.p.pos, p, 12);
        snprintf(out.p.label, sizeof out.p.label, "%s", at ? at->label : nr->label);
        out.left = 3, out.next_us = now;
        return;
    }
    if (have && dist2(p, last_p) > TRAVEL_JUMP_M * TRAVEL_JUMP_M) {
        // a respawn after a death is not a travel: life read 0 in the 20 s before the jump
        if (dead_us >= 0 && now - dead_us < 20000000) rt_log("coop: notices: this Link moved %.0f m after its death: not a travel", sqrtf(dist2(p, last_p)));
        else settle_us = now + TRAVEL_SETTLE_US, memcpy(from_p, last_p, 12);
    }
    memcpy(last_p, p, 12), have = 1;
}

// ---- fast travel: another player's (game thread, coop_world.c peer_tick)
void coop_notice_travel_take(const CoopTravelPacket *p, int len, int from) {
    if (len != (int)sizeof *p || p->kind != COOP_TRAVEL_KIND || from < 1 || from > COOP_MAX_PLAYERS || !coop_notices_on()) return;
    // The second and third copies come within a second of the first. Only then is the same seq a copy: a player who
    // left and came back (or a new one with that id) counts from 1 again.
    static uint32_t told[COOP_MAX_PLAYERS + 1];
    static uint64_t told_ns[COOP_MAX_PLAYERS + 1];
    uint64_t now = plat_now_ns();
    if (p->seq == told[from] && now - told_ns[from] < 5000000000ull) return;
    told[from] = p->seq, told_ns[from] = now;
    char label[sizeof p->label + 1];
    memcpy(label, p->label, sizeof p->label), label[sizeof p->label] = 0;
    places_up();
    const char *where = label[0] ? place_name(label) : NULL;
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17], buf[24];
    const char *who = NULL;
    int n = coop_net_players(ids, names, COOP_MAX_PLAYERS);
    for (int k = 0; k < n; k++)
        if (ids[k] == from) who = names[k];
    who = name_or(who, from, buf, sizeof buf);
    if (!where) notice(menu_text(UI_MENU_COOP_NOTICE_TRAVELLED_SOMEWHERE), who);
    else notice(menu_text(p->near ? UI_MENU_COOP_NOTICE_NEAR : UI_MENU_COOP_NOTICE_TRAVELLED), who, where);
}
