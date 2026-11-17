"""The enemy sync's report for two games' logs of one session on one PC (docs/coop/enemy-sync.md, "Checks").

    python tools/coop/en_report.py <log of game A> <log of game B>

Both games stamp with the PC's performance counter, so a copy's sample "at T" (the time on its owner's clock that the
shown pose is the owner's pose of) is compared with the owner's own samples around T. Printed per enemy and for the
session: position error (shown against the owner at T; the actor against what was asked), command agreement, clock
error, life disagreement, and from the per-second lines: states held, clocks written, leaks, changes of owner, and
whether the two tables end equal.
"""
import bisect
import math
import re
import sys

POS = re.compile(r'coop: en: pos: (-?\d+) ([0-9a-f]{16}) ([oc]) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (\S+) (-?[\d.]+) hp (-?\d+)(?: asked (-?[\d.]+) (-?[\d.]+) (-?[\d.]+))?')
TABLE = re.compile(r'coop: en: table: (\d+) entries \((\d+) this game\'s, (\d+) with an actor here\), (\d+) unsynced, (\d+) resting, (\d+) tombstones; table number ([0-9a-f]{8}); (\d+) changes of owner, (\d+) messages dropped, (\d+) not sent')
MOTION = re.compile(r'coop: en: motion: ([0-9a-f]{16}) (\S+): delay (\d+) ms, (\d+) states taken, (\d+) frames held; (\d+) plays, (\d+) clocks written, (\d+) rates bent, (\d+) frames not at the pose asked')
SWITCH = re.compile(r'coop: en: switch: (\d+) copies and (\d+) held: (\d+) runner entries turned away, (\d+) leaks; (\d+) run here: (\d+) runner entries let through')


def read(path):
    own, cop, tables, motion, switch, names, lines = {}, {}, [], {}, [], {}, []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'coop: en:' not in line:
                continue
            m = POS.search(line)
            if m:
                ns, h, kind = int(m.group(1)), m.group(2), m.group(3)
                rec = dict(ns=ns, p=tuple(float(m.group(i)) for i in (4, 5, 6)), q=tuple(float(m.group(i)) for i in (7, 8, 9, 10)), cmd=m.group(11),
                           clock=float(m.group(12)), hp=int(m.group(13)), asked=tuple(float(m.group(i)) for i in (14, 15, 16)) if m.group(14) else None)
                (own if kind == 'o' else cop).setdefault(h, []).append(rec)
                continue
            m = TABLE.search(line)
            if m:
                tables.append(m.groups())
                continue
            m = MOTION.search(line)
            if m:
                names[m.group(1)] = m.group(2)
                motion.setdefault(m.group(1), []).append(tuple(int(m.group(i)) for i in range(3, 10)))
                continue
            m = SWITCH.search(line)
            if m:
                switch.append(tuple(int(x) for x in m.groups()))
                continue
            if 'coop: en: own:' in line or 'coop: en: leak' in line or 'coop: en: unsynced' in line:
                lines.append(line.strip())
    return dict(own=own, cop=cop, tables=tables, motion=motion, switch=switch, names=names, lines=lines)


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def at(samples, keys, ns):
    """The owner's place, command, clock and life at ns, between its two samples around it (None outside them)."""
    i = bisect.bisect_right(keys, ns)
    if i == 0 or i == len(samples):
        return None
    a, b = samples[i - 1], samples[i]
    if b['ns'] - a['ns'] > 400_000_000:
        return None
    t = (ns - a['ns']) / (b['ns'] - a['ns'])
    p = tuple(x + (y - x) * t for x, y in zip(a['p'], b['p']))
    speed = dist(a['p'], b['p']) / ((b['ns'] - a['ns']) / 1e9)
    if speed > 15:   # a teleport between the two samples (a Yiga's blink, a Wizzrobe's warp): no place in between
        return None
    return dict(p=p, cmd=a['cmd'], cmd2=b['cmd'], clock=a['clock'] + (b['clock'] - a['clock']) * t if a['cmd'] == b['cmd'] and b['clock'] >= a['clock'] else None, hp=a['hp'], hp2=b['hp'], speed=speed)


