// Co-op: the enemy sync's shots (docs/coop/enemy-sync.md section A; research notes R6 and R6b).
//
// An enemy does not create a projectile when it shoots. Its Shooter component ([[actor+0x228]+0x448]) makes its
// projectiles in advance (a Fire Wizzrobe's fireballs, an Octorok's rock, a quiver of arrows), keeps a record of
// 0xf8 bytes for each, and a shot is two calls on a record: PREPARE main+0x1bf9af8(S, R, index, Q), which wakes the
// reserved projectile at a pose and sets the record to state 1, and SHOOT main+0x1bfbc44(S, R, Q, &velocity, flag,
// &target), which fills the record and sets it to state 2; the projectile's own component polls its record and does
// the rest (launch, flight, end, back to the reserve). Q is the request block an AI node builds on its stack
// (0x1980 bytes: a vtable of game functions, a table of 32 string parameters, then plain numbers).
//
// A copy's graphs do not run, so a copy never prepares and never shoots [measured: none of the three entries is
// reached on a copy], but every game's copy has the same Shooter with the same records and reserved projectiles. So
// the game that runs the enemy states each prepare and each shoot with the request block it was made with, and a
// copy makes the same call on its own record of the same key and index, at the time T it shows that owner at. The
// request block is sent with its runs of zeros squeezed out; pointers into the block itself are moved to where the
// block is rebuilt. The one pointer out of it is a prepare's Q+8: the name of the shooter's bone the prepared projectile
// is held at (the node's TrackBoneName, put there by its virtual +0x118, main+0x24521f8 for ExecuteShooterPrepare). The
// prepare starts the projectile at the shooter's place when that name is not empty, and the record keeps the pointer
// (R+0x50) for as long as the projectile is held. So the name is stated whole and the copy's block points at this
// game's own lasting text of it. (The first version took it for a flag byte and gave the copy one byte: its record
// named no bone, the stone a Bokoblin's copy was about to throw was not held in its hand and fell for the second
// before the throw, eight metres, and only the owner's game saw it fly: a play session, 2026-11-17.)
//
// Not here: a mirrored projectile's damage is the game's own (it hurts this game's Link as the owner's would; what
// else it touches is not yet filtered), and its chemistry is the element sync's to switch off.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

#define SEC 1000000000ull
#define Q_SZ 0x1980
#define FN_PREPARE 0x1bf9af8
#define FN_SHOOT 0x1bfbc44
#define BONE_SZ 48
void rt_dispatch(Cpu *c);

typedef struct {
    _Atomic int state;            // 0 free, 1 being filled, 2 waiting (captured here, or taken from the owner), 3 due on the copy
    uint8_t kind;                 // 1 prepare, 2 shoot
    uint8_t flag;                 // the shoot's flag argument
    char bone[BONE_SZ];           // a prepare's track bone name (what Q+8 points to)
    uint8_t via;                  // whose Shooter: 0 the enemy's own; 1 + k what its equipment slot k holds; 0x80 | (1 + k) what is fused to that
    uint32_t key; int32_t idx;    // the record: its slot's key hash and its index in the slot
    uint32_t index_arg;           // prepare's third argument
    uint64_t actor, q_addr;       // capture: the enemy; where the block stood in the owner's game
    Enid id; int64_t ns;
    int cell, tries;              // copy: the cell it is due on; prepares the game refused
    uint64_t taken_ns;            // copy: when it was taken from the owner
    float va[3], vb[3];
    uint8_t q[Q_SZ];
} Shot;
#define SHOT_N 16
static Shot shots[SHOT_N];
static _Atomic uint32_t n_cap, n_sent, n_big, n_taken, n_done, n_miss;

typedef struct { Enid id; int64_t ns; uint32_t key; int32_t idx; uint32_t index_arg; uint8_t kind, pad, flag, via; uint64_t q_addr; float va[3], vb[3]; uint16_t len, pad2[3]; char bone[BONE_SZ]; uint8_t z[1032]; } MShot;
_Static_assert(sizeof(MShot) <= COOP_CTR_PAYLOAD, "a shot fits one transport payload");

// This game's lasting text of a bone name (a record keeps the pointer while its projectile is held; the few names in
// use are kept for the session).
static const char *bone_text(const char *s) {
    static char tab[32][BONE_SZ];
    static _Atomic int used[32];   // 0 free, 1 being written, 2 set
    for (int i = 0; i < 32; i++) {
        int f = 0;
        if (atomic_compare_exchange_strong(&used[i], &f, 1)) { strncpy(tab[i], s, BONE_SZ - 1); atomic_store(&used[i], 2); return tab[i]; }
        while (atomic_load(&used[i]) == 1) {}
        if (!strcmp(tab[i], s)) return tab[i];
    }
    return "";
}

