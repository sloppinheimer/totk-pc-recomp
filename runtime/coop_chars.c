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

// ---- jobs: charmod.py, detached, writing key<TAB>value lines to a status file (its --status) as it goes
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

static int job_start(const char *what, const char *const *args) {
    if (job.state == COOP_JOB_RUNNING) return 0;
    char root[2048], py[2300], tool[2300], romfs[2300] = "", logs[2300];
    if (!root_dir(root, sizeof root)) return 0;
    const char *e = cfg_get("TOTK_COOP_PYTHON");
    snprintf(py, sizeof py, "%s", e && *e ? e : "");
    if (!py[0]) snprintf(py, sizeof py, "%s/.venv/Scripts/python.exe", root);
    e = cfg_get("TOTK_COOP_CHARTOOL");
    snprintf(tool, sizeof tool, "%s", e && *e ? e : "");
    if (!tool[0]) snprintf(tool, sizeof tool, "%s/tools/coop/charmod.py", root);
    const char *img = romfs_image_path();
    if (img && *img) {
        snprintf(romfs, sizeof romfs, "%s", img);
        char *a = strrchr(romfs, '/'), *b = strrchr(romfs, '\\');
        if (b > a) a = b;
        if (a) *a = 0;
    }
    char exe[2048];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *a = strrchr(exe, '/'), *b = strrchr(exe, '\\');
    if (b > a) a = b;
    if (a) *a = 0;
    snprintf(logs, sizeof logs, "%s/logs", exe);
    snprintf(job_status, sizeof job_status, "%s/charmod-status.txt", logs);
    remove(job_status);
    const char *argv[24];
    int n = 0;
    argv[n++] = py, argv[n++] = "-I", argv[n++] = tool;
    for (int i = 0; args[i] && n < 16; i++) argv[n++] = args[i];
    argv[n++] = "--status", argv[n++] = job_status, argv[n++] = "--mods", argv[n++] = mods_dir();
    if (romfs[0]) argv[n++] = "--romfs", argv[n++] = romfs;
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
    rt_log("coop: characters: %s started (%s %s)", what, py, tool);
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
    // A fetched character is another player's: its folder off, or the next start would make it this PC's Link's (a
    // folder the mods list has not seen comes on).
    const char *folder = kv("folder");
    if (job.state == COOP_JOB_DONE && !strcmp(job.what, "fetch") && folder[0]) {
        const char *was = cfg_get("TOTK_MODS_DISABLED");
        char list[4096];
        snprintf(list, sizeof list, "%s%s%s", was && *was ? was : "", was && *was ? "|" : "", folder);
        cfg_set("TOTK_MODS_DISABLED", list);
        cfg_save();
        rt_log("coop: characters: %s is off (another player's character)", folder);
    }
}

const CoopJob *coop_chars_job(void) { job_poll(); return &job; }

// ---- asking
static CoopCharAsk ask;
static int asking, restart_needed;
static char declined[16][COOP_CHAR_KEY];
static int ndeclined;
static char asked_for[COOP_CHAR_KEY];   // the key the current ask is about ("" a lookup typed into the menu)

static int is_declined(const char *key) {
    for (int i = 0; i < ndeclined; i++) if (!strcmp(declined[i], key)) return 1;
    return 0;
}

static void info_for(uint32_t mod_id, uint32_t file_id) {
    char m[16], f[16];
    snprintf(m, sizeof m, "%u", mod_id);
    snprintf(f, sizeof f, "%u", file_id);
    const char *args[] = {"info", m, "--file", file_id ? f : "auto", NULL};
    job_start("info", args);
}

int coop_chars_ask(CoopCharAsk *out) {
    coop_chars_init();
    job_poll();
    if (!asking) {   // a party member's character missing here, not declined, from GameBanana
        take();
        for (int id = 1; id <= COOP_MAX_PLAYERS && !asking; id++) {
            const char *key = party[id].key;
            if (strncmp(key, "gb:", 3) || coop_char_find(key) || is_declined(key) || !party[id].mod_id) continue;
            if (job.state == COOP_JOB_RUNNING) break;
            memset(&ask, 0, sizeof ask);
            ask.player_id = id, ask.mod_id = party[id].mod_id, ask.file_id = (uint32_t)strtoul(key + 3, NULL, 10);
            snprintf(asked_for, sizeof asked_for, "%s", key);
            snprintf(ask.page, sizeof ask.page, "https://gamebanana.com/mods/%u", ask.mod_id);
            asking = 1;
            info_for(ask.mod_id, ask.file_id);
        }
        give();
    }
    if (!asking) return 0;
    if (!ask.ready && !strcmp(job.what, "info") && job.state != COOP_JOB_RUNNING) {
        if (job.state == COOP_JOB_FAILED) {   // GameBanana said no (or no network): not asked again this run
            if (asked_for[0] && ndeclined < 16) snprintf(declined[ndeclined++], COOP_CHAR_KEY, "%s", asked_for);
            asking = 0;
            return 0;
        }
        snprintf(ask.name, sizeof ask.name, "%s", kv("name"));
        snprintf(ask.page, sizeof ask.page, "%s", kv("page"));
        snprintf(ask.file, sizeof ask.file, "%s", kv("file"));
        ask.size = strtoull(kv("size"), NULL, 10);
        ask.file_id = (uint32_t)strtoul(kv("file_id"), NULL, 10);
        ask.mod_id = (uint32_t)strtoul(kv("mod_id"), NULL, 10);
        ask.ready = 1;
    }
    *out = ask;
    return 1;
}

void coop_chars_answer(int yes) {
    if (!asking) return;
    asking = 0;
    if (!yes || !ask.ready) {
        if (asked_for[0] && ndeclined < 16) snprintf(declined[ndeclined++], COOP_CHAR_KEY, "%s", asked_for);
        rt_log("coop: characters: %s not downloaded (the player said no)", ask.name[0] ? ask.name : asked_for);
        return;
    }
    char m[16], f[16];
    snprintf(m, sizeof m, "%u", ask.mod_id);
    snprintf(f, sizeof f, "%u", ask.file_id);
    const char *args[] = {"fetch", f, "--mod", m, "--yes", "--sets", NULL};
    rt_log("coop: characters: downloading %s (%s, %s)", ask.name, ask.file, ask.page);
    if (job_start("fetch", args)) restart_needed = 1;
}

void coop_chars_lookup(const char *link) {
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
    info_for(id, 0);
}

int coop_chars_restart_needed(void) { return restart_needed && job.state == COOP_JOB_DONE && !strcmp(job.what, "fetch"); }
