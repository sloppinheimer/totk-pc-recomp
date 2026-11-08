// Character mods in co-op (docs/coop/character-mods.md). tools/coop/charmod.py gives every character mod in the mods
// folder (and the game's own Link) a "set": a stand-in actor PlayerPuppet_<tag> and bare head and body pieces under
// names of their own, in the mod mods/zz-coop-characters, listed in its characters.tsv. Here:
// - this PC's character: the switched-on character mod (its files are served as Link's own, as any mod), else Link;
// - each other player's: the key their states carry (NetState.character), whose stand-in is spawned as that set's actor;
// - a party member's character this PC has no set for: GameBanana's details of it, a prompt in the Co-op tab, and on a
//   yes the download (charmod.py fetch: MD5 checked, RomFS files only, no code patches, no path out of the folder) and
//   the sets rebuilt. A new set is served from the next start (the RomFS overlay is read at start).
#include "coop_chars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_net.h"
#include "mods.h"
#include "restbl.h"
#include "platform/platform.h"
#include "fs.h"
#include "host.h"

#define MAX_CHARS 64
static CoopChar chars[MAX_CHARS];
static int nchars, local_at = -1, inited;
static const CoopChar link_default = {.key = "Link", .actor = "PlayerPuppet", .name = "Link"};

static char *field(char **p) {   // the next tab-separated field of a line, in place
    char *s = *p, *t = strchr(s, '\t');
    if (t) *t = 0, *p = t + 1;
    else *p = s + strlen(s);
    return s;
}

void coop_chars_init(void) {
    if (inited) return;
    inited = 1;
    const char *md = mods_dir();
    if (!md || !*md) return;
    char path[2300];
    snprintf(path, sizeof path, "%s/zz-coop-characters/characters.tsv", md);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (!text) { rt_log("coop: characters: no %s (tools/coop/charmod.py sets makes it)", path); return; }
    char *buf = realloc(text, size + 1);
    if (!buf) { free(text); return; }
    buf[size] = 0;
    for (char *line = strtok(buf, "\r\n"); line && nchars < MAX_CHARS; line = strtok(NULL, "\r\n")) {
        CoopChar *c = &chars[nchars];
        char *p = line;
        snprintf(c->key, sizeof c->key, "%s", field(&p));
        snprintf(c->tag, sizeof c->tag, "%s", field(&p));
        snprintf(c->actor, sizeof c->actor, "%s", field(&p));
        snprintf(c->folder, sizeof c->folder, "%s", field(&p));
        snprintf(c->name, sizeof c->name, "%s", field(&p));
        snprintf(c->page, sizeof c->page, "%s", field(&p));
        c->mod_id = (uint32_t)strtoul(field(&p), NULL, 10);
        c->file_id = (uint32_t)strtoul(field(&p), NULL, 10);
        for (int k = 0; k < 3; k++) snprintf(c->bare[k], sizeof c->bare[k], "%s", field(&p));
        if (c->key[0] && c->actor[0]) nchars++;
    }
    free(buf);
    // This PC's character: the first switched-on mod in the order that is a character's.
    for (int i = 0; i < mods_count() && local_at < 0; i++) {
        const ModInfo *m = mods_at(i);
        if (!m || !m->enabled || m->problem != MOD_OK) continue;
        for (int k = 0; k < nchars; k++)
            if (chars[k].folder[0] && !strcmp(chars[k].folder, m->name)) { local_at = k; break; }
    }
    if (local_at < 0)
        for (int k = 0; k < nchars; k++) if (!strcmp(chars[k].key, "Link")) local_at = k;
    for (int k = 0; k < nchars; k++)
        rt_log("coop: characters: %s (%s): stand-in %s%s", chars[k].name, chars[k].key, chars[k].actor,
               k == local_at ? ", this PC's Link" : "");
}

const CoopChar *coop_char_local(void) { coop_chars_init(); return local_at >= 0 ? &chars[local_at] : &link_default; }
int coop_char_count(void) { coop_chars_init(); return nchars; }
const CoopChar *coop_char_at(int i) { return i >= 0 && i < nchars ? &chars[i] : NULL; }

