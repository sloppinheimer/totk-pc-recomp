// The stand-in's animation, the masks of what it holds, and its sword trail: moved out of coop_world.c as it was
// (docs/coop/enemy-sync-spec.md B.28: the world file is taken apart so the older enemy sync in it can go). Nothing here
// is the enemy sync's.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// Test (TOTK_COOP_TEST_ANIMSLOTS=1): the animation player TotK Online replays onto its stand-in (read for behaviour
// only; 1.2.1 layout, checked here on 1.4.3): [[[actor+0x228]+8]+0x28]+0x40, its slot count at +0x38 and slots of
// 0xb0 bytes from [+0x40]: animation index u16 +2, weight f32 +0x10, frame f32 +0x28. Logged every 0.5 s for this
// Link and each co-op stand-in.
static uint16_t anim_bind_off(uint64_t g, uint64_t b);
static uint64_t anim_graph_of(uint64_t a);
static void anim_slots_log(const char *who, uint64_t a) {
    uint64_t mc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 8), model = mc ? coop_rd_ptr(mc + 0x28) : 0;
    uint64_t ap = model ? coop_rd_ptr(model + 0x40) : 0, arr = ap ? coop_rd_ptr(ap + 0x40) : 0;
    int32_t n = -1;
    if (ap) coop_rd(ap + 0x38, &n, 4);
    char line[700];
    int m = snprintf(line, sizeof line, "coop: test: anim slots of %s: model %llx player %llx (vtable main+%#llx) count %d:", who,
                     (unsigned long long)model, (unsigned long long)ap, (unsigned long long)(coop_rd_ptr(ap) - MOD_MAIN_BASE), n);
    {   // TotK Online's player for its play call: [G+0x28], G = [[[actor+0x228]+0x40]+0x18]+0x18 (a guess at its chain)
        uint64_t c8 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0, g = x ? coop_rd_ptr(x + 0x18) : 0;
        uint64_t p = g ? coop_rd_ptr(g + 0x28) : 0, pv = p ? coop_rd_ptr(p) : 0;
        rt_log("coop: test: anim chain of %s: comp8 %llx (vt main+%#llx) +0x18 %llx (vt main+%#llx) G %llx (vt main+%#llx) [G+0x28] %llx (vt main+%#llx, +0x30 main+%#llx)%s",
               who, (unsigned long long)c8, (unsigned long long)(coop_rd_ptr(c8) - MOD_MAIN_BASE), (unsigned long long)x,
               (unsigned long long)(coop_rd_ptr(x) - MOD_MAIN_BASE), (unsigned long long)g, (unsigned long long)(coop_rd_ptr(g) - MOD_MAIN_BASE),
               (unsigned long long)p, (unsigned long long)(pv - MOD_MAIN_BASE), (unsigned long long)(coop_rd_ptr(pv + 0x30) - MOD_MAIN_BASE),
               p == ap ? " (= the model's player)" : "");
    }
    {   // the partial binds (TotK Online, 1.2.1): G+0xf8 a count and an array of per-slot entries, each's byte +0x1c
        // the slot's partial index; the bind record [[[G+0x20] + index*0x48]+0x58]+0x18]+8. Raw words to check them.
        uint64_t c8 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0, g = x ? coop_rd_ptr(x + 0x18) : 0;
        uint64_t w[8] = {0};
        if (g) coop_rd(g + 0xf0, w, sizeof w);
        char l2[600];
        int m2 = snprintf(l2, sizeof l2, "coop: test: anim binds of %s: G+0xf0..: %llx %llx %llx %llx %llx %llx;", who, (unsigned long long)w[0],
                          (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5]);
        uint64_t ev[6] = {0};
        if (w[1]) coop_rd(w[1], ev, sizeof ev);
        m2 += snprintf(l2 + m2, sizeof l2 - m2, " [G+0xf8]: %llx %llx %llx %llx %llx %llx;", (unsigned long long)ev[0], (unsigned long long)ev[1],
                       (unsigned long long)ev[2], (unsigned long long)ev[3], (unsigned long long)ev[4], (unsigned long long)ev[5]);
        for (int z = 0; z < 6; z++) {   // each word that points somewhere: its first 8 pointers' byte +0x1c
            uint64_t arr2 = ev[z];
            if (arr2 < 0x100000000ull || arr2 > 0x8000000000ull) continue;
            m2 += snprintf(l2 + m2, sizeof l2 - m2, " w%d:", z);
            for (int i = 0; i < 7 && m2 < 560; i++) {
                uint64_t e = coop_rd_ptr(arr2 + 8ull * i);
                uint8_t b = 0xff;
                if (e) coop_rd(e + 0x1c, &b, 1);
                m2 += snprintf(l2 + m2, sizeof l2 - m2, " %u", b);
            }
        }
        uint64_t g20 = g ? coop_rd_ptr(g + 0x20) : 0;
        int32_t g18 = 0;
        if (g) coop_rd(g + 0x18, &g18, 4);
        m2 += snprintf(l2 + m2, sizeof l2 - m2, "; G+0x18 %d, G+0x20 %llx", g18, (unsigned long long)g20);
        rt_log("%s", l2);
        // each slot's bind (+0x08) and change count (+0xa8), against the table's: [[[G+0x20]+i*0x48]+0x58]+0x18 is
        // TotK Online's record P, whose +8 the play compares with the slot's +0x08
        char l3[700];
        int m3 = snprintf(l3, sizeof l3, "coop: test: anim bind table of %s:", who);
        for (int i = 0; g20 && i < 10 && m3 < 600; i++) {
            uint64_t e = coop_rd_ptr(g20 + 0x48ull * i + 0x58), p = e ? coop_rd_ptr(e + 0x18) : 0, b = p ? coop_rd_ptr(p + 8) : 0;
            m3 += snprintf(l3 + m3, sizeof l3 - m3, " [%d] P %llx B %llx", i, (unsigned long long)p, (unsigned long long)b);
        }
        m3 += snprintf(l3 + m3, sizeof l3 - m3, "; slots:");
        for (int i = 0; arr && i < 7 && m3 < 680; i++) {
            uint64_t b = coop_rd_ptr(arr + 0xb0ull * i + 8);
            uint32_t cc = 0;
            coop_rd(arr + 0xb0ull * i + 0xa8, &cc, 4);
            m3 += snprintf(l3 + m3, sizeof l3 - m3, " %llx/%u", (unsigned long long)b, cc);
        }
        rt_log("%s", l3);
        // where each slot's bind is held: words equal to it in G's first 0x400 bytes, and one level down (0x200 of
        // each object G points to), as "G+off" or "G+off -> +off2"
        static int found_said;
        for (int i = 0; arr && g && i < 3 && found_said < 12; i++) {
            uint64_t b = coop_rd_ptr(arr + 0xb0ull * i + 8);
            if (!b) continue;
            static uint64_t gw[0x400 / 8], ow[0x200 / 8];
            if (!coop_rd(g, gw, sizeof gw)) break;
            char l4[400];
            int m4 = snprintf(l4, sizeof l4, "coop: test: anim bind %llx of slot %d is at:", (unsigned long long)b, i);
            for (int j = 0; j < 0x400 / 8 && m4 < 360; j++) {
                if (gw[j] == b) m4 += snprintf(l4 + m4, sizeof l4 - m4, " G+%#x", j * 8);
                if (gw[j] < 0x100000000ull || gw[j] > 0x8000000000ull || !coop_rd(gw[j], ow, sizeof ow)) continue;
                for (int q = 0; q < 0x200 / 8 && m4 < 360; q++)
                    if (ow[q] == b) m4 += snprintf(l4 + m4, sizeof l4 - m4, " G+%#x->+%#x", j * 8, q * 8);
                    else if (ow[q] > 0x100000000ull && ow[q] < 0x8000000000ull && (q & 1) == 0) {
                        uint64_t z = 0;   // two levels: an array of records whose +8 is the bind
                        for (int r = 0; r < 16; r++)
                            if (coop_rd(ow[q] + 0x48ull * r + 8, &z, 8) && z == b) { m4 += snprintf(l4 + m4, sizeof l4 - m4, " G+%#x->+%#x->[%d*0x48]+8", j * 8, q * 8, r); break; }
                    }
            }
            rt_log("%s", l4), found_said++;
        }
    }
    for (int i = 0; arr && i < n && i < 8 && m < 640; i++) {
        uint8_t s[0x30];
        if (!coop_rd(arr + 0xb0ull * i, s, sizeof s)) break;
        uint16_t idx;
        float w, f;
        memcpy(&idx, s + 2, 2), memcpy(&w, s + 0x10, 4), memcpy(&f, s + 0x28, 4);
        uint64_t b;
        memcpy(&b, s + 8, 8);
        m += snprintf(line + m, sizeof line - m, " [%d] %u w%.2f f%.1f m%#x", i, idx, w, f, anim_bind_off(anim_graph_of(a), b));
    }
    rt_log("%s", line);
}
void coop_anim_slots_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ANIMSLOTS") != NULL;
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (!on || now - last < 500000000ull) return;
    last = now;
    anim_slots_log("this Link", me);
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], n = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int i = 0; i < n; i++) anim_slots_log("a stand-in", co[i]);
}

