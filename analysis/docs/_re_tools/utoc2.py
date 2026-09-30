#!/usr/bin/env python3
"""
Correct IoStore TOC header parser.
Layout after the 16-byte magic + 1-byte version (UE5 ContainerHeaderVersion 8):
  u32 HeaderSize
  u32 TocEntriesPerBlock
  u32 TocCompressedBlockCount
  u32 TocCompressedBlockSize
  u32 TocUncompressedBlockSize   (v8)  |  u32 TocCompressionMethod (<=v7)
  u32 ContainerCount
  u32 ContainerCompressedBlockCount
  u32 ContainerCompressedBlockSize
  u32 ContainerUncompressedBlockSize
  u32 ContainerCompressionMethod
  u64 ContainerCompressionBlockSize
  (v8) u32 TocCompressionMethod
  FIoStoreTocCompressedBlockRef[TocCompressedBlockCount]  (5 * u32 = 20 bytes each)
  ...
"""
import struct, sys, os, json

MAGIC = b'-==--==--==--==-'

class R:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.path = path
        self.size = os.path.getsize(path)
    def u8(self):  return struct.unpack('<B', self.f.read(1))[0]
    def u32(self): return struct.unpack('<I', self.f.read(4))[0]
    def i32(self): return struct.unpack('<i', self.f.read(4))[0]
    def u64(self): return struct.unpack('<Q', self.f.read(8))[0]
    def raw(self,n): return self.f.read(n)
    def tell(self): return self.f.tell()
    def seek(self,p): self.f.seek(p)

def parse(path, dump_asm=False):
    r = R(path)
    print(f"\n{'='*80}\n{os.path.basename(path)}   size={r.size:,}\n{'='*80}")
    magic = r.raw(16)
    if magic != MAGIC:
        print(f"  bad magic {magic!r}"); return None
    ver = r.u8()
    h = {}
    h['version']                = ver
    h['headerSize']             = r.u32()
    h['tocEntriesPerBlock']     = r.u32()
    h['tocCompressedBlockCount']= r.u32()
    h['tocCompressedBlockSize'] = r.u32()
    u = r.u32()
    if ver >= 8:
        h['tocUncompressedBlockSize'] = u
        h['containerCount']                 = r.u32()
        h['containerCompressedBlockCount']  = r.u32()
        h['containerCompressedBlockSize']   = r.u32()
        h['containerUncompressedBlockSize'] = r.u32()
        h['containerCompressionMethod']     = r.u32()
        h['containerCompressionBlockSize']  = r.u64()
        h['tocCompressionMethod']           = r.u32()
    else:
        h['tocCompressionMethod']           = u
        h['containerCount']                 = r.u32()
        h['containerCompressedBlockCount']  = r.u32()
        h['containerCompressedBlockSize']   = r.u32()
        h['containerUncompressedBlockSize'] = r.u32()
        h['containerCompressionMethod']     = r.u32()
        h['containerCompressionBlockSize']  = r.u64()

    for k, v in h.items():
        print(f"  {k:32} = {v}")

    print(f"\n  -- header ends at 0x{r.tell():X} (expected 0x{h['headerSize']:X}) --")

    # FIoStoreTocCompressedBlockRef: 5 x u32 (offset, compressedSize, uncompressedSize, ? , ?)
    # Actually UE5 layout: u64 Offset, u32 CompressedSize, u32 UncompressedSize  => packed as
    # struct FIoStoreTocCompressedBlockRef { int32 Offset; uint32 CompressedSize; uint32 UncompressedSize; }
    # 12 bytes; but older code used 20.  Detect by needing offset<fileSize.
    nblk = h['tocCompressedBlockCount']
    print(f"\n  TOC compressed blocks: {nblk}")
    refs12 = []
    ok12 = True
    for i in range(nblk):
        base = r.tell()
        off = r.u32(); csz = r.u32(); usz = r.u32()
        refs12.append((off, csz, usz))
        if off > r.size or csz > r.size:
            ok12 = False
        if i > 64 and not ok12:
            break
    print(f"  12-byte ref layout plausible: {ok12}")
    for i, (o, c, uu) in enumerate(refs12[:8]):
        print(f"    blk[{i}] off=0x{o:X} csize={c:,} usize={uu:,}")
    if ok12 and refs12:
        total_c = sum(x[1] for x in refs12); total_u = sum(x[2] for x in refs12)
        print(f"    TOTAL toc compressed={total_c:,}  uncompressed={total_u:,}")
    r.f.close()
    return h

if __name__ == '__main__':
    for p in sys.argv[1:]:
        try: parse(p)
        except Exception as e:
            import traceback; traceback.print_exc()
