# MCD2 热注入框架 — 开发计划 / 进度台账

> 状态：**注入链路已端到端跑通并验证**（见 §4 实测记录）
> 目标：C++23 + MSVC + CMake + **MinHook(vcpkg)** 的远程线程注入器 +
> UE5 运行时反射数据采集注入体，UI 为命令行。

---

## 1. 交付物与验收

| # | 产物 | 验收标准 | 状态 |
|---|---|---|---|
| 1 | `build/release/bin/mcd2_injector.exe` | 选进程 → 建管道 → `CreateRemoteThread(LoadLibraryW)` 注入 → 等 ready → 下发命令 / 交互 shell | ✅ **已验证** |
| 2 | `build/release/bin/mcd2_payload.dll` | 连管道 → 定位 GObjects/GNames → 重建属性偏移 → 响应命令 | ✅ **已验证** |
| 3 | `build/release/bin/mcd2_testtarget.exe` | 无 D3D / 无 UE 的靶子，用于验证链路与优雅降级 | ✅ **已验证** |
| 4 | 诊断工具 `d3d_probe` / `pipe_test` | 隔离验证 D3D 调用与管道双向收发 | ✅ 已完成 |

---

## 2. 已锁定的硬性约束（来自需求）

| 约束 | 落实方式 |
|---|---|
| **只做注入** | 已删除 `eject_dll()`、`--eject`、`-e`。`proc_util.h` 里写明了为什么不提供卸载：注入体挂钩子/起线程后半途 `FreeLibrary` 会留下悬空回调，目标必崩 |
| **单一方案，不做多路 fallback** | 见 §3「已删除的 fallback」——共砍掉 10 处多方案分支、2 个模块、约 470 行 |
| **MinHook** | vcpkg manifest 依赖 `minhook 1.3.4`（`x64-windows-static`），注入体静态链接 |
| **vcpkg** | `vcpkg.json` + `CMakePresets.json` 里的 toolchain 文件路径 |
| **C++23 / MSVC** | `/std:c++latest`，MSVC 19.51.36256.0，`/W4` 下 **0 error 0 warning** |
| **不要卸载 DLL** | 无 `FreeLibrary` 路径；`shutdown_runtime()` 只断管道 |

---

## 3. 已删除的 fallback（2026-09-30 精简）

按"同一问题不允许有多条实现路径"的要求做过一次全量审计，删掉以下内容：

| 删除项 | 原 fallback 链 | 现在 |
|---|---|---|
| **管道名两候选** | 环境变量 `MCD2_PIPE_NAME` → PID 推导约定名 | 只用 PID 推导 |
| **远程环境块改写** | 4a 原地覆盖 → 4b 远程分配 + 改 PEB 指针 | **整块删除**（连带 PEB 操作、`NtQueryInformationProcess`、约 190 行） |
| **GObjects 三路** | 实测 RVA → RVA−0x10 → `.text` 特征码扫描 | 只用实测 RVA + 结构校验 |
| **GNames 两路** | 实测 RVA → 扫 `.data` 找 `FNamePool::Blocks` | 只用实测 RVA + 名字命中率校验 |
| **GEngine/GWorld 两路** | 实测 RVA → 扫几十 MB `.data` | 只用实测 RVA + 类名校验 |
| **属性链布局六选一** | 6 组候选布局投票 | 钉死为 `Next=+0x20 Name=+0x28 Offset=+0x40` |
| **`pattern_scan` 模块** | 特征码扫描 + RIP 解析 | **整个模块删除**（砍完零引用） |
| **`MemoryWalker`** | 窗口探测遍历内存 | **删除**（仅被上面两处使用，砍完零引用） |
| **死代码 `to_absolute`、`exe_dir`** | 从未被调用 | 删除 |

**保留的**（不是 fallback，砍了会坏事）：

- `world.cpp` 里"反射问不到偏移 → 用实测经验值 `+0x30`/`+0x40`" —— 游戏更新后反射失效时，这是唯一能让 `actors`/`world` 继续工作的路径
- `command_server` 里"本类找不到属性 → 沿继承链找" —— 这是语义正确的行为，不是兜底
- 引擎定位失败重试 3 次 / 管道连接重试 —— 是重试，不是换方案
- §7 全部安全机制：`safe_read`(SEH)、`ObjectArray::validate` 三级置信度、节区边界检查、`text.cpp` 的 VT 检测

**砍掉的代价（必须记住）**：工具现在**只对当前这个二进制版本有效**。游戏一旦更新，四个 RVA 全部失效，工具会明确报"未定位"而不是尝试自动重定位。维护动作 = 更新 `engine.h` 里的 `baseline::` 四个常量 + `reflection.h` 里的布局常量。这是可预测的手工动作，换来了代码量减半和没有"某条 fallback 悄悄走错路"的风险。

---

## 3. 架构

