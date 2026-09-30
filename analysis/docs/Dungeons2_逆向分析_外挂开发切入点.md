# Minecraft Dungeons II — 目标画像与外挂开发切入点分析

> 分析对象：`E:\SteamLibrary\steamapps\common\Minecraft Dungeons II`
> 分析方式：静态文件取证 + 容器格式解析 + 反射元数据挖掘 + 存档格式逆向
> 全部结论来自实测，标注 `[实测]`；推断标注 `[推断]`。

---

## 一、一句话结论

**技术上"能进去"，但"能改出高价值结果"的窗口很窄。**

理由：这是一个 **无内核反作弊** 的 UE5 游戏，所以进程内读写、DLL 注入、资产重打包在工程上全部可行；但它同时是 **服务端权威 + JWT 保护存档** 的在线游戏，所以"改本地数值 → 变强 → 保留"这条链路在关键节点被切断了。真正的战场不在内存里，在**协议层**。

---

## 二、目标画像（实测数据）

### 2.1 可执行体

| 项目 | 值 |
|---|---|
| 主程序 | `Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe` |
| 大小 | 200.81 MB `[实测]` |
| SHA256 | `7C83AFBF0AD34A40B853CDB25A22FFFB605D08E2A1E2D431974D7C7C1EE0BA54` |
| MD5 | `6A5749107680A32E86CD3A36571B601C` |
| 编译时间 | 2026-09-30 UTC `[实测]` |
| 链接器 | MSVC 11.2 (VS2012+ ABI) `[实测]` |
| 签名 | **Valid** — `CN=Microsoft Corporation`，签发 `Microsoft Code Signing PCA 2024` `[实测]` |
| 启动器 | `Dungeons.exe` = `BootstrapPackagedGame`（Epic Games 标准 UE5 bootstrap，仅负责拉起 Shipping 进程） `[实测]` |
| PDB 泄漏 | **无** — 二进制内 0 条 `.pdb` 路径 `[实测]` |

> ⚠️ 注意：二进制**带有合法的 Microsoft 代码签名**。任何对 Shipping.exe 的磁盘补丁（patch-on-disk）都会破坏签名，Steam 的"验证文件完整性"会立刻检测到并回滚。**这条路线直接判死刑** —— 见 §6.1。

### 2.2 引擎版本

| 证据 | 值 |
|---|---|
| IoStore TOC 魔数 | `-==--==--==--==-` |
| IoStore ContainerHeaderVersion | **8** `[实测]` |
| 字符串 | `5.6.` / `UE5`（GVAS 头内） `[实测]` |
| 存档 GVAS 头 | `...UE5\0` 标记 `[实测]` |
| 脚本系统 | `Verse` + `Solaris` 字符串 + `Dungeons\Script\` 下的 `Binds.Cache` / `PrecompiledScript.Cache` `[实测]` |

**结论：Unreal Engine 5.6，且启用了 Verse 脚本系统。** `[实测+推断]`

> ContainerHeaderVersion 8 是 UE 5.5+ 的特征值，配合 `5.6.` 字符串与 `Saved\Config\Windows\`（无 `WindowsNoEditor`）目录布局，锁定 5.6。

### 2.3 容器与资产

```
Dungeons\Content\Paks\
  Dungeons-Windows.ucas   8,688.84 MB   ← 99% 的游戏内容
  Dungeons-Windows.utoc      16.53 MB   ← 目录索引（元数据）
  Dungeons-Windows.pak      242.13 MB   ← 未走 IoStore 的传统 pak
  global.ucas                 3.62 MB
  global.utoc                 0.0009 MB