// The animation play TotK Online calls on its stand-in (read for behaviour only): the object Q = [G+0x28], G =
// [[[actor+0x228]+0x40]+0x18]+0x18 (component 8's animation graph), its vtable +0x30 (1.4.3 main+0x1b569fc):
// (Q, slot, animation index, partial bind or 0, 0, 0xffff, s0 weight, s1 frame); it writes the slot of the player
// [[Q+8]+0x40] (the model's, anim_player_of).
static uint64_t anim_player_of(uint64_t a) {
    uint64_t mc = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 8), model = mc ? coop_rd_ptr(mc + 0x28) : 0;
    return model ? coop_rd_ptr(model + 0x40) : 0;
}
static uint64_t anim_graph_of(uint64_t a) {
    uint64_t c8 = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0;
    return x ? coop_rd_ptr(x + 0x18) : 0;
}
static uint64_t anim_caller_of(uint64_t a) {
    uint64_t g = anim_graph_of(a);
    return g ? coop_rd_ptr(g + 0x28) : 0;
}
// A slot's body mask (its bind, slot +0x08; the play compares its 4th argument's +8 with it and the bind's +0x18 with
// slot +0xa8) is held at +0xc0 of a layer object the graph points to: [G+0x1a8]+0xc0 for a Link's whole-body layers,
// [G+0x2c8]+0xc0 for the upper-body one (idle, walking and the bow; TOTK_COOP_TEST_ANIMSLOTS, 2026-11-07). The same
// offset names the same mask in every game (both load Link's animation set), so it is what is sent. 0: not found.
static uint16_t anim_bind_off(uint64_t g, uint64_t b) {
    static __thread struct { uint64_t g, b; uint16_t off; } cache[8];
    if (!g || !b) return 0;
    for (int i = 0; i < 8; i++) if (cache[i].g == g && cache[i].b == b) return cache[i].off;
    uint16_t off = 0;
    static __thread uint64_t gw[0x800 / 8];
    if (coop_rd(g, gw, sizeof gw))
        for (int j = 1; j < 0x800 / 8 && !off; j++)
            if (gw[j] > 0x100000000ull && gw[j] < 0x8000000000ull && coop_rd_ptr(gw[j] + 0xc0) == b) off = (uint16_t)(j * 8);
    static __thread int next;
    cache[next].g = g, cache[next].b = b, cache[next].off = off, next = (next + 1) & 7;
    return off;
}
static uint64_t anim_bind_at(uint64_t g, uint16_t off) {
    uint64_t layer = g && off && off < 0x800 ? coop_rd_ptr(g + off) : 0;
    return layer ? coop_rd_ptr(layer + 0xc0) : 0;
}
// bind: the mask (0 none): passed as a record whose +8 is it, as the play reads it.
static void anim_play_bind(Cpu *c, uint64_t q, int slot, int idx, float weight, float frame, uint64_t bind) {
    static __thread uint64_t rec[2];
    rec[0] = 0, rec[1] = bind;
    uint64_t vt = coop_rd_ptr(q), fn = vt ? coop_rd_ptr(vt + 0x30) : 0, ap = q;
    if (!fn) return;
    __uint128_t v0 = c->v[0], v1 = c->v[1];
    uint32_t wb, fb;
    memcpy(&wb, &weight, 4), memcpy(&fb, &frame, 4);
    c->v[0] = wb, c->v[1] = fb;
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    rt_call(c, fn, 6, ap, (uint64_t)slot, (uint64_t)idx, bind ? (uint64_t)(uintptr_t)rec : 0ull, 0ull, 0xffffull);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    c->v[0] = v0, c->v[1] = v1;
}
static void anim_play(Cpu *c, uint64_t q, int slot, int idx, float weight, float frame) {
    uint64_t vt = coop_rd_ptr(q), fn = vt ? coop_rd_ptr(vt + 0x30) : 0, ap = q;
    if (!fn) return;
    __uint128_t v0 = c->v[0], v1 = c->v[1];
    uint32_t wb, fb;
    memcpy(&wb, &weight, 4), memcpy(&fb, &frame, 4);
    c->v[0] = wb, c->v[1] = fb;
    uint64_t saved_x[31], sp0 = c->sp;
    memcpy(saved_x, c->x, sizeof saved_x);
    c->sp = (sp0 - 0x100) & ~0xfull;
    rt_call(c, fn, 6, ap, (uint64_t)slot, (uint64_t)idx, 0ull, 0ull, 0xffffull);
    c->sp = sp0;
    memcpy(c->x, saved_x, sizeof saved_x);
    c->v[0] = v0, c->v[1] = v1;
}
// An animation player's first slots as {index, weight, frame}; the count read (at most max).
#define ANIM_SLOTS COOP_ANIM_SLOTS
typedef struct { uint16_t idx, pad; float w, f; uint64_t bind; } AnimSlot;   // bind: slot +0x08
static int anim_read(uint64_t ap, AnimSlot *out, int max) {
    uint64_t arr = ap ? coop_rd_ptr(ap + 0x40) : 0;
    int32_t n = 0;
    if (!arr || !coop_rd(ap + 0x38, &n, 4) || n <= 0) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint8_t s[0x30];
        if (!coop_rd(arr + 0xb0ull * i, s, sizeof s)) return i;
        memcpy(&out[i].idx, s + 2, 2), memcpy(&out[i].w, s + 0x10, 4), memcpy(&out[i].f, s + 0x28, 4), memcpy(&out[i].bind, s + 8, 8);
    }
    return n;
}
// Test (TOTK_COOP_TEST_ANIMCOPY=1): this game's own Link's slots played on each stand-in every tick (no network).
void coop_anim_copy_test(Cpu *c, uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_ANIMCOPY") != NULL;
    if (!on) return;
    AnimSlot s[ANIM_SLOTS];
    int n = anim_read(anim_player_of(me), s, ANIM_SLOTS);
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int k = 0; k < nc; k++) {
        uint64_t ap = anim_caller_of(co[k]);
        for (int i = 0; ap && i < n; i++) if (s[i].idx != 0xffff) anim_play(c, ap, i, s[i].idx, s[i].w, s[i].f);
        static int said;
        if (ap && !said++) rt_log("coop: test: animcopy: %d slots of this Link played on a stand-in (play main+%#llx)", n,
                                  (unsigned long long)(coop_rd_ptr(coop_rd_ptr(ap) + 0x30) - MOD_MAIN_BASE));
    }
}

