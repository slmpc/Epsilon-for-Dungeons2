#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""disas.py — 反汇编原版 exe 里的**明文区**（保护层的控制面就在这里）。

    python disas.py 0x148bfbc8c 60
    python disas.py 0x148bfc8c0 80
"""

import sys

from peimg import Image

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    _md = Cs(CS_ARCH_X86, CS_MODE_64)
    _md.detail = True
except Exception as exc:          # pragma: no cover
    _md = None
    print('// capstone 不可用: %s' % exc)


def main():
    img = Image()
    if _md is None:
        return 1
    ea = int(sys.argv[1], 0)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    buf = img.read_va(ea, n)
    if not buf:
        print('读不到 %#x' % ea)
        return 1
    print('=== %#x (%d 字节) ===' % (ea, len(buf)))
    for ins in _md.disasm(buf, ea):
        note = ''
        m = ins.mnemonic
        if m in ('call', 'jmp') and ins.op_str.startswith('0x'):
            note = '  -> %s' % ins.op_str
        print('  %#012x  %-24s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic + ' ' + ins.op_str))
    return 0


if __name__ == '__main__':
    sys.exit(main())