```

- `.utoc` 头部**字段布局被打乱**（按标准 UE 结构解析会得到 `headerSize=2415919104` 这类垃圾值）`[实测]`。这是**反解析器指纹**的处理，意味着常规的 `retoc` / `FModel` / `UnrealPak` 会直接失败，需要先做格式适配。
- `global.utoc` 仅 890 字节，`Dungeons-Windows.utoc` 有 17 MB —— 说明**绝大多数资产的目录索引都在后者的压缩块里**。

### 2.4 网络与后端栈

| 组件 | 证据 |
|---|---|
| Xbox Live / GDK | `Xbl` ×158、`XboxLive`、`XGame`、`XUser` `[实测]` |
| PlayFab | `PlayFab` / `playfab` 命中 `[实测]` |
| Epic Online Services | `EOSSDK` 字符串 `[实测]` |
| Steamworks | `steam_api64.dll` (Steamv157) `[实测]` |
| HTTP 栈 | `XCurl.dll`、`libHttpClient.GDK.dll`、`msquic.dll`、WinHTTP `[实测]` |
| 语音 | `GameChat2.dll` `[实测]` |
| 网络驱动 | `Engine.ini` → `[GameNetDriver StatelessConnectHandlerComponent]` `[实测]` |

> **关键**：`StatelessConnectHandlerComponent` 存在 = **有握手/连接校验组件**。这是 UE 的轻量级连接层，通常带 `CachedClientID` 之类的状态。做协议层时需要处理它。

### 2.5 反作弊状态

对以下关键词做**全文件单遍字节扫描**（ASCII + UTF-16 双通道），结果：

| 名称 | 命中 |
|---|---|
| EasyAntiCheat / EAC | **0** |
| BattlEye / BEClient / BEService | **0** |
| Denuvo / Denuvo64 | **0** |
| VMProtect / Themida / Arxan | **0** |
| Steam DRM wrapper | **0** |
| `AntiCheat` 通用串 | 1（弱信号，可能是 UE 默认的 `bAntiCheatEnabled` 配置项） `[实测]` |
| `IsDebuggerPresent` | 1（UE 通用调试检测，非反作弊） `[实测]` |

**结论：没有内核态反作弊、没有商业加壳、没有 Denuvo。** 反作弊完全依赖**服务端权威**。这决定了整个技术路线。

### 2.6 存档系统

位置：`%LOCALAPPDATA%\Dungeons2\Saved\SaveGames\` `[实测]`

| 文件 | 大小 | 解析结果 |
|---|---|---|
| `GlobalSaveDataDefault.sav` | 9.14 KB | UE `GVAS` 格式 |
| `entitlements.jwt.bin.sav` | 3.50 KB | `GVAS`，类名 `EntitlementsTokenSaveGame` |
| `auth_dynamic_ent.jwt.bin.sav` | 2.98 KB | `GVAS`，类名 `EntitlementsTokenSaveGame` |
| `OnlineDataTables.sav` | 3.61 KB | `GVAS` |
| `Guid.bin.sav` | 0.27 KB | `GVAS` |

**`auth_dynamic_ent.jwt.bin.sav` 的完整字符串清单** `[实测]`：

```
/Script/Entitlements.EntitlementsTokenSaveGame   ← SaveGame 类名
ProtectedTokenBytes                              ← ArrayProperty / ByteProperty
EntitlementsJWT                                  ← UTF-16 属性名
```

**这是本次分析最重要的发现之一**：

1. 文件名叫 `*.jwt.*` → 里面装的是 JWT 令牌；
2. 属性名是 **`ProtectedTokenBytes`** → 令牌在落盘前被**加密**了（不是明文 JWT，全文件扫描 0 条 `eyJ...` 形状的 base64url 令牌）`[实测]`；
3. 文件头 0x28 之后是一段密集的 16 字节 GUID 序列（自定义版本号），随后才是加密载荷。

**含义**：存档里的鉴权/权益令牌是加密的，且**密钥在进程内**。想伪造存档 → 必须先拿到 `ProtectedTokenBytes` 的解密密钥 → 而这个密钥大概率是通过 GDK/Xbox Live 的凭证派生的。伪造存档的收益/成本比极差。

### 2.7 配置系统（无保护）

`%LOCALAPPDATA%\Dungeons2\Saved\Config\Windows\` 下的 `Engine.ini` / `GameUserSettings.ini` 是**纯文本、无签名、无校验**的 `[实测]`。

已确认内容：
```ini
;METADATA=(Diff=true, UseCommands=true)      ← 注意 UseCommands=true
[GameNetDriver StatelessConnectHandlerComponent]
CachedClientID=2
[Internationalization]
Language=zh-Hans
```

> `;METADATA=(Diff=true, UseCommands=true)` 是 UE 的配置热重载标记。这个文件**可以被任意编辑且游戏会读取**，是所有路线里成本最低的入口 —— 但收益也最低（只能改渲染/画质/输入，见 §6.2）。

### 2.8 PE 导入表 —— 注入路线分析

这是判断"注入可行性"的决定性证据。完整导入 DLL 列表（41 项）`[实测]`：

```
dxgi.dll                          ← ★ 唯一的高价值代理目标
OPENGL32.dll                      ← ★ 备用
UIAutomationCore.DLL              ← ★ 非常规但可用（UI 自动化）
dwmapi.dll  HID.DLL  IMM32.dll  UxTheme.dll
WINHTTP.dll  IPHLPAPI.DLL  SETUPAPI.dll  bcrypt.dll  CRYPT32.dll  ole32.dll  OLEAUT32.dll
wevtapi.dll  dwmapi.dll
MSVCP140.dll  VCRUNTIME140.dll  VCRUNTIME140_1.dll
api-ms-win-*  (约 18 个 API set 转发)
```

**关键观察**：

- ❌ **没有** `dinput8.dll`、`winmm.dll`、`version.dll`、`dsound.dll` —— 经典代理 DLL 三件套**全部缺席**。
- ❌ **没有** `kernel32.dll` / `user32.dll` / `ntdll.dll` 的显式导入 —— UE5 通过 API set 转发（`api-ms-win-core-*`）间接使用。
- ✅ **有** `dxgi.dll` —— 这是**最强的代理目标**。DXGI 在 D3D12 初始化前必然加载，且 Steam Overlay、RTSS、MSI Afterburner 都走这条路，属于"极度常见"的注入点。
- ✅ **有** `UIAutomationCore.DLL` —— 冷门但合法。UE 的 Slate 无障碍层会加载它。可以作为**低特征值**的备用落点 `[推断]`。
- 💡 没有 `kernel32` 显式导入 → 直接 `LoadLibrary` + `CreateRemoteThread` 的经典注入路径依然有效（通过 API set 解析），但特征更明显。

---

## 三、攻击面清单（按"性价比"排序）

| # | 攻击面 | 可行性 | 收益 | 成本 | 综合 |
|---|---|---|---|---|---|
| 1 | **反射元数据挖掘**（`Binds.Cache`） | ✅ 已成功 | ★★★★★ 情报 | 极低 | **立刻做** |
| 2 | **配置层**（`Engine.ini`） | ✅ 纯文本 | ★☆ 画质/输入 | 极低 | 顺手做 |
| 3 | **CEF3 UI 层** | ✅ 有 `libcef.dll` + 明文日志 | ★★★ 信息/自动化 | 低 | 值得做 |
| 4 | **UE5 反射内省 + ProcessEvent**（内存） | ✅ 无 AC 阻挡 | ★★★★ 本地效果 | 中 | **主线** |
| 5 | **DLL 代理注入**（`dxgi.dll`） | ✅ 导入表确认 | ★★★★ 载体 | 中 | **主线** |
| 6 | **IoStore 重打包**（改资产） | ⚠️ 格式被打乱 | ★★★★ 深度改造 | 高 | 攻坚 |
| 7 | **协议层**（网络/后端 API） | ⚠️ 服务端权威 | ★★★★★ 真正收益 | **极高** | 终局 |
| 8 | **存档伪造** | ❌ JWT 加密 | — | 极高 | **放弃** |
| 9 | **磁盘补丁 Shipping.exe** | ❌ 破坏签名 | — | — | **放弃** |

---

## 四、游戏逻辑架构（来自反射元数据挖掘）

从 `Binds.Cache` 中提取到 **364 个 `/Script/Dungeons.*` 类**。关键发现：

### 4.1 这套游戏用的是 **GAS（Gameplay Ability System）**

```
/Script/Dungeons.GASCharacterMovementComponent
/Script/Dungeons.GA_QuestInteract
/Script/Dungeons.GA_QuestUpdateKillTask
/Script/Dungeons.GCN_SpawnDebrisStatic
/Script/Dungeons.DungeonsGameplayCueNotify_Static
/Script/Dungeons.SpiceOpsAttributeTableRow
```

- `GA_*` = Gameplay Ability
- `GCN_*` = Gameplay Cue Notify
- `GASCharacterMovementComponent` = GAS 集成
- `SpiceOpsAttributeTableRow` = **属性运算表**（"Spice" 是 MCD 项目内部代号，延续自初代）

> ⚠️ **这直接推翻了"找到生命值浮点数，改成 9999"的朴素思路。** GAS 是数据驱动的属性系统，血量/伤害不是散落的裸 float，而是：
> - `UAttributeSet` 子类里的 `FGameplayAttributeData`（BaseValue + CurrentValue 双分量）
> - 修改走 `FActiveGameplayEffect` 的聚合器（Aggregator）栈
> - 任何直接写内存都会被下一次 `RecalculateAttributes` 覆盖

**正确的钩子点**：`UAbilitySystemComponent::ApplyModToAttribute` / `FGameplayEffectAggregator::Evaluate` / `UAttributeSet::PostGameplayEffectExecute`。这是**进程内方案的技术核心**。

### 4.2 StateTree 是 AI/玩家逻辑的驱动器

大量 `ST_*`（StateTree 节点）、`STT_*`（Task）、`STE_*`（Evaluator）、`STC_*`（Condition）类：

```
STT_PlayerRangedAim / STT_PlayerControllerRangedAim / STT_PLayerRangedAttack
STT_PlayerMoveTowardsMouseCursor / STT_PlayerPath / STT_PlayerPathToTarget
STT_DynamicMouseTargetLock / STT_ReacquireMouseTargetLock
STC_IsTargetAlive / STC_IsTargetValid / STC_IsRotatedTowardsTarget
STE_GetCurrentAttackableTarget / STE_GetCurrentTarget
```

> **这是自瞄的天然落点。** 游戏把"选目标 → 判断可否攻击 → 转向 → 远程瞄准 → 攻击"整条链路做成了显式的 StateTree 任务节点。
> 拦截 `STE_GetCurrentAttackableTarget` 的返回值，或强制 `STT_RotateToTarget` 生效，就等于自瞄 —— 而且**调用的是游戏自己的合法逻辑路径**，行为特征最接近正常玩家。
>
> 项目里自带 `STT_DynamicMouseTargetLock` / `STT_ReacquireMouseTargetLock`，说明**手柄/键鼠的目标锁定是游戏原生机制**，不是外挂发明的东西。

### 4.3 移动与角色

```
PlayerCharacter / MobCharacter / BaseCharacter / MockPlayerCharacter
PlayerCharacterMovementComponent / MobCharacterMovementComponent
GASCharacterMovementComponent / DungeonsProjectileMovementComp
MoveToTargetMovementComponent / SimpleMovementComponent / ArcMovement
```

### 4.4 值得注意的缺失

**没有 `Health` / `Damage` / `AttributeSet` 之类的独立类名**，只有 `SpiceOpsAttributeTableRow`。

这不是缺失，而是**证实**：属性定义在 `AttributeSet` 的**属性（FGameplayAttribute）** 层面，而不是类层面。属性名要在 `Binds.Cache` 的**属性记录**里找，不在类记录里。

---

## 五、UE5 反射内省的可行性判定

这是所有内存方案的前提。逐项核实：

| 前提 | 状态 | 依据 |
|---|---|---|
| `GObjects` / `GUObjectArray` 存在 | ✅ | UE 引擎内建，Shipping 不裁剪 |
| `GNames` / `FNamePool` 存在 | ✅ | 同上 |
| `UFunction` 对象 + `ProcessEvent` 可用 | ✅ | Shipping 保留反射调用机制 |
| 类名/属性名可读 | ✅ | `Binds.Cache` 已给出全部名字 `[实测]` |
| **属性内存偏移** | ❌ **不在 `Binds.Cache` 里** | 见下 |
| PDB / 符号 | ❌ 无 | `[实测]` |

### 关于 `Binds.Cache` 的格式（已完全解析）

实测格式为**连续的 `[u32 长度][字符串][\0]` 流**，例如：

```
文件偏移 0x00: 5D 41 00 00   → 长度 0x415D
文件偏移 0x04: 1B 00 00 00   → 长度 27
文件偏移 0x08: "/Script/CoreUObject.Object\0"
文件偏移 0x27: 4A 00 00 00   → 长度 74
文件偏移 0x2B: "../../../Engine/Source/Runtime/CoreUObject/Public/UObject/NoExportTypes.h\0"
```

共 **158,138 条记录** `[实测]`。内容 = 类名 + 头文件路径 + 属性名 + 属性类型 + 函数名。

> ❗ **重要澄清**：我最初以为那些 `u32` 是内存偏移。实测证明**它们是字符串长度**。`Binds.Cache` 是**纯名字/类型元数据**，**不含任何 `PROPERTY_OFFSET` 数字**。
>
> 后果：**属性偏移必须自己在二进制里重建。** 但这不是坏消息 —— 你已经有了完整的"名字 → 类型 → 头文件"映射表，重建偏移需要的全部信息（名字 + 类型 + 继承链）都在手上。这比从零开始 dump 强太多。

### 附带红利：**源码路径泄漏**

`Binds.Cache.Headers` 里包含引擎源码的**相对路径**：

```
../../../Engine/Source/Runtime/CoreUObject/Public/VerseVM/VVMVerseClass.h
../../../Engine/Source/Runtime/Engine/Public/Materials/MaterialExpression.h
../../../Engine/Source/Runtime/CoreUObject/Public/StructUtils/PropertyBag.h
```

`[实测]`

**这意味着你能直接对照 UE 5.6 的公开源码**来确定每个类的字段顺序和布局 —— 对重建偏移是决定性的加速器。

---

## 六、具体路线（按优先级）

### 6.1 【第一优先级】静态情报与符号重建

**目标**：把 200 MB 的裸二进制变成"有名字、有结构"的可分析目标。

1. **建立基线**：先算哈希、备份二进制与全部 `.utoc/.ucas`。后续任何一次游戏更新都会让所有偏移失效。
2. **提取 `.text` 段**：`.text` = 148,649,984 字节（约 148 MB）`[实测]`，是全部代码。
3. **生成 IDA/Ghidra 数据库**：
   - 本机**没有 IDA 本体**（只有桌面上一个第三方生成的 `idapro.hexlic` 伪造授权，`product_version: 9.1`，`owner: 48-2137-ACAB-99`，`id: 48-1337-DEAD-01` —— 这类文件不能替代授权，且来源不可信，**不要加载**）。`[实测]`
   - 推荐 **Ghidra**（免费、脚本化能力强）或自行解决 IDA 授权。
4. **注入符号名**：写脚本把 `Binds.Cache` 里的 158k 条名字写进反编译器，作为字符串/注释锚点。这样能通过"类名 → 静态注册函数 → 构造函数 → 属性偏移"的链条定位一切。
5. **重建偏移**：UE 的 `UClass` 在 `UObject::StaticClass()` 注册时会把属性偏移写进 `FProperty::Offset_Internal`。定位这个注册过程，就能在**运行时**把全部偏移 dump 出来 —— 这比静态猜偏移可靠得多。

> 💡 **更快的路**：直接借力 **UE4SS** 或 **Dumper-7** 这类成熟的 UE 反射 dumper。它们本来就是为"无 PDB 的 Shipping 构建"设计的，会通过特征码扫描定位 `GObjects`/`GNames`/`ProcessEvent` 并枚举全部类、属性、函数、**以及偏移**。你只需要把特征码适配到 UE 5.6 + 这个特定构建。
>
> **这是投入产出比最高的一步。**

### 6.2 【低成本】配置层

`Engine.ini` / `GameUserSettings.ini` 无签名无校验 `[实测]`。可以：
- 调整 `sg.*` 画质分组（性能/视觉，**不是作弊**）
- 强制分辨率、帧率、HDR
- 尝试通过 `[SystemSettings]` 段设置引擎 CVar（FOV、`r.*` 渲染开关等）

> ⚠️ 但**不要指望** `r.` / `g.` 前缀的调试 CVar 能出效果：Shipping 构建在编译期就把大部分调试 CVar 裁掉了，而且**游戏性 CVar（`g.`）不可能在纯客户端生效** —— 因为游戏性是服务端权威的。

### 6.3 【中等成本】CEF3 UI 层

证据 `[实测]`：
- `Engine\Binaries\ThirdParty\CEF3\Win64\libcef.dll` + `chrome_elf.dll` + `icudtl.dat` + 多语言 `.pak`
- `%LOCALAPPDATA%\Dungeons2\Saved\webcache_4430\` —— 完整的 Chromium 缓存目录：`Cache`、`Code Cache\js`、`Code Cache\wasm`、`Local Storage\leveldb`、`Network Persistent State`
- `cef3.log` 明文可读
- 二进制导入 `UIAutomationCore.DLL`

**可做的事**：
1. 读 `Local Storage\leveldb`（LevelDB）→ 提取 UI 层缓存的界面状态、玩家偏好、可能含后端返回的 JSON
2. 给 CEF 传 `--remote-debugging-port=<port>` → 用 Chrome DevTools Protocol 完全控制内嵌浏览器 `[推断，需验证启动参数传递路径]`
3. 程序化驱动 UI（配合 `UIAutomationCore`）→ 自动化刷本、自动领奖

> 这条路的价值在于：**UI 层看到的数据（背包、属性面板、掉落）是最容易被可靠读取的**，而且不需要碰游戏内存。适合做"信息显示"类功能。

### 6.4 【主线】进程内方案

**载体（注入）** —— 导入表已给出答案 `[实测]`：

| 方案 | 评价 |
|---|---|
| **`dxgi.dll` 代理** | ✅ 首选。DXGI 必然在 D3D12 初始化前加载，Steam Overlay/RTSS 同路，特征极低 |
| `OPENGL32.dll` 代理 | ✅ 备用。游戏用 D3D12，但 OpenGL32 仍被导入 |
| `UIAutomationCore.DLL` 代理 | ✅ 冷门低特征值选项 |
| `version.dll` / `dinput8.dll` / `winmm.dll` | ❌ **未导入，此法不可用** |
| `LoadLibrary` + `CreateRemoteThread` | ⚠️ 可行（API set 转发），但特征明显 |
| 手动映射 / 反射式注入 | ⚠️ 可行，但工程量大 |

**功能实现层次（由易到难）**：

```
第 1 层  只读信息
        ├─ 遍历 GObjects 拿 Actor 列表 / 敌人列表 / 掉落物
        ├─ 读 UWorld → PersistentLevel → Actors
        └─ 输出到自绘 overlay（走 dxgi 的 Present 钩子）
        → 收益：透视、雷达、信息面板

