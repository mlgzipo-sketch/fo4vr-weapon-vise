import numpy as np, math, sys
sys.path.insert(0,'.')
from nifread import shapes
D="/mnt/user-data/uploads/Fallout 4 VR/Data/Meshes/RigVR/Custom/"
tris=[]
for f in ('RigBase_Upper','RigBase_Lower'):
    for nm,i,P,T,w in shapes(D+f+'.nif'):
        tris.append(P[T])
tris=np.concatenate(tris)  # (n,3,3)
old={};cur=None
for l in open(sys.argv[1]).read().splitlines():
    p=l.split()
    if p and p[0]=='area': cur=p[1]; old[cur]=[]
    elif p and p[0]=='tri': old[cur].append(np.array(list(map(float,p[1:10]))).reshape(3,3))
def mk(tr):
    A=tr[:,0]; return A,tr[:,1]-A,tr[:,2]-A
RIG=mk(tris)
def ray(o,d,M=None):
    A,E1,E2=M or RIG
    p=np.cross(d,E2); det=(E1*p).sum(1)
    ok=np.abs(det)>1e-9; inv=np.where(ok,1/np.where(ok,det,1),0)
    s=o-A; u=(s*p).sum(1)*inv
    q=np.cross(s,E1); v=(q*d).sum(1)*inv
    t=(E2*q).sum(1)*inv
    m=ok&(u>=0)&(v>=0)&(u+v<=1)&(t>0)
    return t[m].min() if m.any() else None
def strip(name,oldoff,z0,z1,a0,a1,offset,step_a=7.5,step_z=2.5):
    OLD=mk(np.array(old[name]))
    zs=[z0+i*step_z for i in range(int(round((z1-z0)/step_z))+1)]
    angs=[a0+i*step_a for i in range(int(round((a1-a0)/step_a))+1)]
    rows=[]
    for z in zs:
        row=[]
        for a in angs:
            d=np.array([math.sin(math.radians(a)),math.cos(math.radians(a)),0.0])
            t=ray(np.array([0,0,z])+d*60,-d)
            r=60-t if t is not None and t<60 else None
            to=ray(np.array([0,0,z])+d*60,-d,OLD)
            ro=60-to-oldoff+1.0 if to is not None and to<60 else None   # the body (old area less its offset) plus the rig's thickness
            if r is None or (ro is not None and ro>r): r=ro
            row.append((d,r) if r is not None else None)
        rows.append(row)
    for _ in range(3):
        nxt=[]
        for i,row in enumerate(rows):
            out=[]
            for j,c in enumerate(row):
                if c is None: out.append(None); continue
                vals=[c[1]]*2
                for di,dj in ((-1,0),(1,0),(0,-1),(0,1)):
                    ii,jj=i+di,j+dj
                    if 0<=ii<len(rows) and 0<=jj<len(row) and rows[ii][jj] is not None: vals.append(rows[ii][jj][1])
                out.append((c[0],sum(vals)/len(vals)))
            nxt.append(out)
        rows=nxt
    pts=[[(np.array([0,0,zs[i]])+c[0]*(c[1]+offset)) if c else None for c in row] for i,row in enumerate(rows)]
    out=[]
    for i in range(len(pts)-1):
        for j in range(len(angs)-1):
            q=[pts[i][j],pts[i][j+1],pts[i+1][j+1],pts[i+1][j]]
            if all(x is not None for x in q):
                out.append((q[0],q[1],q[2])); out.append((q[0],q[2],q[3]))
    return out
A_={'Belt':strip('Belt',0.6,68.0,79.0,-180.0,180.0,0.4),'Chest':strip('Chest',3.0,84.0,104.0,-45.0,45.0,0.4),'Back':strip('Back',3.5,84.0,110.0,130.0,230.0,0.4)}
src=open(sys.argv[1]).read().splitlines()
# compare winding with the old file's first triangle per area to keep the same order convention
out=[];cur=None
for l in src:
    p=l.split()
    if p and p[0]=='tri': continue
    out.append(l)
    if p and p[0]=='area':
        for t in A_[p[1]]:
            out.append('tri '+' '.join('%.3f %.3f %.3f'%tuple(v) for v in t))
open(sys.argv[2],'w').write('\n'.join(out)+'\n')
print({k:len(v) for k,v in A_.items()})
