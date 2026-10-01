# 注入体生命周期

本文讲注入体 DLL 从被 `CreateRemoteThread(LoadLibraryW)` 塞进游戏进程开始，到
命令循环结束、进程残留驻留为止的完整流程，以及这条路径上每一个"看起来可以简化、
但实测不能简化"的决定。

主要来源：[`src/payload/DllMain.cpp`](../../src/payload/DllMain.cpp)、
[`src/payload/Runtime.cpp`](../../src/payload/Runtime.cpp)、
[`src/payload/PipeClient.cpp`](../../src/payload/PipeClient.cpp)。

---

## 1. 全流程一览

```
CreateRemoteThread(LoadLibraryW)          ← 注入器侧
  └─ DllMain(DLL_PROCESS_ATTACH)          ← 持有加载器锁, 只做两件事
       ├─ DisableThreadLibraryCalls
       ├─ setModuleInfo(宿主 EXE 的基址/大小/路径)
       └─ _beginthreadex(startThread) → 立刻返回
            └─ runtimeMain()              ← 真正的初始化都在这里
                 1. logFileOpen()                    最先开日志
                 2. claimSingletonOrExit()           单实例守卫
                 3. 决定输出通道 (pipe / none)
                 4. 定位引擎 GObjects / GNames (最多 3 次, 每次间隔 1s)
                 5. 建 CommandServer
                 6. initModules + bindEngine + 配置加载 + 起功能线程
                 7. gReady = true, 进入命令循环
                       ├─ sink == pipe : Hello + ready 帧, 起命令读线程
                       └─ sink == none : 静默驻留 (200ms 轮询)
  └─ DllMain(DLL_PROCESS_DETACH, reserved == nullptr) → shutdownRuntime()

功能线程 (30Hz)  ── 与 runtimeMain 并行 ──
     tickFrame() → ModuleManager::onFrame() → maybeAutoInstallHook()
     → 每 5 秒 saveIfDirty() → 每 ~5 秒心跳 trace → Sleep(33)
```

---

## 2. `DllMain`：只创建一个线程，然后立刻返回

**现象/问题**：远程线程注入把 `LoadLibraryW` 当线程入口，所以 `DllMain` 是在
`CreateRemoteThread` 的那个线程上被调用的 —— **此时持有加载器锁**。在锁内创建工作
线程、初始化 CRT、加载别的 DLL 都可能死锁。

**做法**：`DLL_PROCESS_ATTACH` 分支里只做三件事：

1. `DisableThreadLibraryCalls(module)`。
2. 采集**宿主进程主模块**的基址/大小/路径（见 §3）。
3. `_beginthreadex(nullptr, 0, &startThread, ...)`，然后 `break` 返回 `TRUE`。

