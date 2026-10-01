# AGENTS.md — Epsilon For Dungeons II

给在此仓库工作的 AI 代理与协作者。**改动代码前请先读完「硬性约定」。**

本文件只放**规则与入口**。设计成因、实测结论、踩坑历史一律在 [`docs/`](docs/README.md) 里，
本文件不再重复，只链接过去。

---

## 硬性约定

### 1. 游戏内 UI 一律用纯 ASCII 英文 ★

**任何会被绘制在游戏进程内 ImGui 面板上的文案，必须是纯 ASCII 英文。**

原因（实测踩过，不是风格偏好）：覆盖层用的是 ImGui **内置的 ASCII 位图字体**，没有中文字形。
写中文进去不会报错，而是渲染成一串 `?` —— 信息完全丢失。曾出现
`????????????(???, ??? Mock/Mob)`，排查者无法判断那是「没有匹配到玩家」的诊断信息。

| 受约束（会进面板） | 例 |
|---|---|
| `Module` 的 name / description | `addDouble("Multiplier", …)` 的描述参数 |
| `Setting` 的 name / description / `displayValue()` | 控件标签、tooltip、键名 |
| `MovementResolver::lastError()` / `offsetSource()` | 面板顶部的解析状态 |
| `Module::info()` | 模块标题右侧的运行状态行 |
| `Overlay.cpp` / `FeaturePanel.cpp` 里所有 `ImGui::Text*` 字面量 | 面板固定文案 |
| `CommandServer` 里 `player` 命令的输出 | 可能对着面板看 |

**不受约束**：日志与命令输出。`trace` / `logInfo` / `logWarn` / `emitLine` 走文件与命名管道，
中文完全正常且对排查更有用 —— **这些地方继续用中文**。

判断方法：问自己「这段文字会不会出现在 ImGui 面板上」。不确定时按会处理。

### 2. 命名规范

- 文件：大驼峰（`PeImage.h` / `NamePool.cpp`）
- 类型：大驼峰；函数与变量：小驼峰
- 数据成员：**保留尾部下划线**（`className_`）
- 常量：小驼峰，不用 `k` 前缀（`maxPayload`，不是 `kMaxPayload`）
- 文件级全局：`g` 前缀（`gPresentUrl`）
- 命名空间：`epsilon` / `epsilon::game` / `epsilon::game::ue` / `epsilon::game::dungeons2` /
  `epsilon::game::offsets` / `epsilon::payload` / `epsilon::feature`

改动命名后跑 `.\scripts\auditNaming.ps1` 复核残留。

### 3. 第三方名称不要改

标准库、Win32 SDK、D3D12/DXGI、ImGui、nlohmann/json、MinHook、UE 引擎自己的字段名
（`Offset_Internal`、`OnRep_*`…）都保持原拼写。`auditNaming.ps1` 的 allow-list 里已列出常见项；
遇到新的第三方名字时**扩充 allow-list，而不是去改代码**。

### 4. 注释只写契约，成因写进 docs ★

代码里的注释只保留**契约级**信息：

- 文件头：2~4 行 —— 文件名 + 一句话职责 + 指向 `docs/` 的链接
- 函数/类：前置条件、返回什么、失败时怎样、线程安全、所有权、调用顺序要求
- 不明显的「必须这么做否则会崩」的一行提醒

**不要**在代码里写：历史记录、实测数据、踩坑过程、推导、逐行解释、以及「本函数用于…」这类同义反复。

成因、实测依据、踩坑历史 → 写进 [`docs/`](docs/README.md) 对应文档。
新增一篇文档时，同时更新 [`docs/README.md`](docs/README.md) 的索引。

### 5. 偏移只有一处出处 ★

**所有裸偏移常量只能写在 [`src/payload/game/Offsets.h`](src/payload/game/Offsets.h)。**

