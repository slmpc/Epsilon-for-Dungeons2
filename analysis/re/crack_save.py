#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""crack_save.py — 破解 GlobalSaveDataDefault.sav 的字符级混淆。

特征: 全文 100% 可打印、熵 4.69、magic 是 `z!aknar!`。
这种"全可打印 + 固定分隔符"通常是单字节 XOR 或字符移位。逐一试并给评分。
"""

import os
import re
import sys

SAVE = (r'C:\Users\miaoy\AppData\Local\Dungeons2\Saved\SaveGames'
        r'\GlobalSaveDataDefault.sav')

# 明文特征: UE GVAS / JSON / 属性名
MARKERS = [b'{', b'}', b'"', b':', b'None', b'True', b'False', b'GVAS',
           b'Player', b'Save', b'Version', b'Emerald']


def score(b):
    s = 0
    for m in MARKERS:
        s += b.count(m) * (3 if len(m) > 1 else 1)
    # 可打印率惩罚
    pr = sum(1 for c in b if 32 <= c < 127) / max(1, len(b))
    return s + pr * 20


def try_xor(data, key):
    return bytes(c ^ key for c in data)


def try_add(data, k):
    return bytes((c + k) & 0xFF for c in data)


def try_sub(data, k):
    return bytes((c - k) & 0xFF for c in data)


def main():
    data = open(SAVE, 'rb').read()
    print('文件 %d 字节, 头 64: %r' % (len(data), data[:64]))

    best = []
    for k in range(1, 256):
        for name, fn in (('xor', try_xor), ('add', try_add), ('sub', try_sub)):
            out = fn(data, k)
            best.append((score(out), name, k, out))
    best.sort(key=lambda x: -x[0])

    print('\n=== 评分最高的前 6 种变换 ===')
    for sc, name, k, out in best[:6]:
        print('  %-4s key=%3d  score=%.1f  head=%r' % (name, k, sc, out[:56]))

    # 展开最好的那个
    sc, name, k, out = best[0]
    print('\n=== 最优解 (%s key=%d) 前 1200 字节 ===' % (name, k))
    txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in out[:1200])
    for i in range(0, len(txt), 100):
        print('  ' + txt[i:i + 100])

    print('\n=== 明文里的可读标识符(长度>=5) ===')
    ids = sorted(set(m.group().decode() for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_]{4,40}', out)))
    print('  共 %d 个, 前 40:' % len(ids))
    for x in ids[:40]:
        print('    %s' % x)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
