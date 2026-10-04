#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一个名字串的所有引用点（含 4 字节 RVA 形式）连同上下文一起摊出来。

    python whereis.py Emeralds EmeraldAmount
"""

import struct
import sys

from peimg import Image

img = Image()
IMG = 0x140000000
RD_LO, RD_HI = 0x148E33600, 0x14BA78000
ALL = ('.rdata', '.data', '_RDATA', '.text')


def find_names(name):
    pat = name.encode() + b'\0'
    out = []
    for s in img.sections:
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        st = 0
        while True:
            i = d.find(pat, st)
            if i < 0:
                break
            st = i + 1
            va = base + i
            if RD_LO <= va < RD_HI:
                out.append(va)
    return out


def refs(name_va):
    p64 = struct.pack('<Q', name_va)
    p32 = struct.pack('<I', name_va - IMG)
    out = []
    for s in img.sections:
        if s['name'] not in ALL:
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        for pat, kind in ((p64, 'va64'), (p32, 'rva32')):
            st = 0
            while True:
                i = d.find(pat, st)
                if i < 0:
                    break
                st = i + 1
                out.append((base + i, kind, s['name']))
    return out


def dump_semantic(center, span_before=0x40, span_after=0x20):
    lo = center - span_before
    b = img.read_va(lo, span_before + span_after)
    if not b:
        return
    for off in range(0, len(b) - 3, 4):
        v4 = struct.unpack_from('<I', b, off)[0]
        v8 = struct.unpack_from('<Q', b, off)[0] if off + 8 <= len(b) else 0
        note = []
        if off % 8 == 0 and RD_LO <= v8 < RD_HI:
            s = img.cstr(v8, 48)
            if s and len(s) > 1 and all(32 <= ord(c) < 127 for c in s):
                note.append('STRVA %r' % s)
        if 0x1000 <= v4 < 0x8E31000 and RD_LO <= IMG + v4 < RD_HI:
            s = img.cstr(IMG + v4, 48)
            if s and len(s) > 1 and all(32 <= ord(c) < 127 for c in s):
                note.append('STRrva %r' % s)
        if 0x140001000 <= v8 < 0x148E31000:
            note.append('CODEVA')
        print('   %#012x  (%+#05x)  u32=%#010x %-11d u64=%#018x  %s' %
              (lo + off, (lo + off) - center, v4, v4, v8, ' | '.join(note)))


def main():
    for name in sys.argv[1:]:
        for nva in find_names(name):
            print('==== %s name@%#x' % (name, nva))
            for slot, kind, sec in refs(nva):
                print('  ref %#012x  [%s]  %s' % (slot, sec, kind))
                dump_semantic(slot, 0x40, 0x18)
                print()
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
