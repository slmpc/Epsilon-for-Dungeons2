# 渲染挂钩（Present / ExecuteCommandLists）

本文讲注入体怎么在不依赖任何硬编码偏移的前提下找到游戏的 `IDXGISwapChain::Present`、
怎么捕获游戏自己的 D3D12 命令队列、自动安装的时机取舍，以及**一条必须原样保留的历史
记录：关于 Present 钩子崩溃的旧归因已被转储分析推翻**。

主要来源：[`src/payload/Hooks.h`](../../src/payload/Hooks.h)、
[`src/payload/Hooks.cpp`](../../src/payload/Hooks.cpp)、
[`src/payload/Runtime.cpp`](../../src/payload/Runtime.cpp)。

---

## 1. 定位 `Present`：临时 D3D12 交换链的 `vtable[8]`

**定位方式只有一条（刻意不做多路 fallback）**：

```
CreateDXGIFactory1 → QueryInterface(IDXGIFactory2)
  → 建一个临时窗口 (64x64, DefWindowProc)
  → D3D12CreateDevice → CreateCommandQueue(DIRECT) → CreateSwapChainForHwnd
  → 从临时交换链的 vtable 取 [8] = IDXGISwapChain::Present
  → 临时对象全部 Release, 只留下地址
```

### 1.1 为什么是 D3D12 而不是 D3D11

