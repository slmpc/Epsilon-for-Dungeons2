#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验 FField 反射条目里 Offset_Internal 的真实位置。

用 ATR_Movement 的已知偏移当标尺：MovementSpeedMultiplier=0x90、MovementFriction=0xA0、
MovementFrictionMultiplier=0xB0、MovementRotation=0xC0、MovementRotationMultiplier=0xD0、
MovementGravity=0xE0、GravityScale=0xF0、AirControl=0x100、RollCooldown=0x110、Mass=0x160。
"""

import struct

from peimg import Image

img = Image()
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def slots_for(name_va):
    pat = struct.pack('<Q', name_va)
    out = []
    for s in img.sections:
        if s['name'] not in ('.rdata', '.data', '_RDATA'):
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        st = 0
        while True:
            i = d.find(pat, st)
            if i < 0:
                break
            st = i + 1
            out.append(base + i)
    return out


def main():
    expect = {
        'MovementSpeed': 0x80,
        'MovementSpeedMultiplier': 0x90,
        'MovementFriction': 0xA0,
        'MovementFrictionMultiplier': 0xB0,
        'MovementRotation': 0xC0,
        'MovementRotationMultiplier': 0xD0,
        'MovementGravity': 0xE0,
        'GravityScale': 0xF0,
        'AirControl': 0x100,
        'RollCooldown': 0x110,
        'RollCharges': 0x130,
        'Mass': 0x160,
        'InteractionRange': 0x170,
    }
    # 先找名字串地址
    name_vas = {}
    for key in expect:
        pat = key.encode() + b'\0'
        for s in img.sections:
            img.f.seek(s['rawptr'])
            d = img.f.read(s['rawsize'])
            base = img.image_base + s['vaddr']
            st = 0
            while True:
                i = d.find(pat, st)
                if i < 0:
                    break
                st = i + 1
                va = base + i
                # 只取 0x14a0dxxxx 段附近（ATR_Movement 名字池）
                if 0x14A0D6000 <= va <= 0x14A0D7000:
                    name_vas.setdefault(key, []).append(va)

    for key, exp in expect.items():
        for nva in name_vas.get(key, []):
            for slot in slots_for(nva):
                b = img.read_va(slot - 0x28, 0x28)
                if not b:
                    continue
                vals = struct.unpack_from('<' + 'I' * 10, b, 0)
                hits = ['-%#x' % (0x28 - 4 * i) for i, v in enumerate(vals) if v == exp]
                print('%-30s expect=%#-6x slot=%#x vals=%s  命中字段=%s' %
                      (key, exp, slot, ' '.join('%#x' % v for v in vals), hits or 'NONE'))
    return 0


if __name__ == '__main__':
    main()
