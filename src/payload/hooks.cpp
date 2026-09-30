// ============================================================================
//  hooks.cpp — MinHook 挂钩实现
//
//  设计取舍(重要):
//    MinHook 建钩后, 目标函数入口会变成一条 5 字节 jmp 指向 trampoline。
//    如果原本就存在指向该函数入口的间接调用(比如增量链接生成的 ILT 跳转桩),
//    那条 jmp 会被一并改掉, 于是 trampoline 递归调用自己 → 爆栈。
//    因此这里**从 vtable 取地址后再 CreateHook**, 并且在链接时禁用了
//    增量链接(/INCREMENTAL:NO), 从两头规避这个问题。
//
//  调用约定:
//    Present 是 COM 方法, x64 下即 this=RCX, syncInterval=EDX, flags=R8D。
//    我们的 detour 直接用相同签名声明, 编译器生成的形参访问与调用方完全一致,
//    所以不需要写裸汇编蹦床。
//
//  定位 Present 只有一条路: 建临时 D3D11 交换链, 从其 vtable 取。
//    * dxgi/d3d11 加载后**故意不 FreeLibrary** —— 返回的地址在它们代码段里,
//      放掉引用计数会让模块卸载, 地址立刻变野指针。
//    * 取到的地址必须落在可执行模块内才采信(挡住索引数错的情况)。
//    * 已用 tests/d3d_probe 在普通进程里逐字节验证过:
//        sizeof(DXGI_SWAP_CHAIN_DESC) = 72, vtbl[8] = Present, 段属性 = EXECUTE_READ
// ============================================================================
#include "payload/hooks.h"

#include "common/pe_image.h"
#include "common/text.h"
#include "payload/payload.h"   // trace(): 关键步骤落盘, 崩了才查得到

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
// 用官方头拿 DXGI_SWAP_CHAIN_DESC / D3D11 的**真实**结构布局。
// 一开始我手搓过一个"看起来差不多"的 SwapChainDesc —— 40 字节, 而真实的是
// 72 字节。D3D 按真实大小读栈内存 → 读到垃圾 → 目标进程直接崩。
// 教训: 这种 ABI 结构体永远不要手写, 让编译器从 SDK 头里取。
// 这里只用类型, 函数仍走 GetProcAddress, 所以不需要链接 dxgi.lib/d3d11.lib。
#include <dxgi.h>
#include <d3d11.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>

namespace mcd2::payload {
namespace {

using PresentFn = HRESULT(__stdcall*)(void* /*this*/, UINT /*syncInterval*/, UINT /*flags*/);

PresentFn g_original = nullptr;
std::atomic<FrameCallback*> g_callback{nullptr};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_first_tick{0};
std::atomic<uint64_t> g_last_tick{0};
uint64_t g_present_addr = 0;

// 自检: 地址必须落在一个**已映射的可执行模块**里。
// 用来挡住两种情况: vtable 索引数错、取到未映射地址。
// 有这道检查, 索引写错时我们会"定位失败"而不是"把游戏钩崩"。
bool in_executable_module(void const* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.Type != MEM_IMAGE) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExecutable) != 0;
}

// ---------------------------------------------------------------------------
// detour 本体
// ---------------------------------------------------------------------------
HRESULT __stdcall present_detour(void* self, UINT sync_interval, UINT flags) {
    const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;

    if (n == 1) g_first_tick.store(::GetTickCount64(), std::memory_order_relaxed);
    g_last_tick.store(::GetTickCount64(), std::memory_order_relaxed);

    // 回调在渲染线程上, 必须极轻。约定: 只做打标记/计数, 不做遍历。
    if (auto* cb = g_callback.load(std::memory_order_acquire)) {
        if (*cb) {
            __try {
                (*cb)(static_cast<uint32_t>(n), static_cast<int>(sync_interval),
                      static_cast<int>(flags));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // 回调里出任何异常都不能连累渲染线程 —— 直接丢弃这一帧的回调。
            }
        }
    }

    return g_original(self, sync_interval, flags);
}

