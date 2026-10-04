#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在更新后的映像里重新定位四个引擎全局（GObjects / GNames / GEngine / GWorld）。

策略（不依赖运行时）：
  * GObjects: 在 .text 里找 `shr r8, 0x10` 之后 48 字节内的 `mov rax,[rip+disp]`，
    把 disp 解出的地址减去 0x10 当候选；再用 .data 里的结构自洽性挑。
  * GNames  : 扫 .data 找 64KiB 对齐、块头是合法 FNameEntry 链的指针数组。
"""

import re
import struct
import sys

from peimg import Image

img = Image()
IMG = img.image_base


def sections():
    return {s['name']: s for s in img.sections}


def sec_range(name):
    s = sections()[name]
    return IMG + s['vaddr'], IMG + s['vaddr'] + s['vsize'], s


def read(va, n):
    return img.read_va(va, n)


def u32(va):
    return img.u32(va)


def u64(va):
    return img.u64(va)


# ---------------------------------------------------------------- GObjects
def find_gobjects():
    lo, hi, s = sec_range('.text')
    img.f.seek(s['rawptr'])
    text = img.f.read(s['rawsize'])
    base = lo
    out = []
    for i in range(len(text) - 8):
        if text[i:i + 4] != b'\x49\xc1\xe8\x10':
            continue
        for k in range(0, 64):
            j = i + k
            if j + 7 > len(text):
                break
            if text[j:j + 3] == b'\x48\x8b\x05':
                disp = struct.unpack_from('<i', text, j + 3)[0]
                target = base + j + 7 + disp
                out.append((target - 0x10, base + j))
                break
    return out


def validate_gobjects(va):
    """.data 里 FUObjectArray 的形态校验。"""
    dlo, dhi, _ = sec_range('.data')
    if not (dlo <= va < dhi):
        return None
    maxel = u32(va + 0x20)
    numel = u32(va + 0x24)
    maxch = u32(va + 0x28)
    numch = u32(va + 0x2C)
    if None in (maxel, numel, maxch, numch):
        return None
    if not (1 <= numel <= 8_000_000 and numel <= maxel <= 32_000_000):
        return None
    if numch != (numel + 0xFFFF) // 0x10000 or numch == 0:
        return None
    if not (1 <= maxch <= 8192 and maxch >= numch):
        return None
    tbl = u64(va + 0x10)
    if not tbl:
        return None
    c0 = u64(tbl)
    if not c0:
        return None
    # .rdata / _RDATA 里应当有对象的 vtable
    rd = []
    for nm in ('.rdata', '_RDATA'):
        try:
            a, b, _ = sec_range(nm)
            rd.append((a, b))
        except KeyError:
            pass
    ok = 0
    for idx in range(8):
        ch = u64(tbl + 8 * (idx // 0x10000))
        if not ch:
            continue
        obj = u64(ch + (idx % 0x10000) * 0x18)
        if not obj:
            continue
        vt = u64(obj)
        if vt and any(a <= vt < b for a, b in rd):
            ok += 1
    return dict(num=numel, max=maxel, chunks=numch, validated=ok)


# ---------------------------------------------------------------- GNames
def fname_entry_ok(va):
    h = read(va, 2)
    if not h:
        return None
    hh = struct.unpack('<H', h)[0]
    if hh & 1:
        return None
    ln = (hh >> 6) & 0x3FF
    if not (1 <= ln <= 250):
        return None
    s = read(va + 2, ln + 1)
    if not s or len(s) < ln + 1 or s[ln] != 0:
        return None
    if not all(0x20 <= c < 0x7F for c in s[:ln]):
        return None
    return s[:ln]


def chain_ok(va, want=4):
    p = va
    for _ in range(want):
        n = fname_entry_ok(p)
        if n is None:
            return False
        step = 2 + len(n) + 1
        step += step & 1
        p += step
    return True


def find_gnames():
    a, b, _ = sec_range('.data')
    img.f.seek(sections()['.data']['rawptr'])
    data = img.f.read(sections()['.data']['rawsize'])
    slots = []
    for off in range(0, len(data) - 8, 8):
        v = struct.unpack_from('<Q', data, off)[0]
        if v and (v & 0xFFFF) == 0 and 0x10000 < v < 0x7FFFFFFFFFFF and chain_ok(v):
            slots.append(a + off)
    if not slots:
        return None
    p = min(slots)
    steps = 0
    while steps < 1 << 16:
        v = u64(p - 8)
        if v is None:
            break
        if v != 0 and not ((v & 0xFFFF) == 0):
            break
        p -= 8
        steps += 1
    return dict(blocks=p, va=p - 0x10, rva=p - 0x10 - IMG,
                found=len(slots), back=steps)


def main():
    print('image base %#x' % IMG)
    print()
    print('== GObjects 候选 ==')
    seen = set()
    for va, hit in find_gobjects():
        if va in seen:
            continue
        seen.add(va)
        v = validate_gobjects(va)
        print('   VA %#012x  RVA %#010x  hit %#x  %s' %
              (va, va - IMG, hit, v if v else '(校验不过)'))
    print()
    print('== GNames ==')
    gn = find_gnames()
    print('   %s' % gn)
    return 0


if __name__ == '__main__':
    sys.exit(main())
