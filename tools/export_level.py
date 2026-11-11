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
    if e and im is None and len(e)<=0x20 and s16(e,0x18)>=0 and e[0x0c]&0x40 and 0<s16(e,0x18)//512<(len(L.s217)-L.wall_base)//512:im=L.wall_tile(s16(e,0x18)//512)   # a placeable wall cube (level 5 pillars, slots 95/98): the record's word +0x18 is the cube's byte offset in the cube bank (512 per cube)
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
    # (verified on level 1 room 1: tile 24 at cells 1,2 just below the grid; the same tile 24 fits levels 2-5 against emulator frames).
    LEDGE=24;orgr=L.origin(r['sx'],r['sy']);tt=L.floor_tile(LEDGE)
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
# Room-name strings follow the spell names, whose count differs per level (level 1: ARMOURY is id 126 after SUICIDE; level 4: TOWER is 128).
# The port reads text[126 + name], so store the name byte shifted by (first name id - 126).
nb=(T.index('SUICIDE')+1-126) if 'SUICIDE' in T else 0
for n in range(100):
    r=L.room(n)
    if r:P('Bh',r['raw'][0x1a] if r['raw'][0x1a]>=100 else r['raw'][0x1a]+nb,s16(r['raw'],0x18))
    else:P('Bh',255,-1)
# verb icons (16x14, plane-major 4 planes of 14 words, 20 icons) + slot table ($59a6: x,y,w,h words) + verb->icon list for $17 types ($5604)
ip=os.environ.get('CADAVER_ICONS') or os.path.join(os.path.dirname(os.path.abspath(__file__)),'verb_icons.bin')   # the 20 verb icons ship with the tools; without them the interaction menu has no verbs
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
        elif e[p+1]&0x7f==9 and e[p+2:p+4]==b'\0\0':vm.append((i|0x8000,bytes(e[p+4:p+ln])))   # event 9 with the hero (placement 0) as the other party: contact scripts
        elif e[p+1]==0x07 and len(vm)<255:vm.append((i|0x2000,bytes(e[p+2:p+ln])))   # event 7 (no keep bit) on a takeable item: runs once the item has been taken (room 70: the spider appears)
        elif e[p+1]&0x7f==4 and len(vm)<255:vm.append((i|0x1000,bytes(e[p+4:p+ln])))   # event 4: the object is hit by a moving object (missile); 2 parameter bytes (hitter type) then ops: flasks and urns break, release creatures
        elif e[p+1]&0x7f==0x0d:vm.append((i|0x4000,bytes(e[p+4:p+ln])))   # event 0xd: posted by an object's animation program (param word at +2); ops follow
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
alt=[s for s in range(255) if L.g2.rec(s) and len(L.g2.rec(s))>0x24 and L.g2.rec(s)[0x16+1] in (4,5) and L.sprite(s,1) is not None]
P('H',len(alt))
for s in alt:
    e=L.g2.rec(s);tab=0x28+u16(e,0x22)
    ax=s16(e,tab+8+2)-s16(e,tab+2);ay=s16(e,tab+8+4)-s16(e,tab+4)   # offset of frame 1 relative to frame 0
    im=L.sprite(s,1);P('B',s);P('HH',im.width,im.height);o+=im.tobytes()
# offsets of the alternate images (separate section so older/newer builds stay compatible): slot, dx, dy
P('H',len(alt))
for s in alt:
    e=L.g2.rec(s);tab=0x28+u16(e,0x22);P('Bhh',s,s16(e,tab+8+2)-s16(e,tab+2),s16(e,tab+8+4)-s16(e,tab+4))
# room scripts (timed events): [n] then per room: room, period, nblocks, blocks (len + raw bytes incl. length byte)
rs=[]
for n in range(100):
    rm=L.g3.rec(n)
    if len(rm)<0x20:continue
    q=0x20;bl=[]
    for _ in range(rm[0x1f]):
        if q>=len(rm) or rm[q]==0:break
        ln=rm[q];bl.append(bytes(rm[q:q+ln]));q+=ln
    rs.append((n,rm[2],bl))
