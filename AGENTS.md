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

### Present 钩子: 默认关闭, 用 `hook` 命令装

**不要在启动路径或功能线程上自动调用 `findPresent`。**

实测两次崩溃都出在自动安装这条路径上(目标进程以 `EXCEPTION_ACCESS_VIOLATION
writing` 挂死), 而**同一个 `findPresent` 由 `hook` 命令触发时稳定可用**
(连续渲染 33000+ 帧无异常)。

差别在调用线程: 自动安装在功能线程上跑, `hook` 命令在 pipe 读线程上跑。
`findPresent` 会创建临时 D3D12 设备与交换链, 这类调用隐含要求特定的 COM/D3D
线程状态。详见 `autoHookAllowed()` 的注释。

如果要恢复自动安装, 必须先解决线程问题, 而不是把默认值改回去。

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

---

## 逆向相关的重要背景

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

### 崩溃报告位置

UE 的崩溃报告(含 GPU 崩溃转储)在:

```
%LOCALAPPDATA%\Dungeons2\Saved\Crashes\UECC-Windows-*\CrashContext.runtime-xml
                                          D3D12.*.nv-gpudmp
```

注入体的文件日志在 `%TEMP%\epsilonPayload_<pid>_<模块基址>.log`。
命令输出也会镜像到这个文件, 所以**注入器退出后结果仍可读**。

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
