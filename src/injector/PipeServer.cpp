// PipeServer.cpp — InjectorChannel 的实现: 消息分发 + 就绪等待。
#include "injector/PipeServer.h"
#include "injector/Cli.h"

#include "common/Text.h"

#include <chrono>

namespace epsilon {

bool InjectorChannel::start(uint32_t targetPid, std::wstring_view dllPath, std::string* error) {
    const std::wstring slot = moduleSlotFromPath(dllPath);
    name_ = defaultPipeName(targetPid, slot);

    if (!pipe_.start(name_, [this](proto::Kind k, std::string_view b) { onMessage(k, b); },
                     [this] { onDisconnect(); }, error)) {
        return false;
    }
    return true;
}

void InjectorChannel::onMessage(proto::Kind kind, std::string_view body) {
    switch (kind) {
        case proto::Kind::hello: {
            if (body.size() >= sizeof(proto::Hello)) {
                proto::Hello h{};
                std::memcpy(&h, body.data(), sizeof(h));
                moduleBase_.store(h.moduleBase);
                payload_pid_.store(h.pid);
                if (h.protocol != proto::versionValue) {
                    last_error_ = fmt("注入体协议版本 {} != {}", h.protocol, proto::versionValue);
                    uiPayloadError(last_error_);
                }
            }
            break;
        }
        case proto::Kind::ready:
            ready_.store(true);
            cv_.notify_all();
            break;
        case proto::Kind::status:
            uiPayloadStatus(body);
            break;
        case proto::Kind::data:
            uiPayloadData(body);
            break;
        case proto::Kind::error:
            uiPayloadError(body);
            break;
        case proto::Kind::frame:
            break;
        default:
            break;
    }
}

void InjectorChannel::onDisconnect() {
    connected_.store(false);
    cv_.notify_all();
    if (!ready_.load()) {
        cv_.notify_all();
    }
}

bool InjectorChannel::waitReady(uint32_t timeoutMs) {
    // 先等连接, 再等 ready; 失败原因写进 last_error_ 供调用方直接打印。
    if (!pipe_.waitForClient(timeoutMs)) {
        last_error_ = "等注入体连接管道超时 —— DLL 可能已加载但初始化卡住/失败";
        return false;
    }
    connected_.store(true);

    std::unique_lock lk(mu_);
    const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                 [this] { return ready_.load() || !connected_.load(); });
    if (!ok) {
        last_error_ = "等注入体 ready 超时";
        return false;
    }
    return ready_.load();
}

bool InjectorChannel::send(std::string_view command) {
    uiCommandEcho(command);
    return pipe_.sendCommand(command);
}

void InjectorChannel::stop() {
    if (pipe_.connected()) {
        pipe_.send(proto::Kind::bye, "injector exiting");
    }
    pipe_.stop();
}

} // namespace epsilon
