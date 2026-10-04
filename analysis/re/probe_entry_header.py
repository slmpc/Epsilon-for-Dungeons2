#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_entry_header.py — 定死内联 FPakEntry 的头部长度（未压缩条目）。"""

import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_pak import Pak, PAK   # noqa: E402


def main():
    key = bytes.fromhex(sys.argv[1] if len(sys.argv) > 1 else '')
    pk = Pak(key)
    shown = 0
    for path, loc in pk.files:
        if not (path.endswith('.ini') or path.endswith('.uplugin') or path.endswith('.mtl')):
            continue
        e = pk.encoded(loc)
        if not e:
            continue
        pk.f.seek(e['pakoff'])
        b = pk.f.read(0x60)
        # 判据: 前 24 字节里 Size/UncompressedSize 应当与 encoded 条目一致
        size, usize = struct.unpack_from('<qq', b, 8)
        if size != e['csize'] or usize != e['usize']:
            continue
        print('=== %s' % path)
        print('    pakoff=%#x  csize=%d  usize=%d' % (e['pakoff'], e['csize'], e['usize']))
        for i in range(0, 0x60, 16):
            c = b[i:i + 16]
            print('    %02x  %-47s  %s' % (i, ' '.join('%02X' % x for x in c),
                                           ''.join(chr(x) if 32 <= x < 127 else '.' for x in c)))
        # 尝试: 头部之后应当是可读文本
        for hdr in (0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50):
            pk.f.seek(e['pakoff'] + hdr)
            t = pk.f.read(40)
            pr = sum(1 for c in t if 32 <= c < 127) / len(t)
            mark = '  <== 像明文起点' if pr > 0.9 else ''
            print('    hdr=%#04x -> %r (%.2f)%s' % (hdr, t[:40], pr, mark))
        print()
        shown += 1
        if shown >= 3:
            break
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
