// testTarget/Main.cpp — 注入自测靶子: 一个什么都不做的 x64 控制台进程。
// 不碰游戏验证整条注入链路(OpenProcess → VirtualAllocEx → WriteProcessMemory →
// CreateRemoteThread → LoadLibraryW), 以及无 D3D / 无 UE 反射时的优雅降级。
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

int main() {
    epsilon::consoleInit(true);

    epsilon::outColored(epsilon::ansi::cyan,
        "==================================================\n"
        "  epsilonTestTarget — 注入自测靶子\n"
        "==================================================\n");
    epsilon::outLine(epsilon::fmt("  PID      : {}", ::GetCurrentProcessId()));
    epsilon::outLine(epsilon::fmt("  映像名   : epsilonTestTarget.exe"));
    epsilon::outLine("");
    epsilon::outLine("  这个进程故意什么都不做。用它来验证注入链路:");
    epsilon::outLine("    epsilonInjector -n epsilonTestTarget.exe -i");
    epsilon::outLine("");
    epsilon::outLine("  它没有 D3D / UE 反射, 所以 Present 钩子与引擎定位都会失败 ——");
    epsilon::outLine("  这正是要验证的: 失败必须被优雅降级, 不能崩。");
    epsilon::outLine("");
    epsilon::outLine("  按 Ctrl+C 或关闭窗口退出。");

    for (int i = 0;; ++i) {
        ::Sleep(10000);
        epsilon::outColored(epsilon::ansi::gray, epsilon::fmt("  [心跳 {}] 进程存活, 已运行 {} 秒\n",
                                                      i + 1, (i + 1) * 10));
    }
    return 0;
}