static Shot *shot_take(void) {
    for (int i = 0; i < SHOT_N; i++) { int f = 0; if (atomic_compare_exchange_strong(&shots[i].state, &f, 1)) return &shots[i]; }
    return NULL;
}
// The record's slot key and index in a Shooter's first table (S+0x20, count S+0x2c, 0x38 a slot: +0 key, +8 record
// count, +0x10 the records).
static int rec_where(uint64_t s, uint64_t r, uint32_t *key, int32_t *idx) {
    uint64_t tab = coop_rd_ptr(s + 0x20);
    int32_t n = 0;
    if (!tab || !coop_rd(s + 0x2c, &n, 4) || n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) {
        uint64_t arr = coop_rd_ptr(tab + (uint64_t)i * 0x38 + 0x10);
        int32_t cnt = 0;
        if (!arr || !coop_rd(tab + (uint64_t)i * 0x38 + 8, &cnt, 4) || cnt <= 0 || cnt > 64) continue;
        if (r >= arr && r < arr + (uint64_t)cnt * 0xf8 && !((r - arr) % 0xf8)) return coop_rd(tab + (uint64_t)i * 0x38, key, 4), *idx = (int32_t)((r - arr) / 0xf8), 1;
    }
    return 0;
}
static uint64_t rec_at(uint64_t s, uint32_t key, int32_t idx) {
    uint64_t tab = coop_rd_ptr(s + 0x20);
    int32_t n = 0;
    if (!tab || !coop_rd(s + 0x2c, &n, 4) || n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; i++) {
        uint32_t k = 0;
        int32_t cnt = 0;
        uint64_t arr = coop_rd_ptr(tab + (uint64_t)i * 0x38 + 0x10);
        if (arr && coop_rd(tab + (uint64_t)i * 0x38, &k, 4) && k == key && coop_rd(tab + (uint64_t)i * 0x38 + 8, &cnt, 4) && idx >= 0 && idx < cnt) return arr + (uint64_t)idx * 0xf8;
    }
    return 0;
}

static void watch_add(uint64_t r, uint32_t key, int copy);
// An enemy's equipment: what slot k holds, and what is fused to it (the link at +0xb0 of the weapon's [comps+0x208]).
static uint64_t held_of(uint64_t enemy, int k, int fused) {
    uint64_t eu = coop_rd_ptr(en_comps(enemy) + 0x230), ref = eu ? coop_rd_ptr(eu + 0x20 + (uint64_t)k * 0x18 + 8) : 0, w = ref ? coop_rd_ptr(ref + 0x40) : 0;
    if (!fused || !w) return w;
    uint64_t wc = coop_rd_ptr(en_comps(w) + 0x208), aref = wc ? coop_rd_ptr(wc + 0xb0 + 8) : 0;
    return aref ? coop_rd_ptr(aref + 0x40) : 0;
}
// The actor whose Shooter a stated shot is for, on this game's actor of that enemy (0: it holds no such thing here).
static uint64_t via_actor(uint64_t enemy, uint8_t via) {
    if (!via) return enemy;
    int k = (via & 0x7f) - 1;
    return k >= 0 && k < EN_EQ_N ? held_of(enemy, k, via & 0x80) : 0;
}

// Where a request block's 32 parameter cells start (0: the block is not laid out as known). The list header sits at Q+8 in
// a shoot's block and at Q+0x18 in a prepare's; its pointer to the cells points just past itself.
static int cells_at(const uint8_t *q, uint64_t q_addr, int kind) {
    int hdr = kind == 2 ? 8 : 0x18;
    uint64_t cp;
    int32_t cnt;
    memcpy(&cp, q + hdr + 8, 8), memcpy(&cnt, q + hdr + 0x10, 4);
    return cp == q_addr + (uint64_t)hdr + 0x18 && cnt >= 0 && cnt <= 32 ? hdr + 0x18 : 0;
}

// The net for the mistake of the track bone's name: any other word of a captured block that is an address of this
// game's memory (not into the block, not into the game's image) reaches a copy's game as it is, where it names
// something else. Six of the cells' value types (1, 6, 9, 0xd, 0xe, 0x14) hold eight bytes that may be such an
// address; what each holds was not read. Each place is logged once with what the address names here, so a next case
// is seen in a log and not in a play session.
static void foreign_note(const Shot *h, int co, const char *who) {
    static _Atomic uint32_t seen[24];
    for (int k = 8; k + 8 <= Q_SZ; k += 8) {
        uint64_t v;
        uint8_t t[25] = {0};
        memcpy(&v, h->q + k, 8);
        if ((k == 8 && h->kind == 1) || v < 0x10000 || (v >= h->q_addr && v < h->q_addr + Q_SZ) || (v >= MOD_MAIN_BASE && v < MOD_MAIN_BASE + 0x8000000ull) || !plat_mem_read(v, t, 24)) continue;
        uint32_t tag = (uint32_t)h->kind << 16 | (uint32_t)k;
        int isnew = 0;
        for (int i = 0; i < 24 && !isnew; i++) {
            uint32_t have = atomic_load(&seen[i]);
            if (have == tag) break;
            if (!have) { if (atomic_compare_exchange_strong(&seen[i], &have, tag)) isnew = 1; else if (have == tag) break; }
        }
        if (!isnew) continue;
        int ci = co && k >= co && k < co + 32 * 0xc8 ? (k - co) / 0xc8 : -1;
        uint32_t type = 0;
        char name[0x80] = "";
        if (ci >= 0) memcpy(&type, h->q + co + ci * 0xc8, 4), memcpy(name, h->q + co + ci * 0xc8 + 0x14, 0x7f);
        for (int i = 0; i < 24; i++) if (t[i] && (t[i] < 0x20 || t[i] >= 0x7f)) t[i] = '.';
        rt_log("coop: en: shot: FOREIGN ADDRESS in a %s block of %s at +%#x: %llx (cell %d '%s' type %u, +%#x in it); it names here '%s': a copy's game gets the address as it is",
               h->kind == 2 ? "shoot" : "prepare", who, k, (unsigned long long)v, ci, name, type, ci >= 0 ? (k - co) % 0xc8 : 0, (const char *)t);
    }
}

