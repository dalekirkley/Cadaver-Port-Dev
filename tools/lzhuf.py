"""LZHUF (Okumura/Yoshizaki) decoder, as used by Cadaver's level data (big-endian length header)."""
import struct,os
N,F,TH=4096,60,2
NCHAR=256-TH+F; T=NCHAR*2-1; R=T-1; MAXF=0x8000
def load_tables():
    g=open(os.environ.get('CADAVER_GAME_IMAGE','game2_blob.bin'),'rb').read()
    return g[0x117f6-0xb8:0x117f6-0xb8+256], g[0x118f6-0xb8:0x118f6-0xb8+256]
D_CODE,D_LEN=load_tables()
def decode(data, off=0):
    size=struct.unpack('>I',data[off:off+4])[0]; p=off+4
    freq=[0]*(T+1); prnt=[0]*(T+NCHAR); son=[0]*T
    for i in range(NCHAR):
        freq[i]=1; son[i]=i+T; prnt[i+T]=i
    i=0;j=NCHAR
    while j<=R:
        freq[j]=freq[i]+freq[i+1]; son[j]=i; prnt[i]=prnt[i+1]=j; i+=2; j+=1
    freq[T]=0xffff; prnt[R]=0
    st={'buf':0,'len':0,'p':p}
    def getbit():
        while st['len']<=8:
            b=data[st['p']] if st['p']<len(data) else 0
            st['p']+=1
            st['buf']|=b<<(8-st['len']); st['len']+=8
        i=st['buf']; st['buf']=(st['buf']<<1)&0xffff; st['len']-=1
        return 1 if i&0x8000 else 0
    def getbyte():
        while st['len']<=8:
            b=data[st['p']] if st['p']<len(data) else 0
            st['p']+=1
            st['buf']|=b<<(8-st['len']); st['len']+=8
        i=st['buf']; st['buf']=(st['buf']<<8)&0xffff; st['len']-=8
        return i>>8
    def reconst():
        j=0
        for i in range(T):
            if son[i]>=T:
                freq[j]=(freq[i]+1)>>1; son[j]=son[i]; j+=1
        i=0;j=NCHAR
        while j<T:
            k=i+1; f=freq[j]=freq[i]+freq[k]
            k=j-1
            while f<freq[k]: k-=1
            k+=1
            l=j-k
            freq[k+1:k+1+l]=freq[k:k+l]; freq[k]=f
            son[k+1:k+1+l]=son[k:k+l]; son[k]=i
            i+=2;j+=1
        for i in range(T):
            k=son[i]
            if k>=T: prnt[k]=i
            else: prnt[k]=prnt[k+1]=i
    def update(c):
        if freq[R]==MAXF: reconst()
        c=prnt[c+T]
        while True:
            freq[c]+=1; k=freq[c]
            l=c+1
            if k>freq[l]:
                l+=1
                while k>freq[l]: l+=1
                l-=1
                freq[c]=freq[l]; freq[l]=k
                i=son[c]; prnt[i]=l
                if i<T: prnt[i+1]=l
                j=son[l]; son[l]=i
                prnt[j]=c
                if j<T: prnt[j+1]=c
                son[c]=j; c=l
            c=prnt[c]
            if c==0: break
    def decchar():
        c=son[R]
        while c<T: c=son[c+getbit()]
        c-=T; update(c); return c
    def decpos():
        i=getbyte(); c=D_CODE[i]<<6; j=D_LEN[i]-2
        while j>0:
            i=((i<<1)+getbit())&0xffff; j-=1
        return c|(i&0x3f)
    # window initialised exactly like the game's routine at $11576 (LHarc lh1 style dictionary)
    tb=bytearray()
    for v in range(256): tb+=bytes([v])*13
    tb+=bytes(range(256)); tb+=bytes(range(255,-1,-1)); tb+=bytes(128); tb+=b' '*128
    assert len(tb)==N
    r=N-F; out=bytearray()
    while len(out)<size:
        c=decchar()
        if c<256:
            out.append(c); tb[r]=c; r=(r+1)&(N-1)
        else:
            i=(r-decpos()-1)&(N-1); j=c-255+TH
            for k in range(j):
                c=tb[(i+k)&(N-1)]; out.append(c); tb[r]=c; r=(r+1)&(N-1)
    return bytes(out[:size])
if __name__=='__main__':
    import sys
    d2=open('new_d2/Cadaver (1991)(ImageWorks)(M3)(Disk 2 of 2)(Levels).adf','rb').read()
    ram=open('jt/ram_end.bin','rb').read()
    for sec,cnt in((23,5),(28,141),(217,58),(275,12)):
        raw=d2[sec*512:(sec+cnt)*512]
        out=decode(raw)
        print('sector',sec,'->',len(out),'bytes', out[:16].hex())
        open('ext/s%d.bin'%sec,'wb').write(out)
