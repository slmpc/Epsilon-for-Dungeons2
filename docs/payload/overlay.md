# 覆盖层（D3D12 + ImGui）

本文讲注入体在游戏进程里画 ImGui 面板的全部要点：每帧 GPU 顺序、**交换链尺寸/后缓冲
数量检测与重建（实机两次 GPU crash 的根因）**、为什么必须用游戏的命令队列渲染、
资源创建时那几个"漏了就一切成功但屏幕上什么都没有"的步骤，以及窗口过程的输入分流。

主要来源：[`src/payload/Overlay.h`](../../src/payload/Overlay.h)、
[`src/payload/Overlay.cpp`](../../src/payload/Overlay.cpp)。

---

## 1. 生命周期：全部由 Present 钩子驱动，不需要额外线程

| 时机 | 做什么 |
|---|---|
| 第一次 `Present` | `overlay::init(swapchain)`：用**游戏自己的** D3D12 设备建 ImGui 所需的独立资源（命令分配器/描述符堆/命令列表/围栏），并挂钩窗口过程接收输入 |
| 之后每次 `Present` | `render(swapchain)`：尺寸检测 → `NewFrame` → 画面板 → `Render` → 记录命令 → 提交 → 等围栏 |

**为什么第一次 `Present` 才初始化**：`Present` 是唯一能拿到真实交换链的时机。
在那之前我们只有一个 vtable 地址 —— 没有 device、没有 hwnd、也不知道后缓冲格式。

`presentDetour` 里的调用形态（`Hooks.cpp`）：

```cpp
if (!overlay::initialized()) {
    if (!overlayAttempted.exchange(true)) overlay::init(sc);   // 只尝试一次
}
if (overlay::initialized()) overlay::render(sc);
```

`init()` 内部还有一层重入保护：`static std::atomic<bool> inProgress`，万一初始化途中又有
`Present` 进来，直接返回 false。

---

## 2. 每帧的 GPU 侧顺序（必须严格保持）

