// ============================================================================
//  overlay.cpp — D3D12 ImGui 覆盖层实现
//
//  每帧的 GPU 侧顺序(必须严格保持, 否则会把画面搞花或挂住):
//    1. 取当前后缓冲索引 → 取后缓冲资源
//    2. Reset 该索引的命令分配器 → Reset 命令列表
//    3. 屏障 PRESENT → RENDER_TARGET
//    4. 绑定我们自己为该后缓冲建的 RTV
//    5. 绑定 SRV 堆 → ImGui_ImplDX12_RenderDrawData
//    6. 屏障 RENDER_TARGET → PRESENT
//    7. Close → ExecuteCommandLists(我们的队列) → 等围栏
//
//  第 7 步的等待很关键: 必须等我们的队列真的画完, 才能把控制权交回给游戏的
//  Present。否则游戏可能在我们还在写后缓冲时就开始下一帧。
// ============================================================================
#include "payload/Overlay.h"

#include "common/Text.h"
#include "payload/Hooks.h"
#include "payload/Payload.h"
#include "payload/Runtime.h"
#include "payload/ue/Engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// dxgi1_6.h 会把 1_1..1_6 全部拉进来 —— IDXGISwapChain1/2/3、IDXGIFactory2
// 分别定义在不同层级(1_2 / 1_3 / 1_4), 只 include dxgi.h 不够。
#include <dxgi1_6.h>
#include <d3d12.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

// imgui_impl_win32.h 没有导出这个声明, 得自己声明(官方示例就是这么做的)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace epsilon::payload::overlay {
namespace {

constexpr int  kFramesInFlight = 3;
constexpr int  kSrvHeapSize    = 64;      // ImGui 1.92 是动态纹理, 会按需分配
constexpr DWORD kToggleKey     = VK_INSERT;

// --------------------------------------------------------------- SRV 分配器
// ImGui 的后端把"分配纹理描述符"这件事交给宿主, 所以我们要自己管一个小堆。
struct SrvAllocator {
    ID3D12DescriptorHeap*        heap = nullptr;
    UINT                         inc = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE  cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE  gpuBase{};
    bool                         used[kSrvHeapSize]{};
    int                          allocCount = 0;   // 诊断: 后端一共要了几个描述符
};

// --------------------------------------------------------------- 全局状态
struct State {
    ID3D12Device*              device = nullptr;
    ID3D12CommandQueue*        queue = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandAllocator*    alloc[kFramesInFlight]{};
    ID3D12Fence*               fence = nullptr;
    HANDLE                     fenceEvent = nullptr;
    UINT64                     fenceValue = 0;

    ID3D12DescriptorHeap*      rtvHeap = nullptr;
    UINT                       rtvInc = 0;
    UINT                       backbufferCount = 0;
    DXGI_FORMAT                rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT                       width = 0;      // 渲染目标尺寸, 用来设视口
    UINT                       height = 0;

    SrvAllocator               srv;

    HWND                       hwnd = nullptr;
    WNDPROC                    origWndproc = nullptr;

    UINT                       frameIndex = 0;
    bool                       initialized = false;
    bool                       visible = true;
    std::string                error;
};

State    g;
std::mutex gMu;

// ImGui 的帧率统计与画面节流用
float  gUiFps = 0.0f;
double gLastExpensiveRead = 0.0;

// 对象浏览器的缓存(引擎遍历比较慢, 限频刷新)
struct ObjRow { std::string cls; std::string name; uint64_t addr; };
std::vector<ObjRow> gObjCache;
char                gFilter[128]{};
int                 gObjTotal = 0;
std::string         gObjNote;

// --------------------------------------------------------------- 工具
double nowSeconds() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) ::QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