- 其余代码一律通过 [`game/`](src/payload/game/) 下的类型化接口访问字段
  （`readField` / `readPtr` / `readF32` / `MovementComponent` / `MovementAttributeSet` …），
  不允许出现 `addr + 0x234` 这样的字面量。
- 诊断扫描用的窗口（如 `0x20..0x98`、`0x100..0x2000`）也归 `offsets::scan`。
- vtable 索引（`Present` = 8、`ExecuteCommandLists` = 10）同样归 `offsets::d3d12`。

**新偏移必须有实测依据**，并按 [`docs/offsets/README.md`](docs/offsets/README.md) 的
**四步法**验证，然后在 `docs/offsets/` 对应文档里记录来源与可信度（已验证 / 属性表 / 存疑 / 未证实）。

---

## 项目结构

```
src/common/              注入器与注入体共用: PE 解析 / 进程工具 / 管道 / 协议 / 文本
src/injector/            注入器 EXE(控制台前端 + 管道服务端)

src/payload/             注入体 DLL
  game/                  ★ 游戏内存模型
    Offsets.h            全部原始偏移的唯一出处
    Field.h/.cpp         base+offset 读写原语、TArray 头
    ue/                  UE 运行时结构: NamePool / ObjectArray / Reflection / Engine / World
    dungeons2/           本游戏特有模型: Movement / Player / MovementResolver
  feature/               功能模块框架
    module/              模块基类与管理器; impl/ 放具体模块
    settings/            设置类型体系
    config/              配置持久化(~/.epsilon/ext/dungeons2)
    ui/                  ImGui 模块面板
  PipeClient / Runtime / Payload / CommandServer / Hooks / Overlay

tests/                   自测靶子与一次性诊断程序
scripts/                 构建与命名审计
docs/                    ★ 设计成因 / 实测结论 / 偏移表 / 排查手册
analysis/                逆向分析工作区(Python, 独立于 C++ 部分)
```

---

## 构建

```powershell
.\scripts\build.ps1                 # release(全部默认 target)
.\scripts\build.ps1 -Preset debug
.\scripts\build.ps1 -Target epsilonTestTarget
```

要求：C++23 / MSVC / Ninja / vcpkg（manifest 模式）。产物在 `build/<preset>/bin/`。

⚠️ `scripts\build.ps1` 会强制把 `VCPKG_ROOT` 指回 `D:\Programs\vcpkg` —— `vcvars64.bat` 会把它改成
VS 自带的 vcpkg，而本项目的 baseline 与已装依赖在用户自己的 vcpkg 里。
**不要绕过这个脚本直接跑 cmake**，否则 CMake 缓存会被写进错误的 toolchain 路径，之后所有配置都失败
（表现为 `create_directories(...VC\vcpkg\installed): Access is denied`）。
若已踩到：删掉 `build/<preset>` 重新配置。

---

## 关键设计约束

> 每条只有一行，成因与实测记录见对应文档。

