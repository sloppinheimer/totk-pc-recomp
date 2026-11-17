// The stand-in's animation, the masks of what it holds, and its sword trail: moved out of coop_world.c as it was
// (docs/coop/enemy-sync.md B.28: the world file is taken apart so the older enemy sync in it can go). Nothing here
// is the enemy sync's.
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_sess.h"
#include "coop_far.h"
#include "coop_game.h"
#include "coop_net.h"
#include "coop_notices.h"
#include "coop_world_int.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

static uint16_t anim_bind_off(uint64_t g, uint64_t b);
static uint64_t anim_graph_of(uint64_t a);

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
// The offsets a mask may be named by: the three layers facts/player.md lists and every one anim_bind_off found on this
// game's own Link (bit off/8). A peer's offset (NetState.anim[].bind, .mask[].off) is used only when it is one of
// them: any other offset made this game read through, and write into, whatever object it reached (code review,
// 2026-11-17).
static _Atomic uint64_t bind_ok[4] = {(1ull << (0x1a8 / 8 % 64)) | (1ull << (0x1f0 / 8 % 64)), 0, 0, 0};
static int bind_allowed(uint16_t off) {
    if (!off || off >= 0x800 || (off & 7)) return 0;
    if (off == 0x2c8) return 1;
    unsigned j = off / 8;
    return (atomic_load_explicit(&bind_ok[j / 64], memory_order_relaxed) >> (j % 64)) & 1;
}
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
    if (off) atomic_fetch_or_explicit(&bind_ok[off / 8 / 64], 1ull << (off / 8 % 64), memory_order_relaxed);
    return off;
}
// A layer object of the kind the known mask layers are (its first word, the class's table, equals one of theirs):
// a mask the other game's Link uses and this one has not used yet is still taken.
static int layer_kind_ok(uint64_t g, uint64_t layer) {
    uint64_t vt = coop_rd_ptr(layer);
    if (!vt) return 0;
    static const uint16_t known[] = {0x1a8, 0x1f0, 0x2c8};
    for (int i = 0; i < 3; i++) {
        uint64_t l = coop_rd_ptr(g + known[i]);
        if (l && coop_rd_ptr(l) == vt) return 1;
    }
    return 0;
}
static uint64_t anim_bind_at(uint64_t g, uint16_t off) {
    if (!g || !off || off >= 0x800 || (off & 7)) return 0;
    uint64_t layer = coop_rd_ptr(g + off);
    if (layer && !bind_allowed(off) && !layer_kind_ok(g, layer)) layer = 0;
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
    if (!coop_debug()) return;
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

