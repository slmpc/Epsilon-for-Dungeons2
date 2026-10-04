#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_layout.py — 判定属性表里 Offset_Internal 到底在记录的哪个位置。

用两组样本对撞:
  A. ATR_Movement —— 已知真值(来自运行时 CDO 的 float 对):
       slot+0x08 = 700.0  -> MovementSpeed        (基础速度, 700 是速度量级)
       slot+0x08 = 1.2    -> GravityScale         (重力倍率默认 1.2)
  B. ATR_Currency —— 已知真值:
       slot+0x08 = 2.0    -> MaxAdditionalEmeralds

把每个属性名的两个候选位置都读出来，谁的取值能和上面的真值对齐，谁就是对的。
"""

import struct

from peimg import Image

img = Image()
RD_LO, RD_HI = 0x148E33600, 0x14BA78000


def u32(va):
    return img.u32(va) or 0


def name_at(va):
    if not (RD_LO <= va < RD_HI):
        return None
    s = img.cstr(va, 96)
    if s and len(s) >= 2 and all(32 <= ord(c) < 127 for c in s):
        return s
    return None


# (类, 名字串 VA, 名字, 运行时观测到的 slot+8 的值)
SAMPLES = [
    ('ATR_Movement', 0x14A0D6830, 'MovementSpeed', 700.0),
    ('ATR_Movement', 0x14A0D6870, 'MovementSpeedMultiplier', 1.0),
    ('ATR_Movement', 0x14A0D68B0, 'MovementFriction', 1.0),
    ('ATR_Movement', 0x14A0D68F0, 'MovementFrictionMultiplier', 1.0),
    ('ATR_Movement', 0x14A0D6930, 'MovementRotation', 1.0),
    ('ATR_Movement', 0x14A0D6970, 'MovementRotationMultiplier', 1.0),
    ('ATR_Movement', 0x14A0D69B0, 'MovementGravity', 1.0),
    ('ATR_Movement', 0x14A0D69F0, 'GravityScale', 1.2),
    ('ATR_Currency', 0x14A0D00C0, 'Emeralds', 0.0),
    ('ATR_Currency', 0x14A0D0100, 'EmeraldsMax', 0.0),
    ('ATR_Currency', 0x14A0D0140, 'EmeraldsMin', 0.0),
    ('ATR_Currency', 0x14A0D0180, 'EmeraldIncreasePercentage', 0.0),
    ('ATR_Currency', 0x14A0D01C0, 'EmeraldDropChanceIncrease', 0.0),
    ('ATR_Currency', 0x14A0D0200, 'MaxAdditionalEmeralds', 2.0),
    ('ATR_Currency', 0x14A0D0240, 'EmeraldCapForDamageIncrease', 0.0),
]


def main():
    print('%-12s %-30s %-8s %-10s %-10s' %
          ('类', '名字(按 +0x34 规则)', 'off', '-0x0C', '+0x34'))
    print('-' * 78)
    for cls, slot, expect_name, expect_val in SAMPLES:
        got = name_at(u32(slot))
        off_minus = u32(slot - 0x0C)
        off_plus = u32(slot + 0x34)
        print('%-12s %-30s          %#-10x %#-10x' % (cls, got or '?', off_minus, off_plus))
    print()
    print('说明: 名字指针在记录 +0x00 的读法是 +0x34; 在记录 +0x38 的读法是 -0x0C。')
    print('用运行时真值判定: MovementSpeed 应得一个「速度量级(700)」的槽，')
    print('                  GravityScale 应得 1.2 的槽，MaxAdditionalEmeralds 应得 2.0 的槽。')
    print()
    print('=== 按 +0x34 规则展开 ===')
    for cls, slot, expect_name, expect_val in SAMPLES:
        off = u32(slot + 0x34)
        print('  %-12s %-30s -> 声明 %#-8x  数据 %#-8x  (期望 slot+8 = %.1f)'
              % (cls, name_at(u32(slot)) or '?', off, off + 8, expect_val))
    print()
    print('=== 按 -0x0C 规则展开 ===')
    for cls, slot, expect_name, expect_val in SAMPLES:
        off = u32(slot - 0x0C)
        print('  %-12s %-30s -> 声明 %#-8x  数据 %#-8x  (期望 slot+8 = %.1f)'
              % (cls, name_at(u32(slot)) or '?', off, off + 8, expect_val))


if __name__ == '__main__':
    main()
