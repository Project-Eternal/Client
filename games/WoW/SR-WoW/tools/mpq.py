import os, struct, sys, zlib, bz2
def make_table():
    t=[0]*1280; seed=0x00100001
    for i in range(256):
        idx=i
        for _ in range(5):
            seed=(seed*125+3)%0x2AAAAB; a=(seed&0xFFFF)<<16
            seed=(seed*125+3)%0x2AAAAB; b=seed&0xFFFF
            t[idx]=a|b; idx+=256
    return t
T=make_table()
def h(name,typ):
    s1=0x7FED7FED; s2=0xEEEEEEEE
    for c in name.upper().replace('/','\\').encode():
        v=T[typ*256+c]; s1=(v^(s1+s2))&0xFFFFFFFF; s2=(c+s1+s2+(s2<<5)+3)&0xFFFFFFFF
    return s1
def dec(data,key):
    n=len(data)//4; w=list(struct.unpack('<%dI'%n,data[:n*4])); s2=0xEEEEEEEE; out=[]
    for x in w:
        s2=(s2+T[0x400+(key&0xFF)])&0xFFFFFFFF; x^=(key+s2)&0xFFFFFFFF; key=(((~key<<0x15)+0x11111111)|(key>>0x0B))&0xFFFFFFFF
        s2=(x+s2+(s2<<5)+3)&0xFFFFFFFF; out.append(x)
    return struct.pack('<%dI'%n,*out)
class Parts:
    """A read-only file made of the numbered parts of an archive (common.MPQ.000, common.MPQ.001, ...), read as one file."""
    def __init__(s,paths):
        s.files=[open(p,'rb') for p in paths]; s.sizes=[os.path.getsize(p) for p in paths]; s.pos=0
    def seek(s,pos): s.pos=pos
    def read(s,n):
        out=[]; start=0
        for f,size in zip(s.files,s.sizes):
            if n<=0: break
            if s.pos<start+size:
                f.seek(s.pos-start); chunk=f.read(min(n,start+size-s.pos)); out.append(chunk); s.pos+=len(chunk); n-=len(chunk)
            start+=size
        return b''.join(out)
def part_paths(path):
    """The files that hold the archive `path`: the file itself, or its numbered parts; [] when there is none."""
    if os.path.isfile(path): return [path]
    parts=[]
    while os.path.isfile('%s.%03d'%(path,len(parts))): parts.append('%s.%03d'%(path,len(parts)))
    return parts
class MPQ:
    def __init__(s,path):
        parts=part_paths(path)
        if not parts: raise FileNotFoundError(path)
        s.path=path; s.f=open(parts[0],'rb') if len(parts)==1 else Parts(parts); d=s.f.read(0x2c)
        assert d[:4]==b'MPQ\x1a', path
        (s.hsize,s.asize,s.fmt,s.bs,s.ht,s.bt,s.hn,s.bn)=struct.unpack('<IIHHIIII',d[4:0x20])
        if s.hsize>=0x2c:   # (format 2 and later: the high 16 bits of the table positions)
            hh,bh=struct.unpack('<HH',d[0x28:0x2c]); s.ht|=hh<<32; s.bt|=bh<<32
        s.f.seek(s.ht); s.hash=struct.unpack('<%dI'%(s.hn*4),dec(s.f.read(s.hn*16),h('(hash table)',3)))
        s.f.seek(s.bt); s.block=struct.unpack('<%dI'%(s.bn*4),dec(s.f.read(s.bn*16),h('(block table)',3)))
    def find(s,name):
        a,b,c=h(name,0),h(name,1),h(name,2); i=a&(s.hn-1)
        for _ in range(s.hn):
            e=s.hash[i*4:i*4+4]
            if e[3]==0xFFFFFFFF and e[2]==0xFFFF: return None
            if e[0]==b and e[1]==c and e[3]!=0xFFFFFFFE: return e[3]
            i=(i+1)&(s.hn-1)
        return None
    def read(s,name):
        b=s.find(name)
        if b is None: return None
        off,csz,fsz,fl=s.block[b*4:b*4+4]
        if fl&0x10000: raise Exception('encrypted file: '+name)
        s.f.seek(off); raw=s.f.read(csz)
        if not fl&0xff00: return raw[:fsz]   # not compressed
        if fl&0x1000000: return s.dc(raw,fsz) if csz<fsz else raw   # one unit
        ss=512<<s.bs; n=(fsz+ss-1)//ss
        offs=struct.unpack('<%dI'%(n+1),raw[:4*(n+1)]); out=b''
        for i in range(n):
            sec=raw[offs[i]:offs[i+1]]; want=min(ss,fsz-i*ss)
            out+= sec if len(sec)==want else s.dc(sec,want)
        return out
    def dc(s,d,want):
        m=d[0]; d=d[1:]
        if m==2: return zlib.decompress(d)
        if m==0x10: return bz2.decompress(d)
        if m==0: return d
        raise Exception('compression mask %x'%m)
if __name__=='__main__':
    m=MPQ(sys.argv[1]); r=m.read(sys.argv[2])
    if r is None: print('not found',file=sys.stderr); sys.exit(1)
    sys.stdout.buffer.write(r)
