#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按已锁定布局解码 FField 反射条目。

本构建实测（用 ATR_Movement 的已知偏移校准）：
  * 每条 FField 反射条目 0x38 字节
  * +0x18  名字串指针（VA）
  * +0x20  vtable（属性类对象）
  * -0x0C  相对「指向名字串的槽」= Offset_Internal

    python ff.py 0x149f2f480 0x14a0d6ca0 ...
"""

import struct
import sys

from peimg import Image

img = Image()
IMG = 0x140000000
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def str_at(va):
    if not (RD_LO <= va < RD_HI):
        return None
    s = img.cstr(va, 96)
    if s and len(s) >= 2 and all(32 <= ord(c) < 127 for c in s):
        return s
    return None


def main():
    for a in sys.argv[1:]:
        slot = int(a, 0)          # 指向名字串的 8 字节槽
        name = str_at(img.u64(slot))
        off_int = img.u32(slot - 0x0C)
        hash4 = img.u32(slot - 0x08)
        strlen = img.u32(slot - 0x04)
        print('  %-38s off=%#-7x  slot=%#012x  hash=%#010x  namelen=%d' %
              (name or '(?)', off_int, slot, hash4, strlen))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