// ---- the stand-in's animation (TotK Online's ActorAnimationSync, read for behaviour only).
// The owner sends its Link's first seven animation slots with its state (NetState.anim): clip, weight, frame and the
// frames a second it moved since the last send. Each other game plays them on its PlayerPuppet every tick, the frame
// carried on by that rate for the state's age, so the stand-in moves with its owner's own clips and nothing of its
// own chooses another (it has no Player component). Each slot's body mask goes as where it is in the graph
// (anim_bind_off) and is played with the stand-in's own mask from there (TotK Online sends its partial-bind index
// and flags; read for behaviour only).
void coop_anim_pack(uint64_t me, CoopAnim *out) {
    AnimSlot s[COOP_ANIM_SLOTS];
    int n = anim_read(anim_player_of(me), s, COOP_ANIM_SLOTS);
    {   // How many slots this Link's animation player has, and which past the 7 sent carry weight (a two-handed weapon's
        // left hand did not reach it on the stand-in). Logged when that set changes.
        static AnimSlot all[24];
        static uint32_t last_mask = ~0u;
        static int said;
        int na = anim_read(anim_player_of(me), all, 24);
        uint32_t mask = 0;
        for (int i = COOP_ANIM_SLOTS; i < na; i++) if (all[i].idx != 0xffff && all[i].w > 0.001f) mask |= 1u << i;
        if (mask != last_mask && said < 80) {
            last_mask = mask, said++;
            char b[200] = "";
            for (int i = COOP_ANIM_SLOTS; i < na; i++)
                if (mask >> i & 1) { size_t l = strlen(b); snprintf(b + l, sizeof b - l, " %d:clip %u w %.2f", i, all[i].idx, all[i].w); }
            rt_log("coop: this Link's animation slots: %d, past the %d sent with weight:%s", na, COOP_ANIM_SLOTS, b[0] ? b : " none");
        }
    }
    uint64_t g = anim_graph_of(me);
    // Each slot's rate (frames a second) from a reference frame at least RATE_SPAN_US of world time back, the last
    // rate kept in between. Taken from the call before, a second call in the same frame saw no time and no frames
    // pass and sent 0: every other state stopped the stand-in's clips, and the easing pulled them on again (the tester's
    // session, "still quite choppy", 2026-11-07). World time: the clips step with the frames.
    enum { RATE_SPAN_US = 30000 };
    static float ref_f[COOP_ANIM_SLOTS], last_rate[COOP_ANIM_SLOTS];
    static int64_t ref_w[COOP_ANIM_SLOTS];
    static uint16_t ref_i[COOP_ANIM_SLOTS] = {[0 ... COOP_ANIM_SLOTS - 1] = 0xffff};
    int64_t w = frame_step_world_us();
    if (w < 0) w = (int64_t)(plat_now_ns() / 1000);
    for (int i = 0; i < COOP_ANIM_SLOTS; i++) {
        CoopAnim a = {.idx = 0xffff};
        if (i < n && s[i].idx != 0xffff && s[i].w == s[i].w && s[i].f == s[i].f) {
            float wt = s[i].w < 0 ? 0 : s[i].w > 1 ? 1 : s[i].w;
            a.idx = s[i].idx, a.weight = (uint16_t)lroundf(wt * 65535), a.frame = s[i].f, a.bind = anim_bind_off(g, s[i].bind);
            if (ref_i[i] != s[i].idx || w < ref_w[i]) {   // a new clip: its rate from the next span
                ref_i[i] = s[i].idx, ref_f[i] = s[i].f, ref_w[i] = w, last_rate[i] = 0;
            } else if (s[i].f < ref_f[i]) {             // looped: the rate it had, measured again from here
                ref_f[i] = s[i].f, ref_w[i] = w;
            } else if (w - ref_w[i] >= RATE_SPAN_US) {
                float r = (s[i].f - ref_f[i]) / (float)((w - ref_w[i]) / 1e6);
                last_rate[i] = r > 600 ? last_rate[i] : r;
                ref_f[i] = s[i].f, ref_w[i] = w;
            }
            a.rate = last_rate[i];
        } else
            ref_i[i] = 0xffff;
        out[i] = a;
    }
}
// The owner's slots (in, age_ns old) played on a stand-in; a slot the owner left empty is played with weight 0 on
// the clip the stand-in still has there.
// The stand-in's animation, measured (every 5 s, at most 720 lines: an hour): how often slot 0 is set a second, how many of
// those set the same frame as the one before (a held frame: the "choppy" look), the mean and largest step between
// sets, how many new states came, and the oldest state age used. To tell a stand-in fed too rarely from one fed
// unevenly.
static void anim_watch(uint16_t idx, float sent, float fr, float age) {
    static uint64_t t0;
    static int calls, held, news, said;
    static float last_fr = -1, last_sent = -1, step_sum, step_max, age_max;
    static uint16_t last_idx = 0xffff;
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    calls++;
    if (sent != last_sent || idx != last_idx) news++;
    if (idx == last_idx && last_fr >= 0) {
        float d = fr - last_fr;
        if (d == 0) held++;
        if (d > 0) { step_sum += d; if (d > step_max) step_max = d; }
    }
    if (age > age_max) age_max = age;
    last_fr = fr, last_sent = sent, last_idx = idx;
    if (now - t0 >= 5000000000ull) {
        double sec = (now - t0) / 1e9;
        if (said++ < 720)
            rt_log("coop: stand-in anim: %.0f sets/s, %d held (same frame), step mean %.2f max %.2f frames, %.0f new states/s, oldest state %.0f ms",
                   calls / sec, held, calls > held ? step_sum / (calls - held) : 0, step_max, news / sec, age_max * 1000);
        t0 = now, calls = held = news = 0, step_sum = step_max = age_max = 0;
    }
}
// Each stand-in's own clock per slot: the frame it showed, carried on at the owner's rate each set and eased toward
// the owner's (sent frame plus rate times the state's age), so a new state no longer snaps it. States come about 24
// times a second with gaps up to 80 ms, and taking each one's frame moved the clips by up to 2.5 frames at once,
// 24 times a second: every animation looked choppy (a play session, anim_watch, 2026-11-07). A new clip, or one
// over ANIM_SNAP frames off (a loop, a cut), is taken as sent. TOTK_COOP_STANDIN_ANIM_EASE=0: as sent, as before.
// (Two sets a frame were measured; both ease from the same clock.)
#define ANIM_SNAP 4.0f
#define ANIM_EASE_HZ 6.0f   // the share of the gap closed a second
static struct { uint64_t standin, ns; uint16_t idx[COOP_ANIM_SLOTS]; float f[COOP_ANIM_SLOTS]; } aclock[COOP_MAX_PLAYERS];
static int anim_clock(uint64_t standin, float *dt) {   // the stand-in's clock, and the seconds since its last set
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_ANIM_EASE"); on = !(e && *e == '0'); }
    if (!on) return -1;
    int k = -1, free = -1;
    for (int j = 0; j < COOP_MAX_PLAYERS && k < 0; j++) {
        if (aclock[j].standin == standin) k = j;
        else if (!aclock[j].standin && free < 0) free = j;
    }
    uint64_t now = plat_now_ns();
    if (k < 0) {
        k = free >= 0 ? free : 0;
        memset(&aclock[k], 0, sizeof aclock[k]);
        aclock[k].standin = standin, aclock[k].ns = now;
        for (int j = 0; j < COOP_ANIM_SLOTS; j++) aclock[k].idx[j] = 0xffff;
    }
    float d = (float)((now - aclock[k].ns) / 1e9);
    *dt = d > 0.1f ? 0.1f : d;
    aclock[k].ns = now;
    return k;
}
static float anim_eased(int k, float dt, int i, uint16_t idx, float target, float rate) {
    if (k < 0) return target;
    float f = aclock[k].f[i];
    if (aclock[k].idx[i] != idx || fabsf(target - f) > ANIM_SNAP) f = target;
    else {
        f += rate * dt;
        float a = dt * ANIM_EASE_HZ;
        f += (target - f) * (a > 1 ? 1 : a);
    }
    aclock[k].idx[i] = idx, aclock[k].f[i] = f;
    return f;
}
void coop_anim_apply(Cpu *c, uint64_t standin, const CoopAnim *in, int64_t age_ns) {
    uint64_t q = anim_caller_of(standin);
    if (!q) return;
    float cdt = 0;
    int ck = anim_clock(standin, &cdt);
    AnimSlot have[COOP_ANIM_SLOTS];
    int n = anim_read(anim_player_of(standin), have, COOP_ANIM_SLOTS);
    uint64_t g = anim_graph_of(standin);
    float age = age_ns > 1000000000ll ? 1 : age_ns < -1000000000ll ? -1 : (float)(age_ns / 1e9);
    int played = 0, masked = 0;
    for (int i = 0; i < COOP_ANIM_SLOTS; i++) {
        if (in[i].idx != 0xffff) {
            // The masks (TOTK_COOP_STANDIN_MASKS=0: none, every clip on the whole body). Played with the stand-in's
            // own mask flags they put it in a T pose in a sword fight; with its owner's flags on them
            // (coop_anim_masks_apply) it stood and held its sword (attack_enemy_3rd.txt frames, 2026-11-07).
            static int masks = -1;
            if (masks < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_MASKS"); masks = !(e && *e == '0'); }
            uint64_t b = masks ? anim_bind_at(g, in[i].bind) : 0;
            masked += b != 0;
            if (i >= 7) {   // the slots past TotK Online's seven: a two-handed hold (logged as they change, 40 lines)
                static uint16_t last_idx[COOP_ANIM_SLOTS];
                static int said7;
                if (in[i].idx != last_idx[i] && said7 < 40) {
                    said7++;
                    int sent_mask = 0;
                    rt_log("coop: stand-in: slot %d plays clip %u weight %.2f, its owner's mask at %#x %s", i, in[i].idx, in[i].weight / 65535.0f,
                           in[i].bind, b ? "bound" : "not found here");
                    (void)sent_mask;
                }
                last_idx[i] = in[i].idx;
            }
            float tf = in[i].frame + in[i].rate * age;
            float fr = anim_eased(ck, cdt, i, in[i].idx, tf > 0 ? tf : 0, in[i].rate);
            anim_play_bind(c, q, i, in[i].idx, in[i].weight / 65535.0f, fr, b), played++;
            if (i == 0) anim_watch(in[0].idx, in[0].frame, fr, age);
        } else if (i < n && have[i].idx != 0xffff && have[i].w > 0) anim_play(c, q, i, have[i].idx, 0, have[i].f);
    }
    static int said;
    if (played && !said++) rt_log("coop: stand-in: its owner's animation (%d slots, %d with a body mask) plays on it", played, masked);
}

// Test (TOTK_COOP_TEST_EQDIFF=1): this Link's EquipmentUser ([actor+0x228]+0x230, its first 0x4800 bytes) each tick;
// every byte that goes between 0 and 1 is logged with the world time (at most 200 lines): to find the "weapon is
// attacking" byte TotK Online copies onto its stand-in (1.2.1 eq+0x660; read for behaviour only) by a swing's times.
void coop_eqdiff_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_EQDIFF") != NULL;
    if (!on) return;
    enum { N = 0x4800 };
    static uint8_t last[N], now[N];
    static int have, said;
    static uint16_t flips[N];
    uint64_t eq = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x230);
    if (!eq || !coop_rd(eq, now, N)) return;
    if (have)
        for (int i = 0; i < N && said < 200; i++)
            if (now[i] != last[i] && now[i] <= 1 && last[i] <= 1 && flips[i]++ < 12)
                rt_log("coop: test: eqdiff +%#x %u -> %u at %.2f s", i, last[i], now[i], frame_step_world_us() / 1e6), said++;
    memcpy(last, now, N), have = 1;
}

