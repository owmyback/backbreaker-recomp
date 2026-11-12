#!/usr/bin/env python3
"""Find switch jump tables in a decrypted Xbox 360 PE image and write a XenonRecomp switch TOML.

Handles the codegen used by Backbreaker, which XenonAnalyse misses:
  * instruction order lis / rlwinm / addi / lwzx (rlwinm scheduled between lis and addi)
  * nop (ori r0,r0,0) padding interleaved in the sequence
  * byte (lbzx) and halfword (lhzx) offset tables, with or without a shift

Usage: find_switch_tables.py default.pe out.toml
"""
import struct
import sys

NOP = 0x60000000
BCTR = 0x4E800420


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class PE:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        pe = struct.unpack('<I', d[0x3C:0x40])[0]
        ns = struct.unpack('<H', d[pe + 6:pe + 8])[0]
        osz = struct.unpack('<H', d[pe + 20:pe + 22])[0]
        self.base = struct.unpack('<I', d[pe + 24 + 28:pe + 24 + 32])[0]  # ImageBase
        so = pe + 24 + osz
        self.code = []
        self.pdata = None
        for i in range(ns):
            n, vs, va, *_rest = struct.unpack('<8sIIIIIIHHI', d[so + 40 * i:so + 40 * i + 40])
            ch = _rest[-1]
            if ch & 0x20000000:
                self.code.append((va, vs))
            if n.rstrip(b'\0') == b'.pdata':
                self.pdata = (va, vs)
        # function ranges from .pdata: (begin, end)
        self.funcs = []
        va, vs = self.pdata
        for o in range(va, va + vs, 8):
            begin, packed = struct.unpack('>II', d[o:o + 8])
            if begin == 0:
                continue
            length = ((packed >> 8) & 0x3FFFFF) * 4
            self.funcs.append((begin, begin + length))
        self.funcs.sort()

    def w(self, addr):
        o = addr - self.base
        return struct.unpack('>I', self.d[o:o + 4])[0]

    def u8(self, addr):
        return self.d[addr - self.base]

    def u16(self, addr):
        o = addr - self.base
        return struct.unpack('>H', self.d[o:o + 2])[0]

    def u32(self, addr):
        return self.w(addr)

    def func_of(self, addr):
        lo, hi = 0, len(self.funcs)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.funcs[mid][0] <= addr:
                lo = mid + 1
            else:
                hi = mid
        if lo and self.funcs[lo - 1][0] <= addr < self.funcs[lo - 1][1]:
            return self.funcs[lo - 1]
        # Leaf functions without a stack frame have no .pdata entry; use the gap
        # between the surrounding listed functions as the boundary.
        if 0 < lo < len(self.funcs):
            return (self.funcs[lo - 1][1], self.funcs[lo][0])
        return None


def decode(w):
    """Return (name, fields) for the handful of instructions we care about."""
    op = w >> 26
    rt, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op == 15:
        return ('lis' if ra == 0 else 'addis', dict(rt=rt, ra=ra, imm=w & 0xFFFF))
    if op == 14:
        return ('addi', dict(rt=rt, ra=ra, imm=sx16(w & 0xFFFF)))
    if op == 21:
        return ('rlwinm', dict(ra=ra, rs=rt, sh=rb, mb=(w >> 6) & 31, me=(w >> 1) & 31))
    if op == 10:
        return ('cmplwi', dict(cr=rt >> 2, ra=ra, imm=w & 0xFFFF))
    if op == 16:
        bo, bi = rt, ra
        return ('bc', dict(bo=bo, bi=bi, bd=sx16(w & 0xFFFC), lk=w & 1, aa=(w >> 1) & 1))
    if op == 19 and ((w >> 1) & 0x3FF) == 16:
        return ('bclr', dict(bo=rt, bi=ra))
    if op == 31:
        x = (w >> 1) & 0x3FF
        if x in (23, 87, 279, 266):
            return ({23: 'lwzx', 87: 'lbzx', 279: 'lhzx', 266: 'add'}[x], dict(rt=rt, ra=ra, rb=rb))
        if x == 467 and (((w >> 16) & 31) | (((w >> 11) & 31) << 5)) == 9:
            return ('mtctr', dict(rs=rt))
    return ('?', {})


def scan_back(pe, addr, n):
    """Return up to n non-nop instructions before addr, oldest first, as (addr, name, fields)."""
    out = []
    a = addr - 4
    while len(out) < n and a > addr - 4 * (n + 12):
        w = pe.w(a)
        if w != NOP:
            name, f = decode(w)
            out.insert(0, (a, name, f))
        a -= 4
    return out


def is_gt_branch(name, f):
    # bgt: BO=12 (branch if true) on gt bit; ble: BO=4 (branch if false) on gt bit
    return name in ('bc', 'bclr') and f['bi'] & 3 == 1 and f['bo'] in (12, 4)


