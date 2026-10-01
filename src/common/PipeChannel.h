// PipeChannel.h — 双向命名管道消息通道(注入器 PipeServer / 注入体 PipeClient)。
// 注入体所在的游戏进程是 GUI 子系统, stdout 无效且 RemoteThread 注入无法传参,
// 所以输出走管道回传、管道名由两端各自按约定推导。细节见 docs/payload/lifecycle.md。
#pragma once

#include "common/Protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace epsilon {

std::wstring makePipeName(std::wstring_view prefix = L"EpsilonHotInject");

// 约定管道名: 两端各自用 (目标 PID, 己方模块文件名) 算出同一个字符串, 零传递。
// 带槽位让不同文件名的 DLL 拿到不同管道, 可在同一进程并存 —— 这不是放宽单实例守卫。
std::wstring defaultPipeName(uint32_t targetPid, std::wstring_view slot = {});

// 从模块路径/文件名推导槽位(取文件名去扩展名, 只留字母数字); 两端用同一规则各算一次。
std::wstring moduleSlotFromPath(std::wstring_view pathOrName);

std::wstring pipeFullPath(std::wstring_view name);

class PipeServer {
public:
    using MessageHandler = std::function<void(proto::Kind, std::string_view)>;
    using DisconnectHandler = std::function<void()>;

    PipeServer() = default;
    ~PipeServer();
    PipeServer(PipeServer const&) = delete;
    PipeServer& operator=(PipeServer const&) = delete;

    // 创建管道实例并开始等待连接(启动 acceptor 线程, 本身不阻塞)。
    bool start(std::wstring const& pipeName, MessageHandler onMessage,
               DisconnectHandler onDisconnect = {}, std::string* error = nullptr);

    // 阻塞直到有客户端连上或超时; 返回是否连上 —— 真正的 ConnectNamedPipe 在
    // acceptor 线程里, 所以这里可以安全超时返回。
    bool waitForClient(uint32_t timeoutMs);

    [[nodiscard]] bool connected() const noexcept { return connected_; }

    bool sendCommand(std::string_view line);
    bool send(proto::Kind kind, std::string_view payload);

    // 断开并关闭。幂等。
    void stop();

private:
    static unsigned long __stdcall acceptorTrampoline(void* self);
    void acceptorLoop();
    void readerLoop();
    void closeHandles();

    void* pipe_ = nullptr;
    void* stopEvent_ = nullptr;
    void* connectedEvent_ = nullptr;
    void* acceptor_ = nullptr;
    MessageHandler on_message_;
    DisconnectHandler on_disconnect_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::wstring name_;
};

class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(PipeClient const&) = delete;
    PipeClient& operator=(PipeClient const&) = delete;

    // 尝试连接, 内部重试 retryMs(注入器可能还在起管道); error 非空时填失败原因。
    bool connect(std::wstring const& pipeName, uint32_t retryMs = 8000,
                 std::string* error = nullptr);

    [[nodiscard]] bool connected() const noexcept { return pipe_ != nullptr; }

    bool send(proto::Kind kind, std::string_view payload);
    template <typename T>
    bool sendPod(proto::Kind kind, T const& pod) {
        static_assert(std::is_trivially_copyable_v<T>, "只支持 POD");
        return send(kind, std::string_view(reinterpret_cast<const char*>(&pod), sizeof(T)));
    }

    // 阻塞读一条消息(带超时)。返回 false = 超时或断开。
    bool recv(proto::Kind& kind, std::string& payload, uint32_t timeoutMs);

    bool startReader(std::function<void(proto::Kind, std::string_view)> onMessage);

    void close();

private:
    static unsigned long __stdcall readerTrampoline(void* self);
    void readerLoop();

    void* pipe_ = nullptr;
    void* stopEvent_ = nullptr;
    void* reader_ = nullptr;
    std::function<void(proto::Kind, std::string_view)> on_message_;
};

} // namespace epsilon
