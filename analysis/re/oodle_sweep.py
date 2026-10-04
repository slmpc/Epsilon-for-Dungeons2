#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""oodle_sweep.py — 穷举搜索：对 pak 条目窗口，遍历所有 AES 对齐 × 所有偏移，
逐个试 Oodle 解压，找出真正能解开的起点。

为什么要穷举: FPakEntry 的头部长度没定死，"载荷从第几字节开始"只是推测。
与其继续猜，不如把 (解密对齐, 流起点, 原始长度) 的组合全试一遍 ——
每次 Oodle 调用都很快，几千次也就几秒。

自检: 同一 DLL 的 OodleLZ_Compress -> OodleLZ_Decompress 往返已通过,
所以"扫不到"是结论而不是封装问题。

    python oodle_sweep.py <条目偏移> <原始长度候选,逗号分隔> [窗口长度]
"""

import ctypes
import os
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
KEY = bytes.fromhex('1f81803353095534b0544c9e26088a3d4db069c9727d38de649cb18e4fd4a3ec')
DLL = r'E:\SteamLibrary\steamapps\common\Call of Duty HQ\oo2core_8_win64.dll'


def main():
    entry = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x396800
    cands = [int(x, 0) for x in sys.argv[2].split(',')] if len(sys.argv) > 2 else [13978, 14010]
    win = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x2000

    dll = ctypes.WinDLL(DLL)
    fn = dll.OodleLZ_Decompress
    fn.restype = ctypes.c_longlong
    fn.argtypes = [ctypes.c_char_p, ctypes.c_longlong,
                   ctypes.c_char_p, ctypes.c_longlong,
                   ctypes.c_int, ctypes.c_int, ctypes.c_int,
                   ctypes.c_void_p, ctypes.c_longlong,
                   ctypes.c_void_p, ctypes.c_void_p,
                   ctypes.c_void_p, ctypes.c_longlong, ctypes.c_uint]

    with open(PAK, 'rb') as f:
        f.seek(entry)
        raw = f.read(win)

    print('条目 @ %#x, 窗口 %d 字节, 原始长度候选 %s' % (entry, len(raw), cands))
    print('穷举: 解密对齐 0..0xF0 步长 16 × 起点 0..%#x × 长度候选' % min(0x200, win // 2))

    hits = []
    tried = 0
    for align in range(0, 0x100, 16):
        seg = raw[align:]
        n = len(seg) - (len(seg) % 16)
        if n < 32:
            continue
        d = Cipher(algorithms.AES(KEY), modes.ECB()).decryptor()
        dec = d.update(seg[:n]) + d.finalize()

        for off in range(0, min(0x200, len(dec) - 32)):
            for raw_len in cands:
                out = ctypes.create_string_buffer(raw_len)
                got = fn(dec[off:], len(dec) - off, out, raw_len,
                         1, 0, 0, None, 0, None, None, None, 0, 0)
                tried += 1
                if got > 0:
                    hits.append((align, off, raw_len, got, out.raw[:64]))
                    print('   ★ 命中 align=%#x off=%#x rawLen=%d -> %d 字节'
                          % (align, off, raw_len, got))
                    print('       头: %r' % out.raw[:64])

    print('\n共尝试 %d 次组合' % tried)
    if not hits:
        print('没有命中 —— 在「该密钥 + 该 Oodle 版本」下, 这个条目解不开')
        print('可能: (1) 载荷的密钥/算法不是 AES-256-ECB')
        print('      (2) 游戏用的 Oodle 版本比 oo2core_8 新, 解不了')
        print('      (3) 原始长度不是我给的这几个')
    return 0 if hits else 1


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
