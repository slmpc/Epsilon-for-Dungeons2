// ============================================================================
//  d3d12_target — D3D12 渲染靶子
//
//  用途: 在没有游戏的情况下验证 Present 钩子与 ImGui 覆盖层。
//  它建一个真实窗口 + D3D12 设备 + 翻转模型交换链, 然后一直渲染。
//
//  与 mcd2_testtarget 的区别:
//    mcd2_testtarget  什么都不做, 验证"注入链路 + 无 D3D 时的优雅降级"
//    d3d12_target     真实出帧, 验证"Present 钩子 + 覆盖层渲染"
//
//  用法:
//    先跑它, 然后注入:
//      .\build\release\bin\mcd2_injector.exe -n d3d12_target.exe --exec hook
//      (注入器会自动建管道, 不需要交互)
//    窗口里会出现覆盖层, 按 Insert 开关。
// ============================================================================
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include <array>
#include <cstdio>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace {

constexpr UINT   kWidth  = 960;
constexpr UINT   kHeight = 600;
constexpr UINT   kFramesInFlight = 3;

HWND                     g_hwnd = nullptr;
ID3D12Device*            g_device = nullptr;
IDXGISwapChain3*         g_swapchain = nullptr;
ID3D12CommandQueue*      g_queue = nullptr;
ID3D12DescriptorHeap*    g_rtv_heap = nullptr;
ID3D12Resource*          g_backbuffers[2]{};
ID3D12CommandAllocator*  g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence*             g_fence = nullptr;
HANDLE                   g_fence_event = nullptr;
UINT64                   g_fence_value = 0;
UINT                     g_rtv_inc = 0;
UINT                     g_frame = 0;
bool                     g_running = true;

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || m == WM_CLOSE) { g_running = false; ::PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { g_running = false; ::PostQuitMessage(0); return 0; }
    return ::DefWindowProcW(h, m, w, l);
}

// 注意: 不要把 "调用" 和 "检查结果" 塞进同一个函数调用的实参里 ——
// C++ 未规定实参求值顺序, MSVC 是从右往左, 于是出参会在调用发生之前被读走,
// 拿到永远是 null 的旧值。必须拆成两条语句。
template <typename T>
bool ok(HRESULT hr, T* p, char const* what) {
    if (FAILED(hr) || !p) {
        mcd2::err_line(mcd2::fmt("  [x] {} 失败 hr={:#x} ptr={}", what,
                                 static_cast<uint32_t>(hr),
                                 mcd2::hex(reinterpret_cast<uint64_t>(p), 16)));
        return false;
    }
    return true;
}

bool init_d3d12() {
    // 调试层(可选, 生产别开)
    // ID3D12Debug* dbg; D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)); dbg->EnableDebugLayer();

    {
        const HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                               __uuidof(ID3D12Device),
                                               reinterpret_cast<void**>(&g_device));
        if (!ok(hr, g_device, "D3D12CreateDevice")) return false;
    }

    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const HRESULT hr = g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
        if (!ok(hr, g_queue, "CreateCommandQueue")) return false;
    }

    // 交换链(翻转模型)
    IDXGIFactory4* factory = nullptr;
    {
        const HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                                reinterpret_cast<void**>(&factory));
        if (!ok(hr, factory, "CreateDXGIFactory1")) return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = kWidth;
    scd.Height = kHeight;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    IDXGISwapChain1* sc1 = nullptr;
    {
        const HRESULT hr = factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &scd,
                                                           nullptr, nullptr, &sc1);
        factory->Release();
        if (!ok(hr, sc1, "CreateSwapChainForHwnd")) return false;
    }
    g_swapchain = static_cast<IDXGISwapChain3*>(sc1);   // 1→3 是同一对象, 直接转

    // RTV 堆
    {
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 2;
        const HRESULT hr = g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap));
        if (!ok(hr, g_rtv_heap, "CreateDescriptorHeap(RTV)")) return false;
    }
    g_rtv_inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i) {
        const HRESULT hr = g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]));
        if (!ok(hr, g_backbuffers[i], "GetBuffer")) return false;
        g_device->CreateRenderTargetView(g_backbuffers[i], nullptr, h);
        h.ptr += g_rtv_inc;
    }

    {
        const HRESULT hr = g_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
        if (!ok(hr, g_alloc, "CreateCommandAllocator")) return false;
    }
    {
        const HRESULT hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       g_alloc, nullptr,
                                                       IID_PPV_ARGS(&g_list));
        if (!ok(hr, g_list, "CreateCommandList")) return false;
    }
    g_list->Close();

    {
        const HRESULT hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                 IID_PPV_ARGS(&g_fence));
        if (!ok(hr, g_fence, "CreateFence")) return false;
    }
    g_fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_fence_event != nullptr;
}

void render_one_frame() {
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);

    const UINT idx = g_swapchain->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_backbuffers[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx) * g_rtv_inc;
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // 底色随时间轻微变化, 让人一眼看出还在出帧。
    // 设 MCD2_TARGET_NO_CLEAR=1 可跳过清屏 —— 用来判定"覆盖层看不见"到底是
    // 渲染没生效, 还是被游戏这一帧的清屏盖掉了。
    static const bool no_clear = [] {
        wchar_t v[8]{};
        return ::GetEnvironmentVariableW(L"MCD2_TARGET_NO_CLEAR", v, 8) > 0;
    }();
    if (!no_clear) {
        const float t = static_cast<float>(g_frame % 600) / 600.0f;
        const float clear[4] = {0.06f + t * 0.10f, 0.08f, 0.14f - t * 0.06f, 1.0f};
        g_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &b);

    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);

    g_swapchain->Present(1, 0);

    ++g_fence_value;
    g_queue->Signal(g_fence, g_fence_value);
    if (g_fence->GetCompletedValue() < g_fence_value) {
        g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
        ::WaitForSingleObject(g_fence_event, 1000);
    }
    ++g_frame;
}

} // namespace

int main() {
    mcd2::console_init(true);
    mcd2::out_colored(mcd2::ansi::cyan,
        "==================================================\n"
        "  d3d12_target — Present 钩子 / 覆盖层验证靶子\n"
        "==================================================\n");
    mcd2::out_line(mcd2::fmt("  PID : {}", ::GetCurrentProcessId()));
    mcd2::out_line("");
    mcd2::out_line("  注入方式:");
    mcd2::out_line("    mcd2_injector.exe -n d3d12_target.exe --exec hook");
    mcd2::out_line("  覆盖层出现后按 Insert 开关。ESC 退出本程序。");
    mcd2::out_line("");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &wndproc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"mcd2_d3d12_target";
    ::RegisterClassExW(&wc);

    RECT r{0, 0, kWidth, kHeight};
    ::AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"mcd2 d3d12 target — 注入到这里试试",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) { mcd2::err_line("CreateWindowExW 失败"); return 1; }
    ::ShowWindow(g_hwnd, SW_SHOW);

    if (!init_d3d12()) { mcd2::err_line("D3D12 初始化失败"); return 2; }
    mcd2::out_colored(mcd2::ansi::green, "  [+] D3D12 就绪, 开始出帧\n");

    while (g_running) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!g_running) break;
        render_one_frame();
    }

    mcd2::out_line("退出中...");
    return 0;
}
