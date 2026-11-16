// Co-op: the enemy sync's copies (docs/coop/enemy-sync-spec.md sections 5 to 7).
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
int en_copy_msg(const CoopCtrMsg *m, int from) {
    (void)from;
    if (m->type != ENM_PLAY) return 0;
    const MName *n = (const MName *)m->d;
    if (m->len == (int)sizeof *n && n->hash && memchr(n->name, 0, sizeof n->name)) name_add(n->hash, n->name);
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
static void equip_read(uint64_t actor, uint32_t eq[EN_EQ_N], int learn) {
    uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
    for (int s = 0; s < EN_EQ_N; s++) {
        eq[s] = 0;
        uint64_t ref = eu ? coop_rd_ptr(eu + 0x20 + (uint64_t)s * 0x18 + 8) : 0, held = ref ? coop_rd_ptr(ref + 0x40) : 0;
        uint32_t id = 0, gen = 0;
        if (!held || !coop_rd(eu + 0x20 + (uint64_t)s * 0x18 + 0x10, &id, 4) || !coop_rd(held + 0x10, &gen, 4) || id != gen) continue;
        char nm[40] = "";
        coop_actor_name(held, nm, sizeof nm);
        if (!nm[0]) continue;
        eq[s] = fnv_str(nm);
        if (learn && name_add(eq[s], nm)) { MName m; memset(&m, 0, sizeof m); m.hash = eq[s], snprintf(m.name, sizeof m.name, "%s", nm); en_send_all(ENM_PLAY, &m, sizeof m); }
    }
}

// ---- What enemies hold ---------------------------------------------------------------------------------------------------
// Every actor an enemy of a listed kind holds here, and what is fused to it (a held weapon's attachment: the link at
// +0xb0 of the weapon's component [[w+0x228]+0x208]), as a set the object mirror asks (coop_players.c prop_fn): such an
// actor belongs to its enemy and is no loose object. A Fire Wizzrobe's rod carries a ruby (Item_Ore_B) fused to it; the
// mirror sent the ruby as an object of its own, and the other game showed a second gem floating beside the copy's rod
// that did not vanish when the Wizzrobe did (a play session, 2026-11-16). Rebuilt by phase 0 four times a second.
#define HELD_N 1024
static _Atomic uint64_t held_set[2][HELD_N];
static _Atomic int held_cur;
static void held_put(_Atomic uint64_t *t, uint64_t a) {
    for (uint32_t h = (uint32_t)((a >> 4) * 2654435761u) % HELD_N, k = 0; k < 32; k++, h = (h + 1) % HELD_N) {
        uint64_t v = atomic_load_explicit(&t[h], memory_order_relaxed);
        if (v == a) return;
        if (!v) { atomic_store_explicit(&t[h], a, memory_order_relaxed); return; }
    }
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
static void held_rebuild(void) {
    int to = !atomic_load(&held_cur);
    _Atomic uint64_t *t = held_set[to];
    static int said;
    for (int i = 0; i < HELD_N; i++) atomic_store_explicit(&t[i], 0, memory_order_relaxed);
    for (int i = 0; i < EN_MAP_N; i++) {
        uint64_t a = atomic_load(&en_map[i].actor), eu;
        if (a <= 1 || !(eu = coop_rd_ptr(en_comps(a) + 0x230))) continue;
        for (int s = 0; s < EN_EQ_N; s++) {
            uint64_t ref = coop_rd_ptr(eu + 0x20 + (uint64_t)s * 0x18 + 8), w = ref ? coop_rd_ptr(ref + 0x40) : 0;
            uint32_t id = 0, gen = 0;
            if (!w || !coop_rd(eu + 0x20 + (uint64_t)s * 0x18 + 0x10, &id, 4) || !coop_rd(w + 0x10, &gen, 4) || id != gen) continue;
            held_put(t, w);
            uint64_t wc = coop_rd_ptr(en_comps(w) + 0x208), aref = wc ? coop_rd_ptr(wc + 0xb0 + 8) : 0, att = aref ? coop_rd_ptr(aref + 0x40) : 0;
            uint32_t aid = 0, agen = 0;
            if (!att || !coop_rd(wc + 0xb0 + 0x10, &aid, 4) || !coop_rd(att + 0x10, &agen, 4) || aid != agen) continue;
            held_put(t, att);
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

int en_copy_hook(Cpu *c, uint64_t off) {
    if (!atomic_load_explicit(&en_active, memory_order_relaxed)) return 0;
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
    case 0x1bddcb0: {   // a ragdoll start (x0 the component, x1 the request): logged for the enemies this sync knows
        uint64_t a = coop_rd_ptr(c->x[0] + 0x18);
        EnCell *m = a > 0x10000 ? en_cell_of(a) : NULL;
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
            const char *nm = name_of(o->cmd);
            if (!nm || frame - m->slot_try[i] < 12) continue;   // (a command this actor does not take is asked for 5 times a second)
            m->slot_try[i] = frame;
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
        if ((inst && (q || bits)) || coop_sess_frame_no() - m->born_frame >= 30) atomic_store(&m->warm, 1);
        else off = 0;
    }
    tl_off_actor = off ? actor : 0;
    if (!off && !m->sw_on) { en_hit_precalc(c, actor, m, 0); return; }
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
        m->sw_on = m->sw_saved = m->body_static = m->pinned = 0;
        memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
        return;
    }
    m->sw_on = 1;
    four_writes(actor, m);
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
        put(c, actor, pos, quat);
        memcpy(m->pin, pos, 12), memcpy(m->pin + 3, quat, 16), m->pinned = 2;
        slots_apply(c, actor, m, slot, nslot, hold, coop_sess_frame_no());
        for (int k = 0; k < EN_EQ_N; k++) {
            // What the owner's enemy holds and the copy does not (a weapon fetched again, one picked up) the copy is given by
            // the game's own equipment create, called as coop_players.c give_gear calls it for a stand-in: in the hand,
            // default life, no fused attachment. At most one create in two seconds.
            uint32_t want = atomic_exchange_explicit(&m->eq_make[k], 0, memory_order_relaxed);
            const char *wn = want ? name_of(want) : NULL;
            uint64_t eu = coop_rd_ptr(en_comps(actor) + 0x230);
            uint32_t fr = coop_sess_frame_no();
            if (!wn || !eu || fr - m->eq_made_frame < 120 || m->eq_tries[k] >= 2) continue;
            m->eq_made_frame = fr, m->eq_tries[k]++;
            for (int j = 0; j < 2; j++) {
                uint8_t *st = m->eq_str[j];
                memset(st, 0, 0x60);
                snprintf((char *)st + 0x10, 0x40, "%s", j ? "" : wn);
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
    } else {   // held: it stands where it was when the hold began
        if (!m->pinned) en_pose_of(actor, m->pin), m->pinned = 1;
        put(c, actor, m->pin, m->pin + 3);
    }
    memcpy(c->x, x, sizeof x), c->sp = sp, c->pc = pc;
}

// ---- STATE --------------------------------------------------------------------------------------------------------------
typedef struct { uint64_t key; Enid id; uint32_t epoch; uint8_t life, pad[3]; EnState st; } MState;
_Static_assert(sizeof(MState) <= 1100, "a state fits one transport payload");

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
    (void)c;
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
            { uint64_t rg = coop_rd_ptr(en_comps(e->actor) + 0x3e8); int32_t rs = 0; if (rg) coop_rd(rg + 0x20, &rs, 4); if ((uint8_t)rs != s->rag) { e->send_burst = 3; rt_log("coop: en: rag: %s %016llx ragdoll state %d -> %d", e->name, (unsigned long long)e->id.a, s->rag, (int)rs); } s->rag = (uint8_t)rs; }
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
            uint32_t let = atomic_exchange(&m->passed, 0);
            n_owned++, passed_sum += let;
            if (e->life == EN_LIVE) { uint32_t rf = atomic_load(&m->run_frame); n_think++; if (rf && (!newest_run || (int32_t)(rf - newest_run) > 0)) newest_run = rf; }
            if (now - e->log_ns >= SEC / 10) {
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
        uint32_t s0 = atomic_load(&m->seq);
        atomic_store_explicit(&m->seq, s0 + 1, memory_order_release);
        memcpy(m->show_pos, pos, 12), memcpy(m->show_quat, quat, 16);
        // The owner's enemy began a flinch (a partial ragdoll, by its own player's blow or anything else): the copy's is
        // asked of its own ragdoll component, unless this game's player just struck it and it flinched here already.
        if (sl->rag == 2 && m->show_rag != 2 && now - m->hit_ns > SEC) atomic_store(&m->flinch, 1);
        m->show_rag = sl->rag;
        m->show_nslot = sl->nslot, m->show_hold = (uint8_t)hold, m->show_teleport = (uint8_t)jump, m->show_hp = sl->hp, m->show_dstate = sl->dstate;
        for (int k = 0; k < sl->nslot; k++) {
            m->show_slot[k] = sl->sl[k];
            // each controller's clock at T: the stated one plus the rate times the time since (30ths of a second)
            for (int j = 0; j < EN_CTRL_N; j++) m->show_slot[k].c[j].clock += (float)(sl->sl[k].rate * ahead * 30.0);
        }
        m->show_on = 1;
        atomic_store_explicit(&m->seq, s0 + 2, memory_order_release);
        e->hp = sl->hp;
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
                    for (int k = 0; k < EN_EQ_N; k++) if (eq[k] && !sl->eq[k]) drop |= (uint8_t)(1u << k);
                    if (drop) atomic_store(&m->eq_drop, drop);
                    for (int k = 0; k < EN_EQ_N; k++) atomic_store(&m->eq_make[k], !eq[k] && sl->eq[k] ? sl->eq[k] : 0);
                }
            } else {
                e->eq_off = 0;
                for (int k = 0; k < EN_EQ_N; k++) if (eq[k]) m->eq_tries[k] = 0;   // (it holds it: a later loss may be made good again)
            }
        }
        if (now - e->log_ns >= SEC / 10) {
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