[Hooks.cpp 文件头](../../src/payload/Hooks.cpp#L11-L14)给出的理由：

* 目标游戏就是 D3D12（`Dungeons-Win64-Shipping` 用 UE5 + D3D12）。
* `IDXGISwapChain` 是**同一个 COM 接口、实现在同一个 `dxgi.dll` 里**，所以从哪个 API 拿到
  交换链，vtable 都是同一份。
* 既然目标是 D3D12，就直接用 D3D12 建，**少引入一个 API 面**。

D3D12 建交换链必须先有命令队列，所以完整链路是
`D3D12CreateDevice → CreateCommandQueue → CreateSwapChainForHwnd`。

**为什么临时交换链的 `Present` 就是游戏在用的那个**：同一进程里 dxgi 的
`IDXGISwapChain` 实现共享 vtable。

### 1.2 `vtable[8]` 的索引推导（别凭记忆改）

| 父接口 | 虚函数 | 个数 |
|---|---|---|
| `IUnknown` | `QueryInterface` / `AddRef` / `Release` | 3 |
| `IDXGIObject` | `SetPrivateData` / `SetPrivateDataInterface` / `GetPrivateData` / `GetParent` | 4 |
| `IDXGIDeviceSubObject` | `GetDevice` | 1 |
| **`IDXGISwapChain::Present`** | 接口自身第一个方法 | **索引 = 3+4+1 = 8** |

> 用官方头拿真实结构布局。**`IDXGISwapChain` 的 vtable 索引永远不要凭记忆写** ——
> 错一次就是把 `jmp` 写到别的函数头上，目标进程直接崩。

**验证方式（已做过，逐字节）**：`tests/d3d_probe` 在普通进程里验证
`sizeof(DXGI_SWAP_CHAIN_DESC) = 72`、`vtbl[8] = Present`、该地址所在段属性为
`EXECUTE_READ`。

**头文件坑**：`IDXGIFactory2` / `IDXGISwapChain1` / `DXGI_SWAP_CHAIN_DESC1` 都在
`dxgi1_2.h` 之后才定义，**光 `#include <dxgi.h>` 是不够的** —— 项目里统一用
`#include <dxgi1_6.h>` 把 1_1…1_6 全部拉进来。

### 1.3 为什么 `LoadLibrary` 之后**故意不** `FreeLibrary`

```cpp
// ⚠️ 故意不 FreeLibrary, 理由见文件头。
HMODULE dxgi  = ::LoadLibraryW(L"dxgi.dll");
HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
```

返回的 `Present` 地址就在 **dxgi 的代码段**里。放掉引用计数会让模块卸载，
**这个地址立刻变成野指针**，钩子指向已卸载的内存 —— 目标必崩。

### 1.4 为什么必须校验地址落在可执行模块内

```cpp
bool inExecutableModule(void const* p) {
    // VirtualQuery → Type == MEM_IMAGE && State == MEM_COMMIT
    //             && 不含 PAGE_GUARD/PAGE_NOACCESS
    //             && Protect 含 PAGE_EXECUTE*
}
```

自检用来挡住两种情况：**vtable 索引数错**、取到**未映射地址**。

> 有这道检查，索引写错时我们会"**定位失败**"而不是"**把游戏钩崩**"。

取不到合法地址时 `status_.error` 会写成
`vtable[8]=0x... 不在可执行模块内, 判定无效` —— 遇到这条信息，先怀疑索引而不是怀疑游戏版本。

---

## 2. 捕获游戏自己的 `DIRECT` 命令队列

### 2.1 钩什么

`findPresent()` 顺便把 `ID3D12CommandQueue::ExecuteCommandLists` 的地址带出来
（同样要过 `inExecutableModule`）。索引推导：

```
IUnknown(3) + ID3D12Object(4: GetPrivateData/SetPrivateData/
SetPrivateDataInterface/SetName) + ID3D12DeviceChild(1: GetDevice)
+ ID3D12Pageable(0) + UpdateTileMappings, CopyTileMappings = 10
```

### 2.2 detour 只做一件事

```cpp
void __stdcall executeDetour(void* self, UINT num, ID3D12CommandList* const* lists) {
    // 直接调真实方法问队列类型 —— 比手数 vtable 索引可靠。
    const D3D12_COMMAND_QUEUE_DESC qd =
        static_cast<ID3D12CommandQueue*>(self)->GetDesc();
    if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        gGameQueue.store(self, std::memory_order_release);
    }
    gOriginalExec(self, num, lists);
}
```

* 判据是"**Present 之前最后一次提交**" —— 呈现必然紧跟在这一帧的命令之后。
* 用 `GetDesc()` 问真实方法，比再数一遍 vtable 索引可靠。

### 2.3 为什么非要游戏那条队列（flip 模型与队列绑定）

D3D12 的 **flip 模型交换链与创建它的那个队列绑定**。如果覆盖层在另一条队列上渲染：

> 命令确实执行了、围栏也确实完成了，但**画面不会进入呈现结果** ——
> 现象就是"一切都成功，屏幕上什么都没有"。

所以我们自己的临时队列**只能用来取 vtable，不能用来画**。
`presentQueue()` 就是把这个捕获到的队列暴露给覆盖层（见 [overlay.md](overlay.md)）。

---

## 3. `presentDetour` 的顺序与保护

```cpp
HRESULT __stdcall presentDetour(void* self, UINT syncInterval, UINT flags) {
    // 1) 覆盖层(首次 init + 每帧 render)  —— SEH 包住
    // 2) 帧计数与用户回调              —— SEH 包住
    // 3) 原 Present
}
```

* **覆盖层必须在原 `Present` 之前**渲染完并等围栏 —— 否则我们可能在后缓冲**已被呈现出去
  之后**才去写它。
* 覆盖层的 SEH 是**安全机制，不是 fallback 方案**：覆盖层里的 D3D12 调用一旦出错，只把
  覆盖层自己关掉（`overlayBroken` 置位，日志一行
  `[overlay] 渲染时抛异常, 已停用覆盖层(游戏不受影响)`），**绝不能连累游戏的渲染线程**。
* 用户回调也在 SEH 里；回调里出任何异常都直接丢弃这一帧的回调。
* 回调运行在渲染线程上，**必须极轻**：约定只做打标记/计数，不做遍历。
* 帧统计（`gFrames` / `gFirstTick` / `gLastTick`）是 `std::atomic`，供面板/命令读取。

### 3.1 探针为什么包在 SEH 函数里

`probeD3d12Impl` 用独立的 `__try` 包住整条设备/队列/交换链创建链，理由写得很直白：

1. **之前的现象是"日志停在调 D3D 那一行，目标进程直接消失"，完全查不到死因。**
   SEH 能把异常码抓出来，崩溃就变成一条可读信息。
2. **SEH 函数体内不能有需要析构的对象（MSVC C2712）**，所以参数一律裸类型
   （`struct D3d12Probe` 只放指针/`HRESULT`/`DWORD`）。

日志里因此能看到：

```
[hook] D3D12 探测 hr=0x0 device=0x... queue=0x... swapchain=0x...
```

失败时 `probe.hr` / `probe.excCode` 都会落进 `status().error`。

---

## 4. `install()`：加锁 + 只试一次

```cpp
bool Hooks::install() {
    std::lock_guard lk(installMu_);      // 命令线程 + 功能线程都可能调
    if (status_.installed) return true;  // 已装直接短路
    status_.attempted = true;
    ...
}
```

| 关键点 | 说明 |
|---|---|
| 加锁 | `install()` 可能来自命令线程（`hook` 命令）与功能线程（自动安装）。**两个线程同时走 `MH_CreateHook` 会把 MinHook 的内部状态搅坏** |
| `installed` 短路 | 重复调用安全；`status_.attempted` 记录"尝试过" |
| `MH_Initialize` | 接受 `MH_ERROR_ALREADY_INITIALIZED`（其它调用方可能已经初始化过） |
| 先挂钩 `ExecuteCommandLists` | **覆盖层初始化时要用它捕获到的队列**，所以顺序不能反 |
| EC 挂钩失败 | **不致命**：只打一行 trace（`覆盖层可能无法渲染`），采集功能不受影响 |
| Present 挂钩失败 | `MH_CreateHook` 失败 → 写 `status_.error` 返回 false；`MH_EnableHook` 失败 → `MH_RemoveHook` 回滚 |
| 成功后 | `trampoline_ = gOriginal`、`status_.installed = true`、`target = "IDXGISwapChain::Present @ 0x..."` |

`Hooks::status()` 的字段：`attempted` / `installed` / `target` / `how`（定位方式说明）/
`error` / `frameCount` / `fps`。

> **（代码事实）** `Hooks::onFrame()` 目前**没有调用方** —— detour 调用的是
> `setFrameCallback()` 设进去的 `FrameCallback`（实际是 `Runtime.cpp` 的 `payload::onFrame`）。
> 因此 `HookStatus::frameCount` / `fps` 不会被更新；`hook` 命令与面板显示的帧数来自全局
> `frameCount()`（`gFrames`）。`Hooks.h` 里"由 detour 内部调用(不要外部直接调)"这句注释与
> 现状不符。

> **（未证实的低风险点）** `setFrameCallback()` 用 `gCallback.exchange(heap)` 换指针后
> **立即 `delete old`**。若渲染线程恰好在 `exchange` 之前取出旧指针、之后才调用，理论上存在
> use-after-free 窗口。实际使用中只在安装时设置一次（此时还没有帧在跑），**未观察到任何崩溃**；
> 记录在此以免将来有人频繁改回调。

---

## 5. 自动安装与 10 秒延迟

`Runtime.cpp` 的 `maybeAutoInstallHook()` **每轮功能线程都调用**，内部自己决定要不要动手：

```cpp
if (gAutoHookDone) return;                       // 只试一次
if (hooks().installed()) { gAutoHookDone = true; return; }

const uint64_t now = ::GetTickCount64();
if (gAutoHookNotBefore == 0) {                   // 第一次进入
    gAutoHookNotBefore = now + autoHookDelayMs;  // 10000
    if (!autoHookAllowed()) { gAutoHookDone = true; trace("...已由 EPSILON_NO_AUTO_HOOK 关闭"); return; }
    trace(fmt("Present 钩子自动安装已启用(默认); 将在 {} 秒后尝试, 如需关闭请设 EPSILON_NO_AUTO_HOOK=1", ...));
    return;
}
if (now < gAutoHookNotBefore) return;

gAutoHookDone = true;                            // 只试一次
trace("Present 钩子自动安装: 开始尝试(命令路径 hook 亦可手工重试)");
if (installFrameHook()) { trace("Present 钩子自动安装成功"); return; }
trace(fmt("Present 钩子自动安装失败: {}", hooks().status().error));
trace("  覆盖层不可用; 命令采集与功能模块不受影响。可手工执行 `hook` 重试。");
```

### 5.1 为什么延迟 10 秒

**⚠️ 这个延迟不是随手加的。** 实测对比：

| 触发方式 | 结果 |
|---|---|
| 由 `hook` 命令触发（注入后数秒到数分钟） | **稳定**，多次成功，曾连续渲染 **33000+ 帧** |
| 由功能线程在初始化后**立刻**触发 | **两次把目标进程带走**（其中一次为 `EXCEPTION_ACCESS_VIOLATION writing`） |

两者**唯一的已知差别就是时机**。因此自动安装推迟到注入后 10 秒再试，让目标进程的
D3D12 / 加载状态先稳定下来。

> **这只是基于现有证据的缓解，不是已证实的根因。** 若仍然崩，用
> `EPSILON_NO_AUTO_HOOK=1` 关掉，改用 `hook` 命令（那条路已验证）。

### 5.2 为什么只试一次，不重试

> 如果这条路径会崩，反复踩只是多冒几次把目标带走的险；如果它失败但不崩，
> 命令 `hook` 随时可以手工再试。

`Hooks::attempted()` 就是为这个语义准备的。

### 5.3 为什么放在功能线程而不是启动路径

万一 `findPresent` 把目标带走，**至少管道已连、日志已落盘，有迹可循**。
也正因如此，功能线程的心跳是判断"是否卡在 `findPresent`"的关键证据
（见 [lifecycle.md](lifecycle.md#73-心跳)）。

### 5.4 关掉自动安装的两条途径

`autoHookAllowed()` **默认返回 true**（即默认自动安装），两条关闭途径：

| 途径 | 作用域 | 何时生效 |
|---|---|---|
| 环境变量 `EPSILON_NO_AUTO_HOOK=1` | 游戏进程的环境 | 启动前设置；`atoi(buf) != 0` 才算真 |
| 标记文件 `%TEMP%\epsilonPayload_no_autohook` | 整机（当前用户） | **随时建/删，不用重启游戏** |

**为什么环境变量要"留在游戏之外"**：如果这个钩子在某台机器/某个版本上导致崩溃，用户需要
一条**在游戏之外**就能生效的关闭途径 —— 不必先找到配置目录，也不必让游戏成功启动过一次。

**为什么还要一个标记文件**：环境变量属于游戏进程，想在"游戏已经开着"的时候改变行为是做不到
的。而调试时经常需要在同一个进程里再注入一个**不同名字**的注入体（见
[lifecycle.md](lifecycle.md#53-为什么带槽位不算放宽守卫)），那种情况下第二个注入体
**不应该**再去挂钩子 —— 同一个 `Present` 被挂两次没有意义，还有风险。
**建/删这个文件比重启游戏快得多。**

---

## 6. 卸载：本框架不提供

* 注入体一旦挂钩，半途 `FreeLibrary` 会留下**悬空回调**。要清掉就**重启游戏进程**。
* 因此 `ProcUtil.h` 不提供 eject；`shutdownRuntime()` 也不摘钩子（见
  [lifecycle.md](lifecycle.md#10-shutdownruntime不卸载钩子也不-freelibrary)）。
* 这也是"注入体驻留期间无法重新注入同名 DLL"这条约束的来源 —— 想要新构建，
  要么重启游戏，要么**换个 DLL 文件名**（不同槽位可以在同进程并存）。

---

## 7. 历史记录：关于 Present 钩子崩溃的归因 —— 已被转储分析推翻（旧结论）

> **本节是必须保留的历史记录。以下"自动安装必崩"是已被推翻的旧结论，不要再当成既定事实使用。**

**旧结论（错误）**：早先曾把两次崩溃归因于"自动安装 Present 钩子"，
**依据只是日志顺序**（崩溃前最后一行是自动安装提示）。

**推翻它的证据（转储分析）**：

* `14:43:46` / `14:45:04` 两次是 **GPU crash**，根因是**覆盖层的交换链尺寸问题**
  （见 [overlay.md](overlay.md#4-交换链尺寸与后缓冲数量检测)),
  与钩子的安装方式无关。
* `16:06:48` 那次，游戏在**预定的钩子尝试之前 4 秒**就死了 ——
  日志里"`开始尝试`"那一行**根本没出现**，所以不可能是钩子干的。
* `15:09:59` 那次**没有转储可查**（`UEMinidump.dmp` 是 0 字节）。

**因此**："自动安装必崩、命令安装稳定"这个规律**证据不足**，不要把它当成既定事实。

**真正已证实的是**：覆盖层的交换链尺寸处理曾经导致 GPU crash，**已修复**。

**仍需警惕的是**：自动安装与命令安装的差别**只在时机与线程**；若将来又出现钩子相关崩溃，
应当**用转储去定性**，而不是沿用这条旧结论
（流程见 [dev/diagnostics.md](../dev/diagnostics.md#崩溃分析)）。

### 7.1 另一条被实测推翻的旧记录：可以在被注入的上下文里挂钩

`Hooks.h` / `Hooks.cpp` 文件头早期写着"本模块不在默认初始化路径上 / 在被注入上下文里会崩"。
后续实测（注入 `Dungeons-Win64-Shipping` 并执行 `hook`）证明该路径**可用**：

```
[hook] D3D12 探测 hr=0x0 device=... queue=... swapchain=...
[hook] Present = 0x... (临时 D3D12 交换链 vtable[8])
[overlay] 就绪 (后缓冲 3 个, 格式 24, 2560x1600)
[overlay] 提交 bb=2 围栏完成=true
Present 钩子已生效, 游戏正在出帧
```

（这段日志来自 `Hooks.h` 文件头记录的实测输出；"后缓冲 3 个、2560x1600"是那台机器上的实际
交换链形态。）

**临时交换链 vtable 取 `Present` 这条路是可行的**，因此现在改为**自动安装**（见 §5）。

> ⚠️ 快照里仍有陈旧注释与之矛盾，接手时注意：
> * `Hooks.cpp` 文件头：`⚠️ 本模块不在默认初始化路径上。`
> * `Runtime.cpp` 第 4 步：`trace("帧钩子未自动安装(如需请用 hook 命令)")`
>   —— 同一进程里功能线程随后（10 秒后）会尝试自动安装，这行 trace 会误导排查者。
> * `Runtime.h` / `Hooks.h` 里"有已知崩溃风险"的措辞。

---

## 8. 怎么验证挂钩这一层

| 要验证的点 | 怎么做 | 看什么 |
|---|---|---|
| `Present` 地址定位是否正确 | 注入后执行命令 `hook` | 日志里 `[hook] Present = 0x... (临时 D3D12 交换链 vtable[8])`；`status` / 面板 `Frame Hook` 页显示 `installed` + 地址 |
| 地址是否落在 dxgi 代码段 | `tests/d3d_probe`（普通进程逐字节验证） | `sizeof(DXGI_SWAP_CHAIN_DESC) = 72`、`vtbl[8] = Present`、段属性 `EXECUTE_READ` |
| 钩子是否真的在出帧 | 看日志 | `Present 钩子已生效, 游戏正在出帧`（第一帧回调）；`Frames` 计数持续增长 |
| 游戏队列是否被捕获 | 看覆盖层是否渲染出画面 | 未捕获时 `createResources` 直接失败：`还没捕获到游戏的命令队列 —— 无法保证渲染进入呈现结果` |
| 自动安装是否按 10 秒执行 | 注入后看日志时间戳 | `...将在 10 秒后尝试` → 10 秒后 `Present 钩子自动安装: 开始尝试(命令路径 hook 亦可手工重试)` → `...成功` |
| 关闭途径是否生效 | 设 `EPSILON_NO_AUTO_HOOK=1`，或建 `%TEMP%\epsilonPayload_no_autohook` | 日志出现 `Present 钩子自动安装已由 EPSILON_NO_AUTO_HOOK 关闭` |
| 一次注入是否只挂一次 | 手工再执行 `hook` | 直接短路返回 true，不会出现第二次 `已挂上` |

回到索引：[../README.md](../README.md)