// Test (TOTK_COOP_TEST_BINDS=1): the masks at the graph offsets a Link's slots use (anim_bind_off), for this Link and
// each stand-in side by side, every 2 s: the layer object's mask (+0xc0) and that mask's first 0x40 bytes, to see
// whether one offset holds the same mask in both (the stand-in has no Player component).
void coop_binds_test(uint64_t me) {
    static int on = -1;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_BINDS") != NULL;
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (!on || now - last < 2000000000ull) return;
    last = now;
    static const uint16_t offs[] = {0x1a8, 0x1f0, 0x238, 0x280, 0x2c8, 0x310};
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int w = 0; w <= nc && w < 3; w++) {
        uint64_t a = w ? co[w - 1] : me, g = anim_graph_of(a);
        for (unsigned k = 0; g && k < sizeof offs / sizeof offs[0]; k++) {
            uint64_t layer = coop_rd_ptr(g + offs[k]), b = layer ? coop_rd_ptr(layer + 0xc0) : 0;
            uint32_t v[16] = {0};
            if (b) coop_rd(b, v, sizeof v);
            char ln[400];
            int m = snprintf(ln, sizeof ln, "coop: test: binds %s G+%#x: layer %llx (vt main+%#llx) mask %llx:", w ? "stand-in" : "this Link", offs[k],
                             (unsigned long long)layer, (unsigned long long)(layer ? coop_rd_ptr(layer) - MOD_MAIN_BASE : 0), (unsigned long long)b);
            for (int i = 0; i < 16; i++) m += snprintf(ln + m, sizeof ln - m, " %x", v[i]);
            rt_log("%s", ln);
        }
    }
}

