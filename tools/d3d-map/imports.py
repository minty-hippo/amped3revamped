import struct, re, json
xex=open('default.xex','rb').read()
img=open('image.bin','rb').read(); BASE=0x82000000
be32=lambda b,o: struct.unpack_from('>I',b,o)[0]
names={}
for lib,f in [('xboxkrnl','src/kernel/xboxkrnl/export_table.inc'),('xam','src/kernel/xam/export_table.inc')]:
    try:
        for m in re.finditer(r'XE_EXPORT\(\w+,\s*0x([0-9A-Fa-f]+),\s*(\w+)',open('/home/claude/rexglue-sdk/'+f).read()):
            names[(lib,int(m.group(1),16))]=m.group(2)
    except FileNotFoundError: pass
n=be32(xex,0x14); off=0
for i in range(n):
    k=be32(xex,0x18+i*8); v=be32(xex,0x1c+i*8)
    if k==0x000103FF: off=v
size=be32(xex,off); strsz=be32(xex,off+4); cnt=be32(xex,off+8)
strs=xex[off+12:off+12+strsz].split(b'\0'); strs=[s.decode() for s in strs if s]
p=off+12+strsz; out={}
while p<off+size:
    lsz=be32(xex,p); nameidx,rc=struct.unpack_from('>HH',xex,p+36)
    lib=strs[nameidx].split('.')[0]
    recs=[be32(xex,p+40+4*j) for j in range(rc)]
    for r in recs:
        val=be32(img,r-BASE); typ=val>>24; ordn=val&0xffff
        if typ==1:
            out[f'{r:08X}']=(lib,ordn,names.get((lib,ordn),f'ord_{ordn:X}'))
    p+=lsz
json.dump(out,open('d3d/thunks.json','w'),indent=0)
for a,(l,o,nm) in sorted(out.items()):
    if nm.startswith('Vd') or 'Mm' in nm[:2]: print(a,l,hex(o),nm)
print(len(out),'thunks')
