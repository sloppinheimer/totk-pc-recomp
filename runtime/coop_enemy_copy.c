// Co-op: the enemy sync's copies (docs/coop/enemy-sync.md sections 5 to 7).
//
// A copy is this game's own actor of an enemy another game runs. It does not think: the entries of its graph runners
// are turned away (en_copy_hook) and the runner its calc holds inline is emptied before every calc (the four writes).
// Its main body is static and it is put, before every calc, at the pose its owner stated for the time T this game
// shows that owner at; its animation slots are given the owner's commands by the game's own play request and their
// clocks are kept at the owner's. A held actor (new, or in the hold of this game's claim) is switched off the same
// way and stands where it was.
//
// The owner states an enemy as a whole value (STATE, unreliable, the latest per enemy): place, rotation, velocity,
// life points, and per animation slot the command's name hash, rate and the clocks of its blend controllers, read at
// phase 0. Nothing is sent as an event: a copy plays a command when the slot's stated command differs from its own,
// so a lost state costs a frame and no more. Command names travel once per name and peer (NAME, reliable).
//
// The record (once a second, and ten times a second per enemy for the report): see en_copy_phase0.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "coop_enemy.h"
#include "coop_net.h"
#include "coop_sess.h"
#include "host.h"
#include "layout.h"

#define SEC 1000000000ull
#define FN_MOTION_TYPE 0xae4300     // (body, 0 static | 1 keyframed | 2 dynamic)
#define FN_PLAY 0x4dc628            // (request, mode)

static int noswitch(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_EN_NOSWITCH") != NULL;
    return on;
}
static int is_off(int st) { return st == EM_NEW || st == EM_HELD || st == EM_COPY; }

// ---- Command names ----------------------------------------------------------------------------------------------------
#define NAME_N 1024
typedef struct { _Atomic uint32_t hash; char name[40]; } Name;
static Name names[NAME_N];            // strings live for the session: the game keeps the pointer a play request gives
typedef struct { uint32_t hash; char name[40]; } MName;

static uint32_t fnv_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}
static const char *name_of(uint32_t hash) {
    for (uint32_t i = hash % NAME_N, k = 0; k < NAME_N; k++, i = (i + 1) % NAME_N) {
        uint32_t h = atomic_load_explicit(&names[i].hash, memory_order_acquire);
        if (h == hash) return names[i].name;
        if (!h) return NULL;
    }
    return NULL;
}
// 1: the name is new here. (Phase 0 and the net tick add, both under en_lock; update threads only read.)
static int name_add(uint32_t hash, const char *s) {
    for (uint32_t i = hash % NAME_N, k = 0; k < NAME_N; k++, i = (i + 1) % NAME_N) {
        uint32_t h = atomic_load(&names[i].hash);
        if (h == hash) return 0;
        if (h) continue;
        snprintf(names[i].name, sizeof names[i].name, "%s", s);
        atomic_store_explicit(&names[i].hash, hash, memory_order_release);
        return 1;
    }
    return 0;
}
void en_copy_burst(int id) {
    for (int i = 0; i < NAME_N; i++) {
        MName m;
        if (!(m.hash = atomic_load(&names[i].hash))) continue;
        memcpy(m.name, names[i].name, sizeof m.name);
        en_send(id, ENM_PLAY, &m, sizeof m);
    }
}
// An enemy this game runs let go of, or threw, what its slot held: told at once, so its copies let go at once.
typedef struct { Enid id; uint32_t slot; uint32_t thrown; uint64_t x3, x4; uint8_t req[0x30]; } MEquip;
void en_equip_event(uint64_t holder, int slot, int thrown, const uint8_t req[0x30], uint64_t x3, uint64_t x4) {
    EnCell *m = en_cell_of(holder);
    int16_t ei = m ? atomic_load(&m->en) : -1;
    if (ei < 0 || slot < 0 || slot >= EN_EQ_N) return;
    MEquip q;
    memset(&q, 0, sizeof q);
    q.id = en_tab[ei].id, q.slot = (uint32_t)slot, q.thrown = (uint32_t)thrown, q.x3 = x3, q.x4 = x4;
    if (req) memcpy(q.req, req, 0x30);
    if (req && thrown) { void en_foreign_scan(const char *what, const uint8_t *p, int n, int skip); en_foreign_scan("a throw's request", q.req, 0x30, -1); }   // coop_enemy_shot.c
    en_send_all(ENM_EQUIP, &q, sizeof q);
}
// The attention sender's client reason bytes ([[comps+0x130]]+0x24..0x64, one byte a client type: a client with a
// reason is not offered). They decide which prompts the player's game shows on this enemy: the two Sneakstrike
// clients, lock-on, auto-aim. They are written by graph nodes (the utility graph's stealth module, an interrupt node),
// so a copy kept the loaded zeros: every client offered at all times, and a Sneakstrike prompt on an enemy in battle
// (research R4 1.3). The owner states the 0x40 bytes when they change and a copy's are kept the same.
typedef struct { Enid id; uint8_t b[0x40]; } MAttn;
static uint64_t attn_sender(uint64_t actor) { return coop_rd_ptr(en_comps(actor) + 0x130); }   // (the component itself: its bytes at +0x24 are the reason masks)
static void attn_owner(En *e) {
    uint64_t s = attn_sender(e->actor);
    uint8_t b[0x40];
    if (!s) { static _Atomic int said0; if (atomic_fetch_add(&said0, 1) < 3) rt_log("coop: en: attention: %s has no attention sender at [[comps+0x130]]", e->name); return; }
    if (!coop_rd(s + 0x24, b, sizeof b) || (e->attn_have && !memcmp(b, e->attn, sizeof b))) return;
    {
        static _Atomic int said;
        int nz = 0;
        for (int k = 0; k < 0x40; k++) nz += b[k] != 0;
        if (atomic_fetch_add(&said, 1) < 40) rt_log("coop: en: attention: %s %016llx (run here) states its client reasons: %d set (Sneakstrike awake %02x asleep %02x, lock-on %02x)", e->name, (unsigned long long)e->id.a, nz, b[0x2c], b[0x2d], b[0]);
    }
    memcpy(e->attn, b, sizeof b), e->attn_have = 1;
    MAttn m;
    m.id = e->id, memcpy(m.b, b, sizeof b);
    en_send_all(ENM_ATTN, &m, sizeof m);
}
// A copy's reasons are changed by the game's own setter (main+0x4e603c(sender, reason bit, client type, 1 offered | 0
// not), which also marks the clients' records), a few a frame, from the copy's pre-calc.
static void attn_copy(Cpu *c, uint64_t actor, En *e) {
    uint64_t s = e->attn_have ? attn_sender(actor) : 0;
    uint8_t b[0x40];
    int calls = 0;
    if (!s || !coop_rd(s + 0x24, b, sizeof b) || !memcmp(b, e->attn, sizeof b)) return;
    for (int t = 1; t < 0x40 && calls < 6; t++)
        for (int r = 0; r < 8 && calls < 6; r++)
            if ((b[t] ^ e->attn[t]) >> r & 1) coop_guest_call(c, MOD_MAIN_BASE + 0x4e603c, s, (uint64_t)r, (uint64_t)t, (uint64_t)!(e->attn[t] >> r & 1)), calls++;
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: attention: the copy of %s %016llx is given its owner's client reasons, %d changes now (Sneakstrike awake %02x asleep %02x)", e->name, (unsigned long long)e->id.a, calls, e->attn[0x2c], e->attn[0x2d]);
}
int en_copy_msg(const CoopCtrMsg *m, int from) {
    (void)from;
    { int en_fx_msg(const CoopCtrMsg *m); if (en_fx_msg(m)) return 1; }   // coop_enemy_fx.c
    { int en_arm_msg(const CoopCtrMsg *m); if (en_arm_msg(m)) return 1; }   // coop_enemy_arm.c
    { int en_act_msg(const CoopCtrMsg *m); if (en_act_msg(m)) return 1; }   // coop_enemy_act.c
    { int en_shs_msg(const CoopCtrMsg *m); if (en_shs_msg(m)) return 1; }   // coop_enemy_shot.c
    if (m->type == ENM_ATTN) {
        const MAttn *q = (const MAttn *)m->d;
        if (m->len != (int)sizeof *q) return 1;
        for (int i = 0; i < EN_N; i++)
            if (en_tab[i].used && enid_eq(en_tab[i].id, q->id) && en_tab[i].owner == m->from) memcpy(en_tab[i].attn, q->b, sizeof q->b), en_tab[i].attn_have = 1;
        return 1;
    }
    if (m->type == ENM_EQUIP) {
        const MEquip *q = (const MEquip *)m->d;
        if (m->len != (int)sizeof *q || q->slot >= EN_EQ_N) return 1;
        for (int i = 0; i < EN_N; i++)
            if (en_tab[i].used && enid_eq(en_tab[i].id, q->id) && en_tab[i].cell >= 0 && en_tab[i].owner == m->from)   // (its owner's word only)
            {
                EnCell *cm = &en_map[en_tab[i].cell];
                // (a throw is made by the copy itself, with the request its owner's enemy threw with: what flies here
                // is this game's own, and it is this game's Link it can hurt. A drop: the copy lets go at once.)
                if (q->thrown) {   // (one at a time: a second one while the first waits is not made)
                    if (!atomic_load_explicit(&cm->throw_due, memory_order_acquire))
                        memcpy(cm->throw_req, q->req, 0x30), cm->throw_x3 = q->x3, cm->throw_x4 = q->x4, cm->throw_slot = (uint8_t)q->slot,
                        atomic_store_explicit(&cm->throw_due, 1, memory_order_release);
                }
                else atomic_fetch_or(&cm->eq_drop, (uint8_t)(1u << q->slot));
            }
        return 1;
    }
    if (m->type != ENM_PLAY) return 0;
    const MName *n = (const MName *)m->d;
    // (the name must hash to what it is given for: the first pair heard for a hash stays for the session, as the game
    // keeps the string's address, so a wrong pair from another game would have named every play and equipment of it)
    if (m->len == (int)sizeof *n && n->hash && memchr(n->name, 0, sizeof n->name) && fnv_str(n->name) == n->hash) name_add(n->hash, n->name);
    return 1;
}

// ---- The animation player of an actor ---------------------------------------------------------------------------------
static uint64_t anim_g(uint64_t a) { return coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(en_comps(a) + 0x40) + 0x18) + 0x18); }
static int anim_count(uint64_t g) {
    int32_t n = 0;
    return g && coop_rd(g + 0x18, &n, 4) && n > 0 ? (n > EN_SLOT_N ? EN_SLOT_N : n) : 0;
}
static uint64_t anim_slot(uint64_t g, int i) { return coop_rd_ptr(coop_rd_ptr(g + 0x20) + (uint64_t)i * 0x48); }
static void anim_cmd(uint64_t s, char *out, size_t n) {
    uint64_t p = s ? coop_rd_ptr(s + 0x10) : 0;
    out[0] = 0;
    if (!p || !coop_rd(p, out, n - 1)) { out[0] = 0; return; }
    out[n - 1] = 0;
    for (char *q = out; *q; q++) if (*q < 0x20 || *q >= 0x7f) { *q = 0; break; }
}
static uint64_t anim_ctx(uint64_t s, int *cur) {
    uint64_t x = s ? coop_rd_ptr(s + 0x50) : 0;
    uint8_t k = 0xff;
    if (!x || !coop_rd(x + 0x95, &k, 1) || k >= EN_CTRL_N) return 0;
    if (cur) *cur = k;
    return coop_rd_ptr(x + 0x28 + 8ull * k);
}
static void slots_read(uint64_t actor, EnState *st, int learn) {
    uint64_t g = anim_g(actor);
    int n = anim_count(g);
    st->nslot = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        EnSlot *o = &st->sl[i];
        uint64_t s = anim_slot(g, i), x = s ? coop_rd_ptr(s + 0x50) : 0;
        char cmd[40];
        memset(o, 0, sizeof *o);
        anim_cmd(s, cmd, sizeof cmd);
        if (!cmd[0]) continue;
        o->cmd = fnv_str(cmd);
        if (learn && name_add(o->cmd, cmd)) { MName m; memset(&m, 0, sizeof m); m.hash = o->cmd, snprintf(m.name, sizeof m.name, "%s", cmd); en_send_all(ENM_PLAY, &m, sizeof m); }
        coop_rd(s + 0xd0, &o->rate, 4);
        uint8_t k = 0;
        if (!x || !coop_rd(x + 0x95, &k, 1) || k >= EN_CTRL_N) continue;
        o->cur = k, o->n = EN_CTRL_N;
        for (int j = 0; j < EN_CTRL_N; j++) {
            uint64_t ctx = coop_rd_ptr(x + 0x28 + 8ull * j);
            if (ctx) coop_rd(ctx + 0x68, &o->c[j].clock, 4), coop_rd(ctx + 0x6c, &o->c[j].weight, 4);
        }
    }
}

