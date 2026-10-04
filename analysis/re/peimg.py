#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Dungeons-Win64-Shipping.exe 的 PE 视图 + 名字串扫描（纯标准库）。

给逆向用：把 VA / RVA / 文件偏移互相换算，并在整个映像里做 UTF-8 名字串扫描
（UE 的 FName 字符串池里大写驼峰的名字串是连续的，可以直接切出来）。
"""

import re
import struct
import sys

IMAGE_BASE = 0x140000000
TARGET_EXE = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
              r'\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe')
UNPACKED_EXE = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
                r'\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.unpacked.exe')


class Image(object):
    def __init__(self, path=TARGET_EXE):
        self.path = path
        self.f = open(path, 'rb')
        self.f.seek(0)
        head = self.f.read(0x1000)
        e = struct.unpack_from('<I', head, 0x3c)[0]
        nsec = struct.unpack_from('<H', head, e + 6)[0]
        optsz = struct.unpack_from('<H', head, e + 20)[0]
        opt = e + 24
        self.image_base = struct.unpack_from('<Q', head, opt + 24)[0]
        self.size_image = struct.unpack_from('<I', head, opt + 56)[0]
        self.sections = []
        so = opt + optsz
        for i in range(nsec):
            b = so + i * 40
            nm = head[b:b + 8].rstrip(b'\0').decode('latin1')
            vs, va, rs, rp = struct.unpack_from('<IIII', head, b + 8)
            self.sections.append(dict(name=nm, vsize=vs, vaddr=va,
                                      rawsize=rs, rawptr=rp))

    # ---------------------------------------------------------------- 换算
    def section_at_rva(self, rva):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rawsize']):
                return s
        return None

    def rva_to_off(self, rva):
        s = self.section_at_rva(rva)
        if not s:
            return None
        return s['rawptr'] + (rva - s['vaddr'])

    def off_to_rva(self, off):
        for s in self.sections:
            if s['rawptr'] <= off < s['rawptr'] + s['rawsize']:
                return s['vaddr'] + (off - s['rawptr'])
        return None

    def va_to_off(self, va):
        return self.rva_to_off(va - self.image_base)

    def off_to_va(self, off):
        r = self.off_to_rva(off)
        return None if r is None else self.image_base + r

    def read_va(self, va, n):
        off = self.va_to_off(va)
        if off is None:
            return None
        self.f.seek(off)
        return self.f.read(n)

    def read_rva(self, rva, n):
        off = self.rva_to_off(rva)
        if off is None:
            return None
        self.f.seek(off)
        return self.f.read(n)

    def u64(self, va):
        b = self.read_va(va, 8)
        return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None

    def u32(self, va):
        b = self.read_va(va, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None

    def i32(self, va):
        b = self.read_va(va, 4)
        return struct.unpack('<i', b)[0] if b and len(b) == 4 else None

    def cstr(self, va, maxlen=256):
        b = self.read_va(va, maxlen)
        if not b:
            return None
        i = b.find(b'\0')
        if i < 0:
            return None
        try:
            return b[:i].decode('latin1')
        except Exception:
            return None

    def f32(self, va):
        b = self.read_va(va, 4)
        return struct.unpack('<f', b)[0] if b and len(b) == 4 else None


NAME_RX = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{2,127}')


def scan_names(img, section='.rdata', rx=NAME_RX):
    """在指定节区里切出所有形如标识符的 ASCII 串 -> {name: [va, ...]}。"""
    for s in img.sections:
        if s['name'] == section:
            break
    else:
        raise KeyError(section)
    img.f.seek(s['rawptr'])
    data = img.f.read(s['rawsize'])
    out = {}
    for mo in rx.finditer(data):
        nm = mo.group().decode('latin1')
        va = img.image_base + s['vaddr'] + mo.start()
        out.setdefault(nm, []).append(va)
    return out


def main():
    img = Image()
    print('image base %#x  size %#x' % (img.image_base, img.size_image))
    for s in img.sections:
        print('  %-9s RVA %#010x  raw %#010x  vsize %#010x' %
              (s['name'], s['vaddr'], s['rawptr'], s['vsize']))
    if len(sys.argv) < 2:
        return 0
    pats = sys.argv[1:]
    names = scan_names(img)
    print('\nunique identifier strings in .rdata: %d' % len(names))
    for p in pats:
        sel = sorted(n for n in names if p.lower() in n.lower())
        print('\n== %s (%d)' % (p, len(sel)))
        for n in sel[:400]:
            print('   %-58s %s' % (n, ' '.join(hex(v) for v in names[n][:3])))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
