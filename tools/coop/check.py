"""The mismatch checker (docs/coop/PLAN.md step 1): what two co-op games on this PC show of the enemies near the Links.

    python tools/coop/check.py build/v143 build/v143b [--all]

Reads each game's logs/check.jsonl (runtime/coop_check.c) and logs/totk.log. Both games stamp with the PC's
performance counter, so their lines pair by time.

Once a second, every actor either game shows within 120 m of a Link, paired by identity (placement hash, or the spawn
sync's key for one made at run time). A difference counts when it holds in two paired seconds in a row:
  missing     shown in one game only, within 60 m of the other game's own Link (farther, the other game may not
              have that part of the world loaded: listed as "far", not counted)
  unpaired    an actor neither placed nor synced (named by its address): each game has its own, nothing pairs them
  owner       two games run it for all ("o" in both), both show it as a copy, or no game runs it for all while one
              runs it as its own, unsynced ("l"). An enemy no game runs ("r", its own and resting: parked by the
              game's battle LOD manager, or both Links far) is not one: the other checks of the pair still run
              (research R30 2). A record from before the "r" role ("v" under 2) cannot tell "l" from "r": a pair
              "l" "l" there is counted apart and not listed.
  dup         two actors of one game with one identity
  pos         place: more than 0.75 m plus a quarter second of the owner's speed apart
  rot         rotation: more than 25 degrees apart
  life        life points
  dead        actor state alive / going away, or the death word
  ragdoll     ragdoll state
  drawn       the model's hidden mask or the actor's drawn bit
  anim        a slot's command
  equip       a slot's held thing, or drawn / put away
Nothing is compared in the first 10 s after both games are in the world.

Every frame, for actors within 40 m of a Link, the copy against its owner (the game that runs it; "r" in the record):
  lag         the delay that best lines the copy's path up with the owner's (only while the owner moves)
  error       the copy's distance from the owner at that delay: p50, p95, max
  stutter     the copy's frame-to-frame acceleration (RMS) over the owner's
  command     the share of the copy's frames playing the owner's command (first slot) at that delay
  phase       how far the copy's clock of that command is from the owner's, in seconds (each command's clock rate
              and loop length are taken from the owner's own frames)
  armed       attacks armed (main+0x1bf20cc, by the clip or an AI node): of the owner's armed moments, the share the
              copy had one within 50 ms; of the copy's armed frames, the share the owner had none
The owner is taken per second, so an enemy handed over is compared the right way round on each side of the change.

Events, each game's own, matched with the other game's within 0.3 s before to 0.8 s after:
  effects     effects and sounds an enemy starts by key (ELink main+0xa98cf8, SLink main+0xa98d00): of the owner's, the
              share the copy's game made too (while it showed the enemy near a Link), and the copy's the owner had not
  blows       blows between an enemy and a Link, from the contacts the game builds hits from. An enemy copy's blow on
              a Link must come while its owner had an attack armed. A Link's blow on an enemy is judged in its
              player's own game, the only one where it counts (coop_contact.c contacts_filter takes every stand-in's
              contact on an enemy out); the other game's stand-in contact is listed, found or not and with its
              outcome, as information (research R31 1). Life, compared once a second, shows whether each blow
              reached both games. Where the record has the stand-ins' per-frame line, a missing contact is listed
              with whether that stand-in's attack was on and how near its weapon came to the enemy.

A planted desync (TOTK_COOP_TEST_NUDGE, in either game's log) must show as "pos" on its enemy during its window;
--expect-nudge (run_pair.ps1 -Nudge) fails the run when no game logged one.

Targets (PLAN.md step 1) are checked and printed. Exit 1 when any is missed, a FATAL line is in a log, a record has
lines that cannot be read (beyond a last one cut short), or nothing was compared; exit 2 (not judged) when nothing
failed but no copy moved enough to measure lag and stutter. --all prints every occurrence of each difference.
"""
import bisect
import json
import math
import os
import re
import sys
from collections import defaultdict

TARGET = dict(lag_ms=200, err_p95=0.3, stutter=1.5, cmd_share=0.95, phase_p95=0.1, arm_found=0.95, arm_extra=0.05,
              fx_found=0.95, fx_extra=0.05)
MATCH_S = 0.8   # an event of the other game counts as the same one when it comes this long after (or 0.3 s before)
MISS_NEAR = 60.0
SETTLE = 10.0


def load(d):
    path = os.path.join(d, 'logs', 'check.jsonl')
    snaps, frames, events, bad, lost = [], [], [], 0, 0
    if not os.path.exists(path):
        return snaps, frames, path, 0, events, 0
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    for k, line in enumerate(lines):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            bad += k != len(lines) - 1   # (the last one may be cut short when the game was closed)
            continue
        kind = r.get('k')
        if kind == 's':
            snaps.append(r)
        elif kind == 'f':
            frames.append(r)
        elif kind == 'e':
            events.append(r)
        elif kind == 'lost':
            lost = r['n']
    return snaps, frames, path, bad, events, lost


