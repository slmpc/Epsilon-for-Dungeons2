# 功能模块框架（Module / Setting / Config）

代码：`src/payload/feature/`
[`module/Module.h`](../../src/payload/feature/module/Module.h) ·
[`module/ModuleManager.h`](../../src/payload/feature/module/ModuleManager.h) ·
[`settings/Setting.h`](../../src/payload/feature/settings/Setting.h) ·
[`config/ConfigManager.h`](../../src/payload/feature/config/ConfigManager.h)

这一层是**纯框架**：它自己不含任何具体功能模块，也不认识任何具体模块。功能模块继承
`Module` 并在 `initModules()` 里登记。

---

## Module：与 Java 版（Open-Epsilon）的三处刻意偏离

| # | 偏离 | 原因 |
|---|---|---|
| 1 | **去掉 EventBus 订阅** | Java 版 `setEnabled` 会去 EventBus 订阅/退订。这里改成纯虚的 `onEnable`/`onDisable` 钩子 + 可选的帧回调 —— 框架只提供挂载点，事件从哪来由宿主（挂钩层）决定。这样模块框架不依赖钩子实现，单独测试也不用起游戏。 |
| 2 | **去掉 i18n 与通知** | 那是 UI 层的事，不该渗进框架。模块只提供 `name`/`description` 原文。 |
| 3 | **加 dirty 标记** | Java 版靠 UI 主动调 `saveNow`。注入体**没有「退出前保存」这种可靠时机**（游戏可能被直接关掉），所以必须支持随时增量保存 —— 模块自己记「自上次保存后有没有变过」，`ConfigManager` 据此决定要不要落盘。 |

### 开关

`setEnabled(v)` 在状态未变时是**空操作**。这一点很重要：重复 `setEnabled(true)` 不应该
反复触发 `onEnable`，否则挂/摘钩子会被重复执行。

状态**先改再回调** —— `onEnable` 里读 `isEnabled()` 必须已经看到新状态。

`reset()` 的顺序：

1. 先 `setEnabled(false)` —— 走 `onDisable`，让模块有机会撤销做过的事
2. 键位回到 `defaultKeyBind_`
3. 绑定模式回到 `Toggle`
4. 可见性回到 `defaultHidden_`
5. 所有设置回默认值
6. `loadCustomState(nullptr)`，让子类清掉额外状态
7. 置 dirty
8. 最后若 `defaultEnabled_` 则 `setEnabled(true)`

> ⚠️ 第 2 步曾经写成「清空为 unbound」，结果**任何在构造期绑好的热键都会在配置加载时被抹掉** ——
> 模块永远不可能自带默认热键。

### ★ `setDefaultXxx` 而不是 `setXxx`

模块在**构造期**声明「出厂默认」时必须用：

- `setDefaultKeyBind` 而非 `setKeyBind`
- `setDefaultHidden` 而非 `setHidden`
- `setDefaultEnabled` / `applyDefaultEnabled`

因为 `ConfigManager` 加载配置时会调用 `Module::reset()`，而 `reset()` 会把各项恢复成
`defaultXxx_`。只改当前值的话，这次 `reset()` 会把它抹掉 —— 曾导致模块明明注册了却不出现
在面板上（隐藏标志被重置回 `true`）。

#### ⚠️ 连带陷阱：错误的默认值会被持久化

这类「构造期默认值写错」的 bug 会把**错误的值写进配置**。修好代码之后，`reset()` 已经正确地
把 `hidden_` 置回 `false`，但紧接着 `fromJson()` 又从磁盘读到旧配置里的 `"hidden": true`，
于是再次被隐藏 —— 表现为「代码明明修了，还是不显示」。

**排查顺序：**

1. 先看 `~/.epsilon/ext/dungeons2/configs/<配置名>/modules/<模块名>.json`
   里有没有可疑的持久化值（`hidden` / `keyBind` / `ApplyTo` 等）
2. 改掉配置，然后在面板上点 **Reload**（不必重启游戏）
3. 再回去确认代码里的默认值