// What each equipment slot of an actor holds, as the hash of the held actor's name (0: nothing). The equipment user
// is [comps+0x230]; its slot links are 0x18 apart from +0x20, each an actor link (+8 the reference object, whose
// +0x40 is the actor; +0x10 the actor's id).
static uint64_t equip_held(uint64_t eu, int s) {   // the actor a slot holds, 0 none (its link's id must be the actor's)
    uint64_t ref = eu ? coop_rd_ptr(eu + 0x20 + (uint64_t)s * 0x18 + 8) : 0, held = ref ? coop_rd_ptr(ref + 0x40) : 0;
    uint32_t id = 0, gen = 0;
    if (!held || !coop_rd(eu + 0x20 + (uint64_t)s * 0x18 + 0x10, &id, 4) || !coop_rd(held + 0x10, &gen, 4) || id != gen) return 0;
    return held;
}
static void equip_read(uint64_t actor, uint32_t eq[EN_EQ_N], int learn) {
    uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
    for (int s = 0; s < EN_EQ_N; s++) {
        eq[s] = 0;
        uint64_t held = equip_held(eu, s);
        if (!held) continue;
        char nm[40] = "";
        coop_actor_name(held, nm, sizeof nm);
        if (!nm[0]) continue;
        eq[s] = fnv_str(nm);
        if (learn && name_add(eq[s], nm)) { MName m; memset(&m, 0, sizeof m); m.hash = eq[s], snprintf(m.name, sizeof m.name, "%s", nm); en_send_all(ENM_PLAY, &m, sizeof m); }
    }
}
// The same, with what is fused to each held thing (the link at +0xb0 of its [comps+0x208]) and the slot's state byte
// ([equipment user+0x4620+slot]: 2 drawn, 4 put away; research R13 section 3).
static void equip_read_more(uint64_t actor, uint32_t att[EN_EQ_N], uint8_t eqs[EN_EQ_N], int learn) {
    uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
    for (int s = 0; s < EN_EQ_N; s++) {
        att[s] = 0, eqs[s] = 0;
        uint64_t ref = eu ? coop_rd_ptr(eu + 0x20 + (uint64_t)s * 0x18 + 8) : 0, held = ref ? coop_rd_ptr(ref + 0x40) : 0;
        if (!held) continue;
        coop_rd(eu + 0x4620 + (uint64_t)s, &eqs[s], 1);
        eqs[s] &= 0x3f;
        {   // whether what the slot holds is shown (B.34): the slot's visible byte (+0x570 + slot * 0x100 + 0xf4), which the
            // nodes EquipmentUserRequestSetVisible set through main+0xced8c0 with bit 0x200 of the slot's command word
            // (+0xf0); in the top two bits: 0x40 stated, 0x80 shown
            uint8_t vb = 0;
            if (coop_rd(eu + 0x570 + (uint64_t)s * 0x100 + 0xf4, &vb, 1)) eqs[s] |= (uint8_t)(0x40 | (vb ? 0x80 : 0));
        }
        uint64_t wc = coop_rd_ptr(en_comps(held) + 0x208), aref = wc ? coop_rd_ptr(wc + 0xb0 + 8) : 0, a = aref ? coop_rd_ptr(aref + 0x40) : 0;
        int32_t ast = 0;
        char nm[40] = "";
        if (!a || !coop_rd(a + COOP_ACTOR_STATE, &ast, 4) || ast < 2 || ast > 6) continue;
        coop_actor_name(a, nm, sizeof nm);
        if (!nm[0]) continue;
        att[s] = fnv_str(nm);
        if (learn && name_add(att[s], nm)) { MName m; memset(&m, 0, sizeof m); m.hash = att[s], snprintf(m.name, sizeof m.name, "%s", nm); en_send_all(ENM_PLAY, &m, sizeof m); }
    }
}

// For the mismatch checker (coop_check.c), of any actor, owned, copy or neither: each animation slot's command and
// the clock of its current blend controller, and each equipment slot's held thing and state byte (2 drawn, 4 put
// away). The same reads as slots_read and equip_read, without telling other games any name. Returns the slot count.
int en_read_anim(uint64_t actor, char cmd[EN_SLOT_N][40], float clock[EN_SLOT_N]) {
    uint64_t g = anim_g(actor);
    int n = anim_count(g);
    for (int i = 0; i < EN_SLOT_N; i++) {
        cmd[i][0] = 0, clock[i] = 0;
        if (i >= n) continue;
        uint64_t s = anim_slot(g, i), ctx = anim_ctx(s, NULL);
        anim_cmd(s, cmd[i], 40);
        if (ctx) coop_rd(ctx + 0x68, &clock[i], 4);
    }
    return n;
}
void en_read_equip(uint64_t actor, char held[EN_EQ_N][40], uint8_t eqs[EN_EQ_N]) {
    uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
    for (int s = 0; s < EN_EQ_N; s++) {
        uint64_t h = equip_held(eu, s);
        held[s][0] = 0, eqs[s] = 0;
        if (!h) continue;
        coop_actor_name(h, held[s], 40);
        coop_rd(eu + 0x4620 + (uint64_t)s, &eqs[s], 1);
        eqs[s] &= 0x3f;
    }
}

// ---- An enemy's AI memory (enemy-sync.md B.2) --------------------------------------------------------------------------
// What a graph keeps in the actor's blackboard and acts on later belongs to the enemy, not to the game that ran it: a
// Wizzrobe's IsMagicWeather is set by its weather action and, while it is set, the graph that always runs asks for the
// weather and shoots a fireball at intervals (research R8c). It stayed in the game that had run the summon: the rain
// stopped when the other game took the Wizzrobe over and began again when it came back (a play session, 2026-11-16).
// The bools below travel in STATE as bits (0x80: stated at all; bit k: key k is true; a key the actor's blackboard does
// not have reads false) and a copy's own blackboard is given them, so a game that takes the enemy over finds them.
// The blackboard ([[[actor+0x228]+0]+0x10]): its bools' section at +0x198 (count +0, values +8, a table of {u32 hash,
// s32 index} at +0x30 of the size at +0x3c; keys are murmur3_32 of the name: coop_ctn.c bb_index), a lock byte per
// bool at [+0x1e0] and a change counter per bool at [+0x1f0], as the game's own setter writes them
// (ExecuteBlackboardSetBool, main+0x740b68, its actor case: a locked bool is left, the counter goes up by one).
static const char *const mem_keys[] = {"IsMagicWeather", "IsShootMagicObj"};
#define MEM_N ((int)(sizeof mem_keys / sizeof *mem_keys))
static uint32_t murmur3(const char *s) {
    uint32_t h = 0, k, n = (uint32_t)strlen(s), i = 0;
    for (; i + 4 <= n; i += 4) {
        memcpy(&k, s + i, 4);
        k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)(uint8_t)s[i + 2] << 16;   // fallthrough
    case 2: k ^= (uint32_t)(uint8_t)s[i + 1] << 8;    // fallthrough
    case 1: k ^= (uint8_t)s[i], k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u, h ^= k;
    }
    h ^= n, h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}
static int mem_index(uint64_t bb, int key) {
    static uint32_t hash[MEM_N];
    if (!hash[key]) hash[key] = murmur3(mem_keys[key]);
    uint64_t tab = bb ? coop_rd_ptr(bb + 0x198 + 0x30) : 0;
    int32_t size = 0, count = 0;
    if (!tab || !coop_rd(bb + 0x198 + 0x3c, &size, 4) || !coop_rd(bb + 0x198, &count, 4) || size <= 0 || size > 4096) return -1;
    uint32_t at = hash[key] % (uint32_t)size;
    for (int i = 0; i < size; i++, at = (at + 1) % (uint32_t)size) {
        struct { uint32_t hash; int32_t index; } e;
        if (!coop_rd(tab + 8 * (uint64_t)at, &e, 8) || !e.hash) return -1;
        if (e.hash == hash[key]) return e.index >= 0 && e.index < count ? e.index : -1;
    }
    return -1;
}
static uint64_t mem_bb(uint64_t actor) {
    uint64_t c0 = coop_rd_ptr(en_comps(actor));
    return c0 ? coop_rd_ptr(c0 + 0x10) : 0;
}
static uint8_t mem_read(uint64_t actor) {
    uint64_t bb = mem_bb(actor), vals = bb ? coop_rd_ptr(bb + 0x198 + 8) : 0;
    uint8_t out = 0x80;
    for (int k = 0; vals && k < MEM_N; k++) {
        int i = mem_index(bb, k);
        uint8_t b = 0;
        if (i >= 0 && coop_rd(vals + (uint64_t)i, &b, 1) && b) out |= (uint8_t)(1 << k);
    }
    return out;
}
static void mem_write(uint64_t actor, uint8_t want, const char *name) {
    uint64_t bb = mem_bb(actor), vals = bb ? coop_rd_ptr(bb + 0x198 + 8) : 0, locks = bb ? coop_rd_ptr(bb + 0x1e0) : 0, counts = bb ? coop_rd_ptr(bb + 0x1f0) : 0;
    if (!(want & 0x80) || !vals) return;
    for (int k = 0; k < MEM_N; k++) {
        int i = mem_index(bb, k);
        uint8_t b = 0, lock = 0, cn = 0, v = want >> k & 1;
        if (i < 0 || !coop_rd(vals + (uint64_t)i, &b, 1) || !!b == v) continue;
        if (locks && coop_rd(locks + (uint64_t)i, &lock, 1) && (lock & 1)) continue;
        memcpy((void *)(uintptr_t)(vals + (uint64_t)i), &v, 1);
        if (counts && coop_rd(counts + (uint64_t)i, &cn, 1)) cn++, memcpy((void *)(uintptr_t)(counts + (uint64_t)i), &cn, 1);
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 40) rt_log("coop: en: memory: the copy of %s is given %s = %u, as its owner's has it", name, mem_keys[k], v);
    }
}

// ---- An enemy's brain at a change of owner (enemy-sync.md B.6; research R10) ------------------------------------------
// A copy's brain graph never started, so at its first tick as this game's own its root Enter picks an idle verb, the
// idle module notices the player afresh (the Find clip) and everything that is cleared outside battle is cleared (a
// Wizzrobe's IsMagicWeather). The owner states the brain verb ([BSA+0x58], BSA = [comps+0x158]) and whom target slot 0
// names, as a player (0 no one, 1 + that player's net id, 0xff some other actor); the game that takes the enemy over
// fills its own slot 0 with its actor of that player (main+0xa70a1c(slot, link, options), as the game's modules fill a
// slot from an actor link), clears slot 6 (the noticed one), asks the verb with force (main+0xad9844(BSA, verb, 1)),
// and for a few frames turns away what the brain's start would undo: another verb asked by a graph, and the reset of
// the target slots (main+0x24f0a9c). The target holder is [comps+0x4b0], slot k at +0x28 + k*0xe8: its actor link at
// +8 (reference +0x10, id +0x18), +0x84 the emitter id, +0xe2 the kind (2: an actor).
#define FN_BRAIN_VERB 0xad9844
#define FN_SLOT_FILL 0xa70a1c
#define FN_SLOT_CLEAR 0x83be9c
#define FN_TARGETS_RESET 0x24f0a9c
#define BRAIN_GRACE 4
// The brain's start does not always come in those first frames: in three of four runs it came seconds later (when the
// action in hand ended), after the hold, and the Wizzrobe went to verb 18, then 6, with a Find clip. So after the
// strict frames the hold goes on in a narrow form until the start has been seen and turned away once, 20 s at most:
// only the idle verbs the root's Enter chooses among (1 wait, 2 wait at camp, 8 lookout, 18 wander, 19 patrol), asked
// without force, and the target slots' reset are turned away; any other verb is the brain's own business.
#define BRAIN_HOLD 600
static __thread uint64_t tl_grace_bsa;   // the brain of the actor whose calc this thread is in, while it is held
static __thread uint8_t tl_grace_verb;
static __thread int tl_grace_narrow;     // the narrow form of the hold
static __thread EnCell *tl_grace_cell;
static __thread int tl_brain_mine;       // inside this file's own call of the verb request
uint64_t coop_player_standin(int net_id);   // coop_players.c
static uint64_t brain_bsa(uint64_t actor) { return coop_rd_ptr(en_comps(actor) + 0x158); }
static uint64_t brain_slot(uint64_t actor, int k) {
    uint64_t holder = coop_rd_ptr(en_comps(actor) + 0x4b0);
    return holder ? holder + 0x28 + (uint64_t)k * 0xe8 : 0;
}
static uint64_t brain_slot_actor(uint64_t slot) {
    uint8_t kind = 0;
    uint64_t ref;
    if (!slot || !coop_rd(slot + 0xe2, &kind, 1) || kind != 2 || !(ref = coop_rd_ptr(slot + 0x10))) return 0;
    return coop_rd_ptr(ref + 0x40);
}
static void brain_read(uint64_t actor, uint8_t *verb, uint8_t *tgt) {
    uint64_t bsa = brain_bsa(actor), ta = brain_slot_actor(brain_slot(actor, 0));
    uint32_t v = 0;
    if (bsa) coop_rd(bsa + 0x58, &v, 4);
    *verb = v < 0xff ? (uint8_t)v : 0, *tgt = 0;
    if (!ta) return;
    *tgt = 0xff;
    if (ta == coop_player_actor()) *tgt = (uint8_t)(1 + coop_net_id());
    else for (int id = 0; id < 32; id++) if (coop_player_standin(id) == ta) { *tgt = (uint8_t)(1 + id); break; }
}
// An actor's link reference: the object the game's actor links point at ([ref+0x40] the actor, [ref+0x48] its id, which
// is the u32 at actor+0x10). The actor keeps a pointer to it; found by its content, once per call.
static uint64_t brain_ref_of(uint64_t actor) {
    uint32_t id = 0, rid = 0;
    if (!coop_rd(actor + 0x10, &id, 4)) return 0;
    for (int o = 0; o < 0x400; o += 8) {
        uint64_t p = coop_rd_ptr(actor + (uint64_t)o);
        if (p > 0x10000 && p != actor && coop_rd_ptr(p + 0x40) == actor && coop_rd(p + 0x48, &rid, 4) && rid == id) return p;
    }
    return 0;
}
static uint64_t inline_inst(uint64_t actor, uint64_t *ctrl_out);
static void brain_hold(Cpu *c, uint64_t actor, EnCell *m, const char *when) {
    // Nothing is asked of a brain whose graph has not started (B.37). A verb asked with force of an actor made a moment
    // before ran a node of a graph that was not there: the game died in the node's tick (main+0x1920, reading 0x1f),
    // twice in a play session, each time right after this call on a new Yiga (2026-11-17). The graph is looked at
    // again here, so an actor that was slow to start is held once it has.
    if (!m->graph_started) {
        uint64_t ctrl, inst = inline_inst(actor, &ctrl);
        uint32_t q = 0;
        uint16_t bits = 0;
        if (inst) coop_rd(inst + 0x90, &q, 4), coop_rd(inst + 0xd8, &bits, 2);
        if (!inst || !(q || bits)) return;
        m->graph_started = 1;
    }
    uint64_t bsa = brain_bsa(actor), slot0 = brain_slot(actor, 0), holder = coop_rd_ptr(en_comps(actor) + 0x4b0);
    uint64_t want = !m->grace_tgt || m->grace_tgt == 0xff ? 0 : m->grace_tgt - 1 == coop_net_id() ? coop_player_actor() : coop_player_standin(m->grace_tgt - 1);
    uint32_t v = 0;
    int filled = 0, asked = 0;
    if (!bsa || !slot0) return;
    if (want && brain_slot_actor(slot0) != want) {
        static __thread _Alignas(16) uint64_t link[4];
        static __thread _Alignas(16) uint8_t opts[16];
        uint64_t ref = brain_ref_of(want);
        uint32_t id = 0;
        coop_rd(want + 0x10, &id, 4);
        if (ref) {
            memset(link, 0, sizeof link), memset(opts, 0, sizeof opts);
            link[0] = coop_rd_ptr(slot0 + 8), link[1] = ref, memcpy(&link[2], &id, 4), opts[1] = 1;
            coop_guest_call(c, MOD_MAIN_BASE + FN_SLOT_FILL, slot0, (uint64_t)(uintptr_t)link, (uint64_t)(uintptr_t)opts, 0);
            coop_guest_call(c, MOD_MAIN_BASE + FN_SLOT_CLEAR, holder, 6, 0, 0);
            filled = brain_slot_actor(slot0) == want ? 1 : -1;
        } else filled = -2;
    }
    coop_rd(bsa + 0x58, &v, 4);
    if (m->grace_verb && v != m->grace_verb) {
        uint32_t zero = 0;
        tl_brain_mine = 1;
        coop_guest_call(c, MOD_MAIN_BASE + FN_BRAIN_VERB, bsa, m->grace_verb, 1, 0);
        tl_brain_mine = 0;
        memcpy((void *)(uintptr_t)(bsa + 0x5c), &zero, 4);
        asked = 1;
    }
    if (filled || asked) {
        char an[48] = "", tn[48] = "";
        uint8_t kind = 0;
        uint32_t em = 0, now_v = 0;
        coop_rd(slot0 + 0xe2, &kind, 1), coop_rd(slot0 + 0x84, &em, 4), coop_rd(bsa + 0x58, &now_v, 4);
        rt_log("coop: en: brain: %s %llx %s: verb %u -> %u (its owner's %u)%s; target slot 0 %s %s (kind %u, emitter %d)", coop_actor_name(actor, an, sizeof an), (unsigned long long)actor, when, v, now_v, m->grace_verb,
               asked ? ", asked with force" : "", filled == 1 ? "filled with" : filled == -1 ? "NOT filled with" : filled == -2 ? "not filled (no link reference found) with" : "is",
               want ? coop_actor_name(want, tn, sizeof tn) : "no one", kind, (int)em);
    }
}

