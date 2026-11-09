# Experimental full-colour preview of a rendered room (dither removal, smooth shading). usage: edit the paths; input = 320x200 PPM from the headless renderer.
import numpy as np, cv2
im=cv2.imread('/tmp/bd/r0/room0.ppm'); im[150:,230:]=0
S=4;h,w=im.shape[:2]
up=cv2.resize(im,(w*S,h*S),interpolation=cv2.INTER_CUBIC).astype(np.uint8)
x=up.copy()
for i in range(3): x=cv2.bilateralFilter(x,0,26+8*i,7+2*i)
y=x.copy()
for i in range(2): y=cv2.bilateralFilter(y,0,45,12)
lum=cv2.cvtColor(x,cv2.COLOR_BGR2GRAY).astype(np.float32)
g=np.hypot(cv2.Sobel(cv2.GaussianBlur(lum,(0,0),3),cv2.CV_32F,1,0),cv2.Sobel(cv2.GaussianBlur(lum,(0,0),3),cv2.CV_32F,0,1))
m=np.clip(g/50.0,0,1)[...,None]
z=(x*m+(0.5*x+0.5*y)*(1-m)).astype(np.float32)
z=cv2.GaussianBlur(z,(0,0),1.8)
# keep the original local brightness (no washed-out look)
def L(a):return cv2.cvtColor(np.clip(a,0,255).astype(np.uint8),cv2.COLOR_BGR2GRAY).astype(np.float32)
lo=cv2.GaussianBlur(L(up),(0,0),14)+1;lz=cv2.GaussianBlur(L(z),(0,0),14)+1
z=z*(lo/lz)[...,None]
# smooth relight: small, soft, from upper left
hgt=cv2.GaussianBlur(L(z),(0,0),6)
dx=cv2.Sobel(hgt,cv2.CV_32F,1,0,ksize=5);dy=cv2.Sobel(hgt,cv2.CV_32F,0,1,ksize=5)
t=-(dx+dy);t=t/(np.percentile(np.abs(t),97)+1e-3)
shade=np.clip(1+0.22*t,0.8,1.2)
z=z*shade[...,None]
# a little more colour, subtle vignette
hsv=cv2.cvtColor(np.clip(z,0,255).astype(np.uint8),cv2.COLOR_BGR2HSV).astype(np.float32)
hsv[...,1]=np.clip(hsv[...,1]*1.15,0,255)
out=cv2.cvtColor(hsv.astype(np.uint8),cv2.COLOR_HSV2BGR).astype(np.float32)/255
out=np.clip(out,0,1)**1.22                      # richer, darker mid-tones
lu=cv2.cvtColor((out*255).astype(np.uint8),cv2.COLOR_BGR2GRAY).astype(np.float32)[...,None]/255
sh=np.array([1.0,1.05,0.88],np.float32);hi=np.array([0.90,1.03,1.10],np.float32)   # BGR: cool teal shadows, warm highlights
out=out*(sh*(1-lu)+hi*lu)
out=np.clip(out,0,1);out=out*0.7+(out*out*(3-2*out))*0.3
out=(out*255).astype(np.uint8)
cv2.imwrite('/tmp/bd/r0/room0_hq.png',out)
c=lambda a:a[200:520,300:760]
cv2.imwrite('/tmp/bd/r0/cmp.png',np.vstack([c(cv2.resize(cv2.imread('/tmp/bd/r0/room0.ppm'),(1280,800),interpolation=cv2.INTER_NEAREST)),c(out)]))
