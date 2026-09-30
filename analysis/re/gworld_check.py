"""Which global is GWorld? Find the code that stores into each candidate."""
import sys, os, struct, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

EXE = r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe'
p = PE(EXE); RB = p.image_base
ts = p.sec('.text'); TV = RB + ts['vaddr']; tb = p.secbytes('.text')


def find_writes(target_va):
    """mov [rip+d], r64  (48/4C 89 05 d) and mov [rip+d], r32 (89 05 d)"""
    out = []
    for pre in (b'\x48\x89\x05', b'\x4c\x89\x05', b'\x89\x05'):
        base = 7 if len(pre) == 3 else 6
        i = 0
        while True:
            i = tb.find(pre, i)
            if i < 0:
                break
            disp = struct.unpack_from('<i', tb, i + len(pre))[0]
            tgt = TV + i + len(pre) + 4 + disp
            if tgt == target_va:
                out.append(TV + i)
            i += 1
    return sorted(set(out))


def find_reads(target_va):
    out = []
    for pre in (b'\x48\x8b\x05', b'\x4c\x8b\x05'):
        i = 0
        while True:
            i = tb.find(pre, i)
            if i < 0:
                break
            disp = struct.unpack_from('<i', tb, i + 3)[0]
            if TV + i + 7 + disp == target_va:
                out.append(TV + i)
            i += 1
    return sorted(set(out))


def disasm(a, b):
    r = subprocess.run([r'D:\Programs\LLVM\bin\llvm-objdump.exe', '-d',
                        '--start-address=%#x' % a, '--stop-address=%#x' % b,
                        '--x86-asm-syntax=intel', EXE],
                       capture_output=True, text=True, errors='replace')
    return r.stdout


for name, rva in (('candA', 0xBA35810), ('candB', 0xC037A80), ('GEngine', 0xC03A1C0)):
    va = RB + rva
    w = find_writes(va)
    r = find_reads(va)
    print('### %s  %#012x (rva %#x)  writes=%d reads=%d' % (name, va, rva, len(w), len(r)))
    print('    write sites: %s' % [hex(x) for x in w[:10]])
    print('    read  sites: %d  %s' % (len(r), [hex(x) for x in r[:8]]))
    for s in w[:3]:
        print('    --- disasm around %#x ---' % s)
        print('\n'.join(disasm(s - 0x30, s + 0x30).splitlines()[4:]))
    print()