// ---- What enemies hold ---------------------------------------------------------------------------------------------------
// Every actor an enemy of a listed kind holds here, and what is fused to it (a held weapon's attachment: the link at
// +0xb0 of the weapon's component [[w+0x228]+0x208]), as a set the object mirror asks (coop_players.c prop_fn): such an
// actor belongs to its enemy and is no loose object. A Fire Wizzrobe's rod carries a ruby (Item_Ore_B) fused to it; the
// mirror sent the ruby as an object of its own, and the other game showed a second gem floating beside the copy's rod
// that did not vanish when the Wizzrobe did (a play session, 2026-11-16). Rebuilt by phase 0 four times a second.
#define HELD_N 1024
static _Atomic uint64_t held_set[2][HELD_N], held_by[2][HELD_N];   // the held actor, and the enemy that holds it
static _Atomic int held_cur;
static void held_put(_Atomic uint64_t *t, _Atomic uint64_t *by, uint64_t a, uint64_t holder) {
    for (uint32_t h = (uint32_t)((a >> 4) * 2654435761u) % HELD_N, k = 0; k < 32; k++, h = (h + 1) % HELD_N) {
        uint64_t v = atomic_load_explicit(&t[h], memory_order_relaxed);
        if (v == a) return;
        if (!v) { atomic_store_explicit(&by[h], holder, memory_order_relaxed); atomic_store_explicit(&t[h], a, memory_order_relaxed); return; }
    }
}
// An actor known this moment to be an enemy's (a projectile a copy is about to shoot): into the set now, not at the
// next rebuild a quarter of a second on. (Both tables: a rebuild in between keeps it.)
void en_held_add(uint64_t actor, uint64_t holder) {
    if (actor < 0x10000) return;
    held_put(held_set[0], held_by[0], actor, holder), held_put(held_set[1], held_by[1], actor, holder);
}
// The enemy that holds an actor, or holds what it is fused to (0: none).
uint64_t coop_enemy_holder(uint64_t actor) {
    if (!coop_enemy_new() || actor < 0x10000) return 0;
    int cur = atomic_load_explicit(&held_cur, memory_order_acquire);
    for (uint32_t h = (uint32_t)((actor >> 4) * 2654435761u) % HELD_N, k = 0; k < 32; k++, h = (h + 1) % HELD_N) {
        uint64_t v = atomic_load_explicit(&held_set[cur][h], memory_order_relaxed);
        if (v == actor) return atomic_load_explicit(&held_by[cur][h], memory_order_relaxed);
        if (!v) return 0;
    }
    return 0;
}
int coop_enemy_holds(uint64_t actor) {
    if (!coop_enemy_new() || actor < 0x10000) return 0;
    _Atomic uint64_t *t = held_set[atomic_load_explicit(&held_cur, memory_order_acquire)];
    for (uint32_t h = (uint32_t)((actor >> 4) * 2654435761u) % HELD_N, k = 0; k < 32; k++, h = (h + 1) % HELD_N) {
        uint64_t v = atomic_load_explicit(&t[h], memory_order_relaxed);
        if (v == actor) return 1;
        if (!v) return 0;
    }
    return 0;
}
// Whether an enemy of a listed kind that this game has had for under 3 s stands within m of p: what is made beside it
// then is most likely its equipment on the way into its hand (the object mirror waits for it; coop_players.c prop_fn).
int coop_enemy_young_near(const float p[3], float m) {
    if (!coop_enemy_new()) return 0;
    uint32_t frame = coop_sess_frame_no();
    for (int i = 0; i < EN_MAP_N; i++) {
        uint64_t a = atomic_load_explicit(&en_map[i].actor, memory_order_relaxed);
        float q[3];
        if (a <= 1 || frame - en_map[i].born_frame > 180 || !coop_rd(a + COOP_ACTOR_POS, q, 12)) continue;
        if ((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]) < m * m) return 1;
    }
    return 0;
}
// The projectiles an actor's Shooter component keeps ready ([comps+0x448]: slots of 0x38 at +0x20, count +0x2c, each
// with a count at +8 and records of 0xf8 at +0x10; a record's projectile is the link at +0x20) belong to the enemy
// that actor is or is held by: a flame from a rod's ruby has no link back to anyone in a copy's game.
static void held_shooter(_Atomic uint64_t *t, _Atomic uint64_t *by, uint64_t of, uint64_t holder) {
    uint64_t s = coop_rd_ptr(en_comps(of) + 0x448), tab = s ? coop_rd_ptr(s + 0x20) : 0;
    int32_t n = 0;
    if (!tab || !coop_rd(s + 0x2c, &n, 4) || n <= 0 || n > 32) return;
    for (int i = 0; i < n; i++) {
        uint64_t arr = coop_rd_ptr(tab + (uint64_t)i * 0x38 + 0x10);
        int32_t cnt = 0;
        if (!arr || !coop_rd(tab + (uint64_t)i * 0x38 + 8, &cnt, 4) || cnt <= 0 || cnt > 32) continue;
        for (int k = 0; k < cnt; k++) {
            uint64_t ref = coop_rd_ptr(arr + (uint64_t)k * 0xf8 + 0x28), proj = ref ? coop_rd_ptr(ref + 0x40) : 0;
            if (proj > 0x10000) held_put(t, by, proj, holder);
        }
    }
}
static void test_flame(uint64_t a, void *arg) {   // (test) every live FireRodLv1Fire is asked about
    int coop_enemy_shot_mirror(uint64_t proj);
    char nm[40] = "";
    (void)arg;
    if (!strcmp(coop_actor_name(a, nm, sizeof nm), "FireRodLv1Fire")) coop_enemy_shot_mirror(a);
}
static void held_rebuild(void) {
    if (cfg_get("TOTK_COOP_EN_TEST_GMC")) coop_actors_live(test_flame, NULL);
    int to = !atomic_load(&held_cur);
    _Atomic uint64_t *t = held_set[to], *by = held_by[to];
    static int said;
    for (int i = 0; i < HELD_N; i++) atomic_store_explicit(&t[i], 0, memory_order_relaxed);
    for (int i = 0; i < EN_MAP_N; i++) {
        uint64_t a = atomic_load(&en_map[i].actor), eu;
        if (a > 1) {   // (test) an enemy whose name starts with TOTK_COOP_EN_TEST_GMC: its equipment user's first slot links, once
            static uint64_t did;
            const char *want = cfg_get("TOTK_COOP_EN_TEST_GMC");
            char nm[48] = "";
            if (want && did != a && !strncmp(coop_actor_name(a, nm, sizeof nm), want, strlen(want))) {
                uint64_t e0 = coop_rd_ptr(en_comps(a) + 0x230), w[9] = {0};
                did = a;
                if (e0) coop_rd(e0 + 0x20, w, sizeof w);
                rt_log("coop: en: equip: %s equipment user %llx; +0x20: %llx %llx %llx | %llx %llx %llx | %llx %llx %llx", nm, (unsigned long long)e0, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2],
                       (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7], (unsigned long long)w[8]);
            }
        }
        if (a > 1) held_shooter(t, by, a, a);
        if (a <= 1 || !(eu = coop_rd_ptr(en_comps(a) + 0x230))) continue;
        for (int s = 0; s < EN_EQ_N; s++) {
            uint64_t ref = coop_rd_ptr(eu + 0x20 + (uint64_t)s * 0x18 + 8), w = ref ? coop_rd_ptr(ref + 0x40) : 0;
            int32_t ws = 0;
            // (a live actor; the link's id word is not compared: a Wizzrobe's rod did not pass that test and its ruby was
            // then nobody's)
            if (!w || !coop_rd(w + COOP_ACTOR_STATE, &ws, 4) || ws < 2 || ws > 6) continue;
            held_put(t, by, w, a);
            held_shooter(t, by, w, a);
            if (coop_debug()) {   // (record, TOTK_COOP_DEBUG=1) each held actor once: what it is and what its attachment link reads
                static uint64_t seen[64];
                static int nseen;
                int was = 0;
                for (int k = 0; k < nseen; k++) was |= seen[k] == w;
                if (!was && nseen < 64) {
                    uint64_t wc0 = coop_rd_ptr(en_comps(w) + 0x208);
                    char wn[40] = "", en[48] = "";
                    seen[nseen++] = w;
                    rt_log("coop: en: equip: %s slot %d holds %s (%llx); its component +0x208 is %llx, link at +0xb0: %llx %llx %llx", coop_actor_name(a, en, sizeof en), s, coop_actor_name(w, wn, sizeof wn),
                           (unsigned long long)w, (unsigned long long)wc0, (unsigned long long)(wc0 ? coop_rd_ptr(wc0 + 0xb0) : 0), (unsigned long long)(wc0 ? coop_rd_ptr(wc0 + 0xb8) : 0), (unsigned long long)(wc0 ? coop_rd_ptr(wc0 + 0xc0) : 0));
                }
            }
            uint64_t wc = coop_rd_ptr(en_comps(w) + 0x208), aref = wc ? coop_rd_ptr(wc + 0xb0 + 8) : 0, att = aref ? coop_rd_ptr(aref + 0x40) : 0;
            int32_t as = 0;
            if (coop_debug()) {   // (record) each weapon's attachment once: the actor the link gives, its name and state
                static uint64_t seen[64];
                static int nseen;
                int was = 0;
                for (int k = 0; k < nseen; k++) was |= seen[k] == w;
                if (!was && nseen < 64) {
                    char an[40] = "", wn[40] = "";
                    int32_t st = -1;
                    seen[nseen++] = w;
                    if (att) coop_rd(att + COOP_ACTOR_STATE, &st, 4);
                    rt_log("coop: en: equip: %s (%llx): its attachment link gives %llx %s, state %d", coop_actor_name(w, wn, sizeof wn), (unsigned long long)w, (unsigned long long)att, att ? coop_actor_name(att, an, sizeof an) : "-", (int)st);
                }
            }
            if (!att || !coop_rd(att + COOP_ACTOR_STATE, &as, 4) || as < 2 || as > 6) continue;
            held_put(t, by, att, a);
            held_shooter(t, by, att, a);
            if (said < 6) {
                char wn[40] = "", an[40] = "", en[48] = "";
                said++;
                rt_log("coop: en: equip: %s holds %s with %s fused to it: both are its own, not loose objects", coop_actor_name(a, en, sizeof en), coop_actor_name(w, wn, sizeof wn), coop_actor_name(att, an, sizeof an));
            }
        }
    }
    atomic_store_explicit(&held_cur, to, memory_order_release);
}

// ---- The switch (S 5) --------------------------------------------------------------------------------------------------
static __thread uint64_t tl_off_actor;   // the switched-off actor whose calc this thread entered last (the leak net)