def log_lines(d):
    p = os.path.join(d, 'logs', 'totk.log')
    if not os.path.exists(p):
        return []
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.readlines()


NUDGE = re.compile(r'coop: check: test: nudge: (\S+) ([0-9a-f]{16}) put ([\d.]+) m east from ([\d.]+) s for ([\d.]+) s')


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def hdist(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def qangle(a, b):
    d = abs(sum(x * y for x, y in zip(a, b)))
    return math.degrees(2 * math.acos(min(1.0, d)))


def alive(st):
    return 2 <= st <= 6


def pct(xs, p):
    if not xs:
        return float('nan')
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * (len(xs) - 1) + 0.5))]


# ---- once a second ---------------------------------------------------------------------------------------------------

def compare_snaps(SA, SB, names):
    """{(kind, id): [(t, detail)]} of differences that held two paired seconds in a row; plus 'far' notes and counts"""
    bysec = [{int(s['t']): s for s in S} for S in (SA, SB)]
    secs = sorted(set(bysec[0]) & set(bysec[1]))
    start = None
    for s in secs:
        if bysec[0][s].get('me') and bysec[1][s].get('me'):
            start = s + SETTLE
            break
    seen = defaultdict(list)     # (kind, id) -> [(sec, detail)] every second it differed
    far = defaultdict(list)
    rest_old = []                # (id, sec): an older record's "l" in both games

    def newrec(snap):            # both seconds' lines have the "r" role (record version 2)
        return all(x.get('v', 1) >= 2 for x in snap)
    compared = 0
    prev, prev_s = [{}, {}], None
    for s in secs:
        if start is None or s < start:
            continue
        snap = [bysec[0][s], bysec[1][s]]
        acts = [{}, {}]
        for k in (0, 1):
            for a in snap[k]['a']:
                if a['id'] in acts[k]:
                    seen[('dup', a['id'])].append((s, f"two in {'AB'[k]}"))
                acts[k][a['id']] = a
        own_link = [sn['links'][0] if sn['links'] else None for sn in snap]
        for k in (0, 1):
            for i, a in acts[k].items():
                names[i] = a['n']
        for i in set(acts[0]) | set(acts[1]):
            a, b = acts[0].get(i), acts[1].get(i)
            if i.startswith('a'):
                x = a or b
                seen[('unpaired', i)].append((s, f"{'A' if a else 'B'} only, {x['n']}"))
                continue
            if not a or not b:
                x, other = (a, 1) if a else (b, 0)
                lk = own_link[other]
                if lk and hdist(x['p'], lk) <= MISS_NEAR:
                    seen[('missing', i)].append((s, f"in {'A' if a else 'B'} only, {hdist(x['p'], lk):.0f} m from {'B' if a else 'A'}'s Link"))
                else:
                    far[i].append(s)
                continue
            compared += 1
            roles = a['r'] + b['r']
            no = roles.count('o')
            if not newrec(snap) and roles == 'll':
                rest_old.append((i, s))   # (an older record: resting or both running it unsynced, not told apart)
            elif no >= 2 or roles == 'cc' or (no == 0 and 'l' in roles):
                seen[('owner', i)].append((s, f"roles A {a['r']} B {b['r']}"))
            own, cop = (a, b) if a['r'] == 'o' else (b, a)
            pv = prev[0 if own is a else 1].get(i) if prev_s == s - 1 else None
            sp = dist(own['p'], pv['p']) if pv else 0.0   # (metres in the last second: the owner's speed, roughly)
            d = dist(a['p'], b['p'])
            if d > 0.75 + 0.25 * sp:
                seen[('pos', i)].append((s, f'{d:.2f} m apart (owner moving {sp:.1f} m/s)'))
            ang = qangle(a['q'], b['q'])
            if ang > 25:
                seen[('rot', i)].append((s, f'{ang:.0f} degrees apart'))
            if a['hp'] != b['hp']:
                seen[('life', i)].append((s, f"A {a['hp']} B {b['hp']}"))
            if alive(a['st']) != alive(b['st']) or a['ds'] != b['ds']:
                seen[('dead', i)].append((s, f"A state {a['st']} death {a['ds']:#x}, B state {b['st']} death {b['ds']:#x}"))
            if a['rag'] != b['rag']:
                seen[('ragdoll', i)].append((s, f"A {a['rag']} B {b['rag']}"))
            if a['hide'] != b['hide'] or a['vis'] != b['vis']:
                seen[('drawn', i)].append((s, f"A mask {a['hide']:#x} bit {a['vis']}, B mask {b['hide']:#x} bit {b['vis']}"))
            for k in range(max(len(a['anim']), len(b['anim']))):
                x = a['anim'][k][0] if k < len(a['anim']) else ''
                y = b['anim'][k][0] if k < len(b['anim']) else ''
                if x != y:
                    seen[('anim', i)].append((s, f'slot {k}: A {x or "-"} B {y or "-"}'))
            for k in range(min(len(a['eq']), len(b['eq']))):
                if a['eq'][k] != b['eq'][k]:
                    seen[('equip', i)].append((s, f"slot {k}: A {a['eq'][k][0] or '-'} ({a['eq'][k][1]}) B {b['eq'][k][0] or '-'} ({b['eq'][k][1]})"))
        prev, prev_s = acts, s
    held = {}
    for key, occ in seen.items():
        secs_set = {s for s, _ in occ}
        keep = [(s, d) for s, d in occ if s - 1 in secs_set or s + 1 in secs_set]
        if keep:
            held[key] = keep
    return held, far, compared, (secs[0] if secs else None), start, rest_old


