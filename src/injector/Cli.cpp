// Cli.cpp — 参数解析、目标进程查找与注入器控制台 UI 的实现。
// 无参数 = 找运行中的 Dungeons2 → 注入 → 立即装 Present 钩子 → 进交互模式。
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

// 模糊匹配的前缀: 本游戏的引导程序是 Dungeons.exe(不带 Win64), 真正的游戏是
// Dungeons-Win64-Shipping.exe; 这个前缀把两者分开。
constexpr std::string_view gameImagePrefix = "dungeons-win64";

// 引导程序 / 崩溃上报 / 启动器 —— 长得像但绝不能注入。
constexpr std::string_view nonGameImageNames[] = {
    "dungeons.exe",
    "dungeons-win64-shipping-cmd.exe",
    "crashreportclient.exe",
    "unrealcefsubprocess.exe",
    "epicwebhelper.exe",
    "steam.exe",
    "steamwebhelper.exe",
};

std::string lowerBasename(std::string_view pathOrName) {
    const size_t slash = pathOrName.find_last_of("\\/");
    const std::string_view base =
        (slash == std::string_view::npos) ? pathOrName : pathOrName.substr(slash + 1);
    return toLower(base);
}

bool isNonGameImage(std::string const& lowerName) {
    for (auto const& n : nonGameImageNames) {
        if (lowerName == n) return true;
    }
    return false;
}

// 精确名命中优先; 没有精确命中才退回模糊匹配, 避免"两个都像"时选错。
std::vector<ProcInfo> findGameProcesses(std::string const& imageName, std::string* how) {
    if (auto exact = findProcessesByName(imageName); !exact.empty()) {
        if (how) *how = fmt("映像名精确匹配 {}", imageName);
        return exact;
    }

    std::vector<ProcInfo> fuzzy;
    for (auto const& p : enumProcesses()) {
        const std::string lower = lowerBasename(p.name);
        if (isNonGameImage(lower)) continue;
        if (!icontains(lower, gameImagePrefix)) continue;
        fuzzy.push_back(p);
    }
    if (how) {
        *how = fuzzy.empty() ? fmt("没有映像名含 \"{}\" 的进程", gameImagePrefix)
                             : fmt("映像名模糊匹配 \"{}\"", gameImagePrefix);
    }
    return fuzzy;
}

void printCandidate(std::string_view marker, ProcInfo const& p) {
    outLine(fmt("  {} {:<8} {:<32} {}", marker, p.pid, sanitize(p.name, 32),
                sanitize(p.path, 76)));
}

std::optional<Target> toTarget(ProcInfo const& p) {
    Target t;
    t.pid = p.pid;
    t.name = p.name;
    t.path = p.path.empty() ? processPathOf(p.pid) : p.path;
    return t;
}

} // namespace

