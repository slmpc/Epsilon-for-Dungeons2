#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 MSVC RTTI，给出类的继承链、vtable 地址与虚函数条目。

    python rtti.py SWCurrencyAttributeSet ATR_Economy ...

MSVC x64 布局：
    vtable[-1] = &RTTICompleteObjectLocator
    COL +0x00 signature(1) +0x04 offset +0x08 cdOffset +0x0c pTypeDescriptor(RVA)
            +0x10 pClassDescriptor(RVA) +0x14 pSelf(RVA)
    TypeDescriptor: +0x00 pVFTable, +0x08 spare, +0x10 ".?AV<name>@@"
    ClassDescriptor: +0x00 signature, +0x04 attributes, +0x08 numBaseClasses,
                     +0x0c pBaseClassArray, +0x10 .. 各 BaseClassDescriptor
    BaseClassDescriptor(0x18): +0x00 pTypeDescriptor(RVA), +0x04 numContained,
                     +0x08 pmdisp, +0x0c pVFTable(RVA), +0x10 mdisp
"""

import struct
import sys

from peimg import Image

img = Image()

# 先扫出所有 ".?AV<name>@@" 的 TypeDescriptor
TD_RX_OFF = 0x10


def scan_typedescriptors():
    out = {}
    for s in img.sections:
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        st = 0
        while True:
            i = data.find(b'.?AV', st)
            if i < 0:
                break
            st = i + 1
            end = data.find(b'\0\0', i)
            if end < 0:
                continue
            raw = data[i:end]
            try:
                nm = raw.decode('latin1')
            except Exception:
                continue
            # 只接受形如 .?AVFoo@@ 的单个名字
            if not nm.endswith('@@'):
                continue
            name = nm[4:-2]
            va = base + i
            out.setdefault(name, []).append(va)
    return out


def read_names():
    """扫所有 vtable：vtable-8 指向 COL，COL+0x0c 指向 TypeDescriptor。"""
    cols = []
    for s in img.sections:
        if s['name'] not in ('.rdata', '_RDATA'):
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        for off in range(0, len(data) - 8, 8):
            rva = struct.unpack_from('<I', data, off)[0]
            if not (0x8e31000 <= rva < 0x8e31000 + 0x2c465ae):
                continue
            # 这里只是候选，交给调用方按名字过滤
            cols.append(base + off)
    return cols


def main():
    names = sys.argv[1:]
    tds = scan_typedescriptors()
    print('typedescriptors: %d' % len(tds))
    if not names:
        for n in sorted(tds)[:80]:
            print('  %-60s %s' % (n, ' '.join(hex(v) for v in tds[n][:2])))
        return 0
    for n in names:
        hits = tds.get(n)
        print('==== %s -> %s' % (n, [hex(h) for h in hits] if hits else 'NOT FOUND'))
        if not hits:
            cand = sorted(k for k in tds if n.lower() in k.lower())
            print('   fuzzy: %s' % cand[:40])
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
