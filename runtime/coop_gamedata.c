// Co-op: shared world progress (opt-in, TOTK_COOP_SHARE_WORLD=1). Each player keeps their own save; with this on, the
// changes one game makes to the world's game data reach the others' games while they play together: the per-placed-
// object flags (Bool64bitKey: opened chests, found Koroks, broken or collected map objects) and the world's bool flags
// (quest and event flags), but not what belongs to one player (their status, pouch, horses, where they are).
//
// How TotK Online does it (1.2.1 only, no licence: read for behaviour, nothing copied): every frame it walks the game
// data manager's pending set requests per value type, sends the shared ones, sends the event-flag setter's writes at
// once, and applies the others' changes by pushing them into the same pending queues, so the game's own deferred set
// (GameDataMgr::calc) writes them. This file does the same in 1.4.3 at the entry of GameDataMgr::calc, on its thread:
//   1. capture: the pending requests of this game (bool queue, Bool64bitKey queue) whose value differs from the
//      table's, that are shared, go into the outbox;
//   2. apply: the other games' changes received since (coop_gamedata_take, from coop_world.c's packet loop) are
//      pushed into the same queues after them, so calc writes them and they are not captured again (no echo);
//   3. send: the outbox goes out in "GMDT" packets on the HIT channel (coop_net_hit_send); each change is sent again
//      1, 3 and 7 s later (UDP loses packets; applying a change twice is harmless, calc skips a value already set).
// The event-flag setter writes its table directly, not through the queue, so its writes are taken in its own hook
// (coop_gamedata_event_bool).
//
// Addresses (1.4.3 main; how each was found: docs/coop/proxy-plan.md "Shared world progress", and the hand-over note):
//   main+0xa72b00  GameDataMgr::calc (x0 = the manager): 1.2.1 main+0xee9704, the same code (the same offsets 0xd0c,
//                  0xce0, 0xd10, 0xcb8 and per-type queues); its one caller main+0xa72abc locks the manager's mutex
//                  (+0x70) around it and passes [main+0x3ad66b0], as 1.2.1's main+0xee96cc does.
//   main+0xc6a9f0  setEventBool (x0 = the bool table, manager +8; w1 value; x2 the key; w3 the name hash; w4): 1.2.1
//                  main+0x11616bc, the same direct write (+0x16 value, +0xc dirty, the hash onto the changed ring at
//                  table +0x40); its caller main+0xc6a930 is reached from main+0xc6a834, which looks up the same
//                  constant hash 0x2b05cbb9 as 1.2.1's caller of setEventBool.
//   main+0x9240d4  the bool queue push (bool table, hash, &{value, hash}), main+0x2abcf5c the Bool64bitKey queue push
//                  (key table, value, index), from the key setters main+0x2ab6dcc / 0x2ab6e5c (manager, value, u64
//                  key: binary search of the sorted key table). push_queue below does what both do.
//
// Game data manager layout (1.4.3, the same as 1.2.1). Value type t has a block at t * 0x60 (0 Bool, 1 BoolArray,
// 2 Int, 3 IntArray, 4 Float, ... in GameDataList order); for the bools (t = 0):
//   +0x10 u32 entry count, +0x18 entries (0x18 bytes; the current value is the byte at +0x16),
//   +0x20 hash map ({u32 hash, u32 index}, open addressing from hash % size, linear probe, 0 ends), +0x2c its size,
//   +0x30 u32 queue capacity, +0x38 queue entries (8 bytes: u8 value, u32 hash at +4; hash 0 is empty),
//   +0x40 u32 queue word: bits 0..19 the slots claimed, 20..27 a commit counter, 28..31 a generation (calc clears the
//   count and steps the generation when it has written the queue).
// The Bool64bitKey table is at +0xc58: +0 u32 count, +8 entries (0x10 bytes: u64 key, sorted unsigned; u32 at +8 the
// key's reset type, as GameDataList's ResetTypeValue; u8 at +0xc the value), +0x10 queue capacity, +0x18 queue entries
// (8 bytes: u8 value, i32 index at +4; -1 is empty), +0x20 the queue word.
//
// What is personal (decided here; written down in docs/coop/proxy-plan.md):
//   - Bool64bitKey: a key whose reset type has none of the bits 0x1af set (0x50, 3537 of the 130375 keys) belongs to
//     the player, the others (0x150, 0x58, 0x5a, 0xd0, 0x52) to the world. This is TotK Online's server rule
//     (bitKeyMap value & 0x1AF); its bitKeyMap is the game's own GameDataList 1.4.0 Bool64bitKey ResetTypeValue, key for
//     key (same 130375 keys, same values), so the rule reads the game's attribute. The startup line logs the
//     histogram of this game's table so a run shows that the in-memory field is that attribute.
//   - Bool: the reset type does not tell them apart (the player's bools have 16, like 18366 of the 21414), and the
//     game holds no names, only their hashes. So the player's are named below (personal_bools) and hashed at start:
//     the player's status (PlayerStatus.*: companions, belts, special powers), where the player is or what they ride
//     (InLargeDungeon*, Horse_IsRide, Horse_Is_LastRidden_Owned, PumpkinFieldPlayerIsNearby) and the broken Master
//     Sword's flags (each player's own sword). All are 1.4.3 GameDataList bools (checked by hash). Every other bool is
//     shared. TOTK_COOP_SHARE_SKIP adds hashes or keys (hex, comma separated) to keep personal.
//   - Numbers, strings, vectors, arrays: not shared. The player's ones (PlayerStatus.Life, Pouch.*, the horse lists,
//     AlbumData...) have the same reset type and save file as world counters, so there is no split without a name
//     list of every personal entry, which this file does not have.
//
// Test (TOTK_COOP_TEST_GAMEDATA=A:25, in the game whose TOTK_COOP_NAME is A, from 25 s of world time): a shared
// Bool64bitKey that reads 0 here is set to 1 through this game's queue (so the capture finds it as it finds the game's
// own), then put back 10 s later; the same with the bool IsGet.Item_Fruit_A (the apple's "seen" flag) 2 s after.
// tools/coop/gamedata_test.ps1 runs it and lists the lines a passing run shows.
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define GMD_SLOT 0x3ad66b0   // [main+] = the game data manager ([main+0x395dff0] holds main+0x3ad66b0)
#define GMD_CALC 0xa72b00
#define GMD_EVENT_BOOL 0xc6a9f0

