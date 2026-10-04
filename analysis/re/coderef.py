#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从一个属性名字串出发，找引用它的代码（.text 里的 RIP 相对 lea/mov）与其相对位移。"""

import struct
import sys

from peimg import Image

img = Image()
IMG = 0x140000000
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def find_names(name):
    pat = name.encode() + b'\0'
    out = []
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
            va = base + i
            if RD_LO <= va < RD_HI:
                out.append(va)
    return out


def main():
    sec = [s for s in img.sections if s['name'] == '.text'][0]
    img.f.seek(sec['rawptr'])
    d = img.f.read(sec['rawsize'])
    base = img.image_base + sec['vaddr']
    for name in sys.argv[1:]:
        vas = find_names(name)
        print('==== %s  %s' % (name, [hex(v) for v in vas]))
        if not vas:
            continue
        want = set(vas)
        hits = 0
        for i in range(len(d) - 7):
            if d[i] != 0x48 or d[i + 1] not in (0x8D, 0x8B):
                continue
            if (d[i + 2] & 0xC7) != 0x05:
                continue
            disp = struct.unpack_from('<i', d, i + 3)[0]
            va = base + i + 7 + disp
            if va in want:
                print('   lea/mov @ %#x -> %#x   bytes=%s' %
                      (base + i, va, d[i:i + 7].hex()))
                hits += 1
                if hits > 20:
                    break
        if not hits:
            print('   (没有代码直接引用)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
