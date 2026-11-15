"""What both games wrote around a moment of a co-op session: a tester's mark (Home or numpad 0, runtime/coop_ent_log.c)
or a time of day.

    python tools/coop/ent_marks.py <game 1> <game 2> [--mark N] [--at HH:MM:SS] [--before 10] [--after 2]

<game> is a game's folder (its logs are in logs/) or a folder that holds totk.log, motion.jsonl and audit.jsonl
themselves (a session kept by play_two.ps1 in build/sessions). With no --mark and no --at, every mark is shown.

For each moment:
  - the lines of both logs in the window, by time: every change of an entity, jumps, placements by the drive, refused
    collisions, ownership, glue, frame times. A log line carries no time of its own; it is given the time of the
    "coop: clock:" line before it (one a second), so the order inside a second is each game's own.
  - where the two games had each entity (logs/motion.jsonl, ten lines a second while it moves): how far apart at the
    moment, and the most in the window with its time.
  - the drive's own figures for each copy (the "coop: ftrace:" lines): the largest error it saw.
A copy follows its owner's object by the network's delay, so a few centimetres apart while moving is the expected
picture; a difference at rest, or one that grows, is the fault.
"""
import json
import math
import os
import re
import sys

CLOCK = re.compile(r'coop: clock: (\S+) t ([\d.]+) world ([\d.]+)')
MARK = re.compile(r'coop: MARK (\d+) at (\S+) \(t ([\d.]+), world ([\d.]+)')
KEEP = re.compile(r'coop: (ent|veto|perf|glue|g4|MARK|bench|contraption|autobuild)\b|coop: follow: copy')
NAME = re.compile(r'coop: ent: change: (\S+) (\S+): new here')
FTRACE = re.compile(r'coop: ftrace: (\d+) (\w+) cls (\d) rests (\d) .*?\(([\d.]+) m\).*?\(([\d.]+) deg\)')


def folder(d):
    return os.path.join(d, 'logs') if os.path.isdir(os.path.join(d, 'logs')) else d


def read_log(path):
    """(lines [(t, world of the clock line, text)], marks [(n, wall, t, world)], clocks [(wall, t)], names {eid: name},
    ftrace [(world s, key, error m, error deg)])"""
    lines, marks, clocks, names, ftrace = [], [], [], {}, []
    t = world = None
    if not os.path.exists(path):
        return lines, marks, clocks, names, ftrace
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            s = raw.rstrip('\n')
            m = CLOCK.search(s)
            if m:
                t, world = float(m.group(2)), float(m.group(3))
                clocks.append((m.group(1), t))
                continue
            m = FTRACE.search(s)
            if m:
                ftrace.append((int(m.group(1)) / 1e6, m.group(2), float(m.group(5)), float(m.group(6))))
                continue
            m = MARK.search(s)
            if m:
                marks.append((int(m.group(1)), m.group(2), float(m.group(3)), float(m.group(4))))
                t = float(m.group(3))
            m = NAME.search(s)
            if m:
                names[m.group(1)] = m.group(2)
            if t is not None and KEEP.search(s):
                lines.append((t, world, s[s.find('coop:'):]))
    return lines, marks, clocks, names, ftrace


def read_motion(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        for raw in f:
            try:
                out.append(json.loads(raw))
            except ValueError:
                pass   # (a line cut short by the game's end)
    return out


def angle(ra, rb):
    tr = sum(ra[i] * rb[i] for i in range(9))
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1) / 2))))


def states_at(motion, upto):
    """every entity's last written state at or before time upto: {eid: entry}"""
    st = {}
    for ln in motion:
        if ln['t'] > upto:
            break
        for e in ln['e']:
            st[e['id']] = e
    return st


def differences(MA, MB, t0, t1, tm):
    """per entity: (apart at tm (m, deg) or None, most apart in [t0, t1] (m, at s relative to tm), who owns it)"""
    times = sorted({ln['t'] for ln in MA + MB if t0 <= ln['t'] <= t1} | {tm})
    worst, at_mark, owner = {}, {}, {}
    ia = ib = 0
    sa, sb = {}, {}
    for t in times:
        while ia < len(MA) and MA[ia]['t'] <= t:
            for e in MA[ia]['e']:
                sa[e['id']] = e
            ia += 1
        while ib < len(MB) and MB[ib]['t'] <= t:
            for e in MB[ib]['e']:
                sb[e['id']] = e
            ib += 1
        for k in sa.keys() & sb.keys():
            a, b = sa[k], sb[k]
            d = math.dist(a['p'], b['p'])
            g = angle(a['r'], b['r'])
            owner[k] = 'game 1' if a['m'] else 'game 2' if b['m'] else 'neither game here'
            if k not in worst or d > worst[k][0]:
                worst[k] = (d, g, t - tm)
            if t == tm:
                at_mark[k] = (d, g)
    only = (sorted(sa.keys() - sb.keys()), sorted(sb.keys() - sa.keys()))
    return worst, at_mark, owner, only


