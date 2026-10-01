#include "payload/feature/module/impl/Jump.h"

#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/feature/GameContext.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>

namespace epsilon::feature {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;
using namespace epsilon::game::dungeons2;

// 荒谬的倍率会让角色一跳穿出关卡边界, 限制在 10 倍以内。
constexpr double maxMultiplier = 10.0;

constexpr uint64_t warningIntervalMs = 4000;
uint64_t gLastWarnMs = 0;

} // namespace

JumpModule::JumpModule()
    : Module("Jump", Category::player, "Jump height multiplier (JumpZVelocity scaling)") {
    auto& m = addDouble("Multiplier", 2.5, 0.5, maxMultiplier, 0.1,
                        "JumpZVelocity multiplier (height ~= multiplier^2)");
    multiplier_ = &m;

    auto& t = addBool("Notify", false, "Print resolution details once on enable");
    logOnce_ = &t;

    setDefaultHidden(false);
}

void JumpModule::onEnable() {
    hasBase_ = false;
    lastApplied_ = 0.0f;

    auto& mv = movement();
    // 强制重扫一次: 玩家可能刚进关卡, 节流缓存里还是旧的失败结果。
    if (auto* eng = game().engine; eng != nullptr && eng->ready()) {
        mv.resolve(*eng, true);
    }

    if (!mv.ready()) {
        logWarn(fmt("[Jump] 启用但尚未解析到玩家移动组件: {}", mv.lastError()));
        logWarn("[Jump] 会在解析成功后自动生效, 无需手工干预");
        return;
    }

    if (auto z = mv.read(MovementField::jumpZVelocity)) {
        baseZ_ = *z;
        hasBase_ = true;
        if (logOnce_ && logOnce_->value()) {
            logInfo(fmt("[Jump] 目标 {} / {} ; JumpZVelocity 基线 = {:.2f}",
                        mv.target().pawnClass, mv.target().movementClassName, baseZ_));
            logInfo(fmt("[Jump] 属性偏移 {}", mv.layout().describe()));
        }
    } else {
        logWarn("[Jump] 读到 JumpZVelocity 失败(偏移可能失效)");
    }
}

void JumpModule::onDisable() {
    // 复原成基线, 否则关掉模块后跳跃高度会停在放大后的值上。
    if (hasBase_) {
        if (movement().write(MovementField::jumpZVelocity, baseZ_)) {
            logInfo(fmt("[Jump] 已还原 JumpZVelocity = {:.2f}", baseZ_));
        } else {
            logWarn("[Jump] 还原失败 —— 目标可能已销毁(换关卡/死亡)");
        }
    }
    hasBase_ = false;
    lastApplied_ = 0.0f;
}

void JumpModule::onFrame() {
    applyIfNeeded(false);
}

void JumpModule::applyIfNeeded(bool force) {
    auto& mv = movement();
    if (!mv.ready()) return;

    // 换关卡会换掉 movement 对象, 必须重新取基线。
    if (!hasBase_) {
        if (auto z = mv.read(MovementField::jumpZVelocity)) {
            baseZ_ = *z;
            hasBase_ = true;
            lastApplied_ = 0.0f;
        } else {
            return;
        }
    }

    const float want = static_cast<float>(baseZ_ * multiplier_->value());
    if (std::isnan(want) || want <= 0.0f) return;

    const auto current = mv.read(MovementField::jumpZVelocity);
    if (!current) return;

    // 浮点比较留容差: 引擎内部可能做一次 float 往返, 严格相等会导致每帧都判定
    // "被改了"从而一直写。
    const float diff = std::fabs(*current - lastApplied_);
    const bool unchanged = (diff <= 0.01f) && (std::fabs(*current - want) <= 0.01f) && !force;
    if (unchanged) return;

    if (mv.write(MovementField::jumpZVelocity, want)) {
        lastApplied_ = want;
    } else {
        const uint64_t now = ::GetTickCount64();
        if (now - gLastWarnMs > warningIntervalMs) {
            gLastWarnMs = now;
            logWarn("[Jump] 写入 JumpZVelocity 失败(目标内存不可写?)");
        }
    }
}

std::string JumpModule::info() const {
    auto const& mv = movement();
    if (!mv.ready()) return "waiting for player";

    const auto z = mv.read(MovementField::jumpZVelocity);
    const std::string now = z ? fmt("{:.1f}", *z) : std::string("?");
    return fmt("z={} x{:.1f}", now, multiplier_ ? multiplier_->value() : 0.0);
}

} // namespace epsilon::feature