// ---------------------------------------------------------------------------
// 定位 Present: 建临时 D3D11 交换链, 从 vtable 取。
//
// 同一进程内 dxgi 的 IDXGISwapChain 实现共享 vtable, 所以临时交换链上的
// Present 就是游戏在用的那个。这是 overlay 领域的标准做法。
// ---------------------------------------------------------------------------
uint64_t find_present(std::string& how) {
    // ⚠️ 故意不 FreeLibrary, 理由见文件头。
    HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { how = "dxgi.dll 不可用"; return 0; }
    trace("    [hook] dxgi.dll 已加载");

    HMODULE d3d11 = ::LoadLibraryW(L"d3d11.dll");
    if (!d3d11) { how = "d3d11.dll 不可用"; return 0; }
    trace("    [hook] d3d11.dll 已加载");

    using CreateDXGIFactory1_t = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = reinterpret_cast<CreateDXGIFactory1_t>(
        reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory1")));
    if (!create_factory) { how = "dxgi!CreateDXGIFactory1 缺失"; return 0; }

    // IDXGIFactory1 {770AAE78-F26F-4DBA-A829-253C83D1B387}
    const IID iid = {0x770AAE78, 0xF26F, 0x4DBA, {0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87}};
    IUnknown* factory = nullptr;
    const HRESULT fhr = create_factory(iid, reinterpret_cast<void**>(&factory));
    trace(fmt("    [hook] CreateDXGIFactory1 hr={:#x}", static_cast<uint32_t>(fhr)));
    if (FAILED(fhr) || !factory) {
        how = fmt("CreateDXGIFactory1 失败 hr={:#x}", static_cast<uint32_t>(fhr));
        return 0;
    }

    // 临时窗口(不显示内容, 只为满足交换链对 OutputWindow 的要求)
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mcd2_tmp_wnd";
    ::RegisterClassExW(&wc);   // 已注册会失败, 无所谓 —— 下面按类名建窗口

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"mcd2", WS_OVERLAPPEDWINDOW,
                                  0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    trace(fmt("    [hook] 临时窗口 hwnd={}", hex(reinterpret_cast<uint64_t>(hwnd), 16)));

    uint64_t present = 0;

    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        reinterpret_cast<void*>(::GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")));

    if (!create || !hwnd) {
        how = !create ? "d3d11!D3D11CreateDeviceAndSwapChain 缺失" : "CreateWindowExW 失败";
    } else {
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

        const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};

        IDXGISwapChain*      swapchain = nullptr;
        ID3D11Device*        device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL    obtained{};

        trace("    [hook] 调 D3D11CreateDeviceAndSwapChain(HARDWARE)...");
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                            wanted, static_cast<UINT>(std::size(wanted)),
                            D3D11_SDK_VERSION, &scd, &swapchain, &device, &obtained, &context);
        trace(fmt("    [hook] HARDWARE hr={:#x} swapchain={}",
                  static_cast<uint32_t>(hr), hex(reinterpret_cast<uint64_t>(swapchain), 16)));

        // 硬件设备不可用时退到 WARP(软件光栅化)。这不是"另一种定位方案",
        // 只是同一个设备创建的驱动类型选择 —— 交换链实现是同一份。
        if (FAILED(hr) || !swapchain) {
            if (swapchain) { swapchain->Release(); swapchain = nullptr; }
            if (context)   { context->Release();   context = nullptr; }
            if (device)    { device->Release();    device = nullptr; }
            trace("    [hook] 硬件设备不可用, 改试 WARP...");
            hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                        wanted, static_cast<UINT>(std::size(wanted)),
                        D3D11_SDK_VERSION, &scd, &swapchain, &device, &obtained, &context);
            trace(fmt("    [hook] WARP hr={:#x} swapchain={}",
                      static_cast<uint32_t>(hr),
                      hex(reinterpret_cast<uint64_t>(swapchain), 16)));
        }

        if (SUCCEEDED(hr) && swapchain) {
            void** vtbl = *reinterpret_cast<void***>(swapchain);
            if (vtbl) {
                // Present 的 vtable 索引 = 各父接口的虚函数个数之和:
                //   IUnknown              : QueryInterface, AddRef, Release         = 3
                //   IDXGIObject           : SetPrivateData, SetPrivateDataInterface,
                //                           GetPrivateData, GetParent              = 4
                //   IDXGIDeviceSubObject  : GetDevice                               = 1
                //   ----------------------------------------------------------------
                //   IDXGISwapChain::Present 是接口自身第一个方法 → 索引 3+4+1 = 8
                //
                // 已对着 SDK 的 dxgi.h 逐个数过, 并用 tests/d3d_probe 在运行时
                // 验证过 vtbl[8] 落在 dxgi 的可执行段。别凭记忆改这个数字。
                void* candidate = vtbl[8];
                if (candidate && in_executable_module(candidate)) {
                    present = reinterpret_cast<uint64_t>(candidate);
                    how = "临时 D3D11 交换链 vtable[8]";
                } else {
                    how = fmt("vtable[8]={} 不在可执行模块内, 判定无效",
                              hex(reinterpret_cast<uint64_t>(candidate), 16));
                }
            }
            context->Release();
            device->Release();
            swapchain->Release();
        } else if (how.empty()) {
            how = fmt("D3D11CreateDeviceAndSwapChain 失败 hr={:#x}", static_cast<uint32_t>(hr));
        }
    }

    if (hwnd) ::DestroyWindow(hwnd);
    if (factory) factory->Release();

    return present;
}

} // namespace

