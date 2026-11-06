import struct
def table(game):
    return game[0x5482-0xb8:0x5482-0xb8+64]
def decode_text(blob,game):
    tb=table(game);n=struct.unpack('>H',blob[4:6])[0]
    offs=[struct.unpack('>H',blob[6+2*i:8+2*i])[0] for i in range(n+1)]
    base=6+2*(n+1);out=[]
    for i in range(n):
        raw=blob[base+offs[i]:base+offs[i+1]];s=[]
        raw=bytes(raw)+b'\0\0\0'
        for k in range(0,len(raw)-3,3):
            b0,b1,b2=raw[k:k+3]
            for sym in (b0>>2,((b0&3)<<4)|(b1>>4),((b1&15)<<2)|(b2>>6),b2&63):s.append(sym)
        txt='';
        for sym in s:
            if sym==0:break
            c=tb[sym]
            txt+= '\n' if c==0x0d else (chr(c) if c!=0xff else '?')
        out.append(txt)
    return out
