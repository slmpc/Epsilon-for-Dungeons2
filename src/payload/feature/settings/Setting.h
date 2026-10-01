#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::feature {

class Setting {
public:
    // 依赖回调: 返回 true 表示本设置当前可用。
    using Dependency = std::function<bool()>;
    // 变更回调: 值被 setValue 改动后触发(静默设置不触发)。
    using ChangeHook = std::function<void()>;

    virtual ~Setting() = default;

    Setting(Setting const&) = delete;
    Setting& operator=(Setting const&) = delete;

    // ---------------------------------------------------------------- 标识
    [[nodiscard]] std::string const& name() const noexcept { return name_; }
    // name/description 会进 ImGui 面板, 必须纯 ASCII 英文。
    [[nodiscard]] std::string const& description() const noexcept { return description_; }
    // 序列化时用的键。默认就是 name()。
    [[nodiscard]] std::string const& configKey() const noexcept { return configKey_; }

    // ---------------------------------------------------------------- 可用性
    // 依赖未声明时恒为 true —— "没有依赖"与"依赖满足"在调用方看来无差别,
    // 这样 UI 层不用到处判空。
    [[nodiscard]] bool isAvailable() const;
    void setDependency(Dependency dep) { dependency_ = std::move(dep); }

    void setDescription(std::string_view text) { description_.assign(text); }

    // ---------------------------------------------------------------- 回调
    void setOnChanged(ChangeHook hook) { onChanged_ = std::move(hook); }

    // ---------------------------------------------------------------- 复位
    virtual void reset() = 0;

    // ---------------------------------------------------------------- 类型名
    // 用于 UI 分派与序列化分支。用字符串而不是 enum class, 是为了让第三方
    // 设置类型也能接入而不必改框架头文件。
    [[nodiscard]] virtual std::string_view typeName() const noexcept = 0;

    // ---------------------------------------------------------------- 枚举支持
    // 候选值列表。标量设置返回空数组。
    [[nodiscard]] virtual std::vector<std::string> const& choices() const;
    [[nodiscard]] virtual int  choiceIndex() const { return -1; }
    [[nodiscard]] virtual bool setChoiceByIndex(int index) { (void)index; return false; }
    [[nodiscard]] virtual bool setChoiceByName(std::string_view name) { (void)name; return false; }
    // 显示用文本(标量设置返回值的字符串形式)。会进面板, 必须纯 ASCII 英文。
    [[nodiscard]] virtual std::string displayValue() const = 0;

    // ---------------------------------------------------------------- 状态快照
    // 序列化 / 反序列化**值本身**, 而不是包一层的对象 —— 设置名就是它的键,
    // 模块文件里存成 "settings": { "Enabled": true, "Speed": 3.5 }。
    // 类型信息不必落盘: 代码里的 Setting 子类本身就是 schema。
    virtual void writeTo(nlohmann::json& out) const;
    virtual void readFrom(nlohmann::json const& in);

protected:
    Setting(std::string_view name, std::string_view description);

    // 由派生类在值真正改变后调用, 负责触发 onChanged_。
    void notifyChanged() const;

    std::string name_;
    std::string description_;
    std::string configKey_;
    Dependency  dependency_;
    ChangeHook  onChanged_;
};

// ============================================================================
//  BoolSetting
// ============================================================================
class BoolSetting final : public Setting {
public:
    BoolSetting(std::string_view name, bool defaultValue, std::string_view description = {});

    [[nodiscard]] bool value() const noexcept { return value_; }
    void setValue(bool v);
    // 不触发 onChanged_, 供配置加载使用 —— 加载时批量改值不应引发副作用。
    void setValueSilent(bool v) noexcept { value_ = v; }

    void reset() override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Bool"; }
    [[nodiscard]] std::string displayValue() const override;
    void writeTo(nlohmann::json& out) const override;
    void readFrom(nlohmann::json const& in) override;

private:
    bool value_ = false;
    bool defaultValue_ = false;
};

// ============================================================================
//  IntSetting
// ============================================================================
class IntSetting final : public Setting {
public:
    IntSetting(std::string_view name, int64_t defaultValue,
               int64_t min, int64_t max,
               std::string_view description = {});

    [[nodiscard]] int64_t value() const noexcept { return value_; }
    [[nodiscard]] int64_t minValue() const noexcept { return min_; }
    [[nodiscard]] int64_t maxValue() const noexcept { return max_; }

    // 越界会被夹到 [min, max]。
    void setValue(int64_t v);
    // 不夹取, 不触发回调。供配置加载把已越界的旧值原样吃进来, 避免把用户的
    // 错误值静默改成别的数。
    void setValueUnbounded(int64_t v) noexcept { value_ = v; }

    void reset() override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Int"; }
    [[nodiscard]] std::string displayValue() const override;
    void writeTo(nlohmann::json& out) const override;
    void readFrom(nlohmann::json const& in) override;

private:
    int64_t value_ = 0;
    int64_t defaultValue_ = 0;
    int64_t min_ = 0;
    int64_t max_ = 0;
};

