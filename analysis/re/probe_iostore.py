#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_iostore.py — 判断 Minecraft Dungeons II 的容器能不能直接读。

回答三个问题：
  1. 用了什么压缩方法（IoStore 把方法名以 ASCII 存在 TOC 里）
  2. 数据是否加密（抽样看 .ucas 里有没有可读的 UE 内容）
  3. 传统 .pak 在哪、索引是否可解析
"""

import os
import re
import struct
import sys

PAKS = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
        r'\Dungeons\Content\Paks')

MAGIC_TOC = b'-==--==--==--==-'
PAK_MAGIC = 0x5A6F12E1


def probe_utoc(path):
    print('=' * 78)
    print('UTOC %s  (%s bytes)' % (os.path.basename(path), f'{os.path.getsize(path):,}'))
    with open(path, 'rb') as f:
        head = f.read(0x90)

    (ver,) = struct.unpack_from('<B', head, 0x10)
    (hdr_size, entry_count, blk_count, blk_size, method_count, method_len,
     comp_block) = struct.unpack_from('<7I', head, 0x14)
    (dir_index_size, contained) = struct.unpack_from('<2I', head, 0x30)
    print('  version=%d headerSize=%#x entries=%d blocks=%d entrySize=%d' %
          (ver, hdr_size, entry_count, blk_count, blk_size))
    print('  compressionMethods=%d nameLen=%d blockSize=%d' %
          (method_count, method_len, comp_block))
    print('  directoryIndexSize=%d containedChunks=%d' % (dir_index_size, contained))

    # 压缩方法名紧跟压缩块表之后
    name_off = hdr_size + blk_count * blk_size
    if method_count:
        with open(path, 'rb') as f:
            f.seek(name_off)
            names = f.read(method_count * method_len)
        print('  方法名区 @%#x:' % name_off)
        for i in range(method_count):
            chunk = names[i * method_len:(i + 1) * method_len]
            print('     [%d] %r' % (i, chunk.rstrip(b'\0')))
    else:
        print('  方法名区: 空 -> 该容器未压缩')

    # 全文找已知压缩器名字，做交叉印证
    with open(path, 'rb') as f:
        blob = f.read(min(os.path.getsize(path), 0x400000))
    print('  容器内出现的压缩器名: %s' %
          [n for n in ('Oodle', 'Zlib', 'LZ4', 'Gzip', 'Brotli') if n.encode() in blob])


def probe_ucas(path, n=4 << 20):
    print('=' * 78)
    size = os.path.getsize(path)
    print('UCAS %s  (%s bytes)  抽头 %s' % (os.path.basename(path), f'{size:,}', f'{n:,}'))
    with open(path, 'rb') as f:
        blob = f.read(n)
    strs = [m.group().decode('latin1') for m in
            re.finditer(rb'[ -~]{8,60}', blob)]
    print('  前 %d 字节里长度>=8 的可读串: %d 条' % (n, len(strs)))
    for s in strs[:18]:
        print('     %r' % s)


def probe_pak(path):
    print('=' * 78)
    size = os.path.getsize(path)
    print('PAK  %s  (%s bytes)' % (os.path.basename(path), f'{size:,}'))
    with open(path, 'rb') as f:
        tail = f.read()
        # 在最后 1 MiB 里找 UE4 pak 魔数
        seg = tail[-1 << 20:]
        hits = []
        i = 0
        while True:
            i = seg.find(struct.pack('<I', PAK_MAGIC), i)
            if i < 0:
                break
            hits.append(len(tail) - len(seg) + i)
            i += 1
        print('  尾部 1MiB 内 pak 魔数命中: %s' % [hex(h) for h in hits[:6]])
    with open(path, 'rb') as f:
        head = f.read(0x100)
    strs = [m.group().decode('latin1') for m in re.finditer(rb'[ -~]{6,50}', head)]
    print('  头部可读串: %s' % strs[:8])


def main():
    for n in sorted(os.listdir(PAKS)):
        p = os.path.join(PAKS, n)
        if n.endswith('.utoc'):
            probe_utoc(p)
        elif n.endswith('.ucas'):
            probe_ucas(p)
        elif n.endswith('.pak'):
            probe_pak(p)
    return 0


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