P('H',len(rs))
for n,per,bl in rs:
    P('BBB',n,per,len(bl))
    for b in bl:P('B',len(b));o+=b
# multi-frame creature images (water lurker etc.): slot, nframes, per frame w,h,dx,dy (relative to frame 0), RGBA
cf=[]
mvslots=set()
for pid in range(1024):                                       # creature types of later levels live in other slots: every type with a movement program and an animation program
    pr=L.g6.rec(pid)
    if len(pr)>0xd+0xe and pr[0xd] and pr[0xd]+0xe<len(pr):mvslots.add(u16(pr,6))
for s_ in range(255):                                         # ... and every animated prop (multi-image type with behaviour flag)
    e=L.g2.rec(s_)
    if s_!=8 and e and len(e)>=0x30 and s16(e,0x18)<0 and e[0x0c]&4 and e[0x0c]&0x40 and e[0x20]>1:mvslots.add(s_)
for s_ in sorted(set(range(111,122))|{x for x in mvslots if x not in (8,) and x<255}):
    e=L.g2.rec(s_)
    if not e or len(e)<0x30 or (s_>=122 and not (e[0x0c]&4 and e[0x0c]&0x40)):continue
    tab=0x28+u16(e,0x22);fr=[]
    for k in range(16):
        try:im=L.sprite(s_,k)
        except Exception:im=None
        if im is None:break
        fr.append((k,im))
    if len(fr)>=2:cf.append((s_,e,tab,fr))
P('H',len(cf))
for s_,e,tab,fr in cf:
    P('BB',s_,len(fr))
    for k,im in fr:
        P('HHhh',im.width,im.height,s16(e,tab+8*k+2)-s16(e,tab+2),s16(e,tab+8*k+4)-s16(e,tab+4));o+=im.tobytes()
# chest data (types with byte +0x16 == 8): placement id, contents pid, key pid, flags, trap damage (block at placement +[+0xc])
ch=[]
for n in range(100):
    lst=L.g5.rec(n)
    for k in range(0,len(lst),2):
        pid=u16(lst,k);p=L.g6.rec(pid);e=L.g2.rec(u16(p,6))
        if len(e)>0x17 and e[0x16]==8 and p[0xc]+6<=len(p):
            d=p[0xc];ch.append((pid,u16(p,d),u16(p,d+2),p[d+4],p[d+5]))
P('H',len(ch))
for c in ch:P('HHHBB',*c)
# creature movement programs (placement movement record at +[+0xd]: step sizes +6..+8, program from +0xe). Ops: 00 a b c = move (signed counts per axis, one step per tick), 01 d = relative jump, 02 = stop, 03 n = wait n ticks, 04 n = post event $c
mv=[]
for pid in range(L.g6.n):
    p=L.g6.rec(pid)
    if len(p)<0x14 or not 112<=u16(p,6)<=123:continue
    d=p[0xd]
    if d+0xe>=len(p):continue
    mv.append((pid,p[d+6],p[d+7],p[d+8],p[d+9],bytes(p[d+0xe:d+0xe+96])))
P('H',len(mv))
for pid,s0,s1,s2,pat,pr in mv:
    P('HBBBBB',pid,s0,s1,s2,pat,len(pr));o+=pr