**为什么是 `_beginthreadex` 而不是 `CreateThread`**（[`DllMain.cpp`](../../src/payload/DllMain.cpp#L66-L77)）：
这个线程会大量使用 CRT（`std::string` / `std::format` / `std::mutex` / `std::function`）。
`CreateThread` 不初始化 CRT 的线程级状态，在静态链接 CRT 的 DLL 里会导致**不可预测、
延迟发生**的崩溃 —— 极难定位。同样的理由也用在功能线程的创建上（见 §7）。

**失败处理**：线程都起不来就直接 `return FALSE`，宣告模块加载失败（注入器侧会表现为
远端 `LoadLibraryW` 返回 0）。

### `DLL_PROCESS_DETACH`

```cpp
case DLL_PROCESS_DETACH:
    // reserved != nullptr 表示进程正在终止, 此时不宜做任何分配/等待。
    if (reserved == nullptr) {
        epsilon::payload::shutdownRuntime();
    }
```

`reserved != nullptr`（进程正在终止）时**什么都不做**。

---

## 3. ⚠️ 交给运行时的是宿主 EXE 的基址，不是注入体自己的模块

这是**踩过的坑**（[`DllMain.cpp`](../../src/payload/DllMain.cpp#L44-L64)）：

* **现象**：注入成功，但引擎永远定位不到（名字池校验必然 `0/64` 失败）。
* **原因**：基线 RVA（`GObjects 0x0BEA8BF0` / `GNames 0x0BDC5040` / …）全部是相对
  `Dungeons-Win64-Shipping.exe` 的。早先这里传的是**注入体自己的模块**，于是候选地址
  被算成"注入体基址 + RVA"，偏出去几十 MB。
* **结论/做法**：`GetModuleHandleW(nullptr)` 拿到进程主模块，再用
  `GetModuleInformation(GetCurrentProcess(), host, &mi, ...)` 取 `lpBaseOfDll` /
  `SizeOfImage`；取不到时退化传 `(host, 0, path)`。

实现细节：`WIN32_LEAN_AND_MEAN` 之后 `windows.h` 不再带入 `psapi.h`，所以要显式
`#include <psapi.h>`。

> 注意区分两个"模块基址"：
> * `Runtime::moduleBase()` / `Engine::moduleBase()` = **宿主游戏 EXE** 的基址（本节）。
> * 文件日志名里的 `<模块基址>` = **注入体 DLL 自己**的基址（见
>   [output.md](output.md#文件日志命名)）。
>
> 两者用途完全不同，混用会得到"偏移全错"或"日志找不到"两种截然不同的症状。

---

## 4. `runtimeMain` 流程

`runtimeMain` 的第一件事是 `SetThreadDescription(L"epsilon-runtime")`（便于在调试器里
认线程），第二件事是 **`logFileOpen()`**：后面每一步都要留痕，崩了才查得到。

### 4.1 单实例守卫最先做

**理由**（[`Runtime.cpp`](../../src/payload/Runtime.cpp#L306-L313)）：重复实例如果先连上
管道，会让注入器误判成"注入成功但命令没响应"。所以守卫排在连管道之前。

### 4.2 输出通道决策（管道 vs 静默）

```cpp
if (connectInjectorPipe(8000, &pipeErr)) {
    c.sink = Sink::pipe;
    c.verbose = true;
    trace("已连接注入器管道, 输出走管道");
} else {
    // 管道没连上: 不弹控制台(don't add fallback paths), 只留文件日志。
    c.sink = Sink::none;
    trace(fmt("管道连接失败({}) —— 进入静默模式, 只有文件日志", pipeErr));
    trace(fmt("日志文件: {}", logFilePath()));
}
```

要点：

* 只有两条路：`Sink::pipe`（回传注入器，唯一正常路径）与 `Sink::none`（只写文件日志，
  静默驻留）。**刻意不做第三条 fallback**（不 `AllocConsole`，见 §9 陈旧注释）。
* 重试窗口给到 **8000 ms**：同一进程里可能已被注入过本 DLL 的旧实例，它们会短暂抢占
  管道名；旧实例连上后大多因注入器退出而断开，重试窗口要覆盖住这段时间。
* 连不上时把**失败诊断**一并写进日志（见 [dev/diagnostics.md](../dev/diagnostics.md)）。

### 4.3 引擎定位：固定 3 次重试，间隔 1 秒

```cpp
for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
    trace(fmt("引擎定位 第 {} 次尝试...", attempt));
    ok = gEngine->init();
    if (!ok) {
        logWarn(fmt("第 {} 次定位失败(游戏可能仍在加载), 1 秒后重试...", attempt));
        ::Sleep(1000);
    }
}
trace(ok ? "引擎定位成功" : "引擎定位失败(降级为元命令模式)");
```

* 重试理由：注入可以发生在游戏**仍在加载**的时候。
* 3 次都失败**不致命**：降级为"元命令模式"，仍可用 `rescan` / `status` / `help` 等，
  之后还能靠 `rescan` 重新定位。

### 4.4 功能模块：即使引擎没就绪也要走这一步

```cpp
epsilon::feature::initModules();
epsilon::feature::bindEngine(gEngine.get());

auto& cfg = epsilon::feature::ConfigManager::instance();
if (cfg.initialize()) {
    cfg.applyToModules();
} else {
    logWarn(fmt("配置初始化失败, 模块使用默认值: {}", cfg.lastError()));
}

startFeatureThread();
```

* 引擎没就绪也要注册 + 起线程：玩家可能还没进关卡，模块需要在那之后**自己解析成功并
  生效** —— 这正是 `game/dungeons2/MovementResolver` 的节流重试所服务的场景。
* 配置读取失败不致命：模块停在默认值。

| 失败点 | 后果 |
|---|---|
| 引擎定位失败 | 降级为元命令模式（`rescan` 可补救） |
| 配置初始化失败 | 模块用默认值（日志里有 `cfg.lastError()`） |
| 功能线程创建失败 | `logWarn("功能模块线程创建失败 errno=... —— 模块开关将不会自动生效")`，其余功能不受影响 |

---

## 5. 单实例守卫（`claimSingletonOrExit`）

### 5.1 为什么必须有

注入器可以对同一进程**反复注入同一个 DLL**：

* `LoadLibrary` 对"已在进程里的模块"只是加引用计数，**不会重跑 `DllMain`**；
* 但**每次注入都会让注入器新起一个远端线程去调 `LoadLibraryW`**，而 `DllMain` 每次被
  调用都会建一个新的 `runtimeMain` 线程。

于是重复注入 N 次就留下 N 个 `runtimeMain`，全都在抢同一个管道名。

**后果不是"浪费点资源"这么轻**（实测连续迭代时被坑过）：
新实例连不上管道（名字被旧实例占着），注入器报
`等注入体连接管道超时 —— DLL 可能已加载但初始化卡住/失败`，
**看起来像注入失败，实际是旧实例赢了竞争**，会让人误以为代码坏了。

### 5.2 命名规则

```
Local\epsilonPayload_instance_<PID>_<slot>          (slot 非空时)
Local\epsilonPayload_instance_<PID>                (取不到模块名时退化)
```

| 组成 | 作用 |
|---|---|
| `PID` | 把作用域限定在当前进程内 —— 我们只关心"同一进程里别重复起实例"；不同进程各有各的注入体是正常且期望的。 |
| `slot` | 由**注入体自己的模块文件名**推导（`ownModuleSlot()` → `moduleSlotFromPath()`：取文件名、去 `.dll`、只保留字母数字）。 |

**为什么用命名互斥体而不是"枚举模块名"**：后者要处理路径/文件名/重命名副本等一堆边界，
而且判断与创建之间有竞争窗口；互斥体由内核保证原子性，一次成功创建就代表独占。

**容错**：`CreateMutexW` 失败时 `return true` —— 建不出来就别拦，让它继续跑。

### 5.3 为什么带槽位不算"放宽守卫"

这是刻意的设计（[`Runtime.cpp`](../../src/payload/Runtime.cpp#L274-L287)、
[`PipeChannel.h`](../../src/common/PipeChannel.h#L42-L54)）：

* 守卫要防的是**同名实例争抢同一条管道**；不同槽位对应**不同管道**，不存在竞争。
* 收益：**改完代码换个 DLL 名字就能注入，不必重启游戏** —— 注入体驻留期间无法重新注入
  同名 DLL（受守卫阻拦），这是实际开发中最痛的约束之一。

**代价**：同一进程里可能同时存在多个注入体，所以"别让它们都去挂钩子" —— 靠
`%TEMP%\epsilonPayload_no_autohook` 标记文件关掉第二个实例的自动挂钩（见
[hooks.md](hooks.md#关掉自动安装的两条途径)）。

**若要热重载**：正确做法是让旧实例响应一个 `reload` 命令后自行退出，
**而不是放宽守卫**。

### 5.4 守卫失败时的行为

```cpp
if (!claimSingletonOrExit()) {
    trace("本进程里已有一个注入体实例在运行 —— 本次实例退出");
    trace("(预期行为: 避免多个实例争抢同一管道名)");
    logFileClose();
    return 0;
}
```

注意它**先把文件日志开好**再判定守卫，所以这次退出的记录是能查到证据的。

---

## 6. 管道名与合作方式

* 管道名 = `EpsilonHotPipe2.<目标PID>[.<slot>]`，完整路径 `\\.\pipe\<name>`。
* 两端**零传递**：注入器用"目标 PID + 被注入 DLL 文件名"算，注入体用"自己的 PID + 自己的
  模块文件名"算，得到同一个字符串。
* 历史坑：老前缀 `EpsilonHotInject.<pid>` 在真游戏上**稳定拿到 ACCESS_DENIED**，怀疑是
  名字层面的残留/冲突，于是换了个全新前缀来排除这个变量
  （[`PipeChannel.cpp`](../../src/common/PipeChannel.cpp#L145-L157)）。
* 更早还试过用环境变量传递管道名（注入器改写目标 PEB）：**实测
  `GetEnvironmentVariableW` 读不到被外部改写的 PEB**，那条路从未生效，已删除（连同注入器
  侧整个 PEB 改写代码）。现在真正的可靠性来自约定名。

> ⚠️ 快照里 `PipeClient.h` 的 `connectInjectorPipe` 注释仍写着"名字从环境变量
> `EPSILON_PIPE_NAME` 读"，`PipeChannel.h` 与 `Runtime.cpp` 的文件头也提到 `EPSILON_PIPE_NAME` /
> `ALLOC_CONSOLE` 路线。**这些是陈旧注释**：快照代码里没有任何地方读 `EPSILON_PIPE_NAME`，
> 也没有 `AllocConsole` 分支（实际只读两个环境变量：`EPSILON_NO_AUTO_HOOK`、
> `EPSILON_CONFIG_DIR`）。

---

## 7. 功能线程

### 7.1 为什么单独一条线程

而不是挂在 Present 帧钩子上（[`Runtime.cpp`](../../src/payload/Runtime.cpp#L44-L58)）：

* 帧钩子在本项目里是**可选的**，而且它的交换链探测在被注入的上下文里有已知风险
  （见 [hooks.md](hooks.md)）；
* **功能模块不该依赖一个默认关闭、且有风险的机制才能工作**。

### 7.2 为什么是 30 Hz

```cpp
constexpr DWORD featureTickIntervalMs = 33;
```

* 这类模块改的是"手感的标量"（最大步速 / 跳跃初速度），30 Hz 已经完全跟得上；再高只是
  徒增跨线程的内存读。
* 真正的每帧精度留给将来的瞄准类功能 —— 那种**应当走帧钩子**。

线程安全约定：线程只做"读游戏内存 + 写几个 float"；`Module::onFrame` 的实现都只碰自己的
标量状态，不与命令线程/渲染线程共享容器；配置落盘也在本线程内串行完成。

### 7.3 心跳

```cpp
if (++loopCount % 150 == 0) {          // 150 * 33ms ≈ 5 秒
    trace(fmt("功能线程心跳 loop={}", loopCount));
}
```

* 这个线程平时是静默的，没有这条记录就完全看不出它到底有没有起来。
* **还有个实际用途**：自动安装 Present 钩子调用 `findPresent` 时会建临时 D3D12 交换链；
  在**没有 D3D12 的目标**上（比如自测靶子）这一步可能长时间不返回 ——
  **心跳停了就等于告诉你卡在那里了**。

### 7.4 配置落盘节流

```cpp
constexpr DWORD configSaveIntervalMs = 5000;
...
if (now - lastSave >= configSaveIntervalMs) {
    lastSave = now;
    epsilon::feature::ConfigManager::instance().saveIfDirty();
}
```

* 只在**真的有改动时**才写盘（`saveIfDirty()` 内部先 `anyDirty()` 判断），避免每 5 秒
  无条件重写配置文件 —— 那会让配置目录的文件时间戳一直在变，也让"到底改了什么"难以追踪。
* 注入体没有可靠的"退出前保存"时机（游戏可能被直接关掉），所以必须支持随时增量保存
  （见 [module-framework.md](../features/module-framework.md#dirty-标记与增量保存)）。

### 7.5 自动安装 Present 钩子也在这个线程里

```cpp
maybeAutoInstallHook();     // 每轮调用, 内部自己决定要不要真动手
```

放在功能线程而不是 `runtimeMain` 的启动路径上，理由：**万一 `findPresent` 把目标带走，
至少管道已连、日志已落盘，有迹可循**。细节见 [hooks.md](hooks.md#自动安装与-10-秒延迟)。

**历史坑（已修）**：之前的版本用一个"下次尝试时间戳"做门控，结果把自己的重试逻辑绕进了
死胡同（门控值与线程推进时机对不上，表现为**启用了但从不尝试**）。现在改成最简单的形态：
第一次调用就把该做的做完 —— "延迟不是一个有用的保护，反而是一个容易出错的额外状态"。

```cpp
constexpr uint64_t autoHookDelayMs = 10000;
```

### 7.6 关于本线程体的一处重复调用（未证实）

快照里 `featureTickTrampoline` 的循环体把同一段调用写了两遍，连注释都是重复的：

```cpp
// 推进一帧: 内部会(带节流地)重新解析玩家移动组件, 然后驱动各模块。
epsilon::feature::tickFrame();
epsilon::feature::ModuleManager::instance().onFrame();
// 推进一帧: 内部会(带节流地)重新解析玩家移动组件, 然后驱动各模块。
epsilon::feature::tickFrame();
epsilon::feature::ModuleManager::instance().onFrame();
```

看起来是复制粘贴残留（**未证实**，注释里没有解释）。实际影响很小：
`tickFrame()` 只累加帧号并调用带节流的 `resolve()`，`onFrame()` 的语义是幂等的"补写
被改掉的值"。记录在这里是为了避免将来有人把它当成有意设计而照抄。

---

## 8. 帧回调：故意保持极轻

```cpp
void onFrame(uint32_t frameIndex, int /*syncInterval*/, int /*flags*/) {
    if (frameIndex == 1 && !gFirstFrameLogged.exchange(true)) {
        trace("Present 钩子已生效, 游戏正在出帧");
    }
}
```

* 回调运行在**渲染线程**上，约定是"只做打标记/计数，不做遍历"。
* 第一帧打一行日志，让人确认"钩子真的挂上了、游戏在出帧"（这是验证钩子生效的最直接
  证据，比看计数器更早）。
* `installFrameHook()` 的幂等写法：已安装直接返回 true；`install()` 失败返回 false；
  成功后才 `setFrameCallback(&onFrame)`。

---

## 9. 命令循环

```cpp
if (c.sink == Sink::pipe) {
    proto::Hello hello;   // pid / moduleBase / moduleSize / protocol / tag
    c.pipe.sendPod(proto::Kind::hello, hello);
    c.pipe.send(proto::Kind::ready, ok ? "ready" : "ready-no-engine");

    pipeStartCommandReader([](std::string_view cmd) {
        beginResponse();
        const bool cont = gServer->execute(cmd);
        endResponse();
        if (!cont) shutdownRuntime();      // quit / detach
    });

    while (!gStopping.load(std::memory_order_acquire)) ::Sleep(50);
} else {
    // 管道没连上: 静默驻留。这里是**唯一**的等待逻辑, 不做"稍后补连"的
    // 复杂补救 —— 注入器没起来就是没起来, 日志里有记录。
    trace("静默驻留(未连上注入器, 命令不可用)");
    while (!gStopping.load(std::memory_order_acquire)) ::Sleep(200);
}
```

* `ready` 消息带 `ready-no-engine` 变体：注入器据此知道"连上了但引擎没定位到"
  （`waitReady` 只等 `ready`，两者都算就绪）。
* 每条命令用 `beginResponse()` / `endResponse()` 包起来，输出攒成一条管道消息
  （见 [output.md](output.md#响应缓冲)）。
* 静默模式**不支持后补连接**：这是刻意的简化，"注入器没起来就是没起来"。

---

## 10. `shutdownRuntime`：不卸载钩子，也不 `FreeLibrary`

```cpp
void shutdownRuntime() {
    std::lock_guard lk(gLifecycleMu);
    if (gStopping.exchange(true)) return;   // 幂等

    // 注意: 这里**不卸载钩子, 也不 FreeLibrary**。
    // 注入体一旦挂钩子/起线程, 半途卸载会留下悬空回调 → 目标必崩。
    // 本框架只做注入, 不做卸载; 要清掉注入体就重启目标进程。
    trace("运行时停止(注入体仍驻留, 重启目标进程可清除)");
    pipeClose();
}
```

| 做了 | 没做 |
|---|---|
| 置 `gStopping`（命令循环退出） | 卸载 MinHook 钩子 |
| `pipeClose()`（断管道、清 `pipeConnected`） | `FreeLibrary` 自己 |
| 留一行 trace 到文件日志 | 释放游戏对象 / 还原内存改写 |

**触发路径**：

1. 命令返回 `false`（`quit` / `detach`）→ `shutdownRuntime()`。
2. 注入器退出 → 管道断开 → （`bye` 消息 / 读线程结束）→ 停在静默驻留或等 `gStopping`。
3. `DllMain(DLL_PROCESS_DETACH, reserved == nullptr)` → `shutdownRuntime()`。

**排除卸载选项**：真要靠 `FreeLibrary` 干净卸载，需要注入体自己先把一切还原并通知宿主，
那是另一套生命周期协议，不在本框架范围内（[`ProcUtil.h`](../../src/common/ProcUtil.h#L86-L94)）。
`procUtil.h` 因此**不提供 eject**。

**副作用（刻意的取舍）**：注入体驻留期间无法重新注入同名 DLL，**必须重启游戏**。

---

## 11. 陈旧注释清单（以代码为准）

接手时最容易踩的坑是"相信注释"而不是"相信代码"。快照里已有几处注释描述的是历史形态：

| 位置 | 注释说法 | 实际代码 |
|---|---|---|
| `Runtime.h` 文件头 | 生命周期里 `runtimeMain()` 会"装钩子" | 启动路径**不装**钩子；自动安装发生在功能线程（10 秒后），也可用 `hook` 命令手工装 |
| `Runtime.h` 文件头、`DllMain.cpp` 文件头 | "装钩子、**开控制台**、连管道" / `DETACH` 摘钩子、关控制台 | 没有控制台分支；`shutdownRuntime` 不摘钩子 |
| `Runtime.cpp` 文件头 | 输出通道决策第 2 步是 `AllocConsole()` | 代码里明确"不弹控制台(don't add fallback paths)"，只有 pipe / none |
| `PipeClient.h`、`PipeChannel.h`、`Runtime.cpp` 文件头 | 管道名从 `EPSILON_PIPE_NAME` 读 | 无任何 `EPSILON_PIPE_NAME` 读取；用 `EpsilonHotPipe2.<pid>.<slot>` 约定名 |
| `Runtime.cpp` 第 4 步 | `trace("帧钩子未自动安装(如需请用 hook 命令)")` | 同一进程里功能线程随后会尝试自动安装 —— 这行 trace 会误导排查者 |

> 处理建议：改动这些区域时顺手把注释改成与代码一致；**不要**按注释把功能加回去。

---

## 12. 怎么验证本文档描述的流程

| 要验证的点 | 做法 |
|---|---|
| 生命周期是否走完整 | 注入后看 `%TEMP%\epsilonPayload_<pid>_<base>.log`：应有 `启动` banner → `runtimeMain 进入` → `正在连接注入器管道...` → `引擎定位 第 N 次尝试` → `初始化完成, 进入命令循环` |
| 单实例守卫是否生效 | 同名 DLL 注入第二次：日志里出现 `本进程里已有一个注入体实例在运行 —— 本次实例退出`；注入器侧报"等注入体连接管道超时" |
| 槽位是否独立 | 复制 DLL 改名为 `epsilonPayload2.dll` 再注入：两份日志各自存在，两份都能连上（管道名不同） |
| 功能线程是否活着 | 日志里每 ~5 秒一行 `功能线程心跳 loop=N`；`findPresent` 卡住时心跳会停止 |
| 模块定位重试是否发生 | 早期注入（游戏还在加载）时日志里出现 `引擎定位 第 1/2/3 次尝试` |
| 卸载确实没发生 | 执行 `quit` 后进程仍在、钩子仍在（面板/`hook` 状态仍显示 installed），日志一行 `运行时停止(注入体仍驻留, 重启目标进程可清除)` |

回到索引：[../README.md](../README.md)
