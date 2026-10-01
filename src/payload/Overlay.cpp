// Overlay.cpp — D3D12 + ImGui 覆盖层实现
// ⚠️ 每帧的 GPU 侧命令顺序必须严格保持, 且交换链尺寸检测必须在**取后缓冲之前**。
// 细节见 docs/payload/overlay.md
#include "payload/Overlay.h"

#include "common/Text.h"
#include "payload/Hooks.h"
#include "payload/Payload.h"
#include "payload/Runtime.h"
#include "payload/feature/module/ModuleManager.h"
#include "payload/feature/ui/FeaturePanel.h"
#include "payload/game/ue/Engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// dxgi1_6.h 才会带出 IDXGISwapChain1/2/3 与 IDXGIFactory2 —— 只 include dxgi.h 不够。
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
constexpr int  kSrvHeapSize    = 64;      // ImGui 1.92 用动态纹理, 按需分配
constexpr DWORD kToggleKey     = VK_INSERT;

struct SrvAllocator {
    ID3D12DescriptorHeap*        heap = nullptr;
    UINT                         inc = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE  cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE  gpuBase{};
    bool                         used[kSrvHeapSize]{};
    int                          allocCount = 0;
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

float  gUiFps = 0.0f;
double gLastExpensiveRead = 0.0;

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

// 窗口过程: 可见时把输入喂给 ImGui; 切换键始终处理; 其余原样转发给游戏。
LRESULT WINAPI wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (static_cast<DWORD>(wp) == kToggleKey) {
            g.visible = !g.visible;
            return 0;                       // 吃掉, 别让游戏也响应这个键
        }
    }

    // ⚠️ 改键捕获必须早于模块分发与游戏处理: 那个键只该被绑定。
    // 面板不可见时不拦截, 否则一个隐藏的面板会把按键全部吃掉。
    const bool isKey = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
                        msg == WM_KEYUP   || msg == WM_SYSKEYUP);
    if (isKey && g.visible && feature::FeaturePanel::isCapturing()) {
        const int vk = static_cast<int>(wp);
        const bool pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        if (feature::FeaturePanel::captureKey(vk, pressed)) return 0;
    }

    if (g.visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return 1;
    }

    // ImGui 正在接收键盘时不再分发给模块 —— 否则在面板里拖滑块会顺手切了模块开关。
    if (isKey && g.visible && ImGui::GetIO().WantCaptureKeyboard) {
        return ::CallWindowProcW(g.origWndproc, hwnd, msg, wp, lp);
    }
    if (isKey) {
        const bool pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        feature::ModuleManager::instance().dispatchKeyEvent(static_cast<int32_t>(wp), pressed);
    }

    return ::CallWindowProcW(g.origWndproc, hwnd, msg, wp, lp);
}

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
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device),
                             reinterpret_cast<void**>(&g.device))) || !g.device) {
        g.error = "swapchain->GetDevice(ID3D12Device) 失败";
        return false;
    }

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
        RECT rc{};
        if (g.hwnd && ::GetClientRect(g.hwnd, &rc)) {
            g.width = static_cast<UINT>(rc.right - rc.left);
            g.height = static_cast<UINT>(rc.bottom - rc.top);
        }
    }
    if (g.width == 0)  g.width = 1920;
    if (g.height == 0) g.height = 1080;

    // ⚠️ 必须用**游戏自己的那条队列**(由 ExecuteCommandLists 钩子在 Present 之前
    //    捕获): flip 模型交换链与创建它的队列绑定, 在别的队列上渲染等于白画。
    g.queue = static_cast<ID3D12CommandQueue*>(presentQueue());
    if (!g.queue) {
        g.error = "还没捕获到游戏的命令队列 —— 无法保证渲染进入呈现结果";
        return false;
    }

    // 命令分配器(每帧一个) / 命令列表(先建了再 Close, 后面每帧 Reset) / 围栏
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g.alloc[i])))) {
            g.error = fmt("CreateCommandAllocator[{}] 失败", i);
            return false;
        }
    }

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
    return true;
}

