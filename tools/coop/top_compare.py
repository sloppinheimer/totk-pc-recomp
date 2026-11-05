"""Compares a Link in one game with its proxy (or puppet) in the other, from TOTK_COOP_TOP_LOG lines.

    python tools/coop/top_compare.py build/v143/logs/totk.log build/v143b/logs/totk.log

The first log has the owner ("coop: top mine <s> <top> <x> <y> <z>"), the second its copy ("coop: top puppet ...").
Each game's world clock starts at its own load, so the copy's times are shifted by the offset that makes the two
paths agree best (searched from -10 to +10 s; the copy is shown behind its owner on purpose, so the offset includes
that delay). Reports, at that offset: the position error and the body-top difference (the highest body bone above
the root bone, from the skeleton's +0x20 matrices), mean and largest; and shake: how much each frame's step of the copy
differs from its owner's step over the same moment (body top, root bone height, position), over all frames and
while the owner stands still.
"""
import bisect
import math
import re
import sys

LINE = re.compile(r"coop: top (mine|puppet) ([-\d.]+) ([-\d.e]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)")


def read(path, who):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = LINE.search(ln)
            if m and m.group(1) == who:
                out.append(tuple(float(v) for v in m.groups()[1:]))   # t, top, x, y, z, root
    out.sort()
    return out


def at(series, times, t):
    """The owner's sample at time t, linearly between the two around it; None outside the series."""
    i = bisect.bisect_left(times, t)
    if i == 0 or i >= len(series):
        return None
    a, b = series[i - 1], series[i]
    f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0
    return tuple(a[k] + (b[k] - a[k]) * f for k in range(6))


def errors(owner, times, copy, off):
    pe, te = [], []
    for c in copy:
        o = at(owner, times, c[0] + off)
        if o is None:
            continue
        pe.append((math.dist(c[2:5], o[2:5]), c[0]))
        te.append((abs(c[1] - o[1]), c[0]))
    return pe, te


def main():
    owner, copy = read(sys.argv[1], "mine"), read(sys.argv[2], "puppet")
    if not owner or not copy:
        print(f"no samples: owner {len(owner)}, copy {len(copy)}")
        return 1
    times = [o[0] for o in owner]
    best = None
    for k in range(-1000, 1001):
        off = k / 100
        pe, _ = errors(owner, times, copy, off)
        if len(pe) < len(copy) / 2:
            continue
        score = sum(e for e, _ in pe) / len(pe)
        if best is None or score < best[0]:
            best = (score, off)
    if best is None:
        print("the two logs do not overlap")
        return 1
    off = best[1]
    pe, te = errors(owner, times, copy, off)
    print(f"owner {len(owner)} frames, copy {len(copy)} frames; copy time + {off:.2f} s = owner time")
    for name, e in ("position error", pe), ("body top difference", te):
        worst = sorted(e, reverse=True)[:3]
        print(f"{name}: mean {sum(v for v, _ in e) / len(e) * 100:.1f} cm, max {worst[0][0] * 100:.1f} cm; worst at copy time "
              + ", ".join(f"{t:.2f} s ({v * 100:.1f})" for v, t in worst))
    # Shake: each frame, how far the copy's step differs from its owner's over the same moment (body top above the
    # root, the root bone's height, the position). Real motion moves both alike; a jump of the copy's own does not.
    # "standing": the owner did not move (position under 1 mm over the 0.2 s around it, its body top and root bone
    # under 2 mm that frame: running into a wall keeps the position but not the body).
    rows = []
    for a, b in zip(copy, copy[1:]):
        oa, ob = at(owner, times, a[0] + off), at(owner, times, b[0] + off)
        if oa is None or ob is None:
            continue
        ex = max(abs((b[1] - a[1]) - (ob[1] - oa[1])), abs((b[5] - a[5]) - (ob[5] - oa[5])),
                 math.dist([b[k] - a[k] for k in (2, 3, 4)], [ob[k] - oa[k] for k in (2, 3, 4)]))
        o0, o1 = at(owner, times, a[0] + off - 0.1), at(owner, times, b[0] + off + 0.1)
        still = (o0 is not None and o1 is not None and math.dist(o0[2:5], o1[2:5]) < 0.001
                 and abs(ob[1] - oa[1]) < 0.002 and abs(ob[5] - oa[5]) < 0.002)
        rows.append((ex, still, b[0]))
    for name, sel in ("all frames", rows), ("standing", [r for r in rows if r[1]]):
        if not sel:
            print(f"{name}: none")
            continue
        worst = sorted(sel, reverse=True)[:3]
        big = sum(1 for r in sel if r[0] > 0.01)
        print(f"{name}: {len(sel)} frame steps, copy's step off its owner's by more than 1 cm in {big}; largest "
              + ", ".join(f"{r[0] * 100:.2f} cm at {r[2]:.2f} s" for r in worst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