// ---------------------------------------------------------------------------
Hooks& hooks() {
    static Hooks h;
    return h;
}

Hooks::~Hooks() = default;

bool Hooks::install() {
    if (status_.installed) return true;
    status_.attempted = true;

    std::string how;
    trace("  [hook] 定位 Present (临时交换链 vtable)...");
    const uint64_t addr = find_present(how);
    if (!addr) {
        status_.error = fmt("定位 Present 失败: {}", how);
        return false;
    }
    trace(fmt("  [hook] Present = {} ({})", hex(addr, 16), how));

    target_ = reinterpret_cast<void*>(addr);
    g_present_addr = addr;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        status_.error = fmt("MH_Initialize 失败: {}", static_cast<int>(st));
        return false;
    }

    st = MH_CreateHook(target_, reinterpret_cast<void*>(&present_detour),
                       reinterpret_cast<void**>(&g_original));
    if (st != MH_OK) {
        status_.error = fmt("MH_CreateHook 失败: {} (Present={})",
                            static_cast<int>(st), hex(addr, 16));
        return false;
    }

    st = MH_EnableHook(target_);
    if (st != MH_OK) {
        MH_RemoveHook(target_);
        status_.error = fmt("MH_EnableHook 失败: {}", static_cast<int>(st));
        return false;
    }

    trampoline_ = reinterpret_cast<void*>(g_original);
    status_.installed = true;
    status_.how = how;
    status_.target = fmt("IDXGISwapChain::Present @ {}", hex(addr, 16));
    status_.error.clear();
    trace(fmt("  [hook] 已挂上 {}", status_.target));
    return true;
}

void Hooks::set_frame_callback(FrameCallback cb) {
    FrameCallback* heap = nullptr;
    if (cb) heap = new FrameCallback(std::move(cb));
    FrameCallback* old = g_callback.exchange(heap, std::memory_order_acq_rel);
    delete old;
}

void Hooks::on_frame(int sync_interval, int flags) {
    ++frames_;
    status_.frame_count = g_frames.load(std::memory_order_relaxed);

    const uint64_t first = g_first_tick.load(std::memory_order_relaxed);
    const uint64_t last = g_last_tick.load(std::memory_order_relaxed);
    if (first && last > first) {
        const double secs = static_cast<double>(last - first) / 1000.0;
        if (secs > 0.5) status_.fps = static_cast<double>(status_.frame_count) / secs;
    }
    (void)sync_interval;
    (void)flags;
}

uint64_t present_address() { return g_present_addr; }
uint64_t frame_count() { return g_frames.load(std::memory_order_relaxed); }

} // namespace mcd2::payload