#define B_COUNT 0x10
#define B_ENTRIES 0x18
#define B_STRIDE 0x18
#define B_VALUE 0x16
#define B_MAP 0x20
#define B_MAP_SIZE 0x2c
#define B_QCAP 0x30
#define B_QENT 0x38
#define B_QWORD 0x40
#define K_COUNT 0xc58
#define K_ENTRIES 0xc60
#define K_STRIDE 0x10
#define K_RESET 8
#define K_VALUE 0xc
#define K_QCAP 0xc68
#define K_QENT 0xc70
#define K_QWORD 0xc78
#define KEY_SHARED_BITS 0x1afu

enum { T_BOOL = 0, T_KEY = 0x20 };   // the wire's types: TotK Online's numbers (0 Bool, 0x20 Bool64bitKey)

// ---- the wire: "GMDT" packets on the HIT channel (coop_world.c's packet loop hands them to coop_gamedata_take)
#define GD_KIND 0x54444d47u   // "GMDT"
#define GD_MAGIC 0x0101
#define GD_PER_PACKET 60
typedef struct { uint64_t id; uint8_t type, value, pad[6]; } GdEntry;   // id: the bool's hash, or the key
typedef struct { uint32_t kind; uint16_t n, magic; GdEntry e[GD_PER_PACKET]; } GdPacket;
_Static_assert(sizeof(GdPacket) <= COOP_HIT_MAX, "GdPacket fits a HIT packet");

// ---- guest memory (guest addresses are host addresses; the game's own fields, valid while the manager lives)
static inline uint8_t g8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline uint32_t g32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline uint64_t g64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }

static int share_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_SHARE_WORLD"); on = e && *e && *e != '0'; }
    return on;
}

