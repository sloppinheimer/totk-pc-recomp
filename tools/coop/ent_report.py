"""Says whether two co-op games show the same contraption parts, from what each game measured.

    python tools/coop/ent_report.py build/v143 build/v143play

Each folder is a game's (its logs/audit.jsonl and logs/totk.log). The audit has one line a second, written at the
turn of a second of the PC's clock, with every entity of the entity sync (runtime/coop_ent_bench.c, coop_ent_audit):
its id, owner, whether it has an actor, the place and rotation of its main body; and every Zonai device in the world
that is no entity. The lines of the two games are paired by time. Printed, each with PASS or FAIL:

  scenario  the bench's own lines (TOTK_COOP_BENCH): whether the scripted action happened as meant in the game that
            ran it. When it did not, the rest is not a test of it.
  count     every live entity is in both games with an actor (not counted in an entity's first 3 s)
  loose     no Zonai device in either world that the sync does not know
  owner     each entity is owned by exactly one of the two games
  rest      while the owner's object is still (under 0.05 m/s for two lines): the copy within 0.10 m and 5 degrees at
            the 90th percentile, 0.25 m and 10 degrees at most
  moving    while the owner's object moves (over 0.2 m/s): the copy within 0.30 m at the 90th percentile
  bonds     the two games have the same glue bonds (a difference of up to 3 lines in a row is a glue in the making)
  devices   the same devices are switched on in both games
  hand      what a Link holds with Ultrahand, and that the other game has that entity as held by its owner
  speed     from the drive's trace: the velocity an owner sends with a pose against the one its last two poses give;
            the median ratio between 0.7 and 1.4 (a body moved without its stored velocity would send none)
  smooth    from the drive's trace (TOTK_COOP_FOLLOW_TRACE=1), while the owner's object moves: the velocity written to
            the copy changes by at most 1.0 m/s from one frame to the next at the 90th percentile
  settle    how often the drive put a copy in place or held one that kept leaving its rest (told, not judged)

The limits are this script's own, chosen from the first sessions' numbers; they are what "the same" means here until
a play session says otherwise. Exits 1 when a line fails.
"""
import json
import math
import os
import re
import sys
from collections import defaultdict


def load(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass   # a line cut short when the game was closed
    return out


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def angle(ra, rb):
    """degrees between two rotations given as 9 numbers, row by row"""
    tr = sum(ra[i] * rb[i] for i in range(9))
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1) / 2))))


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else 0.0


def live(snap):
    """the live entities that are sent (not a preview: the actors the game's part maker makes for an Autobuild, and
    for the picture of a new build right after a glue, are this game's alone until confirmed)"""
    return {e['id']: e for e in snap.get('ents', []) if e['life'] == 0 and not e['flags'] & 4}


TRACE = re.compile(r"ftrace: (\d+) (\w+) cls (\d) rests \d dt [\d.]+ sample (\d+) .*\| vr ([-\d. ]+) ar .*\| V ([-\d. ]+) W [-\d. ]+ \| vb [-\d.]+ [-\d.]+ [-\d.]+(?: \| vd ([-\d. ]+) sent (\d))?")


def norm(v):
    return math.sqrt(sum(x * x for x in v))


def trace_read(log):
    """per copy: the frame-to-frame change of the written velocity while its owner's object moves (all, and while it
    is held); and, on each frame a new pose arrived with the owner's sent velocity, that velocity against the one the
    last two poses give"""
    steps, held, speed, prev = defaultdict(list), defaultdict(list), [], {}
    if not os.path.exists(log):
        return steps, held, speed
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = TRACE.search(line)
            if not m:
                continue
            now, key, cls, sample = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))
            vr, v = [float(x) for x in m.group(5).split()], [float(x) for x in m.group(6).split()]
            p = prev.get(key)
            if p and now - p[0] < 40000 and norm(vr) > 0.3:
                steps[key].append(dist(v, p[1]))
                if cls:
                    held[key].append(dist(v, p[1]))
            if m.group(7) and m.group(8) == '1' and p and sample != p[2]:
                vd = [float(x) for x in m.group(7).split()]
                if norm(vd) > 0.5:
                    speed.append((norm(vr) / norm(vd), dist(vr, vd), cls))
            prev[key] = (now, v, sample)
    return steps, held, speed


