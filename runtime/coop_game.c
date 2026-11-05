// Co-op: game structures and calls (coop_game.h). Found and tested with coop_spawn.c; docs/coop/re-notes.md.

#include <string.h>

#include "cfg.h"
#include "coop_game.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int coop_rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
uint64_t coop_rd_ptr(uint64_t a) {
    uint64_t p = 0;
    return coop_rd(a, &p, 8) ? p : 0;
}

const char *coop_actor_name(uint64_t actor, char *buf, size_t n) {
    uint64_t nm = actor ? coop_rd_ptr(actor + COOP_ACTOR_NAME) : 0;
    buf[0] = 0;
    if (nm) coop_rd(nm, buf, n - 1), buf[n - 1] = 0;
    return buf;
}

// The resident actor list: [main+0x3add238] +0x1e8 scene, its component 13 (+0x58 array), +0x20 count, +0x28 entries
// of 0x70 bytes; entry +8 the reference object, whose +0x40 is the actor (runtime/frame_rate.c player_pos).
static uint64_t resident_list(int32_t *count) {
    uint64_t module = coop_rd_ptr(MOD_MAIN_BASE + COOP_SCENE_SLOT), scene = module ? coop_rd_ptr(module + 0x1e8) : 0;
    int32_t n = 0;
    if (!scene || !coop_rd(scene + 0x50, &n, 4) || n < 14 || n > 256) return 0;
    uint64_t comps = coop_rd_ptr(scene + 0x58), resident = comps ? coop_rd_ptr(comps + 13 * 8) : 0;
    if (!resident || !coop_rd(resident + 0x20, count, 4) || *count < 0 || *count > 64) return 0;
    return coop_rd_ptr(resident + 0x28);
}

uint64_t coop_resident_named(const char *want, uint64_t *ref_out) {
    int32_t count = 0;
    uint64_t entries = resident_list(&count);
    for (int i = 0; entries && i < count; i++) {
        uint64_t ref = coop_rd_ptr(entries + (uint64_t)i * 0x70 + 8), actor = ref ? coop_rd_ptr(ref + 0x40) : 0;
        char nm[40];
        if (actor && !strcmp(coop_actor_name(actor, nm, sizeof nm), want)) {
            if (ref_out) *ref_out = ref;
            return actor;
        }
    }
    return 0;
}

int coop_is_resident(uint64_t actor) {
    int32_t count = 0;
    uint64_t entries = resident_list(&count);
    for (int i = 0; entries && i < count; i++) {
        uint64_t ref = coop_rd_ptr(entries + (uint64_t)i * 0x70 + 8);
        if (ref && coop_rd_ptr(ref + 0x40) == actor) return 1;
    }
    return 0;
}

uint64_t coop_player_actor(void) { return coop_resident_named("Player", NULL); }

uint64_t coop_guest_call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    uint64_t r = rt_call(c, fn, 4, a0, a1, a2, a3);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return r;
}

// The frame the AI node "ExecuteGenericRequestCreateActor" builds (main+0x231fa58): stack arguments at sp, the name
// pointer at sp+0x20, the create argument at sp+0x28 (+0 position, +0xc rotation, +0x30 scale, +0x40 a string the
// manager reads without a null check, +0x90 flags: bit 0 position, bit 1 rotation given).
int coop_request_create(Cpu *c, const char *name, const float pos[3], const float rot[9], uint8_t *handle) {
    uint64_t mgr = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_ACTOR_MGR_SLOT));
    if (!mgr) return 0;
    uint64_t saved_x[31], sp0 = c->sp, sp = (sp0 - 0x200) & ~0xfull;
    memcpy(saved_x, c->x, sizeof saved_x);
    memset((void *)(uintptr_t)sp, 0, 0x100);
    ST64(sp + 0x20, (uint64_t)(uintptr_t)name);
    uint8_t *arg = (uint8_t *)(uintptr_t)(sp + 0x28);
    static const float one[3] = {1, 1, 1};
    static const char empty[1] = "";
    memcpy(arg + 0x00, pos, 12);
    memcpy(arg + 0x0c, rot, 36);
    memcpy(arg + 0x30, one, 12);
    ST64(sp + 0x28 + 0x40, (uint64_t)(uintptr_t)empty);
    arg[0x90] = 3;
    memset(handle, 0, 0x40);
    c->sp = sp;
    uint64_t ok = rt_call(c, MOD_MAIN_BASE + COOP_REQUEST_CREATE, 8, mgr, sp + 0x20, sp + 0x28, (uint64_t)(uintptr_t)handle,
                          (uint64_t)1, (uint64_t)0, (uint64_t)0, (uint64_t)0);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    return (int)(ok & 1);
}

int coop_handle_status(const uint8_t *handle) {
    uint64_t w = coop_rd_ptr((uint64_t)(uintptr_t)handle);
    uint32_t st = 0;
    return w && coop_rd(w, &st, 4) ? (int)(st & 0xffff) : 0;
}

uint64_t coop_claim(Cpu *c, uint8_t *handle, uint8_t *link, uint64_t *ref) {
    memset(link, 0, 0x40);
    coop_guest_call(c, MOD_MAIN_BASE + COOP_CLAIM_ACTOR, (uint64_t)(uintptr_t)handle, (uint64_t)(uintptr_t)link, 1, 0);
    uint64_t r = 0;
    memcpy(&r, link + 8, 8);
    if (ref) *ref = r;
    return r ? coop_rd_ptr(r + 0x40) : 0;
}

