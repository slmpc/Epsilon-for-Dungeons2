# AGENTS.md — Epsilon For Dungeons II

本文件给在此仓库工作的 AI 代理与协作者阅读。**改动代码前请先读完「硬性约定」一节。**

---

## 硬性约定

### 1. 游戏内 UI 一律用英文 ★

**任何会被绘制在游戏进程内 ImGui 面板上的文案, 必须是纯 ASCII 英文。**

原因（实测踩过, 不是风格偏好）:

- 覆盖层用的是 ImGui **内置的 ASCII 位图字体**, 没有中文字形。
- 写中文进去不会报错, 而是渲染成一串 `?` —— 信息完全丢失。
- 典型踩坑记录: 模块面板上曾显示
  `????????????(???, ??? Mock/Mob)`, 排查者无法判断那是"没有匹配到玩家"的
  诊断信息, 白花了一轮定位。

**受此约束的文案**（都会进入面板）:

| 位置 | 例 |
|---|---|
| `Module` 的 name / description | `addDouble("Multiplier", ...)` 的描述参数 |
| `Setting` 的 name / description | 展开模块后每个控件的标签与 tooltip |
| `Setting::displayValue()` | 如 `KeybindSetting::keyName()` 返回的键名 |
| `MovementAccess::lastError()` | 面板顶部的解析失败说明 |
| `MovementAccess::offsetSource()` | 面板顶部显示的偏移来源 |
| `Module::info()` | 模块标题右侧的运行状态行 |
| `Overlay.cpp` 里所有 `ImGui::Text*` 的字面量 | 面板固定文案 |

**不受约束**: 日志与命令输出。`trace` / `logInfo` / `logWarn` / `emitLine`
走的是文件与命名管道, 中文完全正常, 且对排查更有用。**这些地方继续用中文**,
不要为了统一而改成英文。

判断方法: 问自己"这段文字会不会出现在 ImGui 面板上"。不确定时按会处理。

### 2. 命名规范

- 文件: 大驼峰(`PeImage.h` / `NamePool.cpp`)
- 类型: 大驼峰; 函数与变量: 小驼峰
- 数据成员: **保留尾部下划线**(`className_`), 便于与局部变量区分
- 常量: 小驼峰, 不用 `k` 前缀(`maxPayload`, 不是 `kMaxPayload`)
- 文件级全局: 保留 `g` 前缀(`gPresentUrl` 之类, 即 `gXxx`)
- 命名空间: `epsilon` / `epsilon::ue` / `epsilon::payload` / `epsilon::feature`

改动命名后跑 `.\scripts\auditNaming.ps1` 复核残留。

### 3. 第三方名称不要改

标准库、Win32 SDK、D3D12/DXGI、ImGui、nlohmann/json、UE 引擎自己的字段名
都保持原拼写。`auditNaming.ps1` 的 allow-list 里已列出常见项并注明理由;
遇到新的第三方名字时扩充 allow-list, 而不是去改代码。

---

## 项目结构

```
src/common/          注入器与注入体共用: PE 解析 / 进程工具 / 管道 / 文本
src/injector/        注入器 EXE(控制台前端 + 管道服务端)
src/payload/         注入体 DLL
  ue/                运行时反射: GObjects / GNames / 属性偏移 / UWorld
  feature/           功能模块框架(见下)
    settings/        设置类型体系
    module/          模块基类与管理器; module/impl/ 放具体模块
    config/          配置持久化(~/.epsilon/ext/dungeons2)
    movement/        玩家移动组件的解析与读写
    ui/              ImGui 模块面板
tests/               自测靶子与一次性诊断程序
scripts/             构建与命名审计
analysis/            逆向分析工作区(Python, 独立于 C++ 部分)
```

---

## 构建

```powershell
.\scripts\build.ps1                 # release
.\scripts\build.ps1 -Preset debug
.\scripts\build.ps1 -Target epsilonTestTarget
```

要求: C++23 / MSVC / Ninja / vcpkg(manifest 模式)。产物在
`build/<preset>/bin/`。

⚠️ `scripts\build.ps1` 会强制把 `VCPKG_ROOT` 指回 `D:\Programs\vcpkg` ——
`vcvars64.bat` 会把它改成 VS 自带的 vcpkg, 而本项目的 baseline 与已装的依赖
在用户自己的 vcpkg 里。**不要绕过这个脚本直接跑 cmake**, 否则 CMake 缓存会被
写进错误的 toolchain 路径, 之后所有配置都失败(表现为
`create_directories(...VC\vcpkg\installed): Access is denied`)。若已经踩到,
删掉 `build/<preset>` 重新配置。

