#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自动对齐 UE 属性参数表记录：先找所有名字串指针，再按候选步长/字段位置解出偏移。

不做任何 layout 假设：对每个名字串指针位置 p，尝试把所有记录起点整体平移 4 字节，
选择能同时满足下面三条的对齐：

  * 名字指针落在记录内某个固定偏移
  * 紧邻名字指针前 4 字节是 uint32 Offset（0 或 4 的倍数，且随声明顺序单调不减）
  * 再前面 4 字节（标志位）在同一张表里取值集合很小

    python autotable.py 0x149f2f440 0x149f2f7c0
    python autotable.py 0x14a0d5fc0 0x14a0d6900
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
    s = img.cstr(va, 96)
    if s and len(s) >= 2 and all(32 <= ord(c) < 127 for c in s):
        if s[0].isalpha() or s[0] == '_':
            return s
    return None


def find_name_ptrs(lo, hi, maxn=64):
    """在 [lo,hi) 里找所有「指向 .rdata 名字串」的 8 字节槽。"""
    out = []
    va = lo
    while va < hi and len(out) < maxn:
        v = q(va)
        if v:
            s = name_at(v)
            if s:
                out.append((va, v, s))
        va += 4
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    lo, hi = int(sys.argv[1], 0), int(sys.argv[2], 0)
    ptrs = find_name_ptrs(lo, hi)
    print('# 找到 %d 个名字串指针槽 (扫描 %#x..%#x)' % (len(ptrs), lo, hi))
    for p, v, s in ptrs:
        print('   slot %#x -> %r' % (p, s))

    # 推断步长 = 相邻槽间距的最小公倍形态
    if len(ptrs) >= 2:
        gaps = [ptrs[i + 1][0] - ptrs[i][0] for i in range(len(ptrs) - 1)]
        print('# 间距: %s' % [hex(g) for g in gaps])

    # 用第一个槽反推记录起点：offset 字段在名字槽前 4 字节，正好是 offset+?
    print()
    print('# slot-relative 解码（offset = 名字槽前 4 字节, flags = 其前 4 字节）')
    print('# %-14s %-38s %-10s %-10s %-12s' % ('nameslot', 'name', 'offset', 'flags', 'slot+8'))
    for p, v, s in ptrs:
        print('  %#012x %-38s %#-10x %#-10x %#012x' %
              (p, s, u32(p - 4), u32(p - 8), q(p + 8)))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
