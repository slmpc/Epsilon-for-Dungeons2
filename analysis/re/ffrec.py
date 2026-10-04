#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按绝对地址直接打印一条 FField 反射记录的语义字段。

已知锚点（本项目实测，可用于交叉校验）：
    ATR_Movement::MovementSpeedMultiplier  name@0x14a0d6870  Offset=0x90
    ATR_Movement::GravityScale             name@0x14a0d69f0  Offset=0xF0

    python ffrec.py 0x14a0d6870 0x149f2f6b8 ...
"""

import struct
import sys

from peimg import Image

img = Image()
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def cstr(va, n=96):
    if not (RD_LO <= va < RD_HI):
        return None
    s = img.cstr(va, n)
    if s and len(s) >= 2 and all(32 <= ord(c) < 127 for c in s):
        return s
    return None


def main():
    for a in sys.argv[1:]:
        name_va = int(a, 0)
        name = cstr(name_va)
        print('==== name %#x %r' % (name_va, name))
        # 找引用它的 8 字节槽
        pat = struct.pack('<Q', name_va)
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
                slot = base + i
                print('  slot %#012x' % slot)
                # 按 4 字节粒度打印 slot-0x30 .. slot+0x10
                lo = slot - 0x30
                b = img.read_va(lo, 0x40 + 0x10)
                if not b:
                    continue
                for off in range(0, len(b), 4):
                    v = struct.unpack_from('<I', b, off)[0]
                    v8 = struct.unpack_from('<Q', b, off)[0]
                    note = []
                    if off % 8 == 0:
                        s2 = cstr(v8)
                        if s2:
                            note.append('STR %r' % s2)
                    if 0x140001000 <= v8 < 0x148E31000:
                        note.append('CODE')
                    rel = (lo + off) - slot
                    print('     %#012x  (S%+#05x)  %#010x  %10d  %s' %
                          (lo + off, rel, v, v, ' | '.join(note)))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