const CoopChar *coop_char_find(const char *key) {
    coop_chars_init();
    if (!key || !*key) return NULL;
    for (int k = 0; k < nchars; k++) if (!strcmp(chars[k].key, key)) return &chars[k];
    return NULL;
}

const char *coop_char_actor(const char *key) {
    const CoopChar *c = coop_char_find(key && *key ? key : "Link");
    return c ? c->actor : "PlayerPuppet";
}

const char *coop_char_bare(const char *key, int k) {
    const CoopChar *c = coop_char_find(key && *key ? key : "Link");
    if (!c || k < 0 || k > 2 || !c->bare[k][0] || !strcmp(c->key, coop_char_local()->key)) return NULL;
    return c->bare[k];
}

int coop_char_is_standin(const char *name) {
    return name && !strncmp(name, "PlayerPuppet", 12) && (!name[12] || name[12] == '_');
}

// ---- the party
static struct { char key[COOP_CHAR_KEY]; uint32_t mod_id; } party[COOP_MAX_PLAYERS + 1];
static PlatMutex lock;   // (zero storage is a valid lock)
static void take(void) { plat_mutex_lock(&lock); }
static void give(void) { plat_mutex_unlock(&lock); }

void coop_chars_seen(int id, const char *key, uint32_t mod_id) {
    if (id < 1 || id > COOP_MAX_PLAYERS) return;
    take();
    if (strncmp(party[id].key, key, COOP_CHAR_KEY)) {
        snprintf(party[id].key, sizeof party[id].key, "%.*s", COOP_CHAR_KEY - 1, key);
        rt_log("coop: characters: player %d is %s%s", id, party[id].key[0] ? party[id].key : "Link",
               coop_char_find(party[id].key[0] ? party[id].key : "Link") ? "" : " (no set here)");
    }
    party[id].mod_id = mod_id;
    give();
}

const char *coop_chars_of(int id) { return id >= 1 && id <= COOP_MAX_PLAYERS ? party[id].key : ""; }

// ---- jobs: charmod.py, detached, writing key<TAB>value lines to a status file (its --status) as it goes. The job, the
// ask and the declined list are under lock: coop_chars_tick (the game thread) polls the job every frame, the Co-op tab
// (the menu) reads and answers.
static CoopJob job;
static char job_status[2300];
static uint64_t job_read_ns;
static struct { char k[24], v[256]; } job_kv[24];
static int job_nkv;

static const char *kv(const char *k) {
    for (int i = 0; i < job_nkv; i++) if (!strcmp(job_kv[i].k, k)) return job_kv[i].v;
    return "";
}

static int root_dir(char *out, size_t cap) {   // the checkout: build/<version>/totk.exe's grandparent
    char exe[2048];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    for (int up = 0; up < 3; up++) {
        char *a = strrchr(exe, '/'), *b = strrchr(exe, '\\');
        if (b > a) a = b;
        if (!a) return 0;
        *a = 0;
    }
    return snprintf(out, cap, "%s", exe) < (int)cap;
}

static void cut_name(char *path) {   // path without its last part
    char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > a) a = b;
    if (a) *a = 0;
}

