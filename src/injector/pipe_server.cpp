// ============================================================================
//  pipe_server.cpp
// ============================================================================
#include "injector/pipe_server.h"
#include "injector/cli.h"

#include "common/text.h"

#include <chrono>

namespace mcd2 {

bool InjectorChannel::start(uint32_t target_pid, std::string* error) {
    // 约定名: 注入体用自己的 PID 算出同一个字符串, 无需任何传递。
    name_ = default_pipe_name(target_pid);

    // 管道名仍然写进目标环境块 —— 作为冗余通路保留。
    // (实测 GetEnvironmentVariableW 未必反映被外部改写的 PEB, 所以
    //  真正的可靠性来自上面的约定名; 这一步只是"能传就传"。)
    if (!pipe_.start(name_, [this](proto::Kind k, std::string_view b) { on_message(k, b); },
                     [this] { on_disconnect(); }, error)) {
        return false;
    }
    return true;
}

void InjectorChannel::on_message(proto::Kind kind, std::string_view body) {
    switch (kind) {
        case proto::Kind::hello: {
            if (body.size() >= sizeof(proto::Hello)) {
                proto::Hello h{};
                std::memcpy(&h, body.data(), sizeof(h));
                module_base_.store(h.module_base);
                payload_pid_.store(h.pid);
                if (h.protocol != proto::kVersion) {
                    last_error_ = fmt("注入体协议版本 {} != {}", h.protocol, proto::kVersion);
                    ui_payload_error(last_error_);
                }
            }
            break;
        }
        case proto::Kind::ready:
            ready_.store(true);
            cv_.notify_all();
            break;
        case proto::Kind::status:
            ui_payload_status(body);
            break;
        case proto::Kind::data:
            ui_payload_data(body);
            break;
        case proto::Kind::error:
            ui_payload_error(body);
            break;
        case proto::Kind::frame:
            break;
        default:
            break;
    }
}

void InjectorChannel::on_disconnect() {
    connected_.store(false);
    cv_.notify_all();
    if (!ready_.load()) {
        cv_.notify_all();
    }
}

bool InjectorChannel::wait_ready(uint32_t timeout_ms) {
    // 先等连接
    if (!pipe_.wait_for_client(timeout_ms)) {
        last_error_ = "等注入体连接管道超时 —— DLL 可能已加载但初始化卡住/失败";
        return false;
    }
    connected_.store(true);

    // 再等 ready 消息
    std::unique_lock lk(mu_);
    const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [this] { return ready_.load() || !connected_.load(); });
    if (!ok) {
        last_error_ = "等注入体 ready 超时";
        return false;
    }
    return ready_.load();
}

bool InjectorChannel::send(std::string_view command) {
    ui_command_echo(command);
    return pipe_.send_command(command);
}

void InjectorChannel::stop() {
    if (pipe_.connected()) {
        pipe_.send(proto::Kind::bye, "injector exiting");
    }
    pipe_.stop();
}

} // namespace mcd2
