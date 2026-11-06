"""Readable re-implementation of Cadaver's walking + object pushing (prototype, validated vs emulator)."""
import sys,glob
sys.path.insert(0,'ext')
from cadaver_data import *
ROOM=80
MARGIN=6   # $89b/$89c: minimum allowed min-corner coordinate
class Obj:
    def __init__(s,L,pid):
        p=L.g6.rec(pid);s.pid=pid;s.x,s.y,s.z=p[0],p[1],p[2];s.flags=p[3];s.slot=u16(p,6)
        e=L.g2.rec(s.slot);s.fx,s.fy,s.fz=s16(e,0x10),s16(e,0x12),s16(e,0x14)
        s.pushable=bool(e[0x0c]&3);s.name=s.slot
    def box(s):return (s.x-s.fx+1,s.y-s.fy+1,s.x,s.y,s.z,s.z+s.fz-1)
def overlap(a,b):
    return not(a[2]<b[0] or b[2]<a[0] or a[3]<b[1] or b[3]<a[1] or a[5]<b[4] or b[5]<a[4])
class Room:
    def __init__(s,L,n):
        pl=L.g5.rec(n);ids=[u16(pl,2*i) for i in range(len(pl)//2)]
        s.objs=[Obj(L,i) for i in ids if not L.g6.rec(i)[3]&0x80]
        s.player=s.objs[0];ri=L.room(n);s.w,s.h=ri['sx']*8,ri['sy']*8
    def blockers(s,o,nx,ny):
        ghost=Obj.__new__(Obj);ghost.__dict__.update(o.__dict__);ghost.x,ghost.y=nx,ny
        return [q for q in s.objs if q is not o and overlap(ghost.box(),q.box())]
    def try_move(s,o,dx,dy,depth=0):
        nx,ny=o.x+dx,o.y+dy
        if nx>=s.w or ny>=s.h or nx-o.fx+1<MARGIN or ny-o.fy+1<MARGIN or depth>0x1f:return False
        hits=s.blockers(o,nx,ny)
        if any(not h.pushable for h in hits):return False
        moved=[]
        for h in hits:
            if not s.try_move(h,dx,dy,depth+1):
                for m,(ox,oy) in moved:m.x,m.y=ox,oy
                return False
        o.x,o.y=nx,ny;return True
if __name__=='__main__':
    L=Level(glob.glob('new_d2/*.adf')[0],'game2_blob.bin',1);R=Room(L,0)
    p=R.player;print('start',p.x,p.y)
    xs=[]
    for tick in range(60):
        if not R.try_move(p,1,0):break
        xs.append(p.x)
    print('stopped at',p.x,p.y,'after',len(xs),'steps')
    print({o.slot:(o.x,o.y) for o in R.objs if o.slot in(54,32)})

# ---------------------------------------------------------------- rooms / exits
FPX=FPY=7   # hero footprint
class World:
    """Rooms on a world grid; exits are the wall runs (group 4) at room edges."""
    def __init__(s,L,start=0):
        s.L=L;s.rooms={};s.cur=start;s.load(start)
    def load(s,n):
        if n not in s.rooms:
            R=Room(s.L,n);R.info=s.L.room(n);R.walls=wall_boxes(s.L,R.info);s.rooms[n]=R
        s.R=s.rooms[n];s.cur=n
    def neighbour(s,wx,wy):
        for n in range(len(s.L.g3.entries) if hasattr(s.L.g3,'entries') else 100):
            r=s.L.room(n)
            if not r or n==s.cur:continue
            if r['x0']<=wx<r['x0']+r['sx'] and r['y0']-1<=wy<=r['y0']+r['sy']:return n
        return None
    def step(s,dx,dy):
        R=s.R;p=R.player
        if R.try_move(p,dx,dy):return 'moved'
        nx,ny=p.x+dx,p.y+dy
        out=nx>=R.w or ny>=R.h or nx-FPX+1<MARGIN or ny-FPY+1<MARGIN
        if not out:return 'blocked'
        for w in R.walls:
            mnx,mny,mxx,mxy=w['box']
            if dy!=0:   # up/down pressed: mover's x range must lie inside the wall's x range ($8562)
                if not(mnx<=nx-FPX+1 and nx<=mxx):continue
            else:       # otherwise its y range must lie inside the wall's y range
                if not(mny<=ny-FPY+1 and ny<=mxy):continue
            wx,wy=w['x'],w['y']
            new=s.neighbour(wx,wy)
            if new is None:continue
            old=R.info;s.load(new);ni=s.R.info
            if w['orient']!=0:
                x=(wx+1-ni['x0'])*8+4
                y=ni['sy']*8-1 if ((ny-MARGIN)&~7)==0 else MARGIN+FPY
            else:
                y=(wy+1-ni['y0'])*8+4
                x=ni['sx']*8-1 if ((nx-MARGIN)&~7)==0 else MARGIN+FPX
            pl=Obj.__new__(Obj);pl.__dict__.update(p.__dict__);pl.x,pl.y=x,y
            s.R.objs[0:0]=[pl];s.R.player=pl
            return 'room %d'%new
        return 'blocked'

def wall_boxes(L,r):
    out=[]
    for w in r['walls']:
        if w==0xffff:continue
        a=L.g4.rec(w);x,y=a[0],a[1];ln=a[4];orient=a[5]
        d3=ln-1
        if orient==0:
            d2=2;rx=x-r['x0']
            if rx<0:rx+=1;d2=0
            ry=y-r['y0']
        else:
            d2=3;ry=y-r['y0']
            if ry<0:ry+=1;d2=1
            rx=x-r['x0']
        e=L.g2.rec(d3*4+d2);fx,fy=s16(e,0x10),s16(e,0x12)
        mnx,mny=rx*8,ry*8;X,Y=mnx,mny
        if fx==8:mny-=4;Y+=4
        if fy==8:mnx-=4;X+=4
        out.append(dict(idx=w,x=x,y=y,orient=orient,box=(mnx,mny,X+fx-1,Y+fy-1)))
    return out