第 2 层  调用游戏自己的函数
        ├─ 定位 ProcessEvent (UObject*, UFunction*, void* Params)
        ├─ 用 Binds.Cache 里的函数名查 UFunction 对象
        └─ 直接调用游戏逻辑（比硬改内存安全得多，不会崩）
        → 收益：自瞄（走 STT_*/STE_* 目标选择）、自动交互、自动拾取

第 3 层  改属性
        ├─ 钩 FGameplayEffectAggregator / UAttributeSet::PostGameplayEffectExecute
        ├─ 或改 FGameplayAttributeData 的 BaseValue / CurrentValue
        └─ 注意 GAS 的聚合器会覆盖裸写入 → 必须钩在正确的层
        → 收益：无敌、秒杀、无限资源

第 4 层  改游戏状态机
        ├─ 钩 StateTree 任务节点
        └─ 改 Quest / Spawn / Loot 逻辑
        → 收益：刷特定掉落、跳过任务、加速刷本
```

> **强烈建议走第 2 层而不是第 3 层。** 调用游戏自己的函数（`ProcessEvent` + 真实 `UFunction`）会完整走一遍游戏的状态更新、动画通知、UI 刷新。硬改内存会绕过这些，导致：
> - 动画/特效不同步
> - 服务端校验直接不通过
> - 随机崩溃（GAS 的聚合器会把你写的值覆盖成垃圾）

### 6.5 【攻坚】IoStore 资产重打包

这是**收益最高但工程量最大**的路线，因为它能改"规则本身"而不是"运行时的数值"。

**障碍**：`.utoc` 头部字段布局被打乱 `[实测]`，标准工具链直接失败。

**工作清单**：
1. 反推实际的头部字段顺序（UE 5.6 的 `FIoStoreTocHeader` 有公开定义，对照即可定位偏移置换关系）
2. 找到 **AES 密钥**：UE 的 `FPakFile` / IoStore 用 AES-256 加密 `EncryptionKey`。密钥在二进制里由 `FAES::DecryptData` 的调用点传入，通常经过拆分成多个常量后拼接（防 `strings` 搜索）
   - 定位方法：找 `FAES::DecryptData` → 回溯 `FAesKey` 的构造点
3. 用 `retoc` / `repak` / `UnrealPak` 的解包逻辑 + 自己写的头部适配层
4. 解出资产 → 用 FModel / UE Viewer 查看 → 改 DataTable / Blueprint → 重打包
5. **处理签名校验**：`.pak` footer 里有 20 字节的 `PakFileHash`（`[实测]` 见 footer 结构）。IoStore 也有对应的签名块。要确认游戏是否校验它 —— 若校验，需要同时处理签名，或者用 `-signedpak` / `-unsignedpak` 参数绕过

**改什么最值钱**：
- `SpiceOpsAttributeTableRow` 之类的 **DataTable** —— 直接改属性系数、掉落权重、装备数值
- 掉落表 —— 控制出什么装备
- 关卡/刷怪配置 —— 改怪物密度、Boss 行为

> ⚠️ 但**必须记住**：单机/本地生效，联机时这些改动要么被服务端拒绝，要么直接导致校验失败踢出。

### 6.6 【终局】协议层

**这是唯一能产生"真正高价值、可持久"结果的路线**，也是难度最高的。

已知信息 `[实测]`：
- 后端：PlayFab（+ Xbox Live GDK + EOS）
- HTTP 栈：`XCurl` / `libHttpClient` / `msquic`（HTTP/3 over QUIC）
- 网络驱动：`GameNetDriver` + `StatelessConnectHandlerComponent`
- 鉴权：两个 JWT 令牌文件，但**落盘加密**（`ProtectedTokenBytes`）`[实测]`

**ETH 的工作序列**：

```
1. 抓包                → 但 msquic/HTTP3 加密，常规代理抓不到明文
   ├─ 方案 A: 进程内 Hook XCurl 的 CURLOPT 回调 / libHttpClient 的请求函数
   ├─ 方案 B: Hook WinHTTP / bcrypt 的 TLS 层（在加密之前）
   └─ 方案 C: Hook 游戏自己的请求构造函数（最干净，直接拿结构体）
