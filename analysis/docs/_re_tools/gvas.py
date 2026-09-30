#!/usr/bin/env python3
"""
Unreal Engine GVAS (SaveGame) parser.
Handles the standard header: magic 'GVAS', SaveGameFileVersion, PackageFileVersion,
EngineVersion (major/minor/patch, changelist, branch), CustomVersionFormat + custom
version GUIDs, then SaveGameClassName, then tagged properties.
"""
import struct, sys, os, json

class B:
    def __init__(self, data): self.d = data; self.p = 0
    def u8(self):  v = self.d[self.p]; self.p += 1; return v
    def i32(self): v = struct.unpack_from('<i', self.d, self.p)[0]; self.p += 4; return v
    def u32(self): v = struct.unpack_from('<I', self.d, self.p)[0]; self.p += 4; return v
    def i64(self): v = struct.unpack_from('<q', self.d, self.p)[0]; self.p += 8; return v
    def u64(self): v = struct.unpack_from('<Q', self.d, self.p)[0]; self.p += 8; return v
    def f32(self): v = struct.unpack_from('<f', self.d, self.p)[0]; self.p += 4; return v
    def f64(self): v = struct.unpack_from('<d', self.d, self.p)[0]; self.p += 8; return v
    def guid(self):
        v = self.d[self.p:self.p+16]; self.p += 16
        a, b, c, d = struct.unpack_from('<IHH', v, 0)
        return f"{a:08X}-{b:04X}-{c:04X}-{v[8]:02X}{v[9]:02X}-" + ''.join(f'{x:02X}' for x in v[10:16])
    def fstr(self):
        n = self.i32()
        if n == 0: return ''
        if n < 0:
            n = -n
            raw = self.d[self.p:self.p + n*2]; self.p += n*2
            return raw.decode('utf-16-le', 'replace').rstrip('\x00')
        raw = self.d[self.p:self.p+n]; self.p += n
        return raw.decode('utf-8', 'replace').rstrip('\x00')

def parse(path, maxdepth=8, dump_all=False):
    data = open(path,'rb').read()
    r = B(data)
    print(f"\n{'='*86}\n{os.path.basename(path)}  size={len(data):,}\n{'='*86}")
    magic = data[:4]
    r.p = 4
    if magic != b'GVAS':
        print(f"  !! magic={magic!r} not GVAS"); return
    sgv = r.i32(); pkv = r.i32()
    maj, mino, pat = r.u16() if False else (r.i32() if False else (struct.unpack_from('<H',data,r.p)[0], None, None))
    r.p -= 2
    maj = struct.unpack_from('<H', data, r.p)[0]; r.p += 2
    mino = struct.unpack_from('<H', data, r.p)[0]; r.p += 2
    pat  = struct.unpack_from('<H', data, r.p)[0]; r.p += 2
    cl   = r.u32()
    branch = r.fstr()
    cvf = r.i32()
    print(f"  SaveGameFileVersion : {sgv}")
    print(f"  PackageFileVersion  : {pkv}")
    print(f"  EngineVersion       : {maj}.{mino}.{pat}   changelist={cl}")
    print(f"  Branch              : {branch!r}")
    print(f"  CustomVersionFormat : {cvf}")
    ncv = r.i32()
    print(f"  CustomVersions      : {ncv}")
    cvs = []
    for i in range(ncv):
        g = r.guid(); ver = r.i32()
        cvs.append((g, ver))
    # SaveGameClassName
    sgc = r.fstr()
    print(f"  SaveGameClassName   : {sgc!r}")
    print(f"  -- property stream starts at 0x{r.p:X} --")

    # dump any long strings (find the payload)
    print("\n  === strings in file (len>=12) ===")
    import re
    seen = 0
    for m in re.finditer(rb'[\x20-\x7E]{12,}', data):
        s = m.group().decode()
        print(f"   0x{m.start():06X}  {s[:200]}")
        seen += 1
        if seen > 80: 
            print("   ... truncated")
            break
    # UTF-16
    print("\n  === UTF-16 strings (len>=12) ===")
    seen = 0
    for m in re.finditer(rb'(?:[\x20-\x7E]\x00){12,}', data):
        s = m.group().decode('utf-16-le')
        print(f"   0x{m.start():06X}  {s[:200]}")
        seen += 1
        if seen > 60:
            print("   ... truncated")
            break

if __name__ == '__main__':
    for p in sys.argv[1:]:
        try: parse(p)
        except Exception as e:
            import traceback; traceback.print_exc()
