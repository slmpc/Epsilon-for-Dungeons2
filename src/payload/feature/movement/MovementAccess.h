// ============================================================================
//  MovementAccess.h — 玩家移动组件的解析与读写
//
//  这一层解决一个具体问题: 拿到玩家角色的 UCharacterMovementComponent*,
//  并读/写它上面那几个决定移动手感的浮点属性。
//
//  ── 为什么不写死偏移 ──────────────────────────────────────────────────────
//  IDA 里确实能看到这些属性名(实测都在二进制里):
//      MaxWalkSpeed MaxWalkSpeedCrouched JumpZVelocity GravityScale
//      AirControl AirControlBoostMultiplier GroundFriction MaxFlySpeed ...
//  它们是 UE 的反射名, 但**名字旁边没有偏移数字** —— FProperty 的
//  Offset_Internal 是运行时填的。所以偏移必须现场问, 也就是走 Reflection。
//
//  当前能确认的静态事实(实测, 来自 IDA 里的 MSVC RTTI 名):
//      .?AVUPlayerCharacterMovementComponent@@   ← 游戏自己的移动组件子类
//      MovementSpeedMultiplier / OnRep_MovementSpeedMultiplier  ← 该组件上的
//                                                  复制属性(游戏自己的速度倍率)
//  也就是说这个游戏在 UCharacterMovementComponent 之上又加了一层。因此下面的
//  解析**必须按运行时拿到的真实类**去问属性, 而不能假设是基类布局。
//
//  ── 坐标轨迹 ─────────────────────────────────────────────────────────────
//  玩家对象 -> 它所属的 UWorld -> 该 World 的 PersistentLevel -> 遍历 Actors
//  找到属于本进程的 PlayerCharacter。每一步都靠反射解析的偏移, 且每一跳都走
//  safeRead —— 目标进程里任何指针都可能是垃圾值, 不能让它把游戏带崩。
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace epsilon::ue {
class Engine;
}

namespace epsilon::feature {

// 移动组件上我们要用的那几个属性的偏移集合。
// 全部由反射解析得到, 解析不到就保持 0 并记在 missing 里。
struct MovementOffsets {
    int32_t maxWalkSpeed = 0;    // UCharacterMovementComponent::MaxWalkSpeed
    int32_t jumpZVelocity = 0;   // ::JumpZVelocity
    int32_t gravityScale = 0;    // ::GravityScale
    int32_t airControl = 0;      // ::AirControl
    int32_t maxAcceleration = 0; // ::MaxAcceleration
    // 游戏自己的速度倍率(存在则优先用它, 因为它是被复制的权威字段)
    int32_t speedMultiplier = 0;

    // ---- GAS 属性集 ATR_Movement 里的偏移 ★ ----
    //
    // 实测这两个才是真正驱动移动速度的字段: 原本都是 700.0, 改成 3000 后
    // 游戏内移动明显变快, 且写入**持久**(不像组件上的 MaxWalkSpeed 会被重算)。
    // 它们是 FGameplayAttributeData 的 {BaseValue, CurrentValue} 一对, 两个都要写。
    //
    // 偏移来源: 代码生成参数表(0x14a069800 区段)。注意那张表读出的是 +0x90,
    // **实际数据在 +0x98** —— 表偏移整体差 8 字节, 由相邻校验定下来的
    // (表里 GravityScale 标 +0xf0, 而 +0xf8 处正是 1.2 这样一个合理的重力倍率)。
    int32_t speedAttributeBase = 0;     // ATR_Movement + 0x98
    int32_t speedAttributeCurrent = 0;  // ATR_Movement + 0x9c

    [[nodiscard]] bool hasCore() const noexcept {
        return maxWalkSpeed > 0 && jumpZVelocity > 0;
    }
    [[nodiscard]] bool hasSpeedAttribute() const noexcept {
        return speedAttributeBase > 0;
    }
    // 人类可读的解析结果, 供 UI/命令输出。
    [[nodiscard]] std::string describe() const;
};

// 当前解析到的玩家移动目标。
struct MovementTarget {
    uint64_t pawn = 0;          // APlayerCharacter*
    uint64_t movement = 0;      // UCharacterMovementComponent*
    uint64_t movementClass = 0; // 该组件的 UClass*(问属性偏移要用)
    std::string pawnClass;      // 玩家类名(如 PlayerCharacter_C)
    std::string movementClassName;  // 移动组件的类名(实测游戏有自己的子类)
    bool     resolved = false;

