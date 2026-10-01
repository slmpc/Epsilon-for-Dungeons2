// Cli.h — 命令行解析、目标进程查找与注入器的控制台 UI。
// 无参数即"找 Dungeons2 → 注入 → 装 Present 钩子 → 进交互", 用法见 docs/reverse/toolchain.md
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon {

struct Options {
    bool        listOnly = false;
    bool        verbose = false;
    bool        noColor = false;

    std::string targetName = "Dungeons-Win64-Shipping.exe";
    std::string filter;
    uint32_t    pid = 0;
    std::string dll;                        // 空 = 自动找同目录的 epsilonPayload.dll

    std::vector<std::string> commands;

    // 注入体连上管道并报 ready 的上限。
    uint32_t    waitReadyMs = 20000;
    // 等游戏进程出现(含已存在的)的上限; 0 = 只查一次。
    uint32_t    findWaitMs = 60000;

    bool        hook = true;                // 注入后立即装 Present 钩子(默认)
    bool        interactive = true;         // 默认进交互模式
    bool        interactiveSet = false;     // 用户显式给过 -i/--no-interactive

    std::string logPath;
};

// 解析命令行。返回 nullopt 表示参数有误(错误已打印), 或用户要了 --help。
std::optional<Options> parseArgs(int argc, char** argv);

void printUsage();

void cmdList(std::string_view filter, bool verbose);

struct Target { uint32_t pid = 0; std::string name; std::string path; };

// 找目标进程: --pid 直接采信; 否则先按映像名精确找, 再按 "Dungeons*Win64*.exe"
// 模糊找(排除启动器 Dungeons.exe / 崩溃上报 CrashReportClient 等)。
std::optional<Target> pickTarget(Options const& opt);

void uiBanner();
void uiCommandEcho(std::string_view cmd);
void uiPayloadData(std::string_view text);
void uiPayloadStatus(std::string_view text);
void uiPayloadError(std::string_view text);

class PipeServer;

// 交互式 shell: `epsilon>` 提示符, 输入同时下发给注入体; 返回 false = 用户要退出。
bool interactiveShell(PipeServer& pipe);

} // namespace epsilon
