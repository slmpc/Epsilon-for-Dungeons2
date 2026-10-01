// ============================================================================
//  pipeServer.h — 注入器侧的管道服务端 + 就绪等待
// ============================================================================
#pragma once

#include "common/PipeChannel.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace epsilon {

// 收集注入体回报的状态, 并把数据直接打到控制台。
class InjectorChannel {
public:
    // 创建管道并开始等待注入体连接(非阻塞: 真正的 accept 在后台线程里)。
    // targetPid 与 dllPath 一起推导约定管道名 EpsilonHotPipe2.<pid>.<slot> ——
    // 注入体用自己的 PID 加自己的模块文件名算出同一个名字, 不依赖显式传递。
    // 带 dllPath 槽位是为了让不同文件名的 DLL 能并存于同一进程 ——
    // 这样改完代码可以换个 DLL 名字直接注入, 不必重启游戏。
    bool start(uint32_t targetPid, std::wstring_view dllPath, std::string* error = nullptr);

    // 等注入体报 ready。返回是否就绪。
    bool waitReady(uint32_t timeoutMs);

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    [[nodiscard]] bool ready() const noexcept { return ready_.load(); }
    [[nodiscard]] uint64_t payloadModuleBase() const noexcept { return moduleBase_.load(); }
    [[nodiscard]] uint32_t payloadPid() const noexcept { return payload_pid_.load(); }
    [[nodiscard]] std::string const& lastError() const noexcept { return last_error_; }
    [[nodiscard]] std::wstring const& pipeName() const noexcept { return name_; }

    // 下发一条命令。
    bool send(std::string_view command);

    PipeServer& pipe() noexcept { return pipe_; }
    void stop();

private:
    void onMessage(proto::Kind kind, std::string_view body);
    void onDisconnect();

    PipeServer        pipe_;
    std::mutex        mu_;
    std::condition_variable cv_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> moduleBase_{0};
    std::atomic<uint32_t> payload_pid_{0};
    std::string       last_error_;
    std::wstring      name_;
};

} // namespace epsilon
