#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 UE 反射条目（FField）里的 Offset_Internal。

本构建 FField 反射记录的锚点是指向属性名字串的 8 字节槽 S，字段位置固定：

    S[-0x18] = uint32 Offset_Internal     <-- 属性在 UObject 上的字节偏移
    S[-0x14] = uint32 类型哈希
    S[-0x10] = uint32 名字长度
    S[-0x0c] = uint32 0x45
    S[ 0x00] = char*  名字串

用 ATR_Movement 的已知偏移校准：MovementSpeedMultiplier=0x90、MovementFriction=0xA0、
MovementFrictionMultiplier=0xB0、GravityScale=0xF0。

    python fields.py EmeraldAmount EmeraldIncreasePercentage ...
    python fields.py --all GravityScale
"""

import struct
import sys

from peimg import Image

img = Image()
RD_LO, RD_HI = 0x148E33600, 0x14BA78000
SCAN = ('.rdata', '.data', '_RDATA')


def all_name_ptrs():
    out = []
    for s in img.sections:
        if s['name'] not in SCAN:
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        for off in range(0, len(d) - 8, 8):
            v = struct.unpack_from('<Q', d, off)[0]
            if not (RD_LO <= v < RD_HI):
                continue
            name = img.cstr(v, 96)
            if not name or len(name) < 2:
                continue
            if not (name[0].isalpha() or name[0] == '_'):
                continue
            if not all(c.isalnum() or c == '_' for c in name):
                continue
            out.append((base + off, v, name))
    return out


def decode(slot):
    b = img.read_va(slot - 0x20, 0x20)
    if not b or len(b) < 0x20:
        return None
    mark = struct.unpack_from('<I', b, 0)[0]
    off_int = struct.unpack_from('<I', b, 4)[0]
    thash = struct.unpack_from('<I', b, 8)[0]
    namelen = struct.unpack_from('<I', b, 0x0c)[0]
    marker = struct.unpack_from('<I', b, 0x10)[0]
    return dict(mark=mark, off=off_int, hash=thash, namelen=namelen, marker=marker)


def main():
    args = sys.argv[1:]
    allmode = '--all' in args
    want = [a for a in args if not a.startswith('--')]
    wantset = set(want)
    ptrs = all_name_ptrs()
    print('# %d 个名字槽' % len(ptrs))
    seen = set()
    for slot, nva, name in ptrs:
        if wantset and name not in wantset:
            continue
        d = decode(slot)
        if d is None:
            continue
        if not allmode and not (d['marker'] == 0x45 and d['mark'] == 1):
            continue
        key = (name, d['off'], slot)
        if key in seen:
            continue
        seen.add(key)
        print('  %-42s off=%#-8x slot=%#012x  mark=%#x hash=%#010x len=%d' %
              (name, d['off'], slot, d['mark'], d['hash'], d['namelen']))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
