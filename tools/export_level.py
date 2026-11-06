"""Export one Cadaver level to a flat binary the native game reads (little endian).
usage: python3 export_level.py <disk2.adf> <game_blob.bin> <level> <out.bin>"""
import sys,struct,glob,os
if len(sys.argv)>2:os.environ['CADAVER_GAME_IMAGE']=sys.argv[2]
sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import numpy as np
import bank
from cadaver_data import *

from movesim import wall_boxes
adf,gb,lvl,out=sys.argv[1],sys.argv[2],int(sys.argv[3]),sys.argv[4]
L=Level(adf,gb,lvl)
o=bytearray()
P=lambda fmt,*a:o.extend(struct.pack('<'+fmt,*a))
o+=b'CADL';P('I',1)
for c in L.rgb:o+=bytes(c)
for i in range(64):
    off,sh=L.sub(i%8,i//8);P('hh',off,sh)
# sprites
P('I',255)
for s in range(255):
    e=L.g2.rec(s);im=L.sprite(s,0) if e else None
    if e is None or len(e)<0x20:P('B',0);continue
    P('B',1 if im is not None else 2)
    F=[s16(e,x) for x in (2,4,0x10,0x12,0x14)];P('5h',*F);P('B',e[0x0c]&3);P('3h',F[2],F[3],F[4]);P('h',s16(e,0xa));P('4B',e[0x0c],e[0x16],e[0x17],e[0x1d])
    if im is not None:P('HH',im.width,im.height);o+=im.tobytes()
# player frames
RGB=L.rgb;bank.PAL=list(range(16));bank.rgb=lambda i:RGB[i]
e8=L.g2.rec(8);B=bank.parse_bank(e8,0x22);P('I',B['nframes'])
for n in range(B['nframes']):
    info,im=bank.frame(e8,B,n);hx,hy=info[0],info[1]
    hx=hx-65536 if hx>32767 else hx;hy=hy-65536 if hy>32767 else hy
    P('hhHH',hx,hy,im.width,im.height);o+=im.tobytes()
# rooms
P('I',100)
for n in range(100):
    r=L.room(n)
    if not r:P('B',0);continue
    P('B',1);P('5B',r['x0'],r['y0'],r['sx'],r['sy'],r['raw'][0x1e]);P('H',L.origin(r['sx'],r['sy']))
    bg=L.render_room(n,objects=False);W=wall_boxes(L,r)
    # Exits on the far (bottom/right) sides lie beyond the floor grid; the original draws a doorstep of two plain floor tiles there
    # (verified on level 1 room 1: tile 24 at cells 1,2 just below the grid; level 2's tile is a plain one, id guessed from the playthrough).
    LEDGE={1:24,2:32}.get(lvl,24);orgr=L.origin(r['sx'],r['sy']);tt=L.floor_tile(LEDGE)
    for w in W:
        b=w['box']
        if w['orient']==1 and b[1]>=r['sy']*8: cells=[(i,r['sy']) for i in range(r['sx']) if b[0]<=i*8 and i*8+7<=b[2]]
        elif w['orient']==0 and b[0]>=r['sx']*8: cells=[(r['sx'],j) for j in range(r['sy']) if b[1]<=j*8 and j*8+7<=b[3]]
        else: continue
        for i,j in cells:
            bl,bb=divmod(orgr+322*i+318*j,40);bg.paste(tt,(bb*8,bl),tt)
    o+=bg.tobytes()
    P('H',len(W))
    for w in W:P('hhB4hHBhhHHB',w['x'],w['y'],w['orient'],*w['box'],w['slot'],w['z'],w['wx'],w['wy'],w['idx'],w['flags'],w['b7'])
    lst=L.g5.rec(n);ids=[u16(lst,k) for k in range(0,len(lst),2)];P('H',len(ids))
    for pid in ids:
        p=L.g6.rec(pid);P('HBBBBHB',pid,p[0],p[1],p[2],p[3],u16(p,6),p[15] if len(p)>15 else 0)
# sound effects: 65 records at $3763c (14 bytes: i16 flag, u16 ?, u16 len_words, u32 addr, u32 loop) -> raw signed 8-bit samples
P('I',65)
for i in range(65):
    a=0x3763c-0xb8+14*i
    ln,ad=struct.unpack('>HI',L.game[a+4:a+10]);data=L.game[ad-0xb8:ad-0xb8+ln*2]
    P('I',len(data));o+=bytes(data)
# HUD frame (bottom panel), stored in the game image as per-row left and right strips of n(y) words each
# (n grows by one every 8 rows as the diagonal edges close in); word-interleaved 4-plane, strip pixels are opaque, pixels outside the strips transparent.
hud=np.zeros((200,320,4),np.uint8);off=0x4c5b2-0x4bd48-0xb8
cidx=np.zeros((200,320),int);part_of=np.zeros((200,320),int)
for y in range(115,200):
    n=7+(y-163)//8
    if n<1:continue
    for part in (0,1):
        for wd in range(n):
            ws=[int.from_bytes(L.game[off+2*k:off+2*k+2],'big') for k in range(4)];off+=8
            x0=(wd if part==0 else 20-n+wd)*16
            for b in range(16):
                cidx[y,x0+b]=sum(((ws[k]>>(15-b))&1)<<k for k in range(4));part_of[y,x0+b]=part+1
# A strip pixel is solid if it has colour, or is black on the panel side of the decoration's diagonal outline (left strip: left of its
# last coloured pixel, right strip: right of its first); black beyond the outline is playfield and stays transparent, which gives the smooth
# diagonal edge. (Verified against the original frame: every pixel treated as solid black is black there; the room shows only outside.)
for y in range(115,200):
    for part in (1,2):
        xs=np.where(part_of[y]==part)[0]
        if not len(xs):continue
        nz=[x for x in xs if cidx[y,x]]
        for x in xs:
            solid=cidx[y,x]!=0 or (nz and ((part==1 and x<max(nz)) or (part==2 and x>min(nz))))
            if solid:hud[y,x]=tuple(L.rgb[cidx[y,x]])+(255,)
# Straight diagonal edge: fit one line per side through the decoration outline (last/first coloured pixel of every row), push it
# outwards by ~6 px (the original keeps about that much black; the nearest room pixel in its frames is 10 px away) and fill solid black
# between the outline and that line.
RIGHT_M=int(__import__("os").environ.get("RIGHT_M","8"))
for part in (1,2):
    pts=[]
    for y in range(115,200):
        xs=np.where(part_of[y]==part)[0];nz=[x for x in xs if cidx[y,x]]
        if nz:pts.append((y,max(nz) if part==1 else min(nz)))
    ys=np.array([p[0] for p in pts],float);xs_=np.array([p[1] for p in pts],float)
    k,c=np.polyfit(ys,xs_,1)
    c+=(xs_-(k*ys+c)).max() if part==1 else (xs_-(k*ys+c)).min()      # line just touching the outermost outline pixel
    c+=6 if part==1 else RIGHT_M
    for y,edge in zip(pts,[k*p[0]+c for p in pts]):
        yy=y[0];x_out=y[1]
        rng=range(x_out+1,min(int(np.ceil(edge))+1,320)) if part==1 else range(max(int(np.floor(edge)),0),x_out)
        for x in rng:
            if hud[yy,x,3]==0:hud[yy,x]=(0,0,0,255)
P('B',1);o+=hud.tobytes()
# text: font (46 glyphs, 8x6, 4 plane-bytes per row), char->glyph table for 0x21..0x40, the level's strings and per-room name fields
import lzhuf,textdec
import struct as _st
def _text_ok(b):
    n=_st.unpack('>H',b[4:6])[0];of=[_st.unpack('>H',b[6+2*i:8+2*i])[0] for i in range(n+1)]
    return all(of[i]>=of[i-1] and of[i]-of[i-1]<400 for i in range(1,n+1))
blob=lzhuf.decode(open(adf,'rb').read(),L.sectors['text']*512);T=textdec.decode_text(blob,L.game)
if not _text_ok(blob):
    # Levels 2+ on the supplied Disk 2 image decompress to a damaged string table (the original game's own decoder produces the
    # same bytes). Fall back to level 1's strings for the shared message range (ids < 123: DAY, door/verb messages) and blanks.
    L1=Level(adf,gb,1);T1=textdec.decode_text(lzhuf.decode(open(adf,'rb').read(),L1.sectors['text']*512),L1.game)
    T=[T1[i] if i<123 else '' for i in range(len(T))];print('WARNING: level text damaged on this disk image; using level 1 shared strings only')
P('B',1);o+=bytes(L.game[0x4c22-0xb8:0x4c22-0xb8+46*24]);o+=bytes(L.game[0x560e-0xb8:0x560e-0xb8+32])
P('H',len(T))
for t in T:
    b=t.replace('\n','\r').encode('latin1','replace');P('H',len(b));o+=b
for n in range(100):
    r=L.room(n)
    if r:P('Bh',r['raw'][0x1a],s16(r['raw'],0x18))
    else:P('Bh',255,-1)
# verb icons (16x14, plane-major 4 planes of 14 words, 20 icons) + slot table ($59a6: x,y,w,h words) + verb->icon list for $17 types ($5604)
ip=os.environ.get('CADAVER_ICONS')
if ip and os.path.exists(ip):
    P('B',1);o+=open(ip,'rb').read()[:20*112];o+=bytes(L.game[0x59a6-0xb8:0x59a6-0xb8+64]);o+=bytes(L.game[0x5604-0xb8:0x5604-0xb8+16])
else:P('B',0)
# scroll panel (examine text): 18 tiles of 8x8 (4 plane bytes per row) from $464a, the layout table at $55e8 (0x30 bytes), the small font (46 glyphs,
# 5 rows x 4 plane bytes, $488a) and the examine descriptions: every placement script block "event $10: action $48 text" gives (placement id, text id).
exam=[]
for i in range(L.g6.n):
    e=L.g6.rec(i)
    if len(e)<0x11 or not e[0x0b]:continue
    p=0x10
    for _ in range(e[0x0b]):
        if p>=len(e) or not e[p]:break
        ln=e[p]
        if e[p+1]&0x7f==0x10 and ln>=6 and e[p+2] in (0x48,0x1c) and i not in [x[0] for x in exam]:exam.append((i,(e[p+3]<<8)|e[p+4]))
        p+=ln
P('B',1);o+=bytes(L.game[0x464a-0xb8:0x464a-0xb8+18*32]);o+=bytes(L.game[0x55e8-0xb8:0x55e8-0xb8+0x30]);o+=bytes(L.game[0x488a-0xb8:0x488a-0xb8+46*20])
P('H',len(exam))
for pid,tx in exam:P('HH',pid,tx)
# event-5 message scripts (the USE verb): (placement id, text id) for blocks `05|85 1c tt tt`
use=[]
for i in range(L.g6.n):
    e=L.g6.rec(i)
    if len(e)<0x11 or not e[0x0b]:continue
    p=0x10
    for _ in range(e[0x0b]):
        if p>=len(e) or not e[p]:break
        ln=e[p]
        if e[p+1]&0x7f==5 and ln>=6 and e[p+2]==0x1c and i not in [x[0] for x in use]:use.append((i,(e[p+3]<<8)|e[p+4]))
        p+=ln
P('H',len(use))
for pid,tx in use:P('HH',pid,tx)
# event-5 scripts (use verb / levers): raw action bytes per placement id; at most 255 bytes, one block per placement
vm=[]
for i in range(L.g6.n):
    e=L.g6.rec(i)
    if len(e)<0x11 or not e[0x0b]:continue
    p=0x10
    for _ in range(e[0x0b]):
        if p>=len(e) or not e[p]:break
        ln=e[p]
        if e[p+1]&0x7f==5:vm.append((i,bytes(e[p+2:p+ln])))
        p+=ln
P('H',len(vm))
for pid,b in vm:P('HB',pid,len(b));o+=b
# take scripts (event 0): op 6 vv = gold += vv and XP += vv>>2 (sound 3), op 5 vv = XP += vv (sound $1a), op 4 word = signed XP, op 2 = the take itself
take=[]
for i in range(L.g6.n):
    e=L.g6.rec(i)
    if len(e)<0x11 or not e[0x0b]:continue
    p=0x10;gold=xp=0;got=False
    for _ in range(e[0x0b]):
        if p>=len(e) or not e[p]:break
        ln=e[p]
        if e[p+1]&0x7f==0:
            q=p+2
            while q<p+ln:
                op=e[q]
                if op==6:gold+=e[q+1];xp+=e[q+1]>>2;q+=2;got=True
                elif op==5:xp+=e[q+1];q+=2;got=True
                elif op==4:v=(e[q+1]<<8)|e[q+2];xp+=v-65536 if v>=32768 else v;q+=3;got=True
                elif op==2:q+=1
                else:break
        p+=ln
    if got:take.append((i,gold,xp))
P('H',len(take))
for pid,gd,xp in take:P('Hhh',pid,gd,xp)
# alternate image (frame 1) for levers (switch verb 7): shown while the object's state bit is set
alt=[s for s in range(255) if L.g2.rec(s) and len(L.g2.rec(s))>0x24 and L.g2.rec(s)[0x16+1]==4 and L.sprite(s,1) is not None]
P('H',len(alt))
for s in alt:
    im=L.sprite(s,1);P('B',s);P('HH',im.width,im.height);o+=im.tobytes()
open(out,'wb').write(o);print(len(o),'bytes,',len(exam),'examine texts')
