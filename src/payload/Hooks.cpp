// ============================================================================
//  hooks.cpp — MinHook 挂钩(目标渲染 API: D3D12)
//
//  ⚠️ 本模块不在默认初始化路径上。
//     默认流程只做: 注入 → 连管道 → 定位引擎 → 提供只读采集命令。
//     帧钩子由 `hook` 命令显式触发(见 hooks.h 的说明)。
//
//  定位 Present: 建一个**临时 D3D12 设备**并建交换链, 从其 vtable 取
//  IDXGISwapChain::Present。
//
//    为什么是 D3D12 而不是 D3D11: 目标游戏就是 D3D12(Dungeons-Win64-Shipping
//    用 UE5 + D3D12)。IDXGISwapChain 是同一个 COM 接口、实现在同一个
//    dxgi.dll 里, 所以从哪个 API 拿到交换链, vtable 都是同一份; 既然目标是
//    D3D12, 就直接用 D3D12 建, 少引入一个 API 面。
//
//    D3D12 建交换链必须先有命令队列, 所以完整链路是:
//      D3D12CreateDevice → CreateCommandQueue → CreateSwapChainForHwnd
//
//  两个必须注意的点:
//    * dxgi.dll / d3d12.dll 加载后**故意不 FreeLibrary** —— 返回的 Present
//      地址就在 dxgi 的代码段里, 放掉引用计数会让模块卸载, 地址立刻变野指针。
//    * 取到的地址必须落在可执行模块内才采信(挡住索引数错的情况)。
//
//  已用 tests/d3d_probe 在普通进程里逐字节验证过:
//      sizeof(DXGI_SWAP_CHAIN_DESC) = 72, vtbl[8] = Present, 段属性 EXECUTE_READ
// ============================================================================
#include "payload/Hooks.h"

#include "common/Text.h"
#include "payload/Overlay.h"
#include "payload/Payload.h"   // trace(): 关键步骤落盘, 崩了才查得到

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
// 用官方头拿真实结构布局。IDXGISwapChain 的 vtable 索引永远不要凭记忆写 ——
// 错一次就是把 jmp 写到别的函数头上, 目标进程直接崩。
// 注意: IDXGIFactory2 / IDXGISwapChain1 / DXGI_SWAP_CHAIN_DESC1 都在
// dxgi1_2.h 之后才定义, 光 include dxgi.h 是不够的。
#include <dxgi1_6.h>
#include <d3d12.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>

