// DllMain.cpp — 注入体入口
// DllMain 在加载器锁内被调用, 所以这里只创建一个线程就返回, 初始化全在线程里做。
// 细节见 docs/payload/lifecycle.md
#include "payload/Runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>      // GetModuleInformation (WIN32_LEAN_AND_MEAN 之下需显式带上)
#include <process.h>    // _beginthreadex

#include <string>

namespace {

HMODULE gSelf = nullptr;

std::wstring modulePathOf(HMODULE m) {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = ::GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

unsigned __stdcall startThread(void*) {
    epsilon::payload::runtimeMain(nullptr);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            gSelf = module;
            ::DisableThreadLibraryCalls(module);

            // ⚠️ 交给运行时的是**宿主进程主模块**(游戏 EXE)的基址/大小, 不是本 DLL 的
            //    —— 基线 RVA 全部相对 Dungeons-Win64-Shipping.exe, 传错则永远定位不到。
            HMODULE host = ::GetModuleHandleW(nullptr);
            MODULEINFO mi{};
            if (host && ::GetModuleInformation(::GetCurrentProcess(), host, &mi, sizeof(mi))) {
                epsilon::payload::setModuleInfo(
                    reinterpret_cast<uint64_t>(mi.lpBaseOfDll),
                    mi.SizeOfImage,
                    modulePathOf(host));
            } else {
                epsilon::payload::setModuleInfo(reinterpret_cast<uint64_t>(host), 0,
                                               modulePathOf(host));
            }

            // 必须 _beginthreadex 而非 CreateThread: 该线程大量使用 CRT
            // (string/format/mutex), CreateThread 不初始化 CRT 线程状态。
            uintptr_t t = _beginthreadex(nullptr, 0, &startThread, nullptr, 0, nullptr);
            if (t) {
                ::CloseHandle(reinterpret_cast<HANDLE>(t));
            } else {
                return FALSE;   // 线程都起不来, 宣告加载失败
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // reserved != nullptr 表示进程正在终止, 此时不宜做任何分配/等待。
            if (reserved == nullptr) {
                epsilon::payload::shutdownRuntime();
            }
            break;

        default:
            break;
    }
    return TRUE;
}
