#!/usr/bin/env python3
"""tools/match_functions.py against two made-up images (no game code): the same few functions
linked at other addresses, with their globals moved, one function changed and one added."""
import io
import contextlib
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import match_functions as mf  # noqa: E402

TEXT, DATA = 0x82010000, 0x82020000
BLR, NOP = 0x4E800020, 0x60000000


def lis(r, hi): return 15 << 26 | r << 21 | hi
def addi(rd, ra, lo): return 14 << 26 | rd << 21 | ra << 16 | (lo & 0xFFFF)
def lwz(rd, ra, lo): return 32 << 26 | rd << 21 | ra << 16 | (lo & 0xFFFF)
def li(rd, v): return addi(rd, 0, v)
def bl(at, target): return 18 << 26 | ((target - at) & 0x03FFFFFC) | 1


def image(functions):
    """functions: [(address, [words])] -> (image bytes, mapping text)"""
    d = bytearray(0x30000)
    d[:2] = b'MZ'
    struct.pack_into('<I', d, 0x3C, 0x80)
    pe = 0x80
    d[pe:pe + 4] = b'PE\0\0'
    struct.pack_into('<H', d, pe + 6, 2)       # sections
    struct.pack_into('<H', d, pe + 20, 0xE0)   # optional header size
    for i, (name, va, size) in enumerate(((b'.text', TEXT, 0x1000), (b'.data', DATA, 0x1000))):
        o = pe + 24 + 0xE0 + i * 40
        d[o:o + len(name)] = name
        struct.pack_into('<II', d, o + 8, size, va - mf.BASE)
    for a, words in functions:
        struct.pack_into('>%dI' % len(words), d, a - mf.BASE, *words)
    return bytes(d), ''.join('    { 0x%08X, sub_%08X },\n' % (a, a) for a, _ in functions)


def build(shift, glob, changed=False, extra=False):
    """Three functions at TEXT + shift: a getter of a global, a caller of it, a leaf."""
    f1, f2, f3 = TEXT + shift, TEXT + shift + 0x20, TEXT + shift + 0x40
    getter = [lis(11, glob >> 16), lwz(3, 11, glob & 0xFFFF), BLR]
    caller = [li(3, 7 if changed else 5), bl(f2 + 4, f1), addi(3, 3, 1), BLR]
    leaf = [li(3, 1), BLR]
    out = [(f1, getter), (f2, caller), (f3, leaf)]
    if extra:
        out.append((f3 + 0x20, [NOP, BLR]))
    return out


class MatchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def exe(self, name, functions):
        img, mapping = image(functions)
        pe, mp = os.path.join(self.tmp.name, name + '.pe'), os.path.join(self.tmp.name, name + '.cpp')
        with open(pe, 'wb') as f:
            f.write(img)
        with open(mp, 'w') as f:
            f.write(mapping)
        return pe, mp

    def test_moved_functions_are_the_same_and_give_the_globals(self):
        a = mf.Exe(*self.exe('a', build(0, DATA + 0x10)))
        b = mf.Exe(*self.exe('b', build(0x100, DATA + 0x234, extra=True)))
        pairs = mf.match(a, b)
        self.assertEqual(pairs, {TEXT: (TEXT + 0x100, 'same'), TEXT + 0x20: (TEXT + 0x120, 'same'),
                                 TEXT + 0x40: (TEXT + 0x140, 'same')})
        votes = mf.data_map(a, b, pairs)
        self.assertEqual(dict(votes[DATA + 0x10]), {DATA + 0x234: 1})

    def test_a_changed_function_between_matched_ones_is_paired_as_changed(self):
        a = mf.Exe(*self.exe('a', build(0, DATA + 0x10)))
        b = mf.Exe(*self.exe('b', build(0x100, DATA + 0x10, changed=True)))
        pairs = mf.match(a, b)
        self.assertEqual(pairs[TEXT + 0x20], (TEXT + 0x120, 'changed'))
        self.assertEqual(pairs[TEXT][1], 'same')

    def test_a_branch_inside_the_function_is_part_of_the_signature(self):
        inside = [bl(TEXT, TEXT + 8), NOP, BLR]
        outside = [bl(TEXT, TEXT + 0x800), NOP, BLR]
        other = [bl(TEXT, TEXT + 0x900), NOP, BLR]
        sig = lambda w: mf.Exe.signature(TEXT, w)[0]
        self.assertEqual(sig(outside), sig(other))
        self.assertNotEqual(sig(inside), sig(outside))

    def test_names_header_and_lookups(self):
        fa, fb = build(0, DATA + 0x10), build(0x100, DATA + 0x234)
        (pa, ma), (pb, mb) = self.exe('a', fa), self.exe('b', fb)
        runtime = os.path.join(self.tmp.name, 'runtime')
        os.makedirs(os.path.join(runtime, 'generated'))
        with open(os.path.join(runtime, 'hook.cpp'), 'w') as f:
            f.write('PPC_FUNC(sub_%08X) { __imp__sub_%08X(ctx, base); }\n' % (TEXT + 0x20, TEXT + 0x20))
        with open(os.path.join(runtime, 'generated', 'skipped.cpp'), 'w') as f:
            f.write('sub_82FFFFF0\n')   # generated code is not scanned: this one has no match
        header = os.path.join(runtime, 'names.h')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            mf.main(['match_functions.py', pa, ma, pb, mb, '%08X' % (TEXT + 0x24), '%08X' % (DATA + 0x10),
                     '--names', runtime, header])
        with open(header) as f:
            text = f.read()
        self.assertIn('#define sub_%08X sub_%08X // same; hook.cpp' % (TEXT + 0x20, TEXT + 0x120), text)
        self.assertIn('#define __imp__sub_%08X __imp__sub_%08X' % (TEXT + 0x20, TEXT + 0x120), text)
        self.assertNotIn('82FFFFF0', text)
        self.assertIn('%08X -> %08X  sub_%08X+0x4, function same' % (TEXT + 0x24, TEXT + 0x124, TEXT + 0x20), out.getvalue())
        self.assertIn('%08X -> %08X  data (.data), 1 reference(s) agree' % (DATA + 0x10, DATA + 0x234), out.getvalue())

    def test_names_stops_on_a_function_with_no_match_unless_paired_by_hand(self):
        (pa, ma), (pb, mb) = self.exe('a', build(0, DATA + 0x10)), self.exe('b', build(0x100, DATA + 0x10))
        runtime = os.path.join(self.tmp.name, 'runtime')
        os.makedirs(runtime)
        with open(os.path.join(runtime, 'hook.cpp'), 'w') as f:
            f.write('PPC_FUNC(sub_82010F00) {}\n')
        header = os.path.join(runtime, 'names.h')
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit):
                mf.main(['match_functions.py', pa, ma, pb, mb, '--names', runtime, header])
            mf.main(['match_functions.py', pa, ma, pb, mb, '--names', runtime, header, '--pair', '82010F00=82010140'])
        with open(header) as f:
            self.assertIn('#define sub_82010F00 sub_82010140 // by hand; hook.cpp', f.read())


if __name__ == '__main__':
    unittest.main()