```
mcd2_injector.exe                           mcd2_payload.dll
┌──────────────────────────┐                ┌────────────────────────────┐
│ 1. 选目标进程            │                │ DllMain: 只 CreateThread    │
│ 2. 建命名管道            │                │   (绝不在加载锁里干活)      │
│    MCD2HotInject.<pid>   │                │                            │
│ 3. 写目标环境块(冗余通路)│                │ runtime_main:              │
│ 4. CreateRemoteThread    │  命名管道      │  ├ 连管道                  │
│    (LoadLibraryW)        │◄──────────────►│  ├ 定位 GObjects/GNames    │
│ 5. 等 ready              │  双向消息      │  ├ 重建属性偏移            │
│ 6. 命令行 UI / 交互 shell│                │  └ 命令循环                │
└──────────────────────────┘                └────────────────────────────┘
                                              ↓ 落盘
                                    %TEMP%\mcd2_payload_<pid>.log
```

**为什么注入体不能直接写控制台**：`Dungeons-Win64-Shipping.exe` 是 GUI 子系统、
无有效 stdout；RemoteThread 又传不了命令行参数。所以数据经命名管道回传给
注入器终端，配置走目标进程环境块（作为冗余，主通路是 PID 推导的约定名）。

**为什么管道名用 PID 推导**：注入体自己知道 `GetCurrentProcessId()`，
两端各自算出同一个名字，零传递、零失败点。实测环境变量那条路在目标侧不可靠。

---

## 4. 实测记录

### 4.1 端到端闭环（`mcd2_testtarget.exe`）

```
[+] 注入成功 (0 ms)   远端 HMODULE : 0x00000094570000
[+] 模块已加载 : 0x007FF894570000 (1.73 MB)
[注入体] 已连接注入器管道, 输出走管道
[注入体] 引擎定位 第 1/2/3 次尝试...
[注入体] 引擎定位失败(降级为元命令模式)
[+] 注入体已就绪
mcd2> status   →  完整引擎状态报告
mcd2> hooks    →  帧钩子状态
mcd2> quit     →  bye, 运行时停止
靶子存活: 是（注入体正常驻留）
```

降级路径正确：没有 UE 反射时报"引擎未定位"，`objects` 提示先 `rescan`，
进程不崩。

### 4.2 二进制属性

| 项 | 值 |
|---|---|
| `mcd2_injector.exe` | 523,776 B |
| `mcd2_payload.dll` | 1,797,632 B |
| DLL 导入依赖 | **仅 `KERNEL32.dll` + `USER32.dll`**（静态 CRT，无 VCRUNTIME140/MSVCP140） |
| EXE 导入依赖 | 仅 `KERNEL32.dll` |
| 编译器 | MSVC 19.51.36256.0，`/W4` 0 warning |

---

## 5. 排查过程中定位并修掉的真实缺陷

按发现顺序，每条都是"症状 → 根因 → 修法"：

| # | 症状 | 根因 | 修法 |
|---|---|---|---|
| 1 | CMake 生成阶段失败 | `CMakeLists.txt` 引用了不存在的 `src/injector/inject.cpp`（逻辑已并入 `main.cpp`） | 删掉该条目 |
| 2 | 配置时编译器选成 Clang 22 | PATH 里 `D:\Programs\LLVM\bin` 排在 MSVC 前，Ninja 生成器又没指定编译器 | 加 `scripts/build.ps1`，先导入 `vcvars64` 再调 cmake |
| 3 | `vcvars` 污染 `VCPKG_ROOT` | `vcvars64.bat` 把 `VCPKG_ROOT` 设成 VS 自带的 vcpkg，覆盖了我们装了 MinHook 的那个 | 脚本里强制覆盖回 `D:\Programs\vcpkg` |
| 4 | 数十条编译错误 | `WIN32_NO_STATUS` 抹掉了 `STATUS_WAIT_0`/`STATUS_PENDING`；多个文件漏 `windows.h`；`Engine` 命名空间写成 `payload::Engine` 而非 `ue::Engine`；指针相减 `reinterpret_cast` 到 `size_t` | 逐项修正（见 §7 的坑清单） |
| 5 | 注入体连不上管道 | **`PipeServer::start()` 建完管道却没启动读线程**，且 `ConnectNamedPipe` 在同步句柄上拿 `OVERLAPPED` 调用 | 重写：acceptor 线程负责连接+读循环，主线程只等事件 |
| 6 | 目标进程被注入后立刻死 | **手写的 `DXGI_SWAP_CHAIN_DESC` 是 40 字节，真实结构是 72 字节** → D3D 按真实大小读栈内存读到垃圾 | 改用 SDK 官方头 `<dxgi.h>`/`<d3d11.h>`，不再手搓 ABI 结构体 |
| 7 | 同上 | `PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN` 实际有 12 个参数（`ppImmediateContext` 前还有一个 `D3D_FEATURE_LEVEL*`） | 补齐参数 |
| 8 | 同上 | `present_from_temp_swapchain` 的析构器 `FreeLibrary` 掉了 `dxgi.dll` —— 而返回的 `Present` 地址就在里面，`MH_CreateHook` 写入已解除映射的内存 | 去掉 `FreeLibrary`，并在注释里写明为什么必须留着 |
| 9 | 注入器收不到注入体任何消息（**最隐蔽**） | 存在**两个 `PipeClient` 实例**：`connect_injector_pipe()` 连的是 `pipe_client.cpp` 里的静态 `g_pipe`，而 `emit_now()` 往 `ctx().pipe` 发。后者从未连接，每次 `send` 都返回 false。症状极具误导性——注入器→注入体方向正常（命令能收到），反向全丢 | 统一到 `ctx().pipe` 单一实例，`ctx().pipe_connected` 记状态 |

