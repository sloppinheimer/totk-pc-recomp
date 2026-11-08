"""Character mods for co-op: each player's stand-in drawn as that player's character (docs/coop/character-mods.md).

A character mod (e.g. GameBanana's Playable Riju Mod) replaces Link's body, head and textures under Link's own file
names, so as a plain mod it turns every Link in a game into that character. Here each character gets a copy of those
files under names of its own, its "set", so that one game can show several characters at once:

- the stand-in actor PlayerPuppet_<tag> (the character's Player.pack, as tools/coop/make_puppet_pack.py makes
  PlayerPuppet from the game's), its body model Model/<tag>.<fmdb>.bfres.mc and its animation set AS/<actor>.root.asb;
- the bare head and body pieces Link wears without armor (BARE below) as Armor_..._<tag> actors with their models;
- the files the character mod adds (new names: its animations, textures, AI), so that a game where the mod is off has
  what the stand-in needs.
The models' textures keep their names, so a texture the mod replaces (Link_RaulSkin_Alb...) is the one of the game the
stand-in is shown in: renaming them inside the model made the game refuse the actor (docs/coop/character-mods.md).

The tag is four letters and digits made from the character's key ("Link" for the game's own Link, else the mod's
GameBanana file id or its folder name). Link (the game's files) always gets a set too: a game whose own Link is a
character shows another player's Link through it.

All sets go into one mod folder, mods/zz-coop-characters (written whole every time, with characters.json listing the
characters, their keys, tags and actor names; runtime/coop_chars.c reads it). The actor tables (RSDB ActorInfo and
GameActorInfo) are the game's (or the RomFS overlay's, which already has PlayerPuppet) with a row per new actor,
inserted in name order in both.

    python tools/coop/charmod.py sets --mods build/v143/mods [--romfs build/extracted/v143/romfs]
    python tools/coop/charmod.py info <GameBanana link or mod id>
    python tools/coop/charmod.py fetch <GameBanana file id> --mods build/v143/mods [--yes]
    python tools/coop/charmod.py list --mods build/v143/mods

fetch downloads one file of a GameBanana mod, checks its MD5 against GameBanana's, refuses archives with code patches
(exefs, .ips, .pchtxt, subsdk, main.npdm) or with a path that would leave the mod folder, and unpacks only the files under
romfs into mods/<name>/romfs, with a coop-character-off file: runtime/mods.c keeps such a folder off until the menu
switches it, so it is only another player's character until the Co-op tab makes it this PC's. Without
--yes it asks on the console first; the game's menu asks the player and passes --yes.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path, PurePosixPath

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

SETS = 'zz-coop-characters'
# Folders of a character mod's added files that go into its set (not RSDB, GameData, UI, events: the mod's game changes).
ADDED_KINDS = ('Model', 'TexToGo', 'AI', 'AnimationEvent', 'Effect', 'Phive', 'Shader', 'Component')
INFO = 'coop-character.json'   # in a character mod's folder: where it came from (written by fetch)
# What Link wears with no armor in a slot: the stand-in of another character wears its own of these instead of its owner's
# armor, which this game only has fitted to this game's own character (docs/coop/character-mods.md, "Armor").
BARE = ('Armor_999_Head', 'Armor_Default_Upper', 'Armor_Default_Lower')
# The body model's second model (the right hand Link's gear is bound to).
BODY_EXTRA = ('Link_RaulSkin_RightHand',)
# Components of the Player's actor parameters the stand-in does without (as make_puppet_pack.py's PLAYER_ONLY).
PLAYER_ONLY = ('PlayerRef', 'PlayerCameraTargetRef', 'RiderRef', 'LifterRef', 'SpecialPowerRef', 'BSARef',
               'WorldInfoRef', 'GameBalanceClientRef', 'EventPerformerRef', 'PathFindingRef', 'PerimeterAnalyserRef',
               'ActiveCameraAlphaRef', 'AttentionReceiverRef', 'LifeRef', 'AIInfoRef')
API = 'https://gamebanana.com/apiv11'
UA = 'totk-recomp-coop/1 (character mods)'


def tag_of(key):
    """Four characters, a letter first, for a character key."""
    if key == 'Link': return 'LnkV'
    h = hashlib.sha256(key.encode('utf-8')).digest()
    abc = 'ABCDEFGHJKLMNPQRSTUVWXYZ'
    al = abc + '23456789'
    return abc[h[0] % len(abc)] + ''.join(al[b % len(al)] for b in h[1:4])


# ---- the game's files


class Rom:
    """The extracted RomFS (romfs.bin + index.json) with the zstd dictionaries; files read by path."""

    def __init__(self, src):
        import oead
        import zstandard
        self.oead, self.zstd = oead, zstandard
        self.files = {f['path']: f for f in json.load(open(src / 'index.json'))['files']}
        self.image = open(src / 'romfs.bin', 'rb')
        self.dicts = {}
        for f in oead.Sarc(zstandard.ZstdDecompressor().decompress(self.read('Pack/ZsDic.pack.zs'),
                                                                   max_output_size=1 << 28)).get_files():
            d = zstandard.ZstdCompressionDict(bytes(f.data))
            self.dicts[d.dict_id()] = d

    def has(self, path): return path in self.files

    def read(self, path):
        f = self.files[path]
        self.image.seek(f['offset'])
        return self.image.read(f['size'])

    def unzs(self, raw):
        d = self.dicts.get(self.zstd.get_frame_parameters(raw).dict_id)
        return (self.zstd.ZstdDecompressor(dict_data=d) if d else self.zstd.ZstdDecompressor()).decompress(
            raw, max_output_size=1 << 30)

    def zs(self, data, like):
        """data compressed as like was (with its dictionary, if any)."""
        d = self.dicts.get(self.zstd.get_frame_parameters(like).dict_id)
        c = self.zstd.ZstdCompressor(level=19, dict_data=d) if d else self.zstd.ZstdCompressor(level=19)
        out = c.compress(data)
        assert self.unzs(out) == data
        return out


class Source:
    """A character's files: its mod's RomFS folder first, then the game's (overlay first, then the RomFS image)."""

    def __init__(self, rom, overlay, mod_romfs=None):
        self.rom, self.overlay, self.mod = rom, overlay, mod_romfs

    def get(self, path):
        for d in (self.mod, self.overlay):
            if d and (d / path).is_file(): return (d / path).read_bytes()
        return self.rom.read(path) if self.rom.has(path) else None

    def from_mod(self, path): return bool(self.mod and (self.mod / path).is_file())