static int job_start(const char *what, const char *const *args) {
    if (job.state == COOP_JOB_RUNNING) return 0;
    static char root[2048], py[2300], tool[2300], romfs[2300], exe[2048];
    if (!root_dir(root, sizeof root) || !plat_exe_path(exe, sizeof exe)) return 0;
    const char *e = cfg_get("TOTK_COOP_PYTHON");
    if (e && *e) snprintf(py, sizeof py, "%s", e);
    else snprintf(py, sizeof py, "%s/.venv/Scripts/python.exe", root);
    e = cfg_get("TOTK_COOP_CHARTOOL");
    if (e && *e) snprintf(tool, sizeof tool, "%s", e);
    else snprintf(tool, sizeof tool, "%s/tools/coop/charmod.py", root);
    const char *img = romfs_image_path();
    romfs[0] = 0;
    if (img && *img) snprintf(romfs, sizeof romfs, "%s", img), cut_name(romfs);
    cut_name(exe);
    snprintf(job_status, sizeof job_status, "%s/logs/charmod-status.txt", exe);
    remove(job_status);
    const char *argv[48];
    int n = 0;
    argv[n++] = py, argv[n++] = "-I", argv[n++] = tool;
    for (int i = 0; args[i] && n < 36; i++) argv[n++] = args[i];
    argv[n++] = "--status", argv[n++] = job_status, argv[n++] = "--mods", argv[n++] = mods_dir();
    if (romfs[0]) argv[n++] = "--romfs", argv[n++] = romfs;
    // The RomFS overlay next to totk.exe: the actor tables with PlayerPuppet's row, which the sets build on (the mods
    // folder may be elsewhere, TOTK_MODS_DIR).
    static char overlay[2300];
    snprintf(overlay, sizeof overlay, "%s/romfs-patches", exe);
    if (plat_dir_exists(overlay)) argv[n++] = "--overlay", argv[n++] = overlay;
    argv[n] = NULL;
    memset(&job, 0, sizeof job);
    snprintf(job.what, sizeof job.what, "%s", what);
    job_nkv = 0;
    if (!plat_spawn(argv, root)) {
        job.state = COOP_JOB_FAILED;
        snprintf(job.text, sizeof job.text, "could not start %s", py);
        rt_log("coop: characters: %s", job.text);
        return 0;
    }
    job.state = COOP_JOB_RUNNING;
    job_read_ns = 0;
    char line[4096];
    size_t at = 0;
    for (int i = 0; argv[i] && at < sizeof line - 1; i++) at += (size_t)snprintf(line + at, sizeof line - at, "%s\"%s\"", i ? " " : "", argv[i]);
    rt_log("coop: characters: %s started: %s", what, line);
    return 1;
}

static void job_poll(void) {
    if (job.state != COOP_JOB_RUNNING) return;
    uint64_t now = plat_now_ns();
    if (now - job_read_ns < 250000000ull) return;
    job_read_ns = now;
    uint64_t size = 0;
    char *t = (char *)plat_file_read(job_status, &size);
    if (!t) return;
    char *buf = realloc(t, size + 1);
    if (!buf) { free(t); return; }
    buf[size] = 0;
    job_nkv = 0;
    for (char *line = strtok(buf, "\r\n"); line && job_nkv < 24; line = strtok(NULL, "\r\n")) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        snprintf(job_kv[job_nkv].k, sizeof job_kv[job_nkv].k, "%s", line);
        snprintf(job_kv[job_nkv].v, sizeof job_kv[job_nkv].v, "%s", tab + 1);
        job_nkv++;
    }
    free(buf);
    job.got = strtoull(kv("got"), NULL, 10), job.size = strtoull(kv("size"), NULL, 10);
    snprintf(job.text, sizeof job.text, "%s", kv("text"));
    const char *st = kv("state");
    if (!strcmp(st, "done")) job.state = COOP_JOB_DONE;
    else if (!strcmp(st, "failed")) job.state = COOP_JOB_FAILED;
    if (job.state != COOP_JOB_RUNNING) rt_log("coop: characters: %s %s: %s", job.what, st, job.text);
}

// ---- asking
static int slot_next(char *tag, size_t tn, char *dir, size_t dn);   // (below: the stand-in slots)
static int live_pending;
static char live_dir[2300];
static CoopCharAsk ask;
static int asking, restart_needed;
static char declined[16][COOP_CHAR_KEY];
static int ndeclined;
static char asked_for[COOP_CHAR_KEY];   // the key the current ask is about ("" a lookup typed into the menu)

