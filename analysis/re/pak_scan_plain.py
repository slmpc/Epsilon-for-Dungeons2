#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pak_scan_plain.py — 整段解密 pak 数据区，找没有压缩的条目。

UE 在启用加密时会把条目按 FAES::AESBlockSize(16) 对齐，而 AES-ECB 逐块独立、
无状态，所以**从任意 16 字节对齐处解密都成立** —— 不需要先知道条目偏移。

于是: 解出来出现明文 -> 该条目只加密未压缩, 可以直接提取;
      解出来仍是随机 -> 该条目是 Oodle 压缩的。
"""

import os
import re
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
INDEX_OFF = 0xF19FD2A

MARKERS = [b'[/Script/', b'.uproject', b'\r\n', b'{\n', b'"name"', b'SW.',
           b'<?xml', b'PK\x03\x04', b'\x89PNG', b'RIFF', b'#version', b'[']


def main():
    key = bytes.fromhex(sys.argv[1] if len(sys.argv) > 1 else '')
    size = os.path.getsize(PAK)
    end = INDEX_OFF & ~0xF   # 必须是 16 的倍数, ECB 按块解

    print('解密数据区 [0, %#x) ...' % end)
    dec = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    out = []
    off = 0
    CH = 8 << 20
    with open(PAK, 'rb') as f:
        while off < end:
            n = min(CH, end - off)
            buf = f.read(n)
            if not buf:
                break
            out.append(dec.update(buf))
            off += n
    out.append(dec.finalize())
    blob = b''.join(out)
    print('   解出 %d 字节' % len(blob))

    print('\n=== 明文标志统计 ===')
    for m in MARKERS:
        c = blob.count(m)
        print('   %-16r %d' % (m, c))

    print('\n=== 抽取若干明文样本的上下文 ===')
    shown = 0
    for m in (b'[/Script/', b'\x89PNG', b'PK\x03\x04', b'<?xml', b'RIFF',
              b'"name"', b'{\n'):
        i = blob.find(m)
        if i < 0:
            continue
        lo = max(0, i - 32)
        seg = blob[lo:i + 96]
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in seg)
        print('   %-14r @%#x: %s' % (m, i, txt))
        shown += 1
    if shown == 0:
        print('   (没有找到任何明文 —— 所有条目都被压缩了)')

    # 分块看哪些区块解出来是可读的
    print('\n=== 每 1 MiB 的可打印率（找可读区段） ===')
    for i in range(0, len(blob), 1 << 20):
        seg = blob[i:i + (1 << 20)]
        p = sum(1 for c in seg if 32 <= c < 127) / len(seg)
        if p > 0.45:
            print('   %#010x  pr=%.3f  <-- 可读' % (i, p))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
