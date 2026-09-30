// ============================================================================
//  hooks.h — MinHook 挂钩层
//
//  ⚠️ 重要: 本模块**不在默认初始化路径上**。
//
//  默认流程只做: 注入 → 连管道 → 定位引擎 → 提供只读采集命令。
//  帧钩子需要由使用者显式触发(`hook` 命令), 原因:
//
//    1. 游戏数据采集**不需要**帧回调。命令驱动的按需采样已经够用,
//       而且更安全 —— 不在渲染线程上做任何事。
//    2. 建临时 D3D 交换链取 Present 地址这一步, 在普通进程里验证通过
//       (tests/d3d_probe 有完整探测输出), 但**在被注入的 DLL 上下文里
//       会把目标进程带崩**。这是尚未定位的问题, 所以不放在必经路径上。
//
//  定位方式只有一条(不做多路 fallback):
//    建一个临时 D3D11 设备+交换链, 从它的 vtable 取 Present。
//    同一进程里 dxgi 的 IDXGISwapChain 实现共享 vtable, 所以临时交换链的
//    Present 就是游戏在用的那个。
//
//  卸载: 本框架不提供卸载。注入体一旦挂钩, 半途 FreeLibrary 会留下悬空
//  回调。要清掉就重启游戏进程。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mcd2::payload {

// 每帧回调。参数为 Present 的 (syncInterval, flags), 以及累计帧号。
using FrameCallback = std::function<void(uint32_t frame_index, int sync_interval, int flags)>;

struct HookStatus {
    bool        attempted = false;   // 是否尝试过安装
    bool        installed = false;
    std::string target;              // Present 的地址
    std::string how;                 // 定位方式说明
    std::string error;
    uint64_t    frame_count = 0;
    double      fps = 0.0;
};

class Hooks {
public:
    Hooks() = default;
    ~Hooks();
    Hooks(Hooks const&) = delete;
    Hooks& operator=(Hooks const&) = delete;

    // 安装 Present 钩子。失败不抛异常, 状态可从 status() 读到。
    // 这是**显式动作**, 不在 runtime 启动流程里自动调用。
    bool install();

    [[nodiscard]] bool installed() const noexcept { return status_.installed; }
    [[nodiscard]] HookStatus const& status() const noexcept { return status_; }

    void set_frame_callback(FrameCallback cb);

    // 由 detour 内部调用(不要外部直接调)。
    void on_frame(int sync_interval, int flags);

private:
    HookStatus status_;
    void*      target_ = nullptr;
    void*      trampoline_ = nullptr;
    FrameCallback callback_;
    uint64_t   frames_ = 0;
};

// 全局单例。
Hooks& hooks();

// 当前 Present 地址与累计帧数(未挂钩时为 0)。
uint64_t present_address();
uint64_t frame_count();

// 游戏在用的那条 DIRECT 命令队列(由 ExecuteCommandLists 钩子捕获)。
// 没捕获到返回 nullptr。
//
// 覆盖层**必须**用它渲染: D3D12 的 flip 模型交换链与创建它的队列绑定,
// 在别的队列上渲染会导致"命令执行了、围栏也完成了、但画面不进入呈现结果"。
void* present_queue();

} // namespace mcd2::payload