# ---- every frame -----------------------------------------------------------------------------------------------------

def series(frames):
    out = defaultdict(list)   # id -> [(t, pos, quat, cmd, clock)]
    for r in frames:
        for a in r['a']:
            out[a[0]].append((r['t'], a[1:4], a[4:8], a[8], a[9], a[10] if len(a) > 10 else None))
    return out


def stand_ins(frames):
    """per stand-in id: [(t, its weapon's attack byte, its weapon's place or None)], from the per-frame lines' 'sl'"""
    out = defaultdict(list)
    for r in frames:
        for x in r.get('sl', []):
            out[x[0]].append((r['t'], x[1], x[2:5] if len(x) >= 5 else None))
    return out


def at(ser, ts, t):
    """the owner's place at t (linear between its frames), its command and clock (from the frame before), or None"""
    j = bisect.bisect_right(ts, t)
    if j == 0 or j >= len(ser):
        return None
    a, b = ser[j - 1], ser[j]
    if b[0] - a[0] > 0.25:
        return None   # a gap in its frames
    u = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0
    p = [x + (y - x) * u for x, y in zip(a[1], b[1])]
    clock = a[4] + (b[4] - a[4]) * u if a[3] == b[3] else a[4]
    return p, a[3], clock


def motion(own, cop, skip):
    """the copy's motion against its owner's; skip: time windows (the planted desync) left out"""
    own.sort(key=lambda r: r[0])
    cop.sort(key=lambda r: r[0])
    ts = [r[0] for r in own]
    cop = [r for r in cop if not any(t0 <= r[0] <= t1 for t0, t1 in skip)]
    moving = set()
    for k in range(1, len(own)):
        dt = own[k][0] - own[k - 1][0]
        if 0 < dt < 0.25 and dist(own[k][1], own[k - 1][1]) / dt > 0.5:
            moving.add(k)
    moved_s = sum(own[k][0] - own[k - 1][0] for k in moving)
    res = dict(frames=len(cop), moved_s=moved_s)
    if not cop or len(own) < 2:
        return res
    best = None
    if moved_s >= 2.0:
        for lag_ms in range(0, 601, 10):
            lag = lag_ms / 1000
            errs = []
            for r in cop:
                j = bisect.bisect_right(ts, r[0] - lag)   # the owner between frames j-1 and j, moving there
                if 0 < j < len(ts) and j in moving:
                    o = at(own, ts, r[0] - lag)
                    if o:
                        errs.append(dist(r[1], o[0]))
            if len(errs) > 20 and (best is None or sum(errs) / len(errs) < best[1]):
                best = (lag_ms, sum(errs) / len(errs))
    lag = best[0] / 1000 if best else 0.0
    res['lag_ms'] = best[0] if best else None
    errs, same, both, phase = [], 0, 0, []
    for r in cop:
        o = at(own, ts, r[0] - lag)
        if not o:
            continue
        errs.append(dist(r[1], o[0]))
        if r[3] != '00000000' or o[1] != '00000000':
            both += 1
            if r[3] == o[1]:
                same += 1
                phase.append((r[3], r[4] - o[2]))
    res.update(err_p50=pct(errs, 0.5), err_p95=pct(errs, 0.95), err_max=max(errs) if errs else float('nan'),
               cmd_share=same / both if both else None)
    # attacks armed: of the copy's frames whose moment the owner had an attack armed (at the lag), the share where the
    # copy had one within 50 ms; of the copy's armed frames, the share the owner had none within 50 ms of
    if cop[0][5] is not None and own[0][5] is not None:
        cts = [r[0] for r in cop]
        # (with no lag measured, a copy standing still: a nominal 100 ms and a window of 150 ms each way)
        alag, win = (lag, 0.05) if best else (0.1, 0.15)

        def armed_near(ser, tss, t):
            j0, j1 = bisect.bisect_left(tss, t - win), bisect.bisect_right(tss, t + win)
            return any(ser[j][5] for j in range(j0, j1))
        need = found = carmed = extra = 0
        for r in cop:
            j = bisect.bisect_right(ts, r[0] - alag)
            if 0 < j < len(ts) and own[j - 1][5]:
                need += 1
                found += armed_near(cop, cts, r[0])
            if r[5]:
                carmed += 1
                extra += not armed_near(own, ts, r[0] - alag)
        res.update(arm_need=need, arm_found=found / need if need else None, arm_extra=extra / carmed if carmed else None)
    rates, top = defaultdict(list), defaultdict(float)   # per command: the owner's clock per second, its highest clock
    for k in range(1, len(own)):
        dt = own[k][0] - own[k - 1][0]
        top[own[k][3]] = max(top[own[k][3]], own[k][4])
        if own[k][3] == own[k - 1][3] and own[k][3] != '00000000' and 0 < dt < 0.1 and own[k][4] > own[k - 1][4]:
            rates[own[k][3]].append((own[k][4] - own[k - 1][4]) / dt)
    ph = []
    for cmd, d in phase:
        rate = pct(rates[cmd], 0.5) if rates.get(cmd) else None
        if not rate:
            continue
        if top[cmd] > 0 and abs(d) > top[cmd] / 2:   # one side has looped round already
            d -= math.copysign(top[cmd], d)
        ph.append(abs(d) / rate)
    res['clock_rate'] = pct([pct(v, 0.5) for v in rates.values()], 0.5) if rates else None
    res['phase_p95'] = pct(ph, 0.95) if ph else None

    def jerk(ser):
        acc = []
        for k in range(2, len(ser)):
            t0, t1, t2 = ser[k - 2][0], ser[k - 1][0], ser[k][0]
            if not (0 < t1 - t0 < 0.1 and 0 < t2 - t1 < 0.1):
                continue
            v1 = [(b - a) / (t1 - t0) for a, b in zip(ser[k - 2][1], ser[k - 1][1])]
            v2 = [(b - a) / (t2 - t1) for a, b in zip(ser[k - 1][1], ser[k][1])]
            acc.append(dist(v1, v2) / ((t2 - t0) / 2))
        return math.sqrt(sum(x * x for x in acc) / len(acc)) if acc else None
    jo, jc = jerk(own), jerk(cop)
    res['stutter'] = jc / jo if jo and jc is not None and moved_s >= 2.0 else None
    return res


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    (SA, FA, pa, bada, EA, losta), (SB, FB, pb, badb, EB, lostb) = load(args[0]), load(args[1])
    fail = []
    for p, n in ((pa, losta), (pb, lostb)):
        if n:
            fail.append(f'{n} events of {p} were lost (the ring was full): the event lists are not whole')
    for p, nb in ((pa, bada), (pb, badb)):
        if nb:
            fail.append(f'{nb} lines of {p} could not be read (a number written as nan or inf?)')
    for d, S, p in ((args[0], SA, pa), (args[1], SB, pb)):
        if not S:
            fail.append(f'no record in {p}')
    for d, who in ((args[0], 'A'), (args[1], 'B')):
        fat = [l.strip() for l in log_lines(d) if 'FATAL' in l]
        if fat:
            fail.append(f'{who}: FATAL in its log: {fat[0][:160]}')
    if fail and not (SA and SB):
        print('\n'.join(fail))
        return 1

    names = {}
    held, far, compared, first, start, rest_old = compare_snaps(SA, SB, names)
    nudges = []
    for d, who, S in ((args[0], 'A', SA), (args[1], 'B', SB)):
        for l in log_lines(d):
            m = NUDGE.search(l)
            if m:
                w0, ln = float(m.group(4)), float(m.group(5))
                # world seconds to the PC clock, from that game's own seconds lines
                pts = [(s['world'], s['t']) for s in S if s.get('world', -1) >= 0]
                off = min(pts, key=lambda p: abs(p[0] - w0)) if pts else None
                t0 = w0 - off[0] + off[1] if off else None
                nudges.append(dict(game=who, name=m.group(1), key=m.group(2), t0=t0, t1=t0 + ln if t0 else None))

    print(f'Once a second: {compared} actor-seconds compared in both games (from {start} on, PC seconds).')
    if not compared:
        fail.append('nothing compared once a second')
    kinds = defaultdict(list)
    for (kind, i), occ in held.items():
        kinds[kind].append((i, occ))
    order = ['missing', 'unpaired', 'owner', 'dup', 'pos', 'rot', 'life', 'dead', 'ragdoll', 'drawn', 'anim', 'equip']
    for kind in order:
        for i, occ in sorted(kinds.get(kind, []), key=lambda x: x[1][0][0]):
            print(f'  {kind:8} {names.get(i, "?")} {i}: {len(occ)} s from {occ[0][0]}: {occ[0][1]}')
            if show_all:
                for s, dd in occ[1:]:
                    print(f'           {s}: {dd}')
    if not held:
        print('  no difference held two seconds')
    for i, ss in sorted(far.items()):
        print(f'  far      {names.get(i, "?")} {i}: in one game only for {len(ss)} s, beyond {MISS_NEAR:.0f} m of the other\'s Link (not counted)')
    if rest_old:
        ids = sorted({i for i, _ in rest_old})
        print(f'  (an older record: {len(rest_old)} actor-seconds of roles "l" in both games, {len(ids)} actors, not judged as '
              f'"owner": resting in both or run by both unsynced cannot be told apart without the "r" role: '
              + ', '.join(f'{names.get(i, "?")} {i}' for i in ids[:6]) + ')')

    # the planted desync
    skip = defaultdict(list)
    if '--expect-nudge' in sys.argv and not nudges:
        print('Planted desync: asked for, but no game logged one (no enemy copy was shown when it was due).')
        fail.append('the planted desync was asked for and never made')
    for n in nudges:
        ids = {'p' + n['key'], 'r' + n['key']}
        hit = [(i, occ) for (kind, i), occ in held.items() if kind == 'pos' and i in ids]
        inwin = [s for i, occ in hit for s, _ in occ if n['t0'] and n['t0'] - 2 <= s <= n['t1'] + 2]
        where = f"{n['name']} {n['key']} in {n['game']}, PC {n['t0']:.0f} to {n['t1']:.0f} s" if n['t0'] else f"{n['name']} {n['key']} in {n['game']}"
        if inwin:
            print(f'Planted desync {where}: found ({len(inwin)} s of "pos").')
        else:
            print(f'Planted desync {where}: NOT found.')
            fail.append('the planted desync was not found')
        for i in ids:
            if n['t0']:
                skip[i].append((n['t0'] - 1, n['t1'] + 1))

    # every frame
    owner = defaultdict(dict)   # id -> {second: the game that runs it then}
    for S, who in ((SA, 0), (SB, 1)):
        for s in S:
            for a in s['a']:
                if a['r'] == 'o':
                    owner[a['id']][int(s['t'])] = who
    ser = [series(FA), series(FB)]
    print('Every frame (copy against owner):')
    measured = moved = 0
    for i in sorted(owner):
        for who in (0, 1):
            # (both sides over the seconds who ran it, one second more of the owner's on each side for the lag: the
            # owner's frames of other seconds are of the enemy as a copy there, and a jerk taken over the whole run
            # set a calm copy against a violent owner or the reverse; research R27)
            secs = owner[i]
            own = [r for r in ser[who].get(i, []) if any(secs.get(int(r[0]) + d) == who for d in (-1, 0, 1))]
            cop = [r for r in ser[1 - who].get(i, []) if secs.get(int(r[0])) == who]   # (its frames while who ran it)
            if not own or not cop:
                continue
            r = motion(own, cop, skip.get(i, []))
            if not r.get('frames'):
                continue
            measured += 1
            moved += r.get('lag_ms') is not None
            report_motion(i, who, r, names, fail)
    if not measured:
        print('  no enemy within 40 m was run by one game and shown by the other')
    me_ids = []
    for S in (SA, SB):   # (each game's own Link: "L<its net id>", the id most of its seconds' lines carry)
        cnt = defaultdict(int)
        for x in S:
            if x.get('me'):
                cnt[x['me']] += 1
        me_ids.append(max(cnt, key=cnt.get) if cnt else None)
    compare_events(EA, EB, owner, ser, names, fail, me_ids, (stand_ins(FA), stand_ins(FB)))
    # (a hook that never fires records nothing, and nothing would fail: each record must have something in it)
    armed = sum(1 for g in (0, 1) for s in ser[g].values() for r in s if r[5])
    nfx = sum(1 for E in (EA, EB) for e in E if e['w'] in 'xs')
    nblow = sum(1 for E in (EA, EB) for e in E if e['w'] == 'h')
    print(f'Records: {armed} armed frames, {nfx} effects and sounds by key, {nblow} blow contacts (both games).')
    # (effects by key: only AI nodes and a few engine paths start them, so a fight may have none; that measure alone is
    # then not judged, and says so, while the run is judged on the rest)
    empty = [n for n, v in (('attacks armed', armed), ('blows', nblow)) if not v]
    if not nfx:
        print('Effects by key: none started in either game, so not judged (and the record not shown to work).')
    over = max([s.get('over', 0) for s in SA + SB] or [0])
    arms_full = max([s.get('arms_full', 0) for s in SA + SB] or [0])
    if over or arms_full:
        fail.append(f'the record left actors out ({over} over its effect table, {arms_full} arm calls over its arm table)')

    def planted(key):   # the planted desync's own "pos", inside its window
        kind, i = key
        return kind == 'pos' and any(i in ('p' + n['key'], 'r' + n['key']) and n['t0'] and
                                     all(n['t0'] - 2 <= s <= n['t1'] + 2 for s, _ in held[key]) for n in nudges)
    counted = [k for k in held if not planted(k)]
    if counted:
        fail.append(f'{len(counted)} differences held two seconds' + (' (the planted one aside)' if nudges else ''))
    print()
    print('Not checked here: effects and sounds not started by key (a clip\'s own), blows between two enemies or two'
          ' Links, drops, throws, shots, what a player sees on screen (the pictures are for that).')
    if fail:
        print('FAIL:')
        for f_ in fail:
            print(f'  {f_}')
        return 1
    if not moved or empty:
        if not moved:
            print('NOT JUDGED: no enemy copy moved enough (2 s) to measure lag, place error at that lag and stutter.')
        if empty:
            print(f"NOT JUDGED: nothing recorded of {', '.join(empty)} (a fight has all three; a hook that never fires has none).")
        return 2
    print('PASS')
    return 0


