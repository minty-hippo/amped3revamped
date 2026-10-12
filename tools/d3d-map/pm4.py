import re, collections, pickle
from idx import *
OPS={72: 'ME_INIT', 16: 'NOP', 63: 'INDIRECT_BUFFER', 55: 'INDIRECT_BUFFER_PFD', 38: 'WAIT_FOR_IDLE', 60: 'WAIT_REG_MEM', 82: 'WAIT_REG_EQ', 83: 'WAIT_REG_GTE', 92: 'WAIT_UNTIL_READ', 93: 'WAIT_IB_PFD_COMPLETE', 33: 'REG_RMW', 62: 'REG_TO_MEM', 61: 'MEM_WRITE', 79: 'MEM_WRITE_CNTR', 68: 'COND_EXEC', 69: 'COND_WRITE', 70: 'EVENT_WRITE', 88: 'EVENT_WRITE_SHD', 89: 'EVENT_WRITE_CFL', 90: 'EVENT_WRITE_EXT', 91: 'EVENT_WRITE_ZPD', 34: 'DRAW_INDX', 54: 'DRAW_INDX_2', 52: 'DRAW_INDX_BIN', 53: 'DRAW_INDX_2_BIN', 35: 'VIZ_QUERY', 37: 'SET_STATE', 45: 'SET_CONSTANT', 85: 'SET_CONSTANT2', 86: 'SET_SHADER_CONSTANTS', 47: 'LOAD_ALU_CONSTANT', 39: 'IM_LOAD', 43: 'IM_LOAD_IMMEDIATE', 46: 'LOAD_CONSTANT_CONTEXT', 59: 'INVALIDATE_STATE', 74: 'SET_SHADER_BASES', 75: 'SET_BIN_BASE_OFFSET', 80: 'SET_BIN_MASK', 81: 'SET_BIN_SELECT', 94: 'CONTEXT_UPDATE', 84: 'INTERRUPT', 100: 'XE_SWAP', 44: 'IM_STORE', 96: 'SET_BIN_MASK_LO', 97: 'SET_BIN_MASK_HI', 98: 'SET_BIN_SELECT_LO', 99: 'SET_BIN_SELECT_HI'}
pat=re.compile(r'^([0-9a-f]{8}):\s+(lis|ori|addi|li) (\d+), (?:(\d+), )?(-?\d+)$')
hits=collections.defaultdict(list)
lines=open('/home/claude/xex/text.asm').read().splitlines()
pend={}
for i,l in enumerate(lines):
    m=pat.match(l)
    if not m: continue
    a=int(m.group(1),16); op=m.group(2); rd=m.group(3); rs=m.group(4); v=int(m.group(5))
    if op=='lis':
        hv=v&0xffff
        if 0xC000<=hv<=0xC0FF: pend[rd]=(a,hv,i)
        else: pend.pop(rd,None)
    elif op=='ori' and rs in pend:
        a0,hv,i0=pend[rs]
        if i-i0<12 and (v&0xff)==0 and (v>>8) in OPS:
            f=func_of(a); hits[f].append((a,OPS[v>>8],(hv&0xff)+1))
pickle.dump(dict(hits),open('pm4.pkl','wb'))
for f in sorted(hits): print(f'{f:08X}' if f else '????????', flen(f) if f else 0, ' '.join(sorted(set(f'{o}/{c}' for _,o,c in hits[f]))))
