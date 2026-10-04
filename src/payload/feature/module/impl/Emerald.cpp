#include "payload/feature/module/impl/Emerald.h"

#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/feature/GameContext.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <string>

namespace epsilon::feature {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;
using namespace epsilon::game::dungeons2;

// 荒谬的倍率会把存档里的数字顶到不合理的量级; 100 倍已经远超正常收益。
constexpr double maxMultiplier = 100.0;
// 单次增量放大后的上限, 防止把值写坏。游戏正常单次收益是个位到几百。
constexpr double maxSingleGain = 1.0e7;
constexpr uint64_t warningIntervalMs = 4000;

constexpr char attrEmeralds[]    = "Emeralds";
constexpr char attrMaxAdditional[] = "MaxAdditionalEmeralds";
constexpr char attrDropChance[]  = "EmeraldDropChanceIncrease";

// 面板选项 → 枚举。只暴露常用的三个。
struct AttrChoice {
    char const*    label;
    CurrencyAttribute attribute;
};

constexpr AttrChoice attrChoices[] = {
    {attrEmeralds,      CurrencyAttribute::emeralds},
    {attrMaxAdditional, CurrencyAttribute::maxAdditionalEmeralds},
    {attrDropChance,    CurrencyAttribute::emeraldDropChanceIncrease},
};

[[nodiscard]] CurrencyAttribute attributeFromLabel(std::string_view label) {
    for (auto const& c : attrChoices) {
        if (label == c.label) return c.attribute;
    }
    return CurrencyAttribute::emeralds;
}

[[nodiscard]] char const* labelFromAttribute(CurrencyAttribute attribute) {
    for (auto const& c : attrChoices) {
        if (c.attribute == attribute) return c.label;
    }
    return attrEmeralds;
}

} // namespace

EmeraldModule::EmeraldModule()
    : Module("Emerald", Category::player, "Multiply emerald gains") {
    auto& c = addEnum("Attribute",
                      {attrChoices[0].label, attrChoices[1].label, attrChoices[2].label},
                      attrEmeralds, "Which ATR_Currency attribute to scale");
    attribute_ = &c;

    auto& m = addDouble("Multiplier", 2.0, 1.0, maxMultiplier, 0.5,
                        "Gain multiplier applied to every increase");
    multiplier_ = &m;

    auto& d = addBool("ScaleLoss", false,
                      "Also scale decreases (spending). Off = only gains are boosted");
    scaleDecrease_ = &d;

    auto& n = addBool("Notify", true, "Log each applied change once");
    notify_ = &n;

    auto& s = addDouble("SetAmount", -1.0, -1.0, 1.0e8, 1.0,
                        "Write this value once, then reset to -1 (manual override)");
    setAmount_ = &s;

    setDefaultHidden(false);
}

CurrencyAttribute EmeraldModule::targetAttribute() const {
    if (!attribute_) return CurrencyAttribute::emeralds;
    return attributeFromLabel(attribute_->value());
}

void EmeraldModule::refreshTarget() {
    auto* eng = game().engine;
    if (eng == nullptr || !eng->ready()) return;

    auto& resolver = currency();
    resolver.resolve(*eng, true);

    if (!resolver.ready()) {
        setAddress_ = 0;
        hasBaseline_ = false;
        return;
    }

    const uint64_t now = resolver.set().address();
    if (now != setAddress_) {
        setAddress_ = now;
        hasBaseline_ = false;
        logInfo(fmt("[Emerald] 属性集 {} ({}) 玩家归属={}",
                    hex(now), resolver.target().setName,
                    resolver.target().outerClass.empty()
                        ? std::string("(未匹配到 pawn)")
                        : resolver.target().outerClass));
    }
}

void EmeraldModule::onEnable() {
    setAddress_ = 0;
    hasBaseline_ = false;
    lastApplied_ = 0.0f;
    lastRaw_ = 0.0f;

    refreshTarget();

    auto const& resolver = currency();
    if (!resolver.ready()) {
        logWarn(fmt("[Emerald] 启用但还没找到 ATR_Currency 实例: {}", resolver.lastError()));
        logWarn("[Emerald] 会在解析成功后自动生效");
        return;
    }

    if (notify_ && notify_->value()) {
        const auto& set = resolver.set();
        logInfo(fmt("[Emerald] 目标 {} 的 {} 个属性:", resolver.target().setName,
                    currencyAttributeCount));
        for (size_t i = 0; i < currencyAttributeCount; ++i) {
            const auto a = static_cast<CurrencyAttribute>(i);
            auto v = set.read(a);
            logInfo(fmt("[Emerald]   +{:02x} (数据 +{:02x})  {:<30} = {}", declaredCurrencyOffset(a),
                        currencyDataOffset(a), std::string(currencyAttributeName(a)),
                        v ? fmt("{:.3f}", *v) : std::string("?")));
        }
    }
}

