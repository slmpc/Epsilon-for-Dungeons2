// Main.cpp — 注入器主流程: 选目标 → 起管道 → 注入 → 等 ready → 下发命令。
// 排查见 docs/dev/diagnostics.md。
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

// 注入体的文件日志路径(它在目标进程里没有控制台, 等待失败时要把它指出来)。
std::string payloadLogPath(uint32_t pid) {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    const std::string dir = (n > 0) ? toUtf8(std::wstring_view(tmp, n)) : std::string(".");
    return fmt("{}epsilonPayload_{}.log", dir, pid);
}

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

    // 提权启动的 shell 重定向在 UAC 之后常常静默失效, 所以让程序自己写文件。
    if (!opt->logPath.empty()) {
        if (logOpen(opt->logPath)) {
            outLine(fmt("  [*] 输出镜像到: {}", opt->logPath));
        } else {
            errLine(fmt("  [!] 无法打开日志文件: {}", opt->logPath));
        }
    }

    uiBanner();

    if (opt->listOnly) {
        cmdList(opt->filter, opt->verbose);
        return 0;
    }

    auto target = pickTarget(*opt);
    if (!target) return 3;

    outLine(fmt("  目标进程   : {} (PID {})", target->name, target->pid));
    outLine(fmt("  映像路径   : {}", target->path.empty() ? "(未知)" : target->path));

    const auto modBefore = findModule(target->pid, opt->targetName);
    if (modBefore) {
        outLine(fmt("  模块基址   : {} ({} 字节)", hex(modBefore->base, 16),
                     humanBytes(modBefore->size)));
    }

    std::string dll = opt->dll.empty() ? defaultPayloadPath() : opt->dll;
    if (dll.size() >= 2 && dll[1] != ':') {
        // 目标进程用不了相对路径, 补成绝对路径。
        char full[MAX_PATH * 4]{};
        if (::GetFullPathNameA(dll.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr)) {
            dll = full;
        }
    }

    // 约定名: 由目标 PID + 被注入 DLL 的文件名推导, 注入体用同样两样东西算出同一名字。
    // 槽位带上 DLL 名, 所以不同文件名的注入体可在同一进程并存 —— 换名即可注入, 不必重启游戏。
    InjectorChannel channel;
    {
        std::string err;
        if (!channel.start(target->pid, toUtf16(dll), &err)) {
            errLine(fmt("  创建通信管道失败: {}", err));
            return 5;
        }
        if (opt->verbose) {
            outLine(fmt("  通信管道   : {} (约定名, 由目标 PID 推导)",
                         toUtf8(channel.pipeName())));
        }
    }

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

    if (auto modAfter = findModule(target->pid, "epsilonPayload.dll")) {
        outColored(ansi::green, fmt("  [+] 模块已加载 : {} ({} 字节)\n",
                                    hex(modAfter->base, 16), humanBytes(modAfter->size)));
    } else if (opt->verbose) {
        outLine("      (Toolhelp 里暂未看到模块快照, 通常稍等即出现)");
    }

    outLine("");
    outLine("  等待注入体初始化 (连管道 / 定位 GObjects+GNames / 重建属性偏移)...");
    if (channel.waitReady(opt->waitReadyMs)) {
        outColored(ansi::green, "  [+] 注入体已就绪\n");
    } else {
        outColored(ansi::yellow, fmt("  [!] {}\n", channel.lastError()));
        outLine(fmt("      注入体日志: {}", payloadLogPath(target->pid)));
        outLine("      若目标进程已消失, 日志最后一行就是崩溃点。");
    }

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
            // 命令是同步执行的但没有完成通知, 用固定小延迟等响应写完。
            ::Sleep(400);
        }
    }

    if (opt->interactive && channel.connected()) {
        if (!interactiveShell(channel.pipe())) rc = 0;
    }

    channel.stop();
    return rc;
}
