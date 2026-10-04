#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""oodle_selftest.py — 验证 oo2core DLL 的调用封装是否正确（压一轮再解回来）。

如果这步过不了，说明签名/调用约定写错了，那么"扫不到 Oodle 流"就不能作为
"数据被加密"的证据。
"""

import ctypes
import hashlib
import os
import sys

DLL = r'E:\SteamLibrary\steamapps\common\Call of Duty HQ\oo2core_8_win64.dll'


def main():
    dll = ctypes.WinDLL(DLL)
    comp = dll.OodleLZ_Compress
    decomp = dll.OodleLZ_Decompress

    # OodleLZ_Compress(compressor, rawBuf, rawLen, compBuf, level, opts,
    #                  dictionaryBase, lrm, scratchMem, scratchSize)
    comp.restype = ctypes.c_longlong
    comp.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_longlong,
                     ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                     ctypes.c_void_p, ctypes.c_void_p,
                     ctypes.c_void_p, ctypes.c_longlong]

    decomp.restype = ctypes.c_longlong
    decomp.argtypes = [ctypes.c_char_p, ctypes.c_longlong,
                       ctypes.c_char_p, ctypes.c_longlong,
                       ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       ctypes.c_void_p, ctypes.c_longlong,
                       ctypes.c_void_p, ctypes.c_void_p,
                       ctypes.c_void_p, ctypes.c_longlong, ctypes.c_uint]

    # 造一段有冗余的数据（可压缩）
    raw = (b'Minecraft Dungeons II pak entry test payload. ' * 500)
    print('原始数据 %d 字节  sha256=%s' % (len(raw), hashlib.sha256(raw).hexdigest()[:16]))

    cbuf = ctypes.create_string_buffer(len(raw) + 4096)
    # compressor=4 是 Kraken 附近的口径, level=4 (Normal)
    n = comp(4, raw, len(raw), cbuf, 4, 0, None, None, None, 0)
    print('压缩返回 %d' % n)
    if n <= 0:
        print('压缩失败 —— DLL 可能不可用, 或参数不对')
        return 1
    cdata = cbuf.raw[:n]
    print('压缩后 %d 字节, 前 16: %s' % (n, cdata[:16].hex()))

    obuf = ctypes.create_string_buffer(len(raw))
    m = decomp(cdata, n, obuf, len(raw), 1, 0, 0, None, 0, None, None, None, 0, 0)
    print('解压返回 %d' % m)
    if m != len(raw):
        print('解压长度不符')
        return 1
    got = obuf.raw[:m]
    ok = (got == raw)
    print('往返一致: %s' % ok)
    print('解压后 sha256=%s' % hashlib.sha256(got).hexdigest()[:16])
    if ok:
        print('\n=> 调用封装正确, DLL 可用。扫不到 Oodle 流就不是封装问题。')
        return 0
    return 1


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
