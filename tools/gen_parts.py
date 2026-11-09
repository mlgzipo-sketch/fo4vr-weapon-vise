"""Extract the weapon mods (OMOD) of the supported weapons from Fallout4.esm into data/parts.csv.

Usage: gen_parts.py <Fallout4.esm> <out.csv>
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
byedid={edid(f):f for f in r}
WEAPONS={'10mm':'mod_10mm_'}
REQUIRED={'ap_gun_receiver','ap_gun_Barrel','ap_gun_Mag','ap_gun_Grip'}
rows=[]
for wname,prefix in WEAPONS.items():
    mods=[]
    for fid,(t,s) in sorted(r.items()):
        e=edid(fid)
        if t!=b'OMOD' or not e.startswith(prefix): continue
        d=g(s,'DATA')[0]; o=8+2+4+2
        ap=edid(struct.unpack_from('<I',d,o)[0])
        model=(g(s,'MODL') or [b''])[0].rstrip(b'\0').decode()
        loose=struct.unpack('<I',g(s,'LNAM')[0])[0] if g(s,'LNAM') else 0
        visible= bool(model) and 'ParentObject' not in model and ap!='ap_gun_receiver'
        mods.append(dict(omod=fid,edid=e,slot=ap,model=model,loose=loose,part=int(visible)))
    for m in mods:
        fb=[x for x in mods if x['slot']==m['slot'] and not x['part'] and x['slot']!='ap_gun_receiver']
        m['fallback']=fb[0]['omod'] if fb and m['part'] else 0
        m['required']=int(m['slot'] in REQUIRED)
        m['weapon']=byedid[wname]
        rows.append(m)
w=csv.writer(open(sys.argv[2],'w',newline=''))
w.writerow(['omod','edid','weapon','slot','model','loose','part','fallback','required'])
for m in rows:
    w.writerow(['%06X'%m['omod'],m['edid'],'%06X'%m['weapon'],m['slot'],m['model'],'%06X'%m['loose'],m['part'],'%06X'%m['fallback'],m['required']])
print(len(rows))