[`Overlay.cpp` 文件头](../../src/payload/Overlay.cpp#L4-L14)的原始描述：

```
1. 取当前后缓冲索引 → 取后缓冲资源
2. Reset 该索引的命令分配器 → Reset 命令列表
3. 屏障 PRESENT → RENDER_TARGET
4. 绑定我们自己为该后缓冲建的 RTV
5. 绑定 SRV 堆 → ImGui_ImplDX12_RenderDrawData
6. 屏障 RENDER_TARGET → PRESENT
7. Close → ExecuteCommandLists(我们的队列) → 等围栏
```

> 第 7 步的等待很关键：必须等我们的队列真的画完，才能把控制权交回给游戏的 `Present`。
> 否则游戏可能在我们还在写后缓冲时就开始下一帧。

**⚠️ 注释与实现的一处出入**：第 7 步注释写的是"我们的队列"。**实际代码用的是游戏队列**
（`g.queue = presentQueue()`，由 `ExecuteCommandLists` 钩子捕获），围栏才是我们自己的。
`Overlay.h` 文件头"为什么自己建命令队列而不是用游戏的：我们拿不到游戏内部那份队列指针"
也是**历史理由**——后来通过挂 `ExecuteCommandLists` 拿到了，方案随之改变。
**以代码为准：队列必须用游戏的，见 §5。**

### 2.1 渲染前的空帧短路

```cpp
if (!dd || dd->CmdListsCount == 0) return;   // 没有内容, 一帧 GPU 活都不干
```

面板收起或没有可见内容时**完全跳过** GPU 工作（连分配器都不 Reset）。

### 2.2 命令记录的细节

| 步骤 | 代码要点 |
|---|---|
| 帧索引 | `const UINT idx = g.frameIndex % kFramesInFlight;`（`kFramesInFlight = 3`，每帧一个分配器） |
| 后缓冲索引 | 优先 `IDXGISwapChain3::GetCurrentBackBufferIndex()`；取不到时退回 `idx`；再 `>= backbufferCount` 时取模兜底 |
| 取后缓冲 | `swapchain->GetBuffer(bbIndex, IID_PPV_ARGS(&backbuffer))`，失败直接返回 |
| 屏障 | `PRESENT → RENDER_TARGET`（`ALL_SUBRESOURCES`），画完再 `RENDER_TARGET → PRESENT` |
| RTV | `rtvHeap->GetCPUDescriptorHandleForHeapStart()` + `bbIndex * g.rtvInc` |
| 描述符堆 | `SetDescriptorHeaps(1, {g.srv.heap})` |
| 绘制 | `ImGui_ImplDX12_RenderDrawData(dd, g.list)` |
| 提交 | `Close()` → `g.queue->ExecuteCommandLists(1, lists)` → `waitGpu()`（围栏，2 秒超时） |
| 收尾 | `backbuffer->Release()`；`g.frameIndex = (g.frameIndex + 1) % kFramesInFlight` |

**必须 `Release()` 后缓冲**（`createBackBufferViews` 里也强调）：后缓冲由交换链持有，
我们只是借用 —— 不立刻 `Release`，交换链 `ResizeBuffers` 会因为引用计数不为 0 而失败。

---

## 3. 资源结构：哪些属于"一次性"，哪些属于"与尺寸绑定"

```
State
├── device         (游戏的 ID3D12Device, 通过 swapchain->GetDevice 借来的)
├── queue          (游戏队列, 只持有指针, 无引用计数份额!)
├── list / alloc[3] / fence / fenceEvent       ← 与尺寸无关
├── rtvHeap / rtvInc / backbufferCount / rtvFormat / width / height  ← 与尺寸绑定
├── srv (SrvAllocator: heap/cpuBase/gpuBase/used[64])                ← 与尺寸无关
├── hwnd / origWndproc
└── frameIndex / initialized / visible / error
```

| 类别 | 资源 | 交换链重建时 |
|---|---|---|
| 与尺寸绑定 | `rtvHeap`、各后缓冲的 RTV、`backbufferCount`、`width`/`height` | **必须重建** |
| 与尺寸无关 | `device`、`queue`、`alloc[3]`、`list`、`fence`、`fenceEvent`、`srv.heap` | **绝对不能动** |

> 设备/队列/分配器/围栏/ImGui 的 SRV 堆与尺寸无关，重建时**不要**动它们 ——
> 它们在 `ResizeBuffers` 前后依然有效，重建它们纯属浪费且容易出错。

### 3.1 后缓冲格式与尺寸的获取

```cpp
DXGI_SWAP_CHAIN_DESC desc{};
if (SUCCEEDED(sc->GetDesc(&desc))) {
    g.rtvFormat       = desc.BufferDesc.Format;
    g.backbufferCount = desc.BufferCount ? desc.BufferCount : 2;
    g.width           = desc.BufferDesc.Width;
    g.height          = desc.BufferDesc.Height;
} else {
    g.backbufferCount = 2;
}
if (g.width == 0 || g.height == 0) {
    // 有些交换链的 BufferDesc 尺寸是 0(由窗口决定), 那就问窗口
    RECT rc{};  ::GetClientRect(g.hwnd, &rc); ...
}
if (g.width == 0)  g.width = 1920;      // 最后兜底
if (g.height == 0) g.height = 1080;
```

`g.width` / `g.height` 不只是信息，**它还要用来设视口与裁剪矩形**（见 §7）。

---

## 4. 交换链尺寸与后缓冲数量检测

### 4.1 ⚠️ 这是实机两次 GPU crash 的根因

[`Overlay.cpp`](../../src/payload/Overlay.cpp#L615-L643) 的原始记录：

> ⚠️ 这是 D3D12 覆盖层最经典的一处致命疏漏，本项目的真机崩溃就是它：
> 窗口改尺寸、切全屏、改分辨率都会让 DXGI **重建后缓冲**。此时旧的
> `ID3D12Resource*` 全部作废，而我们缓存着按旧数量建的 RTV 堆 —— 继续拿旧 RTV 去
> `OMSetRenderTargets` 就等于把"已经不存在的后缓冲"绑成渲染目标。
> **D3D12 不会在 CPU 侧报错**（命令照常记录、提交也成功），但 **GPU 侧会挂死**，
> 表现为 UE 弹出 `GPUCrash / GPU Crash dump Triggered` 并留下 `.nv-gpudmp` ——
> **日志里一切正常，完全看不出问题在哪。**

| | |
|---|---|
| 现象 | UE 弹出 `GPUCrash / GPU Crash dump Triggered`，留下 `D3D12.*.nv-gpudmp`；注入体日志一切正常 |
| 原因 | 交换链尺寸/后缓冲数量变化后仍使用旧的 RTV 堆与旧的 `ID3D12Resource*` |
| 实测 | **真机崩过两次**（14:43:46 / 14:45:04，异常码 `0x8000`，见 [dev/diagnostics.md](../dev/diagnostics.md#已定性的崩溃)） |
| 结论 | 每帧在**取后缓冲之前**比对尺寸与后缓冲数量，变化就重建与尺寸绑定的资源 |

### 4.2 为什么必须在**取后缓冲之前**检测

> 一旦尺寸变了，后面的 RTV / 视口 / 裁剪全是错的。

检测发生在 `render()` 的最前面，在 `ImGui::NewFrame()` 之前、更在 `GetBuffer()` 之前。
这一帧检测到变化就重建，然后用**新的**尺寸/数量继续；重建失败则**本帧不渲染**。

### 4.3 `needsRebuild` 的判据与兜底

```cpp
outCount = desc.BufferCount ? desc.BufferCount : 2;
outW = desc.BufferDesc.Width; outH = desc.BufferDesc.Height;
if (outW == 0 || outH == 0) {
    // 有些情况 GetDesc 不给尺寸(比如刚 ResizeBuffers 过), 退回窗口客户区。
    RECT rc{}; if (::GetClientRect(g.hwnd, &rc)) { outW = ...; outH = ...; }
}
if (outW == 0 || outH == 0) return false;   // 拿不到就别动, 免得越修越坏

if (outCount == g.backbufferCount && outW == g.width && outH == g.height) return false;
return true;
```

> **拿不到就别动，免得越修越坏** —— 宁可这一帧用旧尺寸（可能画歪），也不要盲目重建。

### 4.4 重建做什么、不做什么

```cpp
bool rebuildForSwapchain(IDXGISwapChain* sc, UINT newCount, UINT newW, UINT newH) {
    // 1) 等自己的队列跑完。不等的话, GPU 可能还在读我们即将重建的那些资源。
    waitGpu();

    // 2) 丢掉旧的 RTV 堆。注意**不碰** ImGui 的 SRV 堆: 字体纹理与 ImGui
    //    后端状态跟交换链尺寸无关, 重建它们只会引入新的失败点。
    releaseSizeDependent();

    g.backbufferCount = newCount;  g.width = newW;  g.height = newH;

    // 重建 RTV 堆 → 重算 rtvInc → 重建每个后缓冲的 RTV
    return createBackBufferViews(sc);
}
```

`releaseSizeDependent()` 目前**只**释放 `rtvHeap` —— 这是"最小重建面"的体现。

**重建失败的处理**（`render()` 内）：

```cpp
if (!rebuildForSwapchain(...)) {
    // 重建失败: 标记为未初始化, 让上层下一帧重新走 init。
    // 比继续用半套资源去渲染安全得多。
    g.initialized = false;
    trace(fmt("  [overlay] 重建失败, 暂停渲染: {}", g.error));
    return;
}
```

日志里能看到的两次 trace：

```
[overlay] 交换链变化: 2560x1600 x3 -> 1920x1080 x3 , 重建资源
[overlay] 重建完成 (1920x1080, 3 个后缓冲)
```

---

## 5. 命令队列：自己的资源 vs 游戏的队列

### 5.1 为什么在游戏设备上建自己的资源

所有资源都建在**游戏自己的 device** 上（`sc->GetDevice(__uuidof(ID3D12Device), ...)`），
这样后缓冲才互通。ImGui 需要队列来上传字体纹理 —— 用同一个 device 建自己的
DIRECT 队列是合法且互不干扰的：两者操作的是不同资源，只有最终的后缓冲是共享的，
而共享点由"先记录屏障再执行再等围栏"这套顺序保证安全。

### 5.2 但渲染必须用游戏队列

```cpp
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
```

**拿不到队列时直接放弃初始化**（而不是退回自建队列）—— 因为退回去只会得到"一切成功但
画面不动"这种更难查的现象。

**围栏仍是自己的**：`CreateFence` + `CreateEventW`，`waitGpu()` 里 `Signal` → 若已完成直接
返回，否则 `SetEventOnCompletion` + `WaitForSingleObject(event, 2000)`。超时返回 false，
调用方据此在诊断输出里看到 `围栏完成=false`。

### 5.3 ⚠️ 绝不能 `Release` 游戏队列

```cpp
// ⚠️ 不释放 g.queue —— 那是游戏的队列, 我们只是持有指针, 没有引用计数份额。
//    在这里 Release 会减掉不属于我们的一次引用。
```

`shutdown()` 里其余对象逐个 `Release`，唯独跳过 `g.queue`。

---

## 6. SRV 分配器（ImGui 的纹理描述符由宿主提供）

ImGui 的后端把"分配纹理描述符"这件事交给宿主，所以我们要自己管一个小堆：

| 项 | 值/行为 |
|---|---|
| 堆大小 | `kSrvHeapSize = 64`，注释说明：**ImGui 1.92 是动态纹理，会按需分配** |
| 堆属性 | `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV` + `D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE`（给 ImGui 的纹理用，**必须带 SHADER_VISIBLE**） |
| 回调 | `info.SrvDescriptorAllocFn = &srvAlloc`、`info.SrvDescriptorFreeFn = &srvFree`、`info.UserData = &g.srv` |
| 分配 | 找第一个 `used[i] == false` 的槽，返回 `cpuBase + i*inc` / `gpuBase + i*inc`，并 `++allocCount` |
| 耗尽 | `outCpu->ptr = outGpu->ptr = 0`（不崩，但绘制会失败 —— 这正是 `texID=0` 诊断的意义） |
| 释放 | 由 CPU 句柄反算下标并清 `used[i]` |
| `allocCount` | **诊断用**：后端一共要了几个描述符（见 §9） |

RTV 堆是另一个堆（`D3D12_DESCRIPTOR_HEAP_TYPE_RTV`，`FLAG_NONE`，`NumDescriptors = backbufferCount`），
按后缓冲数量建，与 SRV 堆互不相干。

---

## 7. 两个"漏了就一切成功但屏幕上什么都没有"的坑

### 7.1 必须显式检查 `ImGui_ImplDX12_CreateDeviceObjects()`

```cpp
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
```

要点：**`IM_ASSERT` 在 Release 下是空操作**，所以"后端自己会报错"这个假设不成立；
必须自己接住返回值并留 trace。

### 7.2 必须显式设视口与裁剪矩形

```cpp
// ⚠️ 必须显式设视口。D3D12 的视口默认是空的(全 0), 不是"整个渲染目标";
//    ImGui 的 DX12 后端也不会替你设 —— 官方示例里是宿主自己调
//    RSSetViewports 的。漏了这一步的后果是: 一切看起来都成功(初始化、
//    提交、Present 都没报错), 但屏幕上什么都没有, 因为几何体全在
//    零尺寸视口之外被裁掉了。
D3D12_VIEWPORT vp{};  vp.Width = g.width;  vp.Height = g.height;  vp.MaxDepth = 1.0f;
g.list->RSSetViewports(1, &vp);
D3D12_RECT scissor{0, 0, (LONG)g.width, (LONG)g.height};
g.list->RSSetScissorRects(1, &scissor);
```

**这两个值必须来自 `g.width` / `g.height`** —— 也就是 §4 里检测/重建时更新的那对数字。
这解释了"为什么交换链尺寸检测必须在取后缓冲之前"：它同时决定了视口与裁剪。

### 7.3 隐形失败三连（诊断顺序）

| 症状 | 先查什么 | 判据 |
|---|---|---|
| 一切成功但画面不动 | 队列 | `createResources` 是否报 `还没捕获到游戏的命令队列` |
| 一切成功但屏幕上什么都没有 | PSO | 是否打了 `设备对象(PSO/字体纹理)创建成功` |
| 同上 | 视口 | 代码里 `RSSetViewports` 是否被执行（尺寸是 0 就什么都看不见） |
| 同上 | 字体纹理 SRV | 诊断行里的 `texID` 是否为 `0` |

---

## 8. ImGui 初始化与配置

```cpp
IMGUI_CHECKVERSION();
ImGui::CreateContext();
ImGuiIO& io = ImGui::GetIO();
io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // 别跟游戏的鼠标模式打架
io.IniFilename = nullptr;                                  // 不落盘 imgui.ini
ImGui::StyleColorsDark();
```

| 决定 | 理由 |
|---|---|
| **不加载中文字体** | "界面统一用英文，所以直接用 ImGui 内置的默认字体即可 —— 不加载中文字体，**省掉 18 MB 的字体依赖和一份大图集**。（内置字体只有 ASCII，一旦混入中文就会显示成 `?`，所以 UI 文案里不要写中文。）" |
| `NoMouseCursorChange` | 别跟游戏的鼠标模式打架 |
| `IniFilename = nullptr` | 不在游戏目录里落盘 `imgui.ini` |
| `info.NumFramesInFlight = kFramesInFlight` | 与分配器数量一致 |
| `info.RTVFormat = g.rtvFormat`、`DSVFormat = DXGI_FORMAT_UNKNOWN` | 用交换链的真实格式 |

**hwnd 的来源**：

```cpp
IDXGISwapChain1* sc1 = nullptr;
if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
    sc1->GetHwnd(&g.hwnd);  sc1->Release();
}
if (!g.hwnd) g.hwnd = ::GetForegroundWindow();   // 退路: 理论上不该走到这
```

**为什么挑 `IDXGISwapChain1`**：与 `dxgi1_6.h` 那条注释同源 —— `IDXGISwapChain1` 在
`dxgi1_2.h` 才定义，只 include `dxgi.h` 是拿不到的。

`imgui_impl_win32.h` **没有导出** `ImGui_ImplWin32_WndProcHandler` 的声明，得自己声明
（官方示例就是这么做的）：

```cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
```

---

## 9. 窗口过程：输入分流（Insert / 改键 / ImGui / 模块）

```cpp
LRESULT WINAPI wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 1) Insert: 开关覆盖层, 并把按键吃掉
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (DWORD)wp == kToggleKey) {
        g.visible = !g.visible;
        return 0;                       // 吃掉, 别让游戏也响应这个键
    }

    const bool isKey = (KEYDOWN || SYSKEYDOWN || KEYUP || SYSKEYUP);

    // 2) 改键捕获 —— 必须在模块分发与游戏处理之前
    if (isKey && g.visible && feature::FeaturePanel::isCapturing()) {
        if (feature::FeaturePanel::captureKey(vk, pressed)) return 0;
    }

    // 3) 喂给 ImGui
    if (g.visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;

    // 4) ImGui 正在接收键盘时不喂模块
    if (isKey && g.visible && ImGui::GetIO().WantCaptureKeyboard) {
        return ::CallWindowProcW(g.origWndproc, hwnd, msg, wp, lp);
    }

    // 5) 分发给模块, 再交给原窗口过程
    if (isKey) feature::ModuleManager::instance().dispatchKeyEvent(vk, pressed);
    return ::CallWindowProcW(g.origWndproc, hwnd, msg, wp, lp);
}
```

| 顺序 | 规则 | 为什么 |
|---|---|---|
| 1 | `kToggleKey = VK_INSERT`，**始终**处理（与可见性无关） | 需要一个"面板藏起来也能叫回来"的键；返回 0 避免游戏也响应 Insert |
| 2 | 改键捕获**早于**模块分发与游戏处理，且**只在面板可见时**拦截 | 用户点了"改键"之后按下的那个键，**只应该被绑定**，不应该同时触发一次模块开关、也不应该漏进游戏。面板不可见时不拦截，否则一个隐藏的面板会把按键全部吃掉 |
| 3 | 面板可见才把消息喂给 ImGui | 隐藏面板不该消耗输入 |
| 4 | `WantCaptureKeyboard` 为真时不分发模块 | 否则在面板里**拖滑块 / 输入文本会顺手把模块开关切了** |
| 5 | 无论是否分发模块，都调用原窗口过程 | 游戏自己的输入不受影响（除非第 1/2 步已经吃掉） |

窗口过程用 `SetWindowLongPtrW(hwnd, GWLP_WNDPROC, &wndproc)` 替换，原过程存
`g.origWndproc`；失败则 `g.error = "SetWindowLongPtrW(GWLP_WNDPROC) 失败"`。
`shutdown()` 会把它换回去。

---

## 10. 每 97 帧的诊断输出

```cpp
static uint64_t dbg = 0;  ++dbg;
// 用奇数间隔采样 —— 交换链是两个后缓冲, 偶数间隔会永远落在同一个奇偶性上,
// 看不出 bb 到底有没有在 0/1 之间交替。
const bool dbgNow = (dbg % 97 == 1);
if (dbgNow) { ... trace(...) }
```

> **（注释与实现不符）** 紧邻的注释写着"诊断: 每 120 帧打一次关键量"，
> 但实际间隔是 **97**（奇数）—— 后一条注释解释了为什么必须是奇数。
> 以代码为准：**每 97 帧**。

打印的字段与它们的诊断意义：

```
[overlay] #<帧号> visible=<bool> display=<W>x<H> scale=<f>
          cmdlists=<n> vtx=<n> srv分配=<n> srv堆base=<hex> texID=<hex>
```

| 字段 | 来源 | 诊断意义 |
|---|---|---|
| `visible` | `g.visible` | 面板是不是被 Insert 关掉了 |
| `display` / `scale` | `io.DisplaySize` / `io.DisplayFramebufferScale` | ImGui 认为的画布尺寸；和我们设的视口对不上就是尺寸检测出了问题 |
| `cmdlists` / `vtx` | `dd->CmdListsCount` / `dd->TotalVtxCount` | **UI 有没有生成几何体** |
| `srv分配` | `g.srv.allocCount` | 后端一共要了几个描述符（字体纹理 + 动态纹理） |
| `srv堆base` | `g.srv.gpuBase.ptr` | SRV 堆的 GPU 基址，用来判断描述符句柄是否合理 |
| `texID` | `(*dd->Textures)[0]->TexID` | **最关键的一位**：`TexID` 就是 GPU 描述符句柄，后端绘制时直接拿它去 `SetGraphicsRootDescriptorTable(1, ...)`。**如果它是 0，说明字体纹理的 SRV 没分配成功 —— 那么一切都会"成功"但什么都画不出来** |

注释里对这条输出的定位说得很清楚：

> 渲染出问题时，这几个数字能直接指出断层在哪一环 —— **是 UI 没生成几何体，
> 还是几何体生成但没画上去**。

提交那一步的诊断（同一 `dbgNow` 门控）：

```
[overlay] 提交 bb=<索引> 围栏完成=<bool>
[overlay] 命令列表 Close 失败            (Close 失败时)
```

初始化成功时的两行 trace：

```
[overlay] 就绪 (hwnd=0x..., 后缓冲 3 个, 格式 24, 2560x1600)
[overlay] 按 Insert 键开关覆盖层
```

（"格式 24" = `DXGI_FORMAT_R8G8B8A8_UNORM` 的枚举值；"后缓冲 3 个 / 2560x1600"是那台
机器的实测形态。）

---

## 11. 面板内容与开销控制

### 11.1 顶层结构

```cpp
ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
if (ImGui::Begin("Epsilon For Dungeons II - Data Overlay", &g.visible)) {
    // TabBar: Overview / Objects / Modules / About
}
ImGui::End();
```

* `FirstUseEver`：只在第一次用默认尺寸/位置，之后用户可以自己拖（"游戏在跑的时候别铺满屏"）。
* `Begin(..., &g.visible)`：**右上角的关闭按钮也等于把覆盖层置为不可见**（与 Insert 同一状态位）。
* 画布为空时不画（`if (g.visible) drawUi();`）。

| 页签 | 内容 |
|---|---|
| `Overview` | 左：`panelEngine()`（Module base/size、GObjects 地址与 `NumElements/max`、`Layout check n/3`、GNames 地址、GEngine/GWorld 槽位）；右：`panelHooks()`（installed、Present 地址、Frames、`UI FPS`；未安装时显示 `status().error` 的换行文本） |
| `Objects` | `panelObjects()`：按名字/类名子串过滤的对象浏览器，表格列 Address / Class / Name |
| `Modules` | [FeaturePanel::draw()](../features/ui.md) —— 功能模块面板 |
| `About` | 一段英文说明：Insert 切换覆盖层；渲染在**游戏自己的 D3D12 命令队列**上，因此与游戏的帧提交有序、不干扰游戏渲染；每次提交后的围栏等待保证我们不会在 GPU 使用后缓冲时去碰它；所有数据读取走 `safeRead`，所以引擎对象图里的失效指针不会让目标崩溃 |

### 11.2 对象浏览器的节流

```cpp
// 引擎遍历比较慢, 限频到 2 Hz, 避免拖慢渲染线程
const double t = nowSeconds();
if (t - gLastExpensiveRead > 0.5) { ... 遍历 + 过滤 + 填缓存 ... }
```

* `Refresh` 按钮或过滤串变化时把 `gLastExpensiveRead = 0.0` **强制下次立即刷新**。
* 每次刷新最多往缓存里放 **500** 行（`listing at most 500`），统计行写
  `scanned <n> slots, <m> matched (listing at most 500)`。
* 过滤是**大小写不敏感**的子串匹配（`icontains`），同时匹配类名与对象名。

### 11.3 UI 帧率

```cpp
gUiFps = gUiFps * 0.9f + inst * 0.1f;    // 指数平滑
```

`panelHooks()` 里显示为 `UI FPS`，与 `Hooks` 里的帧计数是两套不同指标。

---

## 12. `shutdown()` 与那些不能碰的东西

```cpp
if (g.hwnd && g.origWndproc) { SetWindowLongPtrW(..., g.origWndproc); g.origWndproc = nullptr; }
ImGui_ImplDX12_Shutdown();  ImGui_ImplWin32_Shutdown();  ImGui::DestroyContext();
... Release 我们自己的对象 ...
// ⚠️ 不释放 g.queue —— 那是游戏的队列 ...
// 注意: 不能用 memset —— State 里有 std::string, 那样会破坏它的内部指针。
const bool wasVisible = g.visible;
g = State{};
g.visible = wasVisible;
```

| 注意 | 理由 |
|---|---|
| 先恢复 `GWLP_WNDPROC` | 否则游戏会继续调用我们即将销毁的代码 |
| 不 `Release` `g.queue` | 我们只是持有指针，没有引用计数份额 |
| **不能用 `memset`** 清 `State` | `State` 里有 `std::string error`，`memset` 会破坏它的内部指针；用 `g = State{}` 赋值 |
| 保留 `visible` | 清空状态不该改变用户的面板开关意图 |
| 清空对象浏览器缓存 | `gObjCache` / `gObjTotal` / `gObjNote` 与交换链无关但同样属于旧状态 |

> `Overlay.h` 里 `shutdown()` 的注释是"反初始化(目前只在进程退出路径上用到)"。
> 实际上本框架**不卸载注入体**（见 [hooks.md](hooks.md#6-卸载本框架不提供)），
> 所以这条路径在正常使用中不会被走到。

---

## 13. 怎么验证覆盖层这一层

| 要验证的点 | 怎么做 | 看什么 |
|---|---|---|
| 覆盖层能否起来 | 注入后执行 `hook`，再按 `Insert` | 日志 `[overlay] 首次 Present, 开始初始化 ImGui...` → `[overlay] 就绪 (...)` → `[overlay] 按 Insert 键开关覆盖层` |
| 每帧是否真的在画 | 看 `[overlay] 提交 bb=<n> 围栏完成=true` | 每 97 帧一行；`围栏完成=false` 说明 `waitGpu` 超时（2 秒） |
| UI 有没有几何体 | 诊断行 `cmdlists= / vtx=` | 为 0 说明面板没画（可能 visible=false） |
| 字体纹理是否就绪 | 诊断行 `texID=` | `0` = SRV 没分配成功 → 什么都画不出来 |
| 改窗口尺寸 / 切全屏是否安全 | 游戏内改分辨率、Alt+Enter、拖窗口 | 日志出现 `[overlay] 交换链变化: ... 重建资源` + `[overlay] 重建完成 (...)`；**不应**出现 `GPUCrash` 与 `.nv-gpudmp` |
| 改键是否会误触模块 | 面板里点改键 → 按一个已绑定模块的键 | 只改绑定，不出现 `模块 [x] 已启用/禁用` |
| 面板输入是否泄漏给模块 | 面板里拖滑块 / 在输入框里打字 | 不出现模块开关被切；按键也不进游戏 |
| Insert 是否被游戏吃掉 | 按 Insert | 面板开关，且游戏内没有对应功能被触发 |

回到索引：[../README.md](../README.md)