# ---- byml (tools/coop/byml.py) and SARC (oead)


def byml_mod():
    import byml
    return byml


def pack_files(rom, raw):
    return {f.name: bytes(f.data) for f in rom.oead.Sarc(rom.unzs(raw)).get_files()}


def pack_write(rom, members):
    w = rom.oead.SarcWriter(endian=rom.oead.Endianness.Little)
    for k, v in members.items(): w.files[k] = v
    return bytes(w.write()[1])


# ---- a character's set


def build_set(rom, src, key, out, log):
    """Writes the character's set into out (a RomFS folder); returns its entry for characters.json."""
    byml = byml_mod()
    tag = tag_of(key)
    files = {}   # RomFS path -> bytes

    def model(project, fmdb):
        """Copies Model/<project>.<fmdb>.bfres.mc as the tag's project (the file as it is: the model names its
        textures, which stay this game's, docs/coop/character-mods.md "Textures"); returns the new project name."""
        raw = src.get(f'Model/{project}.{fmdb}.bfres.mc')
        if raw is None: return None
        newp = tag + project[4:] if len(project) > 4 else tag
        files[f'Model/{newp}.{fmdb}.bfres.mc'] = raw
        return newp

    rows = []   # (new actor name, the actor whose rows it copies, ModelProjectName, FmdbName)

    # The stand-in: the character's Player.pack, as PlayerPuppet_<tag>.
    actor = f'PlayerPuppet_{tag}'
    raw = src.get('Pack/Actor/Player.pack.zs')
    members = pack_files(rom, raw)
    param = byml.read(members['Actor/Player.engine__actor__ActorParam.bgyml'])
    param['Category'] = 'Enemy'
    for k in PLAYER_ONLY: param['Components'].pop(k, None)
    comps = param['Components']
    # Its own members under its own names (a member path two packs share is one resource to the game, the first loaded).
    for k, ref in list(comps.items()):
        if not isinstance(ref, str) or not ref.startswith('?'): continue
        path = ref[1:]
        name = PurePosixPath(path).name
        if path in members and name.startswith('Player'):
            new = str(PurePosixPath(path).with_name(name.replace('Player', actor, 1)))
            members[new] = members.pop(path)
            comps[k] = '?' + new
    mi_path = comps['ModelInfoRef'][1:]
    mi = byml.read(members[mi_path])
    newp = model(mi['ModelProjectName'], mi['FmdbName'])
    assert newp, f'{key}: no body model {mi["ModelProjectName"]}.{mi["FmdbName"]}'
    for extra in BODY_EXTRA: model(mi['ModelProjectName'], extra)
    rows.append((actor, 'PlayerPuppet', newp, mi['FmdbName']))
    mi['ModelProjectName'] = newp
    members[mi_path] = byml.write(mi)
    # Its animation set, under its own name.
    asb = src.get('AS/Player.root.asb.zs')
    if asb is not None and comps.get('ASRef') == '?AS/Player.root.asb':
        files[f'AS/{actor}.root.asb.zs'] = asb
        comps['ASRef'] = f'?AS/{actor}.root.asb'
    members.pop('Actor/Player.engine__actor__ActorParam.bgyml')
    members[f'Actor/{actor}.engine__actor__ActorParam.bgyml'] = byml.write(param)
    files[f'Pack/Actor/{actor}.pack.zs'] = rom.zs(pack_write(rom, members), raw)

    # The bare pieces.
    bare = {}
    for piece in BARE:
        raw = src.get(f'Pack/Actor/{piece}.pack.zs')
        members = pack_files(rom, raw)
        new_actor = f'{piece}_{tag}'
        ap_path = f'Actor/{piece}.engine__actor__ActorParam.bgyml'
        param = byml.read(members.pop(ap_path))
        comps = param['Components']
        for k, ref in list(comps.items()):
            if not isinstance(ref, str) or not ref.startswith('?'): continue
            path = ref[1:]
            name = PurePosixPath(path).name
            if path in members and name.startswith(piece + '.'):
                new = str(PurePosixPath(path).with_name(new_actor + name[len(piece):]))
                members[new] = members.pop(path)
                comps[k] = '?' + new
        mi_path = comps['ModelInfoRef'][1:]
        mi = byml.read(members[mi_path])
        newp = model(mi['ModelProjectName'], mi['FmdbName'])
        rows.append((new_actor, piece, newp or mi['ModelProjectName'], mi['FmdbName']))
        if newp:
            mi['ModelProjectName'] = newp
            members[mi_path] = byml.write(mi)
        members[f'Actor/{new_actor}.engine__actor__ActorParam.bgyml'] = byml.write(param)
        files[f'Pack/Actor/{new_actor}.pack.zs'] = rom.zs(pack_write(rom, members), raw)
        bare[piece] = new_actor

    # The files the character mod adds (names the game lacks) its stand-in and pieces may need, as they are: its
    # animations, textures, AI and effects. Without them a game where the mod is off refused the stand-in (the pair test
    # of 2026-11-08). The game's own names are left alone: those are the files a mod replaces.
    added = 0
    if src.mod:
        for f in src.mod.rglob('*'):
            rel = f.relative_to(src.mod).as_posix()
            if f.is_file() and rel.split('/')[0] in ADDED_KINDS and not rom.has(rel) and rel not in files:
                files[rel] = f
                added += 1
    for path, data in files.items():
        t = out / path
        t.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, Path):   # a hard link where it can (same drive), else a copy
            try: os.link(data, t)
            except OSError: shutil.copyfile(data, t)
        else: t.write_bytes(data)
    log(f'{key}: tag {tag}, actor {actor}, {len(files)} files ({added} the mod adds)')
    return dict(key=key, tag=tag, actor=actor, bare=bare, rows=rows)


