"""Read-only runtime forensics on a live Dungeons-Win64-Shipping.exe (ctypes, no writes)."""
import ctypes, ctypes.wintypes as wt, struct, sys, time, json, subprocess

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_ulonglong),
                ('AllocationBase', ctypes.c_ulonglong),
                ('AllocationProtect', wt.DWORD), ('__a1', wt.DWORD),
                ('RegionSize', ctypes.c_ulonglong),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD),
                ('__a2', wt.DWORD)]

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_ulonglong,
                               ctypes.POINTER(MEMORY_BASIC_INFORMATION64), ctypes.c_size_t]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long),
                ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('th32ModuleID', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('GlblcntUsage', wt.DWORD), ('ProccntUsage', wt.DWORD),
                ('modBaseAddr', ctypes.POINTER(ctypes.c_byte)), ('modBaseSize', wt.DWORD),
                ('hModule', wt.HMODULE), ('szModule', ctypes.c_char * 256),
                ('szExePath', ctypes.c_char * 260)]


def enum_procs():
    TH32CS_SNAPPROCESS = 0x02
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    out = []
    if k32.Process32First(snap, ctypes.byref(pe)):
        while True:
            out.append((pe.szExeFile.decode('latin1'), pe.th32ProcessID))
            if not k32.Process32Next(snap, ctypes.byref(pe)):
                break
    k32.CloseHandle(snap)
    return out


class Proc:
    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise OSError('OpenProcess failed err=%d' % ctypes.get_last_error())
        self._regs = None

    def read(self, va, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(self.h, ctypes.c_ulonglong(va), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def u64(self, va):
        b = self.read(va, 8)
        return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None

    def modules(self):
        TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x08, 0x10
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, self.pid)
        me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
        out = []
        if k32.Module32First(snap, ctypes.byref(me)):
            while True:
                base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
                out.append((me.szModule.decode('latin1'), base, me.modBaseSize,
                            me.szExePath.decode('latin1')))
                if not k32.Module32Next(snap, ctypes.byref(me)):
                    break
        k32.CloseHandle(snap)
        return out

    def regions(self, lo=0, hi=0x7FFFFFFFFFFF):
        addr = lo
        mbi = MEMORY_BASIC_INFORMATION64()
        out = []
        while addr < hi:
            r = k32.VirtualQueryEx(self.h, ctypes.c_ulonglong(addr), ctypes.byref(mbi),
                                   ctypes.sizeof(mbi))
            if not r:
                break
            out.append((mbi.BaseAddress, mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type))
            nxt = mbi.BaseAddress + mbi.RegionSize
            if nxt <= addr:
                break
            addr = nxt
        return out

    def committed(self, va):
        """True if va is inside a committed, non-guard, readable region."""
        if self._regs is None:
            self._regs = self.regions()
        for a, sz, st, prot, ty in self._regs:
            if a <= va < a + sz:
                return st == 0x1000 and not (prot & 0x100) and (prot & 0xFF) != 0x01
        return False


def find_main_proc(name='dungeons-win64-shipping.exe'):
    for nm, pid in enum_procs():
        if nm.lower() == name:
            return pid
    return None


def pe_sections(pr, base):
    hdr = pr.read(base, 0x1000)
    e = struct.unpack_from('<I', hdr, 0x3c)[0]
    nsec = struct.unpack_from('<H', hdr, e + 6)[0]
    optsz = struct.unpack_from('<H', hdr, e + 20)[0]
    sizeimg = struct.unpack_from('<I', hdr, e + 24 + 56)[0]
    so = e + 24 + optsz
    secs = []
    for i in range(nsec):
        b = so + i * 40
        nm = hdr[b:b+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', hdr, b + 8)
        secs.append((nm, base + vaddr, vsize, rawptr, rawsize))
    return secs, sizeimg, hdr


if __name__ == '__main__':
    pid = find_main_proc()
    print('pid =', pid)
    pr = Proc(pid)
    mods = pr.modules()
    print('modules:', len(mods))
    main = [m for m in mods if m[0].lower() == 'dungeons-win64-shipping.exe'][0]
    print('MAIN base=%#012x size=%#x' % (main[1], main[2]))
    print('     path=%s' % main[3])
    secs, sizeimg, hdr = pe_sections(pr, main[1])
    for nm, va, vs, rp, rs in secs:
        print('  %-9s va=%#012x vsize=%#010x' % (nm, va, vs))
    print('SizeOfImage=%#x' % sizeimg)
