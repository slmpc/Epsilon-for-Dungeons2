#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""entropy_scan.py — 分块统计熵与可打印率，判定容器是明文/压缩/加密。

判据（1 MiB 块）:
  * 明文 UE 资源: 可打印率明显高(0.6+), 熵 4.5~6.5, 能看到大量可读标识符
  * 纯压缩:      熵 ≈ 8.0, 可打印率 ≈ 0.37(=95/256, 均匀随机下的期望)
  * 加密:        熵 ≈ 8.0, 可打印率 ≈ 0.37, 且**没有任何**结构性文本

均匀随机字节的可打印率期望 = 95/256 ≈ 0.371, 熵 = 8.0 bit/byte。
"""

import math
import os
import sys

PAKS = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
        r'\Dungeons\Content\Paks')
BLOCK = 1 << 20


def stats(buf):
    n = len(buf)
    if not n:
        return 0.0, 0.0
    printable = sum(1 for b in buf if 32 <= b < 127)
    counts = [0] * 256
    for b in buf:
        counts[b] += 1
    ent = 0.0
    for c in counts:
        if c:
            p = c / n
            ent -= p * math.log2(p)
    return ent, printable / n


def scan(path, budget=48 << 20, blocks=48):
    size = os.path.getsize(path)
    print('=' * 78)
    print('%s   (%s bytes)' % (os.path.basename(path), f'{size:,}'))
    print('  %-10s %-8s %-10s %s' % ('offset', 'entropy', 'printable', '备注'))
    with open(path, 'rb') as f:
        for i in range(min(blocks, max(1, min(size, budget) // BLOCK))):
            f.seek(i * BLOCK)
            buf = f.read(BLOCK)
            if not buf:
                break
            e, p = stats(buf)
            note = ''
            if e < 7.0 and p > 0.55:
                note = '像明文'
            elif e > 7.8 and p < 0.42:
                note = '均匀随机(压缩或加密)'
            print('  %#010x %-8.3f %-10.3f %s' % (i * BLOCK, e, p, note))
    return 0


def main():
    names = sys.argv[1:] or ['Dungeons-Windows.ucas', 'global.ucas',
                             'Dungeons-Windows.pak']
    for n in names:
        p = os.path.join(PAKS, n)
        if os.path.exists(p):
            scan(p)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
