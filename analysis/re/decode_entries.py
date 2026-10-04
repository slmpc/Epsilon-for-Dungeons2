#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""decode_entries.py — 反推 EncodedPakEntries 的条目格式。

线索: FullDirectoryIndex 里给的是 (目录, 文件数, [(文件名, 下标)])，
其中下标最大到 5 万+，而 EncodedPakEntries 只有 130792 字节
-> **下标是字节偏移**，不是数组下标。

于是可以拿已知文件（按名字知道大小量级）去对同一条目的解码结果。
"""

import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
INDEX_OFF = 0xF19FD2A


def load(key):
    with open(PAK, 'rb') as f:
        f.seek(INDEX_OFF)
        raw = f.read(0x1FF60)
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    buf = d.update(raw) + d.finalize()
    pos = 0
    (mpl,) = struct.unpack_from('<i', buf, pos); pos += 4
    pos += mpl
    (num,) = struct.unpack_from('<i', buf, pos); pos += 4
    pos += 8
    has_phi = struct.unpack_from('<i', buf, pos)[0]; pos += 4
    if has_phi:
        pos += 16 + 20
    has_fdi = struct.unpack_from('<i', buf, pos)[0]; pos += 4
    fdi_off, fdi_size = struct.unpack_from('<qq', buf, pos); pos += 16 + 20
    (ees,) = struct.unpack_from('<i', buf, pos); pos += 4
    return buf, pos, ees, fdi_off, fdi_size


def main():
    key = bytes.fromhex(sys.argv[1] if len(sys.argv) > 1 else '')
    buf, ee_off, ee_size, fdi_off, fdi_size = load(key)
    ee = buf[ee_off:ee_off + ee_size]
    print('EncodedPakEntries: %d 字节' % ee_size)

    with open(PAK, 'rb') as f:
        f.seek(fdi_off)
        raw = f.read(fdi_size)
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    fdi = d.update(raw) + d.finalize()

    pos = 0
    (nd,) = struct.unpack_from('<i', fdi, pos); pos += 4
    rows = []
    for _ in range(nd):
        (n,) = struct.unpack_from('<i', fdi, pos); pos += 4
        dirn = fdi[pos:pos + n - 1].decode('latin1', 'replace'); pos += n
        (nf,) = struct.unpack_from('<i', fdi, pos); pos += 4
        for _ in range(nf):
            (n,) = struct.unpack_from('<i', fdi, pos); pos += 4
            fn = fdi[pos:pos + n - 1].decode('latin1', 'replace'); pos += n
            (idx,) = struct.unpack_from('<i', fdi, pos); pos += 4
            rows.append((dirn + fn, idx))

    print('文件数 %d, 下标范围 %d .. %d' %
          (len(rows), min(r[1] for r in rows), max(r[1] for r in rows)))

    print('\n=== 若干文件的条目原始字节 ===')
    for path, idx in rows[:6] + [r for r in rows if r[0].endswith('.uproject')][:1] \
                   + [r for r in rows if r[0].endswith('AssetRegistry.bin')][:1]:
        if idx < 0 or idx >= len(ee):
            print('   %-52s idx=%d (越界)' % (path[:52], idx))
            continue
        seg = ee[idx:idx + 24]
        u32 = struct.unpack_from('<IIIIII', seg, 0)
        print('   %-52s idx=%-6d %s' % (path[:52], idx,
                                        ' '.join('%08x' % v for v in u32)))
        print('        bytes: %s' % ' '.join('%02X' % c for c in seg))

    print('\n=== 把所有下标排序, 看步长分布(条目长度) ===')
    idxs = sorted(set(r[1] for r in rows if 0 <= r[1] < len(ee)))
    print('   唯一下标 %d 个' % len(idxs))
    gaps = [idxs[i + 1] - idxs[i] for i in range(min(len(idxs) - 1, 40))]
    print('   前 40 个间距: %s' % gaps)
    print('   间距取值集合(前 20): %s' % sorted(set(gaps))[:20])
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
