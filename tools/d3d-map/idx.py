import struct, re, bisect, pickle, os, json
BASE=0x82000000
img=open('/home/claude/xex/image.bin','rb').read()
def u32(a): return struct.unpack_from('>I',img,a-BASE)[0]
def pdata():
    funcs=[]; a=0x8210AE00
    for i in range(30498):
        b=u32(a+8*i); w=u32(a+8*i+4)
        ln=((w>>8)&0x3FFFFF)*4
        funcs.append((b,ln))
    funcs.sort(); return funcs
CACHE='/home/claude/xex/d3d/idx.pkl'
if os.path.exists(CACHE):
    FUNCS,CALLS=pickle.load(open(CACHE,'rb'))
else:
    FUNCS=pdata(); CALLS={}
    pat=re.compile(r'^([0-9a-f]{8}):\s+(bl|b) 0x([0-9a-f]{8})')
    for l in open('/home/claude/xex/text.asm'):
        m=pat.match(l)
        if m and m.group(2)=='bl': CALLS.setdefault(int(m.group(3),16),[]).append(int(m.group(1),16))
    pickle.dump((FUNCS,CALLS),open(CACHE,'wb'))
STARTS=[f[0] for f in FUNCS]
def func_of(a):
    i=bisect.bisect_right(STARTS,a)-1
    if i>=0 and a<FUNCS[i][0]+FUNCS[i][1]: return FUNCS[i][0]
    return None
def flen(f):
    i=bisect.bisect_left(STARTS,f); return FUNCS[i][1] if i<len(STARTS) and STARTS[i]==f else 0
def callers(t): return sorted(set(func_of(c) or c for c in CALLS.get(t,[])))
THUNKS={int(k,16):v[2] for k,v in json.load(open('/home/claude/xex/d3d/thunks.json')).items()}
