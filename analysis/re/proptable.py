#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 UE 代码生成属性表项布局反查属性偏移。

表项（0x40 字节步长）在本构建里观测到的形态：

    +0x00  Getter 函数指针      (可执行地址)
    +0x08  Setter 函数指针      (可执行地址)
    +0x10  Prop 描述指针        (\0\0\0\0 开头)
    +0x18  Prop 描述指针        (同上)
    +0x20  0
    +0x28  名字串指针
    +0x2c  uint32 名字长度
    +0x30  0
    +0x34  uint32 Offset_Internal

用法:
    python proptable.py EmeraldAmount CurrencyAcquisitionMultiplier ...
"""

import struct
import sys

from peimg import Image


def build_ptr_index(img, sections=('.rdata', '.data', '_RDATA')):
    """扫节区里所有 8 字节槽，建 {被指向的VA: [槽VA,...]} 索引。"""
    by_target = {}
    codesec = None
    for s in img.sections:
        if s['name'] == '.text':
            codesec = s
    text_lo = img.image_base + codesec['vaddr']
    text_hi = text_lo + codesec['vsize']
    for s in img.sections:
        if s['name'] not in sections:
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        for off in range(0, len(data) - 7, 8):
            v = struct.unpack_from('<Q', data, off)[0]
            if text_lo <= v < text_hi:
                by_target.setdefault(v, []).append(base + off)
    return by_target, text_lo, text_hi


def find_prop_entry(img, name_va, lo, hi):
    """名字串指针所在位置 = 属性表项 + 0x28。"""
    pat = struct.pack('<Q', name_va)
    out = []
    for s in img.sections:
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        start = 0
        while True:
            i = data.find(pat, start)
            if i < 0:
                break
            start = i + 1
            va = img.image_base + s['vaddr'] + i
            if va < lo or va >= hi:
                continue
            out.append(va - 0x28)
    return out


def dump_entry(img, entry, label=''):
    b = img.read_va(entry, 0x40)
    if not b:
        return
    getter, setter = struct.unpack_from('<QQ', b, 0)
    name_ptr = struct.unpack_from('<Q', b, 0x28)[0]
    name_len = struct.unpack_from('<I', b, 0x2c)[0]
    off_int = struct.unpack_from('<I', b, 0x34)[0]
    extra = struct.unpack_from('<I', b, 0x38)[0]
    print('  entry %#012x  %s' % (entry, label))
    print('    getter    %#x' % getter)
    print('    setter    %#x' % setter)
    print('    name      %#x  len=%d  %r' %
          (name_ptr, name_len, img.cstr(name_ptr, 64) if name_ptr else None))
    print('    +0x30     %#x' % struct.unpack_from('<I', b, 0x30)[0])
    print('    Offset    %#x  (%d)' % (off_int, off_int))
    print('    +0x38     %#x' % extra)
    print('    +0x3c     %#x' % struct.unpack_from('<I', b, 0x3c)[0])


def main():
    img = Image()
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    name_vas = {}
    for nm in sys.argv[1:]:
        pat = nm.encode('ascii') + b'\0'
        hits = []
        for s in img.sections:
            img.f.seek(s['rawptr'])
            data = img.f.read(s['rawsize'])
            start = 0
            while True:
                i = data.find(pat, start)
                if i < 0:
                    break
                start = i + 1
                hits.append(img.image_base + s['vaddr'] + i)
        name_vas[nm] = hits
        print('%s -> %s' % (nm, [hex(h) for h in hits]))

    print('\nbuilding pointer index ...')
    idx, lo, hi = build_ptr_index(img)
    print('  %d distinct code targets indexed' % len(idx))

    for nm, vas in name_vas.items():
        for va in vas:
            entries = find_prop_entry(img, va, lo, hi)
            print('\n=================== %s @ %#x' % (nm, va))
            if not entries:
                print('  (no property-table reference in section range)')
            for e in entries:
                dump_entry(img, e)
                getter = img.u64(e)
                print('    getter at %#x -> RVA %#x' %
                      (getter, getter - img.image_base) if getter else '')
                # 邻居: 同表里前后各两项
                for d in (-0x80, -0x40, 0x40, 0x80):
                    nb = img.u64(e + d + 0x28)
                    if nb:
                        s = img.cstr(nb, 64)
                        if s and s.isascii() and s.replace('_', '').isalnum():
                            no = img.u32(e + d + 0x34)
                            print('      neighbour %+#x  %-40s off %#x' % (d, s, no))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
