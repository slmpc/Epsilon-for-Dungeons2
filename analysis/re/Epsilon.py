#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Minecraft Dungeons II — 基址 / 运行时定位器（自验证）
====================================================
只读，纯标准库（ctypes + struct）。无需 pymem / pywin32 / frida / 注入。

    python Epsilon.py                 # 完整报告
    python Epsilon.py --json          # 机器可读
    python Epsilon.py --pid 1234

四项定位及各自的验证方式
------------------------
  模块基址   Toolhelp32 枚举模块
  GObjects   .text 特征码（UE5 chunk 索引解码）→ 解析 RIP 位移，无硬编码偏移
  GNames     扫描 .data 中 64KiB 对齐的 FNameEntry 块指针数组（FNamePool::Blocks）
  GEngine    读该槽 → 指针所指对象必须是唯一的 GameEngine 实例
  GWorld     读该槽 → 指针所指对象必须是某个 World 实例

已实测环境：Windows x64, Dungeons-Win64-Shipping.exe 1.1.1.0 / UE 5.6.1
"""

import argparse, ctypes, ctypes.wintypes as wt, json, os, struct, sys

# ---------------------------------------------------------------- 结构常量
ITEM_STRIDE = 0x18
CHUNK_ITEMS = 0x10000

OFF_OBJOBJECTS   = 0x10
OFF_CHUNK_TABLE  = 0x10
OFF_MAX_ELEMENTS = 0x20
OFF_NUM_ELEMENTS = 0x24
OFF_MAX_CHUNKS   = 0x28
OFF_NUM_CHUNKS   = 0x2C

OFF_OBJ_CLASS = 0x10
OFF_OBJ_NAME  = 0x18
OFF_OBJ_OUTER = 0x20
OFF_OBJ_FLAGS = 0x08
OFF_OBJ_INDEX = 0x0C

BLOCK_ALIGN = 0x10000          # FNamePool 块大小
GNAMES_BLOCKS_OFF = 0x10       # FNameEntryAllocator::Blocks

# 默认 RVA（会由运行时验证确认；游戏更新后即使失效也会被验证步骤挡下）
DEF_GOBJECTS_RVA = 0x0BEA8BF0
DEF_GNAMES_RVA   = 0x0BDC5040
DEF_GENGINE_RVA  = 0x0C03A1C0
DEF_GWORLD_RVA   = 0x0C037A80
DEF_GNATIVE_RVA  = 0x0BA35810   # 同上对象，但无代码引用 → 是结构体字段，不是 GWorld

TARGET_EXE = 'dungeons-win64-shipping.exe'
DEFAULT_EXE = (r'E:\SteamLibrary\steamapps\common\Minecraft Dungeons II'
               r'\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe')

PROC_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
MEM_COMMIT, PAGE_GUARD, PAGE_NOACCESS = 0x1000, 0x100, 0x01
k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_ulonglong),
                ('AllocationBase', ctypes.c_ulonglong),
                ('AllocationProtect', wt.DWORD), ('__a1', wt.DWORD),
                ('RegionSize', ctypes.c_ulonglong),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD),
                ('__a2', wt.DWORD)]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD),
                ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long),
                ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('th32ModuleID', wt.DWORD),
                ('th32ProcessID', wt.DWORD), ('GlblcntUsage', wt.DWORD),
                ('ProccntUsage', wt.DWORD),
                ('modBaseAddr', ctypes.POINTER(ctypes.c_byte)),
                ('modBaseSize', wt.DWORD), ('hModule', wt.HMODULE),
                ('szModule', ctypes.c_char * 256), ('szExePath', ctypes.c_char * 260)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(MBI),
                               ctypes.c_size_t]


def enum_processes():
    snap = k32.CreateToolhelp32Snapshot(0x02, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    out = []
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            out.append((pe.szExeFile.decode('latin1'), pe.th32ProcessID))
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return out


def pick_pid():
    """同一个 exe 可能有多个实例；优先返回能被本进程读取的那个。"""
    cands = [p for n, p in enum_processes() if n.lower() == TARGET_EXE]
    for pid in cands:
        h = k32.OpenProcess(PROC_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if h:
            k32.CloseHandle(h)
            return pid, cands
    return (cands[0] if cands else None), cands


class Proc(object):
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROC_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            e = ctypes.get_last_error()
            raise OSError('OpenProcess(pid=%d) 失败 GetLastError=%d%s'
                          % (pid, e, '（目标权限更高：请用同样的权限启动本脚本）' if e == 5 else ''))
        self._regs = None

    def read(self, va, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_ulonglong(va), buf, n,
                                     ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def u64(self, va):
        b = self.read(va, 8)
        return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None

    def i32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<i', b)[0] if b and len(b) == 4 else None

    def modules(self):
        snap = k32.CreateToolhelp32Snapshot(0x18, self.pid)
        me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
        out = []
        if k32.Module32First(snap, ctypes.byref(me)):
            while True:
                out.append((me.szModule.decode('latin1'),
                            ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value,
                            me.modBaseSize))
                if not k32.Module32Next(snap, ctypes.byref(me)):
                    break
        k32.CloseHandle(snap)
        return out

    def regions(self):
        if self._regs is None:
            out, addr, mbi = [], 0, MBI()
            while addr < 0x7FFFFFFFFFFF:
                if not k32.VirtualQueryEx(self.h, ctypes.c_ulonglong(addr),
                                          ctypes.byref(mbi), ctypes.sizeof(mbi)):
                    break
                out.append((mbi.BaseAddress, mbi.RegionSize, mbi.State,
                            mbi.Protect, mbi.Type))
                nxt = mbi.BaseAddress + mbi.RegionSize
                if nxt <= addr:
                    break
                addr = nxt
            self._regs = out
        return self._regs

    def committed(self, va):
        for a, sz, st, prot, ty in self.regions():
            if a <= va < a + sz:
                return st == MEM_COMMIT and not (prot & PAGE_GUARD) and \
                       (prot & 0xFF) != PAGE_NOACCESS
        return False


class PE(object):
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b'MZ':
            raise ValueError('不是 PE 文件')
        e = struct.unpack_from('<I', d, 0x3C)[0]
        coff = e + 4
        (self.machine, self.nsections, self.timestamp, _ps, _ns, optsz,
         self.characteristics) = struct.unpack_from('<HHIIIHH', d, coff)
        opt = coff + 20
        self.image_base = struct.unpack_from('<Q', d, opt + 24)[0]
        self.size_image = struct.unpack_from('<I', d, opt + 56)[0]
        self.dllchar = struct.unpack_from('<H', d, opt + 70)[0]
        self.sections = []
        so = opt + optsz
        for i in range(self.nsections):
            b = so + i * 40
            nm = d[b:b + 8].rstrip(b'\0').decode('latin1')
            vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', d, b + 8)
            self.sections.append(dict(name=nm, vsize=vsize, vaddr=vaddr,
                                      rawsize=rawsize, rawptr=rawptr))

    def secbytes(self, name):
        for s in self.sections:
            if s['name'] == name:
                return self.data[s['rawptr']:s['rawptr'] + s['rawsize']]
        return b''

    def section_at_rva(self, rva):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rawsize']):
                return s['name']
        return None


# ---------------------------------------------------------------- GObjects
SHR_R8_10 = b'\x49\xc1\xe8\x10'
MOV_RAX_RIP = b'\x48\x8b\x05'


def sig_find_gobjects(image):
    """.text 特征码：shr r8,0x10 ... mov rax,[rip+GObjects+0x10]
    返回 [(preferred_base_va, hit_rva)]"""
    tb = image.secbytes('.text')
    tv = image.image_base
    out, seen = [], set()
    i = 0
    while True:
        i = tb.find(SHR_R8_10, i)
        if i < 0:
            break
        for k in range(0, 48):
            j = i + k
            if j + 7 > len(tb):
                break
            if tb[j:j + 3] == MOV_RAX_RIP:
                disp = struct.unpack_from('<i', tb, j + 3)[0]
                tv_sec = tv + image.sections[0]['vaddr'] + j + 7 + disp
                g = tv_sec - OFF_CHUNK_TABLE
                if g not in seen:
                    seen.add(g)
                    out.append((g, image.sections[0]['vaddr'] + j))
                break
        i += 1
    return out


def validate_gobjects(pr, base, rva, image):
    g = base + rva
    if image.section_at_rva(rva) != '.data':
        return None
    maxel = pr.u32(g + OFF_MAX_ELEMENTS)
    numel = pr.u32(g + OFF_NUM_ELEMENTS)
    maxch = pr.u32(g + OFF_MAX_CHUNKS)
    numch = pr.u32(g + OFF_NUM_CHUNKS)
    if None in (maxel, numel, maxch, numch):
        return None
    if not (1 <= numel <= 8_000_000 and numel <= maxel <= 32_000_000):
        return None
    if numch != (numel + CHUNK_ITEMS - 1) // CHUNK_ITEMS or numch == 0:
        return None
    if not (1 <= maxch <= 8192 and maxch >= numch):
        return None
    tbl = pr.u64(g + OFF_CHUNK_TABLE)
    if not tbl or not pr.committed(tbl):
        return None
    c0 = pr.u64(tbl)
    if not c0 or not pr.committed(c0):
        return None
    rng = []
    for nm in ('.rdata', '_RDATA'):
        for s in image.sections:
            if s['name'] == nm:
                rng.append((base + s['vaddr'], base + s['vaddr'] + s['vsize']))
    ok = bad = 0
    for idx in range(8):
        ch = pr.u64(tbl + 8 * (idx // CHUNK_ITEMS))
        obj = pr.u64(ch + (idx % CHUNK_ITEMS) * ITEM_STRIDE) if ch else None
        vt = pr.u64(obj) if obj else None
        if vt and any(a <= vt < b for a, b in rng):
            ok += 1
        else:
            bad += 1
    if ok == 0 or ok < bad:
        return None
    return dict(rva=rva, va=g, chunk_table=tbl, chunk0=c0, num_elements=numel,
                max_elements=maxel, num_chunks=numch, max_chunks=maxch,
                validated=ok)


# ---------------------------------------------------------------- GNames
def find_gnames(pr, base, image):
    """扫描 .data，找 64KiB 对齐且块头是合法 FNameEntry 链的指针数组。"""
    ds = [s for s in image.sections if s['name'] == '.data'][0]
    dbase, dsize = base + ds['vaddr'], ds['vsize']

    def entry_ok(va):
        h = pr.read(va, 2)
        if not h:
            return None
        hh = struct.unpack('<H', h)[0]
        if hh & 1:
            return None
        ln = (hh >> 6) & 0x3FF
        if not (1 <= ln <= 250):
            return None
        s = pr.read(va + 2, ln + 1)
        if not s or len(s) < ln + 1 or s[ln] != 0:
            return None
        if not all(0x20 <= c < 0x7F for c in s[:ln]):
            return None
        return s[:ln]

    def chain_ok(va, want=4):
        p = va
        for _ in range(want):
            n = entry_ok(p)
            if n is None:
                return False
            step = 2 + len(n) + 1
            step += step & 1
            p += step
        return True

    slots = []
    off = 0
    while off < dsize:
        n = min(0x100000, dsize - off)
        buf = pr.read(dbase + off, n)
        off += n
        if not buf:
            continue
        for k in range(0, len(buf) - 8, 8):
            v = struct.unpack_from('<Q', buf, k)[0]
            if v and (v & (BLOCK_ALIGN - 1)) == 0 and 0x10000 < v < 0x7FFFFFFFFFFF \
               and pr.committed(v) and chain_ok(v):
                slots.append(dbase + off - n + k)
    if not slots:
        return None
    p = min(slots)
    steps = 0
    while steps < 1 << 16:
        v = pr.u64(p - 8)
        if v is None:
            break
        if v != 0 and not ((v & (BLOCK_ALIGN - 1)) == 0 and pr.committed(v)):
            break
        p -= 8
        steps += 1
    blocks = p
    return dict(blocks=blocks, va=blocks - GNAMES_BLOCKS_OFF,
                rva=blocks - GNAMES_BLOCKS_OFF - base,
                blocks_found=len(slots), back_steps=steps)


def resolve_name(pr, blocks, index):
    b = pr.u64(blocks + 8 * (index >> 16))
    if not b:
        return None
    p = b + (index & 0xFFFF) * 2
    h = pr.read(p, 2)
    if not h:
        return None
    hh = struct.unpack('<H', h)[0]
    wide = hh & 1
    ln = (hh >> 6) & 0x3FF
    if not (0 < ln <= 500):
        return None
    s = pr.read(p + 2, ln * (2 if wide else 1) + 2)
    if not s:
        return None
    try:
        return s[:ln * (2 if wide else 1)].decode('utf-16-le' if wide else 'latin1')
    except Exception:
        return None


def obj_name(pr, blocks, obj, off):
    """off == OFF_OBJ_CLASS 时取「对象的类名」；off == -1 时取「对象自己的名字」。"""
    if off < 0:
        idx = pr.i32(obj + OFF_OBJ_NAME)
        return resolve_name(pr, blocks, idx) if idx is not None else None
    v = pr.u64(obj + off)
    if not v:
        return None
    b = pr.read(v, 0x28)
    if not b:
        return None
    idx = struct.unpack_from('<i', b, OFF_OBJ_NAME)[0]
    return resolve_name(pr, blocks, idx)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()

    if not os.path.isfile(a.exe):
        print('找不到映像: %s' % a.exe, file=sys.stderr)
        return 2
    image = PE(a.exe)

    pid, allpids = (a.pid, [a.pid]) if a.pid else pick_pid()
    if pid is None:
        print('未找到运行中的 %s' % TARGET_EXE, file=sys.stderr)
        return 2
    pr = Proc(pid)
    mods = [m for m in pr.modules() if m[0].lower() == TARGET_EXE]
    base = mods[0][1]
    size = mods[0][2]

    R = dict(pid=pid, pids_all=allpids,
             static_image_base=image.image_base, module_base=base,
             module_size=size, aslr_delta=base - image.image_base,
             exe=a.exe, sections={})
    for s in image.sections:
        R['sections'][s['name']] = dict(rva=s['vaddr'], va=base + s['vaddr'],
                                        vsize=s['vsize'])
    ok_all = True

    # --- GObjects ---
    cands = sig_find_gobjects(image)
    R['gobjects_signature_hits'] = [hex(c[0]) for c in cands]
    gob = None
    for g_pref, hit in cands:
        v = validate_gobjects(pr, base, g_pref - image.image_base, image)
        if v:
            v['sig_hit_rva'] = hit
            gob = v
            break
    R['gobjects'] = gob
    ok_all &= gob is not None

    # --- GNames ---
    gn = find_gnames(pr, base, image)
    R['gnames'] = gn
    ok_all &= gn is not None
    blocks = gn['blocks'] if gn else None

    # --- GEngine / GWorld ---
    def class_of(obj):
        return obj_name(pr, blocks, obj, OFF_OBJ_CLASS) if (obj and blocks) else None

    def name_of(obj):
        return obj_name(pr, blocks, obj, -1) if (obj and blocks) else None

    glob = {}
    for label, rva in (('GEngine', DEF_GENGINE_RVA), ('GWorld', DEF_GWORLD_RVA),
                       ('NativeWorldPtr', DEF_GNATIVE_RVA)):
        slot = base + rva
        v = pr.u64(slot)
        glob[label] = dict(rva=rva, va=slot, value=('%#x' % v) if v else None,
                           object_name=name_of(v) if v else None,
                           object_class=class_of(v) if v else None)
    R['globals'] = glob
    gengine_ok = glob['GEngine']['object_class'] == 'GameEngine'
    gworld_ok = glob['GWorld']['object_class'] == 'World'
    R['gengine_verified'] = gengine_ok
    R['gworld_verified'] = gworld_ok

    if a.json:
        print(json.dumps(R, indent=2, ensure_ascii=False))
        return 0 if (gob and gn) else 1

    W = sys.stdout.write
    W('=' * 74 + '\n')
    W('Minecraft Dungeons II  —  基址与运行时结构（实测自验证）\n')
    W('=' * 74 + '\n')
    W('进程 PID              : %d%s\n' % (pid,
      ('  （同 exe 还有: %s）' % [p for p in allpids if p != pid]) if len(allpids) > 1 else ''))
    W('映像                  : %s\n' % a.exe)
    W('静态 ImageBase        : %#x\n' % image.image_base)
    W('>>> 运行时模块基址    : %#x\n' % base)
    W('    ASLR 偏移         : %#x\n' % (base - image.image_base))
    W('    模块大小          : %#x\n' % size)
    W('\n节区（运行时 VA = 基址 + RVA）：\n')
    for s in image.sections:
        W('   %-9s RVA %#010x  VA %#012x  size %#010x\n'
          % (s['name'], s['vaddr'], base + s['vaddr'], s['vsize']))

    W('\n' + '-' * 74 + '\nGObjects (FUObjectArray)\n' + '-' * 74 + '\n')
    W('特征码候选: %d 个 %s\n' % (len(cands), [hex(c[0]) for c in cands]))
    if gob:
        W('  GObjects  RVA : %#x        <<< 写进配置\n' % gob['rva'])
        W('  GObjects  VA  : %#x\n' % gob['va'])
        W('  chunk 表      : %#x\n' % gob['chunk_table'])
        W('  chunk[0]      : %#x\n' % gob['chunk0'])
        W('  NumElements   : %d\n' % gob['num_elements'])
        W('  MaxElements   : %d\n' % gob['max_elements'])
        W('  NumChunks/Max : %d / %d\n' % (gob['num_chunks'], gob['max_chunks']))
        W('  FUObjectItem  : 步长 %#x，校验 %d/8 通过\n'
          % (ITEM_STRIDE, gob['validated']))
        W('  取值链        : T=*(u64*)(G+0x10); Ch=*(u64*)(T+8*(I>>16));\n'
          '                  Obj=*(u64*)(Ch+0x18*(I&0xFFFF))\n')
    else:
        W('  !! 未通过校验（游戏可能仍在加载）\n')

    W('\n' + '-' * 74 + '\nGNames (FNamePool)\n' + '-' * 74 + '\n')
    if gn:
        W('  GNames    RVA : %#x        <<< 写进配置\n' % gn['rva'])
        W('  GNames    VA  : %#x\n' % gn['va'])
        W('  Blocks[]  VA  : %#x   (GNames+0x%x)\n' % (gn['blocks'], GNAMES_BLOCKS_OFF))
        W('  校验          : %d 个 64KiB 块指针, 回退 %d 槽到数组首元素\n'
          % (gn['blocks_found'], gn['back_steps']))
        if gob:
            for i in (0, 1, 2, 11):
                ch = pr.u64(gob['chunk_table'] + 8 * (i // CHUNK_ITEMS))
                o = pr.u64(ch + (i % CHUNK_ITEMS) * ITEM_STRIDE) if ch else None
                if o:
                    W('  [%2d] %-32r : %r\n' % (i, name_of(o), class_of(o)))
    else:
        W('  !! 未找到\n')

    W('\n' + '-' * 74 + '\n引擎全局\n' + '-' * 74 + '\n')
    for k in ('GEngine', 'GWorld', 'NativeWorldPtr'):
        d = glob[k]
        W('  %-14s RVA %#-10x VA %#012x -> %-10s %r : %r  %s\n'
          % (k, d['rva'], d['va'], d['value'], d['object_name'],
             d['object_class'],
             '<-- 已验证' if (k == 'GEngine' and gengine_ok) or
                              (k == 'GWorld' and gworld_ok) else
             ('（结构体字段，非全局变量）' if k == 'NativeWorldPtr' else '')))
    W('\n结论: GObjects %s | GNames %s | GEngine %s | GWorld %s\n'
      % ('OK' if gob else 'FAIL', 'OK' if gn else 'FAIL',
         'OK' if gengine_ok else 'FAIL', 'OK' if gworld_ok else 'FAIL'))
    return 0 if (gob and gn) else 1


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    sys.exit(main())
