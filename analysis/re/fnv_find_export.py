#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fnv_find_export.py — 在系统 DLL 的导出表里反查被哈希隐藏的函数名。

`sub_148AD6250` 遍历模块导出表、用 FNV 变体哈希名字后与常量比对；找到后还会
顺着「转发器字符串」继续链式解析 —— 这是典型的「按哈希隐藏 API 调用」手法。
"""

import os
import struct
import sys

FNV_OFFSET = 2166136261
FNV_PRIME = 16777619
MASK64 = (1 << 64) - 1
TARGETS = [
    0x66F841C0F75DB66E,
    0x66F841C0E95DA064,
    0x3F2BCA4589EABB08,
    0x0B2DC38A8E4BAECD,
]

DLLS = [
    r'C:\Windows\System32\kernel32.dll',
    r'C:\Windows\System32\KernelBase.dll',
    r'C:\Windows\System32\ntdll.dll',
    r'C:\Windows\System32\user32.dll',
    r'C:\Windows\System32\advapi32.dll',
    r'C:\Windows\System32\ole32.dll',
    r'C:\Windows\System32\shell32.dll',
    r'C:\Windows\System32\win32u.dll',
    r'C:\Windows\System32\bcrypt.dll',
    r'C:\Windows\System32\d3d12.dll',
    r'C:\Windows\System32\dxgi.dll',
    r'C:\Windows\System32\ws2_32.dll',
    r'C:\Windows\System32\psapi.dll',
    r'C:\Windows\System32\dbghelp.dll',
]


def exports(path):
    try:
        with open(path, 'rb') as f:
            d = f.read()
    except OSError:
        return []
    if d[:2] != b'MZ':
        return []
    e = struct.unpack_from('<I', d, 0x3c)[0]
    if d[e:e + 4] != b'PE\0\0':
        return []
    nsec = struct.unpack_from('<H', d, e + 6)[0]
    optsz = struct.unpack_from('<H', d, e + 20)[0]
    opt = e + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    exp_rva, exp_sz = struct.unpack_from('<II', d, dd)
    if not exp_rva:
        return []
    secs = []
    so = opt + optsz
    for i in range(nsec):
        b = so + i * 40
        vs, va, rs, rp = struct.unpack_from('<IIII', d, b + 8)
        secs.append((va, max(vs, rs), rp))

    def rva2off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rp + (rva - va)
        return None

    off = rva2off(exp_rva)
    if off is None:
        return []
    nnames = struct.unpack_from('<I', d, off + 24)[0]
    names_rva = struct.unpack_from('<I', d, off + 32)[0]
    noff = rva2off(names_rva)
    if noff is None:
        return []
    out = []
    for i in range(min(nnames, 20000)):
        nr = struct.unpack_from('<I', d, noff + i * 4)[0]
        no = rva2off(nr)
        if no is None:
            continue
        end = d.find(b'\0', no, no + 256)
        if end < 0:
            continue
        out.append(d[no:end].decode('latin1'))
    return out


def fnv(name: str) -> int:
    v = FNV_OFFSET
    for ch in name:
        c = ord(ch)
        if 65 <= c <= 90:
            c |= 0x20
        v = (FNV_PRIME * (v ^ c)) & MASK64
    return v


def main():
    total = 0
    hits = []
    for path in DLLS:
        ex = exports(path)
        total += len(ex)
        for n in ex:
            h = fnv(n)
            if h in TARGETS:
                hits.append((os.path.basename(path), n, h))
    print('扫了 %d 个导出名' % total)
    print('=== 命中 ===')
    for dll, n, h in hits:
        print('   %-20s %-40s %#x' % (dll, n, h))
    if not hits:
        print('   （没有命中）')
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
