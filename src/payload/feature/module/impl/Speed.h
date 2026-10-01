// ============================================================================
//  Speed.h — 加速模块
//
//  ── 两个可选的施加目标 ────────────────────────────────────────────────────
//  IDA 实测得到的关键线索: 游戏自己的移动组件(子类
//  UPlayerCharacterMovementComponent)上存在
//      MovementSpeedMultiplier        +0x?? (复制属性)
//      OnRep_MovementSpeedMultiplier  (复制回调)
//  这说明游戏**本来就有**一个速度倍率字段, 并且它是走网络复制的。
//
//  于是本模块提供两个目标:
//
//    SpeedMultiplier(推荐) —— 改游戏自己的倍率字段。
//        好处: 走的是游戏认可的数值通道, 移动逻辑、动画播放速率、UI 显示
//        大概率会跟着变; 而且这个字段本来就是"倍率", 语义上不违背引擎预期。
//        代价: 它是复制字段, 联机时可能被服务端下发的值覆盖(届时需要每帧补写,
//        本模块已经这么做)。
//
//    MaxWalkSpeed —— 改 UCharacterMovementComponent 的原始最大步速。
//        好处: 一定存在, 不依赖游戏子类的字段。
//        代价: 这是引擎的核心移动参数, 与加速度/制动/摩擦的配比被打破后
//        手感会发飘; 而且它经常被游戏自己的逻辑重算。
//
//  默认选 SpeedMultiplier, 解析不到时自动退回 MaxWalkSpeed —— 但**不静默**
//  退回, 会在通知区说明, 免得用户以为设置没生效。
//
//  ── 为什么每帧写 ──────────────────────────────────────────────────────────
//  同 Jump: 复制字段与引擎重算都会覆盖一次性写入。每帧只在"当前值不是我们
//  写的那一个"时补写, 正常帧只读不写。
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
    // 施加目标。名字直接落盘, 便于人工改配置。
    // 三个可选目标。movementAttribute 是唯一真正生效的(见 Speed.cpp 的注释),
    // 另两个保留用于对照排查。
    enum class Sink { gameMultiplier, maxWalkSpeed, movementAttribute };

    [[nodiscard]] Sink currentSink() const;
    // 取当前目标对应的属性偏移(0 表示不可用)。
    [[nodiscard]] int32_t sinkOffset(Sink sink) const;
    // 该偏移上读到的当前值。
    [[nodiscard]] bool readSink(Sink sink, float& out) const;
    bool writeSink(Sink sink, float value) const;

    void applyIfNeeded(bool force);
    void captureBaseline(Sink sink);

    // 目标 → 落盘/显示名。
    [[nodiscard]] static const char* sinkLabel(Sink s);

    // 上一次实际使用的目标。目标切换时必须重取基线, 否则会拿
    // MaxWalkSpeed 的基线去乘倍率写进 SpeedMultiplier。
    Sink  activeSink_ = Sink::gameMultiplier;
    bool  haveActiveSink_ = false;

    float base_ = 0.0f;         // 基线(启用瞬间的原始值)
    bool  hasBase_ = false;
    float lastApplied_ = 0.0f;  // 我们上一次写入的值

    EnumSetting*  sink_ = nullptr;
    DoubleSetting* multiplier_ = nullptr;
    BoolSetting*   notify_ = nullptr;

    // 已经就"退回 MaxWalkSpeed"解释过一次了, 避免每帧刷屏。
    bool explainedFallback_ = false;
};

} // namespace epsilon::feature