def pair(owner, copy, who):
    rows, tot_err, tot_n, bad = [], [], 0, 0
    for h, cs in sorted(copy['cop'].items()):
        os_ = owner['own'].get(h)
        if not os_:
            continue
        keys = [s['ns'] for s in os_]
        err_still, err_move, off, cmd_ok, cmd_n, ck, hp_bad = [], [], [], 0, 0, [], 0
        for c in cs:
            o = at(os_, keys, c['ns'])
            if not o or not c['asked']:
                continue
            e = dist(c['p'], o['p'])   # where the copy's actor stands, not where the sync asked it to be
            (err_move if o['speed'] > 0.3 else err_still).append(e)
            off.append(dist(c['p'], c['asked']))
            cmd_n += 1
            cmd_ok += c['cmd'] in (o['cmd'], o['cmd2'])
            if o['clock'] is not None and c['cmd'] == o['cmd']:
                ck.append(abs(c['clock'] - o['clock']))
            hp_bad += c['hp'] not in (o['hp'], o['hp2'])
        if not cmd_n:
            continue
        mo = copy['motion'].get(h, [])
        rows.append((copy['names'].get(h, '?'), h, cmd_n, pct(err_still, .95) * 100, pct(err_move, .95) * 100, len(err_move), max(err_still + err_move) * 100, pct(off, .95) * 100,
                     100.0 * cmd_ok / cmd_n, pct(ck, .95) / 30 * 1000, 100.0 * hp_bad / cmd_n,
                     sum(m[2] for m in mo), sum(m[4] for m in mo), sum(m[6] for m in mo), (sum(m[0] for m in mo) / len(mo)) if mo else 0))
        tot_err += err_still + err_move
        tot_n += cmd_n
    print(f'\n{who}: {len(rows)} copies compared with their owner at the same moment ({tot_n} samples)')
    print(f'  {"enemy":34} {"n":>5} {"still p95":>9} {"moving p95":>10} {"(n)":>5} {"worst":>7} {"off pose":>8} {"cmd ok":>7} {"clock p95":>9} {"life off":>8} {"held":>5} {"hard":>5} {"notat":>5} {"delay":>6}')
    for r in rows:
        print(f'  {r[0][:34]:34} {r[2]:5d} {r[3]:7.1f}cm {r[4]:8.1f}cm {r[5]:5d} {r[6]:5.1f}cm {r[7]:6.1f}cm {r[8]:6.1f}% {r[9]:7.0f}ms {r[10]:7.1f}% {r[11]:5d} {r[12]:5d} {r[13]:5d} {r[14]:4.0f}ms')
        bad += r[8] < 95 or r[6] > 50
    if tot_err:
        print(f'  all: position error p95 {pct(tot_err, .95) * 100:.1f} cm, worst {max(tot_err) * 100:.1f} cm')
    return bad


def main():
    a, b = read(sys.argv[1]), read(sys.argv[2])
    for name, g in (('A', a), ('B', b)):
        t = g['tables'][-1] if g['tables'] else None
        sw = g['switch']
        print(f'game {name}: ' + (f'{t[0]} entries ({t[1]} its own, {t[2]} with an actor), {t[3]} unsynced, {t[4]} resting, {t[5]} tombstones, table number {t[6]}, {t[7]} changes of owner, {t[8]} dropped, {t[9]} unsent' if t else 'no table line'))
        if sw:
            print(f'  switch: up to {max(s[0] for s in sw)} copies; {sum(s[2] for s in sw)} runner entries turned away, {max(s[3] for s in sw)} leaks; its own enemies let {sum(s[5] for s in sw)} through')
    if a['tables'] and b['tables']:
        print('tables equal at the end: ' + ('YES' if a['tables'][-1][6] == b['tables'][-1][6] else 'NO'))
    bad = pair(a, b, 'B shows A\'s enemies') + pair(b, a, 'A shows B\'s enemies')
    own = [l for g in (a, b) for l in g['lines'] if 'leak' in l or 'unsynced' in l]
    for l in own[:20]:
        print('  ' + l[:200])
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
