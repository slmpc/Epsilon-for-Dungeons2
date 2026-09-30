// ============================================================================
//  dllmain.cpp — 注入体入口
//
//  远程线程注入把 LoadLibraryW 当线程入口, 所以我们的 DllMain 会在
//  CreateRemoteThread 的那个线程上被调用 —— 此时持有加载器锁。
//  因此 DllMain 里**只做一件事**: 创建一个线程, 立刻返回。
//  所有真正的初始化(定引擎、装钩子、开控制台、连管道)都在那个线程里做。
// ============================================================================
#include "payload/Runtime.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>      // GetModuleInformation / MODULEINFO
                        // (WIN32_LEAN_AND_MEAN 之后 windows.h 不再带入 psapi.h)
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

            // ⚠️ 交给运行时的是**宿主进程主模块**(即游戏 EXE)的基址/大小,
            //    不是我们自己这个 DLL 的。
            //
            //    基线 RVA(GObjects 0x0BEA8BF0 / GNames 0x0BDC5040 / ...)全部
            //    是相对 Dungeons-Win64-Shipping.exe 的。之前这里传的是注入体
            //    自己的模块, 于是候选地址被算成 "注入体基址 + RVA", 偏出去
            //    几十 MB, 名字池校验必然 0/64 失败 —— 现象是"注入成功但引擎
            //    永远定位不到"。
            //
            //    GetModuleHandleW(nullptr) 返回的就是进程主模块。
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

            // 只创建线程, 不在加载锁里做任何重活。
            //
            // 用 _beginthreadex 而不是 CreateThread: 这个线程会大量使用 CRT
            // (std::string / std::format / std::mutex / std::function)。
            // CreateThread 不初始化 CRT 的线程级状态, 在静态链接 CRT 的 DLL 里
            // 会导致不可预测的崩溃 —— 而且是延迟发生的, 极难定位。
            uintptr_t t = _beginthreadex(nullptr, 0, &startThread, nullptr, 0, nullptr);
            if (t) {
                ::CloseHandle(reinterpret_cast<HANDLE>(t));
            } else {
                return FALSE;   // 线程都起不来, 宣告加载失败
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // 被 FreeLibrary / 进程退出时走到这里。
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