---

## 关键设计约束

### 注入体只做注入, 不做卸载

`procUtil.h` 里有完整说明。注入体一旦挂钩子/起线程, 半途 `FreeLibrary` 会留下
悬空回调, 目标必崩。要清除注入体就重启目标进程。

### 单实例守卫

`Runtime.cpp` 的 `claimSingletonOrExit()` 用命名互斥体保证**每个目标进程只跑
一个注入体实例**。这是必要的: 注入器可以反复注入同一 DLL, `LoadLibrary` 只加
引用计数不重跑 `DllMain`, 但每次注入都会让注入器新起一个远端线程, 从而多出
一个 `runtimeMain`; 多个实例会争抢同一个管道名, 新实例连不上时注入器报
"等注入体连接管道超时", 看起来像注入失败。

**副作用**: 注入体驻留期间无法重新注入新构建, **必须重启游戏**。这是刻意的
取舍。若将来要做热重载, 正确做法是让旧实例响应一个 `reload` 命令后自行退出,
而不是放宽守卫。

### Present 钩子: 默认开启, 但延迟 10 秒尝试

`autoHookAllowed()` 默认返回 true —— 即注入后会自动尝试安装 Present 钩子,
**延迟 10 秒**(见 `autoHookDelayMs`)。因为 `findPresent` 会建临时 D3D12
设备与交换链, 而唯一能识别的差异是**时机**: 命令 `hook`(注入后数秒到数分钟)
多次成功, 初始化后立刻尝试则出过问题。这个延迟是基于现有证据的**缓解**, 不是
已证实的根因。

关闭途径是环境变量 `EPSILON_NO_AUTO_HOOK=1` —— 刻意放在**游戏之外**,
不必先找到配置目录、也不必让游戏成功启动过一次。

若将来又出现疑似与钩子相关的崩溃, **先按"崩溃分析"一节解析 minidump 定性**,
不要沿用"自动安装必崩"的旧结论(该结论已被转储分析推翻)。

### 覆盖层交换链重建

`Overlay.cpp` 每帧在取后缓冲**之前**比对交换链尺寸与后缓冲数量, 变化时重建
RTV 堆与各后缓冲 RTV。这不是优化, 是**必需品**:

改窗口尺寸/切全屏会让 DXGI 重建后缓冲, 旧的 `ID3D12Resource*` 全部作废。
继续用旧 RTV 做 `OMSetRenderTargets` 时 D3D12 在 CPU 侧**不报错**(命令照记、
提交也成功), 但 GPU 侧会挂死 —— 实机表现为 UE 弹出
`GPUCrash / GPU Crash dump Triggered` 并留下 `.nv-gpudmp`, 而日志里一切正常。
实测崩过两次。

设备/队列/分配器/围栏/ImGui 的 SRV 堆与尺寸无关, 重建时**不要**动它们。

### 内存访问必须走 safeRead / safeWrite

目标进程里任何指针都可能是垃圾值。全部读取走 `safeRead`(SEH 保护), 写入走
`safeWrite`(先直写, 失败才放宽页保护)。

### 配置文件的容错

配置是用户可手改的明文, 出现类型不符的值是常态。`Setting` 的读取一律用带
fallback 的取值方式, **坏值退化为默认值, 不抛异常** —— 异常从注入体的配置加载
路径穿出去会把游戏带崩。

### 命名文案的两个坑(已修, 别改回去)

模块在构造期设置"出厂默认"时必须用 **`setDefaultXxx`** 而不是 `setXxx`:

- `setDefaultKeyBind` 而非 `setKeyBind`
- `setDefaultHidden` 而非 `setHidden`

因为 `ConfigManager` 加载配置时会调用 `Module::reset()`, 而 `reset()` 会把各项
恢复成 `defaultXxx_`。只改当前值的话, 这次 reset 会把它抹掉 —— 曾导致模块明明
注册了却不出现在面板上(隐藏标志被重置回 true)。

**⚠️ 还有一个连带陷阱**: 这类"构造期默认值写错"的 bug 会把**错误的值持久化**到
配置里。修好代码之后, `reset()` 已经正确地把 `hidden_` 置回 false, 但紧接着
`fromJson()` 又从磁盘读到旧配置里的 `"hidden": true`, 于是再次被隐藏 ——
表现为"代码明明修了, 还是不显示"。

排查顺序:

1. 先看 `~/.epsilon/ext/dungeons2/configs/<配置名>/modules/<模块名>.json`
   里有没有可疑的持久化值(`hidden` / `keyBind` / `ApplyTo` 等)
2. 改掉配置, 然后在面板上点 **Reload**(不必重启游戏)
3. 再回去确认代码里的默认值

`hidden` 本身是合法功能(把模块从列表里藏起来), 所以不要把"读配置覆盖它"这件事
改成忽略 —— 要修的是默认值, 以及清掉被污染的历史配置。

---

## 逆向相关的重要背景

### 速度/移动的真正来源是 GAS 属性集 ATR_Movement ★

**不要再试图写 `UCharacterMovementComponent` 的 `MaxWalkSpeed`** —— 实测它每帧
被游戏重算(写入 130 后连续 8 次采样全是 100; 而同样方式写 `MaxAcceleration`
(+0x288) 却完全保持, 说明写路径本身没问题, 是这个字段被重算)。

真正的移动参数在 **`ATR_Movement`**(Attribute, 即 GAS 属性集)里。玩家 pawn 上
挂着唯一一个 `ATR_Movement` 实例(场景里共 30 个, 用 Outer 链判别出属于玩家的那
一个)。**写它的属性是持久的** —— 实测写入后 8 次采样全部保持, 且跨注入仍然保留。

属性偏移来自代码生成参数表(`0x14a069800` 区段, 每项 0x40 字节, 偏移在 +0x24;
注意这张表的项布局与 `UCharacterMovementComponent` 那张**不同**, 后者偏移在 +0x2c):

| 偏移 | 属性 |
|---|---|
| `+0x90` | `MovementSpeedMultiplier` |
| `+0xa0` | `MovementFriction` |
| `+0xb0` | `MovementFrictionMultiplier` |
| `+0xc0` | `MovementRotation` |
| `+0xd0` | `MovementRotationMultiplier` |
| `+0xe0` | `MovementGravity` |
| `+0xf0` | `GravityScale` |
| `+0x100` | `AirControl` |
| `+0x110` | `RollCooldown` |
| `+0x130` | `RollCharges` |
| `+0x160` | `Mass` |
| `+0x170` | `InteractionRange` |

这也解释了两个此前的疑问: 为什么组件上的 `GravityScale`(+0x1c0)与 `AirControl`
(+0x2ac)读出来都是 0 —— **真身在这里, 组件上那份是派生/未使用的**。

判别玩家实例的方法见 `cmdAttributeMovement`: 沿 Outer 链往上走, 命中玩家 pawn
的那一个就是(实测深度 1, 即 ASC 直接挂在 pawn 上)。

**仍未解决**: `MovementSpeedMultiplier` 的当前值是 0(写入 5.0 后无游戏内效果
的反馈), 所以还不能确认它是否就是驱动移动速度的那个属性。下一步应当在
`ATR_Movement` 里找**值呈现单位量级(1.0)**或随移动变化的属性, 用 `floats`
把整个属性块打出来逐个对照。

### 这个构建的 UE5 反射布局被改过

`Dungeons-Win64-Shipping.exe`(UE 5.6.1)里 **`SuperStruct` 与 `ChildProperties`
这两个反射名字符串都不存在**(只有 `Children`), 它的 `UStruct`/`FField` 布局与
公开的 UE5 布局不一致。

后果: `Reflection.cpp` 里那套走 `ChildProperties -> Next -> NamePrivate` 的属性
链遍历**在这个构建上不可用**。已尝试三种判据(值在模块映像外 / 能解出合法
FName / 沿 Next 连续自洽), 全部失败, 记录在 `dumpStructPointerSlots` 的注释里。
`fieldNext(0x20)` 与 `fieldName(0x28)` 至少有一个是错的。

### 属性偏移的正确来源: 代码生成属性表

**不要靠猜布局来拿属性偏移。** UE 的 UHT 会为每个类的属性生成一份参数表
(`FPropertyParams`), 其中 `STRUCT_OFFSET(Class, Property)` 是**编译期常量**。
Shipping 构建里这张表在 `.rdata`, 每项形如:

```
[flags][ArrayDim][Offset][NameUTF8 指针]
```

所以偏移可以直接从二进制里读出来。`UCharacterMovementComponent` 的属性表在
`0x14973xxxx` 区段, 据此得到的偏移见
`feature/movement/MovementAccess.cpp` 的 `knownMovementOffsets`。

验证方式(已做过, 建议保持这个习惯):

