#!/usr/bin/env python3
"""Emit explicit XenonRecomp function boundaries the analyzer cannot find on its own.

1. Leaf functions with switch tables: leaf functions (no stack frame) have no .pdata
   entry, and XenonRecomp's analyzer cuts functions containing jump tables short. For
   every switch table outside a .pdata function, bound its function by the nearest
   function starts around it and verify every case label is inside.
2. Pointer-only functions: functions never called directly (vtables, callbacks, exception
   funclets) are invisible to the analyzer. Code pointers stored in .rdata/.data that land
   right after padding, a blr, an unconditional branch or a bctr (a tail call through a
   register, e.g. vtable thunks packed back to back), and are not a known function start or
   switch case label, become explicit functions bounded by the next function start. Pointers
   into the body of a .pdata function are labels, not functions: the exception scope tables
   hold the addresses of `__except` blocks (#52), and declaring those split the function.
3. Branch-only functions: a leaf function reached only by tail calls (`b`, never `bl`) has no
   .pdata entry, no call to it and no pointer to it. XenonRecomp then emits `// ERROR <target>`
   and a bare `return` at each tail call, so the function silently does nothing (issue #47:
   0x8262B0F8, the controller's stick-axis reader, made every stick axis read 0). A `b` target
   becomes a function when it lies right after padding or a `blr`, is not a known function
   start or switch case label, and is in another function's range than the branch itself (a
   branch within one function to a block after its own `blr` is not a function start).

Usage: function_bounds.py default.pe switch_tables.toml config.toml [ppc_func_mapping.cpp]
The optional mapping (from a previous XenonRecomp run) adds the analyzer's own function
starts as boundaries. Rewrites the block between '# BEGIN leaf functions' and
'# END leaf functions' in the config.

The mapping is the output of a build of this very config, so it lists every function the
block already declares. Those are subtracted from 'known' before the pointer scan, otherwise
a regeneration hides its own starts from itself and the block shrinks on every run; the
addresses already in the block are still boundaries all the same.
"""
import bisect
import re
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
import find_switch_tables as fst  # noqa: E402


def call_targets(pe):
    t = set()
    for va, vs in pe.code:
        for o in range(va, va + vs, 4):
            w = pe.w(pe.base + o)
            if w >> 26 == 18 and w & 1:  # bl
                off = w & 0x3FFFFFC
                if off & 0x2000000:
                    off -= 1 << 26
                t.add(pe.base + o + off)
    return sorted(t)


def in_pdata_body(pe, addr):
    """True when addr lies inside a .pdata function, past its first instruction.

    .pdata functions are contiguous, so such an address is a label of that function and never
    a function start, even when a pointer to it is stored in data: the scope table of a
    function with an exception handler holds the addresses of its __except blocks (#52)."""
    i = bisect.bisect_right(pe.starts, addr) - 1
    return i >= 0 and pe.funcs[i][0] < addr < pe.funcs[i][1]


def branch_functions(pe, known, case_labels):
    boundaries = sorted(known)
    found = set()
    for va, vs in pe.code:
        for o in range(va, va + vs, 4):
            src = pe.base + o
            w = pe.w(src)
            if w >> 26 != 18 or w & 3:  # b only: not bl (LK), not absolute (AA)
                continue
            off = w & 0x3FFFFFC
            if off & 0x2000000:
                off -= 1 << 26
            t = src + off
            if t in known or t in case_labels or in_pdata_body(pe, t):
                continue
            if pe.w(t - 4) not in (0, 0x4E800020):  # after padding or blr
                continue
            if bisect.bisect(boundaries, src) == bisect.bisect(boundaries, t):
                continue  # same function's range: a block of that function
            found.add(t)
    return sorted(found)


def data_sections(pe):
    d = pe.d
    off = struct.unpack('<I', d[0x3C:0x40])[0]
    ns = struct.unpack('<H', d[off + 6:off + 8])[0]
    osz = struct.unpack('<H', d[off + 20:off + 22])[0]
    so = off + 24 + osz
    out = []
    for i in range(ns):
        n, vs, va, *rest = struct.unpack('<8sIIIIIIHHI', d[so + 40 * i:so + 40 * i + 40])
        if not rest[-1] & 0x20000000 and n.rstrip(b'\0') in (b'.rdata', b'.data'):
            out.append((va, vs))
    return out


def pointer_functions(pe, known, case_labels):
    code = [(pe.base + va, pe.base + va + vs) for va, vs in pe.code]
    found = set()
    for va, vs in data_sections(pe):
        for o in range(va, va + vs - 3, 4):
            v = struct.unpack('>I', pe.d[o:o + 4])[0]
            if v & 3 or v in known or not any(s <= v < e for s, e in code) or in_pdata_body(pe, v):
                continue
            prev = pe.w(v - 4)
            if prev == 0x4E800420 and v in case_labels:
                continue  # word jump tables in .rdata hold case addresses, often right after a bctr
            if prev == 0 or prev in (0x4E800020, 0x4E800420) or (prev >> 26 == 18 and not prev & 1):
                found.add(v)
    return sorted(found)