# held-item panel (optional, after the movement programs): while an item is held and nothing is touched, the original replaces the left HUD
# (six verb slots) with another panel: a picture box and a name bar. Captured once from the emulator (tools/held_panel.png, level 1 palette,
# 128x62 at (0,138)); stored as palette indices, mapped through level 1's palette and re-coloured with THIS level's palette, opaque only where
# the normal HUD frame is opaque, so the room still shows elsewhere. Layout: B 1, then H x0, H y0, H w, H h, then w*h RGBA bytes.
from PIL import Image
hp=Image.open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'held_panel.png')).convert('RGB')
l1rgb=Level(adf,gb,1).rgb;hw,hh=hp.size;hov=bytearray()
for yy in range(hh):
    for xx in range(hw):
        c=hp.getpixel((xx,yy));k=min(range(16),key=lambda i:sum(abs(a-b) for a,b in zip(l1rgb[i],c)))
        solid=hud[138+yy,xx,3]==255
        hov+=bytes(L.rgb[k])+bytes([255 if solid else 0])
P('B',1);P('HHHH',0,138,hw,hh);o+=bytes(hov)
# frame-0 image offsets (optional, last section): the first image record of a multi-image sprite carries its own dx, dy (e.g. level 4's
# floor-tile covers, slot 107: +1, +2), added to the object's projected position. Hero (slot 8) and creature frames use their own tables.
f0=[]
for s_ in range(255):
    e=L.g2.rec(s_)
    if s_==8 or not e or len(e)<0x30 or s16(e,0x18)>=0 or not e[0x0c]&4 or e[0x20]==0:continue
    tab=0x28+u16(e,0x22);dx,dy=s16(e,tab+2),s16(e,tab+4)
    if (dx or dy) and abs(dx)<64 and abs(dy)<64:f0.append((s_,dx,dy))
P('B',1);P('H',len(f0))
for s_,dx,dy in f0:P('Bhh',s_,dx,dy)
# animation programs (optional, last section): for every multi-image type record whose flag bit 2 is set (the original steps exactly those) the program bytes (2-byte steps from +0x28
# up to the frame table, interpreted like the original's stepper at $a97c, see animation-tokens.md), then per placement the initial animation record
# (placement[+0xe]: mode, frame, reverse, pc, divider counter, divider reload).
progs={}
for s_ in range(255):
    e=L.g2.rec(s_)
    if not e or len(e)<0x30 or s16(e,0x18)>=0 or not e[0x0c]&4 or e[0x20]==0:continue
    tab=0x28+u16(e,0x22)
    if 0<tab-0x28<=510:progs[s_]=bytes(e[0x28:tab])
P('B',1);P('H',len(progs))
for s_,b in progs.items():P('BH',s_,len(b));o+=b
pl=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16 or u16(pr,6) not in progs:continue
    q=pr[0xe]
    if q+10>len(pr):continue
    pl.append((pid,pr[q],pr[q+1],pr[q+2],pr[q+3],pr[q+8],pr[q+9]))
P('H',len(pl))
for t in pl:P('H6B',*t)
# movement states (optional, very last section): per creature placement (type flag bit 2, class byte +0x16 == 3) the state byte of its movement record
# (record offset = placement byte +0xd, state = its byte 0). State 3 = the creature is driven by the chase routine at $964a0 of the level's behaviour code
# instead of a movement program (level 1: slots 114 and 115 approach the hero; see claude/creature-ai.md).
mst=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    e=L.g2.rec(u16(pr,6))
    if not e or len(e)<0x30 or not e[0x0c]&4 or e[0x16]!=3:continue
    q=pr[0xd]
    if q==0 or q+10>len(pr):continue
    mst.append((pid,pr[q]))
P('B',1);P('H',len(mst))
for t in mst:P('HB',*t)
# kill rewards (optional, after the movement states): death handler $fc5e reads the creature's part (placement + byte +0xc): score += part[4] (x2 if part[6] bit 2),
# sound $21 unless part[6] bit 1. Measured: room 7 worm adds 40 to the score long at $4a8.
kr=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    e=L.g2.rec(u16(pr,6))
    if not e or len(e)<0x30 or not e[0x0c]&4 or e[0x16]!=3:continue
    q=pr[0xc]
    if q==0 or q+8>len(pr):continue
    kr.append((pid,pr[q+4],pr[q+6]))
