import re
from idx import *
regs={}
for m in re.finditer(r'XE_GPU_REGISTER\(0x([0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)',open('/home/claude/rexglue-sdk/include/rex/graphics/register_table.inc').read()):
    regs[int(m.group(1),16)]=m.group(2)
blocks=[(11456,0x2000),(11532,0x2100),(11616,0x2180),(11636,0x2200),(11684,0x2280),(11768,0x2300),(11920,0x2380),(12000,None)]
def reg_of(off):
    for (b,r),(nb,_) in zip(blocks,blocks[1:]):
        if b<=off<nb and r is not None:
            ri=r+(off-b)//4; return f'{regs.get(ri,hex(ri))}' + (f'+{(off-b)%4}' if (off-b)%4 else '')
    return None
lines=open('/home/claude/xex/text.asm').read().splitlines()
pos={int(l[:8],16):i for i,l in enumerate(lines) if len(l)>9 and l[8]==':' and l[0] in '0123456789abcdef'}
def body(a):
    i=pos[a]; out=[]
    for l in lines[i:i+80]:
        out.append(l)
        if re.search(r'\bblr$',l) or re.search(r'\bb 0x',l): break
    return out
def describe(a):
    d=[]
    for l in body(a):
        m=re.search(r'\b(stw|sth|stb|std|stfs)\s+\d+, (\d+)\(3\)',l)
        if m:
            o=int(m.group(2)); r=reg_of(o)
            d.append(r if (r and not r.startswith('PA_CL_UCP')) else f'dev+{o}')
        m=re.search(r'\brlwimi \d+, 4, (\d+), (\d+), (\d+)',l)
        if m: d.append(f'bits{m.group(2)}-{m.group(3)}')
        m=re.search(r'\bbl 0x([0-9a-f]+)',l)
        if m: d.append('call '+m.group(1).upper())
    return ' '.join(dict.fromkeys(d))
if __name__=='__main__':
    T=0x82850E40
    for i in range(89):
        g,s,dflt=[u32(T+12*i+4*k) for k in range(3)]
        print(f'RS#{i:2d} set={s:08X} get={g:08X} default={dflt:08X} : {describe(s) if 0x8259C000<s<0x825A0000 else "-"}')
    T2=0x8285126C
    for i in range(20):
        dflt,g,s=[u32(T2+12*i+4*k) for k in range(3)]
        print(f'SS#{i:2d} set={s:08X} get={g:08X} default={dflt:08X} : {describe(s)}')
