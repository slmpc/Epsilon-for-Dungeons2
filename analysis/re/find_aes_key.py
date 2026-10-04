#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""find_aes_key.py — 在游戏映像里找 IoStore/pak 的 AES 密钥线索。

UE 的密钥来源通常是下面之一:
  * 硬编码在 exe 里的 32 字节数组(FAESKey)
  * 以 64 个十六进制字符的形式出现在 .rdata
  * 运行时由命令行 `-AES=` 传入
  * 由 DefaultEngine.ini / Crypto.json 在打包时读入并嵌进 exe

这里先做只读侦察: 找相关字符串, 并列出「像密钥」的候选。
"""

import re
import sys

from peimg import Image

KEYWORDS = [
    b'AES', b'aes', b'-AES=', b'Crypto.json', b'EncryptionKey', b'Encryption',
    b'FAESKey', b'PakEncryption', b'IoStoreEncryption', b'SigningKey',
    b'DecryptionKey', b'PakSigningKey', b'Encrypt',
]

HEX64 = re.compile(rb'^[0-9A-Fa-f]{64}$')
BASE64ISH = re.compile(rb'^[A-Za-z0-9+/=]{40,50}$')


def main():
    img = Image()
    print('映像 %s' % img.path)
    for s in img.sections:
        print('  %-9s RVA %#010x vsize %#010x rawsize %#010x' %
              (s['name'], s['vaddr'], s['vsize'], s['rawsize']))

    print('\n=== 关键词命中 ===')
    for kw in KEYWORDS:
        hits = []
        for s in img.sections:
            img.f.seek(s['rawptr'])
            data = img.f.read(s['rawsize'])
            i = 0
            while len(hits) < 12:
                i = data.find(kw, i)
                if i < 0:
                    break
                va = img.image_base + s['vaddr'] + i
                # 取上下文
                j = max(0, i - 40)
                ctx = data[j:i + len(kw) + 60]
                txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in ctx)
                hits.append((va, s['name'], txt))
                i += 1
        if hits:
            print('\n  -- %r : %d 处 --' % (kw.decode(), len(hits)))
            for va, sec, txt in hits[:6]:
                print('     %#012x [%s] %s' % (va, sec, txt))

    print('\n=== 形如 64 位十六进制串的候选(可能是密钥) ===')
    n = 0
    for s in img.sections:
        if s['name'] not in ('.rdata', '_RDATA', '.data'):
            continue
        img.f.seek(s['rawptr'])
        data = img.f.read(s['rawsize'])
        base = img.image_base + s['vaddr']
        for mo in re.finditer(rb'[0-9A-Fa-f]{64}(?=\x00)', data):
            va = base + mo.start()
            print('   %#012x  %s' % (va, mo.group().decode()))
            n += 1
            if n > 20:
                break
        if n > 20:
            break
    if n == 0:
        print('   (没有)')
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