static int is_declined(const char *key) {
    for (int i = 0; i < ndeclined; i++) if (!strcmp(declined[i], key)) return 1;
    return 0;
}
static void decline(const char *key) {
    if (key[0] && ndeclined < 16 && !is_declined(key)) snprintf(declined[ndeclined++], COOP_CHAR_KEY, "%s", key);
}

static void info_for(uint32_t mod_id, uint32_t file_id) {
    char m[16], f[16];
    snprintf(m, sizeof m, "%u", mod_id);
    snprintf(f, sizeof f, "%u", file_id);
    const char *args[] = {"info", m, "--file", file_id ? f : "auto", NULL};
    job_start("info", args);
}

static void answer(int yes) {   // under lock
    if (!asking) return;
    asking = 0;
    if (!yes || !ask.ready) {
        decline(asked_for);
        rt_log("coop: characters: %s not downloaded (the player said no)", ask.name[0] ? ask.name : asked_for);
        return;
    }
    char m[16], f[16];
    snprintf(m, sizeof m, "%u", ask.mod_id);
    snprintf(f, sizeof f, "%u", ask.file_id);
    // TOTK_COOP_CHARFETCH_FILE (a test): that file on disk in place of GameBanana's download, its MD5 checked the same
    // way (charmod.py fetch --from-file); no download is made.
    const char *local = cfg_get("TOTK_COOP_CHARFETCH_FILE");
    // Its set in a free stand-in slot too (charmod.py fetch --slot), added when the job is done: no restart.
    char tag[8] = "";
    int slot = slot_next(tag, sizeof tag, live_dir, sizeof live_dir);
    const char *args[16];   // (12 arguments at most, and the NULL)
    int na = 0;
    args[na++] = "fetch", args[na++] = f, args[na++] = "--mod", args[na++] = m, args[na++] = "--yes", args[na++] = "--sets";
    if (slot) args[na++] = "--slot", args[na++] = tag, args[na++] = "--slot-out", args[na++] = live_dir;
    if (local && *local) args[na++] = "--from-file", args[na++] = local;
    args[na] = NULL;
    rt_log("coop: characters: %s %s (%s, %s)", local && *local ? "fetching from a local copy" : "downloading", ask.name,
           ask.file, ask.page);
    decline(asked_for);   // asked once this run: a failed fetch is not offered again until the next start
    if (job_start("fetch", args)) restart_needed = 1, live_pending = slot;
}

static void lookup(const char *link) {   // under lock
    if (job.state == COOP_JOB_RUNNING || asking) return;
    const char *p = strstr(link, "mods/");
    p = p ? p + 5 : link;
    while (*p && (*p < '0' || *p > '9')) p++;
    uint32_t id = (uint32_t)strtoul(p, NULL, 10);
    if (!id) {
        memset(&job, 0, sizeof job);
        job.state = COOP_JOB_FAILED;
        snprintf(job.what, sizeof job.what, "info");
        snprintf(job.text, sizeof job.text, "not a GameBanana mod link or id: %s", link);
        return;
    }
    memset(&ask, 0, sizeof ask);
    ask.mod_id = id;
    snprintf(ask.page, sizeof ask.page, "https://gamebanana.com/mods/%u", id);
    asked_for[0] = 0;
    asking = 1;
    rt_log("coop: characters: looking up GameBanana mod %u", id);
    info_for(id, 0);
}

// Every frame (coop_players.c): the job read, a party member's missing character asked about, the details filled in.

// ---- a character added while the game runs: a stand-in slot (charmod.py SLOT_TAGS; their actors' table rows are in
// the sets from the start) filled with a fetched character's set (charmod.py slot), its files added to the RomFS
// (romfs.c romfs_live_add) and their entries to the game's resource size table, which the game keeps in its heap as
// the file is laid out and reads on every load (a too small entry written there made it refuse the stand-in, 2026-11-08).
static char slot_tags[8][8];
static int nslots, slots_used;
static int live_loaded;   // a character was added this run (no restart needed for it)

