// Co-op: shared world progress (opt-in, TOTK_COOP_SHARE_WORLD=1). Each player keeps their own save; with this on, the
// changes one game makes to the world's game data reach the others' games while they play together: the per-placed-
// object flags (Bool64bitKey: opened chests, found Koroks, broken or collected map objects), the world's bool flags
// (quest and event flags), its numbers and states (Int and Enum: shrine states, quest steps, counters, a few Floats),
// but not what belongs to one player (their status, pouch, horses, recipes, compendium, where they are). A shrine
// cleared in another game gives this game's player the Light of Blessing they would have had (blessing_tick), and a
// player who joins late gets the changes the others made earlier in the session (the session log, catch-up below).
//
// How TotK Online does it (1.2.1 only, no licence: read for behaviour, nothing copied): every frame it walks the game
// data manager's pending set requests per value type, sends the shared ones, sends the event-flag setter's writes at
// once, and applies the others' changes by pushing them into the same pending queues, so the game's own deferred set
// (GameDataMgr::calc) writes them. Its server keeps the shared save and gives each client its blessings as
// shrines cleared in the shared save - spent on hearts - spent on stamina. This file does the same in 1.4.3 at the entry
// of GameDataMgr::calc, on its thread:
//   1. capture: the bool and Bool64bitKey pending requests of this game whose value differs from the table's, and the
//      shared numbers whose value differs from the copy kept here (the numbers are compared, not read off their
//      queue: their queue also carries "add" requests, and a compare also sees a direct write), go into the outbox;
//   2. apply: the other games' changes received since (coop_gamedata_take, from coop_world.c's packet loop) are
//      pushed into the same queues after them, so calc writes them and they are not captured again (no echo);
//   3. send: the outbox goes out in "GMDT" packets on the HIT channel (coop_net_hit_send); each change is sent again
//      1, 3 and 7 s later (UDP loses packets; applying a change twice is harmless, calc skips a value already set).
// The event-flag setter writes its table directly, not through the queue, so its writes are taken in its own hook
// (coop_gamedata_event_bool).
//
// Catch-up: every shared change seen in this session (this game's and the ones applied from the others) is kept in a
// session log, one entry per value with its latest value. When a game starts sharing (it joined, or its world was
// loaded again) it says hello; each game that hears it sends that player its session log, twice, 2 s apart. So a
// player who joins late gets what the others changed while playing together, not their whole saves.
//
// Addresses (1.4.3 main; how each was found: docs/coop/proxy-plan.md "Shared world progress", and the hand-over note):
//   main+0xa72b00  GameDataMgr::calc (x0 = the manager): 1.2.1 main+0xee9704, the same code (the same offsets 0xd0c,
//                  0xce0, 0xd10, 0xcb8 and per-type queues); its one caller main+0xa72abc locks the manager's mutex
//                  (+0x70) around it and passes [main+0x3ad66b0], as 1.2.1's main+0xee96cc does.
//   main+0xc6a9f0  setEventBool (x0 = the bool table, manager +8; w1 value; x2 the key; w3 the name hash; w4): 1.2.1
//                  main+0x11616bc, the same direct write (+0x16 value, +0xc dirty, the hash onto the changed ring at
//                  table +0x40); its caller main+0xc6a930 is reached from main+0xc6a834, which looks up the same
//                  constant hash 0x2b05cbb9 as 1.2.1's caller of setEventBool.
//   main+0x9240d4  the bool queue push, main+0x2abcf5c the Bool64bitKey queue push, main+0x16fdcc the Int/IntArray
//                  setter, main+0x170210 the Enum setter, main+0x170490 the Float setter: q_claim / q_commit below do
//                  what their pushes do.
//
// Game data manager layout (1.4.3, the same as 1.2.1; read in calc). Value type t (0 Bool, 1 BoolArray, 2 Int,
// 3 IntArray, 4 Float, 5 FloatArray, 6 Enum, ... 17 String64Array, in the game's order; Bool64bitKey apart) has a block
// at t * 0x60: +0x10 u32 entry count, +0x18 entries, +0x20 hash map ({u32 name hash, u32 index}, open addressing from
// hash % size, linear probe, 0 ends), +0x2c its size, +0x30 u32 queue capacity, +0x38 queue entries, +0x40 u32 queue
// word (bits 0..19 the slots claimed, 20..27 a commit counter, 28..31 a generation; calc clears the count and steps
// the generation when it has written the queue). Per type:
//   Bool    entries 0x18 bytes, the value the byte at +0x16;      queue 8 bytes {u8 value, u32 hash at +4}
//   Int     entries 0x28 bytes, the value the i32 at +0x1c;       queue 12 bytes {i32 value, u32 hash, u32 op} (op 1 adds)
//   Float   entries 0x28 bytes, the value the f32 at +0x1c;       queue 12 bytes {f32 value, u32 hash, u32 op}
//   Enum    entries 0x38 bytes, the value (a hash) the u32 at +0x1c; queue 8 bytes {u32 value, u32 hash}
//   IntArray entries 0x40 bytes: +0x20 u32 size, +0x28 the i32 values; queue 16 bytes {i32 value, i32 index, u32 hash,
//           u32 op}
//   String64Array entries 0x90 bytes: +0x70 u32 size, +0x78 the strings (0x50 bytes: char *text, i32 its capacity at +8);
//           queue 0x58 bytes {char *text (the queue's own buffer), i32 capacity at +8, ..., i32 index at +0x50, u32 hash
//           at +0x54}
// The hash in a queue entry and in the map is the name's MurmurHash3. The Bool64bitKey table is at +0xc58: +0 u32
// count, +8 entries (0x10 bytes: u64 key, sorted unsigned; u32 at +8 the key's reset type, as GameDataList's
// ResetTypeValue; u8 at +0xc the value), +0x10 queue capacity, +0x18 queue entries (8 bytes: u8 value, i32 index at
// +4; -1 is empty), +0x20 the queue word.
//
// What is shared (decided here; written down in docs/coop/proxy-plan.md):
//   - Bool64bitKey: a key whose reset type has none of the bits 0x1af set (0x50, 3537 of the 130375 keys) belongs to
//     the player, the others (0x150, 0x58, 0x5a, 0xd0, 0x52) to the world. This is TotK Online's server rule
//     (bitKeyMap value & 0x1AF); its bitKeyMap is the game's own GameDataList 1.4.0 Bool64bitKey ResetTypeValue, key for
//     key, so the rule reads the game's attribute. The startup line logs the histogram of this game's table.
//   - Bool, Int, Enum: only an entry GameDataList puts in the progress save (SaveFileIndex 0) can be shared: -1 is never
//     saved (the session's own state), 1 is the options save (buttons, language, camera), 2 and up other files. The
//     game's GameDataList.Product.140 is read at start (load_tables) for that. Of those, the player's are left out:
//       * by name: PlayerStatus.* (hearts, stamina, rupees, companions, special power), the last wild horse, the
//         statues' heart and stamina trades, the clock (World_Current*, which coop_players.c syncs by itself), the
//         pouch's sort orders and shortcuts, the map layer and sensor mode, the compendium's totals, Mineru's
//         construct, where the player is or what they ride, the broken Master Sword's flags (personal_names);
//       * by family, the names made from every actor name in the RomFS (Pack/Actor/<name>.pack.zs) and the cooked
//         dishes' names: EquipTime.<item>, EquipmentPouchTime.<item>, EquipmentDeathCount.<item> (the player's own
//         gear), RecipeCard.Content.<dish>.* (their recipe book), PictureBookData.<actor>.* (their compendium: its
//         photos are theirs, so a shared entry would show no photo), OwnedParasailPattern.* (their paraglider
//         fabrics). Checked offline against TotK Online's name list: every EquipTime, EquipmentPouchTime and
//         EquipmentDeathCount entry, 508 of the 509 compendium entries and 699 of the 738 recipe ones are found, and
//         no other entry of the same value type has one of these hashes.
//     Every other progress-save Bool, Int and Enum is shared: quest steps and flags, shrine and lightroot states,
//     Koroks carried, shop stock, enemy kill counts and levels, mini-game records.
//   - Float: only SageOfGerudo_SliderBlockPos1..4 (a puzzle's blocks in the world). The other progress-save floats are
//     the player's (status, horse) or their own mini-game records.
//   - Arrays (BoolArray, IntArray, ...), strings and vectors: not shared. The arrays in the progress save are the
//     pouch, the horses, the album, the recipe log and per-player histories, and an array element has no name of its
//     own to tell a world one from a player one by.
//   TOTK_COOP_SHARE_SKIP adds hashes or keys (hex, comma separated) to keep personal.
// Without GameDataList (no RomFS file, or it does not read) the bools are shared by the names alone, as before, and
// no number is shared.
//
// Lights of Blessing (blessing_tick): when a shrine's DungeonState.DungeonNNN changes to Clear here because of another
// player, this game's player is owed its blessing. As TotK Online works it out: blessings = the shrines cleared here -
// (MaxLife - 12) - (MaxStamina - 1000) / 200 * 4 (a heart is 4 of MaxLife and costs 4 blessings, a fifth of a stamina
// wheel is 200 and costs 4), at least 0. The pouch's Obj_DungeonClearSeal (Pouch.KeyItem.Content.Name and .StockNum)
// is raised to that through the game's own queues, in an empty slot when the player has none; it is never lowered.
//
// Test (TOTK_COOP_TEST_GAMEDATA=A:25, in the game whose TOTK_COOP_NAME is A, from 25 s of world time): a shared
// Bool64bitKey, the bool IsGet.Item_Fruit_A, a shared Int, a shrine's DungeonState (to Clear) and the Float
// SageOfGerudo_SliderBlockPos1 are changed through this game's queues, 2 s apart, then put back from 40 s later.
// TOTK_COOP_TEST_LATE=B:50 keeps the game named B out of the sharing until 50 s (it drops what it is sent), so it joins
// late and gets the changes from A's session log. tools/coop/gamedata_test.ps1 runs both and lists the lines a passing
// run shows.
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "coop_game.h"
#include "coop_net.h"
#include "fs.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define GMD_SLOT 0x3ad66b0   // [main+] = the game data manager ([main+0x395dff0] holds main+0x3ad66b0)
#define GMD_CALC 0xa72b00
#define GMD_EVENT_BOOL 0xc6a9f0

