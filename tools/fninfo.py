#!/usr/bin/env python3
"""Summarize recompiled guest functions: string references, callees and imports.

Usage: fninfo.py default.pe ppc_dir ADDR [ADDR ...]
Strings are found by tracking `lis rX,hi` / `addi rY,rX,lo` pairs in the generated
disassembly comments and printing the target if it is a printable C string.
"""
import glob
import re
import struct
import sys


def load_bodies(ppc_dir):
    bodies = {}
    for f in glob.glob(f'{ppc_dir}/ppc_recomp.*.cpp'):
        s = open(f).read()
        for m in re.finditer(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{(.*?)\n\}', s, re.S):
            bodies[int(m.group(1), 16)] = m.group(2)
    return bodies


def cstring(pe, addr):
    o = addr - 0x82000000
    if not (0 <= o < len(pe)):
        return None
    e = pe.find(b'\0', o, o + 200)
    if e <= o:
        return None
    s = pe[o:e]
    if len(s) < 4 or any(c < 0x20 or c > 0x7E for c in s):
        return None
    return s.decode()


def main(pe_path, ppc_dir, *addrs):
    pe = open(pe_path, 'rb').read()
    bodies = load_bodies(ppc_dir)
    for a in addrs:
        a = int(a, 16)
        body = bodies.get(a)
        if body is None:
            print(f'sub_{a:08X}: not found'); continue
        hi = {}
        strings, callees, imports = [], [], []
        for line in body.split('\n'):
            line = line.strip()
            m = re.match(r'// lis r(\d+),(-?\d+)', line)
            if m:
                hi[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
                continue
            m = re.match(r'// addi r(\d+),r(\d+),(-?\d+)', line)
            if m and m.group(2) in hi:
                s = cstring(pe, (hi[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF)
                if s and s not in strings:
                    strings.append(s)
                continue
            m = re.match(r'(sub_[0-9A-F]{8})\(ctx, base\);', line)
            if m and m.group(1) not in callees:
                callees.append(m.group(1))
            m = re.match(r'__imp__([A-Za-z_]\w*)\(ctx, base\);', line)
            if m and m.group(1) not in imports:
                imports.append(m.group(1))
        n = body.count('\n\t// ')
        print(f'== sub_{a:08X}  (~{n} instructions)')
        for s in strings[:12]:
            print(f'   str  "{s}"')
        if imports:
            print(f'   imports: {", ".join(imports)}')
        print(f'   calls: {" ".join(c for c in callees if c != "sub_824972A8")[:400]}')


if __name__ == '__main__':
    main(*sys.argv[1:])
