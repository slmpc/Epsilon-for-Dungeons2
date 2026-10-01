#include "payload/game/dungeons2/MovementResolver.h"

#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/game/dungeons2/Player.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace epsilon::game::dungeons2 {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;

// 解析失败时的重试间隔。玩家读图/重生期间解析必然失败, 每帧重试会让帧时间抖,
// 退避太久又会出现"回到关卡后好几秒才生效"。
constexpr uint64_t retryIntervalMs = 1500;
// 成功后的复核间隔。换关卡会换掉 pawn 与移动组件, 到点必须重扫 ——
// 否则会继续往已销毁的对象上写。
constexpr uint64_t recheckIntervalMs = 5000;

constexpr char errEngineNotReady[] =
    "engine not ready (GObjects/GNames not located)";
constexpr char errNoPlayer[] =
    "no local player pawn in level actors - run the 'player' command to list candidates";

uint64_t nowMs() { return ::GetTickCount64(); }

} // namespace

void MovementResolver::clearTarget() {
    target_ = PlayerTarget{};
    component_ = MovementComponent{};
    attributes_ = MovementAttributeSet{};
    offsetSource_.clear();
}

bool MovementResolver::resolve(ue::Engine& engine, bool force) {
    const uint64_t now = nowMs();
    if (!force && now < nextResolveAtMs_) return ready();

    lastError_.clear();

    if (!engine.ready()) {
        lastError_ = errEngineNotReady;
        clearTarget();
        nextResolveAtMs_ = now + retryIntervalMs;
        return false;
    }

    if (!resolveTarget(engine)) {
        nextResolveAtMs_ = now + retryIntervalMs;
        return false;
    }

    if (!component_.layout().hasCore()) {
        if (!resolveLayout(engine)) {
            nextResolveAtMs_ = now + retryIntervalMs;
            return false;
        }
    }

    nextResolveAtMs_ = now + recheckIntervalMs;
    return true;
}

bool MovementResolver::resolveTarget(ue::Engine& engine) {
    const uint64_t pawn = findLocalPlayerPawn(engine);
    if (!pawn) {
        lastError_ = errNoPlayer;
        clearTarget();
        return false;
    }

    const std::string pawnClass = engine.objects().classNameOf(pawn);
    auto movement = findMovementComponent(engine, pawn);
    if (!movement) {
        lastError_ = fmt("no CharacterMovement component on pawn {}", pawnClass);
        clearTarget();
        return false;
    }

    const uint64_t movementClass = engine.objects().classOf(*movement);

    // 目标没换就不要丢掉已解析的布局 —— 反射遍历有几个类, 有成本。
    const bool sameTarget = target_.resolved && target_.pawn == pawn &&
                            target_.movement == *movement &&
                            target_.movementClass == movementClass;
    if (sameTarget) return true;

    clearTarget();
    target_.pawn = pawn;
    target_.pawnClass = pawnClass;
    target_.movement = *movement;
    target_.movementClass = movementClass;
    target_.movementClassName = engine.objects().classNameOf(*movement);
    target_.resolved = true;

    // 属性集找不到不算失败: 组件上的偏移仍然可用, 只是走不了"改属性"这条路。
    if (auto set = findPlayerAttributeSet(engine, pawn)) {
        target_.attributeSet = *set;
        target_.attributeSetClassName = engine.objects().classNameOf(*set);
        attributes_ = MovementAttributeSet(*set);
        logInfo(fmt("[Movement] 玩家属性集: {} @ {}",
                    target_.attributeSetClassName, hex(*set, 16)));
    } else {
        logWarn("[Movement] 没找到属于玩家的 ATR_Movement, 只能退回到写组件字段");
    }

    component_ = MovementComponent(target_.movement, MovementLayout{});
    return true;
}

bool MovementResolver::resolveLayout(ue::Engine& engine) {
    if (!target_.movementClass) {
        lastError_ = "movement component UClass is null";
        return false;
    }

    MovementLayout layout;

    // 1) 先试运行时反射 —— 能自动适配游戏更新。
    const auto props = engine.reflection().allPropertiesInherited(target_.movementClass);
    auto pickFloat = [&](MovementField field, std::string_view name) {
        for (auto const& p : props) {
            if (p.name == name && icontains(p.type, "FloatProperty") && p.offset > 0) {
                layout.set(field, static_cast<uint32_t>(p.offset));
                return;
            }
        }
    };
    pickFloat(MovementField::maxWalkSpeed, "MaxWalkSpeed");
    pickFloat(MovementField::jumpZVelocity, "JumpZVelocity");
    pickFloat(MovementField::gravityScale, "GravityScale");
    pickFloat(MovementField::airControl, "AirControl");
    pickFloat(MovementField::maxAcceleration, "MaxAcceleration");
    pickFloat(MovementField::movementSpeedMultiplier, "MovementSpeedMultiplier");
    if (!layout.has(MovementField::movementSpeedMultiplier)) {
        pickFloat(MovementField::movementSpeedMultiplier, "SpeedMultiplier");
    }
    const size_t reflected = props.size();

    // 2) 反射拿不到的用二进制属性表读出的固有偏移补齐。
    for (size_t i = 0; i < movementFieldCount; ++i) {
        const auto field = static_cast<MovementField>(i);
        if (layout.has(field)) continue;
        layout.set(field, builtinMovementOffset(field));
    }

    // 纯 ASCII: 这个字符串会显示在游戏内的 ImGui 面板上。
    offsetSource_ = fmt("table ({} reflected)", static_cast<uint64_t>(reflected));

    if (!layout.hasCore()) {
        lastError_ = fmt("{}: neither reflection nor the known-offset table gave "
                         "MaxWalkSpeed/JumpZVelocity",
                         target_.movementClassName);
        return false;
    }

    component_ = MovementComponent(target_.movement, layout);
    return true;
}

} // namespace epsilon::game::dungeons2