// The same net for the small requests the sync states as bytes (an action request, a node's arm request, a throw's):
// the first 2000 of them are looked through, each place logged once. `skip` is an offset known to be a pointer and
// handled (-1: none).
void en_foreign_scan(const char *what, const uint8_t *p, int n, int skip) {
    static _Atomic uint32_t seen[32], calls;
    if (atomic_load_explicit(&calls, memory_order_relaxed) > 2000) return;
    atomic_fetch_add(&calls, 1);
    uint32_t hw = 2166136261u;
    for (const char *s = what; *s; s++) hw = (hw ^ (uint8_t)*s) * 16777619u;
    for (int k = 0; k + 8 <= n; k += 8) {
        uint64_t v;
        uint8_t t[25] = {0};
        memcpy(&v, p + k, 8);
        if (k == skip || v < 0x10000 || (v >= MOD_MAIN_BASE && v < MOD_MAIN_BASE + 0x8000000ull) || !plat_mem_read(v, t, 24)) continue;
        uint32_t tag = (hw + (uint32_t)k * 2654435761u) | 1;
        int isnew = 0;
        for (int i = 0; i < 32 && !isnew; i++) {
            uint32_t have = atomic_load(&seen[i]);
            if (have == tag) break;
            if (!have) { if (atomic_compare_exchange_strong(&seen[i], &have, tag)) isnew = 1; else if (have == tag) break; }
        }
        if (!isnew) continue;
        for (int i = 0; i < 24; i++) if (t[i] && (t[i] < 0x20 || t[i] >= 0x7f)) t[i] = '.';
        rt_log("coop: en: FOREIGN ADDRESS in %s at +%#x: %llx; it names here '%s': a copy's game gets the address as it is", what, k, (unsigned long long)v, (const char *)t);
    }
}

