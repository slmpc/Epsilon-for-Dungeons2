// Hooks.h — MinHook 挂钩层(Present / ExecuteCommandLists)
// 自动安装默认开启(延迟 10 秒), EPSILON_NO_AUTO_HOOK=1 关闭; 本框架不提供卸载。
// 定位 Present 只有一条路: 临时 D3D12 交换链的 vtable[8]。
// 细节见 docs/payload/hooks.md
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace epsilon::payload {

// 每帧回调, 在渲染线程上执行 —— 实现必须极轻(只打标记/计数, 不做遍历)。
using FrameCallback = std::function<void(uint32_t frameIndex, int syncInterval, int flags)>;

struct HookStatus {
    bool        attempted = false;
    bool        installed = false;
    std::string target;
    std::string how;
    std::string error;
    uint64_t    frameCount = 0;
    double      fps = 0.0;
};

class Hooks {
public:
    Hooks() = default;
    ~Hooks();
    Hooks(Hooks const&) = delete;
    Hooks& operator=(Hooks const&) = delete;

    // 安装 Present 钩子。失败不抛异常, 状态从 status() 读。
    // 线程安全: 命令线程(`hook`)与功能线程(自动安装)都会调用, 重复调用安全。
    bool install();

    // 是否已经尝试过安装(含失败)。自动安装据此不再重试。
    [[nodiscard]] bool attempted() const noexcept { return status_.attempted; }

    [[nodiscard]] bool installed() const noexcept { return status_.installed; }
    [[nodiscard]] HookStatus const& status() const noexcept { return status_; }

    void setFrameCallback(FrameCallback cb);

    // 由 detour 调用(外部不要直接调)。
    void onFrame(int syncInterval, int flags);

private:
    HookStatus status_;
    void*      target_ = nullptr;
    void*      trampoline_ = nullptr;
    FrameCallback callback_;
    uint64_t   frames_ = 0;
    // 保护 status_/target_/trampoline_: install() 可能来自命令线程或功能线程。
    mutable std::mutex installMu_;
};

Hooks& hooks();

// 当前 Present 地址与累计帧数(未挂钩时为 0)。
uint64_t presentAddress();
uint64_t frameCount();

// 游戏在用的那条 DIRECT 命令队列(由 ExecuteCommandLists 钩子捕获); 没有返回 nullptr。
// ⚠️ 覆盖层必须用它渲染: flip 模型交换链与创建它的队列绑定, 别的队列上画等于白画。
void* presentQueue();

} // namespace epsilon::payload