void EmeraldModule::onDisable() {
    setAddress_ = 0;
    hasBaseline_ = false;
}

void EmeraldModule::onFrame() {
    applyIfNeeded();
}

void EmeraldModule::applyIfNeeded() {
    auto* eng = game().engine;
    if (eng == nullptr || !eng->ready()) return;

    auto& resolver = currency();
    if (!resolver.ready() || resolver.set().address() != setAddress_) {
        resolver.resolve(*eng, false);
        if (!resolver.ready()) return;
        if (resolver.set().address() != setAddress_) {
            setAddress_ = resolver.set().address();
            hasBaseline_ = false;
        }
    }

    const auto& set = resolver.set();
    const CurrencyAttribute attribute = targetAttribute();

    // 手动覆盖优先: 写一次就复位成 -1。
    if (setAmount_ && setAmount_->value() >= 0.0 && setAmount_->value() != -1.0) {
        const auto want = static_cast<float>(setAmount_->value());
        if (set.write(attribute, want)) {
            lastApplied_ = want;
            lastRaw_ = want;
            hasBaseline_ = true;
            logInfo(fmt("[Emerald] 手动写入 {} = {:.1f}", labelFromAttribute(attribute), want));
        } else {
            logWarn("[Emerald] 手动写入失败(目标内存不可写?)");
        }
        setAmount_->setValue(-1.0);
    }

    auto current = set.read(attribute);
    if (!current) {
        const uint64_t now = ::GetTickCount64();
        if (now - lastWarnMs_ > warningIntervalMs) {
            lastWarnMs_ = now;
            logWarn("[Emerald] 读不到货币属性(偏移可能失效)");
        }
        return;
    }

    if (!hasBaseline_) {
        lastApplied_ = *current;
        lastRaw_ = *current;
        hasBaseline_ = true;
        return;
    }

    // 与我们上次写入一致 -> 游戏没有新写入。
    if (*current == lastApplied_) return;

    // 游戏写了新的原始值。
    const float raw = *current;
    const double multiplier = multiplier_ ? multiplier_->value() : 1.0;
    const bool scaleBoth = scaleDecrease_ && scaleDecrease_->value();

    const double delta = static_cast<double>(raw) - static_cast<double>(lastRaw_);
    if (delta == 0.0) {
        lastRaw_ = raw;
        lastApplied_ = raw;
        return;
    }
    if (delta < 0.0 && !scaleBoth) {
        // 花钱: 如实接受, 并把它当成新的基线。
        lastRaw_ = raw;
        lastApplied_ = raw;
        return;
    }

    const double scaledDelta = delta * multiplier;
    if (std::fabs(scaledDelta) > maxSingleGain) {
        const uint64_t now = ::GetTickCount64();
        if (now - lastWarnMs_ > warningIntervalMs) {
            lastWarnMs_ = now;
            logWarn(fmt("[Emerald] 单次增量 {:.0f} 超出上限, 本次不放大", scaledDelta));
        }
        lastRaw_ = raw;
        lastApplied_ = raw;
        return;
    }

    const auto want = static_cast<float>(static_cast<double>(raw) + (scaledDelta - delta));

    if (!set.write(attribute, want)) {
        const uint64_t now = ::GetTickCount64();
        if (now - lastWarnMs_ > warningIntervalMs) {
            lastWarnMs_ = now;
            logWarn("[Emerald] 写入失败(目标内存不可写?)");
        }
        return;
    }

    if (notify_ && notify_->value()) {
        logInfo(fmt("[Emerald] {} {:.1f} -> {:.1f}  (原始 {:.1f}, x{:.2f})",
                    labelFromAttribute(attribute),
                    static_cast<double>(lastApplied_), static_cast<double>(want),
                    static_cast<double>(raw), multiplier));
    }

    lastRaw_ = raw;
    lastApplied_ = want;
}

std::string EmeraldModule::info() const {
    auto const& resolver = currency();
    if (!resolver.ready()) return "waiting for ATR_Currency";

    const auto& set = resolver.set();
    const CurrencyAttribute attribute = targetAttribute();
    auto v = set.read(attribute);
    if (!v) return "attribute unavailable";

    const double m = multiplier_ ? multiplier_->value() : 1.0;
    return fmt("{}={:.0f} x{:.2f} @ {}", labelFromAttribute(attribute),
               static_cast<double>(*v), m, resolver.target().setName);
}

} // namespace epsilon::feature
