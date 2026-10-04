#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 4 字节 RVA 找属性名引用（本构建的表项用 RVA，而不是完整 64 位指针）。

    python find_rva_refs.py EmeraldAmount [more...]

对每个名字串 VA，在 .rdata/.data/_RDATA 里找 4 字节小端整数 == RVA(名字串) 的位置，
并把周围 0x60 字节按 8 字节槽打印出来，人工判读表项。
"""

import struct
import sys

from peimg import Image

SECTIONS = ('.rdata', '.data', '_RDATA')


def scan_refs(img, target_rva, sections=SECTIONS):
    pat = struct.pack('<I', target_rva)
    out = []
    for s in img.sections:
        if s['name'] not in sections:
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        start = 0
        while True:
            i = data.find(pat, start)
            if i < 0:
                break
            start = i + 1
            q = i & ~7
            out.append((base + i, base + q))
    return out


def main():
    img = Image()
    for arg in sys.argv[1:]:
        nm = arg
        pats = [nm.encode('ascii') + b'\0']
        hits = []
        for s in img.sections:
            img.f.seek(s['rawptr'])
            data = img.f.read(s['rawsize'])
            for p in pats:
                start = 0
                while True:
                    i = data.find(p, start)
                    if i < 0:
                        break
                    start = i + 1
                    hits.append(img.image_base + s['vaddr'] + i)
        print('==== %s  name VAs: %s' % (nm, [hex(h) for h in hits]))
        for va in hits:
            refs = scan_refs(img, va - img.image_base)
            print('  RVA-ref hits: %d' % len(refs))
            seen = set()
            for exact, q in refs[:20]:
                if q in seen:
                    continue
                seen.add(q)
                print('   -- slot-aligned at %#x (exact ref %#x)' %
                      (q, exact))
                b = img.read_va(q - 0x20, 0x60)
                for off in range(0, len(b), 8):
                    v = struct.unpack_from('<Q', b, off)[0]
                    lo = struct.unpack_from('<I', b, off)[0]
                    hi = struct.unpack_from('<I', b, off + 4)[0]
                    note = ''
                    if 0x140000000 <= v < 0x14c9bb000:
                        cs = img.cstr(v, 32)
                        note = 'va? %r' % cs
                    elif lo and lo < 0x10000000 and hi == 0:
                        cs = img.cstr(img.image_base + lo, 32)
                        if cs and cs.isprintable():
                            note = 'rva->%r' % cs
                    print('      %012x  %012x  %-28s %s' %
                          (q - 0x20 + off, v, '%08x %08x' % (hi, lo), note))
        print()
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