// An actor's update links: +0x1c0 u16 ring of indices into [[main+0x395da68]] +0x2f8 (16-byte {parent, child}),
// +0x1c8 capacity, +0x1cc start, +0x1d0 count. Only links to resident actors are copied (TOTK_COOP_EDGES=all: every
// link): a new Player makes and links its own gear; the resident helpers are one per game, and without the link to
// ThrowBox its first frame crashes (main+0x4b46a4).
void coop_copy_dependencies(Cpu *c, uint64_t model, uint64_t copy, int verbose) {
    uint64_t sys = coop_rd_ptr(MOD_MAIN_BASE + COOP_ACTOR_SYSTEM_SLOT), tbl = coop_rd_ptr(coop_rd_ptr(MOD_MAIN_BASE + COOP_EDGE_TABLE_SLOT));
    uint64_t ring = coop_rd_ptr(model + 0x1c0), edges = tbl ? coop_rd_ptr(tbl + 0x2f8) : 0;
    int32_t cap = 0, start = 0, count = 0;
    uint32_t n_edges = 0;
    coop_rd(model + 0x1c8, &cap, 4), coop_rd(model + 0x1cc, &start, 4), coop_rd(model + 0x1d0, &count, 4);
    if (tbl) coop_rd(tbl + 0x2f0, &n_edges, 4);
    if (!sys || !ring || !edges || cap <= 0 || count < 0 || count > cap) return;
    static int all = -1;
    if (all < 0) { const char *v = cfg_get("TOTK_COOP_EDGES"); all = v && !strcmp(v, "all"); }
    int copied = 0;
    for (int i = 0; i < count; i++) {
        int slot = start + i >= cap ? start + i - cap : start + i;
        uint16_t idx = 0;
        coop_rd(ring + (uint64_t)slot * 2, &idx, 2);
        uint64_t e[2] = {0, 0};
        coop_rd(edges + (uint64_t)(idx < n_edges ? idx : 0) * 16, e, 16);
        if (e[0] != model && e[1] != model) continue;
        uint64_t other = e[0] == model ? e[1] : e[0];
        if (!all && !coop_is_resident(other)) continue;
        uint64_t parent = e[0] == model ? copy : e[0], child = e[1] == model ? copy : e[1];
        uint64_t ok = coop_guest_call(c, MOD_MAIN_BASE + COOP_SET_DEPENDENCY, sys, parent, child, 0);
        char b[40];
        if (verbose) rt_log("coop: update link to %s copied: %s", coop_actor_name(other, b, sizeof b), ok & 1 ? "queued" : "refused");
        copied++;
    }
    if (!verbose) rt_log("coop: %d update links copied to the new Player", copied);
}

// Controller layout (button-map and diff tests, 2026-11-03): +0x114 and +0x13c held mask, +0x8 pressed and +0xc
// released this frame (sead order, a guess), +0x30 + 4 * bit frames held (float), +0x120 left stick x / y, +0x128
// right stick (guess). Bits: A 0, B 1, ZL 2, X 3, Y 4, ZR 5, right stick click 6, left stick click 7, L 13, R 14,
// d-pad up 16, down 17, left 18, right 19, left stick up / down / left / right 20-23, right stick 24-27. Plus and
// Minus are not mapped (they would open the menus of this PC's player).
void coop_fill_controller(uint64_t ctl, uint64_t buttons, const float stick[4], uint32_t *prev, float held[32]) {
    static const int8_t to_ctl[16] = {0, 1, 3, 4, 7, 6, 13, 14, 2, 5, -1, -1, 18, 16, 19, 17};
    memcpy((void *)(uintptr_t)(ctl + 0x120), stick, 16);
    uint32_t hold = 0;
    for (int b = 0; b < 16; b++)
        if (buttons >> b & 1 && to_ctl[b] >= 0) hold |= 1u << to_ctl[b];
    // No L for a co-op Link: its abilities (Ultrahand, Fuse, Ascend, Recall, Autobuild) would run in this game on this
    // game's objects, which are not shared, and tint this game's view (Ultrahand's highlight, tools/coop/tour2.txt);
    // its own game runs them (2026-11-04). TOTK_COOP_TEST_ABILITY_KEEP: L passes (a control for tests).
    static int keep_l = -1;
    if (keep_l < 0) keep_l = cfg_get("TOTK_COOP_TEST_ABILITY_KEEP") != NULL;
    if (!keep_l) hold &= ~(1u << 13);
    // No A for a co-op Link unless ZL is held: A talks, picks up, opens and checks, on this game's NPCs, items, chests
    // and doors (its talk to Purah started the talk in this game too, tools/coop/tour3.txt); its own game does those.
    // With ZL (shield) A starts a shield surf, kept. TOTK_COOP_TEST_INTERACT_KEEP: A passes (a control) (2026-11-04).
    static int keep_a = -1;
    if (keep_a < 0) keep_a = cfg_get("TOTK_COOP_TEST_INTERACT_KEEP") != NULL;
    if (!keep_a && !(hold & 1u << 2)) hold &= ~1u;
    if (stick[1] > 0.5f) hold |= 1u << 20;
    if (stick[1] < -0.5f) hold |= 1u << 21;
    if (stick[0] < -0.5f) hold |= 1u << 22;
    if (stick[0] > 0.5f) hold |= 1u << 23;
    if (stick[3] > 0.5f) hold |= 1u << 24;
    if (stick[3] < -0.5f) hold |= 1u << 25;
    if (stick[2] < -0.5f) hold |= 1u << 26;
    if (stick[2] > 0.5f) hold |= 1u << 27;
    for (int b = 0; b < 32; b++) held[b] = hold >> b & 1 ? held[b] + 1 : 0;
    ST32(ctl + 0x114, hold), ST32(ctl + 0x13c, hold);
    ST32(ctl + 0x8, hold & ~*prev), ST32(ctl + 0xc, *prev & ~hold);
    memcpy((void *)(uintptr_t)(ctl + 0x30), held, 32 * sizeof(float));
    *prev = hold;
}
