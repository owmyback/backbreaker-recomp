#!/usr/bin/env python3
"""List or extract an Xbox 360 STFS package (LIVE/PIRS/CON), such as a title update.

    tools/stfs.py PACKAGE            what the package is and the files in it
    tools/stfs.py PACKAGE OUT_DIR    ... and the files, written under OUT_DIR

Backbreaker's Title Update #2 (TU_134U1UQ_..., title 464F07DA) holds default.xexp, the patch for
the disc's default.xex, and PatchPackedOutputX360.pkh/.pkd, the update's own archive
(docs/title-update.md).

Layout, after Free60's STFS notes and Velocity's StfsPackage: the header's volume descriptor (at
0x379) gives the file table's first block; a block number maps to a file offset past the hash
tables that sit before every 170 data blocks (one table per group when the descriptor's block
separation bit 0 is set, two otherwise); a file's blocks are chained through the hash table
entries (0x18 bytes: SHA-1, status, next block). The hashes are not checked.
"""
import datetime
import os
import struct
import sys


class Package:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.d = d = f.read()
        if d[:4] not in (b'LIVE', b'PIRS', b'CON '):
            raise SystemExit('%s: not an STFS package (magic %r)' % (path, d[:4]))
        be32 = lambda o: struct.unpack_from('>I', d, o)[0]
        text = lambda o, n: d[o:o + n].decode('utf-16-be', 'replace').split('\0')[0]
        self.info = {
            'kind': d[:4].decode().strip(), 'content type': '0x%X' % be32(0x344),
            'title id': '%08X' % be32(0x360), 'media id': '%08X' % be32(0x354),
            'version': '%08X' % be32(0x358), 'base version': '%08X' % be32(0x35C),
            'name': text(0x411, 0x100),
        }
        vd = 0x379
        self.shift = (~d[vd + 2]) & 1          # 0: one hash table per group, 1: two
        self.step = 0xAB if self.shift == 0 else 0xAC
        self.first = (be32(0x340) + 0xFFF) & 0xFFFFF000
        table_blocks = struct.unpack_from('<H', d, vd + 3)[0]
        table_start = int.from_bytes(d[vd + 5:vd + 8], 'little')
        table = self.read(table_start, table_blocks, table_blocks * 0x1000)
        self.entries = []
        for i in range(0, len(table), 0x40):
            e = table[i:i + 0x40]
            if e[0] == 0:
                break
            flags = e[0x28]
            self.entries.append({
                'name': e[:flags & 0x3F].decode('latin-1'), 'dir': bool(flags & 0x80),
                'blocks': int.from_bytes(e[0x29:0x2C], 'little'), 'start': int.from_bytes(e[0x2F:0x32], 'little'),
                'parent': struct.unpack_from('>h', e, 0x32)[0], 'size': struct.unpack_from('>I', e, 0x34)[0],
                'time': struct.unpack_from('>I', e, 0x38)[0]})

    def data_offset(self, block):
        n = (((block + 0xAA) // 0xAA) << self.shift) + block
        if block >= 0xAA:
            n += ((block + 0x70E4) // 0x70E4) << self.shift
        if block >= 0x70E4:
            n += 1 << self.shift
        return (n << 12) + self.first

    def next_block(self, block):
        n = 0
        if block >= 0xAA:
            n = (block // 0xAA) * self.step + (((block // 0x70E4) + 1) << self.shift)
            if block // 0x70E4:
                n += 1 << self.shift
        table = (n << 12) + self.first
        best = None
        for t in ([table, table + 0x1000] if self.shift else [table]):   # the table in use, of a pair
            o = t + (block % 0xAA) * 0x18
            entry = (self.d[o + 0x14] >> 6, int.from_bytes(self.d[o + 0x15:o + 0x18], 'big'))
            if best is None or entry[0] > best[0]:
                best = entry
        return best[1]

    def read(self, start, blocks, size):
        out, b = [], start
        for _ in range(blocks):
            o = self.data_offset(b)
            out.append(self.d[o:o + 0x1000])
            b = self.next_block(b)
        return b''.join(out)[:size]

    def path(self, i):
        e = self.entries[i]
        return (self.path(e['parent']) + '/' if e['parent'] >= 0 else '') + e['name']


def fat_time(t):
    date, time = t >> 16, t & 0xFFFF
    try:
        return str(datetime.datetime(1980 + (date >> 9), (date >> 5) & 0xF, date & 0x1F, time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2))
    except ValueError:
        return '-'


def main(argv):
    if len(argv) not in (2, 3):
        raise SystemExit(__doc__.strip())
    p = Package(argv[1])
    out = os.path.abspath(argv[2]) if len(argv) == 3 else None
    print(', '.join('%s %s' % kv for kv in p.info.items()))
    for i, e in enumerate(p.entries):
        name = p.path(i)
        print('%-4s %10d  %s  %s' % ('dir' if e['dir'] else 'file', e['size'], fat_time(e['time']), name))
        if out and not e['dir']:
            dest = os.path.normpath(os.path.join(out, name))
            if not dest.startswith(out + os.sep):
                raise SystemExit('%s: an entry names a path outside the output folder' % name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as f:
                f.write(p.read(e['start'], e['blocks'], e['size']))


if __name__ == '__main__':
    main(sys.argv)
