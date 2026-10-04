#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把属性表按 0x40 步长解码成表：名字 / 名字指针槽内偏移 / 槽值向量。

实测本构建每个记录 0x40 字节，名字串指针的位置在记录内不固定（观察值 +0x30 / +0x18 /
+0x00），但每条记录里只会出现一次「指针指向 .rdata 名字串」。

    python table_decode.py 0x149f2f480 12
"""

import struct
import sys

from peimg import Image

img = Image()
TEXT_LO = TEXT_HI = None
RD_LO = RD_HI = None
for _s in img.sections:
    va = img.image_base + _s['vaddr']
    if _s['name'] == '.text':
        TEXT_LO, TEXT_HI = va, va + _s['vsize']
    if _s['name'] == '.rdata':
        RD_LO, RD_HI = va, va + _s['vsize']


def is_name_ptr(q):
    if not (RD_LO <= q < RD_HI):
        return None
    s = img.cstr(q, 64)
    if not s or len(s) < 2:
        return None
    if not all(32 <= ord(c) < 127 for c in s):
        return None
    if not (s[0].isalpha() or s[0] == '_'):
        return None
    if not all(c.isalnum() or c == '_' for c in s):
        return None
    return s


def decode(start, count, stride=0x40):
    print('start %#x  count %d  stride %#x' % (start, count, stride))
    print('  #    record      name_slot  name                      slots')
    for i in range(count):
        rec = start + i * stride
        b = img.read_va(rec, 0x40)
        names = []
        for off in range(0, 0x40, 8):
            q = struct.unpack_from('<Q', b, off)[0]
            s = is_name_ptr(q)
            if s:
                names.append((off, s))
        vals = [struct.unpack_from('<I', b, o)[0] for o in range(0, 0x40, 8)]
        hi = [struct.unpack_from('<I', b, o + 4)[0] for o in range(0, 0x40, 8)]
        tag = ' '.join('%s@+%#x' % (n, o) for o, n in names) or '(no name ptr)'
        print('  %-3d  %#012x  %-9s  %-52s  %s | %s' %
              (i, rec, ' '.join('%#x' % o for o, _ in names), tag,
               ' '.join('%d' % v for v in vals),
               ' '.join('%#x' % v for v in hi)))


if __name__ == '__main__':
    start = int(sys.argv[1], 0)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    stride = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40
    decode(start, count, stride)
