#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""oodle_find.py — 在 exe 里定位 Oodle 解压入口的线索。

思路: Oodle 是静态链接的, UE 侧会有一个封装
(`FOodleDataCompression::Decompress` 之类) 直接 call 进去。
先用字符串找 UE 侧的源码路径/日志, 再顺着交叉引用回到代码。
"""

import re
import sys

from peimg import Image

PATTERNS = [
    rb'OodleDataCompression',
    rb'OodleDataCompression\.cpp',
    rb'FOodleDataCompression',
    rb'OodleLZ_Decompress',
    rb'OodleLZ_Compress',
    rb'OOPlugin',
    rb'OodleCore_Plugins',
    rb'Oodle_SetUsageWarnings',
    rb'bUseOodle',
    rb'Oodle\w*\.cpp',
]


def main():
    img = Image()
    print('映像 %s\n' % img.path)

    for pat in PATTERNS:
        rx = re.compile(pat)
        found = []
        for s in img.sections:
            if s['name'] not in ('.rdata', '_RDATA', '.data', '.text'):
                continue
            img.f.seek(s['rawptr'])
            d = img.f.read(s['rawsize'])
            base = img.image_base + s['vaddr']
            for mo in rx.finditer(d):
                va = base + mo.start()
                # 取周围的可读串
                lo = max(0, mo.start() - 64)
                ctx = d[lo:mo.start() + 96]
                txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in ctx)
                found.append((va, s['name'], txt))
                if len(found) >= 6:
                    break
            if len(found) >= 6:
                break
        if found:
            print('== %s : %d ==' % (pat.decode(), len(found)))
            for va, sec, txt in found:
                print('   %#012x [%s] %s' % (va, sec, txt))
            print()

    # Oodle 名字表本身：列出所有 Oodle* 标识符
    print('=== 全部 Oodle* 标识符 ===')
    names = set()
    for s in img.sections:
        if s['name'] not in ('.rdata', '_RDATA'):
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        for mo in re.finditer(rb'Oodle[A-Za-z0-9_]{2,48}', d):
            names.add(mo.group().decode())
    for n in sorted(names):
        print('   %s' % n)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