void srvAlloc(ImGui_ImplDX12_InitInfo* info,
               D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
               D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    auto* a = static_cast<SrvAllocator*>(info->UserData);
    if (!a) return;
    for (int i = 0; i < kSrvHeapSize; ++i) {
        if (a->used[i]) continue;
        a->used[i] = true;
        ++a->allocCount;
        outCpu->ptr = a->cpuBase.ptr + static_cast<SIZE_T>(i) * a->inc;
        outGpu->ptr = a->gpuBase.ptr + static_cast<UINT64>(i) * a->inc;
        return;
    }
    outCpu->ptr = 0;
    outGpu->ptr = 0;
}

void srvFree(ImGui_ImplDX12_InitInfo* info,
              D3D12_CPU_DESCRIPTOR_HANDLE cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    auto* a = static_cast<SrvAllocator*>(info->UserData);
    if (!a || !cpu.ptr) return;
    const auto i = static_cast<int>((cpu.ptr - a->cpuBase.ptr) / a->inc);
    if (i >= 0 && i < kSrvHeapSize) a->used[i] = false;
}

// 窗口过程: 可见时把输入喂给 ImGui; 切换键始终处理。
LRESULT WINAPI wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (static_cast<DWORD>(wp) == kToggleKey) {
            g.visible = !g.visible;
            return 0;                       // 吃掉, 别让游戏也响应这个键
        }
    }
    if (g.visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return 1;
    }
    return ::CallWindowProcW(g.origWndproc, hwnd, msg, wp, lp);
}

// 等我们的队列把上一批命令执行完。
bool waitGpu() {
    if (!g.queue || !g.fence) return false;
    const UINT64 v = ++g.fenceValue;
    if (FAILED(g.queue->Signal(g.fence, v))) return false;
    if (g.fence->GetCompletedValue() >= v) return true;
    if (FAILED(g.fence->SetEventOnCompletion(v, g.fenceEvent))) return false;
    return ::WaitForSingleObject(g.fenceEvent, 2000) == WAIT_OBJECT_0;
}

// --------------------------------------------------------------- 资源创建
bool createResources(IDXGISwapChain* sc) {
    // 游戏自己的设备 —— 我们所有资源都建在它上面, 这样后缓冲才互通
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device),
                             reinterpret_cast<void**>(&g.device))) || !g.device) {
        g.error = "swapchain->GetDevice(ID3D12Device) 失败";
        return false;
    }

    // 后缓冲格式与数量(决定 RTV 堆大小和 ImGui 的 RTVFormat), 以及尺寸(视口用)
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(sc->GetDesc(&desc))) {
        g.rtvFormat = desc.BufferDesc.Format;
        g.backbufferCount = desc.BufferCount ? desc.BufferCount : 2;
        g.width = desc.BufferDesc.Width;
        g.height = desc.BufferDesc.Height;
    } else {
        g.backbufferCount = 2;
    }
    if (g.width == 0 || g.height == 0) {
        // 有些交换链的 BufferDesc 尺寸是 0(由窗口决定), 那就问窗口
        RECT rc{};
        if (g.hwnd && ::GetClientRect(g.hwnd, &rc)) {
            g.width = static_cast<UINT>(rc.right - rc.left);
            g.height = static_cast<UINT>(rc.bottom - rc.top);
        }
    }
    if (g.width == 0)  g.width = 1920;
    if (g.height == 0) g.height = 1080;

    // 命令队列: 必须用**游戏自己的那条**。
    //   D3D12 的 flip 模型交换链与创建它的队列绑定。我们自己的队列只能用来
    //   取 vtable; 在它上面渲染会出现"命令执行了、围栏完成了、画面却没变"
    //   这种极具误导性的现象。
    //   队列由 hooks.cpp 里的 ExecuteCommandLists 钩子在 Present 之前捕获。
    g.queue = static_cast<ID3D12CommandQueue*>(presentQueue());
    if (!g.queue) {
        g.error = "还没捕获到游戏的命令队列 —— 无法保证渲染进入呈现结果";
        return false;
    }

    // 命令分配器(每帧一个)
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g.alloc[i])))) {
            g.error = fmt("CreateCommandAllocator[{}] 失败", i);
            return false;
        }
    }

    // 命令列表(先建了再 Close, 后面每帧 Reset)
    if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g.alloc[0], nullptr,
                                           IID_PPV_ARGS(&g.list)))) {
        g.error = "CreateCommandList 失败";
        return false;
    }
    g.list->Close();

    // 围栏
    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        g.error = "CreateFence 失败";
        return false;
    }
    g.fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.fenceEvent) {
        g.error = "CreateEventW 失败";
        return false;
    }

    // RTV 堆: 每个后缓冲一个描述符
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = g.backbufferCount;
    rh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g.rtvHeap)))) {
        g.error = "RTV 描述符堆创建失败";
        return false;
    }
    g.rtvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV 堆: 给 ImGui 的纹理用, 必须带 SHADER_VISIBLE
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kSrvHeapSize;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g.srv.heap)))) {
        g.error = "SRV 描述符堆创建失败";
        return false;
    }
    g.srv.inc = g.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.srv.cpuBase = g.srv.heap->GetCPUDescriptorHandleForHeapStart();
    g.srv.gpuBase = g.srv.heap->GetGPUDescriptorHandleForHeapStart();

    // 给每个后缓冲建 RTV
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g.backbufferCount; ++i) {
            ID3D12Resource* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
                g.device->CreateRenderTargetView(bb, nullptr, h);
                bb->Release();
            }
            h.ptr += g.rtvInc;
        }
    }
    return true;
}

