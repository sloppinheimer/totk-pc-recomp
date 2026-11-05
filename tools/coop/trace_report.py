"""Turns a co-op frame trace (runtime/coop_trace.c, TOTK_COOP_TRACE_AT) into a readable report.

    python tools/coop/trace_report.py build/v143/logs/trace_0_20s_1234.csv [--out report.txt]

Writes <csv>.report.txt (the diff and the value changes) and <csv>.timeline.txt (every recorded call, per thread, in
order, indented by stack depth). "L" is this PC's Link, "C1".. the co-op Links; an object path reads
actor>+0x228>+0x8 = the object [actor+0x228] holds at +0x8.
"""
import argparse
import collections
import sys


def load(path):
    nodes, watches, fns, recs = {}, {}, {}, []
    head = ''
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('#'):
                head = line[2:]
                continue
            k, _, rest = line.partition(',')
            if k == 'N':
                p = rest.split(',', 8)
                nodes[int(p[0])] = dict(addr=int(p[1], 16), size=int(p[2], 16), link=int(p[3]), parent=int(p[4]),
                                        shared=int(p[5]), off=int(p[6], 16), kind=int(p[7]), name=p[8])
            elif k == 'W':
                p = rest.split(',', 4)
                watches[int(p[0])] = dict(link=int(p[1]), addr=int(p[2], 16), len=int(p[3]), what=p[4])
            elif k == 'F':
                a, _, d = rest.partition(',')
                fns[int(a, 16)] = d
            elif k in 'CV':
                p = rest.split(',')
                r = dict(type=k, us=float(p[0]), tid=int(p[1]), fn=int(p[2], 16), lr=int(p[3], 16), sp=int(p[4], 16), node=int(p[5]))
                if k == 'C':
                    r.update(off=int(p[6], 16), arg=int(p[7]), mask=int(p[8]))
                else:
                    r.update(v=(float(p[6]), float(p[7]), float(p[8])))
                recs.append(r)
    return head, nodes, watches, fns, recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--top', type=int, default=250)
    a = ap.parse_args()
    head, nodes, watches, fns, recs = load(a.csv)
    labels = {}

    def label(i):
        if i in labels:
            return labels[i]
        n = nodes.get(i)
        if n is None:
            return '?'
        if n['parent'] < 0:
            s = 'actor'
        elif n['kind'] == 1:
            s = n['name']
        else:
            s = '%s>+%#x' % (label(n['parent']), n['off'])
            if n['kind'] in (2, 3):
                s += ' (%s)' % n['name']
        labels[i] = s
        return s

    def who(link):
        return 'L' if link == 0 else 'C%d' % link

    def fname(x):
        return fns.get(x, '%#x' % x).replace('main+', 'm+')

    def obj(r):
        n = nodes[r['node']]
        return '%s%s:%s+%#x' % ('shared/' if n['shared'] else '', who(n['link']), label(r['node']), r['off'])

    out = open(a.csv + '.report.txt', 'w', encoding='utf-8')
    tl = open(a.csv + '.timeline.txt', 'w', encoding='utf-8')
    calls = [r for r in recs if r['type'] == 'C']
    changes = [r for r in recs if r['type'] == 'V']
    print(head, file=out)
    roots = [n for n in nodes.values() if n['parent'] < 0]
    for n in roots:
        print('  %s = %s at %x' % (who(n['link']), n['name'], n['addr']), file=out)
    threads = collections.Counter(r['tid'] for r in calls)
    print('\n%d calls on Link objects, %d value changes, %d threads:' % (len(calls), len(changes), len(threads)), file=out)
    for tid, c in threads.most_common():
        ts = [r['us'] for r in calls if r['tid'] == tid]
        links = collections.Counter(who(nodes[r['node']]['link']) for r in calls if r['tid'] == tid and not nodes[r['node']]['shared'])
        print('  thread %6d: %7d calls, %8.1f..%8.1f us, %s' % (tid, c, min(ts), max(ts), dict(links)), file=out)

    # The value changes: what was entered when each was seen, and the last recorded call before it on any thread.
    print('\n== Watched values: when they changed (seen at the next recorded call; the call before it on any thread is\n'
          '   the nearest candidate writer)', file=out)
    by_t = sorted(recs, key=lambda r: r['us'])
    last_call = None
    prev_on = {}
    for r in by_t:
        if r['type'] == 'C':
            last_call = r
            prev_on[r['tid']] = r
            continue
        w = watches[r['node']]
        before = prev_on.get(r['tid'])
        print('  %8.1f us %s %-14s = %9.3f %9.3f %9.3f  seen entering %s on thread %d; before it there: %s; last call anywhere: %s' % (
            r['us'], who(w['link']), w['what'], r['v'][0], r['v'][1], r['v'][2], fname(r['fn']), r['tid'],
            fname(before['fn']) + ' on ' + obj(before) if before else '-',
            fname(last_call['fn']) + ' on ' + obj(last_call) + ' (thread %d)' % last_call['tid'] if last_call else '-'), file=out)

    # The diff: (function, object path, argument) counted for this PC's Link and for the co-op Links.
    agg = collections.defaultdict(lambda: [0, 0, None])
    for r in calls:
        n = nodes[r['node']]
        if n['shared']:
            continue
        k = (r['fn'], label(r['node']), r['arg'])
        e = agg[k]
        e[0 if n['link'] == 0 else 1] += 1
        if e[2] is None:
            e[2] = r['lr']
    ncoop = max(1, len({nodes[r['node']]['link'] for r in calls if nodes[r['node']]['link'] > 0}))
    only_l = sorted(((v[0], k, v) for k, v in agg.items() if v[0] and not v[1]), key=lambda x: -x[0])
    only_c = sorted(((v[1], k, v) for k, v in agg.items() if v[1] and not v[0]), key=lambda x: -x[0])
    both = sorted(((v[0] - v[1] / ncoop, k, v) for k, v in agg.items() if v[0] and v[1] and v[0] != v[1] / ncoop),
                  key=lambda x: -abs(x[0]))

    def table(title, rows):
        print('\n== %s (%d)' % (title, len(rows)), file=out)
        for _, (fn, lab, arg), v in rows[:a.top]:
            print('  L %5d  C %5d  %-55s x%d=%-40s from %s' % (v[0], v[1], fname(fn), arg, lab, fname(v[2])), file=out)
        if len(rows) > a.top:
            print('  ... %d more' % (len(rows) - a.top), file=out)
    table('Called for this PC\'s Link only', only_l)
    table('Called for the co-op Link only', only_c)
    table('Called for both, a different number of times', both)
    # The display chain on its own: every function that touched the model, a skeleton or its buffers, or the
    # model's component, for both Links (equal counts included), in the order they first ran.
    first = {}
    for r in calls:
        lab = label(r['node'])
        if lab.startswith(('model', 'skel', 'comp+0x8')) and not nodes[r['node']]['shared']:
            first.setdefault((r['fn'], lab, r['arg']), r['us'])
    print('\n== The display chain (model, skeletons, comp+0x8), in first-run order (%d)' % len(first), file=out)
    for k, us in sorted(first.items(), key=lambda x: x[1]):
        v = agg[k]
        print('  %8.1f us  L %5d  C %5d  %-55s x%d=%-40s from %s' % (us, v[0], v[1], fname(k[0]), k[2], k[1], fname(v[2])), file=out)
    same = sum(1 for v in agg.values() if v[0] and v[1] and v[0] == v[1] / ncoop)
    print('\n%d (function, object) pairs ran the same number of times for both.' % same, file=out)

    # The timeline, per thread, indented by stack depth (a smaller sp is deeper), repeats folded.
    for tid, _ in threads.most_common():
        print('\n==== thread %d' % tid, file=tl)
        stack, prev, rep = [], None, 0
        for r in (x for x in by_t if x['tid'] == tid):
            while stack and r['sp'] >= stack[-1]:
                stack.pop()
            depth = len(stack)
            stack.append(r['sp'])
            if r['type'] == 'V':
                w = watches[r['node']]
                print('%9.1f %s** %s %s changed: %.3f %.3f %.3f' % (r['us'], '  ' * depth, who(w['link']), w['what'], *r['v']), file=tl)
                prev = None
                continue
            key = (r['fn'], r['node'], r['off'], depth)
            if key == prev:
                rep += 1
                continue
            if rep:
                print('%9s %s   (x%d more)' % ('', '  ' * depth, rep), file=tl)
            rep, prev = 0, key
            print('%9.1f %s%s  x%d=%s  <- %s' % (r['us'], '  ' * min(depth, 40), fname(r['fn']), r['arg'], obj(r), fname(r['lr'])), file=tl)
    out.close()
    tl.close()
    print('wrote', a.csv + '.report.txt', 'and', a.csv + '.timeline.txt')


if __name__ == '__main__':
    sys.exit(main())
