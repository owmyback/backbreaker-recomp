#!/usr/bin/env python3
"""tools/stfs.py against a made-up package (no game or Xbox Live content): a file in a folder whose
blocks are not next to each other, so the chain through the hash table is followed."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import stfs  # noqa: E402

FIRST = 0xB000   # header size 0xAD0E rounded up: the first hash table


def entry(name, blocks, start, parent, size, folder=False):
    e = bytearray(0x40)
    e[:len(name)] = name.encode()
    e[0x28] = len(name) | (0x80 if folder else 0)
    e[0x29:0x2C] = blocks.to_bytes(3, 'little')
    e[0x2C:0x2F] = blocks.to_bytes(3, 'little')
    e[0x2F:0x32] = start.to_bytes(3, 'little')
    struct.pack_into('>hI', e, 0x32, parent, size)
    struct.pack_into('>I', e, 0x38, (30 << 25 | 5 << 21 | 4 << 16) | (12 << 11 | 30 << 5))  # 2010-05-04 12:30
    return bytes(e)


def package(files_blocks, chain, table):
    """files_blocks: {block: 4096 bytes}; chain: {block: next block}; table: the file table's bytes"""
    blocks = dict(files_blocks)
    blocks[0] = table.ljust(0x1000, b'\0')
    d = bytearray(FIRST + 0x1000 * (max(blocks) + 2))
    d[:4] = b'LIVE'
    struct.pack_into('>II', d, 0x340, 0xAD0E, 0xB0000)
    struct.pack_into('>IIII', d, 0x354, 0x11223344, 2, 2, 0x464F07DA)
    d[0x411:0x411 + 8] = 'Test'.encode('utf-16-be')
    d[0x379], d[0x37B] = 0x24, 1                       # descriptor size; one hash table per group
    struct.pack_into('<H', d, 0x37C, 1)                # the file table: one block, at block 0
    for b, data in blocks.items():
        d[FIRST + 0x1000 * (b + 1):FIRST + 0x1000 * (b + 2)] = data
        o = FIRST + b * 0x18
        d[o + 0x14] = 0x80
        d[o + 0x15:o + 0x18] = chain.get(b, 0xFFFFFF).to_bytes(3, 'big')
    return bytes(d)


class StfsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.a = bytes(range(256)) * 16 + b'tail of a'          # two blocks: 1, then 3
        self.b = b'the second file'                              # one block: 2
        table = (entry('sub', 0, 0, -1, 0, folder=True) + entry('a.bin', 2, 1, 0, len(self.a))
                 + entry('b.txt', 1, 2, -1, len(self.b)))
        blocks = {1: self.a[:0x1000], 3: self.a[0x1000:].ljust(0x1000, b'\0'), 2: self.b.ljust(0x1000, b'\0')}
        self.path = os.path.join(self.tmp.name, 'pkg')
        with open(self.path, 'wb') as f:
            f.write(package(blocks, {1: 3}, table))

    def test_listing_and_header(self):
        p = stfs.Package(self.path)
        self.assertEqual(p.info['title id'], '464F07DA')
        self.assertEqual(p.info['name'], 'Test')
        self.assertEqual([(p.path(i), e['size'], e['dir']) for i, e in enumerate(p.entries)],
                         [('sub', 0, True), ('sub/a.bin', len(self.a), False), ('b.txt', len(self.b), False)])
        self.assertEqual(stfs.fat_time(p.entries[1]['time']), '2010-05-04 12:30:00')

    def test_extract_follows_the_block_chain(self):
        out = os.path.join(self.tmp.name, 'out')
        with contextlib.redirect_stdout(io.StringIO()):
            stfs.main(['stfs.py', self.path, out])
        with open(os.path.join(out, 'sub', 'a.bin'), 'rb') as f:
            self.assertEqual(f.read(), self.a)
        with open(os.path.join(out, 'b.txt'), 'rb') as f:
            self.assertEqual(f.read(), self.b)

    def test_not_a_package(self):
        bad = os.path.join(self.tmp.name, 'bad')
        with open(bad, 'wb') as f:
            f.write(b'XEX2' + bytes(0x2000))
        with self.assertRaises(SystemExit):
            stfs.Package(bad)

    def test_entry_outside_the_output_folder_is_refused(self):
        table = entry('../evil', 1, 1, -1, 4)
        with open(self.path, 'wb') as f:
            f.write(package({1: b'evil'.ljust(0x1000, b'\0')}, {}, table))
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()):
            stfs.main(['stfs.py', self.path, os.path.join(self.tmp.name, 'out2')])
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, 'evil')))


if __name__ == '__main__':
    unittest.main()
