# Epsilon For Dungeons II — 文档索引

这里收录注入器 / 注入体的实现要点、设计取舍、实测结论与踩坑记录。

**这些文档不是代码的复述。** 读者是将来接手这个仓库的人或 AI，重点是
**为什么这样做、踩过什么坑、怎么验证** —— 代码本身已经足够直白，读代码解决不了的是
「为什么不能那么写」。

代码里的注释只保留**契约级**说明（这个函数要求什么、保证什么、被谁调用），
成因、历史与实测过程一律放在这里。

---

## 目录

### 注入体运行时

| 文档 | 一句话简介 |
|---|---|
| [payload/lifecycle.md](payload/lifecycle.md) | 从 `DllMain` 到 `shutdownRuntime` 的完整生命周期：为什么 `DllMain` 只建线程、单实例守卫的命名规则与不能放宽的理由、输出通道决策、引擎定位重试、功能线程节流、以及为什么永不卸载。 |
| [payload/output.md](payload/output.md) | 输出与诊断通道：`Sink` 语义、`emit`/`emitLine`/`emitNow` 与响应缓冲、日志前缀、`%TEMP%\epsilonPayload_<pid>_<模块基址>.log` 的命名与共享模式坑、命令输出镜像落盘、`trace()` 的 `[未送出]` 标记。 |
| [payload/hooks.md](payload/hooks.md) | 渲染挂钩：临时 D3D12 交换链 `vtable[8]` 定位 `Present`、`ExecuteCommandLists` 捕获游戏队列的理由、自动安装与 10 秒延迟、`EPSILON_NO_AUTO_HOOK` 开关；含 **「Present 钩子崩溃归因已被转储分析推翻」的历史记录**。 |
| [payload/overlay.md](payload/overlay.md) | D3D12 + ImGui 覆盖层：每帧 7 步 GPU 顺序、交换链尺寸检测与重建（两次 GPU crash 的根因）、必须用游戏队列渲染、SRV 分配器、显式视口与裁剪、窗口过程输入分流。 |

### 功能模块

| 文档 | 一句话简介 |
|---|---|
| [features/module-framework.md](features/module-framework.md) | 模块 / 设置 / 配置框架：`Module` 基类与 Java 版的三处刻意偏离、`Category`、`BindMode` 与按键分发三步、dirty 增量保存、`Setting` 类型体系、`ConfigManager` 文件布局与容错、`setDefaultXxx` 与 `reset()` 的连带陷阱。 |
| [features/ui.md](features/ui.md) | 游戏内 `Modules` 面板：绘制结构、各类型控件、改键捕获（含悬垂指针坑），以及**面板文案必须纯 ASCII 英文**的原因与 `????????????(???, ??? Mock/Mob)` 踩坑记录。 |
| [features/speed-jump.md](features/speed-jump.md) | `Speed` / `Jump` 两个模块：三个可选目标与走过的弯路、为什么每帧写而不是写一次、为什么默认关闭。 |
| [features/emerald.md](features/emerald.md) | `Emerald` 模块：绿宝石是 GAS 属性不是整数成员、为什么放大「增量」而不是维持「基线 × 倍率」、以及为什么它比 `Speed` 简单。 |

### 偏移表

| 文档 | 一句话简介 |
|---|---|
| [offsets/README.md](offsets/README.md) | **偏移总览**：两种来源（代码生成属性表 / 运行时反射）、四步验证法、可信度标记、游戏更新后的修复路径。建议从这里读起。 |
| [offsets/engine-globals.md](offsets/engine-globals.md) | `GObjects` / `GNames` / `GEngine` / `GWorld` 的 RVA 与各自的校验判据。 |
| [offsets/object-array.md](offsets/object-array.md) | `FUObjectArray` / `FUObjectItem` / `UObject` 的布局与取值链；含「`TUObjectArray` 内嵌」这个关键点。 |
| [offsets/name-pool.md](offsets/name-pool.md) | `FNamePool` / `FNameEntry` 布局；两个致命坑：**不带 NUL 终止符**、**索引里的字节偏移右移了一位**。 |
| [offsets/reflection.md](offsets/reflection.md) | `UStruct` / `UField` / `FField` / `FProperty` 布局、自愈扫描判据、各常量的可信度。 |
| [offsets/world.md](offsets/world.md) | `UWorld::PersistentLevel`、`ULevel::Actors`、`TArray` 头，以及未验证的候选偏移。 |
| [offsets/character-movement.md](offsets/character-movement.md) | `UCharacterMovementComponent` 的 13 个 float 字段、类继承链、为什么 `GravityScale` 存疑。 |
| [offsets/movement-attributes.md](offsets/movement-attributes.md) | GAS 属性集 `ATR_Movement` 的 12 个属性、**表偏移整体差 8 字节**的判定过程、写入实验记录。 |
| [offsets/currency.md](offsets/currency.md) | GAS 属性集 `ATR_Currency` 的 7 个货币属性、**属性表里 `Offset_Internal` 在记录 `+0x34`** 的判定过程（与既有 `ATR_Movement` 读法冲突，已标注）。 |
| [offsets/d3d12-vtable.md](offsets/d3d12-vtable.md) | `Present` = `vtable[8]` 与 `ExecuteCommandLists` = `vtable[10]` 的索引推导与自检。 |