// ---- the manager's per-type blocks
enum { VT_BOOL = 0, VT_INT = 2, VT_INTARR = 3, VT_FLOAT = 4, VT_ENUM = 6, VT_STR64ARR = 17 };
#define TB(t) ((uint32_t)(t) * 0x60u)
#define F_COUNT 0x10
#define F_ENT 0x18
#define F_MAP 0x20
#define F_MSIZE 0x2c
#define F_QCAP 0x30
#define F_QENT 0x38
#define F_QWORD 0x40
#define B_STRIDE 0x18
#define B_VALUE 0x16
#define N_VALUE 0x1c          // Int, Float, Enum: the value
#define IA_STRIDE 0x40
#define IA_SIZE 0x20
#define IA_VALS 0x28
#define SA_STRIDE 0x90
#define SA_SIZE 0x70
#define SA_ELEMS 0x78
#define SA_ELEM 0x50
#define K_COUNT 0xc58
#define K_ENTRIES 0xc60
#define K_STRIDE 0x10
#define K_RESET 8
#define K_VALUE 0xc
#define K_QCAP 0xc68
#define K_QENT 0xc70
#define K_QWORD 0xc78
#define KEY_SHARED_BITS 0x1afu
#define ENUM_CLEAR 0x62965740u   // MurmurHash3("Clear"): DungeonState's value for a cleared shrine

// The wire's types: TotK Online's numbers for Bool, Int, Float, Enum (the game's value types) and Bool64bitKey (0x20);
// 0x40 is this file's hello.
enum { T_BOOL = 0, T_INT = 2, T_FLOAT = 4, T_ENUM = 6, T_KEY = 0x20, T_HELLO = 0x40 };
enum { C_BOOL, C_INT, C_ENUM, C_FLOAT, NCAT };   // the name sets

// ---- the wire: "GMDT" packets on the HIT channel (coop_world.c's packet loop hands them to coop_gamedata_take).
// coop_world.c peer_tick takes HIT packets into a LinkHitPacket (616 bytes) and drops a longer one, so a packet is kept
// to 8 + 36 * 16 = 584 bytes.
#define GD_KIND 0x54444d47u   // "GMDT"
#define GD_VER 2
#define GD_PER_PACKET 36
typedef struct { uint64_t id; uint32_t value; uint8_t type, pad[3]; } GdEntry;   // id: the name's hash, or the key
typedef struct { uint32_t kind; uint8_t n, ver; uint16_t to; GdEntry e[GD_PER_PACKET]; } GdPacket;   // to: 0 for all
_Static_assert(sizeof(GdPacket) <= 616, "GdPacket fits coop_world.c's HIT receive buffer");
_Static_assert(sizeof(GdPacket) <= COOP_HIT_MAX, "GdPacket fits a HIT packet");

// ---- guest memory (guest addresses are host addresses; the game's own fields, valid while the manager lives)
static inline uint8_t g8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline uint32_t g32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline uint64_t g64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline void p32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }

static int share_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_SHARE_WORLD"); on = e && *e && *e != '0'; }
    return on;
}

// MurmurHash3 x86 32-bit, seed 0: the game's hash of game data names (and of an Enum's value names).
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

// ---- the player's entries by name (all 1.4.3 GameDataList entries, checked by hash)
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
static const char *const personal_numbers[] = {   // Int and Enum
#define COMPANION(n) "PlayerStatus.Companion." n ".ForUI.RemainingMinute", "PlayerStatus.Companion." n ".ForUI.RemainingSecond", \
    "PlayerStatus.Companion." n ".JoiningCondition"
    COMPANION("Wind"), COMPANION("Fire"), COMPANION("Electric"), COMPANION("Water"), COMPANION("Soul"),
#undef COMPANION
    "PlayerStatus.Life", "PlayerStatus.MaxLife", "PlayerStatus.MaxLifeForBeforeVSGanon", "PlayerStatus.BreakLife",
    "PlayerStatus.ExtraLife", "PlayerStatus.TemporaryLife", "PlayerStatus.TemporaryBreakLife",
    "PlayerStatus.TemporaryExtraLife", "PlayerStatus.CookBuff", "PlayerStatus.CookBuffLv", "PlayerStatus.CurrentMamo",
    "PlayerStatus.CurrentRupee", "PlayerStatus.WeaponAttachCount", "PlayerStatus.CurrentSpecialPower",
    "PlayerStatus.ParasailPattern",
    "LastWildHorse.ChargeNum", "LastWildHorse.ColorType", "LastWildHorse.FootType", "LastWildHorse.HorsePower",
    "LastWildHorse.HorseType", "LastWildHorse.Speed", "LastWildHorse.Toughness", "LastWildHorse.Body.EyeColor",
    "LastWildHorse.Body.Pattern", "LastWildHorse.Mane", "LastWildHorse.Rein", "LastWildHorse.Saddle",
    "Npc_Goddess_UtuwaSum", "NpcDemonStatue_UtuwaSum",   // the statues' heart and stamina trades
    "World_CurrentDays", "World_CurrentHour", "World_CurrentMinute",
    "MapData.CurrentLayer", "SensorMode", "MaterialShortCut.Category", "MaterialShortCut.PouchIndex",
    "SortPattern.Pouch_Armor", "SortPattern.Pouch_Bow", "SortPattern.Pouch_Food", "SortPattern.Pouch_KeyItem",
    "SortPattern.Pouch_Material", "SortPattern.Pouch_Shield", "SortPattern.Pouch_SpecialParts",
    "SortPattern.Pouch_Weapon", "SortPattern.ShortCut_Bow", "SortPattern.ShortCut_Material",
    "SortPattern.ShortCut_Shield", "SortPattern.ShortCut_Weapon",
    "PictureBook_CompleteRate", "PictureBook_RegisteredNum_Enemy",
#define GOLEM(n) "RaumiGolemAttachment." n ".Life", "RaumiGolemAttachment." n ".RaumiControlLife"
    GOLEM("Tool"), GOLEM("SubTool"), GOLEM("Weapon"), GOLEM("SubWeapon"),
#undef GOLEM
};
static const char *const shared_floats[] = {
    "SageOfGerudo_SliderBlockPos1", "SageOfGerudo_SliderBlockPos2", "SageOfGerudo_SliderBlockPos3",
    "SageOfGerudo_SliderBlockPos4",
};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

