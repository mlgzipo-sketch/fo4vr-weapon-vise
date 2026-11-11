"""Extract the visible gun parts (OMOD with a model of its own) of every weapon in Fallout4.esm.

Usage: gen_parts.py <Fallout4.esm> <parts.csv>
An OMOD belongs to the weapons whose keywords hold its MNAM target keyword (ma_10mm, ...).
Rows already in <parts.csv> keep their place and values (their order is the order of the
plugin's form lists, which saves rely on); new rows are appended.
"""
import struct,csv,sys
import zlib, mmap
def parse(path, want=(b'WEAP',b'OMOD',b'MISC',b'COBJ',b'KYWD',b'FLST')):
    f=open(path,'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
    out={}
    def walk(off,end):
        while off<end:
            t=m[off:off+4]; size=struct.unpack_from('<I',m,off+4)[0]
            if t==b'GRUP':
                label=m[off+8:off+12]; gt=struct.unpack_from('<i',m,off+12)[0]
                if gt!=0 or label in want: walk(off+24,off+size)
                off+=size
            else:
                flags,fid=struct.unpack_from('<II',m,off+8)
                if t in want:
                    data=m[off+24:off+24+size]
                    if flags&0x40000: data=zlib.decompress(data[4:])
                    out[fid]=(t,subs(data))
                off+=24+size
    walk(0,len(m)) if False else None
    # skip TES4
    sz=struct.unpack_from('<I',m,4)[0]; walk(24+sz,len(m))
    return out
def subs(d):
    r=[];i=0;big=None
    while i<len(d):
        t=d[i:i+4].decode('latin1'); s=struct.unpack_from('<H',d,i+4)[0]
        if t=='XXXX': big=struct.unpack_from('<I',d,i+6)[0]; i+=10; continue
        if big is not None: s=big; big=None
        r.append((t,bytes(d[i+6:i+6+s]))); i+=6+s
    return r


r=parse(sys.argv[1])
def g(s,t): return [d for k,d in s if k==t]
def edid(fid):
    x=r.get(fid); e=g(x[1],'EDID') if x else []
    return e[0].rstrip(b'\0').decode() if e else ''
REQUIRED={'ap_gun_receiver','ap_gun_Barrel','ap_gun_Mag','ap_gun_Grip'}
kw2weap={}
for fid,(t,s) in sorted(r.items()):
    if t!=b'WEAP': continue
    for k in g(s,'KWDA'):
        for i in range(0,len(k),4):
            kw2weap.setdefault(struct.unpack_from('<I',k,i)[0],[]).append(fid)
mods=[]
for fid,(t,s) in sorted(r.items()):
    if t!=b'OMOD': continue
    d=(g(s,'DATA') or [b''])[0]
    if len(d)<20: continue
    ap=edid(struct.unpack_from('<I',d,16)[0])
    if not ap.startswith('ap_gun_'): continue
    targets=tuple(sorted(struct.unpack_from('<I',x,i)[0] for x in g(s,'MNAM') for i in range(0,len(x),4)))
    weapons=sorted({w for k in targets for w in kw2weap.get(k,[])})
    if not weapons: continue
    model=(g(s,'MODL') or [b''])[0].rstrip(b'\0').decode()
    loose=struct.unpack('<I',g(s,'LNAM')[0])[0] if g(s,'LNAM') else 0
    visible=bool(model) and 'ParentObject' not in model and ap!='ap_gun_receiver'
    mods.append(dict(omod=fid,edid=edid(fid),slot=ap,model=model,loose=loose,part=int(visible),targets=targets,weapons=weapons))
for m in mods:
    fb=[x for x in mods if x['slot']==m['slot'] and x['targets']==m['targets'] and not x['part']]
    m['fallback']=fb[0]['omod'] if fb and m['part'] else 0
    m['required']=int(m['slot'] in REQUIRED)
old=[]
try:
    old=list(csv.reader(open(sys.argv[2],newline='')))[1:]
except FileNotFoundError:
    pass
known={int(row[0],16) for row in old}
new=[m for m in mods if m['omod'] not in known]
new.sort(key=lambda m:(edid(m['weapons'][0]).lower(), m['slot'], m['edid']))
w=csv.writer(open(sys.argv[2],'w',newline=''), lineterminator='\n')
w.writerow(['omod','edid','weapon','slot','model','loose','part','fallback','required'])
byid={m['omod']:m for m in mods}
for row in old:
    m=byid.get(int(row[0],16))
    if m:   # all weapons this mod fits, the rest as before
        row[2]=';'.join('%06X'%x for x in m['weapons'])
    w.writerow(row)
for m in new:
    w.writerow(['%06X'%m['omod'],m['edid'],';'.join('%06X'%x for x in m['weapons']),m['slot'],m['model'],'%06X'%m['loose'],m['part'],'%06X'%m['fallback'],m['required']])
print(len(old),'kept,',len(new),'added,',sum(m['part'] for m in new),'new parts,',len({w for m in mods for w in m['weapons']}),'weapons')
