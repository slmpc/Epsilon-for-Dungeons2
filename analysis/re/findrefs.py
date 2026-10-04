#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 .text 里找 RIP 相对引用（lea / mov）指向给定 VA 的指令。

    python findrefs.py 0x149f2f6b8 [more...]
"""

import struct
import sys

from peimg import Image


def find_rips(img, target_va):
    """返回 [(指令VA, 指令字节, 位移)] —— 任何 `48 8d/8b <modrm> disp32` 指向 target。"""
    text = None
    for s in img.sections:
        if s['name'] == '.text':
            text = s
            break
    img.f.seek(text['rawptr'])
    data = img.f.read(text['rawsize'])
    base = img.image_base + text['vaddr']
    out = []
    for i in range(len(data) - 7):
        if data[i] != 0x48 or data[i + 1] not in (0x8d, 0x8b, 0x89, 0x3b):
            continue
        modrm = data[i + 2]
        # mod=00, rm=101 -> RIP 相对
        if (modrm & 0xC7) != 0x05:
            continue
        disp = struct.unpack_from('<i', data, i + 3)[0]
        va = base + i
        if va + 7 + disp == target_va:
            out.append((va, data[i:i + 7].hex(), disp))
    return out


def main():
    img = Image()
    for a in sys.argv[1:]:
        va = int(a, 0)
        refs = find_rips(img, va)
        s = img.cstr(va, 64)
        print('%#x %r -> %d refs' % (va, s, len(refs)))
        for r, hx, disp in refs[:60]:
            print('    %#012x  RVA %#010x  %s' % (r, r - img.image_base, hx))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