**方法论**：第 5–9 条全部靠"可观测性"定位。注入体被塞进无控制台的进程，
出问题只能看到"进程没了"。所以加了：
- `trace()` 落盘到 `%TEMP%\mcd2_payload_<pid>.log`，每条 flush，**关键步骤之间全部留痕**
- `trace()` 同时记录 `[未送出]` 标记，把"没写"和"写了没到"直接分开
- `tests/d3d_probe` 在普通进程里复现同一段 D3D 代码，隔离注入上下文
- `tests/pipe_test` 把管道双向收发单独跑，隔离注入环境

第 9 条正是靠 `[未送出]` 标记一眼看出来的。

---

## 9. 待办

- [ ] **真机验证**：对运行中的 `Dungeons-Win64-Shipping.exe` 注入，核验
      GObjects/GNames 定位到的 RVA 是否与实测基线一致（`0x0BEA8BF0` / `0x0BDC5040`）
- [ ] 游戏内实测 `objects` / `props Character` / `actors` 的输出内容
      —— 这一步同时能验证 `reflection.h` 里钉死的布局常量对不对
- [ ] `README.md`：用法、命令表、基线数据、维护说明
- [ ] 排查 Present 钩子在被注入的 DLL 上下文里崩溃的原因
      （普通进程里已验证正常，见 `tests/d3d_probe` 输出）

---

## 7. 已知坑清单（改代码前先读）

1. **不要手写 ABI 结构体**。`DXGI_SWAP_CHAIN_DESC` 我以为 40 字节，实际 72。
   一律从 SDK 头里取。
2. **不要 `FreeLibrary` 掉你要取地址的模块**。`Present` 在 `dxgi.dll` 里，
   放掉引用计数就会把地址变成野指针。
3. **`vtable[8]` 是 `IDXGISwapChain::Present`**。索引 = `IUnknown`(3) +
   `IDXGIObject`(4，注意有 `GetPrivateData`) + `IDXGIDeviceSubObject`(1) = 8。
   已对着 SDK 的 `dxgi.h` 数过，并用 `d3d_probe` 运行时验证。
4. **不要定义 `WIN32_NO_STATUS`**，它会抹掉 `STATUS_WAIT_0` / `STATUS_PENDING`。
5. **窄字符串格式化里不能直接放 `wchar_t*`**，会触发 `std::format` 的
   formatter 查找失败（C2039/C7595）。用 `to_utf8()`。
6. **`WIN32_LEAN_AND_MEAN` 之后要显式 include** `<psapi.h>`（`MODULEINFO`）、
   `<unknwn.h>`（`IUnknown`/`IID`）。
7. **管道方向不对称时先怀疑对象错配**，不要怀疑内核。见 §5 第 9 条。
8. **改代码前先关掉所有靶子进程**，否则 `mcd2_payload.dll` 被锁，
   link 报 `LNK1104`。

---

## 8. 快速命令

```powershell
# 依赖（首次，只需一次）
$env:VCPKG_ROOT = "D:\Programs\vcpkg"
vcpkg install --triplet x64-windows-static

# 构建（脚本会自动导入 vcvars64 并钉死 MSVC）
.\scripts\build.ps1                     # release
.\scripts\build.ps1 -Fresh              # 清场重建
.\scripts\build.ps1 -Target pipe_test   # 只构建某个 target

# 自测（不碰游戏）
.\build\release\bin\mcd2_testtarget.exe          # 一个窗口
.\build\release\bin\mcd2_injector.exe -n mcd2_testtarget.exe -i

# 真机
.\build\release\bin\mcd2_injector.exe -l --filter Dungeons
.\build\release\bin\mcd2_injector.exe -i
.\build\release\bin\mcd2_injector.exe --exec status --exec "props Character"

# 诊断
.\build\release\bin\d3d_probe.exe                # D3D 交换链取 Present
.\build\release\bin\pipe_test.exe server         # 管道双向收发
.\build\release\bin\pipe_test.exe client <pid>
```

**排查注入体问题时先看**：`%TEMP%\mcd2_payload_<pid>.log`