def find_compare(pe, start, idx_reg, limit=120):
    """Walk back from start for `cmplwi crN, idx_reg, max` that is followed by a bgt/ble on crN.

    The bounds check is usually right before the table code, but the compiler sometimes
    schedules it dozens of instructions earlier, so we key on the index register.
    """
    a = start
    branches = []  # (addr, name, fields) seen so far, walking backwards
    for _ in range(limit):
        a -= 4
        w = pe.w(a)
        if w == NOP:
            continue
        name, f = decode(w)
        if is_gt_branch(name, f):
            branches.append((a, name, f))
            continue
        if name == 'cmplwi' and (idx_reg is None or f['ra'] == idx_reg):
            # the earliest gt-branch after the compare on the same cr guards the table
            for ba, bn, bf in reversed(branches):
                if bf['bi'] >> 2 == f['cr']:
                    default = 0
                    if bn == 'bc':
                        default = ba + bf['bd'] if bf['bo'] == 12 else ba + 4
                    return f['ra'], f['imm'] + 1, default, a
            if idx_reg is None:
                return None
    return None


def index_register(seq, li):
    """Register holding the raw switch index, traced back from the table load's index operand."""
    a, n, f = seq[li]
    idx = f['rb']
    for _a, n2, f2 in reversed(seq[:li]):
        if n2 == 'rlwinm' and f2['ra'] == idx:
            return f2['rs']
    return idx


def analyze(pe, bctr):
    seq = scan_back(pe, bctr, 10)
    if not seq or seq[-1][1] != 'mtctr':
        return None
    names = [s[1] for s in seq]
    loads = [i for i, n in enumerate(names) if n in ('lwzx', 'lbzx', 'lhzx')]
    if not loads:
        return None
    li = loads[-1]
    load = seq[li]
    kind = load[1]

    # resolve lis/addi constant pairs, in program order
    consts = []  # (index, reg, value)
    hi = {}
    for i, (a, n, f) in enumerate(seq):
        if n == 'lis':
            hi[f['rt']] = f['imm'] << 16
        elif n == 'addi' and f['ra'] in hi:
            consts.append((i, f['rt'], (hi.pop(f['ra']) + f['imm']) & 0xFFFFFFFF))
    before = [c for c in consts if c[0] < li]
    after = [c for c in consts if c[0] > li]
    if not before:
        return None
    table_addr = before[-1][2]

    # pattern must start at the first lis that feeds the table
    first_lis = next(a for a, n, f in seq if n == 'lis')
    idx = index_register(seq, li)
    cmp = find_compare(pe, first_lis, idx)
    if cmp is None:
        # No bounds check (index range known to the compiler). Only handled for word tables
        # stored inline right after the bctr: count entries while they are code addresses
        # inside this function, after the table itself.
        if kind != 'lwzx' or table_addr != bctr + 4:
            return None
        fn = pe.func_of(bctr)
        if fn is None:
            return None
        count = 0
        while count < 4096:
            v = pe.u32(table_addr + 4 * count)
            if not (fn[0] <= v < fn[1]) or v & 3:
                break
            count += 1
        if count == 0:
            return None
        # targets must lie after the table, which ends at the first non-entry word
        if min(pe.u32(table_addr + 4 * i) for i in range(count)) < table_addr + 4 * count:
            return None
        reg, default, cmp_addr = idx, 0, 0
    else:
        reg, count, default, cmp_addr = cmp
    if count > 4096:
        return None

    labels = []
    if kind == 'lwzx':
        if 'add' in names[li:]:
            return None
        labels = [pe.u32(table_addr + 4 * i) for i in range(count)]
    else:
        if not after or 'add' not in names[li:]:
            return None
        base = after[-1][2]
        shift = 0
        for a, n, f in seq[li + 1:]:
            if n == 'rlwinm' and f['rs'] == load[2]['rt'] and f['mb'] == 0:
                shift = f['sh']
        if kind == 'lbzx':
            labels = [(base + (pe.u8(table_addr + i) << shift)) & 0xFFFFFFFF for i in range(count)]
        else:
            labels = [(base + (pe.u16(table_addr + 2 * i) << shift)) & 0xFFFFFFFF for i in range(count)]

    fn = pe.func_of(bctr)
    ok = fn is not None and all(fn[0] <= l < fn[1] for l in labels)
    return dict(base=first_lis, r=reg, default=default, labels=labels, kind=kind,
                table=table_addr, bctr=bctr, ok=ok, cmp=cmp_addr)


def main(src, dst):
    pe = PE(src)
    found, rejected, total = [], [], 0
    for va, vs in pe.code:
        for o in range(va, va + vs, 4):
            a = pe.base + o
            if pe.w(a) != BCTR:
                continue
            total += 1
            t = analyze(pe, a)
            if t is None:
                continue
            (found if t['ok'] else rejected).append(t)

    with open(dst, 'w') as f:
        f.write('# Generated by tools/find_switch_tables.py\n')
        for t in sorted(found, key=lambda t: t['base']):
            f.write(f"# bctr 0x{t['bctr']:X}  {t['kind']}  table 0x{t['table']:X}\n")
            f.write('[[switch]]\n')
            f.write(f"base = 0x{t['base']:X}\n")
            f.write(f"r = {t['r']}\n")
            f.write(f"default = 0x{t['default']:X}\n")
            f.write('labels = [\n')
            for l in t['labels']:
                f.write(f'    0x{l:X},\n')
            f.write(']\n\n')

    by = {}
    for t in found:
        by[t['kind']] = by.get(t['kind'], 0) + 1
    print(f'bctr instructions: {total}')
    print(f'switch tables written: {len(found)}  {by}')
    print(f'rejected (labels outside function): {len(rejected)}')
    for t in rejected[:10]:
        print(f"   bctr 0x{t['bctr']:X} {t['kind']} count={len(t['labels'])}")


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
