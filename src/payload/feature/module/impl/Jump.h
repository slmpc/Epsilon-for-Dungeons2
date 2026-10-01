// ============================================================================
//  Jump.h — 大跳模块
//
//  ── 原理 ─────────────────────────────────────────────────────────────────
//  UE 的角色跳跃初速度由 UCharacterMovementComponent::JumpZVelocity 决定
//  (调用链 DoJump -> CharacterMovement->JumpZVelocity)。跳跃高度与初速度的
//  平方成正比, 所以把 JumpZVelocity 乘 N 倍, 高度大约是 N² 倍。
//
//  实测确认(IDA): 二进制里存在 "JumpZVelocity" 反射名, 以及
//  "GetMaxJumpHeight" / "GetMaxJumpHeightWithJumpTime" —— 后者是 UE 5.6
//  UCharacterMovementComponent 自带的接口。游戏另有自己的子类
//  UPlayerCharacterMovementComponent(RTTI 实测存在), 所以偏移必须按运行时
//  的真实类去问, 见 MovementAccess。
//
//  ── 为什么"每帧写"而不是"写一次" ──────────────────────────────────────────
//  跳跃过程里引擎会按曲线调整 Z 速度, 而 JumpZVelocity 也可能被游戏自己的
//  逻辑(或从服务端下发的复制属性)覆盖。一次性写入很容易在某次状态更新后
//  失效, 表现为"有时生效有时不生效"。改成每帧仅在**值被改掉时**补写:
//  大部分帧只是一次读取比对, 不产生写入, 对帧时间与内存写痕迹都很轻。
//
//  ── 边界 ─────────────────────────────────────────────────────────────────
//  本模块只改本地客户端的移动参数。联机时移动由服务端校验, 修改可能表现为
//  被拉回(校正)或被拒, 这属于预期行为而非缺陷。
// ============================================================================
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
    // 把计算出的值写进游戏。返回是否真的写了(值没变就不写)。
    void applyIfNeeded(bool force);

    // 游戏里读到的原始值(启用瞬间的基线)。用它乘倍率, 避免"改了设置后
    // 在已放大值上再乘一次"的累积放大。
    float baseZ_ = 0.0f;
    // 我们上一次写进去的值。用来判断当前游戏里的值是不是仍是我们写的 ——
    // 不是就说明被游戏改了, 需要补写。
    float lastApplied_ = 0.0f;
    bool  hasBase_ = false;

    // 设置的快捷引用(构造期取得, 生命周期由 Module 持有)。
    DoubleSetting* multiplier_ = nullptr;
    BoolSetting*   logOnce_ = nullptr;
};

} // namespace epsilon::feature