static void slots_init(void) {
    static int done;
    if (done) return;
    done = 1;
    char path[2300];
    snprintf(path, sizeof path, "%s/zz-coop-characters/slots.tsv", mods_dir());
    uint64_t size = 0;
    char *t = (char *)plat_file_read(path, &size);
    if (!t) return;
    for (uint64_t i = 0, s = 0; i <= size && nslots < 8; i++)
        if (i == size || t[i] == '\n' || t[i] == '\r') {
            if (i > s && i - s < sizeof slot_tags[0]) memcpy(slot_tags[nslots], t + s, i - s), slot_tags[nslots++][i - s] = 0;
            s = i + 1;
        }
    free(t);
}

// The next stand-in slot's tag and its folder, or 0 when all are used this run (each once: the game may keep a pack it
// loaded under a name).
static int slot_next(char *tag, size_t tn, char *dir, size_t dn) {
    slots_init();
    if (slots_used >= nslots) return 0;
    char exe[2048];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    cut_name(exe);
    snprintf(tag, tn, "%s", slot_tags[slots_used++]);
    snprintf(dir, dn, "%s/coop-live/%s", exe, tag);
    return 1;
}

// The game's copy of the resource size table: a "RESTBL" header in its heap whose hashed entries end with the spares
// romfs.c adds (hash 0xffffffff). Found once.
static uint64_t gtable;
static uint32_t gnh;
static int restbl_guest(void) {
    if (gtable) return 1;
    const RtGuestLayout *L = mem_layout();
    RtRange rs[2] = {L->heap, L->alias};
    static uint8_t buf[1 << 16];
    for (int r = 0; r < 2 && !gtable; r++)
        for (uint64_t a = rs[r].base; a + sizeof buf <= rs[r].base + rs[r].size && !gtable; a += sizeof buf - 32) {
            if (!plat_mem_read(a, buf, sizeof buf)) continue;
            for (size_t i = 0; i + 22 <= sizeof buf; i++) {
                if (buf[i] != 'R' || memcmp(buf + i, "RESTBL", 6)) continue;
                uint32_t w[4], last[2] = {0, 0};
                memcpy(w, buf + i + 6, 16);
                if (w[0] != 1 || w[2] < 1000 || !plat_mem_read(a + i + 22 + 8ull * (w[2] - 1), last, 8)) continue;
                if (last[0] != 0xffffffffu) continue;
                gtable = a + i, gnh = w[2];
                rt_log("coop: characters: the game's resource size table at %#llx, %u hashed entries",
                       (unsigned long long)gtable, gnh);
                break;
            }
        }
    if (!gtable) rt_log("coop: characters: the game's resource size table was not found (no spares in it?)");
    return gtable != 0;
}

typedef struct { uint32_t (*e)[2]; size_t n, cap; } Entries;
static void entry_cb(uint32_t hash, uint32_t size, void *ctx) {
    Entries *v = ctx;
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        void *g = realloc(v->e, cap * sizeof *v->e);
        if (!g) return;
        v->e = g, v->cap = cap;
    }
    v->e[v->n][0] = hash, v->e[v->n][1] = size, v->n++;
}
static int by_hash(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return (x > y) - (x < y);
}

