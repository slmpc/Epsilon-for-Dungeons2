// ============================================================================
//  Module.h — 功能模块基类
//
//  对应 Open-Epsilon 的 common/modules/Module.java。移植时做了这些偏离:
//
//    1. **去掉 EventBus 订阅**。Java 版的 setEnabled 会去 EventBus 上订阅/
//       退订事件。C++ 端这里改成纯虚的 onEnable/onDisable 钩子 + 可选的
//       帧回调 —— 框架只提供挂载点, 事件从哪来由宿主(挂钩层)决定。这样
//       模块框架不依赖钩子实现, 单独测试也不用起游戏。
//
//    2. **去掉 i18n 与通知**。那两块是本项目 UI 层的事, 不该渗进框架。
//       模块只提供 name/description 原文, 谁爱翻译谁翻译。
//
//    3. **加 dirty 标记**。Java 版靠 UI 主动调 saveNow。这里让模块自己记
//       "自上次保存后有没有变过", ConfigManager 据此决定要不要落盘 ——
//       注入体没有"退出前保存"这种可靠时机(游戏可能被直接关掉), 所以必
//       须支持随时增量保存。
//
//  这是**纯框架**: 一个具体功能模块都没有。功能模块应当继承 Module 并在
//  ModuleManager::registerModule 里登记, 见该函数注释。
// ============================================================================
#pragma once

#include "payload/feature/settings/Setting.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::feature {

// ---------------------------------------------------------------------------
//  分类。与 Open-Epsilon 的四分类保持一致。
//
//  用 enum class + 名称表而不是纯字符串: 分类是有限集合, 编译期能查错;
//  同时提供 name()/fromName() 便于落盘与 UI 显示。
// ---------------------------------------------------------------------------
enum class Category : uint8_t {
    combat = 0,
    player,
    movement,
    render,
};

// 落盘与显示用的分类名(小写, 与 Open-Epsilon 的枚举名一致)。
[[nodiscard]] std::string_view categoryName(Category c) noexcept;
// 从名字解析分类。无法识别时返回 nullopt —— 配置里出现未知分类应当被忽略,
// 而不是硬塞进某个默认分类(那会让模块莫名出现在错误的页签里)。
[[nodiscard]] std::optional<Category> categoryFromName(std::string_view name);
// 全部分类, 供 UI 遍历。
[[nodiscard]] std::vector<Category> allCategories();

// ---------------------------------------------------------------------------
//  模块
// ---------------------------------------------------------------------------
class Module {
public:
    // 配置格式版本。写进每个模块的 json, 供将来做格式迁移时判断 —— 当前只写
    // 不读。注意这与 ConfigManager 的整体版本是两回事: 模块内部结构变了才动它。
    static constexpr int configVersion = 1;

    // 触发方式。与 Open-Epsilon 的 Module.BindMode 一致。
    enum class BindMode : uint8_t {
        toggle = 0,   // 按一下切一次
        hold,         // 按住生效, 松开失效
    };

    virtual ~Module() = default;

    Module(Module const&) = delete;
    Module& operator=(Module const&) = delete;

    // ---------------------------------------------------------------- 元信息
    [[nodiscard]] std::string const& name() const noexcept { return name_; }
    [[nodiscard]] std::string const& description() const noexcept { return description_; }
    [[nodiscard]] Category category() const noexcept { return category_; }
    // 模块可以额外给一行状态文本(如 "Speed: 1.5"), UI 显示用。默认空。
    [[nodiscard]] virtual std::string info() const { return {}; }

