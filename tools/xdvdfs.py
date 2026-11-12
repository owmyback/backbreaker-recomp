import sys,struct,os
iso=open(sys.argv[1],'rb'); out=sys.argv[2]; want=[w.lower() for w in sys.argv[3:]]
SEC=2048; MAG=b'MICROSOFT*XBOX*MEDIA'
base=None
for off in (0x0,0xFD90000,0x2080000,0x18300000):
    iso.seek(off+32*SEC)
    if iso.read(20)==MAG: base=off;break
print('partition offset',hex(base))
iso.seek(base+32*SEC+20); rs,rz=struct.unpack('<II',iso.read(8))
def walk(sec,size,path):
    iso.seek(base+sec*SEC); d=iso.read(size); stack=[0]; seen=set()
    while stack:
        o=stack.pop()*4
        if o in seen or o+14>len(d): continue
        seen.add(o)
        l,r,s,z,a,n=struct.unpack('<HHIIBB',d[o:o+14])
        if l==0xFFFF: continue
        name=d[o+14:o+14+n].decode('latin1'); p=path+'/'+name
        if a&0x10: yield from walk(s,z,p)
        else:
            yield p,s,z
        if l: stack.append(l)
        if r: stack.append(r)
for p,s,z in walk(rs,rz,''):
    print(f'{z:>12} {p}')
    if p.lower().lstrip('/') in want:
        iso.seek(base+s*SEC); open(os.path.join(out,os.path.basename(p)),'wb').write(iso.read(z))
