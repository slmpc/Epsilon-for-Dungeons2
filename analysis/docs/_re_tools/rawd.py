#!/usr/bin/env python3
"""Raw forensic dump of a UE GVAS save file: hex + all ASCII/UTF16 strings with offsets."""
import re, sys, os, struct

def dump(path, hexlen=0x120, maxstr=200):
    data = open(path,'rb').read()
    print(f"\n{'='*90}\n{os.path.basename(path)}   size={len(data):,}\n{'='*90}")
    print("--- HEADER HEX ---")
    n = min(hexlen, len(data))
    for off in range(0, n, 16):
        chunk = data[off:off+16]
        hx = ' '.join(f'{b:02X}' for b in chunk)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f"  {off:06X}  {hx:<47}  {asc}")

    print("\n--- ENCODED-STRING SCAN (UE FString: i32 len + bytes) ---")
    idx = []
    for off in range(0, min(len(data), 0x400)):
        pass
    # Try sequential FString walk from candidate offsets, printing plausible reads
    def rd_fstr(off):
        if off + 4 > len(data): return None
        ln = struct.unpack_from('<i', data, off)[0]
        if ln == 0: return ('', off+4)
        if ln > 0 and ln < 600 and off+4+ln <= len(data):
            raw = data[off+4:off+4+ln]
            try:
                s = raw.decode('utf-8')
                if all(c.isprintable() or c in '\r\n\t' for c in s):
                    return (s.rstrip('\x00'), off+4+ln)
            except Exception: return None
        return None

    print("\n--- ALL PRINTABLE ASCII RUNS len>=8 ---")
    c = 0
    for m in re.finditer(rb'[\x20-\x7E]{8,}', data):
        print(f"  {m.start():06X}  {m.group().decode()[:maxstr]}")
        c += 1
        if c >= maxstr: print("  ... truncated"); break

    print("\n--- ALL UTF-16LE RUNS len>=8 ---")
    c = 0
    for m in re.finditer(rb'(?:[\x20-\x7E]\x00){8,}', data):
        print(f"  {m.start():06X}  {m.group().decode('utf-16-le')[:maxstr]}")
        c += 1
        if c >= 60: print("  ... truncated"); break

    print("\n--- BASE64/HEX-LIKE LONG TOKENS (len>=80) ---")
    c = 0
    for m in re.finditer(rb'[A-Za-z0-9_\-+/=]{80,}', data):
        print(f"  {m.start():06X}  len={len(m.group())}  {m.group().decode()[:160]}...")
        c += 1
        if c >= 20: print("  ... truncated"); break
    if c == 0: print("  (none)")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        dump(p)