namespace epsilon::payload {
namespace {

using PresentFn = HRESULT(__stdcall*)(void* /*this*/, UINT /*syncInterval*/, UINT /*flags*/);
using ExecuteFn = void(__stdcall*)(void* /*this*/, UINT, ID3D12CommandList* const*);

PresentFn gOriginal = nullptr;
ExecuteFn gOriginalExec = nullptr;
std::atomic<FrameCallback*> gCallback{nullptr};
std::atomic<uint64_t> gFrames{0};
std::atomic<uint64_t> gFirstTick{0};
std::atomic<uint64_t> gLastTick{0};
uint64_t gPresentAddr = 0;

// 游戏自己那条 DIRECT 命令队列。
//
// 为什么要捕获它: D3D12 的 flip 模型交换链与**创建它的那个队列**绑定。如果
// 覆盖层在另一条队列上渲染, 命令确实执行了、围栏也确实完成了, 但画面不会
// 进入呈现结果 —— 现象就是"一切都成功, 屏幕上什么都没有"。
// 我们自己的临时队列因此只能用来取 vtable, 不能用来画。
std::atomic<void*> gGameQueue{nullptr};

// 自检: 地址必须落在一个**已映射的可执行模块**里。
// 用来挡住两种情况: vtable 索引数错、取到未映射地址。
// 有这道检查, 索引写错时我们会"定位失败"而不是"把游戏钩崩"。
bool inExecutableModule(void const* p) {
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
// ExecuteCommandLists 的 detour: 只做一件事 —— 记住游戏在用的 DIRECT 队列。
// 用"Present 之前最后一次提交"作为判据, 因为呈现必然紧跟在这一帧的命令之后。
// ---------------------------------------------------------------------------
void __stdcall executeDetour(void* self, UINT num, ID3D12CommandList* const* lists) {
    // 直接调真实方法问队列类型 —— 比手数 vtable 索引可靠。
    const D3D12_COMMAND_QUEUE_DESC qd =
        static_cast<ID3D12CommandQueue*>(self)->GetDesc();
    if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        gGameQueue.store(self, std::memory_order_release);
    }
    gOriginalExec(self, num, lists);
}

// ---------------------------------------------------------------------------
// detour 本体
//
// 顺序: 覆盖层(首次 init + 每帧 render) → 帧计数与用户回调 → 原 Present。
// 覆盖层必须在原 Present **之前**渲染完并等围栏 —— 否则我们可能在后缓冲
// 已被呈现出去之后才去写它。
// ---------------------------------------------------------------------------
HRESULT __stdcall presentDetour(void* self, UINT syncInterval, UINT flags) {
    // ---- 覆盖层 ----
    // 用 SEH 兜住: 覆盖层里的 D3D12 调用一旦出错, 只把覆盖层自己关掉,
    // 绝不能连累游戏的渲染线程。这是安全机制, 不是 fallback 方案。
    static std::atomic<bool> overlayAttempted{false};
    static std::atomic<bool> overlayBroken{false};

    if (!overlayBroken.load(std::memory_order_relaxed)) {
        __try {
            auto* sc = static_cast<IDXGISwapChain*>(self);
            if (!overlay::initialized()) {
                if (!overlayAttempted.exchange(true)) overlay::init(sc);
            }
            if (overlay::initialized()) overlay::render(sc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            overlayBroken.store(true, std::memory_order_relaxed);
            trace("  [overlay] 渲染时抛异常, 已停用覆盖层(游戏不受影响)");
        }
    }

    // ---- 帧计数与用户回调 ----
    const uint64_t n = gFrames.fetch_add(1, std::memory_order_relaxed) + 1;

    if (n == 1) gFirstTick.store(::GetTickCount64(), std::memory_order_relaxed);
    gLastTick.store(::GetTickCount64(), std::memory_order_relaxed);

    // 回调在渲染线程上, 必须极轻。约定: 只做打标记/计数, 不做遍历。
    if (auto* cb = gCallback.load(std::memory_order_acquire)) {
        if (*cb) {
            __try {
                (*cb)(static_cast<uint32_t>(n), static_cast<int>(syncInterval),
                      static_cast<int>(flags));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // 回调里出任何异常都不能连累渲染线程 —— 直接丢弃这一帧的回调。
            }
        }
    }

    return gOriginal(self, syncInterval, flags);
}

// ---------------------------------------------------------------------------
//  临时 D3D12 设备的创建序列
//
//  放在一个独立的 __try 函数里, 因为:
//    1. 之前的现象是"日志停在调 D3D 那一行, 目标进程直接消失", 完全查不到死因。
//       SEH 能把异常码抓出来, 崩溃就变成一条可读信息。
//    2. SEH 函数体内不能有需要析构的对象(MSVC C2712), 所以参数一律裸类型。
// ---------------------------------------------------------------------------
struct D3d12Probe {
    PFN_D3D12_CREATE_DEVICE createDevice = nullptr;
    IDXGIFactory2*          factory = nullptr;
    HWND                    hwnd = nullptr;
    ID3D12Device**          outDevice = nullptr;
    ID3D12CommandQueue**    outQueue = nullptr;
    IDXGISwapChain1**       outSwapchain = nullptr;
    DWORD                   excCode = 0;
    HRESULT                 hr = E_FAIL;
};

void probeD3d12Impl(D3d12Probe* a) {
    __try {
        a->hr = a->createDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(a->outDevice));
        if (FAILED(a->hr) || !*a->outDevice) return;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        a->hr = (*a->outDevice)->CreateCommandQueue(
            &qd, __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(a->outQueue));
        if (FAILED(a->hr) || !*a->outQueue) return;

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = 64;
        scd.Height = 64;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

        a->hr = a->factory->CreateSwapChainForHwnd(
            *a->outQueue, a->hwnd, &scd, nullptr, nullptr, a->outSwapchain);
    } __except (a->excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        a->hr = E_FAIL;
    }
}

// ---------------------------------------------------------------------------
// 定位 Present(唯一路径, 不做 fallback)
//
// outExecTarget: 顺便把 ID3D12CommandQueue::ExecuteCommandLists 的地址带出来。
//                  调用方要靠挂钩子去捕获游戏在用的那条命令队列。
// ---------------------------------------------------------------------------
uint64_t findPresent(std::string& how, uint64_t* outExecTarget) {
    if (outExecTarget) *outExecTarget = 0;
    // ⚠️ 故意不 FreeLibrary, 理由见文件头。
    HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { how = "dxgi.dll 不可用"; return 0; }

    HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
    if (!d3d12) { how = "d3d12.dll 不可用"; return 0; }

    using CreateDXGIFactory1_t = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = reinterpret_cast<CreateDXGIFactory1_t>(
        reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory1")));
    auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12CreateDevice")));
    if (!createFactory || !createDevice) {
        how = "dxgi!CreateDXGIFactory1 或 d3d12!D3D12CreateDevice 缺失";
        return 0;
    }

    // ---- 1) DXGI factory(不需要设备) ----
    IUnknown* factoryRaw = nullptr;
    const HRESULT fhr = createFactory(__uuidof(IDXGIFactory1),
                                       reinterpret_cast<void**>(&factoryRaw));
    if (FAILED(fhr) || !factoryRaw) {
        how = fmt("CreateDXGIFactory1 失败 hr={:#x}", static_cast<uint32_t>(fhr));
        return 0;
    }

    IDXGIFactory2* factory2 = nullptr;
    if (FAILED(factoryRaw->QueryInterface(__uuidof(IDXGIFactory2),
                                           reinterpret_cast<void**>(&factory2))) ||
        !factory2) {
        factoryRaw->Release();
        how = "拿不到 IDXGIFactory2";
        return 0;
    }

    // ---- 2) 临时窗口(只为满足 CreateSwapChainForHwnd) ----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"epsilonTmpWnd";
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"epsilon", WS_OVERLAPPEDWINDOW,
                                  0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        factory2->Release();
        factoryRaw->Release();
        how = fmt("CreateWindowExW 失败 err={}", ::GetLastError());
        return 0;
    }

    // ---- 3) 设备 → 队列 → 交换链 (全程 SEH 保护) ----
    ID3D12Device*       device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1*    swapchain = nullptr;

    D3d12Probe probe;
    probe.createDevice = createDevice;
    probe.factory = factory2;
    probe.hwnd = hwnd;
    probe.outDevice = &device;
    probe.outQueue = &queue;
    probe.outSwapchain = &swapchain;
    probeD3d12Impl(&probe);

    trace(fmt("    [hook] D3D12 探测 hr={:#x} device={} queue={} swapchain={}{}",
              static_cast<uint32_t>(probe.hr),
              hex(reinterpret_cast<uint64_t>(device), 16),
              hex(reinterpret_cast<uint64_t>(queue), 16),
              hex(reinterpret_cast<uint64_t>(swapchain), 16),
              probe.excCode ? fmt("  SEH异常码={:#x}", probe.excCode) : std::string{}));

    uint64_t present = 0;
    if (probe.excCode) {
        how = fmt("D3D12 探测抛异常, 异常码 {:#x}", probe.excCode);
    } else if (SUCCEEDED(probe.hr) && swapchain) {
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
            // 已对着 SDK 的 dxgi.h 逐个数过, 并用 tests/d3d_probe 在运行时验证
            // 过 vtbl[8] 落在 dxgi 的可执行段。别凭记忆改这个数字。
            void* candidate = vtbl[8];
            if (candidate && inExecutableModule(candidate)) {
                present = reinterpret_cast<uint64_t>(candidate);
                how = "临时 D3D12 交换链 vtable[8]";
            } else {
                how = fmt("vtable[8]={} 不在可执行模块内, 判定无效",
                          hex(reinterpret_cast<uint64_t>(candidate), 16));
            }
        }
    } else {
        how = fmt("D3D12 探测失败 hr={:#x}", static_cast<uint32_t>(probe.hr));
    }

