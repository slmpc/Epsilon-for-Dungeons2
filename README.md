# Epsilon For Dungeons II

面向 **Minecraft Dungeons II**（UE 5.6.1）的**热注入 + 运行时逆向数据采集**框架。

注入器把一个 DLL 送进游戏的 Shipping 进程；注入体在里面定位 UE 的运行时反射
（`GObjects` / `GNames`），把需要的字段偏移重建或读出，并在**游戏自己的 D3D12 命令队列**上
绘制一个 ImGui 覆盖层。所有内存读取都经过 SEH 保护的 `safeRead` —— 目标进程里任何指针
都可能是垃圾值。

> 仅用于个人研究与学习。许可见 [LICENSE](LICENSE)（All Rights Reserved）。

---

## 组成

| 产物 | 说明 |
|---|---|
| `epsilonInjector.exe` | 控制台前端：定位进程 → `CreateRemoteThread` 注入 → 命名管道命令式 UI |
| `epsilonPayload.dll` | 注入体：引擎定位 + 命令服务 + MinHook 帧钩子 + ImGui 覆盖层 |

依赖：MinHook / ImGui / nlohmann-json（vcpkg manifest），C++23 + MSVC + Ninja。

---

## 能力现状

| 能力 | 状态 |
|---|---|
| 注入链路、命名管道命令服务 | ✅ 真机验证 |
| 引擎定位（`GObjects` / `GNames` / `GEngine` / `GWorld`） | ✅ |
| 对象表 / 名称池 / 关卡 Actor 遍历 | ✅ |
| D3D12 + ImGui 覆盖层面板（`Insert` 开关） | ✅ 曾连续渲染 11 万帧无崩溃 |
| 属性链反射（`props`） | ❌ 本构建的 `UStruct` 布局被改过，链式遍历不可用 |
| 加速（`Speed`） | ✅ 已实测生效 |
| 大跳（`Jump`） | ⚠️ 路径已实现，尚未在游戏内确认 |
| 卸载注入体 | ❌ 刻意不提供 —— 见下 |

### 加速为什么改的是属性集

**写 `UCharacterMovementComponent::MaxWalkSpeed` 是无效的** —— 实测它每帧被游戏重算
（写入 130 后连续 8 次采样全是 100）。真正的移动参数在 GAS 属性集 **`ATR_Movement`** 里，
写入持久且跨注入保留。完整实测过程见
[docs/offsets/movement-attributes.md](docs/offsets/movement-attributes.md)。

### 为什么不提供卸载

注入体一旦挂上钩子、起了线程，半途 `FreeLibrary` 会留下悬空回调，目标必崩。
要清除注入体就重启游戏。这是刻意的取舍，不是未完成项。

---

## 构建

```powershell
.\scripts\build.ps1                              # release
.\scripts\build.ps1 -Preset debug
.\scripts\build.ps1 -Target epsilonTestTarget    # 单个 target
```

要求：Visual Studio（含 C++ 工作负载）、CMake ≥ 3.25、Ninja、vcpkg。
产物在 `build/<preset>/bin/`。

⚠️ 脚本会把 `VCPKG_ROOT` 强制指向 `D:\Programs\vcpkg`（`vcvars64.bat` 会把它改成 VS 自带的
vcpkg，而本项目 baseline 与已装依赖在用户自己的 vcpkg 里）。**不要绕过脚本直接跑 cmake**，
否则 CMake 缓存会写进错误的 toolchain 路径，之后所有配置都失败。若已踩到：删掉
`build/<preset>` 重新配置。

---

## 使用

注入器需要**管理员权限**（游戏进程完整性级别更高，否则 `OpenProcess` 返回 `Access denied (5)`）。

```powershell
.\build\release\bin\epsilonInjector.exe                            # 一把梭: 找游戏 → 注入 → 装 Present 钩子 → 交互
.\build\release\bin\epsilonInjector.exe -l --filter Dungeons       # 只看进程
.\build\release\bin\epsilonInjector.exe --pid <PID> -v             # 指定 PID, 仍然是注入+挂钩+交互
.\build\release\bin\epsilonInjector.exe -x status --no-interactive # 只下发命令后退出
```

**不带参数就是完整流程**：按映像名找运行中的 Dungeons2（找不到会退回模糊匹配
`dungeons-win64`，并最多等 `--find-wait` 毫秒）→ `CreateRemoteThread` 注入 → 等
注入体 ready → 下发 `hook` 装 Present 钩子（`--no-hook` 可跳过）→ 进交互模式
（`--exec` 只在给命令时批量模式，`-i` 可强制交互）。

进入 `epsilon>` 提示符后（`injector-help` 看注入器自己的命令，`ps` / `clear` / `exit`）：

```
status     引擎定位总览            player     玩家候选与模块会选中的那个
mv         移动组件关键 float       attrmv     属性集 ATR_Movement 与属性块
actors     关卡 Actor 列表          findprop   绕开 FField 链反查属性偏移
hook       安装 Present 钩子(覆盖层)  floats/poke 原始内存读写(排查用)
```

完整命令表见 [docs/reverse/toolchain.md](docs/reverse/toolchain.md)。
覆盖层可用 `Insert` 开关。

---

## 文档

**入口：[docs/README.md](docs/README.md)**（24 篇）

| 目录 | 内容 |
|---|---|
| [docs/payload](docs/payload/) | 注入体生命周期、输出通道、渲染挂钩、覆盖层 |
| [docs/features](docs/features/) | 模块 / 设置 / 配置框架、面板、Speed & Jump |
| [docs/offsets](docs/offsets/) | **偏移表**：来源、四步验证法、可信度分级、更新后怎么修 |
| [docs/reverse](docs/reverse/) | 逆向笔记：运行时定位、反射为何不可用、移动属性、玩家判定、工具链 |
| [docs/dev](docs/dev/) | 排查手册（症状 → 原因对照） |

给 AI 代理与协作者的规则见 [AGENTS.md](AGENTS.md)。

---

## 许可

**All Rights Reserved（保留所有权利）** —— 见 [LICENSE](LICENSE)。

未经版权所有者书面许可，不得复制、再发布、修改、演绎或用于商业用途；
允许通过 fork / 引用链接查看与讨论。

第三方依赖遵循各自上游许可，不在本项目许可范围内。

Minecraft Dungeons 及相关名称与资产为其各自权利人所有；本项目与
Mojang Studios / Microsoft 无任何关联，亦未获授权或背书，
**不包含**任何游戏本体文件或游戏资产。