2. 逆向请求/响应结构    → 拿到 PlayFab 的 TitleId、API 端点、自定义 endpoint
3. 判定权威边界        → 逐字段测试：哪些字段服务端信、哪些不信
4. 构造合法请求        → 用游戏自己的签名/令牌（从内存里读解密的 JWT）
5. 处理重放保护        → 找 nonce / 时间戳 / 序列号
```

**先验判断（诚实说）**：Mojang/Microsoft 的在线服务几乎肯定是**服务端权威**的。进度、掉落、货币、属性都在服务端算。能撬动的窗口主要在：
- 客户端预测（client-side prediction）与服务端校正之间的**时序窗口**
- 服务端信任的客户端上报字段（如"我完成了这一关"）
- 竞态条件（同一操作快速重复提交）

> ⚠️ **并且这是最高风险区**：动服务端数据 = 直接触发账号封禁，而且这类操作很可能触及《计算机欺诈与滥用法》/ 服务条款的法律边界。相比之下，纯本地的内存修改只影响你自己的客户端。

---

## 七、明确不建议的方向（附理由）

| 方向 | 为什么不行 |
|---|---|
| **磁盘补丁 `Dungeons-Win64-Shipping.exe`** | 破坏 Microsoft 代码签名 `[实测]`，Steam "验证文件完整性"秒回滚 |
| **伪造存档** | 令牌加密（`ProtectedTokenBytes`）`[实测]`，密钥在进程内且大概率由 GDK 凭证派生 |
| **改 `GlobalSaveDataDefault.sav` 刷进度** | 服务端有 JWT 权益票据交叉校验，本地改了服务端不认 |
| **`version.dll` / `dinput8.dll` 代理** | 这两个 DLL **根本不在导入表里** `[实测]`，代理不会被加载 |
| **指望 Shipping 控制台命令** | 调试 CVar 在 Shipping 编译期被裁剪；游戏性 CVar 纯客户端无效 |
| **CE 直接搜血量 float 改 9999** | GAS 的属性聚合器会覆盖裸写入；而且血量是 `FGameplayAttributeData` 双分量结构 |
| **用桌面那个 `idapro.hexlic`** | 第三方伪造授权，`owner/id` 是占位符（`48-1337-DEAD-01`），来源不可信，有安全风险 `[实测]` |

---

## 八、反检测与版本更新

### 8.1 检测面

既然没有内核 AC，检测来自：
1. **服务端行为分析** —— 最高威胁。异常 DPS、异常通关时间、不可能的移动轨迹、掉落率偏离
2. **客户端上报** —— 游戏可能上报模块列表、内存校验和、遥测事件
3. **Microsoft 代码签名 / 文件完整性** —— 只对磁盘篡改有效
4. **Xbox Live / PlayFab 的遥测** —— `PlayFab` 本身就带事件上报能力

### 8.2 原则

- **只读优先**：读内存几乎无迹可寻；写内存才有风险
- **走游戏自己的逻辑**：`ProcessEvent` 调 `UFunction` 比直接写内存的痕迹小得多
- **行为伪装**：加随机延迟、模拟人类输入曲线、限制射速/移速在合理范围
- **不要碰签名文件**：一切改动走内存或注入 DLL，不落盘改游戏文件
- **遥测出口**：考虑 Hook `libHttpClient` / `XCurl` 的请求发送，过滤掉含自己特征的遥测包 `[推断]`

### 8.3 版本更新后的维护成本

**这是这个项目最大的隐性成本**：

- 每次游戏更新 → 二进制重编译 → 所有**特征码失效**、所有**偏移失效**
- 没有 PDB `[实测]` → 无法做符号级 diff，只能重新跑一遍特征码扫描
- `Binds.Cache` 会更新 → 名字通常稳定，但偏移全变

**对策**：把"特征码扫描 + 运行时反射 dump"做成**自动化流水线**，而不是硬编码偏移。每次更新后跑一遍，自动重定位。这是能让项目活下去的唯一方式。

---

## 九、建议的启动顺序（可直接执行）

```
阶段 0  基线固化（30 分钟）
        ├─ 备份 Shipping.exe / *.utoc / *.ucas 的哈希清单
        ├─ 记录游戏版本、Steam buildid
        └─ 备份 %LOCALAPPDATA%\Dungeons2\Saved\

