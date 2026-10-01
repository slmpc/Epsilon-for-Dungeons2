// Main.cpp — 注入器主流程: 找目标 → 起管道 → 注入 → 等 ready → 装 Present 钩子 → 交互。
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

#include <algorithm>
#include <cstdio>
#include <string>

namespace epsilon {
namespace {

// 命令是同步执行但没有完成通知 —— 用固定小延迟等响应写完。
constexpr DWORD commandSettleMs = 400;

// 游戏进程完整性级别更高(它自己提权), 本进程若不是 High 就必然 OpenProcess 失败(5)。
// 提前判掉: 否则会先白等一轮找进程, 最后只给一个 err=5。
bool processElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

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

// 下发一串命令, 每条之间留出响应写回的时间。
bool runCommands(InjectorChannel& channel, std::vector<std::string> const& commands) {
    for (auto const& c : commands) {
        if (!channel.connected()) {
            errLine("  管道未连接, 无法下发命令。");
            return false;
        }
        if (!channel.send(c)) {
            errLine(fmt("  下发失败: {}", c));
            return false;
        }
        ::Sleep(commandSettleMs);
    }
    return true;
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

    consoleInit(true);

    auto opt = parseArgs(argc, argv);
    if (!opt) return 2;

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

    // -l 只是枚举进程, 不需要提权; 真注入前才拦。
    if (!opt->listOnly && !processElevated()) {
        errLine("  [x] 需要管理员权限 —— 游戏进程完整性级别更高, 不提权 OpenProcess 只会返回 5。");
        outLine("      用管理员身份的终端启动; 或直接双击本 EXE(清单是 requireAdministrator, 会弹 UAC)。");
        return 8;
    }

    if (opt->listOnly) {
        cmdList(opt->filter, opt->verbose);
        return 0;
    }

    auto target = pickTarget(*opt);
    if (!target) return 3;

    outLine(fmt("  目标进程   : {} (PID {})", target->name, target->pid));
    outLine(fmt("  映像路径   : {}", target->path.empty() ? "(未知)" : target->path));

    const auto modBefore = findModule(target->pid, target->name);
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

    // ---- Present 钩子: 默认就装 ----
    // 走注入体自己的 `hook` 命令(与手工敲 hook 同一条路), 而不是等它的 10 秒自动安装,
    // 这样"注入完就有覆盖层"。注入体退避的自动安装仍然保留 —— 手工执行注入体 DLL
    // (不经本注入器)时它是唯一的安装途径。
    int rc = 0;
    if (opt->hook && channel.connected()) {
        outLine("");
        outLine("  安装 Present 钩子 (覆盖层/面板需要)...");
        if (!runCommands(channel, {"hook"})) rc = 7;
    } else if (!opt->hook) {
        outLine("");
        outLine("  (--no-hook: 跳过 Present 钩子; 需要时可手工敲 hook)");
    }

    if (!opt->commands.empty()) {
        outLine("");
        if (!runCommands(channel, opt->commands)) rc = 7;
    }

    if (opt->interactive && channel.connected()) {
        interactiveShell(channel.pipe());
    } else if (opt->interactive) {
        errLine("  管道未连接, 不进交互模式。");
        rc = 7;
    }

    channel.stop();
    return rc;
}