// ---- name sets: sorted u32 hashes
typedef struct { uint32_t *h; int n, cap; } HSet;
static void hs_add(HSet *s, uint32_t h) {
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 256;
        uint32_t *p = realloc(s->h, (size_t)cap * 4);
        if (!p) return;
        s->h = p, s->cap = cap;
    }
    s->h[s->n++] = h;
}
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void hs_sort(HSet *s) {
    if (!s->n) return;
    qsort(s->h, (size_t)s->n, 4, cmp_u32);
    int o = 1;
    for (int i = 1; i < s->n; i++)
        if (s->h[i] != s->h[o - 1]) s->h[o++] = s->h[i];
    s->n = o;
}
static int hs_has(const HSet *s, uint32_t h) {
    int lo = 0, hi = s->n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->h[mid] == h) return 1;
        if (s->h[mid] < h) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static HSet personal[NCAT];   // the player's by name or family
static HSet saved[NCAT];      // in the progress save (GameDataList SaveFileIndex 0)
static HSet floats_ok;
static uint32_t dungeon_hash[200];   // DungeonState.DungeonNNN
static int gdl_ok;                   // GameDataList read: saved[] holds the progress save's entries
static int n_actors, n_family;
static _Atomic int tables_ready;     // load_tables has run
#define SKIP_MAX 64
static uint64_t skip_ids[SKIP_MAX];
static int nskip;
static int skipped(uint64_t id) {
    for (int i = 0; i < nskip; i++)
        if (skip_ids[i] == id) return 1;
    return 0;
}

// Shared by the rule in the header comment; cat C_BOOL, C_INT, C_ENUM or C_FLOAT, h the name's hash.
static int hash_shared(int cat, uint32_t h) {
    if (!h || skipped(h) || hs_has(&personal[cat], h)) return 0;
    if (!gdl_ok) return cat == C_BOOL;
    if (!hs_has(&saved[cat], h)) return 0;
    return cat != C_FLOAT || hs_has(&floats_ok, h);
}