阶段 1  符号与偏移重建（1–3 天）★ 关键路径
        ├─ 装 Ghidra；导入 .text 段（148 MB）
        ├─ 把 Binds.Cache 的 158k 个名字导入为符号锚点
        ├─ 编译/适配 UE4SS 或 Dumper-7 到 UE 5.6 这个构建
        └─ 【里程碑】运行时 dump 出全部类 + 属性 + 偏移

阶段 2  最小可用载体（1–2 天）
        ├─ 写 dxgi.dll 代理（转发全部导出 + 加载自身 payload）
        ├─ 验证注入成功：拿到 GObjects 并枚举 UWorld / PlayerCharacter
        └─ 【里程碑】控制台打印出当前关卡的全部 Actor

阶段 3  只读信息功能（2–5 天）
        ├─ 钩 dxgi Present → 自绘 overlay
        ├─ 遍历敌人列表 → 屏幕投影 → 方框/雷达
        └─ 【里程碑】可用的透视/雷达

阶段 4  调用游戏逻辑（3–7 天）
        ├─ 定位 ProcessEvent
        ├─ 用 Binds.Cache 名字解析 UFunction
        ├─ 走 STE_GetCurrentAttackableTarget / STT_RotateToTarget 做自瞄
        └─ 【里程碑】自瞄（行为特征最低的那种）

