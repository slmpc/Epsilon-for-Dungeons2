#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""protect_probe.py — 在原版(带保护)exe 的**明文区**里找自检/终止逻辑的线索。

理由: 保护的第一段代码(解密器 / 自检)必须先于被加密的代码运行, 所以它**只能是明文**。
本脚本做四件事:
  1. 列出各节区熵, 确认哪些区域是明文
  2. 找保护/反调试/校验相关的字符串
  3. 找敏感 API 的名字(导入表 + 字符串)
  4. 找 TLS 回调与入口点(它们是明文控制流的起点)
"""

import math
import re
import struct
import sys

from peimg import Image

ORIG = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
        r'\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe')

KEYWORDS = [
    'IsDebuggerPresent', 'CheckRemoteDebuggerPresent', 'NtQueryInformationProcess',
    'NtSetInformationThread', 'OutputDebugString', 'TerminateProcess', 'ExitProcess',
    'LdrEnumerateLoadedModules', 'EnumProcessModules', 'CreateToolhelp32Snapshot',
    'VirtualProtect', 'VirtualQuery', 'LoadLibrary', 'GetProcAddress',
    'DebugActiveProcess', 'NtClose', 'ZwQuerySystemInformation',
    'integrity', 'Integrity', 'tamper', 'Tamper', 'debugger', 'Debugger',
    'VMProtect', 'Themida', 'Arxan', 'Denuvo', 'EasyAntiCheat', 'BattlEye',
    'obfuscat', 'Obfuscat', 'checksum', 'Checksum', 'selfcheck', 'SelfCheck',
    'xbox', 'Xbox', 'XGame', 'GDK', 'gamingruntime',
]


def entropy(b):
    if not b:
        return 0.0
    c = [0] * 256
    for x in b:
        c[x] += 1
    return -sum((n / len(b)) * math.log2(n / len(b)) for n in c if n)


def main():
    img = Image(ORIG)
    print('=== 节区熵（明文代码约 6.2~6.8，加密约 8.0）===')
    for s in img.sections:
        img.f.seek(s['rawptr'])
        d = img.f.read(min(s['rawsize'], 4 << 20))
        print('   %-9s rawsize %#010x  前4MiB熵 %.4f' % (s['name'], s['rawsize'], entropy(d)))

    print('\n=== 敏感关键字在映像里的出现（限 .rdata/_RDATA/.data，排除 .text 随机噪声）===')
    for kw in KEYWORDS:
        pat = kw.encode()
        hits = []
        for s in img.sections:
            if s['name'] not in ('.rdata', '_RDATA', '.data'):
                continue
            img.f.seek(s['rawptr'])
            d = img.f.read(s['rawsize'])
            i = 0
            while len(hits) < 5:
                i = d.find(pat, i)
                if i < 0:
                    break
                hits.append(img.image_base + s['vaddr'] + i)
                i += 1
            if len(hits) >= 5:
                break
        if hits:
            print('   %-30s %d 处 %s' % (kw, len(hits), [hex(h) for h in hits[:4]]))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
