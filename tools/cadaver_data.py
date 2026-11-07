"""Clean loader for Cadaver (Amiga) level data. No emulator needed.
Inputs: Disk 2 ADF (levels), game image blob (static tables), lzhuf decoder."""
import struct,sys,os
sys.path.insert(0,os.path.dirname(__file__))
import lzhuf
from PIL import Image
u16=lambda b,o:struct.unpack('>H',b[o:o+2])[0]
s16=lambda b,o:struct.unpack('>h',b[o:o+2])[0]
u32=lambda b,o:struct.unpack('>I',b[o:o+4])[0]
PAL=[0x000,0x486,0x800,0x064,0x620,0x840,0x222,0xeea,0xaaa,0x400,0xea4,0x442,0x666,0x888,0xa62,0xc84]  # level 1 (TODO: per level)
RGB=[(((c>>8)&15)*17,((c>>4)&15)*17,(c&15)*17) for c in PAL]
def deinterleave(buf,w,h):
    planes=[bytearray() for _ in range(4)]
    for k in range(w*h):
        for p in range(4):planes[p]+=buf[(k*4+p)*2:(k*4+p)*2+2]
    return planes
def planes_to_image(planes,w,h,RGB=RGB):
    im=Image.new('RGBA',(w*16,h),(0,0,0,0));px=im.load()
    for y in range(h):
        for x in range(w*16):
            c=0
            for p in range(4):c|=((planes[p][y*w*2+(x>>3)]>>(7-(x&7)))&1)<<p
            if c:px[x,y]=RGB[c]+(255,)
    return im
class Group:
    """Groups 0,1,3-6: directory of (len word, offset word); group 2: 32-bit size<<17|offset."""
    def __init__(self,blob,dir_off,n,data_off,long_dir=False):
        self.blob,self.n,self.data_off,self.dir_off,self.long=blob,n,data_off,dir_off,long_dir
    def rec(self,i):
        if i>=self.n:return b''
        if self.long:
            v=u32(self.blob,self.dir_off+4*i);return self.blob[self.data_off+(v&0x1ffff):self.data_off+(v&0x1ffff)+(v>>17)]
        ln=u16(self.blob,self.dir_off+4*i);o=u16(self.blob,self.dir_off+4*i+2)
        return self.blob[self.data_off+o:self.data_off+o+ln]