// ImGui 初始化
bool createImGui(IDXGISwapChain* sc) {
    // 窗口句柄 —— 输入要靠它
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1),
                                     reinterpret_cast<void**>(&sc1))) && sc1) {
        sc1->GetHwnd(&g.hwnd);
        sc1->Release();
    }
    if (!g.hwnd) {
        // 退路: 用当前前台窗口(理论上不该走到这)
        g.hwnd = ::GetForegroundWindow();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // 别跟游戏的鼠标模式打架
    io.IniFilename = nullptr;                                  // 不落盘 imgui.ini

    ImGui::StyleColorsDark();

    // 界面统一用英文, 所以直接用 ImGui 内置的默认字体即可 ——
    // 不加载中文字体, 省掉 18 MB 的字体依赖和一份大图集。
    // (内置字体只有 ASCII, 一旦混入中文就会显示成 '?', 所以 UI 文案里不要写中文。)

    if (!ImGui_ImplWin32_Init(g.hwnd)) {
        g.error = "ImGui_ImplWin32_Init 失败";
        return false;
    }

    ImGui_ImplDX12_InitInfo info;
    info.Device = g.device;
    info.CommandQueue = g.queue;
    info.NumFramesInFlight = kFramesInFlight;
    info.RTVFormat = g.rtvFormat;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = &g.srv;
    info.SrvDescriptorHeap = g.srv.heap;
    info.SrvDescriptorAllocFn = &srvAlloc;
    info.SrvDescriptorFreeFn = &srvFree;

    if (!ImGui_ImplDX12_Init(&info)) {
        g.error = "ImGui_ImplDX12_Init 失败";
        return false;
    }

    // 显式建一次设备对象(PSO / 字体纹理), 并把结果记下来。
    // 为什么必须显式查: 后端在懒加载失败时只有一句
    //     IM_ASSERT(0 && "ImGui_ImplDX12_CreateDeviceObjects() failed!");
    // 而 Release 构建里 IM_ASSERT 展开成空操作 —— 于是 PSO 没建出来时,
    // 一切"看起来"都成功(几何体有、提交成功、围栏完成), 但 SetPipelineState
    // 绑的是空对象, 屏幕上什么都没有。这个坑很难从现象反推。
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        g.error = "ImGui_ImplDX12_CreateDeviceObjects 失败(PSO/字体纹理没建出来)";
        trace(fmt("  [overlay] {}", g.error));
        return false;
    }
    trace("  [overlay] 设备对象(PSO/字体纹理)创建成功");

    // 接收输入
    g.origWndproc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndproc)));
    if (!g.origWndproc) {
        g.error = "SetWindowLongPtrW(GWLP_WNDPROC) 失败";
        return false;
    }
    return true;
}

