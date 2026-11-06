"""Compares the enemies two games show, from their sync-audit snapshots (logs/audit.jsonl, coop_players.c audit_tick).

For each enemy both games show, snapshots are paired by time (the audit's "t" is the PC's clock, the same for two
games on one PC; within 0.5 s) and the differences in place and body shape (top: the highest bone above the root,
reach: the farthest bone from the root sideways) are summed up. How much it moved and how much its shape changed in
the game that ran it says whether there was anything to follow.

    python tools/coop/enemy_compare.py build/v143/logs/audit.jsonl build/v143b/logs/audit.jsonl
"""
import json
import math
import sys


def load(path):
    snaps = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            try:
                d = json.loads(line)
            except ValueError:
                continue
            snaps.append((d['t'], {e['key']: e for e in d.get('enemies', [])}))
    return snaps


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    keys = {}
    j = 0
    for t, ea in a:
        while j + 1 < len(b) and abs(b[j + 1][0] - t) <= abs(b[j][0] - t):
            j += 1
        if not b or abs(b[j][0] - t) > 0.5:
            continue
        eb = b[j][1]
        for k, x in ea.items():
            y = eb.get(k)
            if y and not x['dead'] and not y['dead']:
                keys.setdefault(k, []).append((t, x, y))
    if not keys:
        print('no enemy shown in both games at once')
    for k, rows in sorted(keys.items(), key=lambda kv: -len(kv[1])):
        name = rows[0][1]['name']
        dp = [math.dist(x['p'], y['p']) for _, x, y in rows]
        dtop = [abs(x.get('top', 0) - y.get('top', 0)) for _, x, y in rows]
        dreach = [abs(x.get('reach', 0) - y.get('reach', 0)) for _, x, y in rows]
        # the game that ran it: the one whose copy is not posed from the other's
        own = 'A' if sum(not x.get('pup') for _, x, _ in rows) >= sum(not y.get('pup') for _, _, y in rows) else 'B'
        src = [x if own == 'A' else y for _, x, y in rows]
        moved = sum(math.dist(src[i]['p'], src[i - 1]['p']) for i in range(1, len(src)))
        tops = [s.get('top', 0) for s in src]
        posed = sum((y if own == 'A' else x).get('pup', 0) for _, x, y in rows)
        print(f'{name} {k:08x}: {len(rows)} pairs, run by {own} (posed in the other {posed} of them); moved {moved:.1f} m, '
              f'top {min(tops):.2f}..{max(tops):.2f} m')
        print(f'    place off: mean {sum(dp) / len(dp):.2f} m, max {max(dp):.2f} m; top off: mean {sum(dtop) / len(dtop):.3f}, '
              f'max {max(dtop):.3f} m; reach off: mean {sum(dreach) / len(dreach):.3f}, max {max(dreach):.3f} m')


if __name__ == '__main__':
    main()
