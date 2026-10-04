#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dump_entry_window.py — 把 pak 里某个条目附近的字节切出来，供 oodlefind 扫。

用法: python dump_entry_window.py <条目偏移> <输出文件> [长度]
"""

import os
import sys

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    off = int(sys.argv[1], 0)
    out = sys.argv[2]
    n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x2000
    with open(PAK, 'rb') as f:
        f.seek(off)
        b = f.read(n)
    with open(out, 'wb') as w:
        w.write(b)
    print('写出 %s (%d 字节, 从 pak 偏移 %#x)' % (out, len(b), off))
    return 0


if __name__ == '__main__':
    sys.exit(main())
