#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按标定出的 FField 布局（Offset 在名字槽 -0xC）解出属性的 Offset_Internal。

    python fieldmap.py EmeraldAmount Emeralds ...
    python fieldmap.py --dump EmeraldAmount      # 打印记录原始字节
"""

import struct
import sys

from peimg import Image

img = Image()
RD_LO, RD_HI = 0x148E33600, 0x14BA78000
SCAN = ('.rdata', '.data', '_RDATA')


def slots_for_va(name_va):
    pat = struct.pack('<Q', name_va)
    out = []
    for s in img.sections:
        if s['name'] not in SCAN:
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


def find_names(name):
    pat = name.encode('ascii') + b'\0'
    out = []
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
            if RD_LO <= va < RD_HI:
                out.append(va)
    return out


def decode(slot):
    b = img.read_va(slot - 0x14, 0x18)
    if not b or len(b) < 0x18:
        return None
    thash, name_len, marker, off_int = struct.unpack_from('<IIII', b, 0)
    return dict(hash=thash, name_len=name_len, marker=marker, off=off_int)


def main():
    args = sys.argv[1:]
    dumpmode = '--dump' in args
    names = [a for a in args if not a.startswith('--')]
    for name in names:
        vas = find_names(name)
        if not vas:
            print('%-42s (名字串未找到)' % name)
            continue
        for nva in vas:
            for slot in slots_for_va(nva):
                if not (0x14A000000 <= slot or 0x148E00000 <= slot):
                    continue
                d = decode(slot)
                if d is None:
                    continue
                ok = (d['marker'] == 0x45 and d['name_len'] == len(name))
                tag = 'FField' if ok else '?'
                print('%-42s %s  off=%#-8x slot=%#012x len=%d marker=%#x hash=%#010x' %
                      (name, tag, d['off'], slot, d['name_len'], d['marker'], d['hash']))
                if dumpmode:
                    b = img.read_va(slot - 0x20, 0x20)
                    for off in range(0, len(b), 16):
                        c = b[off:off + 16]
                        print('      %012x  %-47s  %s' %
                              (slot - 0x20 + off, ' '.join('%02x' % x for x in c),
                               ''.join(chr(x) if 32 <= x < 127 else '.' for x in c)))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