// ============================================================================
//  DoubleSetting
// ============================================================================
class DoubleSetting final : public Setting {
public:
    DoubleSetting(std::string_view name, double defaultValue,
                  double min, double max, double step,
                  std::string_view description = {});

    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] double minValue() const noexcept { return min_; }
    [[nodiscard]] double maxValue() const noexcept { return max_; }
    [[nodiscard]] double step() const noexcept { return step_; }

    // 越界会被夹到 [min, max]; NaN 被忽略。
    void setValue(double v);
    void setValueUnbounded(double v) noexcept { value_ = v; }

    void reset() override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Double"; }
    [[nodiscard]] std::string displayValue() const override;
    void writeTo(nlohmann::json& out) const override;
    void readFrom(nlohmann::json const& in) override;

private:
    double value_ = 0.0;
    double defaultValue_ = 0.0;
    double min_ = 0.0;
    double max_ = 0.0;
    double step_ = 0.0;
};

// ============================================================================
//  StringSetting
// ============================================================================
class StringSetting final : public Setting {
public:
    StringSetting(std::string_view name, std::string_view defaultValue,
                  std::string_view description = {});

    [[nodiscard]] std::string const& value() const noexcept { return value_; }
    void setValue(std::string_view v);
    void setValueSilent(std::string_view v) { value_.assign(v); }

    void reset() override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "String"; }
    [[nodiscard]] std::string displayValue() const override { return value_; }
    void writeTo(nlohmann::json& out) const override;
    void readFrom(nlohmann::json const& in) override;

private:
    std::string value_;
    std::string defaultValue_;
};

// ============================================================================
//  KeybindSetting
//
//  键码沿用 Windows 虚拟键码: 按键事件来自 Win32 消息, 直接用 VK 码省掉一层
//  映射。修饰键也按 VK 存。
// ============================================================================
class KeybindSetting final : public Setting {
public:
    // "未绑定" 用 -1 表示。0 是合法的(某些 API 里代表鼠标左键), 不能当哨兵。
    static constexpr int32_t unbound = -1;

    KeybindSetting(std::string_view name, int32_t defaultKey,
                   std::string_view description = {});

    [[nodiscard]] int32_t value() const noexcept { return value_; }
    [[nodiscard]] bool isBound() const noexcept { return value_ >= 0; }

    void setValue(int32_t vk);
    void setValueSilent(int32_t vk) noexcept { value_ = vk; }
    void clear() { setValue(unbound); }

    void reset() override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Keybind"; }
    [[nodiscard]] std::string displayValue() const override;
    void writeTo(nlohmann::json& out) const override;
    void readFrom(nlohmann::json const& in) override;

    // 人类可读的键名。★ 返回值会画在游戏内 ImGui 面板上, 必须纯 ASCII 英文。
    [[nodiscard]] static std::string keyName(int32_t vk);

private:
    int32_t value_ = unbound;
    int32_t defaultValue_ = unbound;
};

// ============================================================================
//  EnumSetting
//
//  只存 "候选名 + 当前下标", 不绑定具体枚举类型。候选表为空会在构造期抛异常。
// ============================================================================
class EnumSetting final : public Setting {
public:
    EnumSetting(std::string_view name,
                std::vector<std::string> choices,
                std::string_view defaultValue,
                std::string_view description = {});

    [[nodiscard]] std::string const& value() const;
    [[nodiscard]] int index() const noexcept { return index_; }
    [[nodiscard]] bool is(std::string_view choice) const;

    void setValue(std::string_view choice);
    void setIndex(int index);
    void setIndexSilent(int index) noexcept { index_ = index; }

    void reset() override;
    [[nodiscard]] std::string_view typeName() const noexcept override { return "Enum"; }
    [[nodiscard]] std::string displayValue() const override { return value(); }

    [[nodiscard]] std::vector<std::string> const& choices() const override { return choices_; }
    [[nodiscard]] int  choiceIndex() const override { return index_; }
    [[nodiscard]] bool setChoiceByIndex(int index) override;
    [[nodiscard]] bool setChoiceByName(std::string_view name) override;

    void writeTo(nlohmann::json& out) const override;
    void readFrom(nlohmann::json const& in) override;

private:
    std::vector<std::string> choices_;
    int index_ = 0;
    int defaultIndex_ = 0;
};

// ============================================================================
//  SettingGroup
//
//  纯逻辑分组, 不持有任何 UI 概念。UI 层用它决定折叠面板的层次。
// ============================================================================
class SettingGroup {
public:
    explicit SettingGroup(std::string_view name) : name_(name) {}

    [[nodiscard]] std::string const& name() const noexcept { return name_; }
    [[nodiscard]] std::vector<std::unique_ptr<SettingGroup>>& children() noexcept { return children_; }
    [[nodiscard]] std::vector<std::unique_ptr<SettingGroup>> const& children() const noexcept { return children_; }

    SettingGroup& addChild(std::string_view name) {
        children_.push_back(std::make_unique<SettingGroup>(name));
        return *children_.back();
    }

private:
    std::string name_;
    std::vector<std::unique_ptr<SettingGroup>> children_;
};

} // namespace epsilon::feature
