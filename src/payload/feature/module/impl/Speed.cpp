#include "payload/feature/module/impl/Speed.h"

#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/feature/GameContext.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <optional>

namespace epsilon::feature {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;
using namespace epsilon::game::dungeons2;

// 荒谬的倍率会让角色穿过碰撞体, 限制在 10 倍以内。
constexpr double maxMultiplier = 10.0;

constexpr char sinkGameMultiplier[] = "SpeedMultiplier";
constexpr char sinkMaxWalkSpeed[]   = "MaxWalkSpeed";
constexpr char sinkAttribute[]      = "MovementAttribute";

constexpr uint64_t rewriteNoteIntervalMs = 3000;
constexpr uint64_t warningIntervalMs = 4000;
uint64_t gLastWarnMs = 0;
uint64_t gLastRewriteNoteMs = 0;

} // namespace

char const* SpeedModule::sinkLabel(Sink sink) {
    switch (sink) {
        case Sink::movementAttribute: return sinkAttribute;
        case Sink::maxWalkSpeed:      return sinkMaxWalkSpeed;
        case Sink::gameMultiplier:
        default:                      return sinkGameMultiplier;
    }
}

SpeedModule::SpeedModule()
    : Module("Speed", Category::player, "Movement speed multiplier") {
    auto& s = addEnum("ApplyTo",
                      {sinkAttribute, sinkMaxWalkSpeed, sinkGameMultiplier},
                      sinkAttribute,
                      "Which field to scale. MovementAttribute is the one that "
                      "actually works on this build");
    sink_ = &s;

    auto& m = addDouble("Multiplier", 1.5, 0.1, maxMultiplier, 0.1, "Speed multiplier");
    multiplier_ = &m;

    auto& n = addBool("Notify", false, "Print resolution details once on enable");
    notify_ = &n;

    setDefaultHidden(false);
}

SpeedModule::Sink SpeedModule::currentSink() const {
    if (sink_ && sink_->is(sinkAttribute))    return Sink::movementAttribute;
    if (sink_ && sink_->is(sinkMaxWalkSpeed)) return Sink::maxWalkSpeed;
    return Sink::gameMultiplier;
}

bool SpeedModule::sinkAvailable(Sink sink) const {
    auto const& mv = movement();
    switch (sink) {
        case Sink::movementAttribute: return mv.attributes().valid();
        case Sink::maxWalkSpeed:      return mv.layout().has(MovementField::maxWalkSpeed);
        case Sink::gameMultiplier:
        default:                      return mv.layout().has(MovementField::movementSpeedMultiplier);
    }
}

bool SpeedModule::readSink(Sink sink, float& out) const {
    auto const& mv = movement();

    std::optional<float> v;
    switch (sink) {
        case Sink::movementAttribute:
            v = mv.readAttribute(MovementAttribute::movementSpeedMultiplier);
            break;
        case Sink::maxWalkSpeed:
            v = mv.read(MovementField::maxWalkSpeed);
            break;
        case Sink::gameMultiplier:
        default:
            v = mv.read(MovementField::movementSpeedMultiplier);
            break;
    }

    if (!v) return false;
    out = *v;
    return true;
}

bool SpeedModule::writeSink(Sink sink, float value) const {
    auto const& mv = movement();
    switch (sink) {
        case Sink::movementAttribute:
            return mv.writeAttribute(MovementAttribute::movementSpeedMultiplier, value);
        case Sink::maxWalkSpeed:
            return mv.write(MovementField::maxWalkSpeed, value);
        case Sink::gameMultiplier:
        default:
            return mv.write(MovementField::movementSpeedMultiplier, value);
    }
}

void SpeedModule::captureBaseline(Sink sink) {
    float v = 0.0f;
    if (readSink(sink, v)) {
        base_ = v;
        hasBase_ = true;
        lastApplied_ = 0.0f;
        // 基线为 0 说明字段没被游戏初始化(或偏移不对)。用 1.0 兜底, 否则乘
        // 出来恒为 0, 角色会被钉在原地。
        if (std::fabs(base_) < 0.0001f) base_ = 1.0f;
    } else {
        hasBase_ = false;
    }
}