// The masks' state (CoopMask). The stand-in's graph has the same layer objects at the same offsets as a Link's, each
// with its mask at +0xc0, but the flags the graph sets in them differ (a Link's whole-body mask 83 83 80 83 80 83 80,
// the stand-in's 83 83 80 80 83, TOTK_COOP_TEST_BINDS, 2026-11-07): the stand-in's own graph does not set them as the
// owner's does. So the owner sends the flags of each mask its slots use (offset, u16 +0x1c, byte flags [+0x10] count
// +8, word flags [+0x28] count +0x20) and the stand-in's masks take them before its slots are played; a change bumps
// the mask's change count (+0x18), which the play compares with the slot's (+0xa8) to bind it again.
void coop_anim_masks_pack(uint64_t me, const CoopAnim *anim, CoopMask *out) {
    memset(out, 0, sizeof(CoopMask) * COOP_ANIM_MASKS);
    uint64_t g = anim_graph_of(me);
    int n = 0;
    for (int i = 0; g && i < COOP_ANIM_SLOTS && n < COOP_ANIM_MASKS; i++) {
        uint16_t off = anim[i].bind;
        int dup = !off;
        for (int k = 0; k < n && !dup; k++) dup = out[k].off == off;
        uint64_t b = dup ? 0 : anim_bind_at(g, off);
        if (!b) continue;
        CoopMask *m = &out[n];
        uint32_t nb = 0, nw = 0;
        coop_rd(b + 8, &nb, 4), coop_rd(b + 0x20, &nw, 4), coop_rd(b + 0x1c, &m->c1c, 2);
        if (nb > 16) nb = 16;
        if (nw > 16) nw = 16;
        uint64_t pb = coop_rd_ptr(b + 0x10), pw = coop_rd_ptr(b + 0x28);
        if ((nb && (!pb || !coop_rd(pb, m->b, nb))) || (nw && (!pw || !coop_rd(pw, m->w, 4 * nw)))) continue;
        m->off = off, m->nb = (uint8_t)nb, m->nw = (uint8_t)nw, n++;
    }
    {   // slots whose mask did not fit in the COOP_ANIM_MASKS sent (logged as that changes, 40 lines)
        static int last_missing = -1, said;
        int missing = 0;
        char b[160] = "";
        for (int i = 0; i < COOP_ANIM_SLOTS; i++) {
            if (!anim[i].bind || anim[i].idx == 0xffff) continue;
            int in = 0;
            for (int k = 0; k < n; k++) in |= out[k].off == anim[i].bind;
            if (!in) { missing++; size_t l = strlen(b); snprintf(b + l, sizeof b - l, " slot %d at %#x", i, anim[i].bind); }
        }
        if (missing != last_missing && said < 40) {
            said++, last_missing = missing;
            rt_log("coop: this Link's masks: %d sent, %d slots' masks not sent:%s", n, missing, b[0] ? b : " none");
        }
    }
}
void coop_anim_masks_apply(uint64_t standin, const CoopMask *in) {
    uint64_t g = anim_graph_of(standin);
    int changed = 0;
    for (int k = 0; g && k < COOP_ANIM_MASKS; k++) {
        if (!in[k].off) continue;
        uint64_t b = anim_bind_at(g, in[k].off);
        uint32_t nb = 0, nw = 0;
        // (at most 16 of each, as CoopMask holds and as coop_anim_masks_pack sends: a larger count from the network
        // would run past hb, hw and the packet's own arrays; code review, 2026-11-07)
        if (in[k].nb > 16 || in[k].nw > 16) continue;
        if (!b || !coop_rd(b + 8, &nb, 4) || !coop_rd(b + 0x20, &nw, 4) || nb != in[k].nb || nw != in[k].nw) continue;   // (another shape)
        uint64_t pb = coop_rd_ptr(b + 0x10), pw = coop_rd_ptr(b + 0x28);
        uint8_t hb[16];
        uint32_t hw[16];
        uint16_t c1c = 0;
        coop_rd(b + 0x1c, &c1c, 2);
        if ((nb && (!pb || !coop_rd(pb, hb, nb))) || (nw && (!pw || !coop_rd(pw, hw, 4 * nw)))) continue;
        if ((!nb || !memcmp(hb, in[k].b, nb)) && (!nw || !memcmp(hw, in[k].w, 4 * nw)) && c1c == in[k].c1c) continue;
        if (nb) memcpy((void *)(uintptr_t)pb, in[k].b, nb);
        if (nw) memcpy((void *)(uintptr_t)pw, in[k].w, 4 * nw);
        ST16(b + 0x1c, in[k].c1c);
        ST32(b + 0x18, LD32(b + 0x18) + 1);
        changed++;
    }
    static int said;
    if (changed && said++ < 5) rt_log("coop: stand-in: %d of its body masks set as its owner's", changed);
}

// ---- the sword trail (SwordBlur component, vtable main+0x367e028, 0xe8 bytes, made by main+0xdd38d4). Its setup
// (main+0x1c1caf4) bakes, for every node of the actor's animation sequence (ASB) with an event of type 17
// ("WeaponBlur" in the event list at main+0x3713400), the blade's path over that event's frames into the table at
// [comp+0xc0] (+8 entries of 0x28 bytes: +4 first frame, +8 last frame, +0x20 the clip's name; +0x28 count).
#define SWORDBLUR_VT 0x367e028ull
static uint64_t swordblur_of(uint64_t actor) {
    uint64_t t = actor ? coop_rd_ptr(actor + 0x228) : 0, p = t ? coop_rd_ptr(t + 0x4a8) : 0;   // (where it is in both, 1.4.3)
    if (p && coop_rd_ptr(p) == MOD_MAIN_BASE + SWORDBLUR_VT) return p;
    for (int i = 0; t && i < 0x900 / 8; i++) {
        uint64_t p = coop_rd_ptr(t + 8ull * i);
        if (p && coop_rd_ptr(p) == MOD_MAIN_BASE + SWORDBLUR_VT) return p;
    }
    return 0;
}
static int swordblur_slot(uint64_t actor) {
    uint64_t t = actor ? coop_rd_ptr(actor + 0x228) : 0;
    for (int i = 0; t && i < 0x900 / 8; i++) {
        uint64_t p = coop_rd_ptr(t + 8ull * i);
        if (p && coop_rd_ptr(p) == MOD_MAIN_BASE + SWORDBLUR_VT) return i * 8;
    }
    return -1;
}
// Test (TOTK_COOP_TEST_BLURWATCH=1): this Link's and each stand-in's SwordBlur component: where it is, its baked
// table once, and every change of its words +0x28..+0xe8 with the world time (at most 400 lines).
void coop_blur_test(uint64_t me) {
    static int on = -1, said, told[1 + COOP_MAX_PLAYERS];
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_BLURWATCH") != NULL;
    if (!on) return;
    static uint32_t last[1 + COOP_MAX_PLAYERS][0xe8 / 4];
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int w = 0; w <= nc && w <= COOP_MAX_PLAYERS; w++) {
        uint64_t a = w ? co[w - 1] : me, b = swordblur_of(a);
        if (!b) continue;
        uint32_t now[0xe8 / 4];
        if (!coop_rd(b, now, sizeof now)) continue;
        if (!told[w]) {
            told[w] = 1;
            uint64_t tab = coop_rd_ptr(b + 0xc0), ents = 0;
            uint32_t n = 0;
            if (tab) coop_rd(tab + 8, &ents, 8), coop_rd(tab + 0x28, &n, 4);
            rt_log("coop: test: blur %s: component [+0x228]+%#x at %llx, baked table %llx with %u entries", w ? "stand-in" : "this Link",
                   swordblur_slot(a), (unsigned long long)b, (unsigned long long)tab, n);
            for (uint32_t k = 0; ents && k < n && k < 400; k++) {
                int32_t e[3] = {0};
                coop_rd(ents + 0x28ull * k, e, 12);
                uint64_t nm = coop_rd_ptr(ents + 0x28ull * k + 0x20);
                char s[64] = {0};
                if (nm) coop_rd(nm, s, sizeof s - 1);
                if (k < 40 || w == 0) rt_log("coop: test: blur   %u: %s frames %d..%d (%d samples)", k, s, e[1], e[2], e[0]);
            }
            memcpy(last[w], now, sizeof now);
            continue;
        }
        for (int i = 0x28 / 4; i < 0xe8 / 4 && said < 400; i++)
            if (now[i] != last[w][i])
                rt_log("coop: test: blur %s +%#x %08x -> %08x at %.3f s", w ? "stand-in" : "this Link", i * 4, last[w][i], now[i], frame_step_world_us() / 1e6), said++;
        memcpy(last[w], now, sizeof now);
    }
}