// MurmurHash3 x86 32-bit, seed 0: the game's hash of game data names (every name below matched GameDataList 1.4.0).
static uint32_t mm3(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    size_t len = strlen(s), n = len / 4;
    uint32_t h = 0, k;
    for (size_t i = 0; i < n; i++) {
        memcpy(&k, p + 4 * i, 4);
        k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[4 * n + 2] << 16; // fall through
    case 2: k ^= (uint32_t)p[4 * n + 1] << 8;  // fall through
    case 1: k ^= p[4 * n], k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u, h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static const char *const personal_bools[] = {
    // the player's status
    "PlayerStatus.IsMasterSwordSleeping", "PlayerStatus.IsEquipShoulderBelt", "PlayerStatus.IsEquipWaistBelt",
    "PlayerStatus.IsForceDisableSummonCompanion", "PlayerStatus.IsForceDisableUseSpecialPower",
    "PlayerStatus.IsUseCameraPointer", "PlayerStatus.IsUseTemporaryLife", "PlayerStatus.UseScopeStartEvent",
    "PlayerStatus.HasAnyBluePrint", "PlayerStatus.ZonauEventFailureOnce", "PlayerStatus.FirstSkyIslandEventFailureOnce",
    "PlayerStatus.Companion.Electric.IsLightningReady",
#define COMPANION(n) "PlayerStatus.Companion." n ".IsSummon", "PlayerStatus.Companion." n ".IsLeaveFollower", \
    "PlayerStatus.Companion." n ".IsGoThroughAirWall", "PlayerStatus.Companion." n ".IsGoThroughAirWallPrepare"
    COMPANION("Wind"), COMPANION("Fire"), COMPANION("Electric"), COMPANION("Water"), COMPANION("Soul"),
#undef COMPANION
    // where the player is, what they ride
    "InLargeDungeonWind", "InLargeDungeonThunder", "InLargeDungeonFire", "InLargeDungeonWater",
    "Horse_IsRide", "Horse_Is_LastRidden_Owned", "PumpkinFieldPlayerIsNearby",
    // the player's own broken Master Sword
    "IsGet.Weapon_Sword_070_Broken", "IsGetAnyway.Weapon_Sword_070_Broken", "SageOfSky_Destroy_Weapon_Sword_070_Broken_Demo",
};
#define NPERSONAL ((int)(sizeof personal_bools / sizeof *personal_bools))
#define SKIP_MAX 64
static uint32_t personal_hash[NPERSONAL];
static uint64_t skip_ids[SKIP_MAX];
static int nskip;

static void personal_init(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < NPERSONAL; i++) personal_hash[i] = mm3(personal_bools[i]);
    const char *e = cfg_get("TOTK_COOP_SHARE_SKIP");
    while (e && *e && nskip < SKIP_MAX) {
        char *end;
        unsigned long long v = strtoull(e, &end, 16);
        if (end == e) break;
        skip_ids[nskip++] = v;
        e = *end == ',' ? end + 1 : end;
    }
}
static int skipped(uint64_t id) {
    for (int i = 0; i < nskip; i++)
        if (skip_ids[i] == id) return 1;
    return 0;
}
static int bool_personal(uint32_t h) {
    for (int i = 0; i < NPERSONAL; i++)
        if (personal_hash[i] == h) return 1;
    return skipped(h);
}

// ---- the tables
// A bool's index from its hash (the manager's map at +0x20), -1 when it has none.
static int64_t bool_index(uint64_t gmd, uint32_t h) {
    uint64_t map = g64(gmd + B_MAP);
    uint32_t size = g32(gmd + B_MAP_SIZE), count = g32(gmd + B_COUNT);
    if (!map || !size || !h) return -1;
    uint32_t start = h % size, i = start;
    do {
        uint32_t have = g32(map + (uint64_t)i * 8);
        if (have == h) { uint32_t idx = g32(map + (uint64_t)i * 8 + 4); return idx < count ? (int64_t)idx : -1; }
        if (!have) return -1;
        i = i + 1 >= size ? 0 : i + 1;
    } while (i != start);
    return -1;
}
static int bool_value(uint64_t gmd, int64_t idx) { return g8(g64(gmd + B_ENTRIES) + (uint64_t)idx * B_STRIDE + B_VALUE); }
// A key's index in the sorted key table, -1 when it has none.
static int64_t key_index(uint64_t gmd, uint64_t key) {
    int64_t lo = 0, hi = (int64_t)g32(gmd + K_COUNT) - 1;
    uint64_t ents = g64(gmd + K_ENTRIES);
    if (!ents) return -1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint64_t k = g64(ents + (uint64_t)mid * K_STRIDE);
        if (k == key) return mid;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static uint64_t key_entry(uint64_t gmd, int64_t idx) { return g64(gmd + K_ENTRIES) + (uint64_t)idx * K_STRIDE; }
static int key_personal(uint64_t gmd, int64_t idx) {
    uint64_t e = key_entry(gmd, idx);
    return !(g32(e + K_RESET) & KEY_SHARED_BITS) || skipped(g64(e));
}

// Pushes {value, id} onto a queue as the game's pushes do (main+0x9240d4, main+0x2abcf5c): claim a slot by raising
// the count, write it, step the commit counter. 0 when the queue is full (the rest waits for the next frame).
static int push_queue(uint64_t gmd, uint32_t word_off, uint32_t cap_off, uint32_t ent_off, uint8_t value, uint32_t id) {
    uint32_t *w = (uint32_t *)(uintptr_t)(gmd + word_off), cap = g32(gmd + cap_off);
    uint32_t old = __atomic_load_n(w, __ATOMIC_ACQUIRE), n;
    for (;;) {
        n = old & 0xfffff;
        if (n >= cap) return 0;
        if (__atomic_compare_exchange_n(w, &old, old + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    uint64_t e = g64(gmd + ent_off) + (uint64_t)n * 8;
    *(volatile uint8_t *)(uintptr_t)e = value;
    uint32_t idv = id;
    memcpy((void *)(uintptr_t)(e + 4), &idv, 4);
    uint32_t cur = __atomic_load_n(w, __ATOMIC_ACQUIRE), nw;
    do nw = (cur & 0x0ff00000u) == 0x0ff00000u ? (cur & 0xf00fffffu) : cur + 0x100000u;
    while (!__atomic_compare_exchange_n(w, &cur, nw, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return 1;
}

// ---- outbox: this game's shared changes, each sent at once and again 1, 3 and 7 s later. Locked: the event-flag
// hook may run on another thread than calc.
#define OUT_N 2048
static const uint64_t resend_ns[] = {1000000000ull, 2000000000ull, 4000000000ull};
static struct { uint64_t id, next_ns; uint8_t type, value, sends; } out[OUT_N];
static int out_n;
static PlatMutex out_lock = PLAT_MUTEX_INIT;
static _Atomic uint32_t n_sent_key, n_sent_bool, n_sent_event, n_out_full, n_packets;

static void out_add(uint8_t type, uint64_t id, uint8_t value) {
    plat_mutex_lock(&out_lock);
    int i = 0;
    while (i < out_n && !(out[i].type == type && out[i].id == id)) i++;
    if (i == out_n) {
        if (out_n == OUT_N) { plat_mutex_unlock(&out_lock); atomic_fetch_add(&n_out_full, 1); return; }
        out_n++;
    }
    out[i].type = type, out[i].id = id, out[i].value = value, out[i].sends = 0, out[i].next_ns = 0;
    plat_mutex_unlock(&out_lock);
}
// A change from another game for an id this game also has in its outbox, with another value: theirs is the later
// one, so this game stops sending its own (else the resends would undo theirs).
static void out_drop(uint8_t type, uint64_t id, uint8_t value) {
    plat_mutex_lock(&out_lock);
    for (int i = 0; i < out_n; i++)
        if (out[i].type == type && out[i].id == id && out[i].value != value) { out[i] = out[--out_n]; break; }
    plat_mutex_unlock(&out_lock);
}
static void out_flush(uint64_t now) {
    static GdPacket p;
    plat_mutex_lock(&out_lock);
    for (int pk = 0; pk < 4; pk++) {   // at most 4 packets a frame: the HIT queue (64) carries hits and poses too
        p.kind = GD_KIND, p.magic = GD_MAGIC, p.n = 0;
        for (int i = 0; i < out_n && p.n < GD_PER_PACKET; i++) {
            if (out[i].next_ns > now) continue;
            p.e[p.n++] = (GdEntry){.id = out[i].id, .type = out[i].type, .value = out[i].value};
            if (out[i].sends == 0) atomic_fetch_add(out[i].type == T_KEY ? &n_sent_key : &n_sent_bool, 1);
            if (out[i].sends < sizeof resend_ns / sizeof *resend_ns) out[i].next_ns = now + resend_ns[out[i].sends++];
            else out[i--] = out[--out_n];
        }
        if (!p.n) break;
        coop_net_hit_send(&p, (int)(offsetof(GdPacket, e) + p.n * sizeof(GdEntry)));
        atomic_fetch_add(&n_packets, 1);
    }
    plat_mutex_unlock(&out_lock);
}

// ---- inbox: the other games' changes, filled by coop_gamedata_take, applied at the next calc
#define IN_N 4096
static struct { uint64_t id; uint8_t type, value; int from; } in[IN_N];
static int in_n;
static PlatMutex in_lock = PLAT_MUTEX_INIT;
static _Atomic uint32_t n_recv, n_applied, n_same, n_personal_in, n_unknown, n_in_full;

int coop_gamedata_take(const void *data, int n, int from) {   // coop_world.c's packet loop: 1 when it was ours
    const GdPacket *p = data;
    if (n < (int)offsetof(GdPacket, e) || p->kind != GD_KIND || p->magic != GD_MAGIC || p->n < 1 || p->n > GD_PER_PACKET ||
        n != (int)(offsetof(GdPacket, e) + p->n * sizeof(GdEntry)))
        return 0;
    if (!share_on()) return 1;
    plat_mutex_lock(&in_lock);
    for (int i = 0; i < p->n; i++) {
        if (p->e[i].type != T_BOOL && p->e[i].type != T_KEY) continue;
        if (in_n == IN_N) { atomic_fetch_add(&n_in_full, 1); continue; }
        in[in_n].id = p->e[i].id, in[in_n].type = p->e[i].type, in[in_n].value = p->e[i].value & 1, in[in_n].from = from;
        in_n++;
        atomic_fetch_add(&n_recv, 1);
    }
    plat_mutex_unlock(&in_lock);
    return 1;
}

// ---- state shared by the hooks
static _Atomic int gate;              // 1 while this game shares: in a session with other players, in the world, settled
static _Atomic uint64_t gate_gmd;     // the manager while the gate is open
static uint64_t world_since_ns, last_me;
static int said_start;
#define VERIFY_N 8
static struct { uint64_t id; uint8_t type, value; int from, frames; } verify[VERIFY_N];   // applied, checked after calc

static void log_start(uint64_t gmd) {
    uint32_t nk = g32(gmd + K_COUNT), nb = g32(gmd + B_COUNT), shared = 0;
    struct { uint32_t v, n; } h[12];
    int nh = 0, other = 0;
    for (uint32_t i = 0; i < nk; i++) {
        uint32_t v = g32(key_entry(gmd, i) + K_RESET);
        if (v & KEY_SHARED_BITS) shared++;
        int j = 0;
        while (j < nh && h[j].v != v) j++;
        if (j == nh) { if (nh == 12) { other++; continue; } h[nh].v = v, h[nh].n = 0, nh++; }
        h[j].n++;
    }
    char line[400];
    int len = 0;
    for (int j = 0; j < nh; j++) len += snprintf(line + len, sizeof line - len, "%s%#x x%u", j ? ", " : "", h[j].v, h[j].n);
    rt_log("coop: gamedata: sharing world progress (manager %llx): %u Bool64bitKey keys, %u shared by reset type and %u "
           "the player's; %u bools, %d of them named the player's; queues %u bools, %u keys",
           (unsigned long long)gmd, nk, shared, nk - shared, nb, NPERSONAL, g32(gmd + B_QCAP), g32(gmd + K_QCAP));
    rt_log("coop: gamedata: Bool64bitKey reset types here: %s%s (GameDataList 1.4.0: 0x150 x100751, 0x58 x22312, "
           "0x50 x3537, 0x5a x1846, 0xd0 x982, 0x52 x947)", line, other ? " and more" : "");
}

// The test (header comment): the game named by TOTK_COOP_TEST_GAMEDATA ("A:25") changes a key and a bool through its
// own queue, then puts them back.
static void test_tick(uint64_t gmd, int64_t world_us) {
    static int on = -1, step;
    static int64_t at_us;
    static uint64_t key;
    static uint32_t bh;
    static uint8_t bwas;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_TEST_GAMEDATA"), *me = cfg_get("TOTK_COOP_NAME"), *colon = e ? strchr(e, ':') : NULL;
        on = e && colon && me && (size_t)(colon - e) == strlen(me) && !strncmp(e, me, (size_t)(colon - e));
        if (on) at_us = (int64_t)(atof(colon + 1) * 1e6);
    }
    if (!on || step > 3 || world_us < at_us + step * 2000000ll + (step >= 2 ? 6000000ll : 0)) return;
    if (step == 0) {   // a shared key that reads 0 here, from a tenth of the way in (not the table's first ones)
        uint32_t nk = g32(gmd + K_COUNT);
        for (uint32_t i = nk / 10; i < nk && !key; i++) {
            uint64_t e = key_entry(gmd, i);
            if ((g32(e + K_RESET) & KEY_SHARED_BITS) && !g8(e + K_VALUE)) key = g64(e);
        }
        if (!key) { rt_log("coop: gamedata: test: no shared key reads 0 here"); step = 4; return; }
        int64_t i = key_index(gmd, key);
        push_queue(gmd, K_QWORD, K_QCAP, K_QENT, 1, (uint32_t)i);
        rt_log("coop: gamedata: test: key %016llx (index %lld, reset type %#x) set to 1 in this game (it read 0)",
               (unsigned long long)key, (long long)i, g32(key_entry(gmd, i) + K_RESET));
    } else if (step == 1) {
        bh = mm3("IsGet.Item_Fruit_A");
        int64_t i = bool_index(gmd, bh);
        if (i < 0) { rt_log("coop: gamedata: test: bool IsGet.Item_Fruit_A (%08x) not in this game's table", bh); step = 2; return; }
        bwas = (uint8_t)bool_value(gmd, i);
        push_queue(gmd, B_QWORD, B_QCAP, B_QENT, !bwas, bh);
        rt_log("coop: gamedata: test: bool IsGet.Item_Fruit_A (%08x) set to %u in this game (it read %u)", bh, !bwas, bwas);
    } else if (step == 2) {
        int64_t i = key_index(gmd, key);
        push_queue(gmd, K_QWORD, K_QCAP, K_QENT, 0, (uint32_t)i);
        rt_log("coop: gamedata: test: key %016llx set back to 0 in this game (it read %u)", (unsigned long long)key,
               g8(key_entry(gmd, i) + K_VALUE));
    } else if (bh && bool_index(gmd, bh) >= 0) {
        push_queue(gmd, B_QWORD, B_QCAP, B_QENT, bwas, bh);
        rt_log("coop: gamedata: test: bool %08x set back to %u in this game (it read %u)", bh, bwas, bool_value(gmd, bool_index(gmd, bh)));
    }
    step++;
}

static void capture(uint64_t gmd) {
    static _Atomic uint32_t said;
    int taken = 0, over = 0;
    uint32_t cap = g32(gmd + B_QCAP), n = g32(gmd + B_QWORD) & 0xfffff;
    uint64_t ents = g64(gmd + B_QENT);
    for (uint32_t i = 0; ents && i < n && i < cap; i++) {
        uint64_t e = ents + (uint64_t)i * 8;
        uint32_t h = g32(e + 4);
        if (!h) break;   // claimed, not written yet: calc stops here too
        uint8_t v = g8(e) & 1;
        int64_t idx = bool_index(gmd, h);
        if (idx < 0 || bool_value(gmd, idx) == v || bool_personal(h)) continue;
        if (taken++ >= 256) { over++; continue; }
        out_add(T_BOOL, h, v);
        if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: gamedata: bool %08x = %u in this game, shared", h, v);
    }
    cap = g32(gmd + K_QCAP), n = g32(gmd + K_QWORD) & 0xfffff, ents = g64(gmd + K_QENT);
    uint32_t nk = g32(gmd + K_COUNT);
    for (uint32_t i = 0; ents && i < n && i < cap; i++) {
        uint64_t e = ents + (uint64_t)i * 8;
        int32_t idx;
        memcpy(&idx, (const void *)(uintptr_t)(e + 4), 4);
        if (idx == -1) break;
        uint8_t v = g8(e) & 1;
        if (idx < 0 || (uint32_t)idx >= nk) continue;
        uint64_t ke = key_entry(gmd, idx);
        if (g8(ke + K_VALUE) == v || key_personal(gmd, idx)) continue;
        if (taken++ >= 256) { over++; continue; }
        out_add(T_KEY, g64(ke), v);
        if (atomic_fetch_add(&said, 1) < 20)
            rt_log("coop: gamedata: key %016llx = %u in this game (reset type %#x), shared", (unsigned long long)g64(ke), v, g32(ke + K_RESET));
    }
    if (over) {
        static uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last > 5000000000ull) { last = now; rt_log("coop: gamedata: %d changes in one frame not shared (256 at most)", over); }
    }
}

static void apply(uint64_t gmd) {
    static _Atomic uint32_t said;
    plat_mutex_lock(&in_lock);
    int done = 0;
    for (; done < in_n; done++) {
        uint64_t id = in[done].id;
        uint8_t v = in[done].value, t = in[done].type;
        int from = in[done].from, cur, ok;
        if (t == T_BOOL) {
            int64_t idx = bool_index(gmd, (uint32_t)id);
            if (idx < 0) { atomic_fetch_add(&n_unknown, 1); continue; }
            if (bool_personal((uint32_t)id)) { atomic_fetch_add(&n_personal_in, 1); continue; }
            cur = bool_value(gmd, idx);
            if (cur == v) { atomic_fetch_add(&n_same, 1); continue; }
            ok = push_queue(gmd, B_QWORD, B_QCAP, B_QENT, v, (uint32_t)id);
        } else {
            int64_t idx = key_index(gmd, id);
            if (idx < 0) { atomic_fetch_add(&n_unknown, 1); continue; }
            if (key_personal(gmd, idx)) { atomic_fetch_add(&n_personal_in, 1); continue; }
            cur = g8(key_entry(gmd, idx) + K_VALUE);
            if (cur == v) { atomic_fetch_add(&n_same, 1); continue; }
            ok = push_queue(gmd, K_QWORD, K_QCAP, K_QENT, v, (uint32_t)idx);
        }
        if (!ok) break;   // the queue is full: the rest next frame
        atomic_fetch_add(&n_applied, 1);
        out_drop(t, id, v);
        if (atomic_fetch_add(&said, 1) < 20)
            rt_log("coop: gamedata: player %d's %s %0*llx = %u applied here (it read %d)", from, t == T_KEY ? "key" : "bool",
                   t == T_KEY ? 16 : 8, (unsigned long long)id, v, cur);
        for (int k = 0; k < VERIFY_N; k++)
            if (!verify[k].frames) { verify[k].id = id, verify[k].type = t, verify[k].value = v, verify[k].from = from, verify[k].frames = 1; break; }
    }
    memmove(in, in + done, (size_t)(in_n - done) * sizeof *in), in_n -= done;
    plat_mutex_unlock(&in_lock);
}

// After calc has run (the next calc's entry): do the changes applied read so? Logs each once, for the test.
static void verify_tick(uint64_t gmd) {
    static _Atomic uint32_t said;
    for (int k = 0; k < VERIFY_N; k++) {
        if (!verify[k].frames || ++verify[k].frames < 3) continue;
        int64_t idx = verify[k].type == T_KEY ? key_index(gmd, verify[k].id) : bool_index(gmd, (uint32_t)verify[k].id);
        int now = idx < 0 ? -1 : verify[k].type == T_KEY ? g8(key_entry(gmd, idx) + K_VALUE) : bool_value(gmd, idx);
        if (atomic_fetch_add(&said, 1) < 20)
            rt_log("coop: gamedata: player %d's %s %0*llx now reads %d here (%s)", verify[k].from, verify[k].type == T_KEY ? "key" : "bool",
                   verify[k].type == T_KEY ? 16 : 8, (unsigned long long)verify[k].id, now, now == verify[k].value ? "as sent" : "NOT as sent");
        verify[k].frames = 0;
    }
}

// GameDataMgr::calc's entry (coop_spawn.c coop_hook), x0 = the manager; me: this game's Link (0 outside the world).
void coop_gamedata_calc(uint64_t gmd, uint64_t me) {
    if (!share_on() || !gmd) return;
    personal_init();
    uint64_t now = plat_now_ns();
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int others = coop_net_on() && coop_net_id() ? coop_net_players(ids, names, COOP_MAX_PLAYERS) : 0;
    if (me != last_me) last_me = me, world_since_ns = now;
    // Shared from 3 s after this game's Link is there (a save's load writes many values first) and while another
    // player is in the session.
    int open = others > 0 && me && now - world_since_ns > 3000000000ull;
    atomic_store(&gate_gmd, open ? gmd : 0);
    atomic_store(&gate, open);
    if (!open) return;
    if (!said_start) said_start = 1, log_start(gmd);
    verify_tick(gmd);
    test_tick(gmd, frame_step_world_us());
    capture(gmd);
    apply(gmd);
    out_flush(now);
    static uint64_t last_stats;
    if (now - last_stats > 30000000000ull) {
        if (last_stats)
            rt_log("coop: gamedata: %u keys and %u bools shared (%u of them event flags), %u packets; %u received: %u applied, "
                   "%u already so, %u the player's here, %u unknown here; full: out %u, in %u",
                   atomic_load(&n_sent_key), atomic_load(&n_sent_bool), atomic_load(&n_sent_event), atomic_load(&n_packets),
                   atomic_load(&n_recv), atomic_load(&n_applied), atomic_load(&n_same), atomic_load(&n_personal_in),
                   atomic_load(&n_unknown), atomic_load(&n_out_full), atomic_load(&n_in_full));
        last_stats = now;
    }
}

// setEventBool's entry (main+0xc6a9f0, coop_spawn.c coop_hook): x0 the bool table (manager +8), w1 the value, x2 the
// key ({+0 a name map: +8 entries {u32 hash, u32 table hash}, +0x14 size; +0xc 1 for an array key}), w3 the name's
// hash. It writes the table directly (no queue), so its write is shared here, after it ran: TotK Online sends its
// writes at once too. Runs the setter itself: 1.
int coop_gamedata_event_bool(Cpu *c, uint64_t addr) {
    static __thread int inside;
    if (inside || !share_on() || !atomic_load(&gate)) return 0;
    uint64_t gmd = atomic_load(&gate_gmd), key = c->x[2];
    uint32_t h = (uint32_t)c->x[3], th = 0;
    uint8_t v = c->x[1] & 1;
    uint64_t map = key ? g64(key) : 0;
    if (gmd && c->x[0] == gmd + 8 && map && h && !g8(key + 0xc)) {   // the name's hash to the table's, as the setter does
        uint64_t ents = g64(map + 8);
        uint32_t size = g32(map + 0x14);
        if (ents && size) {
            uint32_t start = h % size, i = start;
            do {
                uint32_t have = g32(ents + (uint64_t)i * 8);
                if (have == h) { th = g32(ents + (uint64_t)i * 8 + 4); break; }
                if (!have) break;
                i = i + 1 >= size ? 0 : i + 1;
            } while (i != start);
        }
    }
    int64_t idx = th ? bool_index(gmd, th) : -1;
    int was = idx >= 0 ? bool_value(gmd, idx) : -1;
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    if ((c->x[0] & 1) && idx >= 0 && was != v && !bool_personal(th)) {
        out_add(T_BOOL, th, v);
        atomic_fetch_add(&n_sent_event, 1);
        static _Atomic uint32_t said;
        if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: gamedata: event bool %08x = %u in this game, shared", th, v);
    }
    return 1;
}
