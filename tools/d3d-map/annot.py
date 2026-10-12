import re, pickle, collections, sys
from idx import *
OUT=pickle.load(open('out.pkl','rb')); PM4=pickle.load(open('pm4.pkl','rb'))
LO,HI=0x82593000,0x825B0000
lines=open('/home/claude/xex/text.asm').read().splitlines()
pos={}
for i,l in enumerate(lines):
    if len(l)>9 and l[8]==':' and l[0] in '0123456789abcdef': pos[int(l[:8],16)]=i
memre=re.compile(r'\b(lwz|stw|lbz|stb|lhz|sth|lfs|stfs|ld|std|lwzu|stwu)\s+(\d+), (-?\d+)\((\d+)\)')
def ann(f):
    n=flen(f); i0=pos.get(f); offs=collections.Counter(); consts=set()
    if i0 is not None:
        for l in lines[i0:i0+n//4]:
            m=memre.search(l)
            if m and int(m.group(3))>=4096 and m.group(4)!='1': offs[int(m.group(3))]+=1
            m2=re.search(r'\b(lis) \d+, (-?\d+)$',l)
            if m2:
                v=int(m2.group(2))&0xffff
                if v not in (0x8200,0x8201,0x8202,0x8203,0x8204,0x8205,0x8206,0x8207,0x8208,0x8209,0x820A,0x820B,0x820C,0x820D,0x820E,0x820F) and not (0x8200<=v<0x8300): consts.add(v)
    cs=callers(f)
    g=[c for c in cs if not LO<=c<HI]
    callee=sorted(OUT.get(f,()))
    th=[THUNKS[t] for t in callee if t in THUNKS]
    cl=[f'{t:08X}' for t in callee if t not in THUNKS]
    pm=sorted(set(o for _,o,_ in PM4.get(f,[])))
    return f'{f:08X} len={n} callers={len(cs)} game={len(g)}[{" ".join(f"{c:08X}" for c in g[:6])}] int=[{" ".join(f"{c:08X}" for c in cs if LO<=c<HI)[:60]}]\n   calls: {" ".join(cl)[:150]} {" ".join(th)}\n   pm4: {" ".join(pm)}  devoffs: {" ".join(str(o) for o,_ in offs.most_common(10))}  lis: {" ".join(hex(c) for c in sorted(consts))[:80]}'
if __name__=='__main__':
    for f,l in FUNCS:
        if LO<=f<HI: print(ann(f))