extern __thread uint64_t tl_act_actor;   // coop_enemy_act.c: the copy whose action request the mod is making
int en_copy_hook(Cpu *c, uint64_t off) {
    if (!atomic_load_explicit(&en_active, memory_order_relaxed)) return 0;
    if (tl_act_actor) {   // a copy runs its owner's action (B.38): this one call tree goes through the switch, whole
        extern __thread uint32_t tl_act_ticks, tl_act_runs, tl_act_other;   // (counted for the prototype's log)
        extern __thread uint64_t tl_act_lr;
        if (off == 0x1920) { int en_act_tick_seen(uint64_t node); tl_act_ticks++; return en_act_tick_seen(c->x[0]); }
        if (off == 0x50ad74 && coop_rd_ptr(c->x[0] + 0x18) == tl_act_actor) return 0;
        if (off == 0x50b404 && c->x[30] - MOD_MAIN_BASE == 0x50afe4 && coop_rd_ptr(c->x[0] + 0x1f8) == tl_act_actor) { tl_act_runs++; return 0; }
        if (off == 0x50b404 || off == 0x50ad74 || off == 0x2beb80 || off == 0x44ff30 || off == 0x42e0c8) tl_act_other++, tl_act_lr = (c->x[30] - MOD_MAIN_BASE) | off << 32;
        if (off == FN_PLAY && coop_rd_ptr(c->x[0]) == tl_act_actor) {
            // A play asked by a node of the copy's action (the request's actor at +0): the copy's slots play what its
            // owner states (slots_apply), so this one is answered "played" and not made (B.41).
            int en_act_plays(void);
            extern __thread uint32_t tl_act_plays;
            if (!en_act_plays()) { tl_act_plays++; c->x[0] = 1; return 1; }
        }
    } else if (off == 0x50ad74) { void en_act_owner(Cpu *c); en_act_owner(c); }   // (of an enemy this game runs: told)
    uint64_t actor;
    switch (off) {
    case 0x50b404: case 0x2beb80: case 0x42e0c8: actor = coop_rd_ptr(c->x[0] + 0x1f8); break;
    case 0x44ff30: actor = coop_rd_ptr(c->x[0] + 0xb70); break;
    case 0x50ad74: actor = coop_rd_ptr(c->x[0] + 0x18); break;
    case 0x1920: {   // a node's tick: none may run for a switched-off actor
        if (!tl_off_actor) return 0;
        uint64_t ctrl = coop_rd_ptr(coop_rd_ptr(c->x[0] + 8) + 8);
        actor = ctrl ? coop_rd_ptr(ctrl + 0x1f8) : 0;
        if (actor != tl_off_actor) return 0;
        EnCell *m = en_cell_of(actor);
        if (!m || !is_off(atomic_load(&m->state)) || !m->sw_on) return 0;
        if (atomic_fetch_add(&m->leaks, 1) < 3)
            rt_log("coop: en: leak: a node of %llx ticked with the switch on (from main+%#llx)", (unsigned long long)actor, (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
        return 1;
    }
    case 0x1bf9af8: case 0x1bfbc44: case 0x1bfbaa4: case 0x1bf8f74: case 0x1bfbb28: case 0x1bfd6a8: case 0xa9c250: {
        // The Shooter component (x0 = S, its owning actor at S+0x18; for main+0xa9c250 x0 is a shot record and w1 its new
        // state): prepare, shoot and request entries, logged for the enemies this sync knows, with what the game that
        // runs the enemy and a copy each do.
        if (off == 0x1bf9af8 || off == 0x1bfbc44) {   // of an enemy this game runs: made here, captured and stated (coop_enemy_shot.c)
            uint64_t a0 = coop_rd_ptr(c->x[0] + 0x18);
            EnCell *m0 = a0 > 0x10000 ? en_cell_of(a0) : NULL;
            if (!m0 && a0 > 0x10000) { uint64_t h0 = coop_enemy_holder(a0); m0 = h0 ? en_cell_of(h0) : NULL; }   // (the Shooter of a thing an enemy holds)
            if (m0 && en_shot_hook(c, off, m0)) return 1;
        }
        static int on = -1;
        static _Atomic int said;
        if (on < 0) on = cfg_get("TOTK_COOP_EN_TEST_SHOTS") != NULL;
        if (!on || off == 0xa9c250) return 0;
        uint64_t a = coop_rd_ptr(c->x[0] + 0x18);
        EnCell *m = a > 0x10000 ? en_cell_of(a) : NULL;
        {   // (every caller, for the first calls: which actors use these at all)
            static _Atomic int any;
            char an[48] = "?";
            if (atomic_fetch_add(&any, 1) < 40) rt_log("coop: en: shot: (any) main+%#llx x0 %llx, [x0+0x18] %llx %s, from main+%#llx", (unsigned long long)off, (unsigned long long)c->x[0], (unsigned long long)a, a > 0x10000 ? coop_actor_name(a, an, sizeof an) : "-", (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
        }
        if (!m || atomic_fetch_add(&said, 1) >= 400) return 0;
        uint64_t rec = c->x[1], q = off == 0x1bfbc44 ? c->x[2] : c->x[1];
        uint32_t key = 0, idx = 0, flags = 0;
        float v[12] = {0};
        uint8_t st = 0;
        if (q > 0x10000) coop_rd(q + 0x1928, &key, 4), coop_rd(q + 0x192c, &idx, 4), coop_rd(q + 0x1938, &flags, 4), coop_rd(q + 0x1930, v, sizeof v);
        if (off == 0x1bfbc44 && rec > 0x10000) coop_rd(rec + 0xdf, &st, 1);
        rt_log("coop: en: shot: main+%#llx on %llx (cell state %d) from main+%#llx: x1 %llx x2 %llx x3 %llx; request key %08x index %d flags %x; vectors %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f; record state %u",
               (unsigned long long)off, (unsigned long long)a, (int)atomic_load(&m->state), (unsigned long long)(c->x[30] - MOD_MAIN_BASE), (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
               key, (int)idx, flags, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], st);
        return 0;
    }
    case 0x1bf20cc: { int en_arm_hook(Cpu *c, uint64_t off); return en_arm_hook(c, off); }   // coop_enemy_arm.c
    case 0x1bfa698: case 0x1bfa87c: case 0x1bfa4e0: { int en_shs_hook(Cpu *c, uint64_t off); return en_shs_hook(c, off); }   // coop_enemy_shot.c: a Shooter put to sleep
    case 0x25aafec: case 0x21cdf44: case 0x1d71dfc: case 0x24901b0: case 0x24902e0: case 0xa80f28: { int en_fx_hook(Cpu *c, uint64_t off); return en_fx_hook(c, off); }   // coop_enemy_fx.c
    case 0xd7ff68: { int coop_mount_herd_hook(Cpu *c); return coop_mount_herd_hook(c); }   // a wild herd is made in one game (coop_mount.c)
    case 0x21cd650: { void en_moon_local(void); en_moon_local(); return 0; }   // the blood moon begins here (coop_enemy.c)
    case 0x1c78060: case 0x1b738e4: case 0x1b73be4: case 0xb28394: { int en_loot_hook(Cpu *c, uint64_t off); return en_loot_hook(c, off); }   // coop_enemy_loot.c
    case FN_BRAIN_VERB: {   // (x0 the brain, w1 the verb, w2 bit 0 force): a held brain keeps its owner's verb
        if (tl_brain_mine || !tl_grace_bsa || c->x[0] != tl_grace_bsa) return 0;
        static _Atomic int said;
        uint32_t v = (uint32_t)c->x[1];
        int idle = v == 1 || v == 2 || v == 8 || v == 18 || v == 19;
        int drop = tl_grace_narrow ? idle && !(c->x[2] & 1) && v != tl_grace_verb : v != tl_grace_verb;
        if (drop && tl_grace_narrow && tl_grace_cell) tl_grace_cell->hold_frame = coop_sess_frame_no() + 2;   // (the start has come: its other nodes run in this tick and the next)
        if (atomic_fetch_add(&said, 1) < 80) rt_log("coop: en: brain: a verb asked of a held brain: %u (force %llu) from main+%#llx: %s", (uint32_t)c->x[1], (unsigned long long)(c->x[2] & 1), (unsigned long long)(c->x[30] - MOD_MAIN_BASE), drop ? "turned away" : "let through");
        if (!drop) return 0;
        c->x[0] = 0;
        return 1;
    }
    case FN_TARGETS_RESET: {   // the reset of the target slots, in the calc of a held brain's actor
        if (!tl_grace_bsa) return 0;
        static _Atomic int said;
        if (tl_grace_narrow && tl_grace_cell) tl_grace_cell->hold_frame = coop_sess_frame_no() + 2;
        if (atomic_fetch_add(&said, 1) < 40) rt_log("coop: en: brain: the target slots' reset of a held brain turned away (from main+%#llx)", (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
        return 1;
    }
    case 0xbb0dd0: {   // the model controller's show/hide (x0 the controller, w1 visible, w2 the reason): logged for known enemies
        static _Atomic int said;
        uint64_t a = coop_rd_ptr(c->x[0] + 0x18);
        EnCell *m = a > 0x10000 ? en_cell_of(a) : NULL;
        if (m && atomic_fetch_add(&said, 1) < 60)
            rt_log("coop: en: hide: main+0xbb0dd0 on %llx (cell state %d): visible %llu, reason %llu, from main+%#llx", (unsigned long long)a, (int)atomic_load(&m->state), (unsigned long long)(c->x[1] & 1), (unsigned long long)c->x[2], (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
        return 0;
    }
    case 0x1bddcb0: {   // a ragdoll start (x0 the component, x1 the request): logged for the enemies this sync knows
        uint64_t a = coop_rd_ptr(c->x[0] + 0x18);
        EnCell *m = a > 0x10000 ? en_cell_of(a) : NULL;
        // A COPY starts no ragdoll of its own, partial or full (TOTK_COOP_EN_TEST_SKIP=r lets it): its main body is static
        // and its root is written, and a partial ragdoll started on it (the flinch this sync used to ask for, or the one
        // its own damage step asks when this game's player strikes it) threw the model into the sky until it ended, when
        // it snapped back (a tester watching the runs, 2026-11-16; with the flinch request off, no more). What the
        // owner's enemy's ragdoll looks like, flinch included, comes as bones (coop_enemy_pose.c).
        if (m && atomic_load_explicit(&m->state, memory_order_relaxed) == EM_COPY && !en_test_skip('r')) {
            static _Atomic int said_c;
            if (atomic_fetch_add(&said_c, 1) < 20) rt_log("coop: en: rag: a ragdoll start on the copy %llx turned away (from main+%#llx): its owner's comes as bones", (unsigned long long)a, (unsigned long long)(c->x[30] - MOD_MAIN_BASE));
            c->x[0] = 0;
            return 1;
        }
        static _Atomic int said;
        uint8_t rq[0x40] = {0};
        if (!m || !coop_rd(c->x[1], rq, sizeof rq) || atomic_fetch_add(&said, 1) >= 60) return 0;
        char hex[0x40 * 2 + 20];
        for (int i = 0; i < 0x40; i++) snprintf(hex + i * 2 + i / 8, 4, "%02x%s", rq[i], (i & 7) == 7 ? " " : "");
        rt_log("coop: en: rag: start asked of %llx (cell state %d) from main+%#llx: %s", (unsigned long long)a, (int)atomic_load(&m->state), (unsigned long long)(c->x[30] - MOD_MAIN_BASE), hex);
        return 0;
    }
    default: return 0;
    }
    EnCell *m = actor > 0x10000 ? en_cell_of(actor) : NULL;
    if (!m) return 0;
    atomic_store_explicit(&m->run_frame, coop_sess_frame_no(), memory_order_relaxed);
    if (!is_off(atomic_load_explicit(&m->state, memory_order_relaxed)) || noswitch() || !atomic_load_explicit(&m->warm, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&m->passed, 1, memory_order_relaxed);
        return 0;
    }
    atomic_fetch_add_explicit(&m->turned, 1, memory_order_relaxed);
    if (off == 0x50ad74) c->x[0] = 2;   // what a caller of a request without a slot gets
    return 1;
}

static uint64_t inline_inst(uint64_t actor, uint64_t *ctrl_out) {
    uint64_t bsa = en_bsa(actor), rec = bsa ? coop_rd_ptr(bsa + 0x2618) : 0, ctrl = rec ? coop_rd_ptr(rec + 0x18) : 0;
    *ctrl_out = ctrl;
    return ctrl ? coop_rd_ptr(coop_rd_ptr(ctrl + 0x20)) : 0;
}
static void four_writes(uint64_t actor, EnCell *m) {
    uint64_t ctrl, inst = inline_inst(actor, &ctrl);
    if (!ctrl || !inst) return;
    // (all four are kept: on enemies that had run for a while the root index and the pending root read -1 already,
    // but a young actor's can still name the root its interrupt graph has to start)
    if (!m->sw_saved) {
        m->sw_saved = 1, m->sw_root = -1, m->sw_pend = -1;
        coop_rd(inst + 0x90, &m->sw_qcount, 4), coop_rd(inst + 0xd8, &m->sw_bits, 2), coop_rd(ctrl + 0x19c, &m->sw_root, 4), coop_rd(inst + 0xde, &m->sw_pend, 2);
    }
    int32_t m1 = -1;
    int16_t s1 = -1;
    uint32_t zero = 0;
    uint16_t w = 0;
    memcpy((void *)(uintptr_t)(ctrl + 0x19c), &m1, 4);
    memcpy((void *)(uintptr_t)(inst + 0xde), &s1, 2);
    memcpy((void *)(uintptr_t)(inst + 0x90), &zero, 4);
    coop_rd(inst + 0xd8, &w, 2);
    w &= (uint16_t)~1u;
    memcpy((void *)(uintptr_t)(inst + 0xd8), &w, 2);
}
static uint64_t main_body(uint64_t actor) {
    uint64_t phys = coop_rd_ptr(en_comps(actor) + 0x50), ctl = phys ? coop_rd_ptr(phys + 0x20) : 0;
    return ctl ? coop_rd_ptr(ctl + 0x150) : 0;
}
static int body_type(uint64_t body) {   // 0 static, 1 keyframed, 2 dynamic, as main+0xae4300 reads it
    uint64_t flags = coop_rd_ptr(body + 0x68), req = coop_rd_ptr(body + 0x60);
    uint32_t rq = 0, rt = 0;
    if (req && coop_rd(req + 0xd4, &rq, 4) && (rq & 4) && coop_rd(req + 0x80, &rt, 4)) return rt > 2 ? 2 : (int)rt;
    return flags & 0x40 ? 0 : flags & 0x80 ? 1 : 2;
}
static void put(Cpu *c, uint64_t actor, const float pos[3], const float quat[4]) {
    float r[9];
    en_rot_of_quat(quat, r);
    _Alignas(16) float m12[12] = { r[0], r[1], r[2], pos[0], r[3], r[4], r[5], pos[1], r[6], r[7], r[8], pos[2] };
    coop_guest_call(c, MOD_MAIN_BASE + COOP_FORCE_MATRIX, actor, (uint64_t)(uintptr_t)m12, 0, 0);
    // The call moves the body. The actor's own place follows its body only while its move control runs, which the
    // switch turns away (two-game run: the bodies of the camp's copies were at the pose asked, their actors where
    // they had stood). So the actor's two transforms are written as the call writes them for an actor with no body.
    memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_POS), pos, 12), memcpy((void *)(uintptr_t)(actor + COOP_ACTOR_ROT), r, 36);
    memcpy((void *)(uintptr_t)(actor + 0x2f0), pos, 12), memcpy((void *)(uintptr_t)(actor + 0x2fc), r, 36);
}

// The stated slots on this game's actor (S 7.3). Before its calc, so a command asked now shows this frame.
static void slots_apply(Cpu *c, uint64_t actor, EnCell *m, const EnSlot *want, int nwant, int hold, uint32_t frame) {
    uint64_t g = anim_g(actor);
    int n = anim_count(g);
    for (int i = 0; i < n && i < nwant; i++) {
        const EnSlot *o = &want[i];
        uint64_t s = anim_slot(g, i);
        if (!s || !o->cmd) continue;
        char cmd[40];
        anim_cmd(s, cmd, sizeof cmd);
        int played = 0;
        if (fnv_str(cmd) != o->cmd || !cmd[0]) {
            // A command this actor did not take the last time is asked again 5 times a second at most. Only a play that
            // did not take starts that wait: a command that follows another within 200 ms (Turn, then Attack) plays at
            // once. When every play was limited, such a copy started its attack 12 frames late, and the clock jump
            // that caught it up does not move the clip's events, so it armed that much late (facts/enemies.md 5.3).
            const char *nm = name_of(o->cmd);
            if (!nm || frame - m->slot_try[i] < 12) continue;
            static __thread _Alignas(16) uint8_t req[0x60];
            uint64_t np = (uint64_t)(uintptr_t)nm;
            uint32_t slot = (uint32_t)i;
            float m1 = -1.f;
            memset(req, 0, sizeof req);
            memcpy(req, &actor, 8), memcpy(req + 8, &np, 8), memcpy(req + 0x10, &slot, 4), memcpy(req + 0x14, &o->rate, 4);
            memcpy(req + 0x18, &m1, 4), memcpy(req + 0x1c, &m1, 4);
            coop_guest_call(c, MOD_MAIN_BASE + FN_PLAY, (uint64_t)(uintptr_t)req, 0, 0, 0);
            atomic_fetch_add_explicit(&m->n_play, 1, memory_order_relaxed);
            played = 1;
            anim_cmd(s, cmd, sizeof cmd);   // (the play changes the slot's command at once: facts/enemies.md 5.1)
            if (fnv_str(cmd) != o->cmd) m->slot_try[i] = frame;
        }
        int cur = 0;
        uint64_t ctx = anim_ctx(s, &cur);
        if (!ctx || o->cur >= EN_CTRL_N) continue;
        float local = 0, rate = o->rate, wantc = o->c[o->cur].clock;
        coop_rd(ctx + 0x68, &local, 4);
        float e = wantc - (local + 0.5f * rate);   // (the calc that follows advances the clock by half a 30th per 60th of a second)
        if (hold) {   // no state after T: place and clip stand together
            __atomic_fetch_or((uint32_t *)(uintptr_t)(ctx + 0xa0), 0x400u, __ATOMIC_SEQ_CST);
            continue;
        }
        if (played || e > 3.f || e < -3.f) {
            memcpy((void *)(uintptr_t)(ctx + 0x68), &wantc, 4);
            __atomic_fetch_or((uint32_t *)(uintptr_t)(ctx + 0xa0), 0x400u, __ATOMIC_SEQ_CST);
            if (!played) atomic_fetch_add_explicit(&m->n_hard, 1, memory_order_relaxed);
        } else if (e > 0.5f || e < -0.5f) {
            float k = e / 30.f;
            rate *= 1.f + (k > 0.1f ? 0.1f : k < -0.1f ? -0.1f : k);
            atomic_fetch_add_explicit(&m->n_soft, 1, memory_order_relaxed);
        }
        memcpy((void *)(uintptr_t)(s + 0xd0), &rate, 4);
    }
}

void en_copy_precalc(Cpu *c, uint64_t actor, EnCell *m) {
    int st = atomic_load_explicit(&m->state, memory_order_relaxed);
    int off = is_off(st) && !noswitch() && atomic_load_explicit(&en_active, memory_order_relaxed);
    if (!atomic_load_explicit(&m->warm, memory_order_relaxed)) {
        // A new actor thinks until its interrupt graph has started (its instance has a queue count or a bit word), at
        // most 30 frames. Switched off from its first frame, what was saved of that graph was nothing, and the nothing
        // was put back when the actor was let think: it then never ran an interrupt action again, and a spawned
        // Lizalfos at 0 life went on fighting with no death (a play session and its scripted repeats, 2026-11-16).
        uint64_t ctrl, inst = inline_inst(actor, &ctrl);
        uint32_t q = 0;
        uint16_t bits = 0;
        if (inst) coop_rd(inst + 0x90, &q, 4), coop_rd(inst + 0xd8, &bits, 2);
        if ((inst && (q || bits)) || coop_sess_frame_no() - m->born_frame >= 30) {
            // (Whether its graph had started is kept: a brain whose graph has not is asked nothing, B.37.)
            m->graph_started = inst && (q || bits);
            if (!m->graph_started) {
                static _Atomic int said;
                char nm[48];
                if (atomic_fetch_add(&said, 1) < 20) rt_log("coop: en: brain: %s %llx is switched off before its graph started (30 frames): its brain is asked nothing until it has", coop_actor_name(actor, nm, sizeof nm), (unsigned long long)actor);
            }
            atomic_store(&m->warm, 1);
        } else off = 0;
    }
    tl_off_actor = off ? actor : 0;
    tl_grace_bsa = 0, tl_grace_narrow = 0, tl_grace_cell = NULL;
    if (off) m->grace_frame = m->hold_frame = 0;
    if (!off && m->grace_frame) {
        uint32_t frame = coop_sess_frame_no();
        if ((int32_t)(m->grace_frame - frame) > 0) {
            brain_hold(c, actor, m, "held");
            tl_grace_bsa = brain_bsa(actor), tl_grace_verb = m->grace_verb;
        } else m->grace_frame = 0, m->brain_log_frame = frame, m->hold_frame = frame + BRAIN_HOLD;
    }
    if (!off && !m->grace_frame && m->hold_frame) {
        uint32_t frame = coop_sess_frame_no();
        if ((int32_t)(m->hold_frame - frame) > 0) tl_grace_bsa = brain_bsa(actor), tl_grace_verb = m->grace_verb, tl_grace_narrow = 1, tl_grace_cell = m;
        else {
            m->hold_frame = 0;
            rt_log("coop: en: brain: %llx is let go, %u frames after its take-over: its own from here", (unsigned long long)actor, frame - m->brain_log_frame);
        }
    }
    if (!off && m->brain_log_frame && !m->grace_frame) {   // the record: once a second for 10 s after a take-over
        uint32_t frame = coop_sess_frame_no(), d = frame - m->brain_log_frame;
        if (d > 600) m->brain_log_frame = 0;
        else if (d && !(d % 30)) {
            uint8_t v, t;
            brain_read(actor, &v, &t);
            uint64_t s0 = brain_slot(actor, 0), ta = brain_slot_actor(s0);
            float since = -1, tp[3] = {0}, ep[3] = {0}, cp[3] = {0};
            uint32_t em = 0, fl = 0;
            uint8_t k93 = 0, ke2 = 0;
            char tn[48] = "";
            if (s0) coop_rd(s0 + 0x80, &since, 4), coop_rd(s0 + 0x84, &em, 4), coop_rd(s0 + 0x90, &fl, 4), coop_rd(s0 + 0x93, &k93, 1), coop_rd(s0 + 0xe2, &ke2, 1), coop_rd(s0 + 0x20, cp, 12);
            if (ta) coop_rd(ta + COOP_ACTOR_POS, tp, 12);
            coop_rd(actor + COOP_ACTOR_POS, ep, 12);
            rt_log("coop: en: brain: %llx %u s after its take-over: verb %u, target %u (0 no one, 1 + player, 255 another actor); memory %02x; slot 0: %s, not seen for %.2f s, emitter %d, flags %08x, kinds %u %u, %.1f m away, its kept place %.1f m from the target",
                   (unsigned long long)actor, d / 30, v, t, mem_read(actor), ta ? coop_actor_name(ta, tn, sizeof tn) : "-", since, (int)em, fl, k93, ke2,
                   ta ? sqrtf((tp[0] - ep[0]) * (tp[0] - ep[0]) + (tp[1] - ep[1]) * (tp[1] - ep[1]) + (tp[2] - ep[2]) * (tp[2] - ep[2])) : -1.0f,
                   ta ? sqrtf((tp[0] - cp[0]) * (tp[0] - cp[0]) + (tp[1] - cp[1]) * (tp[1] - cp[1]) + (tp[2] - cp[2]) * (tp[2] - cp[2])) : -1.0f);
        }
    }
    if (!off && !m->sw_on) {
        {   // (its target put back to the player it had: B.22)
            uint8_t back = atomic_exchange(&m->tgt_back, 0);
            if (back && st == EM_OWNED) { uint8_t gv = m->grace_verb; m->grace_verb = 0, m->grace_tgt = back; brain_hold(c, actor, m, "its target put back"); m->grace_verb = gv; }
        }
        en_hit_precalc(c, actor, m, 0);
        return;
    }
    int32_t ast = 0;
    if (!coop_rd(actor + COOP_ACTOR_STATE, &ast, 4) || ast < 2 || ast > 6) return;
    uint64_t x[31], sp = c->sp, pc = c->pc, body = main_body(actor);
    memcpy(x, c->x, sizeof x);
    if (!off) {   // it thinks again (S 4.8): the interrupt graph gets back what the writes took, the body moves again
        uint64_t ctrl, inst = inline_inst(actor, &ctrl);
        if (inst && m->sw_saved) {
            memcpy((void *)(uintptr_t)(inst + 0x90), &m->sw_qcount, 4), memcpy((void *)(uintptr_t)(inst + 0xd8), &m->sw_bits, 2);
            memcpy((void *)(uintptr_t)(ctrl + 0x19c), &m->sw_root, 4), memcpy((void *)(uintptr_t)(inst + 0xde), &m->sw_pend, 2);
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 12) rt_log("coop: en: switch: %llx thinks again: its interrupt graph gets back queue count %u, bits %x, root %d, pending root %d", (unsigned long long)actor, m->sw_qcount, m->sw_bits, (int)m->sw_root, (int)m->sw_pend);
        }
        en_hit_leave(actor, m);
        if (body && m->body_static) coop_guest_call(c, MOD_MAIN_BASE + FN_MOTION_TYPE, body, 2, 0, 0);
        if (st == EM_OWNED && m->show_verb && !en_test_skip('b')) {   // this game runs it from here: its brain goes on where its owner's was
            m->grace_verb = m->show_verb, m->grace_tgt = m->show_tgt, m->grace_frame = coop_sess_frame_no() + BRAIN_GRACE;
            mem_write(actor, m->show_mem, "the enemy taken over");
            brain_hold(c, actor, m, "taken over");
            tl_grace_bsa = brain_bsa(actor), tl_grace_verb = m->grace_verb;
        }
        {   // It is drawn again, with what it holds: the game that takes an enemy over when its owner left or paused may get
            // it unseen, and its own graphs would not know to show it.
            uint64_t who[1 + EN_EQ_N * 2], eu = coop_rd_ptr(en_comps(actor) + 0x230);
            int nw = 0;
            uint8_t zero = 0;
            who[nw++] = actor;
            for (int k = 0; eu && k < EN_EQ_N; k++) {
                uint64_t ref = coop_rd_ptr(eu + 0x20 + (uint64_t)k * 0x18 + 8), w = ref ? coop_rd_ptr(ref + 0x40) : 0;
                if (!w) continue;
                who[nw++] = w;
                uint64_t wc = coop_rd_ptr(en_comps(w) + 0x208), aref = wc ? coop_rd_ptr(wc + 0xb0 + 8) : 0, att = aref ? coop_rd_ptr(aref + 0x40) : 0;
                if (att) who[nw++] = att;
            }
            for (int k = 0; k < nw; k++) {
                uint64_t gmc = coop_rd_ptr(en_comps(who[k]) + 0x280);
                uint8_t hm = 0;
                if (!gmc || !coop_rd(gmc + 0x9d, &hm, 1) || !hm) continue;
                memcpy((void *)(uintptr_t)(gmc + 0x9d), &zero, 1);
                __atomic_fetch_or((uint32_t *)(uintptr_t)(gmc + 0xac), 0x200u, __ATOMIC_SEQ_CST);
            }
        }
        m->sw_on = m->sw_saved = m->body_static = m->pinned = 0;
        memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
        return;
    }
    { void en_pose_copy_precalc(uint64_t actor, EnCell *m); en_pose_copy_precalc(actor, m); }   // (test: coop_enemy_pose.c)
    m->sw_on = 1;
    four_writes(actor, m);
    // A copy holds its owner's brain verb and battle target all the time, not only when it is taken over (B.21): the verb
    // asked with force runs the game's own procedures for it (in battle: the head's look-at at target slot 0), and slot
    // 0 names this game's actor of the player its owner's enemy fights. Only at a change. TOTK_COOP_EN_TEST_SKIP=v: not.
    if (m->show_verb && !en_test_skip('v')) m->grace_verb = m->show_verb, m->grace_tgt = m->show_tgt, brain_hold(c, actor, m, "a copy");
    if (body && !m->body_static && !en_test_skip('s')) coop_guest_call(c, MOD_MAIN_BASE + FN_MOTION_TYPE, body, 0, 0, 0), m->body_static = 1;
    // What to show: phase 0 wrote it whole under the sequence.
    float pos[3], quat[4];
    static __thread EnSlot slot[EN_SLOT_N];
    int on = 0, nslot = 0, hold = 0, tele = 0;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s0 = atomic_load_explicit(&m->seq, memory_order_acquire);
        if (s0 & 1) continue;
        on = m->show_on, nslot = m->show_nslot, hold = m->show_hold, tele = m->show_teleport;
        memcpy(pos, m->show_pos, 12), memcpy(quat, m->show_quat, 16), memcpy(slot, m->show_slot, sizeof slot);
        if (atomic_load_explicit(&m->seq, memory_order_acquire) == s0) break;
        on = 0;
    }
    (void)tele;
    if (st == EM_COPY && on) {
        if (m->pinned == 2) {   // was it where it was put a frame ago?
            float p[3];
            if (coop_rd(actor + COOP_ACTOR_POS, p, 12)) {
                float dx = p[0] - m->pin[0], dy = p[1] - m->pin[1], dz = p[2] - m->pin[2];
                if (dx * dx + dy * dy + dz * dz > 0.05f * 0.05f) {
                    atomic_fetch_add_explicit(&m->n_off, 1, memory_order_relaxed);
                    if (body && body_type(body) != 0) coop_guest_call(c, MOD_MAIN_BASE + FN_MOTION_TYPE, body, 0, 0, 0);   // (something made it move again)
                }
            }
        }
        if (!coop_mount_precalc_ride(c, actor, m)) {   // (a rider seated on a horse here is placed by its seat: coop_mount.c)
            put(c, actor, pos, quat);
            memcpy(m->pin, pos, 12), memcpy(m->pin + 3, quat, 16), m->pinned = 2;
        } else m->pinned = 0;
        slots_apply(c, actor, m, slot, nslot, hold, coop_sess_frame_no());
        coop_mount_precalc(c, actor, m);
        for (int k = 0; k < EN_EQ_N; k++) {
            // What the owner's enemy holds and the copy does not (a weapon fetched again, one picked up) the copy is given by
            // the game's own equipment create, called as coop_players.c give_gear calls it for a stand-in: in the hand,
            // default life, no fused attachment. At most one create in two seconds.
            uint32_t want = atomic_exchange_explicit(&m->eq_make[k], 0, memory_order_relaxed);
            const char *wn = want ? name_of(want) : NULL;
            // (what an enemy can hold: never a player, a character, another enemy or a chest, whatever name the owner states)
            static const char *const no_eq[] = {"Player", "Npc", "Enemy_", "Animal_", "Horse_", "TBox_", "Dgn", "Miasma", "Armor_"};
            for (size_t j = 0; wn && j < sizeof no_eq / sizeof *no_eq; j++) if (!strncmp(wn, no_eq[j], strlen(no_eq[j]))) wn = NULL;
            uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
            uint32_t fr = coop_sess_frame_no();
            if (!wn || !eu || fr - m->eq_made_frame < 120 || m->eq_tries[k] >= 2) continue;
            m->eq_made_frame = fr, m->eq_tries[k]++;
            for (int j = 0; j < 2; j++) {
                uint8_t *st = m->eq_str[j];
                memset(st, 0, 0x60);
                const char *an = m->eq_att[k] ? name_of(m->eq_att[k]) : NULL;   // (what is fused to its owner's: the create's seventh argument)
                snprintf((char *)st + 0x10, 0x40, "%s", j ? an ? an : "" : wn);
                uint64_t tp = (uint64_t)(uintptr_t)(st + 0x10);
                int32_t cap = 0x40;
                memcpy(st, &tp, 8), memcpy(st + 8, &cap, 4);
            }
            uint64_t sp0 = c->sp, sp = (sp0 - 0x100) & ~0xfull;
            uint64_t stack[6] = {0, 0, 0, (uint64_t)(uint32_t)-1, 0, (uint64_t)(uint32_t)-1};   // arguments 9 to 14
            memcpy((void *)(uintptr_t)sp, stack, sizeof stack);
            c->sp = sp;
            uint64_t r = rt_call(c, MOD_MAIN_BASE + 0xaf100c, 8, eu, (uint64_t)k, (uint64_t)(uintptr_t)m->eq_str[0], (uint64_t)1, (uint64_t)(uint32_t)-1,
                                 (uint64_t)(uint32_t)-1, (uint64_t)(uintptr_t)m->eq_str[1], (uint64_t)1);
            c->sp = sp0;
            rt_log("coop: en: equip: the copy %llx is given %s in slot %d, as its owner's enemy holds (the create answered %llx; try %d)", (unsigned long long)actor, wn, k, (unsigned long long)r, m->eq_tries[k]);
            break;
        }
        {   // Drawn or put away as its owner's (main+0x4f59ec(equipment user, slot, 1 draw | 2 sheathe)): the slot's link is
            // the same in both states, so the held-things rule never saw it. One call a second a slot at most.
            uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
            uint32_t fr = coop_sess_frame_no();
            for (int k = 0; eu && k < EN_EQ_N && fr - m->eq_state_frame >= 30; k++) {
                uint8_t own = 0, want = m->eq_state[k] & 0x3f;
                if ((want != 2 && want != 4) || !coop_rd(eu + 0x4620 + (uint64_t)k, &own, 1) || own == want || (own != 2 && own != 4)) continue;
                m->eq_state_frame = fr;
                coop_guest_call(c, MOD_MAIN_BASE + 0x4f59ec, eu, (uint64_t)k, want == 2 ? 1 : 2, 0);
                static _Atomic int said;
                if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: equip: the copy %llx %s what its slot %d holds, as its owner's (state %u -> %u)", (unsigned long long)actor, want == 2 ? "draws" : "puts away", k, own, want);
                break;
            }
        }
        {   // What a slot holds is shown or not as its owner's (B.34), written as the game's setter does: the byte and the
            // command bit, which the equipment user's own calc then acts on.
            uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
            for (int k = 0; eu && k < EN_EQ_N; k++) {
                uint8_t want = m->eq_state[k], own = 0;
                uint64_t sl = eu + 0x570 + (uint64_t)k * 0x100, ref = coop_rd_ptr(eu + 0x20 + (uint64_t)k * 0x18 + 8);
                if (!(want & 0x40) || !ref || !coop_rd_ptr(ref + 0x40) || !coop_rd(sl + 0xf4, &own, 1) || !own == !(want & 0x80)) continue;
                uint8_t v = want & 0x80 ? 1 : 0;
                memcpy((void *)(uintptr_t)(sl + 0xf4), &v, 1);
                __atomic_fetch_or((uint32_t *)(uintptr_t)(sl + 0xf0), 0x200u, __ATOMIC_SEQ_CST);
                static _Atomic int said;
                if (atomic_fetch_add(&said, 1) < 30) rt_log("coop: en: equip: the copy %llx %s what its slot %d holds, as its owner's", (unsigned long long)actor, v ? "shows" : "hides", k);
            }
        }
        if (atomic_load_explicit(&m->throw_due, memory_order_acquire)) {   // its owner's enemy threw what this slot holds: the same throw, by the game's own call (main+0x1b73be4)
            static __thread _Alignas(16) uint8_t req[0x30];
            memcpy(req, m->throw_req, 0x30);
            int slot = m->throw_slot;
            uint64_t x3 = m->throw_x3, x4 = m->throw_x4;
            atomic_store_explicit(&m->throw_due, 0, memory_order_release);
            uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230), ref = eu ? coop_rd_ptr(eu + 0x20 + (uint64_t)slot * 0x18 + 8) : 0, held = ref ? coop_rd_ptr(ref + 0x40) : 0;
            if (eu && held) {
                void en_loot_thrown(uint64_t actor);   // coop_enemy_loot.c: this game's own flying copy, gone when it rests
                char hn[40] = "";
                en_loot_thrown(held);
                uint64_t r = rt_call(c, MOD_MAIN_BASE + 0x1b73be4, 5, eu, (uint64_t)(uintptr_t)req, (uint64_t)slot, x3, x4);
                rt_log("coop: en: equip: the copy %llx throws its %s (slot %d), as its owner's enemy did (the call answered %llx)", (unsigned long long)actor, coop_actor_name(held, hn, sizeof hn), slot, (unsigned long long)r);
            }
        }
        uint8_t drop = atomic_exchange_explicit(&m->eq_drop, 0, memory_order_relaxed);
        for (int k = 0; drop && k < EN_EQ_N; k++) {
            // What the owner's enemy no longer holds (dropped, broken, knocked away) the copy's is rid of: the held actor is
            // deleted by request, as coop_ctn.c part_delete asks (a ready actor with its lock object). The owner's
            // dropped thing itself is a world object and the object mirror's.
            if (!(drop >> k & 1)) continue;
            uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230), ref = eu ? coop_rd_ptr(eu + 0x20 + (uint64_t)k * 0x18 + 8) : 0, held = ref ? coop_rd_ptr(ref + 0x40) : 0;
            int32_t hs = 0;
            if (!held || !coop_rd(held + COOP_ACTOR_STATE, &hs, 4) || hs < 2 || hs > 6 || !coop_rd_ptr(held + 0x1a8)) continue;
            char hn[40] = "";
            uint64_t r = coop_guest_call(c, MOD_MAIN_BASE + COOP_REQUEST_DELETE, held, 0, 0, 0);
            rt_log("coop: en: equip: the copy %llx lets go of its %s (slot %d): its owner's enemy holds nothing there (the delete answered %llu)", (unsigned long long)actor, coop_actor_name(held, hn, sizeof hn), k, (unsigned long long)(r & 1));
        }
        en_hit_precalc(c, actor, m, 1);
        en_shot_precalc(c, actor, m);
        {   // Not drawn while its owner's is not (a Wizzrobe flies unseen between its Disappear and Appear clips). The
            // model controller ([comps+0x280]) holds the request at +0x9d; its setter main+0x1b87b74 writes the byte and
            // sets dirty bit 0x200 of the word at +0xac, and the controller's apply then tells the models (research
            // R7b). Written the same way here, on the copy and on what it holds and what is fused to that: they are
            // actors of their own with controllers of their own. [m: the byte alone, without the dirty bit, hid nothing.]
            {   // its owner's AI memory, kept in the copy's own blackboard for the day this game runs it
                int16_t mei = atomic_load(&m->en);
                mem_write(actor, m->show_mem, mei >= 0 ? en_tab[mei].name : "?");
            }
            { int16_t aei = atomic_load(&m->en); if (aei >= 0 && !(coop_sess_frame_no() & 7)) attn_copy(c, actor, &en_tab[aei]); }   // the prompts its owner's offers
            // (The battle-info component's flags ([comps+0x140]+0x38) are NOT written on a copy: tried, and the copy's own
            // component puts its own value back every frame (measured: still 0 or 0x200 at phase 0 against its owner's
            // 0x720 in battle). What works them out on an enemy that runs is not read: B.20, open.)
            // (its action first: what its own nodes start is known when the stated effects are looked at)
            { int en_act_precalc(Cpu *c, uint64_t actor, EnCell *m); m->act_run = (uint8_t)en_act_precalc(c, actor, m); }   // coop_enemy_act.c
            { void en_fx_precalc(Cpu *c, uint64_t actor, EnCell *m); en_fx_precalc(c, actor, m); }   // coop_enemy_fx.c
            { void en_arm_precalc(Cpu *c, uint64_t actor, EnCell *m); if (!m->act_run) en_arm_precalc(c, actor, m); }   // coop_enemy_arm.c
            { void en_shs_precalc(Cpu *c, uint64_t actor, EnCell *m); en_shs_precalc(c, actor, m); }   // coop_enemy_shot.c
            if (m->show_lk[3]) {   // its owner's model looks, each written with its dirty bit as the game's setters do (B.33)
                uint64_t g = coop_rd_ptr(en_comps(actor) + 0x280);
                static const struct { uint16_t off; uint32_t bit; } F[4] = {{0x4c, 0x1000}, {0x54, 0x10000000}, {0x58, 0x100}, {0x5c, 0x20000}};
                static const struct { uint16_t off; uint32_t bit; } Bt[3] = {{0x9b, 1}, {0x9c, 0x100000}, {0x9e, 0x10000}};
                uint32_t dirty = 0;
                for (int k = 0; g && k < 4; k++) {
                    float v = 0;
                    if (coop_rd(g + F[k].off, &v, 4) && memcmp(&v, &m->show_look[k], 4)) memcpy((void *)(uintptr_t)(g + F[k].off), &m->show_look[k], 4), dirty |= F[k].bit;
                }
                for (int k = 0; g && k < 3; k++) {
                    uint8_t v = 0;
                    if (!coop_rd(g + Bt[k].off, &v, 1) || v == m->show_lk[k]) continue;
                    // (the hidden mask's dirty bit is set only when the mask goes between none and some, as main+0xbb0dd0 does)
                    if (k || !v != !m->show_lk[k]) dirty |= Bt[k].bit;
                    memcpy((void *)(uintptr_t)(g + Bt[k].off), &m->show_lk[k], 1);
                }
                if (dirty) {
                    __atomic_fetch_or((uint32_t *)(uintptr_t)(g + 0xac), dirty, __ATOMIC_SEQ_CST);
                    static _Atomic int said_l;
                    if (atomic_fetch_add(&said_l, 1) < 60) rt_log("coop: en: look: the copy %llx takes its owner's model looks (dirty %08x: ice %.2f, fade %.2f, hidden mask %02x, enabled %u)", (unsigned long long)actor, dirty, m->show_look[0], m->show_look[3], m->show_lk[0], m->show_lk[1]);
                }
            }
            uint8_t want = m->show_hide;
            uint64_t who[1 + EN_EQ_N * 2], eu = coop_rd_ptr(en_comps(actor) + 0x230);
            int nw = 0;
            who[nw++] = actor;
            for (int k = 0; eu && k < EN_EQ_N; k++) {
                uint64_t ref = coop_rd_ptr(eu + 0x20 + (uint64_t)k * 0x18 + 8), w = ref ? coop_rd_ptr(ref + 0x40) : 0;
                if (!w) continue;
                who[nw++] = w;
                uint64_t wc = coop_rd_ptr(en_comps(w) + 0x208), aref = wc ? coop_rd_ptr(wc + 0xb0 + 8) : 0, att = aref ? coop_rd_ptr(aref + 0x40) : 0;
                if (att) who[nw++] = att;
            }
            for (int k = 0; k < nw; k++) {
                uint64_t gmc = coop_rd_ptr(en_comps(who[k]) + 0x280);
                uint8_t hm = 0;
                if (!gmc || !coop_rd(gmc + 0x9d, &hm, 1) || hm == want) continue;
                memcpy((void *)(uintptr_t)(gmc + 0x9d), &want, 1);
                __atomic_fetch_or((uint32_t *)(uintptr_t)(gmc + 0xac), 0x200u, __ATOMIC_SEQ_CST);
            }
        }
    } else {   // held: it stands where it was when the hold began
        if (!m->pinned) en_pose_of(actor, m->pin), m->pinned = 1;
        put(c, actor, m->pin, m->pin + 3);
    }
    memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
}

// Test (TOTK_COOP_EN_TEST_GMC=<name prefix>): the model controller of an enemy of that name, 0x90 bytes from +0x40, at
// each change of its first slot's command and 40 frames after it; in both games, to find what differs when one game
// shows the enemy and the other does not.
static void gmc_log(uint64_t actor, const char *role) {
    static const char *want;
    static int read;
    static struct { uint64_t a; uint32_t cmd, at; } t[8];
    if (!read) want = cfg_get("TOTK_COOP_EN_TEST_GMC"), read = 1;
    char nm[48] = "", cmd[40] = "";
    if (!want || strncmp(coop_actor_name(actor, nm, sizeof nm), want, strlen(want))) return;
    anim_cmd(anim_slot(anim_g(actor), 0), cmd, sizeof cmd);
    uint32_t h = fnv_str(cmd), frame = coop_sess_frame_no();
    int k = -1;
    for (int i = 0; i < 8; i++) if (t[i].a == actor) k = i;
    if (k < 0) { for (int i = 0; i < 8 && k < 0; i++) if (!t[i].a) k = i; if (k < 0) return; t[k].a = actor, t[k].cmd = 0; }
    int now = t[k].cmd != h, later = t[k].at && frame - t[k].at == 40;
    if (now) t[k].cmd = h, t[k].at = frame;
    if (!now && !later) return;
    uint64_t gmc = coop_rd_ptr(en_comps(actor) + 0x280);
    uint8_t b[0x90] = {0};
    char hex[0x90 * 2 + 0x90 / 4 + 4];
    int n = 0;
    if (gmc) coop_rd(gmc + 0x40, b, sizeof b);
    for (int i = 0; i < 0x90; i++) n += snprintf(hex + n, sizeof hex - (size_t)n, "%02x%s", b[i], (i & 3) == 3 ? " " : "");
    rt_log("coop: en: gmc: %s (%s) '%s' %s: %s", nm, role, cmd, now ? "starts" : "+40 frames", hex);
}

// ---- STATE --------------------------------------------------------------------------------------------------------------
typedef struct { uint64_t key; Enid id; uint32_t epoch; uint8_t life, pad[3]; EnState st; } MState;
_Static_assert(sizeof(MState) <= 1100, "a state fits one transport payload");

uint64_t en_state_key(Enid id);
static uint64_t key_of(Enid id) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)&id;
    for (size_t i = 0; i < sizeof id; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h ? h : 1;
}

static void quat_slerp(const float a[4], const float b[4], float t, float out[4]) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1.f : 1.f, n = 0;
    for (int i = 0; i < 4; i++) out[i] = a[i] * (1 - t) + s * b[i] * t, n += out[i] * out[i];
    n = n > 1e-12f ? 1.f / sqrtf(n) : 0;   // (nlerp: the states are a frame or two apart)
    if (n == 0) { memcpy(out, a, 16); return; }
    for (int i = 0; i < 4; i++) out[i] *= n;
}

