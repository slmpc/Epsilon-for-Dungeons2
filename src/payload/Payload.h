// Payload.h — 注入体的共享上下文与输出 API
// 细节见 docs/payload/output.md
#pragma once

#include "common/PipeChannel.h"
#include "common/Text.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::payload {

// 输出只有命名管道一条路; 连不上就只写文件日志, 不弹控制台。
enum class Sink {
    pipe,   // 回传给注入器 —— 唯一正常路径
    none,   // 管道没连上: 只写文件日志, 静默驻留
};

struct Context {
    Sink        sink = Sink::none;
    // 唯一的管道实例: 所有新增的收发路径都必须走它(不得另建 PipeClient)。
    PipeClient  pipe;
    bool        pipeConnected = false;
    uint32_t    pid = 0;
    uint64_t    moduleBase = 0;
    uint64_t    moduleSize = 0;
    std::wstring modulePath;
    bool        verbose = false;
    bool        running = false;

    // 命令响应缓冲: 一条命令的所有输出攒成一个管道消息。
    std::string pending;
    bool        pendingActive = false;
};

Context& ctx();

// ---------------------------------------------------------------- 输出
// 追加文本到当前输出目标; 命令响应期间先攒进缓冲。
void emit(std::string_view text);
void emitLine(std::string_view text = {});

template <typename... Args>
void emitFmt(std::format_string<Args...> f, Args&&... args) {
    emitLine(std::format(f, std::forward<Args>(args)...));
}

// 绕过响应缓冲直接送一条消息(异步输出用)。
// 返回是否真的送出去了(管道未连上/写失败时为 false)。
bool emitNow(proto::Kind kind, std::string_view text);

// 命令响应边界: server 在每条命令前后调用。
void beginResponse();
void endResponse();

// 日志: 走 emitNow, 因此不落盘 —— 关键步骤请用 trace()。
void logInfo(std::string_view s);
void logWarn(std::string_view s);
void logError(std::string_view s);
void logVerbose(std::string_view s);

// ---------------------------------------------------------------- 文件日志
// 注入体在没有控制台的进程里出问题时没有任何可见输出, 所以关键路径一律落盘:
//     %TEMP%\epsilonPayload_<pid>_<模块基址>.log
// 打开失败是静默的(必然是权限/共享冲突)。
void logFileOpen() noexcept;
void logFileClose() noexcept;
[[nodiscard]] std::string logFilePath() noexcept;

// 落盘 + 同时送当前输出通道。前面带 "[未送出]" 表示管道那一侧没接上。
void trace(std::string_view msg);

} // namespace epsilon::payload
