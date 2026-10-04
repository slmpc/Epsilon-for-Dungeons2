#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""decrypt_windows.py — 按不同 16 字节对齐起点解密条目窗口。

pak 的 FPakEntry 头部是明文，载荷是「先 Oodle 压缩、再 AES 加密」。
加密区间的起点必须 16 字节对齐，但具体从哪开始（头部长度）未知，
所以把若干候选对齐点各解一份出来，交给 oodlefind 扫 —— 命中的那个就是真起点。
"""

import os
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
KEY = bytes.fromhex('1f81803353095534b0544c9e26088a3d4db069c9727d38de649cb18e4fd4a3ec')

OUTDIR = r'D:\Dev\OpenEpsilon\EpsilonForDungeons2\build'


def dec(data):
    n = len(data) - (len(data) % 16)
    d = Cipher(algorithms.AES(KEY), modes.ECB()).decryptor()
    return d.update(data[:n]) + d.finalize()


def main():
    entry = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x396800
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x1000
    tag = sys.argv[3] if len(sys.argv) > 3 else 'ini'

    print('条目 @ %#x, 每个候选窗口 %d 字节' % (entry, n))
    made = []
    for rel in (0x30, 0x40, 0x50, 0x60, 0x70, 0x80):
        start = entry + rel
        with open(PAK, 'rb') as f:
            f.seek(start)
            raw = f.read(n)
        plain = dec(raw)
        path = os.path.join(OUTDIR, 'dec_%s_%02x.bin' % (tag, rel))
        with open(path, 'wb') as w:
            w.write(plain)
        pr = sum(1 for c in plain[:512] if 32 <= c < 127) / 512
        print('   rel=%#04x -> %s  (前 512 可打印率 %.3f)' % (rel, path, pr))
        made.append(path)
    print('\n把下面这些路径交给 oodlefind:')
    for p in made:
        print('   %s' % p)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
