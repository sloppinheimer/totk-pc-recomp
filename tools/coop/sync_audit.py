"""Compares what two co-op games show, from their logs/audit.jsonl (runtime/coop_players.c, audit_tick).

    python tools/coop/sync_audit.py build/v143/logs/audit.jsonl build/v143b/logs/audit.jsonl [--all]

Each game writes one snapshot a second, at the turn of a second of the PC's performance counter, so the two games'
lines pair by time (both games on one PC). For each pair, both ways (the owner's game against the other's):
  link        the owner's Link shown in the other game: there at all, its place (not when the owner is over 200 m
              from that game's own Link, where that game has not loaded the world), life, gear, hands, fused
              materials, dyes
  object      each object the owner sends: shown in the other game (a copy, or its own placed one held at the
              report), and at its place
  stale       a copy shown of an object its owner no longer sends
  dup / dead  two copies of one object; a copy whose actor is gone
  enemy       each enemy near a Link in both games: its place and life the same (the host is the lower id)
  enemy-only  an enemy within 100 m of the host's Link in one game only (not counted as a failure: it can be the
              other game's streaming)
  time        the time of day: the guest follows the host's (within 180 game seconds, 3 s of real time)
  weather     the same within 300 m of each other
Nothing is checked in the first 10 s after both games have an id (the Links are being made). A mismatch is counted
when it holds in two snapshots in a row (one-off timing is not). Places allow for movement:
0.75 m plus a quarter second of the owner's speed. Prints each kind with its first time and example, and exits 1
when there is any (enemy-only aside). --all prints every occurrence.
"""
import json
import math
import sys
from collections import defaultdict


def load(path):
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass   # a line cut short when the game was closed
    return out


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def speed(prev, cur, getp):
    """m/s of a thing between two snapshots of the same game (0 when unknown)"""
    if not prev:
        return 0.0
    a, b = getp(prev), getp(cur)
    dt = cur['t'] - prev['t']
    return dist(a, b) / dt if a and b and dt > 0 else 0.0


def sent_by_id(snap):
    return {o['id']: o for o in snap.get('sent', [])} if snap else {}


JOIN_GRACE = 10.0   # s after both games first have an id: the other's Link is still being made and given its gear
joined_at = {}


def check_way(own, own_prev, view, view_prev, report):
    """own: the owner's snapshot; view: the other game's at the same time"""
    oid, t = own['id'], own['t']
    if oid <= 0 or view['id'] <= 0:
        return
    if t - joined_at.setdefault((oid, view['id']), t) < JOIN_GRACE:
        return
    me = own['me']
    links = [l for l in view.get('links', []) if l['owner'] == oid]
    if not links:
        report('link', f'{oid}', t, f"player {oid}'s Link is not shown in player {view['id']}'s game")
        return
    link = links[0]
    v = speed(own_prev, own, lambda s: s['me']['p'])
    tol = 0.75 + v * 0.25
    d = dist(link['p'], me['p'])
    far = dist(view['me']['p'], me['p']) > 200   # the viewer's world is not loaded about the owner: its Link stands
    if d > tol and not far:
        report('link-place', f'{oid}', t, f"player {oid}'s Link is {d:.2f} m off in player {view['id']}'s game (allowed {tol:.2f}, moving {v:.1f} m/s)")
    if me['life'] > 0 and link['life'] >= 0 and link['life'] != me['life']:
        report('link-life', f'{oid}', t, f"player {oid}'s life {me['life']}, its Link there {link['life']}")
    for k, (a, b) in enumerate(zip(me['gear'], link['gear'])):
        if a and b and a != b:
            report('link-gear', f'{oid}/{k}', t, f"player {oid}'s gear slot {k} is {a}, its Link there has {b}")
    if link['gear'] and any(link['gear']) and me['hands'] != link['hands']:
        report('link-hands', f'{oid}', t, f"player {oid} holds {me['hands']:03b} (weapon, bow, shield), its Link there {link['hands']:03b}")
    for k in range(3):
        a, b = me['fuse'][k], link['fuse'][k]
        if me['gear'][k] not in ('', '-') and a != b:
            report('link-fuse', f'{oid}/{k}', t, f"player {oid}'s slot {k} fused with '{a}', its Link there '{b}'")
    for k in range(3, 6):
        a, b = me['dye'][k], link['dye'][k]
        if a >= 0 and a != b:
            report('link-dye', f'{oid}/{k}', t, f"player {oid}'s armor slot {k} dye {a}, its Link there {b}")
    # objects
    prev_sent = sent_by_id(own_prev)
    copies = defaultdict(list)
    for c in view.get('copies', []):
        if c['owner'] == oid:
            copies[c['id']].append(c)
    held = {h['id']: h for h in view.get('held', []) if h['owner'] == oid}
    for o in own.get('sent', []):
        if o['id'] not in prev_sent:
            continue   # new this second: its copy may still be being made
        ov = speed({'t': own_prev['t'], 'p': prev_sent[o['id']]['p']}, {'t': t, 'p': o['p']}, lambda s: s['p'])
        otol = 1.0 + ov * 0.25
        keyed = o['id'] & 0x80000000
        shown = copies.get(o['id']) or ([held[o['id']]] if o['id'] in held else [])
        if not shown:
            if not keyed:   # a keyed one at rest needs no hold: the other game has its own, where the map put it
                report('object', f"{oid}/{o['id']}", t, f"player {oid}'s {o['name']} ({o['id']:#x}) is not shown in player {view['id']}'s game")
            continue
        d = dist(shown[0]['p'], o['p'])
        if d > otol:
            report('object-place', f"{oid}/{o['id']}", t, f"player {oid}'s {o['name']} ({o['id']:#x}) is {d:.2f} m off there (allowed {otol:.2f})")
    view_prev_copies = {(c['owner'], c['id']) for c in (view_prev or {}).get('copies', [])}
    sent_now = sent_by_id(own)
    for cid, cs in copies.items():
        if len(cs) > 1:
            report('dup', f'{oid}/{cid}', t, f"player {view['id']}'s game shows {len(cs)} copies of player {oid}'s {cs[0]['name']} ({cid:#x})")
        for c in cs:
            if not c['alive']:
                report('dead', f'{oid}/{cid}', t, f"player {view['id']}'s copy of {c['name']} ({cid:#x}) has no live actor")
            if cid not in sent_now and cid not in prev_sent and not c['blowing'] and (oid, cid) in view_prev_copies:
                report('stale', f'{oid}/{cid}', t, f"player {view['id']}'s game still shows a copy of {c['name']} ({cid:#x}); player {oid} no longer sends it")


