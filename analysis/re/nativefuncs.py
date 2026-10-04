#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""nativefuncs.py — 全量抓出 UFunction 的原生实现映射。

本构建的 UFunction 记录（步长 0x48）长这样：

    +0x00  const char*     名字
    +0x08  FNativeFuncPtr  exec 跳板（签名 `(UObject*, FFrame&)`，取参后转发）
    +0x10  void*           真实实现
    +0x18 .. 参数/标志信息

判据：+0x00 指向 .rdata 里的合法标识符、+0x08 与 +0x10 都落在 .text。
按 8 字节对齐扫全段就能把它们全捞出来，不必先知道表在哪。

    python nativefuncs.py --dump map.txt      # 导出全部
    python nativefuncs.py Currency Emerald    # 按关键词筛
"""

import re
import struct
import sys

from peimg import Image

IMG = 0x140000000
TEXT_LO, TEXT_HI = 0x140001000, 0x148E31000
RD_LO, RD_HI = 0x148E33600, 0x14BA78000
SCAN_SECTIONS = ('.rdata', '.data', '_RDATA')
IDENT_RX = re.compile(rb'^[A-Za-z_][A-Za-z0-9_]{2,79}$')


def collect(img):
    """返回 [(名字, 记录地址, exec, impl)]。"""
    out = []
    for s in img.sections:
        if s['name'] not in SCAN_SECTIONS:
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = IMG + s['vaddr']
        for off in range(0, len(data) - 0x18, 8):
            nva = struct.unpack_from('<Q', data, off)[0]
            if not (RD_LO <= nva < RD_HI):
                continue
            execp = struct.unpack_from('<Q', data, off + 8)[0]
            impl = struct.unpack_from('<Q', data, off + 0x10)[0]
            if not (TEXT_LO <= execp < TEXT_HI and TEXT_LO <= impl < TEXT_HI):
                continue
            nm = img.cstr(nva, 80)
            if not nm or len(nm) < 3:
                continue
            if not IDENT_RX.match(nm.encode('latin1', 'replace')):
                continue
            out.append((nm, base + off, execp, impl))
    return out


def main():
    img = Image()
    recs = collect(img)
    print('# 抓到 %d 条 UFunction 原生实现记录' % len(recs))

    if len(sys.argv) > 2 and sys.argv[1] == '--dump':
        with open(sys.argv[2], 'w', encoding='utf-8') as f:
            f.write('# name\trecord\texec\timpl\n')
            for nm, rec, e, p in sorted(recs):
                f.write('%s\t%#x\t%#x\t%#x\n' % (nm, rec, e, p))
        print('# 已写出 %s' % sys.argv[2])
        return 0

    for kw in sys.argv[1:]:
        sel = [r for r in recs if kw in r[0]]
        print('\n===== %s : %d 条 =====' % (kw, len(sel)))
        for nm, rec, e, p in sorted(sel):
            print('  %-52s impl=%#x  (rec %#x)' % (nm, p, rec))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
