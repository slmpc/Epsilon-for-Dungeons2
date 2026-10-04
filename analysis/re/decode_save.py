#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""decode_save.py — 解开 GlobalSaveDataDefault.sav。

实测混淆方式: **写入时每个字节减 1**, 所以读回来每个字节加 1 就是明文 JSON。

    python decode_save.py            # 摘要 + 顶层键
    python decode_save.py --dump out.json
"""

import json
import os
import sys

SAVES = r'C:\Users\miaoy\AppData\Local\Dungeons2\Saved\SaveGames'


def decode(path):
    raw = open(path, 'rb').read()
    return bytes((c + 1) & 0xFF for c in raw)


def main():
    src = os.path.join(SAVES, 'GlobalSaveDataDefault.sav')
    plain = decode(src)
    print('解码后前 200 字节:')
    print('  ' + plain[:200].decode('utf-8', 'replace').replace('\n', ' '))

    try:
        doc = json.loads(plain.decode('utf-8'))
    except Exception as e:
        print('JSON 解析失败: %s' % e)
        return 1

    print('\n顶层键: %s' % list(doc.keys()))
    for k, v in doc.items():
        if isinstance(v, dict):
            print('  %s: dict, %d 个子键' % (k, len(v)))
        elif isinstance(v, list):
            print('  %s: list, %d 项' % (k, len(v)))
        else:
            print('  %s: %r' % (k, v))

    blobs = doc.get('blobs')
    if isinstance(blobs, list):
        print('\nblobs[0] 的键:')
        for b in blobs[:3]:
            if isinstance(b, dict):
                print('  %s' % list(b.keys())[:20])
                for kk, vv in list(b.items())[:25]:
                    s = json.dumps(vv, ensure_ascii=False)
                    print('     %-34s %s' % (kk, s[:90]))

    if len(sys.argv) > 2 and sys.argv[1] == '--dump':
        with open(sys.argv[2], 'w', encoding='utf-8') as f:
            json.dump(doc, f, indent=2, ensure_ascii=False)
        print('\n已写出 %s' % sys.argv[2])
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
