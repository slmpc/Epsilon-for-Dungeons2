# 游戏内面板（FeaturePanel）

代码：[`feature/ui/FeaturePanel.cpp`](../../src/payload/feature/ui/FeaturePanel.cpp)
宿主：[`Overlay.cpp`](../../src/payload/Overlay.cpp) 的 `Modules` 页签

面板画在游戏进程内的 ImGui 上，由 Present 钩子驱动。本文件说明它的结构与几个必须注意的约束。

---

## ★ 面板文案必须纯 ASCII 英文

**任何会被绘制在面板上的文案，必须是纯 ASCII 英文。**

原因（实测踩过，不是风格偏好）：

- 覆盖层用的是 ImGui **内置的 ASCII 位图字体**，没有中文字形。
- 写中文进去不会报错，而是渲染成一串 `?` —— 信息完全丢失。
- 典型踩坑记录：模块面板上曾显示
  `????????????(???, ??? Mock/Mob)`，排查者无法判断那是「没有匹配到玩家」的诊断信息，
  白花了一轮定位。

**受约束的文案**（都会进入面板）：

| 位置 | 例 |
|---|---|
| `Module` 的 name / description | `addDouble("Multiplier", …)` 的描述参数 |
| `Setting` 的 name / description | 展开模块后每个控件的标签与 tooltip |
| `Setting::displayValue()` | 如 `KeybindSetting::keyName()` 返回的键名 |
| `MovementResolver::lastError()` | 面板顶部的解析失败说明 |
| `MovementResolver::offsetSource()` | 面板顶部显示的偏移来源 |
| `Module::info()` | 模块标题右侧的运行状态行 |
| `Overlay.cpp` 里所有 `ImGui::Text*` 的字面量 | 面板固定文案 |

**不受约束**：日志与命令输出。`trace` / `logInfo` / `logWarn` / `emitLine` 走的是文件与命名管道，
中文完全正常，且对排查更有用。**这些地方继续用中文**，不要为了统一而改成英文。

判断方法：问自己「这段文字会不会出现在 ImGui 面板上」。不确定时按会处理。

---

## 绘制结构

`FeaturePanel::draw()` 每次渲染时重建，不保留 ImGui 状态：

1. **顶部状态**：玩家解析结果（pawn 类名 / 移动组件类名 / 偏移来源 / `JumpZ=+0x…  MaxWalk=+0x…`），
   未解析时显示 `player: not resolved` + `lastError()`
2. **批量操作**：模块计数、`Disable all`、`Rescan`（强制重解析玩家目标）
3. **按分类列出模块**：`allCategories()` 顺序遍历 → `modulesIn(cat)` → `drawModule()`
4. **配置动作**：当前配置名、`Save now`、`Reload`、`(unsaved changes)` 提示

### 单个模块（`drawModule`）

- 标题行：启用复选框 + 折叠头
- 展开后：description → `info()`（真实读数，用来一眼判断模块到底有没有生效）→ 热键行 →
  绑定模式 → 可见性 → 各设置控件 → `Reset to defaults`

配置的落盘由 `Runtime` 周期调用 `saveIfDirty()`，面板**不主动写盘** ——
避免拖滑块时每帧都写磁盘。

### 设置控件分派

`drawSetting()` 用虚函数 `typeName()` 分派（`"Bool"` / `"Int"` / `"Double"` / `"String"` /
`"Keybind"` / `"Enum"`），不是 `dynamic_cast` 链 —— 加第三方设置类型时不用改分派逻辑。

**依赖不满足的设置置灰但仍然显示**（`BeginDisabled` + `(unavailable)`），不隐藏 ——
隐藏掉会让用户以为设置丢了。

---

## 改键捕获

### 为什么要区分两种目标

```cpp
struct CaptureTarget {
    enum class Kind { none, moduleKey, settingKey };
    Module*         module;    // Kind::moduleKey
    KeybindSetting* setting;   // Kind::settingKey
};
```

早先这里只存了一个 `KeybindSetting*`，而模块自身的「热键」那一行是**临时构造**的
`KeybindSetting` —— 把它交给下一帧的按键回调就是一个**悬垂指针**。
所以现在明确区分「模块热键」与「模块内部的键位设置」，模块热键直接操作 `Module`，
不经过临时 Setting 对象。

### 捕获流程

`Overlay` 的窗口过程里，改键捕获**早于**模块分发与游戏处理：

```cpp
if (isKey && g.visible && FeaturePanel::isCapturing()) {
    if (FeaturePanel::captureKey(vk, pressed)) return 0;   // 吃掉这个键
}
```

用户点了「改键」之后按下的那个键，只应该被绑定 —— 不应该同时触发一次模块开关、
也不应该漏进游戏。面板不可见时不拦截，否则一个隐藏的面板会把按键全部吃掉。

- `Esc` = 取消改键
- 只处理按下；抬起事件直接吞掉（避免漏到游戏里）
- 右键点击按钮 = 解绑（否则想把键位改回「未绑定」就只能改配置文件）

---

## 字符串设置编辑

ImGui 的 `InputText` 需要可写的 `char*`，而 `StringSetting` 内部是 `std::string`，
所以过一道 256 字节的缓冲（`StringEdit`，用设置指针做键，避免多个字符串设置互相串值）。

**只在失去焦点时提交**（`IsItemDeactivatedAfterEdit`）—— 每敲一个字符都写设置会让
`onChanged` 高频触发，进而让配置每 5 秒都判定为「脏」。

---

## 相关文档

- 覆盖层本身的渲染细节：[../payload/overlay.md](../payload/overlay.md)
- 模块与设置框架：[module-framework.md](module-framework.md)
