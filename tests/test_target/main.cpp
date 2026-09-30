// ============================================================================
//  test_target/main.cpp — 注入自测靶子
//
//  一个什么都不做的 x64 控制台进程。用途: 在不碰游戏的前提下验证
//  「OpenProcess → VirtualAllocEx → WriteProcessMemory → CreateRemoteThread」
//  整条链路, 以及注入体的输出通道切换。
//
//  用法:
//    build/bin/mcd2_testtarget.exe
//    build/bin/mcd2_injector.exe -n mcd2_testtarget.exe -i
//
//  它不加载 DXGI/D3D, 所以 Present 钩子一定失败 —— 这正好验证了"钩子失败
//  不影响采集"这条设计。也不含 UE 反射, 所以引擎定位一定失败 —— 验证
//  "注入成功但定位失败"时的降级路径不崩。
// ============================================================================
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

int main() {
    mcd2::console_init(true);

    mcd2::out_colored(mcd2::ansi::cyan,
        "==================================================\n"
        "  mcd2_testtarget — 注入自测靶子\n"
        "==================================================\n");
    mcd2::out_line(mcd2::fmt("  PID      : {}", ::GetCurrentProcessId()));
    mcd2::out_line(mcd2::fmt("  映像名   : mcd2_testtarget.exe"));
    mcd2::out_line("");
    mcd2::out_line("  这个进程故意什么都不做。用它来验证注入链路:");
    mcd2::out_line("    mcd2_injector -n mcd2_testtarget.exe -i");
    mcd2::out_line("");
    mcd2::out_line("  它没有 D3D / UE 反射, 所以 Present 钩子与引擎定位都会失败 ——");
    mcd2::out_line("  这正是要验证的: 失败必须被优雅降级, 不能崩。");
    mcd2::out_line("");
    mcd2::out_line("  按 Ctrl+C 或关闭窗口退出。");

    // 每 10 秒打一行心跳, 方便确认进程还活着 / 注入后有没有被搞崩。
    for (int i = 0;; ++i) {
        ::Sleep(10000);
        mcd2::out_colored(mcd2::ansi::gray, mcd2::fmt("  [心跳 {}] 进程存活, 已运行 {} 秒\n",
                                                      i + 1, (i + 1) * 10));
    }
    return 0;
}
