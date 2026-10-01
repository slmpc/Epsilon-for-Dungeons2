# 排查手册

这份文档把「症状 → 原因 → 做法」集中到一处。绝大多数条目来自实际踩过的坑。

---

## 先看哪些文件

| 文件 | 位置 | 内容 |
|---|---|---|
| **注入体文件日志** | `%TEMP%\epsilonPayload_<pid>_<模块基址>.log` | 注入体每一步的 trace、命令输出**镜像**、警告 |
| 注入器 stdout | 控制台 | 进程定位、注入结果、管道状态 |
| UE 崩溃报告 | `%LOCALAPPDATA%\Dungeons2\Saved\Crashes\UECC-Windows-*\` | `CrashContext.runtime-xml` + `UEMinidump.dmp` |
| GPU 崩溃转储 | 同上目录 | `D3D12.*.nv-gpudmp`（仅 GPU 崩溃时有） |

> ⚠️ **命令输出也会镜像到文件日志。** 注入器退出后结果仍可读 —— 排查时不要只看注入器的
> stdout，它可能先于响应到达就退出了。

日志文件名里带**模块基址**是刻意的：同一个 DLL 可以被注入多次（换个文件名就行），
每个副本各写各的，避免共享冲突导致「注入了但看不到任何日志」。

---

## 权限与前置条件

**注入器需要管理员权限。** 游戏进程完整性级别更高，否则 `OpenProcess` 返回
`Access denied (5)`。

```powershell
.\build\release\bin\epsilonInjector.exe                            # 找游戏 → 注入 → 装钩子 → 交互
.\build\release\bin\epsilonInjector.exe -l --filter Dungeons       # 只看进程
.\build\release\bin\epsilonInjector.exe --pid <PID> -v             # 指定 PID
```

无参数时的完整流程与各开关见 [`../reverse/toolchain.md`](../reverse/toolchain.md)。

### 注入器找不到游戏时看什么

| 症状 | 原因 | 做法 |
|---|---|---|
| `找不到运行中的 Dungeons-Win64-Shipping.exe (等了 60000 毫秒)` | 游戏没启动，或者映像名与默认不符 | 先启动游戏；`--list --filter Dungeons` 看真实映像名，再用 `--name` |
| `找到 N 个候选进程, 用 --pid 指定其中一个` | 同时跑着多个实例 / 另一个 UE 程序 | 按列出的 PID 用 `--pid` |
| 等游戏时一直刷 `...仍在等` | 正常 —— 只是在轮询 | 不想等就给 `--find-wait 0` |

---

## 症状 → 原因对照

### 注入相关

| 症状 | 原因 | 做法 |
|---|---|---|
| `OpenProcess` 返回 5 | 注入器没提权 | 用管理员身份运行 |
| 注入"成功"但没有任何日志 | ① 单实例守卫把新实例挡掉了（旧实例正在运行）② 日志文件共享冲突 | 见下面两条 |
| 「等注入体连接管道超时」 | 同一进程里已有旧实例占着管道名 | 这是**预期行为**，见下 |
| 第二次注入看不到任何日志 | 旧注入体还持有日志文件句柄 | 已修：文件名带模块基址 + `FILE_SHARE_READ\|WRITE` |

#### 单实例守卫是必要的，不是 bug

注入器可以反复注入同一 DLL。`LoadLibrary` 对「已在进程里的模块」只加引用计数、
**不会重跑 `DllMain`** —— 但每次注入都会让注入器**新起一个远端线程**去调 `LoadLibraryW`，
而 `DllMain` 每次被调用都会建一个新的 `runtimeMain` 线程。

后果不是「浪费点资源」这么轻：新实例连不上管道（名字被旧实例占着），注入器于是报
「等注入体连接管道超时」，看起来像注入失败，实际是旧实例赢了竞争。

用命名互斥体而不是「枚举模块名」：后者要处理路径/文件名/重命名副本等一堆边界，
而且判断与创建之间有竞争窗口。互斥体由内核保证原子性。

**副作用：注入体驻留期间无法重新注入新构建，必须重启游戏。** 这是刻意的取舍。

绕开方式：**换个 DLL 文件名**。互斥体名与管道名都带「槽位」（模块文件名去扩展名），
不同文件名 = 不同槽位 = 不同管道，可以并存。调试时常用这个。

> 若将来要做热重载，正确做法是让旧实例响应一个 `reload` 命令后自行退出，
> 而不是放宽守卫。

### 引擎定位相关

| 症状 | 原因 | 做法 |
|---|---|---|
| `status` 报 GObjects/GNames「未定位」 | 游戏还没加载完，或游戏更新后 RVA 失效 | 先 `rescan`；仍然失败就按 [../offsets/README.md](../offsets/README.md) 重测 RVA |
| `props <类名>` 属性数恒为 0 | **本构建的 `UStruct` 布局被改过**，`ChildProperties` 链走不通 | 这是已知限制，见 [../reverse/reflection-limits.md](../reverse/reflection-limits.md) |
| `props` 输出值荒谬（偏移离谱） | 反射布局常量失效 | 用 `probeff` / `dumpStructPointerSlots` 的槽扫描取证 |
| `actors` 读出 0 个 Actor | `ULevel::Actors` 偏移失效 | 用 `scanlevel <ULevel 地址>` 扫 TArray 形态的候选 |

### 功能模块相关

| 症状 | 原因 | 做法 |
|---|---|---|
| 模块注册了但**不出现在面板上** | `hidden_` 被 `reset()` 恢复成了 `defaultHidden_(true)` | 构造期必须用 `setDefaultHidden` 而非 `setHidden`，见 [../features/module-framework.md](../features/module-framework.md) |
| 代码改对了，模块**还是不显示** | 错误默认值已经把 `"hidden": true` **持久化**到配置里 | 手改配置或删掉该模块的 json，再点面板 **Reload**（不必重启游戏） |
| 改键绑好了但一重启就丢 | 同样：构造期用了 `setKeyBind` 而非 `setDefaultKeyBind` | 同上 |
| 模块开着重启后又变成关闭 | 默认值是 `enabled: false`（Speed/Jump 的刻意选择） | 见 [../features/speed-jump.md](../features/speed-jump.md) |
| 面板上数值在闪，角色却没变快 | 该字段每帧被游戏重算 | 日志里会有 `[Speed] 持续重写 …` 警告；该去**挂钩**而不是继续写 |
| 面板文案显示成一串 `?` | 文案里写了中文，而 ImGui 内置字体只有 ASCII 字形 | 面板文案一律纯 ASCII 英文，见 [../features/ui.md](../features/ui.md) |

### 覆盖层 / 钩子相关

| 症状 | 原因 | 做法 |
|---|---|---|
| 覆盖层不显示，但 `hook` 报成功 | 还没捕获到游戏的 DIRECT 命令队列 | 见下 |
| 切全屏 / 改窗口尺寸后 UE 弹 `GPUCrash`，日志一切正常 | 交换链重建没跟上（**曾经的根因，已修**） | 见 [../payload/overlay.md](../payload/overlay.md) |
| 一切"成功"但屏幕上什么都没有 | ① 没显式设视口 ② PSO 没建出来（Release 下 `IM_ASSERT` 是空操作） | 见 [../payload/overlay.md](../payload/overlay.md) |
| 疑似钩子相关崩溃 | —— | **先按下面「崩溃分析」解析 minidump 定性**，不要靠「崩的时候我们在干什么」推断 |
| 自动挂钩想临时关掉 | —— | 设 `EPSILON_NO_AUTO_HOOK=1`（**在游戏之外**生效，不必先让游戏成功启动过）；或创建 `%TEMP%\epsilonPayload_no_autohook` 标记文件（**不用重启游戏**） |

#### 为什么覆盖层必须用游戏自己的命令队列

D3D12 的 flip 模型交换链与**创建它的那个队列**绑定。在别的队列上渲染会导致
「命令执行了、围栏也完成了、但画面不进入呈现结果」。

我们自己的临时队列因此只能用来取 vtable，**不能用来画**。队列由
`ExecuteCommandLists` 钩子在 Present 之前捕获。

---

## 崩溃分析

### 先从转储定性，不要靠「崩的时候我们在干什么」推断

UE 崩溃目录里同时有 `CrashContext.runtime-xml` 与 `UEMinidump.dmp`。
`build/minsd.py` 能从 minidump 里取出三样决定性信息：

| 字段 | 含义 |
|---|---|
| exception code | `0xC0000005` 访问违例 / `0x8000` GPU 崩溃 |
| exception address | 出错指令的地址（换算成模块 RVA 就能回 IDA 查函数） |
| fault address | 被访问的那个地址（`0x0` 就是空指针解引用） |

⚠️ **faulting RIP 通常落在 `ntdll` 的异常分发处**（`ntdll+0x161c44`），
不要把它当成崩溃现场。

```powershell
python build/minsd.py <UEMinidump.dmp>
```

**定位步骤：** `exception address` − 模块基址 = RVA，再加 IDA 的 image base
（`0x140000000`）即可在 IDA 里定位到函数。

> ⚠️ 有的转储是 **0 字节**，那就无法分析 —— 记录下时间点即可，不要强行归因。

### 已定性的崩溃（供将来对照）

| 时间 | 类型 | 结论 |
|---|---|---|
| 14:43:46 / 14:45:04 | `0x8000` GPU crash | **我们造成的**：覆盖层未处理交换链尺寸变化。已修 |
| 15:45:38 / 16:06:48 | `0xC0000005` 读 | 空指针解引用（`fault address = 0x0`），出错指令在**游戏自身代码**里（RVA `0x12A0536` / `0x55DA583`），两次位置不同。**未定性** |
| 15:09:59 | 无转储（0 字节） | 无法分析 |

### 关于 Present 钩子的崩溃归因 —— 已修正

早先曾把两次崩溃归因于「自动安装 Present 钩子」，依据只是日志顺序（崩溃前最后一行是
自动安装提示）。**转储分析不支持这个结论：**

- 14:43 / 14:45 两次是 GPU crash，根因是覆盖层的交换链尺寸问题，与钩子安装方式无关
- 16:06:48 那次，游戏在**预定的钩子尝试之前 4 秒**就死了 —— 日志里「开始尝试」那一行
  根本没出现，所以不可能是钩子干的
- 15:09 那次没有转储可查

因此「自动安装必崩、命令安装稳定」这个规律**证据不足**，不要把它当成既定事实。
真正已证实的是：**覆盖层的交换链尺寸处理曾经导致 GPU crash，已修复。**

（仍需警惕的是：自动安装与命令安装的差别只在时机与线程。若将来又出现钩子相关崩溃，
应当用转储去定性，而不是沿用这条旧结论。）

---

## 输出通道

注入体**只有一种**输出通道：命名管道，数据回吐给注入器的控制台。
注入体自身**不弹控制台**（它在游戏进程里，弹窗只会碍事）。

管道没连上时进入 `Sink::none`：只写文件日志，静默驻留，命令不可用。

`trace()` 的每一行都带一个「`[未送出]`」标记位 —— 它把「注入体没写」和「写了但没到」
两种情况直接分开。排查「注入器收不到消息」时这一位是决定性的。

---

## 相关文档

- 注入体生命周期：[../payload/lifecycle.md](../payload/lifecycle.md)
- 输出与日志通道：[../payload/output.md](../payload/output.md)
- 渲染挂钩：[../payload/hooks.md](../payload/hooks.md)
- 覆盖层：[../payload/overlay.md](../payload/overlay.md)
- 逆向工具链与命令表：[../reverse/toolchain.md](../reverse/toolchain.md)
