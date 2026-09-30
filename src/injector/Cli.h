// ============================================================================
//  cli.h — 命令行界面
// ============================================================================
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
    std::string filter;                     // --list 时的进程名过滤
    uint32_t    pid = 0;                    // 非 0 时优先于 targetName
    std::string dll;                        // 空 = 自动找同目录的 epsilonPayload.dll

    std::vector<std::string> commands;      // 注入后依次下发的命令
    uint32_t    waitReadyMs = 20000;      // 等注入体报 ready 的上限
    bool        interactive = false;        // 是否进入交互式命令行
    std::string logPath;                   // --log: 把输出同时写一份到文件
};

// 解析命令行。返回 nullopt 表示参数有误(错误已打印)。
std::optional<Options> parseArgs(int argc, char** argv);

void printUsage();

// 打印进程列表。
void cmdList(std::string_view filter, bool verbose);

// 在候选进程里挑一个。多个实例时打印让用户用 --pid 指定。
struct Target { uint32_t pid = 0; std::string name; std::string path; };
std::optional<Target> pickTarget(Options const& opt);

// UI 事件
void uiBanner();
void uiCommandEcho(std::string_view cmd);
void uiPayloadData(std::string_view text);
void uiPayloadStatus(std::string_view text);
void uiPayloadError(std::string_view text);

// 交互式命令行:`epsilon>` 提示符, 输入同时下发给注入体。
class PipeServer;

// 返回 false 表示用户要退出。
bool interactiveShell(PipeServer& pipe);

} // namespace epsilon
