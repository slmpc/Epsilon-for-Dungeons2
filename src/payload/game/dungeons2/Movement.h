// ============================================================================
//  Movement.h — 玩家移动参数的字段级视图
//
//  · UCharacterMovementComponent 的偏移: docs/offsets/character-movement.md
//  · GAS 属性集 ATR_Movement 的偏移:    docs/offsets/movement-attributes.md
// ============================================================================
#pragma once

#include "payload/game/Offsets.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace epsilon::game::dungeons2 {

// ---------------------------------------------------------------- 移动组件字段
// UCharacterMovementComponent 上可访问的 float 字段。
enum class MovementField : uint8_t {
    maxStepHeight = 0,
    jumpZVelocity,
    walkableFloorAngle,
    gravityScale,
    gravityDirection,
    maxWalkSpeed,
    maxWalkSpeedCrouched,
    maxSwimSpeed,
    maxFlySpeed,
    maxAcceleration,
    brakingDecelerationWalking,
    airControl,
    mass,
    // 游戏自己的速度倍率。只可能来自反射, 不在已知表里。
    movementSpeedMultiplier,
    count,
};

inline constexpr size_t movementFieldCount = static_cast<size_t>(MovementField::count);

[[nodiscard]] std::string_view movementFieldName(MovementField field) noexcept;

// 从二进制里的代码生成属性表读出的偏移。0 表示本构建上不可信/不可用。
[[nodiscard]] uint32_t builtinMovementOffset(MovementField field) noexcept;

// 反射优先、已知表补齐的偏移集合。全 0 表示这一项在该构建上拿不到。
class MovementLayout {
public:
    void set(MovementField field, uint32_t offset) noexcept;
    [[nodiscard]] uint32_t offsetOf(MovementField field) const noexcept;
    [[nodiscard]] bool has(MovementField field) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return filled_ == 0; }
    [[nodiscard]] size_t filled() const noexcept { return filled_; }

    // 至少要能拿到 MaxWalkSpeed 与 JumpZVelocity 才算可用。
    [[nodiscard]] bool hasCore() const noexcept;

    // 纯 ASCII —— 这个字符串会显示在游戏内的 ImGui 面板上。
    [[nodiscard]] std::string describe() const;

private:
    std::array<uint32_t, movementFieldCount> offsets_{};
    size_t filled_ = 0;
};

// 绑定一个 UCharacterMovementComponent*. 所有读写都走 safeRead/safeWrite。
class MovementComponent {
public:
    MovementComponent() = default;
    MovementComponent(uint64_t address, MovementLayout layout)
        : address_(address), layout_(layout) {}

    [[nodiscard]] bool valid() const noexcept { return address_ != 0; }
    [[nodiscard]] uint64_t address() const noexcept { return address_; }
    [[nodiscard]] MovementLayout const& layout() const noexcept { return layout_; }

    [[nodiscard]] std::optional<float> read(MovementField field) const;
    bool write(MovementField field, float value) const;

    // 按裸偏移读写。只在诊断命令里用 —— 排查"真值落在相邻 4 字节"时需要它。
    [[nodiscard]] std::optional<float> readAt(uint32_t offset) const;
    bool writeAt(uint32_t offset, float value) const;

private:
    uint64_t       address_ = 0;
    MovementLayout layout_{};
};

// ---------------------------------------------------------------- GAS 属性集
// ATR_Movement 上的属性。间隔 0x10, 每个占 BaseValue/CurrentValue 两个 float。
enum class MovementAttribute : uint8_t {
    movementSpeedMultiplier = 0,
    movementFriction,
    movementFrictionMultiplier,
    movementRotation,
    movementRotationMultiplier,
    movementGravity,
    gravityScale,
    airControl,
    rollCooldown,
    rollCharges,
    mass,
    interactionRange,
    count,
};

inline constexpr size_t movementAttributeCount = static_cast<size_t>(MovementAttribute::count);

[[nodiscard]] std::string_view movementAttributeName(MovementAttribute attribute) noexcept;

// 代码生成属性表里**声明**的位置。
[[nodiscard]] uint32_t declaredAttributeOffset(MovementAttribute attribute) noexcept;

// 实测的数据位置 = 声明值 + 8。仅 movementSpeedMultiplier 经过写入验证。
[[nodiscard]] uint32_t attributeDataOffset(MovementAttribute attribute) noexcept;

// FGameplayAttributeData 是 {BaseValue, CurrentValue} 一对。
class MovementAttributeSet {
public:
    explicit MovementAttributeSet(uint64_t address = 0) : address_(address) {}

    [[nodiscard]] bool valid() const noexcept { return address_ != 0; }
    [[nodiscard]] uint64_t address() const noexcept { return address_; }

    // 读 CurrentValue。
    [[nodiscard]] std::optional<float> read(MovementAttribute attribute) const;
    // 写 Base 与 Current 两者 —— 只写其一会被 GAS 聚合时的另一个值盖回去。
    bool write(MovementAttribute attribute, float value) const;

    [[nodiscard]] std::optional<float> readAt(uint32_t offset) const;
    bool writeAt(uint32_t offset, float value) const;

private:
    uint64_t address_ = 0;
};

} // namespace epsilon::game::dungeons2