def block_functions(cfg):
    """Functions the config's generated block already declares: address -> end address."""
    m = re.search(r'# BEGIN leaf functions.*?# END leaf functions', cfg, re.S)
    if not m:
        return {}
    return {int(a, 16): int(a, 16) + int(s, 16)
            for a, s in re.findall(r'\{ address = 0x([0-9A-F]+), size = 0x([0-9A-F]+) \}', m.group(0))}


def main(pe_path, switch_path, config_path, mapping_path=None):
    pe = fst.PE(pe_path)
    pe.starts = [s for s, _ in pe.funcs]
    sw = open(switch_path).read()
    tables = []
    for block in sw.split('[[switch]]')[1:]:
        base = int(re.search(r'base = 0x([0-9A-F]+)', block).group(1), 16)
        labels = [int(x, 16) for x in re.findall(r'^\s+0x([0-9A-F]+),', block, re.M)]
        tables.append((base, labels))

    cfg = open(config_path).read()
    current = block_functions(cfg)

    starts = [s for s, _ in pe.funcs]
    known = set(starts) | set(call_targets(pe))
    if mapping_path:
        # the previous run's functions, import thunks (__imp__) included
        known |= {int(a, 16) for a in re.findall(r'0x([0-9A-F]{8}), (?:sub|__imp__)', open(mapping_path).read())}
    # Subtracting `current` matters: the mapping contains those addresses because XenonRecomp
    # compiled the block we are about to rewrite, and a pointer-only candidate that is 'known'
    # is skipped. Everything else in the mapping stays known, which is what keeps the
    # thousands of functions XenonAnalyse finds on its own out of the block.
    case_labels = {l for _, labels in tables for l in labels}
    pointers = pointer_functions(pe, known - set(current), case_labels)
    branches = [b for b in branch_functions(pe, (known - set(current)) | set(pointers), case_labels) if b not in pointers]
    calls = sorted(set(call_targets(pe)) | set(pointers) | set(branches))
    funcs = {}
    problems = []

    # Pointer-only and branch-only functions, bounded by the next function start of any kind
    boundaries = sorted(known | set(pointers) | set(branches))
    for p in pointers + branches:
        i = bisect.bisect(boundaries, p)
        end = boundaries[i] if i < len(boundaries) else p + 4
        while end > p + 4 and pe.w(end - 4) == 0:
            end -= 4
        funcs[p] = end
    for base, labels in tables:
        i = bisect.bisect(starts, base)
        if i and pe.funcs[i - 1][0] <= base < pe.funcs[i - 1][1]:
            continue  # inside a .pdata function; XenonRecomp handles it
        gs, ge = pe.funcs[i - 1][1], pe.funcs[i][0]
        while pe.w(gs) == 0:
            gs += 4
        inside = [c for c in calls if gs <= c < ge]
        lo = max([c for c in inside if c <= base] or [gs])
        hi = min([c for c in inside if c > max(labels + [base])] or [ge])
        if not all(lo <= l < hi for l in labels):
            problems.append(base)
            continue
        # trim trailing zero padding
        while hi > lo and pe.w(hi - 4) == 0:
            hi -= 4
        prev = funcs.get(lo)
        funcs[lo] = max(prev or 0, hi)

    lines = ['# BEGIN leaf functions (generated by tools/function_bounds.py)', 'functions = [']
    for a in sorted(funcs):
        lines.append(f'    {{ address = 0x{a:X}, size = 0x{funcs[a] - a:X} }},')
    lines += [']', '# END leaf functions']
    block = '\n'.join(lines)

    pat = re.compile(r'# BEGIN leaf functions.*?# END leaf functions', re.S)
    cfg = pat.sub(block, cfg) if pat.search(cfg) else cfg.rstrip() + '\n\n' + block + '\n'
    open(config_path, 'w').write(cfg)
    print(f'functions written: {len(funcs)} ({len(pointers)} pointer-only, {len(branches)} branch-only); '
          f'switches that did not fit: {len(problems)}')
    dropped = sorted(set(current) - set(funcs))
    if dropped:
        # The usual cause is the mapping: a function the block declares is in any mapping of a
        # build of that block, and if it stops being re-derivable here it used to vanish quietly.
        print(f'note: {len(dropped)} functions in the previous block are no longer candidates: '
              + ', '.join(f'0x{a:X}' for a in dropped))
    for p in problems:
        print(f'   0x{p:X}')


if __name__ == '__main__':
    main(*sys.argv[1:5])