1. 属性表读出的偏移
2. 指令特征: 全 `.text` 里 `movss xmm,[reg+offset]` 的出现次数是否合理
   (`+0x234` 只出现 1 次, 与"MaxWalkSpeed 只在 GetMaxSpeed 一处被读"相符)
3. 相邻性: UE 中连续声明的几个 float 应当被同一函数一起读
   (`{+0x1a0, +0x1a4, +0x1a8}` = MaxStepHeight / JumpZVelocity /
   JumpOffJumpZFactor 被同一函数读取)
4. 运行时读出**合理的游戏数值** —— UE 默认 `MaxWalkSpeed` 是 600、
   `JumpZVelocity` 是 420; 实测读到 100 / 110, 说明读的确实是本游戏调过的值

### 已知偏移

`UCharacterMovementComponent`(玩家用的子类继承自它, 基类字段偏移不变):

| 属性 | 偏移 | 可信度 |
|---|---|---|
| `MaxStepHeight` | `+0x1a0` | 已验证 |
| `JumpZVelocity` | `+0x1a4` | 已验证(读出 110) |
| `JumpOffJumpZFactor` | `+0x1a8` | 已验证 |
| `WalkableFloorAngle` | `+0x1ac` | 属性表 |
| `GravityScale` | `+0x1c0` | ⚠️ **存疑** — 读出 0, 且其属性表项在另一地址区段, 可能属于别的类 |
| `GravityDirection` | `+0x1d0` | 属性表 |
| `MaxWalkSpeed` | `+0x234` | 已验证(读出 100) |
| `MaxWalkSpeedCrouched` | `+0x278` | 属性表 |
| `MaxSwimSpeed` | `+0x27c` | 属性表 |
| `MaxFlySpeed` | `+0x280` | 属性表 |
| `MaxAcceleration` | `+0x288` | 已验证(读出 600) |
| `BrakingDecelerationWalking` | `+0x29c` | 属性表 |
| `AirControl` | `+0x2ac` | 属性表(俯视角游戏为 0, 合理) |
| `Mass` | `+0x2fc` | 属性表 |

`ULevel::Actors` = **`+0xa0`**（实测: `{data, num=972, max=1364}`, 首元素
`WorldSettings` —— 它在 UE 里恒定位于索引 0）。注意不是常见的 `0x40` 或 `0x98`。

### 玩家角色的类名

实测是 **`BP_AlexCharacter_C`**, 不是 `PlayerCharacter`/`PlayerPawn`(这两个类名
在本构建里根本不存在)。判定的完整规则见
`feature/movement/MovementAccess.cpp` 的 `playerClassRules`, 要点:

- 命中: `AlexCharacter` / `BP_SteveCharacter` / `DungeonsCharacter` /
  `PlayerCharacter` / `PlayerPawn`(按优先级)
- 排除: `Controller` / `PlayerState` / `HUD` / `Mock` / `Mob` / `Wolf`
- **必须按优先级挑, 不能取第一个命中的** —— 实测 actor 列表里
  `BP_GameplayPlayerController_C` 排在 `BP_AlexCharacter_C` 之前

### 崩溃分析

### 先从转储定性, 不要靠"崩的时候我们在干什么"推断

UE 崩溃目录里同时有 `CrashContext.runtime-xml` 与 `UEMinidump.dmp`。
`build/minsd.py` 这个一次性小工具能从 minidump 里取出三样决定性信息:

```
exception code     <- 0xC0000005 访问违例 / 0x8000 GPU 崩溃
exception address  <- 出错指令的地址(换算成模块 RVA 就能回 IDA 查函数)
fault address      <- 被访问的那个地址(0x0 就是空指针解引用)
faulting RIP       <- 注意: 无调试器接管时它通常落在 ntdll 的异常分发处
                      (ntdll+0x161c44), 不要把它当成崩溃现场
```

用法: `python build/minsd.py <UEMinidump.dmp>`

把 `exception address` 减去模块基址得到 RVA, 再加 IDA 的 image base(0x140000000)
即可定位到函数。**先做这一步再谈归因。**

崩溃报告位置:

```
%LOCALAPPDATA%\Dungeons2\Saved\Crashes\UECC-Windows-*\
    CrashContext.runtime-xml     异常码/调用栈(文本)
    UEMinidump.dmp               给 minsd.py 用的转储(有时是 0 字节)
    D3D12.*.nv-gpudmp            GPU 崩溃转储(仅 GPU 崩溃时有)
```

