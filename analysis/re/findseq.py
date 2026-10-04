#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 .rdata 里找 ATR_Movement 那一串已知偏移的落点，从而锁定属性表项里偏移字段的位置。

已知（项目 doc 实测）：MovementSpeedMultiplier=0x90、MovementFriction=0xA0、
MovementFrictionMultiplier=0xB0、MovementRotation=0xC0、MovementRotationMultiplier=0xD0、
MovementGravity=0xE0、GravityScale=0xF0、AirControl=0x100、RollCooldown=0x110、
RollCharges=0x130、Mass=0x160、InteractionRange=0x170。
"""

from peimg import Image

img = Image()
SEQ = [0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x100, 0x110]
SEC = '.rdata'


def read_u32(va):
    return img.u32(va)


def main():
    for s in img.sections:
        if s['name'] != SEC:
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        n = len(d)
        step = [0x38, 0x40, 0x44, 0x48, 0x50, 0x20, 0x28, 0x30, 0x18, 0x10]
        for st in step:
            for k, first in enumerate(SEQ):
                if k + 4 > len(SEQ):
                    break
            for start in range(0, n - st * 8, 4):
                if img.u32(base + start) != SEQ[0]:
                    continue
                ok = True
                for j in range(1, len(SEQ)):
                    if img.u32(base + start + j * st) != SEQ[j]:
                        ok = False
                        break
                if ok:
                    print('步长 %#x  起点 %#x  ->  %s' %
                          (st, base + start,
                           [img.u32(base + start + j * st) for j in range(len(SEQ))]))
    return 0


if __name__ == '__main__':
    main()
