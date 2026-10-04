#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""unpak.py — 解析 Dungeons-Windows.pak 的索引并列出全部资源。

密钥来源: 用户提供，已用 AES-256-ECB 解出合法索引结构验证通过。
UE 的 FAES 用 AES-256 **ECB**（无 IV），所以逐块独立解密即可。

索引结构（PakFile_Version_PathHashIndex = 10 及以上，本构建 v11）:

    int32   MountPointLength
    char    MountPoint[]
    int32   NumEntries
    uint64  PathHashSeed
    int32   bReaderHasPathHashIndex
    int64   PathHashIndexOffset
    int64   PathHashIndexSize
    uint8   PathHashIndexHash[20]
    int32   bReaderHasFullDirectoryIndex
    int64   FullDirectoryIndexOffset
    int64   FullDirectoryIndexSize
    uint8   FullDirectoryIndexHash[20]
    int32   EncodedPakEntriesSize
    uint8   EncodedPakEntries[]

FullDirectoryIndex 用 (目录名, 文件数, [(文件名, 编码条目下标)]) 的形式列出全部路径。

    python unpak.py --key <hex>            # 列文件
    python unpak.py --key <hex> --stat     # 统计
    python unpak.py --key <hex> --extract <子串> <输出目录>
"""

import argparse
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAK = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
       r'\Dungeons\Content\Paks\Dungeons-Windows.pak')
PAK_MAGIC = 0x5A6F12E1


class Pak:
    def __init__(self, path, key):
        self.path = path
        self.key = key
        self.size = os.path.getsize(path)
        self._read_info()
        self._read_index()

    # ---------------------------------------------------------------- AES
    def dec(self, data):
        n = len(data) - (len(data) % 16)
        d = Cipher(algorithms.AES(self.key), modes.ECB()).decryptor()
        return d.update(data[:n]) + d.finalize()

    def read_at(self, off, n):
        with open(self.path, 'rb') as f:
            f.seek(off)
            return f.read(n)

    # ---------------------------------------------------------------- footer
    def _read_info(self):
        tail = self.read_at(max(0, self.size - (1 << 20)), 1 << 20)
        base = max(0, self.size - (1 << 20))
        i = tail.rfind(struct.pack('<I', PAK_MAGIC))
        if i < 0:
            raise ValueError('找不到 pak 尾部魔数')
        off = base + i
        blk = self.read_at(off, 0x60)
        self.version = struct.unpack_from('<i', blk, 4)[0]
        self.index_offset, self.index_size = struct.unpack_from('<qq', blk, 8)
        self.index_hash = blk[0x18:0x2C]
        self.method = blk[0x2C:0x2C + 32].split(b'\0')[0].decode('latin1')

    # ---------------------------------------------------------------- index
    def _cstr(self, buf, pos):
        (n,) = struct.unpack_from('<i', buf, pos)
        pos += 4
        s = buf[pos:pos + max(0, n - 1)].decode('latin1', 'replace')
        return s, pos + n

    def _read_index(self):
        raw = self.read_at(self.index_offset, self.index_size)
        buf = self.dec(raw)
        pos = 0
        mp_len = struct.unpack_from('<i', buf, pos)[0]
        pos += 4
        self.mount = buf[pos:pos + mp_len].split(b'\0')[0].decode('latin1')
        pos += mp_len
        self.num_entries = struct.unpack_from('<i', buf, pos)[0]
        pos += 4
        self.path_hash_seed = struct.unpack_from('<Q', buf, pos)[0]
        pos += 8

        has_phi = struct.unpack_from('<i', buf, pos)[0]
        pos += 4
        self.phi_off = self.phi_size = 0
        if has_phi:
            self.phi_off, self.phi_size = struct.unpack_from('<qq', buf, pos)
            pos += 16 + 20
        has_fdi = struct.unpack_from('<i', buf, pos)[0]
        pos += 4
        self.fdi_off = self.fdi_size = 0
        if has_fdi:
            self.fdi_off, self.fdi_size = struct.unpack_from('<qq', buf, pos)
            pos += 16 + 20
        self.enc_entries_size = struct.unpack_from('<i', buf, pos)[0]
        pos += 4
        self.enc_entries = buf[pos:pos + self.enc_entries_size]

    # ---------------------------------------------------- full directory index
    def files(self):
        raw = self.read_at(self.fdi_off, self.fdi_size)
        buf = self.dec(raw)
        pos = 0
        (num_dirs,) = struct.unpack_from('<i', buf, pos)
        pos += 4
        out = []
        for _ in range(num_dirs):
            d, pos = self._cstr(buf, pos)
            (num_files,) = struct.unpack_from('<i', buf, pos)
            pos += 4
            for _ in range(num_files):
                fn, pos = self._cstr(buf, pos)
                (entry_idx,) = struct.unpack_from('<i', buf, pos)
                pos += 4
                out.append((d + fn, entry_idx))
            if pos > len(buf):
                break
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--key', required=True)
    ap.add_argument('--pak', default=PAK)
    ap.add_argument('--stat', action='store_true')
    ap.add_argument('--extract')
    ap.add_argument('--out', default='unpak_out')
    ap.add_argument('--limit', type=int, default=60)
    a = ap.parse_args()

    key = bytes.fromhex(a.key[2:] if a.key.lower().startswith('0x') else a.key)
    pk = Pak(a.pak, key)

    print('pak          : %s (%d 字节)' % (os.path.basename(pk.path), pk.size))
    print('version      : %d' % pk.version)
    print('压缩方法     : %s' % pk.method)
    print('挂载点       : %r' % pk.mount)
    print('索引         : off=%#x size=%#x' % (pk.index_offset, pk.index_size))
    print('NumEntries   : %d' % pk.num_entries)
    print('PathHashIdx  : off=%#x size=%#x' % (pk.phi_off, pk.phi_size))
    print('FullDirIdx   : off=%#x size=%#x' % (pk.fdi_off, pk.fdi_size))
    print('EncodedEntry : %d 字节' % pk.enc_entries_size)

    fl = pk.files()
    print('\n目录索引解出 %d 个文件' % len(fl))
    for p, idx in fl[:a.limit]:
        print('   [%6d] %s' % (idx, p))

    if a.stat:
        import collections
        ext = collections.Counter(os.path.splitext(p)[1] for p, _ in fl)
        print('\n扩展名分布:')
        for e, c in ext.most_common(20):
            print('   %-10s %d' % (e or '(无)', c))
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
