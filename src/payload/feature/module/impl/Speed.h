// ============================================================================
//  Speed.h — 加速模块。
//  movementAttribute 是唯一实测生效的施加目标。
//  三个目标的取舍与实测依据见 docs/features/speed-jump.md
// ============================================================================
#pragma once

#include "payload/feature/module/Module.h"

namespace epsilon::feature {

class SpeedModule final : public Module {
public:
    SpeedModule();

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    [[nodiscard]] std::string info() const override;

private:
    // 施加目标。落盘名见 sinkLabel()。
    enum class Sink { gameMultiplier, maxWalkSpeed, movementAttribute };

    [[nodiscard]] Sink currentSink() const;
    // 该目标在当前构建上是否拿得到(有可用偏移 / 有属性集)。
    [[nodiscard]] bool sinkAvailable(Sink sink) const;
    [[nodiscard]] bool readSink(Sink sink, float& out) const;
    bool writeSink(Sink sink, float value) const;

    void applyIfNeeded(bool force);
    void captureBaseline(Sink sink);

    // 目标 → 落盘/显示名。
    [[nodiscard]] static char const* sinkLabel(Sink sink);

    Sink  activeSink_ = Sink::movementAttribute;
    bool  haveActiveSink_ = false;

    float base_ = 0.0f;         // 基线(启用瞬间的原始值)
    bool  hasBase_ = false;
    float lastApplied_ = 0.0f;  // 我们上一次写入的值

    EnumSetting*   sink_ = nullptr;
    DoubleSetting* multiplier_ = nullptr;
    BoolSetting*   notify_ = nullptr;

    // 已经就"退回 MaxWalkSpeed"解释过一次了, 避免每帧刷屏。
    bool explainedFallback_ = false;};

} // namespace epsilon::feature
