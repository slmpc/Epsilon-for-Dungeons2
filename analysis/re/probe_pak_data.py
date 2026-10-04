#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_pak_data.py — 判断 pak 数据区是明文 / 加密 / 压缩。

已知: 压缩方法 Oodle, 密钥可解索引。
数据区 = [0, IndexOffset)。

做法: 在数据区取样, 分别按「原样」与「AES-256-ECB 解密」两种读法,
统计可读文本量。明文 INI/JSON 会给出大量可读串。
"""

import os
import re
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
INDEX_OFF = 0xF19FD2A

# 明文里一定会有的东西
MARKERS = [b'[/Script/', b'.uproject', b'"name"', b'{', b'SW_', b'Dungeons',
           b'PlayerController', b'\r\n']


def pr(b):
    return sum(1 for c in b if 32 <= c < 127) / max(1, len(b))


def count_strings(b, minlen=8):
    return len(re.findall(rb'[ -~]{%d,}' % minlen, b))


def main():
    key = bytes.fromhex(sys.argv[1] if len(sys.argv) > 1 else '')
    size = os.path.getsize(PAK)
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()

    print('数据区 [0, %#x) = %d 字节' % (INDEX_OFF, INDEX_OFF))
    samples = [0, 0x100000, 0x800000, 0x2000000, 0x6000000, 0xA000000, 0xE000000]
    print('\n%-12s %-10s %-10s %-10s %s' %
          ('offset', '原样pr', '原样串数', '解密pr', '解密后头部 48 字节'))
    with open(PAK, 'rb') as f:
        for off in samples:
            if off >= INDEX_OFF:
                continue
            f.seek(off)
            raw = f.read(1 << 16)
            dec = d.update(raw) + d.finalize() if False else None
            dd = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
            dec = dd.update(raw) + dd.finalize()
            print('%#-12x %-10.3f %-10d %-10.3f %r' %
                  (off, pr(raw), count_strings(raw), pr(dec), dec[:48]))

    print('\n=== 在数据区里直接搜明文标志 ===')
    with open(PAK, 'rb') as f:
        blob = f.read(INDEX_OFF)
    for m in MARKERS:
        c = blob.count(m)
        if c:
            print('   %-20r 命中 %d 次' % (m, c))
    print('   (全 0 = 数据区没有明文)')
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