阶段 5  GAS 属性层（1–2 周，难度陡增）
        ├─ 逆向 UAttributeSet 子类布局
        ├─ 钩 FGameplayEffectAggregator::Evaluate
        └─ 【里程碑】可控的属性修改，不崩溃

阶段 6  资产重打包（2–4 周，平行推进）
        ├─ 适配 .utoc 头部 → 用 retoc/FModel 成功解包
        ├─ 定位 AES 密钥
        └─ 【里程碑】改一个 DataTable 并成功加载

阶段 7  协议层（不封顶，高风险）
        └─ 只有前面全部做完、且明确接受封号风险后再考虑
```

---

## 十、风险提示（必须说清）

1. **法律与条款**：修改在线游戏客户端、伪造服务端数据，违反 Minecraft/Microsoft 服务条款，可能构成违约；若涉及绕过技术保护措施或干扰服务端，可能触及《计算机欺诈与滥用法》及国内相关法规。**纯本地单机的内存修改**与**攻击在线服务**是性质完全不同的两件事。
2. **账号风险**：即使没有内核 AC，服务端行为分析 + 遥测足以识别异常并封号。
3. **技术风险**：注入 DLL 到带签名的商业游戏进程，可能触发崩溃上报（`CrashReportClient` 存在 `[实测]`），你的注入痕迹会随崩溃报告回到厂商。
4. **维护风险**：见 §8.3，这是一条不断需要维护的长线。

---

## 附录 A：实测数据速查

```
主程序 SHA256   7C83AFBF0AD34A40B853CDB25A22FFFB605D08E2A1E2D431974D7C7C1EE0BA54
主程序 MD5      6A5749107680A32E86CD3A36571B601C
引擎            Unreal Engine 5.6  (IoStore ContainerHeaderVersion = 8)
反作弊          无（无 EAC / BattlEye / Denuvo / 壳）
签名            Valid, CN=Microsoft Corporation
.text           148,649,984 bytes @ RVA 0x1000
.rdata           46,294,912 bytes @ RVA 0x8DC5000
导入 DLL        41 个（含 dxgi.dll / OPENGL32.dll / UIAutomationCore.DLL）
                 不含 version.dll / dinput8.dll / winmm.dll
游戏类数        364 个 /Script/Dungeons.*
反射记录数      158,138 条 (Binds.Cache)
反射格式        [u32 长度][字符串\0] 连续流（非偏移表）
存档格式        UE GVAS；令牌加密 (ProtectedTokenBytes)
存档位置        %LOCALAPPDATA%\Dungeons2\Saved\SaveGames\
配置位置        %LOCALAPPDATA%\Dungeons2\Saved\Config\Windows\
CEF 缓存        %LOCALAPPDATA%\Dungeons2\Saved\webcache_4430\
```

## 附录 B：本次分析产出的工具

| 文件 | 作用 |
|---|---|
| `utoc2.py` | IoStore `.utoc` 头部解析器（UE5 v8 布局） |
| `bindmine.py` | `Binds.Cache` 反射元数据挖掘器 |
| `gvas.py` | UE `GVAS` 存档解析器 |
| `rawd.py` | GVAS 原始取证 dump（hex + ASCII + UTF-16 + base64 扫描） |
| `jwt.py` | 存档内 JWT 提取与解码 |
| `strdump.py` | 二进制字符串/结构 dump |
| `dungeons_classes.txt` | 364 个游戏类的完整清单 |