    // ---------------------------------------------------------------- 开关
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }
    void toggle() { setEnabled(!enabled_); }

    // 改变开关状态。状态未变时是空操作 —— 这一点很重要: 重复 setEnabled(true)
    // 不应该反复触发 onEnable, 否则挂/摘钩子会被重复执行。
    void setEnabled(bool enabled);

    // 默认是否启用。只在第一次构造/复位时生效。
    void setDefaultEnabled(bool v) noexcept { defaultEnabled_ = v; }
    [[nodiscard]] bool defaultEnabled() const noexcept { return defaultEnabled_; }

    // ---------------------------------------------------------------- 键位
    [[nodiscard]] int32_t keyBind() const noexcept { return keyBind_; }
    void setKeyBind(int32_t vk) noexcept;
    // 默认键位。子类在构造期设置, reset() 会回到它, 而不是清成"未绑定"。
    // 不设时默认就是未绑定。
    void setDefaultKeyBind(int32_t vk) noexcept;
    [[nodiscard]] int32_t defaultKeyBind() const noexcept { return defaultKeyBind_; }
    [[nodiscard]] BindMode bindMode() const noexcept { return bindMode_; }
    void setBindMode(BindMode mode) noexcept;
    [[nodiscard]] bool isBound() const noexcept { return keyBind_ >= 0; }
    [[nodiscard]] std::string bindModeName() const;
    bool setBindModeByName(std::string_view name);

    // ---------------------------------------------------------------- 可见性
    // 是否在 UI 列表里隐藏(与 enabled 无关)。
    [[nodiscard]] bool isHidden() const noexcept { return hidden_; }
    void setHidden(bool v) noexcept;
    void setDefaultHidden(bool v) noexcept { defaultHidden_ = v; }
    [[nodiscard]] bool defaultHidden() const noexcept { return defaultHidden_; }

    // ---------------------------------------------------------------- 设置容器
    // 类型化添加。返回引用可以直接接链式配置:
    //     auto& s = addDouble("Speed", 1.0, 0.1, 10.0, 0.1);
    BoolSetting&   addBool(std::string_view name, bool defaultValue,
                           std::string_view description = {});
    IntSetting&    addInt(std::string_view name, int64_t defaultValue,
                          int64_t min, int64_t max,
                          std::string_view description = {});
    DoubleSetting& addDouble(std::string_view name, double defaultValue,
                             double min, double max, double step,
                             std::string_view description = {});
    StringSetting& addString(std::string_view name, std::string_view defaultValue,
                             std::string_view description = {});
    KeybindSetting& addKeybind(std::string_view name, int32_t defaultKey,
                               std::string_view description = {});
    EnumSetting&   addEnum(std::string_view name, std::vector<std::string> choices,
                           std::string_view defaultValue,
                           std::string_view description = {});

    [[nodiscard]] std::vector<std::unique_ptr<Setting>>&       settings() noexcept { return settings_; }
    [[nodiscard]] std::vector<std::unique_ptr<Setting>> const& settings() const noexcept { return settings_; }
    [[nodiscard]] std::vector<std::unique_ptr<SettingGroup>>&  groups() noexcept { return groups_; }
    [[nodiscard]] std::vector<std::unique_ptr<SettingGroup>> const& groups() const noexcept { return groups_; }

    // 按名字查设置(大小写不敏感)。找不到返回 nullptr。
    [[nodiscard]] Setting* findSetting(std::string_view name) const;

    SettingGroup& addGroup(std::string_view name);

    // ---------------------------------------------------------------- 复位
    // 回到默认: 关掉、键位清空、可见性复位、所有设置回默认值。
    // 注意 setEnabled(false) 会触发 onDisable() —— 这是有意的, 让模块有机会
    // 撤销自己做的改动。
    virtual void reset();

    // ---------------------------------------------------------------- 生命周期
    // 由 setEnabled 在状态真正翻转时调用。默认空实现, 功能模块按需覆盖。
    virtual void onEnable() {}
    virtual void onDisable() {}

    // 每帧回调, 由宿主(帧钩子)驱动。默认不做事。
    //
    // 只在模块启用时被调用 —— 让模块自己判 enabled 会把判断散到各处。
    virtual void onFrame() {}

    // 游戏内按键事件, 由宿主喂进来。返回 true 表示已消费, 宿主不应再处理。
    // vk 是 Windows 虚拟键码, pressed = true 为按下。默认不消费。
    virtual bool onKeyEvent(int32_t vk, bool pressed) { (void)vk; (void)pressed; return false; }

    // ---------------------------------------------------------------- 状态快照
    // 模块级数据(开关/键位/隐藏/设置)。自定义状态请覆盖 saveCustomState /
    // loadCustomState —— 框架不碰那部分。
    [[nodiscard]] nlohmann::json toJson() const;
    void fromJson(nlohmann::json const& in);

    // 自定义状态的挂载点。默认返回 null(表示没有额外状态)。
    [[nodiscard]] virtual nlohmann::json saveCustomState() const { return nullptr; }
    virtual void loadCustomState(nlohmann::json const& state) { (void)state; }

    // ---------------------------------------------------------------- 变更追踪
    // 自上次 markClean 以来是否有改动。ConfigManager 用它做增量保存。
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
    void markDirty() noexcept { dirty_ = true; }
    void markClean() noexcept { dirty_ = false; }

protected:
    Module(std::string_view name, Category category, std::string_view description = {});

    // 子类在构造里改默认值后调用, 让"当前值"与"默认值"对齐。
    // 必须在构造期调用, 之后再调会覆盖用户的运行时改动。
    void applyDefaultEnabled();

private:
    // 设置变更时把它标脏。所有 add* 都会挂上这个回调。
    void hookSettingDirty(Setting& s);

    std::string name_;
    std::string description_;
    Category    category_;

    bool enabled_ = false;
    bool defaultEnabled_ = false;
    bool hidden_ = true;
    bool defaultHidden_ = true;

    int32_t  keyBind_ = KeybindSetting::unbound;
    int32_t  defaultKeyBind_ = KeybindSetting::unbound;
    BindMode bindMode_ = BindMode::toggle;

    std::vector<std::unique_ptr<Setting>> settings_;
    std::vector<std::unique_ptr<SettingGroup>> groups_;

    bool dirty_ = false;
};

} // namespace epsilon::feature
