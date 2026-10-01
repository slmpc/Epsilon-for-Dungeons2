#include "payload/feature/module/Module.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace epsilon::feature {
namespace {

// 分类名表。顺序必须与 Category 枚举一致。
constexpr std::string_view categoryNames[] = {
    "combat",     // Category::combat
    "player",     // Category::player
    "movement",   // Category::movement
    "render",     // Category::render
};
constexpr size_t categoryCount = std::size(categoryNames);

std::string_view bindModeNameOf(Module::BindMode mode) noexcept {
    return mode == Module::BindMode::hold ? "Hold" : "Toggle";
}

} // namespace

std::string_view categoryName(Category c) noexcept {
    const auto i = static_cast<size_t>(c);
    if (i >= categoryCount) return "unknown";
    return categoryNames[i];
}

std::optional<Category> categoryFromName(std::string_view name) {
    auto normalize = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            if (ch == ' ' || ch == '-' || ch == '_') continue;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    };
    const std::string want = normalize(name);
    for (size_t i = 0; i < categoryCount; ++i) {
        if (normalize(categoryNames[i]) == want) return static_cast<Category>(i);
    }
    return std::nullopt;
}

std::vector<Category> allCategories() {
    std::vector<Category> out;
    out.reserve(categoryCount);
    for (size_t i = 0; i < categoryCount; ++i) out.push_back(static_cast<Category>(i));
    return out;
}

Module::Module(std::string_view name, Category category, std::string_view description)
    : name_(name), description_(description), category_(category) {}

void Module::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;    // 状态没变 -> 不重复触发生命周期钩子

    enabled_ = enabled;
    dirty_ = true;

    // 先改状态再回调 —— 回调里读 isEnabled() 必须已经看到新状态。
    if (enabled_) {
        onEnable();
    } else {
        onDisable();
    }
}

void Module::setKeyBind(int32_t vk) noexcept {
    if (keyBind_ == vk) return;
    keyBind_ = vk;
    dirty_ = true;
}

void Module::setDefaultKeyBind(int32_t vk) noexcept {
    // 默认键位同时是当前键位: 构造期还没有用户配置覆盖, 两者理应一致。
    defaultKeyBind_ = vk;
    keyBind_ = vk;
}

void Module::setBindMode(BindMode mode) noexcept {
    if (bindMode_ == mode) return;
    bindMode_ = mode;
    dirty_ = true;
}

std::string Module::bindModeName() const {
    return std::string(bindModeNameOf(bindMode_));
}

bool Module::setBindModeByName(std::string_view name) {
    auto normalize = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            if (ch == ' ' || ch == '-' || ch == '_') continue;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    };
    const std::string want = normalize(name);
    for (auto mode : {BindMode::toggle, BindMode::hold}) {
        if (normalize(bindModeNameOf(mode)) == want) {
            setBindMode(mode);
            return true;
        }
    }
    return false;
}

void Module::setHidden(bool v) noexcept {
    if (hidden_ == v) return;
    hidden_ = v;
    dirty_ = true;
}

// 设置变更时把它标脏。所有 add* 都会挂上这个回调。
void Module::hookSettingDirty(Setting& s) {
    s.setOnChanged([this] { markDirty(); });
}

BoolSetting& Module::addBool(std::string_view name, bool defaultValue, std::string_view description) {
    auto owned = std::make_unique<BoolSetting>(name, defaultValue, description);
    BoolSetting& ref = *owned;
    hookSettingDirty(ref);
    settings_.push_back(std::move(owned));
    return ref;
}

IntSetting& Module::addInt(std::string_view name, int64_t defaultValue, int64_t min, int64_t max,
                           std::string_view description) {
    auto owned = std::make_unique<IntSetting>(name, defaultValue, min, max, description);
    IntSetting& ref = *owned;
    hookSettingDirty(ref);
    settings_.push_back(std::move(owned));
    return ref;
}

DoubleSetting& Module::addDouble(std::string_view name, double defaultValue,
                                 double min, double max, double step,
                                 std::string_view description) {
    auto owned = std::make_unique<DoubleSetting>(name, defaultValue, min, max, step, description);
    DoubleSetting& ref = *owned;
    hookSettingDirty(ref);
    settings_.push_back(std::move(owned));
    return ref;
}

StringSetting& Module::addString(std::string_view name, std::string_view defaultValue,
                                 std::string_view description) {
    auto owned = std::make_unique<StringSetting>(name, defaultValue, description);
    StringSetting& ref = *owned;
    hookSettingDirty(ref);
    settings_.push_back(std::move(owned));
    return ref;
}

