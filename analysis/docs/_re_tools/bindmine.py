#!/usr/bin/env python3
"""
Mine the cooked UE5 reflection metadata (Binds.Cache) for game-specific symbols.
This file is a linear serialized dump: [u32 offset][ASCII string]\0 repeated.
We parse it as a string table and classify entries.
"""
import re, sys, os, collections

def load(path):
    with open(path, 'rb') as f:
        return f.read()

STR = re.compile(rb'[\x20-\x7E]{4,}')

def main(path, patterns, minlen=4):
    data = load(path)
    # Parse the offset-prefixed record stream properly:
    # each record = u32 offset ; then null-terminated ascii
    recs = []
    i = 0
    n = len(data)
    while i + 5 < n:
        off = int.from_bytes(data[i:i+4], 'little')
        j = data.find(b'\x00', i+4)
        if j == -1:
            break
        s = data[i+4:j]
        # validate: printable, and off should be plausible (< 64MB) and monotonic-ish
        if len(s) >= 3 and off < 64*1024*1024 and all(32 <= c < 127 for c in s):
            recs.append((off, s.decode('ascii')))
            i = j + 1
        else:
            i += 1
    print(f"[parsed] {len(recs):,} records from {os.path.basename(path)}")
    return recs

def report(recs, patterns, limit=200):
    rx = re.compile('|'.join(patterns), re.I)
    hits = [(o, s) for o, s in recs if rx.search(s)]
    print(f"[match] {len(hits):,} records matching {patterns}")
    seen = set()
    shown = 0
    for o, s in hits:
        key = s
        if key in seen:
            continue
        seen.add(key)
        if shown < limit:
            print(f"  {o:>9}  0x{o:08X}  {s}")
            shown += 1
    if len(seen) > limit:
        print(f"  ... (+{len(seen)-limit} more unique)")
    return hits

if __name__ == '__main__':
    path = sys.argv[1]
    pats = sys.argv[2:] or ['.']
    recs = main(path, pats)
    report(recs, pats)
