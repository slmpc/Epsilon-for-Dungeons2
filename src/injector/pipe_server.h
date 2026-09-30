// ============================================================================
//  pipe_server.h — 注入器侧的管道服务端 + 就绪等待
// ============================================================================
#pragma once

#include "common/pipe_channel.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace mcd2 {

// 收集注入体回报的状态, 并把数据直接打到控制台。
class InjectorChannel {
public:
    // 创建管道并开始等待注入体连接(非阻塞: 真正的 accept 在后台线程里)。
    // target_pid 用来推导约定管道名 MCD2HotInject.<pid> —— 注入体靠自己的
    // PID 算出同一个名字, 所以不依赖环境变量传递。
    bool start(uint32_t target_pid, std::string* error = nullptr);

    // 等注入体报 ready。返回是否就绪。
    bool wait_ready(uint32_t timeout_ms);

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    [[nodiscard]] bool ready() const noexcept { return ready_.load(); }
    [[nodiscard]] uint64_t payload_module_base() const noexcept { return module_base_.load(); }
    [[nodiscard]] uint32_t payload_pid() const noexcept { return payload_pid_.load(); }
    [[nodiscard]] std::string const& last_error() const noexcept { return last_error_; }
    [[nodiscard]] std::wstring const& pipe_name() const noexcept { return name_; }

    // 下发一条命令。
    bool send(std::string_view command);

    PipeServer& pipe() noexcept { return pipe_; }
    void stop();

private:
    void on_message(proto::Kind kind, std::string_view body);
    void on_disconnect();

    PipeServer        pipe_;
    std::mutex        mu_;
    std::condition_variable cv_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    std::atomic<uint64_t> module_base_{0};
    std::atomic<uint32_t> payload_pid_{0};
    std::string       last_error_;
    std::wstring      name_;
};

} // namespace mcd2
