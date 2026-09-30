// ============================================================================
//  payload.h — 注入体的共享上下文
//
//  输出通道有两种, 运行时二选一(见 runtime.cpp):
//    * pipe  : 注入器通过命令行持有 UI, 我们经命名管道把数据回吐给它的控制台
//    * console: 自己 AllocConsole(), 在游戏进程里开一个独立控制台窗口
//  两种模式对命令层完全透明 —— 命令处理只调用 emit() 之类的接口。
// ============================================================================
#pragma once

#include "common/pipe_channel.h"
#include "common/text.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mcd2::payload {

//  输出通道只有一种: 命名管道, 数据回吐给注入器的控制台。
//  注入体自身不弹控制台(它在游戏进程里, 弹窗只会碍事)。
enum class Sink {
    pipe,   // 回传给注入器 —— 唯一正常路径
    none,   // 管道没连上: 只写文件日志, 静默驻留
};

struct Context {
    Sink        sink = Sink::none;
    // 唯一的管道实例。pipe_client.cpp 与这里的所有发送路径都必须用它 ——
    // 曾经因为存在第二个 PipeClient 导致"连上了但发不出去"。
    PipeClient  pipe;
    bool        pipe_connected = false;
    uint32_t    pid = 0;
    uint64_t    module_base = 0;
    uint64_t    module_size = 0;
    std::wstring module_path;
    bool        verbose = false;
    bool        running = false;

    // 命令响应缓冲: 一条命令的所有输出攒成一个管道消息, 避免碎片化。
    std::string pending;
    bool        pending_active = false;
};

Context& ctx();

// ---------------------------------------------------------------- 输出
// 追加一段文本到当前输出目标。命令响应期间会先攒进缓冲区。
void emit(std::string_view text);
void emit_line(std::string_view text = {});

template <typename... Args>
void emit_fmt(std::format_string<Args...> f, Args&&... args) {
    emit_line(std::format(f, std::forward<Args>(args)...));
}

// 立即送出(不参与响应缓冲), 用于日志/错误等异步输出。
// 返回是否真的送出去了(管道没连上/写失败时为 false)。
bool emit_now(proto::Kind kind, std::string_view text);

// 命令响应边界: server 在每条命令前后调用。
void begin_response();
void end_response();

// 日志。同时写控制台/管道, 并可按 verbose 过滤。
void log_info(std::string_view s);
void log_warn(std::string_view s);
void log_error(std::string_view s);
void log_verbose(std::string_view s);

// ---------------------------------------------------------------- 文件日志
// 注入体被塞进一个**没有控制台**的进程里时, 出问题没有任何可见输出 ——
// 目标进程直接消失, 你只知道"它崩了", 不知道崩在哪一步。
// 所以关键路径一律落盘:
//     %TEMP%\mcd2_payload_<pid>.log
// 部署到游戏里时, 这份日志就是唯一的排查依据。
void log_file_open() noexcept;
void log_file_close() noexcept;
[[nodiscard]] std::string log_file_path() noexcept;

// 落盘 + 同时送到当前输出通道(管道/控制台)。
// 关键步骤之间的"我走到这了"全部用它, 便于二分定位崩溃点。
void trace(std::string_view msg);

} // namespace mcd2::payload