KeybindSetting& Module::addKeybind(std::string_view name, int32_t defaultKey,
                                   std::string_view description) {
    auto owned = std::make_unique<KeybindSetting>(name, defaultKey, description);
    KeybindSetting& ref = *owned;
    hookSettingDirty(ref);
    settings_.push_back(std::move(owned));
    return ref;
}

EnumSetting& Module::addEnum(std::string_view name, std::vector<std::string> choices,
                             std::string_view defaultValue, std::string_view description) {
    auto owned = std::make_unique<EnumSetting>(name, std::move(choices), defaultValue, description);
    EnumSetting& ref = *owned;
    hookSettingDirty(ref);
    settings_.push_back(std::move(owned));
    return ref;
}

SettingGroup& Module::addGroup(std::string_view name) {
    groups_.push_back(std::make_unique<SettingGroup>(name));
    return *groups_.back();
}

Setting* Module::findSetting(std::string_view name) const {
    for (auto const& s : settings_) {
        if (s && s->name() == name) return s.get();
    }
    // 再来一轮大小写不敏感 —— 手写配置里大小写写错很常见。
    auto iequals = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };
    for (auto const& s : settings_) {
        if (s && iequals(s->name(), name)) return s.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------- 复位
void Module::reset() {
    // 走 setEnabled(false) 而不是直接写 enabled_, 让 onDisable 有机会撤销
    // 模块做过的事(还原钩子/恢复内存改写等)。
    setEnabled(false);
    // 键位回默认而不是清空: 键位属于"设置", 出厂就该有值。
    keyBind_ = defaultKeyBind_;
    bindMode_ = BindMode::toggle;
    hidden_ = defaultHidden_;

    for (auto& s : settings_) {
        if (s) s->reset();
    }

    loadCustomState(nullptr);   // 让子类有机会清掉自己的额外状态

    dirty_ = true;

    if (defaultEnabled_) setEnabled(true);
}

void Module::applyDefaultEnabled() {
    if (defaultEnabled_) setEnabled(true);
}

// ---------------------------------------------------------------- 序列化
nlohmann::json Module::toJson() const {
    nlohmann::json out = nlohmann::json::object();

    // version 留着给将来的格式迁移判断用。当前只写不读。
    out["version"] = configVersion;
    out["enabled"] = enabled_;
    out["keyBind"] = keyBind_;
    out["bindMode"] = std::string(bindModeNameOf(bindMode_));
    out["hidden"] = hidden_;

    nlohmann::json settings = nlohmann::json::object();
    for (auto const& s : settings_) {
        if (!s) continue;
        nlohmann::json value = nullptr;
        s->writeTo(value);
        settings[s->configKey()] = std::move(value);
    }
    out["settings"] = std::move(settings);

    nlohmann::json custom = saveCustomState();
    if (!custom.is_null()) out["state"] = std::move(custom);

    return out;
}

void Module::fromJson(nlohmann::json const& in) {
    if (!in.is_object()) return;

    // ---- 键位 / 绑定模式 / 可见性 ----
    // 单独 try: 手改配置里 keyBind 写成字符串是常态, 不该让整个模块加载失败。
    if (auto it = in.find("keyBind"); it != in.end() && it->is_number_integer()) {
        // 不直接改 keyBind_ —— 走 setKeyBind 保持"改动即脏"的语义一致。
        try { setKeyBind(it->get<int32_t>()); } catch (...) {}
    }

    if (auto it = in.find("bindMode"); it != in.end() && it->is_string()) {
        try { setBindModeByName(it->get<std::string>()); } catch (...) {}
    }

    if (auto it = in.find("hidden"); it != in.end() && it->is_boolean()) {
        try { setHidden(it->get<bool>()); } catch (...) {}
    }

    // ---- 设置值 ----
    // 遍历的是**代码里的设置表**而不是 json 的键: 多余的键被忽略, 缺的键保持默认。
    if (auto it = in.find("settings"); it != in.end() && it->is_object()) {
        auto const& settingsObj = *it;
        for (auto const& s : settings_) {
            if (!s) continue;
            auto valueIt = settingsObj.find(s->configKey());
            if (valueIt == settingsObj.end()) continue;
            s->readFrom(*valueIt);
        }
    }

    // ---- 自定义状态 ----
    if (auto it = in.find("state"); it != in.end()) {
        try { loadCustomState(*it); } catch (...) {}
    }

    // ---- 开关放最后 ----
    // ★ 必须最后应用: onEnable 里通常会读设置值, 只有设置都就位了再启用,
    // 模块才会看到正确的配置。顺序反了会让模块以默认参数启用一次。
    if (auto it = in.find("enabled"); it != in.end() && it->is_boolean()) {
        try { setEnabled(it->get<bool>()); } catch (...) {}
    }
}

} // namespace epsilon::feature
