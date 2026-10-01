// d3d12Target/Main.cpp — D3D12 渲染靶子: 真实窗口 + D3D12 设备 + 翻转模型交换链, 一直出帧。
// 没有游戏在跑时用它验证 Present 钩子与 ImGui 覆盖层(注入方式见程序启动时的打印)。
// 与 testTarget 的分工: testTarget 验注入链路与无 D3D 时的降级, 本靶子验真实出帧的渲染路径。
#include "common/Text.h"

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

HWND                     gHwnd = nullptr;
ID3D12Device*            gDevice = nullptr;
IDXGISwapChain3*         gSwapchain = nullptr;
ID3D12CommandQueue*      gQueue = nullptr;
ID3D12DescriptorHeap*    gRtvHeap = nullptr;
ID3D12Resource*          gBackbuffers[2]{};
ID3D12CommandAllocator*  gAlloc = nullptr;
ID3D12GraphicsCommandList* gList = nullptr;
ID3D12Fence*             gFence = nullptr;
HANDLE                   gFenceEvent = nullptr;
UINT64                   gFenceValue = 0;
UINT                     gRtvInc = 0;
UINT                     gFrame = 0;
bool                     gRunning = true;

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY || m == WM_CLOSE) { gRunning = false; ::PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { gRunning = false; ::PostQuitMessage(0); return 0; }
    return ::DefWindowProcW(h, m, w, l);
}

// 注意: 不要把"调用"和"检查结果"塞进同一个调用的实参 —— C++ 未规定实参求值顺序,
// MSVC 从右往左, 出参会在调用发生前被读走, 拿到永远是 null 的旧值。必须拆成两条语句。
template <typename T>
bool ok(HRESULT hr, T* p, char const* what) {
    if (FAILED(hr) || !p) {
        epsilon::errLine(epsilon::fmt("  [x] {} 失败 hr={:#x} ptr={}", what,
                                 static_cast<uint32_t>(hr),
                                 epsilon::hex(reinterpret_cast<uint64_t>(p), 16)));
        return false;
    }
    return true;
}

bool initD3d12() {
    // 调试层(可选, 生产别开)
    // ID3D12Debug* dbg; D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)); dbg->EnableDebugLayer();

    {
        const HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                               __uuidof(ID3D12Device),
                                               reinterpret_cast<void**>(&gDevice));
        if (!ok(hr, gDevice, "D3D12CreateDevice")) return false;
    }

    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const HRESULT hr = gDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&gQueue));
        if (!ok(hr, gQueue, "CreateCommandQueue")) return false;
    }

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
        const HRESULT hr = factory->CreateSwapChainForHwnd(gQueue, gHwnd, &scd,
                                                           nullptr, nullptr, &sc1);
        factory->Release();
        if (!ok(hr, sc1, "CreateSwapChainForHwnd")) return false;
    }
    gSwapchain = static_cast<IDXGISwapChain3*>(sc1);

    {
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 2;
        const HRESULT hr = gDevice->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&gRtvHeap));
        if (!ok(hr, gRtvHeap, "CreateDescriptorHeap(RTV)")) return false;
    }
    gRtvInc = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE h = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i) {
        const HRESULT hr = gSwapchain->GetBuffer(i, IID_PPV_ARGS(&gBackbuffers[i]));
        if (!ok(hr, gBackbuffers[i], "GetBuffer")) return false;
        gDevice->CreateRenderTargetView(gBackbuffers[i], nullptr, h);
        h.ptr += gRtvInc;
    }

    {
        const HRESULT hr = gDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gAlloc));
        if (!ok(hr, gAlloc, "CreateCommandAllocator")) return false;
    }
    {
        const HRESULT hr = gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       gAlloc, nullptr,
                                                       IID_PPV_ARGS(&gList));
        if (!ok(hr, gList, "CreateCommandList")) return false;
    }
    gList->Close();

    {
        const HRESULT hr = gDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                 IID_PPV_ARGS(&gFence));
        if (!ok(hr, gFence, "CreateFence")) return false;
    }
    gFenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return gFenceEvent != nullptr;
}

void renderOneFrame() {
    gAlloc->Reset();
    gList->Reset(gAlloc, nullptr);

    const UINT idx = gSwapchain->GetCurrentBackBufferIndex();

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = gBackbuffers[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gList->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx) * gRtvInc;
    gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // EPSILON_TARGET_NO_CLEAR=1 跳过清屏, 用来判定"覆盖层看不见"是渲染没生效还是被清屏盖掉。
    static const bool noClear = [] {
        wchar_t v[8]{};
        return ::GetEnvironmentVariableW(L"EPSILON_TARGET_NO_CLEAR", v, 8) > 0;
    }();
    if (!noClear) {
        const float t = static_cast<float>(gFrame % 600) / 600.0f;
        const float clear[4] = {0.06f + t * 0.10f, 0.08f, 0.14f - t * 0.06f, 1.0f};
        gList->ClearRenderTargetView(rtv, clear, 0, nullptr);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    gList->ResourceBarrier(1, &b);

    gList->Close();
    ID3D12CommandList* lists[] = {gList};
    gQueue->ExecuteCommandLists(1, lists);

    gSwapchain->Present(1, 0);

    ++gFenceValue;
    gQueue->Signal(gFence, gFenceValue);
    if (gFence->GetCompletedValue() < gFenceValue) {
        gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
        ::WaitForSingleObject(gFenceEvent, 1000);
    }
    ++gFrame;
}

} // namespace

int main() {
    epsilon::consoleInit(true);
    epsilon::outColored(epsilon::ansi::cyan,
        "==================================================\n"
        "  d3d12_target — Present 钩子 / 覆盖层验证靶子\n"
        "==================================================\n");
    epsilon::outLine(epsilon::fmt("  PID : {}", ::GetCurrentProcessId()));
    epsilon::outLine("");
    epsilon::outLine("  注入方式:");
    epsilon::outLine("    epsilonInjector.exe -n d3d12_target.exe --exec hook");
    epsilon::outLine("  覆盖层出现后按 Insert 开关。ESC 退出本程序。");
    epsilon::outLine("");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &wndproc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"epsilonD3d12Target";
    ::RegisterClassExW(&wc);

    RECT r{0, 0, kWidth, kHeight};
    ::AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    gHwnd = ::CreateWindowExW(0, wc.lpszClassName, L"epsilon d3d12 target — 注入到这里试试",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!gHwnd) { epsilon::errLine("CreateWindowExW 失败"); return 1; }
    ::ShowWindow(gHwnd, SW_SHOW);

    if (!initD3d12()) { epsilon::errLine("D3D12 初始化失败"); return 2; }
    epsilon::outColored(epsilon::ansi::green, "  [+] D3D12 就绪, 开始出帧\n");

    while (gRunning) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { gRunning = false; break; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!gRunning) break;
        renderOneFrame();
    }

    epsilon::outLine("退出中...");
    return 0;
}