    // 顺便取 ExecuteCommandLists 的地址。索引推导:
    //   IUnknown(3) + ID3D12Object(4: GetPrivateData, SetPrivateData,
    //   SetPrivateDataInterface, SetName) + ID3D12DeviceChild(1: GetDevice)
    //   + ID3D12Pageable(0) + UpdateTileMappings, CopyTileMappings = 10
    if (outExecTarget && queue) {
        void** qvtbl = *reinterpret_cast<void***>(queue);
        if (qvtbl) {
            void* exec = qvtbl[10];
            if (exec && inExecutableModule(exec)) {
                *outExecTarget = reinterpret_cast<uint64_t>(exec);
            }
        }
    }

    // ---- 4) 收尾: 我们只要地址, 临时对象全部释放 ----
    if (swapchain) swapchain->Release();
    if (queue)     queue->Release();
    if (device)    device->Release();
    if (hwnd)      ::DestroyWindow(hwnd);
    factory2->Release();
    factoryRaw->Release();

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
    trace("  [hook] 定位 Present (临时 D3D12 交换链 vtable)...");
    uint64_t execTarget = 0;
    const uint64_t addr = findPresent(how, &execTarget);
    if (!addr) {
        status_.error = fmt("定位 Present 失败: {}", how);
        return false;
    }
    trace(fmt("  [hook] Present = {} ({})", hex(addr, 16), how));

