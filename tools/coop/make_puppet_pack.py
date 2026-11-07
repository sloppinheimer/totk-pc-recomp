"""Build Pack/Actor/PlayerPuppet.pack.zs, a stand-in actor for the other players, into the RomFS overlay.

TotK Online (read for behaviour only; none of its files are used) shows each other player as an actor of its own
named PlayerPuppet: Link's model, animation set, equipment user and physics, but no Player component, so no player
state machine and no AI. Its game copies the owner's animation slots onto it and draws its gear with the game's own
in-hand calls; nothing in the actor fights them (docs/coop/proxy-plan.md, "Parity with TotK Online").

This pack is made here from the player's own Player.pack.zs: every member kept, plus
Actor/PlayerPuppet.engine__actor__ActorParam.bgyml, the Player's actor parameters with the category Enemy (as the
mod's) and without the components that make a Player act or that only one Link may have (PLAYER_ONLY below), and
Component/PerceptionEmitterParam/PlayerPuppet...bgyml, the Player's perception emitter made not resident (EMITTER below).

    .venv\\Scripts\\python tools\\coop\\make_puppet_pack.py [--version v143]

Input: build/extracted/<version>/romfs. Output: build/<version>/romfs-patches/Pack/Actor/PlayerPuppet.pack.zs and its
line in that folder's manifest.tsv (an added file: original "-"), and the actor tables RSDB/ActorInfo and GameActorInfo with the
row of an unused test actor made PlayerPuppet's (REPLACED). Run after tools/make_romfs_patches.py, which writes
the manifest anew.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import oead
import zstandard

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import byml  # noqa: E402

NAME = 'PlayerPuppet'
PACK = f'Pack/Actor/{NAME}.pack.zs'
# Components of the Player's actor parameters left out: the Player component and what drives it (its state machine,
# camera, riding, lifting, the special powers, its brain), what assumes the one Link (world info, game balance,
# events, path finding), and its life: the Player's life parameters crashed the stand-in's damage update
# (main+0x3185b0 from the hit update main+0x300a30, reading 0x30; 2026-11-07). TotK Online's gives it an NPC's.
PLAYER_ONLY = ('PlayerRef', 'PlayerCameraTargetRef', 'RiderRef', 'LifterRef', 'SpecialPowerRef', 'BSARef',
               'WorldInfoRef', 'GameBalanceClientRef', 'EventPerformerRef', 'PathFindingRef', 'PerimeterAnalyserRef',
               'ActiveCameraAlphaRef', 'AttentionReceiverRef', 'LifeRef')

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--version', choices=('v121', 'v143'), default='v143')
ap.add_argument('--out', type=Path, help='default build/<version>/romfs-patches')
ap.add_argument('--fields', default='InstanceHeapSize,DisplayRadius,LoadRadius',
                help='(a test) the fields of the Player ActorInfo row put on the stand-in')
ap.add_argument('--drop', default='', help='(a test) more components of the actor parameters left out, comma separated')
ap.add_argument('--resident-emitter', action='store_true',
                help="(a test, the old pack) keep the Player's perception emitter as it is: resident, which enemies ignore")
ap.add_argument('--emitter-sublayer', default='',
                help="(a test) the stand-in emitter's SubLayerSensor instead of the Player's PerceptionPlayer, e.g. PerceptionEnemy")
args = ap.parse_args()
src = root / 'build' / 'extracted' / args.version / 'romfs'
out_dir = args.out or root / 'build' / args.version / 'romfs-patches'
files = {f['path']: f for f in json.load(open(src / 'index.json'))['files']}
image = open(src / 'romfs.bin', 'rb')


def read(path):
    f = files[path]
    image.seek(f['offset'])
    return image.read(f['size'])


dicts = {}
for f in oead.Sarc(zstandard.ZstdDecompressor().decompress(read('Pack/ZsDic.pack.zs'), max_output_size=1 << 28)).get_files():
    d = zstandard.ZstdCompressionDict(bytes(f.data))
    dicts[d.dict_id()] = d
raw = read('Pack/Actor/Player.pack.zs')
dic = dicts[zstandard.get_frame_parameters(raw).dict_id]
pack = oead.Sarc(zstandard.ZstdDecompressor(dict_data=dic).decompress(raw, max_output_size=1 << 28))

param = byml.read(bytes(pack.get_file('Actor/Player.engine__actor__ActorParam.bgyml').data))
param['Category'] = 'Enemy'
gone = [k for k in PLAYER_ONLY + tuple(x for x in args.drop.split(',') if x) if param['Components'].pop(k, None) is not None]
w = oead.SarcWriter(endian=oead.Endianness.Little)
for f in pack.get_files():
    w.files[f.name] = bytes(f.data)
# The perception emitter. The Player's (and a Sage's) is resident (IsResidentEmitter): the perception manager keeps
# the one player emitter in a field of its own ([main+0x3a6ea80] +0x58, docs/coop/re-notes.md "Enemy targets: not
# found") that enemies check instead of sensing it, so a second actor with that emitter is never noticed (a second
# Player, a pigeon with the Player's or a Sage's emitter: proxy-plan.md). Enemies and horses have resident false (the
# enemies' EnemyCommon) and are sensed through their emitter's sensor sublayer. The stand-in gets the Player's
# emitter with resident false, so enemies sense it as they do any other actor and pick it by its affiliation (the
# Player's "Player", which a Bokoblin's TargetAffiliations hold).
EMITTER = f'Component/PerceptionEmitterParam/{NAME}.game__component__PerceptionEmitterParam.bgyml'
if not args.resident_emitter:
    ref = param['Components']['PerceptionEmitterRef']
    em = byml.read(bytes(pack.get_file(ref.lstrip('?')).data))
    assert em.get('IsResidentEmitter') is True, em
    em['IsResidentEmitter'] = False
    if args.emitter_sublayer: em['SubLayerSensor'] = args.emitter_sublayer
    new = byml.write(em)
    assert byml.same(byml.read(new), em)
    w.files[EMITTER] = new
    param['Components']['PerceptionEmitterRef'] = '?' + EMITTER
new = byml.write(param)
assert byml.same(byml.read(new), param)
w.files[f'Actor/{NAME}.engine__actor__ActorParam.bgyml'] = new
_, data = w.write()
data = bytes(data)


def zs(data, like):
    """data compressed as the original file like was (with its dictionary, if any)."""
    d = dicts.get(zstandard.get_frame_parameters(like).dict_id)
    c = zstandard.ZstdCompressor(level=19, dict_data=d) if d else zstandard.ZstdCompressor(level=19)
    out = c.compress(data)
    assert (zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()).decompress(out) == data
    return out


def unzs(raw):
    d = dicts.get(zstandard.get_frame_parameters(raw).dict_id)
    return (zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()).decompress(raw, max_output_size=1 << 28)


# The actor tables (RSDB ActorInfo and GameActorInfo, one row per actor, the two in the same order: the game pairs
# their rows by place, so a row added to both crashed the load in the actor creator, main+0x514aac, 2026-11-07). An
# actor missing from them is refused at creation. So the rows of an unused test actor (REPLACED, in no map) become the
# stand-in's, in place: renamed, with the category of the actor parameters, the Player's instance heap size, display
# and load radii and an update radius that never ends (the
# whole Player row in ActorInfo crashed the same load). TotK Online turns a pigeon's row into its PlayerPuppet's the
# same way (read for behaviour only).
REPLACED = 'Npc_Hylia_TestTube_001'
outputs = {PACK: (zs(data, raw), '-')}
ver = args.version[1:]
for table in (f'RSDB/ActorInfo.Product.{ver}.rstbl.byml.zs', f'RSDB/GameActorInfo.Product.{ver}.rstbl.byml.zs'):
    traw = read(table)
    rows = byml.read(unzs(traw))
    ids = [r['__RowId'] for r in rows]
    row, player = dict(rows[ids.index(REPLACED)]), rows[ids.index('Player')]
    if 'GameActorInfo' not in table:
        for k in (x for x in args.fields.split(',') if x):
            if k in player: row[k] = player[k]
            else: row.pop(k, None)
        # Its update radius (CalcRadius): the test actor's 40 m would stop its update, and so its placing at its owner,
        # when its owner is 40 m from this game's Link. (The handover suite case's "went away (state 7)" was game A
        # falling off the island and reloading, 2026-11-07.) The stand-in is wherever its owner is: always updated.
        row['CalcRadius'] = 100000.0
    row['__RowId'] = NAME
    if 'Category' in row: row['Category'] = param['Category']
    rows[ids.index(REPLACED)] = row
    new = byml.write(rows)
    assert byml.same(byml.read(new), rows)
    outputs[table] = (zs(new, traw), hashlib.sha256(traw).hexdigest())

man = out_dir / 'manifest.tsv'
lines = [l for l in man.read_text(encoding='utf-8').splitlines() if l.split('\t')[0] not in outputs] if man.exists() else \
    ['# path\tsize\tsha256\toriginal_sha256 (tools/make_romfs_patches.py)']
for path, (z, orig) in outputs.items():
    target = out_dir / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(z)
    lines.append(f'{path}\t{len(z)}\t{hashlib.sha256(z).hexdigest()}\t{orig}')
man.write_text('\n'.join(lines) + '\n', encoding='utf-8')
print(json.dumps(dict(status='pass', files=list(outputs), members=len(w.files), left_out=gone,
                      emitter='resident (Player)' if args.resident_emitter else EMITTER)))
