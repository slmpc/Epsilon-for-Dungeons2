#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""oodle_offline.py — 用第三方 oo2core DLL 离线解 Oodle 流。

本机在 Call of Duty HQ 下有一份 `oo2core_8_win64.dll`。Oodle 的**解码器对旧版本
流是向后兼容的**（编码器版本 <= 解码器版本即可），所以值得一试 —— 若能用，
整条 pak 提取链就能离线跑完，不必依赖游戏进程。

用法:
    python oodle_offline.py --probe                # 只验证 DLL 能否加载
    python oodle_offline.py <文件> <原始长度>       # 在文件里扫 Oodle 流起点
"""

import ctypes
import os
import sys

DLL_CANDIDATES = [
    r'E:\SteamLibrary\steamapps\common\Call of Duty HQ\oo2core_8_win64.dll',
    r'oo2core_9_win64.dll',
    r'C:\Windows\System32\oo2core_8_win64.dll',
]

OODLELZ_FUZZSAFE_YES = 1
OODLELZ_CHECKCRC_NO = 0
OODLELZ_VERBOSITY_NONE = 0


def load():
    for p in DLL_CANDIDATES:
        if not os.path.exists(p):
            continue
        try:
            dll = ctypes.WinDLL(p)
        except OSError as e:
            print('   %s 加载失败: %s' % (p, e))
            continue
        fn = getattr(dll, 'OodleLZ_Decompress', None)
        if fn is None:
            print('   %s 没有 OodleLZ_Decompress 导出' % p)
            continue
        fn.restype = ctypes.c_longlong
        fn.argtypes = [ctypes.c_char_p, ctypes.c_longlong,
                       ctypes.c_char_p, ctypes.c_longlong,
                       ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       ctypes.c_void_p, ctypes.c_longlong,
                       ctypes.c_void_p, ctypes.c_void_p,
                       ctypes.c_void_p, ctypes.c_longlong, ctypes.c_uint]
        print('   OK: %s' % p)
        return fn
    return None


def main():
    print('=== 找可用的 Oodle 解码器 ===')
    fn = load()
    if fn is None:
        print('   （没有可用的 Oodle DLL）')
        return 1

    if len(sys.argv) < 3 or sys.argv[1] == '--probe':
        print('\n仅验证加载。要扫起点: python oodle_offline.py <文件> <原始长度>')
        return 0

    path = sys.argv[1]
    raw_len = int(sys.argv[2], 0)
    data = open(path, 'rb').read()
    out = ctypes.create_string_buffer(raw_len)

    print('\n=== 在 %s (%d 字节) 里扫 Oodle 流起点, 期望解出 %d 字节 ==='
          % (path, len(data), raw_len))
    hits = 0
    for off in range(0, min(len(data) - 16, 0x400)):
        n = fn(data[off:], len(data) - off, out, raw_len,
               OODLELZ_FUZZSAFE_YES, OODLELZ_CHECKCRC_NO, OODLELZ_VERBOSITY_NONE,
               None, 0, None, None, None, 0, 0)
        if n > 0:
            head = out.raw[:64]
            print('   +%#06x  解出 %d 字节  head=%r'
                  % (off, n, head[:48]))
            hits += 1
            if hits >= 10:
                break
    if hits == 0:
        print('   没有命中')
    else:
        print('   命中 %d 处' % hits)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
