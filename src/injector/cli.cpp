// ============================================================================
//  cli.cpp
// ============================================================================
#include "injector/cli.h"

#include "common/pipe_channel.h"
#include "common/proc_util.h"
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace mcd2 {
namespace {

constexpr std::string_view kProgram = "mcd2_injector";

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return ".";
    const std::wstring dir = p.substr(0, slash);
    const int m = ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, m)), '\0');
    if (m > 0) ::WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                     s.data(), m, nullptr, nullptr);
    return s;
}

// 把相对路径补成绝对路径(目标进程需要绝对路径才能 LoadLibrary)。
std::string to_absolute(std::string const& p) {
    if (p.size() >= 2 && p[1] == ':') return p;         // 已是盘符绝对路径
    if (p.rfind("\\\\", 0) == 0) return p;              // UNC
    return exe_dir() + "\\" + p;
}

} // namespace

// ---------------------------------------------------------------------------
void print_usage() {
    out_line("");
    out_colored(ansi::bold, "  mcd2_injector — Minecraft Dungeons II 热注入器 (RemoteThread)\n");
    out_line("");
    out_line("  用法:");
    out_line("    mcd2_injector [选项] [--exec <命令>]...");
    out_line("");
    out_line("  目标选择:");
    out_line("    -l, --list              列出进程后退出 (配合 --filter)");
    out_line("        --filter <子串>     过滤进程名");
    out_line("    -p, --pid <PID>         直接指定 PID");
    out_line("    -n, --name <映像名>     按映像名找 (默认 Dungeons-Win64-Shipping.exe)");
    out_line("");
    out_line("  动作:");
    out_line("    -d, --dll <路径>        要注入的 DLL (默认同目录 mcd2_payload.dll)");
    out_line("    -x, --exec <命令>       注入后下发一条命令到注入体 (可重复)");
    out_line("    -i, --interactive       注入后进入交互式命令行");
    out_line("        --wait <毫秒>       等注入体就绪的上限 (默认 20000)");
    out_line("");
    out_line("  说明: 本工具**只做注入**。不提供卸载 —— 注入体挂钩子/起线程后");
    out_line("        半途 FreeLibrary 会留下悬空回调, 目标必崩。要清除就重启");
    out_line("        目标进程。");
    out_line("");
    out_line("  其它:");
    out_line("    -v, --verbose           打印更多过程信息");
    out_line("        --no-color          关闭彩色输出");
    out_line("    -h, --help              显示本帮助");
    out_line("");
    out_line("  注入后进入交互模式时可直接输入命令, 例如:");
    out_line("    status                  引擎定位总览");
    out_line("    props Character         列出 Character 类的属性与偏移");
    out_line("    actors limit=20         枚举关卡 Actor");
    out_line("    get PlayerController MyPawn   读某个属性");
    out_line("");
    out_line("  例:");
    out_line("    mcd2_injector -l --filter Dungeons");
    out_line("    mcd2_injector --exec status --exec \"actors limit=30\"");
    out_line("    mcd2_injector -i");
    out_line("");
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need_value = [&](std::string_view flag) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                err_line(fmt("{} 需要一个参数值", flag));
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (a == "-h" || a == "--help") { print_usage(); return std::nullopt; }
        else if (a == "-l" || a == "--list")        { o.list_only = true; }
        else if (a == "-v" || a == "--verbose")     { o.verbose = true; }
        else if (a == "--no-color")                 { o.no_color = true; }
        else if (a == "-i" || a == "--interactive") { o.interactive = true; }
        else if (a == "--filter")  {
            auto v = need_value(a); if (!v) return std::nullopt; o.filter = *v;
        }
        else if (a == "-n" || a == "--name") {
            auto v = need_value(a); if (!v) return std::nullopt; o.target_name = *v;
        }
        else if (a == "-p" || a == "--pid") {
            auto v = need_value(a); if (!v) return std::nullopt;
            try { o.pid = static_cast<uint32_t>(std::stoul(*v, nullptr, 0)); }
            catch (...) { err_line(fmt("PID 不是数字: {}", *v)); return std::nullopt; }
        }
        else if (a == "-d" || a == "--dll") {
            auto v = need_value(a); if (!v) return std::nullopt; o.dll = *v;
        }
        else if (a == "-x" || a == "--exec") {
            auto v = need_value(a); if (!v) return std::nullopt; o.commands.push_back(*v);
        }
        else if (a == "--wait") {
            auto v = need_value(a); if (!v) return std::nullopt;
            try { o.wait_ready_ms = static_cast<uint32_t>(std::stoul(*v)); }
            catch (...) { err_line("--wait 需要一个毫秒数"); return std::nullopt; }
        }
        else {
            err_line(fmt("未知参数: {}", a));
            out_line("用 --help 看用法。");
            return std::nullopt;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
void cmd_list(std::string_view filter, bool verbose) {
    const auto procs = enum_processes();
    out_colored(ansi::bold, fmt("  进程列表 (共 {})\n", procs.size()));
    out_line(fmt("  {:<8} {:<36} {}", "PID", "映像名", "路径"));
    out_line("  " + std::string(100, '-'));

    size_t shown = 0;
    for (auto const& p : procs) {
        if (!filter.empty() && !icontains(p.name, filter)) continue;
        out_line(fmt("  {:<8} {:<36} {}", p.pid, sanitize(p.name, 36), 
                    verbose ? sanitize(p.path, 70) : ""));
        ++shown;
    }
    out_line(fmt("  --- 匹配 {} 个 ---", shown));
}

std::optional<Target> pick_target(Options const& opt) {
    if (opt.pid) {
        if (!process_alive(opt.pid)) {
            err_line(fmt("PID {} 不存在或已退出", opt.pid));
            return std::nullopt;
        }
        Target t;
        t.pid = opt.pid;
        t.name = process_name_of(opt.pid);
        t.path = process_path_of(opt.pid);
        return t;
    }

    const auto found = find_processes_by_name(opt.target_name);
    if (found.empty()) {
        err_line(fmt("找不到运行中的 {}", opt.target_name));
        out_line("用 --list --filter <子串> 看看有哪些进程, 或用 --name 指定别的映像名。");
        return std::nullopt;
    }
    if (found.size() > 1) {
        err_line(fmt("{} 有 {} 个实例, 用 --pid 指定:", opt.target_name, found.size()));
        for (auto const& p : found) out_line(fmt("    --pid {}", p.pid));
        return std::nullopt;
    }

    Target t;
    t.pid = found.front().pid;
    t.name = found.front().name;
    t.path = found.front().path;
    return t;
}

// ---------------------------------------------------------------------------
void ui_banner() {
    out_colored(ansi::cyan, "\n  ============================================================\n");
    out_colored(ansi::cyan,   "   MCD2 热注入器 — RemoteThread (CreateRemoteThread + LoadLibraryW)\n");
    out_colored(ansi::cyan,   "  ============================================================\n\n");
}

void ui_command_echo(std::string_view cmd) {
    out_colored(ansi::yellow, "mcd2> ");
    out_colored(ansi::bold, std::string(cmd));
    out("\n");
}

void ui_payload_data(std::string_view text) {
    // 注入体已经排好版, 原样透出
    out(text);
}

void ui_payload_status(std::string_view text) {
    out_colored(ansi::gray, fmt("  [注入体] {}\n", text));
}

void ui_payload_error(std::string_view text) {
    out_colored(ansi::red, fmt("  [注入体:错误] {}\n", text));
}

// ---------------------------------------------------------------------------
bool interactive_shell(PipeServer& pipe) {
    out_line("");
    out_colored(ansi::gray, "  输入命令后回车; 帮助输 help, 退出输 quit (或 Ctrl+C)。\n");
    out_line("");

    std::string line;
    for (;;) {
        out_colored(ansi::yellow, "mcd2> ");
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
            out_line("");
            return false;
        }

        const std::string cmd = trim(line);
        if (cmd.empty()) continue;
        if (cmd == "quit" || cmd == "exit") return false;

        // 本地命令: 不以注入体为准
        if (cmd == "clear" || cmd == "cls") { ::system("cls"); continue; }
        if (cmd == "list") { cmd_list("", false); continue; }

        if (!pipe.connected()) {
            err_line("与注入体的管道已断开, 无法下发命令。");
            return false;
        }
        if (!pipe.send_command(cmd)) {
            err_line("命令下发失败。");
            return false;
        }
    }
}

} // namespace mcd2