P('B',1);P('H',len(kr))
for t in kr:P('HBB',*t)
# appearance sounds (optional, after the kill rewards): the display routine $d14a plays sound (type byte +0x1e) unless it is 0xff; measured: the room 7 worm announces itself with sound 0x42.
sn=[]
for s_ in range(255):
    e=L.g2.rec(s_)
    if not e or len(e)<0x30 or s16(e,0x18)>=0 or not e[0x0c]&4 or e[0x1e]==0xff:continue
    sn.append((s_,e[0x1e]))
P('B',1);P('H',len(sn))
for t in sn:P('BB',*t)
# creature health (optional, after the appearance sounds): the missile hit code $f7b2 subtracts the attacker's damage (its part byte +1) from the target's part byte +0;
# the target is immune when its part byte +5 has the attacker's damage-type bit (the stone is type 0) and invulnerable when part byte +6 bit 0 is set. At <= 0 event $17 (death) is posted.
ch=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    e=L.g2.rec(u16(pr,6))
    if not e or len(e)<0x30 or not e[0x0c]&4 or e[0x16]!=3:continue
    q=pr[0xc]
    if q==0 or q+8>len(pr):continue
    ch.append((pid,pr[q],pr[q+5],pr[q+6]))
P('B',1);P('H',len(ch))
for t in ch:P('HBBB',*t)
# consumables (optional, after the creature health): objects whose type class byte +0x16 is 2 are drunk/eaten with verb 9 ($9f92): part byte 0 = effect number
# (4 poison, 6/8/10/12 protection spell types 0..3, 7 message only), +1 = amount, +2 = doses, +3 = flags (bit 2: the dose hurts instead, bit 1: keep state).
cs=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    e=L.g2.rec(u16(pr,6))
    if not e or len(e)<0x30 or e[0x16]!=2:continue
    q=pr[0xc]
    if q==0 or q+4>len(pr):continue
    cs.append((pid,pr[q],pr[q+1],pr[q+2],pr[q+3]))
P('B',1);P('H',len(cs))
for t in cs:P('HBBBB',*t)
# spells (optional, after the consumables): class byte +0x16 == 1; part byte 0 = spell number, +1 = power, +3 bit 7 = not yet understood, +7 = charges ($ed06)
sp=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    e=L.g2.rec(u16(pr,6))
    if not e or len(e)<0x30 or e[0x16]!=1:continue
    q=pr[0xc]
    if q==0 or q+8>len(pr):continue
    sp.append((pid,pr[q],pr[q+1],pr[q+3],pr[q+7]))
P('B',1);P('H',len(sp))
for t in sp:P('HBBBB',*t)
# use-on scripts (optional, after the spells): event 0x12 blocks [len][12][item id word][ops]: the item is used on this placement (verb 12, $a036)
uo=[]
for i in range(L.g6.n):
    e=L.g6.rec(i)
    if len(e)<0x11 or not e[0x0b]:continue
    p=0x10
    for _ in range(e[0x0b]):
        if p>=len(e) or not e[p]:break
        ln=e[p]
        if e[p+1]&0x7f==0x12 and ln>=4 and len(uo)<255:uo.append((i,u16(e,p+2),bytes(e[p+4:p+ln][:250])))
        elif e[p+1]&0x7f==0x1a and ln>=4 and u16(e,p+2)<0x400 and len(uo)<255:uo.append((i,0x4000|u16(e,p+2),bytes(e[p+4:p+ln][:250])))   # event 0x1a (verb 15, $a136): the item is used on this placement
        elif e[p+1]&0x7f==9 and ln>=4 and e[p+2:p+4]!=b'\0\0' and u16(e,p+2)<0x400 and len(uo)<255:uo.append((i,0x8000|u16(e,p+2),bytes(e[p+4:p+ln][:250])))   # event 9 with a specific other object: contact
        p+=ln
