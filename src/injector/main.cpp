// ============================================================================
//  main.cpp — 注入器主流程
//
//  整体节奏:
//    1. 解析参数 / 列表模式直接退出
//    2. 选目标进程
//    3. 起管道服务端, 并把管道名写进目标环境块(注入体在 DllMain 后读它)
//    4. CreateRemoteThread(LoadLibraryW) 注入
//    5. 等注入体报 ready
//    6. 下发命令 / 进入交互
// ============================================================================
#include "common/PipeChannel.h"
#include "common/ProcUtil.h"
#include "common/Text.h"
#include "injector/Cli.h"
#include "injector/PipeServer.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace epsilon {
namespace {

// 找同目录下的注入体 DLL。
std::string defaultPayloadPath() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
    const std::wstring dll = dir + L"\\epsilonPayload.dll";

    const int m = ::WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), static_cast<int>(dll.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, m)), '\0');
    if (m > 0) ::WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), static_cast<int>(dll.size()),
                                     s.data(), m, nullptr, nullptr);
    return s;
}

// 注入体把日志写在 %TEMP%\epsilonPayload_<pid>.log。
// 注入体在目标进程里没有控制台, 排查问题时这份日志是唯一线索, 所以
// 任何等待失败都把它指出来。
std::string payloadLogPath(uint32_t pid) {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    const std::string dir = (n > 0) ? toUtf8(std::wstring_view(tmp, n)) : std::string(".");
    return fmt("{}epsilonPayload_{}.log", dir, pid);
}

// 注入器是控制台程序, Ctrl+C 会让它直接死掉, 留下一堆没清理的句柄。
// 交给系统回收即可 —— 但要在退出前把 bye 发给注入体, 所以装一个简单的处理器。
volatile BOOL gInterrupted = FALSE;

BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        gInterrupted = TRUE;
        return TRUE;      // 已处理, 不要立刻终止进程
    }
    return FALSE;
}

} // namespace
} // namespace epsilon

int main(int argc, char** argv) {
    using namespace epsilon;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    auto opt = parseArgs(argc, argv);
    if (!opt) return 2;

    consoleInit(true);
    setColorEnabled(!opt->noColor);
    ::SetConsoleCtrlHandler(&ctrlHandler, TRUE);

    // --log: 让程序自己写文件。提权运行时这是唯一可靠的取回输出的方式 ——
    // 提权启动的 shell 重定向在 UAC 之后常常静默失效。
    if (!opt->logPath.empty()) {
        if (logOpen(opt->logPath)) {
            outLine(fmt("  [*] 输出镜像到: {}", opt->logPath));
        } else {
            errLine(fmt("  [!] 无法打开日志文件: {}", opt->logPath));
        }
    }

    uiBanner();

    // ---------------------------------------------------------------- 列表模式
    if (opt->listOnly) {
        cmdList(opt->filter, opt->verbose);
        return 0;
    }

    // ---------------------------------------------------------------- 选目标
    auto target = pickTarget(*opt);
    if (!target) return 3;

    outLine(fmt("  目标进程   : {} (PID {})", target->name, target->pid));
    outLine(fmt("  映像路径   : {}", target->path.empty() ? "(未知)" : target->path));

    // 目标模块基址 —— 记录基线, 注入后好做差异对比
    const auto modBefore = findModule(target->pid, opt->targetName);
    if (modBefore) {
        outLine(fmt("  模块基址   : {} ({} 字节)", hex(modBefore->base, 16),
                     humanBytes(modBefore->size)));
    }

    // 解析 DLL 路径
    std::string dll = opt->dll.empty() ? defaultPayloadPath() : opt->dll;
    if (dll.size() >= 2 && dll[1] != ':') {
        // 相对路径补成绝对路径 —— 目标进程用不了相对路径
        char full[MAX_PATH * 4]{};
        if (::GetFullPathNameA(dll.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr)) {
            dll = full;
        }
    }

    // ---------------------------------------------------------------- 管道
    // 约定名: 由目标 PID 推导。注入体用自己的 PID 算出同一名字, 无需传参。
    // 注入器**只做注入**, 不做卸载 —— 见 procUtil.h 里的说明。
    InjectorChannel channel;
    {
        std::string err;
        if (!channel.start(target->pid, &err)) {
            errLine(fmt("  创建通信管道失败: {}", err));
            return 5;
        }
        if (opt->verbose) {
            outLine(fmt("  通信管道   : {} (约定名, 由目标 PID 推导)",
                         toUtf8(channel.pipeName())));
        }
    }

    // ---------------------------------------------------------------- 注入
    outLine("");
    outColored(ansi::bold, "  正在注入 (CreateRemoteThread + LoadLibraryW)...\n");
    outLine(fmt("  DLL        : {}", dll));

    const InjectResult r = injectDll(target->pid, dll);
    if (r.status != InjectStatus::ok) {
        outColored(ansi::red, fmt("  [x] 注入失败: {}\n", statusToString(r.status)));
        if (!r.detail.empty()) outLine(fmt("      {}", r.detail));
        if (r.win32Error) {
            outLine(fmt("      Win32 错误: {} ({})", r.win32Error,
                         std::system_category().message(static_cast<int>(r.win32Error))));
        }
        if (opt->verbose) {
            outLine(fmt("      远端参数串: {}", hex(r.remoteString, 16)));
        }
        return 6;
    }

    outColored(ansi::green, fmt("  [+] 注入成功 ({} ms)\n", r.elapsedMs));
    outLine(fmt("      远端 HMODULE : {}", hex(r.remoteModule, 16)));

    // 差异: 注入后再看模块列表, 确认 DLL 真的进去了
    if (auto modAfter = findModule(target->pid, "epsilonPayload.dll")) {
        outColored(ansi::green, fmt("  [+] 模块已加载 : {} ({} 字节)\n",
                                    hex(modAfter->base, 16), humanBytes(modAfter->size)));
    } else if (opt->verbose) {
        outLine("      (Toolhelp 里暂未看到模块快照, 通常稍等即出现)");
    }

    // ---------------------------------------------------------------- 等就绪
    outLine("");
    outLine("  等待注入体初始化 (连管道 / 定位 GObjects+GNames / 重建属性偏移)...");
    if (channel.waitReady(opt->waitReadyMs)) {
        outColored(ansi::green, "  [+] 注入体已就绪\n");
    } else {
        outColored(ansi::yellow, fmt("  [!] {}\n", channel.lastError()));
        outLine(fmt("      注入体日志: {}", payloadLogPath(target->pid)));
        outLine("      若目标进程已消失, 日志最后一行就是崩溃点。");
    }

    // ---------------------------------------------------------------- 命令
    int rc = 0;
    if (!opt->commands.empty()) {
        outLine("");
        for (auto const& c : opt->commands) {
            if (!channel.connected()) {
                errLine("  管道未连接, 无法下发命令。");
                rc = 7;
                break;
            }
            if (!channel.send(c)) {
                errLine(fmt("  下发失败: {}", c));
                rc = 7;
                break;
            }
            // 给注入体一点时间把响应写完。命令是同步执行的, 但没有完成通知,
            // 所以这里用固定小延迟 —— 采集类命令都很快。
            ::Sleep(400);
        }
    }

    if (opt->interactive && channel.connected()) {
        if (!interactiveShell(channel.pipe())) rc = 0;
    }

    channel.stop();
    return rc;
}