| 约束 | 一句话 | 详见 |
|---|---|---|
| 注入体只做注入，不做卸载 | 一旦挂钩子/起线程，半途 `FreeLibrary` 会留下悬空回调，目标必崩。要清除就重启目标进程 | [lifecycle](docs/payload/lifecycle.md) |
| 单实例守卫 | 命名互斥体保证每进程只跑一个实例；副作用是**驻留期间无法注入新构建**。换个 DLL 文件名即可并存 | [lifecycle](docs/payload/lifecycle.md) |
| Present 钩子默认开启，延迟 10 秒 | `findPresent` 会建临时 D3D12 交换链，唯一能识别的差异是**时机**。`EPSILON_NO_AUTO_HOOK=1` 关闭（刻意放在游戏之外） | [hooks](docs/payload/hooks.md) |
| 覆盖层交换链重建 | 必须在取后缓冲**之前**比对尺寸/后缓冲数量。忽略它会导致 GPU 侧挂死（UE 弹 `GPUCrash`）而 CPU 侧一切正常 | [overlay](docs/payload/overlay.md) |
| 覆盖层必须用游戏的命令队列渲染 | flip 模型交换链与创建它的队列绑定；在别的队列上渲染会「命令成功、围栏完成、画面不动」 | [overlay](docs/payload/overlay.md) |
| 内存访问必须走 `safeRead` / `safeWrite` | 目标进程里任何指针都可能是垃圾值。读走 `safeRead`（SEH 保护），写走 `safeWrite`（先直写，失败才放宽页保护） | [diagnostics](docs/dev/diagnostics.md) |
| 配置容错 | 配置是用户可手改的明文，坏值退化为默认值，**不抛异常** —— 异常从注入体的配置加载路径穿出去会把游戏带崩 | [module-framework](docs/features/module-framework.md) |
| 出厂默认必须用 `setDefaultXxx` | 配置加载会调用 `reset()`，它把各项恢复成 `defaultXxx_`。用 `setXxx` 会被抹掉，且**错误默认值会被持久化** | [module-framework](docs/features/module-framework.md) |

---

## 逆向相关的重要背景

> 完整实测过程与未决问题见 [`docs/reverse/`](docs/reverse/) 与 [`docs/offsets/`](docs/offsets/)。

### 速度/移动的真正来源是 GAS 属性集 `ATR_Movement` ★

**不要试图写 `UCharacterMovementComponent::MaxWalkSpeed`** —— 实测它每帧被游戏重算
（写入 130 后连续 8 次采样全是 100；而同样方式写 `MaxAcceleration` 却完全保持，
说明写路径本身没问题）。

真正的移动参数在 `ATR_Movement` 里，写入**持久**且跨注入保留。
偏移与「表偏移整体差 8 字节」的判定过程见
[`docs/offsets/movement-attributes.md`](docs/offsets/movement-attributes.md)。

**仍未解决**：`MovementSpeedMultiplier` 的当前值是 0，写入后无可感知效果，
所以还不能确认它是驱动移动速度的那个属性。下一步应当在 `ATR_Movement` 里找**值呈单位量级
（1.0）**或随移动变化的属性，用 `floats` 把整个属性块打出来逐个对照。

### 这个构建的 UE5 反射布局被改过

`SuperStruct` 与 `ChildProperties` 两个反射名字符串**都不存在**（只有 `Children`），
`UStruct` / `FField` 布局与公开的 UE5 布局不一致 → 属性链遍历**在本构建上不可用**。

`fieldNext(0x20)` 与 `fieldName(0x28)` 至少有一个是错的（未决）。
`Reflection` 因此带一套自愈扫描，判据与两次踩坑见
[`docs/reverse/reflection-limits.md`](docs/reverse/reflection-limits.md)。

### 属性偏移的正确来源：代码生成属性表

**不要靠猜布局来拿属性偏移。** UE 的 UHT 会为每个类生成参数表，其中
`STRUCT_OFFSET(Class, Property)` 是**编译期常量**，Shipping 构建里就在 `.rdata` 里可直接读。

⚠️ 两张表的项布局不同：`UCharacterMovementComponent` 表的偏移在 `+0x2c`，
`ATR_Movement` 表在 `+0x24`。见 [`docs/offsets/README.md`](docs/offsets/README.md)。

### 玩家角色的类名

实测是 **`BP_AlexCharacter_C`**，不是 `PlayerCharacter` / `PlayerPawn`。

- 命中：`AlexCharacter` / `BP_SteveCharacter` / `DungeonsCharacter` / `PlayerCharacter` / `PlayerPawn`（按优先级）
- 排除：`Controller` / `PlayerState` / `HUD` / `Mock` / `Mob` / `Wolf`
- **必须按优先级挑，不能取第一个命中的** —— 实测 actor 列表里 `BP_GameplayPlayerController_C`
  排在 `BP_AlexCharacter_C` 之前

