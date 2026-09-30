// ============================================================================
//  procUtil.cpp — RemoteThread 注入的实现细节
//
//  注入序列(每一步的失败都能单独报出来, 便于定位是哪一环被拦):
//    1. 本地确认 DLL 存在
//    2. OpenProcess(PROCESS_CREATE_THREAD | QUERY_INFORMATION |
//                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ)
//       —— 最小必要权限集合, 不是 PROCESS_ALL_ACCESS
//    3. 确认目标不是 WOW64(32 位)
//    4. VirtualAllocEx 分配存放 DLL 路径(Unicode)的内存
//    5. WriteProcessMemory 写入路径
//    6. GetProcAddress(kernel32!LoadLibraryW)
//       —— kernel32 在每个进程里的基址相同(同一会话内), 所以本地函数指针
//          在目标进程里同样有效, 这正是远程线程注入成立的基石
//    7. CreateRemoteThread(lpStartAddress = LoadLibraryW, lpParameter = Str)
//       —— LoadLibraryW 的参数是 LPCWSTR 且返回 HMODULE, 与
//          LPTHREAD_START_ROUTINE 的签名(LPVOID)->DWORD 在 x64 ABI 上完全兼容
//    8. WaitForSingleObject + GetExitCodeThread → 远端 HMODULE
// ============================================================================
#include "common/ProcUtil.h"
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>

namespace epsilon {
namespace {

// RAII: 保证任何提前 return 都不漏句柄。
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); h = o.h; o.h = nullptr; }
        return *this;
    }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return valid(); }
};

// 注入所需的最小权限集合。
constexpr DWORD injectAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

std::string wideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string base_name_of(std::string_view path) {
    const size_t pos = path.find_last_of("\\/");
    return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

} // namespace

std::string_view statusToString(InjectStatus s) {
    switch (s) {
        case InjectStatus::ok:                  return "成功";
        case InjectStatus::processNotFound:   return "找不到目标进程";
        case InjectStatus::openProcessFailed: return "OpenProcess 被拒(权限不足或进程已退出)";
        case InjectStatus::moduleNotFound:    return "kernel32 或目标模块未找到";
        case InjectStatus::dllMissing:         return "DLL 文件不存在";
        case InjectStatus::archMismatch:       return "目标是 32 位(WOW64)进程, 架构不匹配";
        case InjectStatus::allocFailed:        return "VirtualAllocEx 失败";
        case InjectStatus::writeFailed:        return "WriteProcessMemory 失败";
        case InjectStatus::threadFailed:       return "CreateRemoteThread 失败";
        case InjectStatus::remoteLoadFailed:  return "远端 LoadLibraryW 返回 NULL";
        case InjectStatus::remoteTimeout:      return "等待远端线程超时";
    }
    return "未知";
}

// ---------------------------------------------------------------------------
std::vector<ProcInfo> enumProcesses() {
    std::vector<ProcInfo> out;
    Handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!::Process32FirstW(snap.h, &pe)) return out;
    do {
        ProcInfo pi;
        pi.pid  = pe.th32ProcessID;
        pi.name = wideToUtf8(pe.szExeFile);
        if (pi.pid != 0) pi.path = processPathOf(pi.pid);
        out.push_back(std::move(pi));
    } while (::Process32NextW(snap.h, &pe));

    std::sort(out.begin(), out.end(), [](ProcInfo const& a, ProcInfo const& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.pid < b.pid;
    });
    return out;
}

std::vector<ProcInfo> findProcessesByName(std::string_view exeName) {
    std::vector<ProcInfo> out;
    for (auto& p : enumProcesses()) {
        if (iequals(p.name, exeName)) out.push_back(p);
    }
    return out;
}

bool enumModules(uint32_t pid, std::vector<ModuleInfo>& out) {
    out.clear();
    Handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (!::Module32FirstW(snap.h, &me)) return false;
    do {
        ModuleInfo mi;
        mi.name = wideToUtf8(me.szModule);
        mi.base = reinterpret_cast<uint64_t>(me.modBaseAddr);
        mi.size = me.modBaseSize;
        out.push_back(std::move(mi));
    } while (::Module32NextW(snap.h, &me));
    return true;
}

std::optional<ModuleInfo> findModule(uint32_t pid, std::string_view name) {
    std::vector<ModuleInfo> mods;
    if (!enumModules(pid, mods)) return std::nullopt;
    for (auto& m : mods) {
        if (iequals(m.name, name)) return m;
    }
    return std::nullopt;
}

bool processAlive(uint32_t pid) {
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return false;
    DWORD code = 0;
    if (!::GetExitCodeProcess(h.h, &code)) return false;
    return code == STILL_ACTIVE;
}

std::string processNameOf(uint32_t pid) {
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (!::QueryFullProcessImageNameW(h.h, 0, buf, &n)) return {};
    return base_name_of(wideToUtf8(std::wstring_view(buf, n)));
}

std::string processPathOf(uint32_t pid) {
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return {};
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (!::QueryFullProcessImageNameW(h.h, 0, buf, &n)) return {};
    return wideToUtf8(std::wstring_view(buf, n));
}

bool isWow64Process(uint32_t pid) {
#if defined(_WIN64)
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return false;
    BOOL wow = FALSE;
    if (::IsWow64Process(h.h, &wow)) return wow != FALSE;
#endif
    return false;
}

