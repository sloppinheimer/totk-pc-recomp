"""Find where code of one game version sits in another: for each address of version A (a function, or a place inside
one), the address in version B, matched function by function.

    .venv\\Scripts\\python tools\\coop\\xver_match.py --a build/extracted/v121-exefs/exefs/main \\
        --b build/extracted/v143/exefs/main --targets targets.txt --out matches.tsv

targets.txt: one main offset (hex) per line, optionally a tab and a label. Made to find, in 1.4.3, the game functions
and hook sites the TotK Online mod uses in 1.2.1 (its offsets are for 1.2.1 only); it reads only the game's
executables and writes addresses.

How: functions are the unwind table's (.eh_frame_hdr, one per FDE). Between 1.2.1 and 1.4.3 the code moved and was
compiled anew (registers and order changed), so bytes alone match little. Functions are matched, in rounds, by
  1. the same set of C strings they load (ADRP + ADD of one register into .rodata), when that set is unique in
     each version;
  2. the same code with what moves masked out (ADRP pages, ADD and load/store offsets, branch targets), unique;
  3. the call graph: two matched functions with the same number of calls pair their callees in order, and two
     matched callees whose callers each have one caller of a size within 25% pair those callers.
A place inside a function is found by lining up the two functions' instruction kinds (difflib). Each output line
says how its function was matched (strings / code / calls / callers) and the alignment's share of matching kinds.
"""
import argparse
import difflib
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'recomp'))
import nso  # noqa: E402