// ---- The owner: a prepare or a shoot of an enemy this game runs is made, then captured ---------------------------------
// From en_copy_hook, at the function's entry. 1: the function was run here.
int en_shot_hook(Cpu *c, uint64_t off, EnCell *m) {
    static __thread int deep;
    if (deep || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return 0;
    uint64_t s = c->x[0], r = c->x[1], q = off == FN_SHOOT ? c->x[2] : c->x[3], shooter = coop_rd_ptr(s + 0x18), actor = atomic_load(&m->actor);
    uint8_t via = 0;   // (the Shooter of the enemy itself, or of something it holds: a fire rod's flame is its ruby's shot)
    for (int k = 0; shooter != actor && !via && k < EN_EQ_N; k++) via = held_of(actor, k, 0) == shooter ? (uint8_t)(1 + k) : held_of(actor, k, 1) == shooter ? (uint8_t)(0x80 | (1 + k)) : 0;
    if (shooter != actor && !via) return 0;
    uint32_t index_arg = (uint32_t)c->x[2];
    float va[3] = {0}, vb[3] = {0};
    uint8_t flag = (uint8_t)c->x[4];
    if (off == FN_SHOOT) coop_rd(c->x[3], va, 12), coop_rd(c->x[5], vb, 12);
    Shot *h = q > 0x10000 ? shot_take() : NULL;
    if (h && !coop_rd(q, h->q, Q_SZ)) atomic_store(&h->state, 0), h = NULL;   // (the block as the caller built it: the call may change it)
    if (h) {   // (a prepare's track bone name, read a byte at a time: it may end at the edge of what can be read)
        uint64_t fp = 0;
        memset(h->bone, 0, BONE_SZ);
        if (off != FN_SHOOT) { memcpy(&fp, h->q + 8, 8); for (int k = 0; fp && k < BONE_SZ - 1 && coop_rd(fp + (uint64_t)k, &h->bone[k], 1) && h->bone[k]; k++) {} }
        h->bone[BONE_SZ - 1] = 0;
    }
    deep = 1, c->pc = MOD_MAIN_BASE + off;
    rt_dispatch(c);
    deep = 0;
    {
        static _Atomic int said;
        float rv[12] = {0};
        uint8_t rb[8] = {0};
        coop_rd(r + 0x7c, rv, sizeof rv), coop_rd(r + 0xd8, rb, 8);
        if (atomic_fetch_add(&said, 1) < 16)
            rt_log("coop: en: shot: this game's %s (shooter %u) answered %llx; given %.2f %.2f %.2f flag %u target %.2f %.2f %.2f; record +0x7c: %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f; +0xd8: %02x %02x %02x %02x %02x %02x %02x %02x",
                   off == FN_SHOOT ? "shoot" : "prepare", via, (unsigned long long)c->x[0], va[0], va[1], va[2], flag, vb[0], vb[1], vb[2], rv[0], rv[1], rv[2], rv[3], rv[4], rv[5], rv[6], rv[7], rv[8], rv[9], rv[10], rv[11],
                   rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
    }
    if (off == FN_SHOOT) { uint32_t k0 = 0; int32_t i0 = 0; if (rec_where(s, r, &k0, &i0)) watch_add(r, k0, 0); }
    if (!h) return 1;
    if (!(off == FN_SHOOT || (c->x[0] & 1)) || !rec_where(s, r, &h->key, &h->idx)) { atomic_store(&h->state, 0); return 1; }   // (a prepare the game refused is not stated)
    h->kind = off == FN_SHOOT ? 2 : 1, h->flag = flag, h->index_arg = index_arg, h->actor = actor, h->via = via, h->q_addr = q, h->ns = (int64_t)coop_sess_frame_ns();
    memcpy(h->va, va, 12), memcpy(h->vb, vb, 12);
    h->cell = -1;
    atomic_fetch_add(&n_cap, 1);
    atomic_store(&h->state, 2);
    return 1;
}

// The record: where a shot projectile is 1, 10, 30 and 60 frames after its shoot, in the game that shot it and on a copy
// (the first 24 shots of each kind), so the two games' flights can be laid side by side.
uint64_t coop_enemy_holder(uint64_t actor);   // coop_enemy_copy.c
static struct { _Atomic uint64_t proj; uint64_t rec; uint32_t frame, key; uint8_t copy; } watch[8];
static void watch_add(uint64_t r, uint32_t key, int copy) {
    static _Atomic int n;
    uint64_t ref = coop_rd_ptr(r + 0x28), proj = ref ? coop_rd_ptr(ref + 0x40) : 0;
    if (!proj || atomic_fetch_add(&n, 1) >= 24) return;
    for (int i = 0; i < 8; i++) { uint64_t z = 0; if (atomic_compare_exchange_strong(&watch[i].proj, &z, 1)) { watch[i].frame = coop_sess_frame_no(), watch[i].rec = r, watch[i].key = key, watch[i].copy = (uint8_t)copy; atomic_store(&watch[i].proj, proj); return; } }
}
static void watch_phase0(void) {
    uint32_t frame = coop_sess_frame_no();
    for (int i = 0; i < 8; i++) {
        uint64_t p = atomic_load(&watch[i].proj);
        uint32_t d = frame - watch[i].frame;
        if (p <= 1) continue;
        if (d <= 6 || d == 10 || d == 30 || d == 60) {
            float q[3] = {0};
            int32_t st = -1;
            uint8_t hm = 0;
            char nm[40] = "";
            uint64_t gmc = coop_rd_ptr(en_comps(p) + 0x280);
            coop_rd(p + COOP_ACTOR_POS, q, 12), coop_rd(p + COOP_ACTOR_STATE, &st, 4);
            if (gmc) coop_rd(gmc + 0x9b, &hm, 1);
            uint64_t sc = coop_rd_ptr(en_comps(p) + 0x440), lk[3] = {0};
            uint32_t scst = 0, sc340 = 0;
            uint8_t rdf = 0xff, rde = 0xff;
            float aim[3] = {0};
            if (sc) coop_rd(sc + 0x70, lk, sizeof lk), coop_rd(sc + 0x24, &scst, 4), coop_rd(sc + 0x340, &sc340, 4), coop_rd(sc + 0x100, aim, 12);
            coop_rd(watch[i].rec + 0xdf, &rdf, 1), coop_rd(watch[i].rec + 0xde, &rde, 1);
            rt_log("coop: en: shot: flight of %s %08x (%s) +%u frames: at %.2f %.2f %.2f, actor state %d, hidden mask %02x; its component state %u, flags %x, shooter link %llx %llx %llx; record state %u kind %u; aim %.2f %.2f %.2f", coop_actor_name(p, nm, sizeof nm), watch[i].key, watch[i].copy ? "copy" : "run here", d, q[0], q[1], q[2], (int)st, hm,
                   scst, sc340, (unsigned long long)lk[0], (unsigned long long)lk[1], (unsigned long long)lk[2], rdf, rde, aim[0], aim[1], aim[2]);
        }
        if (d > 60) atomic_store(&watch[i].proj, 0);
    }
}

// ---- Phase 0: captures go out; what was taken becomes due at T -----------------------------------------------------------
static int64_t peer_T[COOP_MAX_PLAYERS + 1];
void en_shot_T(int id, int64_t t) { if (id >= 1 && id <= COOP_MAX_PLAYERS) peer_T[id] = t; }   // from en_copy_phase0, once a frame
int64_t en_shot_T_of(int id) { return id >= 1 && id <= COOP_MAX_PLAYERS ? peer_T[id] : 0; }   // (0: not known)

void en_shot_phase0(void) {
    uint64_t me = coop_ctr_me();
    watch_phase0();
    for (int i = 0; i < SHOT_N; i++) {
        Shot *h = &shots[i];
        if (atomic_load(&h->state) != 2) continue;
        if (h->actor) {   // captured here: stated to everyone
            EnCell *m = en_cell_of(h->actor);
            int16_t ei = m ? atomic_load(&m->en) : -1;
            if (ei >= 0 && en_tab[ei].used && en_tab[ei].owner == me) {
                static MShot ms;
                memset(&ms, 0, offsetof(MShot, z));
                ms.id = en_tab[ei].id, ms.ns = h->ns, ms.key = h->key, ms.idx = h->idx, ms.index_arg = h->index_arg, ms.kind = h->kind, ms.flag = h->flag, ms.via = h->via, ms.q_addr = h->q_addr;
                memcpy(ms.va, h->va, 12), memcpy(ms.vb, h->vb, 12), memcpy(ms.bone, h->bone, BONE_SZ);
                // The block's named parameters: a list header (capacity u32, pointer to the cells, count i32) at Q+8 in a
                // shoot's block and at Q+0x18 in a prepare's, then 32 cells of 0xc8 bytes (type u32 +0, pointer to the name
                // +8, the name's capacity 0x80 +0x10, the name +0x14, the value +0x98..0xc8). A shoot's are its InitPos,
                // Velocity, Range, MotionProperty and Level. Cells past the count and each name's bytes after its end are
                // stack leftovers: zeroed, or nothing squeezes. A cell's pointer to its own name and its capacity are
                // the same in every block: left out, and written again where the block is rebuilt. (The first version
                // took a shoot's cells to start at Q+0x30 as a prepare's do and zeroed each next cell's type and name
                // pointer: a copy's rod got its flame's Range and no Velocity, and the flame ended where it began.)
                int co = cells_at(h->q, h->q_addr, h->kind), cnt = 0;
                if (co) memcpy(&cnt, h->q + co - 8, 4);
                for (int k = 0; co && k < 32; k++) {
                    uint8_t *cell = h->q + co + k * 0xc8, *text = cell + 0x14;
                    if (k >= cnt) { memset(cell, 0, 0xc8); continue; }
                    size_t len = strnlen((const char *)text, 0x80);
                    memset(text + len, 0, 0x80 - len);
                    memset(cell + 4, 0, 4), memset(cell + 0x94, 0, 4);   // (padding)
                    uint64_t tp;
                    uint32_t cap;
                    memcpy(&tp, cell + 8, 8), memcpy(&cap, cell + 0x10, 4);
                    if (tp == h->q_addr + (uint64_t)co + (uint64_t)k * 0xc8 + 0x14 && cap == 0x80) memset(cell + 8, 0, 12);
                }
                foreign_note(h, co, en_tab[ei].name);
                // the block with its runs of zeros squeezed: (count of zeros u16, count of bytes u16, the bytes) ...
                int n = 0, at = 0, ok = 1;
                while (at < Q_SZ && ok) {
                    int z = 0, lit = 0;
                    while (at + z < Q_SZ && !h->q[at + z]) z++;
                    at += z;
                    while (at + lit < Q_SZ && !(h->q[at + lit] == 0 && at + lit + 8 <= Q_SZ && !memcmp(h->q + at + lit, "\0\0\0\0\0\0\0\0", 8))) lit++;
                    if (n + 4 + lit > (int)sizeof ms.z) { ok = 0; break; }
                    ms.z[n] = (uint8_t)z, ms.z[n + 1] = (uint8_t)(z >> 8), ms.z[n + 2] = (uint8_t)lit, ms.z[n + 3] = (uint8_t)(lit >> 8);
                    memcpy(ms.z + n + 4, h->q + at, (size_t)lit);
                    n += 4 + lit, at += lit;
                }
                if (ok) ms.len = (uint16_t)n, en_send_all(ENM_PROJ, &ms, (int)offsetof(MShot, z) + n), atomic_fetch_add(&n_sent, 1);
                else if (atomic_fetch_add(&n_big, 1) < 5) rt_log("coop: en: shot: a request block of %s does not fit a message: not stated", en_tab[ei].name);
                if (ok && atomic_load(&n_sent) <= 12)
                    rt_log("coop: en: shot: %s %016llx %s its projectile %08x/%d (shooter %u): stated (%d bytes)", en_tab[ei].name, (unsigned long long)en_tab[ei].id.a, h->kind == 2 ? "shoots" : "prepares", h->key, h->idx, h->via, n);
            }
            atomic_store(&h->state, 0);
            continue;
        }
        // taken from the owner: due when this game shows that owner at the shot's time (or a second late at most)
        En *e = NULL;
        for (int k = 0; k < EN_N; k++) if (en_tab[k].used && enid_eq(en_tab[k].id, h->id)) e = &en_tab[k];
        int id = e ? en_peer_id(e->owner) : 0;
        if (!e || e->owner == me || e->cell < 0) { if (!e || en_now() - h->taken_ns > 2 * SEC) atomic_store(&h->state, 0), atomic_fetch_add(&n_miss, 1); continue; }
        if (id && peer_T[id] && peer_T[id] < h->ns && en_now() - h->taken_ns < SEC) continue;
        h->cell = e->cell;
        atomic_store(&h->state, 3);
    }
    static uint64_t said;
    uint64_t now = en_now();
    if (now - said >= 10 * SEC && (atomic_load(&n_cap) || atomic_load(&n_taken))) {
        said = now;
        rt_log("coop: en: shots: %u of this game's enemies captured, %u stated, %u too big; %u taken from owners, %u made on copies, %u dropped (no copy here)",
               atomic_load(&n_cap), atomic_load(&n_sent), atomic_load(&n_big), atomic_load(&n_taken), atomic_load(&n_done), atomic_load(&n_miss));
    }
}

int en_shot_msg(const CoopCtrMsg *m, int from) {
    (void)from;
    if (m->type != ENM_PROJ) return 0;
    const MShot *ms = (const MShot *)m->d;
    if (m->len < (int)offsetof(MShot, z) || ms->len > sizeof ms->z || m->len != (int)offsetof(MShot, z) + ms->len || (ms->kind != 1 && ms->kind != 2)) return 1;
    Shot *h = shot_take();
    if (!h) { atomic_fetch_add(&n_miss, 1); return 1; }
    memset(h->q, 0, Q_SZ);
    int at = 0, n = 0, ok = 1;
    while (n + 4 <= ms->len && ok) {
        int z = ms->z[n] | ms->z[n + 1] << 8, lit = ms->z[n + 2] | ms->z[n + 3] << 8;
        if (at + z + lit > Q_SZ || n + 4 + lit > ms->len) { ok = 0; break; }
        memcpy(h->q + at + z, ms->z + n + 4, (size_t)lit);
        at += z + lit, n += 4 + lit;
    }
    if (!ok) { atomic_store(&h->state, 0); return 1; }
    h->actor = 0, h->id = ms->id, h->ns = ms->ns, h->key = ms->key, h->idx = ms->idx, h->index_arg = ms->index_arg, h->kind = ms->kind, h->flag = ms->flag, h->via = ms->via, h->q_addr = ms->q_addr;
    memcpy(h->va, ms->va, 12), memcpy(h->vb, ms->vb, 12), memcpy(h->bone, ms->bone, BONE_SZ), h->bone[BONE_SZ - 1] = 0;
    h->cell = -1, h->tries = 0, h->taken_ns = en_now();
    atomic_fetch_add(&n_taken, 1);
    atomic_store(&h->state, 2);
    return 1;
}

// ---- The copy's own thread: the stated call on its own record ---------------------------------------------------------------
void en_shot_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    int cell = (int)(m - en_map);
    for (int pass = 1; pass <= 2; pass++)   // (a prepare before a shoot of the same frame)
        for (int i = 0; i < SHOT_N; i++) {
            Shot *h = &shots[i];
            if (atomic_load_explicit(&h->state, memory_order_relaxed) != 3 || h->cell != cell || h->kind != pass) continue;
            if (pass == 2) {   // (a shoot waits for the prepare of its record that is still being tried)
                int waits = 0;
                for (int k = 0; k < SHOT_N; k++) waits |= k != i && atomic_load_explicit(&shots[k].state, memory_order_relaxed) == 3 && shots[k].cell == cell && shots[k].kind == 1 && shots[k].key == h->key && shots[k].idx == h->idx;
                if (waits) continue;
            }
            uint64_t of = via_actor(actor, h->via), s = of ? coop_rd_ptr(en_comps(of) + 0x448) : 0, r = s ? rec_at(s, h->key, h->idx) : 0;
            static __thread _Alignas(16) uint8_t q[Q_SZ];
            static __thread _Alignas(16) float va[4], vb[4];
            uint64_t ret = 0;
            if (r) {
                {   // (the record's projectile is this copy's from now, for whoever asks: coop_enemy_shot_mirror)
                    void en_held_add(uint64_t actor, uint64_t holder);
                    uint64_t pref = coop_rd_ptr(r + 0x28);
                    en_held_add(pref ? coop_rd_ptr(pref + 0x40) : 0, actor);
                }
                memcpy(q, h->q, Q_SZ);
                uint64_t here = (uint64_t)(uintptr_t)q;
                for (int k = 0; k + 8 <= Q_SZ; k += 8) {   // pointers into the block go where the block is now
                    uint64_t v;
                    memcpy(&v, q + k, 8);
                    // (a prepare's block has the track bone's name at +8, which the record keeps at +0x50: into the game's
                    // image the pointer is the same in every game; any other is given this game's text of the name)
                    if (k == 8 && h->kind == 1 && v && !(v >= MOD_MAIN_BASE && v < MOD_MAIN_BASE + 0x8000000ull) && !(v >= h->q_addr && v < h->q_addr + Q_SZ)) v = (uint64_t)(uintptr_t)bone_text(h->bone);
                    else if (v >= h->q_addr && v < h->q_addr + Q_SZ) v = v - h->q_addr + here;
                    else continue;
                    memcpy(q + k, &v, 8);
                }
                int co = cells_at(h->q, h->q_addr, h->kind);
                for (int k = 0; co && k < 32; k++) {   // the cells' own pointers and capacities
                    uint8_t *cell = q + co + k * 0xc8;
                    uint64_t tp;
                    uint32_t cap = 0x80;
                    memcpy(&tp, cell + 8, 8);
                    if (!tp) tp = here + (uint64_t)co + (uint64_t)k * 0xc8 + 0x14, memcpy(cell + 8, &tp, 8), memcpy(cell + 0x10, &cap, 4);
                }
                memcpy(va, h->va, 12), memcpy(vb, h->vb, 12);
                uint64_t x[31], sp = c->sp, pc = c->pc;
                memcpy(x, c->x, sizeof x);
                if (h->kind == 1) ret = rt_call(c, MOD_MAIN_BASE + FN_PREPARE, 4, s, r, (uint64_t)h->index_arg, here);
                else ret = rt_call(c, MOD_MAIN_BASE + FN_SHOOT, 6, s, r, here, (uint64_t)(uintptr_t)va, (uint64_t)h->flag, (uint64_t)(uintptr_t)vb);
                if (h->kind == 1 && !(ret & 1) && h->tries < 150) {
                    // Refused: the record's projectile is not there to wake. Arrows and thrown things are made on request
                    // (main+0x1bfd6a8(S, key, count, flag), which an archer's graph asks all the time and a copy's never):
                    // asked here as the owner's game asks it, and the prepare is tried again for up to 150 frames (the first
                    // arrow of a copy came after more than 45).
                    if (!h->tries++) rt_call(c, MOD_MAIN_BASE + 0x1bfd6a8, 4, s, (uint64_t)h->key, (uint64_t)0xff, (uint64_t)1);
                    memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
                    continue;
                }
                memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
                if (h->kind == 2) watch_add(r, h->key, 1);
                atomic_fetch_add(&n_done, 1);
            } else atomic_fetch_add(&n_miss, 1);
            if (r && atomic_load(&n_done) <= 16) {
                float rv[12] = {0};
                uint8_t rb[8] = {0};
                coop_rd(r + 0x7c, rv, sizeof rv), coop_rd(r + 0xd8, rb, 8);
                rt_log("coop: en: shot: the copy's %s: given %.2f %.2f %.2f flag %u target %.2f %.2f %.2f; record +0x7c: %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f; +0xd8: %02x %02x %02x %02x %02x %02x %02x %02x",
                       h->kind == 2 ? "shoot" : "prepare", h->va[0], h->va[1], h->va[2], h->flag, h->vb[0], h->vb[1], h->vb[2], rv[0], rv[1], rv[2], rv[3], rv[4], rv[5], rv[6], rv[7], rv[8], rv[9], rv[10], rv[11],
                       rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
            }
            if (atomic_load(&n_done) + atomic_load(&n_miss) <= 12)
                rt_log("coop: en: shot: the copy %llx %s its projectile %08x/%d as its owner's did: %s (the call answered %llx)", (unsigned long long)actor, h->kind == 2 ? "shoots" : "prepares", h->key, h->idx,
                       r ? "made" : "no such record here", (unsigned long long)ret);
            if (r && h->kind == 1 && atomic_load(&n_done) <= 12) rt_log("coop: en: shot: (held at the bone '%s')", h->bone);
            if (r && h->tries) rt_log("coop: en: shot: (the projectile was asked for first: %d frames)", h->tries);
            atomic_store(&h->state, 0);
        }
}

// For the element sync: whether a projectile actor is a copy's (its shooter, the link at +0x70 of the projectile's
// Shootable component [[proj+0x228]+0x440], is an enemy another game runs). Such a projectile is shown here and
// decides nothing: what the owner's real one lights or freezes is stated by the game that runs the enemy.
int coop_enemy_shot_mirror(uint64_t proj) {
    int coop_enemy_role(uint64_t actor);
    uint64_t coop_enemy_holder(uint64_t actor);   // coop_enemy_copy.c
    // (first by the set of ready projectiles: one a copy's rod shoots by itself has no shooter link at all)
    {
        uint64_t h0 = coop_enemy_holder(proj);
        static _Atomic int said0;
        char pn[40] = "";
        if (h0 && atomic_fetch_add(&said0, 1) < 6) rt_log("coop: en: shot: mirror asked of %s (%llx): it is a ready projectile of %llx (role %d)", coop_actor_name(proj, pn, sizeof pn), (unsigned long long)proj, (unsigned long long)h0, coop_enemy_role(h0));
        if (h0) return coop_enemy_role(h0) == 2;
    }
    uint64_t sc = proj > 0x10000 ? coop_rd_ptr(en_comps(proj) + 0x440) : 0, ref = sc ? coop_rd_ptr(sc + 0x78) : 0, shooter = ref ? coop_rd_ptr(ref + 0x40) : 0;
    // (the shooter can be what the enemy holds: a fire rod's flame is shot by the ruby fused to the rod)
    {   // (test) the first shooters asked about that are no enemy: who they are and whether a holder is known
        static _Atomic int said;
        char pn[40] = "", sn[40] = "";
        if (atomic_fetch_add(&said, 1) < 8)
            rt_log("coop: en: shot: mirror asked of %s (%llx): its shooter %llx %s, holder %llx", coop_actor_name(proj, pn, sizeof pn), (unsigned long long)proj, (unsigned long long)shooter, coop_actor_name(shooter, sn, sizeof sn), (unsigned long long)coop_enemy_holder(shooter));
    }
    if (shooter && !coop_enemy_role(shooter)) { uint64_t h = coop_enemy_holder(shooter); if (h) shooter = h; }
    return shooter && coop_enemy_role(shooter) == 2;
}

// ---- A Shooter put to sleep (docs/coop/enemy-sync.md B.36; research R19, the shooter-state nodes) -----------------------
// Besides prepare and shoot, the nodes of an enemy's graph put its Shooter's shots to sleep: a prepared arrow put away
// when an archer gives up its shot (OneShotEquipmentUserCancelShooter, OneShotShooterRequestSleep, the "drop prepared"
// nodes when the shot is no longer in the hand), all shots of a name (main+0x1bfa4e0), every prepared one
// (main+0x1bfa87c). A copy's graph does not run, so the shot its copy prepared from its owner's request stayed nocked.
// The three entries take the Shooter and plain numbers (main+0x1bfa698(S, name hash, index), main+0x1bfa4e0(S, name
// hash, force), main+0x1bfa87c(S)), so the game that runs the enemy tells each call and the copy's game makes it on
// its own Shooter before the copy's calc. TOTK_COOP_EN_SHS=0: not.
void coop_lock_wait(PlatMutex *m);   // coop_spawn.c
typedef struct { Enid id; uint32_t fn, a1, a2; uint8_t via, pad[3]; } MShs;
#define SHSQ 16
static struct { _Atomic int cell; uint32_t fn, a1, a2; uint8_t via; uint32_t frame; } shsq[SHSQ];   // cell + 1; 0 free, -1 being filled
static int shs_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_EN_SHS"); on = !(e && *e == '0'); }
    return on;
}
int en_shs_hook(Cpu *c, uint64_t off) {
    if (!shs_on()) return 0;
    uint64_t s = c->x[0], shooter = s > 0x10000 ? coop_rd_ptr(s + 0x18) : 0;
    EnCell *m = shooter > 0x10000 ? en_cell_of(shooter) : NULL;
    if (!m && shooter > 0x10000) { uint64_t h = coop_enemy_holder(shooter); m = h ? en_cell_of(h) : NULL; }
    if (!m || atomic_load_explicit(&m->state, memory_order_relaxed) != EM_OWNED) return 0;
    uint64_t actor = atomic_load(&m->actor);
    MShs q;
    memset(&q, 0, sizeof q);
    for (int k = 0; shooter != actor && !q.via && k < EN_EQ_N; k++) q.via = held_of(actor, k, 0) == shooter ? (uint8_t)(1 + k) : held_of(actor, k, 1) == shooter ? (uint8_t)(0x80 | (1 + k)) : 0;
    if (shooter != actor && !q.via) return 0;
    q.fn = (uint32_t)off, q.a1 = off == 0x1bfa87c ? 0 : (uint32_t)c->x[1], q.a2 = off == 0x1bfa87c ? 0 : (uint32_t)c->x[2];
    coop_lock_wait(&en_lock);
    int16_t ei = atomic_load(&m->en);
    if (ei >= 0 && en_tab[ei].used) {
        q.id = en_tab[ei].id;
        en_send_all(ENM_SHS, &q, sizeof q);
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: shot: %s puts its Shooter's shots to sleep (main+%#x, %08x, %u): told to its copies", en_tab[ei].name, q.fn, q.a1, q.a2);
    }
    plat_mutex_unlock(&en_lock);
    return 0;
}
int en_shs_msg(const CoopCtrMsg *m) {
    if (m->type != ENM_SHS) return 0;
    const MShs *q = (const MShs *)m->d;
    uint64_t me = coop_ctr_me();
    if (m->len != (int)sizeof *q || (q->fn != 0x1bfa698 && q->fn != 0x1bfa87c && q->fn != 0x1bfa4e0)) return 1;
    for (int i = 0; i < EN_N; i++) {
        if (!en_tab[i].used || !enid_eq(en_tab[i].id, q->id) || en_tab[i].owner == me || en_tab[i].cell < 0) continue;
        for (int k = 0; k < SHSQ; k++) {
            int z = 0;
            if (!atomic_compare_exchange_strong(&shsq[k].cell, &z, -1)) continue;
            shsq[k].fn = q->fn, shsq[k].a1 = q->a1, shsq[k].a2 = q->a2, shsq[k].via = q->via, shsq[k].frame = coop_sess_frame_no();
            atomic_store(&shsq[k].cell, en_tab[i].cell + 1);
            break;
        }
    }
    return 1;
}
void en_shs_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    int cell = (int)(m - en_map) + 1;
    for (int k = 0; k < SHSQ; k++) {
        if (atomic_load_explicit(&shsq[k].cell, memory_order_relaxed) != cell) continue;
        uint64_t of = via_actor(actor, shsq[k].via), s = of ? coop_rd_ptr(en_comps(of) + 0x448) : 0;
        if (s && coop_sess_frame_no() - shsq[k].frame < 120) {
            coop_guest_call(c, MOD_MAIN_BASE + shsq[k].fn, s, shsq[k].a1, shsq[k].a2, 0);
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: shot: the copy %llx puts its Shooter's shots to sleep as its owner's did (main+%#x)", (unsigned long long)actor, shsq[k].fn);
        }
        atomic_store(&shsq[k].cell, 0);
    }
}