注入体的文件日志在 `%TEMP%\epsilonPayload_<pid>_<模块基址>.log`。
**命令输出也会镜像到这个文件**, 所以注入器退出后结果仍可读 ——
排查时不要只看注入器的 stdout, 它可能先于响应到达就退出了。

### 已定性的崩溃(供将来对照)

| 时间 | 类型 | 结论 |
|---|---|---|
| 14:43:46 / 14:45:04 | `0x8000` GPU crash | **我们造成的**: 覆盖层未处理交换链尺寸变化。已修(见上文"交换链重建") |
| 15:45:38 / 16:06:48 | `0xC0000005` 读 | 空指针解引用(`fault address = 0x0`), 出错指令在**游戏自身代码**里 (RVA 0x12A0536 / 0x55DA583), 两次位置不同。**未定性** |
| 15:09:59 | 无转储(0 字节) | 无法分析 |

### 关于 Present 钩子的崩溃归因 —— 已修正

早先我曾把两次崩溃归因于"自动安装 Present 钩子", 依据只是日志顺序
(崩溃前最后一行是自动安装提示)。**转储分析不支持这个结论**:

* 14:43 / 14:45 两次是 GPU crash, 根因是覆盖层的交换链尺寸问题, 与钩子安装方式无关
* 16:06:48 那次, 游戏在**预定的钩子尝试之前 4 秒**就死了 —— 日志里
  "开始尝试"那一行根本没出现, 所以不可能是钩子干的
* 15:09 那次没有转储可查

因此"自动安装必崩、命令安装稳定"这个规律**证据不足**, 不要把它当成既定事实。
真正已证实的是: **覆盖层的交换链尺寸处理曾经导致 GPU crash, 已修复。**
(仍需警惕的是: 自动安装与命令安装的差别只在时机与线程, 若将来又出现
钩子相关崩溃, 应当用转储去定性, 而不是沿用这条旧结论。)

### 功能模块的可靠性

`Speed`/`Jump` 两个模块在真机上**尚未成功生效**:

* `MaxWalkSpeed` 写入后被游戏每帧重算覆盖 —— 实测写入 130 后连续 8 次采样
  全是 100。每帧补写压不住。
* 反汇编发现一个每帧同步函数把 +0x1018 复制进 +0x234, 但实测 +0x1018 恒为 0,
  写入它也不会影响 MaxWalkSpeed —— 该假设**已被证伪**, 真实数据流仍未找到。
* 16:06 与 15:45 两次空指针崩溃都发生在 Speed 模块持续写该字段期间。
  **因果关系未证实, 但不能排除**, 所以默认配置里两个模块都保持关闭
  (`~/.epsilon/ext/dungeons2/configs/*/modules/*.json` 的 `enabled: false`)。

在找到游戏真正读取速度的路径之前, 不要再默认启用它们。
更可能正确的方向是**挂钩**游戏自己的读取/计算路径, 而不是反复写被重算的字段。

---

## 调试方式

注入器需要**管理员权限**(游戏进程完整性级别更高, 否则 `OpenProcess` 返回
`Access denied (5)`)。

```powershell
.\build\release\bin\epsilonInjector.exe -l --filter Dungeons      # 找进程
.\build\release\bin\epsilonInjector.exe --pid <PID> -v -i         # 注入并交互
```

常用命令(进 `epsilon>` 提示符后):

| 命令 | 作用 |
|---|---|
| `status` | 引擎定位总览 |
| `player` | 列出关卡里的玩家候选与模块会选中的那个 |
| `mv` | 读玩家移动组件上几个关键 float 的当前值 |
| `hook` | 装 Present 钩子(覆盖层需要) |
| `props <类名>` | 属性链(⚠️ 本构建反射不可用, 通常为空) |
| `scanlevel <地址>` | 在对象上扫 TArray 形态的字段 |
| `findprop <类> <属性>` | 绕开 FField 链反查属性偏移 |
| `ptr <地址> [n]` | 按指针逐个解释一段内存 |
| `mem <rva\|va=>` | 原始内存 dump |

## 验证习惯

- 改完**跑一次完整构建**(全部 7 个 target), `/W4` 下保持 0 error 0 warning
- 涉及内存布局的改动, 要有**实测依据**, 不要只靠推理; 并在注释里写清
  "怎么验证的", 便于游戏更新后复现
- 一次性诊断程序放在 `build/` 下手工编译, **不要加进 CMakeLists**
  (`build/` 已在 .gitignore 中)
