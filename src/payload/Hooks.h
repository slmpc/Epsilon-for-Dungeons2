// ============================================================================
//  hooks.h — MinHook 挂钩层
//
//  ⚠️ 关于"在被注入上下文里会崩"的历史记录 —— 已经实测推翻。
//
//  早期版本在真机上装 Present 钩子会把目标带崩, 因此本模块长期不在默认
//  初始化路径上。后续实测(注入 Dungeons-Win64-Shipping 并执行 hook)证明该
//  路径是**可用**的:
//      [hook] D3D12 探测 hr=0x0 device=... queue=... swapchain=...
//      [hook] Present = 0x... (临时 D3D12 交换链 vtable[8])
//      [overlay] 就绪 (后缓冲 3 个, 格式 24, 2560x1600)
//      [overlay] 提交 bb=2 围栏完成=true
//      Present 钩子已生效, 游戏正在出帧
//  临时交换链 vtable 取 Present 这条路是可行的。
//
//  因此现在改为**自动安装**(用户要求), 由功能线程在启动后尝试一次, 见
//  Runtime.cpp 的 maybeAutoInstallHook()。可用环境变量
//      EPSILON_NO_AUTO_HOOK=1
//  关闭 —— 这条逃生通道是刻意留在游戏之外的: 万一某个游戏版本上这条路又
//  出问题, 用户不必先让游戏成功启动一次才能关掉它。
//
//  定位方式只有一条(不做多路 fallback):
//    建一个临时 D3D12 设备+交换链, 从它的 vtable 取 Present。
//    同一进程里 dxgi 的 IDXGISwapChain 实现共享 vtable, 所以临时交换链的
//    Present 就是游戏在用的那个。
//
//  卸载: 本框架不提供卸载。注入体一旦挂钩, 半途 FreeLibrary 会留下悬空
//  回调。要清掉就重启游戏进程。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace epsilon::payload {

// 每帧回调。参数为 Present 的 (syncInterval, flags), 以及累计帧号。
using FrameCallback = std::function<void(uint32_t frameIndex, int syncInterval, int flags)>;

struct HookStatus {
    bool        attempted = false;   // 是否尝试过安装
    bool        installed = false;
    std::string target;              // Present 的地址
    std::string how;                 // 定位方式说明
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

    // 安装 Present 钩子。失败不抛异常, 状态可从 status() 读到。
    //
    // 线程安全: 现在由**两个**来源调用 —— 命令线程的 `hook` 命令, 以及启动后
    // 的自动安装(功能线程)。内部用互斥量与"已安装"短路保护, 重复调用是安全的。
    bool install();

    // 是否已经尝试过安装并且失败过。自动安装据此决定要不要再试 ——
    // 失败后无限重试只会反复触发同一条崩溃路径(见 runtime 里的说明)。
    [[nodiscard]] bool attempted() const noexcept { return status_.attempted; }

    [[nodiscard]] bool installed() const noexcept { return status_.installed; }
    [[nodiscard]] HookStatus const& status() const noexcept { return status_; }

    void setFrameCallback(FrameCallback cb);

    // 由 detour 内部调用(不要外部直接调)。
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

// 全局单例。
Hooks& hooks();

// 当前 Present 地址与累计帧数(未挂钩时为 0)。
uint64_t presentAddress();
uint64_t frameCount();

// 游戏在用的那条 DIRECT 命令队列(由 ExecuteCommandLists 钩子捕获)。
// 没捕获到返回 nullptr。
//
// 覆盖层**必须**用它渲染: D3D12 的 flip 模型交换链与创建它的队列绑定,
// 在别的队列上渲染会导致"命令执行了、围栏也完成了、但画面不进入呈现结果"。
void* presentQueue();

} // namespace epsilon::payload
