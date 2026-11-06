import struct,sys
d=open('game2_blob.bin','rb').read();B=0xb8
def rb(a):return d[a-B]
def rw(a):return struct.unpack('>H',d[a-B:a-B+2])[0]
def rl(a):return struct.unpack('>I',d[a-B:a-B+4])[0]
NOARG={2,4,11,13,17,19}
def chan(si):
    a=rl(0x16940+si*4); inst=None; vol=0x20; tempo=2; trans=0; out=[]; steps=0
    while steps<200:
        steps+=1
        o=rb(a); a+=1
        if o>=0x80:
            i=(o-0x80)//4
            if i in NOARG: arg=None
            else: arg=rb(a); a+=1
            if i==0: vol=arg
            elif i==20: inst=arg
            elif i==5: tempo=0x2ee//arg if arg else tempo
            elif i==15: trans=arg if arg<128 else arg-256
            elif i==14: trans+=arg if arg<128 else arg-256
            elif i==11: break
            elif i==21: a=rl(0x16940+arg*4)
            elif i==2: break
            elif i>=22: break
        else:
            if inst is None: break
            r=0x375be+inst*14
            pt=rl(r+10)
            per=rw(pt+(o*4+trans)*2)
            out.append((inst-9,per,vol,o)); 
            if len(out)>=1:break
    return out
if __name__=='__main__':
    for s in range(0,0x70):
        t=0x1596e+s*6
        if t-B+6>len(d):break
        r=d[t-B:t-B+6]
        ch=[chan(x) for x in r[2:6] if x]
        print(s,r.hex(),ch)
