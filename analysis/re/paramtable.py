#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解码 UE 属性参数表记录（UHT 生成的 FPropertyParams）。

实测本构建每个属性参数记录 **0x40 字节**，字段位置随记录类型略有不同，但稳定出现：

    +0x00  TArray 头/标志
    +0x08  名字串指针 (const char*)
    +0x20  uint32 标志 (0x01 / 0x03 / 0x82 等)
    +0x24  uint32 Offset_Internal          <-- 属性在 UObject 上的字节偏移
    +0x28  uint32 类型哈希 / 其他元数据
    +0x30  uint32 (0 或小整数)
    +0x38  下一个 8 字节槽

用法：
    python paramtable.py 0x149f2f440 6
"""

import struct
import sys

from peimg import Image

img = Image()
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def q(va):
    b = img.read_va(va, 8)
    return struct.unpack('<Q', b)[0] if b and len(b) == 8 else 0


def u32(va):
    b = img.read_va(va, 4)
    return struct.unpack('<I', b)[0] if b and len(b) == 4 else 0


def name_at(va):
    if not (RD_LO <= va < RD_HI):
        return None
    s = img.cstr(va, 80)
    if s and len(s) > 1 and all(32 <= ord(c) < 127 for c in s):
        return s
    return None


def decode(start, count, stride=0x40):
    print('# start %#x  count %d  stride %#x' % (start, count, stride))
    print('# %-3s %-14s %-8s %-38s %-8s %-10s' %
          ('i', 'record', 'nameslot', 'name', 'offset', 'flags'))
    for i in range(count):
        rec = start + i * stride
        name = slot = None
        for off in range(0, stride, 8):
            s = name_at(q(rec + off))
            if s:
                name, slot = s, off
                break
        off_int = u32(rec + 0x24)
        flags = u32(rec + 0x20)
        print('  %-3d %#012x %-8s %-38s %#-8x %#010x' %
              (i, rec, ('+%#x' % slot) if slot is not None else '-',
               name or '(none)', off_int, flags))


if __name__ == '__main__':
    start = int(sys.argv[1], 0)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    decode(start, count)