// --------------------------------------------------------------- 面板
// 全部文案用英文: 内置字体只有 ASCII, 中文会变成 '?'。
void panelEngine() {
    auto& eng = engine();
    const auto& objs = eng.objects();
    const auto& names = eng.names();

    ImGui::TextUnformatted("Engine");
    ImGui::Separator();
    ImGui::Text("Module base   %s", hex(eng.moduleBase(), 16).c_str());
    ImGui::Text("Module size   %s", humanBytes(eng.moduleSize()).c_str());

    if (objs.valid()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "GObjects      %s",
                           hex(objs.address(), 16).c_str());
        ImGui::Text("NumElements   %s (max %s)",
                    thousands(static_cast<uint64_t>(objs.numElements())).c_str(),
                    thousands(static_cast<uint64_t>(objs.maxElements())).c_str());
        ImGui::Text("Layout check  %d/3", objs.validate());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "GObjects      not located");
    }

    if (names.valid()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "GNames        %s",
                           hex(names.blocksAddress(), 16).c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "GNames        not located");
    }

    auto slot = [](char const* label, ue::GlobalSlot const& s) {
        if (s.value) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s  %s %s", label,
                               s.objectClass.c_str(), s.objectName.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s  not located", label);
        }
    };
    slot("GEngine      ", eng.gengine());
    slot("GWorld       ", eng.gworld());
}

void panelHooks() {
    const auto& st = hooks().status();
    ImGui::TextUnformatted("Frame Hook");
    ImGui::Separator();
    if (st.installed) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "installed");
        ImGui::Text("Present       %s", hex(presentAddress(), 16).c_str());
        ImGui::Text("Frames        %s", thousands(frameCount()).c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "not installed");
        if (!st.error.empty()) ImGui::TextWrapped("%s", st.error.c_str());
    }
    ImGui::Text("UI FPS        %.1f", gUiFps);
}

void panelObjects() {
    auto& eng = engine();
    ImGui::TextUnformatted("Object Browser");
    ImGui::Separator();

    if (!eng.objects().valid()) {
        ImGui::TextWrapped("Engine not located. Run 'rescan' in the console first.");
        return;
    }

    ImGui::SetNextItemWidth(-90.0f);
    const bool changed = ImGui::InputTextWithHint("##filter", "name / class substring",
                                                  gFilter, IM_ARRAYSIZE(gFilter));
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(80, 0)) || (changed && gFilter[0])) {
        gLastExpensiveRead = 0.0;      // 强制下次立即刷新
    }

    // 引擎遍历比较慢, 限频到 2 Hz, 避免拖慢渲染线程
    const double t = nowSeconds();
    if (t - gLastExpensiveRead > 0.5) {
        gLastExpensiveRead = t;
        gObjCache.clear();
        std::string f = toLower(gFilter);

        int scanned = 0, matched = 0;
        eng.objects().for_each([&](ue::ObjectStat const& st) {
            ++scanned;
            if (!f.empty()) {
                if (!icontains(st.name, f) && !icontains(st.className, f)) return true;
            }
            ++matched;
            if (gObjCache.size() < 500) {
                gObjCache.push_back({st.className, st.name, st.address});
            }
            return true;
        });
        gObjTotal = matched;
        gObjNote = fmt("scanned {} slots, {} matched (listing at most 500)",
                         scanned, matched);
    }

    ImGui::TextDisabled("%s", gObjNote.c_str());

    if (ImGui::BeginTable("objs", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY, ImVec2(0, 260))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto const& r : gObjCache) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(hex(r.addr, 16).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.cls.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.name.c_str());
        }
        ImGui::EndTable();
    }
}

