"""Append the scrolling story to data/intro.bin (after the title tune).
usage: python3 export_story.py <frames_dir> <ram_dump.bin> <intro.bin in> <intro.bin out>
frames_dir: emulator frames (640x400 ppm) of the [cr CLS] Disk 1 title sequence at rest on each story page:
  2048, 4344, 5504, 7288, 8624 (see PAGES). ram_dump.bin: the emulator's RAM; the story is plain ASCII at $3300.. with
  '!' lines marking the pauses. The font is learned from those frames (cells of 6x6 px, 6 px advance, 6 px line pitch).
Output section: u16 w, u16 h, RGBA strip; row 0 is text line 7 (the first '!' line), x origin 69 on screen."""
import sys,struct,numpy as np
from PIL import Image
fr,ram,src,dst=sys.argv[1:5]
d=open(ram,'rb').read()
L=d[0x3300:0x4540].decode('latin1').replace('\r','').split('\n')
BG=np.array([238,204,102])
def img(n):return np.asarray(Image.open('%s/frame_%04d.ppm'%(fr,n)).convert('RGB'))[::2,::2].astype(np.uint8)
# (frame, first line, end line, screen y of the first line)
PAGES=[(2048,8,27,45),(4344,28,46,51),(5504,47,68,33),(7288,69,87,51),(8624,88,106,51)]
G={}
for n,a,b,y0 in PAGES:
    im=img(n)
    for j,li in enumerate(range(a,b)):
        for i,ch in enumerate(L[li]):
            if ch==' ':continue
            c=im[y0+6*j:y0+6*j+6,69+6*i:69+6*i+6]
            if ch in G and not (G[ch]==c).all():raise SystemExit('glyph conflict %r'%ch)
            G[ch]=c.copy()
first,last=7,218
W,H=33*6,(last-first)*6
st=np.zeros((H,W,4),np.uint8)
miss=set()
for li in range(first,last):
    for i,ch in enumerate(L[li]):
        if ch in ' !#' or i>=33:continue
        if ch not in G:miss.add(ch);continue
        c=G[ch];m=(np.abs(c.astype(int)-BG).sum(axis=2)>0)
        y=6*(li-first);x=6*i
        blk=st[y:y+6,x:x+6];blk[m,:3]=c[m];blk[m,3]=255
print('glyphs',''.join(sorted(G)),'missing',miss)
o=open(src,'rb').read()+struct.pack('<HH',W,H)+st.tobytes()
open(dst,'wb').write(o);print(len(o))
