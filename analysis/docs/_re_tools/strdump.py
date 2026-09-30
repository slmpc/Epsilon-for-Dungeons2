#!/usr/bin/env python3
"""Dump printable strings + structure of a binary blob, with offset/context."""
import sys, os, re

def dump(path, minlen=6, limit=400, show_hex=True, offset_range=None):
    size = os.path.getsize(path)
    print(f"\n{'='*88}\n{path}\n  size = {size:,} bytes\n{'='*88}")
    with open(path, 'rb') as f:
        data = f.read()
    if offset_range:
        a, b = offset_range
        data = data[a:b]
        base = a
    else:
        base = 0
    # printable ASCII runs
    pat = re.compile(rb'[\x20-\x7E]{%d,}' % minlen)
    n = 0
    seen = {}
    for m in pat.finditer(data):
        s = m.group().decode('ascii')
        off = base + m.start()
        # count duplicates (very common in cache/table files)
        if s in seen:
            seen[s][1] += 1
            continue
        seen[s] = [off, 1]
        n += 1
        if n <= limit:
            print(f"  {off:>10} (0x{off:08X})  {s[:160]}")
    print(f"\n  -- {n} unique strings shown (of {sum(v[1] for v in seen.values())} total runs, {len(seen)} unique) --")

if __name__ == '__main__':
    args = sys.argv[1:]
    minlen = 6
    if args and args[0].startswith('--minlen='):
        minlen = int(args[0].split('=')[1]); args = args[1:]
    for p in args:
        dump(p, minlen=minlen)
