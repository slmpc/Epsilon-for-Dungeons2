#pragma once

#include "payload/feature/settings/Setting.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::feature {

// 模块分类。用 enum class 而非字符串, 编译期可查错。
enum class Category : uint8_t {
    combat = 0,
    player,
    movement,
    render,
};

// 落盘与显示用的分类名(小写)。
[[nodiscard]] std::string_view categoryName(Category c) noexcept;
// 从名字解析分类, 大小写与分隔符不敏感。无法识别时返回 nullopt —— 配置里
// 出现未知分类应当被忽略, 而不是硬塞进某个默认分类。
[[nodiscard]] std::optional<Category> categoryFromName(std::string_view name);
// 全部分类, 供 UI 遍历。
[[nodiscard]] std::vector<Category> allCategories();

// ---------------------------------------------------------------------------
//  模块
// ---------------------------------------------------------------------------
class Module {
public:
    // 配置格式版本, 写进每个模块的 json。当前只写不读 —— 模块内部结构变了才动它。
    static constexpr int configVersion = 1;

    // 触发方式。
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
    // 模块可以额外给一行状态文本, UI 显示用。返回值会进面板, 必须纯 ASCII 英文。默认空。
    [[nodiscard]] virtual std::string info() const { return {}; }

    // ---------------------------------------------------------------- 开关
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }
    void toggle() { setEnabled(!enabled_); }

    // 改变开关状态。状态未变时是空操作 —— 重复 setEnabled(true) 不应反复触发
    // onEnable, 否则挂/摘钩子会被重复执行。
    void setEnabled(bool enabled);

    // 默认是否启用。只在第一次构造/复位时生效。
    void setDefaultEnabled(bool v) noexcept { defaultEnabled_ = v; }
    [[nodiscard]] bool defaultEnabled() const noexcept { return defaultEnabled_; }

    // ---------------------------------------------------------------- 键位
    [[nodiscard]] int32_t keyBind() const noexcept { return keyBind_; }
    void setKeyBind(int32_t vk) noexcept;
    // 默认键位。★ 构造期必须用 setDefaultKeyBind 而不是 setKeyBind ——
    // ConfigManager 加载配置时会调 reset(), 它把各项恢复成 defaultXxx_。
    // reset() 回到它, 而不是清成"未绑定"。不设时默认就是未绑定。
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
    // ★ 同上: 构造期必须用 setDefaultHidden 而不是 setHidden。
    void setDefaultHidden(bool v) noexcept { defaultHidden_ = v; }
    [[nodiscard]] bool defaultHidden() const noexcept { return defaultHidden_; }

    // ---------------------------------------------------------------- 设置容器
    // 类型化添加, 返回引用便于链式配置。name/description 会进面板, 必须纯 ASCII 英文。
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

    // 类型化查找。名字对但类型不符时返回 nullptr —— 这类错误应当表现为
    // "拿不到设置"而不是在调用点硬转出一个错误的指针。
    template <typename T>
    [[nodiscard]] T* findSettingAs(std::string_view name) const {
        return dynamic_cast<T*>(findSetting(name));
    }

    // 取一个已知存在设置的引用: 找不到就抛逻辑错误。
    template <typename T>
    [[nodiscard]] T& requireSetting(std::string_view name) const {
        T* s = findSettingAs<T>(name);
        if (!s) throw std::logic_error("模块 " + name_ + " 缺少设置: " + std::string(name));
        return *s;
    }

    SettingGroup& addGroup(std::string_view name);

    // ---------------------------------------------------------------- 复位
    // 回到默认: 关掉、键位回 defaultKeyBind_、绑定模式回 Toggle、可见性回
    // defaultHidden_、所有设置回默认值。会触发 onDisable()(让模块撤销做过的事)。
    virtual void reset();

    // ---------------------------------------------------------------- 生命周期
    // 由 setEnabled 在状态真正翻转时调用, 且**先改状态再回调** —— 回调里
    // isEnabled() 已是新值。默认空实现, 功能模块按需覆盖。
    virtual void onEnable() {}
    virtual void onDisable() {}

    // 每帧回调, 由宿主(帧钩子)驱动。只在模块启用时被调用。默认不做事。
    virtual void onFrame() {}

    // 游戏内按键事件, 由宿主喂进来。vk 是 Windows 虚拟键码, pressed = true 为按下。
    // 返回 true 表示已消费, 宿主不应再处理。默认不消费。
    virtual bool onKeyEvent(int32_t vk, bool pressed) { (void)vk; (void)pressed; return false; }

    // ---------------------------------------------------------------- 状态快照
    // 模块级数据(开关/键位/隐藏/设置)。自定义状态请覆盖 saveCustomState /
    // loadCustomState —— 框架不碰那部分。
    // fromJson 的**开关必须最后应用**: onEnable 里通常要读设置值。
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

    // ★ 构造期改完默认值后调用(让"当前值"与"默认值"对齐)。
    // 只能在构造期调用 —— 之后再调会覆盖用户的运行时改动。
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