def find_romfs(folder):
    """A mod folder's romfs root (romfs/, or the emulators' layouts mods.md lists)."""
    for rel in ('romfs', 'atmosphere/contents/0100F2C0115B6000/romfs', 'contents/0100F2C0115B6000/romfs',
                '0100F2C0115B6000/romfs'):
        if (folder / rel).is_dir(): return folder / rel
    subs = [p for p in folder.iterdir() if p.is_dir()]
    if len(subs) == 1 and (subs[0] / 'romfs').is_dir(): return subs[0] / 'romfs'   # one more level (a zip's top folder)
    return None


def is_character(romfs):
    return (romfs / 'Pack/Actor/Player.pack.zs').is_file() or (romfs / 'Model/Link.Link_RaulSkin.bfres.mc').is_file()


def characters(mods):
    """The character mods in a mods folder: [(folder name, key, romfs, info)]."""
    out = []
    if not mods.is_dir(): return out
    for d in sorted(mods.iterdir()):
        if not d.is_dir() or d.name == SETS: continue
        r = find_romfs(d)
        if not r or not is_character(r): continue
        info = {}
        if (d / INFO).is_file():
            try: info = json.loads((d / INFO).read_text(encoding='utf-8'))
            except ValueError: info = {}
        key = f'gb:{info["file_id"]}' if info.get('file_id') else d.name
        out.append((d.name, key, r, info))
    return out


