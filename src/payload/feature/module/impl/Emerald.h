#pragma once

#include "payload/feature/module/Module.h"
#include "payload/game/dungeons2/Currency.h"

#include <cstdint>

namespace epsilon::feature {

// 绿宝石获取倍率。
//
// 施加方式: 每帧观察玩家的 `ATR_Currency::Emeralds`。游戏里这个值走 GAS,
// 每次拾取/结算都会把**原始值**写进去; 我们在同一帧把「相对上一次原始值的增量」
// 放大后写回, 并记住这一笔。读到的值仍等于我们上次写入的 -> 游戏本帧没写, 不动它。
class EmeraldModule final : public Module {
public:
    EmeraldModule();

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    [[nodiscard]] std::string info() const override;

private:
    void applyIfNeeded();
    // 强制重扫属性集(换关卡/重生后实例会换)。
    void refreshTarget();
    [[nodiscard]] game::dungeons2::CurrencyAttribute targetAttribute() const;

    uint64_t setAddress_ = 0;
    // 我们上一次写进去的值。读回等于它 -> 是我们的写入, 不该再放大一次。
    float    lastApplied_ = 0.0f;
    // 与 lastApplied_ 对应、扣掉我们加成后的游戏原始值。
    float    lastRaw_ = 0.0f;
    bool     hasBaseline_ = false;

    uint64_t lastWarnMs_ = 0;

    EnumSetting*   attribute_ = nullptr;
    DoubleSetting* multiplier_ = nullptr;
    BoolSetting*   scaleDecrease_ = nullptr;
    BoolSetting*   notify_ = nullptr;
    DoubleSetting* setAmount_ = nullptr;
};

} // namespace epsilon::feature