class Mod:
    def __init__(self, path):
        m = nso.Nso(str(path))
        self.m = m
        base, size = m.text
        self.base = base
        self.w = np.frombuffer(bytes(m.image[base:base + size // 4 * 4]), dtype='<u4')
        self.funcs = self._fdes()
        self.starts = np.array([s for s, _ in self.funcs], dtype=np.int64)
        self.ends = np.array([s + n for s, n in self.funcs], dtype=np.int64)
        self._refs()

    def _fdes(self):
        img, h = self.m.image, self.m.eh_start
        assert img[h] == 1 and img[h + 1] == 0x1b and img[h + 2] == 0x03 and img[h + 3] == 0x3b
        n = struct.unpack_from('<I', img, h + 8)[0]
        out = []
        for i in range(n):
            loc, fde = struct.unpack_from('<ii', img, h + 12 + 8 * i)
            start, fde = h + loc, h + fde
            # FDE: length, CIE pointer, pc_begin (pcrel sdata4), pc_range (udata4) as the CIE's 'zR' encoding 0x1b says
            rng = struct.unpack_from('<I', img, fde + 12)[0]
            if self.m.text[0] <= start < self.m.text[0] + self.m.text[1] and rng:
                out.append((start, rng))
        out.sort()
        return out

    def _refs(self):
        w = self.w
        n = len(w)
        idx = np.arange(n, dtype=np.int64)
        pc = self.base + idx * 4
        # BL
        bl = (w & 0xfc000000) == 0x94000000
        imm = (w & 0x03ffffff).astype(np.int64)
        imm = np.where(imm & 0x02000000, imm - 0x04000000, imm)
        self.bl_at = idx[bl]
        self.bl_to = pc[bl] + imm[bl] * 4
        # ADRP + ADD (same register) and ADRP + LDR/STR, next instruction
        adrp = (w & 0x9f000000) == 0x90000000
        rd = (w & 0x1f).astype(np.int64)
        immlo = ((w >> 29) & 3).astype(np.int64)
        immhi = ((w >> 5) & 0x7ffff).astype(np.int64)
        pimm = (immhi << 2) | immlo
        pimm = np.where(pimm & (1 << 20), pimm - (1 << 21), pimm)
        page = (pc & ~0xfff) + (pimm << 12)
        nxt = np.roll(w, -1)
        add = ((nxt & 0xffc00000) == 0x91000000) & (((nxt >> 5) & 0x1f) == rd)
        tgt = page + ((nxt >> 10) & 0xfff).astype(np.int64)
        sel = adrp & add
        self.str_at = idx[sel]
        self.str_to = tgt[sel]
        ro0, ro1 = self.m.rodata[0], self.m.rodata[0] + self.m.rodata[1]
        keep = (self.str_to >= ro0) & (self.str_to < ro1)
        self.str_at, self.str_to = self.str_at[keep], self.str_to[keep]

    def func_of(self, off):
        i = int(np.searchsorted(self.starts, off, side='right')) - 1
        return i if i >= 0 and off < self.ends[i] else -1

    def cstr(self, off):
        img = self.m.image
        e = img.find(b'\0', off, off + 200)
        if e < 0 or e - off < 4:
            return None
        s = bytes(img[off:e])
        return s if all(32 <= c < 127 for c in s) else None

    def per_func(self):
        """Per function index: strings loaded, calls in order (target function indices)."""
        fi_str = defaultdict(set)
        fidx = np.searchsorted(self.starts, self.base + self.str_at * 4, side='right') - 1
        for f, t in zip(fidx, self.str_to):
            if f >= 0:
                s = self.cstr(int(t))
                if s:
                    fi_str[int(f)].add(s)
        calls = defaultdict(list)
        fat = np.searchsorted(self.starts, self.base + self.bl_at * 4, side='right') - 1
        fto = np.searchsorted(self.starts, self.bl_to, side='right') - 1
        for a, b, t in zip(fat, fto, self.bl_to):
            if a >= 0 and b >= 0 and self.starts[b] == t:
                calls[int(a)].append(int(b))
        return fi_str, calls

    def norm(self, i):
        s, e = (self.starts[i] - self.base) // 4, (self.ends[i] - self.base) // 4
        w = self.w[s:e].copy()
        w[(w & 0x9f000000) == 0x90000000] &= 0x9f00001f
        w[(w & 0x7c000000) == 0x14000000] &= 0xfc000000
        w[(w & 0x7f800000) == 0x11000000] &= 0xffc003ff
        w[(w & 0x3b000000) == 0x39000000] &= 0xffc003ff
        w[(w & 0x3b000000) == 0x18000000] &= 0xff00001f
        return w

    def kinds(self, i):
        """Instruction kinds of a function (top opcode bits, registers dropped) for aligning places inside it."""
        s, e = (self.starts[i] - self.base) // 4, (self.ends[i] - self.base) // 4
        return [int(x) for x in (self.w[s:e] >> 21)]


def match(A, B):
    sa, ca = A.per_func()
    sb, cb = B.per_func()
    pair, how = {}, {}
    used = set()

    def put(i, j, why):
        if i in pair or j in used:
            return False
        pair[i], how[i] = j, why
        used.add(j)
        return True

    # 1. string sets
    ka = Counter(frozenset(v) for v in sa.values())
    kb = Counter(frozenset(v) for v in sb.values())
    ib = {frozenset(v): k for k, v in sb.items() if kb[frozenset(v)] == 1}
    for i, v in sa.items():
        f = frozenset(v)
        if ka[f] == 1 and f in ib:
            put(i, ib[f], 'strings')
    print(f'  strings: {len(pair)}')
    # 2. masked code, unique, for functions of 8+ instructions
    ha = defaultdict(list)
    for i in range(len(A.starts)):
        if A.ends[i] - A.starts[i] >= 32:
            ha[A.norm(i).tobytes()].append(i)
    hb = defaultdict(list)
    for j in range(len(B.starts)):
        if B.ends[j] - B.starts[j] >= 32:
            hb[B.norm(j).tobytes()].append(j)
    n0 = len(pair)
    for k, v in ha.items():
        if len(v) == 1 and len(hb.get(k, ())) == 1:
            put(v[0], hb[k][0], 'code')
    print(f'  code: {len(pair) - n0}')
    # 2b. instruction kinds (registers and fields dropped), unique, for functions of 12+ instructions: the compiler
    # chose other registers in places between versions
    ha, hb = defaultdict(list), defaultdict(list)
    for i in range(len(A.starts)):
        if i not in pair and A.ends[i] - A.starts[i] >= 48:
            ha[(A.w[(A.starts[i] - A.base) // 4:(A.ends[i] - A.base) // 4] >> 24).tobytes()].append(i)
    for j in range(len(B.starts)):
        if j not in used and B.ends[j] - B.starts[j] >= 48:
            hb[(B.w[(B.starts[j] - B.base) // 4:(B.ends[j] - B.base) // 4] >> 24).tobytes()].append(j)
    n0 = len(pair)
    for k, v in ha.items():
        if len(v) == 1 and len(hb.get(k, ())) == 1:
            put(v[0], hb[k][0], 'shape')
    print(f'  shape: {len(pair) - n0}')
    # 3. call graph, in rounds
    callers_a, callers_b = defaultdict(set), defaultdict(set)
    for i, v in ca.items():
        for t in v:
            callers_a[t].add(i)
    for j, v in cb.items():
        for t in v:
            callers_b[t].add(j)
    size = lambda M, i: M.ends[i] - M.starts[i]
    for rnd in range(30):
        n0 = len(pair)
        for i, j in list(pair.items()):
            x, y = ca.get(i, []), cb.get(j, [])
            if len(x) == len(y) and x:
                for p, q in zip(x, y):
                    if 0.6 < size(A, p) / max(size(B, q), 1) < 1.6:
                        put(p, q, 'calls')
            elif x and y:
                # counts differ: matched callees are anchors; one unmatched call alone between two anchors (or the
                # ends) on each side pairs with the other
                ta = [('m', pair[p]) if p in pair else ('u', p) for p in x]
                tb = [('m', q) if q in used else ('u', q) for q in y]
                ka = [t[1] if t[0] == 'm' else None for t in ta]
                kb = [t[1] if t[0] == 'm' else None for t in tb]
                pos_b = {}
                for k, v in enumerate(kb):
                    if v is not None:
                        pos_b.setdefault(v, k)
                last_b = -1
                seq = []
                for k, v in enumerate(ka):
                    if v is not None and v in pos_b and pos_b[v] > last_b:
                        seq.append((k, pos_b[v]))
                        last_b = pos_b[v]
                seq = [(-1, -1)] + seq + [(len(ka), len(kb))]
                for (a0, b0), (a1, b1) in zip(seq, seq[1:]):
                    ga = [x[k] for k in range(a0 + 1, a1) if ka[k] is None]
                    gb = [y[k] for k in range(b0 + 1, b1) if kb[k] is None]
                    if len(ga) == 1 and len(gb) == 1 and 0.6 < size(A, ga[0]) / max(size(B, gb[0]), 1) < 1.6:
                        put(ga[0], gb[0], 'calls')
            ua = [c for c in callers_a.get(i, ()) if c not in pair]
            ub = [c for c in callers_b.get(j, ()) if c not in used]
            if len(ua) == 1 and len(ub) == 1 and 0.75 < size(A, ua[0]) / max(size(B, ub[0]), 1) < 1.33:
                put(ua[0], ub[0], 'callers')
        print(f'  round {rnd}: +{len(pair) - n0}')
        if len(pair) == n0:
            break
    return pair, how, used, ca, cb, callers_a, callers_b


def guess(A, B, i, pair, used, ca, cb, cra, crb):
    """An unmatched function: the B function (not matched yet) sharing the most matched neighbours (its callees' and
    callers' partners), of a size within a factor 2. Taken only with a lead of 2 or more over the next candidate;
    'guess k/n' says how many of its n matched neighbours agree."""
    want_callees = {pair[c] for c in ca.get(i, []) if c in pair}
    want_callers = {pair[c] for c in cra.get(i, ()) if c in pair}
    n = len(want_callees) + len(want_callers)
    if not n:
        return None, ' (no matched neighbours)'
    score = Counter()
    for q in want_callers:          # B functions those callers call
        for t in set(cb.get(q, [])):
            score[t] += 1
    for q in want_callees:          # B functions calling those callees
        for t in crb.get(q, ()):
            score[t] += 1
    size = lambda M, k: M.ends[k] - M.starts[k]
    cands = sorted(((v, t) for t, v in score.items() if t not in used and 0.5 < size(A, i) / max(size(B, t), 1) < 2),
                   reverse=True)
    if not cands:
        return None, f' (no candidate; {n} matched neighbours)'
    if cands[0][0] < 2 or (len(cands) > 1 and cands[1][0] > cands[0][0] - 2):
        return None, (f' (ambiguous: {cands[0][0]} vs {cands[1][0]} of {n}: '
                      f'{B.starts[cands[0][1]]:#x} / {B.starts[cands[1][1]]:#x})')
    return cands[0][1], f'guess {cands[0][0]}/{n}'


def place(A, B, i, j, off):
    """off inside A's function i -> the place in B's function j, and the share of matching kinds."""
    ka, kb = A.kinds(i), B.kinds(j)
    k = (off - A.starts[i]) // 4
    sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)
    for a, b, n in sm.get_matching_blocks():
        if a <= k < a + n:
            return B.starts[j] + (b + k - a) * 4, sm.ratio(), 'aligned'
    # not inside a matching block: nearest block before it
    best = None
    for a, b, n in sm.get_matching_blocks():
        if a + n <= k:
            best = (a, b, n)
    if best:
        a, b, n = best
        return B.starts[j] + (b + n - 1 + (k - (a + n - 1))) * 4, sm.ratio(), 'near'
    return B.starts[j] + k * 4, sm.ratio(), 'offset'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--a', required=True)
    ap.add_argument('--b', required=True)
    ap.add_argument('--targets', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    print('loading A'), sys.stdout.flush()
    A = Mod(args.a)
    print(f'  {len(A.starts)} functions'), print('loading B')
    B = Mod(args.b)
    print(f'  {len(B.starts)} functions'), print('matching')
    pair, how, used, ca, cb, cra, crb = match(A, B)
    print(f'  {len(pair)} of {len(A.starts)} functions matched')
    rows = ['a\tb\ta_func\tb_func\thow\tshape\tplace\tlabel']
    for line in Path(args.targets).read_text().splitlines():
        if not line.strip():
            continue
        s, _, label = line.partition('\t')
        off = int(s, 16)
        i = A.func_of(off)
        if i < 0:
            rows.append(f'{off:#x}\t-\t-\t-\tnot in a function (data?)\t\t\t{label}')
            continue
        j = pair.get(i)
        if j is None:
            j, why = guess(A, B, i, pair, used, ca, cb, cra, crb)
            if j is None:
                rows.append(f'{off:#x}\t-\t{A.starts[i]:#x}\t-\tfunction not matched{why}\t\t\t{label}')
                continue
            how[i], pair[i] = why, j
            used.add(j)   # (not guessed again for another target)
        b, ratio, kind = place(A, B, i, j, off)
        rows.append(f'{off:#x}\t{b:#x}\t{A.starts[i]:#x}\t{B.starts[j]:#x}\t{how[i]}\t{ratio:.2f}\t{kind}\t{label}')
    Path(args.out).write_text('\n'.join(rows) + '\n')
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