def compare_events(EA, EB, owner, ser, names, fail, me_ids, sl):
    """effects and sounds by key: the owner's against the copy's game; blows between an enemy and a Link"""
    E = [EA, EB]
    shown = [{i: [r[0] for r in s] for i, s in ser[g].items()} for g in (0, 1)]

    def near(g, i, t):   # the game shows that enemy within 40 m about then
        tss = shown[g].get(i)
        if not tss:
            return False
        j = bisect.bisect_left(tss, t - 0.5)
        return j < len(tss) and tss[j] <= t + 0.5

    def take(pool, t, test):   # the first unused event of the pool at t - 0.3 .. t + MATCH_S that passes test
        for e in pool:
            if not e.get('used') and t - 0.3 <= e['t'] <= t + MATCH_S and test(e):
                e['used'] = 1
                return e
        return None
    print('Effects and sounds started by key (the owner\'s against the copy\'s game):')
    fx = defaultdict(lambda: [0, 0, 0, 0, [], []])   # (id, owner game) -> made, found, copy's, extra, missing keys, extra keys
    for g in (0, 1):
        for e in E[g]:
            if e['w'] not in 'xs':
                continue
            i, sec = e['a'], int(e['t'])
            ow = owner.get(i, {}).get(sec)
            if ow is None:
                continue
            o = 1 - ow
            if g == ow and near(o, i, e['t']):
                fx[(i, ow)][0] += 1
                pool = [x for x in E[o] if x['w'] == e['w'] and x['a'] == i]
                if take(pool, e['t'], lambda x: x['key'] == e['key']):
                    fx[(i, ow)][1] += 1
                else:
                    fx[(i, ow)][4].append(e['key'])
    for g in (0, 1):   # the copy's game: what it made that the owner had not
        for e in E[g]:
            if e['w'] not in 'xs' or e.get('used'):
                continue
            ow = owner.get(e['a'], {}).get(int(e['t']))
            if ow is None or ow == g:
                continue
            fx[(e['a'], ow)][2] += 1
            fx[(e['a'], ow)][3] += 1
            fx[(e['a'], ow)][5].append(e['key'])
    for g in (0, 1):
        for e in E[g]:
            if e['w'] in 'xs' and e.get('used'):
                ow = owner.get(e['a'], {}).get(int(e['t']))
                if ow is not None:
                    fx[(e['a'], ow)][2] += 1
    if not fx:
        print('  none (no enemy near both Links started one by key)')
    for (i, ow), (made, found, copys, extra, miss, ext) in sorted(fx.items()):
        tag = f"{names.get(i, '?')} {i} (run by {'AB'[ow]})"
        fr = found / made if made else None
        xr = extra / copys if copys else None
        print(f"  {tag}: {made} by the owner, {found} in the copy's game too" +
              (f"; {extra} of the copy's {copys} the owner had not" if copys else '') +
              (f"; missing {sorted(set(miss))[:6]}" if miss else '') + (f"; extra {sorted(set(ext))[:6]}" if ext else ''))
        if made and fr < TARGET['fx_found']:
            fail.append(f"{tag}: effects by key found in the copy's game {fr * 100:.0f} % (under {TARGET['fx_found'] * 100:.0f} %)")
        if copys and xr > TARGET['fx_extra']:
            fail.append(f"{tag}: effects by key the owner had not {xr * 100:.0f} % (over {TARGET['fx_extra'] * 100:.0f} %)")

    # An enemy's blow on a Link is decided in that Link's own game (from what it shows; a stand-in takes no enemy
    # contact), so it is in one game only by design: there it must come while the enemy's owner had an attack armed
    # (within 0.4 s before to 0.1 s after), or the blow is one the owner never made. A Link's blow on an enemy counts
    # in its player's own game alone (coop_contact.c contacts_filter takes every stand-in's contact on an enemy out,
    # owner's or copy's): it is listed there with its outcome, and the other game's stand-in contact as information.
    print('Blows of an enemy on a Link (each in a moment its owner had an attack armed); blows of a Link on an enemy '
          '(judged in its player\'s own game, where it counts):')
    blows = [[], []]
    for g in (0, 1):   # (one blow per swing: a contact seen in more than one hit update is one blow; a pair named by an
        last = {}      # address, this game's alone, cannot be matched)
        for e in E[g]:
            if e['w'] != 'h' or e['a'].startswith('a') or e['by'].startswith('a'):
                continue
            k = (e['a'], e['by'], e['aid'])
            if k in last and e['t'] - last[k] < 1.0:
                continue
            last[k] = e['t']
            blows[g].append(e)
    cls_name = {0: 'hit', 1: 'guarded', 3: 'guarded in time', 4: 'avoided'}
    phantom, backed, unjudged = [], 0, 0
    for g in (0, 1):
        for e in blows[g]:
            if not e['a'].startswith('L'):
                continue
            en, ow = e['by'], owner.get(e['by'], {}).get(int(e['t']))
            if ow is None or ow == g:
                # (in the owner's own game the blow is the enemy's own: nothing to hold it against; and not every blow
                # is armed by main+0x1bf20cc, measured 2026-11-17: some of an owner's own blows came with none armed)
                unjudged += ow is None
                continue
            s_ = ser[ow].get(en, [])
            tss = [r[0] for r in s_]
            j0, j1 = bisect.bisect_left(tss, e['t'] - 0.4), bisect.bisect_right(tss, e['t'] + 0.1)
            if j0 >= j1 or s_[j0][5] is None:
                unjudged += 1   # (the owner's game has no frames of it then, or no arm record)
            elif any(s_[j][5] for j in range(j0, j1)):
                backed += 1
            else:
                phantom.append((g, e, ow))
    print(f"  enemy (a copy) on a Link: {backed} while its owner had an attack armed, {len(phantom)} with none armed there, "
          f'{unjudged} not judged (no owner then); blows in the owner\'s own game are not judged')
    for g, e, ow in phantom[:12]:
        print(f"    in {'AB'[g]} at {e['t']:.1f}: {names.get(e['by'], e['by'])} {e['by']} -> {e['a']}, "
              f"{cls_name.get(e['cls'], e['cls'])}; its owner ({'AB'[ow]}) had no attack armed")
    if phantom:
        fail.append(f'{len(phantom)} blows of an enemy on a Link while its owner had no attack armed')

    def stand_in_then(g, lid, en, t):   # what the record has of that stand-in in game g about t: attack on, nearest weapon
        rows = [r for r in sl[g].get(lid, []) if t - 0.3 <= r[0] <= t + 0.5]
        if not sl[g]:
            return 'no stand-in record'
        if not rows:
            return 'the stand-in not recorded then'
        on = any(r[1] for r in rows)
        es = ser[g].get(en, [])
        ets = [r[0] for r in es]
        near_m = None
        for r in rows:
            if r[2] is None or not es:
                continue
            j = min(max(bisect.bisect_left(ets, r[0]), 0), len(es) - 1)
            d = dist(r[2], es[j][1])
            near_m = d if near_m is None else min(near_m, d)
        return (f"its attack {'on' if on else 'off'}" +
                (f", its weapon within {near_m:.1f} m of the enemy" if near_m is not None else ', no weapon place'))
    counted = defaultdict(int)
    found = differ = 0
    missing, differs, unpaired = [], [], 0
    for g in (0, 1):
        own = f'L{me_ids[g]}' if me_ids[g] is not None else None
        for e in blows[g]:
            if e['a'].startswith('L') or e['by'] != own:
                continue   # (an enemy's blow, or a stand-in's contact: the latter is listed with its striker's blow below)
            counted[e['cls']] += 1
            en = e['a']
            if not near(1 - g, en, e['t']):
                continue   # (the other game does not show that enemy near a Link then)
            m = None
            for x in blows[1 - g]:
                if not x.get('seen') and x['a'] == en and x['by'] == e['by'] and abs(x['t'] - e['t']) <= MATCH_S:
                    m = x
                    break
            if not m:
                missing.append((g, e))
                continue
            m['seen'] = 1
            found += 1
            if m['cls'] != e['cls']:
                differ += 1
                differs.append((g, e, m))
    for g in (0, 1):   # stand-in contacts with no blow of their player in that player's game
        own = f'L{me_ids[g]}' if me_ids[g] is not None else None
        unpaired += sum(1 for x in blows[g] if not x['a'].startswith('L') and x['by'] != own and not x.get('seen'))
    n = sum(counted.values())
    print(f"  Link on an enemy, in its player's own game: {n} blows (" +
          ', '.join(f"{v} {cls_name.get(k, k)}" for k, v in sorted(counted.items())) + ')' if n else
          "  Link on an enemy, in its player's own game: none")
    print(f"  the other game's stand-in contact (not counted, information): {found} found ({differ} with another "
          f"outcome), {len(missing)} none; {unpaired} stand-in contacts with no blow in their player's game")
    for g, e in missing[:12]:
        print(f"    at {e['t']:.1f}: {e['by']} -> {names.get(e['a'], e['a'])} {e['a']}, {cls_name.get(e['cls'], e['cls'])} in "
              f"{'AB'[g]}; no stand-in contact in {'AB'[1 - g]} ({stand_in_then(1 - g, e['by'], e['a'], e['t'])})")
    for g, e, m in differs[:12]:
        print(f"    at {e['t']:.1f}: {e['by']} -> {e['a']}: {cls_name.get(e['cls'], e['cls'])} in {'AB'[g]} (counted), "
              f"{cls_name.get(m['cls'], m['cls'])} by the stand-in in {'AB'[1 - g]} {m['t'] - e['t']:+.2f} s")
    if any(x is None for x in me_ids):
        print("  (a game's own Link id is not in its record: its Links' blows were not judged)")


