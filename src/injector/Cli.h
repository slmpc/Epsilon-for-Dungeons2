// Cli.h — 命令行解析、进程列表与注入器的控制台 UI。
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
    uint32_t    waitReadyMs = 20000;
    bool        interactive = false;
    std::string logPath;
};

// 解析命令行。返回 nullopt 表示参数有误(错误已打印)。
std::optional<Options> parseArgs(int argc, char** argv);

void printUsage();

void cmdList(std::string_view filter, bool verbose);

// 在候选进程里挑一个; 多个实例时打印让用户用 --pid 指定。
struct Target { uint32_t pid = 0; std::string name; std::string path; };
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
