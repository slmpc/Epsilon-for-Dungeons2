// ============================================================================
//  Setting.cpp
// ============================================================================
#include "payload/feature/settings/Setting.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

namespace epsilon::feature {
namespace {

// ---------------------------------------------------------------- 取值辅助
//
// 这几个函数收的是 "设置的值本身"(不是包着它的对象), 所以签名不带 key。
//
// 为什么全用这套带 fallback 的取法而不是 in.get<T>() 直接转换: 配置文件是
// 用户可手改的明文, 里面出现 "速度": "快" 这种类型不符的值是常态。直接转换
// 会抛异常, 而异常从注入体的配置加载路径里穿出去会把游戏带崩。这里的策略是
// "单条值退化到默认值, 其余照常加载", 坏值不会连累整份配置。

// 数值的特殊处理: 允许 "3000" 这种被引号包起来的数字。
// 手写配置时很常见, 为此报错属于不必要的严苛。
double asNumberOr(nlohmann::json const& in, double fallback) {
    if (in.is_number()) {
        try { return in.get<double>(); } catch (...) { return fallback; }
    }
    if (in.is_string()) {
        try { return std::stod(in.get<std::string>()); } catch (...) { return fallback; }
    }
    if (in.is_boolean()) return in.get<bool>() ? 1.0 : 0.0;
    return fallback;
}

bool asBoolOr(nlohmann::json const& in, bool fallback) {
    if (in.is_boolean()) return in.get<bool>();
    // 0/1 与 "true"/"false" 一并接受, 理由同上。
    if (in.is_number()) return asNumberOr(in, fallback ? 1.0 : 0.0) != 0.0;
    if (in.is_string()) {
        std::string s = in.get<std::string>();
        return !(s.empty() || s == "0" || s == "false" || s == "False");
    }
    return fallback;
}

std::string asStringOr(nlohmann::json const& in, std::string_view fallback) {
    if (in.is_string()) return in.get<std::string>();
    // 数值/布尔也能当字符串用, 免得手改成 123 之后整个字段读不出来。
    if (in.is_number() || in.is_boolean()) {
        try { return in.dump(); } catch (...) { return std::string(fallback); }
    }
    return std::string(fallback);
}

// 去掉末尾的 ".000000" 之类零头, 让 double 在 UI 上显示得干净些。
std::string trimNumber(double v) {
    std::string s = std::format("{}", v);
    if (s.find('.') != std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

} // namespace

// ===========================================================================
//  Setting
// ===========================================================================
Setting::Setting(std::string_view name, std::string_view description)
    : name_(name), description_(description), configKey_(name) {}

bool Setting::isAvailable() const {
    return !dependency_ || dependency_();
}

void Setting::notifyChanged() const {
    if (onChanged_) onChanged_();
}

std::vector<std::string> const& Setting::choices() const {
    static const std::vector<std::string> empty;
    return empty;
}

void Setting::writeTo(nlohmann::json& out) const { (void)out; }
void Setting::readFrom(nlohmann::json const& in) { (void)in; }

// ===========================================================================
//  BoolSetting
// ===========================================================================
BoolSetting::BoolSetting(std::string_view name, bool defaultValue, std::string_view description)
    : Setting(name, description), value_(defaultValue), defaultValue_(defaultValue) {}

void BoolSetting::setValue(bool v) {
    if (value_ == v) return;      // 值没变就不触发回调, 避免 UI 高频写入刷爆 hook
    value_ = v;
    notifyChanged();
}

void BoolSetting::reset() {
    const bool changed = (value_ != defaultValue_);
    value_ = defaultValue_;
    if (changed) notifyChanged();
}

std::string BoolSetting::displayValue() const { return value_ ? "true" : "false"; }

void BoolSetting::writeTo(nlohmann::json& out) const { out = value_; }
void BoolSetting::readFrom(nlohmann::json const& in) { setValueSilent(asBoolOr(in, value_)); }

// ===========================================================================
//  IntSetting
// ===========================================================================
IntSetting::IntSetting(std::string_view name, int64_t defaultValue, int64_t min, int64_t max,
                       std::string_view description)
    : Setting(name, description),
      value_(std::clamp(defaultValue, min, max)),
      defaultValue_(std::clamp(defaultValue, min, max)),
      min_(min), max_(max) {}

void IntSetting::setValue(int64_t v) {
    const int64_t clamped = std::clamp(v, min_, max_);
    if (value_ == clamped) return;
    value_ = clamped;
    notifyChanged();
}

void IntSetting::reset() {
    const bool changed = (value_ != defaultValue_);
    value_ = defaultValue_;
    if (changed) notifyChanged();
}

std::string IntSetting::displayValue() const { return std::to_string(value_); }

void IntSetting::writeTo(nlohmann::json& out) const { out = value_; }

void IntSetting::readFrom(nlohmann::json const& in) {
    // 先取数值再夹取。用 unbounded 写入是为了不在这里触发回调 ——
    // 加载配置不该产生副作用。
    const double raw = asNumberOr(in, static_cast<double>(value_));
    // double 转 int64 前必须判范围, 否则是实现定义行为。
    if (raw > 9.2e18 || raw < -9.2e18) return;
    const int64_t parsed = static_cast<int64_t>(raw);
    setValueUnbounded(std::clamp(parsed, min_, max_));
}

// ===========================================================================
//  DoubleSetting
// ===========================================================================
DoubleSetting::DoubleSetting(std::string_view name, double defaultValue,
                             double min, double max, double step,
                             std::string_view description)
    : Setting(name, description),
      value_(std::clamp(defaultValue, min, max)),
      defaultValue_(std::clamp(defaultValue, min, max)),
      min_(min), max_(max), step_(step) {}

void DoubleSetting::setValue(double v) {
    // NaN 会把 std::clamp 的比较全变成 false, 结果是 value 被写成 NaN 而且
    // 以后再也夹不回来。显式挡掉。
    if (std::isnan(v)) return;
    const double clamped = std::clamp(v, min_, max_);
    if (value_ == clamped) return;
    value_ = clamped;
    notifyChanged();
}

void DoubleSetting::reset() {
    const bool changed = (value_ != defaultValue_);
    value_ = defaultValue_;
    if (changed) notifyChanged();
}

std::string DoubleSetting::displayValue() const { return trimNumber(value_); }

void DoubleSetting::writeTo(nlohmann::json& out) const { out = value_; }

void DoubleSetting::readFrom(nlohmann::json const& in) {
    const double raw = asNumberOr(in, value_);
    if (std::isnan(raw)) return;
    setValueUnbounded(std::clamp(raw, min_, max_));
}

// ===========================================================================
//  StringSetting
// ===========================================================================
StringSetting::StringSetting(std::string_view name, std::string_view defaultValue,
                             std::string_view description)
    : Setting(name, description), value_(defaultValue), defaultValue_(defaultValue) {}

void StringSetting::setValue(std::string_view v) {
    if (value_ == v) return;
    value_.assign(v);
    notifyChanged();
}

void StringSetting::reset() {
    const bool changed = (value_ != defaultValue_);
    value_ = defaultValue_;
    if (changed) notifyChanged();
}

void StringSetting::writeTo(nlohmann::json& out) const { out = value_; }
void StringSetting::readFrom(nlohmann::json const& in) {
    setValueSilent(asStringOr(in, value_));
}

// ===========================================================================
//  KeybindSetting
// ===========================================================================
KeybindSetting::KeybindSetting(std::string_view name, int32_t defaultKey, std::string_view description)
    : Setting(name, description), value_(defaultKey), defaultValue_(defaultKey) {}

void KeybindSetting::setValue(int32_t vk) {
    if (value_ == vk) return;
    value_ = vk;
    notifyChanged();
}

void KeybindSetting::reset() {
    const bool changed = (value_ != defaultValue_);
    value_ = defaultValue_;
    if (changed) notifyChanged();
}

std::string KeybindSetting::displayValue() const { return keyName(value_); }

void KeybindSetting::writeTo(nlohmann::json& out) const { out = value_; }

void KeybindSetting::readFrom(nlohmann::json const& in) {
    const double raw = asNumberOr(in, static_cast<double>(value_));
    if (raw > 100000.0 || raw < -100000.0) return;
    setValueSilent(static_cast<int32_t>(raw));
}

std::string KeybindSetting::keyName(int32_t vk) {
    if (vk < 0) return "未绑定";

    // 常见键给可读名。完整的 VK 表有 200 多项, 全列出来收益很低 ——
    // 未覆盖的落到最后的十六进制兜底, 仍然可用于人工对表。
    switch (vk) {
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0D: return "Enter";
        case 0x10: return "Shift";
        case 0x11: return "Ctrl";
        case 0x12: return "Alt";
        case 0x13: return "Pause";
        case 0x14: return "CapsLock";
        case 0x1B: return "Esc";
        case 0x20: return "Space";
        case 0x21: return "PageUp";
        case 0x22: return "PageDown";
        case 0x23: return "End";
        case 0x24: return "Home";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0x5B: return "LWin";
        case 0x5C: return "RWin";
        case 0x90: return "NumLock";
        case 0x91: return "ScrollLock";
        default: break;
    }
    if (vk >= 0x30 && vk <= 0x39) return std::string(1, static_cast<char>('0' + (vk - 0x30)));
    if (vk >= 0x41 && vk <= 0x5A) return std::string(1, static_cast<char>('A' + (vk - 0x41)));
    if (vk >= 0x60 && vk <= 0x69) return "Num" + std::to_string(vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F);
    return std::format("未知键(0x{:X})", vk);
}

// ===========================================================================
//  EnumSetting
// ===========================================================================
EnumSetting::EnumSetting(std::string_view name,
                         std::vector<std::string> choices,
                         std::string_view defaultValue,
                         std::string_view description)
    : Setting(name, description), choices_(std::move(choices)) {
    // 候选表为空是构建期错误, 值得炸出来而不是让 value() 越界。
    if (choices_.empty()) {
        throw std::invalid_argument("EnumSetting 的候选值不能为空: " + name_);
    }
    int found = 0;
    for (size_t i = 0; i < choices_.size(); ++i) {
        if (choices_[i] == defaultValue) { found = static_cast<int>(i); break; }
    }
    index_ = found;
    defaultIndex_ = found;
}

std::string const& EnumSetting::value() const {
    static const std::string unknown{"<非法>"};
    if (index_ < 0 || static_cast<size_t>(index_) >= choices_.size()) return unknown;
    return choices_[static_cast<size_t>(index_)];
}

bool EnumSetting::is(std::string_view choice) const { return value() == choice; }

void EnumSetting::setValue(std::string_view choice) {
    if (setChoiceByName(choice)) return;
    // 未匹配到候选值时保持原样 —— 静默拒绝比抛异常合适, 调用方多半是在
    // 响应 UI 输入, 不应该因为打错字就崩。
}

void EnumSetting::setIndex(int index) {
    if (index < 0 || static_cast<size_t>(index) >= choices_.size()) return;
    if (index_ == index) return;
    index_ = index;
    notifyChanged();
}

void EnumSetting::reset() {
    const bool changed = (index_ != defaultIndex_);
    index_ = defaultIndex_;
    if (changed) notifyChanged();
}

bool EnumSetting::setChoiceByIndex(int index) {
    if (index < 0 || static_cast<size_t>(index) >= choices_.size()) return false;
    setIndex(index);
    return true;
}

bool EnumSetting::setChoiceByName(std::string_view name) {
    // 大小写不敏感匹配, 并额外接受候选名的规范化形式(去空格/连字符/下划线),
    // 这样配置文件里写 "hold"、"Hold"、"HOLD" 都能落到同一个候选值。
    auto normalize = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == ' ' || c == '-' || c == '_' || c == '\t') continue;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    const std::string want = normalize(name);
    for (size_t i = 0; i < choices_.size(); ++i) {
        if (normalize(choices_[i]) == want) {
            setIndex(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

void EnumSetting::writeTo(nlohmann::json& out) const { out = value(); }

void EnumSetting::readFrom(nlohmann::json const& in) {
    // 落盘的是候选名而不是下标 —— 下标会随候选表增删而错位, 名字不会。
    // 这样加一个候选值不会把已有配置读串。
    const std::string raw = asStringOr(in, value());
    // 静默: 配置加载路径不应触发回调。
    if (setChoiceByName(raw)) setIndexSilent(index_);
    // 名字不在候选表里(多半是旧版本配置)就保持当前值, 不报错。
}

} // namespace epsilon::feature
