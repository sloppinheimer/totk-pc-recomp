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
    return {e['id']: e for e in snap.get('ents', []) if e['life'] == 0}


TRACE = re.compile(r"ftrace: (\d+) (\w+) cls \d rests \d dt [\d.]+ sample \d+ .*\| vr ([-\d. ]+) ar .*\| V ([-\d. ]+) W ")


def trace_steps(log):
    """per copy: the frame-to-frame change of the written velocity while its owner's object moves"""
    out, prev = defaultdict(list), {}
    if not os.path.exists(log):
        return out
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = TRACE.search(line)
            if not m:
                continue
            now, key = int(m.group(1)), m.group(2)
            vr, v = [float(x) for x in m.group(3).split()], [float(x) for x in m.group(4).split()]
            p = prev.get(key)
            if p and now - p[0] < 40000 and math.sqrt(sum(x * x for x in vr)) > 0.3:
                out[key].append(dist(v, p[1]))
            prev[key] = (now, v)
    return out


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
    if not A or not B or 'ents' not in A[-1] or 'ents' not in B[-1]:
        print(f'no entity audit: {len(A)} lines in {dirs[0]}, {len(B)} in {dirs[1]} (a build before the audit, or TOTK_COOP_CONTRAPTION is off)')
        return 2
    results = []   # (name, ok or None, text)
    # the bench's own word on its scenario
    bench = [l for log in logs for l in grep(log, r'coop: bench: .*(landed|did not land|not ready|is gone)')]
    if bench:
        bad = [l for l in bench if 'did not land' in l or 'not ready' in l or 'is gone' in l]
        results.append(('scenario', not bad, '; '.join(l.split('coop: bench: ')[1] for l in bench)))
    first_seen, last_pair = {}, {}
    missing, owners = defaultdict(list), defaultdict(list)
    rest_d, rest_a, move_d = [], [], []
    worst_rest, worst_move = (0, ''), (0, '')
    loose = [0, 0]
    loose_names = [set(), set()]
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
        ea, eb = live(a), live(b)
        for k, s in enumerate((a, b)):
            loose[k] = max(loose[k], len(s.get('loose', [])))
            loose_names[k] |= {x['name'] for x in s.get('loose', [])}
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
            was_still = last_pair.get(eid, (9, 0))[0] < 0.05
            last_pair[eid] = (v, t)
            if v < 0.05 and was_still:
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
    steps = [s for log in logs for s in trace_steps(log).values() if len(s) >= 30]
    if steps:
        allv = [x for s in steps for x in s]
        results.append(('smooth', pct(allv, .9) <= 1.0, f'{len(allv)} frames of {len(steps)} copies: the written velocity changes by {pct(allv, .5):.2f} m/s a frame median, '
                        f'{pct(allv, .9):.2f} at 90%, {max(allv):.2f} at most'))
    else:
        results.append(('smooth', None, 'no trace of a moving copy (TOTK_COOP_FOLLOW_TRACE=1)'))
    placed = sum(len(grep(log, 'put in place')) for log in logs)
    heldc = sum(len(grep(log, 'left its rest three times')) for log in logs)
    results.append(('settle', None, f'{placed} copies put in place, {heldc} held by the drive at rest'))
    print(f'{pairs} paired lines over {A[-1]["t"] - t0:.0f} s')
    failed = 0
    for name, ok, text in results:
        failed += ok is False
        print(f'  {"PASS" if ok else "    " if ok is None else "FAIL"}  {name:9s} {text}')
    print('all measured lines pass' if not failed else f'{failed} line(s) fail')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
