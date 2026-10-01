# 逆向工作流与工具链

> **依据**：`CommandServer` 的命令实现与帮助文本、`CommandServer.h` 里各诊断函数的说明、
> `AGENTS.md` 的调试与验证习惯、以及仓库早期注释里记录的崩溃分析过程。

---

## 环境与前置

**注入器需要管理员权限。** 游戏进程完整性级别更高，否则 `OpenProcess` 返回
`Access denied (5)`。

```powershell
.\build\release\bin\epsilonInjector.exe                            # 一把梭: 找游戏 → 注入 → 装钩子 → 交互
.\build\release\bin\epsilonInjector.exe -l --filter Dungeons       # 只看进程
.\build\release\bin\epsilonInjector.exe --pid <PID> -v             # 指定 PID
```

无参数时的完整流程：

| 步骤 | 行为 | 开关 |
|---|---|---|
| 1. 找进程 | 映像名精确匹配 `Dungeons-Win64-Shipping.exe`；没有则模糊匹配映像名含 `dungeons-win64` 的进程（排除 `Dungeons.exe` 引导器、`CrashReportClient` 等）。**最多等 `--find-wait` 毫秒**（默认 60000）等游戏启动 | `-p/--pid`、`-n/--name`、`--find-wait 0` 只查一次 |
| 2. 注入 | `CreateRemoteThread` + `LoadLibraryW`，等注入体 ready | `-d/--dll`、`--wait <ms>` |
| 3. 装 Present 钩子 | ready 后下发注入体命令 `hook` —— 与手工敲 `hook` 同一条路，不等注入体自己的 10 秒自动安装 | `--no-hook` 跳过 |
| 4. 进交互 | `epsilon>` 提示符，输入原样下发给注入体 | `-x/--exec` 变成批量模式；`-i` 强制交互，`--no-interactive` 禁掉 |

进 `epsilon>` 提示符后即可用下面的命令。注入器自己的命令（不下发给注入体）：

| 本地命令 | 作用 |
|---|---|
| `injector-help` | 列出本地命令 |
| `ps [子串]` | 列进程（注入体的 `list` 是「枚举 Actor」，所以本地这条刻意叫 `ps`） |
| `clear` / `cls` | 清屏 |
| `exit` / `quit` | 退出注入器（**注入体仍驻留**，不会卸载） |

产物：`build/<preset>/bin/`（`epsilonInjector.exe` + `epsilonPayload.dll` 同目录）。

---

## 命令表

### 元信息

| 命令 | 作用 |
|---|---|
| `status` | 引擎定位总览（各全局的定位方式与结构校验结果） |
| `rescan` / `init` | 重新定位 `GObjects` / `GNames` / 全局槽 |
| `globals` | `GEngine` / `GWorld` 详情与校验来源 |
| `hooks` / `hook` | 帧钩子状态；显式安装 Present 钩子 |
| `help` / `?` | 命令一览 |
| `quit` / `exit` / `detach` | 结束命令循环 |

### 对象表

| 命令 | 作用 |
|---|---|
| `objects [filter=子串] [class=类名] [limit=N] [skip=N]` | 列出 UObject |
| `find <名字>` | 按名字查对象（支持子串，全表扫描） |
| `class <类名>` | 类的大小 / 父类 / 本类属性数 / 实例数 |

### 属性与偏移

| 命令 | 作用 |
|---|---|
| `props <类名> [inherited=1]` | 列出该类的属性链与偏移（含继承）。⚠️ 本构建反射不可用时为空，会转为**指针槽取证** |
| `probeff <FField 地址>` | 在已知是 `FField` 的对象上，从 `0x18..0x48` 每 4 字节试一次，定出 `NamePrivate` 的真实偏移 |
| `findprop <类名> <属性名>` | **绕开 FField 链**直接从 `UClass` 内存反查属性偏移 |
| `get <对象名> <属性名>` | 读某个对象的属性值（按类型渲染） |
| `set <对象名> <属性名> <值>` | 写一个裸值属性（只支持数值 / 布尔） |

### 世界

| 命令 | 作用 |
|---|---|
| `world` / `level` | 当前 `UWorld` / `PersistentLevel` 与偏移来源 |
| `actors [limit=N] [class=类名]` | 枚举关卡里的 Actor |
| `player` / `pawn` | 玩家候选诊断：列出「长得像玩家」的 Actor 并标出模块会选中的那个 |
| `scanlevel <ULevel 地址>` | 在该对象上扫 TArray 形态的字段（找回 `Actors` 偏移用） |

### 原始内存

| 命令 | 作用 |
|---|---|
| `mem <rva> [len=N]` 或 `mem va=<绝对地址> [len=N]` | 原始内存 dump + 疑似指针列表 |
| `ptr <地址> [个数]` | 按指针逐个解释一段内存（是 UObject？还是 FName？） |
| `floats <地址> [个数]` | 按 float 解读一段内存（**个数是第二个位置参数**，不是 `n=` 选项） |
| `poke <绝对地址> <浮点值>` | 往任意地址写一个 float，带回读与 8 次采样 |

