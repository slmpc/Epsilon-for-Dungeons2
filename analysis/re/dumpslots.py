#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一个 VA 附近的 0x40*N 字节按 8 字节槽和 4 字节槽同时打印，并解释每个槽。"""

import struct
import sys

from peimg import Image

img = Image()
TEXT_LO = TEXT_HI = None
for _s in img.sections:
    if _s['name'] == '.text':
        TEXT_LO = img.image_base + _s['vaddr']
        TEXT_HI = TEXT_LO + _s['vsize']


def code_rva(v):
    return TEXT_LO - img.image_base <= v < TEXT_HI - img.image_base


def note32(v, base):
    out = []
    if code_rva(v):
        out.append('CODE_RVA %#x -> VA %#x' % (v, img.image_base + v))
    if 0x1000 <= v < 0xcd94000:
        s = img.cstr(img.image_base + v, 32)
        if s and s.isprintable() and len(s) > 1:
            out.append('STRrva %r' % s)
    return out


def main():
    for a in sys.argv[1:]:
        start = int(a, 0)
        print('==== dump %#x' % start)
        for row in range(start, start + 0x100, 0x40):
            b = img.read_va(row, 0x40)
            print('  --- %#012x' % row)
            for off in range(0, 0x40, 8):
                q = struct.unpack_from('<Q', b, off)[0]
                lo = struct.unpack_from('<I', b, off)[0]
                hi = struct.unpack_from('<I', b, off + 4)[0]
                notes = []
                s = img.cstr(q, 32) if 0x140000000 <= q < 0x14c9bb000 else None
                if s and s.isprintable() and len(s) > 1:
                    notes.append('STR %r' % s)
                notes += note32(lo, row + off)
                notes += note32(hi, row + off + 4)
                print('    +%02x  q=%016x  lo=%08x hi=%08x  %s' %
                      (off, q, lo, hi, ' | '.join(notes)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
