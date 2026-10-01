// Cli.cpp — 参数解析、进程列表与注入器控制台 UI 的实现。
#include "injector/Cli.h"

#include "common/PipeChannel.h"
#include "common/ProcUtil.h"
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace epsilon {
namespace {

constexpr std::string_view kProgram = "epsilonInjector";

} // namespace

void printUsage() {
    outLine("");
    outColored(ansi::bold, "  epsilonInjector — Minecraft Dungeons II 热注入器 (RemoteThread)\n");
    outLine("");
    outLine("  用法:");
    outLine("    epsilonInjector [选项] [--exec <命令>]...");
    outLine("");
    outLine("  目标选择:");
    outLine("    -l, --list              列出进程后退出 (配合 --filter)");
    outLine("        --filter <子串>     过滤进程名");
    outLine("    -p, --pid <PID>         直接指定 PID");
    outLine("    -n, --name <映像名>     按映像名找 (默认 Dungeons-Win64-Shipping.exe)");
    outLine("");
    outLine("  动作:");
    outLine("    -d, --dll <路径>        要注入的 DLL (默认同目录 epsilonPayload.dll)");
    outLine("    -x, --exec <命令>       注入后下发一条命令到注入体 (可重复)");
    outLine("    -i, --interactive       注入后进入交互式命令行");
    outLine("        --wait <毫秒>       等注入体就绪的上限 (默认 20000)");
    outLine("");
    outLine("  说明: 本工具**只做注入**。不提供卸载 —— 注入体挂钩子/起线程后");
    outLine("        半途 FreeLibrary 会留下悬空回调, 目标必崩。要清除就重启");
    outLine("        目标进程。");
    outLine("");
    outLine("  其它:");
    outLine("    -v, --verbose           打印更多过程信息");
    outLine("        --no-color          关闭彩色输出");
    outLine("        --log <路径>        把输出同时写一份到文件");
    outLine("                            (提权运行时最可靠 —— 提权启动的 shell");
    outLine("                             重定向在 UAC 之后常常静默失效)");
    outLine("    -h, --help              显示本帮助");
    outLine("");
    outLine("  注入后进入交互模式时可直接输入命令, 例如:");
    outLine("    status                  引擎定位总览");
    outLine("    props Character         列出 Character 类的属性与偏移");
    outLine("    actors limit=20         枚举关卡 Actor");
    outLine("    get PlayerController MyPawn   读某个属性");
    outLine("");
    outLine("  例:");
    outLine("    epsilonInjector -l --filter Dungeons");
    outLine("    epsilonInjector --exec status --exec \"actors limit=30\"");
    outLine("    epsilonInjector -i");
    outLine("");
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto needValue = [&](std::string_view flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                errLine(fmt("{} 需要一个参数值", flag));
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (a == "-h" || a == "--help") { printUsage(); return std::nullopt; }
        else if (a == "-l" || a == "--list")        { o.listOnly = true; }
        else if (a == "-v" || a == "--verbose")     { o.verbose = true; }
        else if (a == "--no-color")                 { o.noColor = true; }
        else if (a == "-i" || a == "--interactive") { o.interactive = true; }
        else if (a == "--filter")  {
            auto v = needValue(a); if (!v) return std::nullopt; o.filter = *v;
        }
        else if (a == "-n" || a == "--name") {
            auto v = needValue(a); if (!v) return std::nullopt; o.targetName = *v;
        }
        else if (a == "-p" || a == "--pid") {
            auto v = needValue(a); if (!v) return std::nullopt;
            try { o.pid = static_cast<uint32_t>(std::stoul(*v, nullptr, 0)); }
            catch (...) { errLine(fmt("PID 不是数字: {}", *v)); return std::nullopt; }
        }
        else if (a == "-d" || a == "--dll") {
            auto v = needValue(a); if (!v) return std::nullopt; o.dll = *v;
        }
        else if (a == "-x" || a == "--exec") {
            auto v = needValue(a); if (!v) return std::nullopt; o.commands.push_back(*v);
        }
        else if (a == "--wait") {
            auto v = needValue(a); if (!v) return std::nullopt;
            try { o.waitReadyMs = static_cast<uint32_t>(std::stoul(*v)); }
            catch (...) { errLine("--wait 需要一个毫秒数"); return std::nullopt; }
        }
        else if (a == "--log") {
            auto v = needValue(a); if (!v) return std::nullopt; o.logPath = *v;
        }
        else {
            errLine(fmt("未知参数: {}", a));
            outLine("用 --help 看用法。");
            return std::nullopt;
        }
    }
    return o;
}

void cmdList(std::string_view filter, bool verbose) {
    const auto procs = enumProcesses();
    outColored(ansi::bold, fmt("  进程列表 (共 {})\n", procs.size()));
    outLine(fmt("  {:<8} {:<36} {}", "PID", "映像名", "路径"));
    outLine("  " + std::string(100, '-'));

    size_t shown = 0;
    for (auto const& p : procs) {
        if (!filter.empty() && !icontains(p.name, filter)) continue;
        outLine(fmt("  {:<8} {:<36} {}", p.pid, sanitize(p.name, 36), 
                    verbose ? sanitize(p.path, 70) : ""));
        ++shown;
    }
    outLine(fmt("  --- 匹配 {} 个 ---", shown));
}

std::optional<Target> pickTarget(Options const& opt) {
    if (opt.pid) {
        if (!processAlive(opt.pid)) {
            errLine(fmt("PID {} 不存在或已退出", opt.pid));
            return std::nullopt;
        }
        Target t;
        t.pid = opt.pid;
        t.name = processNameOf(opt.pid);
        t.path = processPathOf(opt.pid);
        return t;
    }

    const auto found = findProcessesByName(opt.targetName);
    if (found.empty()) {
        errLine(fmt("找不到运行中的 {}", opt.targetName));
        outLine("用 --list --filter <子串> 看看有哪些进程, 或用 --name 指定别的映像名。");
        return std::nullopt;
    }
    if (found.size() > 1) {
        errLine(fmt("{} 有 {} 个实例, 用 --pid 指定:", opt.targetName, found.size()));
        for (auto const& p : found) outLine(fmt("    --pid {}", p.pid));
        return std::nullopt;
    }

    Target t;
    t.pid = found.front().pid;
    t.name = found.front().name;
    t.path = found.front().path;
    return t;
}

void uiBanner() {
    outColored(ansi::cyan, "\n  ============================================================\n");
    outColored(ansi::cyan,   "   Epsilon For Dungeons II 热注入器 — RemoteThread (CreateRemoteThread + LoadLibraryW)\n");
    outColored(ansi::cyan,   "  ============================================================\n\n");
}

void uiCommandEcho(std::string_view cmd) {
    outColored(ansi::yellow, "epsilon> ");
    outColored(ansi::bold, std::string(cmd));
    out("\n");
}

void uiPayloadData(std::string_view text) {
    // 注入体已经排好版, 原样透出
    out(text);
}

void uiPayloadStatus(std::string_view text) {
    outColored(ansi::gray, fmt("  [注入体] {}\n", text));
}

void uiPayloadError(std::string_view text) {
    outColored(ansi::red, fmt("  [注入体:错误] {}\n", text));
}

bool interactiveShell(PipeServer& pipe) {
    outLine("");
    outColored(ansi::gray, "  输入命令后回车; 帮助输 help, 退出输 quit (或 Ctrl+C)。\n");
    outLine("");

    std::string line;
    for (;;) {
        outColored(ansi::yellow, "epsilon> ");
        line.clear();
        char c = 0;
        HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
        bool eof = false;
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(in, &c, 1, &got, nullptr) || got == 0) { eof = true; break; }
            if (c == '\n') break;
            if (c == '\r') continue;
            if (c == 3) { eof = true; break; }      // Ctrl+C
            line += c;
        }
        if (eof && line.empty()) {
            outLine("");
            return false;
        }

        const std::string cmd = trim(line);
        if (cmd.empty()) continue;
        if (cmd == "quit" || cmd == "exit") return false;

        if (cmd == "clear" || cmd == "cls") { ::system("cls"); continue; }
        if (cmd == "list") { cmdList("", false); continue; }

        if (!pipe.connected()) {
            errLine("与注入体的管道已断开, 无法下发命令。");
            return false;
        }
        if (!pipe.sendCommand(cmd)) {
            errLine("命令下发失败。");
            return false;
        }
    }
}

} // namespace epsilon
