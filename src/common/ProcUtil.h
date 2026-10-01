// ProcUtil.h — 进程发现 / 远程内存 / RemoteThread(LoadLibraryW) 注入。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon {

struct ProcInfo {
    uint32_t    pid = 0;
    std::string name;        // 映像名, 如 Dungeons-Win64-Shipping.exe
    std::string path;        // 完整路径(取不到则为空)
};

std::vector<ProcInfo> enumProcesses();

std::vector<ProcInfo> findProcessesByName(std::string_view exeName);

// Toolhelp 模块快照。返回 false 表示进程不可访问或已退出。
struct ModuleInfo {
    std::string name;
    uint64_t    base = 0;
    uint32_t    size = 0;
};
bool enumModules(uint32_t pid, std::vector<ModuleInfo>& out);
std::optional<ModuleInfo> findModule(uint32_t pid, std::string_view name);

bool processAlive(uint32_t pid);
std::string processNameOf(uint32_t pid);
std::string processPathOf(uint32_t pid);

// 目标是否为 WOW64(32 位)进程: x64 注入器不能把 x64 DLL 塞进去。
bool isWow64Process(uint32_t pid);

uint64_t remoteAlloc(uint32_t pid, size_t size, uint32_t protect = 0x04 /*PAGE_READWRITE*/);
bool     remoteFree(uint32_t pid, uint64_t addr, uint32_t freeType = 0x8000 /*MEM_RELEASE*/);
bool     remoteWrite(uint32_t pid, uint64_t addr, void const* data, size_t size);
bool     remoteRead(uint32_t pid, uint64_t addr, void* out, size_t size);
// 只做 VirtualQueryEx, 不触发读异常。
bool     remoteReadable(uint32_t pid, uint64_t addr, size_t size);

enum class InjectStatus {
    ok,
    processNotFound,
    openProcessFailed,
    moduleNotFound,
    dllMissing,
    archMismatch,
    allocFailed,
    writeFailed,
    threadFailed,
    remoteLoadFailed,
    remoteTimeout,
};

std::string_view statusToString(InjectStatus s);

struct InjectResult {
    InjectStatus status = InjectStatus::processNotFound;
    uint32_t     win32Error = 0;
    uint64_t     remoteString = 0;
    uint64_t     remoteModule = 0;
    uint32_t     elapsedMs = 0;
    std::string  detail;
};

struct InjectOptions {
    uint32_t timeoutMs = 15000;
};

// 核心: CreateRemoteThread(LoadLibraryW)。dllPath 必须是绝对路径, 且在目标可见的卷上。
// 本框架只做注入不做卸载 —— 注入体一旦挂钩子/建线程, 半途 FreeLibrary 会留下悬空回调
// → 目标必崩; 要清掉注入体就重启目标进程。因此不提供 eject。
InjectResult injectDll(uint32_t pid, std::string const& dllPath, InjectOptions const& opt = {});

} // namespace epsilon
