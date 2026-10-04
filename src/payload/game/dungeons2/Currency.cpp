#include "payload/game/dungeons2/Currency.h"

#include "common/Text.h"
#include "payload/game/dungeons2/Player.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <string>

namespace epsilon::game::dungeons2 {
namespace {

// 形态校验上限: 货币属性是有限的非负数。合理上限取 1e8, 再大就不是游戏里的数了。
constexpr float maxSanityValue = 1.0e8f;

constexpr uint64_t resolveIntervalMs = 2000;
constexpr int      maxOuterDepth = 8;

} // namespace

std::string_view currencyAttributeName(CurrencyAttribute attribute) noexcept {
    switch (attribute) {
        case CurrencyAttribute::emeralds:                    return "Emeralds";
        case CurrencyAttribute::emeraldsMax:                 return "EmeraldsMax";
        case CurrencyAttribute::emeraldsMin:                 return "EmeraldsMin";
        case CurrencyAttribute::emeraldIncreasePercentage:   return "EmeraldIncreasePercentage";
        case CurrencyAttribute::emeraldDropChanceIncrease:   return "EmeraldDropChanceIncrease";
        case CurrencyAttribute::maxAdditionalEmeralds:       return "MaxAdditionalEmeralds";
        case CurrencyAttribute::emeraldCapForDamageIncrease: return "EmeraldCapForDamageIncrease";
        case CurrencyAttribute::count:
        default:                                             return "?";
    }
}

uint32_t declaredCurrencyOffset(CurrencyAttribute attribute) noexcept {
    using namespace offsets::currencyAttribute;
    switch (attribute) {
        case CurrencyAttribute::emeralds:                    return emeralds;
        case CurrencyAttribute::emeraldsMax:                 return emeraldsMax;
        case CurrencyAttribute::emeraldsMin:                 return emeraldsMin;
        case CurrencyAttribute::emeraldIncreasePercentage:   return emeraldIncreasePercentage;
        case CurrencyAttribute::emeraldDropChanceIncrease:   return emeraldDropChanceIncrease;
        case CurrencyAttribute::maxAdditionalEmeralds:       return maxAdditionalEmeralds;
        case CurrencyAttribute::emeraldCapForDamageIncrease: return emeraldCapForDamageIncrease;
        case CurrencyAttribute::count:
        default:                                             return 0;
    }
}

uint32_t currencyDataOffset(CurrencyAttribute attribute) noexcept {
    return declaredCurrencyOffset(attribute) + offsets::currencyAttribute::dataShift;
}

// ---------------------------------------------------------------- CurrencyAttributeSet
std::optional<float> CurrencyAttributeSet::read(CurrencyAttribute attribute) const {
    if (attribute == CurrencyAttribute::count) return std::nullopt;
    return readAt(currencyDataOffset(attribute));
}

bool CurrencyAttributeSet::write(CurrencyAttribute attribute, float value) const {
    if (attribute == CurrencyAttribute::count) return false;

    const uint32_t base = currencyDataOffset(attribute);
    // Base 与 Current 都要写: GAS 聚合时读 CurrentValue, 但下一次重算会从 BaseValue 取。
    const bool okBase = writeAt(base, value);
    const bool okCurrent = writeAt(
        base + offsets::currencyAttribute::currentValueDelta, value);
    return okBase && okCurrent;
}

std::optional<float> CurrencyAttributeSet::readAt(uint32_t offset) const {
    return readF32(address_, offset);
}

bool CurrencyAttributeSet::writeAt(uint32_t offset, float value) const {
    return writeF32(address_, offset, value);
}

bool CurrencyAttributeSet::looksLikeCurrencySet(float limit) const {
    if (!address_ || !(limit > 0.0f)) return false;

    for (size_t i = 0; i < currencyAttributeCount; ++i) {
        const auto attribute = static_cast<CurrencyAttribute>(i);
        auto v = read(attribute);
        if (!v) return false;
        if (!std::isfinite(*v)) return false;
        if (*v < 0.0f || *v > limit) return false;
    }
    return true;
}

// ---------------------------------------------------------------- CurrencyResolver
bool CurrencyResolver::resolve(ue::Engine& engine, bool force) {
    const uint64_t now = ::GetTickCount64();
    if (!force && now < nextResolveAtMs_) return set_.valid();
    nextResolveAtMs_ = now + resolveIntervalMs;
    candidates_ = 0;

    if (!engine.ready()) {
        lastError_ = "engine not ready";
        return false;
    }

    auto& objects = engine.objects();
    const uint64_t pawn = findLocalPlayerPawn(engine);

    uint64_t best = 0;
    int bestScore = -1;
    std::string bestName;
    std::string bestOuter;

    objects.for_each([&](ue::ObjectStat const& st) {
        if (st.className != offsets::currencyOwnerClass) return true;
        if (!st.address) return true;

        // 类默认对象不是玩家身上的实例。按标志位判, 不按名字前缀 ——
        // 名字可能解析不出来(实测踩过: CDO 的名字为空, 于是被当成正常实例选中,
        // 模块接着往 CDO 上写)。写 CDO 会影响之后所有新实例。
        auto flags = readField<uint32_t>(st.address, offsets::object::flags);
        if (!flags) return true;
        if ((*flags & offsets::object::classDefaultObjectFlag) != 0) return true;

        CurrencyAttributeSet candidate(st.address);
        if (!candidate.looksLikeCurrencySet(maxSanityValue)) return true;
        ++candidates_;

        // 分数: Outer 链能走到本地玩家 pawn 的优先。
        int score = 0;
        std::string outerClass;
        uint64_t outer = st.address;
        for (int depth = 1; depth <= maxOuterDepth; ++depth) {
            outer = objects.outerOf(outer);
            if (!outer) break;
            if (pawn && outer == pawn) {
                score += 100;
                outerClass = objects.classNameOf(outer);
                break;
            }
            if (outerClass.empty()) outerClass = objects.classNameOf(outer);
        }

        if (score > bestScore) {
            bestScore = score;
            best = st.address;
            bestName = st.name;
            bestOuter = outerClass;
        }
        return true;
    });

    if (!best) {
        set_ = CurrencyAttributeSet{};
        target_ = CurrencyTarget{};
        lastError_ = fmt("no {} instance in object table ({} candidates rejected)",
                         offsets::currencyOwnerClass, candidates_);
        return false;
    }

    set_ = CurrencyAttributeSet(best);
    target_.set = best;
    target_.setName = bestName;
    target_.outerClass = bestOuter;
    target_.resolved = true;
    lastError_.clear();
    return true;
}

} // namespace epsilon::game::dungeons2
