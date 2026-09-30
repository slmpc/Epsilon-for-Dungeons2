"""Thin llvm-objdump wrapper: disassemble by RVA/VA range of the shipping exe."""
import subprocess, sys, os, re

OBJDUMP = r'D:\Programs\LLVM\bin\llvm-objdump.exe'
EXE = r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe'
IMAGE_BASE = 0x140000000

def disasm(va_start, va_end, intel=True):
    cmd = [OBJDUMP, '-d', '--start-address=%#x' % va_start,
           '--stop-address=%#x' % va_end]
    if intel:
        cmd.append('--x86-asm-syntax=intel')
    cmd.append(EXE)
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return r.stdout

if __name__ == '__main__':
    a = int(sys.argv[1], 0); b = int(sys.argv[2], 0)
    print(disasm(a, b))