### 逆向分析

| 文档 | 一句话简介 |
|---|---|
| [reverse/ue-runtime.md](reverse/ue-runtime.md) | 运行时反射定位：四个全局的定位策略与自洽校验、`GObjects` 布局要点、`NamePool` 的两个致命坑及其实测样本。 |
| [reverse/reflection-limits.md](reverse/reflection-limits.md) | 本构建的反射为什么不可用：缺失的反射名、三种判据全部失败的记录、自愈扫描的判据演进史（含两次踩坑）、`findPropertyDirect` 的绕行思路。 |
| [reverse/movement-attributes.md](reverse/movement-attributes.md) | 移动速度的真正来源：为什么不能写 `MaxWalkSpeed`、`ATR_Movement` 的发现过程、属性表偏移差 8 字节、仍未解决的问题。 |
| [reverse/player-detection.md](reverse/player-detection.md) | 玩家角色判定：实测类名、为什么必须按优先级挑而不是取第一个命中、规则表的唯一出处。 |
| [reverse/property-verification.md](reverse/property-verification.md) | 属性偏移的四步验证法，附两个实例与完整的可信度分级。 |
| [reverse/toolchain.md](reverse/toolchain.md) | 逆向工作流：命令表、minidump 三件套、RVA ↔ IDA image base 换算、崩溃分析纪律、已定性崩溃对照表。 |

### 排查

| 文档 | 一句话简介 |
|---|---|
| [dev/diagnostics.md](dev/diagnostics.md) | **排查手册**：日志位置、注入器需要管理员权限、症状 → 原因对照表（注入 / 定位 / 模块 / 渲染）、崩溃转储分析流程。 |

---

## 阅读顺序建议

1. **先读 [payload/lifecycle.md](payload/lifecycle.md)** —— 建立「注入体是怎么活起来的」整体图景。
2. 要动**偏移或字段读写**前，先读 [offsets/README.md](offsets/README.md)：
   偏移必须实测，不能推理；改哪儿、怎么验证都写在里面。
3. 排查具体问题时按 [dev/diagnostics.md](dev/diagnostics.md) 的症状表跳转。
4. 要动**配置 / 模块 / UI** 代码前，先读
   [features/module-framework.md](features/module-framework.md) 的踩坑段
   （`setDefaultXxx`、坏值容错、配置持久化污染）。
5. 要动**渲染 / 覆盖层**代码前，先读 [payload/overlay.md](payload/overlay.md) 的
   交换链重建小节 —— 那里是已知能造成 GPU crash 的地方，而且症状极难反推。

---

## 代码结构对照

```
src/payload/game/          游戏内存模型
  Offsets.h                ★ 全部原始偏移的唯一出处
  Field.h/.cpp             base+offset 读写原语、TArray 头
  ue/                      UE 运行时结构（NamePool / ObjectArray / Reflection / Engine / World）
  dungeons2/               本游戏特有模型（Movement / Player / MovementResolver / Currency）

src/payload/feature/       功能模块框架
  module/                  模块基类与管理器；impl/ 放具体模块
  settings/                设置类型体系
  config/                  配置持久化
  ui/                      模块面板

src/payload/               Payload / Runtime / CommandServer / Hooks / Overlay / PipeClient
src/common/                注入器与注入体共用: PE 解析 / 进程工具 / 管道 / 文本
src/injector/              注入器 EXE
tests/                     自测靶子与一次性诊断程序
analysis/                  逆向分析工作区(Python, 独立于 C++)
```

---

## 关于「未证实」标注

注释里明确了验证方式与结论的条目，本文档直接陈述并注明验证方式；凡是推测、
或者注释与代码不一致的地方，都显式标注 **（未证实）** 或 **（注释与实现不符）**。

遇到标注为「陈旧注释」的地方，说明注释描述的是历史形态 —— **以代码为准**。
