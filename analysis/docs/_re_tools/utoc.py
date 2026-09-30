#!/usr/bin/env python3
"""Parse Unreal Engine IoStore .utoc headers to determine container version + layout."""
import struct, sys, os

def rd(f, n):
    b = f.read(n)
    if len(b) < n:
        raise EOFError(f"want {n} got {len(b)} at {f.tell()-len(b)}")
    return b

def u8(f):  return struct.unpack('<B', rd(f,1))[0]
def u32(f): return struct.unpack('<I', rd(f,4))[0]
def i32(f): return struct.unpack('<i', rd(f,4))[0]
def u64(f): return struct.unpack('<Q', rd(f,8))[0]

def parse(path):
    print(f"\n{'='*78}\nFILE: {path}\nSIZE: {os.path.getsize(path):,} bytes\n{'='*78}")
    with open(path,'rb') as f:
        magic = rd(f,16)
        print(f"magic        : {magic!r}")
        if magic != b'-==--==--==--==-':
            print("  !! not an IoStore TOC magic"); return
        ver = u8(f)
        print(f"version      : {ver}  (UE5: 4=5.0-5.1, 5=5.2-5.3, 6=5.4+, 7/8=5.5+)")
        # UE5 iostore: version byte, then header size + entries per block as u32 pairs
        hdr_size = u32(f)
        toc_entries_per_block = u32(f)
        print(f"headerSize   : {hdr_size}")
        print(f"entriesPerBlk: {toc_entries_per_block}")

        # TOC: compressed block refs -> then the actual entries
        def read_block_ref():
            return (u32(f), u32(f), u64(f), u64(f))  # offset,size,uncompSize,? (varies)
        # Try to read compressed block table for TOC entries
        pos = f.tell()
        print(f"\n-- raw bytes at 0x{pos:X} (next 96) --")
        raw = rd(f, 96)
        print('   ' + ' '.join(f'{b:02X}' for b in raw[:48]))
        print('   ' + ' '.join(f'{b:02X}' for b in raw[48:]))
        print('   ascii: ' + ''.join(chr(b) if 32<=b<127 else '.' for b in raw))
        f.seek(pos)

if __name__ == '__main__':
    for p in sys.argv[1:]:
        try:
            parse(p)
        except Exception as e:
            print(f"ERROR on {p}: {type(e).__name__}: {e}")
