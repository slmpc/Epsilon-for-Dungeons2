// ============================================================================
//  overlay.h — D3D12 上的 ImGui 覆盖层
//
//  生命周期(全部由 Present 钩子驱动, 不需要额外线程):
//    第一次 Present → init(): 用**游戏自己的** D3D12 设备建 ImGui 所需的
//                            独立资源(命令队列/描述符堆/分配器/围栏),
//                            并挂钩窗口过程接收输入
//    之后每次 Present → render(): NewFrame → 画面板 → Render → 提交
//
//  为什么自己建命令队列而不是用游戏的:
//    我们拿不到游戏内部那份队列指针(它藏在 UE 的 RHI 里)。ImGui 需要队列来
//    上传字体纹理。用同一个 device 建一个我们自己的 DIRECT 队列是合法且
//    互不干扰的 —— 两者操作的是不同的资源, 只有最终的后缓冲是共享的,
//    而共享点由"先记录屏障再执行再等围栏"这套顺序保证安全。
//
//  为什么第一次 Present 才初始化:
//    Present 是唯一能拿到真实交换链的时机。在那之前我们只有一个 vtable 地址,
//    没有 device、没有 hwnd、也不知道后缓冲格式。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

struct IDXGISwapChain;

namespace mcd2::payload::overlay {

// 第一次 Present 时调用。swapchain 就是钩子抓到的那个。
// 失败不抛异常, 错误可从 last_error() 读到。
bool init(IDXGISwapChain* swapchain);

// 每帧在 Present 钩子里调用。
void render(IDXGISwapChain* swapchain);

// 反初始化(目前只在进程退出路径上用到)。
void shutdown();

[[nodiscard]] bool initialized() noexcept;
[[nodiscard]] bool visible() noexcept;
void set_visible(bool v) noexcept;
[[nodiscard]] std::string const& last_error() noexcept;

} // namespace mcd2::payload::overlay