// ---- GameDataList: the RomFS file decompressed with its dictionary from Pack/ZsDic.pack.zs, a big-endian BYML
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// The SARC member k of p[0..n): its name, start and size; NULL past the last.
static const char *sarc_at(const uint8_t *p, size_t n, unsigned k, size_t *at, size_t *size) {
    if (n < 0x14 || memcmp(p, "SARC", 4)) return NULL;
    uint16_t head, count;
    memcpy(&head, p + 4, 2);
    uint32_t data = le32(p + 12);
    if ((size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return NULL;
    memcpy(&count, p + head + 6, 2);
    size_t names = (size_t)head + 12 + (size_t)count * 16;
    if (k >= count || names + 8 > n || memcmp(p + names, "SFNT", 4)) return NULL;
    names += 8;
    size_t e = (size_t)head + 12 + (size_t)k * 16;
    uint32_t attr = le32(p + e + 4), lo = le32(p + e + 8), hi = le32(p + e + 12);
    size_t s = names + (size_t)(attr & 0xffffff) * 4;
    if (!(attr >> 24) || s >= n || !memchr(p + s, 0, n - s) || lo > hi || (size_t)data + hi > n) return NULL;
    *at = data + lo, *size = hi - lo;
    return (const char *)p + s;
}

static uint8_t *read_zs(const char *rel, size_t *out_n) {
    size_t n, dn = 0;
    uint8_t *dicts = NULL, *raw = NULL, *in = NULL, *z = romfs_read_whole("Pack/ZsDic.pack.zs", &n, "shared world numbers");
    unsigned long long r = z ? ZSTD_getFrameContentSize(z, n) : 0;
    if (z && r != ZSTD_CONTENTSIZE_UNKNOWN && r != ZSTD_CONTENTSIZE_ERROR && r < (64u << 20) && (dicts = malloc((size_t)r)) &&
        ZSTD_decompress(dicts, (size_t)r, z, n) == r)
        dn = (size_t)r;
    free(z);
    if (dn && (in = romfs_read_whole(rel, &n, "shared world numbers"))) {
        unsigned long long rn = ZSTD_getFrameContentSize(in, n);
        unsigned id = ZSTD_getDictID_fromFrame(in, n);
        ZSTD_DDict *dd = NULL;
        size_t at, sz;
        for (unsigned k = 0; id && !dd && sarc_at(dicts, dn, k, &at, &sz); k++)
            if (ZSTD_getDictID_fromDict(dicts + at, sz) == id) dd = ZSTD_createDDict(dicts + at, sz);
        if (rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn < (64u << 20) && (!id || dd) &&
            (raw = malloc((size_t)rn))) {
            ZSTD_DCtx *dc = ZSTD_createDCtx();
            size_t got = !dc ? 0 : dd ? ZSTD_decompress_usingDDict(dc, raw, (size_t)rn, in, n, dd)
                                      : ZSTD_decompressDCtx(dc, raw, (size_t)rn, in, n);
            ZSTD_freeDCtx(dc);
            if (ZSTD_isError(got) || got != rn) free(raw), raw = NULL;
            else *out_n = (size_t)rn;
        }
        ZSTD_freeDDict(dd);
    }
    free(in);
    free(dicts);
    return raw;
}

typedef struct { const uint8_t *d; size_t n; uint32_t keys; } Byml;
enum { BY_ARR = 0xc0, BY_DICT = 0xc1, BY_TAB = 0xc2, BY_INT = 0xd1, BY_UINT = 0xd3 };
static uint32_t by_count(const Byml *b, uint32_t off, int type) {
    return (size_t)off + 4 <= b->n && b->d[off] == type ? be32(b->d + off) & 0xffffff : 0;
}
static const char *by_key(const Byml *b, uint32_t i) {
    uint32_t tab = b->keys;
    if (i >= by_count(b, tab, BY_TAB) || (size_t)tab + 8 + 4 * (size_t)i > b->n) return NULL;
    size_t o = (size_t)tab + be32(b->d + tab + 4 + 4 * (size_t)i);
    return o < b->n && memchr(b->d + o, 0, b->n - o) ? (const char *)b->d + o : NULL;
}
static int by_get(const Byml *b, uint32_t off, const char *key, int *t, uint32_t *v) {
    uint32_t n = by_count(b, off, BY_DICT);
    for (uint32_t i = 0; i < n; i++) {
        size_t e = (size_t)off + 4 + 8 * (size_t)i;
        if (e + 8 > b->n) return 0;
        const char *k = by_key(b, be32(b->d + e) >> 8);
        if (k && !strcmp(k, key)) { *t = b->d[e + 3], *v = be32(b->d + e + 4); return 1; }
    }
    return 0;
}

// saved[cat] from GameDataList's Data.<type>[]: the hashes whose SaveFileIndex is 0; how many entries it had.
static int gdl_type(const Byml *b, uint32_t data, const char *type, HSet *out, int *total) {
    int t;
    uint32_t arr;
    if (!by_get(b, data, type, &t, &arr) || t != BY_ARR) return 0;
    uint32_t n = by_count(b, arr, BY_ARR);
    size_t vals = (size_t)arr + 4 + ((n + 3) & ~3u);
    if (vals + 4 * (size_t)n > b->n) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = be32(b->d + vals + 4 * i), h, sfi;
        int th, ts;
        if (by_get(b, e, "Hash", &th, &h) && (th == BY_UINT || th == BY_INT) && by_get(b, e, "SaveFileIndex", &ts, &sfi) &&
            ts == BY_INT && sfi == 0)
            hs_add(out, h);
    }
    *total = (int)n;
    hs_sort(out);
    return 1;
}

static void load_gdl(void) {
    size_t n = 0;
    uint8_t *raw = read_zs("GameData/GameDataList.Product.140.byml.zs", &n);
    Byml b = {raw, n, 0};
    int t, total[NCAT] = {0};
    uint32_t data, root;
    if (raw && n >= 16 && !memcmp(raw, "BY", 2)) b.keys = be32(raw + 4), root = be32(raw + 12);
    if (!raw || !b.keys || !by_get(&b, root, "Data", &t, &data) || t != BY_DICT ||
        !gdl_type(&b, data, "Bool", &saved[C_BOOL], &total[C_BOOL]) || !gdl_type(&b, data, "Int", &saved[C_INT], &total[C_INT]) ||
        !gdl_type(&b, data, "Enum", &saved[C_ENUM], &total[C_ENUM]) ||
        !gdl_type(&b, data, "Float", &saved[C_FLOAT], &total[C_FLOAT])) {
        rt_log("coop: gamedata: GameDataList.Product.140 not read (%s): bools shared by name, numbers not shared",
               raw ? "not the BYML expected" : "missing or not decompressed");
        free(raw);
        return;
    }
    gdl_ok = 1;
    rt_log("coop: gamedata: GameDataList: in the progress save %d of %d bools, %d of %d Ints, %d of %d Enums, %d of %d Floats",
           saved[C_BOOL].n, total[C_BOOL], saved[C_INT].n, total[C_INT], saved[C_ENUM].n, total[C_ENUM], saved[C_FLOAT].n,
           total[C_FLOAT]);
    free(raw);
}

// The personal families from every actor name and dish name (header comment).
static void family(HSet *s, const char *fmt, const char *name) {
    char buf[256];
    if (snprintf(buf, sizeof buf, fmt, name) < (int)sizeof buf) hs_add(s, mm3(buf)), n_family++;
}
static void family_names(const char *a) {
    family(&personal[C_INT], "EquipTime.%s", a);
    family(&personal[C_INT], "EquipmentPouchTime.%s", a);
    family(&personal[C_INT], "EquipmentDeathCount.%s", a);
    family(&personal[C_INT], "RecipeCard.Content.%s.LatestLogIndex", a);
    family(&personal[C_BOOL], "RecipeCard.Content.%s.IsCooked", a);
    family(&personal[C_BOOL], "RecipeCard.Content.%s.IsNew", a);
}
static void load_names(void) {
    for (int i = 0; i < COUNT(personal_bools); i++) hs_add(&personal[C_BOOL], mm3(personal_bools[i]));
    for (int i = 0; i < COUNT(personal_numbers); i++)
        hs_add(&personal[C_INT], mm3(personal_numbers[i])), hs_add(&personal[C_ENUM], mm3(personal_numbers[i]));
    for (int i = 0; i < COUNT(shared_floats); i++) hs_add(&floats_ok, mm3(shared_floats[i]));
    char buf[256];
    for (uint64_t i = 0, n = romfs_file_count(); i < n; i++) {
        const char *p = romfs_entry(i, NULL, NULL);
        if (!p) continue;
        if (*p == '/') p++;
        if (strncmp(p, "Pack/Actor/", 11)) continue;
        p += 11;
        const char *dot = strstr(p, ".pack");
        size_t len = dot ? (size_t)(dot - p) : 0;
        if (!len || len >= 200 || strchr(p, '/')) continue;
        memcpy(buf, p, len), buf[len] = 0;
        n_actors++;
        family_names(buf);
        family(&personal[C_ENUM], "PictureBookData.%s.State", buf);
        family(&personal[C_BOOL], "PictureBookData.%s.IsNew", buf);
    }
    for (char c = 'A'; c <= 'Z'; c++)   // the cooked dishes: one actor, Item_Cook_<letter>_<nn> recipes
        for (int k = 0; k < 100; k++) snprintf(buf, sizeof buf, "Item_Cook_%c_%02d", c, k), family_names(buf);
    hs_add(&personal[C_BOOL], mm3("OwnedParasailPattern.Default"));
    for (int k = 0; k < 100; k++) snprintf(buf, sizeof buf, "OwnedParasailPattern.Pattern%02d", k), hs_add(&personal[C_BOOL], mm3(buf));
    for (int k = 0; k < 200; k++) snprintf(buf, sizeof buf, "DungeonState.Dungeon%03d", k), dungeon_hash[k] = mm3(buf);
    for (int c = 0; c < NCAT; c++) hs_sort(&personal[c]);
    hs_sort(&floats_ok);
    const char *e = cfg_get("TOTK_COOP_SHARE_SKIP");
    while (e && *e && nskip < SKIP_MAX) {
        char *end;
        unsigned long long v = strtoull(e, &end, 16);
        if (end == e) break;
        skip_ids[nskip++] = v;
        e = *end == ',' ? end + 1 : end;
    }
}
static void load_tables(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns();
    load_names();
    load_gdl();
    rt_log("coop: gamedata: the player's entries by family: %d names from %d actors and the cooked dishes (%.0f ms)", n_family,
           n_actors, (double)(plat_now_ns() - t0) / 1e6);
    atomic_store(&tables_ready, 1);
}

// ---- the tables
// An entry's index in value type vt's table from its name hash, -1 when it has none.
static int64_t map_index(uint64_t gmd, int vt, uint32_t h) {
    uint64_t b = gmd + TB(vt), map = g64(b + F_MAP);
    uint32_t size = g32(b + F_MSIZE), count = g32(b + F_COUNT);
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
static int bool_value(uint64_t gmd, int64_t idx) { return g8(g64(gmd + F_ENT) + (uint64_t)idx * B_STRIDE + B_VALUE); }
static int bool_shared(uint32_t h) { return hash_shared(C_BOOL, h); }
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

// A queue slot claimed as the game's pushes do (main+0x9240d4, main+0x2abcf5c, main+0x16fdcc): raise the count, get
// the slot; the caller writes it and calls q_commit, which steps the commit counter. 0 when the queue is full (the rest
// waits for the next frame).
static uint64_t q_claim(uint64_t gmd, uint32_t word_off, uint32_t cap_off, uint32_t ent_off, uint32_t stride) {
    uint32_t *w = (uint32_t *)(uintptr_t)(gmd + word_off), cap = g32(gmd + cap_off);
    uint32_t old = __atomic_load_n(w, __ATOMIC_ACQUIRE), n;
    uint64_t ents = g64(gmd + ent_off);
    if (!ents) return 0;
    for (;;) {
        n = old & 0xfffff;
        if (n >= cap) return 0;
        if (__atomic_compare_exchange_n(w, &old, old + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    return ents + (uint64_t)n * stride;
}
static void q_commit(uint64_t gmd, uint32_t word_off) {
    uint32_t *w = (uint32_t *)(uintptr_t)(gmd + word_off);
    uint32_t cur = __atomic_load_n(w, __ATOMIC_ACQUIRE), nw;
    do nw = (cur & 0x0ff00000u) == 0x0ff00000u ? (cur & 0xf00fffffu) : cur + 0x100000u;
    while (!__atomic_compare_exchange_n(w, &cur, nw, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}
#define Q_OF(vt) TB(vt) + F_QWORD, TB(vt) + F_QCAP, TB(vt) + F_QENT
static int push_bool(uint64_t gmd, uint8_t v, uint32_t h) {
    uint64_t e = q_claim(gmd, Q_OF(VT_BOOL), 8);
    if (!e) return 0;
    *(volatile uint8_t *)(uintptr_t)e = v;
    p32(e + 4, h);
    q_commit(gmd, TB(VT_BOOL) + F_QWORD);
    return 1;
}
static int push_key(uint64_t gmd, uint8_t v, int64_t idx) {
    uint64_t e = q_claim(gmd, K_QWORD, K_QCAP, K_QENT, 8);
    if (!e) return 0;
    *(volatile uint8_t *)(uintptr_t)e = v;
    p32(e + 4, (uint32_t)idx);
    q_commit(gmd, K_QWORD);
    return 1;
}

// ---- the numbers: a copy of each table's values, compared every frame (header comment)
typedef struct {
    int cat, vt, wire;
    uint32_t stride, qstride;
    const char *name;
    uint32_t n, nshared, npersonal, nunsaved;
    uint32_t *hash, *shadow;
    uint8_t *shared;
} NumTab;
static NumTab nt[3] = {
    {.cat = C_INT, .vt = VT_INT, .wire = T_INT, .stride = 0x28, .qstride = 12, .name = "Int"},
    {.cat = C_ENUM, .vt = VT_ENUM, .wire = T_ENUM, .stride = 0x38, .qstride = 8, .name = "Enum"},
    {.cat = C_FLOAT, .vt = VT_FLOAT, .wire = T_FLOAT, .stride = 0x28, .qstride = 12, .name = "Float"},
};
static NumTab *tab_of(int wire) {
    for (int i = 0; i < 3; i++)
        if (nt[i].wire == wire) return &nt[i];
    return NULL;
}
static uint32_t num_value(uint64_t gmd, const NumTab *t, int64_t idx) {
    return g32(g64(gmd + TB(t->vt) + F_ENT) + (uint64_t)idx * t->stride + N_VALUE);
}
static int push_num(uint64_t gmd, const NumTab *t, uint32_t h, uint32_t v) {
    uint64_t e = q_claim(gmd, Q_OF(t->vt), t->qstride);
    if (!e) return 0;
    p32(e, v), p32(e + 4, h);
    if (t->qstride >= 12) p32(e + 8, 0);   // op 0: set
    q_commit(gmd, TB(t->vt) + F_QWORD);
    return 1;
}
// The copies, made when sharing starts (the save's values: not changes).
static void snapshot(uint64_t gmd) {
    for (int k = 0; k < 3; k++) {
        NumTab *t = &nt[k];
        uint64_t b = gmd + TB(t->vt), map = g64(b + F_MAP);
        uint32_t n = g32(b + F_COUNT), size = g32(b + F_MSIZE);
        if (n > 1u << 20) n = 0;
        uint32_t *hash = realloc(t->hash, (size_t)(n ? n : 1) * 4), *shadow = realloc(t->shadow, (size_t)(n ? n : 1) * 4);
        uint8_t *shared = realloc(t->shared, n ? n : 1);
        if (!hash || !shadow || !shared) { free(hash), free(shadow), free(shared); t->hash = t->shadow = NULL, t->shared = NULL, t->n = 0; continue; }
        t->hash = hash, t->shadow = shadow, t->shared = shared, t->n = n;
        memset(hash, 0, (size_t)n * 4);
        for (uint32_t i = 0; map && i < size; i++) {
            uint32_t h = g32(map + (uint64_t)i * 8), idx = g32(map + (uint64_t)i * 8 + 4);
            if (h && idx < n) hash[idx] = h;
        }
        t->nshared = t->npersonal = t->nunsaved = 0;
        for (uint32_t i = 0; i < n; i++) {
            shared[i] = hash_shared(t->cat, hash[i]);
            shadow[i] = num_value(gmd, t, i);
            t->nshared += shared[i];
            if (!shared[i] && gdl_ok && !hs_has(&saved[t->cat], hash[i])) t->nunsaved++;
            else if (!shared[i]) t->npersonal++;
        }
    }
}

// ---- the session log: every shared change seen in this session, its latest value (catch-up, header comment)
#define SLOG_N 32768   // a power of 2
static struct { uint64_t id; uint32_t value; uint8_t type, used; } slog[SLOG_N];
static int slog_n;
static PlatMutex slog_lock = PLAT_MUTEX_INIT;
static void slog_set(uint8_t type, uint64_t id, uint32_t value) {
    plat_mutex_lock(&slog_lock);
    uint64_t x = (id ^ (uint64_t)type << 56) * 0x9e3779b97f4a7c15ull;
    for (uint32_t i = (uint32_t)(x >> 49), k = 0; k < SLOG_N; k++, i = (i + 1) & (SLOG_N - 1)) {
        if (slog[i].used && slog[i].id == id && slog[i].type == type) { slog[i].value = value; break; }
        if (!slog[i].used) {
            if (slog_n >= SLOG_N * 3 / 4) break;   // full: the change goes out live but is not caught up
            slog[i].used = 1, slog[i].id = id, slog[i].type = type, slog[i].value = value, slog_n++;
            break;
        }
    }
    plat_mutex_unlock(&slog_lock);
}

// ---- outbox: this game's shared changes, each sent at once and again 1, 3 and 7 s later; a value changed again
// within 1 s of being sent waits for that second (a counter that changes every frame is sent once a second). Locked:
// the event-flag hook may run on another thread than calc.
#define OUT_N 2048
static const uint64_t resend_ns[] = {1000000000ull, 2000000000ull, 4000000000ull};
static struct { uint64_t id, next_ns, last_ns; uint32_t value; uint8_t type, sends; } out[OUT_N];
static int out_n;
static PlatMutex out_lock = PLAT_MUTEX_INIT;
static _Atomic uint32_t n_sent_key, n_sent_bool, n_sent_event, n_sent_num, n_out_full, n_packets;

static void out_add(uint8_t type, uint64_t id, uint32_t value) {
    if (type != T_HELLO) slog_set(type, id, value);
    plat_mutex_lock(&out_lock);
    int i = 0;
    while (i < out_n && !(out[i].type == type && out[i].id == id)) i++;
    if (i == out_n) {
        if (out_n == OUT_N) { plat_mutex_unlock(&out_lock); atomic_fetch_add(&n_out_full, 1); return; }
        out_n++;
        out[i].last_ns = 0;
    }
    out[i].type = type, out[i].id = id, out[i].value = value, out[i].sends = 0;
    out[i].next_ns = out[i].last_ns ? out[i].last_ns + 1000000000ull : 0;
    plat_mutex_unlock(&out_lock);
}
// A change from another game for an id this game also has in its outbox, with another value: theirs is the later
// one, so this game stops sending its own (else the resends would undo theirs).
static void out_drop(uint8_t type, uint64_t id, uint32_t value) {
    plat_mutex_lock(&out_lock);
    for (int i = 0; i < out_n; i++)
        if (out[i].type == type && out[i].id == id && out[i].value != value) { out[i] = out[--out_n]; break; }
    plat_mutex_unlock(&out_lock);
}

// The catch-up being sent: the session log, from cursor, to player `to` (0: everyone, when two joined at once).
static struct { int on, to, pass; uint32_t cursor, sent; uint64_t next_ns; } cu;
static uint64_t hello_ns[COOP_MAX_PLAYERS + 1];

static void catchup_start(int from, uint64_t now) {
    if (from < 1 || from > COOP_MAX_PLAYERS) return;
    if (hello_ns[from] && now - hello_ns[from] < 10000000000ull) return;   // the hello's resends
    hello_ns[from] = now;
    cu.to = cu.on && cu.to != from ? 0 : from;
    cu.on = 1, cu.pass = 0, cu.cursor = 0, cu.sent = 0, cu.next_ns = now;
    plat_mutex_lock(&slog_lock);
    int n = slog_n;
    plat_mutex_unlock(&slog_lock);
    rt_log("coop: gamedata: player %d says hello: catching %s up on the %d changes of this session", from,
           cu.to ? "them" : "everyone", n);
}

static void out_flush(uint64_t now) {
    static GdPacket p;
    int budget = 4;   // packets a frame: the HIT queue (64) carries hits and poses too
    plat_mutex_lock(&out_lock);
    for (; budget > 0; budget--) {
        p.kind = GD_KIND, p.ver = GD_VER, p.n = 0, p.to = 0;
        for (int i = 0; i < out_n && p.n < GD_PER_PACKET; i++) {
            if (out[i].next_ns > now) continue;
            p.e[p.n++] = (GdEntry){.id = out[i].id, .value = out[i].value, .type = out[i].type};
            if (out[i].sends == 0) {
                uint8_t t = out[i].type;
                if (t != T_HELLO) atomic_fetch_add(t == T_KEY ? &n_sent_key : t == T_BOOL ? &n_sent_bool : &n_sent_num, 1);
            }
            out[i].last_ns = now;
            if (out[i].sends < sizeof resend_ns / sizeof *resend_ns) out[i].next_ns = now + resend_ns[out[i].sends++];
            else out[i--] = out[--out_n];
        }
        if (!p.n) break;
        coop_net_hit_send(&p, (int)(offsetof(GdPacket, e) + p.n * sizeof(GdEntry)));
        atomic_fetch_add(&n_packets, 1);
    }
    plat_mutex_unlock(&out_lock);
    // the catch-up, with what is left of this frame's packets
    while (cu.on && budget > 0 && now >= cu.next_ns) {
        p.kind = GD_KIND, p.ver = GD_VER, p.n = 0, p.to = (uint16_t)cu.to;
        plat_mutex_lock(&slog_lock);
        while (cu.cursor < SLOG_N && p.n < GD_PER_PACKET) {
            uint32_t i = cu.cursor++;
            if (slog[i].used) p.e[p.n++] = (GdEntry){.id = slog[i].id, .value = slog[i].value, .type = slog[i].type};
        }
        plat_mutex_unlock(&slog_lock);
        if (p.n) {
            coop_net_hit_send(&p, (int)(offsetof(GdPacket, e) + p.n * sizeof(GdEntry)));
            atomic_fetch_add(&n_packets, 1), budget--, cu.sent += p.n;
        }
        if (cu.cursor >= SLOG_N) {
            rt_log("coop: gamedata: catch-up for %s%d: %u changes sent (pass %d of 2)", cu.to ? "player " : "everyone",
                   cu.to, cu.sent, cu.pass + 1);
            if (++cu.pass >= 2) cu.on = 0;
            else cu.cursor = 0, cu.sent = 0, cu.next_ns = now + 2000000000ull;
        }
    }
}

// ---- inbox: the other games' changes, filled by coop_gamedata_take, applied at the next calc
#define IN_N 16384
static struct { uint64_t id; uint32_t value; uint8_t type, catchup; int from; } in[IN_N];
static int in_n;
static PlatMutex in_lock = PLAT_MUTEX_INIT;
static _Atomic uint32_t n_recv, n_applied, n_same, n_personal_in, n_unknown, n_in_full, n_catchup_in, n_late_drop, n_not_mine;
static _Atomic int late_now;   // the test's late join: this game is not in the sharing yet

int coop_gamedata_take(const void *data, int n, int from) {   // coop_world.c's packet loop: 1 when it was ours
    const GdPacket *p = data;
    if (n < (int)offsetof(GdPacket, e) || p->kind != GD_KIND) return 0;
    if (p->ver != GD_VER || p->n < 1 || p->n > GD_PER_PACKET || n != (int)(offsetof(GdPacket, e) + p->n * sizeof(GdEntry))) return 1;
    if (!share_on()) return 1;
    if (atomic_load(&late_now)) { atomic_fetch_add(&n_late_drop, p->n); return 1; }
    if (p->to && p->to != coop_net_id()) { atomic_fetch_add(&n_not_mine, p->n); return 1; }
    plat_mutex_lock(&in_lock);
    for (int i = 0; i < p->n; i++) {
        uint8_t t = p->e[i].type;
        if (t != T_BOOL && t != T_KEY && t != T_INT && t != T_ENUM && t != T_FLOAT && t != T_HELLO) continue;
        if (in_n == IN_N) { atomic_fetch_add(&n_in_full, 1); continue; }
        in[in_n].id = p->e[i].id, in[in_n].type = t, in[in_n].from = from, in[in_n].catchup = p->to != 0;
        in[in_n].value = t == T_BOOL || t == T_KEY ? p->e[i].value & 1 : p->e[i].value;
        in_n++;
        if (t != T_HELLO) atomic_fetch_add(&n_recv, 1);
        if (p->to) atomic_fetch_add(&n_catchup_in, 1);
    }
    plat_mutex_unlock(&in_lock);
    return 1;
}

// ---- state shared by the hooks
static _Atomic int gate;              // 1 while this game shares: in a session with other players, in the world, settled
static _Atomic uint64_t gate_gmd;     // the manager while the gate is open
static uint64_t world_since_ns, last_me;
static int said_start, was_open, bless_pending;
#define VERIFY_N 16
static struct { uint64_t id; uint32_t value; uint8_t type; int from, frames; } verify[VERIFY_N];   // applied, checked after calc

static const char *type_name(int t) {
    return t == T_KEY ? "key" : t == T_BOOL ? "bool" : t == T_INT ? "int" : t == T_ENUM ? "enum" : t == T_FLOAT ? "float" : "?";
}
static int id_width(int t) { return t == T_KEY ? 16 : 8; }

static void log_start(uint64_t gmd) {
    uint32_t nk = g32(gmd + K_COUNT), nb = g32(gmd + F_COUNT), shared = 0, bshared = 0;
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
    uint64_t map = g64(gmd + F_MAP);
    for (uint32_t i = 0, size = g32(gmd + F_MSIZE); map && i < size; i++) {
        uint32_t bh = g32(map + (uint64_t)i * 8);
        if (bh) bshared += bool_shared(bh);
    }
    char line[400];
    int len = 0;
    for (int j = 0; j < nh; j++) len += snprintf(line + len, sizeof line - len, "%s%#x x%u", j ? ", " : "", h[j].v, h[j].n);
    rt_log("coop: gamedata: sharing world progress (manager %llx): %u Bool64bitKey keys, %u shared by reset type and %u "
           "the player's; %u bools, %u shared; queues %u bools, %u keys",
           (unsigned long long)gmd, nk, shared, nk - shared, nb, bshared, g32(gmd + TB(VT_BOOL) + F_QCAP), g32(gmd + K_QCAP));
    rt_log("coop: gamedata: Bool64bitKey reset types here: %s%s (GameDataList 1.4.0: 0x150 x100751, 0x58 x22312, "
           "0x50 x3537, 0x5a x1846, 0xd0 x982, 0x52 x947)", line, other ? " and more" : "");
    for (int k = 0; k < 3; k++)
        rt_log("coop: gamedata: %s: %u entries, %u shared, %u the player's, %u not in the progress save; queue %u", nt[k].name,
               nt[k].n, nt[k].nshared, nt[k].npersonal, nt[k].nunsaved, g32(gmd + TB(nt[k].vt) + F_QCAP));
}

// ---- Lights of Blessing (header comment)
static int64_t dungeon_of(uint32_t h) {
    for (int k = 0; k < 200; k++)
        if (dungeon_hash[k] == h) return k;
    return -1;
}
static void blessing_tick(uint64_t gmd) {
    static uint32_t h_life, h_sta, h_name, h_stock;
    if (!h_life)
        h_life = mm3("PlayerStatus.MaxLife"), h_sta = mm3("PlayerStatus.MaxStamina"), h_name = mm3("Pouch.KeyItem.Content.Name"),
        h_stock = mm3("Pouch.KeyItem.Content.StockNum");
    const NumTab *te = tab_of(T_ENUM), *ti = tab_of(T_INT), *tf = tab_of(T_FLOAT);
    int clears = 0, shrines = 0;
    for (int k = 0; k < 200; k++) {
        int64_t idx = map_index(gmd, VT_ENUM, dungeon_hash[k]);
        if (idx < 0) continue;
        shrines++;
        clears += num_value(gmd, te, idx) == ENUM_CLEAR;
    }
    int64_t il = map_index(gmd, VT_INT, h_life), is = map_index(gmd, VT_FLOAT, h_sta);
    int64_t in_ = map_index(gmd, VT_STR64ARR, h_name), ik = map_index(gmd, VT_INTARR, h_stock);
    if (il < 0 || is < 0 || in_ < 0 || ik < 0 || !shrines) {
        rt_log("coop: gamedata: blessings: not worked out (MaxLife %lld, MaxStamina %lld, pouch names %lld, counts %lld, %d shrines)",
               (long long)il, (long long)is, (long long)in_, (long long)ik, shrines);
        return;
    }
    int32_t life = (int32_t)num_value(gmd, ti, il);
    uint32_t sbits = num_value(gmd, tf, is);
    float sta;
    memcpy(&sta, &sbits, 4);
    int hearts = life - 12, vessels = (int)((sta - 1000.0f) / 200.0f);
    int spent = (hearts > 0 ? hearts : 0) + (vessels > 0 ? vessels : 0) * 4, want = clears - spent;
    if (want < 0) want = 0;
    uint64_t ne = g64(gmd + TB(VT_STR64ARR) + F_ENT) + (uint64_t)in_ * SA_STRIDE, ke = g64(gmd + TB(VT_INTARR) + F_ENT) + (uint64_t)ik * IA_STRIDE;
    uint32_t nn = g32(ne + SA_SIZE), nk = g32(ke + IA_SIZE);
    uint64_t names = g64(ne + SA_ELEMS), vals = g64(ke + IA_VALS);
    int slot = -1, empty = -1;
    for (uint32_t i = 0; names && i < nn && i < nk; i++) {
        const char *s = (const char *)(uintptr_t)g64(names + (uint64_t)i * SA_ELEM);
        if (!s) continue;
        if (!strcmp(s, "Obj_DungeonClearSeal")) { slot = (int)i; break; }
        if (empty < 0 && !s[0]) empty = (int)i;
    }
    int32_t have = slot >= 0 && vals ? (int32_t)g32(vals + (uint64_t)slot * 4) : 0;
    char why[96];
    snprintf(why, sizeof why, "%d of %d shrines cleared here, %d hearts and %d stamina vessels bought", clears, shrines,
             hearts / 4, vessels);
    if (want <= have) {
        rt_log("coop: gamedata: blessings: %s, so %d; the pouch has %d: left so", why, want, have);
        return;
    }
    int at = slot >= 0 ? slot : empty;
    if (at < 0) { rt_log("coop: gamedata: blessings: %s, so %d; no empty key item slot: not given", why, want); return; }
    if (slot < 0) {   // the name into the empty slot first, through the String64Array queue (its own text buffer)
        uint64_t e = q_claim(gmd, Q_OF(VT_STR64ARR), 0x58);
        if (!e) return;
        char *buf = (char *)(uintptr_t)g64(e);
        int32_t cap = (int32_t)g32(e + 8);
        if (buf && cap > (int)sizeof "Obj_DungeonClearSeal") memcpy(buf, "Obj_DungeonClearSeal", sizeof "Obj_DungeonClearSeal");
        p32(e + 0x50, (uint32_t)at), p32(e + 0x54, buf && cap > (int)sizeof "Obj_DungeonClearSeal" ? h_name : 0);
        q_commit(gmd, TB(VT_STR64ARR) + F_QWORD);
        if (!buf || cap <= (int)sizeof "Obj_DungeonClearSeal") { rt_log("coop: gamedata: blessings: the name queue has no text buffer"); return; }
    }
    uint64_t e = q_claim(gmd, Q_OF(VT_INTARR), 16);
    if (!e) return;
    p32(e, (uint32_t)want), p32(e + 4, (uint32_t)at), p32(e + 8, h_stock), p32(e + 12, 0);
    q_commit(gmd, TB(VT_INTARR) + F_QWORD);
    rt_log("coop: gamedata: blessings: %s, so %d; the pouch had %d: set to %d (key item slot %d%s)", why, want, have, want, at,
           slot < 0 ? ", was empty" : "");
}

// ---- the test (header comment)
static int test_named(const char *var, int64_t *at_us) {
    const char *e = cfg_get(var), *me = cfg_get("TOTK_COOP_NAME"), *colon = e ? strchr(e, ':') : NULL;
    int on = e && colon && me && (size_t)(colon - e) == strlen(me) && !strncmp(e, me, (size_t)(colon - e));
    if (on) *at_us = (int64_t)(atof(colon + 1) * 1e6);
    return on;
}
static void test_tick(uint64_t gmd, int64_t world_us) {
    static int on = -1, step;
    static int64_t at_us;
    static uint64_t key;
    static uint32_t bh, ih, eh, fh, iwas, ewas, fwas;
    static uint8_t bwas;
    if (on < 0) on = test_named("TOTK_COOP_TEST_GAMEDATA", &at_us);
    if (!on || step > 9 || world_us < at_us + step * 2000000ll + (step >= 5 ? 30000000ll : 0)) return;
    NumTab *ti = tab_of(T_INT), *te = tab_of(T_ENUM), *tf = tab_of(T_FLOAT);
    switch (step) {
    case 0: {   // a shared key that reads 0 here, from a tenth of the way in (not the table's first ones)
        uint32_t nk = g32(gmd + K_COUNT);
        for (uint32_t i = nk / 10; i < nk && !key; i++) {
            uint64_t e = key_entry(gmd, i);
            if ((g32(e + K_RESET) & KEY_SHARED_BITS) && !g8(e + K_VALUE)) key = g64(e);
        }
        if (!key) { rt_log("coop: gamedata: test: no shared key reads 0 here"); break; }
        int64_t i = key_index(gmd, key);
        push_key(gmd, 1, i);
        rt_log("coop: gamedata: test: key %016llx (index %lld, reset type %#x) set to 1 in this game (it read 0)",
               (unsigned long long)key, (long long)i, g32(key_entry(gmd, i) + K_RESET));
        break;
    }
    case 1: {
        bh = mm3("IsGet.Item_Fruit_A");
        int64_t i = map_index(gmd, VT_BOOL, bh);
        if (i < 0 || !bool_shared(bh)) { rt_log("coop: gamedata: test: bool IsGet.Item_Fruit_A (%08x) not shared here", bh); bh = 0; break; }
        bwas = (uint8_t)bool_value(gmd, i);
        push_bool(gmd, !bwas, bh);
        rt_log("coop: gamedata: test: bool IsGet.Item_Fruit_A (%08x) set to %u in this game (it read %u)", bh, !bwas, bwas);
        break;
    }
    case 2:   // a shared Int, from a tenth of the way in
        for (uint32_t i = ti->n / 10; i < ti->n && !ih; i++)
            if (ti->shared[i]) ih = ti->hash[i], iwas = num_value(gmd, ti, i);
        if (!ih) { rt_log("coop: gamedata: test: no shared Int here"); break; }
        push_num(gmd, ti, ih, iwas + 1);
        rt_log("coop: gamedata: test: int %08x set to %d in this game (it read %d)", ih, (int32_t)iwas + 1, (int32_t)iwas);
        break;
    case 3:   // a shrine not cleared here, from the last one down
        for (int k = 199; k >= 0 && !eh; k--) {
            int64_t i = map_index(gmd, VT_ENUM, dungeon_hash[k]);
            if (i >= 0 && te->shared[i] && num_value(gmd, te, i) != ENUM_CLEAR) eh = dungeon_hash[k], ewas = num_value(gmd, te, i);
        }
        if (!eh) { rt_log("coop: gamedata: test: no shrine left to clear here"); break; }
        push_num(gmd, te, eh, ENUM_CLEAR);
        rt_log("coop: gamedata: test: enum DungeonState.Dungeon%03lld (%08x) set to Clear (%08x) in this game (it read %08x)",
               (long long)dungeon_of(eh), eh, ENUM_CLEAR, ewas);
        break;
    case 4: {
        fh = mm3("SageOfGerudo_SliderBlockPos1");
        int64_t i = map_index(gmd, VT_FLOAT, fh);
        if (i < 0 || !tf->shared[i]) { rt_log("coop: gamedata: test: float SageOfGerudo_SliderBlockPos1 (%08x) not shared here", fh); fh = 0; break; }
        fwas = num_value(gmd, tf, i);
        float f;
        memcpy(&f, &fwas, 4);
        f += 1.5f;
        uint32_t fb;
        memcpy(&fb, &f, 4);
        push_num(gmd, tf, fh, fb);
        rt_log("coop: gamedata: test: float SageOfGerudo_SliderBlockPos1 (%08x) set to %.2f (%08x) in this game (it read %08x)", fh,
               (double)f, fb, fwas);
        break;
    }
    case 5:
        if (key) {
            push_key(gmd, 0, key_index(gmd, key));
            rt_log("coop: gamedata: test: key %016llx set back to 0 in this game (it read %u)", (unsigned long long)key,
                   g8(key_entry(gmd, key_index(gmd, key)) + K_VALUE));
        }
        break;
    case 6:
        if (bh) {
            push_bool(gmd, bwas, bh);
            rt_log("coop: gamedata: test: bool %08x set back to %u in this game (it read %u)", bh, bwas,
                   bool_value(gmd, map_index(gmd, VT_BOOL, bh)));
        }
        break;
    case 7:
        if (ih) {
            push_num(gmd, ti, ih, iwas);
            rt_log("coop: gamedata: test: int %08x set back to %d in this game", ih, (int32_t)iwas);
        }
        break;
    case 8:
        if (eh) {
            push_num(gmd, te, eh, ewas);
            rt_log("coop: gamedata: test: enum %08x set back to %08x in this game", eh, ewas);
        }
        break;
    case 9:
        if (fh) {
            push_num(gmd, tf, fh, fwas);
            rt_log("coop: gamedata: test: float %08x set back to %08x in this game", fh, fwas);
        }
        break;
    }
    step++;
}

// ---- capture, apply, verify
static void capture(uint64_t gmd) {
    static _Atomic uint32_t said;
    int taken = 0, over = 0;
    uint64_t b = gmd + TB(VT_BOOL);
    uint32_t cap = g32(b + F_QCAP), n = g32(b + F_QWORD) & 0xfffff;
    uint64_t ents = g64(b + F_QENT);
    for (uint32_t i = 0; ents && i < n && i < cap; i++) {
        uint64_t e = ents + (uint64_t)i * 8;
        uint32_t h = g32(e + 4);
        if (!h) break;   // claimed, not written yet: calc stops here too
        uint8_t v = g8(e) & 1;
        int64_t idx = map_index(gmd, VT_BOOL, h);
        if (idx < 0 || bool_value(gmd, idx) == v || !bool_shared(h)) continue;
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
    // the numbers: what changed since the last frame (calc has written this game's requests since)
    for (int k = 0; k < 3; k++) {
        NumTab *t = &nt[k];
        uint64_t te = g64(gmd + TB(t->vt) + F_ENT);
        if (!te || g32(gmd + TB(t->vt) + F_COUNT) != t->n) continue;
        for (uint32_t i = 0; i < t->n; i++) {
            if (!t->shared[i]) continue;
            uint32_t v = g32(te + (uint64_t)i * t->stride + N_VALUE);
            if (v == t->shadow[i]) continue;
            if (taken++ >= 256) { over++; continue; }   // left different: taken next frame
            t->shadow[i] = v;
            out_add((uint8_t)t->wire, t->hash[i], v);
            if (atomic_fetch_add(&said, 1) < 40) {
                if (t->wire == T_FLOAT) { float f; memcpy(&f, &v, 4); rt_log("coop: gamedata: float %08x = %.2f in this game, shared", t->hash[i], (double)f); }
                else if (t->wire == T_ENUM) rt_log("coop: gamedata: enum %08x = %08x in this game, shared", t->hash[i], v);
                else rt_log("coop: gamedata: int %08x = %d in this game, shared", t->hash[i], (int32_t)v);
            }
        }
    }
    if (over) {
        static uint64_t last;
        uint64_t now = plat_now_ns();
        if (now - last > 5000000000ull) { last = now; rt_log("coop: gamedata: %d changes in one frame not shared yet (256 at most)", over); }
    }
}

static void apply(uint64_t gmd, uint64_t now) {
    static _Atomic uint32_t said, said_cu;
    plat_mutex_lock(&in_lock);
    int done = 0;
    for (; done < in_n; done++) {
        uint64_t id = in[done].id;
        uint32_t v = in[done].value, cur;
        uint8_t t = in[done].type;
        int from = in[done].from, ok;
        if (t == T_HELLO) { catchup_start(from, now); continue; }
        if (t == T_BOOL) {
            int64_t idx = map_index(gmd, VT_BOOL, (uint32_t)id);
            if (idx < 0) { atomic_fetch_add(&n_unknown, 1); continue; }
            if (!bool_shared((uint32_t)id)) { atomic_fetch_add(&n_personal_in, 1); continue; }
            cur = (uint32_t)bool_value(gmd, idx);
            if (cur == v) { atomic_fetch_add(&n_same, 1); slog_set(t, id, v); continue; }
            ok = push_bool(gmd, (uint8_t)v, (uint32_t)id);
        } else if (t == T_KEY) {
            int64_t idx = key_index(gmd, id);
            if (idx < 0) { atomic_fetch_add(&n_unknown, 1); continue; }
            if (key_personal(gmd, idx)) { atomic_fetch_add(&n_personal_in, 1); continue; }
            cur = g8(key_entry(gmd, idx) + K_VALUE);
            if (cur == v) { atomic_fetch_add(&n_same, 1); slog_set(t, id, v); continue; }
            ok = push_key(gmd, (uint8_t)v, idx);
        } else {
            NumTab *nt_ = tab_of(t);
            int64_t idx = nt_ ? map_index(gmd, nt_->vt, (uint32_t)id) : -1;
            if (idx < 0 || (uint32_t)idx >= nt_->n) { atomic_fetch_add(&n_unknown, 1); continue; }
            if (!nt_->shared[idx]) { atomic_fetch_add(&n_personal_in, 1); continue; }
            cur = num_value(gmd, nt_, idx);
            if (cur == v) { atomic_fetch_add(&n_same, 1); nt_->shadow[idx] = v; slog_set(t, id, v); continue; }
            ok = push_num(gmd, nt_, (uint32_t)id, v);
            if (ok) nt_->shadow[idx] = v;   // calc writes it now: not this game's change
            if (ok && t == T_ENUM && v == ENUM_CLEAR && dungeon_of((uint32_t)id) >= 0) bless_pending = 3;
        }
        if (!ok) break;   // the queue is full: the rest next frame
        atomic_fetch_add(&n_applied, 1);
        slog_set(t, id, v);
        out_drop(t, id, v);
        if (atomic_fetch_add(in[done].catchup ? &said_cu : &said, 1) < 20)
            rt_log("coop: gamedata: player %d's %s %0*llx = %#x applied here%s (it read %#x)", from, type_name(t), id_width(t),
                   (unsigned long long)id, v, in[done].catchup ? " (catch-up)" : "", cur);
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
        uint8_t t = verify[k].type;
        int64_t now = -1;
        if (t == T_KEY) { int64_t i = key_index(gmd, verify[k].id); now = i < 0 ? -1 : g8(key_entry(gmd, i) + K_VALUE); }
        else if (t == T_BOOL) { int64_t i = map_index(gmd, VT_BOOL, (uint32_t)verify[k].id); now = i < 0 ? -1 : bool_value(gmd, i); }
        else { NumTab *x = tab_of(t); int64_t i = map_index(gmd, x->vt, (uint32_t)verify[k].id); now = i < 0 ? -1 : num_value(gmd, x, i); }
        if (atomic_fetch_add(&said, 1) < 40)
            rt_log("coop: gamedata: player %d's %s %0*llx now reads %#llx here (%s)", verify[k].from, type_name(t), id_width(t),
                   (unsigned long long)verify[k].id, (unsigned long long)now, now == verify[k].value ? "as sent" : "NOT as sent");
        verify[k].frames = 0;
    }
}

// GameDataMgr::calc's entry (coop_spawn.c coop_hook), x0 = the manager; me: this game's Link (0 outside the world).
void coop_gamedata_calc(uint64_t gmd, uint64_t me) {
    if (!share_on() || !gmd) return;
    static int started;
    if (!started) started = 1, plat_thread_spawn(load_tables, NULL, "coop-gamedata", 0);
    uint64_t now = plat_now_ns();
    int ids[COOP_MAX_PLAYERS];
    char names[COOP_MAX_PLAYERS][17];
    int others = coop_net_on() && coop_net_id() ? coop_net_players(ids, names, COOP_MAX_PLAYERS) : 0;
    if (me != last_me) last_me = me, world_since_ns = now;
    static int late = -1;
    static int64_t late_us;
    if (late < 0) late = test_named("TOTK_COOP_TEST_LATE", &late_us);
    int64_t world_us = frame_step_world_us();
    int is_late = late && world_us < late_us;
    atomic_store(&late_now, is_late);
    // Shared from 3 s after this game's Link is there (a save's load writes many values first), while another player is
    // in the session, once the name tables are loaded.
    int open = others > 0 && me && now - world_since_ns > 3000000000ull && atomic_load(&tables_ready) && !is_late;
    atomic_store(&gate_gmd, open ? gmd : 0);
    atomic_store(&gate, open);
    if (!open) { was_open = 0; return; }
    if (!was_open) {   // sharing starts (again): copy the numbers, say hello so the others catch this game up
        was_open = 1;
        snapshot(gmd);
        if (!said_start) said_start = 1, log_start(gmd);
        out_add(T_HELLO, (uint64_t)coop_net_id(), 0);
        rt_log("coop: gamedata: hello sent (%d other player%s here)%s", others, others == 1 ? "" : "s", late ? " (joined late, the test)" : "");
    }
    verify_tick(gmd);
    test_tick(gmd, world_us);
    capture(gmd);
    apply(gmd, now);
    if (bless_pending && !--bless_pending) blessing_tick(gmd);   // after calc wrote the shrine's state
    out_flush(now);
    static uint64_t last_stats;
    if (now - last_stats > 30000000000ull) {
        if (last_stats)
            rt_log("coop: gamedata: %u keys, %u bools (%u of them event flags) and %u numbers shared, %u packets; %u received: "
                   "%u applied, %u already so, %u the player's here, %u unknown here, %u by catch-up; dropped: %u not joined "
                   "yet, %u for another player; full: out %u, in %u; session log %d",
                   atomic_load(&n_sent_key), atomic_load(&n_sent_bool), atomic_load(&n_sent_event), atomic_load(&n_sent_num),
                   atomic_load(&n_packets), atomic_load(&n_recv), atomic_load(&n_applied), atomic_load(&n_same),
                   atomic_load(&n_personal_in), atomic_load(&n_unknown), atomic_load(&n_catchup_in), atomic_load(&n_late_drop),
                   atomic_load(&n_not_mine), atomic_load(&n_out_full), atomic_load(&n_in_full), slog_n);
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
    int64_t idx = th ? map_index(gmd, VT_BOOL, th) : -1;
    int was = idx >= 0 ? bool_value(gmd, idx) : -1;
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    if ((c->x[0] & 1) && idx >= 0 && was != v && bool_shared(th)) {
        out_add(T_BOOL, th, v);
        atomic_fetch_add(&n_sent_event, 1);
        static _Atomic uint32_t said;
        if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: gamedata: event bool %08x = %u in this game, shared", th, v);
    }
    return 1;
}