def table_rows(rom, overlay, table, rows, field_source):
    """table (an RSDB path) with a row for each new actor, a copy of its source actor's row, in name order."""
    byml = byml_mod()
    traw = (overlay / table).read_bytes() if (overlay and (overlay / table).is_file()) else rom.read(table)
    data = byml.read(rom.unzs(traw))
    by = {r['__RowId']: r for r in data}
    for name, base, project, fmdb in rows:
        if name in by: continue
        if base not in by:
            raise SystemExit(f'the actor tables have no {base} row: pass --overlay with the build\'s romfs-patches '
                             '(tools/coop/make_puppet_pack.py adds PlayerPuppet there)')
        row = dict(by[base])
        row['__RowId'] = name
        if 'ModelProjectName' in row and field_source: row['ModelProjectName'], row['FmdbName'] = project, fmdb
        data.append(row)
        by[name] = row
    # The game's own tables are in name order (byte order); the overlay's PlayerPuppet row sits where the row it replaced
    # was. Rows keep their order and the new ones go in before the first row whose name is greater.
    old = [r for r in data if r['__RowId'] not in {x[0] for x in rows}]
    for name, *_ in sorted(rows):
        i = next((k for k, r in enumerate(old) if r['__RowId'] > name), len(old))
        old.insert(i, by[name])
    new = byml.write(old)
    assert byml.same(byml.read(new), old)
    return rom.zs(new, traw)


def cmd_sets(args):
    rom = Rom(args.romfs)
    out = args.mods / SETS
    if out.exists(): shutil.rmtree(out)
    rf = out / 'romfs'
    rf.mkdir(parents=True)
    log = lambda m: print(m, file=sys.stderr)
    entries = [build_set(rom, Source(rom, args.overlay), 'Link', rf, log) | dict(folder='', name='Link')]
    for folder, key, r, info in characters(args.mods):
        try:
            e = build_set(rom, Source(rom, args.overlay, r), key, rf, log)
        except Exception as ex:   # one bad character does not stop the others
            log(f'{folder}: left out: {ex!r}')
            continue
        entries.append(e | dict(folder=folder, name=info.get('name') or folder, page=info.get('page', ''),
                                file_id=info.get('file_id', 0), mod_id=info.get('mod_id', 0)))
    rows = [r for e in entries for r in e.pop('rows')]
    for table in ('RSDB/ActorInfo.Product.143.rstbl.byml.zs', 'RSDB/GameActorInfo.Product.143.rstbl.byml.zs'):
        t = rf / table
        t.parent.mkdir(parents=True, exist_ok=True)
        t.write_bytes(table_rows(rom, args.overlay, table, rows, 'GameActorInfo' not in table))
    (out / 'characters.json').write_text(json.dumps(dict(characters=entries), indent=1), encoding='utf-8')
    # The same for runtime/coop_chars.c, one character a line: key, tag, stand-in actor, mod folder ("" for Link),
    # name, GameBanana page, mod id, file id, then the bare head, upper and lower actors.
    clean = lambda v: str(v).replace('\t', ' ').replace('\n', ' ')
    (out / 'characters.tsv').write_text(''.join('\t'.join(clean(e.get(k, '')) for k in (
        'key', 'tag', 'actor', 'folder', 'name', 'page', 'mod_id', 'file_id')) + '\t' + '\t'.join(
        e['bare'].get(p, '') for p in BARE) + '\n' for e in entries), encoding='utf-8')
    print(json.dumps(dict(status='pass', out=str(out), characters=[(e['name'], e['key'], e['tag']) for e in entries])))