// ---------------------------------------------------------------------------
uint64_t remoteAlloc(uint32_t pid, size_t size, uint32_t protect) {
    Handle h(::OpenProcess(PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!h) return 0;
    auto p = ::VirtualAllocEx(h.h, nullptr, size, MEM_COMMIT | MEM_RESERVE, protect);
    return reinterpret_cast<uint64_t>(p);
}

bool remoteFree(uint32_t pid, uint64_t addr, uint32_t freeType) {
    Handle h(::OpenProcess(PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!h) return false;
    return ::VirtualFreeEx(h.h, reinterpret_cast<LPVOID>(addr), 0, freeType) != FALSE;
}

bool remoteWrite(uint32_t pid, uint64_t addr, void const* data, size_t size) {
    Handle h(::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!h) return false;
    SIZE_T written = 0;
    return ::WriteProcessMemory(h.h, reinterpret_cast<LPVOID>(addr), data, size, &written) &&
           written == size;
}

bool remoteRead(uint32_t pid, uint64_t addr, void* out, size_t size) {
    Handle h(::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!h) return false;
    SIZE_T got = 0;
    return ::ReadProcessMemory(h.h, reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}

bool remoteReadable(uint32_t pid, uint64_t addr, size_t size) {
    Handle h(::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!h) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQueryEx(h.h, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const uint64_t region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + size <= region_end;
}

// ---------------------------------------------------------------------------
InjectResult injectDll(uint32_t pid, std::string const& dllPath, InjectOptions const& opt) {
    InjectResult r;
    const ULONGLONG t0 = ::GetTickCount64();

    // 本地先确认文件在, 免得把错误推给远端。
    const std::wstring wpath = utf8ToWide(dllPath);
    {
        const DWORD attr = ::GetFileAttributesW(wpath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            r.status = InjectStatus::dllMissing;
            r.detail = fmt("本地看不到文件: {}", dllPath);
            return r;
        }
    }

    if (!processAlive(pid)) {
        r.status = InjectStatus::processNotFound;
        r.win32Error = ERROR_NOT_FOUND;
        return r;
    }

    if (isWow64Process(pid)) {
        r.status = InjectStatus::archMismatch;
        return r;
    }

    Handle h(::OpenProcess(injectAccess, FALSE, pid));
    if (!h) {
        r.status = InjectStatus::openProcessFailed;
        r.win32Error = ::GetLastError();
        return r;
    }

    // ---- 1) 在目标里分配存放路径的内存 ----
    const size_t bytes = (wpath.size() + 1) * sizeof(wchar_t);
    const uint64_t remoteStr = remoteAlloc(pid, bytes, PAGE_READWRITE);
    if (!remoteStr) {
        r.status = InjectStatus::allocFailed;
        r.win32Error = ::GetLastError();
        return r;
    }
    r.remoteString = remoteStr;

    // ---- 2) 把路径写进去 ----
    if (!remoteWrite(pid, remoteStr, wpath.c_str(), bytes)) {
        r.status = InjectStatus::writeFailed;
        r.win32Error = ::GetLastError();
        remoteFree(pid, remoteStr);
        r.remoteString = 0;
        return r;
    }

    // ---- 3) 取 LoadLibraryW 的地址 ----
    // kernel32.dll 在同一会话的所有进程里基址相同, 所以这个指针在目标里也有效。
    const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        r.status = InjectStatus::moduleNotFound;
        r.detail = "本地 GetModuleHandleW(kernel32.dll) 失败";
        remoteFree(pid, remoteStr);
        r.remoteString = 0;
        return r;
    }
    const FARPROC loadLibrary = ::GetProcAddress(k32, "LoadLibraryW");
    if (!loadLibrary) {
        r.status = InjectStatus::moduleNotFound;
        r.detail = "kernel32!LoadLibraryW 解析失败";
        remoteFree(pid, remoteStr);
        r.remoteString = 0;
        return r;
    }

    // ---- 4) 远程线程 ----
    Handle th(::CreateRemoteThread(h.h, nullptr, 0,
                                   reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                   reinterpret_cast<LPVOID>(remoteStr), 0, nullptr));
    if (!th) {
        r.status = InjectStatus::threadFailed;
        r.win32Error = ::GetLastError();
        remoteFree(pid, remoteStr);
        r.remoteString = 0;
        return r;
    }

    // ---- 5) 等 LoadLibrary 返回 ----
    const DWORD wait = ::WaitForSingleObject(th.h, opt.timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        r.status = InjectStatus::remoteTimeout;
        // 线程还在跑, 绝不能释放 Str —— 会让远端踩空指针导致目标崩溃。
        r.detail = "远端线程未在超时内结束; 已保留参数字符串以免目标踩空指针";
        r.elapsedMs = static_cast<uint32_t>(::GetTickCount64() - t0);
        return r;
    }

    DWORD exitCode = 0;
    ::GetExitCodeThread(th.h, &exitCode);
    r.remoteModule = exitCode;

    // 远端 LoadLibrary 已返回, 字符串可以安全回收。
    remoteFree(pid, remoteStr);
    r.remoteString = 0;
    r.elapsedMs = static_cast<uint32_t>(::GetTickCount64() - t0);

    if (exitCode == 0) {
        r.status = InjectStatus::remoteLoadFailed;
        r.win32Error = ::GetLastError();
        r.detail = "远端 LoadLibraryW 返回 NULL —— 常见原因: DLL 依赖缺失、"
                   "位数不符(x86/x64)、路径在目标侧不可访问";
        return r;
    }

    r.status = InjectStatus::ok;
    return r;
}

} // namespace epsilon
