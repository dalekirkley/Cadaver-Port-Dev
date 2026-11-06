import sys
from PIL import Image
PAL=[0x000,0x486,0x800,0x064,0x620,0x840,0x222,0xeea,0xaaa,0x400,0xea4,0x442,0x666,0x888,0xa62,0xc84]
def rgb(c):return ((c>>8&15)*17,(c>>4&15)*17,(c&15)*17)
u16=lambda d,o:int.from_bytes(d[o:o+2],'big')
def parse_bank(d,b):
    t=b+4; d6=u16(d,b-2); d5=u16(d,b); d4=u16(d,b+2)
    nframes=d6//2
    return dict(t=t,imgtab=t+d6,rec=t+d5,imgs=t+d4,nframes=nframes,nimgs=(d5-d6)//2)
def part_image(d,B,idx):
    o=B['imgs']+u16(d,B['imgtab']+2*idx)
    w=u16(d,o);h=u16(d,o+2);return w,h,o+4
def frame(d,B,n):
    r=B['rec']+u16(d,B['t']+2*n)
    hx=u16(d,r);hy=u16(d,r+2)
    W=((d[r+4]>>1)&0xfff8)+8;H=d[r+5];cnt=d[r+7]
    # word at r+6: high byte unused, low byte = count
    img=Image.new('RGBA',(W+32,H+1),(0,0,0,0));px=img.load()
    p=r+8
    for _ in range(cnt):
        x=u16(d,p);y=u16(d,p+2);i=u16(d,p+4);p+=6
        w,h,o=part_image(d,B,i)
        for yy in range(h):
            for ww in range(w):
                pl=[u16(d,o+((yy*w+ww)*4+k)*2) for k in range(4)]
                for bit in range(16):
                    c=sum(((pl[k]>>(15-bit))&1)<<k for k in range(4))
                    X=x+ww*16+bit;Y=y+yy
                    if c and 0<=X<img.width and 0<=Y<img.height:px[X,Y]=rgb(PAL[c])+(255,)
    return (hx,hy,W,H,cnt),img
if __name__=='__main__':
    d=open('../jt/ram_play.bin','rb').read()
    B=parse_bank(d,0x702e4);print(B)
    for n in range(6):
        i,im=frame(d,B,n);print(n,i);im.resize((im.width*4,im.height*4),0).save(f'/tmp/bf{n}.png')