def check_pair(a, a_prev, b, b_prev, report):
    check_way(a, a_prev, b, b_prev, report)
    check_way(b, b_prev, a, a_prev, report)
    if a['id'] <= 0 or b['id'] <= 0:
        return
    host, guest = (a, b) if a['id'] < b['id'] else (b, a)
    t = a['t']
    # enemies
    he = {e['key']: e for e in host.get('enemies', []) if not e.get('dup')}
    ge = {e['key']: e for e in guest.get('enemies', []) if not e.get('dup')}
    for k in he.keys() & ge.keys():
        x, y = he[k], ge[k]
        d = dist(x['p'], y['p'])
        if d > 2.0:
            report('enemy-place', f'{k:08x}', t, f"enemy {x['name']} {k:08x} is {d:.2f} m apart (host {host['id']}, guest {guest['id']})")
        if x['dead'] != y['dead'] or (not x['dead'] and x['life'] != y['life']):
            report('enemy-life', f'{k:08x}', t, f"enemy {x['name']} {k:08x}: host life {x['life']}{' dead' if x['dead'] else ''}, guest {y['life']}{' dead' if y['dead'] else ''}")
    for k in he.keys() ^ ge.keys():
        e = he.get(k) or ge.get(k)
        if dist(e['p'], host['me']['p']) > 100:
            continue   # the host has only the area about its own Link loaded
        where = f"host {host['id']}" if k in he else f"guest {guest['id']}"
        report('enemy-only', f'{k:08x}', t, f"enemy {e['name']} {k:08x} is near a Link in the {where}'s game only")
    # time of day and weather
    if host['tod'] >= 0 and guest['tod'] >= 0:
        dt = abs(host['tod'] - guest['tod'])
        dt = min(dt, 86400 - dt)
        if dt > 180:   # game time runs 60 times as fast: snapshots up to 0.5 s apart and a follower set every few seconds
            report('time', 'tod', t, f"time of day {host['tod']:.0f} s on the host, {guest['tod']:.0f} s on the guest")
    if dist(host['me']['p'], guest['me']['p']) < 300 and host['weather'] >= 0 and guest['weather'] >= 0 and host['weather'] != guest['weather']:
        report('weather', 'w', t, f"weather {host['weather']} on the host, {guest['weather']} on the guest, {dist(host['me']['p'], guest['me']['p']):.0f} m apart")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    A, B = load(args[0]), load(args[1])
    if not A or not B:
        print(f'no snapshots: {len(A)} in {args[0]}, {len(B)} in {args[1]}')
        return 2
    seen = defaultdict(list)   # (kind, subject) -> [(t, text)] per pair index
    pairs = 0
    bi = 0
    prev = (None, None)
    for ai, a in enumerate(A):
        while bi + 1 < len(B) and abs(B[bi + 1]['t'] - a['t']) <= abs(B[bi]['t'] - a['t']):
            bi += 1
        b = B[bi]
        if abs(b['t'] - a['t']) > 0.5:
            continue
        found = []
        check_pair(a, prev[0], b, prev[1], lambda kind, subj, t, text: found.append((kind, subj, t, text)))
        for kind, subj, t, text in found:
            seen[(kind, subj)].append((pairs, t, text))
        prev = (a, b)
        pairs += 1
    # held for two pairs in a row
    kinds = defaultdict(list)
    for (kind, subj), hits in seen.items():
        runs = [h for i, h in enumerate(hits) if (i > 0 and hits[i - 1][0] == h[0] - 1) or (i + 1 < len(hits) and hits[i + 1][0] == h[0] + 1)]
        if runs:
            kinds[kind].append((subj, runs))
    t0 = min(A[0]['t'], B[0]['t'])
    print(f'{pairs} paired snapshots ({len(A)} and {len(B)}), players {A[-1]["id"]} and {B[-1]["id"]}')
    failures = 0
    order = ['link', 'link-place', 'link-life', 'link-gear', 'link-hands', 'link-fuse', 'link-dye', 'object', 'object-place', 'stale', 'dup', 'dead',
             'enemy-place', 'enemy-life', 'time', 'weather', 'enemy-only']
    for kind in order:
        if kind not in kinds:
            continue
        items = kinds[kind]
        n = sum(len(r) for _, r in items)
        if kind != 'enemy-only':
            failures += 1
        print(f'{kind}: {len(items)} subject(s), {n} snapshot(s)' + (' (not counted)' if kind == 'enemy-only' else ''))
        for subj, runs in sorted(items, key=lambda x: x[1][0][1]):
            for i, (_, t, text) in enumerate(runs if show_all else runs[:1]):
                print(f'  {t - t0:7.1f} s  {text}' + ('' if show_all or len(runs) == 1 else f'  (x{len(runs)})'))
    print('no mismatches' if not failures else f'{failures} kind(s) of mismatch')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