void printUsage() {
    outLine("");
    outColored(ansi::bold, "  epsilonInjector — Minecraft Dungeons II 热注入器 (RemoteThread)\n");
    outLine("");
    outLine("  用法:");
    outLine("    epsilonInjector [选项] [--exec <命令>]...");
    outLine("");
    outLine("  不带任何参数 = 找运行中的 Dungeons2 → 注入 → 立即装 Present 钩子");
    outLine("              → 进入交互模式 (游戏没开就等 --find-wait 毫秒)。");
    outLine("");
    outLine("  目标选择:");
    outLine("    -l, --list              列出进程后退出 (配合 --filter)");
    outLine("        --filter <子串>     过滤进程名");
    outLine("    -p, --pid <PID>         直接指定 PID");
    outLine("    -n, --name <映像名>     按映像名找 (默认 Dungeons-Win64-Shipping.exe);");
    outLine("                            找不到会退回模糊匹配 \"dungeons-win64\"");
    outLine("        --find-wait <毫秒>  等游戏进程出现的上限 (默认 60000, 0 = 只查一次)");
    outLine("");
    outLine("  动作:");
    outLine("    -d, --dll <路径>        要注入的 DLL (默认同目录 epsilonPayload.dll)");
    outLine("        --no-hook           注入后不装 Present 钩子 (覆盖层/面板就没有)");
    outLine("    -x, --exec <命令>       注入后下发一条命令到注入体 (可重复)");
    outLine("    -i, --interactive       进交互模式 (默认; 给了 --exec 才需要显式写)");
    outLine("        --no-interactive    下完 --exec 就退出");
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
    outLine("  交互模式里可直接输入命令(exit 退出, injector-help 看注入器自己的):");
    outLine("    status                  引擎定位总览");
    outLine("    player                  关卡里的玩家候选与模块会选中的那个");
    outLine("    hooks                   帧钩子状态");
    outLine("    actors limit=20         枚举关卡 Actor");
    outLine("");
    outLine("  例:");
    outLine("    epsilonInjector                        # 一把梭: 找游戏 → 注入 → 挂钩 → 交互");
    outLine("    epsilonInjector -l --filter Dungeons   # 只看进程");
    outLine("    epsilonInjector -x status --no-interactive");
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
        else if (a == "-l" || a == "--list")         { o.listOnly = true; }
        else if (a == "-v" || a == "--verbose")      { o.verbose = true; }
        else if (a == "--no-color")                  { o.noColor = true; }
        else if (a == "-i" || a == "--interactive")  { o.interactive = true;  o.interactiveSet = true; }
        else if (a == "--no-interactive")            { o.interactive = false; o.interactiveSet = true; }
        else if (a == "--no-hook")                   { o.hook = false; }
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
        else if (a == "--find-wait") {
            auto v = needValue(a); if (!v) return std::nullopt;
            try { o.findWaitMs = static_cast<uint32_t>(std::stoul(*v)); }
            catch (...) { errLine("--find-wait 需要一个毫秒数"); return std::nullopt; }
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

    // 默认交互: 给了 --exec 就当成批处理脚本, 除非用户显式要交互。
    if (!o.interactiveSet) o.interactive = o.commands.empty();
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

    const uint64_t startTick = ::GetTickCount64();
    const uint64_t deadline = startTick + opt.findWaitMs;
    bool announced = false;
    uint64_t nextNotice = 0;

    for (;;) {
        std::string how;
        auto found = findGameProcesses(opt.targetName, &how);

        if (found.size() == 1) {
            if (announced) outLine("");
            if (opt.verbose) outLine(fmt("  目标定位   : {}", how));
            return toTarget(found.front());
        }
        if (found.size() > 1) {
            errLine(fmt("找到 {} 个候选进程, 用 --pid 指定其中一个:", found.size()));
            for (auto const& p : found) printCandidate("->", p);
            return std::nullopt;
        }

        const uint64_t now = ::GetTickCount64();
        if (now >= deadline) {
            if (announced) outLine("");
            errLine(fmt("找不到运行中的 {} (等了 {} 毫秒)", opt.targetName, opt.findWaitMs));
            outLine("先启动游戏, 或用 --list --filter Dungeons 看有哪些进程。");
            return std::nullopt;
        }

        if (!announced) {
            outLine(fmt("  正在等游戏进程 ({}; 最长 {:.0f} 秒, --find-wait 可调)...",
                         opt.targetName, static_cast<double>(opt.findWaitMs) / 1000.0));
            announced = true;
            nextNotice = now + 5000;
        } else if (now >= nextNotice) {
            outLine(fmt("  ...仍在等 (已等 {:.0f} 秒)",
                         static_cast<double>(now - startTick) / 1000.0));
            nextNotice = now + 5000;
        }
        ::Sleep(500);
    }
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
    outColored(ansi::gray, "  输入命令后回车; 注入体命令输 help, 注入器自己的输 injector-help;");
    outColored(ansi::gray, "  退出输 exit (或 Ctrl+C)。\n");
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

        // 注入器自己的命令: 注入体没有这两个, 直接拦下来本地处理。
        if (cmd == "injector-help" || cmd == "ih") {
            outLine("  注入器本地命令:");
            outLine("    ps [子串]          列出进程 (子串过滤)");
            outLine("    clear / cls        清屏");
            outLine("    injector-help      本帮助");
            outLine("    exit / quit        退出 (注入体仍驻留)");
            outLine("  其余输入原样下发给注入体, 例如 status / hooks / player / actors limit=20");
            continue;
        }
        if (cmd == "clear" || cmd == "cls") { ::system("cls"); continue; }
        if (cmd == "ps" || istartsWith(cmd, "ps ")) {
            cmdList(cmd.size() > 3 ? trim(cmd.substr(3)) : std::string(), false);
            continue;
        }

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
