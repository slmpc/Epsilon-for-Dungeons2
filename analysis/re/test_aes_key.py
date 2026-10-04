#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_aes_key.py — 用一个候选 AES-256 密钥去试容器的索引。

.pak 的 FPakInfo 尾部（本构建 v11，实测偏移）:
    +0x00 u32 magic 0x5A6F12E1
    +0x04 i32 version
    +0x08 i64 IndexOffset
    +0x10 i64 IndexSize
    +0x18 u8  IndexHash[20]      (SHA-1)
    +0x2C char CompressionMethodName[]   -> "Oodle"

判据：索引若被 AES-256-ECB 加密，用正确密钥解出来后应当**明显不再是均匀随机**
（Pak 索引解压前也带结构；最直接是看可打印率与是否出现路径/魔数）。
"""

import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAKS = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
        r'\Dungeons\Content\Paks')
PAK_MAGIC = 0x5A6F12E1


def aes_ecb_dec(key, data):
    data = data[:len(data) - (len(data) % 16)]
    d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return d.update(data) + d.finalize()


def aes_cbc_dec(key, data, iv=b'\0' * 16):
    data = data[:len(data) - (len(data) % 16)]
    d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return d.update(data) + d.finalize()


def pr(b):
    return sum(1 for c in b if 32 <= c < 127) / max(1, len(b)) if b else 0.0


def read_footer(path):
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        f.seek(max(0, size - (1 << 20)))
        tail = f.read()
        base = size - len(tail)
    i = tail.rfind(struct.pack('<I', PAK_MAGIC))
    if i < 0:
        return None
    off = base + i
    with open(path, 'rb') as f:
        f.seek(off)
        blk = f.read(0x60)
    magic, ver = struct.unpack_from('<Ii', blk, 0)
    ioff, isize = struct.unpack_from('<qq', blk, 8)
    ihash = blk[0x18:0x2C]
    mname = blk[0x2C:0x2C + 32].split(b'\0')[0]
    return dict(off=off, version=ver, index_offset=ioff, index_size=isize,
                index_hash=ihash, method=mname, size=size)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    hx = sys.argv[1].strip()
    if hx.lower().startswith('0x'):
        hx = hx[2:]
    key = bytes.fromhex(hx)
    print('候选密钥 %s  (%d 字节)\n' % (key.hex(), len(key)))

    pak = os.path.join(PAKS, 'Dungeons-Windows.pak')
    info = read_footer(pak)
    print('=== .pak 尾部 ===')
    for k, v in info.items():
        if isinstance(v, bytes):
            print('   %-14s %s' % (k, v.hex()))
        elif isinstance(v, int) and k != 'version':
            print('   %-14s %#x' % (k, v))
        else:
            print('   %-14s %s' % (k, v))

    ioff, isize = info['index_offset'], info['index_size']
    if not (0 < ioff < info['size'] and 0 < isize < info['size']):
        print('   索引偏移/大小不合理，放弃')
        return 1
    with open(pak, 'rb') as f:
        f.seek(ioff)
        enc = f.read(isize)

    print('\n=== 原始索引数据 ===')
    print('   头 32 密文: %r' % enc[:32])
    print('   可打印率: %.3f  (随机期望 0.371)' % pr(enc[:4096]))

    print('\n=== 各种解法的结果（可打印率 / 头部） ===')
    trials = [
        ('ECB(原样)', aes_ecb_dec(key, enc)),
        ('CBC(IV=0)', aes_cbc_dec(key, enc)),
        ('ECB(密钥反转)', aes_ecb_dec(key[::-1], enc)),
    ]
    for name, out in trials:
        head = out[:48]
        print('   %-16s pr=%.3f  head=%r' % (name, pr(out[:4096]), head))
        for probe in (b'../', b'.uasset', b'.uexp', b'.ubulk', b'Mount', b'/Game'):
            if probe in out[: 4 << 20]:
                print('        ★ 命中 %r' % probe)

    # 对照：不加密直接看
    print('\n   未解密对照       pr=%.3f' % pr(enc[:4096]))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
