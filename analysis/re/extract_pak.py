#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract_pak.py — 解析并抽出 pak 内容。

实测结论（本构建 Dungeons-Windows.pak，v11）:

  1. **只有索引被 AES-256-ECB 加密**；数据区是明文。索引密钥见
     docs/reverse/containers.md。
  2. `FullDirectoryIndex` 给的是 `(目录, 文件数, [(文件名, loc)])`，
     `loc` 是 **EncodedPakEntries 里的字节偏移**（不是文件偏移、也不是数组下标）。
  3. `EncodedPakEntries[loc]` 是一个变长条目，实测形态:
         u32 flags          (0xE0C00040 之类)
         u32 pak 文件偏移
         u32 原始大小
         u32 压缩后大小
         u32 (可选, 仅 20 字节条目)
     观测量 12 / 20 字节两种长度。
  4. pak 文件偏移处内联着一个 `FPakEntry` 结构，紧跟数据:
         int64 Offset(内联时为 0) / int64 Size / int64 UncompressedSize
         u32 CompressionMethodIndex (0 = 未压缩)
         u8  Hash[20]
         int32 NumBlocks + NumBlocks*(int64 start, int64 end)
         u8  Flags
         u32 CompressionBlockSize

    python extract_pak.py --key <hex> --scan
    python extract_pak.py --key <hex> --dump <子串> <输出目录>
"""

import argparse
import collections
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
INDEX_OFF = 0xF19FD2A
INDEX_SIZE = 0x1FF60


class Pak:
    def __init__(self, key):
        self.key = key
        self.f = open(PAK, 'rb')
        self.size = os.path.getsize(PAK)
        self._load_index()

    def _dec(self, data):
        n = len(data) - (len(data) % 16)
        d = Cipher(algorithms.AES(self.key), modes.ECB()).decryptor()
        return d.update(data[:n]) + d.finalize()

    def _cstr(self, buf, pos):
        (n,) = struct.unpack_from('<i', buf, pos)
        pos += 4
        s = buf[pos:pos + max(0, n - 1)].decode('latin1', 'replace')
        return s, pos + n

    def _load_index(self):
        self.f.seek(INDEX_OFF)
        buf = self._dec(self.f.read(INDEX_SIZE))
        p = 0
        (mpl,) = struct.unpack_from('<i', buf, p); p += 4
        self.mount = buf[p:p + mpl].split(b'\0')[0].decode('latin1'); p += mpl
        (self.num,) = struct.unpack_from('<i', buf, p); p += 4
        p += 8
        if struct.unpack_from('<i', buf, p)[0]:
            p += 4 + 16 + 20
        else:
            p += 4
        has_fdi = struct.unpack_from('<i', buf, p)[0]; p += 4
        if has_fdi:
            fdi_off, fdi_size = struct.unpack_from('<qq', buf, p); p += 16 + 20
        else:
            raise ValueError('没有 FullDirectoryIndex')
        (ee_size,) = struct.unpack_from('<i', buf, p); p += 4
        self.ee = buf[p:p + ee_size]

        self.f.seek(fdi_off)
        fdi = self._dec(self.f.read(fdi_size))
        q = 0
        (nd,) = struct.unpack_from('<i', fdi, q); q += 4
        self.files = []
        for _ in range(nd):
            dn, q = self._cstr(fdi, q)
            (nf,) = struct.unpack_from('<i', fdi, q); q += 4
            for _ in range(nf):
                fn, q = self._cstr(fdi, q)
                (loc,) = struct.unpack_from('<i', fdi, q); q += 4
                self.files.append((dn + fn, loc))

    def encoded(self, loc):
        if not (0 <= loc <= len(self.ee) - 16):
            return None
        flags, pakoff, usize, csize = struct.unpack_from('<IIII', self.ee, loc)
        return dict(flags=flags, pakoff=pakoff, usize=usize, csize=csize)

    def inline_entry(self, pakoff):
        self.f.seek(pakoff)
        b = self.f.read(0x1000)
        if len(b) < 0x40:
            return None, 0
        p = 0
        ioff, size, usize = struct.unpack_from('<qqq', b, p); p += 24
        (method,) = struct.unpack_from('<I', b, p); p += 4
        # 形态校验: 压缩方法只可能是 0..4, 大小必须是正数且自洽
        if method > 4 or not (0 < size <= self.size) or not (0 < usize <= self.size):
            return None, 0
        sha = b[p:p + 20]; p += 20
        blocks = []
        if method != 0:
            (nb,) = struct.unpack_from('<i', b, p)
            if not (0 < nb <= 256):
                return None, 0
            p += 4
            for _ in range(nb):
                s, en = struct.unpack_from('<qq', b, p)
                blocks.append((s, en)); p += 16
        if p + 5 > len(b):
            return None, 0
        flags = b[p]; p += 1
        (blk,) = struct.unpack_from('<I', b, p); p += 4
        return dict(size=size, usize=usize, method=method, sha=sha,
                    blocks=blocks, flags=flags, block_size=blk), pakoff + p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--key', required=True)
    ap.add_argument('--scan', action='store_true')
    ap.add_argument('--dump', nargs=2, metavar=('SUBSTR', 'OUTDIR'))
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()

    key = bytes.fromhex(a.key[2:] if a.key.lower().startswith('0x') else a.key)
    pk = Pak(key)
    print('挂载点 %r   条目 %d   EncodedPakEntries %d 字节'
          % (pk.mount, len(pk.files), len(pk.ee)))

    methods = collections.Counter()
    stored = []
    rows = []
    mismatch = 0
    for path, loc in pk.files:
        e = pk.encoded(loc)
        if not e or not (0 < e['pakoff'] < pk.size):
            methods['坏条目'] += 1
            continue
        info, data_off = pk.inline_entry(e['pakoff'])
        if not info:
            methods['读不到'] += 1
            continue
        # 校验: 内联头的大小必须与编码条目给的一致, 否则说明这条 encoded
        # 记录是 12 字节变体、字段位置不同 —— 不能信。
        if info['size'] != e['csize']:
            mismatch += 1
            continue
        methods['method=%d' % info['method']] += 1
        rows.append((path, e, info, data_off))
        if info['method'] == 0:
            stored.append((path, e, info, data_off))

    print('\n大小校验: %d 条不一致(疑为 12 字节变体的 encoded 记录)' % mismatch)

    print('\n压缩方法分布:')
    for k, c in sorted(methods.items()):
        print('   %-12s %d' % (k, c))

    rows.sort(key=lambda r: -r[2]['usize'])
    print('\n最大的 10 个:')
    for path, e, info, _ in rows[:10]:
        print('   %10d -> %-9d method=%d @%#x  %s'
              % (info['usize'], info['size'], info['method'], e['pakoff'], path[:52]))

    print('\n未压缩条目(method=0): %d 个' % len(stored))
    for path, e, info, _ in stored[:15]:
        print('   %9d  %s' % (info['usize'], path[:64]))

    if a.dump:
        sub, outdir = a.dump
        os.makedirs(outdir, exist_ok=True)
        n = 0
        for path, e, info, data_off in stored:
            if sub and sub not in path:
                continue
            pk.f.seek(data_off)
            data = pk.f.read(info['size'])
            dst = os.path.join(outdir, os.path.basename(path))
            with open(dst, 'wb') as w:
                w.write(data)
            print('   抽出 %s (%d 字节, usize=%d)' % (dst, len(data), info['usize']))
            n += 1
            if a.limit and n >= a.limit:
                break
        if n == 0:
            print('   (没有匹配的未压缩条目)')
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