规则的**唯一出处**是 [`src/payload/game/dungeons2/Player.h`](src/payload/game/dungeons2/Player.h)；
命令层与模块都必须调用它，不允许各自复刻一份。

### 崩溃分析：先从转储定性

**不要靠「崩的时候我们在干什么」推断归因。** 用 `build/minsd.py <UEMinidump.dmp>` 取出
exception code / exception address / fault address，换算 RVA 后回 IDA 定位。

⚠️ 无调试器接管时 **faulting RIP 通常落在 ntdll 的异常分发处**（`ntdll+0x161c44`），
不要把它当成崩溃现场。

已定性的崩溃对照表与「Present 钩子归因已被推翻」的记录见
[`docs/dev/diagnostics.md`](docs/dev/diagnostics.md)。

---

## 调试方式

注入器需要**管理员权限**（游戏进程完整性级别更高，否则 `OpenProcess` 返回 `Access denied (5)`）。

```powershell
.\build\release\bin\epsilonInjector.exe -l --filter Dungeons      # 找进程
.\build\release\bin\epsilonInjector.exe --pid <PID> -v -i         # 注入并交互
```

| 命令 | 作用 |
|---|---|
| `status` | 引擎定位总览 |
| `player` | 列出关卡里的玩家候选与模块会选中的那个 |
| `mv` | 读玩家移动组件上几个关键 float 的当前值 |
| `attrmv` | 找出玩家的 `ATR_Movement` 并打印属性块 |
| `hook` | 装 Present 钩子（覆盖层需要） |
| `world` / `actors` | 当前 `UWorld` / `ULevel` / Actor 列表 |
| `objects` / `find` / `class` | 对象表查询 |
| `props <类名>` | 属性链（⚠️ 本构建反射不可用，通常为空） |
| `scanlevel <地址>` | 在对象上扫 TArray 形态的字段 |
| `findprop <类> <属性>` | 绕开 FField 链反查属性偏移 |
| `probeff <地址>` | 找 `FField::NamePrivate` 的真实偏移 |
| `ptr <地址> [n]` | 按指针逐个解释一段内存 |
| `floats <地址> [n]` | 按 float 解读一段内存（个数是第二个位置参数） |
| `poke <地址> <值>` | 往任意地址写一个 float，带回读与 8 次采样 |
| `mvset <字段> <值>` | 写一个移动组件字段（支持裸偏移 `+0x230`） |
| `mem <rva\|va=>` | 原始内存 dump |

完整命令说明与排查流程见 [`docs/reverse/toolchain.md`](docs/reverse/toolchain.md)
与 [`docs/dev/diagnostics.md`](docs/dev/diagnostics.md)。

---

## 验证习惯

- 改完**跑一次完整构建**（`.\scripts\build.ps1` + 四个 `-Target` 测试靶子），
  `/W4` 下保持 0 error 0 warning
- 涉及内存布局的改动，要有**实测依据**，不要只靠推理；并在 `docs/offsets/` 里写清
  「怎么验证的」，便于游戏更新后复现
- 一次性诊断程序放在 `build/` 下手工编译，**不要加进 CMakeLists**（`build/` 已在 `.gitignore` 中）
- 注入体驻留期间**无法重新注入新构建** —— 迭代时换个 DLL 文件名，或重启游戏

---

## 许可

本项目以 **All Rights Reserved（保留所有权利）** 发布，见 [`LICENSE`](LICENSE)。

- 未经版权所有者书面许可，**不得**复制、再发布、修改、演绎或用于商业用途
- 允许通过 fork / 引用链接查看与讨论
- 第三方依赖（MinHook / ImGui / nlohmann-json，见 `vcpkg.json`）遵循各自上游许可，**不在**本项目许可范围内
- 本仓库**不含**任何游戏本体文件或游戏资产；与 Mojang Studios / Microsoft 无关联

