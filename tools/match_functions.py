#!/usr/bin/env python3
"""Match the functions of two builds of the game's executable (the disc's and a title update's).

A title update is a relink: nearly every function is the same code at another address. This pairs
them up, so that what the runtime knows about one executable by address (hooked functions, return
addresses inside them, globals) can be carried to the other.

  match_functions.py A.pe A_func_mapping.cpp B.pe B_func_mapping.cpp [ADDR ...] [--json OUT]
                     [--names RUNTIME_DIR OUT.h] [--pair A=B ...]

A function's signature is its instructions with what a relink moves masked out: the target of a
branch that leaves the function, the immediate of `lis`, and the immediate of an instruction that
uses a register a `lis` set (the low half of an address). Functions are matched by aligning the two
executables' signatures in address order (the linker keeps the order of the objects), so the
thousands of functions that share a signature (thunks, small getters) are told apart by where they
are. Functions between two matched runs, in equal numbers on both sides, are paired in order and
reported as `changed`.

The matched pairs also give addresses of data: wherever both sides build an address with the same
`lis`/low-half instructions, A's address corresponds to B's. An ADDR that is not a function start
is looked up as a place inside a matched function (same offset, when the function is `same`), then
as data.

`--names` writes the header a runtime built for B is compiled with: a `#define sub_A sub_B` (and
`__imp__sub_A`) for every guest function the runtime's sources name, so the hooks keep the names
they were written under. It stops if one of them has no match; `--pair A=B` supplies a pairing
found by hand (and overrides the matcher's).
"""
import bisect
import collections
import difflib
import hashlib
import json
import re
import struct
import sys

BASE = 0x82000000


def read(path, mode='r'):
    with open(path, mode) as f:
        return f.read()


