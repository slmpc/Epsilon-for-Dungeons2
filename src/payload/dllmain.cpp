// ============================================================================
//  dllmain.cpp — 注入体入口
//
//  远程线程注入把 LoadLibraryW 当线程入口, 所以我们的 DllMain 会在
//  CreateRemoteThread 的那个线程上被调用 —— 此时持有加载器锁。
//  因此 DllMain 里**只做一件事**: 创建一个线程, 立刻返回。
//  所有真正的初始化(定引擎、装钩子、开控制台、连管道)都在那个线程里做。
// ============================================================================
#include "payload/runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>      // GetModuleInformation / MODULEINFO
                        // (WIN32_LEAN_AND_MEAN 之后 windows.h 不再带入 psapi.h)

#include <string>

namespace {

HMODULE g_self = nullptr;

std::wstring self_path() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = ::GetModuleFileNameW(g_self, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

DWORD WINAPI start_thread(LPVOID) {
    mcd2::payload::runtime_main(nullptr);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = module;
            ::DisableThreadLibraryCalls(module);

            // 把自己所在模块的基址/大小交给运行时 —— 后面所有扫描都基于它。
            MODULEINFO mi{};
            if (::GetModuleInformation(::GetCurrentProcess(), module, &mi, sizeof(mi))) {
                mcd2::payload::set_module_info(
                    reinterpret_cast<uint64_t>(mi.lpBaseOfDll),
                    mi.SizeOfImage,
                    self_path());
            } else {
                mcd2::payload::set_module_info(reinterpret_cast<uint64_t>(module), 0, self_path());
            }

            // 只创建线程, 不在加载锁里做任何重活。
            HANDLE t = ::CreateThread(nullptr, 0, &start_thread, nullptr, 0, nullptr);
            if (t) {
                ::CloseHandle(t);
            } else {
                return FALSE;   // 线程都起不来, 宣告加载失败
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // 被 FreeLibrary / 进程退出时走到这里。
            // reserved != nullptr 表示进程正在终止, 此时不宜做任何分配/等待。
            if (reserved == nullptr) {
                mcd2::payload::shutdown_runtime();
            }
            break;

        default:
            break;
    }
    return TRUE;
}