void drawUi() {
    // 游戏在跑的时候别铺满屏, 给个固定大小、可拖动的窗口
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Epsilon For Dungeons II - Data Overlay", &g.visible)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Overview")) {
                if (ImGui::BeginChild("left", ImVec2(0, 170))) panelEngine();
                ImGui::EndChild();
                ImGui::Separator();
                if (ImGui::BeginChild("right", ImVec2(0, 0))) panelHooks();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Objects")) {
                panelObjects();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About")) {
                ImGui::TextWrapped(
                    "Press Insert to toggle this overlay.\n\n"
                    "Rendered by the injected payload on the game's own D3D12 command "
                    "queue, so it is ordered with the game's frame submission and does "
                    "not disturb the game's rendering. A fence wait after each submit "
                    "guarantees we never touch the back buffer while the GPU is using "
                    "it.\n\n"
                    "All data reads go through safeRead, so a stale pointer in the "
                    "engine's object graph cannot crash the target.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace

// ===========================================================================
//  对外接口
// ===========================================================================
bool initialized() noexcept { return g.initialized; }
bool visible() noexcept { return g.visible; }
void setVisible(bool v) noexcept { g.visible = v; }
std::string const& lastError() noexcept { return g.error; }

bool init(IDXGISwapChain* swapchain) {
    std::lock_guard lk(gMu);
    if (g.initialized) return true;
    if (!swapchain) { g.error = "swapchain 为空"; return false; }

    // 重入保护: 万一在初始化途中又有 Present 进来
    static std::atomic<bool> inProgress{false};
    if (inProgress.exchange(true)) return false;

    trace("  [overlay] 首次 Present, 开始初始化 ImGui...");

    bool ok = createResources(swapchain);
    if (ok) ok = createImGui(swapchain);

    if (ok) {
        g.initialized = true;
        trace(fmt("  [overlay] 就绪 (hwnd={}, 后缓冲 {} 个, 格式 {}, {}x{})",
                  hex(reinterpret_cast<uint64_t>(g.hwnd), 16),
                  g.backbufferCount, static_cast<int>(g.rtvFormat),
                  g.width, g.height));
        trace("  [overlay] 按 Insert 键开关覆盖层");
    } else {
        trace(fmt("  [overlay] 初始化失败: {}", g.error));
    }

    inProgress.store(false);
    return ok;
}

void render(IDXGISwapChain* swapchain) {
    if (!g.initialized || !swapchain) return;

    // 帧率统计
    {
        const double t = nowSeconds();
        static double last = 0.0;
        if (last > 0.0 && t > last) {
            const float inst = static_cast<float>(1.0 / (t - last));
            gUiFps = gUiFps * 0.9f + inst * 0.1f;
        }
        last = t;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g.visible) drawUi();

    ImGui::Render();

    ImDrawData* dd = ImGui::GetDrawData();

    // 诊断: 每 120 帧打一次关键量。渲染出问题时, 这几个数字能直接指出断层
    // 在哪一环 —— 是 UI 没生成几何体, 还是几何体生成但没画上去。
    static uint64_t dbg = 0;
    ++dbg;
    // 用奇数间隔采样 —— 交换链是两个后缓冲, 偶数间隔会永远落在同一个奇偶性上,
    // 看不出 bb 到底有没有在 0/1 之间交替。
    const bool dbgNow = (dbg % 97 == 1);
    if (dbgNow) {
        const ImGuiIO& io = ImGui::GetIO();
        // TexID 就是 GPU 描述符句柄, 后端在绘制时直接拿它去
        // SetGraphicsRootDescriptorTable(1, ...)。如果它是 0, 说明字体纹理
        // 的 SRV 没分配成功 —— 那么一切都会"成功"但什么都画不出来。
        UINT64 texid = 0;
        if (dd && dd->Textures && dd->Textures->Size > 0) {
            texid = static_cast<UINT64>((*dd->Textures)[0]->TexID);
        }
        trace(fmt("  [overlay] #{} visible={} display={:.0f}x{:.0f} scale={:.2f} "
                  "cmdlists={} vtx={} srv分配={} srv堆base={:#x} texID={:#x}",
                  dbg, g.visible, io.DisplaySize.x, io.DisplaySize.y,
                  io.DisplayFramebufferScale.x,
                  dd ? dd->CmdListsCount : -1, dd ? dd->TotalVtxCount : -1,
                  g.srv.allocCount, g.srv.gpuBase.ptr, texid));
    }
    if (!dd || dd->CmdListsCount == 0) return;   // 没有内容, 一帧 GPU 活都不干

    // ---- 取后缓冲 ----
    const UINT idx = g.frameIndex % kFramesInFlight;
    IDXGISwapChain3* sc3 = nullptr;
    UINT bbIndex = idx;
    if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain3),
                                            reinterpret_cast<void**>(&sc3))) && sc3) {
        bbIndex = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
    }
    if (bbIndex >= g.backbufferCount) bbIndex = bbIndex % g.backbufferCount;

    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(bbIndex, IID_PPV_ARGS(&backbuffer))) || !backbuffer) {
        return;
    }

    // ---- 记录命令 ----
    if (FAILED(g.alloc[idx]->Reset())) { backbuffer->Release(); return; }
    if (FAILED(g.list->Reset(g.alloc[idx], nullptr))) { backbuffer->Release(); return; }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bbIndex) * g.rtvInc;
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // ⚠️ 必须显式设视口。D3D12 的视口默认是空的(全 0), 不是"整个渲染目标";
    //    ImGui 的 DX12 后端也不会替你设 —— 官方示例里是宿主自己调
    //    RSSetViewports 的。漏了这一步的后果是: 一切看起来都成功(初始化、
    //    提交、Present 都没报错), 但屏幕上什么都没有, 因为几何体全在
    //    零尺寸视口之外被裁掉了。
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(g.width);
    vp.Height = static_cast<float>(g.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g.list->RSSetViewports(1, &vp);

    D3D12_RECT scissor{0, 0, static_cast<LONG>(g.width), static_cast<LONG>(g.height)};
    g.list->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {g.srv.heap};
    g.list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(dd, g.list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->ResourceBarrier(1, &barrier);

    if (SUCCEEDED(g.list->Close())) {
        ID3D12CommandList* lists[] = {g.list};
        g.queue->ExecuteCommandLists(1, lists);
        const bool done = waitGpu();
        if (dbgNow) trace(fmt("  [overlay] 提交 bb={} 围栏完成={}", bbIndex, done));
    } else if (dbgNow) {
        trace("  [overlay] 命令列表 Close 失败");
    }

    backbuffer->Release();
    g.frameIndex = (g.frameIndex + 1) % kFramesInFlight;
}

void shutdown() {
    std::lock_guard lk(gMu);
    if (!g.initialized) return;

    if (g.hwnd && g.origWndproc) {
        ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC,
                            reinterpret_cast<LONG_PTR>(g.origWndproc));
        g.origWndproc = nullptr;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g.list)        g.list->Release();
    for (auto*& a : g.alloc) { if (a) a->Release(); a = nullptr; }
    if (g.fence)       g.fence->Release();
    if (g.fenceEvent) ::CloseHandle(g.fenceEvent);
    if (g.rtvHeap)    g.rtvHeap->Release();
    if (g.srv.heap)    g.srv.heap->Release();
    // ⚠️ 不释放 g.queue —— 那是游戏的队列, 我们只是持有指针, 没有引用计数份额。
    //    在这里 Release 会减掉不属于我们的一次引用。
    if (g.device)      g.device->Release();

    // 注意: 不能用 memset —— State 里有 std::string, 那样会破坏它的内部指针。
    const bool wasVisible = g.visible;
    g = State{};
    g.visible = wasVisible;
    gObjCache.clear();
    gObjTotal = 0;
    gObjNote.clear();
}

} // namespace epsilon::payload::overlay
