import struct
from peimg import Image

img = Image()

for va in (0x149f2f6b8, 0x149f2d608, 0x14a0cec68, 0x14a3dde08, 0x14a5a5638):
    b = img.read_va(va - 0x40, 0xc0)
    print('==== around %#x' % va)
    for off in range(0, len(b), 16):
        chunk = b[off:off + 16]
        print('  %012x  %-47s  %s' % (va - 0x40 + off,
              ' '.join('%02x' % c for c in chunk),
              ''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)))
    print()