def grep(log, pattern):
    if not os.path.exists(log):
        return []
    rx = re.compile(pattern)
    with open(log, encoding='utf-8', errors='replace') as f:
        return [line.strip() for line in f if rx.search(line)]


def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(dirs) != 2:
        print(__doc__)
        return 2
    A, B = (load(os.path.join(d, 'logs', 'audit.jsonl')) for d in dirs)
    logs = [os.path.join(d, 'logs', 'totk.log') for d in dirs]
    results = []   # (name, ok or None, text)
    # the bench's own word on its scenario
    bench = [l for log in logs for l in grep(log, r'coop: bench: .*(landed|did not land|not ready|is gone)')]
    if bench:
        bad = [l for l in bench if 'did not land' in l or 'not ready' in l or 'is gone' in l]
        results.append(('scenario', not bad, '; '.join(l.split('coop: bench: ')[1] for l in bench)))
    if '--bench' in sys.argv:   # a bench run: both games must have been in the world, and the scene made
        for k in (0, 1):
            if not grep(logs[k], r'coop: bench: .* this Link .* put at'):
                results.append(('scene', False, f'the game of {dirs[k]} never put its Link in place: it did not reach the world, or had no other player (the bench runs with one)'))
        if not grep(logs[0], 'coop: bench: .*scene ready') and not grep(logs[1], 'coop: bench: .*scene ready'):
            results.append(('scene', False, 'the fans were never made'))
    if not A or not B or 'ents' not in A[-1] or 'ents' not in B[-1]:
        for name, ok, text in results:
            print(f'  {"PASS" if ok else "FAIL"}  {name:9s} {text}')
        print(f'no entity audit to compare: {len(A)} lines in {dirs[0]}, {len(B)} in {dirs[1]}')
        return 1
    first_seen, last_pair = {}, {}
    missing, owners = defaultdict(list), defaultdict(list)
    rest_d, rest_a, move_d = [], [], []
    worst_rest, worst_move = (0, ''), (0, '')
    loose = [0, 0]
    loose_run, loose_len = [0, 0], [0, 0]
    loose_names = [set(), set()]
    hand_s, hand_told = [0, 0], [0, 0]
    timeline = defaultdict(list)
    paused_until, paused_n = -1.0, 0
    bond_diff, bond_max = [], 0
    on_diff, on_max = [], 0
    pairs, bi = 0, 0
    prev = None
    t0 = min(A[0]['t'], B[0]['t'])
    for a in A:
        while bi + 1 < len(B) and abs(B[bi + 1]['t'] - a['t']) <= abs(B[bi]['t'] - a['t']):
            bi += 1
        b = B[bi]
        if abs(b['t'] - a['t']) > 0.5 or a['id'] <= 0 or b['id'] <= 0:
            continue
        pairs += 1
        t = a['t']
        # A game that is paused or loading (the audit says so), or whose world clock stands, moves nothing: places are not judged then, nor for 3 s after
        if a.get('menu') or b.get('menu') or a.get('paused') or b.get('paused') or a.get('loading') or b.get('loading') or (prev and (a['world'] - prev[0]['world'] < 0.5 or b['world'] - prev[1]['world'] < 0.5)):
            if t > paused_until:
                paused_n += 1
            paused_until = t + 3
        frozen = t <= paused_until
        ea, eb = live(a), live(b)
        for k, s in enumerate((a, b)):   # (a loose device in three lines in a row: one line can be a copy in the making)
            n = len(s.get('loose', []))
            loose_run[k] = min(loose_run[k], n) if loose_run[k] and n else n
            loose_len[k] = loose_len[k] + 1 if n else 0
            if loose_len[k] >= 3:
                loose[k] = max(loose[k], loose_run[k])
                loose_names[k] |= {x['name'] for x in s.get('loose', [])}
        for k, s in enumerate((a, b)):   # Ultrahand: what each Link holds, and whether the other game was told (class 1)
            h = s.get('hand', '')
            if h:
                hand_s[k] += 1
                other = (eb, ea)[k].get(h)
                hand_told[k] += bool(other and other.get('cls') == 1)
        ba, bb = ({tuple(sorted(x)) for x in s.get('bonds', [])} for s in (a, b))
        if ba != bb:
            bond_diff.append((t, f'{len(ba)} bonds in {dirs[0]}, {len(bb)} in {dirs[1]}'))
        bond_max = max(bond_max, len(ba), len(bb))
        on_a, on_b = ({k for k, e in s.items() if e.get('on')} for s in (ea, eb))
        on_max = max(on_max, min(len(on_a), len(on_b)))
        if on_a != on_b:
            on_diff.append((t, f'{len(on_a)} devices on in {dirs[0]}, {len(on_b)} in {dirs[1]}'))
        for eid in ea.keys() | eb.keys():
            first_seen.setdefault(eid, t)
            if t - first_seen[eid] < 3:
                continue
            x, y = ea.get(eid), eb.get(eid)
            if not x or not y or not x['actor'] or not y['actor']:
                where = dirs[1] if x and x['actor'] else dirs[0]
                missing[eid].append((t, f"{(x or y)['name']} {eid} has no actor in {where}" if (x or y) else eid))
                continue
            if x['mine'] + y['mine'] != 1:
                owners[eid].append((t, f"{x['name']} {eid}: owned by {'both' if x['mine'] else 'neither'}"))
                continue
            own, own_was = (x, prev and live(prev[0]).get(eid)) if x['mine'] else (y, prev and live(prev[1]).get(eid))
            d, ang = dist(x['p'], y['p']), angle(x['r'], y['r'])
            if not own_was or 'p' not in own_was:
                continue
            dt = t - prev[0]['t']
            v = dist(own['p'], own_was['p']) / dt if dt > 0 else 0
            timeline[eid].append(f"{t - t0:6.1f} s  owner {'1' if x['mine'] else '2'} class {own['cls']}  owner speed {v:5.2f} m/s  apart {d:.3f} m {ang:5.1f} deg")
            was_still = last_pair.get(eid, (9, 0))[0] < 0.05
            last_pair[eid] = (v, t)
            if frozen:
                timeline[eid][-1] += '  (a game is paused: not judged)'
            elif v < 0.05 and was_still:
                rest_d.append(d), rest_a.append(ang)
                if d / 0.25 + ang / 10 > worst_rest[0]:
                    worst_rest = (d / 0.25 + ang / 10, f"{x['name']} {eid} at {t - t0:.0f} s: {d:.2f} m, {ang:.1f} degrees")
            elif v > 0.2:
                move_d.append(d)
                if d > worst_move[0]:
                    worst_move = (d, f"{x['name']} {eid} at {t - t0:.0f} s: {d:.2f} m at {v:.1f} m/s")
        prev = (a, b)

    def held(hits):   # in two paired lines in a row
        return [h for i, h in enumerate(hits) if (i and hits[i - 1][0] > h[0] - 1.6) or (i + 1 < len(hits) and hits[i + 1][0] < h[0] + 1.6)]
    miss = {k: held(v) for k, v in missing.items() if held(v)}
    own = {k: held(v) for k, v in owners.items() if held(v)}
    n_ents = len(first_seen)
    results.append(('count', not miss, f'{n_ents} entities seen' + (''.join(f'; {v[0][1]} from {v[0][0] - t0:.0f} s for {len(v)} lines' for v in miss.values()) if miss else ', each in both games')))
    results.append(('loose', not any(loose), 'none' if not any(loose) else '; '.join(f'{loose[k]} in {dirs[k]} ({", ".join(sorted(loose_names[k]))})' for k in (0, 1) if loose[k])))
    results.append(('owner', not own, 'one owner each' if not own else '; '.join(f'{v[0][1]} from {v[0][0] - t0:.0f} s for {len(v)} lines' for v in own.values())))
    # a difference of the bond sets that lasts over 3 lines in a row
    run, longest, first_long = 0, 0, None
    for i, h in enumerate(bond_diff):
        run = run + 1 if i and bond_diff[i - 1][0] > h[0] - 1.6 else 1
        if run > longest:
            longest = run
        if run == 4 and not first_long:
            first_long = h
    results.append(('bonds', None if not bond_max and not bond_diff else longest <= 3,
                    f'at most {bond_max} bonds; ' + ('the same in both games' if not bond_diff else f'different in {len(bond_diff)} lines, {longest} in a row at most'
                                                     + (f' ({first_long[1]} at {first_long[0] - t0:.0f} s)' if first_long else ''))))
    # devices switched on: the same ones in both games (a difference of up to 2 lines in a row is a switch on its way)
    run = longest_on = 0
    for i, h in enumerate(on_diff):
        run = run + 1 if i and on_diff[i - 1][0] > h[0] - 1.6 else 1
        longest_on = max(longest_on, run)
    results.append(('devices', None if not on_max and not on_diff else longest_on <= 2,
                    f'at most {on_max} on in both games at once; ' + ('the same ones on in both' if not on_diff else f'different in {len(on_diff)} lines, {longest_on} in a row at most ({on_diff[0][1]} at {on_diff[0][0] - t0:.0f} s)')))
    for k in (0, 1):
        if hand_s[k]:
            results.append(('hand', hand_told[k] >= hand_s[k] - 2, f'the Link of {dirs[k]} held an entity with Ultrahand in {hand_s[k]} lines; the other game had it as held in {hand_told[k]}'))
    if not any(hand_s):
        results.append(('hand', None, 'no Link held an entity with Ultrahand'))
    # what the run was meant to do (--expect hand1,hand2,bonds=2): a scripted action that did not happen fails the run
    for arg in sys.argv:
        if not arg.startswith('--expect='):
            continue
        for want in arg[9:].split(','):
            if want in ('hand1', 'hand2'):
                k = int(want[4]) - 1
                results.append(('expected', hand_s[k] >= 2, f'player {k + 1} takes an entity with Ultrahand: ' + (f'held for {hand_s[k]} lines' if hand_s[k] else 'it never held one (the scripted grab did not land)')))
            elif want == 'onseen':
                results.append(('expected', on_max >= 1, f'a device is on in both games at some time: at most {on_max} at once'))
            elif want.startswith('ents='):
                na, nb2 = len(live(A[-1])), len(live(B[-1]))
                results.append(('expected', na == nb2 == int(want[5:]), f'{want[5:]} entities at the end: {na} and {nb2}'))
            elif want.startswith('bonds='):
                last = min(len(A[-1].get('bonds', [])), len(B[-1].get('bonds', [])))
                results.append(('expected', last == int(want[6:]), f'{want[6:]} bonds at the end: {len(A[-1].get("bonds", []))} and {len(B[-1].get("bonds", []))}'))
    if rest_d:
        ok = pct(rest_d, .9) <= 0.10 and pct(rest_a, .9) <= 5 and max(rest_d) <= 0.25 and max(rest_a) <= 10
        results.append(('rest', ok, f'{len(rest_d)} lines: {pct(rest_d, .5):.3f} m median, {pct(rest_d, .9):.3f} at 90%, {max(rest_d):.3f} at most; '
                        f'{pct(rest_a, .5):.1f} degrees median, {pct(rest_a, .9):.1f} at 90%, {max(rest_a):.1f} at most; worst: {worst_rest[1]}'))
    else:
        results.append(('rest', None, 'no line with an owner\'s object at rest'))
    if move_d:
        results.append(('moving', pct(move_d, .9) <= 0.30, f'{len(move_d)} lines: {pct(move_d, .5):.3f} m median, {pct(move_d, .9):.3f} at 90%, {max(move_d):.3f} at most; worst: {worst_move[1]}'))
    else:
        results.append(('moving', None, 'no line with an owner\'s object moving'))
    # A bench that moved a Link by jumps while it held a part (join) moved the part by jumps: the two lines below then
    # measure the bench, and are told but not judged.
    jumped = any(grep(log, r'coop: bench: .* join: .* moved by') for log in logs)
    traces = [trace_read(log) for log in logs]
    steps = [s for tr in traces for s in tr[0].values() if len(s) >= 30]
    if steps:
        allv = [x for s in steps for x in s]
        heldv = [x for tr in traces for s in tr[1].values() for x in s]
        results.append(('smooth', None if jumped else pct(allv, .9) <= 1.0, ('(not judged: the bench moved a holding Link by jumps) ' if jumped else '') + f'{len(allv)} frames of {len(steps)} copies: the written velocity changes by {pct(allv, .5):.2f} m/s a frame median, '
                        f'{pct(allv, .9):.2f} at 90%, {max(allv):.2f} at most' + (f'; while held ({len(heldv)} frames) {pct(heldv, .5):.2f} median, {pct(heldv, .9):.2f} at 90%' if heldv else '')))
    else:
        results.append(('smooth', None, 'no trace of a moving copy (TOTK_COOP_FOLLOW_TRACE=1)'))
    speed = [x for tr in traces for x in tr[2]]
    if len(speed) >= 20:
        ratio, off = [x[0] for x in speed], [x[1] for x in speed]
        heldn = sum(1 for x in speed if x[2])
        ok = None if jumped else 0.7 <= pct(ratio, .5) <= 1.4
        results.append(('speed', ok, ('(not judged: the bench moved a holding Link by jumps) ' if jumped else '') + f'{len(speed)} poses ({heldn} while held): the velocity the owner sends is {pct(ratio, .5):.2f} of what its poses give (median; '
                        f'{pct(ratio, .1):.2f} to {pct(ratio, .9):.2f} for 8 in 10), {pct(off, .5):.2f} m/s apart median'))
    else:
        results.append(('speed', None, f'{len(speed)} poses of a moving owner with a sent velocity: too few to say'))
    placed = sum(len(grep(log, 'coop: follow: .*: put in place')) for log in logs)
    heldc = sum(len(grep(log, 'left its rest three times')) for log in logs)
    if paused_n:
        results.append(('paused', None, f'a game\'s world clock stood {paused_n} time(s): places were not judged then, nor for 3 s after'))
    results.append(('settle', None, f'{placed} copies put in place, {heldc} held by the drive at rest'))
    print(f'{pairs} paired lines over {A[-1]["t"] - t0:.0f} s')
    failed = 0
    if pairs < 10:   # (nothing to compare is no pass)
        results.insert(0, ('pairs', False, f'only {pairs} lines of the two games pair by time: the games were not both in the world together, or an audit file is an older run\'s'))
    for name, ok, text in results:
        failed += ok is False
        print(f'  {"PASS" if ok else "    " if ok is None else "FAIL"}  {name:9s} {text}')
    print('all measured lines pass' if not failed else f'{failed} line(s) fail')
    if '--timeline' in sys.argv:   # every paired line of every entity: when it was apart, and what its owner did then
        for eid in sorted(timeline):
            print(f'-- {eid}')
            print('\n'.join(timeline[eid]))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
