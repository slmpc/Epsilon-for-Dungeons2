// ============================================================================
//  pipe_channel.h — 双向命名管道消息通道
//
//  为什么注入体不能直接往控制台写:
//    Dungeons-Win64-Shipping.exe 是 GUI 子系统进程, 注入后它的 stdout 句柄无效。
//    RemoteThread 注入又无法传命令行参数。所以拿两件事解决:
//      * 输出 → 命名管道回传给注入器, 由注入器的控制台显示(数据可即时看到)
//      * 配置 → 注入器先改写目标进程环境块(MCD2_PIPE_NAME 等), 注入体在 DllMain 里读
//    另有一条 ALLOC_CONSOLE 路线: 注入体在游戏进程里自己 AllocConsole(),
//    适合"不通过注入器、纯注入 DLL"的场景。
// ============================================================================
#pragma once

#include "common/protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mcd2 {

// 生成一个本会话唯一的管道名。
std::wstring make_pipe_name(std::wstring_view prefix = L"MCD2HotInject");

// 注入器与注入体之间的**约定管道名**: 由目标进程 PID 推导。
//
// 为什么不用环境变量传这个名字:
//   RemoteThread 注入没有命令行参数可用, 所以最初是让注入器改写目标进程的
//   PEB 环境块来传。实测这条路在目标侧不可靠 —— GetEnvironmentVariableW
//   未必反映被外部改掉的 PEB 指针。而注入体**自己知道自己的 PID**
//   (GetCurrentProcessId), 所以两端各自算出同一个名字, 零传递成本、
//   零失败点。环境变量仍然会被写入并作为第一优先(见 pipe_client.cpp),
//   但不再是唯一通路。
std::wstring default_pipe_name(uint32_t target_pid);

// 管道名 → 完整路径 (\\.\pipe\...)
std::wstring pipe_full_path(std::wstring_view name);

// ---------------------------------------------------------------------------
//  服务端(注入器侧)
// ---------------------------------------------------------------------------
class PipeServer {
public:
    using MessageHandler = std::function<void(proto::Kind, std::string_view)>;
    using DisconnectHandler = std::function<void()>;

    PipeServer() = default;
    ~PipeServer();
    PipeServer(PipeServer const&) = delete;
    PipeServer& operator=(PipeServer const&) = delete;

    // 创建管道实例并开始等待连接(启动 acceptor 线程, 本身不阻塞)。
    bool start(std::wstring const& pipe_name, MessageHandler on_message,
               DisconnectHandler on_disconnect = {}, std::string* error = nullptr);

    // 阻塞直到有客户端连上, 或超时。返回是否连上。
    // 注意: 真正的 ConnectNamedPipe 在 acceptor 线程里, 所以这里可以安全超时返回。
    bool wait_for_client(uint32_t timeout_ms);

    [[nodiscard]] bool connected() const noexcept { return connected_; }

    // 发给注入体一条命令行。返回是否写出成功。
    bool send_command(std::string_view line);
    bool send(proto::Kind kind, std::string_view payload);

    // 断开并关闭。幂等。
    void stop();

private:
    static unsigned long __stdcall acceptor_trampoline(void* self);
    // 先 ConnectNamedPipe(阻塞), 连上后再进入读循环。
    void acceptor_loop();
    void reader_loop();
    void close_handles();

    void* pipe_ = nullptr;          // HANDLE
    void* stop_event_ = nullptr;    // HANDLE
    void* connected_event_ = nullptr;
    void* acceptor_ = nullptr;      // HANDLE: 连接 + 读线程
    MessageHandler on_message_;
    DisconnectHandler on_disconnect_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::wstring name_;
};

// ---------------------------------------------------------------------------
//  客户端(注入体侧)
// ---------------------------------------------------------------------------
class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(PipeClient const&) = delete;
    PipeClient& operator=(PipeClient const&) = delete;

    // 尝试连接, 内部会重试 retry_ms(注入器可能还在起管道)。
    bool connect(std::wstring const& pipe_name, uint32_t retry_ms = 8000,
                 std::string* error = nullptr);

    [[nodiscard]] bool connected() const noexcept { return pipe_ != nullptr; }

    bool send(proto::Kind kind, std::string_view payload);
    // 发送定长 POD 负载。
    template <typename T>
    bool send_pod(proto::Kind kind, T const& pod) {
        static_assert(std::is_trivially_copyable_v<T>, "只支持 POD");
        return send(kind, std::string_view(reinterpret_cast<const char*>(&pod), sizeof(T)));
    }

    // 阻塞读一条消息(带超时)。返回 false = 超时/断开。
    bool recv(proto::Kind& kind, std::string& payload, uint32_t timeout_ms);

    // 起一个后台线程持续收命令。
    bool start_reader(std::function<void(proto::Kind, std::string_view)> on_message);

    void close();

private:
    static unsigned long __stdcall reader_trampoline(void* self);
    void reader_loop();

    void* pipe_ = nullptr;         // HANDLE
    void* stop_event_ = nullptr;   // HANDLE
    void* reader_ = nullptr;
    std::function<void(proto::Kind, std::string_view)> on_message_;
};

} // namespace mcd2
