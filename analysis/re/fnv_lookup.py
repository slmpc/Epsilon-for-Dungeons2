#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fnv_lookup.py — 反查被哈希隐藏的 API 名。

`sub_148AD6250` 里用 FNV 变体对导出名做哈希, 与硬编码常量比对 —— 典型隐藏手法。
哈希常量 0x66F841C0F75DB66E。

代码里的形式（读反编译得出）:
    v = 2166136261                     # 0x811C9DC5
    for c in name:                     # 名字先转小写
        v = 16777619 * (v ^ c)         # 0x01000193，64 位乘法不回绕到 32 位
    compare v == 0x66F841C0F75DB66E

本脚本对映像里出现的全部标识符做同款计算，找出匹配项。
"""

import re
import sys

from peimg import Image

FNV_OFFSET = 2166136261
FNV_PRIME = 16777619
TARGET = 0x66F841C0F75DB66E
MASK64 = (1 << 64) - 1


def fnv(name: str, lower: bool = True) -> int:
    v = FNV_OFFSET
    for ch in name:
        c = ord(ch)
        if lower and 65 <= c <= 90:
            c |= 0x20
        v = (FNV_PRIME * (v ^ c)) & MASK64
    return v


def main():
    img = Image()
    names = set()
    for s in img.sections:
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        for mo in re.finditer(rb'[A-Za-z_][A-Za-z0-9_@?$.]{2,120}', d):
            names.add(mo.group().decode('latin1'))

    print('候选标识符 %d 个' % len(names))
    hits = []
    for n in names:
        if fnv(n) == TARGET:
            hits.append((n, 'full'))
        # 也试「最后一个 . 之后」的部分（代码里有跳过 '.' 的动作）
        if '.' in n:
            tail = n.rsplit('.', 1)[1]
            if fnv(tail) == TARGET:
                hits.append((tail, 'after-dot'))
    print('=== 匹配 ===')
    for n, how in hits:
        print('   %-50s (%s)' % (n, how))
    if not hits:
        print('   （没有匹配 —— 可能大小写处理不同, 或目标名不在映像里）')
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