// The entries of v merged into the game's table: a hash it has is raised (never lowered), a new one takes a spare.
// Written back in one copy. The number added; *raised the number raised.
static int restbl_guest_add(Entries *v, int *raised) {
    *raised = 0;
    if (!v->n || !restbl_guest()) return 0;
    qsort(v->e, v->n, sizeof *v->e, by_hash);
    size_t m = 0;   // the same hash twice: the larger entry
    for (size_t j = 0; j < v->n; j++) {
        if (m && v->e[m - 1][0] == v->e[j][0]) {
            if (v->e[m - 1][1] < v->e[j][1]) v->e[m - 1][1] = v->e[j][1];
        } else v->e[m][0] = v->e[j][0], v->e[m][1] = v->e[j][1], m++;
    }
    v->n = m;
    uint32_t (*a)[2] = malloc((size_t)gnh * 8), (*b)[2] = malloc((size_t)gnh * 8);
    if (!a || !b || !plat_mem_read(gtable + 22, a, (size_t)gnh * 8)) {
        free(a), free(b);
        return 0;
    }
    size_t real = gnh;
    while (real && a[real - 1][0] == 0xffffffffu && !a[real - 1][1]) real--;
    size_t spares = gnh - real, i = 0, j = 0, o = 0;
    int added = 0;
    while (i < real || j < v->n) {
        if (j >= v->n || (i < real && a[i][0] < v->e[j][0])) {
            b[o][0] = a[i][0], b[o][1] = a[i][1], o++, i++;
        } else if (i < real && a[i][0] == v->e[j][0]) {
            b[o][0] = a[i][0], b[o][1] = a[i][1] > v->e[j][1] ? a[i][1] : v->e[j][1];
            *raised += b[o][1] != a[i][1];
            o++, i++, j++;
        } else {
            if (spares) b[o][0] = v->e[j][0], b[o][1] = v->e[j][1], o++, spares--, added++;
            j++;
        }
    }
    while (o < gnh) b[o][0] = 0xffffffffu, b[o][1] = 0, o++;
    memcpy((void *)(uintptr_t)(gtable + 22), b, (size_t)gnh * 8);
    free(a), free(b);
    rt_log("coop: characters: resource size table: %d entries added, %d raised, %zu spares left", added, *raised, spares);
    return added;
}

// The slot folder dir (charmod.py slot): its files into the RomFS, their entries into the game's table, its character
// into the list. Under lock, on the game thread.
static void live_load(const char *dir) {
    char romfs[2300], path[2300];
    snprintf(romfs, sizeof romfs, "%s/romfs", dir);
    Entries v = {0};
    int files = romfs_live_add(romfs, entry_cb, &v), raised = 0;
    int added = restbl_guest_add(&v, &raised);
    free(v.e);
    snprintf(path, sizeof path, "%s/slot.tsv", dir);
    uint64_t size = 0;
    char *t = (char *)plat_file_read(path, &size);
    if (files < 0 || !t || nchars >= MAX_CHARS) {
        rt_log("coop: characters: %s could not be added (%d files)", dir, files);
        free(t);
        return;
    }
    char *buf = realloc(t, size + 1);
    if (!buf) { free(t); return; }
    buf[size] = 0;
    char *line = strtok(buf, "\r\n"), *p = line;
    CoopChar c = {0};
    if (p) {
        snprintf(c.key, sizeof c.key, "%s", field(&p));
        snprintf(c.tag, sizeof c.tag, "%s", field(&p));
        snprintf(c.actor, sizeof c.actor, "%s", field(&p));
        snprintf(c.folder, sizeof c.folder, "%s", field(&p));
        snprintf(c.name, sizeof c.name, "%s", field(&p));
        snprintf(c.page, sizeof c.page, "%s", field(&p));
        c.mod_id = (uint32_t)strtoul(field(&p), NULL, 10);
        c.file_id = (uint32_t)strtoul(field(&p), NULL, 10);
        for (int k = 0; k < 3; k++) snprintf(c.bare[k], sizeof c.bare[k], "%s", field(&p));
    }
    free(buf);
    if (!c.key[0] || !c.actor[0]) return;
    // A key this PC had no set for at start, or one it had: the slot's set is used from now on.
    CoopChar *old = (CoopChar *)coop_char_find(c.key);
    if (old) *old = c;
    else chars[nchars] = c, __atomic_store_n(&nchars, nchars + 1, __ATOMIC_RELEASE);
    live_loaded = 1;
    rt_log("coop: characters: %s (%s) added while the game runs: stand-in %s, %d files, %d table entries", c.name,
           c.key, c.actor, files, added);
}

int coop_chars_live_loaded(void) { return live_loaded; }

static int live_pending;   // the running job makes a slot folder (live_dir) to add when it is done
static char live_dir[2300];