    target_ = reinterpret_cast<void*>(addr);
    gPresentAddr = addr;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        status_.error = fmt("MH_Initialize 失败: {}", static_cast<int>(st));
        return false;
    }

    // 先挂钩 ExecuteCommandLists —— 覆盖层初始化时要用它捕获到的队列。
    if (execTarget) {
        st = MH_CreateHook(reinterpret_cast<void*>(execTarget),
                           reinterpret_cast<void*>(&executeDetour),
                           reinterpret_cast<void**>(&gOriginalExec));
        if (st == MH_OK) {
            st = MH_EnableHook(reinterpret_cast<void*>(execTarget));
        }
        if (st == MH_OK) {
            trace(fmt("  [hook] 已挂上 ExecuteCommandLists @ {} (用于捕获游戏队列)",
                      hex(execTarget, 16)));
        } else {
            // 捕获不到队列只是让覆盖层画不出来, 不影响采集功能, 所以不致命。
            trace(fmt("  [hook] ExecuteCommandLists 挂钩失败 st={}", static_cast<int>(st)));
        }
    } else {
        trace("  [hook] 未取到 ExecuteCommandLists 地址, 覆盖层可能无法渲染");
    }

    st = MH_CreateHook(target_, reinterpret_cast<void*>(&presentDetour),
                       reinterpret_cast<void**>(&gOriginal));
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

    trampoline_ = reinterpret_cast<void*>(gOriginal);
    status_.installed = true;
    status_.how = how;
    status_.target = fmt("IDXGISwapChain::Present @ {}", hex(addr, 16));
    status_.error.clear();
    trace(fmt("  [hook] 已挂上 {}", status_.target));
    return true;
}

void Hooks::setFrameCallback(FrameCallback cb) {
    FrameCallback* heap = nullptr;
    if (cb) heap = new FrameCallback(std::move(cb));
    FrameCallback* old = gCallback.exchange(heap, std::memory_order_acq_rel);
    delete old;
}

void Hooks::onFrame(int syncInterval, int flags) {
    ++frames_;
    status_.frameCount = gFrames.load(std::memory_order_relaxed);

    const uint64_t first = gFirstTick.load(std::memory_order_relaxed);
    const uint64_t last = gLastTick.load(std::memory_order_relaxed);
    if (first && last > first) {
        const double secs = static_cast<double>(last - first) / 1000.0;
        if (secs > 0.5) status_.fps = static_cast<double>(status_.frameCount) / secs;
    }
    (void)syncInterval;
    (void)flags;
}

uint64_t presentAddress() { return gPresentAddr; }
uint64_t frameCount() { return gFrames.load(std::memory_order_relaxed); }
void* presentQueue() { return gGameQueue.load(std::memory_order_acquire); }

} // namespace epsilon::payload
