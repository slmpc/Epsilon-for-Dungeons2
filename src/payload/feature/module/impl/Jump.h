#pragma once

#include "payload/feature/module/Module.h"

namespace epsilon::feature {

class JumpModule final : public Module {
public:
    JumpModule();

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    [[nodiscard]] std::string info() const override;

private:
    // 把计算出的值写进游戏。值没变就不写。
    void applyIfNeeded(bool force);

    // 启用瞬间读到的原始值。用它乘倍率, 避免在已放大的值上再乘一次。
    float baseZ_ = 0.0f;
    // 我们上一次写进去的值, 用来判断当前值是否仍是我们写的。
    float lastApplied_ = 0.0f;
    bool  hasBase_ = false;

    DoubleSetting* multiplier_ = nullptr;
    BoolSetting*   logOnce_ = nullptr;
};

} // namespace epsilon::feature
