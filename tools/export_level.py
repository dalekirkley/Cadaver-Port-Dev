"""Export one Cadaver level to a flat binary the native game reads (little endian).
usage: python3 export_level.py <disk2.adf> <game_blob.bin> <level> <out.bin>"""
import sys,struct,glob
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
    F=[s16(e,x) for x in (2,4,0x10,0x12,0x14)];P('5h',*F);P('B',e[0x0c]&3);P('3h',F[2],F[3],F[4])
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
    bg=L.render_room(n,objects=False);o+=bg.tobytes()
    W=wall_boxes(L,r);P('H',len(W))
    for w in W:P('hhB4h',w['x'],w['y'],w['orient'],*w['box'])
    lst=L.g5.rec(n);ids=[u16(lst,k) for k in range(0,len(lst),2)];P('H',len(ids))
    for pid in ids:
        p=L.g6.rec(pid);P('HBBBBH',pid,p[0],p[1],p[2],p[3],u16(p,6))
open(out,'wb').write(o);print(len(o),'bytes')
