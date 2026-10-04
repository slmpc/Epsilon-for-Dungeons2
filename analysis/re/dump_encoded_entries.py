#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dump_encoded_entries.py — 把 pak 索引里的 EncodedPakEntries 摊出来反推格式。

索引解出后的布局:
    [0]    int32 MountPointLength + MountPoint
           int32 NumEntries
           uint64 PathHashSeed
           int32 bReaderHasPathHashIndex + (int64,int64,20B)
           int32 bReaderHasFullDirectoryIndex + (int64,int64,20B)
           int32 EncodedPakEntriesSize
           uint8 EncodedPakEntries[]        <- 这里
"""

import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')


def main():
    key = bytes.fromhex(sys.argv[1] if len(sys.argv) > 1 else '')
    with open(PAK, 'rb') as f:
        f.seek(0xF19FD2A)
        raw = f.read(0x1FF60)
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    buf = d.update(raw) + d.finalize()

    pos = 0
    (mpl,) = struct.unpack_from('<i', buf, pos); pos += 4
    mp = buf[pos:pos + mpl]; pos += mpl
    (num,) = struct.unpack_from('<i', buf, pos); pos += 4
    seed = struct.unpack_from('<Q', buf, pos)[0]; pos += 8
    has_phi = struct.unpack_from('<i', buf, pos)[0]; pos += 4
    phi = struct.unpack_from('<qq', buf, pos); pos += 16 + 20
    has_fdi = struct.unpack_from('<i', buf, pos)[0]; pos += 4
    fdi = struct.unpack_from('<qq', buf, pos); pos += 16 + 20
    (ees,) = struct.unpack_from('<i', buf, pos); pos += 4

    print('mount=%r num=%d seed=%#x phi=%s fdi=%s' % (mp, num, seed, phi, fdi))
    print('EncodedPakEntries @索引偏移 %#x, 长 %d' % (pos, ees))
    ee = buf[pos:pos + ees]

    print('\n=== 前 40 个 u32 ===')
    for i in range(0, 160, 4):
        v = struct.unpack_from('<I', ee, i)[0]
        print('  +%#05x  %#010x  %12d' % (i, v, v))

    print('\n=== 前 128 字节原始 ===')
    for i in range(0, 128, 16):
        c = ee[i:i + 16]
        print('  %04x  %-47s  %s' % (i, ' '.join('%02X' % x for x in c),
                                     ''.join(chr(x) if 32 <= x < 127 else '.' for x in c)))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
