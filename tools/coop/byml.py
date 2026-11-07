"""BYML (version 7, little endian, as TotK's .byml/.bgyml files): read into Python values and write back.

Values: dict (str keys), list, str, bool, int (signed 32-bit), float (32-bit), None, and the wrappers below for the
other number types, so a file read and written keeps every value's type. The writer sorts the key and string tables
(the game looks keys up by binary search) and lays containers out after their parent, depth first; it does not try to
reproduce the original's byte layout, only its content (checked by reading the written file back: roundtrip()).

    python tools/coop/byml.py <file.byml>    # prints the content as JSON
"""
import json
import struct
import sys


class UInt(int):
    """Unsigned 32-bit (0xd3)."""


class Int64(int):
    """Signed 64-bit (0xd4)."""


class UInt64(int):
    """Unsigned 64-bit (0xd5)."""


class Double(float):
    """64-bit float (0xd6)."""


STR, ARR, DICT, STRTAB = 0xa0, 0xc0, 0xc1, 0xc2
BOOL, INT, FLOAT, UINT, INT64, UINT64, DOUBLE, NULL = 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xff


def read(data):
    assert data[:2] == b'YB', 'not a little-endian BYML file'
    version, keys_off, strs_off, root_off = struct.unpack_from('<HIII', data, 2)
    assert version in (2, 3, 4, 5, 6, 7), version

    def table(off):
        if not off:
            return []
        assert data[off] == STRTAB
        n = struct.unpack_from('<I', data, off)[0] >> 8
        offs = struct.unpack_from(f'<{n + 1}I', data, off + 4)
        return [data[off + o:data.index(0, off + o)].decode('utf-8') for o in offs[:n]]

    keys, strs = table(keys_off), table(strs_off)

    def value(t, v):
        if t == STR:
            return strs[v]
        if t in (ARR, DICT):
            return container(v)
        if t == BOOL:
            return bool(v)
        if t == INT:
            return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == FLOAT:
            return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == UINT:
            return UInt(v)
        if t == INT64:
            return Int64(struct.unpack_from('<q', data, v)[0])
        if t == UINT64:
            return UInt64(struct.unpack_from('<Q', data, v)[0])
        if t == DOUBLE:
            return Double(struct.unpack_from('<d', data, v)[0])
        if t == NULL:
            return None
        raise ValueError(f'BYML node type {t:#x} not supported')

    def container(off):
        t = data[off]
        n = struct.unpack_from('<I', data, off)[0] >> 8
        if t == ARR:
            types = data[off + 4:off + 4 + n]
            vals = struct.unpack_from(f'<{n}I', data, off + 4 + ((n + 3) & ~3))
            return [value(types[i], vals[i]) for i in range(n)]
        if t == DICT:
            out = {}
            for i in range(n):
                e = off + 4 + i * 8
                k = struct.unpack_from('<I', data, e)[0] & 0xffffff
                out[keys[k]] = value(data[e + 3], struct.unpack_from('<I', data, e + 4)[0])
            return out
        raise ValueError(f'BYML container type {t:#x} not supported')

    return container(root_off) if root_off else None


def write(root, version=7):
    keys, strs = set(), set()

    def collect(v):
        if isinstance(v, dict):
            for k, x in v.items():
                keys.add(k)
                collect(x)
        elif isinstance(v, list):
            for x in v:
                collect(x)
        elif isinstance(v, str):
            strs.add(v)
    collect(root)
    key_list = sorted(keys, key=lambda s: s.encode('utf-8'))
    str_list = sorted(strs, key=lambda s: s.encode('utf-8'))
    key_idx = {k: i for i, k in enumerate(key_list)}
    str_idx = {s: i for i, s in enumerate(str_list)}

    out = bytearray(16)

    def align(n):
        while len(out) % n:
            out.append(0)

    def strtab(lst):
        if not lst:
            return 0
        align(4)
        start = len(out)
        enc = [s.encode('utf-8') + b'\0' for s in lst]
        out.extend(struct.pack('<I', STRTAB | len(lst) << 8))
        pos = 4 + 4 * (len(lst) + 1)
        offs = []
        for e in enc:
            offs.append(pos)
            pos += len(e)
        offs.append(pos)
        out.extend(struct.pack(f'<{len(offs)}I', *offs))
        for e in enc:
            out.extend(e)
        return start

    keys_off = strtab(key_list)
    strs_off = strtab(str_list)

    def kind(v):
        if v is None:
            return NULL
        if isinstance(v, bool):
            return BOOL
        if isinstance(v, UInt):
            return UINT
        if isinstance(v, Int64):
            return INT64
        if isinstance(v, UInt64):
            return UINT64
        if isinstance(v, int):
            return INT
        if isinstance(v, Double):
            return DOUBLE
        if isinstance(v, float):
            return FLOAT
        if isinstance(v, str):
            return STR
        if isinstance(v, list):
            return ARR
        if isinstance(v, dict):
            return DICT
        raise TypeError(f'no BYML type for {type(v)}')

    def scalar(t, v):
        if t == NULL:
            return 0
        if t == BOOL:
            return int(v)
        if t == INT:
            return struct.unpack('<I', struct.pack('<i', v))[0]
        if t == UINT:
            return int(v)
        if t == FLOAT:
            return struct.unpack('<I', struct.pack('<f', v))[0]
        if t == STR:
            return str_idx[v]
        raise AssertionError(t)

    def container(v):
        """Writes v (a list or dict) and its children; returns its offset."""
        align(4)
        start = len(out)
        if isinstance(v, list):
            n = len(v)
            types = [kind(x) for x in v]
            out.extend(struct.pack('<I', ARR | n << 8))
            out.extend(bytes(types))
            align(4)
            slots = len(out)
            out.extend(bytes(4 * n))
            items = list(enumerate(v))
            def put(i, val):
                struct.pack_into('<I', out, slots + 4 * i, val)
        else:
            items_sorted = sorted(v.items(), key=lambda kv: key_idx[kv[0]])
            n = len(items_sorted)
            out.extend(struct.pack('<I', DICT | n << 8))
            slots = len(out)
            for k, x in items_sorted:
                out.extend(struct.pack('<I', key_idx[k] | kind(x) << 24))
                out.extend(bytes(4))
            items = [(i, x) for i, (_, x) in enumerate(items_sorted)]
            types = [kind(x) for _, x in items_sorted]
            def put(i, val):
                struct.pack_into('<I', out, slots + 8 * i + 4, val)
        for i, x in items:
            t = types[i]
            if t in (ARR, DICT):
                put(i, container(x))
            elif t in (INT64, UINT64, DOUBLE):
                align(8)
                put(i, len(out))
                out.extend(struct.pack({INT64: '<q', UINT64: '<Q', DOUBLE: '<d'}[t], x))
            else:
                put(i, scalar(t, x))
        return start

    root_off = container(root) if root is not None else 0
    struct.pack_into('<2sHIII', out, 0, b'YB', version, keys_off, strs_off, root_off)
    return bytes(out)


def same(a, b):
    """Equal content, types included (1.0 != 1, UInt(1) != 1)."""
    if type(a) is not type(b):
        return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float):
        return struct.pack('<d', a) == struct.pack('<d', b) or (a != a and b != b)
    return a == b


def roundtrip(data):
    """Reads data, writes it, reads that back: True when the content (types included) is the same."""
    a = read(data)
    return same(a, read(write(a)))


if __name__ == '__main__':
    print(json.dumps(read(open(sys.argv[1], 'rb').read()), indent=1, ensure_ascii=False, default=str))
