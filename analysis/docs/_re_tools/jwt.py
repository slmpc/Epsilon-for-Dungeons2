#!/usr/bin/env python3
"""Extract and decode JWT tokens from Minecraft Dungeons save files."""
import re, sys, os, base64, json, struct

JWT_RX = re.compile(rb'eyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}')

def b64d(s):
    s = s + '=' * (-len(s) % 4)
    return base64.urlsafe_b64decode(s)

def scan(path):
    print(f"\n{'='*84}\n{os.path.basename(path)}   size={os.path.getsize(path):,}\n{'='*84}")
    data = open(path,'rb').read()
    # header bytes
    print(f"  first 32 bytes: {' '.join(f'{b:02X}' for b in data[:32])}")
    ascii_head = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[:64])
    print(f"  ascii: {ascii_head!r}")

    toks = JWT_RX.findall(data)
    print(f"  JWT-shaped tokens found: {len(toks)}")
    for t in toks[:6]:
        t = t.decode()
        parts = t.split('.')
        print(f"\n  --- token len={len(t)} ---")
        for name, seg in zip(('HEADER','PAYLOAD','SIG'), parts[:3]):
            try:
                dec = json.loads(b64d(seg))
                print(f"  [{name}] {json.dumps(dec, indent=2, ensure_ascii=False)[:2400]}")
            except Exception as e:
                print(f"  [{name}] <undecodable: {type(e).__name__}> raw={seg[:80]}")

    # also look for plaintext json
    for m in re.finditer(rb'\{"[ -~]{20,}\}', data):
        try:
            j = json.loads(m.group())
            print(f"\n  [JSON @0x{m.start():X}] {json.dumps(j, ensure_ascii=False)[:800]}")
        except Exception:
            pass
        if m.start() > 5_000_000: break

if __name__ == '__main__':
    for p in sys.argv[1:]:
        try: scan(p)
        except Exception as e:
            import traceback; traceback.print_exc()