def cmd_list(args):
    print(json.dumps([dict(folder=f, key=k, tag=tag_of(k), name=i.get('name') or f, page=i.get('page', ''))
                      for f, k, _, i in characters(args.mods)]))


# ---- GameBanana


def gb_get(url):
    req = urllib.request.Request(url, headers={'User-Agent': UA, 'Accept': 'application/json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode('utf-8'))


def mod_id_of(text):
    m = re.search(r'gamebanana\.com/mods/(\d+)', text) or re.fullmatch(r'\s*(\d+)\s*', text)
    if not m: raise SystemExit(f'not a GameBanana mod link: {text}')
    return int(m.group(1))


def info(mod_id):
    p = gb_get(f'{API}/Mod/{mod_id}/ProfilePage')
    game = (p.get('_aGame') or {}).get('_idRow')
    return dict(mod_id=mod_id, name=p.get('_sName', ''), page=f'https://gamebanana.com/mods/{mod_id}',
                game_id=game, totk=game == 7617, author=(p.get('_aSubmitter') or {}).get('_sName', ''),
                files=[dict(file_id=f['_idRow'], file=f.get('_sFile', ''), size=f.get('_nFilesize', 0),
                            md5=f.get('_sMd5Checksum', ''), description=f.get('_sDescription', ''),
                            analysis=f.get('_sAnalysisResult', ''))
                       for f in p.get('_aFiles') or []])


class Status:
    """--status: key<TAB>value lines, the whole file written anew at each change (runtime/coop_chars.c reads it)."""

    def __init__(self, path):
        self.path, self.kv = path, {'state': 'running'}

    def set(self, **kv):
        self.kv.update({k: str(v).replace('\t', ' ').replace('\n', ' ') for k, v in kv.items()})
        if not self.path: return
        tmp = Path(str(self.path) + '.tmp')
        tmp.write_text(''.join(f'{k}\t{v}\n' for k, v in self.kv.items()), encoding='utf-8')
        for _ in range(40):   # the game may have the file open for reading at that moment (a sharing violation)
            try:
                tmp.replace(self.path)
                return
            except PermissionError:
                time.sleep(0.05)
        tmp.replace(self.path)


status = Status(None)


def pick_file(files):
    """The file of a mod for game 1.4.x: one whose name or description says 1.4 (14x), not a TKMM package; else the
    first that is not one."""
    ok = [f for f in files if 'tkcl' not in (f['file'] + f['description']).lower()]
    for f in ok:
        t = (f['file'] + ' ' + f['description']).lower()
        if re.search(r'1\.4|14[0-3]|_14[0-3]_', t): return f
    return ok[0] if ok else None


def cmd_info(args):
    mi = info(mod_id_of(args.link))
    f = next((x for x in mi['files'] if str(x['file_id']) == args.file), None) if args.file != 'auto' else pick_file(mi['files'])
    if not mi['totk']: raise SystemExit(f'{mi["name"]} is not a Tears of the Kingdom mod')
    if not f: raise SystemExit(f'{mi["name"]} has no file for this game')
    status.set(name=mi['name'], page=mi['page'], mod_id=mi['mod_id'], author=mi['author'], file_id=f['file_id'],
               file=f['file'], size=f['size'], md5=f['md5'], description=f['description'],
               text=f'{mi["name"]}: {f["file"]} ({f["size"] / 1e6:.0f} MB)')
    print(json.dumps(mi | dict(picked=f)))


CODE = re.compile(r'(^|/)(exefs|exefs_patches|cheats)(/|$)|\.(ips|pchtxt|nso|npdm)$|(^|/)(subsdk\d*|main|rtld|sdk)$',
                  re.IGNORECASE)


def safe_member(name):
    """The member's path as parts, or None when it is absolute, has .. or a drive, or is otherwise unsafe."""
    n = name.replace('\\', '/')
    if not n or n.startswith('/') or re.match(r'^[A-Za-z]:', n) or '\x00' in n: return None
    parts = [p for p in n.split('/') if p not in ('', '.')]
    if any(p == '..' or ':' in p for p in parts): return None
    return parts


def unpack(archive, dest):
    """Lists the archive (zip in Python; 7z and rar through Windows' tar), checks every path, refuses code, then
    unpacks the files under the first romfs folder into dest/romfs. Returns the number of files."""
    import zipfile
    names = []
    if zipfile.is_zipfile(archive):
        z = zipfile.ZipFile(archive)
        names = [(i.filename, i) for i in z.infolist() if not i.is_dir()]
    else:
        tar = shutil.which('tar') or r'C:\Windows\System32\tar.exe'
        r = subprocess.run([tar, '-tf', str(archive)], capture_output=True, text=True)
        if r.returncode: raise SystemExit(f'cannot read the archive: {r.stderr.strip()[:300]}')
        names = [(n, None) for n in r.stdout.splitlines() if n and not n.endswith('/')]
    picked, bad = [], []
    for n, i in names:
        parts = safe_member(n)
        if parts is None: bad.append(n); continue
        if CODE.search('/'.join(parts)): raise SystemExit(f'refused: the mod has code patches or cheats ({n}); only '
                                                          'RomFS files are taken')
        low = [p.lower() for p in parts]
        if 'romfs' in low:
            k = low.index('romfs')
            picked.append((n, i, parts[k + 1:]))
    if bad: raise SystemExit(f'refused: {len(bad)} paths would leave the mod folder, e.g. {bad[0]}')
    if not picked: raise SystemExit('refused: no romfs folder in the archive')
    out = dest / 'romfs'
    if zipfile.is_zipfile(archive):
        for n, i, rel in picked:
            if not rel: continue
            t = out.joinpath(*rel)
            assert out.resolve() in t.resolve().parents
            t.parent.mkdir(parents=True, exist_ok=True)
            with z.open(i) as f, open(t, 'wb') as g: shutil.copyfileobj(f, g)
    else:
        with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
            tar = shutil.which('tar') or r'C:\Windows\System32\tar.exe'
            r = subprocess.run([tar, '-xf', str(archive), '-C', tmp] + [n for n, _, _ in picked], capture_output=True,
                               text=True)
            if r.returncode: raise SystemExit(f'cannot unpack: {r.stderr.strip()[:300]}')
            for n, _, rel in picked:
                s, t = Path(tmp).joinpath(*safe_member(n)), out.joinpath(*rel)
                assert Path(tmp).resolve() in s.resolve().parents and out.resolve() in t.resolve().parents
                t.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(s), str(t))
    return len(picked)


def cmd_fetch(args):
    mods = args.mods
    mi = info(args.mod) if args.mod else None
    if not mi: raise SystemExit('fetch needs --mod <id>')
    f = next((x for x in mi['files'] if x['file_id'] == args.file), None)
    if not f: raise SystemExit(f'mod {mi["mod_id"]} has no file {args.file}')
    if not mi['totk']: raise SystemExit(f'{mi["name"]} is not a Tears of the Kingdom mod')
    folder = re.sub(r'[^A-Za-z0-9 _.-]', '', args.name or mi['name']).strip() or f'gb{mi["mod_id"]}'
    dest = mods / folder
    if not args.yes:
        print(f'Download "{mi["name"]}" ({f["file"]}, {f["size"] / 1e6:.0f} MB) from {mi["page"]} into {dest}? [y/N] ',
              end='', flush=True)
        if input().strip().lower() not in ('y', 'yes'): raise SystemExit('not downloaded')
    if dest.exists(): raise SystemExit(f'{dest} exists already')
    mods.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=mods) as tmp:
        arc = Path(tmp) / ('download' + Path(f['file']).suffix)
        md5 = hashlib.md5()
        got = status_at = 0
        status.set(text=f'downloading {f["file"]}', got=0, size=f['size'])
        if args.from_file:   # (a test) a copy of GameBanana's file on disk in place of the download, checked the same way
            src_stream = open(args.from_file, 'rb')
        else:
            req = urllib.request.Request(f'https://gamebanana.com/dl/{f["file_id"]}', headers={'User-Agent': UA})
            src_stream = urllib.request.urlopen(req, timeout=60)
        with src_stream as r, open(arc, 'wb') as g:
            while chunk := r.read(1 << 20):
                g.write(chunk), md5.update(chunk)
                got += len(chunk)
                if got - status_at >= (4 << 20) or got == f['size']: status.set(got=got, size=f['size']); status_at = got
        if f['md5'] and md5.hexdigest() != f['md5'].lower():
            raise SystemExit(f'download damaged: MD5 {md5.hexdigest()}, GameBanana says {f["md5"]}')
        stage = Path(tmp) / 'mod'
        stage.mkdir()
        status.set(text='unpacking')
        n = unpack(arc, stage)
        if not is_character(stage / 'romfs'):
            print(json.dumps(dict(note='not a character mod (no Player.pack or Link body model)')), file=sys.stderr)
        # Off until this PC's player switches it on (runtime/mods.c): it was fetched as another player's character.
        (stage / 'coop-character-off').write_text('tools/coop/charmod.py fetch\n', encoding='utf-8')
        (stage / INFO).write_text(json.dumps(dict(name=mi['name'], mod_id=mi['mod_id'], file_id=f['file_id'],
                                                  file=f['file'], page=mi['page'], md5=f['md5'])), encoding='utf-8')
        shutil.move(str(stage), str(dest))
    print(json.dumps(dict(status='pass', folder=folder, files=n, key=f'gb:{f["file_id"]}')))
    status.set(text=f'{mi["name"]}: {n} files in {folder}', folder=folder)
    if args.sets:
        status.set(text='making the character sets')
        cmd_sets(args)
        status.set(text=f'{mi["name"]} is ready; it shows after a restart')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    def common(s, mods_needed=True):
        s.add_argument('--mods', type=Path, required=mods_needed)
        s.add_argument('--romfs', type=Path, default=root / 'build/extracted/v143/romfs')
        s.add_argument('--overlay', type=Path, default=None, help='the RomFS overlay (default: romfs-patches beside mods)')
        s.add_argument('--status', type=Path, help='key<TAB>value status lines into this file (the game reads them)')
    common(sub.add_parser('sets'))
    common(sub.add_parser('list'))
    s = sub.add_parser('info')
    s.add_argument('link')
    s.add_argument('--file', default='auto', help='a file id, or auto: the one for 1.4.x')
    common(s, False)
    s = sub.add_parser('fetch')
    s.add_argument('file', type=int)
    s.add_argument('--mod', type=int, required=True)
    s.add_argument('--name')
    s.add_argument('--yes', action='store_true')
    s.add_argument('--sets', action='store_true', help='then make the character sets again')
    s.add_argument('--from-file', type=Path, help='(a test) this file in place of the download (its MD5 is still checked)')
    common(s)
    args = ap.parse_args()
    global status
    status = Status(args.status)
    status.set(state='running', text=args.cmd)
    if args.overlay is None and args.mods:
        args.overlay = args.mods.parent / 'romfs-patches'
        if not args.overlay.is_dir(): args.overlay = None
    try:
        dict(sets=cmd_sets, list=cmd_list, info=cmd_info, fetch=cmd_fetch)[args.cmd](args)
    except SystemExit as e:
        if e.code not in (None, 0):
            status.set(state='failed', text=str(e.code))
        raise
    except Exception as e:
        status.set(state='failed', text=f'{type(e).__name__}: {e}')
        raise
    status.set(state='done')


if __name__ == '__main__':
    main()
