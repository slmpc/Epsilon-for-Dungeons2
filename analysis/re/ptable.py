#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 0x40 步长解码 UE 属性参数表（FPropertyParamsBase），偏移字段在 record+0x34。

本构建实测：
  记录 = 名字串指针 - 0x38
  记录 + 0x00  uint64 标志 / TArray 头
  记录 + 0x08  uint64 (0x10000000000001 之类)
  记录 + 0x10  uint64
  记录 + 0x18  uint64
  记录 + 0x20  uint64
  记录 + 0x28  uint32 数组维度 / 标志
  记录 + 0x2c  uint32
  记录 + 0x30  uint32
  记录 + 0x34  uint32  Offset_Internal      <-- 属性在对象上的字节偏移
  记录 + 0x38  char*  名字

    python ptable.py 0x14a0d6864 12          # ATR_Movement（以 MovementSpeedMultiplier 的 offset 槽为起点）
    python ptable.py 0x149f2f470 10          # Emerald 组
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
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    start = int(sys.argv[1], 0)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    print('# 起始 %#x  条数 %d  步长 0x40' % (start, count))
    print('# %-4s %-14s %-40s %-9s %-10s %-10s' %
          ('i', 'record', 'name', 'offset', 'f28', 'f30'))
    for i in range(count):
        rec = start + i * 0x40
        name = str_at(img.u64(rec + 0x38))
        off_int = img.u32(rec + 0x34)
        f28 = img.u32(rec + 0x28)
        f30 = img.u32(rec + 0x30)
        print('  %-4d %#012x %-40s %#-9x %#-10x %#-10x' %
              (i, rec, name or '(none)', off_int or 0, f28 or 0, f30 or 0))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
