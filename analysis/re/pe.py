"""Minimal dependency-free PE parser + section/VA helpers for RE work."""
import struct, re, sys

class PE:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        assert d[:2] == b'MZ', 'not MZ'
        self.e_lfanew = struct.unpack_from('<I', d, 0x3C)[0]
        assert d[self.e_lfanew:self.e_lfanew+4] == b'PE\0\0', 'not PE'
        coff = self.e_lfanew + 4
        (self.machine, self.nsections, self.timestamp, self.ptr_symtab,
         self.nsymbols, self.size_opt, self.characteristics) = struct.unpack_from('<HHIIIHH', d, coff)
        opt = coff + 20
        self.opt_off = opt
        self.magic = struct.unpack_from('<H', d, opt)[0]
        assert self.magic == 0x20b, f'not PE32+ ({self.magic:#x})'
        self.entry = struct.unpack_from('<I', d, opt + 16)[0]
        self.image_base = struct.unpack_from('<Q', d, opt + 24)[0]
        self.section_align = struct.unpack_from('<I', d, opt + 32)[0]
        self.file_align = struct.unpack_from('<I', d, opt + 36)[0]
        self.size_image = struct.unpack_from('<I', d, opt + 56)[0]
        self.size_headers = struct.unpack_from('<I', d, opt + 60)[0]
        self.subsystem = struct.unpack_from('<H', d, opt + 68)[0]
        self.dllchar = struct.unpack_from('<H', d, opt + 70)[0]
        # data directories
        self.ddir = []
        ndd = struct.unpack_from('<I', d, opt + 108)[0]
        for i in range(ndd):
            rva, size = struct.unpack_from('<II', d, opt + 112 + i*8)
            self.ddir.append((rva, size))
        self.sections = []
        so = opt + self.size_opt
        for i in range(self.nsections):
            base = so + i*40
            name = d[base:base+8].rstrip(b'\0').decode('latin1')
            vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', d, base + 8)
            chars = struct.unpack_from('<I', d, base + 36)[0]
            self.sections.append(dict(name=name, vsize=vsize, vaddr=vaddr,
                                      rawsize=rawsize, rawptr=rawptr, chars=chars))

    # ---------- address conversion ----------
    def sec_of_rva(self, rva):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rawsize']):
                return s
        return None

    def rva2off(self, rva):
        s = self.sec_of_rva(rva)
        if not s:
            return None
        d = rva - s['vaddr']
        if d >= s['rawsize']:
            return None
        return s['rawptr'] + d

    def off2rva(self, off):
        for s in self.sections:
            if s['rawptr'] <= off < s['rawptr'] + s['rawsize']:
                return s['vaddr'] + (off - s['rawptr'])
        return None

    def va(self, rva):
        return self.image_base + rva

    def off2va(self, off):
        r = self.off2rva(off)
        return None if r is None else self.image_base + r

    def va2off(self, v):
        return self.rva2off(v - self.image_base)

    def sec(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def secbytes(self, name):
        s = self.sec(name)
        return self.data[s['rawptr']:s['rawptr']+s['rawsize']]

    def read(self, va, n):
        o = self.va2off(va)
        if o is None:
            return None
        return self.data[o:o+n]

    def u64(self, va):
        b = self.read(va, 8)
        return None if not b or len(b) < 8 else struct.unpack('<Q', b)[0]

    def u32(self, va):
        b = self.read(va, 4)
        return None if not b or len(b) < 4 else struct.unpack('<I', b)[0]

    def getstr(self, va, maxlen=256):
        o = self.va2off(va)
        if o is None:
            return None
        e = self.data.find(b'\0', o, o+maxlen)
        if e < 0:
            e = o + maxlen
        return self.data[o:e]

    def imports(self):
        """yield (dllname, [funcnames])"""
        out = []
        if len(self.ddir) < 2:
            return out
        rva = self.ddir[1][0]
        off = self.rva2off(rva)
        if off is None:
            return out
        while True:
            desc = self.data[off:off+20]
            if len(desc) < 20 or desc == b'\0'*20:
                break
            oft, ts, fc, name_rva, iat = struct.unpack('<IIIII', desc)
            no = self.rva2off(name_rva)
            dll = self.data[no:self.data.find(b'\0', no)] .decode('latin1') if no else '?'
            funcs = []
            t = self.rva2off(oft or iat)
            if t:
                p = t
                while True:
                    v = struct.unpack_from('<Q', self.data, p)[0]
                    if v == 0:
                        break
                    if v & 0x8000000000000000:
                        funcs.append('ord#%d' % (v & 0xffff))
                    else:
                        fo = self.rva2off(v & 0x7fffffff)
                        if fo:
                            nm = self.data[fo+2:self.data.find(b'\0', fo+2)].decode('latin1')
                            funcs.append(nm)
                    p += 8
                    if len(funcs) > 5000:
                        break
            out.append((dll, funcs))
            off += 20
        return out

    def exports(self):
        out = []
        if len(self.ddir) < 1 or not self.ddir[0][0]:
            return out
        rva, size = self.ddir[0]
        off = self.rva2off(rva)
        if off is None:
            return out
        (chars, ts, maj, mnr, namerva, ordinal_base, nfunc, nname,
         afunc, aname, aord) = struct.unpack_from('<IIHHIIIIIII', self.data, off)
        no = self.rva2off(namerva)
        dll = self.data[no:self.data.find(b'\0', no)].decode('latin1') if no else '?'
        return [dll, nfunc, nname]

    def relocs_count(self):
        if len(self.ddir) < 6 or not self.ddir[5][0]:
            return 0
        rva, size = self.ddir[5]
        off = self.rva2off(rva)
        end = off + size
        n = 0
        while off < end:
            page, bsize = struct.unpack_from('<II', self.data, off)
            if bsize == 0:
                break
            n += (bsize - 8) // 2
            off += bsize
        return n


def utf16le(s):
    return s.encode('utf-16-le')


def find_all(hay, needle, limit=1000):
    out = []
    i = hay.find(needle)
    while i >= 0 and len(out) < limit:
        out.append(i)
        i = hay.find(needle, i+1)
    return out


if __name__ == '__main__':
    p = PE(sys.argv[1])
    print('path', p.path)
    print('machine %#x  sections %d  ts %#x' % (p.machine, p.nsections, p.timestamp))
    print('imagebase %#x  entry %#x  size_image %#x  subsystem %d' % (
        p.image_base, p.image_base+p.entry, p.size_image, p.subsystem))
    for s in p.sections:
        print('  %-8s va=%#012x vsz=%#010x raw=%#010x ptr=%#010x chars=%#010x' % (
            s['name'], p.image_base+s['vaddr'], s['vsize'], s['rawsize'], s['rawptr'], s['chars']))
    print('relocs:', p.relocs_count())
    for dll, fs in p.imports():
        print('  import %-30s %d funcs' % (dll, len(fs)))
        print('        ', ', '.join(fs[:25]))