P('B',1);P('H',len(uo))
for i,it,b in uo:P('HHB',i,it,len(b));o+=b
# zone boxes (optional, last): room record byte 0 = offset of the zone section [type = number of zones][1 pad][6 bytes per zone: x0 y0 x1 y1 z0 z1] ($8ae0..$8c68). The hero overlapping zone n posts room event 0x0f with n.
zs=[]
for n in range(100):
    rm=L.g3.rec(n)
    if len(rm)<0x20 or not rm[0x17]&2:continue
    q=rm[0]
    if q+2>=len(rm) or not 1<=rm[q]<=4:continue
    nz=min(rm[q],(len(rm)-q-2)//6)
    if nz:zs.append((n,nz,rm[0x17],bytes(rm[q+2:q+2+6*nz])))
P('B',1);P('H',len(zs))
for n,nz,f,b in zs:P('BBB',n,nz,f);o+=b
# ranged weapons (optional, after the zones): class byte +0x16 == 4 (stone bag); part byte +3 = number of shots, part bytes +0/+1 = the ammo placement id ($ee28/$eeb6/$ef58; texts 14/15/17)
rg=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    e=L.g2.rec(u16(pr,6))
    if not e or len(e)<0x30 or e[0x16]!=4:continue
    q=pr[0xc]
    if q==0 or q+4>len(pr):continue
    rg.append((pid,pr[q+3],u16(pr,q)))
P('B',1);P('H',len(rg))
for pid,sh,am in rg:P('HBH',pid,sh,am)
# hit damage (optional, after the ranged weapons): the damage routine $f7b2 reads the attacker's placement memory at [record + part offset (+0xc)]: +1 = damage, +5 = damage-type bit (tested against the victim's part byte +5).
# Placements without a part have the offset pointing at the end of their own record, so the bytes come from the NEXT placement record in memory (verified in RAM: pickaxe pid 168 -> 17 and bit 169&7).
hd=[]
for pid in range(1024):
    pr=L.g6.rec(pid)
    if len(pr)<16:continue
    buf=bytes(pr)
    for k in range(1,6):
        nx=L.g6.rec(pid+k) if pid+k<L.g6.n else b''
        buf+=bytes(nx)
    q=pr[0xc]
    if q+5<len(buf):hd.append((pid,buf[q+1],buf[q+5]))
P('B',1);P('H',len(hd))
for pid,dm,bt in hd:P('HBB',pid,dm,bt)
# carry-over table (optional, last): the level code's table at base+8 ($e17a) maps the pid of every carried item to its pid in the NEXT level; items not listed are
# dropped at a level change. The part state (charges, doses, flags) of carried items is copied across at run time. Written as B 1, H n, then n x (H old pid, H new pid, H new slot or 0xffff).
import struct as _st
_d=open(adf,'rb').read();_ix=_st.unpack('>256H',_d[22*512:23*512]);_ent=[(_ix[8+2*i],_ix[9+2*i]) for i in range(40)]
cy=[]
_ci=7*(lvl-1)
if _ci<35 and _ent[_ci][0]:
    import lzhuf as _lz
    _code=_lz.decode(_d,_ent[_ci][0]*512)
    _p=4+u16(_code,4+8)
    try:L2=Level(adf,gb,lvl+1) if _ent[7*lvl][0] else None   # the next level may not exist on the disk
    except Exception:L2=None
    while _p+4<=len(_code) and u16(_code,_p)<0x8000 and len(cy)<64:
        o_,n_=u16(_code,_p),u16(_code,_p+2);_p+=4
        sl=0xffff
        if L2:
            try:
                pr=L2.g6.rec(n_)
                if len(pr)>7:sl=u16(pr,6)
            except Exception:pass
        cy.append((o_,n_,sl))
P('B',1);P('H',len(cy))
for t in cy:P('HHH',*t)
open(out,'wb').write(o);print(len(o),'bytes,',len(exam),'examine texts')