def report_motion(i, who, r, names, fail):
    tag = f"{names.get(i, '?')} {i} (run by {'AB'[who]})"
    if r.get('lag_ms') is None:
        print(f"  {tag}: it moved {r['moved_s']:.1f} s, too little for lag and stutter; error p95 {r.get('err_p95', float('nan')):.2f} m")
    else:
        print(f"  {tag}: lag {r['lag_ms']} ms, error p50 {r['err_p50']:.2f} p95 {r['err_p95']:.2f} max {r['err_max']:.2f} m, "
              f"stutter {r['stutter']:.2f}x" if r['stutter'] is not None else f"  {tag}: lag {r['lag_ms']} ms, error p95 {r['err_p95']:.2f} m, stutter not measured")
    cs = r.get('cmd_share')
    ph = r.get('phase_p95')
    print(f"      command same {cs * 100:.0f} % of frames" if cs is not None else '      command: none played',
          f", phase p95 {ph:.3f} s (clock {r['clock_rate']:.1f}/s)" if ph is not None else ', phase not measured')
    af, ax = r.get('arm_found'), r.get('arm_extra')
    if af is not None or ax is not None:
        print(f"      attacks armed: {r['arm_need']} owner frames armed, the copy too in {af * 100:.0f} %" if af is not None else '      attacks armed: the owner none',
              f"; the copy armed without the owner {ax * 100:.0f} % of its armed frames" if ax is not None else '')
    for v, ok, msg in ((af, lambda v: v >= TARGET['arm_found'], f"attacks armed with the owner under {TARGET['arm_found'] * 100:.0f} %"),
                       (ax, lambda v: v <= TARGET['arm_extra'], f"attacks armed without the owner over {TARGET['arm_extra'] * 100:.0f} %")):
        if v is not None and not ok(v):
            fail.append(f"{names.get(i, '?')} {i} (run by {'AB'[who]}): {msg}")
    checks = [('lag_ms', r.get('lag_ms'), lambda v: v <= TARGET['lag_ms'], f"lag over {TARGET['lag_ms']} ms"),
              ('err_p95', r.get('err_p95') if r.get('lag_ms') is not None else None, lambda v: v <= TARGET['err_p95'], f"error p95 over {TARGET['err_p95']} m"),
              ('stutter', r.get('stutter'), lambda v: v <= TARGET['stutter'], f"stutter over {TARGET['stutter']}x"),
              ('cmd_share', cs, lambda v: v >= TARGET['cmd_share'], f"command same under {TARGET['cmd_share'] * 100:.0f} %"),
              ('phase_p95', ph, lambda v: v <= TARGET['phase_p95'], f"phase p95 over {TARGET['phase_p95']} s")]
    for _, v, ok, msg in checks:
        if v is not None and not (isinstance(v, float) and math.isnan(v)) and not ok(v):
            fail.append(f'{tag}: {msg}')


if __name__ == '__main__':
    sys.exit(main())
