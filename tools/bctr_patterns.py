#!/usr/bin/env python3
"""Group the instruction sequences leading up to every bctr in a decrypted XEX PE image."""
import struct, sys, collections
d = open(sys.argv[1], 'rb').read()
N = int(sys.argv[2]) if len(sys.argv) > 2 else 7

def mn(w):
    op = w >> 26
    if op == 15: return 'lis' if ((w >> 16) & 31) == 0 else 'addis'
    if op == 14: return 'addi'
    if op == 21: return 'rlwinm'
    if op == 10: return 'cmplwi'
    if op == 11: return 'cmpwi'
    if op == 32: return 'lwz'
    if op == 34: return 'lbz'
    if op == 40: return 'lhz'
    if op == 36: return 'stw'
    if op == 16: return 'bc'
    if op == 18: return 'b'
    if op == 24: return 'ori'
    if op == 31:
        x = (w >> 1) & 0x3FF
        return {23:'lwzx',87:'lbzx',279:'lhzx',266:'add',467:'mtspr',444:'or',24:'slw',40:'subf',339:'mfspr',824:'srawi',8:'subfc'}.get(x, f'x31/{x}')
    if op == 19:
        x = (w >> 1) & 0x3FF
        return {528:'bcctr',16:'bclr'}.get(x, f'x19/{x}')
    return f'op{op}'

pe = struct.unpack('<I', d[0x3c:0x40])[0]
ns = struct.unpack('<H', d[pe+6:pe+8])[0]; osz = struct.unpack('<H', d[pe+20:pe+22])[0]
so = pe + 24 + osz
pats = collections.Counter(); ex = {}; total = 0
for i in range(ns):
    n, vs, va, rs, ra, _, _, _, _, ch = struct.unpack('<8sIIIIIIHHI', d[so+40*i:so+40*i+40])
    if not ch & 0x20000000: continue
    for o in range(va, va + vs, 4):
        if struct.unpack('>I', d[o:o+4])[0] == 0x4E800420:
            total += 1
            seq = ' '.join(mn(struct.unpack('>I', d[o-4*k:o-4*k+4])[0]) for k in range(N, 0, -1))
            pats[seq] += 1; ex.setdefault(seq, hex(0x82000000 + o))
print('total bctr:', total)
for s, c in pats.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 15):
    print(f'{c:6}  {s}  -> bctr   (e.g. {ex[s]})')
