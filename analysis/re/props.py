#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""定位 UE 代码生成属性表项并读出字段偏移。

本构建实测表项布局（每项 0x40 字节）：

    +0x00  Getter 函数指针 (VA)
    +0x08  Setter 函数指针 (VA)
    +0x10  Prop 描述指针 (VA, 指向 \0\0\0\0 + 名字指针 的块)
    +0x18  Prop 描述指针 (同上)
    +0x20  0
    +0x28  名字串指针 (VA)
    +0x2c  uint32 名字长度
    +0x30  uint32 (0)
    +0x34  uint32 Offset_Internal      <-- 要的就是它
    +0x38  uint32
    +0x3c  uint32

用法:
    python props.py EmeraldAmount PlayerEmeralds ...
    python props.py --dump 0x149f2f448      # 直接 dump 一个表项
"""

import struct
import sys

from peimg import Image


def ptr_hits(img, target_va, sections=('.rdata', '.data', '_RDATA')):
    pat = struct.pack('<Q', target_va)
    out = []
    for s in img.sections:
        if s['name'] not in sections:
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        st = 0
        while True:
            i = data.find(pat, st)
            if i < 0:
                break
            st = i + 1
            out.append(base + i)
    return out


def find_names(img, name):
    pat = name.encode('ascii') + b'\0'
    out = []
    for s in img.sections:
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        st = 0
        while True:
            i = data.find(pat, st)
            if i < 0:
                break
            st = i + 1
            out.append(base + i)
    return out


def show_entry(img, entry, label=''):
    b = img.read_va(entry, 0x40)
    if not b or len(b) < 0x40:
        print('  %#x: unreadable' % entry)
        return None
    getter, setter = struct.unpack_from('<QQ', b, 0)
    d1, d2 = struct.unpack_from('<QQ', b, 0x10)
    name_ptr = struct.unpack_from('<Q', b, 0x28)[0]
    name_len = struct.unpack_from('<I', b, 0x2c)[0]
    z = struct.unpack_from('<I', b, 0x30)[0]
    off_int = struct.unpack_from('<I', b, 0x34)[0]
    tail = struct.unpack_from('<II', b, 0x38)
    nm = img.cstr(name_ptr, 96) if 0x140000000 <= name_ptr < 0x14c9bb000 else None
    print('  %s entry %#012x  name=%r' % (label, entry, nm))
    print('      getter %#x  setter %#x' % (getter, setter))
    print('      desc   %#x  %#x' % (d1, d2))
    print('      name   %#x len=%d  +0x30=%#x' % (name_ptr, name_len, z))
    print('      OFFSET %#x (%d)   +0x38=%#x +0x3c=%#x' %
          (off_int, off_int, tail[0], tail[1]))
    return off_int


def main():
    img = Image()
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args[0] == '--dump':
        for a in args[1:]:
            show_entry(img, int(a, 0))
        return 0
    for nm in args:
        print('==================== %s' % nm)
        for nva in find_names(img, nm):
            hits = ptr_hits(img, nva)
            print('  name @ %#x  referenced by %d slot(s)' % (nva, len(hits)))
            for h in hits:
                show_entry(img, h - 0x28)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