    // ---- 玩家的 GAS 移动属性集(ATR_Movement) ----
    //
    // ★ 真正该改的东西在这里, 不在移动组件上。
    //   组件上的 MaxWalkSpeed 每帧被游戏从属性重算, 写它必然被覆盖(实测写入后
    //   40ms 内就被改回); 而属性集里的字段写进去是**持久**的。
    uint64_t attributeSet = 0;          // ATR_Movement* —— 属于本玩家的那一个
    std::string attributeSetClassName;  // 一般是 "ATR_Movement"
};

// 解析器。持有上一帧的解析结果, 避免每帧都做全量扫描。
//
// 线程约定: 只在游戏线程(帧钩子回调)里用, 自带一个轻量的"多久重试一次"
// 节流。不跨线程共享。
class PlayerMovement {
public:
    // 重新解析玩家与其移动组件。传入引擎实例。
    // force = true 时忽略节流, 立即重扫(玩家换关卡/重生后需要这个)。
    bool resolve(ue::Engine& engine, bool force = false);

    [[nodiscard]] MovementTarget const& target() const noexcept { return target_; }
    [[nodiscard]] MovementOffsets const& offsets() const noexcept { return offsets_; }
    [[nodiscard]] bool ready() const noexcept { return target_.resolved && offsets_.hasCore(); }

    // 让下一次 resolve 立刻重扫。
    void invalidate() noexcept { nextResolveAtMs_ = 0; }

    // ---------------------------------------------------------------- 读写
    // 读一个 float 属性。失败返回 nullopt。
    [[nodiscard]] std::optional<float> readFloat(int32_t offset) const;
    // 写一个 float 属性。走 safeWrite(必要时放宽页保护)。
    bool writeFloat(int32_t offset, float value) const;

    // 任意地址上的 float 读写(不依赖已解析的移动组件)。
    //
    // 排查属性集(ATR_Movement)时用得上: 那时目标是 GAS 的属性对象, 和
    // 移动组件不是同一个东西, 用不了上面那两个按组件取址的接口。
    [[nodiscard]] std::optional<float> readFloatAt(uint64_t addr, int32_t offset) const;
    bool writeFloatAt(uint64_t addr, int32_t offset, float value) const;

    // ---- 移动速度属性(真正生效的那一对) --------------------------------
    // 读/写 ATR_Movement 上的 MovementSpeed 属性。
    // writeMovementSpeed 会**同时写 Base 和 Current** —— 只写其一的话, GAS
    // 在下次聚合时可能用另一个值把它盖回去。
    [[nodiscard]] std::optional<float> readMovementSpeed() const;
    bool writeMovementSpeed(float value) const;

    // 便捷访问。属性偏移缺失时返回 nullopt / false。
    [[nodiscard]] std::optional<float> maxWalkSpeed() const;
    [[nodiscard]] std::optional<float> jumpZVelocity() const;
    [[nodiscard]] std::optional<float> speedMultiplier() const;
    bool setMaxWalkSpeed(float v) const;
    bool setJumpZVelocity(float v) const;

    // 诊断: 上一次解析失败的原因。
    [[nodiscard]] std::string const& lastError() const noexcept { return lastError_; }

    // 偏移的来源("反射" 或 "静态表(...)")。供 UI/日志显示, 便于判断当前用的是
    // 运行时反射还是从二进制读出的已知偏移。
    [[nodiscard]] std::string const& offsetSource() const noexcept { return offsetSource_; }

private:
    bool resolveOffsets(ue::Engine& engine);
    bool resolveTarget(ue::Engine& engine);
    bool resolveAttributeSet(ue::Engine& engine);
    [[nodiscard]] uint64_t findLocalPlayerPawn(ue::Engine& engine) const;

    MovementTarget  target_{};
    MovementOffsets offsets_{};
    std::string     lastError_;
    std::string     offsetSource_;
    uint64_t        nextResolveAtMs_ = 0;
};

} // namespace epsilon::feature