void coop_chars_tick(void) {
    coop_chars_init();
    static unsigned ticks;
    ticks++;
    take();
    job_poll();
    if (live_pending && job.state != COOP_JOB_RUNNING) {
        live_pending = 0;
        if (job.state == COOP_JOB_DONE) live_load(live_dir);
    }
    if (asking && asked_for[0] && coop_char_find(asked_for)) asking = 0;   // that character came in another way
    {   // TOTK_COOP_TEST_CHARLIVE=<character mod folder> (a test): 10 s into the world, that mod's set made in a free
        // slot (charmod.py slot) and added while the game runs, as after a fetch
        static int done;
        const char *t = done || ticks < 600 ? NULL : cfg_get("TOTK_COOP_TEST_CHARLIVE");
        char tag[8];
        if (t && *t && job.state != COOP_JOB_RUNNING && (done = 1) && slot_next(tag, sizeof tag, live_dir, sizeof live_dir)) {
            const char *args[] = {"slot", t, "--tag", tag, "--out", live_dir, NULL};
            if (job_start("slot", args)) live_pending = 1;
        }
    }
    {   // TOTK_COOP_TEST_CHARLOOKUP=<link or id> (a test): that lookup once, as the Co-op tab's button does
        static int done;
        const char *t = done ? NULL : cfg_get("TOTK_COOP_TEST_CHARLOOKUP");
        if (t && *t && job.state != COOP_JOB_RUNNING && !asking) done = 1, lookup(t);
    }
    for (int id = 1; id <= COOP_MAX_PLAYERS && !asking && job.state != COOP_JOB_RUNNING; id++) {
        const char *key = party[id].key;
        if (strncmp(key, "gb:", 3) || coop_char_find(key) || is_declined(key) || !party[id].mod_id) continue;
        memset(&ask, 0, sizeof ask);
        ask.player_id = id, ask.mod_id = party[id].mod_id, ask.file_id = (uint32_t)strtoul(key + 3, NULL, 10);
        snprintf(asked_for, sizeof asked_for, "%s", key);
        snprintf(ask.page, sizeof ask.page, "https://gamebanana.com/mods/%u", ask.mod_id);
        asking = 1;
        info_for(ask.mod_id, ask.file_id);
    }
    if (asking && !ask.ready && !strcmp(job.what, "info") && job.state != COOP_JOB_RUNNING) {
        if (job.state == COOP_JOB_FAILED) decline(asked_for), asking = 0;   // GameBanana said no, or no network
        else {
            snprintf(ask.name, sizeof ask.name, "%s", kv("name"));
            snprintf(ask.page, sizeof ask.page, "%s", kv("page"));
            snprintf(ask.file, sizeof ask.file, "%s", kv("file"));
            ask.size = strtoull(kv("size"), NULL, 10);
            ask.file_id = (uint32_t)strtoul(kv("file_id"), NULL, 10);
            ask.mod_id = (uint32_t)strtoul(kv("mod_id"), NULL, 10);
            ask.ready = 1;
            rt_log("coop: characters: asking whether to download %s (%s, %llu bytes, %s)", ask.name, ask.file,
                   (unsigned long long)ask.size, ask.page);
            // TOTK_COOP_TEST_CHARYES=1 (a test): yes, as if the player pressed Download
            const char *y = cfg_get("TOTK_COOP_TEST_CHARYES");
            if (y && *y == '1') rt_log("coop: characters: test: answered yes"), answer(1);
        }
    }
    give();
}

void coop_chars_job(CoopJob *out) { take(); *out = job; give(); }

int coop_chars_ask(CoopCharAsk *out) {
    take();
    int a = asking;
    if (a) *out = ask;
    give();
    return a;
}

void coop_chars_answer(int yes) { take(); answer(yes); give(); }
void coop_chars_lookup(const char *link) { take(); lookup(link); give(); }

int coop_chars_restart_needed(void) {
    take();
    int r = restart_needed && job.state == COOP_JOB_DONE && !strcmp(job.what, "fetch") && !live_loaded;
    give();
    return r;
}
