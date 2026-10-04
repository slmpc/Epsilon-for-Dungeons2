#pragma once

#include "payload/game/Field.h"
#include "payload/game/Offsets.h"
#include "payload/game/ue/Engine.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace epsilon::game::dungeons2 {

// ATR_Currency 上的属性。偏移见 offsets::currencyAttribute。
enum class CurrencyAttribute : uint8_t {
    emeralds = 0,
    emeraldsMax,
    emeraldsMin,
    emeraldIncreasePercentage,
    emeraldDropChanceIncrease,
    maxAdditionalEmeralds,
    emeraldCapForDamageIncrease,
    count,
};

inline constexpr size_t currencyAttributeCount =
    static_cast<size_t>(CurrencyAttribute::count);

// 纯 ASCII, 会进面板。
[[nodiscard]] std::string_view currencyAttributeName(CurrencyAttribute attribute) noexcept;

// 代码生成属性表里**声明**的位置。
[[nodiscard]] uint32_t declaredCurrencyOffset(CurrencyAttribute attribute) noexcept;

// 实测的数据位置 = 声明值 + offsets::currencyAttribute::dataShift。
[[nodiscard]] uint32_t currencyDataOffset(CurrencyAttribute attribute) noexcept;

// ---------------------------------------------------------------- 属性集
// 绑定一个 ATR_Currency*。所有读写都走 safeRead / safeWrite。
class CurrencyAttributeSet {
public:
    CurrencyAttributeSet() = default;
    explicit CurrencyAttributeSet(uint64_t address) : address_(address) {}

    [[nodiscard]] bool valid() const noexcept { return address_ != 0; }
    [[nodiscard]] uint64_t address() const noexcept { return address_; }

    // 读 CurrentValue。
    [[nodiscard]] std::optional<float> read(CurrencyAttribute attribute) const;
    // 写 Base 与 Current 两者 —— 只写其一会被 GAS 聚合时的另一个值盖回去。
    bool write(CurrencyAttribute attribute, float value) const;

    // 按裸偏移读写, 供诊断命令用。
    [[nodiscard]] std::optional<float> readAt(uint32_t offset) const;
    bool writeAt(uint32_t offset, float value) const;

    // 形态校验: 所有属性都必须是有限的非负数, 且不超过 limit。
    // 用来把"随便一个对象"挡掉 —— 偏移失效时读出的是垃圾。
    [[nodiscard]] bool looksLikeCurrencySet(float limit) const;

private:
    uint64_t address_ = 0;
};

// ---------------------------------------------------------------- 解析器
struct CurrencyTarget {
    uint64_t    set = 0;
    std::string setName;
    std::string outerClass;
    bool        resolved = false;
};

// 在对象表里找属于本地玩家的 ATR_Currency 实例。
// 优先取 Outer 链能走到玩家 pawn 的那个; 退而取任意非 CDO 实例。
class CurrencyResolver {
public:
    bool resolve(ue::Engine& engine, bool force = false);
    void invalidate() noexcept { nextResolveAtMs_ = 0; }

    [[nodiscard]] bool ready() const noexcept { return set_.valid(); }
    [[nodiscard]] CurrencyAttributeSet const& set() const noexcept { return set_; }
    [[nodiscard]] CurrencyTarget const& target() const noexcept { return target_; }

    // 诊断。纯 ASCII, 会被面板显示。
    [[nodiscard]] std::string const& lastError() const noexcept { return lastError_; }
    [[nodiscard]] int candidates() const noexcept { return candidates_; }

private:
    CurrencyAttributeSet set_{};
    CurrencyTarget       target_{};
    std::string          lastError_;
    int                  candidates_ = 0;
    uint64_t             nextResolveAtMs_ = 0;
};

} // namespace epsilon::game::dungeons2
