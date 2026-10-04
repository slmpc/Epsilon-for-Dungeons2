#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对一个 VA，列出所有引用它的位置：8 字节绝对指针 / 4 字节 RVA / .text 里的 RIP 相对。"""

import struct
import sys

from peimg import Image

ALL = ('.text', '.rdata', '.data', '.pdata', '_RDATA', '.reloc')


def refs_of(img, target_va, target_rva):
    p64 = struct.pack('<Q', target_va)
    p32 = struct.pack('<I', target_rva)
    out = {'ptr64': [], 'rva32': [], 'rip': []}
    text_lo = text_hi = None
    for s in img.sections:
        if s['name'] == '.text':
            text_lo = img.image_base + s['vaddr']
            text_hi = text_lo + s['vsize']
    for s in img.sections:
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        st = 0
        while True:
            i = data.find(p64, st)
            if i < 0:
                break
            st = i + 1
            out['ptr64'].append(base + i)
        if s['name'] in ('.rdata', '.data', '_RDATA'):
            st = 0
            while True:
                i = data.find(p32, st)
                if i < 0:
                    break
                st = i + 1
                out['rva32'].append(base + i)
        if s['name'] == '.text':
            for i in range(len(data) - 7):
                if data[i] != 0x48:
                    continue
                if (data[i + 2] & 0xC7) != 0x05:
                    continue
                disp = struct.unpack_from('<i', data, i + 3)[0]
                if base + i + 7 + disp == target_va:
                    out['rip'].append(base + i)
    return out


def main():
    img = Image()
    for a in sys.argv[1:]:
        va = int(a, 0)
        r = refs_of(img, va, va - img.image_base)
        print('%#x %r' % (va, img.cstr(va, 48)))
        for k in ('ptr64', 'rva32', 'rip'):
            print('  %-6s %d: %s' % (k, len(r[k]), ' '.join(hex(x) for x in r[k][:40])))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
