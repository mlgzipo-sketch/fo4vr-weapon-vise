import struct, numpy as np, sys
def read(path):
    b=open(path,'rb').read(); o=0
    nl=b.index(b'\n'); o=nl+1
    ver,=struct.unpack_from('<I',b,o); o+=4
    endian=b[o]; o+=1
    user,=struct.unpack_from('<I',b,o); o+=4
    nblocks,=struct.unpack_from('<I',b,o); o+=4
    bsver,=struct.unpack_from('<I',b,o); o+=4
    # export info: author, process script, export script (byte len strings); bsver>=130 adds max filepath
    for _ in range(3 + (1 if bsver>=103 else 0)):
        l=b[o]; o+=1+l
    nt,=struct.unpack_from('<H',b,o); o+=2
    types=[]
    for _ in range(nt):
        l,=struct.unpack_from('<I',b,o); o+=4; types.append(b[o:o+l].decode()); o+=l
    bt=struct.unpack_from('<%dH'%nblocks,b,o); o+=2*nblocks
    sizes=struct.unpack_from('<%dI'%nblocks,b,o); o+=4*nblocks
    ns,maxl=struct.unpack_from('<II',b,o); o+=8
    strings=[]
    for _ in range(ns):
        l,=struct.unpack_from('<I',b,o); o+=4; strings.append(b[o:o+l].decode('latin1')); o+=l
    ng,=struct.unpack_from('<I',b,o); o+=4+4*ng
    blocks=[]
    for i in range(nblocks):
        blocks.append((types[bt[i]],b[o:o+sizes[i]])); o+=sizes[i]
    return bsver,blocks,strings
def avobj(d,o):
    name,=struct.unpack_from('<i',d,o); o+=4
    ne,=struct.unpack_from('<I',d,o); o+=4+4*ne
    o+=4 # controller
    flags,=struct.unpack_from('<I',d,o); o+=4
    t=np.array(struct.unpack_from('<3f',d,o)); o+=12
    r=np.array(struct.unpack_from('<9f',d,o)).reshape(3,3); o+=36
    s,=struct.unpack_from('<f',d,o); o+=4
    o+=4 # collision
    return o,name,t,r,s
def shapes(path):
    bsver,blocks,strings=read(path)
    out=[]
    # parent map
    children={}
    for i,(t,d) in enumerate(blocks):
        if t in ('NiNode','BSFadeNode','BSLeafAnimNode'):
            o,name,tr,r,s=avobj(d,0)
            nc,=struct.unpack_from('<I',d,o); o+=4
            for c in struct.unpack_from('<%di'%nc,d,o): children[c]=i
    xf={}
    for i,(t,d) in enumerate(blocks):
        if t in ('NiNode','BSFadeNode','BSTriShape','BSSubIndexTriShape','BSLeafAnimNode'):
            o,name,tr,r,s=avobj(d,0); xf[i]=(tr,r,s,name)
    def world(i,p):
        while i is not None:
            tr,r,s,_=xf[i]; p=(p*s)@r + tr if False else (p@ r.T)*s+tr  # try r as rows=local axes? check
            i=children.get(i)
        return p
    for i,(t,d) in enumerate(blocks):
        if t not in ('BSTriShape','BSSubIndexTriShape'): continue
        o,name,tr,r,s=avobj(d,0)
        o+=16 # bound
        o+=4*3 # skin, shader, alpha
        desc,=struct.unpack_from('<Q',d,o); o+=8
        ntri,=struct.unpack_from('<I',d,o); o+=4
        nv,=struct.unpack_from('<H',d,o); o+=2
        ds,=struct.unpack_from('<I',d,o); o+=4
        vsize=(desc&0xF)*4
        flags=(desc>>44)
        full = bool(flags & 0x400)
        P=[]
        for v in range(nv):
            if full: P.append(struct.unpack_from('<3f',d,o+v*vsize))
            else: P.append(np.frombuffer(d[o+v*vsize:o+v*vsize+6],dtype='<f2').astype(float))
        o+=nv*vsize
        T=np.array(struct.unpack_from('<%dH'%(ntri*3),d,o)).reshape(-1,3)
        P=np.array(P,dtype=float)
        out.append((strings[name] if name>=0 else '',i,P,T,world))
    return out
if __name__=='__main__':
    for nm,i,P,T,w in shapes(sys.argv[1]):
        Pw=w(i,P)
        print(nm,len(P),len(T),'local min',P.min(0).round(1),'max',P.max(0).round(1),'world min',Pw.min(0).round(1),'max',Pw.max(0).round(1))