class Level:
    """level = 1..5. Files come from the Disk 2 index in sector 22: entry 1+7*(level-1) = main data (LZHUF),
    +1 = rooms/objects (raw), +2 = floor/wall tiles (LZHUF), +3 = text (LZHUF)."""
    def __init__(self,adf,game_blob,level=1):
        d=open(adf,'rb').read();self.level=level
        idx=struct.unpack('>256H',d[22*512:23*512])
        ents=[(idx[8+2*i],idx[9+2*i]) for i in range(40)]
        b=1+7*(level-1)
        (s28,_),(s169,n169),(s217,_),(s275,_)=ents[b:b+4]
        self.sectors=dict(main=s28,rooms=s169,tiles=s217,text=s275)
        self.s28=lzhuf.decode(d,s28*512);self.s217=lzhuf.decode(d,s217*512)
        self.s169=d[s169*512:(s169+n169)*512]
        self.game=open(game_blob,'rb').read()
        a=self.s28
        # groups 0..2 of main data
        self.g0=Group(a,0x56,100,0x56+400)
        p=0x4c
        # group 1 header found by scanning (a code block of per-level length sits between groups 0 and 1)
        p1=self._find_group1(a)
        sz1=u32(a,p1);self.g1=Group(a,p1+10,100,p1+10+400)
        p2=p1+10+400+sz1;sz2=u32(a,p2)
        self.g2=Group(a,p2+10,255,p2+10+1020,True)
        pal_off=p2+10+1020+sz2
        self.palette_raw=[u16(a,pal_off+2*i) for i in range(16)]
        self.rgb=[(((w>>8)&15)*34,((w>>4)&15)*34,(w&15)*34) for w in self.palette_raw]
        # groups 3..6 of the rooms file
        b6=self.s169;q=0;sz=u32(b6,4);n=u16(b6,12)
        self.g3=Group(b6,14,n,14+4*n);q=14+4*n+sz
        gs=[]
        for _ in range(3):
            sz=u32(b6,q);n=u16(b6,q+8);gs.append(Group(b6,q+10,n,q+10+4*n));q+=10+4*n+sz
        self.g4,self.g5,self.g6=gs
        self._sprite={};self._ft={};self._wt={}
        t=self.s217;self.n_floor_bytes=u32(t,4);self.wall_base=8+self.n_floor_bytes+4
    def _find_group1(self,a):
        prev=None
        for p in range(0x40,len(a)-20,2):
            if u16(a,p+8)==100 and u32(a,p)<0x20000 and u32(a,p+4)==0x7d08:return p
        raise ValueError('group1 header not found')
    def static(self,addr,n):return self.game[addr-0xb8:addr-0xb8+n]   # game image loaded at $b8
    # --- sprites (group 2 images, word-interleaved planes) ---
    def sprite(self,slot,k=0):
        key=(slot,k)
        if key in self._sprite:return self._sprite[key]
        e=self.g2.rec(slot);im=None
        if e and s16(e,0x18)<0:
            hdr=None
            if e[0x0c]&4:
                if not e[0x0c]&0x20 and k<e[0x20]:
                    tab=0x28+u16(e,0x22);rec=tab+8*k;hdr=rec+s16(e,rec)
            elif k==0:hdr=0x20
            if hdr is not None:
                w,h=u16(e,hdr),u16(e,hdr+2)
                if 0<w<0x100 and 0<h<0x100 and hdr+4+w*8*h<=len(e):
                    im=planes_to_image(deinterleave(e[hdr+4:hdr+4+w*8*h],w,h),w,h,self.rgb)
        self._sprite[key]=im;return im
    # --- floor tiles: s217 block1, 60 x 288 bytes, word-interleaved 32x18 ---
    def floor_tile(self,t):
        if t in self._ft:return self._ft[t]
        b=self.s217[8+t*288:8+(t+1)*288];im=Image.new('RGBA',(32,18),(0,0,0,0));px=im.load()
        for y in range(18):
            for x in range(32):
                c=0;g=x//16
                for k in range(4):c|=((u16(b,y*16+g*8+k*2)>>(15-x%16))&1)<<k
                if c:px[x,y]=self.rgb[c]+(255,)
        self._ft[t]=im;return im
    # --- wall cubes: s217 block2, 80 x 512 bytes, 32x32 ---
    def wall_tile(self,t):
        if t in self._wt:return self._wt[t]
        base=self.wall_base;b=self.s217[base+t*512:base+(t+1)*512];im=Image.new('RGBA',(32,32),(0,0,0,0));px=im.load()
        for y in range(32):
            for x in range(32):
                c=0;g=x//16
                for k in range(4):c|=((u16(b,y*16+g*8+k*2)>>(15-x%16))&1)<<k
                if c:px[x,y]=self.rgb[c]+(255,)
        self._wt[t]=im;return im
    # --- projection ---
    def origin(self,sx,sy):return u16(self.static(0x53d2+2*(sx-3)+16*(sy-3),2),0)
    def sub(self,xs,ys):
        o=struct.unpack('>hh',self.static(0x51ae+4*(ys*8+xs),4));return o
    def project(self,org,x,y,z,F):
        f2,f4,f10,f12,f14=F
        ci,cj,xs,ys=x>>3,y>>3,x&7,y&7
        bl,bb=divmod(org+322*ci+318*cj,40);off,sh=self.sub(xs,ys)
        ol=round(off/40);ob=off-40*ol
        return (bb+ob)*8+sh-16*(f4-1),bl+ol-(z-f2+f10+f12+f14)
    # --- room ---
    def room(self,n):
        rm=self.g3.rec(n)
        if len(rm)<0x1f:return None
        return dict(n=n,raw=rm,x0=rm[1],y0=rm[3],sx=rm[4],sy=rm[5],flags=rm[0x17],walls=[int.from_bytes(rm[6+2*k:8+2*k],'big') for k in range(rm[0x16])])
    def render_room(self,n,objects=True):
        r=self.room(n)
        if not r:return None
        sx,sy=r['sx'],r['sy'];org=self.origin(sx,sy)
        im=Image.new('RGBA',(320,200),(0,0,0,255))
        # floor
        fl=self.g0.rec(n);const=not(r['flags']&4)
        for j in range(sy):
            for i in range(sx):
                idx=j*sx+i;t=(fl[0] if const else (fl[idx] if idx<len(fl) else 0)) if fl else 0
                if t:
                    bl,bb=divmod(org+322*i+318*j,40);tt=self.floor_tile(t);im.paste(tt,(bb*8,bl),tt)
        # wall cubes
        gr=self.g1.rec(n)
        if r['flags']&0x20 and gr:
            N=gr[0];p=2+(2 if gr[1] else 0);rows=[]
            for _ in range(sx+sy):
                rows.append(list(gr[p:p+N])+[0]*(8-N));p+=N
            WF=(s16(self.static(0x52ae,2),0),)+tuple(s16(self.static(0x52ae+o,2),0) for o in (2,4,0x10,0x12,0x14))
            F=tuple(s16(self.static(0x52ae+o,2),0) for o in (2,4,0x10,0x12,0x14))
            for idx,row in enumerate(rows):
                isA=idx<sx;ri=idx if isA else idx-sx
                if not isA and ri>=sy-1:continue
                cx,cy=(ri,0) if isA else (0,ri+1)
                for h,t in enumerate(row):
                    if t==0 or t>=min(0x80,(len(self.s217)-self.wall_base)//512):continue
                    px,py=self.project(org,cx*8+7,cy*8+7,h*16,F);tt=self.wall_tile(t);im.paste(tt,(px,py),tt)
        # objects
        if objects:
            items=[]
            lst=self.g5.rec(n)
            for k in range(0,len(lst),2):
                pr=self.g6.rec(u16(lst,k))
                if len(pr)>=8:items.append((pr[0]+pr[1],pr[0],pr[1],pr[2],u16(pr,6)))
            for _,x,y,z,s in sorted(items):
                e=self.g2.rec(s)
                if not e:continue
                t=self.sprite(s,0)
                if t is None:continue
                F=tuple(s16(e,o) for o in (2,4,0x10,0x12,0x14))
                px,py=self.project(org,x,y,z,F);im.paste(t,(px,py),t)
        return im
if __name__=='__main__':
    import glob
    adf=glob.glob('new_d2/*.adf')[0]
    lv=Level(adf,'game2_blob.bin',int(os.environ.get('LEVEL','1')))
    rooms=[int(a) for a in sys.argv[1:]] or list(range(6))
    sheet=Image.new('RGBA',(640,200*((len(rooms)+1)//2)))
    for n,r in enumerate(rooms):
        im=lv.render_room(r)
        if im:sheet.paste(im,((n%2)*320,(n//2)*200))
    sheet.save('ext/rooms_sheet2.png')
