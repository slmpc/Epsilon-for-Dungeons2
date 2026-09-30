// ============================================================================
//  d3d_probe — 最小复现：在普通进程里做一遍 payload 里那套"临时交换链取
//  Present 地址"的流程，逐步打印，看它到底在哪一步炸。
//
//  为什么要这个: 在测试靶子里，payload 走到 D3D11CreateDeviceAndSwapChain
//  就没下文了(目标进程直接死)。需要在**不受注入影响**的普通进程里复现同一段
//  代码，才能区分两种情况:
//    * 普通进程里也炸  → 是这段代码/驱动环境的问题
//    * 普通进程里正常  → 是注入上下文的问题(线程/窗口站/初始化时机)
// ============================================================================
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>

#include <cstdarg>
#include <cstdio>
#include <iterator>

namespace {

void step(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("  ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}

} // namespace

int main() {
    std::printf("=== d3d_probe: 临时交换链取 Present ===\n");
    std::fflush(stdout);

    HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    step("dxgi.dll      = %p", dxgi);
    HMODULE d3d11 = ::LoadLibraryW(L"d3d11.dll");
    step("d3d11.dll     = %p", d3d11);
    if (!dxgi || !d3d11) { step("加载失败"); return 1; }

    using CreateDXGIFactory1_t = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = reinterpret_cast<CreateDXGIFactory1_t>(
        reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory1")));
    step("CreateDXGIFactory1 = %p", (void*)create_factory);
    if (!create_factory) return 1;

    // IDXGIFactory1 {770AAE78-F26F-4DBA-A829-253C83D1B387}
    const IID iid = {0x770AAE78, 0xF26F, 0x4DBA, {0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87}};
    void* factory = nullptr;
    const HRESULT fhr = create_factory(iid, &factory);
    step("CreateDXGIFactory1 hr=0x%08X factory=%p", (unsigned)fhr, factory);
    if (FAILED(fhr) || !factory) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mcd2_probe_wnd";
    const ATOM atom = ::RegisterClassExW(&wc);
    step("RegisterClassExW atom=%u err=%lu", atom, ::GetLastError());

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    step("CreateWindowExW hwnd=%p", hwnd);

    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        reinterpret_cast<void*>(::GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")));
    step("D3D11CreateDeviceAndSwapChain = %p", (void*)create);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferDesc.Width = 64;
    scd.BufferDesc.Height = 64;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};

    IDXGISwapChain*      swapchain = nullptr;
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL    obtained{};

    step("sizeof(DXGI_SWAP_CHAIN_DESC) = %zu  (真实布局由 SDK 保证)", sizeof(DXGI_SWAP_CHAIN_DESC));
    step("调 HARDWARE...");
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                        wanted, (UINT)std::size(wanted), D3D11_SDK_VERSION,
                        &scd, &swapchain, &device, &obtained, &context);
    step("HARDWARE hr=0x%08X swapchain=%p device=%p context=%p obtained=%d",
         (unsigned)hr, swapchain, device, context, (int)obtained);

    if (FAILED(hr) || !swapchain) {
        if (swapchain) { swapchain->Release(); swapchain = nullptr; }
        if (context)   { context->Release();   context = nullptr; }
        if (device)    { device->Release();    device = nullptr; }
        step("调 WARP...");
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                    wanted, (UINT)std::size(wanted), D3D11_SDK_VERSION,
                    &scd, &swapchain, &device, &obtained, &context);
        step("WARP hr=0x%08X swapchain=%p", (unsigned)hr, swapchain);
    }

    if (SUCCEEDED(hr) && swapchain) {
        void** vtbl = *reinterpret_cast<void***>(swapchain);
        step("vtable = %p", (void*)vtbl);
        for (int i = 6; i <= 10; ++i) {
            step("  vtbl[%d] = %p", i, vtbl[i]);
        }
        void* present = vtbl[8];
        step(">>> Present = %p", present);

        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(present, &mbi, sizeof(mbi))) {
            step("    所在模块 = %p  Type=%lu  Protect=0x%lX",
                 mbi.AllocationBase, mbi.Type, mbi.Protect);
        }
        step("    前 16 字节:");
        auto* b = static_cast<const unsigned char*>(present);
        std::printf("      ");
        for (int i = 0; i < 16; ++i) std::printf("%02X ", b[i]);
        std::printf("\n");
    }

    if (context)   context->Release();
    if (device)    device->Release();
    if (swapchain) swapchain->Release();
    if (hwnd) ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    reinterpret_cast<IUnknown*>(factory)->Release();

    std::printf("=== 探测完成, 未崩溃 ===\n");
    return 0;
}
