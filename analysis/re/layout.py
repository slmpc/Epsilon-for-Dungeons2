#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 .rdata 里的属性表/FField 记录按 4 字节粒度摊平，打印每一格的两个解释。

    python layout.py 0x14a0d6800 0x60
"""

import struct
import sys

from peimg import Image

img = Image()
TEXT_LO, TEXT_HI = 0x140001000, 0x148E31000
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def str_at(va, n=48):
    if not (RD_LO <= va < RD_HI):
        return None
    s = img.cstr(va, n)
    if s and len(s) > 1 and all(32 <= ord(c) < 127 for c in s):
        return s
    return None


def main():
    for a in sys.argv[1:]:
        base = int(a, 0)
        n = 0x80
        b = img.read_va(base, n)
        print('==== %#x' % base)
        for off in range(0, len(b), 4):
            v4 = struct.unpack_from('<I', b, off)[0]
            v8 = struct.unpack_from('<Q', b, off)[0]
            note = []
            s = str_at(v8)
            if s:
                note.append('STR %r' % s)
            if TEXT_LO <= v8 < TEXT_HI:
                note.append('CODE VA')
            if 0x1000 <= v4 < 0x8E31000:
                if TEXT_LO - 0x140000000 <= v4 < TEXT_HI - 0x140000000:
                    note.append('CODE RVA -> %#x' % (0x140000000 + v4))
                s2 = str_at(0x140000000 + v4)
                if s2:
                    note.append('STRrva %r' % s2)
            print('  +%03x  u32=%-12d %#010x   u64=%#018x   %s' %
                  (off, v4, v4, v8, ' | '.join(note)))


if __name__ == '__main__':
    sys.exit(main())