### 移动参数

| 命令 | 作用 |
|---|---|
| `mv` / `movement` | 读玩家移动组件上几个关键 float 的当前值 + 三处邻域窗口 |
| `attrmv` / `attrs` | 找出玩家的 `ATR_Movement`，打印属性块，并试写 `MovementSpeedMultiplier` |
| `mvset <字段名> <数值>` | 一次性直写一个移动字段（支持裸偏移 `+0x230`），带回读与 8 次采样 |

> `floats` 的「个数是位置参数」这条是踩过坑的：早期用 `optInt` 找 `n=` 选项，
> 于是调用方传的位置参数被静默忽略，**永远只打 16 个**。
> 排查属性块时正好被这个坑到 —— 明明要 48 个却只看到 16 个。

> `mvset` 的**立刻回读**也是踩过坑的：早期打印的是「我们打算写进去的值」而不是回读结果，
> 导致「写入失败」与「写入成功但被游戏覆盖」两种情况在输出上**完全一样**。
> 现在先做一次不睡眠的回读，把两者分开。

---

## 日志与崩溃报告

| 文件 | 位置 |
|---|---|
| 注入体文件日志 | `%TEMP%\epsilonPayload_<pid>_<模块基址>.log` |
| 崩溃报告 | `%LOCALAPPDATA%\Dungeons2\Saved\Crashes\UECC-Windows-*\CrashContext.runtime-xml` |
| 崩溃转储 | 同目录 `UEMinidump.dmp`（**有时是 0 字节**） |
| GPU 崩溃转储 | 同目录 `D3D12.*.nv-gpudmp`（仅 GPU 崩溃时有） |

> ⚠️ **命令输出也会镜像到文件日志。** 注入器一旦退出（比如 `--exec` 跑完就关），
> 还在路上的响应就彻底丢了 —— 排查时表现为「命令执行了但没有任何输出」。
> 落盘之后无论注入器是否还在，结果都能读回来。

---

## 崩溃分析

### 先从转储定性，不要靠「崩的时候我们在干什么」推断

`build/minsd.py` 这个一次性小工具能从 minidump 里取出三样决定性信息：

| 字段 | 含义 |
|---|---|
| exception code | `0xC0000005` 访问违例 / `0x8000` GPU 崩溃 |
| exception address | 出错指令的地址（换算成模块 RVA 就能回 IDA 查函数） |
| fault address | 被访问的那个地址（`0x0` 就是空指针解引用） |

```powershell
python build/minsd.py <UEMinidump.dmp>
```

⚠️ **faulting RIP 通常落在 `ntdll` 的异常分发处**（`ntdll+0x161c44`）——
无调试器接管时它不是崩溃现场，不要拿它定位。

### RVA ↔ IDA 换算

```
模块 RVA = exception address − 模块运行时基址
IDA 地址  = RVA + 0x140000000          (IDA 的 image base)
```

**先做这一步再谈归因。**

### 已定性的崩溃（供将来对照）

| 时间 | 类型 | 结论 |
|---|---|---|
| 14:43:46 / 14:45:04 | `0x8000` GPU crash | **我们造成的**：覆盖层未处理交换链尺寸变化。已修 |
| 15:45:38 / 16:06:48 | `0xC0000005` 读 | 空指针解引用（`fault address = 0x0`），出错指令在**游戏自身代码**里（RVA `0x12A0536` / `0x55DA583`），两次位置不同。**未定性** |
| 15:09:59 | 无转储（0 字节） | 无法分析 |

### 关于 Present 钩子归因的修正

早先曾把两次崩溃归因于「自动安装 Present 钩子」，依据只是日志顺序。
**转储分析不支持这个结论** —— 其中一次游戏在预定的钩子尝试**之前 4 秒**就死了，
日志里「开始尝试」那一行根本没出现。

详见 [`../payload/hooks.md`](../payload/hooks.md) 与
[`../dev/diagnostics.md`](../dev/diagnostics.md)。

---

## 改完代码后的验证习惯

- 跑一次**完整构建**（`.\scripts\build.ps1` + 四个 `-Target` 测试靶子），
  `/W4` 下保持 0 error 0 warning
- 涉及内存布局的改动要有**实测依据**，不要只靠推理；
  并在 [`../offsets/`](../offsets/) 里写清「怎么验证的」，便于游戏更新后复现
- 一次性诊断程序放在 `build/` 下手工编译，**不要加进 CMakeLists**（`build/` 已在 `.gitignore` 中）
- 注入体驻留期间**无法重新注入新构建**（单实例守卫）—— 迭代时换个 DLL 文件名，或重启游戏

---

## 相关文档

- 定位与校验：[ue-runtime.md](ue-runtime.md)
- 反射为何不可用：[reflection-limits.md](reflection-limits.md)
- 偏移验证方法：[property-verification.md](property-verification.md)
- 症状对照表：[../dev/diagnostics.md](../dev/diagnostics.md)
