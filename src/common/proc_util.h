// ============================================================================
//  proc_util.h — 进程发现 / 远程内存 / RemoteThread 注入
//
//  这是注入器的主干。整条注入路径只有 5 个内核调用:
//      OpenProcess → VirtualAllocEx → WriteProcessMemory
//                  → CreateRemoteThread(LoadLibraryW) → WaitForSingleObject
//  没有驱动、没有手动映射、没有隐蔽处理 —— 目标没有反作弊, 用最经典可靠的路径。
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcd2 {

// ---------------------------------------------------------------- 进程 / 模块
struct ProcInfo {
    uint32_t    pid = 0;
    std::string name;        // 映像名, 如 Dungeons-Win64-Shipping.exe
    std::string path;        // 完整路径(取不到则为空)
};

// 会话内全部可见进程(按名字排序)。
std::vector<ProcInfo> enum_processes();

// 按映像名精确匹配(不区分大小写)找所有实例。
std::vector<ProcInfo> find_processes_by_name(std::string_view exe_name);

// Toolhelp 模块快照。返回 false 表示进程不可访问或已退出。
struct ModuleInfo {
    std::string name;
    uint64_t    base = 0;
    uint32_t    size = 0;
};
bool enum_modules(uint32_t pid, std::vector<ModuleInfo>& out);
std::optional<ModuleInfo> find_module(uint32_t pid, std::string_view name);

bool process_alive(uint32_t pid);
std::string process_name_of(uint32_t pid);
std::string process_path_of(uint32_t pid);

// 目标是否为 WOW64(32 位)进程。x64 注入器不能把 x64 DLL 塞进 WOW64 进程。
bool is_wow64_process(uint32_t pid);

// ---------------------------------------------------------------- 远程内存
uint64_t remote_alloc(uint32_t pid, size_t size, uint32_t protect = 0x04 /*PAGE_READWRITE*/);
bool     remote_free(uint32_t pid, uint64_t addr, uint32_t free_type = 0x8000 /*MEM_RELEASE*/);
bool     remote_write(uint32_t pid, uint64_t addr, void const* data, size_t size);
bool     remote_read(uint32_t pid, uint64_t addr, void* out, size_t size);
// 目标进程里某段内存是否可读(用于扫描/校验, 不触发读异常)。
bool     remote_readable(uint32_t pid, uint64_t addr, size_t size);

// ---------------------------------------------------------------- 注入 / 卸载
enum class InjectStatus {
    ok,
    process_not_found,
    open_process_failed,
    module_not_found,
    dll_missing,
    arch_mismatch,
    alloc_failed,
    write_failed,
    thread_failed,
    remote_load_failed,
    remote_timeout,
};

std::string_view to_string(InjectStatus s);

struct InjectResult {
    InjectStatus status = InjectStatus::process_not_found;
    uint32_t     win32_error = 0;
    uint64_t     remote_string = 0;   // 参数串在目标里的落地地址
    uint64_t     remote_module = 0;   // LoadLibrary 返回的 HMODULE(0 = 失败)
    uint32_t     elapsed_ms = 0;
    std::string  detail;
};

struct InjectOptions {
    uint32_t timeout_ms = 15000;      // 等远端线程返回的上限
};

// 核心: CreateRemoteThread(LoadLibraryW)。
// dll_path 必须是绝对路径, 且在目标进程可见的卷上。全 ASCII 路径最稳。
//
// 注意: 本框架**只做注入, 不做卸载**。设计上就是"注进去让它跑到进程结束":
//   * 注入体一旦挂钩子/建线程, 半途 FreeLibrary 会留下悬空回调 → 目标必崩
//   * 真要靠 FreeLibrary 干净卸载, 需要注入体自己先把一切还原并通知宿主,
//     那是另一套生命周期协议, 不在本框架范围内
// 因此这里不提供 eject。要清掉注入体, 重启游戏进程即可。
InjectResult inject_dll(uint32_t pid, std::string const& dll_path, InjectOptions const& opt = {});

} // namespace mcd2
