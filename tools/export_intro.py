"""Export the title/intro sequence to data/intro.bin.
usage: python3 export_intro.py <frames_dir> <out.bin> [music.wav]
frames_dir holds frame_NNNN.ppm (640x400) from the emulator running the CLEAN Disk 1 for 283 frames, every frame dumped:
  cd jt; DUMP=$(seq -s, 1 1 283) ./hl2 "<disk1>" "<disk2>" 284
The title appears at frame 15 (logos before it), candle flames change every frame, the credits text rises 1 px per 2 frames
(first visible frame 44, at rest on frame 210), clipped at y=160."""
import sys,struct,numpy as np
from PIL import Image
d,out=sys.argv[1],sys.argv[2]
def A(n):return np.asarray(Image.open('%s/frame_%04d.ppm'%(d,n)).convert('RGB'))[::2,::2].astype(np.int32)
def rgba(a,mask=None):
    h,w=a.shape[:2];o=np.zeros((h,w,4),np.uint8);o[:,:,:3]=a;o[:,:,3]=255 if mask is None else (mask*255)
    return o
o=bytearray(b'INTR')
def img(a,x=0,y=0):
    global o
    h,w=a.shape[:2];o+=struct.pack('<hhHH',x,y,w,h)+a.tobytes()
img(rgba(A(14)))                                  # logos screen
base=A(19);img(rgba(base))                       # title: parchment, header, candles
# flames
FL=[(14,20,14,26),(286,20,14,26)]
seqs=[]
for (x,y,w,h) in FL:
    ids={};seq=[]
    for n in range(15,284):
        r=A(n)[y:y+h,x:x+w];k=ids.setdefault(r.tobytes(),len(ids));seq.append(k)
    seqs.append((ids,seq))
o+=struct.pack('<H',len(FL))
for (x,y,w,h),(ids,seq) in zip(FL,seqs):
    o+=struct.pack('<HHHH',x,y,w,h)+struct.pack('<H',len(ids))
    for key in ids:o+=np.frombuffer(key,np.int32).reshape(h,w,3).astype(np.uint8).tobytes() if False else rgba(np.frombuffer(key,np.int32).reshape(h,w,3)).tobytes()
    o+=struct.pack('<H',len(seq))+bytes(k if k<256 else 0 for k in seq)
# credits text layer
fin=A(283);m=(np.abs(fin-base).sum(axis=2)>0);m[:60,:]=False;m[:,:40]=False;m[:,285:]=False
ys,xs=np.where(m);x0,y0,x1,y1=xs.min(),ys.min(),xs.max(),ys.max()
img(rgba(fin[y0:y1+1,x0:x1+1],m[y0:y1+1,x0:x1+1]),int(x0),int(y0))
o+=struct.pack('<HHHH',160,195,44-15,210-15)      # clip y, rest frame (title frames), first visible, rest absolute
if len(sys.argv)>3:
    # music: the emulator's WAV capture of the [cr CLS] disk (44.1 kHz stereo). The tune loops after 181.44 s (it restarts at
    # 181.541 s, the same onset as the first pass at 0.101 s); both channels are mixed to mono 22.05 kHz int16.
    w=np.fromfile(sys.argv[3],dtype=np.int16,offset=44).reshape(-1,2).astype(np.int32)[int(0.1015*44100):int(181.541*44100)]
    w=(w-w[:50].mean(axis=0).astype(np.int32)).sum(axis=1)//2          # remove the idle-channel DC offsets
    w=((w[0::2]+w[1::2])//2).clip(-32768,32767).astype(np.int16)
    o+=struct.pack('<II',22050,len(w))+w.tobytes()
open(out,'wb').write(o);print(len(o),'bytes; text',x0,y0,x1,y1,'flame images',[len(s[0]) for s in seqs])