`hidden` 本身是合法功能（把模块从列表里藏起来），所以**不要**把「读配置覆盖它」改成忽略 ——
要修的是默认值，以及清掉被污染的历史配置。

### 序列化

`toJson()` 写：`version` / `enabled` / `keyBind` / `bindMode` / `hidden` / `settings` / （可选）`state`。
`version` 当前只写不读，留给将来的格式迁移判断。

`fromJson()` 的顺序**开关放最后**：`onEnable` 里通常会读设置值，只有设置都就位了再启用，
模块才会看到正确的配置。顺序反了会让模块以默认参数启用一次。

坏值一律吞掉（单项 `try/catch`）：手改配置里 `keyBind` 写成字符串是常态，不该让整个模块加载失败。

---

## Category

```cpp
enum class Category : uint8_t { combat, player, movement, render };
```

落盘与显示用小写名字（与 Open-Epsilon 的枚举名一致）。`categoryFromName()` 大小写不敏感、
忽略空格/下划线/连字符，**无法识别时返回 `nullopt`** —— 配置里出现未知分类应当被忽略，
而不是硬塞进某个默认分类（那会让模块莫名出现在错误的页签里）。

---

## ModuleManager

职责：拥有全部模块（唯一所有权）、按名字/分类查询、把按键事件按 Toggle/Hold 语义分发、
把每帧回调转发给已启用的模块。

与 Java 版的差别：

- **没有 EventBus**。Java 版靠 `@EventHandler` 订阅按键/鼠标事件，因为 Minecraft 提供了现成的
  事件总线。本项目没有，所以改成「宿主喂事件」：`Overlay` 的窗口过程拿到按键后调
  `dispatchKeyEvent`。框架不知道事件从哪来，也不知道游戏是否在前台。
- **不持有游戏状态**。Java 版的 `nullCheck()` 会去问 `Minecraft.getInstance()`。这里由宿主决定
  何时不该分发，框架只做分发。

### 注册

重复名字**被拒绝**并返回 `false`。静默覆盖会让「我注册了两个同名模块却只有一个生效」
变成极难查的问题。

`modules_` 是**按注册顺序**保存的 vector，不是直接遍历 `unordered_map` ——
否则「设置界面里模块顺序每次启动都在变」。

### 按键分发：三步

| 步 | 动作 | 为什么 |
|---|---|---|
| 1 | 收集本次事件涉及的模块，并判断是否有模块因此被**启用** | 必须先收集再改状态 —— 遍历过程中改 `enabled` 会让后面的模块看到已经变化的世界，Toggle/Hold 的语义就乱了 |
| 2 | 应用状态变更 | Toggle：按下时切一次；Hold：按下启用、抬起禁用 |
| 3 | 让模块有机会消费（`onKeyEvent`） | 先问受影响模块，未消费再问全部已启用模块 —— 一个没绑键但需要观察按键的模块（如宏录制）也能收到事件 |

返回值语义：**只要「有模块绑定了这个键并因此动作了」就算已处理**。
绑定本身就是一种处理 —— 宿主据此决定不再把按键交给游戏
（否则「用 F5 开关模块」会同时触发游戏里绑在 F5 的功能）。

### 每帧

`onFrame()` 只转发给**已启用**的模块。让每个模块自己判 `isEnabled()` 会把同样的判断抄到
每一份实现里，而且很容易漏。

---

## Setting 类型体系

### 与 Java 版的三处偏离

| # | 偏离 | 原因 |
|---|---|---|
| 1 | 数值类型分流成 `IntSetting` / `DoubleSetting` | Java 靠泛型 + 装箱。C++ 里让整数与浮点共用一份实现只会换来一堆 `static_cast` 和重载 |
| 2 | `EnumSetting` 不持有枚举类型，只持有「候选名列表 + 当前下标」 | C++ 的 enum 没有 Java 那样的 `EnumConstants` 反射。用字符串候选表换来：序列化天然可读（config 里直接写 `"Hold"` 而不是 `1`），且不需要反射 |
| 3 | 依赖关系用一个 bool 回调（`Setting::dependsOn`） | Java 版是 `Dependency` 接口；C++ 里 `std::function` 已经够用 |