void en_copy_phase0(Cpu *c) {
    coop_mount_phase0(c);
    uint64_t me = coop_ctr_me(), now = en_now(), stamp = coop_sess_frame_ns();
    uint32_t frame = coop_sess_frame_no();
    if (!(frame % 15)) held_rebuild();
    static uint64_t rec_ns;
    int rec = now - rec_ns >= SEC;
    if (rec) rec_ns = now;
    float links[COOP_MAX_PLAYERS + 1][3];
    int nl = coop_player_positions(links, COOP_MAX_PLAYERS);
    uint64_t my = coop_player_actor();
    if (my && coop_rd(my + COOP_ACTOR_POS, links[nl], 12)) nl++;
    // The newest stamp taken from each peer this frame is a sample of its clock; T per peer is fixed for the frame.
    int64_t newest[COOP_MAX_PLAYERS + 1] = {0}, T[COOP_MAX_PLAYERS + 1] = {0}, delay[COOP_MAX_PLAYERS + 1] = {0};
    uint32_t tele[COOP_MAX_PLAYERS + 1] = {0};
    static uint32_t tele_was[COOP_MAX_PLAYERS + 1];
    int have_t[COOP_MAX_PLAYERS + 1] = {0};
    int n_copy = 0, n_held = 0, n_owned = 0, n_think = 0;
    uint32_t turned = 0, leaks = 0, newest_run = 0;
    static uint32_t passed_sum, st_bytes, st_n;

    // Pass 1: this game's enemies are read and stated; the others' states are taken.
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->life == EN_UNSYNCED || e->cell < 0) continue;
        EnCell *m = &en_map[e->cell];
        if (e->owner == me) {
            EnState *s = &e->cur;
            float was[3];
            int64_t was_ns = s->s.ns;
            memcpy(was, s->s.pos, 12);
            float pose[7];
            en_pose_of(e->actor, pose);
            memcpy(s->s.pos, pose, 12), memcpy(s->s.quat, pose + 3, 16);
            s->s.ns = (int64_t)stamp, s->hp = en_hp_of(e->actor);
            double dt = was_ns ? (double)((int64_t)stamp - was_ns) / 1e9 : 0;
            float d2 = 0;
            for (int k = 0; k < 3; k++) {
                float d = s->s.pos[k] - was[k];
                d2 += d * d;
                s->s.vel[k] = dt > 0.001 && dt < 0.2 ? (float)(d / dt) : 0;
            }
            if (was_ns && d2 > 1.5f * 1.5f) e->teleports++, memset(s->s.vel, 0, 12);   // (a jump, 1.5 m in a frame: not a path to follow; a Yiga footsoldier's blinks of 3 m were shown as a slide)
            s->s.teleports = e->teleports;
            uint32_t cmds[EN_SLOT_N];
            for (int k = 0; k < EN_SLOT_N; k++) cmds[k] = s->sl[k].cmd;
            slots_read(e->actor, s, 1);
            for (int k = 0; k < s->nslot; k++) if (s->sl[k].cmd != cmds[k]) e->send_burst = 3;
            { uint32_t eq[EN_EQ_N]; equip_read(e->actor, eq, 1); if (memcmp(eq, s->eq, sizeof eq)) e->send_burst = 3; memcpy(s->eq, eq, sizeof eq); }
            { uint32_t att[EN_EQ_N]; uint8_t eqs[EN_EQ_N]; equip_read_more(e->actor, att, eqs, 1); if (memcmp(att, s->att, sizeof att) || memcmp(eqs, s->eqs, sizeof eqs)) e->send_burst = 3; memcpy(s->att, att, sizeof att), memcpy(s->eqs, eqs, sizeof eqs); }
            { uint64_t rg = coop_rd_ptr(en_comps(e->actor) + 0x3e8); int32_t rs = 0; if (rg) coop_rd(rg + 0x20, &rs, 4); if ((uint8_t)rs != s->rag) { e->send_burst = 3; rt_log("coop: en: rag: %s %016llx ragdoll state %d -> %d", e->name, (unsigned long long)e->id.a, s->rag, (int)rs); } s->rag = (uint8_t)rs; }
            {   // the model's hidden mask and the actor's fallback bit (test: logged when they change)
                uint64_t gmc = coop_rd_ptr(en_comps(e->actor) + 0x280);
                uint8_t hm = 0;
                uint32_t fl = 0;
                if (gmc) coop_rd(gmc + 0x9d, &hm, 1);
                coop_rd(e->actor + 0x444, &fl, 4);
                uint8_t vb = (uint8_t)(fl >> 23 & 1);
                if (hm != s->hide || vb != s->vis) { e->send_burst = 3; rt_log("coop: en: hide: %s %016llx (run here) mask %02x -> %02x, bit %u -> %u, slot 0 plays %08x", e->name, (unsigned long long)e->id.a, s->hide, hm, s->vis, vb, s->nslot ? s->sl[0].cmd : 0); }
                s->hide = hm, s->vis = vb;
                // The model controller's other looks (B.33; research R7b, R19): each a field with its own dirty bit in
                // the word at +0xac, set by nodes of the AI graph that a copy does not run: the ice look's amount
                // (+0x4c, node OneShotGameModelControllerSetIceEffect), +0x54, the death effect's amount (+0x58), the
                // fade (+0x5c, the fade-in of an enemy that appears), the hidden mask (+0x9b, SetVisible), the model
                // enabled (+0x9c, SetOnlyModelVisible), and +0x9e.
                float lf[4] = {0};
                uint8_t lb[4] = {0};
                if (gmc && coop_rd(gmc + 0x4c, &lf[0], 4) && coop_rd(gmc + 0x54, &lf[1], 12) && coop_rd(gmc + 0x9b, &lb[0], 2) && coop_rd(gmc + 0x9e, &lb[2], 1)) lb[3] = 1;
                if (memcmp(lf, s->look, sizeof lf) || memcmp(lb, s->lk, sizeof lb)) {
                    e->send_burst = 3;
                    static _Atomic int said_l;
                    if (atomic_fetch_add(&said_l, 1) < 60) rt_log("coop: en: look: %s %016llx (run here) ice %.2f, +0x54 %.2f, death %.2f, fade %.2f, hidden mask %02x, enabled %u, +0x9e %u", e->name, (unsigned long long)e->id.a, lf[0], lf[1], lf[2], lf[3], lb[0], lb[1], lb[2]);
                }
                memcpy(s->look, lf, sizeof lf), memcpy(s->lk, lb, sizeof lb);
            }
            brain_read(e->actor, &s->verb, &s->tgt);
            {   // One target at a time, no flicker (design 9.4; B.22). The game's own rule can swap between two players at
                // about the same distance from one frame to the next (research G2 section 7). A change from one player to
                // another less than three seconds after the last change is put back: the enemy keeps the player it had
                // just turned to. A change to no one, or to something that is no player, is the game's own business.
                uint8_t t = s->tgt;
                int player = t && t != 0xff, was_player = e->tgt_cur && e->tgt_cur != 0xff;
                if (t != e->tgt_cur) {
                    if (player && was_player && now - e->tgt_since_ns < 3 * SEC && !en_test_skip('t')) {
                        atomic_store(&m->tgt_back, e->tgt_cur);
                        static _Atomic int said;
                        if (atomic_fetch_add(&said, 1) < 40) rt_log("coop: en: target: %s %016llx turns from player %u to player %u %.1f s after its last change: put back", e->name, (unsigned long long)e->id.a, e->tgt_cur - 1, t - 1, (double)(now - e->tgt_since_ns) / 1e9);
                        s->tgt = e->tgt_cur;
                    } else e->tgt_cur = t, e->tgt_since_ns = now;
                }
            }
            {   // the battle-info component's words ([comps+0x140]: the flags at +0x38, the word at +0x40 in the low half and the
                // int at +0x90 in the high; research R15: the life gauge, the marks and the battle music hang on it)
                uint64_t bi = coop_rd_ptr(en_comps(e->actor) + 0x140);
                uint32_t f = 0, n = 0;
                uint16_t w = 0;
                if (bi) coop_rd(bi + 0x38, &f, 4), coop_rd(bi + 0x40, &w, 2), coop_rd(bi + 0x90, &n, 4);
                uint32_t b2 = (uint32_t)w | n << 16;
                s->blink = bi && (coop_rd_ptr(bi + 0x58) || coop_rd_ptr(bi + 0x60));   // (in the battle manager's list)
                if (f != s->binfo || b2 != s->binfo2) {
                    static _Atomic int said;
                    if (atomic_fetch_add(&said, 1) < 60) rt_log("coop: en: battle info: %s %016llx (run here) flags %08x -> %08x, word %04x, count %d, on the list %u; brain verb %u", e->name, (unsigned long long)e->id.a, s->binfo, f, w, (int)n, s->blink, s->verb);
                    e->send_burst = 3;
                }
                s->binfo = f, s->binfo2 = b2;
            }
            {   // Its attack is a swift one (B.31; research R4 section 5): bit 7 of its game-balance client's word +0x34
                // ([comps+0x268]), which the node OneShotGameBalanceSetSwiftAttack of its attack's graph sets and
                // clears. A copy's graph does not run, so the bit is stated.
                uint64_t cl = coop_rd_ptr(en_comps(e->actor) + 0x268);
                uint32_t w34 = 0;
                uint8_t gb = cl && coop_rd(cl + 0x34, &w34, 4) ? (uint8_t)(0x80 | (w34 >> 7 & 1)) : 0;
                if (gb != s->gb) e->send_burst = 3;
                s->gb = gb;
                {   // its character controller's move state and velocity, for a copy that runs its action (B.38)
                    int en_act_cct_read(uint64_t actor, uint8_t *idx, float vel[3]);   // coop_enemy_act.c
                    uint8_t ci = 0;
                    float cv[3] = {0};
                    if (!en_act_cct_read(e->actor, &ci, cv)) ci = 0;
                    if (ci != s->cct) e->send_burst = 3;
                    s->cct = ci, memcpy(s->cvel, cv, 12);
                }
                uint8_t ar[2];
                void en_arm_read(uint64_t actor, uint8_t out[2]);   // coop_enemy_arm.c
                en_arm_read(e->actor, ar);
                if (memcmp(ar, s->arm, 2)) e->send_burst = 3;
                memcpy(s->arm, ar, 2);
            }
            { uint8_t wx = frame - m->wx_frame < 10 ? m->wx_type : 0; if (wx != s->wx) e->send_burst = 3; s->wx = wx; }   // (the magic weather its graph asks for: coop_enemy_fx.c)
            if (!((frame + (uint32_t)i) & 7)) attn_owner(e);
            {   // its AI memory (B.2)
                uint8_t mm = mem_read(e->actor);
                if (mm != s->mem) { e->send_burst = 3; if (s->mem) rt_log("coop: en: memory: %s %016llx (run here) %02x -> %02x (bit 0 IsMagicWeather, 1 IsShootMagicObj)", e->name, (unsigned long long)e->id.a, s->mem, mm); }
                s->mem = mm;
            }
            { uint32_t dw = 0; uint64_t dc = coop_rd_ptr(en_comps(e->actor) + 0x1c8); if (dc) coop_rd(dc + 0x20, &dw, 4); if (dw != s->dstate) e->send_burst = 3; s->dstate = dw; }
            float near2 = 1e30f;
            for (int k = 0; k < nl; k++) {
                float dx = links[k][0] - s->s.pos[0], dy = links[k][1] - s->s.pos[1], dz = links[k][2] - s->s.pos[2], q = dx * dx + dy * dy + dz * dz;
                if (q < near2) near2 = q;
            }
            // every frame while a player is within 40 m, every second frame while it moved, else once a second (S 6.1)
            int due = near2 < 40.f * 40.f || d2 > 1e-6f || e->send_burst || now - e->sent_ns >= SEC;
            if (e->send_burst) e->send_burst--;
            if (due) {
                MState ms;
                memset(&ms, 0, offsetof(MState, st));
                ms.key = key_of(e->id), ms.id = e->id, ms.epoch = e->epoch, ms.life = e->life, ms.st = *s;
                int len = (int)(offsetof(MState, st) + offsetof(EnState, sl) + s->nslot * sizeof s->sl[0]);
                coop_ctr_send(0, ENM_STATE, 0, &ms, len);
                st_bytes += (uint32_t)len, st_n++;
                e->sent_ns = now;
            }
            coop_mount_owner(c, e, due);   // a horse's or an animal's gait, tack and rider, an enemy's horse (coop_mount.c)
            uint32_t let = atomic_exchange(&m->passed, 0);
            n_owned++, passed_sum += let;
            gmc_log(e->actor, "run here");
            if (e->life == EN_LIVE) { uint32_t rf = atomic_load(&m->run_frame); n_think++; if (rf && (!newest_run || (int32_t)(rf - newest_run) > 0)) newest_run = rf; }
            // (ten samples a second; every frame while it moves faster than 4 m/s, so the report can place a fast enemy)
            if (coop_debug() && (now - e->log_ns >= SEC / 10 || s->s.vel[0] * s->s.vel[0] + s->s.vel[1] * s->s.vel[1] + s->s.vel[2] * s->s.vel[2] > 16.f)) {
                char cmd[40] = "";
                anim_cmd(anim_slot(anim_g(e->actor), 0), cmd, sizeof cmd);
                e->log_ns = now;
                rt_log("coop: en: pos: %lld %016llx o %.3f %.3f %.3f %.4f %.4f %.4f %.4f %s %.2f hp %d", (long long)stamp, (unsigned long long)e->id.a, s->s.pos[0], s->s.pos[1], s->s.pos[2],
                       s->s.quat[0], s->s.quat[1], s->s.quat[2], s->s.quat[3], cmd[0] ? cmd : "-", s->nslot ? s->sl[0].c[s->sl[0].cur].clock : 0.f, s->hp);
            }
            continue;
        }
        int id = en_peer_id(e->owner);
        if (!id) continue;
        coop_mount_take(e, id);
        MState ms;
        uint64_t age = 0;
        int len = coop_ctr_latest(id, ENM_STATE, key_of(e->id), &ms, sizeof ms, &age);
        if (len >= (int)(offsetof(MState, st) + offsetof(EnState, sl)) && enid_eq(ms.id, e->id) && ms.epoch == e->epoch && age < 2 * SEC &&
            ms.st.nslot <= EN_SLOT_N && len == (int)(offsetof(MState, st) + offsetof(EnState, sl) + ms.st.nslot * sizeof ms.st.sl[0])) {
            int64_t top = e->snap_w ? e->st[(e->snap_w - 1) % EN_SNAP_N].s.ns : 0;
            if (ms.st.s.ns > top) {
                EnState *d = &e->st[e->snap_w++ % EN_SNAP_N];
                memset(d, 0, sizeof *d);
                memcpy(d, &ms.st, offsetof(EnState, sl) + ms.st.nslot * sizeof ms.st.sl[0]);
                e->st_used++;
                if (ms.st.s.ns > newest[id]) newest[id] = ms.st.s.ns;
            }
        }
    }
    // This game's world stands (a menu): the calcs go on, but no graph runner is entered any more for any enemy this
    // game runs. A far enemy's runners come a few times a second, so a second without one for all of them is the
    // sign. The heartbeat then says PAUSED (coop_sess.c), which is what lets another game take a fight over.
    atomic_store(&en_world_stands, n_think && newest_run && frame - newest_run > 60);
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) {
        if (newest[id]) coop_sess_clock_sample(id, newest[id]);
        have_t[id] = coop_sess_clock(id, 16666667ull, &T[id], &delay[id], &tele[id]);
        en_shot_T(id, have_t[id] ? T[id] : 0);
    }

    // Pass 2: what each copy shows at T of its owner (S 6.3).
    for (int i = 0; i < EN_N; i++) {
        En *e = &en_tab[i];
        if (!e->used || e->life == EN_UNSYNCED || e->cell < 0 || e->owner == me) continue;
        EnCell *m = &en_map[e->cell];
        int st = atomic_load(&m->state), id = en_peer_id(e->owner);
        if (st == EM_COPY) n_copy++; else if (st == EM_HELD || st == EM_NEW) n_held++;
        turned += atomic_exchange(&m->turned, 0), leaks += atomic_load(&m->leaks);
        if (st != EM_COPY || !id || !have_t[id] || !e->snap_w) continue;
        uint32_t cnt = e->snap_w < EN_SNAP_N ? e->snap_w : EN_SNAP_N;
        const EnState *a = NULL, *b = NULL;
        for (uint32_t k = 0; k < cnt; k++) {   // newest first
            const EnState *s = &e->st[(e->snap_w - 1 - k) % EN_SNAP_N];
            if (s->s.ns > T[id]) b = s; else { a = s; break; }
        }
        int hold = 0, jump = tele[id] != tele_was[id];
        float pos[3], quat[4];
        const EnState *sl = a ? a : b;
        double ahead = 0;
        if (a && b && b->s.teleports == a->s.teleports && b->s.ns - a->s.ns < (int64_t)(SEC / 2)) {
            double h = (double)(b->s.ns - a->s.ns) / 1e9, t = (double)(T[id] - a->s.ns) / (double)(b->s.ns - a->s.ns);
            double t2 = t * t, t3 = t2 * t, h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
            for (int k = 0; k < 3; k++) pos[k] = (float)(h00 * a->s.pos[k] + h10 * h * a->s.vel[k] + h01 * b->s.pos[k] + h11 * h * b->s.vel[k]);
            quat_slerp(a->s.quat, b->s.quat, (float)t, quat);
            ahead = (double)(T[id] - a->s.ns) / 1e9;
        } else if (a) {   // no state after T: the newest is held, and the clips with it
            memcpy(pos, a->s.pos, 12), memcpy(quat, a->s.quat, 16);
            double gap = (double)(T[id] - a->s.ns) / 1e9;
            float sp2 = a->s.vel[0] * a->s.vel[0] + a->s.vel[1] * a->s.vel[1] + a->s.vel[2] * a->s.vel[2];
            ahead = gap < 0 ? 0 : gap < 2.5 ? gap : 2.5;   // (a standing enemy is stated once a second: its clips run on between)
            hold = !b && gap > 0.25 && sp2 > 0.01f, jump |= b != NULL;
            if (hold) e->st_held++, ahead = 0;
        } else {          // T is before every state kept: the oldest, as it is
            memcpy(pos, b->s.pos, 12), memcpy(quat, b->s.quat, 16);
        }
        coop_check_nudge(e->actor, e->id.a, pos);   // (a test of the mismatch checker, TOTK_COOP_TEST_NUDGE: coop_check.c)
        uint32_t s0 = atomic_load(&m->seq);
        atomic_store_explicit(&m->seq, s0 + 1, memory_order_release);
        memcpy(m->show_pos, pos, 12), memcpy(m->show_quat, quat, 16);
        // The owner's enemy began a flinch (a partial ragdoll, by its own player's blow or anything else): the copy's is
        // asked of its own ragdoll component, unless this game's player just struck it and it flinched here already.
        // (no flinch is asked of the copy's own ragdoll component any more: see en_copy_hook, the ragdoll start)
        m->show_rag = sl->rag, m->show_hide = sl->hide, m->show_mem = sl->mem, m->show_verb = sl->verb, m->show_tgt = sl->tgt, m->show_wx = sl->wx, m->show_binfo = sl->binfo, m->show_blink = sl->blink, m->show_bhave = 1, m->show_gb = sl->gb, m->show_cct = sl->cct, memcpy(m->show_cvel, sl->cvel, 12), memcpy(m->show_arm, sl->arm, 2), memcpy(m->show_look, sl->look, sizeof m->show_look), memcpy(m->show_lk, sl->lk, sizeof m->show_lk);
        m->show_nslot = sl->nslot, m->show_hold = (uint8_t)hold, m->show_teleport = (uint8_t)jump, m->show_hp = sl->hp, m->show_dstate = sl->dstate;
        for (int k = 0; k < sl->nslot; k++) {
            m->show_slot[k] = sl->sl[k];
            // each controller's clock at T: the stated one plus the rate times the time since (30ths of a second)
            for (int j = 0; j < EN_CTRL_N; j++) m->show_slot[k].c[j].clock += (float)(sl->sl[k].rate * ahead * 30.0);
        }
        m->show_on = 1;
        atomic_store_explicit(&m->seq, s0 + 2, memory_order_release);
        e->hp = sl->hp;
        gmc_log(e->actor, "copy");
        if (coop_debug()) {   // (record) the copy's own hidden mask and bit against its owner's
            uint64_t gmc = coop_rd_ptr(en_comps(e->actor) + 0x280);
            uint8_t hm = 0;
            uint32_t fl = 0;
            static uint8_t was[EN_N][4];
            if (gmc) coop_rd(gmc + 0x9d, &hm, 1);
            coop_rd(e->actor + 0x444, &fl, 4);
            uint8_t vb = (uint8_t)(fl >> 23 & 1);
            if (was[i][0] != hm || was[i][1] != vb || was[i][2] != sl->hide || was[i][3] != sl->vis)
                rt_log("coop: en: hide: %s %016llx (copy) own mask %02x bit %u; its owner's mask %02x bit %u", e->name, (unsigned long long)e->id.a, hm, vb, sl->hide, sl->vis);
            was[i][0] = hm, was[i][1] = vb, was[i][2] = sl->hide, was[i][3] = sl->vis;
        }
        if (coop_debug() && (frame & 15) == (uint32_t)((i + 5) & 15)) {   // (the record: the copy's battle-info words against its owner's, at each change)
            uint64_t bi = coop_rd_ptr(en_comps(e->actor) + 0x140);
            uint32_t f = 0, n = 0;
            uint16_t w = 0;
            static uint32_t was[EN_N][2];
            static int said;
            if (bi) coop_rd(bi + 0x38, &f, 4), coop_rd(bi + 0x40, &w, 2), coop_rd(bi + 0x90, &n, 4);
            if ((was[i][0] != f || was[i][1] != sl->binfo) && said++ < 80)
                rt_log("coop: en: battle info: %s %016llx (copy) own flags %08x word %04x count %d; its owner's flags %08x word %04x count %d", e->name, (unsigned long long)e->id.a, f, w, (int)n, sl->binfo, sl->binfo2 & 0xffff, (int)(sl->binfo2 >> 16));
            was[i][0] = f, was[i][1] = sl->binfo;
        }
        if ((frame & 15) == (uint32_t)(i & 15)) {   // what the copy holds against what its owner's enemy holds
            uint32_t eq[EN_EQ_N];
            equip_read(e->actor, eq, 0);
            if (memcmp(eq, sl->eq, sizeof eq)) {
                if (!e->eq_off++ || rec)
                    for (int k = 0; k < EN_EQ_N; k++)
                        if (eq[k] != sl->eq[k]) {
                            const char *a0 = name_of(sl->eq[k]), *b0 = name_of(eq[k]);
                            rt_log("coop: en: equip: %s %016llx slot %d: its owner's holds %s, this game's copy %s", e->name, (unsigned long long)e->id.a, k, sl->eq[k] ? a0 ? a0 : "?" : "nothing", eq[k] ? b0 ? b0 : "(a name not stated)" : "nothing");
                        }
                if (e->eq_off >= 3) {   // (for three looks, 0.8 s: not a weapon on its way between two states)
                    uint8_t drop = 0;
                    // (another thing than its owner's in the slot is let go of as well: a swap was never made good, since
                    // neither "holds and the owner does not" nor "holds nothing" fitted)
                    for (int k = 0; k < EN_EQ_N; k++) if (eq[k] && eq[k] != sl->eq[k]) drop |= (uint8_t)(1u << k);
                    if (drop) atomic_store(&m->eq_drop, drop);
                    for (int k = 0; k < EN_EQ_N; k++) atomic_store(&m->eq_make[k], !eq[k] && sl->eq[k] ? sl->eq[k] : 0);
                }
            } else {
                e->eq_off = 0;
                for (int k = 0; k < EN_EQ_N; k++) if (eq[k]) m->eq_tries[k] = 0;   // (it holds it: a later loss may be made good again)
                // The same things held: what is fused to each must be the same too. A copy's held thing with another
                // attachment than its owner's (none where there is one, as every thing the copy was given before the
                // attachment was stated) is let go of, and made again with it, twice at most.
                uint32_t att[EN_EQ_N];
                uint8_t eqs[EN_EQ_N], drop = 0;
                equip_read_more(e->actor, att, eqs, 0);
                for (int k = 0; k < EN_EQ_N; k++) if (eq[k] && sl->att[k] && !att[k] && m->eq_att_tries[k] < 2) drop |= (uint8_t)(1u << k), m->eq_att_tries[k]++;
                if (drop && ++e->eq_att_off >= 3) atomic_store(&m->eq_drop, drop), e->eq_att_off = 0;
                else if (!drop) e->eq_att_off = 0;
                else for (int k = 0; k < EN_EQ_N; k++) if (drop >> k & 1) m->eq_att_tries[k]--;
            }
            memcpy(m->eq_att, sl->att, sizeof m->eq_att), memcpy(m->eq_state, sl->eqs, sizeof m->eq_state);
        }
        if (coop_debug() && (now - e->log_ns >= SEC / 10 || sl->s.vel[0] * sl->s.vel[0] + sl->s.vel[1] * sl->s.vel[1] + sl->s.vel[2] * sl->s.vel[2] > 16.f)) {
            char cmd[40] = "";
            anim_cmd(anim_slot(anim_g(e->actor), 0), cmd, sizeof cmd);
            int cur = 0;
            uint64_t ctx = anim_ctx(anim_slot(anim_g(e->actor), 0), &cur);
            float clock = 0, p[3] = {0};
            if (ctx) coop_rd(ctx + 0x68, &clock, 4);
            coop_rd(e->actor + COOP_ACTOR_POS, p, 12);
            e->log_ns = now;
            // (the time is T on the owner's clock: what the shown pose is the owner's pose of; the place is the actor's own, a frame old)
            rt_log("coop: en: pos: %lld %016llx c %.3f %.3f %.3f %.4f %.4f %.4f %.4f %s %.2f hp %d asked %.3f %.3f %.3f", (long long)T[id], (unsigned long long)e->id.a, p[0], p[1], p[2],
                   quat[0], quat[1], quat[2], quat[3], cmd[0] ? cmd : "-", clock, en_hp_of(e->actor), pos[0], pos[1], pos[2]);
        }
        if (rec) {
            rt_log("coop: en: motion: %016llx %s: delay %.0f ms, %u states taken, %u frames held; %u plays, %u clocks written, %u rates bent, %u frames not at the pose asked",
                   (unsigned long long)e->id.a, e->name, (double)delay[id] / 1e6, e->st_used, e->st_held, atomic_exchange(&m->n_play, 0), atomic_exchange(&m->n_hard, 0),
                   atomic_exchange(&m->n_soft, 0), atomic_exchange(&m->n_off, 0));
            e->st_used = e->st_held = 0;
        }
    }
    for (int id = 1; id <= COOP_MAX_PLAYERS; id++) tele_was[id] = tele[id];
    if (rec && (n_copy || n_held || n_owned))
        rt_log("coop: en: switch: %d copies and %d held: %u runner entries turned away, %u leaks; %d run here: %u runner entries let through",
               n_copy, n_held, turned, leaks, n_owned, passed_sum), passed_sum = 0;
    if (rec && st_n) rt_log("coop: en: sent: %u states, %u bytes in the last second (to each peer)", st_n, st_bytes), st_n = st_bytes = 0;
}

uint64_t en_state_key(Enid id) { return key_of(id); }
