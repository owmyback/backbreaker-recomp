#!/usr/bin/env python3
"""Decrypt a retail XEX2 (normal encryption, basic compression) to a PE image.
Usage: xex_decrypt.py default.xex default.pe   (needs openssl on PATH)"""
import struct, subprocess, sys
RETAIL = '20B185A59D28FDC340583FBB0896BF91'

def aes(mode, key, data, iv=None):
    cmd = ['openssl', 'enc', '-d', '-' + mode, '-K', key, '-nopad'] + (['-iv', iv] if iv else [])
    return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout

def main(src, dst):
    d = open(src, 'rb').read()
    magic, _, pe_off, _, sec_off, nopt = struct.unpack('>4sIIIII', d[:24])
    assert magic == b'XEX2', 'not an XEX2 file'
    hdr = {}
    for i in range(nopt):
        k, v = struct.unpack('>II', d[24 + 8*i:32 + 8*i]); hdr[k] = v
    ff = hdr[0x3FF]
    sz, enc, comp = struct.unpack('>IHH', d[ff:ff+8])
    assert enc == 1 and comp == 1, f'unsupported encryption={enc} compression={comp}'
    sk = aes('aes-128-ecb', RETAIL, d[sec_off+0x150:sec_off+0x160]).hex()
    payload = d[pe_off:]; payload = payload[:len(payload)//16*16]
    dec = aes('aes-128-cbc', sk, payload, '0'*32)
    out = bytearray(); p = 0
    for i in range((sz - 8)//8):
        ds, zs = struct.unpack('>II', d[ff+8+8*i:ff+16+8*i])
        out += dec[p:p+ds]; out += b'\0'*zs; p += ds
    assert out[:2] == b'MZ', 'decryption failed'
    open(dst, 'wb').write(out)
    print(f'wrote {dst} ({len(out):#x} bytes)')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