### 序列化按值收发

设置**名字**就是它的键，所以落盘形态是：

```json
"settings": { "Enabled": true, "Multiplier": 3.5 }
```

比每个设置再裹一层 `{"value": …, "type": …, "name": …}` 短得多，也更符合「配置就是给人改的」
这个前提。类型信息不必落盘 —— 代码里的 `Setting` 子类本身就是 schema。

### 容错

- 越界会被**夹到** `[min, max]`（`setValue`）
- `setValueUnbounded` 既不夹取也不触发回调，用于把配置文件里已经越界的旧值**原样**吃进来，
  避免把用户的错误值静默改成别的数
- `setValueSilent` 不触发 `onChanged_` —— 加载时批量改值不应引发副作用
- 依赖未声明时 `isAvailable()` 恒为 `true`（「没有依赖」与「依赖满足」在调用方看来应当无差别）
- 依赖不满足的设置由 UI 层**置灰但仍然显示**，不隐藏

### KeybindSetting

键码沿用 **Windows 虚拟键码**（不是 GLFW 键码）：注入体跑在 Windows 上，按键事件最终来自
Win32 消息，直接用 VK 码省掉一层映射。修饰键也按 VK 存。

`unbound` 用 `-1` 表示 —— `0` 是合法值（某些 API 里代表鼠标左键），不能当哨兵。

---

## ConfigManager

### 文件布局

```
<配置根>/                        默认 ~/.epsilon/ext/dungeons2
  active-config.txt              当前生效的配置名(纯文本单行)
  configs/
    default/
      modules/
        <模块名>.json            一个模块一个文件
    <其它配置名>/
      modules/...
```

**为什么按模块拆文件而不是一个大 json：** 模块是最小的独立功能单元，拆开之后「某个模块配置
写坏了」只影响它自己，手改时也不用在几百行里找那一段。代价是加载要走一次目录遍历 ——
但配置目录只有几十个文件，这点开销相对注入流程可以忽略。

**为什么默认落在 `~/.epsilon/ext/dungeons2`：** 与 Open-Epsilon 的 `~/.epsilon` 保持同一父目录，
便于用户在一处管理。多出的 `ext/dungeons2` 一层是因为 Open-Epsilon 自身的配置文件已经占了
`~/.epsilon` 根目录（`accounts.json` / `client-settings.json` 等），直接往同一个目录里塞会互相干扰；
按「扩展名 / 目标」分层也让同一台机器上可以同时存在多个 Epsilon 目标的配置。

**覆盖方式：** 环境变量 `EPSILON_CONFIG_DIR` 指向别的目录（测试与多开用）。

### 与 Java 版的差异

- **去掉 zip 导入/导出** —— 注入体跑在游戏进程里，没有合适的 UI 触发这些操作
- **去掉账号 / 好友 / 根级客户端设置的持久化** —— 那是 Open-Epsilon 特有的域概念
- **去掉旧版布局迁移**（`LegacyConfigMigrator`）—— 本项目尚无历史版本需要兼容
- **保留** `active-config` 与多配置切换 —— 这直接决定「用哪份配置启动」

### 其它约束

- `initialize()` 可重复调用（等价于 `reload`）。失败时配置仍可用，只是全部停在默认值
- `save()` 任何时候都可安全调用，包括尚未 `initialize()` 时（那时会先补齐目录结构）
- `saveIfDirty()` 由宿主**周期性**调用（Runtime 的功能线程每 5 秒一次），只在真的有改动时写盘 ——
  避免无条件重写配置文件
- `deleteConfig()` 不允许删掉最后一个，也不允许删当前生效的那个
- `isValidConfigName()` 必须挡住路径穿越（配置名要当目录名用）
- 内部实现都不加锁，由公开接口统一持锁；带 `Locked` 后缀的成员要求调用方已持锁

---

## 相关文档

- 面板文案为什么必须纯 ASCII 英文：[ui.md](ui.md)
- `Speed` / `Jump` 两个模块的现状：[speed-jump.md](speed-jump.md)
