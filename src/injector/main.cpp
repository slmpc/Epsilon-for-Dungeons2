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
#include "common/pipe_channel.h"
#include "common/proc_util.h"
#include "common/text.h"
#include "injector/cli.h"
#include "injector/pipe_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace mcd2 {
namespace {

// 找同目录下的注入体 DLL。
std::string default_payload_path() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
    const std::wstring dll = dir + L"\\mcd2_payload.dll";

    const int m = ::WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), static_cast<int>(dll.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, m)), '\0');
    if (m > 0) ::WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), static_cast<int>(dll.size()),
                                     s.data(), m, nullptr, nullptr);
    return s;
}

// 注入体把日志写在 %TEMP%\mcd2_payload_<pid>.log。
// 注入体在目标进程里没有控制台, 排查问题时这份日志是唯一线索, 所以
// 任何等待失败都把它指出来。
std::string payload_log_path(uint32_t pid) {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    const std::string dir = (n > 0) ? to_utf8(std::wstring_view(tmp, n)) : std::string(".");
    return fmt("{}mcd2_payload_{}.log", dir, pid);
}

// 注入器是控制台程序, Ctrl+C 会让它直接死掉, 留下一堆没清理的句柄。
// 交给系统回收即可 —— 但要在退出前把 bye 发给注入体, 所以装一个简单的处理器。
volatile BOOL g_interrupted = FALSE;

BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_interrupted = TRUE;
        return TRUE;      // 已处理, 不要立刻终止进程
    }
    return FALSE;
}

} // namespace
} // namespace mcd2

int main(int argc, char** argv) {
    using namespace mcd2;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    auto opt = parse_args(argc, argv);
    if (!opt) return 2;

    console_init(true);
    set_color_enabled(!opt->no_color);
    ::SetConsoleCtrlHandler(&ctrl_handler, TRUE);

    ui_banner();

    // ---------------------------------------------------------------- 列表模式
    if (opt->list_only) {
        cmd_list(opt->filter, opt->verbose);
        return 0;
    }

    // ---------------------------------------------------------------- 选目标
    auto target = pick_target(*opt);
    if (!target) return 3;

    out_line(fmt("  目标进程   : {} (PID {})", target->name, target->pid));
    out_line(fmt("  映像路径   : {}", target->path.empty() ? "(未知)" : target->path));

    // 目标模块基址 —— 记录基线, 注入后好做差异对比
    const auto mod_before = find_module(target->pid, opt->target_name);
    if (mod_before) {
        out_line(fmt("  模块基址   : {} ({} 字节)", hex(mod_before->base, 16),
                     human_bytes(mod_before->size)));
    }

    // 解析 DLL 路径
    std::string dll = opt->dll.empty() ? default_payload_path() : opt->dll;
    if (dll.size() >= 2 && dll[1] != ':') {
        // 相对路径补成绝对路径 —— 目标进程用不了相对路径
        char full[MAX_PATH * 4]{};
        if (::GetFullPathNameA(dll.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr)) {
            dll = full;
        }
    }

    // ---------------------------------------------------------------- 管道
    // 约定名: 由目标 PID 推导。注入体用自己的 PID 算出同一名字, 无需传参。
    // 注入器**只做注入**, 不做卸载 —— 见 proc_util.h 里的说明。
    InjectorChannel channel;
    {
        std::string err;
        if (!channel.start(target->pid, &err)) {
            err_line(fmt("  创建通信管道失败: {}", err));
            return 5;
        }
        if (opt->verbose) {
            out_line(fmt("  通信管道   : {} (约定名, 由目标 PID 推导)",
                         to_utf8(channel.pipe_name())));
        }
    }

    // 管道名也写进目标环境块作为冗余通路(注入体优先读它, 失败则用约定名)。
    {
        std::string detail;
        const bool env_ok = set_remote_env(target->pid, {
            {proto::kEnvPipeName, pipe_full_path(channel.pipe_name())},
        }, &detail);
        if (env_ok) {
            if (opt->verbose) out_line(fmt("  远程环境块 : {}", detail));
        } else if (opt->verbose) {
            out_line(fmt("  远程环境块 : 写入失败({}) —— 无妨, 注入体用约定名", detail));
        }
    }

    // ---------------------------------------------------------------- 注入
    out_line("");
    out_colored(ansi::bold, "  正在注入 (CreateRemoteThread + LoadLibraryW)...\n");
    out_line(fmt("  DLL        : {}", dll));

    const InjectResult r = inject_dll(target->pid, dll);
    if (r.status != InjectStatus::ok) {
        out_colored(ansi::red, fmt("  [x] 注入失败: {}\n", to_string(r.status)));
        if (!r.detail.empty()) out_line(fmt("      {}", r.detail));
        if (r.win32_error) {
            out_line(fmt("      Win32 错误: {} ({})", r.win32_error,
                         std::system_category().message(static_cast<int>(r.win32_error))));
        }
        if (opt->verbose) {
            out_line(fmt("      远端参数串: {}", hex(r.remote_string, 16)));
        }
        return 6;
    }

    out_colored(ansi::green, fmt("  [+] 注入成功 ({} ms)\n", r.elapsed_ms));
    out_line(fmt("      远端 HMODULE : {}", hex(r.remote_module, 16)));

    // 差异: 注入后再看模块列表, 确认 DLL 真的进去了
    if (auto mod_after = find_module(target->pid, "mcd2_payload.dll")) {
        out_colored(ansi::green, fmt("  [+] 模块已加载 : {} ({} 字节)\n",
                                    hex(mod_after->base, 16), human_bytes(mod_after->size)));
    } else if (opt->verbose) {
        out_line("      (Toolhelp 里暂未看到模块快照, 通常稍等即出现)");
    }

    // ---------------------------------------------------------------- 等就绪
    out_line("");
    out_line("  等待注入体初始化 (连管道 / 定位 GObjects+GNames / 重建属性偏移)...");
    if (channel.wait_ready(opt->wait_ready_ms)) {
        out_colored(ansi::green, "  [+] 注入体已就绪\n");
    } else {
        out_colored(ansi::yellow, fmt("  [!] {}\n", channel.last_error()));
        out_line(fmt("      注入体日志: {}", payload_log_path(target->pid)));
        out_line("      若目标进程已消失, 日志最后一行就是崩溃点。");
    }

    // ---------------------------------------------------------------- 命令
    int rc = 0;
    if (!opt->commands.empty()) {
        out_line("");
        for (auto const& c : opt->commands) {
            if (!channel.connected()) {
                err_line("  管道未连接, 无法下发命令。");
                rc = 7;
                break;
            }
            if (!channel.send(c)) {
                err_line(fmt("  下发失败: {}", c));
                rc = 7;
                break;
            }
            // 给注入体一点时间把响应写完。命令是同步执行的, 但没有完成通知,
            // 所以这里用固定小延迟 —— 采集类命令都很快。
            ::Sleep(400);
        }
    }

    if (opt->interactive && channel.connected()) {
        if (!interactive_shell(channel.pipe())) rc = 0;
    }

    channel.stop();
    return rc;
}
