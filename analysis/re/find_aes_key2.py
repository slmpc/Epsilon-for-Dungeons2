#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""find_aes_key2.py — 继续找 IoStore 密钥线索（UTF-16 与命令行开关）。"""

import re
import sys

from peimg import Image


def main():
    img = Image()
    print('映像 %s' % img.path)

    print('\n=== UTF-16LE 关键词 ===')
    for kw in ('AES=', 'Crypto', 'EncryptionKey', 'PakSigning', 'SigningKey',
               'DecryptionKey', 'key=', 'KEY='):
        u = kw.encode('utf-16-le')
        n = 0
        for s in img.sections:
            img.f.seek(s['rawptr'])
            d = img.f.read(s['rawsize'])
            i = 0
            while n < 5:
                i = d.find(u, i)
                if i < 0:
                    break
                print('   %-16s %#012x [%s]' % (kw, img.image_base + s['vaddr'] + i, s['name']))
                n += 1
                i += 1
        if n == 0:
            print('   %-16s (无)' % kw)

    print('\n=== UTF-16 里以 - 开头、且提到 AES/KEY 的开关 ===')
    found = 0
    for s in img.sections:
        if s['name'] not in ('.rdata', '_RDATA'):
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        for mo in re.finditer(rb'(?:[\x20-\x7e]\x00){3,40}', d):
            t = mo.group().decode('utf-16-le')
            if t.startswith('-') and re.search(r'AES|CRYPTO|KEY', t, re.I):
                print('   %#012x %r' % (img.image_base + s['vaddr'] + mo.start(), t))
                found += 1
    if not found:
        print('   (无)')

    print('\n=== ASCII 里以 - 开头、且提到 AES/KEY 的开关 ===')
    found = 0
    for s in img.sections:
        if s['name'] not in ('.rdata', '_RDATA'):
            continue
        img.f.seek(s['rawptr'])
        d = img.f.read(s['rawsize'])
        for mo in re.finditer(rb'^-[A-Za-z][A-Za-z0-9_]{1,24}$', d, re.M):
            t = mo.group().decode()
            if re.search(r'AES|CRYPTO|KEY', t, re.I):
                print('   %#012x %r' % (img.image_base + s['vaddr'] + mo.start(), t))
                found += 1
    if not found:
        print('   (无)')
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