void SpeedModule::onEnable() {
    hasBase_ = false;
    haveActiveSink_ = false;
    explainedFallback_ = false;

    auto& mv = movement();
    if (auto* eng = game().engine; eng != nullptr && eng->ready()) {
        mv.resolve(*eng, true);     // 强制重扫: 玩家可能刚进关卡
    }

    if (!mv.ready()) {
        logWarn(fmt("[Speed] 启用但尚未解析到玩家移动组件: {}", mv.lastError()));
        logWarn("[Speed] 会在解析成功后自动生效");
        return;
    }

    Sink sink = currentSink();

    // 首选目标不可用时退回 MaxWalkSpeed 并且**明确告知** —— 静默退回会让用户
    // 以为改的是别的字段。
    if (!sinkAvailable(sink) && sink != Sink::maxWalkSpeed) {
        logWarn(fmt("[Speed] 目标 {} 在本构建上不可用, 退回 MaxWalkSpeed",
                    sinkLabel(sink)));
        sink = Sink::maxWalkSpeed;
        explainedFallback_ = true;
    }

    if (!sinkAvailable(sink)) {
        logWarn("[Speed] 该目标属性不可用(反射与已知表都没给出偏移)");
        return;
    }

    activeSink_ = sink;
    haveActiveSink_ = true;
    captureBaseline(sink);

    if (notify_ && notify_->value()) {
        logInfo(fmt("[Speed] 目标 {} / {} ; 施加于 {} ; 基线 = {:.3f}",
                    mv.target().pawnClass, mv.target().movementClassName,
                    sinkLabel(sink), base_));
        logInfo(fmt("[Speed] 属性偏移 {}", mv.layout().describe()));
    }
}

void SpeedModule::onDisable() {
    if (haveActiveSink_ && hasBase_) {
        if (writeSink(activeSink_, base_)) {
            logInfo(fmt("[Speed] 已还原 = {:.3f}", base_));
        } else {
            logWarn("[Speed] 还原失败 —— 目标可能已销毁(换关卡/死亡)");
        }
    }
    hasBase_ = false;
    haveActiveSink_ = false;
    lastApplied_ = 0.0f;
}

void SpeedModule::onFrame() {
    applyIfNeeded(false);
}

void SpeedModule::applyIfNeeded(bool force) {
    auto& mv = movement();
    if (!mv.ready()) return;

    const Sink want = currentSink();

    // 目标切换 / 首次生效 / 换关卡(偏移重新解析)都要重取基线。
    if (!haveActiveSink_ || want != activeSink_ || !hasBase_) {
        activeSink_ = want;
        haveActiveSink_ = true;
        captureBaseline(activeSink_);
        if (!hasBase_) return;
    }

    const float target = static_cast<float>(base_ * multiplier_->value());
    if (std::isnan(target) || target <= 0.0f) return;

    float current = 0.0f;
    if (!readSink(activeSink_, current)) return;

    // 浮点比较留容差: 引擎内部可能做一次 float 往返, 严格相等会导致每帧都判定
    // "被改了"从而一直写。
    const float diff = std::fabs(current - lastApplied_);
    const bool unchanged = (diff <= 0.001f) && (std::fabs(current - target) <= 0.001f) && !force;
    if (unchanged) return;

    if (writeSink(activeSink_, target)) {
        lastApplied_ = target;
        // 连续重写说明游戏在覆盖这个字段, 每帧补写压不住 —— 该去挂钩游戏自己
        // 的读取/计算路径, 而不是加倍努力地写。
        const uint64_t now = ::GetTickCount64();
        if (now - gLastRewriteNoteMs > rewriteNoteIntervalMs) {
            gLastRewriteNoteMs = now;
            logWarn(fmt("[Speed] 持续重写 {} (读到 {:.3f} -> 写入 {:.3f}) —— "
                        "该字段疑似每帧被游戏重置, 每帧补写无法稳定生效",
                        sinkLabel(activeSink_), current, target));
        }
    } else {
        const uint64_t now = ::GetTickCount64();
        if (now - gLastWarnMs > warningIntervalMs) {
            gLastWarnMs = now;
            logWarn("[Speed] 写入失败(目标内存不可写?)");
        }
    }
}

std::string SpeedModule::info() const {
    auto const& mv = movement();
    if (!mv.ready()) return "waiting for player";

    const Sink sink = haveActiveSink_ ? activeSink_ : currentSink();
    float current = 0.0f;
    const bool ok = readSink(sink, current);
    char const* name = sinkLabel(sink);
    return ok ? fmt("{}={:.2f} x{:.1f}", name, current,
                    multiplier_ ? multiplier_->value() : 0.0)
              : fmt("{} unavailable", name);
}

} // namespace epsilon::feature