// 只为后缓冲建 RTV; 与设备/SRV 堆等"与尺寸无关"的资源分开, 重建时只跑这一部分。
bool createBackBufferViews(IDXGISwapChain* sc) {
    if (!g.device || !g.rtvHeap) { g.error = "RTV 堆不存在"; return false; }

    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g.backbufferCount; ++i) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            g.device->CreateRenderTargetView(bb, nullptr, h);
            // ⚠️ 后缓冲由交换链持有, 这里只是借用 —— 必须立刻 Release,
            //    否则交换链 ResizeBuffers 会因引用计数不为 0 而失败。
            bb->Release();
        } else {
            g.error = fmt("GetBuffer({}) 失败", i);
            return false;
        }
        h.ptr += g.rtvInc;
    }
    return true;
}

// 释放与交换链尺寸绑定的资源; 设备/队列/分配器/围栏/SRV 堆一律保留(与尺寸无关)。
void releaseSizeDependent() {
    if (g.rtvHeap) { g.rtvHeap->Release(); g.rtvHeap = nullptr; }
}

// 交换链尺寸/后缓冲数量变化时重建。调用方需持 gMu。
bool rebuildForSwapchain(IDXGISwapChain* sc, UINT newCount, UINT newW, UINT newH) {
    if (!g.device) return false;

    // 先等自己的队列跑完, 否则 GPU 可能还在读即将被丢掉的资源。
    waitGpu();

    // ⚠️ 不碰 ImGui 的 SRV 堆: 字体纹理与交换链尺寸无关, 重建只会引入新的失败点。
    releaseSizeDependent();

    g.backbufferCount = newCount;
    g.width = newW;
    g.height = newH;

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = g.backbufferCount;
    rh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g.rtvHeap)))) {
        g.error = "RTV 堆重建失败";
        return false;
    }
    g.rtvInc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return createBackBufferViews(sc);
}

bool createImGui(IDXGISwapChain* sc) {
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1),
                                     reinterpret_cast<void**>(&sc1))) && sc1) {
        sc1->GetHwnd(&g.hwnd);
        sc1->Release();
    }
    if (!g.hwnd) {
        g.hwnd = ::GetForegroundWindow();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // 别跟游戏的鼠标模式打架
    io.IniFilename = nullptr;                                  // 不落盘 imgui.ini

    ImGui::StyleColorsDark();

    // ⚠️ 面板文案必须纯 ASCII 英文: 界面统一英文所以直接用 ImGui 内置字体,
    //    而内置字体只有 ASCII, 混入中文会被渲染成一串 '?'。
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

    // 必须显式检查: Release 构建里后端懒加载失败的 IM_ASSERT 是空操作 ——
    // PSO 没建出来时一切"看起来"都成功, 但屏幕上什么都没有。
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        g.error = "ImGui_ImplDX12_CreateDeviceObjects 失败(PSO/字体纹理没建出来)";
        trace(fmt("  [overlay] {}", g.error));
        return false;
    }
    trace("  [overlay] 设备对象(PSO/字体纹理)创建成功");

    // 挂钩窗口过程接收输入
    g.origWndproc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndproc)));
    if (!g.origWndproc) {
        g.error = "SetWindowLongPtrW(GWLP_WNDPROC) 失败";
        return false;
    }
    return true;
}