// ---- The stand-in's sword trail. Its SwordBlur update (main+0x275954, x0 the component) clears +0x2a, then looks
// through the actor's animation graph G (anim_graph_of) for an event of kind 17 that is on: G +0x18 the number of
// layers and +0x20 their records (0x48 bytes, the first word the layer; the first layer's weight at +0xd0 must not be
// 0), a layer's node at +0x60, the node's events at +0x28 (0x48 bytes each, +0xa0 how many, +0x20 the bound of the
// index), an event's kind at [+0x28]+0x2c (or [+0x20]+4), its on byte at +0, its frame at [+0x10]+8, its first frame
// at [+0x30]+0x10 or [+0x20]+0x14 (main+0x293ac18) and its hand from main+0x293b3d4 (node, event, 0; with [+0x30]:
// its +0 entries, +8 their count, +0xc an entry's size, an entry the address of a record whose +8 is the hand).
// With one found it emits the "Blur_Default" effect on the weapon in that hand (the links at +0x30/+0x48/+0x60, the
// EquipmentUser [+0x228]+0x230), keeps the effect's handle at +0xc8 and the event at +0xe0 (a new event: a new
// effect), sets +0x2a while the effect lives and +0xdc to the event's frame. The stand-in's animation is played clip
// by clip (coop_anim_apply), so its graph never has the swing's node and its event: no trail.
// (The component's setup, main+0x1c1caf4, also bakes the blade's path over every WeaponBlur event of the sequence
// into a table at +0xc0; the stand-in's has the same 198 entries as a Link's, TOTK_COOP_TEST_BLURWATCH.)
// So the owner sends its event (NetState.blur_*: on, hand, a number per event, frame, first frame, parameter), and on
// the stand-in the update runs once with a graph of one layer holding one such event made from them (a copy of its
// own graph with only the layers changed, put in its place for that call; coop_blur_hook). TOTK_COOP_STANDIN_BLUR=0:
// none of it.
static int blur_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_COOP_STANDIN_BLUR"); on = !(e && *e == '0'); }
    return on;
}
#define BLUR_EVENT_KIND 0x11
#define BLUR_HAND 0x293b3d4   // (node, event, 0): the event's game::WeaponBlurType
// This Link's graph's WeaponBlur event that is on, found as the update finds it: 1 with the node and event, else 0.
static int blur_event_find(uint64_t g, uint64_t *node_out, uint64_t *ev_out) {
    int32_t n = 0;
    uint64_t arr = g ? coop_rd_ptr(g + 0x20) : 0;
    if (!arr || !coop_rd(g + 0x18, &n, 4) || n < 1 || n > 64) return 0;
    float w0 = 0;
    uint64_t l0 = coop_rd_ptr(arr);
    if (!l0 || !coop_rd(l0 + 0xd0, &w0, 4) || w0 == 0) return 0;
    for (int k = 0; k < n; k++) {
        uint64_t l = coop_rd_ptr(arr + 0x48ull * k), nd = l ? coop_rd_ptr(l + 0x60) : 0;
        if (!nd) return 0;
        uint32_t cnt = 0, bound = 0;
        coop_rd(nd + 0xa0, &cnt, 4), coop_rd(nd + 0x20, &bound, 4);
        uint64_t evs = coop_rd_ptr(nd + 0x28);
        for (uint32_t j = 0; evs && j < cnt && j < 256; j++) {
            uint64_t e = evs + 0x48ull * (j < bound ? j : 0), d = coop_rd_ptr(e + 0x28);
            uint32_t kind = 0;
            if (d) coop_rd(d + 0x2c, &kind, 4);
            else { uint64_t d2 = coop_rd_ptr(e + 0x20); if (d2) coop_rd(d2 + 4, &kind, 4); }
            uint8_t on = 0;
            if (kind == BLUR_EVENT_KIND && coop_rd(e, &on, 1) && on) { *node_out = nd, *ev_out = e; return 1; }
        }
    }
    return 0;
}
void coop_blur_pack(Cpu *c, uint64_t me, NetState *st) {
    static uint64_t last_ev;
    static float last_frame;
    static uint16_t serial;
    st->blur_on = 0, st->blur_hand = 0, st->blur_frame = st->blur_start = st->blur_param = 0, st->blur_serial = serial;
    uint64_t nd = 0, ev = 0, p;
    if (!blur_on() || !blur_event_find(anim_graph_of(me), &nd, &ev) || !(p = coop_rd_ptr(ev + 0x10))) { last_ev = 0; return; }
    float fr = 0, prm = 0, first = 0;
    coop_rd(p + 8, &fr, 4), coop_rd(p + 0xc, &prm, 4);
    uint64_t d2 = coop_rd_ptr(ev + 0x20), d3 = coop_rd_ptr(ev + 0x30);
    if (d2) coop_rd(d2 + 0x14, &first, 4);   // (main+0x293ac18 read here: it returns in s0)
    else if (d3) coop_rd(d3 + 0x10, &first, 4);
    if (ev != last_ev || fr < last_frame) serial++;   // another event, or the same one again from its start
    last_ev = ev, last_frame = fr;
    st->blur_on = 1, st->blur_serial = serial, st->blur_frame = fr, st->blur_start = first, st->blur_param = prm;
    st->blur_hand = (uint8_t)coop_guest_call(c, MOD_MAIN_BASE + BLUR_HAND, nd, ev, 0, 0);
    static int said;
    if (said++ < 5) rt_log("coop: sword trail: this Link's WeaponBlur event (hand %u, frame %.1f from %.1f, parameter %.1f) goes to the others",
                           st->blur_hand, fr, first, prm);
}
// The owner's event as last received, per stand-in (written by the co-op tick, read by the update's hook on the
// actor's own thread: seq is odd while it is written).
typedef struct { uint8_t hand; uint16_t serial; float frame, start, param; } BlurEv;
static struct { _Atomic uint64_t actor; _Atomic uint32_t seq; BlurEv ev; uint64_t until_ns; } blur_live[COOP_MAX_PLAYERS];
void coop_blur_apply(uint64_t standin, const NetState *st, uint64_t age_ns) {
    if (!blur_on() || !standin) return;
    int k = -1, free = -1;
    for (int j = 0; j < COOP_MAX_PLAYERS && k < 0; j++) {
        uint64_t a = atomic_load(&blur_live[j].actor);
        if (a == standin) k = j;
        else if (!a && free < 0) free = j;
    }
    static int said_on;
    if (st->blur_on && said_on++ < 3) rt_log("coop: stand-in: its owner's WeaponBlur event came (age %.0f ms)", age_ns / 1e6);
    if (k < 0 && !st->blur_on) return;
    if (k < 0) { if (free < 0) return; k = free; }
    atomic_fetch_add(&blur_live[k].seq, 1);
    if (st->blur_on && age_ns < 250000000ull) {
        BlurEv *b = &blur_live[k].ev;
        // The frame never goes back within one event: the update starts a new trail when it is below +0xdc.
        float fr = st->blur_frame;
        if (b->serial == st->blur_serial && blur_live[k].until_ns && fr < b->frame) fr = b->frame;
        b->hand = st->blur_hand, b->serial = st->blur_serial, b->frame = fr, b->start = st->blur_start, b->param = st->blur_param;
        blur_live[k].until_ns = plat_now_ns() + 100000000ull;   // (held 100 ms past the last state that had it)
    } else blur_live[k].until_ns = 0;
    atomic_store(&blur_live[k].actor, standin);
    atomic_fetch_add(&blur_live[k].seq, 1);
}
// The made-up graph, per stand-in and in two sets chosen by the event's number, so that a new event has another
// address (the update tells events apart by their address, kept at +0xe0).
enum { BLUR_G = 0x400 };
typedef struct {
    uint8_t g[BLUR_G];     // a copy of the stand-in's graph: +0x18 one layer, +0x20 its record
    uint64_t rec[9];       // the layer's record (0x48 bytes): the layer
    uint8_t layer[0x100];  // +0x60 the node, +0xd0 its weight
    uint8_t node[0xa8];    // +0x20 the bound, +0x28 the event, +0xa0 one event
    uint8_t ev[0x48];      // +0 on, +0x10 its frames, +0x20 0, +0x28 its kind's record, +0x30 its hands' record
    uint8_t frames[0x10];  // +8 the frame, +0xc the parameter
    uint8_t kind[0x30];    // +0x2c the kind
    uint8_t hands[0x18];   // +0 its entries, +8 one, +0xc an entry's size, +0x10 the first frame
    uint64_t entries[2];   // the entries: each the address of a record whose +8 is a hand
    uint8_t hand[0x10];    // +8 the hand
} BlurFake;
static BlurFake blur_fake[COOP_MAX_PLAYERS][2] __attribute__((aligned(16)));
int coop_blur_hook(Cpu *c, uint64_t addr) {
    static __thread int inside;
    if (inside || !blur_on()) return 0;
    uint64_t actor = coop_rd_ptr(c->x[0] + 0x18);
    int k = -1;
    for (int j = 0; actor && j < COOP_MAX_PLAYERS && k < 0; j++) if (atomic_load(&blur_live[j].actor) == actor) k = j;
    if (k < 0) return 0;
    BlurEv b;
    uint64_t until;
    uint32_t s0, s1;
    int tries = 0;
    do {
        s0 = atomic_load(&blur_live[k].seq);
        b = blur_live[k].ev, until = blur_live[k].until_ns;
        s1 = atomic_load(&blur_live[k].seq);
    } while ((s0 != s1 || (s0 & 1)) && ++tries < 100);
    {   // Test (TOTK_COOP_TEST_BLURWATCH): the stand-in's updates, every 2 s: how many, how many with its owner's event
        static int on = -1;
        static _Atomic uint32_t calls, with;
        static _Atomic uint64_t t0;
        if (on < 0) on = cfg_get("TOTK_COOP_TEST_BLURWATCH") != NULL;
        if (on) {
            uint64_t now = plat_now_ns(), was = atomic_load(&t0);
            atomic_fetch_add(&calls, 1);
            if (s0 == s1 && !(s0 & 1) && until && now <= until) atomic_fetch_add(&with, 1);
            static _Atomic int said_u;
            if (until && atomic_fetch_add(&said_u, 1) < 20)
                rt_log("coop: test: blur: update with a held event %.0f ms %s its end (seq %u %u)", fabs((double)(int64_t)(until - now)) / 1e6, now <= until ? "before" : "after", s0, s1);
            if (!was) atomic_store(&t0, now);
            else if (now - was > 2000000000ull && atomic_compare_exchange_strong(&t0, &was, now))
                rt_log("coop: test: blur: %u stand-in SwordBlur updates in 2 s, %u with its owner's event", atomic_exchange(&calls, 0), atomic_exchange(&with, 0));
        }
    }
    if (s0 != s1 || (s0 & 1) || !until || plat_now_ns() > until) return 0;
    uint64_t c8 = coop_rd_ptr(coop_rd_ptr(actor + 0x228) + 0x40), x = c8 ? coop_rd_ptr(c8 + 0x18) : 0, g = x ? coop_rd_ptr(x + 0x18) : 0;
    BlurFake *f = &blur_fake[k][b.serial & 1];
    if (!g || !coop_rd(g, f->g, BLUR_G)) return 0;
    memset((uint8_t *)f + BLUR_G, 0, sizeof *f - BLUR_G);
    int32_t one = 1, size = 8;
    float w = 1;
    uint32_t kind = BLUR_EVENT_KIND, hand = b.hand;
    uint64_t prec = (uint64_t)(uintptr_t)f->rec, pl = (uint64_t)(uintptr_t)f->layer, pn = (uint64_t)(uintptr_t)f->node;
    uint64_t pe = (uint64_t)(uintptr_t)f->ev, pf = (uint64_t)(uintptr_t)f->frames, pk = (uint64_t)(uintptr_t)f->kind;
    uint64_t phs = (uint64_t)(uintptr_t)f->hands, pes = (uint64_t)(uintptr_t)f->entries;
    memcpy(f->g + 0x18, &one, 4), memcpy(f->g + 0x20, &prec, 8);
    f->rec[0] = pl;
    memcpy(f->layer + 0x60, &pn, 8), memcpy(f->layer + 0xd0, &w, 4);
    memcpy(f->node + 0x20, &one, 4), memcpy(f->node + 0x28, &pe, 8), memcpy(f->node + 0xa0, &one, 4);
    f->ev[0] = 1;
    memcpy(f->ev + 0x10, &pf, 8), memcpy(f->ev + 0x28, &pk, 8), memcpy(f->ev + 0x30, &phs, 8);
    memcpy(f->frames + 8, &b.frame, 4), memcpy(f->frames + 0xc, &b.param, 4);
    memcpy(f->kind + 0x2c, &kind, 4);
    f->entries[0] = (uint64_t)(uintptr_t)f->hand;
    memcpy(f->hands, &pes, 8), memcpy(f->hands + 8, &one, 4), memcpy(f->hands + 0xc, &size, 4), memcpy(f->hands + 0x10, &b.start, 4);
    memcpy(f->hand + 8, &hand, 4);
    // The made-up graph in the stand-in's own one's place for this one call (a copy of it, so anything else reading
    // it meanwhile reads the same but for its layers), then its own back.
    ST64(x + 0x18, (uint64_t)(uintptr_t)f->g);
    inside = 1;
    c->pc = addr;
    rt_dispatch(c);
    inside = 0;
    ST64(x + 0x18, g);
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 5)
        rt_log("coop: stand-in: its SwordBlur update ran with its owner's WeaponBlur event (hand %u, frame %.1f from %.1f, event %u)",
               b.hand, b.frame, b.start, b.serial);
    return 1;
}

