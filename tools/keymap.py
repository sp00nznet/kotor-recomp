#!/usr/bin/env python3
"""Print KotOR's default key bindings (keymap.2da) from your game folder.

The gamepad mapping in src/runtime/gamepad.c sends the game its own keys, so it
has to know which DirectInput key does what; this reads the table the game
reads instead of trusting memory or a wiki. Nothing is written or copied.

    py -3 tools/keymap.py [game]          # the game folder; game by default

Formats (BioWare Aurora): chitin.key names every resource and which BIF holds
it; a BIF is a table of (offset, size) entries; keymap.2da is a binary 2DA
("2DA V2.b"): column names, row labels, then an offset per cell into a string
pool.
"""
import os
import struct
import sys

T_2DA = 2017


def read_key(game):
    d = open(os.path.join(game, 'chitin.key'), 'rb').read()
    assert d[:8] == b'KEY V1  ', d[:8]
    nbif, nkey, off_files, off_keys = struct.unpack_from('<4I', d, 8)
    bifs = []
    for i in range(nbif):
        _size, name_off, name_len, _drives = struct.unpack_from('<IIHH', d, off_files + 12 * i)
        bifs.append(d[name_off:name_off + name_len].rstrip(b'\0').decode().replace('\\', os.sep))
    keys = {}
    for i in range(nkey):
        resref, rtype, rid = struct.unpack_from('<16sHI', d, off_keys + 22 * i)
        keys[(resref.rstrip(b'\0').decode().lower(), rtype)] = rid
    return bifs, keys


def read_resource(game, name, rtype):
    bifs, keys = read_key(game)
    rid = keys[(name, rtype)]
    b = open(os.path.join(game, bifs[rid >> 20]), 'rb').read()
    assert b[:8] == b'BIFFV1  ', b[:8]
    nvar, _nfix, off_var = struct.unpack_from('<3I', b, 8)
    for i in range(nvar):
        eid, off, size, _t = struct.unpack_from('<4I', b, off_var + 16 * i)
        if eid & 0xFFFFF == rid & 0xFFFFF:
            return b[off:off + size]
    raise KeyError(name)


def parse_2da(d):
    assert d[:9] == b'2DA V2.b\n', d[:9]
    p = 9
    end = d.index(b'\0', p)
    cols = d[p:end].decode().split('\t')[:-1]
    p = end + 1
    nrows = struct.unpack_from('<I', d, p)[0]
    p += 4
    rows = []
    for _ in range(nrows):
        t = d.index(b'\t', p)
        rows.append(d[p:t].decode())
        p = t + 1
    ncell = nrows * len(cols)
    offs = struct.unpack_from('<%dH' % ncell, d, p)
    p += 2 * ncell + 2                      # the cell offsets, then the pool's size
    cell = lambda o: d[p + o:d.index(b'\0', p + o)].decode()   # noqa: E731
    return cols, [[cell(offs[r * len(cols) + c]) for c in range(len(cols))] for r in range(nrows)]


def selftest():
    blob = (b'2DA V2.b\n' + b'name\tkey\t\0' + struct.pack('<I', 2) + b'0\t1\t'
            + struct.pack('<5H', 0, 5, 5, 0, 7) + b'walk\0W\0')
    cols, rows = parse_2da(blob)
    assert cols == ['name', 'key'] and rows == [['walk', 'W'], ['W', 'walk']], (cols, rows)
    print('keymap selftest: ok')


def main():
    if '--selftest' in sys.argv:
        return selftest()
    game = sys.argv[1] if len(sys.argv) > 1 else 'game'
    cols, rows = parse_2da(read_resource(game, 'keymap', T_2DA))
    print('\t'.join(cols))
    for r in rows:
        print('\t'.join(r))


if __name__ == '__main__':
    sys.exit(main())
