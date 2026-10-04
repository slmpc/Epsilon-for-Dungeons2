#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描 .rdata 里 0x40 步长的属性表项，自动判定 8 字节槽里哪些是 VA、哪些是 RVA。

本构建实测：表项里的两个函数指针槽是 **RVA**（不是 VA），所以直接按 8 字节读会读到
两条 RVA 拼起来的垃圾。脚本把每个槽同时按 VA 与 RVA 解释，并给出判定结论。

    python table.py --scan EmeraldAmount
    python table.py --entry 0x149f2ce40
"""

import struct
import sys

from peimg import Image

STRIDE = 0x40


class View(object):
    def __init__(self, img):
        self.img = img
        self.text_lo = self.text_hi = None
        for s in img.sections:
            if s['name'] == '.text':
                self.text_lo = img.image_base + s['vaddr']
                self.text_hi = self.text_lo + s['vsize']
        self.sections = [(s['name'], img.image_base + s['vaddr'], s['rawsize'],
                          s['rawptr']) for s in img.sections]

    def is_code(self, va):
        return self.text_lo <= va < self.text_hi

    def is_rva_code(self, rva):
        return self.text_lo - self.img.image_base <= rva < self.text_hi - self.img.image_base

    def is_va_data(self, va):
        return 0x140000000 <= va < 0x14c9bb000

    def is_rva_data(self, rva):
        return 0x1000 <= rva < 0xcd94000

    def cstr_va(self, va, n=64):
        if not self.is_va_data(va):
            return None
        return self.img.cstr(va, n)

    def cstr_rva(self, rva, n=64):
        if not self.is_rva_data(rva):
            return None
        return self.img.cstr(self.img.image_base + rva, n)


def scan_table_for_name(img, name_va):
    """找到引用 name_va 的 8 字节槽 -> 推断表项起点候选。"""
    v = View(img)
    pat = struct.pack('<Q', name_va)
    res = []
    for nm, base, rawsize, rawptr in v.sections:
        if nm not in ('.rdata', '.data', '_RDATA'):
            continue
        img.f.seek(rawptr)
        data = img.f.read(rawsize)
        st = 0
        while True:
            i = data.find(pat, st)
            if i < 0:
                break
            st = i + 1
            va = base + i
            # 假定它是表项里的第 k 个 8 字节槽
            for k in range(0, 6):
                ent = va - k * 8
                res.append((ent, k))
    return res


def describe_entry(img, ent):
    v = View(img)
    b = img.read_va(ent, STRIDE)
    if not b or len(b) != STRIDE:
        return
    print('  entry %#012x' % ent)
    for off in range(0, STRIDE, 8):
        q = struct.unpack_from('<Q', b, off)[0]
        lo = struct.unpack_from('<I', b, off)[0]
        hi = struct.unpack_from('<I', b, off + 4)[0]
        note = []
        if v.is_code(q):
            note.append('CODE_VA %#x' % q)
        if v.is_rva_code(lo):
            note.append('CODE_RVA %#x' % lo)
        s = v.cstr_va(q, 48)
        if s and s.isprintable() and len(s) > 1:
            note.append('STR %r' % s)
        s2 = v.cstr_rva(lo, 48)
        if s2 and s2.isprintable() and len(s2) > 1:
            note.append('STRrva %r' % s2)
        if v.is_va_data(q):
            note.append('DATA_VA')
        if v.is_rva_data(lo) and not note:
            note.append('maybe RVA %#x' % lo)
        print('    +%02x  %016x  %-18s %s' % (off, q,
              '%08x %08x' % (hi, lo), ' | '.join(note)))


def main():
    img = Image()
    if len(sys.argv) >= 3 and sys.argv[1] == '--scan':
        for nm in sys.argv[2:]:
            pat = nm.encode() + b'\0'
            for s in img.sections:
                img.f.seek(s['rawptr'])
                data = img.f.read(s['rawsize'])
                base = img.image_base + s['vaddr']
                st = 0
                while True:
                    i = data.find(pat, st)
                    if i < 0:
                        break
                    st = i + 1
                    nva = base + i
                    print('==== %s @ %#x' % (nm, nva))
                    for ent, k in scan_table_for_name(img, nva):
                        print('  (name is slot +%#x of entry %#x)' % (k * 8, ent))
                        describe_entry(img, ent)
        return 0
    if len(sys.argv) >= 3 and sys.argv[1] == '--entry':
        for a in sys.argv[2:]:
            describe_entry(img, int(a, 0))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
