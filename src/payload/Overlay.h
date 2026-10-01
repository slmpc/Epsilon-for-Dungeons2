// Overlay.h — D3D12 上的 ImGui 覆盖层
// 全部由 Present 钩子驱动: 第一次 Present → init(), 之后每帧 → render()。
// 细节见 docs/payload/overlay.md
#pragma once

#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace epsilon::payload::overlay {

// 第一次 Present 时调用(swapchain 就是钩子抓到的那个); 用游戏自己的设备建资源。
// 失败不抛异常, 错误从 lastError() 读。
bool init(IDXGISwapChain* swapchain);

// 每帧在 Present 钩子里调用。
void render(IDXGISwapChain* swapchain);

// 反初始化(进程退出路径)。⚠️ 不释放游戏的命令队列 —— 我们没有它的引用计数份额。
void shutdown();

[[nodiscard]] bool initialized() noexcept;
[[nodiscard]] bool visible() noexcept;
void setVisible(bool v) noexcept;
[[nodiscard]] std::string const& lastError() noexcept;

} // namespace epsilon::payload::overlay
