#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ufuncs.py — 枚举 UFunction 名与它们的 exec 跳板 / 真实实现。

本构建实测两种记录：

  A) `FNativeFunctionRegistrar` 风格: { const char* Name; UFunction* (*Getter)(); }
     +8 是「惰性取 UFunction」的小函数, 不是实现 —— 别拿它当目标。

  B) `{ const char* Name; FNativeFuncPtr Exec; void* Impl; }`
     +8 是标准 exec 跳板(签名 `(UObject*, FFrame&)`, 内部取参后转发),
     +0x10 是**真实实现**。要反编译就找这条。

所以筛名字时要同时要求 +8 与 +0x10 都是代码。

    python ufuncs.py Currency Emerald
    python ufuncs.py --table 0x149f8e8b0 30
"""

import re
import struct
import sys

from peimg import Image

IMG = 0x140000000
TEXT_LO, TEXT_HI = 0x140001000, 0x148E31000
SCAN_SECTIONS = ('.rdata', '.data', '_RDATA')
NAME_SECTIONS = ('.rdata', '_RDATA')
NAME_RX = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{3,79}')


def build(img):
    names = {}
    ptr_slots = {}
    for s in img.sections:
        if s['name'] not in SCAN_SECTIONS:
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = IMG + s['vaddr']
        if s['name'] in NAME_SECTIONS:
            for mo in NAME_RX.finditer(data):
                names.setdefault(mo.group().decode('latin1'), []).append(base + mo.start())
        for off in range(0, len(data) - 16, 8):
            v = struct.unpack_from('<Q', data, off)[0]
            if 0x140001000 <= v < 0x14CA04000:
                ptr_slots.setdefault(v, []).append(base + off)
    return names, ptr_slots


def main():
    img = Image()
    names, ptr_slots = build(img)
    print('# 标识符串 %d 个, 指针槽目标 %d 个' % (len(names), len(ptr_slots)))

    if len(sys.argv) > 2 and sys.argv[1] == '--table':
        base = int(sys.argv[2], 0)
        cnt = int(sys.argv[3]) if len(sys.argv) > 3 else 24
        for i in range(cnt):
            slot = base + i * 0x18
            nva = img.u64(slot)
            s = img.cstr(nva, 96) if nva and 0x148E33600 <= nva < 0x14BA78000 else None
            e = img.u64(slot + 8) or 0
            p = img.u64(slot + 0x10) or 0
            print('  %-3d %#x  %-42s exec=%#x impl=%#x' %
                  (i, slot, (s or '')[:42], e, p))
        return 0

    for kw in sys.argv[1:]:
        sel = sorted(n for n in names if kw in n)
        print('\n===== %s : %d 个名字 =====' % (kw, len(sel)))
        hit = 0
        for nm in sel:
            for nva in names[nm][:1]:
                for slot in ptr_slots.get(nva, []):
                    e = img.u64(slot + 8) or 0
                    p = img.u64(slot + 0x10) or 0
                    if not (TEXT_LO <= e < TEXT_HI):
                        continue
                    mark = 'impl' if TEXT_LO <= p < TEXT_HI else 'getter'
                    print('  %-46s %-6s exec=%#x impl=%#x' % (nm, mark, e, p))
                    hit += 1
                    break
                else:
                    continue
                break
        print('  -> %d 个配到记录' % hit)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
