"""Static checks of the co-op runtime that need no game (docs/coop/README.md, rule 7 and the review of 2026-11-17).

    python tests/coop_static_check.py

1. Every reader of the transport's inbox (runtime/coop_msg.h COOP_MSG_*) is opened somewhere (coop_msg_open): an
   unopened reader's messages are taken and dropped by the other reader, which is how the entity sync lost every
   reliable message once the enemy sync was on by default.
2. Every hooked address in runtime/coop_hooks.h is main+offset written as 0x70 and eight hex digits, and none is listed
   twice (an extra digit never matches; three hooks were dead that way).
3. Each TOTK_COOP_TEST_* setting the runtime reads is set by a script in tools/coop (rule 6): listed, not failed.
Exit 1 when check 1 or 2 fails.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rt = root / 'runtime'
fail = 0

src = {p: p.read_text(encoding='utf-8', errors='replace') for p in list(rt.glob('coop_*.c')) + list((rt / 'ui').glob('coop_*.cpp'))}

readers = re.findall(r'\b(COOP_MSG_[A-Z]+)\b', (rt / 'coop_msg.h').read_text(encoding='utf-8'))
readers = sorted({r for r in readers if r != 'COOP_MSG_READERS'})
for r in readers:
    if not any(re.search(r'coop_msg_open\(\s*' + r + r'\s*\)', s) for s in src.values()):
        print(f'FAIL reader {r} is never opened (coop_msg_open)')
        fail = 1
print(f'readers: {len(readers)} checked')

hooks = (rt / 'coop_hooks.h').read_text(encoding='utf-8')
addrs = re.findall(r'\(a\) == (0x[0-9a-fA-F]+)ull', hooks)
bad = [a for a in addrs if not re.fullmatch(r'0x70[0-9a-f]{8}', a)]
dups = sorted({a for a in addrs if addrs.count(a) > 1})
for a in bad:
    print(f'FAIL hook address {a} is not 0x70 and eight hex digits (main+0 to main+0xffffffff)')
for a in dups:
    print(f'FAIL hook address {a} is listed twice')
fail |= bool(bad or dups)
print(f'hook addresses: {len(addrs)} checked')

read = set()
for s in src.values():
    read |= set(re.findall(r'cfg_get\("(TOTK_COOP_TEST_[A-Z0-9_]+)"\)', s))
scripts = ''.join(p.read_text(encoding='utf-8', errors='replace') for p in (root / 'tools' / 'coop').iterdir() if p.is_file())
unset = sorted(n for n in read if n not in scripts)
print(f'test settings read: {len(read)}; set by no script in tools/coop: {", ".join(unset) if unset else "none"}')
sys.exit(fail)