// --------------------------------------------------------------- 面板
// ⚠️ 面板文案必须纯 ASCII 英文 —— 内置字体没有中文字形, 中文会变成 '?'。
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

    auto slot = [](char const* label, game::ue::GlobalSlot const& s) {
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
        eng.objects().for_each([&](game::ue::ObjectStat const& st) {
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
            if (ImGui::BeginTabItem("Modules")) {
                feature::FeaturePanel::draw();
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

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
bool initialized() noexcept { return g.initialized; }
bool visible() noexcept { return g.visible; }
void setVisible(bool v) noexcept { g.visible = v; }
std::string const& lastError() noexcept { return g.error; }

bool init(IDXGISwapChain* swapchain) {
    std::lock_guard lk(gMu);
    if (g.initialized) return true;
    if (!swapchain) { g.error = "swapchain 为空"; return false; }

    // 重入保护: 初始化途中又进来一次 Present。
    static std::atomic<bool> inProgress{false};
    if (inProgress.exchange(true)) return false;

    trace("  [overlay] 首次 Present, 开始初始化 ImGui...");

    bool ok = createResources(swapchain);
    if (ok) ok = createBackBufferViews(swapchain);
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

// 交换链尺寸/后缓冲数量是否与已建好的资源一致。
// ⚠️ 这是必须处理的硬约束: 改窗口尺寸/切全屏会让 DXGI 重建后缓冲, 旧的
//    ID3D12Resource*/RTV 全部作废。继续绑旧 RTV 时 D3D12 在 CPU 侧不报错,
//    但 GPU 侧会挂死(UE 弹 GPUCrash) —— 根因记录见 docs/payload/overlay.md
bool needsRebuild(IDXGISwapChain* swapchain, UINT& outCount, UINT& outW, UINT& outH) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) return false;

    outCount = desc.BufferCount ? desc.BufferCount : 2;
    outW = desc.BufferDesc.Width;
    outH = desc.BufferDesc.Height;
    if (outW == 0 || outH == 0) {
        // GetDesc 有时不给尺寸(刚 ResizeBuffers 过), 退回窗口客户区。
        RECT rc{};
        if (::GetClientRect(g.hwnd, &rc)) {
            outW = static_cast<UINT>(rc.right - rc.left);
            outH = static_cast<UINT>(rc.bottom - rc.top);
        }
    }
    if (outW == 0 || outH == 0) return false;   // 判不出来就别动, 免得越修越坏

    if (outCount == g.backbufferCount && outW == g.width && outH == g.height) return false;
    return true;
}

void render(IDXGISwapChain* swapchain) {
    if (!g.initialized || !swapchain) return;

    // ⚠️ 尺寸变化检测必须在**取后缓冲之前**做(见 needsRebuild)。
    {
        UINT newCount = 0, newW = 0, newH = 0;
        if (needsRebuild(swapchain, newCount, newW, newH)) {
            trace(fmt("  [overlay] 交换链变化: {}x{} x{} -> {}x{} x{} , 重建资源",
                      g.width, g.height, g.backbufferCount, newW, newH, newCount));

            std::lock_guard lk(gMu);
            // 只重建与尺寸绑定的一小部分; 设备/队列/分配器/围栏/SRV 堆一律保留。
            if (!rebuildForSwapchain(swapchain, newCount, newW, newH)) {
                // 重建失败: 标记未初始化, 让上层下一帧重走 init —— 比用半套资源安全。
                g.initialized = false;
                trace(fmt("  [overlay] 重建失败, 暂停渲染: {}", g.error));
                return;
            }
            trace(fmt("  [overlay] 重建完成 ({}x{}, {} 个后缓冲)",
                      g.width, g.height, g.backbufferCount));
        }
    }

    // 帧率统计(指数平滑)
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

    // 诊断: 每 97 帧(奇数)打一次关键量 —— 用来判断断层在"没生成几何体"
    // 还是"生成了没画上去"。
    static uint64_t dbg = 0;
    ++dbg;
    // 用奇数间隔采样: 交换链两个后缓冲, 偶数间隔会永远落在同一奇偶性上看不出交替。
    const bool dbgNow = (dbg % 97 == 1);
    if (dbgNow) {
        const ImGuiIO& io = ImGui::GetIO();
        // TexID 是 GPU 描述符句柄; 为 0 说明字体纹理的 SRV 没分配成功。
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
    if (!dd || dd->CmdListsCount == 0) return;   // 没有内容就不干 GPU 活

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

    // ⚠️ 必须显式设视口: D3D12 的视口默认是全 0(不是"整个渲染目标"), ImGui 的
    //    DX12 后端也不替你设 —— 漏了就是"一切成功但屏幕上什么都没有"。
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
    // ⚠️ 不释放 g.queue —— 那是游戏的队列, 我们只持有指针, 没有引用计数份额。
    if (g.device)      g.device->Release();

    // 不能用 memset: State 里有 std::string, 那样会破坏它的内部指针。
    const bool wasVisible = g.visible;
    g = State{};
    g.visible = wasVisible;
    gObjCache.clear();
    gObjTotal = 0;
    gObjNote.clear();
}

} // namespace epsilon::payload::overlay