class Exe:
    def __init__(self, pe_path, mapping_path):
        self.d = read(pe_path, 'rb')
        pe = struct.unpack_from('<I', self.d, 0x3C)[0]
        n = struct.unpack_from('<H', self.d, pe + 6)[0]
        opt = struct.unpack_from('<H', self.d, pe + 20)[0]
        self.sections = []
        for i in range(n):
            o = pe + 24 + opt + i * 40
            name = self.d[o:o + 8].rstrip(b'\0').decode()
            vsize, va = struct.unpack_from('<II', self.d, o + 8)
            self.sections.append((name, BASE + va, vsize))
        self.starts = sorted({int(a, 16) for a in re.findall(r'\{ (0x[0-9A-Fa-f]+), sub_', read(mapping_path))})
        self.funcs = {}   # start -> (signature hash, loose hash, [addresses built with lis], size in words)
        for i, s in enumerate(self.starts):
            end = self.starts[i + 1] if i + 1 < len(self.starts) else self.section_end(s)
            end = min(end, self.section_end(s))
            words = list(struct.unpack_from('>%dI' % ((end - s) // 4), self.d, s - BASE))
            while words and words[-1] == 0:
                words.pop()
            self.funcs[s] = self.signature(s, words)

    def section_end(self, a):
        for _, va, size in self.sections:
            if va <= a < va + size:
                return va + (size & ~3)
        return a

    def section_of(self, a):
        for name, va, size in self.sections:
            if va <= a < va + size:
                return name
        return None

    @staticmethod
    def signature(start, words):
        end = start + 4 * len(words)
        strict, loose, built = [], [], []
        hi = {}   # register -> value a lis gave it
        for i, w in enumerate(words):
            a = start + 4 * i
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            s = w
            if op == 18:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                target = li if w & 2 else a + li
                if not start <= target < end:
                    s = w & 0xFC000003
            elif op == 15 and ra == 0:
                s = w & 0xFFFF0000
                hi[rd] = imm << 16
            elif op == 14 and ra in hi:           # addi rd, ra, lo
                s = w & 0xFFFF0000
                built.append((hi[ra] + simm) & 0xFFFFFFFF)
                if rd != ra:
                    hi.pop(rd, None)
                else:
                    hi.pop(ra)
            elif op == 24 and rd in hi:           # ori ra, rs(=rd field), lo
                s = w & 0xFFFF0000
                built.append(hi[rd] | imm)
                hi.pop(ra, None)
            elif 32 <= op <= 55 and ra in hi:     # load/store off a lis register
                s = w & 0xFFFF0000
                built.append((hi[ra] + simm) & 0xFFFFFFFF)
                if op < 48 and op not in (36, 37, 38, 39, 44, 45, 47):   # an integer load overwrites rd
                    hi.pop(rd, None)
            elif op in (14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 46):
                hi.pop(rd, None)
            elif op in (24, 25, 26, 27, 28, 29, 20, 21, 23):
                hi.pop(ra, None)
            elif op == 31:
                hi.pop(rd, None)
                hi.pop(ra, None)
            strict.append(s)
            loose.append(op << 10 | ((w >> 1) & 0x3FF if op in (19, 31, 59, 63) else 0))
        pack = lambda v: hashlib.blake2b(struct.pack('>%dI' % len(v), *v), digest_size=8).digest()
        return pack(strict), pack(loose), built, len(words)


def match(a, b):
    """{A start: (B start, 'same' | 'changed')}"""
    sa = [a.funcs[s][0] for s in a.starts]
    sb = [b.funcs[s][0] for s in b.starts]
    out = {}
    sm = difflib.SequenceMatcher(None, sa, sb, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                out[a.starts[i1 + k]] = (b.starts[j1 + k], 'same')
        elif tag == 'replace' and i2 - i1 == j2 - j1:
            for k in range(i2 - i1):
                out[a.starts[i1 + k]] = (b.starts[j1 + k], 'changed')
    return out


def data_map(a, b, pairs):
    votes = collections.defaultdict(collections.Counter)
    for fa, (fb, how) in pairs.items():
        if how != 'same':
            continue
        for x, y in zip(a.funcs[fa][2], b.funcs[fb][2]):
            votes[x][y] += 1
    return votes


def write_names(runtime_dir, out_path, pairs, by_hand):
    import os
    used = {}
    for root, dirs, files in os.walk(runtime_dir):
        dirs[:] = [d for d in dirs if d != 'generated']
        for f in sorted(files):
            if f.endswith(('.cpp', '.h')) and os.path.join(root, f) != out_path:
                for m in re.finditer(r'sub_(8[0-9A-F]{7})', read(os.path.join(root, f))):
                    used.setdefault(int(m.group(1), 16), set()).add(os.path.relpath(os.path.join(root, f), runtime_dir))
    missing = [x for x in used if x not in pairs]
    if missing:
        sys.exit('no match for: ' + ', '.join('sub_%08X (%s)' % (x, ', '.join(sorted(used[x]))) for x in sorted(missing))
                 + '\npair them by hand with --pair A=B')
    lines = ['// Generated by tools/match_functions.py --names: do not edit. The runtime\'s sources name guest',
             '// functions by their address in the disc\'s executable; these give each its address in the title',
             '// update\'s, where the same function is. "by hand": paired by reading both, not by the matcher.',
             '#pragma once', '']
    for x in sorted(used):
        y, how = pairs[x]
        note = 'by hand' if x in by_hand else how
        lines.append('#define sub_%08X sub_%08X // %s; %s' % (x, y, note, ', '.join(sorted(used[x]))))
        lines.append('#define __imp__sub_%08X __imp__sub_%08X' % (x, y))
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d functions' % (out_path, len(used)))


def main(argv):
    out_json = names = None
    by_hand = {}
    while '--pair' in argv:
        i = argv.index('--pair')
        x, y = argv[i + 1].split('=')
        by_hand[int(x, 16)] = int(y, 16)
        del argv[i:i + 2]
    if '--names' in argv:
        i = argv.index('--names')
        names = argv[i + 1:i + 3]
        del argv[i:i + 3]
    if '--json' in argv:
        i = argv.index('--json')
        out_json = argv[i + 1]
        del argv[i:i + 2]
    a, b = Exe(argv[1], argv[2]), Exe(argv[3], argv[4])
    pairs = match(a, b)
    for x, y in by_hand.items():
        pairs[x] = (y, 'by hand')
    same = sum(1 for v in pairs.values() if v[1] == 'same')
    print('A: %d functions, B: %d; matched %d the same, %d changed, %d of A unmatched'
          % (len(a.starts), len(b.starts), same, len(pairs) - same, len(a.starts) - len(pairs)))
    votes = data_map(a, b, pairs)
    for arg in argv[5:]:
        x = int(arg, 16)
        if x in pairs:
            y, how = pairs[x]
            print('%08X -> %08X  function, %s (%d -> %d instructions)' % (x, y, how, a.funcs[x][3], b.funcs[y][3]))
            continue
        i = bisect.bisect_right(a.starts, x) - 1
        f = a.starts[i] if i >= 0 else None
        if f is not None and a.section_of(x) == a.section_of(f) and x < f + 4 * a.funcs[f][3] and a.section_of(x) != '.data':
            if f in pairs:
                y, how = pairs[f]
                print('%08X -> %08X  %s+0x%X, function %s%s' % (x, y + (x - f), 'sub_%08X' % f, x - f, how,
                                                             '' if how == 'same' else ' (offset not checked)'))
            else:
                print('%08X -> ?         in sub_%08X, which is unmatched' % (x, f))
            continue
        if x in votes:
            best = votes[x].most_common(3)
            print('%08X -> %08X  data (%s), %d reference(s) agree%s' % (x, best[0][0], a.section_of(x), best[0][1],
                  '' if len(best) == 1 else '; others: ' + ', '.join('%08X x%d' % e for e in best[1:])))
        else:
            print('%08X -> ?         not a function start, not in a function, no matched reference' % x)
    if names:
        write_names(names[0], names[1], pairs, by_hand)
    if out_json:
        with open(out_json, 'w') as f:
            json.dump({'functions': {'%08X' % k: ['%08X' % v[0], v[1]] for k, v in sorted(pairs.items())},
                       'data': {'%08X' % k: '%08X' % v.most_common(1)[0][0] for k, v in sorted(votes.items())
                                if len(v) == 1 or v.most_common(2)[0][1] > 2 * v.most_common(2)[1][1]}}, f, indent=0)


if __name__ == '__main__':
    main(sys.argv)