def show(dirs, logs, motions, label, tm, before, after):
    print(f'== {label}')
    t0, t1 = tm - before, tm + after
    rows = []
    for g in (0, 1):
        rows += [(t, g, s) for t, _, s in logs[g][0] if t0 - 1 <= t <= t1]
    rows.sort(key=lambda r: (r[0], r[1]))
    print(f'-- both logs, {before:.0f} s before to {after:.0f} s after (times to the second: the clock line before each)')
    if not rows:
        print('   (no lines: is the session record on? TOTK_COOP_RECORD)')
    for t, g, s in rows:
        print(f'  {t - tm:+6.1f} s  game {g + 1}  {s}')
    names = {**logs[0][3], **logs[1][3]}
    worst, at_mark, owner, only = differences(motions[0], motions[1], t0, t1, tm)
    print('-- where the two games had each entity (motion files)')
    if not motions[0] or not motions[1]:
        print('   (a motion file is missing or empty)')
    for k in sorted(worst, key=lambda k: -worst[k][0]):
        d, g, at = worst[k]
        m = at_mark.get(k)
        now = f'{m[0]:.2f} m, {m[1]:.1f} deg apart at the moment' if m else 'not compared at the moment'
        print(f'  {k} {names.get(k, "?")} (owned by {owner[k]}): {now}; most {d:.2f} m ({g:.1f} deg) at {at:+.1f} s')
    for g in (0, 1):
        for k in only[g]:
            print(f'  {k} {names.get(k, "?")}: written by game {g + 1} only (no actor in the other game, or it never moved there)')
    print('-- the drive, per copy (ftrace)')
    for g in (0, 1):
        marks_world = [w for _, _, t, w in logs[g][1] if abs(t - tm) < 0.5]
        # the window on this game's world clock: from its clock lines
        wl = [(t, w) for t, w, _ in logs[g][0] if t0 <= t <= t1 and w is not None]
        if marks_world:
            w0, w1 = marks_world[0] - before, marks_world[0] + after
        elif wl:
            w0, w1 = wl[0][1] - 1, wl[-1][1] + 1
        else:
            continue
        per = {}
        for w, key, e, deg in logs[g][4]:
            if w0 <= w <= w1:
                p = per.setdefault(key, [0, 0.0, 0.0, 0.0])
                p[0] += 1
                if e > p[1]:
                    p[1], p[3] = e, w
                p[2] = max(p[2], deg)
        for key, (n, e, deg, w) in sorted(per.items()):
            print(f'  game {g + 1} copy {key}: written in {n} frames; largest error {e:.2f} m (at world {w:.2f} s), {deg:.1f} deg')
        if not per:
            print(f'  game {g + 1}: the drive wrote to no copy in the window')
    print()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '') for a in sys.argv[1:] if a.startswith('--'))
    # ("--mark 3" as well as "--mark=3")
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a in ('--mark', '--at', '--before', '--after') and i + 1 < len(argv):
            opts[a[2:]] = argv[i + 1]
            if argv[i + 1] in args:
                args.remove(argv[i + 1])
    if len(args) != 2:
        print(__doc__)
        return 2
    before, after = float(opts.get('before') or 10), float(opts.get('after') or 2)
    dirs = [folder(d) for d in args]
    logs = [read_log(os.path.join(d, 'totk.log')) for d in dirs]
    motions = [read_motion(os.path.join(d, 'motion.jsonl')) for d in dirs]
    for g in (0, 1):
        if not logs[g][2]:
            print(f'game {g + 1}: no "coop: clock:" line in {dirs[g]}/totk.log: its lines cannot be placed in time')
    if opts.get('at'):
        want = opts['at']
        hits = [t for wall, t in logs[0][2] + logs[1][2] if wall.startswith(want)]
        if not hits:
            print(f'no clock line at {want} in either log')
            return 1
        show(dirs, logs, motions, f'{want}', min(hits), before, after)
        return 0
    marks = {}
    for g in (0, 1):
        for n, wall, t, _ in logs[g][1]:
            marks.setdefault(round(t, 1), (n, wall, t, []))[3].append(g + 1)
    # (both games write a mark within a frame of each other: one moment)
    moments = []
    for key in sorted(marks):
        n, wall, t, games = marks[key]
        if moments and t - moments[-1][2] < 0.5:
            moments[-1][3].extend(games)
        else:
            moments.append((n, wall, t, games))
    if opts.get('mark'):
        moments = [m for m in moments if m[0] == int(opts['mark'])]
    if not moments:
        print('no mark found (Home or numpad 0 during the session); use --at HH:MM:SS for a time of day')
        return 1
    for n, wall, t, games in moments:
        pics = ', '.join(os.path.join(dirs[g - 1], f'mark-{n:03d}.png') for g in sorted(set(games)))
        show(dirs, logs, motions, f'MARK {n} at {wall} (written by game {" and ".join(str(g) for g in sorted(set(games)))}; pictures: {pics})', t, before, after)
    return 0


if __name__ == '__main__':
    sys.exit(main())