// Test (TOTK_COOP_TEST_EQCMP=<s>[,<s2>...]): at those world seconds, this Link's EquipmentUser ([+0x228]+0x230) and each
// stand-in's side by side: the weapon slot's record (+0x20, 0x18 bytes), its attach record (+0x570, 0x100 bytes) and
// the in-hand bytes (+0x4620, 8), the words that differ (to find why a stand-in's sword "in hand" stays on its back).
void coop_eqcmp_test(uint64_t me) {
    static double at[8];
    static int n = -1, next;
    if (n < 0) {
        n = 0;
        const char *e = cfg_get("TOTK_COOP_TEST_EQCMP");
        for (const char *p = e; p && *p && n < 8; ) { at[n++] = atof(p); p = strchr(p, ','); if (p) p++; }
    }
    if (next >= n || frame_step_world_us() / 1e6 < at[next]) return;
    next++;
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    uint64_t e0 = coop_rd_ptr(coop_rd_ptr(me + 0x228) + 0x230);
    static const struct { unsigned off, len; } part[] = {{0x20, 0x18}, {0x570, 0x100}, {0x4620, 8}, {0x4600, 0x20}};
    for (int w = 0; w < nc; w++) {
        uint64_t e1 = coop_rd_ptr(coop_rd_ptr(co[w] + 0x228) + 0x230);
        if (!e0 || !e1) continue;
        for (unsigned p = 0; p < sizeof part / sizeof part[0]; p++) {
            uint32_t a[64], b[64];
            unsigned nw = part[p].len / 4;
            if (!coop_rd(e0 + part[p].off, a, part[p].len) || !coop_rd(e1 + part[p].off, b, part[p].len)) continue;
            char line[1500];
            int m = snprintf(line, sizeof line, "coop: test: eqcmp %.1f s +%#x (this Link / stand-in):", frame_step_world_us() / 1e6, part[p].off);
            for (unsigned i = 0; i < nw && m < 1400; i++)
                if (a[i] != b[i]) m += snprintf(line + m, sizeof line - m, " +%#x %x/%x", part[p].off + i * 4, a[i], b[i]);
            rt_log("%s", line);
        }
        // The two weapons (row 0's reference +0x40): each component of their +0x228 tables, 0x400 bytes, the words
        // that differ where neither is an address
        uint64_t w0 = coop_rd_ptr(coop_rd_ptr(e0 + 0x28) + 0x40), w1 = coop_rd_ptr(coop_rd_ptr(e1 + 0x28) + 0x40);
        uint64_t t0 = w0 ? coop_rd_ptr(w0 + 0x228) : 0, t1 = w1 ? coop_rd_ptr(w1 + 0x228) : 0;
        for (int k = -1; t0 && t1 && k < 128; k++) {
            uint64_t c0 = k < 0 ? w0 : coop_rd_ptr(t0 + 8ull * k), c1 = k < 0 ? w1 : coop_rd_ptr(t1 + 8ull * k);
            static uint32_t a[0x400 / 4], b[0x400 / 4];
            if (!c0 || !c1 || !coop_rd(c0, a, sizeof a) || !coop_rd(c1, b, sizeof b)) continue;
            char line[1500];
            int m = snprintf(line, sizeof line, "coop: test: eqcmp weapon %s+%#x (vt main+%#llx):", k < 0 ? "actor" : "comp ", k < 0 ? 0 : k * 8,
                             (unsigned long long)(coop_rd_ptr(c0) - MOD_MAIN_BASE)), any = 0;
            for (unsigned i = 0; i < 0x400 / 4 && m < 1400; i++) {
                uint32_t hi0 = i & 1 ? a[i] : a[i + 1 < 0x100 ? i + 1 : i], hi1 = i & 1 ? b[i] : b[i + 1 < 0x100 ? i + 1 : i];
                int ptr = (hi0 >= 1 && hi0 < 0x80) || (hi1 >= 1 && hi1 < 0x80);   // (the high half of an address)
                if (a[i] != b[i] && !ptr) m += snprintf(line + m, sizeof line - m, " +%#x %x/%x", i * 4, a[i], b[i]), any = 1;
            }
            if (any) rt_log("%s", line);
        }
    }
}
// Test (TOTK_COOP_TEST_WEAPLOG=1): twice a second, this Link's and each stand-in's weapon (EquipmentUser row 0's
// reference +0x40): its actor's place, its model's world matrix place (model +0x1f8) and its holder's place.
void coop_weaplog_test(uint64_t me) {
    static int on = -1;
    static uint64_t last;
    if (on < 0) on = cfg_get("TOTK_COOP_TEST_WEAPLOG") != NULL;
    uint64_t now = plat_now_ns();
    if (!on || now - last < 500000000ull) return;
    last = now;
    uint64_t co[COOP_MAX_PLAYERS];
    int ids[COOP_MAX_PLAYERS], nc = coop_player_actors(co, ids, COOP_MAX_PLAYERS);
    for (int w = 0; w <= nc; w++) {
        uint64_t a = w ? co[w - 1] : me, eq = coop_rd_ptr(coop_rd_ptr(a + 0x228) + 0x230);
        uint64_t wp = eq ? coop_rd_ptr(coop_rd_ptr(eq + 0x28) + 0x40) : 0;
        uint64_t model = wp ? coop_rd_ptr(coop_rd_ptr(coop_rd_ptr(wp + 0x228) + 0x8) + 0x28) : 0;
        float ap[3] = {0}, hp[3] = {0}, m[12] = {0};
        if (!wp) continue;
        coop_rd(wp + COOP_ACTOR_POS, ap, 12), coop_rd(a + COOP_ACTOR_POS, hp, 12);
        if (model) coop_rd(model + 0x1f8, m, sizeof m);
        uint8_t dirty = model ? LD8(model + 0x240) : 0;
        rt_log("coop: test: weap %s %.2f s: actor %.2f %.2f %.2f, model %.2f %.2f %.2f (flags %02x), holder %.2f %.2f %.2f",
               w ? "stand-in" : "this Link", frame_step_world_us() / 1e6, ap[0], ap[1], ap[2], m[3], m[7], m[11], dirty, hp[0], hp[1], hp[2]);
    }
}
